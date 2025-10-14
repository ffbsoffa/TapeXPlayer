#include "FSTPAudioModule_wrapper.h"
#include <iostream>
#include <iomanip>
#include <vector>
#include <thread>
#include <mutex>
#include <cstring>
#include <cmath>
#include <chrono>
#include <atomic>

// Headers for mmap
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstdio>

// PortAudio
#include <portaudio.h>

// Settings
#include "../FSTPMainModule/WSGUI/FSTPSettings.h"

extern "C" {
    #include <libavcodec/avcodec.h>
    #include <libavformat/avformat.h>
    #include <libavutil/opt.h>
    #include <libavutil/channel_layout.h>
}

// FSTPAudioModule implementation as internal class
class FSTPAudioModuleWrapper::FSTPAudioModuleImpl {
public:
    // Mmap buffer
    int16_t* mmap_buffer = nullptr;
    size_t mmap_size_bytes = 0;
    int mmap_fd = -1;
    std::string temp_filename;

    // File information
    int sample_rate = 0;
    int channels = 0;
    double duration = 0.0;
    std::string codec_name;

    // For tracking current PortAudio stream parameters
    int pa_stream_sample_rate = 0;
    int pa_stream_channels = 0;
    size_t total_samples = 0;

    // Playback state
    std::atomic<double> playback_position{0.0};
    std::atomic<double> playback_speed{0.0};
    std::atomic<bool> is_playing{false};
    std::atomic<bool> is_reverse{false};
    
    // Old smooth speed change system
    std::atomic<double> target_playback_speed{0.0};
    std::thread smooth_speed_thread;
    std::atomic<bool> smooth_speed_running{false};
    std::atomic<bool> should_exit_smooth_speed{false};

    // FIX: Background decoding thread
    std::thread background_decode_thread;
    std::atomic<bool> background_decode_running{false};
    std::atomic<bool> should_stop_background_decode{false};

    // Instant speed setting request (for mouse shuttle)
    std::atomic<bool> instant_speed_requested{false};

    // Variables for jog mode
    std::atomic<bool> jog_forward{false};
    std::atomic<bool> jog_backward{false};
    static constexpr double JOG_SPEED = 0.25;

    // Main variables for old system (quit, shouldExit)
    std::atomic<bool> quit{false};
    std::atomic<bool> shouldExit{false};

    // Volume for old system
    std::atomic<float> volume{1.0f};

    // Audio signal levels for VU meters
    std::atomic<float> audio_level_left{0.0f};
    std::atomic<float> audio_level_right{0.0f};
    std::atomic<float> audio_peak_left{0.0f};
    std::atomic<float> audio_peak_right{0.0f};

    // Decoding progress
    std::atomic<size_t> decoded_samples{0};
    std::atomic<size_t> fast_buffer_samples{0}; // Fast buffer size
    std::atomic<bool> fast_buffer_ready{false};
    std::atomic<bool> full_buffer_ready{false};

    // PortAudio
    PaStream* pa_stream = nullptr;
    bool pa_initialized = false;

    // CRITICAL: Per-instance cached speed to eliminate race condition between players!
    // Was static - conflicted with multiple players
    double cached_speed = 0.0;

    // CRITICAL FIX: Flag to detect destructor cleanup vs normal cleanup
    // Problem: Pa_CloseStream() → PipeWire's malloc_trim() crashes on heap corrupted by video av_frame_ref()
    // Solution: Skip Pa_CloseStream() during destructor, let OS clean up (leak acceptable on exit)
    bool m_in_destructor_cleanup = false;

    static constexpr double FAST_BUFFER_DURATION = 720.0;

    FSTPAudioModuleImpl() = default;

    ~FSTPAudioModuleImpl() {
        // CRITICAL: Set destructor flag BEFORE cleanup
        // This tells Cleanup() to skip Pa_CloseStream() to avoid PipeWire malloc_trim() crash
        m_in_destructor_cleanup = true;

        // FIX: Stop background decoding before cleanup
        // CRITICAL: Check ONLY joinable(), don't rely on flag!
        if (background_decode_thread.joinable()) {
            should_stop_background_decode.store(true);
            background_decode_thread.join();
        }
        background_decode_running.store(false);

        StopSmoothSpeedChange();
        Cleanup();
    }

    bool Initialize() {
        if (!pa_initialized) {
            // CRITICAL PROTECTION: check global PortAudio state
            PaError check_err = Pa_GetDeviceCount();
            if (check_err < 0) {
                std::cout << "Audio module: PortAudio not initialized correctly, error: "
                          << Pa_GetErrorText(check_err) << std::endl;

                // Recovery attempt: reinitialize PortAudio
                std::cout << "Audio module: attempting PortAudio recovery..." << std::endl;
                Pa_Terminate();
                std::this_thread::sleep_for(std::chrono::milliseconds(100));

                PaError recover_err = Pa_Initialize();
                if (recover_err != paNoError) {
                    std::cout << "Audio module: PortAudio recovery failed: "
                              << Pa_GetErrorText(recover_err) << std::endl;
                    return false;
                }
                std::cout << "Audio module: PortAudio recovered" << std::endl;
            } else {
                std::cout << "Audio module: using globally initialized PortAudio ("
                          << check_err << " devices)" << std::endl;
            }

            pa_initialized = true;
            // DON'T start stream here - only after buffer decoding
        }
        return true;
    }
    
