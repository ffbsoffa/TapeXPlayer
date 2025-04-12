#include "common.h"
extern "C" {
    #include <libavcodec/avcodec.h>
    #include <libavformat/avformat.h>
    #include <libavutil/opt.h>
    #include <libavutil/channel_layout.h>
}
#include <portaudio.h>
#include <iostream>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <gst/gst.h>
#include "common.h"
#include <random>
#include <nfd.hpp>
#include <map>

std::vector<float> audio_buffer;
size_t audio_buffer_index = 0;
std::atomic<bool> decoding_finished{false};
std::atomic<bool> decoding_completed{false};

extern std::atomic<bool> quit;
extern std::atomic<double> playback_rate;
extern std::atomic<bool> is_reverse;
extern std::atomic<double> current_audio_time;
extern std::atomic<double> total_duration;
extern std::atomic<double> original_fps;

std::atomic<bool> jog_forward(false);
std::atomic<bool> jog_backward(false);
const double JOG_SPEED = 0.25;

std::atomic<int> sample_rate{44100};

// Global variables for audio device management
std::atomic<int> current_audio_device_index(0);
std::atomic<int> selected_audio_device_index(-1); // Index of user-selected audio card
std::mutex audio_device_mutex;
PaStream* stream = nullptr;

// Map to store the actual device indices for each menu item index
std::map<int, int> menu_to_device_index;

// Function declarations
std::vector<std::string> get_audio_output_devices();

