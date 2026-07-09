#include "FSTPWindowManager.h"
#include "FSTPOSDSystem.h"
#include "FSTPOSDInstance.h"
#include "FSTPPixelBufferManager.h"
#include "FSTPBetacamEffect.h"
#include "FSTPSettings.h"
#include "FSTPZoom.h"
#include "FSTPKeyboard.h"
#include "FSTPWelcomeScreen.h"
#include "FSTPSubtitles.h"
#include "FSTPLuaExtension.h"
#include "../FSTPPlayerModule/FSTPPlayerManager.h"
#include "../FSTPAudioModule/FSTPAudioModule_API.h"
#include "../FSTPAudioModule/FSTPAudioModule_wrapper.h"
#include <iostream>
#include <iomanip>
#include <cstring>
#include <mutex>
#include <chrono>
#include <thread>
#include <atomic>
#include <cmath>

#ifdef __APPLE__
#include <dispatch/dispatch.h>
#include "darwin/sdl/FSTPToolsMenu.h"  // RefreshMemoryLocationsWindowIfOpen()
#endif

extern "C" {
#include <libavutil/frame.h>
}


// Global counter of skipped texture updates (for adaptive FPS)
std::atomic<int> g_texture_skip_counter{0};

// Debug control: set to true to enable verbose logging
static constexpr bool ENABLE_WINDOW_MANAGER_DEBUG = false;

// Counting rectangle with preserving aspect ratio (letterbox/pillarbox)
static SDL_Rect ComputeAspectFitRect(int texture_width, int texture_height, int target_width, int target_height) {
    SDL_Rect dst = {0, 0, target_width, target_height};

    if (texture_width <= 0 || texture_height <= 0 || target_width <= 0 || target_height <= 0) {
        return dst;
    }

    // Compare aspect ratios
    // Use double precision for stability
    double tex_aspect = static_cast<double>(texture_width) / static_cast<double>(texture_height);
    double tgt_aspect = static_cast<double>(target_width) / static_cast<double>(target_height);

    if (tgt_aspect > tex_aspect) {
        // Target wider: fit by height, add side fields
        int h = target_height;
        int w = static_cast<int>(h * tex_aspect + 0.5);
        int x = (target_width - w) / 2;
        dst = { x, 0, w, h };
    } else {
        // Target narrower: fit by width, add top/bottom fields
        int w = target_width;
        int h = static_cast<int>(w / tex_aspect + 0.5);
        int y = (target_height - h) / 2;
        dst = { 0, y, w, h };
    }

    return dst;
}

static SDL_Renderer* CreateRendererWithVSync(SDL_Window* window, int index, Uint32 flags) {
#if !SDL_VERSION_ATLEAST(2,0,18)
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "1");
#endif
    SDL_Renderer* renderer = SDL_CreateRenderer(window, index, flags);
#if SDL_VERSION_ATLEAST(2,0,18)
    if (renderer) {
        SDL_RenderSetVSync(renderer, 1);
    }
#endif
    return renderer;
}

// Global array of windows
static FSTPWindow g_windows[MAX_WINDOWS];
static bool g_window_manager_initialized = false;

// Global pixel buffer manager
static std::unique_ptr<FSTPPixelBufferManager> g_pixel_buffer_manager;

// Presentation mode: external display output (separate YUV renderer for separate SDL_Renderer)
static struct {
    SDL_Window*   window   = nullptr;
    SDL_Renderer* renderer = nullptr;
    int  linked_player_id  = -1;
    bool active  = false;
    bool enabled = false;
    double last_timestamp = -1.0;
    bool pending_renderer = false;
    FSTPYUVRenderer* yuv_renderer = nullptr;
    int    yuv_w   = 0;
    int    yuv_h   = 0;
    Uint32 yuv_fmt = 0;
    // Output target + focus behaviour (mirrors Settings; see CreatePresentationWindow).
    bool follow_focus  = true;   // true = follow focused player; false = pinned
    int  pinned_player = 0;      // player id used when follow_focus == false
    int  output_mode   = 0;      // 0 = external display, 1 = separate window
    int  display_index = -1;     // chosen external display (for re-apply)
    bool windowed      = false;  // true = separate window (cursor allowed on chrome)
    Uint32 window_id   = 0;      // for ENTER/LEAVE cursor handling
} g_presentation;

// Serialises the render thread's presentation block against ClosePresentationWindow /
// CreatePresentationWindow (main thread). Without it, tearing the renderer/window/yuv_renderer
// down (e.g. Shift+P close) while the render thread is mid-blit is a use-after-free → segfault.
static std::mutex g_presentation_mutex;

// Serialises the ENTIRE render pass (RenderAllWindows) against window lifecycle changes
// (CreateNewWindow / FSTPCloseWindow / ShutdownWindowManager). On macOS the render thread runs
// on Metal, which tolerates the old "is_closing flag + 35ms sleep" handshake; on Windows the
// render thread runs on Direct3D 11, whose device context is NOT thread-safe, so destroying a
// renderer/texture/window on the event thread while the render thread is mid-frame is a hard
// race → crash. This mutex makes render vs. create/close mutually exclusive on every platform.
// Lock ordering: g_render_mutex is always acquired BEFORE g_presentation_mutex (RenderAllWindows
// takes render first, then presentation later in the same function); the lifecycle functions and
// the presentation functions never hold both, so there is no inversion.
static std::mutex g_render_mutex;

int InitWindowManager() {
    if (g_window_manager_initialized) {
        std::cout << "Window manager already initialized" << std::endl;
        return 0;
    }

    std::cout << "Initializing window manager..." << std::endl;

    // Initialize pixel buffer manager
    g_pixel_buffer_manager = std::make_unique<FSTPPixelBufferManager>();
    if (!g_pixel_buffer_manager->Initialize()) {
        std::cerr << "Failed to initialize pixel buffer manager" << std::endl;
        return -1;
    }

    SetBetacamEffectEnabled(GetBetacamEffectEnabled());

    // Initialize array of windows
    for (int i = 0; i < MAX_WINDOWS; i++) {
        g_windows[i].window = nullptr;
        g_windows[i].renderer = nullptr;
        g_windows[i].window_id = 0;
        g_windows[i].is_active = false;
        g_windows[i].has_focus = false;
        g_windows[i].is_minimized = false;
        g_windows[i].player_instance_id = -1;
        g_windows[i].osd_instance = nullptr;
        memset(g_windows[i].window_title, 0, sizeof(g_windows[i].window_title));

        // Initialize DOUBLE TEXTURE BUFFERING
        g_windows[i].texture_buffer[0] = nullptr;
        g_windows[i].texture_buffer[1] = nullptr;
        g_windows[i].texture_buffer_width[0] = 0;
        g_windows[i].texture_buffer_width[1] = 0;
        g_windows[i].texture_buffer_height[0] = 0;
        g_windows[i].texture_buffer_height[1] = 0;
        g_windows[i].texture_buffer_timestamp[0] = 0.0;
        g_windows[i].texture_buffer_timestamp[1] = 0.0;
        g_windows[i].texture_buffer_valid[0] = false;
        g_windows[i].texture_buffer_valid[1] = false;
        g_windows[i].current_buffer_index = 0;  // Start with buffer 0
        g_windows[i].write_buffer_index = 1;    // Write to buffer 1
        g_windows[i].last_rendered_frame = -1;
        g_windows[i].betacam_hold_frames = 0;
        g_windows[i].last_effect_speed = 0.0;

        // Deprecated fields for compatibility
        g_windows[i].video_texture = nullptr;
        g_windows[i].previous_video_texture = nullptr;
        g_windows[i].video_width = 0;
        g_windows[i].video_height = 0;
        g_windows[i].video_timestamp = 0.0;
        g_windows[i].video_texture_valid = false;
    }

    g_window_manager_initialized = true;
    std::cout << "Window manager initialized successfully" << std::endl;
    return 0;
}

void ShutdownWindowManager() {
    if (!g_window_manager_initialized) {
        return;
    }

    std::cout << "Shutting down window manager..." << std::endl;

    // Shutdown pixel buffer manager
    if (g_pixel_buffer_manager) {
        g_pixel_buffer_manager->Shutdown();
        g_pixel_buffer_manager.reset();
    }

    // Close all active windows
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (g_windows[i].is_active) {
            FSTPCloseWindow(i);
        }
    }

    // Close presentation window
    ClosePresentationWindow();

    g_window_manager_initialized = false;
    std::cout << "Window manager shutdown complete" << std::endl;
}