    bool StartPermanentAudioStream() {
        // FIXED: removed global limit that blocked 4+ players
        // Each player should be able to create its own audio stream
        std::cout << "🎵 [AUDIO START] StartPermanentAudioStream called (SR:" << sample_rate << ", CH:" << channels << ")" << std::endl;

        // Check if stream reinitialization is needed
        if (pa_stream) {
            if (pa_stream_sample_rate == sample_rate && pa_stream_channels == channels) {
                return true; // Stream already running with correct parameters
            } else {
                std::cout << "PortAudio stream needs reinitialization: " << pa_stream_sample_rate << "Hz/" << pa_stream_channels << "ch -> " << sample_rate << "Hz/" << channels << "ch" << std::endl;
                // Stop and close current stream
                if (Pa_IsStreamActive(pa_stream)) {
                    Pa_StopStream(pa_stream);
                }
                Pa_CloseStream(pa_stream);
                pa_stream = nullptr;
                pa_stream_sample_rate = 0;
                pa_stream_channels = 0;
            }
        }

        PaStreamParameters outputParameters;

        // CRITICAL PROTECTION: PortAudio state validation
        PaDeviceIndex device_count = Pa_GetDeviceCount();
        if (device_count < 0) {
            std::cout << "❌ [AUDIO START] No devices available: " << Pa_GetErrorText(device_count) << std::endl;
            return false;
        }
        std::cout << "✅ [AUDIO START] Found " << device_count << " audio devices" << std::endl;

        int buffer_size = GetAudioBufferSize();

        // CRITICAL FIX: Direct ALSA hardware access to bypass PipeWire malloc_trim() crash
        // PipeWire's pw_impl_node_destroy() calls malloc_trim() which crashes on heap corrupted by video
        // Solution: Use ALSA host API directly (hw:0,0) instead of PipeWire/PulseAudio
        PaDeviceIndex selected_device = paNoDevice;

        // Try to find ALSA host API
        PaHostApiIndex alsa_api = Pa_HostApiTypeIdToHostApiIndex(paALSA);
        if (alsa_api >= 0) {
            const PaHostApiInfo* alsa_info = Pa_GetHostApiInfo(alsa_api);
            if (alsa_info && alsa_info->deviceCount > 0) {
                std::cout << "🎵 [AUDIO] Found ALSA host API with " << alsa_info->deviceCount << " devices" << std::endl;

                // Try to find first working ALSA output device
                for (int i = 0; i < alsa_info->deviceCount; i++) {
                    PaDeviceIndex dev_idx = Pa_HostApiDeviceIndexToDeviceIndex(alsa_api, i);
                    const PaDeviceInfo* dev_info = Pa_GetDeviceInfo(dev_idx);

                    if (dev_info && dev_info->maxOutputChannels > 0) {
                        std::cout << "🎵 [ALSA] Device " << i << ": " << dev_info->name
                                  << " (channels: " << dev_info->maxOutputChannels << ")" << std::endl;

                        // Use first available ALSA device with output channels
                        if (selected_device == paNoDevice) {
                            selected_device = dev_idx;
                            std::cout << "✅ [ALSA] Selected direct hardware device: " << dev_info->name << std::endl;
                        }
                    }
                }
            }
        } else {
            std::cout << "⚠️  [AUDIO] ALSA host API not found, trying default device" << std::endl;
        }

        // Fallback to default if ALSA not found
        if (selected_device == paNoDevice) {
            selected_device = Pa_GetDefaultOutputDevice();
            std::cout << "⚠️  [AUDIO] Using default device as fallback" << std::endl;
        }

        if (selected_device == paNoDevice) {
            std::cout << "❌ [AUDIO] No output device available" << std::endl;
            return false;
        }

        outputParameters.device = selected_device;
        
        // CRITICAL PROTECTION: device parameter validation
        const PaDeviceInfo* device_info = Pa_GetDeviceInfo(outputParameters.device);
        if (!device_info) {
            std::cout << "PortAudio: failed to get device information" << std::endl;
            return false;
        }

        // Use current file parameters with checks
        outputParameters.channelCount = std::min(channels, device_info->maxOutputChannels);
        outputParameters.sampleFormat = paInt16;
        outputParameters.suggestedLatency = device_info->defaultLowOutputLatency;
        outputParameters.hostApiSpecificStreamInfo = nullptr;

        std::cout << "PortAudio: starting stream SR:" << sample_rate << "Hz, CH:" << outputParameters.channelCount
                  << "/" << channels << ", device: " << device_info->name << std::endl;

        // CRITICAL PROTECTION: wrap Pa_OpenStream in try-catch
        PaError err = paNoError;
        try {
            std::cout << "PortAudio: calling Pa_OpenStream..." << std::endl;
            err = Pa_OpenStream(&pa_stream, nullptr, &outputParameters, sample_rate, buffer_size,
                               paClipOff, AudioCallback, this);
            std::cout << "PortAudio: Pa_OpenStream completed with code: " << err << std::endl;
        } catch (...) {
            std::cout << "PortAudio: EXCEPTION in Pa_OpenStream!" << std::endl;
            return false;
        }

        if (err == paNoError) {
            std::cout << "✅ [AUDIO START] Pa_OpenStream succeeded" << std::endl;
            playback_speed.store(0.0); // Start with zero speed

            // CRITICAL PROTECTION: wrap Pa_StartStream in try-catch
            try {
                std::cout << "🚀 [AUDIO START] Calling Pa_StartStream..." << std::endl;
                err = Pa_StartStream(pa_stream);
                std::cout << "📊 [AUDIO START] Pa_StartStream returned code: " << err << std::endl;
            } catch (...) {
                std::cout << "❌ [AUDIO START] EXCEPTION in Pa_StartStream!" << std::endl;
                Pa_CloseStream(pa_stream);
                pa_stream = nullptr;
                return false;
            }

            if (err == paNoError) {
                // Save parameters of successfully started stream
                pa_stream_sample_rate = sample_rate;
                pa_stream_channels = channels;
                std::cout << "🎉 [AUDIO START] Stream started successfully! (SR:" << sample_rate << ", CH:" << channels << ")" << std::endl;
                return true;
            } else {
                std::cout << "❌ [AUDIO START] Pa_StartStream failed: " << Pa_GetErrorText(err) << std::endl;
                Pa_CloseStream(pa_stream);
                pa_stream = nullptr;
            }
        } else {
            std::cout << "❌ [AUDIO START] Pa_OpenStream failed: " << Pa_GetErrorText(err) << std::endl;
        }

        // CRITICAL PROTECTION: if initialization attempts fail, temporarily disable audio
        std::cout << "PortAudio: initialization failed, working in silent mode" << std::endl;
        return false;
    }

    void Cleanup() {
        StopSmoothSpeedChange();

        // CRITICAL FIX: Skip Pa_CloseStream() during destructor cleanup
        // Problem: Heap corruption from video av_frame_ref() causes PipeWire's malloc_trim() to crash
        // When Pa_CloseStream() → pw_impl_node_destroy() → malloc_trim(), it crashes on corrupted heap
        // Solution: During destructor (program exit), skip Pa_CloseStream() and let OS clean up
        // Memory leak ~few KB acceptable on program exit (OS frees all resources anyway)
        if (m_in_destructor_cleanup) {
            if (pa_stream) {
                std::cout << "[AUDIO] ⚠️  DESTRUCTOR MODE: Skipping Pa_CloseStream() to avoid PipeWire crash" << std::endl;
                std::cout << "[AUDIO] Stream leak acceptable on program exit - OS will clean up" << std::endl;
                pa_stream = nullptr;  // Just clear pointer, don't close
                pa_stream_sample_rate = 0;
                pa_stream_channels = 0;
            }

            CleanupMmap();
            if (pa_initialized) {
                std::cout << "Audio module: cleanup complete (destructor mode)" << std::endl;
                pa_initialized = false;
            }
            return;  // Early exit - skip normal cleanup
        }

        // NORMAL CLEANUP (file changes, not program exit)
        // CRITICAL: Careful PortAudio stream shutdown with PipeWire workarounds
        // Problem: PipeWire has race condition in pw_stream_destroy causing malloc corruption
        // Solution: Stop stream early, wait for buffers to drain, then close carefully
        if (pa_stream) {
            std::cout << "[AUDIO] Starting careful PortAudio stream shutdown..." << std::endl;

            try {
                // Step 1: Stop stream if active
                PaError is_active = Pa_IsStreamActive(pa_stream);
                if (is_active == 1) {
                    std::cout << "[AUDIO] Stream is active, stopping..." << std::endl;
                    PaError stop_err = Pa_StopStream(pa_stream);
                    if (stop_err != paNoError && stop_err != paStreamIsStopped) {
                        std::cout << "[AUDIO] Pa_StopStream warning: " << Pa_GetErrorText(stop_err) << std::endl;
                    }

                    // CRITICAL: Wait longer for PipeWire to fully drain buffers
                    // PipeWire needs time to communicate with daemon
                    std::cout << "[AUDIO] Waiting 200ms for PipeWire buffer drain..." << std::endl;
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }

                // Step 2: Close stream (this is where PipeWire crashes during destructor)
                std::cout << "[AUDIO] Closing stream..." << std::endl;
                PaError close_err = Pa_CloseStream(pa_stream);
                if (close_err != paNoError) {
                    std::cout << "[AUDIO] Pa_CloseStream warning: " << Pa_GetErrorText(close_err) << std::endl;
                }

                pa_stream = nullptr;
                pa_stream_sample_rate = 0;
                pa_stream_channels = 0;
                std::cout << "[AUDIO] Stream closed successfully" << std::endl;

            } catch (const std::exception& e) {
                std::cout << "[AUDIO] Exception during cleanup: " << e.what() << std::endl;
                pa_stream = nullptr;
            } catch (...) {
                std::cout << "[AUDIO] Unknown exception during cleanup" << std::endl;
                pa_stream = nullptr;
            }
        }

        CleanupMmap();
        if (pa_initialized) {
            std::cout << "Audio module: cleanup complete" << std::endl;
            pa_initialized = false;
        }
    }