void smooth_speed_change() {
    const double normal_step = 0.25;
    const double pause_step = 1.0; // Still needed for pausing interpolation
    const int normal_interval = 7; // Re-add missing constant
    const int pause_interval = 4;  // Still needed for pausing interpolation

    // --- Base Overshoot Curve Parameters (for scaling) ---
    const double baseOvershootTotalDurationMs = 250.0;
    const double baseOvershootDipSpeed = 0.7;
    const int baseOvershootPeakTimeMs = 50;
    const int baseOvershootDipTimeMs = 75;
    const int baseOvershootRecoverTimeMs = 125;
    // --- End Base Parameters ---

    // --- Randomness Setup ---
    static bool is_first_play = true; // Flag for first play overshoot
    static std::random_device rd;       // Seed device
    static std::mt19937 gen(rd());      // Mersenne Twister generator
    static bool seeded = false;         // Flag to seed only once

    if (!seeded) {
        gen.seed(rd()); // Seed the generator properly
        seeded = true;
    }
    std::uniform_real_distribution<double> peak_dist(1.2, 1.7); // Distribution for peak speed
    std::uniform_int_distribution<int> chance_dist(1, 10);      // Distribution for overshoot chance (1 in 10)
    std::uniform_int_distribution<int> duration_dist(250, 300); // Distribution for total duration
    // --- End Randomness Setup ---

    // --- Lambda for Stateless Volume Calculation (defined locally) ---
    auto calculate_and_set_volume = [&](double current_rate) {
        float new_volume = 1.0f; // Default to full volume
        if (current_rate <= 0.3) { // Start fading below 0.3x
            new_volume = current_rate / 0.3f; // Linear fade from 0.3x down to 0
        } else if (current_rate >= 7.0) {
            if (current_rate >= 18.0) {
                new_volume = 0.15f;
            } else {
                float t = (current_rate - 7.0f) / (18.0f - 7.0f);
                new_volume = 1.0f - (t * 0.85f);
            }
        }
        volume.store(new_volume);
    };
    // --- End Lambda ---

    while (!quit.load()) {
        double current = playback_rate.load();
        double target = target_playback_rate.load();

        bool is_pausing = (target == 0.0 && current > 0.0);
        bool is_resuming = (std::abs(current) < 0.001 && target > 0.0);
        bool is_jogging = jog_forward.load() || jog_backward.load();

        int interval = normal_interval; // Default interval

        // --- Prioritize JOGGING check --- 
        if (is_jogging) {
            playback_rate.store(JOG_SPEED); // Set speed directly to JOG_SPEED
            calculate_and_set_volume(JOG_SPEED);
            interval = normal_interval; // Use normal interval during jog
        }
        // --- Handle RESUMING (includes overshoot logic) --- 
        else if (is_resuming) {
            // Original overshoot logic (randomized)
             bool should_overshoot = false;
             if (is_first_play) {
                 should_overshoot = true;
                 is_first_play = false; // Only overshoot guaranteed on the very first play
             } else {
                 int chance = chance_dist(gen); // Generate number between 1 and 10
                 if (chance == 1) { // 1 in 10 chance for subsequent overshoots
                     should_overshoot = true;
                 }
             }

             if (should_overshoot) {
                 // --- Execute New Overshoot Curve ---
                 volume.store(1.0f); // Force volume to 1.0 before starting the curve
                 // ... (Generate random parameters for overshoot) ...
                 double currentOvershootPeakSpeed = peak_dist(gen);
                 int currentOvershootTotalDurationMs = duration_dist(gen);
                 double timeScaleFactor = static_cast<double>(currentOvershootTotalDurationMs) / baseOvershootTotalDurationMs;
                 int currentPeakTimeMs = static_cast<int>(baseOvershootPeakTimeMs * timeScaleFactor);
                 int currentDipTimeMs = static_cast<int>(baseOvershootDipTimeMs * timeScaleFactor);
                 int currentRecoverTimeMs = static_cast<int>(baseOvershootRecoverTimeMs * timeScaleFactor);
                 auto overshoot_start_time = std::chrono::steady_clock::now();

                 while (true) {
                     // ... (Overshoot curve calculation logic) ...
                     auto now = std::chrono::steady_clock::now();
                     auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - overshoot_start_time);
                     int elapsedMs = elapsed.count();

                     if (elapsedMs >= currentOvershootTotalDurationMs) { break; }
                     double calculated_speed = 1.0;
                     if (elapsedMs < currentPeakTimeMs) { double phase_progress = (currentPeakTimeMs > 0) ? static_cast<double>(elapsedMs) / currentPeakTimeMs : 1.0; calculated_speed = currentOvershootPeakSpeed * (1.0 - std::pow(1.0 - phase_progress, 2)); }
                     else if (elapsedMs < currentDipTimeMs) { double phase_duration = currentDipTimeMs - currentPeakTimeMs; double phase_progress = (phase_duration > 0) ? static_cast<double>(elapsedMs - currentPeakTimeMs) / phase_duration : 1.0; calculated_speed = currentOvershootPeakSpeed + (baseOvershootDipSpeed - currentOvershootPeakSpeed) * std::pow(phase_progress, 2); }
                     else if (elapsedMs < currentRecoverTimeMs) { double phase_duration = currentRecoverTimeMs - currentDipTimeMs; double phase_progress = (phase_duration > 0) ? static_cast<double>(elapsedMs - currentDipTimeMs) / phase_duration : 1.0; calculated_speed = baseOvershootDipSpeed + (1.0 - baseOvershootDipSpeed) * std::pow(phase_progress, 2); }

                     playback_rate.store(calculated_speed);
                     calculate_and_set_volume(calculated_speed);
                     std::this_thread::sleep_for(std::chrono::milliseconds(5));
                 }
                 playback_rate.store(1.0);
                 target_playback_rate.store(1.0);
             } else { // should_overshoot is false
                 // --- Simple Smooth Ramp-Up (No Overshoot) ---
                 volume.store(1.0f);
                 const int rampUpDurationMs = 100;
                 auto ramp_start_time = std::chrono::steady_clock::now();

                 while (true) {
                     // ... (Simple ramp-up logic) ...
                     auto now = std::chrono::steady_clock::now();
                     auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - ramp_start_time);
                     int elapsedMs = elapsed.count();
                     if (elapsedMs >= rampUpDurationMs) { break; }
                     double progress = static_cast<double>(elapsedMs) / rampUpDurationMs;
                     double calculated_speed = progress * 1.0;
                     playback_rate.store(calculated_speed);
                     calculate_and_set_volume(calculated_speed);
                     std::this_thread::sleep_for(std::chrono::milliseconds(5));
                 }
                 playback_rate.store(1.0);
                 target_playback_rate.store(1.0);
             }
            // Update current/target to prevent immediate adjustment in next section
            current = 1.0; 
            target = 1.0;
            interval = normal_interval; // Ensure normal interval after resume/overshoot
        }
        // --- Handle OTHER speed changes (including PAUSING) --- 
        else if (current != target) { 
            if (is_pausing) {
                 interval = pause_interval; // Use shorter interval for faster pausing
            } else {
                 interval = normal_interval;
            }

            double diff = target - current;
            double step_multiplier = is_pausing ? 0.15 : 0.1; // Make pausing slightly faster
            double interpolated_step = std::min(std::abs(diff), std::max(0.01, std::abs(diff) * step_multiplier)); // Ensure minimum step
            
            // Adjust current rate towards target
            if (diff > 0) {
                playback_rate.store(current + interpolated_step);
                calculate_and_set_volume(current + interpolated_step);
            } else {
                playback_rate.store(current - interpolated_step);
                calculate_and_set_volume(current - interpolated_step);
            }

             // If very close to target, snap to target
             if (std::abs(playback_rate.load() - target) < 0.01) {
                 playback_rate.store(target);
                 calculate_and_set_volume(target);
             } 
        } 
        // --- No Change Needed --- 
        // else { /* current == target and not jogging/resuming */ }
 
        std::this_thread::sleep_for(std::chrono::milliseconds(interval));
    }
}

