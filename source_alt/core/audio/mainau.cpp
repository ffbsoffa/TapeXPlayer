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
#include <limits> // Needed for std::numeric_limits
#include <cstring> // For strerror
#include <algorithm> // For std::remove

// Headers for mmap and file operations
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstdio> // For mkstemp, unlink

// Use int16_t for audio buffer to save memory
// std::vector<int16_t> audio_buffer;

// Add mmap state variables
std::string audio_temp_filename; // Path to the mmap temp file
int audio_write_fd = -1;         // FD for decoder writing
int audio_read_fd = -1;          // FD for player reading
int16_t* audio_write_ptr = nullptr; // Mmap pointer for writing
const int16_t* audio_read_ptr = nullptr; // Mmap pointer for reading
size_t audio_total_bytes = 0;      // Total size of the mapped file in bytes
size_t audio_total_samples = 0;    // Total number of int16_t samples expected
std::atomic<size_t> audio_decoded_samples_count(0); // Atomic counter for available samples
std::mutex mmap_init_mutex; // Mutex to protect access during setup/cleanup

// Playback position (remains similar)
double audio_buffer_index = 0.0; // Fractional sample index (stereo pairs)

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
    const double normal_step = 2.0;
    const double pause_step = 2.0; // Still needed for pausing interpolation
    const int normal_interval = 14; // Re-add missing constant
    const int pause_interval = 14;  // Still needed for pausing interpolation

    // --- Base Overshoot Curve Parameters (for scaling) ---
    const double baseOvershootTotalDurationMs = 350.0;
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

    while (!quit.load() && !shouldExit.load()) {
        double current = playback_rate.load();
        double target = target_playback_rate.load();

        bool is_pausing = (target == 0.0 && current > 0.0);
        bool is_resuming = (std::abs(current) < 0.001 && target > 0.0);
        bool is_jogging = jog_forward.load() || jog_backward.load();

        int interval = normal_interval; // Default interval

        // --- Check for 3.0x speed exception ---
        if (std::abs(target - 3.0) < 0.01) { // Check if target speed is 3.0x
            interval = 4;
        } else {
            interval = normal_interval; // Default for other speeds
        }
        // --- End 3.0x speed exception ---

        // --- Prioritize JOGGING check --- 
        if (is_jogging) {
            playback_rate.store(JOG_SPEED); // Set speed directly to JOG_SPEED
            calculate_and_set_volume(JOG_SPEED);
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
                     if (quit.load() || shouldExit.load()) break; // Check for exit signal
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
                     if (quit.load() || shouldExit.load()) break; // Check for exit signal
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
        }
        // --- Handle OTHER speed changes (including PAUSING) --- 
        else if (current != target) { 
            if (is_pausing) {
                 interval = pause_interval; // Use shorter interval for faster pausing
            } else {
                 // Interval is already set based on target (3.0x exception) or default normal_interval
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
    (void) timeInfo;
    (void) statusFlags;
    (void) userData;

    static double beep_phase = 0.0;
    static int beep_counter = 0;
    
    // Get mmap pointer and decoded count (check if ready)
    const int16_t* current_read_ptr = audio_read_ptr;
    size_t current_total_samples = audio_total_samples;
    size_t available_samples = audio_decoded_samples_count.load(std::memory_order_acquire);
    double rate = playback_rate.load();
    double target_rate = target_playback_rate.load();
    
    // Check boundaries based on target rate instead of current rate
    const bool is_at_start = (audio_buffer_index <= 0.1) && std::abs(target_rate) >= 1.5;
    const bool is_at_end = (audio_buffer_index >= ((available_samples / 2) - 1)) && std::abs(target_rate) >= 1.5;
    const bool is_at_boundary = is_at_start || is_at_end;

    // If mmap not ready or no samples available/decoded yet, output silence
    if (current_read_ptr == nullptr || current_total_samples == 0) {
        for (unsigned int i = 0; i < framesPerBuffer * 2; ++i) {
            *out++ = 0.0f;
        }
        return paContinue;
    }

    // Check for pause state
    if (std::abs(rate) < 0.001) {
        for (unsigned int i = 0; i < framesPerBuffer * 2; ++i) {
            *out++ = 0.0f;
        }
        return paContinue;
    }

    float current_volume = volume.load();
    double current_position = audio_buffer_index;
    bool reverse = is_reverse.load();
    int current_channels = 2;
    size_t buffer_num_sample_pairs = current_total_samples / current_channels;

    // Helper function for converting int16_t sample to float
    auto int16_to_float = [](int16_t sample) -> float {
        return static_cast<float>(sample) / 32768.0f;
    };

    // Helper function for Catmull-Rom interpolation (operates on floats)
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

    for (unsigned int i = 0; i < framesPerBuffer; i++) {
        float left_sample = 0.0f;
        float right_sample = 0.0f;

        // Generate beep if at boundary - independent of main volume
        if (is_at_boundary) {
            beep_counter++;
            if (beep_counter < sample_rate.load() * 0.048) { // 48ms on
                beep_phase += 2.0 * M_PI * 2000.0 / sample_rate.load();
                if (beep_phase >= 2.0 * M_PI) beep_phase -= 2.0 * M_PI;
                // 0.02 amplitude = -34 dB
                const float beep_volume = 0.02f;
                float beep = sinf(beep_phase) * beep_volume;
                // Add beep directly to output, will not be affected by main volume
                *out++ = beep;
                *out++ = beep;
                continue; // Skip normal audio processing when beeping
            } else if (beep_counter >= sample_rate.load() * 0.096) { // Reset after 96ms (48ms on + 48ms off)
                beep_counter = 0;
            }
        } else {
            beep_counter = 0;
            beep_phase = 0.0;
        }

        // Normal audio processing
        // --- Calculate next position --- 
        if (reverse) {
            current_position -= rate;
            current_position = std::max(0.0, current_position);
        } else {
            current_position += rate;
            size_t available_pairs = available_samples / current_channels;
            if (available_pairs > 0) {
                current_position = std::min(current_position, static_cast<double>(available_pairs - 1));
            }
        }

        // --- Check Data Availability and Interpolate --- 
        size_t index1 = static_cast<size_t>(current_position);
        float frac = static_cast<float>(current_position - index1);

        size_t idx_pair0_rel = (index1 >= 1) ? index1 - 1 : 0;
        size_t idx_pair1_rel = index1;
        size_t idx_pair2_rel = index1 + 1;
        size_t idx_pair3_rel = index1 + 2;

        size_t max_needed_abs_sample_index = idx_pair3_rel * current_channels + (current_channels - 1);

        if (idx_pair3_rel < buffer_num_sample_pairs && max_needed_abs_sample_index < available_samples) {
            size_t abs_idx0 = idx_pair0_rel * current_channels;
            size_t abs_idx1 = idx_pair1_rel * current_channels;
            size_t abs_idx2 = idx_pair2_rel * current_channels;
            size_t abs_idx3 = idx_pair3_rel * current_channels;

            int16_t sL0 = current_read_ptr[abs_idx0];
            int16_t sL1 = current_read_ptr[abs_idx1];
            int16_t sL2 = current_read_ptr[abs_idx2];
            int16_t sL3 = current_read_ptr[abs_idx3];
            int16_t sR0 = current_read_ptr[abs_idx0 + 1];
            int16_t sR1 = current_read_ptr[abs_idx1 + 1];
            int16_t sR2 = current_read_ptr[abs_idx2 + 1];
            int16_t sR3 = current_read_ptr[abs_idx3 + 1];

            float yL0 = int16_to_float(sL0); float yL1 = int16_to_float(sL1);
            float yL2 = int16_to_float(sL2); float yL3 = int16_to_float(sL3);
            float yR0 = int16_to_float(sR0); float yR1 = int16_to_float(sR1);
            float yR2 = int16_to_float(sR2); float yR3 = int16_to_float(sR3);

            float interpolated_left = interpolate_catmull_rom(yL0, yL1, yL2, yL3, frac);
            float interpolated_right = interpolate_catmull_rom(yR0, yR1, yR2, yR3, frac);

            left_sample = interpolated_left;
            right_sample = interpolated_right;
        }

        // Apply main volume only to normal audio
        *out++ = left_sample * current_volume;
        *out++ = right_sample * current_volume;
    }

    // Store the final position back to the global variable
    audio_buffer_index = current_position;

    // --- Update current_audio_time based on mmap position --- 
    int current_sample_rate = sample_rate.load();
    if (current_sample_rate > 0) {
        double time_per_sample_pair = 1.0 / current_sample_rate;
        double elapsed_time = audio_buffer_index * time_per_sample_pair; 
        
        // Clamp time to total duration if known
        double total_dur = total_duration.load();
        if (total_dur > 0) {
            elapsed_time = std::min(elapsed_time, total_dur - 0.01);
        }
        elapsed_time = std::max(0.0, elapsed_time);
        
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

    // --- Reset mmap state before decoding --- 
    std::lock_guard<std::mutex> lock(mmap_init_mutex); // Protect cleanup/setup
    // Cleanup previous mmap if any (e.g., if a previous decode failed midway)
    if (audio_read_ptr) { munmap((void*)audio_read_ptr, audio_total_bytes); audio_read_ptr = nullptr; }
    if (audio_write_ptr) { munmap(audio_write_ptr, audio_total_bytes); audio_write_ptr = nullptr; }
    if (audio_read_fd != -1) { close(audio_read_fd); audio_read_fd = -1; }
    if (audio_write_fd != -1) { close(audio_write_fd); audio_write_fd = -1; }
    if (!audio_temp_filename.empty()) { unlink(audio_temp_filename.c_str()); audio_temp_filename.clear(); }
    audio_total_bytes = 0;
    audio_total_samples = 0;
    audio_decoded_samples_count.store(0); 
    decoding_finished.store(false); 
    decoding_completed.store(false);
    // --- End Reset --- 
    // Mutex lock released here

    try {
        std::cout << "Starting audio decoding for mmap..." << std::endl;
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

        std::cout << "Audio decoding setup complete." << std::endl;

        // --- Determine Size and Create/Truncate Temp File --- 
        int current_sample_rate = audio_codec_ctx->sample_rate;
        int current_channels = audio_codec_ctx->ch_layout.nb_channels;
        double duration_sec = 0.0;
        if (format_ctx->duration != AV_NOPTS_VALUE) {
             duration_sec = static_cast<double>(format_ctx->duration) / AV_TIME_BASE;
        }
        else if (format_ctx->streams[audio_stream_index]->duration != AV_NOPTS_VALUE) {
            AVRational time_base = format_ctx->streams[audio_stream_index]->time_base;
             duration_sec = static_cast<double>(format_ctx->streams[audio_stream_index]->duration) * time_base.num / time_base.den;
        }

        if (duration_sec <= 0 || current_sample_rate <= 0 || current_channels <= 0) {
             throw std::runtime_error("Could not determine audio duration/parameters for mmap.");
        }

        // Add 10% safety margin to the calculated size
        double duration_with_margin = duration_sec * 1.1; // Add 10% margin
        audio_total_samples = static_cast<size_t>(duration_with_margin * current_sample_rate * current_channels + 0.5); // +0.5 for rounding
        audio_total_bytes = audio_total_samples * sizeof(int16_t);
        sample_rate.store(current_sample_rate);

        std::cout << "Estimated duration: " << duration_sec << "s (with 10% margin: " << duration_with_margin << "s), Sample Rate: " << current_sample_rate << " Hz, Channels: " << current_channels << std::endl;
        std::cout << "Calculated total size: " << audio_total_samples << " samples, " << audio_total_bytes / (1024.0*1024.0) << " MB." << std::endl;

        char temp_filename_template[] = "/tmp/tapexplayer_audio_XXXXXX";
        audio_write_fd = mkstemp(temp_filename_template);
        if (audio_write_fd == -1) {
             throw std::runtime_error("Failed to create temporary file: " + std::string(strerror(errno)));
        }
        audio_temp_filename = temp_filename_template;
        std::cout << "Created temporary file: " << audio_temp_filename << " (fd: " << audio_write_fd << ")" << std::endl;

        // Ensure file is closed if we exit prematurely (needed for unlink)
        struct FdCloser { int fd; ~FdCloser() { if(fd != -1) close(fd); } } writeFdCloser{audio_write_fd};

        if (ftruncate(audio_write_fd, audio_total_bytes) == -1) {
            unlink(audio_temp_filename.c_str()); // Cleanup before throwing
            audio_temp_filename.clear();
            throw std::runtime_error("Failed to truncate temporary file: " + std::string(strerror(errno)));
        }
        std::cout << "Truncated temporary file successfully." << std::endl;
        // --- End File Creation/Truncation ---

        // --- Map File for Writing --- 
        audio_write_ptr = static_cast<int16_t*>(mmap(
            nullptr, audio_total_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, audio_write_fd, 0
        ));
        if (audio_write_ptr == MAP_FAILED) {
             audio_write_ptr = nullptr; // Ensure it's null on failure
             unlink(audio_temp_filename.c_str());
             audio_temp_filename.clear();
             throw std::runtime_error("Failed to map temporary file for writing: " + std::string(strerror(errno)));
        }
        std::cout << "Memory mapping for writing successful." << std::endl;
        // --- End Map File --- 

        std::cout << "Starting frame reading and writing to mmap..." << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Keep this pause?

        int frame_count = 0;
        int packet_count = 0;
        int ret;
        size_t current_write_offset = 0; // Offset in int16_t samples

        while ((ret = av_read_frame(format_ctx, packet)) >= 0 && !quit.load()) {
            if (packet->stream_index == audio_stream_index) {
                packet_count++;
                ret = avcodec_send_packet(audio_codec_ctx, packet);
                if (ret < 0) { std::cerr << "Error sending packet: " << av_err2str(ret) << std::endl; continue; }

                while (ret >= 0) {
                    ret = avcodec_receive_frame(audio_codec_ctx, frame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) { break; }
                    else if (ret < 0) { std::cerr << "Error receiving frame: " << av_err2str(ret) << std::endl; break; }

                    // --- Convert samples to int16_t and WRITE TO MMAP --- 
                    const int16_t int16_max = std::numeric_limits<int16_t>::max();
                    const int16_t int16_min = std::numeric_limits<int16_t>::min();
                    size_t samples_in_frame = 0;

                    // Check if write would exceed total allocated size
                    if (current_write_offset + frame->nb_samples * current_channels > audio_total_samples) {
                        std::cerr << "Warning: Decoded samples exceed estimated file size. Current offset: " << current_write_offset 
                                 << ", Frame samples: " << frame->nb_samples * current_channels
                                 << ", Total allocated: " << audio_total_samples 
                                 << ". Stopping decode prematurely." << std::endl;
                        ret = AVERROR_EOF; // Force loop exit
                        break;
                    }

                    int16_t* current_write_pos = audio_write_ptr + current_write_offset;

                    if (audio_codec_ctx->sample_fmt == AV_SAMPLE_FMT_FLTP) {
                        int samples_per_channel = frame->nb_samples;
                        int num_channels = frame->ch_layout.nb_channels;
                        float** float_data = reinterpret_cast<float**>(frame->data);
                        samples_in_frame = samples_per_channel * num_channels;

                        for (int i = 0; i < samples_per_channel; ++i) {
                            for (int ch = 0; ch < num_channels; ++ch) {
                                float sample_float = float_data[ch][i];
                                float scaled_sample = sample_float * 32767.0f;
                                int16_t sample_int16 = static_cast<int16_t>(std::round(std::max(static_cast<float>(int16_min), std::min(static_cast<float>(int16_max), scaled_sample))));
                                *current_write_pos++ = sample_int16;
                            }
                        }
                    } else if (audio_codec_ctx->sample_fmt == AV_SAMPLE_FMT_S16P) {
                        int samples_per_channel = frame->nb_samples;
                        int num_channels = frame->ch_layout.nb_channels;
                        int16_t** s16p_data = reinterpret_cast<int16_t**>(frame->data);
                        samples_in_frame = samples_per_channel * num_channels;

                        for (int i = 0; i < samples_per_channel; ++i) {
                            for (int ch = 0; ch < num_channels; ++ch) {
                                *current_write_pos++ = s16p_data[ch][i];
                            }
                        }
                    } else if (audio_codec_ctx->sample_fmt == AV_SAMPLE_FMT_S16) {
                        int16_t* samples = reinterpret_cast<int16_t*>(frame->data[0]);
                        samples_in_frame = frame->nb_samples * frame->ch_layout.nb_channels;
                        memcpy(current_write_pos, samples, samples_in_frame * sizeof(int16_t));
                    } else if (audio_codec_ctx->sample_fmt == AV_SAMPLE_FMT_S32P) {
                        // --- Restore S32P handling with float scaling --- 
                        int samples_per_channel = frame->nb_samples;
                        int num_channels = frame->ch_layout.nb_channels;
                        int32_t** s32p_data = reinterpret_cast<int32_t**>(frame->data);
                        samples_in_frame = samples_per_channel * num_channels;

                        for (int i = 0; i < samples_per_channel; ++i) {
                            for (int ch = 0; ch < num_channels; ++ch) {
                                int32_t sample_s32 = s32p_data[ch][i];
                                // Scale S32 to S16 range using float division
                                float scaled_sample = static_cast<float>(sample_s32) / 65536.0f;
                                int16_t sample_s16 = static_cast<int16_t>(std::round(std::max(-32768.0f, std::min(32767.0f, scaled_sample))));
                                *current_write_pos++ = sample_s16;
                            }
                        }
                    } else if (audio_codec_ctx->sample_fmt == AV_SAMPLE_FMT_S32) {
                        // --- Restore S32 handling with float scaling ---
                        int samples_per_channel = frame->nb_samples;
                        int num_channels = frame->ch_layout.nb_channels;
                        int32_t* s32_samples = reinterpret_cast<int32_t*>(frame->data[0]);
                        samples_in_frame = samples_per_channel * num_channels;

                        for (size_t i = 0; i < samples_in_frame; ++i) {
                            int32_t sample_s32 = s32_samples[i];
                            // Scale S32 to S16 range using float division
                            float scaled_sample = static_cast<float>(sample_s32) / 65536.0f;
                            int16_t sample_s16 = static_cast<int16_t>(std::round(std::max(-32768.0f, std::min(32767.0f, scaled_sample))));
                            *current_write_pos++ = sample_s16;
                        }
                    } else {
                        std::cerr << "Unsupported audio format for mmap: " << av_get_sample_fmt_name(audio_codec_ctx->sample_fmt) << std::endl;
                        // Skip frame, don't advance offset or count
                        samples_in_frame = 0; 
                    }
                    
                    if (samples_in_frame > 0) {
                        current_write_offset += samples_in_frame;
                        // Atomically update the count of available samples
                        audio_decoded_samples_count.store(current_write_offset, std::memory_order_release);
                    }
                    // --- End sample conversion and write ---

                    frame_count++;
                } // End while receive_frame
            } // End if audio stream
            av_packet_unref(packet);
            if(ret == AVERROR_EOF) break; // Exit outer loop if needed
        } // End while read_frame

         // --- Flush decoder --- 
         // Send NULL packet to flush remaining frames
         avcodec_send_packet(audio_codec_ctx, NULL);
         while (true) {
             ret = avcodec_receive_frame(audio_codec_ctx, frame);
             if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                 break;
             }
              if (ret < 0) {
                  std::cerr << "Error flushing decoder: " << av_err2str(ret) << std::endl;
                  break;
              }
              // Process flushed frames (same logic as above)
              // --- Convert samples to int16_t and WRITE TO MMAP --- 
              const int16_t int16_max = std::numeric_limits<int16_t>::max();
              const int16_t int16_min = std::numeric_limits<int16_t>::min();
              size_t samples_in_frame = 0;
              if (current_write_offset + frame->nb_samples * current_channels > audio_total_samples) {
                  std::cerr << "Warning: Flushed samples exceed estimated file size." << std::endl;
                  break; 
              }
              int16_t* current_write_pos = audio_write_ptr + current_write_offset;
              if (audio_codec_ctx->sample_fmt == AV_SAMPLE_FMT_FLTP) {
                  int samples_per_channel = frame->nb_samples; int num_channels = frame->ch_layout.nb_channels; float** float_data = reinterpret_cast<float**>(frame->data); 
                  for (int i = 0; i < samples_per_channel; ++i) for (int ch = 0; ch < num_channels; ++ch) { float s_f = float_data[ch][i]; float sc_s = s_f * 32767.0f; int16_t s_i = static_cast<int16_t>(std::round(std::max(static_cast<float>(int16_min), std::min(static_cast<float>(int16_max), sc_s)))); *current_write_pos++ = s_i; } 
              } else if (audio_codec_ctx->sample_fmt == AV_SAMPLE_FMT_S16P) {
                  int samples_per_channel = frame->nb_samples; int num_channels = frame->ch_layout.nb_channels; int16_t** s16p_data = reinterpret_cast<int16_t**>(frame->data); 
                  for (int i = 0; i < samples_per_channel; ++i) for (int ch = 0; ch < num_channels; ++ch) *current_write_pos++ = s16p_data[ch][i]; 
              } else if (audio_codec_ctx->sample_fmt == AV_SAMPLE_FMT_S16) {
                  int16_t* samples = reinterpret_cast<int16_t*>(frame->data[0]); memcpy(current_write_pos, samples, samples_in_frame * sizeof(int16_t)); 
              } else if (audio_codec_ctx->sample_fmt == AV_SAMPLE_FMT_S32P) {
                  // --- Restore S32P handling with float scaling in flush loop ---
                   int samples_per_channel = frame->nb_samples; int num_channels = frame->ch_layout.nb_channels; int32_t** s32p_data = reinterpret_cast<int32_t**>(frame->data);
                   samples_in_frame = samples_per_channel * num_channels;
                   for (int i = 0; i < samples_per_channel; ++i) {
                        for (int ch = 0; ch < num_channels; ++ch) {
                            int32_t sample_s32 = s32p_data[ch][i];
                            float scaled_sample = static_cast<float>(sample_s32) / 65536.0f;
                            int16_t sample_s16 = static_cast<int16_t>(std::round(std::max(-32768.0f, std::min(32767.0f, scaled_sample))));
                            *current_write_pos++ = sample_s16;
                        }
                    }
              } else if (audio_codec_ctx->sample_fmt == AV_SAMPLE_FMT_S32) {
                  // --- Restore S32 handling with float scaling in flush loop ---
                   int samples_per_channel = frame->nb_samples;
                   int num_channels = frame->ch_layout.nb_channels;
                   int32_t* s32_samples = reinterpret_cast<int32_t*>(frame->data[0]);
                   samples_in_frame = samples_per_channel * num_channels;
 
                   for (size_t i = 0; i < samples_in_frame; ++i) {
                       int32_t sample_s32 = s32_samples[i];
                       float scaled_sample = static_cast<float>(sample_s32) / 65536.0f;
                       int16_t sample_s16 = static_cast<int16_t>(std::round(std::max(-32768.0f, std::min(32767.0f, scaled_sample))));
                       *current_write_pos++ = sample_s16;
                   }
              } else { samples_in_frame = 0; } 
              
              if (samples_in_frame > 0) {
                  current_write_offset += samples_in_frame;
                  audio_decoded_samples_count.store(current_write_offset, std::memory_order_release);
              }
              // --- End sample conversion and write ---
         } // End while flushing
         // --- End Flush --- 

        if (ret < 0 && ret != AVERROR_EOF) {
            std::cerr << "Error reading frame: " << av_err2str(ret) << std::endl;
        }

        std::cout << "Audio decoding finished. Total samples written: " << audio_decoded_samples_count.load() << std::endl;
        decoding_finished.store(true);

        // --- Finalize mmap write --- 
        if (audio_write_ptr) {
             if (msync(audio_write_ptr, audio_total_bytes, MS_SYNC) == -1) {
                 std::cerr << "Warning: msync failed: " << strerror(errno) << std::endl;
             }
             if (munmap(audio_write_ptr, audio_total_bytes) == -1) {
                 std::cerr << "Error unmapping write region: " << strerror(errno) << std::endl;
             }
             audio_write_ptr = nullptr; // Mark as unmapped
        }
        writeFdCloser.fd = -1; // Prevent RAII closer from closing again
        if (audio_write_fd != -1) {
             if (close(audio_write_fd) == -1) {
                 std::cerr << "Error closing write fd: " << strerror(errno) << std::endl;
             }
             audio_write_fd = -1;
        }
        // --- End Finalize --- 

        decoding_completed.store(true);  
    }
    catch (const std::exception& e) {
        std::cerr << "Decoding error: " << e.what() << std::endl;
        decoding_finished.store(true); // Indicate finished even on error
        decoding_completed.store(true); 

        // --- Cleanup on error --- 
        std::lock_guard<std::mutex> lock(mmap_init_mutex); // Protect cleanup
        if (audio_write_ptr) { munmap(audio_write_ptr, audio_total_bytes); audio_write_ptr = nullptr; }
        if (audio_write_fd != -1) { close(audio_write_fd); audio_write_fd = -1; }
        if (!audio_temp_filename.empty()) { unlink(audio_temp_filename.c_str()); audio_temp_filename.clear(); }
        audio_total_bytes = 0;
        audio_total_samples = 0;
        audio_decoded_samples_count.store(0);
         // --- End Cleanup on error --- 
    }

    // --- Final FFmpeg cleanup --- 
    if (frame) av_frame_free(&frame);
    if (audio_codec_ctx) avcodec_free_context(&audio_codec_ctx);
    if (format_ctx) avformat_close_input(&format_ctx);
    av_packet_free(&packet);
    std::cout << "decode_audio finished execution." << std::endl;
}

void start_audio(const char* filename) {
    // --- No need to clear vector --- 
    // audio_buffer.clear();
    // audio_buffer_index = 0;
    // --- Reset state handled in decode_audio --- 
    // decoding_finished.store(false);
    // decoding_completed.store(false);

    try {
        std::cout << "Starting audio initialization..." << std::endl;

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

        // --- Start decoding audio in a separate thread --- 
        // Decoder thread now CREATES the mmap file
        std::thread decoding_thread([filename]() {
            decode_audio(filename);
        });
        decoding_thread.detach(); 

        // --- Wait briefly for decoder to create and setup mmap file --- 
        // This is a potential race condition point. A more robust solution might 
        // involve a condition variable signaled by decode_audio after mmap setup.
        std::cout << "Waiting for decoder to setup mmap file..." << std::endl;
        for(int i=0; i < 100; ++i) { // Wait up to ~2 seconds
             std::this_thread::sleep_for(std::chrono::milliseconds(20));
             std::lock_guard<std::mutex> lock(mmap_init_mutex);
             if (!audio_temp_filename.empty() && audio_total_bytes > 0) break; 
        }
        
        std::string temp_file_to_open;
        size_t expected_bytes;
        {
             std::lock_guard<std::mutex> lock(mmap_init_mutex);
             if (audio_temp_filename.empty() || audio_total_bytes == 0) {
                  throw std::runtime_error("Decoder did not create/setup mmap file in time.");
             }
             temp_file_to_open = audio_temp_filename;
             expected_bytes = audio_total_bytes;
             std::cout << "Decoder setup complete. Opening mmap file: " << temp_file_to_open << " for reading (" << expected_bytes << " bytes)." << std::endl;
        }
        // --- End Wait --- 

        // --- Open and Map the Temporary File for Reading --- 
        audio_read_fd = open(temp_file_to_open.c_str(), O_RDONLY);
        if (audio_read_fd == -1) {
            throw std::runtime_error("Failed to open temporary audio file for reading: " + std::string(strerror(errno)));
        }
        std::cout << "Opened temp file for reading (fd: " << audio_read_fd << ")" << std::endl;

        // Map the *entire* pre-allocated file
        audio_read_ptr = static_cast<const int16_t*>(mmap(
            nullptr, expected_bytes, PROT_READ, MAP_SHARED, audio_read_fd, 0
        ));

        if (audio_read_ptr == MAP_FAILED) {
            audio_read_ptr = nullptr; // Ensure null on failure
            close(audio_read_fd);
            audio_read_fd = -1;
            throw std::runtime_error("Failed to map temporary file for reading: " + std::string(strerror(errno)));
        }
        std::cout << "Memory mapping for reading successful." << std::endl;
        // --- End Open/Map --- 

        // Give the decoding thread a moment to start writing data
        // std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Maybe not needed?

        // Close any existing stream
        if (stream) {
            Pa_StopStream(stream);
            Pa_CloseStream(stream);
            stream = nullptr;
        }

        // Get the sample rate determined during decoding
        int pa_sample_rate = sample_rate.load(); 
        if(pa_sample_rate <= 0) {
             std::cerr << "Warning: Invalid sample rate detected (" << pa_sample_rate << "). Defaulting to 44100 Hz for PortAudio." << std::endl;
             pa_sample_rate = 44100;
        }

        err = Pa_OpenStream(
            &stream,
            NULL, 
            &outputParameters,
            pa_sample_rate, 
            256, // framesPerBuffer - keep relatively small for low latency
            paClipOff,
            patestCallback,
            NULL); 

        if (err != paNoError) {
            // Cleanup mmap before throwing
            if (audio_read_ptr) { munmap((void*)audio_read_ptr, expected_bytes); audio_read_ptr = nullptr; }
            if (audio_read_fd != -1) { close(audio_read_fd); audio_read_fd = -1; }
            throw std::runtime_error("PortAudio error opening stream: " + std::string(Pa_GetErrorText(err)));
        }

        err = Pa_StartStream(stream);
        if (err != paNoError) {
             // Cleanup mmap before throwing
            Pa_CloseStream(stream); stream = nullptr;
            if (audio_read_ptr) { munmap((void*)audio_read_ptr, expected_bytes); audio_read_ptr = nullptr; }
            if (audio_read_fd != -1) { close(audio_read_fd); audio_read_fd = -1; }
            throw std::runtime_error("PortAudio error starting stream: " + std::string(Pa_GetErrorText(err)));
        }

        std::cout << "Audio device opened successfully, PortAudio stream started." << std::endl;

        audio_buffer_index = 0.0; // Reset playback position
        current_audio_time.store(0.0);
        playback_rate.store(0.0);
        target_playback_rate.store(0.0);

        std::cout << "Audio playback ready. Decoding runs in background." << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Audio Error in start_audio: " << e.what() << std::endl;
        // Ensure partial cleanup if error occurred during setup
        std::lock_guard<std::mutex> lock(mmap_init_mutex);
        if (stream) { Pa_CloseStream(stream); stream = nullptr; }
        if (audio_read_ptr) { munmap((void*)audio_read_ptr, audio_total_bytes); audio_read_ptr = nullptr; }
        if (audio_read_fd != -1) { close(audio_read_fd); audio_read_fd = -1; }
        // Don't unlink here, let decode_audio or cleanup_audio handle it
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

    // Safety check for fps - use 25 fps as a safe default if fps is 0 or negative
    if (fps <= 0.0) {
        fps = 25.0;
    }

    // Hard limit time to file duration
    time = std::min(std::max(time, 0.0), total_dur);

    // Direct calculation from time to components
    int hours = static_cast<int>(time / 3600.0);
    time -= hours * 3600.0;
    
    int minutes = static_cast<int>(time / 60.0);
    time -= minutes * 60.0;
    
    int seconds = static_cast<int>(time);
    double fractional_seconds = time - seconds;
    
    // Calculate frames from fractional seconds
    int frames = static_cast<int>(fractional_seconds * fps);
    
    // Ensure frames are within valid range [0, fps-1]
    // This handles rounding errors
    if (frames >= static_cast<int>(fps)) {
        frames = static_cast<int>(fps) - 1;
    }

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
    // Ensure mmap is ready before seeking
    if (audio_read_ptr == nullptr || audio_total_samples == 0) {
        std::cerr << "Warning: Attempted to seek before audio mmap is ready." << std::endl;
        return;
    }

    double total_dur = total_duration.load(); // Get total duration
    if (total_dur <= 0) { // Use calculated duration if global not set
        int current_sample_rate = sample_rate.load();
        if (current_sample_rate > 0) {
            total_dur = static_cast<double>(audio_total_samples) / (current_sample_rate * 2.0); // Assuming stereo
        }
    }

    // Clamp target time to valid range [0, total_duration]
    target_time = std::max(0.0, target_time);
    if (total_dur > 0) {
        target_time = std::min(target_time, total_dur);
    }

    // Calculate target sample pair index
    int current_sample_rate = sample_rate.load();
    if (current_sample_rate <= 0) {
         std::cerr << "Warning: Cannot calculate seek index due to invalid sample rate." << std::endl;
         return;
    }
    // Calculate index based on sample pairs (stereo)
    double target_index_double = target_time * current_sample_rate; 
    
    // Clamp index to valid range [0, total_pairs - 1]
    size_t buffer_num_sample_pairs = audio_total_samples / 2; // Assuming stereo
    if (buffer_num_sample_pairs > 0) {
         target_index_double = std::min(target_index_double, static_cast<double>(buffer_num_sample_pairs - 1));
    }
    target_index_double = std::max(0.0, target_index_double);
    
    // Update the playback index (used by patestCallback)
    audio_buffer_index = target_index_double; 

    // Update the current time displayed/reported
    current_audio_time.store(target_time);
    
    seek_performed.store(true); // Keep this flag if used elsewhere
    std::cout << "Seeked to time: " << target_time << "s (index: " << audio_buffer_index << ")" << std::endl;
}

double parse_timecode(const std::string& timecode) {
    // Pad with leading zeros if less than 8 characters (standard timecode behavior)
    std::string padded_timecode = timecode;
    if (padded_timecode.length() < 8) {
        padded_timecode = std::string(8 - padded_timecode.length(), '0') + padded_timecode;
    }
    // Truncate if more than 8 characters
    if (padded_timecode.length() > 8) {
        padded_timecode = padded_timecode.substr(0, 8);
    }

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

    double result = hours * 3600.0 + minutes * 60.0 + seconds + frames / fps;
    
    std::cout << "[parse_timecode] Input: \"" << timecode << "\" -> Padded: \"" << padded_timecode 
              << "\" -> " << hours << ":" << minutes << ":" << seconds << ":" << frames 
              << " (FPS: " << fps << ") -> " << std::fixed << std::setprecision(6) << result << "s" << std::endl;

    return result;
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
    
    // --- Remember playback state AND mmap info --- 
    double current_time = current_audio_time.load();
    double current_rate = playback_rate.load();
    double current_target_rate = target_playback_rate.load();
    bool current_reverse = is_reverse.load();
    double current_index = audio_buffer_index; // Remember fractional index
    std::string current_temp_file = audio_temp_filename; // Copy needed info before closing
    size_t current_total_bytes = audio_total_bytes;
    
    // --- Stop stream and release current mmap read resources --- 
    if (stream) {
        Pa_StopStream(stream);
        Pa_CloseStream(stream);
        stream = nullptr;
        std::cout << "Closed existing PortAudio stream." << std::endl;
    }
    if (audio_read_ptr) {
         if (munmap((void*)audio_read_ptr, current_total_bytes) == -1) {
              std::cerr << "Warning: Failed to unmap read region during device switch: " << strerror(errno) << std::endl;
         }
         audio_read_ptr = nullptr;
         std::cout << "Unmapped existing audio read region." << std::endl;
    }
    if (audio_read_fd != -1) {
         if (close(audio_read_fd) == -1) {
              std::cerr << "Warning: Failed to close read fd during device switch: " << strerror(errno) << std::endl;
         }
         audio_read_fd = -1;
         std::cout << "Closed existing audio read fd." << std::endl;
    }
    // --- End Stop/Release --- 

    // Check if temp file still exists (might have been cleaned up if decode thread finished/failed)
    if (current_temp_file.empty()) {
         std::cerr << "Error switching device: Temporary audio file info lost." << std::endl;
         return false;
    }
    // Re-open the *same* temp file for reading
    audio_read_fd = open(current_temp_file.c_str(), O_RDONLY);
    if (audio_read_fd == -1) {
         std::cerr << "Error switching device: Failed to re-open temp file: " << strerror(errno) << std::endl;
         return false; // Cannot continue without the file
    }
    std::cout << "Re-opened temp file for reading (fd: " << audio_read_fd << ")" << std::endl;

    // Re-map the file for reading
    audio_read_ptr = static_cast<const int16_t*>(mmap(
            nullptr, current_total_bytes, PROT_READ, MAP_SHARED, audio_read_fd, 0
    ));

    if (audio_read_ptr == MAP_FAILED) {
         std::cerr << "Error switching device: Failed to re-map file: " << strerror(errno) << std::endl;
         close(audio_read_fd);
         audio_read_fd = -1;
         audio_read_ptr = nullptr;
         return false;
    }
     std::cout << "Re-mapped file for reading successfully." << std::endl;
    
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
    audio_buffer_index = current_index; // Restore fractional index
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
        
        // --- Cleanup mmap resources --- 
        std::lock_guard<std::mutex> lock(mmap_init_mutex); // Protect concurrent access

        // Unmap read region if mapped
        if (audio_read_ptr) {
            if (munmap((void*)audio_read_ptr, audio_total_bytes) == -1) {
                 std::cerr << "Error unmapping read region: " << strerror(errno) << std::endl;
            }
            audio_read_ptr = nullptr;
        }
        // Unmap write region if mapped (should normally be done by decoder thread, but cleanup just in case)
        if (audio_write_ptr) {
            // Note: might already be unmapped by decoder thread
            munmap(audio_write_ptr, audio_total_bytes); // Ignore error here, might already be gone
            audio_write_ptr = nullptr;
        }

        // Close file descriptors
        if (audio_read_fd != -1) {
            if (close(audio_read_fd) == -1) {
                 std::cerr << "Error closing read fd: " << strerror(errno) << std::endl;
            }
            audio_read_fd = -1;
        }
        if (audio_write_fd != -1) {
            if (close(audio_write_fd) == -1) {
                // Might already be closed by decoder thread
            }
            audio_write_fd = -1;
        }

        // Delete the temporary file
        if (!audio_temp_filename.empty()) {
            if (unlink(audio_temp_filename.c_str()) == -1) {
                 std::cerr << "Error deleting temporary audio file: " << strerror(errno) << std::endl;
            }
             std::cout << "Deleted temporary audio file: " << audio_temp_filename << std::endl;
            audio_temp_filename.clear();
        }

        // Reset size/count variables
        audio_total_bytes = 0;
        audio_total_samples = 0;
        audio_decoded_samples_count.store(0);
        audio_buffer_index = 0.0; // Reset playback position
        // --- End mmap cleanup --- 
        
        PaError err = Pa_Terminate();
        if (err != paNoError) {
            std::cerr << "Error terminating PortAudio: " << Pa_GetErrorText(err) << std::endl;
        }
        
        std::cout << "Audio system cleaned up successfully" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Error during audio cleanup: " << e.what() << std::endl;
    }
}
