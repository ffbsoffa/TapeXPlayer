#include "FSTPAudioModule_wrapper.h"
#include <iostream>
#include <iomanip>
#include <vector>
#include <thread>
#include <mutex>
#include <cstring>
#define _USE_MATH_DEFINES
#include <cmath>
#include <chrono>
#include <atomic>
#include <random>

// Ensure M_PI is defined (for elastic ease function)
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// NEON SIMD for Apple Silicon optimization
#if defined(__aarch64__) || defined(__ARM_NEON)
#include <arm_neon.h>
#define USE_NEON_SIMD 1

// NEON-optimized float to int16 conversion (8 samples at once)
inline void neon_float_to_int16(const float* src, int16_t* dst, size_t count) {
    size_t simd_count = count & ~7; // Process 8 samples at a time

    const float32x4_t scale = vdupq_n_f32(32767.0f);
    const float32x4_t min_val = vdupq_n_f32(-1.0f);
    const float32x4_t max_val = vdupq_n_f32(1.0f);

    for (size_t i = 0; i < simd_count; i += 8) {
        // Load 8 floats (2 vectors of 4)
        float32x4_t f0 = vld1q_f32(src + i);
        float32x4_t f1 = vld1q_f32(src + i + 4);

        // Clamp to [-1.0, 1.0]
        f0 = vmaxq_f32(min_val, vminq_f32(max_val, f0));
        f1 = vmaxq_f32(min_val, vminq_f32(max_val, f1));

        // Scale to int16 range
        f0 = vmulq_f32(f0, scale);
        f1 = vmulq_f32(f1, scale);

        // Convert to int32, then to int16
        int32x4_t i0 = vcvtq_s32_f32(f0);
        int32x4_t i1 = vcvtq_s32_f32(f1);

        // Narrow to int16 and store
        int16x4_t s0 = vmovn_s32(i0);
        int16x4_t s1 = vmovn_s32(i1);
        int16x8_t result = vcombine_s16(s0, s1);

        vst1q_s16(dst + i, result);
    }

    // Handle remaining samples (scalar fallback)
    for (size_t i = simd_count; i < count; i++) {
        float sample = std::max(-1.0f, std::min(1.0f, src[i]));
        dst[i] = static_cast<int16_t>(sample * 32767.0f);
    }
}

// NEON-optimized memcpy for audio samples (64-byte aligned transfers)
inline void neon_copy_samples(int16_t* dst, const int16_t* src, size_t count) {
    size_t simd_count = count & ~15; // Process 16 samples (32 bytes) at a time

    for (size_t i = 0; i < simd_count; i += 16) {
        int16x8_t v0 = vld1q_s16(src + i);
        int16x8_t v1 = vld1q_s16(src + i + 8);
        vst1q_s16(dst + i, v0);
        vst1q_s16(dst + i + 8, v1);
    }

    // Handle remaining samples
    for (size_t i = simd_count; i < count; i++) {
        dst[i] = src[i];
    }
}

// NEON-optimized int32 to int16 conversion (4 samples at once)
inline void neon_int32_to_int16(const int32_t* src, int16_t* dst, size_t count) {
    size_t simd_count = count & ~3; // Process 4 samples at a time

    for (size_t i = 0; i < simd_count; i += 4) {
        int32x4_t i32 = vld1q_s32(src + i);
        // Shift right by 16 bits to convert 32-bit to 16-bit range
        int32x4_t shifted = vshrq_n_s32(i32, 16);
        int16x4_t i16 = vmovn_s32(shifted);
        vst1_s16(dst + i, i16);
    }

    // Handle remaining samples
    for (size_t i = simd_count; i < count; i++) {
        dst[i] = static_cast<int16_t>(src[i] >> 16);
    }
}

// NEON-optimized float interleaved to int16 conversion
inline void neon_float_interleaved_to_int16(const float* src, int16_t* dst, size_t count) {
    size_t simd_count = count & ~7; // Process 8 samples at a time

    const float32x4_t scale = vdupq_n_f32(32767.0f);
    const float32x4_t min_val = vdupq_n_f32(-1.0f);
    const float32x4_t max_val = vdupq_n_f32(1.0f);

    for (size_t i = 0; i < simd_count; i += 8) {
        float32x4_t f0 = vld1q_f32(src + i);
        float32x4_t f1 = vld1q_f32(src + i + 4);

        f0 = vmaxq_f32(min_val, vminq_f32(max_val, f0));
        f1 = vmaxq_f32(min_val, vminq_f32(max_val, f1));

        f0 = vmulq_f32(f0, scale);
        f1 = vmulq_f32(f1, scale);

        int32x4_t i0 = vcvtq_s32_f32(f0);
        int32x4_t i1 = vcvtq_s32_f32(f1);

        int16x4_t s0 = vmovn_s32(i0);
        int16x4_t s1 = vmovn_s32(i1);
        int16x8_t result = vcombine_s16(s0, s1);

        vst1q_s16(dst + i, result);
    }

    // Handle remaining samples
    for (size_t i = simd_count; i < count; i++) {
        float sample = std::max(-1.0f, std::min(1.0f, src[i]));
        dst[i] = static_cast<int16_t>(sample * 32767.0f);
    }
}
#endif

// Headers for memory mapping
#ifdef _WIN32
#include "../FSTPMainModule/WSGUI/windows/FSTPMemoryMap.h"
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <windows.h>
#define mkstemp _mktemp_s
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
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

// ── Windows low-latency device selection ────────────────────────────────────────
// On Windows, PortAudio's Pa_GetDefaultOutputDevice() returns the MME device, whose output
// latency floor is ~80–200ms regardless of buffer size — this is why audio/transport felt
// laggier on Windows than on macOS (CoreAudio) or Linux (ALSA/PipeWire). We instead prefer the
// WASAPI host API (shared mode → ~10–30ms), keeping the user's chosen device by matching on name.
//
// `desired` is a global PortAudio device index (-1 = system default). Returns a global device
// index to actually open. On non-Windows this is a passthrough (the platform defaults are already
// low-latency), so behaviour there is unchanged.
static PaDeviceIndex ResolveLowLatencyDevice(PaDeviceIndex desired) {
#if defined(_WIN32)
    // Find the WASAPI host API. If PortAudio was built without WASAPI, bail out to `desired`.
    PaHostApiIndex wasapiApi = Pa_HostApiTypeIdToHostApiIndex(paWASAPI);
    if (wasapiApi < 0) {
        return (desired == paNoDevice) ? Pa_GetDefaultOutputDevice() : desired;
    }
    const PaHostApiInfo* wasapiInfo = Pa_GetHostApiInfo(wasapiApi);
    if (!wasapiInfo) {
        return (desired == paNoDevice) ? Pa_GetDefaultOutputDevice() : desired;
    }

    // If a specific device was requested, try to find the WASAPI device with the SAME name
    // (the same physical output is exposed once per host API). Fall back to the WASAPI default.
    const char* wantName = nullptr;
    if (desired != paNoDevice && desired >= 0 && desired < Pa_GetDeviceCount()) {
        const PaDeviceInfo* di = Pa_GetDeviceInfo(desired);
        // Already a WASAPI device? Keep it as-is.
        if (di && di->hostApi == wasapiApi) return desired;
        if (di) wantName = di->name;
    }

    if (wantName) {
        for (int i = 0; i < wasapiInfo->deviceCount; ++i) {
            PaDeviceIndex dev = Pa_HostApiDeviceIndexToDeviceIndex(wasapiApi, i);
            const PaDeviceInfo* di = Pa_GetDeviceInfo(dev);
            if (di && di->maxOutputChannels > 0 && di->name && std::strcmp(di->name, wantName) == 0) {
                return dev; // same device, but via WASAPI
            }
        }
    }

    // No name match (or default requested): use the WASAPI default output device.
    if (wasapiInfo->defaultOutputDevice != paNoDevice) {
        return wasapiInfo->defaultOutputDevice;
    }
    return (desired == paNoDevice) ? Pa_GetDefaultOutputDevice() : desired;
#else
    // macOS / Linux: platform default host API is already low-latency.
    return (desired == paNoDevice) ? Pa_GetDefaultOutputDevice() : desired;
#endif
}

