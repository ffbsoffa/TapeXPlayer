#ifdef __linux__

#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <gtk/gtk.h>
#include <portaudio.h>
#include <cmath>
#include <atomic>
#include <thread>
#include <chrono>
#include <iostream>
#include <condition_variable>
#include <mutex>
#include <string>
#include <cstring>
#include <unistd.h>  // For _exit()
#include "../../main.h"
#include "../FSTPOSDSystem.h"
#include "../FSTPSettings.h"
#include "../../FSTPPlayerModule/FSTPPlayerManager.h"
#include "../FSTPWindowManager.h"
#include "../FSTPPixelBufferManager.h"
#include "../FSTPKeyboard.h"
#include "../FSTPMemoryLocations.h"
#include "../FSTPWelcomeScreen.h"
#include "FSTPWaylandWS.h"
#include "FSTPSettingsDialog.h"
#include "FSTPMemoryLocationsWindow.h"
#include "FSTPAboutDialog.h"
#include "../FSTPScreenshot.h"
#include "../FSTPLog.h"
#include "../FSTPZoom.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif

// Debug control: set to true to enable verbose logging
static constexpr bool ENABLE_LINUX_WS_DEBUG = false;

// Window system implementation for Linux (Wayland/X11)
// SDL integration for cross-platform compatibility

// Autonomous renderer variables
static bool g_renderingActive = false;
static std::atomic<bool> g_renderThreadRunning{false};
static std::thread g_renderThread;

// Asynchronous shutdown variables (like macOS)
static std::atomic<bool> g_shutdownRequested{false};
static std::atomic<bool> g_shutdownComplete{false};
static std::atomic<bool> g_cleanupThreadStarted{false};

// Autonomous rendering function - works independently of events (like macOS)
void AutoRenderFrame() {
    if (!g_renderingActive) return;

    // FIRST: Render all active windows through WindowManager (SAME AS macOS!)
    RenderAllWindows();

    // SECOND: Update OSD data for all active windows with real player data (SAME AS macOS!)
    for (int i = 0; i < MAX_WINDOWS; i++) {
        FSTPWindow* window = GetWindowByIndex(i);
        if (window && window->is_active) {
            int player_id = window->player_instance_id;

            if (player_id >= 0 && IsPlayerInstanceActive(player_id)) {
                // Get real player data
                bool is_playing = IsInstancePlaying(player_id);
                double current_position = GetInstancePosition(player_id);
                double duration = GetInstanceDuration(player_id);
                double speed = GetInstanceSpeed(player_id);
                double actual_speed = GetInstanceActualSpeed(player_id);
                bool is_reverse = IsInstanceReverse(player_id);

                // Get audio signal levels
                float audio_left = GetInstanceAudioLevelLeft(player_id);
                float audio_right = GetInstanceAudioLevelRight(player_id);
                float peak_left = GetInstanceAudioPeakLeft(player_id);
                float peak_right = GetInstanceAudioPeakRight(player_id);

                // Update OSD for window
                UpdateWindowOSD(i, current_position, duration, is_playing, speed, is_reverse);
                UpdateOSDActualSpeed(player_id, actual_speed);
                UpdateOSDAudioLevels(player_id, audio_left, audio_right, peak_left, peak_right);
            } else {
                // Even without loaded file, update OSD to show NO_FILE state
                UpdateWindowOSD(i, 0.0, 0.0, false, 1.0, false);
            }
        }
    }
}

// Start autonomous rendering (60 FPS like macOS)
void StartAutonomousRendering(SDL_Renderer* renderer) {
    (void)renderer;  // Unused, rendering happens through WindowManager
    g_renderingActive = true;
    g_renderThreadRunning = true;

    g_renderThread = std::thread([]() {
        std::cout << "🎬 [RENDER THREAD] Linux render thread started (60 FPS)" << std::endl;

        const int TARGET_FPS = 60;
        const auto FRAME_DURATION = std::chrono::microseconds(1000000 / TARGET_FPS);

        while (g_renderThreadRunning.load()) {
            auto frame_start = std::chrono::high_resolution_clock::now();

            // Render frame
            AutoRenderFrame();

            // Calculate sleep time to maintain 60 FPS
            auto frame_end = std::chrono::high_resolution_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(frame_end - frame_start);
            auto sleep_time = FRAME_DURATION - elapsed;

            if (sleep_time.count() > 0) {
                std::this_thread::sleep_for(sleep_time);
            }
        }

        std::cout << "🎬 [RENDER THREAD] Linux render thread stopped" << std::endl;
    });
}

// Stop autonomous rendering
void StopAutonomousRendering() {
    g_renderingActive = false;
    g_renderThreadRunning = false;

    if (g_renderThread.joinable()) {
        g_renderThread.join();
    }
}

// GTK initialization flags (shared across dialog files)
bool g_gtk_initialized = false;
bool g_dialog_open = false;

// Initial file to load from command line
static const char* g_initial_file_to_load = nullptr;

// RAII guard to ensure dialog flag is always reset
class DialogGuard {
public:
    DialogGuard() {
        g_dialog_open = true;
    }
    ~DialogGuard() {
        g_dialog_open = false;
        std::cout << "✅ File dialog closed (flag reset)" << std::endl;
    }
};

