#include "FSTPWindowManager.h"
#include "FSTPOSDSystem.h"
#include "FSTPOSDInstance.h"
#include "FSTPPixelBufferManager.h"
#include "FSTPBetacamEffect.h"
#include "FSTPSettings.h"
#include "FSTPZoom.h"
#include "FSTPKeyboard.h"
#include "../FSTPPlayerModule/FSTPPlayerManager.h"
#include "../FSTPAudioModule/FSTPAudioModule_API.h"
#include "../FSTPAudioModule/FSTPAudioModule_wrapper.h"
#include <iostream>
#include <cstring>
#include <mutex>
#include <chrono>
#include <thread>
#include <atomic>
#include <cmath>

#ifdef __APPLE__
#include <dispatch/dispatch.h>
#endif

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
            CloseWindow(i);
        }
    }

    g_window_manager_initialized = false;
    std::cout << "Window manager shutdown complete" << std::endl;
}

int CreateNewWindow(const char* title, int width, int height) {
    if (!g_window_manager_initialized) {
        std::cerr << "Window manager not initialized" << std::endl;
        return -1;
    }

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

void CloseWindow(int window_index) {
    if (window_index < 0 || window_index >= MAX_WINDOWS || !g_windows[window_index].is_active) {
        return;
    }

    std::cout << "Closing window " << window_index << " (bound to player " << g_windows[window_index].player_instance_id << ")" << std::endl;

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
                }
            }
        }
    }
}

void HandleWindowEvents(SDL_Event* event) {
    if (event->type == SDL_WINDOWEVENT) {
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

                        // Mark window as closing to prevent OSD updates during cleanup
                        g_windows[i].is_closing = true;

                        // First stop player, then destroy
                        if (player_id >= 0 && IsPlayerInstanceActive(player_id)) {
                            std::cout << "Stopping player " << player_id << " before destroying..." << std::endl;

                            // Stop playback
                            if (IsInstancePlaying(player_id)) {
                                StopInstance(player_id);
                                std::cout << "Player " << player_id << " stopped" << std::endl;
                            }

                            // Now destroy instance
                            DestroyPlayerInstance(player_id);
                        }

                        // Close window
                        CloseWindow(i);
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
        if (g_windows[i].is_active && !g_windows[i].is_minimized && g_windows[i].renderer) {
            SDL_Renderer* renderer = g_windows[i].renderer;
            int player_id = g_windows[i].player_instance_id;

            // REMOVED: Throttling optimization that prevented OSD from showing immediately
            // macOS version doesn't have this check and renders every frame
            // if (player_id >= 0 && !ShouldRenderOSDForPlayer(player_id)) {
            //     continue;  // Skip entire rendering for this window
            // }

            // Update video for this player
            if (player_id >= 0) {
                UpdateVideoFrameForPlayer(player_id);
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
                playback_metrics.frame_rate = GetInstanceVideoFPS(player_id);
                g_pixel_buffer_manager->UpdatePlaybackMetrics(player_id, playback_metrics);

                const double holdSpeedThreshold = 0.05;
                double absPlaybackRate = std::fabs(playback_metrics.playback_rate);
                int holdFramesTarget = 0;
                if (playback_metrics.frame_rate > 1.0) {
                    holdFramesTarget = static_cast<int>(std::round(playback_metrics.frame_rate * 0.5));
                } else {
                    holdFramesTarget = 30;
                }
                holdFramesTarget = std::clamp(holdFramesTarget, 15, 90);

                if (g_windows[i].last_effect_speed >= holdSpeedThreshold && absPlaybackRate < holdSpeedThreshold) {
                    g_windows[i].betacam_hold_frames = std::max(g_windows[i].betacam_hold_frames, holdFramesTarget);
                } else if (absPlaybackRate >= holdSpeedThreshold) {
                    g_windows[i].betacam_hold_frames = 0;
                }
                g_windows[i].last_effect_speed = absPlaybackRate;

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
                        int win_w = 0, win_h = 0;
                        // Prefer renderer output size to account for HiDPI/scaling
                        if (SDL_GetRendererOutputSize(renderer, &win_w, &win_h) != 0 || win_w <= 0 || win_h <= 0) {
                            // Fallback: take window size
                            if (g_windows[i].window) {
                                SDL_GetWindowSize(g_windows[i].window, &win_w, &win_h);
                            }
                        }

                        SDL_Rect dst_rect = ComputeAspectFitRect(
                            pixel_buffer->width,
                            pixel_buffer->height,
                            win_w,
                            win_h
                        );

                        if (g_pixel_buffer_manager) {
                            FSTPBetacamEffect::RenderContext render_ctx;
                            render_ctx.dest_rect = &dst_rect;
                            render_ctx.window_width = win_w;
                            render_ctx.window_height = win_h;
                            render_ctx.target_aspect_ratio = (pixel_buffer->height > 0)
                                ? static_cast<float>(pixel_buffer->width) / static_cast<float>(pixel_buffer->height)
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

                        SDL_RenderCopy(renderer, new_texture, src_rect_ptr, &dst_rect);

                        if (is_new_frame) {
                            g_windows[i].last_rendered_frame = pixel_buffer->frame_number;
                        }

                        // Render zoom thumbnail if enabled
                        if (zoom_state && zoom_state->enabled && zoom_state->show_thumbnail && zoom_state->factor > 1.0f) {
                            // Thumbnail size (15% of window width, max 180px)
                            int thumb_w = std::min(180, static_cast<int>(win_w * 0.15f));
                            int thumb_h = static_cast<int>(thumb_w * ((float)pixel_buffer->height / (float)pixel_buffer->width));

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

            // 4. Final Present
            auto t_before_present = std::chrono::high_resolution_clock::now();
            SDL_RenderPresent(renderer);
            auto t_after_present = std::chrono::high_resolution_clock::now();
            total_render_present_us.fetch_add(std::chrono::duration_cast<std::chrono::microseconds>(t_after_present - t_before_present).count());

            perf_sample_count.fetch_add(1);
            // ============================================================
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

FSTPWindow* GetActiveWindow() {
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
    FSTPWindow* active_window = GetActiveWindow();
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
void SubmitAVFrame(int player_id, std::shared_ptr<AVFrame> av_frame,
                  double timestamp, int frame_number) {
    if (g_pixel_buffer_manager) {
        g_pixel_buffer_manager->SubmitAVFrame(player_id, av_frame, timestamp, frame_number);
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

    // Enable audio servomotor for all active players
    for (int player_id = 0; player_id < MAX_PLAYER_INSTANCES; player_id++) {
        FSTPAudioModuleWrapper* audio_module = GetInstanceAudioModule(player_id);
        if (audio_module) {
            audio_module->SetBetacamAudioEnabled(enabled != 0);
        }
    }
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