int CreateNewWindow(const char* title, int width, int height) {
    if (!g_window_manager_initialized) {
        std::cerr << "Window manager not initialized" << std::endl;
        return -1;
    }

    // Serialise against the render thread: it must not iterate g_windows while we create and
    // half-initialise a new slot. Held across SDL_CreateWindow/Renderer (a few ms) — the render
    // thread simply waits one frame.
    std::lock_guard<std::mutex> _render_lock(g_render_mutex);

    // Add stack trace for debugging
    std::cout << "CreateNewWindow called with title: " << title << std::endl;
    std::cout << "Stack trace: CreateNewWindow called from somewhere" << std::endl;

    // Find free place for window
    int window_index = -1;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!g_windows[i].is_active) {
            window_index = i;
            break;
        }
    }

    if (window_index == -1) {
        std::cerr << "Maximum number of windows reached (" << MAX_WINDOWS << ")" << std::endl;
        return -2;
    }

    std::cout << "Creating window " << window_index << ": " << title << std::endl;

    // Prefer software rendering (hints already set globally)
    std::cout << "🖼️ [WINDOW MANAGER] Attempting to create software renderer..." << std::endl;
    
    // Create SDL window without hardware acceleration
    SDL_Window* window = SDL_CreateWindow(
        title,
        SDL_WINDOWPOS_UNDEFINED,
        SDL_WINDOWPOS_UNDEFINED,
        width, height,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
    );

    if (window == nullptr) {
        std::cerr << "Failed to create window: " << SDL_GetError() << std::endl;
        return -3;
    }

    // Set minimum window size to prevent OSD elements from overlapping
    // Minimum 800x450 (16:9) ensures all OSD elements fit without overlap:
    // - VU meters (left): 180px
    // - Timecode (center): ~400px
    // - Position indicator (right): 112px
    // - Bottom OSD height: ~100px
    SDL_SetWindowMinimumSize(window, 800, 450);

    // Try creating renderer with fallback strategy
    SDL_Renderer* renderer = nullptr;

    // Attempt 1: Try SOFTWARE renderer first on Linux for debugging
    // SOFTWARE renderer should always work
    std::cout << "🔧 [RENDERER] Trying SOFTWARE renderer for debugging..." << std::endl;

    #ifdef __linux__
        // On Linux: try hardware acceleration first for Intel Celeron + VA-API
        std::cout << "🔧 [RENDERER] Trying hardware acceleration for Intel Celeron + VA-API..." << std::endl;
        
        // Try OpenGL with VA-API support first
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl");
        SDL_SetHint(SDL_HINT_RENDER_VSYNC, "0");
        renderer = CreateRendererWithVSync(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        
        if (!renderer) {
            std::cout << "⚠️  OpenGL failed, trying Vulkan..." << std::endl;
            SDL_SetHint(SDL_HINT_RENDER_DRIVER, "vulkan");
            renderer = CreateRendererWithVSync(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        }
        
        if (!renderer) {
            std::cout << "⚠️  Vulkan failed, trying Direct3D..." << std::endl;
            SDL_SetHint(SDL_HINT_RENDER_DRIVER, "direct3d");
            renderer = CreateRendererWithVSync(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        }
        
        if (!renderer) {
            std::cout << "⚠️  All hardware renderers failed, falling back to software..." << std::endl;
            SDL_SetHint(SDL_HINT_RENDER_DRIVER, "");  // Reset hint
            renderer = CreateRendererWithVSync(window, -1, SDL_RENDERER_SOFTWARE);
        }
    #else
        // On macOS: use Metal
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
        SDL_SetHint(SDL_HINT_RENDER_VSYNC, "0");
        renderer = CreateRendererWithVSync(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    #endif

    if (!renderer) {
        std::cout << "⚠️  Hardware accelerated renderer failed (" << SDL_GetError() << "), trying default..." << std::endl;

        // Attempt 2: Any available renderer
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, "");  // Reset hint
        renderer = CreateRendererWithVSync(window, -1, SDL_RENDERER_PRESENTVSYNC);

        if (!renderer) {
            std::cout << "⚠️  Default renderer failed (" << SDL_GetError() << "), trying software..." << std::endl;

            // Attempt 3: Software fallback (last attempt)
            renderer = CreateRendererWithVSync(window, -1, SDL_RENDERER_SOFTWARE);

            if (!renderer) {
                std::cout << "⚠️  Software renderer failed (" << SDL_GetError() << "), trying with index..." << std::endl;

                // Attempt 4: Explicitly specify any renderer by index
                int num_drivers = SDL_GetNumRenderDrivers();
                for (int i = 0; i < num_drivers; i++) {
                    SDL_RendererInfo info;
                    if (SDL_GetRenderDriverInfo(i, &info) == 0) {
                        std::cout << "Available driver " << i << ": " << info.name << std::endl;
                        renderer = CreateRendererWithVSync(window, i, SDL_RENDERER_PRESENTVSYNC);
                        if (renderer) {
                            std::cout << "✅ Success with driver: " << info.name << std::endl;
                            break;
                        }
                    }
                }
            }
        }
    }
    
    if (renderer == nullptr) {
        std::cerr << "Failed to create any renderer: " << SDL_GetError() << std::endl;
        SDL_DestroyWindow(window);
        return -4;
    }
    
    // Get information about renderer
    SDL_RendererInfo renderer_info;
    if (SDL_GetRendererInfo(renderer, &renderer_info) == 0) {
        std::cout << "🖼️ [RENDERER] Using: " << renderer_info.name << std::endl;

        // Metal - optimal choice for macOS
        if (strstr(renderer_info.name, "metal") || strstr(renderer_info.name, "Metal")) {
            std::cout << "✅ [METAL] Native GPU acceleration enabled (optimal for macOS)" << std::endl;
        } else if (strstr(renderer_info.name, "opengl") || strstr(renderer_info.name, "OpenGL")) {
            std::cout << "⚠️  [OPENGL] Using OpenGL (Metal would be faster on macOS)" << std::endl;
        }
    }
    
    std::cout << "🎬 [FINAL SETUP] Window " << window_index << " ready: YUV textures + Double buffering + Software timing (no VSync)" << std::endl;

    // Fill window structure
    g_windows[window_index].window = window;
    g_windows[window_index].renderer = renderer;
    g_windows[window_index].window_id = SDL_GetWindowID(window);
    g_windows[window_index].is_active = true;
    g_windows[window_index].has_focus = true;
    g_windows[window_index].is_minimized = false;
    g_windows[window_index].is_closing = false;

    // Hard binding: window N bound to player N
    g_windows[window_index].player_instance_id = WINDOW_PLAYER_BINDING(window_index);
    strncpy(g_windows[window_index].window_title, title, sizeof(g_windows[window_index].window_title) - 1);

    // OSD uses original system - nothing to create
    g_windows[window_index].osd_instance = nullptr;

    // Initialize zoom state for this window
    InitZoomForWindow(window_index);

    std::cout << "Window " << window_index << " created successfully with ID: " << g_windows[window_index].window_id
              << " (bound to player " << g_windows[window_index].player_instance_id << ")" << std::endl;

    // Set correct window title with format "TapeXPlayer - Instance #X"
    UpdateWindowTitle(window_index, g_windows[window_index].player_instance_id, nullptr);

    return window_index;
}

void FSTPCloseWindow(int window_index) {
    if (window_index < 0 || window_index >= MAX_WINDOWS || !g_windows[window_index].is_active) {
        return;
    }

    // Serialise against the render thread. Acquiring g_render_mutex GUARANTEES the render pass is
    // not in flight, so destroying textures/renderer/window below cannot race a mid-frame present.
    // This replaces the old "is_closing flag + 35ms sleep" handshake, which was only empirically
    // safe on Metal and was a genuine race on Direct3D 11 (Windows).
    std::lock_guard<std::mutex> _render_lock(g_render_mutex);

    std::cout << "Closing window " << window_index << " (bound to player " << g_windows[window_index].player_instance_id << ")" << std::endl;

    // Still set is_closing: RenderAllWindows checks it (belt-and-braces), and any code path that
    // reads it outside the render lock (e.g. OSD update helpers) sees the window going away.
    g_windows[window_index].is_closing = true;

    // OSD uses original system - nothing to free
    g_windows[window_index].osd_instance = nullptr;

    // Free DOUBLE TEXTURE BUFFERING
    for (int buf = 0; buf < 2; buf++) {
        if (g_windows[window_index].texture_buffer[buf]) {
            SDL_Texture* texture = g_windows[window_index].texture_buffer[buf];
            g_windows[window_index].texture_buffer[buf] = nullptr;
            g_windows[window_index].texture_buffer_valid[buf] = false;
            
            // Check validity before freeing
            Uint32 format;
            int access, width, height;
            if (SDL_QueryTexture(texture, &format, &access, &width, &height) == 0) {
                SDL_DestroyTexture(texture);
            }
        }
    }
    
    // Clear deprecated fields
    g_windows[window_index].video_texture = nullptr;
    g_windows[window_index].previous_video_texture = nullptr;
    g_windows[window_index].video_texture_valid = false;

    // Destroy SDL resources
    if (g_windows[window_index].renderer) {
        // CRITICAL: Clear OSD cache BEFORE destroying renderer
        // Otherwise ShutdownOSDSystem will try to delete textures of already destroyed renderer → CRASH
        ClearOSDCacheForRenderer(g_windows[window_index].renderer);

        SDL_DestroyRenderer(g_windows[window_index].renderer);
        g_windows[window_index].renderer = nullptr;
    }

    if (g_windows[window_index].window) {
        SDL_DestroyWindow(g_windows[window_index].window);
        g_windows[window_index].window = nullptr;
    }

    // Reset structure
    g_windows[window_index].window_id = 0;
    g_windows[window_index].is_active = false;
    g_windows[window_index].has_focus = false;
    g_windows[window_index].is_minimized = false;
    g_windows[window_index].player_instance_id = -1;
    memset(g_windows[window_index].window_title, 0, sizeof(g_windows[window_index].window_title));
    g_windows[window_index].betacam_hold_frames = 0;
    g_windows[window_index].last_effect_speed = 0.0;

    std::cout << "Window " << window_index << " closed" << std::endl;
}

int GetPlayerIDForWindow(int window_index) {
    if (window_index < 0 || window_index >= MAX_WINDOWS) {
        return -1;
    }

    // Hard binding: window N always bound to player N+1
    return WINDOW_PLAYER_BINDING(window_index);
}

int IsWindowPlayerBindingActive(int window_index) {
    if (window_index < 0 || window_index >= MAX_WINDOWS || !g_windows[window_index].is_active) {
        return 0;
    }

    // In hard binding window always bound to its player
    return 1;
}

FSTPWindow* GetWindowBySDLID(Uint32 sdl_window_id) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (g_windows[i].is_active && g_windows[i].window_id == sdl_window_id) {
            return &g_windows[i];
        }
    }
    return nullptr;
}