// Native file dialog for Linux using GTK+
void ShowNativeFileDialog(int target_player_id) {
    std::cout << "🔵 ShowNativeFileDialog called with target_player_id=" << target_player_id << std::endl;

    // Prevent multiple dialog invocations
    if (g_dialog_open) {
        std::cout << "⚠️ Dialog already open, skipping call" << std::endl;
        return;
    }

    // RAII guard - automatically resets flag when function exits
    DialogGuard guard;
    std::cout << "📂 Opening file dialog..." << std::endl;

    // Initialize GTK if not already done
    if (!g_gtk_initialized) {
        std::cout << "🔧 Initializing GTK+..." << std::endl;
        gtk_init(nullptr, nullptr);
        g_gtk_initialized = true;
        std::cout << "✅ GTK+ initialized for file dialogs" << std::endl;
    } else {
        std::cout << "📌 GTK+ already initialized" << std::endl;
    }

    // Create file chooser dialog
    GtkWidget* dialog = gtk_file_chooser_dialog_new(
        "Open File",
        nullptr,
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Open", GTK_RESPONSE_ACCEPT,
        nullptr
    );

    // Add file filters for video and audio
    GtkFileFilter* video_filter = gtk_file_filter_new();
    gtk_file_filter_set_name(video_filter, "Video Files");
    gtk_file_filter_add_pattern(video_filter, "*.mp4");
    gtk_file_filter_add_pattern(video_filter, "*.mov");
    gtk_file_filter_add_pattern(video_filter, "*.avi");
    gtk_file_filter_add_pattern(video_filter, "*.mkv");
    gtk_file_filter_add_pattern(video_filter, "*.m4v");
    gtk_file_filter_add_pattern(video_filter, "*.webm");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), video_filter);

    GtkFileFilter* audio_filter = gtk_file_filter_new();
    gtk_file_filter_set_name(audio_filter, "Audio Files");
    gtk_file_filter_add_pattern(audio_filter, "*.mp3");
    gtk_file_filter_add_pattern(audio_filter, "*.wav");
    gtk_file_filter_add_pattern(audio_filter, "*.flac");
    gtk_file_filter_add_pattern(audio_filter, "*.aac");
    gtk_file_filter_add_pattern(audio_filter, "*.m4a");
    gtk_file_filter_add_pattern(audio_filter, "*.ogg");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), audio_filter);

    GtkFileFilter* all_filter = gtk_file_filter_new();
    gtk_file_filter_set_name(all_filter, "All Files");
    gtk_file_filter_add_pattern(all_filter, "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), all_filter);

    // Run dialog
    std::cout << "🎯 Running GTK file dialog..." << std::endl;
    gint result = gtk_dialog_run(GTK_DIALOG(dialog));
    std::cout << "📝 Dialog result: " << result << " (GTK_RESPONSE_ACCEPT=" << GTK_RESPONSE_ACCEPT << ")" << std::endl;

    if (result == GTK_RESPONSE_ACCEPT) {
        char* filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));

        if (filename) {
            std::cout << "📂 Selected file: " << filename << std::endl;

            // Determine player_id based on parameter
            int active_player_id;
            if (target_player_id >= 0) {
                // Open for specific instance
                active_player_id = target_player_id;
                std::cout << "Opening file for specific player instance: " << target_player_id << std::endl;
            } else {
                // Auto-detect active player
                active_player_id = GetActivePlayerID();
                std::cout << "Opening file for active player: " << active_player_id << std::endl;
            }

            // Set initial loading state IMMEDIATELY
            UpdateOSDPosition(active_player_id, 0.0, 0.0);
            UpdateOSDPlayState(active_player_id, false, false, false);
            SetPlayerLoadingState(active_player_id, true);
            UpdateOSDDisplayMode(active_player_id, OSD_MODE_LOADING);
            SetPlayerLoadingProgress(active_player_id, 0);
            SetPlayerLoadingStatus(active_player_id, "threading");

            // Clear old video texture
            FSTPPixelBufferManager* pixel_mgr = GetPixelBufferManager();
            if (pixel_mgr) {
                pixel_mgr->ClearPlayerBuffers(active_player_id);
            }

            // Load file asynchronously
            std::thread([filename, active_player_id, target_player_id]() {
                std::cout << "Loading file: " << filename << std::endl;

                int instance_id = -1;

                // Check if player instance is already active
                if (active_player_id >= 0 && IsPlayerInstanceActive(active_player_id)) {
                    // Load file into existing instance
                    std::cout << "Loading file into existing player instance " << active_player_id << std::endl;
                    instance_id = LoadFileIntoPlayerInstance(filename, active_player_id);
                } else {
                    // Create new player instance
                    std::cout << "Creating new player instance for player " << active_player_id << std::endl;
                    instance_id = CreatePlayerInstance(filename, active_player_id);
                }

                if (instance_id >= 0) {
                    std::cout << "✅ File successfully loaded into player instance " << instance_id << std::endl;

                    // CRITICAL: Stop loading mode and switch to NORMAL
                    SetPlayerLoadingState(active_player_id, false);
                    UpdateOSDDisplayMode(active_player_id, OSD_MODE_NORMAL);
                    std::cout << "🔄 Loading complete, switched to NORMAL mode" << std::endl;

                    // Find window index for OSD update
                    int window_index = -1;
                    for (int i = 0; i < MAX_WINDOWS; i++) {
                        FSTPWindow* window = GetWindowByIndex(i);
                        if (window && window->player_instance_id == active_player_id) {
                            window_index = i;
                            break;
                        }
                    }

                    if (window_index >= 0) {
                        // Get file info and update OSD for specific window
                        double duration = GetInstanceDuration(instance_id);
                        UpdateWindowOSD(window_index, 0.0, duration, false, 1.0, false);
                        std::cout << "OSD updated for window " << window_index
                                 << " (player " << instance_id << "), duration: "
                                 << duration << " sec" << std::endl;
                    }
                } else {
                    std::cout << "❌ File loading error: " << instance_id << std::endl;
                    // On error return OSD to normal mode
                    SetPlayerLoadingState(active_player_id, false);
                    UpdateOSDDisplayMode(active_player_id, OSD_MODE_NO_FILE);
                }

                g_free(filename);
            }).detach();
        }
    }

    gtk_widget_destroy(dialog);
    std::cout << "🗑️ Dialog destroyed" << std::endl;

    // Process GTK events to ensure dialog closes properly
    while (gtk_events_pending()) {
        gtk_main_iteration();
    }

    // Flag will be automatically reset by DialogGuard destructor
}