    // Old smooth speed change system - without easing functions

    // Catmull-Rom interpolation for audio samples
    double CatmullRomInterpolate(double p0, double p1, double p2, double p3, double t) {
        double t2 = t * t;
        double t3 = t2 * t;

        return 0.5 * ((2.0 * p1) +
                     (-p0 + p2) * t +
                     (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t2 +
                     (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * t3);
    }

    // Safe sample retrieval with bounds checking
    int16_t GetSafeSample(size_t index, int channel, size_t max_samples) {
        if (index >= max_samples || !mmap_buffer) {
            return 0;
        }
        return mmap_buffer[index * channels + channel];
    }


    void StartSmoothSpeedChange() {
        if (smooth_speed_running.load()) {
            return; // Already running
        }

        should_exit_smooth_speed.store(false);
        smooth_speed_running.store(true);

        smooth_speed_thread = std::thread([this]() {
            smooth_speed_change();
        });
    }

    void StopSmoothSpeedChange() {
        should_exit_smooth_speed.store(true);
        smooth_speed_running.store(false);

        if (smooth_speed_thread.joinable()) {
            smooth_speed_thread.join();
        }
    }

    void smooth_speed_change() {
        // FAST RESPONSE + SMOOTHNESS: Optimized intervals
        const int normal_interval = 8; // 8ms for normal operations - fast response
        const int pause_interval = 5; // 5ms for pause - fast
        const int smooth_interval = 2; // 2ms for shuttle - smooth but responsive

        // --- Lambda for Stateless Volume Calculation (defined locally) ---
        auto calculate_and_set_volume = [&](double current_rate) {
            float new_volume = 1.0f; // Default to full volume
            if (current_rate <= 0.3) { // Start fading below 0.3x
                new_volume = static_cast<float>(current_rate / 0.3); // Linear fade from 0.3x down to 0 (cast needed)
            } else if (current_rate >= 7.0) {
                if (current_rate < 10.0) { // Fade from 7x to 10x
                    float t = static_cast<float>((current_rate - 7.0) / (10.0 - 7.0)); // Range is now 3.0
                    new_volume = 1.0f - (t * 0.85f);
                } else { // Fade further from 10x to 24x
                    const float start_speed = 10.0f;
                    const float end_speed = 24.0f;
                    const float start_volume = 0.15f;
                    const float end_volume = 0.05f;
                    // Clamp speed to the fade range [10, 24]
                    float clamped_rate = std::min(static_cast<float>(current_rate), end_speed);
                    // Calculate progress within the 10-24 range
                    float t = (clamped_rate - start_speed) / (end_speed - start_speed);
                    // Linear interpolation between start_volume and end_volume
                    new_volume = start_volume + (end_volume - start_volume) * t;
                }
            }
            volume.store(new_volume);
        };
        // --- End Lambda ---

        while (!should_exit_smooth_speed.load()) {
            // Instant speed request (mouse shuttle) with micro-smoothing
            if (instant_speed_requested.load()) {
                double start_rate = playback_speed.load();
                double snap_target = target_playback_speed.load();

                const int smooth_ms = 100;   // total smoothing duration ~40ms
                const int step_ms = 2;      // interval step 2ms
                const int steps = smooth_ms / step_ms;

                for (int s = 1; s <= steps; ++s) {
                    double t = static_cast<double>(s) / steps;
                    // Simple linear interpolation is enough for small duration
                    double rate = start_rate + (snap_target - start_rate) * t;
                    playback_speed.store(rate);
                    calculate_and_set_volume(rate);
                    if (should_exit_smooth_speed.load()) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(step_ms));
                }

                // Ensure exact target hit
                playback_speed.store(snap_target);
                calculate_and_set_volume(snap_target);

                instant_speed_requested.store(false);
                continue;
            }

            double current = playback_speed.load();
            double target = target_playback_speed.load();

            bool is_pausing = (target == 0.0 && current > 0.0);
            bool is_jogging = jog_forward.load() || jog_backward.load();

            int interval = normal_interval; // Default interval

            // --- Fast Response + Smoothness: Adaptive intervals ---
            double speed_diff = std::abs(target - current);
            bool needs_smooth_mode = speed_diff > 1.0; // Smooth mode only for large changes (shuttle)

            if (is_pausing) {
                // Pause: Fast response
                interval = pause_interval; // 5ms - fast pause
            } else if (needs_smooth_mode) {
                // Smooth mode for shuttle (large changes)
                interval = smooth_interval; // 2ms for shuttle smoothness
            } else if (std::abs(target - 3.0) < 0.01) {
                // 3.0x speed - special mode
                interval = 4; // 4ms as in original
            } else {
                // Ordinary Play/Stop operations: Fast response
                interval = normal_interval; // 8ms - fast but smooth
            }
            // --- End Fast Response Control ---

            // --- Prioritize JOGGING check ---
            if (is_jogging) {
                playback_speed.store(JOG_SPEED); // Set speed directly to JOG_SPEED
                calculate_and_set_volume(JOG_SPEED);
            }
            // --- Handle speed changes with smooth ease-out interpolation ---
            else if (current != target) {
                // OPTIMIZATION: Ease-out interpolation instead of linear steps
                // Removes "staircase" effect when changing speed

                double diff = target - current;

                // Ease-out exponential curve: fast at start, smooth at end
                // Formula: new = current + diff * (1 - e^(-k*t))
                // where k is speed coefficient (greater = faster)
                double ease_factor;

                if (needs_smooth_mode) {
                    // Smooth mode for shuttle: slow curve
                    ease_factor = 0.20;  // Smooth change
                } else if (is_pausing) {
                    // Pause: fast curve
                    ease_factor = 0.50;  // Fast response
                } else {
                    // Ordinary changes: medium curve
                    ease_factor = 0.35;  // Balance between speed and smoothness
                }

                // Exponential ease-out interpolation
                double new_speed = current + diff * ease_factor;

                // Precision check - snap to target if very close
                double precision_threshold = needs_smooth_mode ? 0.002 : 0.005;
                if (std::abs(new_speed - target) < precision_threshold) {
                    new_speed = target;
                }

                playback_speed.store(new_speed);
                calculate_and_set_volume(new_speed);

                // Adaptive interval for different modes
                if (is_pausing) {
                    interval = pause_interval; // 5ms for fast pause
                } else {
                    interval = normal_interval; // 8ms for ordinary changes
                }
            }
            // --- No Change Needed ---
            // else { /* current == target and not jogging/resuming */ }

            std::this_thread::sleep_for(std::chrono::milliseconds(interval));
        }
    }

    bool LoadFile(const std::string& filepath) {
        std::cout << "FSTPAudioModuleImpl::LoadFile called with: " << filepath << std::endl;
        
        std::cout << "Analyzing file..." << std::endl;
        if (!AnalyzeFile(filepath)) {
            std::cerr << "AnalyzeFile failed" << std::endl;
            return false;
        }
        std::cout << "File analyzed successfully. Sample rate: " << sample_rate << ", Channels: " << channels << ", Duration: " << duration << std::endl;
        
        std::cout << "Creating mmap buffer..." << std::endl;
        if (!CreateMmapBuffer()) {
            std::cerr << "CreateMmapBuffer failed" << std::endl;
            return false;
        }
        std::cout << "Mmap buffer created successfully" << std::endl;

        if (duration <= FAST_BUFFER_DURATION) {
            std::cout << "Decoding full file (duration <= " << FAST_BUFFER_DURATION << " seconds)" << std::endl;
            return DecodeFullFile(filepath);
        } else {
            std::cout << "Decoding two-stage (duration > " << FAST_BUFFER_DURATION << " seconds)" << std::endl;
            return DecodeTwoStage(filepath);
        }
    }

    void UnloadFile() {
        // FIX: Stop background decoding BEFORE cleaning the buffer!
        // This is critical to avoid race condition when reloading the file
        // CRITICAL: Check ONLY joinable(), not relying on flag!
        if (background_decode_thread.joinable()) {
            std::cout << "Stopping background decode thread..." << std::endl;
            should_stop_background_decode.store(true);
            background_decode_thread.join();
            std::cout << "Background decode thread stopped" << std::endl;
        }
        background_decode_running.store(false);

        StopPlayback();
        CleanupMmap();
        sample_rate = 0;
        channels = 0;
        duration = 0.0;
        total_samples = 0;
        playback_position.store(0.0);
        decoded_samples.store(0);
        fast_buffer_samples.store(0);
        fast_buffer_ready.store(false);
        full_buffer_ready.store(false);
        // first_play is no longer needed - old system
    }

    bool StartPlayback() {
        bool fast_ready = fast_buffer_ready.load();
        bool stream_active = pa_stream && Pa_IsStreamActive(pa_stream);

        std::cout << "StartPlayback: fast_buffer_ready=" << fast_ready
                  << ", stream_active=" << stream_active << std::endl;

        if (!fast_ready) {
            std::cout << "StartPlayback failed: fast buffer not ready" << std::endl;
            return false;
        }

        if (!stream_active) {
            std::cout << "StartPlayback failed: permanent stream not active" << std::endl;
            return false;
        }

        is_playing.store(true);
        std::cout << "StartPlayback successful - using permanent stream" << std::endl;
        return true;
    }
    
    void RestartStreamForFile() {
        if (pa_stream) {
            Pa_StopStream(pa_stream);
            Pa_CloseStream(pa_stream);
            pa_stream = nullptr;
        }
        
        PaStreamParameters outputParameters;
        int device_index = GetAudioDeviceIndex();
        int buffer_size = GetAudioBufferSize();
        
        if (device_index == -1) {
            outputParameters.device = Pa_GetDefaultOutputDevice();
        } else if (device_index >= 0 && device_index < Pa_GetDeviceCount()) {
            const PaDeviceInfo* info = Pa_GetDeviceInfo(device_index);
            if (info && info->maxOutputChannels > 0) {
                outputParameters.device = device_index;
            } else {
                outputParameters.device = Pa_GetDefaultOutputDevice();
            }
        } else {
            outputParameters.device = Pa_GetDefaultOutputDevice();
        }
        
        outputParameters.channelCount = channels;
        outputParameters.sampleFormat = paInt16;
        outputParameters.suggestedLatency = Pa_GetDeviceInfo(outputParameters.device)->defaultLowOutputLatency;
        outputParameters.hostApiSpecificStreamInfo = nullptr;

        PaError err = Pa_OpenStream(&pa_stream, nullptr, &outputParameters, sample_rate, buffer_size,
                                   paClipOff, AudioCallback, this);

        if (err == paNoError) {
            playback_speed.store(0.0);
            err = Pa_StartStream(pa_stream);
            if (err != paNoError) {
                Pa_CloseStream(pa_stream);
                pa_stream = nullptr;
            }
        }
    }

    void StopPlayback() {
        is_playing.store(false);
        playback_speed.store(0.0); // Simply set speed to zero
        // Stream continues working, but outputs silence
    }
    
    bool RestartAudioStream() {
        if (!pa_initialized || !fast_buffer_ready.load()) {
            return false;
        }
        
        // Stop current stream
        StopPlayback();
        
        // Start new stream with updated settings
        return StartPlayback();
    }

private:
    static int AudioCallback(const void* inputBuffer, void* outputBuffer, unsigned long framesPerBuffer,
                           const PaStreamCallbackTimeInfo* timeInfo, PaStreamCallbackFlags statusFlags, void* userData) {
        (void)inputBuffer; (void)timeInfo; (void)statusFlags;

        // PROFILING AUDIO CALLBACK: Measure time of each call
        static std::atomic<uint64_t> total_callback_us{0};
        static std::atomic<int> callback_count{0};
        static auto last_report = std::chrono::steady_clock::now();

        auto callback_start = std::chrono::high_resolution_clock::now();

        FSTPAudioModuleImpl* impl = static_cast<FSTPAudioModuleImpl*>(userData);
        int16_t* output = static_cast<int16_t*>(outputBuffer);

        if (!impl->mmap_buffer) {
            std::memset(output, 0, framesPerBuffer * impl->channels * sizeof(int16_t));

            // PROFILING: Early exit (no buffer)
            auto callback_end = std::chrono::high_resolution_clock::now();
            uint64_t callback_us = std::chrono::duration_cast<std::chrono::microseconds>(callback_end - callback_start).count();
            total_callback_us.fetch_add(callback_us);
            callback_count.fetch_add(1);

            return paContinue;
        }

        

        double position = impl->playback_position.load();
        bool is_reverse = impl->is_reverse.load();

        // Determine available samples depending on buffer state
        size_t available_samples;
        bool full_ready = impl->full_buffer_ready.load();
        bool fast_ready = impl->fast_buffer_ready.load();
        size_t decoded_samples = impl->decoded_samples.load();
        size_t fast_buffer_samples = impl->fast_buffer_samples.load();

        if (full_ready) {
            available_samples = decoded_samples;
        } else if (fast_ready) {
            // If full buffer is not ready, but fast buffer is ready,
            // use fast buffer size, but allow playback to continue beyond it (silence until full buffer is ready)
            // continue beyond it (silence until full buffer is ready)
            available_samples = impl->total_samples;
        } else {
            available_samples = decoded_samples;
        }


        // OPTIMIZATION: Smooth speed interpolation inside audio callback
        // Removes "staircase" effect when changing speed in another thread
        const unsigned long SPEED_UPDATE_INTERVAL = 64;

        double target_speed = impl->playback_speed.load();
        if (is_reverse) target_speed = -target_speed;

        // Cache last speed for smooth interpolation
        // FIXED: Now per-instance (in impl->cached_speed) instead of static!
        double speed_start = impl->cached_speed;
        double speed_end = target_speed;

        // If speed is close to zero, output silence
        if (std::abs(target_speed) < 0.01) {
            std::memset(output, 0, framesPerBuffer * impl->channels * sizeof(int16_t));
            // Reset levels when no signal
            impl->audio_level_left.store(0.0f);
            impl->audio_level_right.store(0.0f);
            impl->cached_speed = target_speed;

            // PROFILING: Early exit (speed ~0)
            auto callback_end = std::chrono::high_resolution_clock::now();
            uint64_t callback_us = std::chrono::duration_cast<std::chrono::microseconds>(callback_end - callback_start).count();
            total_callback_us.fetch_add(callback_us);
            callback_count.fetch_add(1);

            return paContinue;
        }

        for (unsigned long frame = 0; frame < framesPerBuffer; frame++) {
            // Linear speed interpolation inside buffer for perfect smoothness
            double t = static_cast<double>(frame) / static_cast<double>(framesPerBuffer);
            double speed = speed_start + (speed_end - speed_start) * t;

            // Update target every SPEED_UPDATE_INTERVAL frames
            if (frame % SPEED_UPDATE_INTERVAL == 0) {
                double new_target = impl->playback_speed.load();
                if (is_reverse) new_target = -new_target;

                // Smooth transition to new target speed
                if (std::abs(new_target - speed_end) > 0.001) {
                    speed_start = speed;
                    speed_end = new_target;
                    t = 0.0;
                }
            }
            // Position in samples with fractional part
            double exact_position = position / impl->channels;
            size_t base_index = static_cast<size_t>(exact_position);
            double fractional = exact_position - base_index;

            // Dynamic check of boundaries with consideration of buffer states
            size_t max_available_samples;
            size_t actual_decoded;

            if (impl->full_buffer_ready.load()) {
                actual_decoded = impl->decoded_samples.load();
                max_available_samples = actual_decoded / impl->channels;
            } else if (impl->fast_buffer_ready.load()) {
                actual_decoded = impl->fast_buffer_samples.load();
                max_available_samples = actual_decoded / impl->channels;
            } else {
                actual_decoded = impl->decoded_samples.load();
                max_available_samples = actual_decoded / impl->channels;
            }

            if (base_index >= max_available_samples - 3 || base_index == 0) {
                // Outside boundaries - silence
                for (int ch = 0; ch < impl->channels; ch++) {
                    output[frame * impl->channels + ch] = 0;
                }
            } else {
                // Catmull-Rom interpolation for each channel
                for (int ch = 0; ch < impl->channels; ch++) {
                    // Get 4 neighboring samples for Catmull-Rom
                    double p0 = impl->GetSafeSample(base_index - 1, ch, max_available_samples);
                    double p1 = impl->GetSafeSample(base_index, ch, max_available_samples);
                    double p2 = impl->GetSafeSample(base_index + 1, ch, max_available_samples);
                    double p3 = impl->GetSafeSample(base_index + 2, ch, max_available_samples);

                    // Apply Catmull-Rom interpolation
                    double interpolated = impl->CatmullRomInterpolate(p0, p1, p2, p3, fractional);

                    // Apply attenuation at low speeds to prevent clicks
                    double abs_speed = std::abs(speed);
                    if (abs_speed <= 0.3) {
                        // Linear attenuation from 0.3x to 0.0x
                        double speed_attenuation = abs_speed / 0.3;
                        interpolated *= speed_attenuation;
                    }
                    
                    // Apply master volume from settings
                    float master_volume = GetMasterVolume();
                    interpolated *= master_volume;

                    // Limit value within int16_t
                    interpolated = std::max(-32768.0, std::min(32767.0, interpolated));
                    output[frame * impl->channels + ch] = static_cast<int16_t>(interpolated);
                }
            }

            position += speed * impl->channels;
            if (position < 0) position = 0;
            // Prevent looping at 12 minutes - allow position to continue growing
            // even if full buffer is not ready yet
            if (position >= available_samples) {
                if (impl->full_buffer_ready.load()) {
                    // If full buffer is ready, use real size
                    size_t total_decoded = impl->decoded_samples.load();
                    if (position >= total_decoded) {
                        position = total_decoded - 1;
                    }
                } else {
                    // If full buffer is not ready, but position exceeds available,
                    // simply continue - this will allow playback to continue
                    // when buffer will be ready
                    if (position >= impl->total_samples) {
                        position = impl->total_samples - 1;
                    }
                }
            }
        }

        // OPTIMIZATION: Calculate VU meters only every 4th callback (reduces CPU)
        static int vu_meter_skip_counter = 0;
        if (++vu_meter_skip_counter >= 4) {
            vu_meter_skip_counter = 0;

            float sum_left = 0.0f, sum_right = 0.0f;
            float peak_left = 0.0f, peak_right = 0.0f;

            for (unsigned long frame = 0; frame < framesPerBuffer; frame++) {
                if (impl->channels >= 1) {
                    float sample_left = static_cast<float>(output[frame * impl->channels]) / 32768.0f;
                    float abs_left = std::abs(sample_left);
                    sum_left += abs_left;
                    peak_left = std::max(peak_left, abs_left);
                }

                if (impl->channels >= 2) {
                    float sample_right = static_cast<float>(output[frame * impl->channels + 1]) / 32768.0f;
                    float abs_right = std::abs(sample_right);
                    sum_right += abs_right;
                    peak_right = std::max(peak_right, abs_right);
                } else if (impl->channels == 1) {
                    // Mono: copy left channel to right for VU meters
                    float sample = static_cast<float>(output[frame * impl->channels]) / 32768.0f;
                    float abs_sample = std::abs(sample);
                    sum_right += abs_sample;
                    peak_right = std::max(peak_right, abs_sample);
                }
            }

            // Average levels (RMS-like value)
            float rms_left = sum_left / static_cast<float>(framesPerBuffer);
            float rms_right = sum_right / static_cast<float>(framesPerBuffer);

            // Save raw data without smoothing (smoothing will be in OSD)
            impl->audio_level_left.store(rms_left);
            impl->audio_level_right.store(rms_right);
            impl->audio_peak_left.store(peak_left);
            impl->audio_peak_right.store(peak_right);
        }

        impl->playback_position.store(position);

        // Save final speed for next callback
        impl->cached_speed = speed_end;

        // PROFILING: Write callback time
        auto callback_end = std::chrono::high_resolution_clock::now();
        uint64_t callback_us = std::chrono::duration_cast<std::chrono::microseconds>(callback_end - callback_start).count();
        total_callback_us.fetch_add(callback_us);
        callback_count.fetch_add(1);

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_report).count() >= 1) {
            int count = callback_count.load();
            if (count > 0) {
                uint64_t avg_us = total_callback_us.load() / count;
                uint64_t total_ms_per_sec = (total_callback_us.load() / 1000);
                double cpu_percent = (total_ms_per_sec / 10.0); // ms/sec → % (out of 1000ms)

                std::cout << "🎵 [AUDIO] Callbacks: " << count << "/sec"
                          << ", Avg: " << avg_us << "μs"
                          << ", Total: " << total_ms_per_sec << "ms/sec"
                          << " ≈ " << cpu_percent << "% CPU" << std::endl;
            }

            total_callback_us.store(0);
            callback_count.store(0);
            last_report = now;
        }