FSTPWindow* GetWindowByIndex(int index) {
    if (index < 0 || index >= MAX_WINDOWS || !g_windows[index].is_active) {
        return nullptr;
    }
    return &g_windows[index];
}

// AUTO-FREEZE: Freezing inactive players when switching focus
static void HandleAutoFreezeOnFocusChange(int focused_window_index, bool gained_focus) {
    // Check setting
    if (GetAutoFreezeInactive() != 1) {
        return; // Function disabled in settings
    }

    if (!gained_focus) {
        // FOCUS_LOST: Check if we switched to another OUR window or to EXTERNAL program
        bool any_our_window_focused = false;
        for (int i = 0; i < MAX_WINDOWS; i++) {
            if (g_windows[i].is_active && g_windows[i].has_focus) {
                any_our_window_focused = true;
                break;
            }
        }

        // EXCEPTION: If we switched to EXTERNAL program - NOT freezing
        if (!any_our_window_focused) {
            if (ENABLE_WINDOW_MANAGER_DEBUG) {
                std::cout << "🔓 [AUTO-FREEZE] Focus lost to external app - NOT freezing players" << std::endl;
            }
            return;
        }
    }

    // FOCUS_GAINED: Freezing all players except active window
    if (gained_focus) {
        int active_player_id = g_windows[focused_window_index].player_instance_id;

        // Resource courtesy: free the heavy full-res decoder of UNFOCUSED instances — UNLESS a
        // presentation/conference window is up. There the operator switches focus to drive the big
        // screen, so BOTH materials must stay instantly ready (no cold-start when cutting to them).
        bool keep_all_ready = IsPresentationWindowActive();

        // The newly-focused instance becomes the foreground one — restore its full-res decoder.
        if (active_player_id >= 0 && IsPlayerInstanceActive(active_player_id)) {
            SetInstanceBackgrounded(active_player_id, 0);
        }

        for (int i = 0; i < MAX_WINDOWS; i++) {
            if (g_windows[i].is_active && i != focused_window_index) {
                int player_id = g_windows[i].player_instance_id;

                if (player_id >= 0 && IsPlayerInstanceActive(player_id)) {
                    // Freeze only if player is playing
                    if (IsInstancePlaying(player_id)) {
                        PauseInstance(player_id);
                        if (ENABLE_WINDOW_MANAGER_DEBUG) {
                            std::cout << "❄️ [AUTO-FREEZE] Player " << player_id
                                      << " frozen (active: Player " << active_player_id << ")" << std::endl;
                        }
                    }
                    // Free its full-res decoder (keeps the cached still frame) unless presenting.
                    if (!keep_all_ready) {
                        SetInstanceBackgrounded(player_id, 1);
                    }
                }
            }
        }
    }
}

void HandleWindowEvents(SDL_Event* event) {
    if (event->type == SDL_WINDOWEVENT) {
        // Presentation window: keep the surface clean — hide the cursor while the pointer is over
        // the video (the Betacam hardware-protection principle: nothing but picture on the output),
        // restore it on leave. SDL cursor visibility is global, so this is safe (pointer is over
        // exactly one window at a time).
        if (g_presentation.active && g_presentation.window_id != 0 &&
            event->window.windowID == g_presentation.window_id) {
            if (event->window.event == SDL_WINDOWEVENT_ENTER)      SDL_ShowCursor(SDL_DISABLE);
            else if (event->window.event == SDL_WINDOWEVENT_LEAVE) SDL_ShowCursor(SDL_ENABLE);
            return;
        }

        FSTPWindow* window = GetWindowBySDLID(event->window.windowID);
        if (window == nullptr) {
            return;
        }

        switch (event->window.event) {
            case SDL_WINDOWEVENT_CLOSE:
                // Find window index and close it together with bound player
                for (int i = 0; i < MAX_WINDOWS; i++) {
                    if (&g_windows[i] == window) {
                        int player_id = g_windows[i].player_instance_id;
                        std::cout << "Window " << i << " close requested - stopping player " << player_id << std::endl;

                        // REMOVED: Don't mark is_closing yet - it blocks OSD during unthreading!
                        // g_windows[i].is_closing = true;

                        // First stop player, then destroy
                        if (player_id >= 0 && IsPlayerInstanceActive(player_id)) {
                            std::cout << "Stopping player " << player_id << " before destroying..." << std::endl;

                            // Stop playback
                            if (IsInstancePlaying(player_id)) {
                                StopInstance(player_id);
                                std::cout << "Player " << player_id << " stopped" << std::endl;
                            }

                            // CRITICAL: Destroy instance with OSD visible
                            // UnloadFile will show "unthreading" progress
                            DestroyPlayerInstance(player_id);
                        }

                        // NOW mark as closing and close window (AFTER player destroyed)
                        g_windows[i].is_closing = true;
                        FSTPCloseWindow(i);
                        break;
                    }
                }
                break;

            case SDL_WINDOWEVENT_FOCUS_GAINED:
                window->has_focus = true;
                // AUTO-FREEZE: When window gets focus, freeze all other players (if setting is enabled)
                for (int i = 0; i < MAX_WINDOWS; i++) {
                    if (&g_windows[i] == window) {
                        HandleAutoFreezeOnFocusChange(i, true);
                        break;
                    }
                }
#ifdef __APPLE__
                // Memory Locations are per-instance — refresh the open window so it
                // shows the newly focused player's marker set.
                RefreshMemoryLocationsWindowIfOpen();
#endif
                break;

            case SDL_WINDOWEVENT_FOCUS_LOST:
                window->has_focus = false;
                // If we lose focus to EXTERNAL program - NOT freezing (exception!)
                // Check will happen in HandleAutoFreezeOnFocusChange
                for (int i = 0; i < MAX_WINDOWS; i++) {
                    if (&g_windows[i] == window) {
                        HandleAutoFreezeOnFocusChange(i, false);
                        break;
                    }
                }
                break;

            case SDL_WINDOWEVENT_MINIMIZED:
                window->is_minimized = true;
                break;

            case SDL_WINDOWEVENT_RESTORED:
                window->is_minimized = false;
                break;
        }
    }
}