// Load file directly from path (for command line arguments)
void LoadFileFromPath(const char* filepath, int target_player_id) {
    std::cout << "🔵 LoadFileFromPath called: " << filepath << std::endl;

    if (!filepath) {
        std::cerr << "❌ No filepath provided" << std::endl;
        return;
    }

    // Determine player_id based on parameter
    int active_player_id;
    if (target_player_id >= 0) {
        active_player_id = target_player_id;
        std::cout << "Loading file for specific player instance: " << target_player_id << std::endl;
    } else {
        active_player_id = GetActivePlayerID();
        std::cout << "Loading file for active player: " << active_player_id << std::endl;
    }

    // Set initial loading state
    UpdateOSDPosition(active_player_id, 0.0, 0.0);
    UpdateOSDPlayState(active_player_id, false, false, false);
    SetPlayerLoadingState(active_player_id, true);
    UpdateOSDDisplayMode(active_player_id, OSD_MODE_LOADING);
    SetPlayerLoadingProgress(active_player_id, 0);
    SetPlayerLoadingStatus(active_player_id, "threading");

    // Clear old video texture
    FSTPPixelBufferManager* pixel_mgr = GetPixelBufferManager();
    if (pixel_mgr) {
        pixel_mgr->ClearPlayerBuffers(active_player_id);
    }

    // Copy filepath to heap for thread
    char* filename_copy = strdup(filepath);

    // Load file asynchronously
    std::thread([filename_copy, active_player_id]() {
        std::cout << "Loading file from command line: " << filename_copy << std::endl;

        int instance_id = -1;

        // Check if player instance is already active
        if (active_player_id >= 0 && IsPlayerInstanceActive(active_player_id)) {
            // Load file into existing instance
            std::cout << "Loading file into existing player instance " << active_player_id << std::endl;
            instance_id = LoadFileIntoPlayerInstance(filename_copy, active_player_id);
        } else {
            // Create new player instance
            std::cout << "Creating new player instance for player " << active_player_id << std::endl;
            instance_id = CreatePlayerInstance(filename_copy, active_player_id);
        }

        if (instance_id >= 0) {
            std::cout << "✅ File successfully loaded into player instance " << instance_id << std::endl;

            // Stop loading mode and switch to NORMAL
            SetPlayerLoadingState(active_player_id, false);
            UpdateOSDDisplayMode(active_player_id, OSD_MODE_NORMAL);
            std::cout << "🔄 Loading complete, switched to NORMAL mode" << std::endl;

            // Find window index for OSD update
            int window_index = -1;
            for (int i = 0; i < MAX_WINDOWS; i++) {
                FSTPWindow* window = GetWindowByIndex(i);
                if (window && window->player_instance_id == active_player_id) {
                    window_index = i;
                    break;
                }
            }

            if (window_index >= 0) {
                // Get file info and update OSD for specific window
                double duration = GetInstanceDuration(instance_id);
                UpdateWindowOSD(window_index, 0.0, duration, false, 1.0, false);
                std::cout << "OSD updated for window " << window_index
                         << " (player " << instance_id << "), duration: "
                         << duration << " sec" << std::endl;
            }
        } else {
            std::cout << "❌ File loading error: " << instance_id << std::endl;
            // On error return OSD to normal mode
            SetPlayerLoadingState(active_player_id, false);
            UpdateOSDDisplayMode(active_player_id, OSD_MODE_NO_FILE);
        }

        free(filename_copy);
    }).detach();
}

// Forward declarations for helper functions
extern "C" double GetInstanceVideoFPS(int player_id);

// Memory Location Dialog callback
static void (*g_memory_location_dialog_closed_callback)() = nullptr;

extern "C" void OnMemoryLocationDialogClosedCallback() {
    if (g_memory_location_dialog_closed_callback) {
        g_memory_location_dialog_closed_callback();
    }
}

// GTK3 Memory Location Dialog for Linux (wrapper for backwards compatibility)
void ShowMemoryLocationDialog(int player_id, double current_time) {
    std::cout << "🔵 ShowMemoryLocationDialog called for player " << player_id
              << " at time " << current_time << std::endl;

    // Prevent multiple dialog invocations
    if (g_dialog_open) {
        std::cout << "⚠️ Dialog already open, skipping call" << std::endl;
        return;
    }

    DialogGuard guard;

    // Initialize GTK if needed
    if (!g_gtk_initialized) {
        gtk_init(nullptr, nullptr);
        g_gtk_initialized = true;
    }

    // Call unified dialog in add mode (location_id = -1)
    ShowGTKMemoryLocationDialog(player_id, -1, current_time);

    // Process events
    while (gtk_events_pending()) {
        gtk_main_iteration();
    }

    // Call close callback
    OnMemoryLocationDialogClosedCallback();

    std::cout << "🗑️  Memory Location dialog closed" << std::endl;
}

// Settings dialog is now in FSTPSettingsDialog.cpp (separate file)