        return paContinue;
    }

    bool AnalyzeFile(const std::string& filepath) {
        AVFormatContext* format_ctx = nullptr;
        if (avformat_open_input(&format_ctx, filepath.c_str(), nullptr, nullptr) != 0 ||
            avformat_find_stream_info(format_ctx, nullptr) < 0) {
            return false;
        }

        const AVCodec* codec = nullptr;
        int audio_stream_index = av_find_best_stream(format_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
        if (audio_stream_index < 0) {
            avformat_close_input(&format_ctx);
            return false;
        }

        sample_rate = format_ctx->streams[audio_stream_index]->codecpar->sample_rate;
        channels = format_ctx->streams[audio_stream_index]->codecpar->ch_layout.nb_channels;
        if (format_ctx->duration != AV_NOPTS_VALUE) {
            duration = static_cast<double>(format_ctx->duration) / AV_TIME_BASE;
        }
        total_samples = static_cast<size_t>(duration * sample_rate * channels * 1.1);

        // Get codec name
        if (codec && codec->long_name) {
            codec_name = codec->long_name;
        } else if (codec && codec->name) {
            codec_name = codec->name;
        } else {
            codec_name = "Unknown";
        }

        avformat_close_input(&format_ctx);
        return true;
    }

    bool CreateMmapBuffer() {
        mmap_size_bytes = total_samples * sizeof(int16_t);
        char temp_template[] = "/tmp/fstp_audio_XXXXXX";
        mmap_fd = mkstemp(temp_template);
        if (mmap_fd == -1 || ftruncate(mmap_fd, mmap_size_bytes) == -1) return false;

        temp_filename = temp_template;

        // CRITICAL: Unlink file immediately after creation
        // File remains accessible via fd, but automatically deleted on close
        // This prevents accumulation of temp files if process crashes
        unlink(temp_filename.c_str());
        std::cout << "🗑️  [MMAP] Temp file unlinked (auto-cleanup on close): " << temp_filename << std::endl;

        mmap_buffer = static_cast<int16_t*>(mmap(nullptr, mmap_size_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, mmap_fd, 0));
        return (mmap_buffer != MAP_FAILED);
    }

    void CleanupMmap() {
        if (mmap_buffer && mmap_buffer != MAP_FAILED) {
            munmap(mmap_buffer, mmap_size_bytes);
            mmap_buffer = nullptr;
        }
        if (mmap_fd != -1) {
            close(mmap_fd);
            mmap_fd = -1;
        }
        // File already unlinked in CreateMmapBuffer()
        temp_filename.clear();
    }

    bool DecodeFullFile(const std::string& filepath) {
        size_t decoded = DecodeToBuffer(filepath, 0, total_samples);
        if (decoded > 0) {
            decoded_samples.store(decoded);
            fast_buffer_ready.store(true);
            full_buffer_ready.store(true);
            
            // Start permanent stream after successful decode
            // StartPermanentAudioStream() will check if reinitialization is needed
            std::cout << "Starting/checking permanent stream after successful decode (SR:" << sample_rate << ", CH:" << channels << ")..." << std::endl;
            if (!StartPermanentAudioStream()) {
                std::cout << "ERROR: Failed to start/reinitialize permanent stream after decode!" << std::endl;
            }
            return true;
        }
        return false;
    }

    bool DecodeTwoStage(const std::string& filepath) {
        size_t fast_samples = static_cast<size_t>(FAST_BUFFER_DURATION * sample_rate * channels);
        size_t decoded_fast = DecodeToBuffer(filepath, 0, fast_samples);

        if (decoded_fast > 0) {
            fast_buffer_samples.store(decoded_fast);
            decoded_samples.store(decoded_fast);
            fast_buffer_ready.store(true);

            // Start permanent stream after decoding fast buffer
            // StartPermanentAudioStream() will check if reinitialization is needed
            std::cout << "Starting/checking permanent stream after fast buffer ready (SR:" << sample_rate << ", CH:" << channels << ")..." << std::endl;
            if (!StartPermanentAudioStream()) {
                std::cout << "ERROR: Failed to start/reinitialize permanent stream after fast buffer!" << std::endl;
            }

            // FIXED: Stop old stream if it is still running
            // CRITICAL: Check ONLY joinable(), not relying on flag!
            if (background_decode_thread.joinable()) {
                should_stop_background_decode.store(true);
                background_decode_thread.join();
            }

            should_stop_background_decode.store(false);
            background_decode_running.store(true);

            background_decode_thread = std::thread([this, filepath, decoded_fast]() {
                size_t remaining = DecodeToBuffer(filepath, decoded_fast, total_samples - decoded_fast, true);
                if (remaining > 0) {
                    decoded_samples.store(decoded_fast + remaining);
                    full_buffer_ready.store(true);
                    std::cout << "Background decoding completed. Total samples: " << (decoded_fast + remaining) << std::endl;
                }
                background_decode_running.store(false);
            });

            return true;
        }
        return false;
    }

    size_t DecodeToBuffer(const std::string& filepath, size_t start_sample, size_t max_samples, bool is_background = false) {
        // CRITICAL PROTECTION: block FFmpeg decoding if there are problems with PortAudio
        static std::atomic<bool> ffmpeg_audio_blocked{false};
        if (ffmpeg_audio_blocked.load()) {
            std::cout << "DecodeToBuffer: FFmpeg audio decoding blocked due to conflicts" << std::endl;
            return 0;
        }

        if (is_background) {
            std::cout << "DecodeToBuffer: starts FFmpeg background decoding (with pauses to reduce load)..." << std::endl;
        } else {
            std::cout << "DecodeToBuffer: starts FFmpeg decoding..." << std::endl;
        }

        AVFormatContext* format_ctx = nullptr;
        AVCodecContext* codec_ctx = nullptr;
        const AVCodec* codec = nullptr;
        AVFrame* frame = nullptr;
        AVPacket* packet = av_packet_alloc();

        if (avformat_open_input(&format_ctx, filepath.c_str(), nullptr, nullptr) != 0 ||
            avformat_find_stream_info(format_ctx, nullptr) < 0) {
            return 0;
        }

        int audio_stream_index = av_find_best_stream(format_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
        if (audio_stream_index < 0) {
            avformat_close_input(&format_ctx);
            return 0;
        }

        codec_ctx = avcodec_alloc_context3(codec);
        if (avcodec_parameters_to_context(codec_ctx, format_ctx->streams[audio_stream_index]->codecpar) < 0 ||
            avcodec_open2(codec_ctx, codec, nullptr) < 0) {
            avformat_close_input(&format_ctx);
            return 0;
        }

        frame = av_frame_alloc();
        size_t current_sample = 0;
        size_t write_offset = start_sample;
        int ret;

        // Counter for background decoding - add pauses to reduce CPU spikes
        int packet_counter = 0;
        const int PACKETS_PER_PAUSE = 100;     // Every 100 packets - pause
        const int PAUSE_MS = 10;            // Pause 10ms to reduce load

        // If start_sample > 0, need to go to the desired position in the file
        if (start_sample > 0) {
            // Calculate start time in seconds
            double start_time_seconds = static_cast<double>(start_sample) / (sample_rate * channels);

            // Convert to AVStream time base
            AVStream* stream = format_ctx->streams[audio_stream_index];
            int64_t seek_target = static_cast<int64_t>(start_time_seconds * stream->time_base.den / stream->time_base.num);

            std::cout << "DecodeToBuffer: seeking to " << start_time_seconds << "s (sample " << start_sample << ")" << std::endl;

            // Perform seek
            if (av_seek_frame(format_ctx, audio_stream_index, seek_target, AVSEEK_FLAG_BACKWARD) < 0) {
                std::cout << "DecodeToBuffer: seek failed, starting from beginning" << std::endl;
            } else {
                // Reset codec after seek
                avcodec_flush_buffers(codec_ctx);
            }
        }

        while ((ret = av_read_frame(format_ctx, packet)) >= 0 && current_sample < max_samples) {
            // FIXED: Check background decoding stop flag
            if (is_background && should_stop_background_decode.load()) {
                std::cout << "DecodeToBuffer: stop background decoding by request" << std::endl;
                av_packet_unref(packet);
                break;
            }

            if (packet->stream_index == audio_stream_index) {
                // OPTIMIZATION: Background decoding with pauses to reduce CPU load
                if (is_background) {
                    packet_counter++;
                    if (packet_counter >= PACKETS_PER_PAUSE) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(PAUSE_MS));
                        packet_counter = 0;
                    }
                }

                ret = avcodec_send_packet(codec_ctx, packet);
                if (ret < 0) {
                    av_packet_unref(packet);
                    continue;
                }

                while (ret >= 0 && current_sample < max_samples) {
                    ret = avcodec_receive_frame(codec_ctx, frame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                    if (ret < 0) break;

                    int samples_per_channel = frame->nb_samples;
                    int frame_channels = frame->ch_layout.nb_channels;

                    for (int i = 0; i < samples_per_channel && current_sample < max_samples; i++) {
                        for (int ch = 0; ch < frame_channels && current_sample < max_samples; ch++) {
                            int16_t sample_value = 0;

                            if (codec_ctx->sample_fmt == AV_SAMPLE_FMT_FLTP) {
                                float* channel_data = reinterpret_cast<float*>(frame->data[ch]);
                                float sample = std::max(-1.0f, std::min(1.0f, channel_data[i]));
                                sample_value = static_cast<int16_t>(sample * 32767.0f);
                            } else if (codec_ctx->sample_fmt == AV_SAMPLE_FMT_S16) {
                                int16_t* data = reinterpret_cast<int16_t*>(frame->data[0]);
                                sample_value = data[i * frame_channels + ch];
                            }

                            if (write_offset < total_samples) {
                                mmap_buffer[write_offset] = sample_value;
                                write_offset++;
                                current_sample++;
                            }
                        }
                    }
                }
            }
            av_packet_unref(packet);
        }

        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&format_ctx);
        return current_sample;
    }

    // Functions to manage old system
    void toggle_pause() {
        if (target_playback_speed.load() == 0.0) {
            target_playback_speed.store(1.0);
        } else {
            target_playback_speed.store(0.0);
        }
    }

    void set_jog_forward(bool enable) {
        jog_forward.store(enable);
        if (!enable) {
            target_playback_speed.store(1.0); // Return to normal speed
        }
    }

    void set_jog_backward(bool enable) {
        jog_backward.store(enable);
        if (!enable) {
            target_playback_speed.store(1.0); // Return to normal speed
        }
    }
};

    // Implementation of wrapper