void RenderAllWindows() {
    // ============================================================
    // OPTIMIZATION FOR MULTIPLE INSTANCES:
    // Update video for each player INSIDE its rendering loop,
    // instead of sequentially for all players before rendering.
    // This eliminates serialization bottleneck when 2+ instances!
    // ============================================================

    // PROFILING: Thread-safe through atomic (protection from race when multiple instances)
    static std::atomic<uint64_t> total_update_video_us{0};
    static std::atomic<uint64_t> total_texture_update_us{0};
    static std::atomic<uint64_t> total_osd_render_us{0};
    static std::atomic<uint64_t> total_render_present_us{0};
    static std::atomic<int> perf_sample_count{0};

    // Hold the render lock for the whole pass so a concurrent CreateNewWindow / FSTPCloseWindow /
    // ShutdownWindowManager (event thread) cannot mutate g_windows or destroy SDL objects while
    // we iterate and present. Releases at function return. (g_presentation_mutex is taken later,
    // nested inside this — see the lock-ordering note at g_render_mutex.)
    std::lock_guard<std::mutex> _render_lock(g_render_mutex);

    auto t_start = std::chrono::high_resolution_clock::now();

    // REMOVED: UpdateAllVideoFrames() - each player is updated inside its rendering loop
    // UpdateAllVideoFrames();

    auto t_after_video = std::chrono::high_resolution_clock::now();

    // Diagnostics: how many windows are active and profiling
    static int window_count_report = 0;
    if (++window_count_report >= 60) {
        // Render timing statistics (disabled in production)
        // int active_window_count = 0;
        // for (int i = 0; i < MAX_WINDOWS; i++) {
        //     if (g_windows[i].is_active && !g_windows[i].is_minimized && g_windows[i].renderer) {
        //         active_window_count++;
        //     }
        // }

        // int samples = perf_sample_count.load();
        // if (samples > 0) {
        //     uint64_t avg_video = total_update_video_us.load() / samples;
        //     uint64_t avg_texture = total_texture_update_us.load() / samples;
        //     uint64_t avg_osd = total_osd_render_us.load() / samples;
        //     uint64_t avg_present = total_render_present_us.load() / samples;
        //     uint64_t total_avg = avg_video + avg_texture + avg_osd + avg_present;
        //
        //     std::cout << "⏱️  [RENDER] UpdateVideo: " << avg_video << "μs"
        //               << ", Texture: " << avg_texture << "μs"
        //               << ", OSD: " << avg_osd << "μs"
        //               << ", Present: " << avg_present << "μs"
        //               << " | TOTAL: " << total_avg << "μs"
        //               << " (" << active_window_count << " win)" << std::endl;
        // }

        window_count_report = 0;
        total_update_video_us.store(0);
        total_texture_update_us.store(0);
        total_osd_render_us.store(0);
        total_render_present_us.store(0);
        perf_sample_count.store(0);
    }

    total_update_video_us.fetch_add(std::chrono::duration_cast<std::chrono::microseconds>(t_after_video - t_start).count());


    // CPU OPTIMIZATION: Throttling for static screens (no file/loading)
    for (int i = 0; i < MAX_WINDOWS; i++) {
        // CRITICAL FIX: Skip windows that are being closed to avoid Metal command encoder crash
        if (g_windows[i].is_active && !g_windows[i].is_minimized && !g_windows[i].is_closing && g_windows[i].renderer) {
            SDL_Renderer* renderer = g_windows[i].renderer;
            int player_id = g_windows[i].player_instance_id;

            // CRITICAL: Take snapshot of is_closing state at start of render block
            // This ensures consistent state throughout rendering operations
            // If window starts closing mid-render, we still need to finish cleanly
            bool window_closing = g_windows[i].is_closing;
            if (window_closing) {
                continue;  // Skip this window entirely
            }

            // REMOVED: Throttling optimization that prevented OSD from showing immediately
            // macOS version doesn't have this check and renders every frame
            // if (player_id >= 0 && !ShouldRenderOSDForPlayer(player_id)) {
            //     continue;  // Skip entire rendering for this window
            // }

            // Update video for this player
            if (player_id >= 0) {
                UpdateVideoFrameForPlayer(player_id);
            }

            // While a Betacam dropout burst is showing, bypass the FPS-saving throttles below
            // (settled-pause skip + timestamp-skip) so the dropout re-applies and animates at
            // full render rate instead of the ~25fps adaptation. Until dropouts disappear.
            bool dropout_active = (player_id >= 0 && g_pixel_buffer_manager &&
                                   g_pixel_buffer_manager->HasPendingDropout(player_id));
            // Film grain animates at full render rate (~60fps) during playback: re-apply every
            // render frame. The settled-pause throttle below still applies, so a fully-still
            // pause stays cheap.
            bool grain_active = (player_id >= 0 && g_pixel_buffer_manager &&
                                 g_pixel_buffer_manager->IsFilmGrainActive());

            // SETTLED PAUSE THROTTLE: when frame is aligned, betacam stripe gone,
            // and zoom is not active — throttle full render cycle to ~5fps.
            // SDL events and menus are processed independently on all platforms.
            {
                FSTPZoomState* zs = GetZoomState(i);
                bool zoom_active = (zs && zs->enabled && zs->factor > 1.0f) || IsZoomPanningActive();
                bool settled = player_id >= 0 &&
                               g_windows[i].last_frame_aligned &&
                               g_windows[i].betacam_hold_frames == 0;

                if (settled && !zoom_active && !dropout_active) {
                    uint64_t now_ms = static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count());
                    if (now_ms - g_windows[i].last_settled_render_ms < 200) {
                        continue; // Nothing changed — skip Clear/Texture/OSD/Present
                    }
                    g_windows[i].last_settled_render_ms = now_ms;
                } else {
                    g_windows[i].last_settled_render_ms = 0; // Reset when leaving settled state
                }
            }

            // ============================================================
            // SINGLE RENDERING LOOP - ALL IN ONE PLACE
            // ============================================================

            // 1. Clear screen
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderClear(renderer);

            // 2. VIDEO TEXTURE
            auto t_before_texture = std::chrono::high_resolution_clock::now();

            // FIX: Don't render video texture in loading mode
            // to clear previous frame when opening new file
            bool is_loading = (player_id >= 0) ? IsPlayerLoading(player_id) : false;

            if (player_id >= 0 && g_pixel_buffer_manager && !is_loading) {
                FSTPBetacamEffect::PlaybackMetrics playback_metrics{};
                double actual_speed = GetInstanceActualSpeed(player_id);
                bool is_reverse_playback = IsInstanceReverse(player_id);
                playback_metrics.playback_rate = is_reverse_playback ? -std::fabs(actual_speed) : actual_speed;
                playback_metrics.is_reverse = is_reverse_playback;
                playback_metrics.position_seconds = GetInstancePosition(player_id);
                playback_metrics.duration_seconds = GetInstanceDuration(player_id);
                playback_metrics.timecode_offset_seconds = GetInstanceTimecodeOffset(player_id);
                playback_metrics.frame_rate = GetInstanceVideoFPS(player_id);

                double absPlaybackRate = std::fabs(playback_metrics.playback_rate);
                bool is_pure_pause = (absPlaybackRate < 0.05);

                // betacam_hold_frames: short buffer after IsFrameAligned() fires (stripe just disappeared)
                // Triggered by false→true transition of frame alignment, not by speed change.
                bool frame_aligned = is_pure_pause && player_id >= 0 && GetInstanceFrameAligned(player_id);
                if (frame_aligned && !g_windows[i].last_frame_aligned) {
                    // Alignment just happened — hold a few frames for visual smoothness
                    g_windows[i].betacam_hold_frames = 30;
                }
                if (!is_pure_pause) {
                    g_windows[i].betacam_hold_frames = 0;
                }
                g_windows[i].last_frame_aligned = frame_aligned;

                // Pass frame_aligned to betacam effect so it can render clean frame
                playback_metrics.frame_aligned = frame_aligned;
                g_pixel_buffer_manager->UpdatePlaybackMetrics(player_id, playback_metrics);

                const FSTPPixelBufferManager::PixelBuffer* pixel_buffer =
                    g_pixel_buffer_manager->GetPixelBuffer(player_id);

                if (pixel_buffer) {
                    int current_buf = g_windows[i].current_buffer_index;

                    // OPTIMIZATION: skip texture update if timestamp didn't change
                    bool need_update = true;
                    if (g_windows[i].texture_buffer_valid[current_buf] &&
                        g_windows[i].texture_buffer_timestamp[current_buf] == pixel_buffer->timestamp) {
                        need_update = false;
                        // Debug log disabled for performance
                    }
                    if (g_windows[i].betacam_hold_frames > 0) {
                        need_update = true;
                        g_windows[i].betacam_hold_frames--;
                    }

                    // BETACAM EFFECT: Always update at slow motion or shuttle speeds.
                    // During pure pause: force until IsFrameAligned fires, then hold 30 frames.
                    // Once frame_aligned and hold_frames == 0 — stripe gone, allow frame skip.
                    bool betacam_speed = (absPlaybackRate < 0.9 || absPlaybackRate > 1.1);
                    bool pause_settled = is_pure_pause && frame_aligned && g_windows[i].betacam_hold_frames == 0;
                    if (betacam_speed && !pause_settled) {
                        need_update = true;
                    }
                    // Dropout burst: force re-apply every render frame so it animates at full
                    // rate (CreateOrUpdateTexture re-runs the effect), bypassing the 25fps skip.
                    if (dropout_active) {
                        need_update = true;
                    }
                    // Film grain: re-apply every render frame so it shimmers at ~60fps (the
                    // grain pass re-rolls each call). Settled-still pause already skipped above.
                    if (grain_active) {
                        need_update = true;
                    }

                    bool is_new_frame = need_update || (pixel_buffer->frame_number != g_windows[i].last_rendered_frame);

                    SDL_Texture* new_texture = nullptr;
                    if (need_update) {
                        new_texture = g_pixel_buffer_manager->CreateOrUpdateTexture(
                            player_id, renderer, pixel_buffer, g_windows[i].texture_buffer[current_buf]);
                    } else {
                        // Use existing texture
                        new_texture = g_windows[i].texture_buffer[current_buf];
                    }

                    if (new_texture) {
                        if (g_windows[i].texture_buffer[current_buf] != new_texture) {
                            if (g_windows[i].texture_buffer[current_buf]) {
                                SDL_DestroyTexture(g_windows[i].texture_buffer[current_buf]);
                            }
                            g_windows[i].texture_buffer[current_buf] = new_texture;
                        }

                        g_windows[i].texture_buffer_valid[current_buf] = true;
                        g_windows[i].texture_buffer_width[current_buf] = pixel_buffer->width;
                        g_windows[i].texture_buffer_height[current_buf] = pixel_buffer->height;
                        g_windows[i].texture_buffer_timestamp[current_buf] = pixel_buffer->timestamp;

                        // Render video with aspect ratio within window
                        // Use logical window size (NOT renderer output size) to avoid HiDPI scaling issues
                        int win_w = 0, win_h = 0;
                        if (g_windows[i].window) {
                            SDL_GetWindowSize(g_windows[i].window, &win_w, &win_h);
                        }

                        // Compute display dimensions accounting for SAR (Sample Aspect Ratio)
                        // For anamorphic content: display_width = pixel_width × (sar_num / sar_den)
                        int display_width = pixel_buffer->width;
                        int display_height = pixel_buffer->height;

                        if (pixel_buffer->sar_num > 0 && pixel_buffer->sar_den > 0) {
                            // Apply SAR correction to width
                            display_width = static_cast<int>(pixel_buffer->width *
                                static_cast<double>(pixel_buffer->sar_num) /
                                static_cast<double>(pixel_buffer->sar_den));
                        }

                        SDL_Rect dst_rect = ComputeAspectFitRect(
                            display_width,   // Use display width (SAR-corrected)
                            display_height,  // Height stays the same
                            win_w,
                            win_h
                        );

                        if (g_pixel_buffer_manager) {
                            FSTPBetacamEffect::RenderContext render_ctx;
                            render_ctx.dest_rect = &dst_rect;
                            render_ctx.window_width = win_w;
                            render_ctx.window_height = win_h;
                            // Compute aspect ratio from display dimensions (SAR-corrected)
                            render_ctx.target_aspect_ratio = (display_height > 0)
                                ? static_cast<float>(display_width) / static_cast<float>(display_height)
                                : 1.0f;
                            render_ctx.frame_number = pixel_buffer->frame_number;
                            render_ctx.new_frame = is_new_frame;
                            g_pixel_buffer_manager->ApplyRenderJitter(player_id, render_ctx);
                        }

                        // Apply zoom if enabled for this window
                        FSTPZoomState* zoom_state = GetZoomState(i);
                        SDL_Rect src_rect_for_zoom = {0, 0, pixel_buffer->width, pixel_buffer->height};
                        SDL_Rect* src_rect_ptr = nullptr;

                        if (zoom_state && zoom_state->enabled && zoom_state->factor > 1.0f) {
                            // Calculate zoomed source rectangle for zooming
                            int src_w = (int)(pixel_buffer->width / zoom_state->factor);
                            int src_h = (int)(pixel_buffer->height / zoom_state->factor);
                            int src_x = (int)(pixel_buffer->width * zoom_state->center_x) - src_w / 2;
                            int src_y = (int)(pixel_buffer->height * zoom_state->center_y) - src_h / 2;

                            // Clamp to texture bounds
                            if (src_x < 0) src_x = 0;
                            if (src_y < 0) src_y = 0;
                            if (src_x + src_w > pixel_buffer->width) src_x = pixel_buffer->width - src_w;
                            if (src_y + src_h > pixel_buffer->height) src_y = pixel_buffer->height - src_h;

                            src_rect_for_zoom = {src_x, src_y, src_w, src_h};
                            src_rect_ptr = &src_rect_for_zoom;
                        }

                        // Try HSync loss effect rendering first (only when no zoom active)
                        bool hsync_rendered = false;
                        if (g_pixel_buffer_manager && !src_rect_ptr) {
                            // HSync effect only works with full frame (no zoom)
                            hsync_rendered = g_pixel_buffer_manager->RenderWithHsync(
                                player_id, renderer, new_texture,
                                pixel_buffer->width, pixel_buffer->height, dst_rect
                            );
                        }

                        // Fallback to normal rendering if hsync was not applied
                        if (!hsync_rendered) {
                            SDL_RenderCopy(renderer, new_texture, src_rect_ptr, &dst_rect);
                        }

                        if (is_new_frame) {
                            g_windows[i].last_rendered_frame = pixel_buffer->frame_number;
                        }

                        // Render zoom thumbnail if enabled
                        if (zoom_state && zoom_state->enabled && zoom_state->show_thumbnail && zoom_state->factor > 1.0f) {
                            // Thumbnail size (15% of window width, max 180px)
                            int thumb_w = std::min(180, static_cast<int>(win_w * 0.15f));
                            // Use display dimensions (SAR-corrected) for correct aspect ratio
                            int thumb_h = static_cast<int>(thumb_w * ((float)display_height / (float)display_width));

                            // Thumbnail position (top-right corner with padding)
                            int padding = 10;
                            SDL_Rect thumb_dst = {
                                win_w - thumb_w - padding,
                                padding,
                                thumb_w,
                                thumb_h
                            };

                            // Draw full image as thumbnail
                            SDL_RenderCopy(renderer, new_texture, nullptr, &thumb_dst);

                            // Draw white border around thumbnail
                            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
                            SDL_RenderDrawRect(renderer, &thumb_dst);

                            // Calculate zoom area rectangle on thumbnail
                            SDL_Rect zoom_rect;
                            zoom_rect.w = static_cast<int>(thumb_w / zoom_state->factor);
                            zoom_rect.h = static_cast<int>(thumb_h / zoom_state->factor);
                            zoom_rect.x = thumb_dst.x + static_cast<int>(zoom_state->center_x * thumb_w - zoom_rect.w / 2);
                            zoom_rect.y = thumb_dst.y + static_cast<int>(zoom_state->center_y * thumb_h - zoom_rect.h / 2);

                            // Constrain zoom area to thumbnail bounds
                            if (zoom_rect.x < thumb_dst.x) zoom_rect.x = thumb_dst.x;
                            if (zoom_rect.y < thumb_dst.y) zoom_rect.y = thumb_dst.y;
                            if (zoom_rect.x + zoom_rect.w > thumb_dst.x + thumb_dst.w)
                                zoom_rect.x = thumb_dst.x + thumb_dst.w - zoom_rect.w;
                            if (zoom_rect.y + zoom_rect.h > thumb_dst.y + thumb_dst.h)
                                zoom_rect.y = thumb_dst.y + thumb_dst.h - zoom_rect.h;

                            // Draw red rectangle showing zoomed area
                            SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255);
                            SDL_RenderDrawRect(renderer, &zoom_rect);
                        }
                    }
                }

                // Update deprecated fields for compatibility
                int current_buf = g_windows[i].current_buffer_index;
                g_windows[i].video_texture = g_windows[i].texture_buffer[current_buf];
                g_windows[i].video_texture_valid = g_windows[i].texture_buffer_valid[current_buf];
                g_windows[i].video_width = g_windows[i].texture_buffer_width[current_buf];
                g_windows[i].video_height = g_windows[i].texture_buffer_height[current_buf];
                g_windows[i].video_timestamp = g_windows[i].texture_buffer_timestamp[current_buf];
            }

            auto t_after_texture = std::chrono::high_resolution_clock::now();
            total_texture_update_us.fetch_add(std::chrono::duration_cast<std::chrono::microseconds>(t_after_texture - t_before_texture).count());

            // 3. OSD ELEMENTS (all in one place)
            auto t_before_osd = std::chrono::high_resolution_clock::now();
            if (player_id >= 0) {
                RenderOSDForPlayer(renderer, player_id);
            }
            auto t_after_osd = std::chrono::high_resolution_clock::now();
            total_osd_render_us.fetch_add(std::chrono::duration_cast<std::chrono::microseconds>(t_after_osd - t_before_osd).count());

            // 3.5. MENU BAR removed - using GTK context menu instead

            // 3.55. SUBTITLES (first extension) — cross-platform SDL overlay,
            //       synced to the player position. No-op when disabled / no track.
            if (player_id >= 0 && FSTPSubtitles_IsEnabled() && IsPlayerInstanceActive(player_id)) {
                int sub_w = 0, sub_h = 0;
                SDL_GetRendererOutputSize(renderer, &sub_w, &sub_h);
                FSTPSubtitles_Render(renderer, player_id, GetInstancePosition(player_id), sub_w, sub_h);
            }

            // 3.57. LUA EXTENSIONS — per-frame draw callbacks (script overlays).
            //        Cheap no-op when no lua extension is loaded.
            {
                int lua_w = 0, lua_h = 0;
                SDL_GetRendererOutputSize(renderer, &lua_w, &lua_h);
                double lua_t = (player_id >= 0 && IsPlayerInstanceActive(player_id))
                                   ? GetInstancePosition(player_id) : 0.0;
                FSTPLua_Render(renderer, player_id, lua_t, lua_w, lua_h);
            }

            // 3.6. WELCOME OVERLAY (first-run onboarding) — cross-platform SDL,
            //      drawn on top of everything. No-op unless active.
            if (FSTPWelcome_IsActive()) {
                int welcome_w = 0, welcome_h = 0;
                SDL_GetRendererOutputSize(renderer, &welcome_w, &welcome_h);
                FSTPWelcome_Render(renderer, welcome_w, welcome_h);
            }

            // 4. Final Present
            auto t_before_present = std::chrono::high_resolution_clock::now();

            // CRITICAL: Double-check window is not closing before SDL_RenderPresent
            // Race condition: window might start closing between loop check and here
            if (!g_windows[i].is_closing && g_windows[i].is_active) {
                SDL_RenderPresent(renderer);
            }

            auto t_after_present = std::chrono::high_resolution_clock::now();
            total_render_present_us.fetch_add(std::chrono::duration_cast<std::chrono::microseconds>(t_after_present - t_before_present).count());

            perf_sample_count.fetch_add(1);
            // ============================================================
        }
    }

    // ========================================================================
    // PRESENTATION MODE: Separate YUV renderer — CLEAN output only (no OSD / indicators / cursor).
    // ========================================================================
    // Hold the presentation lock for the whole block (active-check + render) so the main thread
    // can't destroy the renderer/window mid-blit (Shift+P close). This is the last thing in
    // RenderAllWindows, so the guard releases at function return.
    std::lock_guard<std::mutex> _pres_lock(g_presentation_mutex);
    // Source selection: in follow-focus mode, whichever player window has focus drives the output
    // (hold the last one when focus leaves to another app); in pinned mode, a fixed player drives it.
    if (g_presentation.active && g_presentation.enabled) {
        if (g_presentation.follow_focus) {
            int focused = GetActivePlayerID();
            if (focused >= 0) g_presentation.linked_player_id = focused;
        } else {
            g_presentation.linked_player_id = g_presentation.pinned_player;
        }
    }

    if (g_presentation.active && g_presentation.enabled && g_presentation.linked_player_id >= 0) {
        int player_id = g_presentation.linked_player_id;

        if (!IsPlayerInstanceActive(player_id)) {
            // Render thread must NOT call SDL_DestroyWindow (requires main thread).
            // Just detach and go black; main thread will destroy via ClosePresentationWindow().
            if (g_presentation.renderer) {
                SDL_SetRenderDrawColor(g_presentation.renderer, 0, 0, 0, 255);
                SDL_RenderClear(g_presentation.renderer);
                SDL_RenderPresent(g_presentation.renderer);
            }
            g_presentation.active           = false;
            g_presentation.linked_player_id = -1;
        } else if (g_presentation.renderer) {
            const FSTPPixelBufferManager::PixelBuffer* pb =
                g_pixel_buffer_manager ? g_pixel_buffer_manager->GetPixelBuffer(player_id) : nullptr;

            if (!pb || !pb->is_valid || !pb->av_frame || pb->width <= 0) {
                // No frame yet — black screen
                SDL_SetRenderDrawColor(g_presentation.renderer, 0, 0, 0, 255);
                SDL_RenderClear(g_presentation.renderer);
                SDL_RenderPresent(g_presentation.renderer);
            } else {
                SDL_Renderer* renderer = g_presentation.renderer;
                AVFrame* f = pb->av_frame.get();

                // SDL format from AVFrame format
                bool is_nv12 = (f->format == AV_PIX_FMT_NV12 || f->format == AV_PIX_FMT_NV21);
                Uint32 sdl_fmt = is_nv12 ? SDL_PIXELFORMAT_NV12 : SDL_PIXELFORMAT_IYUV;

                // (Re)create YUV renderer when resolution or format changes
                bool needs_yuv = !g_presentation.yuv_renderer ||
                                 g_presentation.yuv_w   != pb->width  ||
                                 g_presentation.yuv_h   != pb->height ||
                                 g_presentation.yuv_fmt != sdl_fmt;
                if (needs_yuv) {
                    if (g_presentation.yuv_renderer) {
                        g_presentation.yuv_renderer->Cleanup();
                        delete g_presentation.yuv_renderer;
                        g_presentation.yuv_renderer = nullptr;
                    }
                    g_presentation.yuv_renderer = new FSTPYUVRenderer();
                    if (!g_presentation.yuv_renderer->Initialize(renderer, pb->width, pb->height, sdl_fmt)) {
                        std::cerr << "❌ [PRESENTATION] YUV renderer init failed" << std::endl;
                        delete g_presentation.yuv_renderer;
                        g_presentation.yuv_renderer = nullptr;
                    } else {
                        g_presentation.yuv_w   = pb->width;
                        g_presentation.yuv_h   = pb->height;
                        g_presentation.yuv_fmt = sdl_fmt;
                        std::cout << "📺 [PRESENTATION] YUV renderer ready: "
                                  << pb->width << "x" << pb->height
                                  << (is_nv12 ? " NV12" : " IYUV") << std::endl;
                    }
                }
                if (!g_presentation.yuv_renderer) goto pres_done;

                // Copy YUV planes to scratch and apply Betacam pixel effects.
                // new_frame=false: re-apply current state without advancing it —
                // state was already advanced by the main window this frame.
                {
                    static std::vector<uint8_t> scratch_y, scratch_uv, scratch_v;
                    int y_pitch  = f->linesize[0];
                    int uv_pitch = f->linesize[1];
                    int v_pitch  = f->linesize[2];
                    int chroma_h = pb->height / 2;

                    scratch_y.resize(static_cast<size_t>(y_pitch)  * pb->height);
                    scratch_uv.resize(static_cast<size_t>(uv_pitch) * chroma_h);
                    if (!is_nv12)
                        scratch_v.resize(static_cast<size_t>(v_pitch) * chroma_h);

                    for (int row = 0; row < pb->height; ++row)
                        std::memcpy(scratch_y.data()  + row * y_pitch,
                                    f->data[0] + row * f->linesize[0], y_pitch);
                    for (int row = 0; row < chroma_h; ++row)
                        std::memcpy(scratch_uv.data() + row * uv_pitch,
                                    f->data[1] + row * f->linesize[1], uv_pitch);
                    if (!is_nv12)
                        for (int row = 0; row < chroma_h; ++row)
                            std::memcpy(scratch_v.data() + row * v_pitch,
                                        f->data[2] + row * f->linesize[2], v_pitch);

                    if (g_pixel_buffer_manager) {
                        FSTPBetacamEffect::FrameContext ctx;
                        ctx.pixel_format      = sdl_fmt;
                        ctx.width             = pb->width;
                        ctx.height            = pb->height;
                        ctx.frame_number      = pb->frame_number;
                        ctx.new_frame         = false; // mirror: don't advance state
                        ctx.planes[0]         = scratch_y.data();
                        ctx.linesize[0]       = y_pitch;
                        ctx.planes[1]         = scratch_uv.data();
                        ctx.linesize[1]       = uv_pitch;
                        ctx.planes[2]         = is_nv12 ? nullptr : scratch_v.data();
                        ctx.linesize[2]       = is_nv12 ? 0 : v_pitch;
                        ctx.source_frame      = f;
                        ctx.prev_source_frame = pb->prev_frame ? pb->prev_frame.get() : nullptr;
                        ctx.next_source_frame = pb->next_frame ? pb->next_frame.get() : nullptr;
                        // Mirror the always-on baseline Betacam elements the main window applies
                        // (these were missing on the presentation output → "not all elements
                        // transferred"): analog smear + soft L/R edge fade.
                        ctx.smear     = g_pixel_buffer_manager->IsSmearEnabled();
                        ctx.edge_fade = g_pixel_buffer_manager->IsEdgeFadeEnabled();
                        bool fx = g_pixel_buffer_manager->ApplyPixelFX(player_id, ctx);
                        if (!fx) {
                            // No per-speed effect this frame (e.g. 1×) — apply smear + soft border
                            // directly, exactly as the main window does, so the presentation screen
                            // carries the full Betacam look.
                            g_pixel_buffer_manager->ApplyBaselineSmearEdgeFade(
                                scratch_y.data(), y_pitch,
                                scratch_uv.data(), is_nv12 ? nullptr : scratch_v.data(),
                                uv_pitch, is_nv12 ? 0 : v_pitch,
                                pb->width, pb->height, sdl_fmt);
                        }
                    }

                    YUVPlanes planes;
                    planes.width       = pb->width;
                    planes.height      = pb->height;
                    planes.y_plane     = scratch_y.data();
                    planes.y_pitch     = y_pitch;
                    planes.u_plane     = scratch_uv.data();
                    planes.u_pitch     = uv_pitch;
                    planes.v_plane     = is_nv12 ? nullptr : scratch_v.data();
                    planes.v_pitch     = is_nv12 ? 0 : v_pitch;
                    planes.is_full_range = (f->color_range == AVCOL_RANGE_JPEG);
                    planes.format      = f->format;
                    g_presentation.yuv_renderer->UpdateYUVTexture(planes);
                }

                // Render with Betacam geometry effects (jitter, HSync).
                // new_frame=false: mirror current state without advancing.
                {
                    SDL_Texture* tex = g_presentation.yuv_renderer->GetTexture();
                    int win_w, win_h;
                    SDL_GetWindowSize(g_presentation.window, &win_w, &win_h);

                    int disp_w = pb->width, disp_h = pb->height;
                    if (pb->sar_num > 0 && pb->sar_den > 0)
                        disp_w = static_cast<int>(pb->width *
                            static_cast<double>(pb->sar_num) / pb->sar_den);

                    SDL_Rect dst = ComputeAspectFitRect(disp_w, disp_h, win_w, win_h);

                    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
                    SDL_RenderClear(renderer);

                    bool hsync_applied = false;
                    if (g_pixel_buffer_manager) {
                        FSTPBetacamEffect::RenderContext rctx;
                        rctx.dest_rect           = &dst;
                        rctx.window_width        = win_w;
                        rctx.window_height       = win_h;
                        rctx.target_aspect_ratio = disp_h > 0
                            ? static_cast<float>(disp_w) / disp_h : 1.0f;
                        rctx.frame_number        = pb->frame_number;
                        rctx.new_frame           = false; // mirror: don't advance state
                        g_pixel_buffer_manager->ApplyRenderJitter(player_id, rctx);
                        hsync_applied = g_pixel_buffer_manager->RenderWithHsync(
                            player_id, renderer, tex, pb->width, pb->height, dst);
                    }
                    if (!hsync_applied)
                        SDL_RenderCopy(renderer, tex, nullptr, &dst);

                    SDL_RenderPresent(renderer);
                    g_presentation.last_timestamp = pb->timestamp;
                }
            }
            pres_done:;
        }
    }
}