// Implementation of copy screenshot to clipboard function for Linux
void CopyScreenshotToClipboard() {
    std::cout << "📸 Copying screenshot to clipboard..." << std::endl;

    // Get active player
    int active_player = GetActivePlayerID();
    if (active_player < 0) {
        std::cerr << "⚠️ No active player for screenshot" << std::endl;
        return;
    }

    // Get current pixel buffer
    FSTPPixelBufferManager* manager = GetPixelBufferManager();
    if (!manager) {
        std::cerr << "⚠️ Pixel buffer manager not available" << std::endl;
        return;
    }

    const FSTPPixelBufferManager::PixelBuffer* pixel_buffer =
        manager->GetPixelBuffer(active_player);

    if (!pixel_buffer || !pixel_buffer->is_valid || !pixel_buffer->av_frame) {
        std::cerr << "⚠️ No valid frame for screenshot" << std::endl;
        return;
    }

    int width = pixel_buffer->width;
    int height = pixel_buffer->height;
    double current_time = GetInstancePosition(active_player);

    // Get actual video FPS
    double video_fps = GetInstanceVideoFPS(active_player);
    if (video_fps <= 0) video_fps = 25.0; // fallback

    // Format timecode (HH:MM:SS:FF)
    int hours = (int)(current_time / 3600);
    int minutes = (int)((current_time - hours * 3600) / 60);
    int seconds = (int)(current_time - hours * 3600 - minutes * 60);
    int frames = (int)((current_time - (int)current_time) * video_fps);

    char timecode_buf[32];
    snprintf(timecode_buf, sizeof(timecode_buf), "%02d:%02d:%02d:%02d", hours, minutes, seconds, frames);
    std::string timecode(timecode_buf);

    // Determine pixel format
    enum AVPixelFormat pix_fmt = (enum AVPixelFormat)pixel_buffer->av_frame->format;
    bool is_nv12 = (pix_fmt == AV_PIX_FMT_NV12);

    // Validate av_frame data before copying
    if (!pixel_buffer->av_frame->data[0] || !pixel_buffer->av_frame->data[1]) {
        std::cerr << "❌ Screenshot failed: invalid av_frame data pointers (Y=" << (void*)pixel_buffer->av_frame->data[0]
                  << " UV=" << (void*)pixel_buffer->av_frame->data[1] << ")" << std::endl;
        return;
    }

    // For YUV420P check V plane
    if (!is_nv12 && !pixel_buffer->av_frame->data[2]) {
        std::cerr << "❌ Screenshot failed: YUV420P format but V plane is null" << std::endl;
        return;
    }

    std::cout << "📸 Screenshot format: " << (is_nv12 ? "NV12 (semi-planar)" : "YUV420P (planar)") << std::endl;

    // Get window index for active player
    int window_idx = -1;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        FSTPWindow* window = GetWindowByIndex(i);
        if (window && window->is_active && window->player_instance_id == active_player) {
            window_idx = i;
            break;
        }
    }

    // Get zoom state
    FSTPZoomState* zoom = nullptr;
    int window_width = width;
    int window_height = height;

    if (window_idx >= 0) {
        zoom = GetZoomState(window_idx);

        FSTPWindow* window = GetWindowByIndex(window_idx);
        if (window && window->window) {
            SDL_GetWindowSize(window->window, &window_width, &window_height);
        }
    }

    // Create temporary contiguous buffer for screenshot
    int y_size = width * height;
    int uv_size = (width / 2) * (height / 2);
    std::vector<uint8_t> temp_yuv_buffer(y_size + uv_size * 2);

    uint8_t* dst_y = temp_yuv_buffer.data();
    uint8_t* dst_u = dst_y + y_size;
    uint8_t* dst_v = dst_u + uv_size;

    // Copy Y plane
    for (int row = 0; row < height; row++) {
        memcpy(dst_y + row * width,
               pixel_buffer->av_frame->data[0] + row * pixel_buffer->av_frame->linesize[0],
               width);
    }

    // Copy U and V planes (NV12 vs YUV420P handling)
    int uv_height = height / 2;
    int uv_width = width / 2;

    if (is_nv12) {
        // NV12: UV interleaved (UVUVUVUV...) in data[1]
        // Split into separate U and V planes
        for (int row = 0; row < uv_height; row++) {
            const uint8_t* src_uv = pixel_buffer->av_frame->data[1] +
                                    row * pixel_buffer->av_frame->linesize[1];
            uint8_t* row_dst_u = dst_u + row * uv_width;
            uint8_t* row_dst_v = dst_v + row * uv_width;

            for (int col = 0; col < uv_width; col++) {
                row_dst_u[col] = src_uv[col * 2];     // U (even bytes)
                row_dst_v[col] = src_uv[col * 2 + 1]; // V (odd bytes)
            }
        }
    } else {
        // YUV420P: U and V separate planes
        for (int row = 0; row < uv_height; row++) {
            memcpy(dst_u + row * uv_width,
                   pixel_buffer->av_frame->data[1] + row * pixel_buffer->av_frame->linesize[1],
                   uv_width);
            memcpy(dst_v + row * uv_width,
                   pixel_buffer->av_frame->data[2] + row * pixel_buffer->av_frame->linesize[2],
                   uv_width);
        }
    }

    // For screenshots ALWAYS show thumbnail if zoom is active
    bool show_thumb = zoom && zoom->enabled && zoom->factor > 1.0f;

    bool success = TakeScreenshotFromPixelBuffer(
        temp_yuv_buffer.data(),  // YUV420P data
        width,
        height,
        timecode,
        window_width,
        window_height,
        zoom ? zoom->enabled : false,
        zoom ? zoom->factor : 1.0f,
        zoom ? zoom->center_x : 0.5f,
        zoom ? zoom->center_y : 0.5f,
        show_thumb
    );

    if (success) {
        std::cout << "✅ Screenshot copied to clipboard (" << width << "x" << height << ")" << std::endl;
    } else {
        std::cerr << "❌ Failed to copy screenshot to clipboard" << std::endl;
    }
}