FSTPAudioModuleWrapper::FSTPAudioModuleWrapper() : m_impl(std::make_unique<FSTPAudioModuleImpl>()) {
}

FSTPAudioModuleWrapper::~FSTPAudioModuleWrapper() {
}

bool FSTPAudioModuleWrapper::Initialize() {
    if (m_initialized) return true;
    m_initialized = m_impl->Initialize();
    return m_initialized;
}

void FSTPAudioModuleWrapper::Shutdown() {
    if (m_initialized) {
        UnloadFile();
        m_impl->Cleanup();
        m_initialized = false;
    }
}

bool FSTPAudioModuleWrapper::LoadFile(const std::string& filepath) {
    std::cout << "FSTPAudioModuleWrapper::LoadFile called with: " << filepath << std::endl;
    
    if (!m_initialized) {
        std::cerr << "Audio module not initialized" << std::endl;
        return false;
    }

    if (m_loaded) {
        std::cout << "Unloading previous file" << std::endl;
        UnloadFile();
    }

    std::cout << "Calling m_impl->LoadFile..." << std::endl;
    if (m_impl->LoadFile(filepath)) {
        std::cout << "File loaded successfully, duration: " << m_impl->duration << " seconds" << std::endl;
        m_current_file = filepath;
        m_loaded = true;
        m_duration.store(m_impl->duration);
        return true;
    } else {
        std::cerr << "m_impl->LoadFile failed" << std::endl;
    }
    return false;
}