int GetActiveWindowCount() {
    int count = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (g_windows[i].is_active) {
            count++;
        }
    }
    return count;
}

// Helper functions for updating OSD of specific window
void UpdateWindowOSD(int window_index, double current_time, double total_duration, bool is_playing, double speed, bool is_reverse) {
    if (window_index < 0 || window_index >= MAX_WINDOWS || !g_windows[window_index].is_active) {
        return;
    }

    int player_id = g_windows[window_index].player_instance_id;
    UpdateOSDPosition(player_id, current_time, total_duration);
    UpdateOSDPlayState(player_id, is_playing, false, false);
    UpdateOSDSpeed(player_id, speed, is_reverse);

    // CRITICAL FIX: Set correct display mode based on player state
    // Check if player has loaded file (duration > 0) or is loading
    bool is_loading = (player_id >= 0) ? IsPlayerLoading(player_id) : false;
    bool has_file = (total_duration > 0.0);

    // CRITICAL: Don't update OSD if window or player instance is being destroyed
    // This prevents "no file" flash when closing window or exiting application
    if (!g_windows[window_index].is_active || g_windows[window_index].is_closing) {
        return;  // Window is closing, don't update OSD
    }

    if (player_id >= 0 && !IsPlayerInstanceActive(player_id)) {
        return;  // Player instance destroyed, don't update OSD
    }

    if (is_loading) {
        // Keep LOADING mode (don't override it)
        // Mode is set by LoadFileIntoPlayerInstance
    } else if (!has_file) {
        // No file loaded - show NO_FILE screen
        UpdateOSDDisplayMode(player_id, OSD_MODE_NO_FILE);
    } else {
        // File loaded - normal mode
        UpdateOSDDisplayMode(player_id, OSD_MODE_NORMAL);
    }

    // Update frame number for OSD if frame mode is enabled
    FSTPAudioModuleWrapper* audio_module = GetInstanceAudioModule(player_id);
    if (audio_module) {
        double fps = GetInstanceVideoFPS(player_id);
        int frame_number = FSTPAudioModule_API::GetCurrentFrame(audio_module, fps);
        UpdateOSDFrameNumber(player_id, frame_number);
    }
}