// GTK3 Context Menu Window for Linux (Wayland-compatible)
// Instead of popup menu, we create a small GTK window with buttons
static void ShowContextMenu() {
    std::cout << "🔵 ShowContextMenu called (GTK Window mode for Wayland)" << std::endl;

    // Initialize GTK if needed
    if (!g_gtk_initialized) {
        gtk_init(nullptr, nullptr);
        g_gtk_initialized = true;
    }

    // Get mouse position
    int mouse_x, mouse_y;
    SDL_GetGlobalMouseState(&mouse_x, &mouse_y);
    std::cout << "🖱️  Mouse position: " << mouse_x << ", " << mouse_y << std::endl;

    // Create a small popup window (use TOPLEVEL for Wayland compatibility)
    GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Menu");
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);  // No title bar
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
    gtk_window_set_type_hint(GTK_WINDOW(window), GDK_WINDOW_TYPE_HINT_DIALOG);  // Use DIALOG instead of POPUP_MENU
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(window), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(window), TRUE);
    gtk_window_set_keep_above(GTK_WINDOW(window), TRUE);

    // Set window position at mouse cursor (after realizing the window)
    gtk_widget_realize(window);
    gtk_window_move(GTK_WINDOW(window), mouse_x, mouse_y);

    // Create vertical box for menu items
    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(window), vbox);

    // Style: add some padding
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 4);

    // Open File button
    GtkWidget* open_btn = gtk_button_new_with_label("📂 Open File...          Ctrl+O");
    gtk_button_set_relief(GTK_BUTTON(open_btn), GTK_RELIEF_NONE);
    gtk_widget_set_size_request(open_btn, 280, 32);
    g_signal_connect(open_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        GtkWidget* win = (GtkWidget*)data;
        gtk_widget_destroy(win);
        ShowNativeFileDialog(-1);
    }), window);
    gtk_box_pack_start(GTK_BOX(vbox), open_btn, FALSE, FALSE, 0);

    // Recent files — one button per entry (basename shown), loads the full path.
    // Each button carries a heap {window, path} pair freed when it fires.
    struct RecentClickData { GtkWidget* win; char* path; };
    int recent_count = GetRecentFileCount();
    for (int i = 0; i < recent_count; i++) {
        const char* rpath = GetRecentFile(i);
        if (!rpath) continue;
        std::string full(rpath);
        size_t slash = full.find_last_of('/');
        std::string name = (slash == std::string::npos) ? full : full.substr(slash + 1);
        std::string label = "    " + name; // slight indent under "Open File..."

        char* path_copy = new char[full.size() + 1];
        std::memcpy(path_copy, full.c_str(), full.size() + 1);
        auto* rc = new RecentClickData{ window, path_copy };

        GtkWidget* recent_btn = gtk_button_new_with_label(label.c_str());
        gtk_button_set_relief(GTK_BUTTON(recent_btn), GTK_RELIEF_NONE);
        gtk_widget_set_size_request(recent_btn, 280, 28);
        // Tie rc's lifetime to the button so it is freed even if the menu is
        // dismissed without clicking (no leak on the not-clicked path).
        g_object_set_data_full(G_OBJECT(recent_btn), "txp_recent",
            rc, +[](gpointer p) {
                auto* d = (RecentClickData*)p;
                delete[] d->path;
                delete d;
            });
        g_signal_connect(recent_btn, "clicked", G_CALLBACK(+[](GtkButton* btn, gpointer data) {
            auto* d = (RecentClickData*)data;
            GtkWidget* win = d->win;
            // Copy the path before destroying the window: destruction frees `d`
            // via the "txp_recent" destroy-notify above, invalidating d->path.
            std::string path = d->path ? d->path : "";
            gtk_widget_destroy(win);
            if (!path.empty()) LoadFileFromPath(path.c_str(), -1);
        }), rc);
        gtk_box_pack_start(GTK_BOX(vbox), recent_btn, FALSE, FALSE, 0);
    }
    if (recent_count > 0) {
        GtkWidget* clear_btn = gtk_button_new_with_label("    Clear Recent");
        gtk_button_set_relief(GTK_BUTTON(clear_btn), GTK_RELIEF_NONE);
        gtk_widget_set_size_request(clear_btn, 280, 28);
        g_signal_connect(clear_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
            GtkWidget* win = (GtkWidget*)data;
            gtk_widget_destroy(win);
            ClearRecentFiles();
        }), window);
        gtk_box_pack_start(GTK_BOX(vbox), clear_btn, FALSE, FALSE, 0);

        GtkWidget* sep_recent = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
        gtk_box_pack_start(GTK_BOX(vbox), sep_recent, FALSE, FALSE, 2);
    }

    // Copy Screenshot button
    GtkWidget* screenshot_btn = gtk_button_new_with_label("📸 Copy Screenshot       Ctrl+C");
    gtk_button_set_relief(GTK_BUTTON(screenshot_btn), GTK_RELIEF_NONE);
    gtk_widget_set_size_request(screenshot_btn, 280, 32);
    g_signal_connect(screenshot_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        GtkWidget* win = (GtkWidget*)data;
        gtk_widget_destroy(win);
        CopyScreenshotToClipboard();
    }), window);
    gtk_box_pack_start(GTK_BOX(vbox), screenshot_btn, FALSE, FALSE, 0);

    // Memory Locations Inspector button
    GtkWidget* inspector_btn = gtk_button_new_with_label("🔍 Memory Locations");
    gtk_button_set_relief(GTK_BUTTON(inspector_btn), GTK_RELIEF_NONE);
    gtk_widget_set_size_request(inspector_btn, 280, 32);
    g_signal_connect(inspector_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        GtkWidget* win = (GtkWidget*)data;
        gtk_widget_destroy(win);
        ShowGTKMemoryLocationsWindow();
    }), window);
    gtk_box_pack_start(GTK_BOX(vbox), inspector_btn, FALSE, FALSE, 0);

    // Separator
    GtkWidget* sep1 = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(vbox), sep1, FALSE, FALSE, 2);

    // Settings button
    GtkWidget* settings_btn = gtk_button_new_with_label("⚙️ Settings...           Ctrl+,");
    gtk_button_set_relief(GTK_BUTTON(settings_btn), GTK_RELIEF_NONE);
    gtk_widget_set_size_request(settings_btn, 280, 32);
    g_signal_connect(settings_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        GtkWidget* win = (GtkWidget*)data;
        gtk_widget_destroy(win);
        ShowGTKSettingsDialog();
    }), window);
    gtk_box_pack_start(GTK_BOX(vbox), settings_btn, FALSE, FALSE, 0);

    // Separator
    GtkWidget* sep2 = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(vbox), sep2, FALSE, FALSE, 2);

    // About button
    GtkWidget* about_btn = gtk_button_new_with_label("ℹ️  About TapeXPlayer");
    gtk_button_set_relief(GTK_BUTTON(about_btn), GTK_RELIEF_NONE);
    gtk_widget_set_size_request(about_btn, 280, 32);
    g_signal_connect(about_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        GtkWidget* menu_win = (GtkWidget*)data;
        gtk_widget_destroy(menu_win);

        // Show new About dialog (full-featured like macOS)
        ShowGTKAboutDialog();

        // Process GTK events
        while (gtk_events_pending()) {
            gtk_main_iteration();
        }
    }), window);
    gtk_box_pack_start(GTK_BOX(vbox), about_btn, FALSE, FALSE, 0);

    // Separator
    GtkWidget* sep_log = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(vbox), sep_log, FALSE, FALSE, 2);

    // Open Log Folder — reveals the always-on session log in the file manager.
    GtkWidget* open_log_btn = gtk_button_new_with_label("🗂  Open Log Folder");
    gtk_button_set_relief(GTK_BUTTON(open_log_btn), GTK_RELIEF_NONE);
    gtk_widget_set_size_request(open_log_btn, 280, 32);
    g_signal_connect(open_log_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        GtkWidget* win = (GtkWidget*)data;
        gtk_widget_destroy(win);
        FSTPLog::RevealLogFolder();
    }), window);
    gtk_box_pack_start(GTK_BOX(vbox), open_log_btn, FALSE, FALSE, 0);

    // Save Diagnostic Report — log + system info as one file for a bug report.
    GtkWidget* save_report_btn = gtk_button_new_with_label("💾 Save Diagnostic Report...");
    gtk_button_set_relief(GTK_BUTTON(save_report_btn), GTK_RELIEF_NONE);
    gtk_widget_set_size_request(save_report_btn, 280, 32);
    g_signal_connect(save_report_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        GtkWidget* menu_win = (GtkWidget*)data;
        gtk_widget_destroy(menu_win);
        GtkWidget* dlg = gtk_file_chooser_dialog_new(
            "Save Diagnostic Report", nullptr, GTK_FILE_CHOOSER_ACTION_SAVE,
            "_Cancel", GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_ACCEPT, nullptr);
        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), "TapeXPlayer-diagnostic.txt");
        gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
        if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
            char* fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
            if (fn) { FSTPLog::SaveDiagnosticReport(std::string(fn)); g_free(fn); }
        }
        gtk_widget_destroy(dlg);
        while (gtk_events_pending()) gtk_main_iteration();
    }), window);
    gtk_box_pack_start(GTK_BOX(vbox), save_report_btn, FALSE, FALSE, 0);

    // Close window when it loses focus
    g_signal_connect(window, "focus-out-event", G_CALLBACK(+[](GtkWidget* win, GdkEvent*, gpointer) -> gboolean {
        std::cout << "🗑️  Menu window lost focus, closing..." << std::endl;
        gtk_widget_destroy(win);
        return FALSE;
    }), nullptr);

    // Show window
    gtk_widget_show_all(window);
    gtk_window_present(GTK_WINDOW(window));

    // Grab focus to detect clicks outside
    gtk_widget_grab_focus(window);

    std::cout << "✅ Context menu window displayed" << std::endl;

    // Process GTK events
    for (int i = 0; i < 5; i++) {
        while (gtk_events_pending()) {
            gtk_main_iteration();
        }
        SDL_Delay(1);
    }
}