void FSTPAudioModuleWrapper::UnloadFile() {
    if (m_loaded) {
        Stop();
        m_impl->UnloadFile();
        m_current_file.clear();
        m_loaded = false;
        m_duration.store(0.0);
    }
}

bool FSTPAudioModuleWrapper::Play() {
    if (!m_loaded) return false;

    if (m_impl) {
        // Start old system smooth speed change
        m_impl->StartSmoothSpeedChange();

        // Set target speed 1.0 for start
        m_impl->target_playback_speed.store(1.0);
        std::cout << "Play(): starting legacy smooth_speed_change system" << std::endl;
    }

    bool result = m_impl->StartPlayback();
    if (result) {
        m_playing.store(true);
    }
    return result;
}

bool FSTPAudioModuleWrapper::Pause() {
    if (!m_loaded) return false;

    // Use old system - simply set target speed 0
    if (m_impl) {
        // Instantly, but with short micro-smoothing to zero
        m_impl->target_playback_speed.store(0.0);
        m_impl->instant_speed_requested.store(true);
        std::cout << "Pause(): short ease to 0.0" << std::endl;
    }

    m_playing.store(false);
    return true;
}

bool FSTPAudioModuleWrapper::Stop() {
    if (!m_loaded) return false;

    // Use old system - simply set target speed 0
    if (m_impl) {
        m_impl->target_playback_speed.store(0.0);
        m_impl->instant_speed_requested.store(true);
        m_impl->playback_position.store(0.0); // Reset position
        std::cout << "Stop(): short ease to 0.0 and reset position" << std::endl;
    }

    m_playing.store(false);
    return true;
}