void UpdateWindowOSDLoading(int window_index, bool is_loading, int progress) {
    if (window_index < 0 || window_index >= MAX_WINDOWS || !g_windows[window_index].is_active) {
        return;
    }
    
    int player_id = g_windows[window_index].player_instance_id;
    SetPlayerLoadingState(player_id, is_loading);
    if (is_loading) {
        SetPlayerLoadingProgress(player_id, progress);
        UpdateOSDDisplayMode(player_id, OSD_MODE_LOADING);
    } else {
        UpdateOSDDisplayMode(player_id, OSD_MODE_NORMAL);
    }
}

FSTPWindow* GetMainWindow() {
    return GetWindowByIndex(0);
}

FSTPWindow* FSTPGetActiveWindow() {
    // Find window with focus
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (g_windows[i].is_active && g_windows[i].has_focus) {
            return &g_windows[i];
        }
    }

    // If no focused window, return main
    return GetMainWindow();
}

int GetActivePlayerID() {
    FSTPWindow* active_window = FSTPGetActiveWindow();
    if (active_window) {
        return active_window->player_instance_id;
    }
    return 0; // Default player 0
}

void RestoreFocusToMainWindow() {
    FSTPWindow* main_window = GetMainWindow();
    if (main_window && main_window->window) {
        SDL_RaiseWindow(main_window->window);
        SDL_SetWindowInputFocus(main_window->window);
        std::cout << "🎯 Focus restored to main SDL window" << std::endl;
    }
}