// C API bridge for context menu
extern "C" void ShowGTKContextMenu() {
    ShowContextMenu();
}

// Set initial file to load from command line
void SetInitialFileToLoad(const char* filepath) {
    g_initial_file_to_load = filepath;
    std::cout << "📂 Initial file to load set: " << filepath << std::endl;
}

// Main UI loop for Linux - full implementation (based on macOS)
int RunMainUILoop() {
    std::cout << "Starting Linux/SDL2 UI main loop..." << std::endl;

    // SDL hints for optimal rendering
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");  // Bilinear filtering
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "1");           // Enable VSync

    // CRITICAL: Set application ID for GNOME dock integration (SDL 2.0.22+)
    // This sets WM_CLASS on X11 and app_id on Wayland
    SDL_SetHint("SDL_APP_ID", "TapeXPlayer");
    std::cout << "[GNOME] Set SDL_APP_ID=TapeXPlayer for dock integration" << std::endl;

    // Allow system screen saver and display sleep to work normally
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");

    // Initialize SDL
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        std::cerr << "SDL initialization error: " << SDL_GetError() << std::endl;
        return -1;
    }

    // SDL disables the screensaver by default - re-enable it so the display can sleep normally
    SDL_EnableScreenSaver();

    std::cout << "SDL2 initialized successfully" << std::endl;

    // Initialize window manager
    if (InitWindowManager() != 0) {
        std::cerr << "Window manager initialization error" << std::endl;
        SDL_Quit();
        return -1;
    }

    std::cout << "Window manager initialized" << std::endl;

    // Create main window (automatically bound to player #0)
    int main_window_index = CreateNewWindow("TapeXPlayer 2026 - Player 0", 1280, 720);
    if (main_window_index < 0) {
        std::cerr << "Main window creation error" << std::endl;
        ShutdownWindowManager();
        SDL_Quit();
        return -1;
    }

    // Get main window for OSD system
    FSTPWindow* main_window = GetMainWindow();
    if (main_window == nullptr) {
        std::cerr << "Error getting main window" << std::endl;
        ShutdownWindowManager();
        SDL_Quit();
        return -1;
    }

    std::cout << "Main window created (ID: " << main_window_index << ")" << std::endl;

    // Initialize settings system
    if (InitSettings() != 0) {
        std::cerr << "Settings system initialization warning (non-critical)" << std::endl;
    }

    // Initialize OSD system with main window renderer
    if (InitOSDSystem(main_window->renderer) != 0) {
        std::cerr << "OSD system initialization error" << std::endl;
        ShutdownWindowManager();
        SDL_Quit();
        return -1;
    }

    std::cout << "OSD system initialized" << std::endl;

    // CRITICAL: On Linux with SSH/X11, render from MAIN thread (SDL/OpenGL context requirement)
    // Instead, we'll make CLEANUP asynchronous to keep rendering active
    // StartAutonomousRendering(main_window->renderer);
    g_renderingActive = true;
    std::cout << "🔧 [LINUX] Rendering from main thread (SDL context requirement)" << std::endl;

    std::cout << "Ready! Press ESC or Ctrl+Q to exit, Ctrl+O to open file" << std::endl;

    // Load initial file from command line if provided
    if (g_initial_file_to_load) {
        std::cout << "📂 Loading initial file from command line: " << g_initial_file_to_load << std::endl;
        LoadFileFromPath(g_initial_file_to_load, 0);  // Load into player 0
    }

    // First-run onboarding: show the Welcome overlay once per FSTP_WELCOME_VERSION.
    if (GetWelcomeVersion() < FSTP_WELCOME_VERSION) {
        FSTPWelcome_Show();
        SetWelcomeVersion(FSTP_WELCOME_VERSION);
    }

    // Main event loop - similar to macOS version
    bool running = true;
    SDL_Event event;
    std::thread cleanup_thread;

    while (running) {
        // Render frame from main thread (SDL context requirement)
        AutoRenderFrame();

        // CRITICAL: Asynchronous shutdown - destroy player instances in SEPARATE THREAD
        // This allows main thread to continue rendering "unthreading" OSD
        if (g_shutdownRequested.load() && !g_cleanupThreadStarted.load()) {
            std::cout << "🛑 [SHUTDOWN] Shutdown requested, starting async cleanup thread..." << std::endl;
            g_cleanupThreadStarted.store(true);

            // Launch cleanup in separate thread while main loop continues rendering
            cleanup_thread = std::thread([&]() {
                std::cout << "🧵 [CLEANUP THREAD] Started cleanup while main thread renders..." << std::endl;

                // Destroy all player instances while main thread CONTINUES RENDERING
                for (int i = 0; i < MAX_WINDOWS; i++) {
                    FSTPWindow* window = GetWindowByIndex(i);
                    if (window && window->is_active) {
                        int player_id = window->player_instance_id;
                        if (player_id >= 0 && IsPlayerInstanceActive(player_id)) {
                            std::cout << "🎬 [CLEANUP] Destroying player " << player_id << " (main thread rendering)" << std::endl;
                            DestroyPlayerInstance(player_id);
                            std::cout << "✅ [CLEANUP] Player " << player_id << " destroyed" << std::endl;
                        }
                    }
                }

                std::cout << "✅ [CLEANUP THREAD] All player instances destroyed" << std::endl;

                // WORKAROUND: PipeWire has corrupted heap during normal operation
                // Calling exit() or returning to main would trigger PipeWire thread cleanup
                // which crashes in tcache_thread_shutdown due to malloc corruption
                // Solution: Use _exit() to terminate immediately without library cleanup
                std::cout << "🚀 [CLEANUP] Exiting immediately to avoid PipeWire cleanup crash..." << std::endl;
                _exit(0);  // Exit without calling atexit handlers or thread cleanup
            });
        }

        // Exit when cleanup is complete
        if (g_shutdownComplete.load()) {
            std::cout << "🛑 [MAIN] Cleanup complete, exiting UI loop" << std::endl;
            running = false;
            break;
        }

        // Process events with PollEvent (non-blocking)
        while (SDL_PollEvent(&event)) {
            // Welcome overlay (first-run) intercepts its own clicks/keys — and
            // Esc to dismiss — before the loop treats Esc as "quit".
            if (FSTPWelcome_HandleEvent(&event)) { continue; }

            // Intercept exit events and trigger graceful shutdown
            if (event.type == SDL_QUIT) {
                std::cout << "[EXIT] SDL_QUIT received - requesting asynchronous shutdown" << std::endl;
                g_shutdownRequested.store(true);
                break;
            }

            // Check for window close event
            if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE) {
                std::cout << "[EXIT] Window close requested - requesting asynchronous shutdown" << std::endl;
                g_shutdownRequested.store(true);
                break;
            }

            // Check for ESC and Ctrl+Q BEFORE keyboard handler
            if (event.type == SDL_KEYDOWN) {
                if (event.key.keysym.sym == SDLK_ESCAPE) {
                    std::cout << "[EXIT] ESC pressed - requesting asynchronous shutdown" << std::endl;
                    g_shutdownRequested.store(true);
                    break;
                }
                if (event.key.keysym.sym == SDLK_q && (event.key.keysym.mod & KMOD_CTRL)) {
                    std::cout << "[EXIT] Ctrl+Q pressed - requesting asynchronous shutdown" << std::endl;
                    g_shutdownRequested.store(true);
                    break;
                }
            }

            // Skip other event handling if we're shutting down
            if (g_shutdownRequested.load()) break;

            // Handle mouse events
            if (event.type == SDL_MOUSEBUTTONDOWN) {
                std::cout << "DEBUG: Mouse button " << (int)event.button.button << " pressed" << std::endl;
                if (event.button.button == SDL_BUTTON_RIGHT) {
                    // Right click shows context menu
                    std::cout << "🖱️  Right mouse button clicked, showing context menu..." << std::endl;
                    ShowGTKContextMenu();
                    continue; // Skip other handlers
                }
            }

            // Pass events to window manager
            HandleWindowEvents(&event);

            // Process keyboard events for player control
            HandleKeyboardEvents(event);
        }

        // Process pending GTK events (needed for context menu and dialogs)
        if (g_gtk_initialized) {
            while (gtk_events_pending()) {
                gtk_main_iteration();
            }
        }

        // Small delay to limit FPS
        SDL_Delay(16); // ~60 FPS
    }

    std::cout << "🛑 Shutting down gracefully..." << std::endl;

    // Step 1: Wait for cleanup thread to finish
    if (cleanup_thread.joinable()) {
        cleanup_thread.join();
        std::cout << "✅ Cleanup thread joined" << std::endl;
    }

    // Step 2: Stop rendering
    g_renderingActive = false;
    std::cout << "✅ Rendering stopped" << std::endl;

    // Step 3: CRITICAL - Clear PixelBufferManager
    // PixelBufferManager holds shared_ptr<AVFrame> that reference decoder contexts.
    std::cout << "🖼️  Clearing pixel buffer manager..." << std::endl;
    FSTPPixelBufferManager* pixel_mgr = GetPixelBufferManager();
    if (pixel_mgr) {
        // Clear all player buffers (frees AVFrames)
        for (int i = 0; i < 16; i++) {  // MAX_PLAYERS
            pixel_mgr->ClearPlayerBuffers(i);
        }
        std::cout << "✅ Pixel buffer manager cleared" << std::endl;
    }

    // Step 4: Shutdown player manager (instances already destroyed in async shutdown)
    std::cout << "🎬 Shutting down player manager..." << std::endl;
    ShutdownPlayerManager();
    std::cout << "✅ Player manager shutdown complete" << std::endl;

    // Step 5: Shutdown OSD system
    std::cout << "📊 Shutting down OSD system..." << std::endl;
    ShutdownOSDSystem();
    std::cout << "✅ OSD system shutdown complete" << std::endl;

    // Step 6: Shutdown Settings system
    std::cout << "⚙️  Shutting down Settings system..." << std::endl;
    ShutdownSettings();
    std::cout << "✅ Settings system shutdown complete" << std::endl;

    // Step 7: Shutdown Window Manager (pixel buffers already cleared in Step 3)
    std::cout << "🪟 Shutting down Window Manager..." << std::endl;
    ShutdownWindowManager();
    std::cout << "✅ Window Manager shutdown complete" << std::endl;

    // Step 8: Process remaining GTK events
    if (g_gtk_initialized) {
        std::cout << "🔧 Processing final GTK events..." << std::endl;
        // Process any pending GTK events
        while (gtk_events_pending()) {
            gtk_main_iteration();
        }
        std::cout << "✅ GTK events processed" << std::endl;
    }

    std::cout << "✨ Clean shutdown complete!" << std::endl;

    // CRITICAL: Skip SDL_Quit() to avoid GTK/SDL X11 display conflicts!
    //
    // Problem: When mixing GTK and SDL2, both manage X11 display connections.
    // Calling SDL_Quit() → SDL_QuitSubSystem(VIDEO) → XCloseDisplay() can cause
    // heap corruption due to conflicts with GTK's X11 connection.
    //
    // Solution: Use _exit() to let the OS clean up all resources atomically.
    // This is the recommended approach for mixed-toolkit applications.
    // All file descriptors, memory, and handles are properly cleaned by the kernel.
    //
    // Note: All application state (video frames, decoder contexts, audio, etc.)
    // has been properly cleaned up in previous steps. Only SDL/GTK/X11 resources
    // remain, and these are safer to let the OS clean up.

    std::cout << "🔚 Exiting cleanly via _exit(0) (OS will clean up SDL/GTK/X11)" << std::endl;
    _exit(0);  // Clean exit via OS cleanup (avoids GTK/SDL conflicts)
}

// Request force render (for UI changes like zoom)
extern "C" void RequestForceRender() {
    // Force render on next frame
    // (simplified version for Linux, no special flag needed)
}

#endif // __linux__