void FSTPAudioModuleWrapper::SetSpeed(double speed) {
    m_speed.store(speed);
    if (m_impl) {
        // Use old system - simply set target speed
        m_impl->target_playback_speed.store(speed);
        std::cout << "SetSpeed(" << speed << "): setting target_playback_speed" << std::endl;
    }
}

void FSTPAudioModuleWrapper::SetSpeedInstant(double speed) {
    m_speed.store(speed);
    if (m_impl) {
        // For Mouse Shuttle - instantly set speed (old system works better)
        m_impl->target_playback_speed.store(speed);
        m_impl->instant_speed_requested.store(true);
        // std::cout << "SetSpeedInstant(" << speed << "): instantly set target_playback_speed" << std::endl;
    }
}

void FSTPAudioModuleWrapper::SetReverse(bool reverse) {
    m_reverse.store(reverse);
    if (m_impl) {
        m_impl->is_reverse.store(reverse);
    }
}

void FSTPAudioModuleWrapper::SetPosition(double position_seconds) {
    if (m_impl && m_loaded) {
        double sample_pos = position_seconds * m_impl->sample_rate * m_impl->channels;
        sample_pos = std::max(0.0, std::min(sample_pos, static_cast<double>(m_impl->decoded_samples.load())));
        m_impl->playback_position.store(sample_pos);
    }
}