// === OSD management for windows ===

// TODO: Implement OSD management for windows through original OSD system
void UpdateWindowOSD(int window_index, double current_time, bool is_playing) {
    // Stub - data passed through original OSD system
    (void)window_index; (void)current_time; (void)is_playing;
}

void SetWindowOSDMode(int window_index, int osd_mode) {
    // Stub - mode set through original OSD system
    (void)window_index; (void)osd_mode;
}

void UpdateWindowLoadingProgress(int window_index, int progress) {
    // Stub - progress updated through original OSD system
    (void)window_index; (void)progress;
}

    // === Texture interface for video ===

SDL_Renderer* GetWindowRenderer(int window_index) {
    if (window_index < 0 || window_index >= MAX_WINDOWS || !g_windows[window_index].is_active) {
        return nullptr;
    }
    return g_windows[window_index].renderer;
}

// === NEW SAFE FUNCTIONS FOR PIXEL DATA ===

// ZERO-COPY method - pass AVFrame directly
// Optional: prev_frame (N-1) and next_frame (N+1) for Betacam slow-motion compositing
void SubmitAVFrame(int player_id, std::shared_ptr<AVFrame> av_frame,
                  double timestamp, int frame_number,
                  std::shared_ptr<AVFrame> prev_frame,
                  std::shared_ptr<AVFrame> next_frame) {
    if (g_pixel_buffer_manager) {
        g_pixel_buffer_manager->SubmitAVFrame(player_id, av_frame, timestamp, frame_number,
                                               prev_frame, next_frame);
    }
}

// Legacy method with copying
void SubmitPixelData(int player_id, const uint8_t* pixel_data, int width, int height,
                    unsigned int format, double timestamp, int frame_number) {
    if (g_pixel_buffer_manager) {
        g_pixel_buffer_manager->SubmitPixelData(player_id, pixel_data, width, height,
                                               format, timestamp, frame_number);
    }
}

FSTPPixelBufferManager* GetPixelBufferManager() {
    return g_pixel_buffer_manager.get();
}

// Simplified export for decoders: update player color metadata
extern "C" void FSTP_UpdatePlayerColorMetadata(int player_id, int colorspace, int color_range, int color_primaries, int color_trc) {
    if (!g_pixel_buffer_manager) return;
    FSTPPixelBufferManager::ColorMetadata meta;
    meta.colorspace = colorspace;
    meta.color_range = color_range;
    meta.color_primaries = color_primaries;
    meta.color_trc = color_trc;
    g_pixel_buffer_manager->UpdateColorMetadata(player_id, meta);
}

extern "C" void SetBetacamEffectEnabled(int enabled) {
    if (!g_pixel_buffer_manager) {
        return;
    }

    // Enable visual Betacam effect
    g_pixel_buffer_manager->SetBetacamEffectEnabled(enabled != 0);
}

