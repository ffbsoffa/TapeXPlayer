#include "globals.h"
#include "keyboard_manager.h"
#include "core/decode/decode.h"
#include "core/remote/remote_control.h"

// Pending FSTP URL processing globals
std::string g_pendingFstpUrlPath;
double g_pendingFstpUrlTime = -1.0;
std::atomic<bool> g_hasPendingFstpUrl(false);

// Global RemoteControl instance
RemoteControl* g_remote_control = nullptr;

// Core application state
std::mutex cout_mutex;
std::atomic<bool> quit(false);
std::atomic<double> current_audio_time(0.0);
std::atomic<double> playback_rate(1.0);
std::atomic<double> target_playback_rate(1.0);
std::atomic<bool> is_reverse(false);
std::atomic<bool> is_seeking(false);
std::atomic<double> total_duration(0.0);
std::atomic<double> original_fps(0.0);
std::atomic<bool> shouldExit(false);
std::atomic<float> volume(1.0f);
std::atomic<bool> toggle_fullscreen_requested(false);

// Memory markers (-1 means marker is not set)
std::array<double, 5> memoryMarkers = {-1, -1, -1, -1, -1};

// UI state
bool showIndex = false;
bool showOSD = true;
std::atomic<double> previous_playback_rate(1.0);
std::string input_timecode;
bool waiting_for_timecode = false;

// Seek and decoding state
std::atomic<bool> seek_performed(false);
std::atomic<bool> is_force_decoding(false);
std::future<void> force_decode_future;
std::mutex frameIndexMutex;
SeekInfo seekInfo;

// Audio state
std::atomic<bool> audio_initialized(false);
std::thread audio_thread;
std::thread speed_change_thread;
std::atomic<bool> speed_reset_requested(false);

// Display state
float currentDisplayAspectRatio = 16.0f / 9.0f;
std::atomic<bool> window_has_focus(true); // Track window focus state

// Zoom control
std::atomic<bool> zoom_enabled(false);
std::atomic<float> zoom_factor(1.0f);
std::atomic<float> zoom_center_x(0.5f);
std::atomic<float> zoom_center_y(0.5f);
std::atomic<bool> show_zoom_thumbnail(true);

// Betacam effect control
std::atomic<bool> betacam_effect_enabled(false);

// MCP sound feedback control
std::atomic<bool> mcp_command_beep_requested(false);
std::atomic<bool> screenshot_click_requested(false);

// Zoom constants
const float MAX_ZOOM_FACTOR = 10.0f;
const float MIN_ZOOM_FACTOR = 1.0f;
const float ZOOM_STEP = 1.2f;

// Screenshot functionality
std::atomic<bool> screenshot_requested(false);
std::string screenshots_directory = "Screenshots";

// External variables from mainau.cpp
extern double audio_buffer_index; // Keep this, it's now the fractional mmap index
extern std::atomic<bool> decoding_finished;
extern std::atomic<bool> decoding_completed;

// Constants for frame rate limiting
const int TARGET_FPS = 60;
const int FRAME_DELAY = 1000 / TARGET_FPS;
const int DEEP_PAUSE_RENDER_FPS = 1; // FPS for deep pause - reduced to 1 FPS for maximum CPU savings
const int DEEP_PAUSE_FRAME_DELAY = 1000 / DEEP_PAUSE_RENDER_FPS; // 1000ms delay

// Application restart variables
std::string argv0;
std::vector<std::string> restartArgs;
bool restart_requested = false;
std::string restart_filename;
std::atomic<bool> reload_file_requested(false); // Use atomic consistent with extern declaration

// File path variables
std::string g_currentOpenFilePath = "";
double g_seek_after_next_load_time = -1.0; // For URL-triggered loads with seek time

// Global manager pointers
KeyboardManager* g_keyboardManager = nullptr;
WindowManager* g_windowManager = nullptr;
DeepPauseManager* g_deepPauseManager = nullptr;

// SDL window pointer
SDL_Window* window = nullptr;

// Speed control variables
std::vector<double> speed_steps = {1.0, 3.0, 10.0, 24.0};
int current_speed_index = 0; // Index for 1.0x speed

// Mouse shuttle control
std::atomic<bool> mouse_shuttle_active(false);
int mouse_shuttle_start_x = 0;