void toggle_pause() {
    if (target_playback_rate.load() == 0.0) {
        target_playback_rate.store(1.0);
    } else {
        target_playback_rate.store(0.0);
    }
}

void check_audio_stream(AVFormatContext* format_ctx, int audio_stream_index) {
    AVStream* audio_stream = format_ctx->streams[audio_stream_index];
    AVCodecParameters* codecpar = audio_stream->codecpar;

    std::cout << "Audio stream details:" << std::endl;
    std::cout << "  Codec: " << avcodec_get_name(codecpar->codec_id) << std::endl;
    std::cout << "  Sample rate: " << codecpar->sample_rate << " Hz" << std::endl;
    std::cout << "  Channels: " << codecpar->ch_layout.nb_channels << std::endl;
    std::cout << "  Bit rate: " << codecpar->bit_rate << " bps" << std::endl;
    std::cout << "  Duration: " << av_rescale_q(audio_stream->duration, audio_stream->time_base, {1, AV_TIME_BASE}) / 1000000.0 << " seconds" << std::endl;
}

static int patestCallback(const void *inputBuffer, void *outputBuffer,
                          unsigned long framesPerBuffer,
                          const PaStreamCallbackTimeInfo* timeInfo,
                          PaStreamCallbackFlags statusFlags,
                          void *userData) {
    float *out = (float*)outputBuffer;
    (void) inputBuffer; 

    // Check if audio buffer is empty or not initialized
    if (audio_buffer.empty()) {
        // Fill output buffer with silence
        for (unsigned int i = 0; i < framesPerBuffer * 2; i++) {
            *out++ = 0.0f;
        }
        return paContinue;
    }

    double rate = playback_rate.load();
    if (std::abs(rate) < 0.001) {
        for (unsigned int i = 0; i < framesPerBuffer * 2; i++) {
            *out++ = 0.0f;
        }
        return paContinue;
    }

    float current_volume = volume.load(); // Громкость считывается ПОСЛЕ проверки rate == 0.0
    double position = audio_buffer_index;

    // Helper function for Catmull-Rom interpolation
    auto interpolate_catmull_rom = [](float p0, float p1, float p2, float p3, float t) -> float {
        // Formula from https://en.wikipedia.org/wiki/Cubic_Hermite_spline#Catmull%E2%80%93Rom_spline
        // (Slightly rearranged for efficiency)
        float t2 = t * t;
        float t3 = t2 * t;
        return 0.5f * (
            (2.0f * p1) +
            (-p0 + p2) * t +
            (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
            (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3
        );
    };

    size_t buffer_half_size = audio_buffer.size() / 2;

    for (unsigned int i = 0; i < framesPerBuffer; i++) {
        if (is_reverse.load()) {
            position = std::max(0.0, position - rate);
        } else {
            position = std::min(static_cast<double>(buffer_half_size - 2), position + rate);
        }

        // --- Cubic Interpolation Logic ---
        // Check bounds (need index - 1 to index + 2)
        if (position >= 1.0 && position < static_cast<double>(buffer_half_size - 2) && audio_buffer.size() >= 8) 
        { // Can perform Cubic interpolation

            size_t index1 = static_cast<size_t>(position); // Index before the fractional part
            float frac = static_cast<float>(position - index1); // Fractional part

            // Get the 4 points for interpolation (indices: index1-1, index1, index1+1, index1+2)
            size_t idx0 = (index1 - 1) * 2;
            size_t idx1 = index1 * 2;
            size_t idx2 = (index1 + 1) * 2;
            size_t idx3 = (index1 + 2) * 2;

            // Left channel interpolation
            float yL0 = audio_buffer[idx0];
            float yL1 = audio_buffer[idx1];
            float yL2 = audio_buffer[idx2];
            float yL3 = audio_buffer[idx3];
            float interpolated_left = interpolate_catmull_rom(yL0, yL1, yL2, yL3, frac);

            // Right channel interpolation
            float yR0 = audio_buffer[idx0 + 1];
            float yR1 = audio_buffer[idx1 + 1];
            float yR2 = audio_buffer[idx2 + 1];
            float yR3 = audio_buffer[idx3 + 1];
            float interpolated_right = interpolate_catmull_rom(yR0, yR1, yR2, yR3, frac);

            // Apply volume and output
            *out++ = interpolated_left * current_volume;
            *out++ = interpolated_right * current_volume;

        } else if (position >= 0.0 && position < static_cast<double>(buffer_half_size - 1) && audio_buffer.size() >= 4) 
        { // Can perform Linear interpolation (near edges)

            size_t index1 = static_cast<size_t>(position); // Index before the fractional part
            float frac = static_cast<float>(position - index1); // Fractional part

            // Get the 2 points for interpolation (indices: index1, index1+1)
            size_t idx1 = index1 * 2;
            size_t idx2 = (index1 + 1) * 2;

            // Left channel interpolation
            float yL1 = audio_buffer[idx1];
            float yL2 = audio_buffer[idx2];
            float interpolated_left = yL1 + frac * (yL2 - yL1);

            // Right channel interpolation
            float yR1 = audio_buffer[idx1 + 1];
            float yR2 = audio_buffer[idx2 + 1];
            float interpolated_right = yR1 + frac * (yR2 - yR1);

             // Apply volume and output
            *out++ = interpolated_left * current_volume;
            *out++ = interpolated_right * current_volume;

        } else { // Cannot interpolate (too close to edge or buffer too small)
            // Cannot interpolate near edges or if buffer is too small
            *out++ = 0.0f;
            *out++ = 0.0f;
        }
        // --- End Interpolation Logic ---
    }

    audio_buffer_index = static_cast<size_t>(position);

    // Prevent division by zero if sample_rate is 0
    double time_per_sample = sample_rate.load() > 0 ? 1.0 / sample_rate.load() : 0.0;
    
    // Check if audio_buffer is valid before calculating times
    if (!audio_buffer.empty()) {
        double elapsed_time = audio_buffer_index * time_per_sample;
        double total_time = (audio_buffer.size() / 2) * time_per_sample; 
        double adjusted_total_time = total_time - 0.1; 

        elapsed_time = std::max(0.0, std::min(elapsed_time, adjusted_total_time));
        current_audio_time.store(elapsed_time, std::memory_order_release);
    }

    return paContinue;
}

void decode_audio(const char* filename) {
    AVFormatContext* format_ctx = nullptr;
    AVCodecContext* audio_codec_ctx = nullptr;
    const AVCodec* audio_codec = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = av_packet_alloc();
    int audio_stream_index = -1;

    try {
        std::cout << "Starting audio decoding..." << std::endl;
        std::cout << "FFmpeg version: " << av_version_info() << std::endl;

        if (avformat_open_input(&format_ctx, filename, nullptr, nullptr) != 0) {
            throw std::runtime_error("Could not open file");
        }
        if (avformat_find_stream_info(format_ctx, nullptr) < 0) {
            throw std::runtime_error("Could not find stream information");
        }

        for (unsigned int i = 0; i < format_ctx->nb_streams; i++) {
            std::cout << "Stream #" << i << " type: " 
                      << av_get_media_type_string(format_ctx->streams[i]->codecpar->codec_type) 
                      << ", codec: " << avcodec_get_name(format_ctx->streams[i]->codecpar->codec_id) << std::endl;
        }

        audio_stream_index = av_find_best_stream(format_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, &audio_codec, 0);
        if (audio_stream_index < 0) {
            throw std::runtime_error("Could not find audio stream");
        }

        std::cout << "Audio stream index: " << audio_stream_index << std::endl;
        std::cout << "Audio codec: " << avcodec_get_name(audio_codec->id) << std::endl;

        check_audio_stream(format_ctx, audio_stream_index);

        audio_codec_ctx = avcodec_alloc_context3(audio_codec);
        if (!audio_codec_ctx) {
            throw std::runtime_error("Could not allocate audio codec context");
        }

        if (avcodec_parameters_to_context(audio_codec_ctx, format_ctx->streams[audio_stream_index]->codecpar) < 0) {
            throw std::runtime_error("Could not set codec parameters");
        }

        if (avcodec_open2(audio_codec_ctx, audio_codec, nullptr) < 0) {
            throw std::runtime_error("Could not open audio codec");
        }

        frame = av_frame_alloc();
        if (!frame) {
            throw std::runtime_error("Could not allocate frame");
        }

        std::cout << "Audio decoding setup complete, starting to read frames..." << std::endl;

        sample_rate.store(audio_codec_ctx->sample_rate);
        std::cout << "Audio sample rate: " << sample_rate.load() << " Hz" << std::endl;

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        int frame_count = 0;
        int packet_count = 0;
        int ret;
        while ((ret = av_read_frame(format_ctx, packet)) >= 0 && !quit.load()) {
            if (packet->stream_index == audio_stream_index) {
                packet_count++;
                ret = avcodec_send_packet(audio_codec_ctx, packet);
                if (ret < 0) {
                    std::cerr << "Error sending packet for decoding: " << av_err2str(ret) << std::endl;
                    continue;
                }

                while (ret >= 0) {
                    ret = avcodec_receive_frame(audio_codec_ctx, frame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                        break;
                    } else if (ret < 0) {
                        std::cerr << "Error during decoding: " << av_err2str(ret) << std::endl;
                        break;
                    }

                    if (audio_codec_ctx->sample_fmt == AV_SAMPLE_FMT_FLTP) {
                        for (int i = 0; i < frame->nb_samples; ++i) {
                            for (int ch = 0; ch < frame->ch_layout.nb_channels; ++ch) {
                                float sample = reinterpret_cast<float*>(frame->data[ch])[i];
                                audio_buffer.push_back(sample);
                            }
                        }
                    } else if (audio_codec_ctx->sample_fmt == AV_SAMPLE_FMT_S16P) {
                        for (int i = 0; i < frame->nb_samples; ++i) {
                            for (int ch = 0; ch < frame->ch_layout.nb_channels; ++ch) {
                                int16_t sample = reinterpret_cast<int16_t*>(frame->data[ch])[i];
                                audio_buffer.push_back(sample / 32768.0f);
                            }
                        }
                    } else if (audio_codec_ctx->sample_fmt == AV_SAMPLE_FMT_S16) {
                        int16_t* samples = reinterpret_cast<int16_t*>(frame->data[0]);
                        for (int i = 0; i < frame->nb_samples * frame->ch_layout.nb_channels; ++i) {
                            audio_buffer.push_back(samples[i] / 32768.0f);
                        }
                    } else {
                        std::cerr << "Unsupported audio format: " << av_get_sample_fmt_name(audio_codec_ctx->sample_fmt) << std::endl;
                    }

                    frame_count++;
                    if (frame_count % 1000 == 0) {
                    }
                }
            }
            av_packet_unref(packet);
            if (packet_count % 1000 == 0) {
            }
        }

        if (ret < 0 && ret != AVERROR_EOF) {
            std::cerr << "Error reading frame: " << av_err2str(ret) << std::endl;
        }

        std::cout << "Audio decoding finished. Total frames: " << frame_count << ", Total packets: " << packet_count << std::endl;
        decoding_finished.store(true);
        decoding_completed.store(true);  
    }
    catch (const std::exception& e) {
        std::cerr << "Decoding error: " << e.what() << std::endl;
        decoding_finished.store(true);
    }

    if (frame) av_frame_free(&frame);
    if (audio_codec_ctx) avcodec_free_context(&audio_codec_ctx);
    if (format_ctx) avformat_close_input(&format_ctx);
    av_packet_free(&packet);
}

void start_audio(const char* filename) {
    try {
        std::cout << "Starting audio initialization..." << std::endl;

        // Clear any existing audio buffer
        audio_buffer.clear();
        audio_buffer_index = 0;
        decoding_finished.store(false);
        decoding_completed.store(false);

        PaError err;
        err = Pa_Initialize();
        if (err != paNoError) {
            throw std::runtime_error("PortAudio error: " + std::string(Pa_GetErrorText(err)));
        }

        PaStreamParameters outputParameters;

        // Use selected device if set, otherwise use default
        int deviceIndex;
        if (selected_audio_device_index.load() >= 0) {
            deviceIndex = selected_audio_device_index.load();
            std::cout << "Using previously selected audio device (index " << deviceIndex << ")" << std::endl;
        } else {
            deviceIndex = Pa_GetDefaultOutputDevice();
            std::cout << "Using default audio device (index " << deviceIndex << ")" << std::endl;
        }
        
        if (deviceIndex == paNoDevice) {
            throw std::runtime_error("No default output device.");
        }
        
        // Check if device supports output
        const PaDeviceInfo* deviceInfo = Pa_GetDeviceInfo(deviceIndex);
        if (!deviceInfo || deviceInfo->maxOutputChannels <= 0) {
            std::cerr << "Selected device does not support output, falling back to default" << std::endl;
            deviceIndex = Pa_GetDefaultOutputDevice();
            if (deviceIndex == paNoDevice) {
                throw std::runtime_error("No default output device.");
            }
        }
        
        // Store the current device index
        current_audio_device_index.store(deviceIndex);
        
        // Make sure the menu_to_device_index map is initialized
        get_audio_output_devices();
        
        outputParameters.device = deviceIndex;
        outputParameters.channelCount = 2;
        outputParameters.sampleFormat = paFloat32;
        outputParameters.suggestedLatency = Pa_GetDeviceInfo(outputParameters.device)->defaultLowOutputLatency;
        outputParameters.hostApiSpecificStreamInfo = NULL;

        // Start decoding audio in a separate thread
        std::thread decoding_thread([filename]() {
            decode_audio(filename);
        });
        decoding_thread.detach(); 

        // Give the decoding thread a moment to start
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Close any existing stream
        if (stream) {
            Pa_StopStream(stream);
            Pa_CloseStream(stream);
            stream = nullptr;
        }

        err = Pa_OpenStream(
            &stream,
            NULL, 
            &outputParameters,
            sample_rate.load(), 
            256,   
            paClipOff,
            patestCallback,
            NULL); 

        if (err != paNoError) {
            throw std::runtime_error("PortAudio error: " + std::string(Pa_GetErrorText(err)));
        }

        err = Pa_StartStream(stream);
        if (err != paNoError) {
            throw std::runtime_error("PortAudio error: " + std::string(Pa_GetErrorText(err)));
        }

        std::cout << "Audio device opened successfully" << std::endl;

        audio_buffer_index = 0;
        current_audio_time.store(0.0);
        playback_rate.store(0.0);
        target_playback_rate.store(0.0);

        std::cout << "Audio playback started. Decoding continues in background." << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Audio Error: " << e.what() << std::endl;
        // Don't quit the application on audio error, just report it
        // quit.store(true);
    }
}

std::string format_time(double time_in_seconds, int fps) {
    int hours = static_cast<int>(time_in_seconds / 3600);
    int minutes = static_cast<int>((time_in_seconds - hours * 3600) / 60);
    int seconds = static_cast<int>(time_in_seconds) % 60;
    int frames = static_cast<int>((time_in_seconds - static_cast<int>(time_in_seconds)) * fps);

    std::ostringstream oss;
    oss << std::setw(2) << std::setfill('0') << hours << ":"
        << std::setw(2) << std::setfill('0') << minutes << ":"
        << std::setw(2) << std::setfill('0') << seconds << ":"
        << std::setw(2) << std::setfill('0') << frames;
    return oss.str();
}

gint64 get_total_duration() {
    gint64 duration = static_cast<gint64>(total_duration.load() * GST_SECOND);
    return duration;
}

double get_current_audio_time() {
    return current_audio_time.load();
}

double get_original_fps() {
    return original_fps.load();
}

std::string generateTXTimecode(double time) {
    double fps = original_fps.load();
    double total_dur = total_duration.load();

    // Hard limit time to file duration
    time = std::min(std::max(time, 0.0), total_dur);

    int64_t total_frames = static_cast<int64_t>(std::round(time * fps));
    int64_t max_frames = static_cast<int64_t>(std::floor(total_dur * fps));

    // Hard limit frame count
    total_frames = std::min(total_frames, max_frames);

    int hours = static_cast<int>(total_frames / (3600 * fps));
    int minutes = static_cast<int>((total_frames / (60 * fps))) % 60;
    int seconds = static_cast<int>((total_frames / fps)) % 60;
    int frames = static_cast<int>(total_frames % static_cast<int64_t>(std::round(fps)));

    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(2) << hours << ":"
        << std::setfill('0') << std::setw(2) << minutes << ":"
        << std::setfill('0') << std::setw(2) << seconds << ":"
        << std::setfill('0') << std::setw(2) << frames;
    return oss.str();
}

double get_video_fps(const char* filename) {
    AVFormatContext* format_ctx = avformat_alloc_context();
    if (avformat_open_input(&format_ctx, filename, nullptr, nullptr) != 0) {
        return 25.0;
    }

    double fps = 25.0;
    for (unsigned int i = 0; i < format_ctx->nb_streams; i++) {
        if (format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            AVRational frame_rate = av_guess_frame_rate(format_ctx, format_ctx->streams[i], NULL);
            if (frame_rate.num && frame_rate.den) {
                double calculated_fps = static_cast<double>(frame_rate.num) / frame_rate.den;
                
                if (std::abs(calculated_fps - 29.97) < 0.01) {
                    fps = 30000.0 / 1001.0;
                } else if (std::abs(calculated_fps - 59.94) < 0.01) {
                    fps = 60000.0 / 1001.0;
                } else {
                    fps = calculated_fps;
                }
                break;
            }
        }
    }

    avformat_close_input(&format_ctx);
    return fps;
}

double get_file_duration(const char* filename) {
    AVFormatContext* format_ctx = avformat_alloc_context();
    double duration = 0.0;

    if (avformat_open_input(&format_ctx, filename, nullptr, nullptr) == 0) {
        if (avformat_find_stream_info(format_ctx, nullptr) >= 0) {
            duration = static_cast<double>(format_ctx->duration) / AV_TIME_BASE;
        }
        avformat_close_input(&format_ctx);
    }

    return duration;
}

void increase_volume() {
    float current = volume.load();
    float new_volume = std::min(current + 0.1f, 1.0f);
    volume.store(new_volume);
}

void decrease_volume() {
    float current = volume.load();
    float new_volume = std::max(current - 0.1f, 0.0f);
    volume.store(new_volume);
}

void start_jog_forward() {
    jog_forward.store(true);
    jog_backward.store(false);
    target_playback_rate.store(JOG_SPEED);
    is_reverse.store(false);
}

void start_jog_backward() {
    jog_backward.store(true);
    jog_forward.store(false);
    target_playback_rate.store(JOG_SPEED);
    is_reverse.store(true);
}

void stop_jog() {
    jog_forward.store(false);
    jog_backward.store(false);
    // Force immediate stop when jog ends
    target_playback_rate.store(0.0);
    playback_rate.store(0.0);
    // Also update volume immediately to match paused state
    volume.store(0.0f); 
}

void seek_to_time(double target_time) {
    if (target_time < 0) {
        target_time = 0;
    } else if (target_time > total_duration.load()) {
        target_time = total_duration.load();
    }

    size_t target_index = static_cast<size_t>(target_time * sample_rate.load());
    audio_buffer_index = target_index;
    current_audio_time.store(target_time);
    seek_performed.store(true);
}

double parse_timecode(const std::string& timecode) {
    std::string padded_timecode = timecode + std::string(8 - timecode.length(), '0');

    int hours = std::stoi(padded_timecode.substr(0, 2));
    int minutes = std::stoi(padded_timecode.substr(2, 2));
    int seconds = std::stoi(padded_timecode.substr(4, 2));
    int frames = std::stoi(padded_timecode.substr(6, 2));

    if (hours > 23 || minutes > 59 || seconds > 59) {
        throw std::runtime_error("Invalid timecode: hours, minutes, or seconds out of range");
    }

    double fps = original_fps.load();
    if (frames >= fps) {
        throw std::runtime_error("Invalid timecode: frames exceed FPS");
    }

    return hours * 3600.0 + minutes * 60.0 + seconds + frames / fps;
}

// Function to get the number of available audio devices
int get_audio_device_count() {
    return Pa_GetDeviceCount();
}

// Function to get the name of an audio device by index
std::string get_audio_device_name(int index) {
    const PaDeviceInfo* deviceInfo = Pa_GetDeviceInfo(index);
    if (deviceInfo) {
        return deviceInfo->name;
    }
    return "Unknown Device";
}

// Function to get the current audio device index as a menu index
int get_current_audio_device_index() {
    int currentDeviceIndex = current_audio_device_index.load();
    
    // Find the menu index that corresponds to this device index
    for (const auto& pair : menu_to_device_index) {
        if (pair.second == currentDeviceIndex) {
            return pair.first;
        }
    }
    
    // If not found, return -1
    return -1;
}

// Function to get a list of all available audio output devices with their actual indices
std::vector<std::string> get_audio_output_devices() {
    std::vector<std::string> devices;
    menu_to_device_index.clear();
    
    // Initialize PortAudio if needed
    static bool pa_initialized = false;
    if (!pa_initialized) {
        PaError err = Pa_Initialize();
        if (err != paNoError) {
            return devices; // Return empty list on error
        }
        pa_initialized = true;
    }
    
    int numDevices = Pa_GetDeviceCount();
    if (numDevices < 0) {
        return devices; // Return empty list on error
    }
    
    // Add all output devices to the list
    int menuIndex = 0;
    for (int i = 0; i < numDevices; i++) {
        const PaDeviceInfo* deviceInfo = Pa_GetDeviceInfo(i);
        if (deviceInfo && deviceInfo->maxOutputChannels > 0) {
            devices.push_back(deviceInfo->name);
            menu_to_device_index[menuIndex] = i;  // Map menu index to actual device index
            menuIndex++;
        }
    }
    
    return devices;
}

// Function to switch to a different audio device
bool switch_audio_device(int menuIndex) {
    std::lock_guard<std::mutex> lock(audio_device_mutex);
    
    // Get the actual device index from the menu index
    if (menu_to_device_index.find(menuIndex) == menu_to_device_index.end()) {
        std::cerr << "Invalid menu index: " << menuIndex << std::endl;
        return false;
    }
    
    int deviceIndex = menu_to_device_index[menuIndex];
    
    // Check if the device index is valid
    if (deviceIndex < 0 || deviceIndex >= Pa_GetDeviceCount()) {
        std::cerr << "Invalid device index: " << deviceIndex << std::endl;
        return false;
    }
    
    // Check if the device supports output
    const PaDeviceInfo* deviceInfo = Pa_GetDeviceInfo(deviceIndex);
    if (!deviceInfo || deviceInfo->maxOutputChannels <= 0) {
        std::cerr << "Device does not support output" << std::endl;
        return false;
    }
    
    // Сохраняем выбранный индекс устройства
    selected_audio_device_index.store(deviceIndex);
    
    // If we're already using this device, do nothing
    if (deviceIndex == current_audio_device_index.load() && stream) {
        return true;
    }
    
    // Remember the current playback state
    double current_time = current_audio_time.load();
    double current_rate = playback_rate.load();
    double current_target_rate = target_playback_rate.load();
    bool current_reverse = is_reverse.load();
    
    // Stop and close the current stream
    if (stream) {
        Pa_StopStream(stream);
        Pa_CloseStream(stream);
        stream = nullptr;
    }
    
    // Set up new stream parameters
    PaStreamParameters outputParameters;
    outputParameters.device = deviceIndex;
    outputParameters.channelCount = 2;
    outputParameters.sampleFormat = paFloat32;
    outputParameters.suggestedLatency = Pa_GetDeviceInfo(deviceIndex)->defaultLowOutputLatency;
    outputParameters.hostApiSpecificStreamInfo = NULL;
    
    // Open and start the new stream
    PaError err = Pa_OpenStream(
        &stream,
        NULL, 
        &outputParameters,
        sample_rate.load(), 
        256,   
        paClipOff,
        patestCallback,
        NULL);
    
    if (err != paNoError) {
        std::cerr << "Failed to open new audio device: " << Pa_GetErrorText(err) << std::endl;
        return false;
    }
    
    err = Pa_StartStream(stream);
    if (err != paNoError) {
        std::cerr << "Failed to start new audio device: " << Pa_GetErrorText(err) << std::endl;
        Pa_CloseStream(stream);
        stream = nullptr;
        return false;
    }
    
    // Update the current device index
    current_audio_device_index.store(deviceIndex);
    
    // Restore playback state
    current_audio_time.store(current_time);
    playback_rate.store(current_rate);
    target_playback_rate.store(current_target_rate);
    is_reverse.store(current_reverse);
    
    std::cout << "Switched to audio device: " << deviceInfo->name << std::endl;
    return true;
}

void cleanup_audio() {
    std::lock_guard<std::mutex> lock(audio_device_mutex);
    
    // Set volume to 0 immediately to silence output
    volume.store(0.0f);
    // Give the callback a tiny bit of time to process with zero volume
    std::this_thread::sleep_for(std::chrono::milliseconds(10)); 
    
    try {
        if (stream) {
            PaError err = Pa_StopStream(stream);
            if (err != paNoError) {
                std::cerr << "Error stopping stream: " << Pa_GetErrorText(err) << std::endl;
            }
            
            err = Pa_CloseStream(stream);
            if (err != paNoError) {
                std::cerr << "Error closing stream: " << Pa_GetErrorText(err) << std::endl;
            }
            stream = nullptr;
        }
        
        PaError err = Pa_Terminate();
        if (err != paNoError) {
            std::cerr << "Error terminating PortAudio: " << Pa_GetErrorText(err) << std::endl;
        }
        
        // Clear audio buffer to free memory
        std::vector<float>().swap(audio_buffer);
        audio_buffer_index = 0;
        
        std::cout << "Audio system cleaned up successfully" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Error during audio cleanup: " << e.what() << std::endl;
    }
}
