#ifdef __linux__

#include "../main.h"
#include "FSTPWindowManager.h"
#include "FSTPKeyboard.h"
#include "FSTPOSDSystem.h"
#include "FSTPSettings.h"
#include "../FSTPPlayerModule/FSTPPlayerManager.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>

// Global rendering control
static std::atomic<bool> g_renderingActive{false};
static std::atomic<bool> g_renderThreadRunning{false};
static std::thread g_renderThread;

// Autonomous rendering function - works independently of events (like macOS version)
void AutoRenderFrame() {
    if (!g_renderingActive.load()) return;

    // Render all active windows through WindowManager
    RenderAllWindows();

    // Update OSD data for all active windows with real player data
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
            }
        }
    }
}

// Start autonomous rendering thread (60 FPS like macOS)
void StartAutonomousRendering() {
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

// Main Linux/SDL2 UI loop - full implementation
int RunMainUILoop() {
    std::cout << "Starting Linux/SDL2 UI main loop..." << std::endl;

    // Configure SDL hints for optimal performance
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");  // Bilinear filtering
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "1");           // Enable VSync

    // Initialize SDL video subsystem
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        std::cerr << "Failed to initialize SDL: " << SDL_GetError() << std::endl;
        return -1;
    }

    // Initialize SDL_ttf for text rendering
    if (TTF_Init() != 0) {
        std::cerr << "Failed to initialize SDL_ttf: " << TTF_GetError() << std::endl;
        SDL_Quit();
        return -1;
    }

    std::cout << "SDL2 initialized successfully" << std::endl;

    // Initialize window manager
    if (InitWindowManager() != 0) {
        std::cerr << "Failed to initialize window manager" << std::endl;
        TTF_Quit();
        SDL_Quit();
        return -1;
    }

    std::cout << "Window manager initialized" << std::endl;

    // Create main window (automatically bound to player #0)
    int main_window_index = CreateNewWindow("TapeXPlayer - Player 0", 1280, 720);
    if (main_window_index < 0) {
        std::cerr << "Failed to create main window" << std::endl;
        ShutdownWindowManager();
        TTF_Quit();
        SDL_Quit();
        return -1;
    }

    // Get main window for OSD system
    FSTPWindow* main_window = GetMainWindow();
    if (main_window == nullptr) {
        std::cerr << "Error getting main window" << std::endl;
        ShutdownWindowManager();
        TTF_Quit();
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
        TTF_Quit();
        SDL_Quit();
        return -1;
    }

    std::cout << "OSD system initialized" << std::endl;

    // Start autonomous rendering of all windows (60 FPS in separate thread)
    StartAutonomousRendering();

    std::cout << "Ready! Press ESC or close window to exit." << std::endl;

    // Main event loop - similar to macOS version
    bool running = true;
    SDL_Event event;

    while (running) {
        // CPU OPTIMIZATION: Use SDL_WaitEventTimeout instead of PollEvent + delay
        // Blocks until event or timeout → CPU savings!
        // Adaptive timeout: zoom panning requires fast response (1ms), normally 16ms (60 Hz)
        bool zoom_panning = IsZoomPanningActive();
        int timeout_ms = zoom_panning ? 1 : 16;

        // Wait for event OR timeout (blocks thread → saves CPU!)
        if (SDL_WaitEventTimeout(&event, timeout_ms)) {
            // Event available - process it
            do {
                // Pass events to window manager
                HandleWindowEvents(&event);

                // Process keyboard events for player control
                if (!HandleKeyboardEvents(event)) {
                    running = false;
                    break;
                }

                switch (event.type) {
                    case SDL_QUIT:
                        std::cout << "SDL_QUIT received, exiting..." << std::endl;
                        running = false;
                        break;

                    case SDL_KEYDOWN:
                        switch (event.key.keysym.sym) {
                            case SDLK_ESCAPE:
                            case SDLK_q:
                                if (event.key.keysym.mod & KMOD_CTRL) {
                                    std::cout << "Ctrl+Q pressed, exiting..." << std::endl;
                                    running = false;
                                }
                                break;
                        }
                        break;
                }

                // Process all accumulated events after WaitEventTimeout
            } while (SDL_PollEvent(&event));
        }
        // If SDL_WaitEventTimeout returned false (timeout without events) - just continue loop
        // Rendering happens in autonomous thread!
    }

    std::cout << "Shutting down..." << std::endl;

    // Stop autonomous rendering
    StopAutonomousRendering();

    // Shutdown OSD system
    ShutdownOSDSystem();

    // Shutdown settings system
    ShutdownSettings();

    // Shutdown window manager (will automatically close all windows)
    ShutdownWindowManager();

    // Shutdown SDL
    TTF_Quit();
    SDL_Quit();

    std::cout << "Linux UI loop finished cleanly" << std::endl;
    return 0;
}

#endif // __linux__