double FSTPAudioModuleWrapper::GetPosition() const {
    if (m_impl && m_loaded && m_impl->sample_rate > 0 && m_impl->channels > 0) {
        return m_impl->playback_position.load() / (m_impl->sample_rate * m_impl->channels);
    }
    return 0.0;
}

double FSTPAudioModuleWrapper::GetDuration() const {
    return m_duration.load();
}

bool FSTPAudioModuleWrapper::IsPlaying() const {
    return m_playing.load();
}

bool FSTPAudioModuleWrapper::IsLoaded() const {
    return m_loaded;
}

double FSTPAudioModuleWrapper::GetSpeed() const {
    return m_speed.load();
}

double FSTPAudioModuleWrapper::GetActualSpeed() const {
    if (!m_impl || !m_loaded) {
        return 0.0;
    }
    return m_impl->playback_speed.load();
}

bool FSTPAudioModuleWrapper::IsReverse() const {
    return m_reverse.load();
}

bool FSTPAudioModuleWrapper::IsFastBufferReady() const {
    return m_impl && m_impl->fast_buffer_ready.load();
}

bool FSTPAudioModuleWrapper::IsFullBufferReady() const {
    return m_impl && m_impl->full_buffer_ready.load();
}

int FSTPAudioModuleWrapper::GetSampleRate() const {
    if (!m_impl || !m_loaded) {
        return 0;
    }
    return m_impl->sample_rate;
}

int FSTPAudioModuleWrapper::GetChannels() const {
    if (!m_impl || !m_loaded) {
        return 0;
    }
    return m_impl->channels;
}

std::string FSTPAudioModuleWrapper::GetAudioCodecName() const {
    if (!m_impl || !m_loaded) {
        return "Unknown";
    }
    return m_impl->codec_name;
}

float FSTPAudioModuleWrapper::GetAudioLevelLeft() const {
    if (!m_impl || !m_loaded) {
        return 0.0f;
    }
    return m_impl->audio_level_left.load();
}

float FSTPAudioModuleWrapper::GetAudioLevelRight() const {
    if (!m_impl || !m_loaded) {
        return 0.0f;
    }
    return m_impl->audio_level_right.load();
}

float FSTPAudioModuleWrapper::GetAudioPeakLeft() const {
    if (!m_impl || !m_loaded) {
        return 0.0f;
    }
    return m_impl->audio_peak_left.load();
}

float FSTPAudioModuleWrapper::GetAudioPeakRight() const {
    if (!m_impl || !m_loaded) {
        return 0.0f;
    }
    return m_impl->audio_peak_right.load();
}

bool FSTPAudioModuleWrapper::RestartAudioStream() {
    if (!m_impl || !m_loaded) {
        return false;
    }
    return m_impl->RestartAudioStream();
}

// Accessor methods for API
int FSTPAudioModuleWrapper::GetSampleRateInternal() const {
    if (!m_impl || !m_loaded) return 0;
    return m_impl->sample_rate;
}

int FSTPAudioModuleWrapper::GetChannelsInternal() const {
    if (!m_impl || !m_loaded) return 0;
    return m_impl->channels;
}

double FSTPAudioModuleWrapper::GetPlaybackPositionInternal() const {
    if (!m_impl || !m_loaded) return 0.0;
    return m_impl->playback_position.load();
}

void FSTPAudioModuleWrapper::SetPlaybackPositionInternal(double sample_position) {
    if (m_impl && m_loaded) {
        m_impl->playback_position.store(sample_position);
    }
}

size_t FSTPAudioModuleWrapper::GetDecodedSamplesInternal() const {
    if (!m_impl || !m_loaded) return 0;
    return m_impl->decoded_samples.load();
}