#pragma once

#ifndef MAIN_H
#define MAIN_H

#include <atomic>
#include <string>
#include <vector>
#include <mutex>
#include <future>
#include <array>
#include <SDL2/SDL.h> // For SDL_Window
#include "../common/common.h" // For SeekInfo and ProgressCallback (if needed globally)

// --- Global Player State ---
extern std::mutex cout_mutex;
extern std::atomic<bool> quit;
extern std::atomic<double> current_audio_time;
extern std::atomic<double> playback_rate;
extern std::atomic<double> target_playback_rate;
extern std::atomic<bool> is_reverse;
extern std::atomic<bool> is_seeking;
extern std::atomic<double> total_duration;
extern std::atomic<double> original_fps;
extern std::atomic<bool> shouldExit; // Controls main loops
extern std::atomic<float> volume;
extern std::atomic<bool> toggle_fullscreen_requested;

// --- UI/Interaction State ---
extern std::array<double, 5> memoryMarkers;
extern bool showIndex;
extern bool showOSD;
extern std::string input_timecode;
extern bool waiting_for_timecode;
extern std::atomic<bool> seek_performed;
extern SeekInfo seekInfo; // Defined in common.h

// --- Decoding/Audio State ---
extern std::atomic<bool> is_force_decoding;
extern std::future<void> force_decode_future;
extern std::mutex frameIndexMutex;
extern std::atomic<bool> audio_initialized; // Potentially belongs elsewhere?
extern std::thread audio_thread;
extern std::thread speed_change_thread;
extern std::atomic<bool> speed_reset_requested;
extern double audio_buffer_index; // From mainau.cpp
extern std::atomic<bool> decoding_finished; // From mainau.cpp
extern std::atomic<bool> decoding_completed; // From mainau.cpp

// --- Zoom State ---
extern std::atomic<bool> zoom_enabled;
extern std::atomic<float> zoom_factor;
extern std::atomic<float> zoom_center_x;
extern std::atomic<float> zoom_center_y;
extern std::atomic<bool> show_zoom_thumbnail;

// --- Restart/Reload State ---
extern bool restart_requested;
extern std::string restart_filename;
extern std::atomic<bool> reload_file_requested;

// --- Function Declarations (defined in main.cpp or linked) ---
extern void start_audio(const char* filename); // Defined in mainau.cpp? Needs confirmation.
extern std::string generateTXTimecode(double time);
extern void smooth_speed_change();
extern void cleanup_audio(); // Defined in mainau.cpp? Needs confirmation.
extern void log(const std::string& message); // Defined in main.cpp
extern void takeCurrentFrameScreenshot(); // Function to take screenshot of current frame

// Forward declare from other modules if needed globally
class RemoteControl; // Forward declaration
extern RemoteControl* g_remote_control; // Global instance pointer

// Potentially needed forward declarations from initmanager.h if used globally
struct WindowSettings;
extern WindowSettings loadWindowSettings();
extern void saveWindowSettings(SDL_Window* window);
// ... etc. - decide which functions need global visibility

#endif // MAIN_H