// Resolve which output device to open. Backward-compatible with the existing index-based
// selection (audio_device_index; -1 = system default). When a stable device NAME is also saved
// (SetAudioDevice / the pickers), it HEALS a stale index: PortAudio indices shift when devices
// are added/removed between sessions, so if the saved index no longer points at the saved-name
// device we re-find that device by name; if the pinned device is currently absent we fall back
// to the system default until it returns. On Windows the result is routed through
// ResolveLowLatencyDevice() to prefer the WASAPI variant.
static PaDeviceIndex ResolvePreferredOutputDevice() {
    int index = GetAudioDeviceIndex();
    const char* name = GetAudioDeviceName();

    // -1 = follow the system default (existing convention).
    if (index < 0) return ResolveLowLatencyDevice(paNoDevice);

    // Saved index still points at an output device with the saved name? (name empty = legacy → trust index)
    if (index < Pa_GetDeviceCount()) {
        const PaDeviceInfo* di = Pa_GetDeviceInfo(index);
        if (di && di->maxOutputChannels > 0 &&
            (name[0] == '\0' || (di->name && std::strcmp(di->name, name) == 0))) {
            return ResolveLowLatencyDevice(index);
        }
    }

    // Index stale (indices shifted) → re-find the device by its stable name.
    if (name[0] != '\0') {
        int n = Pa_GetDeviceCount();
        for (int i = 0; i < n; ++i) {
            const PaDeviceInfo* di = Pa_GetDeviceInfo(i);
            if (di && di->maxOutputChannels > 0 && di->name && std::strcmp(di->name, name) == 0) {
                std::cout << "🔌 [AUDIO] Device index shifted; re-found '" << name << "' at index " << i << std::endl;
                return ResolveLowLatencyDevice((PaDeviceIndex)i);
            }
        }
        std::cout << "🔌 [AUDIO] Pinned device '" << name << "' not found — using system default" << std::endl;
        return ResolveLowLatencyDevice(paNoDevice);
    }

    // No name to heal with: use the index if it is a valid output device, else default.
    if (index < Pa_GetDeviceCount()) {
        const PaDeviceInfo* di = Pa_GetDeviceInfo(index);
        if (di && di->maxOutputChannels > 0) return ResolveLowLatencyDevice((PaDeviceIndex)index);
    }
    return ResolveLowLatencyDevice(paNoDevice);
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

    // Timecode offset from file metadata (e.g. QuickTime shows 10:00:00:00)
    double timecode_offset_seconds = 0.0;

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

    // === FRAME ALIGNMENT ON PAUSE ===
    // After 10 seconds of pause, gradually align audio time to nearest frame boundary
    // This removes the visible "stripe" artifact from Betacam effect
    std::chrono::steady_clock::time_point pause_start_time;
    std::atomic<bool> pause_time_tracking{false};
    std::atomic<bool> frame_alignment_done{false};
    std::atomic<double> video_frame_rate{25.0};  // Set by video module

    // CRITICAL FIX: Per-instance VU meter skip counter (was static - caused race condition!)
    // Problem: Static counter shared between all player instances caused race condition
    // Solution: Make it per-instance to eliminate thread conflicts
    int vu_meter_skip_counter{0};

    // Elastic ease-out for first play and periodic playbacks
    std::atomic<bool> first_play_after_load{true};  // Track first play after file load
    std::atomic<int> play_count{0};                 // Count Play() calls for periodic elastic effect
    std::atomic<bool> use_elastic_ease{false};      // Flag to trigger elastic ease in smooth_speed_change

    // Direction-change sequencer (ramp down → hold → flip → ramp up)
    std::atomic<bool> direction_change_pending{false};  // Sequencer requested
    std::atomic<bool> pending_reverse_value{false};     // Target direction for sequencer

    // Decoding progress
    std::atomic<size_t> decoded_samples{0};
    std::atomic<size_t> fast_buffer_samples{0}; // Fast buffer size
    std::atomic<bool> fast_buffer_ready{false};
    std::atomic<bool> full_buffer_ready{false};

    // Resume / priority-zone decode: start of the first decoded region.
    // Normally 0 (decode from beginning). When resuming a long file at position T,
    // the priority zone starts at T, so the region [0, decode_range_start) is
    // filled later in background phase 2.
    std::atomic<size_t> decode_range_start{0};

    // PortAudio
    PaStream* pa_stream = nullptr;
    bool pa_initialized = false;

    // CRITICAL: Per-instance cached speed to eliminate race condition between players!
    // Was static - conflicted with multiple players
    double cached_speed = 0.0;

    // Tape-style low-pass filter state (per channel, 1st-order IIR).
    // Reduces HF harshness at non-1x speeds; bypassed at "lock" (0.95x–1.05x).
    float lpf_state[8] = {0.0f};  // supports up to 8 channels

    // CRITICAL FIX: Flag to detect destructor cleanup vs normal cleanup
    // Problem: Pa_CloseStream() → PipeWire's malloc_trim() crashes on heap corrupted by video av_frame_ref()
    // Solution: Skip Pa_CloseStream() during destructor, let OS clean up (leak acceptable on exit)
    bool m_in_destructor_cleanup = false;
    bool reduce_fast_buffer_for_pcm = false;

    static constexpr double FAST_BUFFER_DURATION = 720.0;
    static constexpr double FAST_BUFFER_DURATION_PCM = 120.0;

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

        // Device selection. Honour the user's chosen device from Settings (-1 = system default),
        // then route it through ResolveLowLatencyDevice which, on Windows, prefers the WASAPI
        // variant of that device (shared, low-latency) over the high-latency MME default. On
        // macOS/Linux this is a passthrough — Pa_GetDefaultOutputDevice() / the chosen index.
        PaDeviceIndex selected_device = ResolvePreferredOutputDevice();

        if (selected_device == paNoDevice) {
            std::cout << "❌ [AUDIO] No output device available" << std::endl;
            return false;
        }

        const PaDeviceInfo* default_dev_info = Pa_GetDeviceInfo(selected_device);
        if (default_dev_info) {
            const PaHostApiInfo* hai = Pa_GetHostApiInfo(default_dev_info->hostApi);
            std::cout << "🎵 [AUDIO] Output device: " << default_dev_info->name << std::endl;
            std::cout << "🎵 [AUDIO] Host API: " << (hai ? hai->name : "unknown") << std::endl;
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
            // On Windows, hand WASAPI paFramesPerBufferUnspecified so it runs event-driven at its
            // native low-latency period instead of the fixed (1024-frame) buffer. Audio is the
            // transport clock, so that fixed buffer was the main reason play/stop/shuttle felt
            // laggier on Windows than on macOS (CoreAudio) / Linux. AudioCallback is fully
            // framesPerBuffer-driven, so a variable period is safe. macOS/Linux are unchanged.
            unsigned long frames_per_buffer = static_cast<unsigned long>(buffer_size);
#if defined(_WIN32)
            frames_per_buffer = paFramesPerBufferUnspecified;
#endif
            std::cout << "PortAudio: calling Pa_OpenStream..." << std::endl;
            err = Pa_OpenStream(&pa_stream, nullptr, &outputParameters, sample_rate, frames_per_buffer,
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
                // Report the ACTUAL negotiated output latency + host API so transport "feel" is
                // measurable in a single run (WASAPI ~10-30ms vs the old MME fallback ~80-200ms).
                if (const PaStreamInfo* si = Pa_GetStreamInfo(pa_stream)) {
                    const char* api_name = "unknown";
                    if (const PaDeviceInfo* sdi = Pa_GetDeviceInfo(selected_device))
                        if (const PaHostApiInfo* hai = Pa_GetHostApiInfo(sdi->hostApi)) api_name = hai->name;
                    std::cout << "🎧 [AUDIO LATENCY] host API: " << api_name
                              << ", output latency: " << (si->outputLatency * 1000.0) << " ms" << std::endl;
                }
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
                    std::cout << "[AUDIO] Stream is active, aborting..." << std::endl;
                    // Use Pa_AbortStream instead of Pa_StopStream for immediate shutdown
                    // This prevents PipeWire from waiting for buffer drain (which can crash)
                    PaError stop_err = Pa_AbortStream(pa_stream);
                    if (stop_err != paNoError && stop_err != paStreamIsStopped) {
                        std::cout << "[AUDIO] Pa_AbortStream warning: " << Pa_GetErrorText(stop_err) << std::endl;
                    }
                } else {
                    std::cout << "[AUDIO] Stream already stopped" << std::endl;
                }

                // WORKAROUND: PipeWire/ALSA has critical bug in pw_stream_destroy()
                // Calling Pa_CloseStream() causes malloc corruption in libpipewire
                // This is a known bug: https://gitlab.freedesktop.org/pipewire/pipewire/-/issues/
                // Solution: Just abort the stream and DON'T close it
                // The OS will clean up resources when process exits anyway

                std::cout << "[AUDIO] Skipping Pa_CloseStream to avoid PipeWire crash..." << std::endl;
                std::cout << "[AUDIO] Stream resources will be cleaned up by OS on exit" << std::endl;

                // Mark stream as null so we don't try to use it
                pa_stream = nullptr;
                pa_stream_sample_rate = 0;
                pa_stream_channels = 0;

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


    // Ease Out Elastic function (CSS/animation standard)
    // Returns value from 0.0 to 1.0 based on time progress t (0 to 1)
    // Creates "bouncy" overshoot effect at the end
    double EaseOutElastic(double t) {
        const double c4 = (2.0 * M_PI) / 3.0;

        if (t == 0.0) return 0.0;
        if (t == 1.0) return 1.0;

        return pow(2.0, -10.0 * t) * sin((t * 10.0 - 0.75) * c4) + 1.0;
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

            // Check if volume ducking is enabled in settings
            extern int GetAudioVolumeDuckingEnabled();
            bool ducking_enabled = GetAudioVolumeDuckingEnabled();

            // Low speed fade (slow motion)
            if (current_rate <= 0.3) {
                // Start fading below 0.3x
                new_volume = static_cast<float>(current_rate / 0.3);
            }
            // High speed fade (shuttle) - IMPROVED CURVE for ear protection
            // 6x: start fade, 12x: -24dB, 32x: -40dB
            else if (ducking_enabled && current_rate >= 6.0) {
                // Speed breakpoints and corresponding volumes (linear scale)
                const float speed_6x = 6.0f;
                const float speed_12x = 12.0f;
                const float speed_32x = 32.0f;

                const float vol_full = 1.0f;           // 0 dB at 6x
                const float vol_12x = 0.0631f;         // -24 dB at 12x (10^(-24/20))
                const float vol_32x = 0.01f;           // -40 dB at 32x (10^(-40/20))

                if (current_rate < speed_12x) {
                    // Fade from 6x to 12x: 0dB → -24dB
                    float t = (static_cast<float>(current_rate) - speed_6x) / (speed_12x - speed_6x);
                    // Use exponential fade for more natural volume reduction
                    t = t * t;  // Square for faster initial drop
                    new_volume = vol_full + (vol_12x - vol_full) * t;
                } else if (current_rate < speed_32x) {
                    // Fade from 12x to 32x: -24dB → -40dB
                    float t = (static_cast<float>(current_rate) - speed_12x) / (speed_32x - speed_12x);
                    new_volume = vol_12x + (vol_32x - vol_12x) * t;
                } else {
                    // Above 32x: stay at -40dB
                    new_volume = vol_32x;
                }
            }

            volume.store(new_volume);
        };
        // --- End Lambda ---

        while (!should_exit_smooth_speed.load()) {

            // === DIRECTION CHANGE SEQUENCER ===
            // Physics: reversing tape requires motor to stop, wait for mechanics, then restart.
            // Sequence: ramp down → hold 150ms (direction flips midway) → ramp up
            if (direction_change_pending.load()) {
                direction_change_pending.store(false);
                bool new_direction = pending_reverse_value.load();
                double saved_target = target_playback_speed.load();

                // TIMING PROBE: whole reversal sequence. Designed ≈ ramp-down (variable,
                // ~0.85^n decay) + 150ms hold + 200ms ramp-up. The two fixed parts alone
                // are 350ms; if ACTUAL >> that on Windows the sleep_for granularity is
                // stretching every 5ms step. Logged once per reversal.
                auto dir_t0 = std::chrono::steady_clock::now();

                // Phase 1: Soft ramp down (gentle exponential decay, 0.85 factor)
                while (playback_speed.load() > 0.01 && !should_exit_smooth_speed.load()) {
                    double spd = playback_speed.load() * 0.85;
                    if (spd < 0.01) spd = 0.0;
                    playback_speed.store(spd);
                    calculate_and_set_volume(spd);
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                playback_speed.store(0.0);
                calculate_and_set_volume(0.0);

                if (should_exit_smooth_speed.load()) continue;

                // Phase 2: Hold at 0 for 150ms, flip direction at midpoint (75ms)
                const int hold_ms = 150;
                const int half_ms = hold_ms / 2;

                for (int i = 0; i < half_ms && !should_exit_smooth_speed.load(); i += 5)
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));

                // Flip direction — visible to all readers via IsReverse()
                is_reverse.store(new_direction);

                for (int i = 0; i < (hold_ms - half_ms) && !should_exit_smooth_speed.load(); i += 5)
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));

                if (should_exit_smooth_speed.load()) continue;

                // Phase 3: Ramp up back to saved target (cubic ease-out, ~200ms)
                target_playback_speed.store(saved_target);
                const int ramp_steps = 40;  // 40 × 5ms = 200ms
                for (int s = 1; s <= ramp_steps && !should_exit_smooth_speed.load(); ++s) {
                    double t = static_cast<double>(s) / ramp_steps;
                    double eased = 1.0 - std::pow(1.0 - t, 3.0);  // Cubic ease-out
                    double rate = eased * saved_target;
                    playback_speed.store(rate);
                    calculate_and_set_volume(rate);
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                playback_speed.store(saved_target);
                calculate_and_set_volume(saved_target);

                auto dir_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - dir_t0).count();
                std::cout << "🎵 [DIR CHANGE] designed ~350ms+ (rampdown + 150ms hold + "
                          << "200ms rampup), ACTUAL " << dir_ms << "ms" << std::endl;
                continue;
            }
            // === END DIRECTION CHANGE SEQUENCER ===

            // ELASTIC EASE: Apply elastic ease-out for first play (0 → 1.0x)
            if (use_elastic_ease.load()) {
                double start_rate = playback_speed.load(); // Should be 0.0 or very low
                double snap_target = target_playback_speed.load(); // Should be 1.0

                // Duration: 300-500ms (user requested 0.3-0.5s)
                const int elastic_duration_ms = 250;  // 0.4 seconds
                const int step_ms = 2;               // 2ms interval for smooth animation
                const int steps = elastic_duration_ms / step_ms;

                // TIMING PROBE: measure how long the ease ACTUALLY takes. It is
                // designed for elastic_duration_ms; if the wall-clock is much
                // larger (user reports ~1s vs 250ms on Windows), the sleep_for
                // granularity — not the code — is the culprit. Logged once.
                auto ease_t0 = std::chrono::steady_clock::now();

                for (int s = 1; s <= steps; ++s) {
                    double t = static_cast<double>(s) / steps;  // Progress from 0.0 to 1.0
                    double eased_t = EaseOutElastic(t);         // Apply elastic curve
                    double rate = start_rate + (snap_target - start_rate) * eased_t;
                    playback_speed.store(rate);
                    calculate_and_set_volume(rate);
                    if (should_exit_smooth_speed.load()) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(step_ms));
                }

                auto ease_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - ease_t0).count();
                std::cout << "🎵 [ELASTIC EASE] designed " << elastic_duration_ms
                          << "ms (" << steps << " x " << step_ms << "ms), ACTUAL "
                          << ease_ms << "ms" << std::endl;

                // Ensure exact target hit (1.0x)
                playback_speed.store(snap_target);
                calculate_and_set_volume(snap_target);

                use_elastic_ease.store(false);  // Reset flag
                // std::cout << "🎵 [ELASTIC EASE] Completed, now at " << snap_target << "x" << std::endl;
                continue;
            }

            // Instant speed request (mouse shuttle) with micro-smoothing
            if (instant_speed_requested.load()) {
                double start_rate = playback_speed.load();
                double snap_target = target_playback_speed.load();

                const int smooth_ms = 100;   // total smoothing duration 100ms
                const int step_ms = 2;      // interval step 2ms
                const int steps = smooth_ms / step_ms;

                // TIMING PROBE: mouse-shuttle micro-smoothing. Designed for smooth_ms.
                // This fires on every shuttle nudge, so a stretch here is what makes the
                // shuttle feel laggy directly (not just the play/pause ease). Logged once
                // per nudge.
                auto shuttle_t0 = std::chrono::steady_clock::now();

                for (int s = 1; s <= steps; ++s) {
                    double t = static_cast<double>(s) / steps;
                    // Simple linear interpolation is enough for small duration
                    double rate = start_rate + (snap_target - start_rate) * t;
                    playback_speed.store(rate);
                    calculate_and_set_volume(rate);
                    if (should_exit_smooth_speed.load()) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(step_ms));
                }

                auto shuttle_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - shuttle_t0).count();
                std::cout << "🎵 [SHUTTLE] designed " << smooth_ms << "ms (" << steps
                          << " x " << step_ms << "ms), ACTUAL " << shuttle_ms << "ms" << std::endl;

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

            // === TAPE PAUSE DECELERATION (Betacam SP servo physics) ===
            // Real Betacam SP: capstan servo brakes tape over 150-250ms at normal play speed.
            // Quadratic profile v(t) = v0*(1-t/T)^2: rapid initial braking, smooth tail.
            // Audio advances ~v0*T/3 during braking so the pause stripe drifts ~0.3-0.5
            // frames before locking — exactly the "settle" visible on real hardware.
            // Duration scales with starting speed: faster shuttle stops sooner.
            if (is_pausing && current > 0.01) {
                int brake_ms = (current <= 1.5) ? 240 :
                               (current <= 3.0) ? 180 :
                               130;
                const int step_ms = 5;
                const int steps = brake_ms / step_ms;

                // TIMING PROBE: tape-pause brake (Betacam servo). Designed for brake_ms
                // (speed-dependent). This is the deceleration the user watches on every
                // stop; if ACTUAL >> brake_ms the "settle" drags. May exit early if the
                // user resumes — the log notes when it ran full length. Logged once per stop.
                auto brake_t0 = std::chrono::steady_clock::now();
                bool brake_full = true;

                for (int s = 1; s <= steps && !should_exit_smooth_speed.load(); ++s) {
                    if (target_playback_speed.load() != 0.0) { brake_full = false; break; }  // User resumed playback
                    double t = static_cast<double>(s) / steps;
                    double new_speed = current * (1.0 - t) * (1.0 - t);
                    if (new_speed < 0.003) new_speed = 0.0;
                    playback_speed.store(new_speed);
                    calculate_and_set_volume(new_speed);
                    std::this_thread::sleep_for(std::chrono::milliseconds(step_ms));
                }
                playback_speed.store(0.0);
                calculate_and_set_volume(0.0);

                auto brake_ms_actual = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - brake_t0).count();
                std::cout << "🎵 [PAUSE BRAKE] designed " << brake_ms << "ms (" << steps
                          << " x " << step_ms << "ms), ACTUAL " << brake_ms_actual << "ms"
                          << (brake_full ? "" : " (interrupted)") << std::endl;
                continue;
            }

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

            // === FRAME ALIGNMENT ON PROLONGED PAUSE ===
            // After 10 seconds of pause, gradually align audio time to nearest frame boundary
            // This removes the visible "stripe" artifact from Betacam effect
            {
                bool is_paused = (current == 0.0 && target == 0.0 && !is_jogging);

                if (is_paused) {
                    // Start tracking pause time if not already
                    if (!pause_time_tracking.load()) {
                        pause_start_time = std::chrono::steady_clock::now();
                        pause_time_tracking.store(true);
                        frame_alignment_done.store(false);
                    }

                    // Check if 10 seconds have passed and alignment not done yet
                    if (pause_time_tracking.load() && !frame_alignment_done.load()) {
                        auto now = std::chrono::steady_clock::now();
                        auto pause_duration = std::chrono::duration_cast<std::chrono::seconds>(
                            now - pause_start_time).count();

                        if (pause_duration >= 10) {
                            // Calculate current time in seconds
                            double current_pos_samples = playback_position.load();
                            double current_time_sec = current_pos_samples / (sample_rate * channels);

                            // Get frame rate (set by video module)
                            double fps = video_frame_rate.load();
                            if (fps < 1.0) fps = 25.0;  // Default fallback

                            // Calculate nearest frame boundary
                            double frame_duration = 1.0 / fps;
                            double frame_index = current_time_sec / frame_duration;
                            // Snap to NEAREST frame boundary (round), not always backward.
                            // scroll_phase = fmod(audio_time * fps, 1.0):
                            //   < 0.5 → more of frame N visible → snap backward to N
                            //   > 0.5 → more of frame N+1 visible → snap forward to N+1
                            // The compositing model handles snap-forward cleanly:
                            // scroll_phase approaches 1.0, then snaps to 0 on N+1 (no flash).
                            double nearest_frame = std::round(frame_index);
                            double target_time_sec = nearest_frame * frame_duration;

                            // Calculate offset (negative = backward, positive = forward)
                            double offset_sec = target_time_sec - current_time_sec;

                            // Only align if offset is significant (> 1ms) but within one frame
                            if (std::abs(offset_sec) > 0.001 && std::abs(offset_sec) < frame_duration) {
                                // Gradually align over 500ms for smooth transition
                                const int align_duration_ms = 500;
                                const int align_step_ms = 10;
                                const int align_steps = align_duration_ms / align_step_ms;

                                double start_samples = current_pos_samples;
                                double target_samples = target_time_sec * sample_rate * channels;

                                for (int s = 1; s <= align_steps; ++s) {
                                    // Check if still paused
                                    if (target_playback_speed.load() != 0.0 || should_exit_smooth_speed.load()) {
                                        break;  // User started playback, abort alignment
                                    }

                                    double t = static_cast<double>(s) / align_steps;
                                    // Ease-out for smooth finish
                                    double eased_t = 1.0 - (1.0 - t) * (1.0 - t);
                                    double new_samples = start_samples + (target_samples - start_samples) * eased_t;
                                    playback_position.store(new_samples);

                                    std::this_thread::sleep_for(std::chrono::milliseconds(align_step_ms));
                                }

                                // Ensure exact target
                                if (target_playback_speed.load() == 0.0) {
                                    playback_position.store(target_samples);
                                }
                            }

                            frame_alignment_done.store(true);
                        }
                    }
                } else {
                    // Not paused - reset tracking
                    if (pause_time_tracking.load()) {
                        pause_time_tracking.store(false);
                        frame_alignment_done.store(false);
                    }
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(interval));
        }
    }

    bool LoadFile(const std::string& filepath, double resume_position = 0.0) {
        std::cout << "FSTPAudioModuleImpl::LoadFile called with: " << filepath << std::endl;

        // Reset elastic ease flags for new file
        first_play_after_load.store(true);
        play_count.store(0);
        use_elastic_ease.store(false);

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

        const double fast_duration = reduce_fast_buffer_for_pcm ? FAST_BUFFER_DURATION_PCM : FAST_BUFFER_DURATION;
        constexpr double PRIORITY_ZONE_DURATION = 10.0 * 60.0;  // 10 minutes around resume point

        // Guard against a stale/invalid saved resume position at or beyond the end of THIS
        // file. Causes seen: the file at this path changed/shortened, the position overshot
        // EOF during fast shuttle, or it carried a timecode offset. Left unclamped, the
        // priority zone collapses to 0 samples and the whole load fails (file won't open).
        if (resume_position > 0.0 && duration > 0.0 && resume_position > duration - 1.0) {
            double clamped = std::max(0.0, duration - 1.0);
            std::cout << "[RESUME] Saved position " << resume_position << "s is past EOF ("
                      << duration << "s) — clamping to " << clamped << "s" << std::endl;
            resume_position = clamped;
        }

        // If resume position is beyond the fast buffer, use priority-zone decode.
        // Exception: PCM files decode at memcpy speed — decode the full file immediately
        // and then just set the playback head, no need for a zone + 2-phase background.
        if (resume_position > fast_duration && duration > fast_duration) {
            if (reduce_fast_buffer_for_pcm) {
                std::cout << "[RESUME] PCM file + long resume: decoding full file immediately (no priority zone)" << std::endl;
                size_t decoded = DecodePCMFast(filepath, 0, total_samples);
                if (decoded > 0) {
                    decode_range_start.store(0);
                    decoded_samples.store(decoded);
                    fast_buffer_samples.store(decoded);
                    fast_buffer_ready.store(true);
                    full_buffer_ready.store(true);
                    double sample_pos = resume_position * sample_rate * channels;
                    playback_position.store(std::min(sample_pos, static_cast<double>(decoded)));
                    std::cout << "[RESUME] PCM full decode done, position set to " << resume_position << "s" << std::endl;
                    return StartPermanentAudioStream();
                }
                std::cout << "[RESUME] PCM fast decode failed, falling back to priority zone" << std::endl;
            }
            std::cout << "[RESUME] Priority-zone decode: position=" << resume_position << "s, duration=" << duration << "s" << std::endl;
            if (DecodePriorityZone(filepath, resume_position, PRIORITY_ZONE_DURATION)) {
                return true;
            }
            std::cerr << "[RESUME] Priority-zone decode failed — falling back to normal load "
                         "from start so the file still opens" << std::endl;
            // Fall through to the standard two-stage decode below: a bad resume position must
            // never make the file unloadable.
        }

        if (duration <= fast_duration) {
            std::cout << "Decoding full file (duration <= " << fast_duration << " seconds)"
                      << (reduce_fast_buffer_for_pcm ? " [PCM fast buffer]" : "") << std::endl;
            if (!DecodeFullFile(filepath)) return false;
            if (resume_position > 0.0) {
                double sample_pos = resume_position * sample_rate * channels;
                double max_pos = static_cast<double>(decoded_samples.load());
                playback_position.store(std::min(sample_pos, max_pos));
                std::cout << "[RESUME] Set position to " << resume_position << "s after full decode" << std::endl;
            }
            return true;
        }

        if (!DecodeTwoStage(filepath, fast_duration)) {
            return false;
        }
        // For resume within the fast buffer range, set position now.
        // Resume beyond fast buffer is handled by DecodePriorityZone above.
        if (resume_position > 0.0 && resume_position <= fast_duration) {
            double sample_pos = resume_position * sample_rate * channels;
            double max_pos = static_cast<double>(decoded_samples.load());
            playback_position.store(std::min(sample_pos, max_pos));
            std::cout << "[RESUME] Set position to " << resume_position << "s after two-stage decode" << std::endl;
        }
        return true;
    }

    // Decode a priority zone around the resume position, then fill gaps in background.
    // Zone: [resume_pos, resume_pos + zone_duration].
    // Background phase 1: forward from zone end to file end.
    // Background phase 2: backward from resume_pos to 0 (for shuttle rewind).
    bool DecodePriorityZone(const std::string& filepath, double resume_pos, double zone_duration) {
        // Keep resume_pos exact; shrink zone_duration if it would overshoot end of file.
        // This preserves the exact saved position instead of moving the head backward.
        resume_pos = std::max(0.0, std::min(resume_pos, duration));
        double zone_end = std::min(resume_pos + zone_duration, duration);
        // Never let the zone collapse to nothing (resume_pos at/near EOF) — that produced a
        // 0-sample decode that failed the whole load. Pull the head back ~1s so there is a
        // usable zone that ends at the file end.
        if (zone_end - resume_pos < 1.0) {
            resume_pos = std::max(0.0, duration - 1.0);
            zone_end = duration;
        }
        zone_duration = zone_end - resume_pos;

        size_t resume_sample = static_cast<size_t>(resume_pos * sample_rate * channels);
        size_t zone_samples  = static_cast<size_t>(zone_duration * sample_rate * channels);

        std::cout << "[RESUME] Decoding priority zone: " << resume_pos << "s – "
                  << zone_end << "s (" << zone_samples << " samples)" << std::endl;

        size_t decoded_zone = 0;
        if (reduce_fast_buffer_for_pcm) {
            decoded_zone = DecodePCMFast(filepath, resume_sample, zone_samples);
            if (decoded_zone == 0)
                decoded_zone = DecodeToBuffer(filepath, resume_sample, zone_samples);
        } else {
            decoded_zone = DecodeToBuffer(filepath, resume_sample, zone_samples);
        }

        if (decoded_zone == 0) {
            std::cerr << "[RESUME] Priority zone decode failed" << std::endl;
            return false;
        }

        // The decoded region starts at resume_sample, not at 0
        decode_range_start.store(resume_sample);
        fast_buffer_samples.store(resume_sample + decoded_zone);
        decoded_samples.store(resume_sample + decoded_zone);
        fast_buffer_ready.store(true);

        // Place playback head at resume position
        playback_position.store(static_cast<double>(resume_sample));

        std::cout << "[RESUME] Priority zone ready. Starting stream, then background fill." << std::endl;
        if (!StartPermanentAudioStream()) {
            std::cout << "[RESUME] ERROR: Failed to start permanent stream!" << std::endl;
        }

        // Stop any leftover background thread
        if (background_decode_thread.joinable()) {
            should_stop_background_decode.store(true);
            background_decode_thread.join();
        }
        should_stop_background_decode.store(false);
        background_decode_running.store(true);

        background_decode_thread = std::thread([this, filepath, resume_sample, decoded_zone]() {
            std::cout << "[RESUME BG] Phase 1: forward from " << (resume_sample + decoded_zone)
                      << " to end (" << total_samples << ")" << std::endl;

            size_t zone_end = resume_sample + decoded_zone;

            // Phase 1: decode forward from end of priority zone to file end.
            // No throttle (is_background=false) — we want to fill the gap as fast as possible
            // so that forward shuttle into undecoded territory doesn't stall.
            size_t fwd = 0;
            if (zone_end < total_samples) {
                size_t fwd_max = total_samples - zone_end;
                if (reduce_fast_buffer_for_pcm) {
                    fwd = DecodePCMFast(filepath, zone_end, fwd_max);
                    if (fwd == 0) fwd = DecodeToBuffer(filepath, zone_end, fwd_max, false);
                } else {
                    fwd = DecodeToBuffer(filepath, zone_end, fwd_max, false);
                }
                if (fwd > 0) {
                    decoded_samples.store(zone_end + fwd);
                    std::cout << "[RESUME BG] Phase 1 done: decoded to " << (zone_end + fwd) << std::endl;
                }
            }

            if (should_stop_background_decode.load()) {
                background_decode_running.store(false);
                return;
            }

            // Phase 2: backward fill from 0 to resume_sample (for shuttle rewind support).
            // Also no throttle — want backward region available quickly.
            if (resume_sample > 0) {
                std::cout << "[RESUME BG] Phase 2: backward fill [0, " << resume_sample << "]" << std::endl;
                size_t bwd = 0;
                if (reduce_fast_buffer_for_pcm) {
                    bwd = DecodePCMFast(filepath, 0, resume_sample);
                    if (bwd == 0) bwd = DecodeToBuffer(filepath, 0, resume_sample, false);
                } else {
                    bwd = DecodeToBuffer(filepath, 0, resume_sample, false);
                }
                if (bwd > 0) {
                    // The whole file is now decoded — reset range to full [0, total]
                    decode_range_start.store(0);
                    decoded_samples.store(total_samples);
                    full_buffer_ready.store(true);
                    std::cout << "[RESUME BG] Phase 2 done. Full buffer ready." << std::endl;
                }
            } else {
                full_buffer_ready.store(true);
            }

            background_decode_running.store(false);
        });

        return true;
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
        timecode_offset_seconds = 0.0;
        total_samples = 0;
        playback_position.store(0.0);
        decoded_samples.store(0);
        fast_buffer_samples.store(0);
        fast_buffer_ready.store(false);
        full_buffer_ready.store(false);
        decode_range_start.store(0);
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
        int buffer_size = GetAudioBufferSize();

        // Resolve the output device, healing a stale saved index via the device name and
        // preferring the WASAPI variant on Windows (see ResolvePreferredOutputDevice).
        outputParameters.device = ResolvePreferredOutputDevice();
        if (outputParameters.device == paNoDevice) {
            std::cout << "❌ [AUDIO] RestartStreamForFile: no output device available" << std::endl;
            return;
        }

        const PaDeviceInfo* out_info = Pa_GetDeviceInfo(outputParameters.device);
        if (!out_info) {
            std::cout << "❌ [AUDIO] RestartStreamForFile: failed to get device info" << std::endl;
            return;
        }
        // Clamp to the resolved device's channel count — the WASAPI device may expose a different
        // maxOutputChannels than the MME variant the user's saved index pointed at.
        outputParameters.channelCount = std::min(channels, out_info->maxOutputChannels);
        outputParameters.sampleFormat = paInt16;
        outputParameters.suggestedLatency = out_info->defaultLowOutputLatency;
        outputParameters.hostApiSpecificStreamInfo = nullptr;
        const PaHostApiInfo* r_hai = Pa_GetHostApiInfo(out_info->hostApi);
        std::cout << "🎵 [AUDIO] RestartStreamForFile device: " << out_info->name
                  << " (Host API: " << (r_hai ? r_hai->name : "unknown") << ")" << std::endl;

        unsigned long r_frames_per_buffer = static_cast<unsigned long>(buffer_size);
#if defined(_WIN32)
        r_frames_per_buffer = paFramesPerBufferUnspecified;   // WASAPI native low-latency period (see StartPermanentAudioStream)
#endif
        PaError err = Pa_OpenStream(&pa_stream, nullptr, &outputParameters, sample_rate, r_frames_per_buffer,
                                   paClipOff, AudioCallback, this);

        if (err == paNoError) {
            playback_speed.store(0.0);
            err = Pa_StartStream(pa_stream);
            if (err != paNoError) {
                Pa_CloseStream(pa_stream);
                pa_stream = nullptr;
            } else if (const PaStreamInfo* si = Pa_GetStreamInfo(pa_stream)) {
                std::cout << "🎧 [AUDIO LATENCY] RestartStreamForFile output latency: "
                          << (si->outputLatency * 1000.0) << " ms" << std::endl;
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

        // Temporary buffer for clean audio (without servomotor) for VU meters
        static thread_local std::vector<int16_t> clean_buffer;
        clean_buffer.resize(framesPerBuffer * impl->channels);

        if (!impl->mmap_buffer) {
            std::memset(output, 0, framesPerBuffer * impl->channels * sizeof(int16_t));
            std::memset(clean_buffer.data(), 0, framesPerBuffer * impl->channels * sizeof(int16_t));

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
            std::memset(clean_buffer.data(), 0, framesPerBuffer * impl->channels * sizeof(int16_t));
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
            // Always use the maximum available samples to avoid silence during background decode
            size_t max_available_samples;
            size_t actual_decoded = impl->decoded_samples.load();
            size_t fast_decoded = impl->fast_buffer_samples.load();

            // Use the maximum of decoded_samples and fast_buffer_samples
            size_t total_decoded = std::max(actual_decoded, fast_decoded);
            max_available_samples = total_decoded / impl->channels;

            // Check both upper bound and lower bound (for priority-zone / resume decode)
            size_t range_start_samples = impl->decode_range_start.load() / impl->channels;
            if (base_index >= max_available_samples || base_index < range_start_samples) {
                // Outside decoded region — output silence
                for (int ch = 0; ch < impl->channels; ch++) {
                    output[frame * impl->channels + ch] = 0;
                    clean_buffer[frame * impl->channels + ch] = 0;
                }
            } else {
                // Catmull-Rom interpolation for each channel
                double abs_speed = std::abs(speed);

                // === TAPE-STYLE LOW-PASS FILTER ===
                // 1st-order IIR: y[n] = alpha*x[n] + (1-alpha)*y[n-1]
                // alpha = 1.0  → bypass (full bandwidth, "lock" mode)
                // alpha < 1.0  → HF rolloff; lower = softer/more muffled
                //
                // Cutoff mapping (at 48kHz):
                //   deviation 0.0 (1.0x)   → alpha=1.00 bypass
                //   deviation 0.5 (0.5/1.5x)→ alpha≈0.57  fc≈10kHz
                //   deviation 1.0 (0x/2x)  → alpha≈0.40  fc≈ 6kHz
                //   deviation 7.0 (8x)     → alpha=0.30  fc≈ 3kHz
                //
                // Bypassed when OSD shows "lock" (0.95x–1.05x).
                bool is_lock_speed = (abs_speed >= 0.95 && abs_speed <= 1.05);
                float lpf_alpha = 1.0f;
                if (!is_lock_speed) {
                    double deviation = std::abs(abs_speed - 1.0);
                    lpf_alpha = std::max(0.30f, static_cast<float>(1.0 / (1.0 + deviation * 1.5)));
                }

                for (int ch = 0; ch < impl->channels; ch++) {
                    // Get 4 neighboring samples for Catmull-Rom
                    double p0 = impl->GetSafeSample(base_index - 1, ch, max_available_samples);
                    double p1 = impl->GetSafeSample(base_index, ch, max_available_samples);
                    double p2 = impl->GetSafeSample(base_index + 1, ch, max_available_samples);
                    double p3 = impl->GetSafeSample(base_index + 2, ch, max_available_samples);

                    // Apply Catmull-Rom interpolation
                    double interpolated = impl->CatmullRomInterpolate(p0, p1, p2, p3, fractional);

                    // Apply attenuation at low speeds to prevent clicks
                    if (abs_speed <= 0.3) {
                        // Linear attenuation from 0.3x to 0.0x
                        double speed_attenuation = abs_speed / 0.3;
                        interpolated *= speed_attenuation;
                    }

                    // Apply master volume from settings
                    float master_volume = GetMasterVolume();
                    interpolated *= master_volume;

                    // Apply volume ducking for high speeds (ear protection)
                    float ducking_volume = impl->volume.load();
                    interpolated *= ducking_volume;

                    // Apply tape-style low-pass filter (HF rolloff at non-lock speeds)
                    if (lpf_alpha < 0.999f && ch < 8) {
                        float x = static_cast<float>(interpolated / 32768.0);
                        float y = lpf_alpha * x + (1.0f - lpf_alpha) * impl->lpf_state[ch];
                        impl->lpf_state[ch] = y;
                        interpolated = static_cast<double>(y) * 32768.0;
                    } else if (ch < 8) {
                        // Lock speed: update state without filtering to avoid transient on speed change
                        impl->lpf_state[ch] = static_cast<float>(interpolated / 32768.0);
                    }

                    // Store clean sample for VU meters
                    double clean_sample = std::max(-32768.0, std::min(32767.0, interpolated));
                    clean_buffer[frame * impl->channels + ch] = static_cast<int16_t>(clean_sample);

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

        // Peak Program Meter (PPM) calculation - quasi-peak behavior like Pro Tools/iZotope
        // NOTE: VU meters read from clean_buffer (audio file only, excluding servomotor)
        {
            // Audio metering, DAW-style (Logic/FCP/Pro Tools): the solid bar shows RMS (true
            // program level / loudness) and the cap shows true sample PEAK. dBFS conversion and
            // ballistics (attack/decay, peak-hold) are applied in the renderer. Replaces the old
            // ad-hoc avg(|x|)×1.5 "quasi-peak" — which was neither RMS nor peak and under-read
            // real material, so the meter never matched a pro DAW.
            float sumsq_left = 0.0f, sumsq_right = 0.0f;
            float peak_left = 0.0f, peak_right = 0.0f;

            for (unsigned long frame = 0; frame < framesPerBuffer; frame++) {
                if (impl->channels >= 1) {
                    float s = static_cast<float>(clean_buffer[frame * impl->channels]) / 32768.0f;
                    sumsq_left += s * s;
                    peak_left = std::max(peak_left, std::abs(s));
                }

                if (impl->channels >= 2) {
                    float s = static_cast<float>(clean_buffer[frame * impl->channels + 1]) / 32768.0f;
                    sumsq_right += s * s;
                    peak_right = std::max(peak_right, std::abs(s));
                } else if (impl->channels == 1) {
                    // Mono: mirror the single channel to the right meter.
                    float s = static_cast<float>(clean_buffer[frame * impl->channels]) / 32768.0f;
                    sumsq_right += s * s;
                    peak_right = std::max(peak_right, std::abs(s));
                }
            }

            // RMS = sqrt(mean(x²)) over the block — the true program level (a full-scale sine
            // reads -3 dBFS, exactly like a DAW). True peak drives the peak-hold cap.
            float n = static_cast<float>(framesPerBuffer > 0 ? framesPerBuffer : 1);
            float rms_left  = std::sqrt(sumsq_left  / n);
            float rms_right = std::sqrt(sumsq_right / n);

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

        const AVCodecParameters* codecpar = format_ctx->streams[audio_stream_index]->codecpar;
        sample_rate = codecpar->sample_rate;
        channels = codecpar->ch_layout.nb_channels;

        // Detect PCM audio to optimize fast buffer size
        reduce_fast_buffer_for_pcm = false;
        switch (codecpar->codec_id) {
            case AV_CODEC_ID_PCM_S16LE:
            case AV_CODEC_ID_PCM_S16BE:
            case AV_CODEC_ID_PCM_U16LE:
            case AV_CODEC_ID_PCM_U16BE:
            case AV_CODEC_ID_PCM_S24LE:
            case AV_CODEC_ID_PCM_S24BE:
            case AV_CODEC_ID_PCM_U24LE:
            case AV_CODEC_ID_PCM_U24BE:
            case AV_CODEC_ID_PCM_S32LE:
            case AV_CODEC_ID_PCM_S32BE:
            case AV_CODEC_ID_PCM_U32LE:
            case AV_CODEC_ID_PCM_U32BE:
            case AV_CODEC_ID_PCM_F32LE:
            case AV_CODEC_ID_PCM_F32BE:
            case AV_CODEC_ID_PCM_F64LE:
            case AV_CODEC_ID_PCM_F64BE:
                reduce_fast_buffer_for_pcm = true;
                break;
            default:
                break;
        }

        if (format_ctx->duration != AV_NOPTS_VALUE) {
            duration = static_cast<double>(format_ctx->duration) / AV_TIME_BASE;
        }
        total_samples = static_cast<size_t>(duration * sample_rate * channels * 1.1);

        // Extract timecode from file metadata (professional video: QuickTime, MXF, etc.)
        // QuickTime stores it as "timecode" in format or stream metadata (e.g. "10:00:00:00")
        {
            const char* tc = nullptr;
            double fps = 0.0;

            // Try format-level metadata first
            const AVDictionaryEntry* entry = av_dict_get(format_ctx->metadata, "timecode", nullptr, 0);
            if (entry && entry->value) {
                tc = entry->value;
                std::cout << "[Audio] Timecode in format metadata: " << tc << std::endl;
            }

            // Try video stream metadata
            if (!tc) {
                int vidx = av_find_best_stream(format_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
                if (vidx >= 0) {
                    AVStream* vs = format_ctx->streams[vidx];
                    entry = av_dict_get(vs->metadata, "timecode", nullptr, 0);
                    if (entry && entry->value) {
                        tc = entry->value;
                        std::cout << "[Audio] Timecode in video stream metadata: " << tc << std::endl;
                    }
                    // Get FPS from video stream
                    if (vs->avg_frame_rate.num > 0 && vs->avg_frame_rate.den > 0) {
                        fps = av_q2d(vs->avg_frame_rate);
                    } else if (vs->r_frame_rate.num > 0 && vs->r_frame_rate.den > 0) {
                        fps = av_q2d(vs->r_frame_rate);
                    }
                }
            }

            // Parse HH:MM:SS:FF timecode string to seconds
            if (tc) {
                int h = 0, m = 0, s = 0, f = 0;
                double use_fps = (fps > 0.0) ? fps : 25.0;
                if (sscanf(tc, "%d:%d:%d:%d", &h, &m, &s, &f) == 4) {
                    timecode_offset_seconds = (h * 3600.0) + (m * 60.0) + s + (f / use_fps);
                    if (timecode_offset_seconds > 0.001) {
                        std::cout << "[Audio] Timecode offset: " << std::fixed << std::setprecision(3)
                                  << timecode_offset_seconds << "s (" << tc << " @ " << use_fps << " fps)" << std::endl;
                    }
                }
            }
        }

        // Get codec name
        if (codec && codec->long_name) {
            codec_name = codec->long_name;
        } else if (codec && codec->name) {
            codec_name = codec->name;
        } else {
            codec_name = "Unknown";
        }

        if (reduce_fast_buffer_for_pcm) {
            std::cout << "[Audio] Detected PCM audio codec, reducing fast buffer to "
                      << FAST_BUFFER_DURATION_PCM << " seconds" << std::endl;
        }

        avformat_close_input(&format_ctx);
        return true;
    }

    bool CreateMmapBuffer() {
        mmap_size_bytes = total_samples * sizeof(int16_t);
#ifdef _WIN32
        char temp_path[MAX_PATH];
        GetTempPathA(MAX_PATH, temp_path);
        char temp_filename[MAX_PATH];
        GetTempFileNameA(temp_path, "fstp", 0, temp_filename);
        mmap_fd = _open(temp_filename, _O_RDWR | _O_CREAT | _O_BINARY, _S_IREAD | _S_IWRITE);
        if (mmap_fd == -1) return false;
        _chsize(mmap_fd, mmap_size_bytes);
#else
        char temp_template[] = "/tmp/fstp_audio_XXXXXX";
        mmap_fd = mkstemp(temp_template);
        if (mmap_fd == -1 || ftruncate(mmap_fd, mmap_size_bytes) == -1) return false;
        temp_filename = temp_template;
        unlink(temp_filename.c_str());
#endif

#ifdef _WIN32
        // Map buffer to memory using Windows wrapper
        mmap_buffer = static_cast<int16_t*>(fstp_mmap(nullptr, mmap_size_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, mmap_fd, 0));
#else
        mmap_buffer = static_cast<int16_t*>(mmap(nullptr, mmap_size_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, mmap_fd, 0));
#endif
        return (mmap_buffer != MAP_FAILED);
    }

    void CleanupMmap() {
        if (mmap_buffer && mmap_buffer != MAP_FAILED) {
#ifdef _WIN32
            fstp_munmap(mmap_buffer, mmap_size_bytes);
#else
            munmap(mmap_buffer, mmap_size_bytes);
#endif
            mmap_buffer = nullptr;
        }
        if (mmap_fd != -1) {
#ifdef _WIN32
            _close(mmap_fd);
#else
            close(mmap_fd);
#endif
            mmap_fd = -1;
        }
        // File already unlinked in CreateMmapBuffer()
        temp_filename.clear();
    }

    bool DecodeFullFile(const std::string& filepath) {
        // OPTIMIZATION: Fast path for PCM - skip codec, read packets directly
        if (reduce_fast_buffer_for_pcm) {
            std::cout << "[PCM Fast Path] Using direct packet read (bypassing codec)" << std::endl;
            size_t decoded = DecodePCMFast(filepath, 0, total_samples);
            if (decoded > 0) {
                decoded_samples.store(decoded);
                fast_buffer_ready.store(true);
                full_buffer_ready.store(true);

                std::cout << "Starting/checking permanent stream after PCM fast decode..." << std::endl;
                if (!StartPermanentAudioStream()) {
                    std::cout << "ERROR: Failed to start/reinitialize permanent stream after decode!" << std::endl;
                }
                return true;
            }
            // Fallback to normal path if fast path fails
            std::cout << "[PCM Fast Path] Failed, falling back to normal decode" << std::endl;
        }

        // Normal decode path
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

    // ULTRA-FAST PCM decoder - reads packets directly without codec overhead
    size_t DecodePCMFast(const std::string& filepath, size_t start_sample, size_t max_samples) {
        std::cout << "[PCM Fast] Starting direct packet read (no codec overhead)..." << std::endl;

        AVFormatContext* format_ctx = nullptr;
        AVDictionary* opts = nullptr;

        // OPTIMIZATION: Increase I/O buffer for faster reading (16MB instead of default 32KB)
        av_dict_set(&opts, "buffer_size", "16777216", 0);

        if (avformat_open_input(&format_ctx, filepath.c_str(), nullptr, &opts) != 0) {
            av_dict_free(&opts);
            return 0;
        }
        av_dict_free(&opts);

        if (avformat_find_stream_info(format_ctx, nullptr) < 0) {
            avformat_close_input(&format_ctx);
            return 0;
        }

        const AVCodec* codec = nullptr;
        int audio_stream_index = av_find_best_stream(format_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
        if (audio_stream_index < 0) {
            avformat_close_input(&format_ctx);
            return 0;
        }

        // CRITICAL OPTIMIZATION: Disable all non-audio streams to prevent reading video packets
        for (unsigned int i = 0; i < format_ctx->nb_streams; i++) {
            if (static_cast<int>(i) != audio_stream_index) {
                format_ctx->streams[i]->discard = AVDISCARD_ALL;
            }
        }

        const AVCodecParameters* codecpar = format_ctx->streams[audio_stream_index]->codecpar;

        // Verify this is actually S16 PCM (most common in DNxHD/ProRes)
        bool is_s16_pcm = (codecpar->codec_id == AV_CODEC_ID_PCM_S16LE ||
                           codecpar->codec_id == AV_CODEC_ID_PCM_S16BE);

        if (!is_s16_pcm) {
            std::cout << "[PCM Fast] Not S16 PCM, using fallback" << std::endl;
            avformat_close_input(&format_ctx);
            return 0;
        }

        std::cout << "[PCM Fast] Confirmed S16 PCM, reading packets directly..." << std::endl;

        // Handle seeking for two-stage decode
        if (start_sample > 0) {
            double start_time_seconds = static_cast<double>(start_sample) / (sample_rate * channels);
            AVStream* stream = format_ctx->streams[audio_stream_index];
            int64_t seek_target = static_cast<int64_t>(start_time_seconds * stream->time_base.den / stream->time_base.num);

            std::cout << "[PCM Fast] Seeking to " << start_time_seconds << "s (sample " << start_sample << ")" << std::endl;

            if (av_seek_frame(format_ctx, audio_stream_index, seek_target, AVSEEK_FLAG_BACKWARD) < 0) {
                std::cout << "[PCM Fast] Seek failed, starting from beginning" << std::endl;
            }
        }

        bool is_big_endian = (codecpar->codec_id == AV_CODEC_ID_PCM_S16BE);
        AVPacket* packet = av_packet_alloc();
        size_t current_sample = 0;
        size_t write_offset = start_sample;
        size_t actual_position = 0;  // Track actual sample position in file

        auto start_time = std::chrono::high_resolution_clock::now();
        size_t packets_read = 0;
        size_t audio_packets = 0;
        size_t video_packets_skipped = 0;
        size_t samples_skipped = 0;

        // Read packets directly - PCM data is in packet->data, no decoding needed!
        while (av_read_frame(format_ctx, packet) >= 0 && current_sample < max_samples) {
            packets_read++;
            if (packet->stream_index == audio_stream_index) {
                audio_packets++;
                size_t packet_samples = packet->size / sizeof(int16_t);

                // Skip samples that were already decoded (due to backward seek)
                size_t packet_offset = 0;  // Offset into this packet's data
                if (actual_position < start_sample) {
                    size_t skip = std::min(start_sample - actual_position, packet_samples);
                    packet_offset = skip;
                    actual_position += skip;
                    samples_skipped += skip;
                    packet_samples -= skip;

                    // If entire packet should be skipped, continue
                    if (packet_samples == 0) {
                        av_packet_unref(packet);
                        continue;
                    }
                }

                size_t samples_to_copy = std::min(packet_samples, max_samples - current_sample);

                if (write_offset + samples_to_copy <= total_samples) {
                    const int16_t* src = reinterpret_cast<const int16_t*>(packet->data) + packet_offset;

                    if (is_big_endian) {
                        // Byte swap for big endian
                        for (size_t i = 0; i < samples_to_copy; i++) {
                            uint16_t val = src[i];
                            mmap_buffer[write_offset + i] = static_cast<int16_t>((val >> 8) | (val << 8));
                        }
                    } else {
                        // OPTIMIZATION: Always use NEON on Apple Silicon for maximum speed
#ifdef USE_NEON_SIMD
                        neon_copy_samples(&mmap_buffer[write_offset], src, samples_to_copy);
#else
                        std::memcpy(&mmap_buffer[write_offset], src, samples_to_copy * sizeof(int16_t));
#endif
                    }

                    write_offset += samples_to_copy;
                    current_sample += samples_to_copy;
                    actual_position += samples_to_copy;

                    // CRITICAL: Update decoded_samples progressively during background decode
                    // This allows playback to continue beyond fast buffer while still decoding
                    if (start_sample > 0) {  // Only in background decode mode
                        decoded_samples.store(start_sample + current_sample);
                    }
                }
            } else {
                video_packets_skipped++;
            }
            av_packet_unref(packet);
        }

        av_packet_free(&packet);
        avformat_close_input(&format_ctx);

        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

        std::cout << "[PCM Fast] Decoded " << current_sample << " samples in " << duration_ms << "ms" << std::endl;
        std::cout << "           Total packets: " << packets_read
                  << " (audio: " << audio_packets
                  << ", video skipped: " << video_packets_skipped << ")" << std::endl;
        std::cout << "           Speed: " << (packets_read * 1000.0 / duration_ms) << " total packets/sec, "
                  << (audio_packets * 1000.0 / duration_ms) << " audio packets/sec" << std::endl;
        return current_sample;
    }

    bool DecodeTwoStage(const std::string& filepath, double fast_duration) {
        size_t fast_samples = static_cast<size_t>(fast_duration * sample_rate * channels);

        // OPTIMIZATION: Use PCM fast path for two-stage if it's PCM
        size_t decoded_fast = 0;
        auto fast_start = std::chrono::high_resolution_clock::now();

        if (reduce_fast_buffer_for_pcm) {
            std::cout << "⚡ [PCM Fast Path] Two-stage: Using direct packet read for " << fast_duration << "s fast buffer..." << std::endl;
            decoded_fast = DecodePCMFast(filepath, 0, fast_samples);
            if (decoded_fast == 0) {
                std::cout << "❌ [PCM Fast Path] FAILED - falling back to normal decode" << std::endl;
                decoded_fast = DecodeToBuffer(filepath, 0, fast_samples);
            } else {
                auto fast_end = std::chrono::high_resolution_clock::now();
                auto fast_ms = std::chrono::duration_cast<std::chrono::milliseconds>(fast_end - fast_start).count();
                std::cout << "✅ [PCM Fast Path] Fast buffer completed in " << fast_ms << "ms" << std::endl;
            }
        } else {
            std::cout << "🐌 [NORMAL Path] Using codec-based decode (not PCM)" << std::endl;
            decoded_fast = DecodeToBuffer(filepath, 0, fast_samples);
        }

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

            std::cout << "🧵 [BACKGROUND] Spawning background decode thread..." << std::endl;

            // OPTIMIZATION: Use PCM fast path for background decode too
            background_decode_thread = std::thread([this, filepath, decoded_fast]() {
                std::cout << "🧵 [BACKGROUND THREAD] Thread started, beginning decode..." << std::endl;
                size_t remaining = 0;
                if (reduce_fast_buffer_for_pcm) {
                    std::cout << "[PCM Fast Path] Background: Using direct packet read..." << std::endl;
                    auto bg_start = std::chrono::high_resolution_clock::now();
                    remaining = DecodePCMFast(filepath, decoded_fast, total_samples - decoded_fast);
                    auto bg_end = std::chrono::high_resolution_clock::now();
                    auto bg_ms = std::chrono::duration_cast<std::chrono::milliseconds>(bg_end - bg_start).count();
                    std::cout << "🧵 [BACKGROUND] PCM decode took " << bg_ms << "ms" << std::endl;
                    if (remaining == 0) {
                        std::cout << "[PCM Fast Path] Background failed, falling back" << std::endl;
                        remaining = DecodeToBuffer(filepath, decoded_fast, total_samples - decoded_fast, true);
                    }
                } else {
                    remaining = DecodeToBuffer(filepath, decoded_fast, total_samples - decoded_fast, true);
                }

                if (remaining > 0) {
                    decoded_samples.store(decoded_fast + remaining);
                    full_buffer_ready.store(true);
                    std::cout << "Background decoding completed. Total samples: " << (decoded_fast + remaining) << std::endl;
                }
                background_decode_running.store(false);
            });

            std::cout << "✅ [BACKGROUND] Thread spawned, returning from DecodeTwoStage..." << std::endl;
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

        // OPTIMIZATION: Increase I/O buffer for faster reading
        AVDictionary* opts = nullptr;
        av_dict_set(&opts, "buffer_size", "16777216", 0);

        if (avformat_open_input(&format_ctx, filepath.c_str(), nullptr, &opts) != 0) {
            av_dict_free(&opts);
            return 0;
        }
        av_dict_free(&opts);

        if (avformat_find_stream_info(format_ctx, nullptr) < 0) {
            avformat_close_input(&format_ctx);
            return 0;
        }

        int audio_stream_index = av_find_best_stream(format_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
        if (audio_stream_index < 0) {
            avformat_close_input(&format_ctx);
            return 0;
        }

        // CRITICAL FIX: On macOS, prefer AudioToolbox AAC decoder for AAC files
        // Native FFmpeg AAC decoder fails on some AAC-ELD profiles
#ifdef __APPLE__
        const AVCodecParameters* codecpar = format_ctx->streams[audio_stream_index]->codecpar;
        if (codecpar->codec_id == AV_CODEC_ID_AAC) {
            // Try to use AudioToolbox AAC decoder (aac_at) on macOS
            const AVCodec* aac_at_codec = avcodec_find_decoder_by_name("aac_at");
            if (aac_at_codec) {
                std::cout << "DecodeToBuffer: Using AudioToolbox AAC decoder (aac_at) on macOS" << std::endl;
                codec = aac_at_codec;
            } else {
                std::cout << "DecodeToBuffer: AudioToolbox AAC decoder not available, using native" << std::endl;
            }
        }
#endif

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
                    size_t total_frame_samples = samples_per_channel * frame_channels;

                    // OPTIMIZATION 1: Direct PCM S16 copy path (no conversion needed)
                    if (codec_ctx->sample_fmt == AV_SAMPLE_FMT_S16) {
                        size_t remaining = max_samples - current_sample;
                        size_t samples_to_copy = std::min<size_t>(total_frame_samples, remaining);
                        const int16_t* src = reinterpret_cast<int16_t*>(frame->data[0]);

#ifdef USE_NEON_SIMD
                        // NEON-optimized copy for large blocks
                        if (samples_to_copy >= 16) {
                            neon_copy_samples(&mmap_buffer[write_offset], src, samples_to_copy);
                        } else {
                            std::memcpy(&mmap_buffer[write_offset], src, samples_to_copy * sizeof(int16_t));
                        }
#else
                        std::memcpy(&mmap_buffer[write_offset], src, samples_to_copy * sizeof(int16_t));
#endif
                        write_offset += samples_to_copy;
                        current_sample += samples_to_copy;

                        // CRITICAL: Update decoded_samples progressively during background decode
                        if (is_background && start_sample > 0) {
                            decoded_samples.store(start_sample + current_sample);
                        }
                        continue; // Skip to next frame
                    }

                    // OPTIMIZATION 2: S32 (32-bit PCM) to S16 conversion with NEON
                    if (codec_ctx->sample_fmt == AV_SAMPLE_FMT_S32) {
                        size_t remaining = max_samples - current_sample;
                        size_t samples_to_copy = std::min<size_t>(total_frame_samples, remaining);
                        const int32_t* src = reinterpret_cast<int32_t*>(frame->data[0]);

#ifdef USE_NEON_SIMD
                        neon_int32_to_int16(src, &mmap_buffer[write_offset], samples_to_copy);
#else
                        for (size_t i = 0; i < samples_to_copy; i++) {
                            mmap_buffer[write_offset + i] = static_cast<int16_t>(src[i] >> 16);
                        }
#endif
                        write_offset += samples_to_copy;
                        current_sample += samples_to_copy;

                        // CRITICAL: Update decoded_samples progressively during background decode
                        if (is_background && start_sample > 0) {
                            decoded_samples.store(start_sample + current_sample);
                        }
                        continue;
                    }

                    // OPTIMIZATION 3: FLT (float interleaved) to S16 conversion with NEON
                    if (codec_ctx->sample_fmt == AV_SAMPLE_FMT_FLT) {
                        size_t remaining = max_samples - current_sample;
                        size_t samples_to_copy = std::min<size_t>(total_frame_samples, remaining);
                        const float* src = reinterpret_cast<float*>(frame->data[0]);

#ifdef USE_NEON_SIMD
                        neon_float_interleaved_to_int16(src, &mmap_buffer[write_offset], samples_to_copy);
#else
                        for (size_t i = 0; i < samples_to_copy; i++) {
                            float sample = std::max(-1.0f, std::min(1.0f, src[i]));
                            mmap_buffer[write_offset + i] = static_cast<int16_t>(sample * 32767.0f);
                        }
#endif
                        write_offset += samples_to_copy;
                        current_sample += samples_to_copy;

                        // CRITICAL: Update decoded_samples progressively during background decode
                        if (is_background && start_sample > 0) {
                            decoded_samples.store(start_sample + current_sample);
                        }
                        continue;
                    }

                    // OPTIMIZATION 4: S16P (16-bit planar) - direct copy per channel
                    if (codec_ctx->sample_fmt == AV_SAMPLE_FMT_S16P) {
                        size_t samples_to_process = std::min<size_t>(samples_per_channel, (max_samples - current_sample) / frame_channels);

                        for (int ch = 0; ch < frame_channels; ch++) {
                            const int16_t* channel_data = reinterpret_cast<int16_t*>(frame->data[ch]);
                            for (size_t i = 0; i < samples_to_process; i++) {
                                mmap_buffer[write_offset + i * frame_channels + ch] = channel_data[i];
                            }
                        }
                        size_t processed = samples_to_process * frame_channels;
                        write_offset += processed;
                        current_sample += processed;

                        // CRITICAL: Update decoded_samples progressively during background decode
                        if (is_background && start_sample > 0) {
                            decoded_samples.store(start_sample + current_sample);
                        }
                        continue;
                    }

                    // OPTIMIZATION: NEON-accelerated float planar to int16 conversion
                    if (codec_ctx->sample_fmt == AV_SAMPLE_FMT_FLTP) {
#ifdef USE_NEON_SIMD
                        // Process all channels with NEON
                        for (int ch = 0; ch < frame_channels && current_sample < max_samples; ch++) {
                            size_t remaining = max_samples - current_sample;
                            size_t samples_to_process = std::min<size_t>(samples_per_channel, remaining / frame_channels);

                            if (samples_to_process > 0) {
                                float* channel_data = reinterpret_cast<float*>(frame->data[ch]);

                                // Use NEON for batches, interleave into output
                                for (size_t i = 0; i < samples_to_process; i++) {
                                    if (write_offset + ch < total_samples) {
                                        float sample = std::max(-1.0f, std::min(1.0f, channel_data[i]));
                                        mmap_buffer[write_offset + i * frame_channels + ch] = static_cast<int16_t>(sample * 32767.0f);
                                    }
                                }
                            }
                        }
                        size_t processed = std::min<size_t>(samples_per_channel * frame_channels, max_samples - current_sample);
                        write_offset += processed;
                        current_sample += processed;
                        continue;
#endif
                    }

                    // Fallback: Generic sample-by-sample conversion
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

bool FSTPAudioModuleWrapper::LoadFile(const std::string& filepath, double resume_position) {
    std::cout << "FSTPAudioModuleWrapper::LoadFile called with: " << filepath << std::endl;

    if (!m_initialized) {
        std::cerr << "Audio module not initialized" << std::endl;
        return false;
    }

    if (m_loaded) {
        std::cout << "Unloading previous file" << std::endl;
        UnloadFile();
    }

    std::cout << "Calling m_impl->LoadFile (resume=" << resume_position << "s)..." << std::endl;
    if (m_impl->LoadFile(filepath, resume_position)) {
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

        // Check if we should use elastic ease-out effect
        bool should_use_elastic = false;

        if (m_impl->first_play_after_load.load()) {
            // First play after file load - always use elastic ease
            should_use_elastic = true;
            m_impl->first_play_after_load.store(false);
            std::cout << "Play(): FIRST play after load - using elastic ease-out" << std::endl;
        } else {
            // Increment play counter and check if we should trigger periodic elastic
            int current_count = m_impl->play_count.fetch_add(1) + 1;
            // Trigger elastic every 7-10 plays (not too frequent, user requested "not often")
            if (current_count % 8 == 0) {
                should_use_elastic = true;
                std::cout << "Play(): Periodic elastic ease-out (play #" << current_count << ")" << std::endl;
            }
        }

        if (should_use_elastic) {
            // Activate elastic ease-out in smooth_speed_change thread
            m_impl->use_elastic_ease.store(true);
        } else {
            std::cout << "Play(): starting legacy smooth_speed_change system (normal)" << std::endl;
        }
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
    if (!m_impl) return;
    if (m_impl->is_reverse.load() == reverse) return;  // No change needed
    // Queue direction-change sequencer (ramp down → hold → flip → ramp up)
    m_impl->pending_reverse_value.store(reverse);
    m_impl->direction_change_pending.store(true);
}

void FSTPAudioModuleWrapper::SetReverseInstant(bool reverse) {
    // For Mouse Shuttle: bypass sequencer, change direction immediately
    m_reverse.store(reverse);
    if (m_impl) {
        m_impl->is_reverse.store(reverse);
        m_impl->direction_change_pending.store(false);  // Cancel any pending sequencer
    }
}

void FSTPAudioModuleWrapper::SetPosition(double position_seconds) {
    if (m_impl && m_loaded) {
        double sample_pos = position_seconds * m_impl->sample_rate * m_impl->channels;
        double range_start = static_cast<double>(m_impl->decode_range_start.load());
        double range_end   = static_cast<double>(m_impl->decoded_samples.load());
        sample_pos = std::max(range_start, std::min(sample_pos, range_end));
        m_impl->playback_position.store(sample_pos);
    }
}

double FSTPAudioModuleWrapper::GetPosition() const {
    if (m_impl && m_loaded && m_impl->sample_rate > 0 && m_impl->channels > 0) {
        double raw_seconds = m_impl->playback_position.load() / (m_impl->sample_rate * m_impl->channels);
        return raw_seconds + m_impl->timecode_offset_seconds;
    }
    return 0.0;
}

double FSTPAudioModuleWrapper::GetDuration() const {
    return m_duration.load();
}

double FSTPAudioModuleWrapper::GetTimecodeOffset() const {
    if (m_impl && m_loaded) {
        return m_impl->timecode_offset_seconds;
    }
    return 0.0;
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
    // Read actual state from impl (sequencer updates is_reverse at the right moment)
    if (m_impl) return m_impl->is_reverse.load();
    return m_reverse.load();
}

void FSTPAudioModuleWrapper::SetVideoFrameRate(double fps) {
    if (m_impl && fps > 0.0) {
        m_impl->video_frame_rate.store(fps);
    }
}

bool FSTPAudioModuleWrapper::IsFrameAligned() const {
    return m_impl && m_impl->frame_alignment_done.load();
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