// Update window title with instance number and filename
void UpdateWindowTitle(int window_index, int instance_id, const char* filename) {
    if (window_index < 0 || window_index >= MAX_WINDOWS || !g_windows[window_index].is_active) {
        return;
    }

    if (!filename || filename[0] == '\0') {
        // If no file, show only instance number
        snprintf(g_windows[window_index].window_title, sizeof(g_windows[window_index].window_title),
                 "TapeXPlayer - Instance #%d", instance_id + 1);
    } else {
        // Extract only filename from path
        const char* file_name_only = strrchr(filename, '/');
        if (!file_name_only) {
            file_name_only = strrchr(filename, '\\');
        }
        file_name_only = file_name_only ? file_name_only + 1 : filename;

        // Form title: "TapeXPlayer - Instance #X - filename.mov"
        snprintf(g_windows[window_index].window_title, sizeof(g_windows[window_index].window_title),
                 "TapeXPlayer - Instance #%d - %s", instance_id + 1, file_name_only);
    }

        // Update SDL window title
    if (g_windows[window_index].window) {
#ifdef __APPLE__
        // On macOS - ONLY in main thread (requirement)
        SDL_Window* window = g_windows[window_index].window;
        std::string title_copy = g_windows[window_index].window_title;

        dispatch_async(dispatch_get_main_queue(), ^{
            SDL_SetWindowTitle(window, title_copy.c_str());
        });
#else
        // On Linux/Windows - can update directly
        SDL_SetWindowTitle(g_windows[window_index].window, g_windows[window_index].window_title);
#endif
    }
}

// ============================================================================
// PRESENTATION MODE API
// ============================================================================

bool CreatePresentationWindow(int player_id, int output_mode, int display_index) {
    if (g_presentation.active) {
        ClosePresentationWindow();
    }

    int display_count = SDL_GetNumVideoDisplays();

    // Decide the actual target. External mode needs a second display; if none is connected we
    // gracefully fall back to a separate window instead of erroring out (so single-screen
    // presenters / conferences with screen-share still work).
    bool windowed = (output_mode == 1) || (display_count <= 1);

    int    target_display = -1;
    int    win_x, win_y, win_w, win_h;
    Uint32 win_flags;

    if (windowed) {
        // Separate, movable/resizable window centred on the main display. Drag it to any screen.
        target_display = -1;
        win_x = SDL_WINDOWPOS_CENTERED;
        win_y = SDL_WINDOWPOS_CENTERED;
        win_w = 1280;
        win_h = 720;
        // No SDL_WINDOW_ALLOW_HIGHDPI — match the main player windows. With HIGHDPI the Metal
        // drawable is 2× the window points on Retina, but the video is sized from SDL_GetWindowSize
        // (points), so it would render into a fraction of the drawable and look soft/low-res.
        win_flags = SDL_WINDOW_RESIZABLE;
    } else {
        target_display = (display_index < 0 || display_index >= display_count) ? 1 : display_index;
        SDL_Rect db;
        SDL_GetDisplayBounds(target_display, &db);
        win_x = db.x; win_y = db.y; win_w = db.w; win_h = db.h;
        // Borderless covering the external display (no fullscreen = no Mission Control Space).
        win_flags = SDL_WINDOW_BORDERLESS;
    }

    g_presentation.window = SDL_CreateWindow(
        "TapeXPlayer — Presentation",
        win_x, win_y, win_w, win_h, win_flags);

    if (!g_presentation.window) {
        std::cerr << "❌ [PRESENTATION] Failed to create window: " << SDL_GetError() << std::endl;
        return false;
    }

    SDL_RaiseWindow(g_presentation.window);

    // Create renderer on main thread — REQUIRED for Metal (CAMetalLayer must be
    // set up on main thread, same as all other windows in this app).
    // The render thread will USE this renderer, which Metal supports.
    // VSync enabled (same as main windows): SDL_RenderSetVSync sets
    // CAMetalLayer.displaySyncEnabled=YES — non-blocking, prevents tearing.
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
    g_presentation.renderer = CreateRendererWithVSync(
        g_presentation.window, -1, SDL_RENDERER_ACCELERATED);
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, nullptr);

    if (!g_presentation.renderer) {
        std::cerr << "❌ [PRESENTATION] Failed to create renderer: " << SDL_GetError() << std::endl;
        SDL_DestroyWindow(g_presentation.window);
        g_presentation.window = nullptr;
        return false;
    }

    SDL_RendererInfo info;
    SDL_GetRendererInfo(g_presentation.renderer, &info);
    std::cout << "✅ [PRESENTATION] Renderer created on main thread: " << info.name << std::endl;

    // Output target + focus behaviour (focus/pin read from Settings).
    g_presentation.follow_focus  = GetPresentationFollowFocus() != 0;
    g_presentation.pinned_player  = GetPresentationPinnedPlayer();
    g_presentation.output_mode    = windowed ? 1 : 0;
    g_presentation.display_index  = target_display;
    g_presentation.windowed       = windowed;
    g_presentation.window_id       = SDL_GetWindowID(g_presentation.window);

    // active must be set AFTER renderer is ready — render thread checks active first.
    // Initial linked player: explicit arg wins; otherwise follow-focus starts unbound (-1) and
    // pinned starts on the pinned player.
    g_presentation.linked_player_id =
        (player_id >= 0) ? player_id
                         : (g_presentation.follow_focus ? -1 : g_presentation.pinned_player);
    g_presentation.pending_renderer = false;
    g_presentation.last_timestamp = -1.0;
    g_presentation.enabled = true;
    g_presentation.active = true;  // SET LAST — render thread starts using from here

    std::cout << "✅ [PRESENTATION] "
              << (windowed ? "Windowed output" : ("External display " + std::to_string(target_display)))
              << " ready for player " << player_id
              << (g_presentation.follow_focus ? " (follow focus)" : " (pinned)") << std::endl;

    return true;
}

void ClosePresentationWindow() {
    // Serialise against the render thread's presentation block (see g_presentation_mutex): wait until
    // it is not mid-blit before destroying the renderer/window/yuv_renderer.
    std::lock_guard<std::mutex> _pres_lock(g_presentation_mutex);

    // Allow call even when active=false (render thread may have cleared it already)
    if (!g_presentation.active && !g_presentation.renderer && !g_presentation.window)
        return;

    // YUV renderer doesn't need main thread
    if (g_presentation.yuv_renderer) {
        g_presentation.yuv_renderer->Cleanup();
        delete g_presentation.yuv_renderer;
        g_presentation.yuv_renderer = nullptr;
        g_presentation.yuv_w   = 0;
        g_presentation.yuv_h   = 0;
        g_presentation.yuv_fmt = 0;
    }

    // Set active=false BEFORE SDL cleanup so the render thread
    // stops using these resources immediately (no race)
    g_presentation.active           = false;
    g_presentation.linked_player_id = -1;
    g_presentation.last_timestamp   = -1.0;
    g_presentation.pending_renderer = false;
    g_presentation.window_id        = 0;
    // Restore the cursor in case it was hidden while the pointer was over the presentation window
    // when it closed (no LEAVE event fires on destroy).
    SDL_ShowCursor(SDL_ENABLE);

    // Capture pointers — ownership transfers to the cleanup block
    SDL_Renderer* r = g_presentation.renderer;
    SDL_Window*   w = g_presentation.window;
    g_presentation.renderer = nullptr;
    g_presentation.window   = nullptr;

    // ClosePresentationWindow() must always be called from the main thread.
    // (menu callbacks, StopAutonomousRendering — all main thread)
    // Render thread only sets active=false; it never calls this function.
    if (r) SDL_DestroyRenderer(r);
    if (w) SDL_DestroyWindow(w);
    std::cout << "✅ [PRESENTATION] Window closed" << std::endl;
}

bool IsPresentationWindowActive() {
    return g_presentation.active;
}

void SetPresentationModeEnabled(bool enabled) {
    g_presentation.enabled = enabled;
    std::cout << (enabled ? "✅" : "⚠️") << " [PRESENTATION] Mode "
              << (enabled ? "ENABLED" : "DISABLED") << std::endl;
}

bool IsPresentationModeEnabled() {
    return g_presentation.enabled;
}

void SetPresentationFollow(bool follow_focus, int pinned_player) {
    g_presentation.follow_focus  = follow_focus;
    g_presentation.pinned_player = pinned_player;
    if (!follow_focus) g_presentation.linked_player_id = pinned_player;
}

// ---- C bridge for the Settings UI -------------------------------------------------------------
extern "C" {

int FSTP_GetPresentationDisplayCount(void) {
    int n = SDL_GetNumVideoDisplays();
    return (n < 0) ? 0 : n;
}

const char* FSTP_GetPresentationDisplayName(int idx) {
    const char* name = SDL_GetDisplayName(idx);
    return name ? name : "Display";
}

void FSTP_ReapplyPresentationIfActive(void) {
    // Always refresh the follow/pin behaviour so the next activation honours the latest Settings.
    g_presentation.follow_focus  = GetPresentationFollowFocus() != 0;
    g_presentation.pinned_player = GetPresentationPinnedPlayer();
    if (!g_presentation.active) return;

    // Active → rebuild the output target from current Settings (display / output mode), keeping the
    // same linked player so the picture continues where possible. Main thread only (Settings save).
    int player = g_presentation.linked_player_id;
    CreatePresentationWindow(player, GetPresentationOutputMode(), GetPresentationDisplayIndex());
}

} // extern "C"
