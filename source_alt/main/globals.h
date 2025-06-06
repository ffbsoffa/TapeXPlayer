#ifndef GLOBALS_H
#define GLOBALS_H

#include <string>
#include <atomic>
#include <mutex>
#include <thread>
#include <future>
#include <array>
#include <vector>
#include <SDL2/SDL.h>

// Forward declarations
class RemoteControl;
struct SeekInfo;

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

// Function declarations
extern double parse_timecode(const std::string& timecode);
void handleZoomMouseEvent(SDL_Event& event, int windowWidth, int windowHeight, int frameWidth, int frameHeight);
void start_audio(const char* filename);
std::string generateTXTimecode(double time);
void smooth_speed_change();
void check_and_reset_threshold();
extern void cleanup_audio();

// Pending FSTP URL processing globals
extern std::string g_pendingFstpUrlPath;
extern double g_pendingFstpUrlTime;
extern std::atomic<bool> g_hasPendingFstpUrl;

// Global RemoteControl instance
extern RemoteControl* g_remote_control;

// Core application state
extern std::mutex cout_mutex;
extern std::atomic<bool> quit;
extern std::atomic<double> current_audio_time;
extern std::atomic<double> playback_rate;
extern std::atomic<double> target_playback_rate;
extern std::atomic<bool> is_reverse;
extern std::atomic<bool> is_seeking;
extern std::atomic<double> total_duration;
extern std::atomic<double> original_fps;
extern std::atomic<bool> shouldExit;
extern std::atomic<float> volume;
extern std::atomic<bool> toggle_fullscreen_requested;

// Memory markers
extern std::array<double, 5> memoryMarkers;

// UI state
extern bool showIndex;
extern bool showOSD;
extern std::atomic<double> previous_playback_rate;
extern std::string input_timecode;
extern bool waiting_for_timecode;

// Seek and decoding state
extern std::atomic<bool> seek_performed;
extern std::atomic<bool> is_force_decoding;
extern std::future<void> force_decode_future;
extern std::mutex frameIndexMutex;
extern SeekInfo seekInfo;

// Audio state
extern std::atomic<bool> audio_initialized;
extern std::thread audio_thread;
extern std::thread speed_change_thread;
extern std::atomic<bool> speed_reset_requested;

// Display state
extern float currentDisplayAspectRatio;
extern std::atomic<bool> window_has_focus;

// Zoom control
extern std::atomic<bool> zoom_enabled;
extern std::atomic<float> zoom_factor;
extern std::atomic<float> zoom_center_x;
extern std::atomic<float> zoom_center_y;
extern std::atomic<bool> show_zoom_thumbnail;

// Betacam effect control
extern std::atomic<bool> betacam_effect_enabled;

// MCP sound feedback control
extern std::atomic<bool> mcp_command_beep_requested;
extern std::atomic<bool> screenshot_click_requested;

// Zoom constants
extern const float MAX_ZOOM_FACTOR;
extern const float MIN_ZOOM_FACTOR;
extern const float ZOOM_STEP;

// Screenshot functionality
extern std::atomic<bool> screenshot_requested;
extern std::string screenshots_directory;

// External variables from mainau.cpp
extern double audio_buffer_index;
extern std::atomic<bool> decoding_finished;
extern std::atomic<bool> decoding_completed;

// Constants for frame rate limiting
extern const int TARGET_FPS;
extern const int FRAME_DELAY;
extern const int DEEP_PAUSE_RENDER_FPS;
extern const int DEEP_PAUSE_FRAME_DELAY;

// Application restart variables
extern std::string argv0;
extern std::vector<std::string> restartArgs;
extern bool restart_requested;
extern std::string restart_filename;
extern std::atomic<bool> reload_file_requested;

// File path variables
extern std::string g_currentOpenFilePath;
extern double g_seek_after_next_load_time;

// Forward declarations
class KeyboardManager;
class WindowManager;
class DeepPauseManager;

extern KeyboardManager* g_keyboardManager;
extern WindowManager* g_windowManager;
extern DeepPauseManager* g_deepPauseManager;
extern SDL_Window* window;
extern std::vector<double> speed_steps;
extern int current_speed_index;

// Mouse shuttle control
extern std::atomic<bool> mouse_shuttle_active;
extern int mouse_shuttle_start_x;

#endif // GLOBALS_H