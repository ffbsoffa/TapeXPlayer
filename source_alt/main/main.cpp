#include <iostream>
#include <vector>
#include <memory>
#include <chrono>
#include <thread>
#include <mutex>
#include <atomic>
#include <future>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <cstdlib>
#include <sys/resource.h>
#include <cstring> // For strerror
#include "core/decode/decode.h"
#include "core/decode/low_cached_decoder_manager.h"
#include "core/decode/full_res_decoder_manager.h"
#include "core/decode/low_res_decoder.h"
#include "common/common.h"
#include "core/display/display.h"
#include "common/fontdata.h"
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <limits.h>
#include <random>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <algorithm> // For std::transform
#include "nfd.hpp"
#include "core/remote/remote_control.h"
#include "core/menu/menu_system.h"
#include "core/decode/cached_decoder.h"
#include "../core/decode/cached_decoder_manager.h"
#include "main.h"
#include "initmanager.h"

// Forward declaration for zoom event handler from display.mm
void handleZoomMouseEvent(SDL_Event& event, int windowWidth, int windowHeight, int frameWidth, int frameHeight);

// Global RemoteControl instance
RemoteControl* g_remote_control = nullptr;

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

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
std::array<double, 5> memoryMarkers = {-1, -1, -1, -1, -1}; // -1 means marker is not set
bool showIndex = false;
bool showOSD = true;
std::atomic<double> previous_playback_rate(1.0);
std::string input_timecode;
bool waiting_for_timecode = false;
std::atomic<bool> seek_performed(false);
std::atomic<bool> is_force_decoding(false);
std::future<void> force_decode_future;
std::mutex frameIndexMutex;
void start_audio(const char* filename);
std::string generateTXTimecode(double time);
void smooth_speed_change();
void check_and_reset_threshold();
SeekInfo seekInfo;
std::atomic<bool> audio_initialized(false);
std::thread audio_thread;
std::thread speed_change_thread;
extern void cleanup_audio();
std::atomic<bool> speed_reset_requested(false);

// Variables for zoom control
std::atomic<bool> zoom_enabled(false);
std::atomic<float> zoom_factor(1.0f);
std::atomic<float> zoom_center_x(0.5f);
std::atomic<float> zoom_center_y(0.5f);
std::atomic<bool> show_zoom_thumbnail(true);

// Zoom constants
const float MAX_ZOOM_FACTOR = 10.0f;
const float MIN_ZOOM_FACTOR = 1.0f;
const float ZOOM_STEP = 1.2f;

// External variables from mainau.cpp
// extern std::vector<int16_t> audio_buffer; // REMOVED: No longer using vector buffer
extern double audio_buffer_index; // Keep this, it's now the fractional mmap index
extern std::atomic<bool> decoding_finished;
extern std::atomic<bool> decoding_completed;

// Constants for frame rate limiting (Restored)
const int TARGET_FPS = 60;
const int FRAME_DELAY = 1000 / TARGET_FPS;

// Add this at the beginning of the file with other global variables
static std::string argv0;
static std::vector<std::string> restartArgs;
bool restart_requested = false;
std::string restart_filename;
std::atomic<bool> reload_file_requested = false; // Use atomic consistent with extern declaration

// Functions to access playback rate variables
std::atomic<double>& get_playback_rate() {
    return playback_rate;
}

std::atomic<double>& get_target_playback_rate() {
    return target_playback_rate;
}

void log(const std::string& message) {
    static std::ofstream logFile("TapeXPlayer.log", std::ios::app);
    logFile << message << std::endl;
}

std::string get_current_timecode() {
    static char timecode[12];  // HH:MM:SS:FF\0
    
    // Get current playback time in seconds
    double current_time = current_audio_time.load();
    
    // Convert to HH:MM:SS:FF format
    int total_seconds = static_cast<int>(current_time);
    int hours = total_seconds / 3600;
    int minutes = (total_seconds % 3600) / 60;
    int seconds = total_seconds % 60;
    
    // Calculate frames (FF) from fractional part of time
    // Use real video FPS
    double fps = original_fps.load();
    if (fps <= 0) fps = 30.0;  // Use 30 as default value if FPS is not yet defined
    
    int frames = static_cast<int>((current_time - total_seconds) * fps);
    
    // Make sure frame number doesn't exceed fps-1
    frames = std::min(frames, static_cast<int>(fps) - 1);
    
    // Format string
    snprintf(timecode, sizeof(timecode), "%02d:%02d:%02d:%02d", 
             hours, minutes, seconds, frames);
    
    return std::string(timecode);
}

// Function to reset speed
void reset_to_normal_speed() {
    // First set flag for decoder
    speed_reset_requested.store(true);
    
    // Temporarily increase speed threshold to 24x during reset
    LowCachedDecoderManager::speed_threshold.store(24.0);
    
    // Then reset speed to 1x and direction forward
    target_playback_rate.store(1.0);
    is_reverse.store(false);
    
    // Set pause depending on requirements
    // is_paused.store(false); // uncomment if auto-play needed
    
    // Start a timer or trigger to restore the threshold back to 16x
    // This could be done via a separate thread, timer, or check in the main loop
    
    // Example of a check that could be placed in the main loop:
    // if speed_reset_requested is true and current_playback_rate is close to 1.0,
    // then reset speed_threshold back to 16.0 and set speed_reset_requested to false
}

void handleMenuCommand(int command) {
    switch (command) {
        case MENU_FILE_OPEN: {
            NFD::UniquePath outPath;
            nfdfilteritem_t filterItem[1] = { { "Video files", "mp4,mov,avi,mkv,wmv,flv,webm" } };
            nfdresult_t result = NFD::OpenDialog(outPath, filterItem, 1);
            if (result == NFD_OKAY) {
                std::string filename = outPath.get();
                std::cout << "Selected file: " << filename << std::endl;
                
                // Restart player with new file
                restartPlayerWithFile(filename);
            }
            break;
        }
        case MENU_INTERFACE_SELECT: {
            // TODO: Implement interface selection logic
            break;
        }
    }
}

// Functions for zoom control
void increase_zoom() {
    float current = zoom_factor.load();
    float new_factor = current * ZOOM_STEP;
    if (new_factor > MAX_ZOOM_FACTOR) new_factor = MAX_ZOOM_FACTOR;
    zoom_factor.store(new_factor);
    
    // Enable zoom if it was disabled
    if (!zoom_enabled.load() && new_factor > MIN_ZOOM_FACTOR) {
        zoom_enabled.store(true);
    }
}

void decrease_zoom() {
    float current = zoom_factor.load();
    float new_factor = current / ZOOM_STEP;
    if (new_factor < MIN_ZOOM_FACTOR) {
        new_factor = MIN_ZOOM_FACTOR;
        zoom_enabled.store(false);
    }
    zoom_factor.store(new_factor);
}

void reset_zoom() {
    zoom_factor.store(MIN_ZOOM_FACTOR);
    zoom_center_x.store(0.5f);
    zoom_center_y.store(0.5f);
    zoom_enabled.store(false);
}

void set_zoom_center(float x, float y) {
    zoom_center_x.store(x);
    zoom_center_y.store(y);
}

void toggle_zoom_thumbnail() {
    show_zoom_thumbnail.store(!show_zoom_thumbnail.load());
}

int main(int argc, char* argv[]) {
    // Store the program path for potential relaunch
    argv0 = argv[0];
    
    // Save command line arguments for possible restart
    restartArgs.clear();
    for (int i = 0; i < argc; ++i) {
        restartArgs.push_back(argv[i]);
    }
    
    log("Program started");
    log("Number of arguments: " + std::to_string(argc));
    for (int i = 0; i < argc; ++i) {
        log("Argument " + std::to_string(i) + ": " + argv[i]);
    }
    log("Current working directory: " + std::string(getcwd(NULL, 0)));

    // Initialize remote control
    g_remote_control = new RemoteControl();
    if (!g_remote_control->initialize()) {
        std::cerr << "Warning: Failed to initialize remote control" << std::endl;
        log("Warning: Failed to initialize remote control");
    }

#ifdef __APPLE__
    // Initialize menu system
    initializeMenuSystem();
#endif

    // SDL initialization and window creation is done once
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        std::cerr << "SDL initialization error: " << SDL_GetError() << std::endl;
        return 1;
    }

    // Initialize TTF
    if (TTF_Init() == -1) {
        std::cerr << "SDL_ttf initialization error: " << TTF_GetError() << std::endl;
        return 1;
    }

    // Load font
    SDL_RWops* rw = SDL_RWFromConstMem(font_otf, sizeof(font_otf));
    TTF_Font* font = TTF_OpenFontRW(rw, 1, 24);
    if (!font) {
        std::cerr << "Error loading font from memory: " << TTF_GetError() << std::endl;
        return 1;
    }

    // Load saved window settings
    WindowSettings windowSettings = loadWindowSettings();
    
    // Use saved settings if available, otherwise use default dimensions
    int windowWidth = windowSettings.isValid ? windowSettings.width : 1280;
    int windowHeight = windowSettings.isValid ? windowSettings.height : 720;
    int windowX = windowSettings.isValid ? windowSettings.x : SDL_WINDOWPOS_CENTERED;
    int windowY = windowSettings.isValid ? windowSettings.y : SDL_WINDOWPOS_CENTERED;

    // Set window creation flags
    Uint32 windowFlags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_METAL; // Removed HIGHDPI
    if (windowSettings.isValid && windowSettings.isFullscreen) {
        windowFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    }

    SDL_Window* window = SDL_CreateWindow("TapeXPlayer", 
        windowX, windowY, 
        windowWidth, windowHeight, 
        windowFlags);
    if (!window) {
        std::cerr << "Window creation error: " << SDL_GetError() << std::endl;
        SDL_Quit();
        return 1;
    }

    // Enable file drag and drop support
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        std::cerr << "Renderer creation error with VSync: " << SDL_GetError() << std::endl;
        // Try to create renderer without VSync
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
        if (!renderer) {
            std::cerr << "Renderer creation error without VSync: " << SDL_GetError() << std::endl;
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 1;
        }
        std::cout << "Renderer created without VSync" << std::endl;
    } else {
        std::cout << "Renderer created with VSync" << std::endl;
    }

    // Main file loading loop
    std::string currentFilename;
    bool firstRun = true;
    bool fileProvided = (argc > 1); // Check if a file was provided as a command line argument

    // --- Define fixed speed steps ---
    const std::vector<double> speed_steps = {0.5, 1.0, 3.0, 10.0, 24.0};
    static int current_speed_index = 1; // Start at 1.0x (index 1)

    while (true) {
        // Determine which file to load
        std::string fileToLoad;
        bool shouldLoadFile = false;
        
        if (firstRun && fileProvided) {
            // On first run, use command line argument if provided
            fileToLoad = argv[1];
            shouldLoadFile = true;
            firstRun = false;
        } else if (reload_file_requested.load()) {
            // --- Show Loading Screen BEFORE processing --- 
            // renderLoadingScreen(renderer, font, "file", 0); // Removed
            // --- End Show Loading Screen ---
            // On reload, use filename from reload request
            fileToLoad = restart_filename;
            shouldLoadFile = true;
            reload_file_requested.store(false);
        } else if (firstRun) {
            // First run but no file provided - just show the empty player
            firstRun = false;
            shouldLoadFile = false;
            
            // Set window title to indicate no file is loaded
            SDL_SetWindowTitle(window, "TapeXPlayer - No File Loaded");
            
            // Clear the screen with a black background
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderClear(renderer);
            SDL_RenderPresent(renderer);
        } else {
            // No file loaded - show empty player and wait for user to open a file
            bool noFileLoaded = true;
            
            while (noFileLoaded && !shouldExit) {
                Uint32 frameStart = SDL_GetTicks();
                
                // Process remote commands at the start of each frame
                if (g_remote_control->is_initialized()) {
                    g_remote_control->process_commands();
                }
                
                // Handle fullscreen toggle request from menu
                if (toggle_fullscreen_requested.load()) {
                    toggle_fullscreen_requested.store(false);
                    Uint32 flags = SDL_GetWindowFlags(window);
                    if (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) {
                        SDL_SetWindowFullscreen(window, 0);
                    } else {
                        SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
                    }
                }
                
                SDL_Event e;
                while (SDL_PollEvent(&e)) {
                    if (e.type == SDL_QUIT) {
                        saveWindowSettings(window);
                        quit = true;
                        shouldExit = true;
                        restart_requested = false; // Cancel restart on window close
                    } else if (e.type == SDL_KEYDOWN) {
                        switch (e.key.keysym.sym) {
                            case SDLK_o:
                                // Ctrl+O to open file
                                if (e.key.keysym.mod & KMOD_CTRL) {
                                    NFD::UniquePath outPath;
                                    nfdfilteritem_t filterItem[1] = { { "Video files", "mp4,mov,avi,mkv,wmv,flv,webm" } };
                                    nfdresult_t result = NFD::OpenDialog(outPath, filterItem, 1);
                                    if (result == NFD_OKAY) {
                                        std::string filename = outPath.get();
                                        std::cout << "Selected file: " << filename << std::endl;
                                        
                                        // Restart player with new file
                                        restartPlayerWithFile(filename);
                                        noFileLoaded = false;
                                    }
                                }
                                break;
                            case SDLK_ESCAPE:
                                saveWindowSettings(window);
                                shouldExit = true;
                                restart_requested = false; // Cancel restart on Escape
                                break;
                        }
                    }
                }
                
                // Clear the screen with a black background
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
                SDL_RenderClear(renderer);
                
                // Use renderOSD to display placeholder values for timecode and playback direction
                // Parameters: renderer, font, isPlaying, playbackRate, isReverse, currentTime, frameNumber, 
                // showOSD, waiting_for_timecode, input_timecode, original_fps, jog_forward, jog_backward, 
                // Removed isLoading, loadingType, loadingProgress
                renderOSD(renderer, font, false, 0.0, false, 0.0, 0, true, false, "", 25.0, false, false);
                
                // Render a small hint at the center of the screen
                SDL_Color textColor = {200, 200, 200, 200};
                SDL_Surface* messageSurface = TTF_RenderText_Blended(font, "Press Ctrl+O to open a file", textColor);
                if (messageSurface) {
                    SDL_Texture* messageTexture = SDL_CreateTextureFromSurface(renderer, messageSurface);
                    if (messageTexture) {
                        SDL_Rect messageRect = {
                            windowWidth / 2 - messageSurface->w / 2,
                            windowHeight / 2 - messageSurface->h / 2,
                            messageSurface->w,
                            messageSurface->h
                        };
                        
                        SDL_RenderCopy(renderer, messageTexture, NULL, &messageRect);
                        SDL_DestroyTexture(messageTexture);
                    }
                    SDL_FreeSurface(messageSurface);
                }
                
                SDL_RenderPresent(renderer);
                
                // Cap the frame rate
                Uint32 frameTime = SDL_GetTicks() - frameStart;
                if (frameTime < FRAME_DELAY) {
                    SDL_Delay(FRAME_DELAY - frameTime);
                }
            }

            // If the user requested to exit from the 'no file loaded' state,
            // AND it wasn't a request to reload a file, break the main loop
            if (shouldExit && !reload_file_requested.load()) { 
                break;
            }
        }
        
        if (shouldLoadFile) {
            resetPlayerState();
            
            if (firstRun && fileProvided) { 
                 SDL_SetWindowTitle(window, "TapeXPlayer"); // Set title directly
                 SDL_RenderPresent(renderer); // Force immediate display
                 firstRun = false; // Mark first run as handled
            }
            // Initialize variables for the playback loop that need to be passed to/from loading sequence
            std::vector<FrameInfo> frameIndex; // Needs to be populated by loading sequence
            std::unique_ptr<FullResDecoderManager> fullResManagerPtr;
            std::unique_ptr<LowCachedDecoderManager> lowCachedManagerPtr;
            std::unique_ptr<CachedDecoderManager> cachedManagerPtr;
            std::atomic<int> currentFrame(0); // Will be reset inside loading sequence
            std::atomic<bool> isPlaying(false); // Will be set inside loading sequence

            // Create LoadingStatus object for this loading operation
            LoadingStatus loadingStatus;

            // Call the loading sequence function asynchronously
            std::future<bool> loading_future = mainLoadingSequence(
                renderer, window,
                loadingStatus, // 3rd argument is LoadingStatus&
                fileToLoad,    // 4th argument is const std::string&
                    currentFilename, 
                frameIndex, fullResManagerPtr, lowCachedManagerPtr, cachedManagerPtr,
                currentFrame, isPlaying // Atomics are passed by reference implicitly
            );

            // --- Show Loading Screen while waiting for the future --- 
            SDL_SetWindowTitle(window, "TapeXPlayer - Loading..."); // Set loading title
            while (loading_future.wait_for(std::chrono::milliseconds(10)) != std::future_status::ready) {
                // Render loading screen
                renderLoadingScreen(renderer, font, loadingStatus);

                // Handle essential events (like SDL_QUIT) during loading
                SDL_Event e;
                while (SDL_PollEvent(&e)) {
                    if (e.type == SDL_QUIT) {
                        // Need to signal the loading thread to stop if possible
                        // This requires adding stop logic to mainLoadingSequence/its sub-tasks
                        // For now, just set quit flags and break loops
                        quit = true;
                        shouldExit = true; 
                        // Optionally try to cancel the future if the task supports cancellation
                        goto loading_exit; // Exit loading loop immediately
                    } 
                    // Handle other minimal events if necessary
                }
                if (quit.load()) goto loading_exit; // Exit if quit was set

                // Small delay to prevent busy-waiting
                 // SDL_Delay(10); // wait_for already provides a delay
            }
            loading_exit:; // Label for exiting the loading loop

            // Check if we exited due to quit signal
            if (quit.load()) {
                // Cleanup resources acquired before loading started if necessary
                continue; // Go to the start of the outer loop, which will handle exit
            }

            // Loading finished, get the result
            bool loading_success = loading_future.get();

             if (!loading_success) {
                  // Loading failed, let the outer loop handle showing the "Press Ctrl+O" screen
                   continue; // Go to the start of the outer while(true) loop
              }

            // --- Loading successful, update window title --- 
            std::string filename_only = std::filesystem::path(currentFilename).filename().string();
            std::string windowTitle = "TapeXPlayer - " + filename_only;
            SDL_SetWindowTitle(window, windowTitle.c_str());

             // --- Start Decoder Manager Threads (after successful load and no quit signal) ---
                if (fullResManagerPtr) fullResManagerPtr->run(); 
                if (lowCachedManagerPtr) lowCachedManagerPtr->run(); 
             if (cachedManagerPtr) cachedManagerPtr->run();
             // -------------------------------------------------------------------------

             // --- Duration handling moved outside loading sequence for simplicity ---
             // Let's get duration after successful load if needed immediately
             double duration = get_file_duration(currentFilename.c_str());
             total_duration.store(duration);
             std::cout << "Total duration: " << total_duration.load() << " seconds" << std::endl;

            // Start playback after a short delay and manager setup
            std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Shorter delay maybe?
             // Ensure playback starts paused (already handled by isPlaying init and loading func)
            target_playback_rate.store(0.0); 
            playback_rate.store(0.0); // Ensure current rate is also 0

             // --- Start of the inner playback loop ---
            while (!shouldExit) {
                // Process remote commands at the start of each frame
                if (g_remote_control->is_initialized()) {
                    g_remote_control->process_commands();
                }
                
                // Check if we need to reset the speed threshold
                check_and_reset_threshold();

                // Handle fullscreen toggle request from menu
                if (toggle_fullscreen_requested.load()) {
                    toggle_fullscreen_requested.store(false);
                    Uint32 flags = SDL_GetWindowFlags(window);
                    if (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) {
                        SDL_SetWindowFullscreen(window, 0);
                    } else {
                        SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
                    }
                }

                Uint32 frameStart = SDL_GetTicks();

                SDL_Event e;
                while (SDL_PollEvent(&e)) {
                    if (e.type == SDL_QUIT) {
                        saveWindowSettings(window);
                        quit = true;
                        shouldExit = true;
                        restart_requested = false; // Cancel restart on window close
                    } else if (e.type == SDL_KEYDOWN) {
                        if (waiting_for_timecode) {
                            if (e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_KP_ENTER) {
                                try {
                                    double target_time = parse_timecode(input_timecode);
                                    seek_to_time(target_time);
                                    waiting_for_timecode = false;
                                    input_timecode.clear();
                                } catch (const std::exception& ex) {
                                }
                            } else if (e.key.keysym.sym == SDLK_BACKSPACE && !input_timecode.empty()) {
                                input_timecode.pop_back();
                            } else if (input_timecode.length() < 8) {
                                const char* keyName = SDL_GetKeyName(e.key.keysym.sym);
                                char digit = '\0';
                                
                                if (keyName[0] >= '0' && keyName[0] <= '9' && keyName[1] == '\0') {
                                    // Regular number keys
                                    digit = keyName[0];
                                } else if (strncmp(keyName, "Keypad ", 7) == 0 && keyName[7] >= '0' && keyName[7] <= '9' && keyName[8] == '\0') {
                                    // NumPad keys
                                    digit = keyName[7];
                                }
                                
                                if (digit != '\0') {
                                    input_timecode += digit;
                                }
                            } else if (e.key.keysym.sym == SDLK_ESCAPE) {
                                waiting_for_timecode = false;
                                input_timecode.clear();
                            }
                        } else {
                            switch (e.key.keysym.sym) {
                                case SDLK_SPACE:
                                    // Regular pause/play
                                    if (std::abs(playback_rate.load()) > 1.1) {
                                        // If speed is above 1.1x, first reset speed
                                        reset_to_normal_speed();
                                        // --- Reset speed index to 1.0x --- 
                                        current_speed_index = 1; // Index for 1.0x in speed_steps
                                        // --- End Reset speed index --- 
                                    } else {
                                        toggle_pause();
                                    }
                                    break;
                                case SDLK_RIGHT:
                                    if (e.key.keysym.mod & KMOD_SHIFT) {
                                        if (e.key.repeat == 0) { // Check if it's not a repeat event
                                            start_jog_forward();
                                        }
                                    } else {
                                        // Regular right arrow behavior
                                        target_playback_rate.store(1.0);
                                        is_reverse.store(false);
                                    }
                                    break;
                                case SDLK_LEFT:
                                    if (e.key.keysym.mod & KMOD_SHIFT) {
                                        if (e.key.repeat == 0) {
                                            start_jog_backward();
                                        }
                                    } else {
                                        // Regular left arrow behavior
                                        is_reverse.store(!is_reverse.load());
                                    }
                                    break;
                                case SDLK_UP:
                                    if (current_speed_index < speed_steps.size() - 1) {
                                        current_speed_index++;
                                    }
                                    target_playback_rate.store(speed_steps[current_speed_index]);
                                    break;
                                case SDLK_DOWN:
                                    if (current_speed_index > 0) {
                                        current_speed_index--;
                                    }
                                    target_playback_rate.store(speed_steps[current_speed_index]);
                                    break;
                                case SDLK_r:
                                    is_reverse.store(!is_reverse.load());
                                    break;
                                case SDLK_ESCAPE:
                                    saveWindowSettings(window);
                                    shouldExit = true;
                                    restart_requested = false; // Cancel restart on Escape
                                    break;
                                case SDLK_PLUS:
                                case SDLK_EQUALS:
                                    increase_volume();
                                    break;
                                case SDLK_MINUS:
                                    decrease_volume();
                                    break;
                                case SDLK_o:
                                    // Ctrl+O to open file
                                    if (e.key.keysym.mod & KMOD_CTRL) {
                                        NFD::UniquePath outPath;
                                        nfdfilteritem_t filterItem[1] = { { "Video files", "mp4,mov,avi,mkv,wmv,flv,webm" } };
                                        nfdresult_t result = NFD::OpenDialog(outPath, filterItem, 1);
                                        if (result == NFD_OKAY) {
                                            std::string filename = outPath.get();
                                            std::cout << "Selected file: " << filename << std::endl;
                                            
                                            // Restart player with new file
                                            restartPlayerWithFile(filename);
                                        }
                                    }
                                    break;
                                case SDLK_d:
                                    if (e.key.keysym.mod & KMOD_ALT) {
                                        showOSD = !showOSD;
                                    } else if (SDL_GetModState() & KMOD_SHIFT) {
                                        showIndex = !showIndex;
                                    }
                                    break;
                                case SDLK_g:
                                    waiting_for_timecode = true;
                                    break;
                                case SDLK_1:
                                case SDLK_2:
                                case SDLK_3:
                                case SDLK_4:
                                case SDLK_5:
                                    {
                                        // Check if we're not in timecode input mode
                                        if (!waiting_for_timecode) {
                                            int markerIndex = e.key.keysym.sym - SDLK_1;
                                            if (e.key.keysym.mod & KMOD_ALT) {
                                                // Set marker
                                                memoryMarkers[markerIndex] = current_audio_time.load();
                                                std::cout << "Marker " << (markerIndex + 1) << " set at " 
                                                        << generateTXTimecode(memoryMarkers[markerIndex]) << std::endl;
                                            } else {
                                                // Jump to marker
                                                if (memoryMarkers[markerIndex] >= 0) {
                                                    seek_to_time(memoryMarkers[markerIndex]);
                                                }
                                            }
                                        }
                                    }
                                    break;
                                case SDLK_f:
                                    // Toggle fullscreen mode
                                    {
                                        Uint32 flags = SDL_GetWindowFlags(window);
                                        if (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) {
                                            SDL_SetWindowFullscreen(window, 0);
                                        } else {
                                            SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
                                        }
                                    }
                                    break;
                                case SDLK_z:
                                    // Toggle zoom mode
                                    if (!zoom_enabled.load()) {
                                        int mouseX, mouseY;
                                        SDL_GetMouseState(&mouseX, &mouseY);
                                        // Pass texture dimensions to the function
                                        handleZoomMouseEvent(e, windowWidth, windowHeight, get_last_texture_width(), get_last_texture_height()); 
                                    } else {
                                        reset_zoom(); // Disable zoom if it was enabled
                                    }
                                    zoom_enabled.store(!zoom_enabled.load());
                                    break;
                                case SDLK_t:
                                    // Toggle thumbnail display
                                    toggle_zoom_thumbnail();
                                    break;
                            }
                        }
                    } else if (e.type == SDL_KEYUP) {
                        switch (e.key.keysym.sym) {
                            case SDLK_RIGHT:
                            case SDLK_LEFT:
                                if (e.key.keysym.mod & KMOD_SHIFT) {
                                    stop_jog();
                                }
                                break;
                        }
                    } else if (e.type == SDL_WINDOWEVENT) {
                        if (e.window.event == SDL_WINDOWEVENT_RESIZED) {
                            int windowWidth, windowHeight;
                            SDL_GetWindowSize(window, &windowWidth, &windowHeight);
                        }
                    } else if (e.type == SDL_DROPFILE) {
                        // Handle file drop
                        char* droppedFile = e.drop.file;
                        if (droppedFile) {
                            std::string filename = droppedFile;
                            std::cout << "File dropped: " << filename << std::endl;
                            log("File dropped: " + filename);
                            
                            // Free memory allocated by SDL
                            SDL_free(droppedFile);
                            
                            // Check file extension
                            std::string extension = filename.substr(filename.find_last_of(".") + 1);
                            std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
                            
                            if (extension == "mp4" || extension == "mov" || extension == "avi" || extension == "mkv" || extension == "wmv" || extension == "flv" || extension == "webm") {
                                // Restart player with new file
                                restartPlayerWithFile(filename);
                            } else {
                                std::cerr << "Unsupported file format: " << extension << std::endl;
                                log("Unsupported file format: " + extension);
                            }
                        }
                    } else if (e.type == SDL_MOUSEMOTION || e.type == SDL_MOUSEWHEEL || e.type == SDL_MOUSEBUTTONDOWN) { // Re-enabled Mouse Events
                        // Handle mouse events for zooming
                        int windowWidth, windowHeight;
                        SDL_GetRendererOutputSize(renderer, &windowWidth, &windowHeight);
                        
                        // Get current frame dimensions from frame buffer
                        // --- Get frame dimensions using getters from display.mm --- 
                        int frameWidth = get_last_texture_width(); 
                        int frameHeight = get_last_texture_height();
                        // --- End Get frame dimensions ---
                        
                        // If frame dimensions are known, handle mouse events
                        if (frameWidth > 0 && frameHeight > 0) {
                            handleZoomMouseEvent(e, windowWidth, windowHeight, frameWidth, frameHeight);
                        }
                    }
                }

                // Update currentFrame based on current audio time
                double currentTime = current_audio_time.load();
                int64_t target_time_ms = static_cast<int64_t>(currentTime * 1000.0);
                int newCurrentFrame = findClosestFrameIndexByTime(frameIndex, target_time_ms);
                 if (!frameIndex.empty()) { newCurrentFrame = std::max(0, std::min(newCurrentFrame, static_cast<int>(frameIndex.size()) - 1)); } else { newCurrentFrame = 0; }
                 
                 // Notify managers ONLY if the frame has actually changed
                 if (newCurrentFrame != currentFrame.load()) {
                     currentFrame.store(newCurrentFrame);
                     if (fullResManagerPtr) fullResManagerPtr->notifyFrameChange();
                     if (lowCachedManagerPtr) lowCachedManagerPtr->notifyFrameChange();
                     if (cachedManagerPtr) cachedManagerPtr->notifyFrameChange();
                 }

                // --- Frame Selection Logic ---
                std::shared_ptr<AVFrame> frameToDisplay = nullptr;
                FrameInfo::FrameType frameTypeToDisplay = FrameInfo::EMPTY;
                double currentPlaybackRate = std::abs(playback_rate.load()); if (newCurrentFrame >= 0 && newCurrentFrame < frameIndex.size()) { const FrameInfo& currentFrameInfo = frameIndex[newCurrentFrame]; std::unique_lock<std::mutex> lock(currentFrameInfo.mutex); if (!currentFrameInfo.is_decoding) { if (currentPlaybackRate <= 1.1) { if (currentFrameInfo.frame) { frameToDisplay = currentFrameInfo.frame; frameTypeToDisplay = FrameInfo::FULL_RES; } else if (currentFrameInfo.low_res_frame) { frameToDisplay = currentFrameInfo.low_res_frame; frameTypeToDisplay = FrameInfo::LOW_RES; } else if (currentFrameInfo.cached_frame) { frameToDisplay = currentFrameInfo.cached_frame; frameTypeToDisplay = FrameInfo::CACHED; } lock.unlock(); } else { if (currentFrameInfo.low_res_frame) { frameToDisplay = currentFrameInfo.low_res_frame; frameTypeToDisplay = FrameInfo::LOW_RES; } else if (currentFrameInfo.cached_frame) { frameToDisplay = currentFrameInfo.cached_frame; frameTypeToDisplay = FrameInfo::CACHED; } else { lock.unlock(); bool isForward = playback_rate.load() >= 0; int step = isForward ? 1 : -1; int searchRange = 15; for (int i = 1; i <= searchRange; ++i) { int checkFrameIdx = newCurrentFrame + (i * step); if (checkFrameIdx >= 0 && checkFrameIdx < frameIndex.size()) { std::lock_guard<std::mutex> search_lock(frameIndex[checkFrameIdx].mutex); if (!frameIndex[checkFrameIdx].is_decoding) { if (frameIndex[checkFrameIdx].low_res_frame) { frameToDisplay = frameIndex[checkFrameIdx].low_res_frame; frameTypeToDisplay = FrameInfo::LOW_RES; break; } else if (frameIndex[checkFrameIdx].cached_frame) { frameToDisplay = frameIndex[checkFrameIdx].cached_frame; frameTypeToDisplay = FrameInfo::CACHED; break; } } } else { break; } } } if (lock.owns_lock()) { lock.unlock(); } } } else { lock.unlock(); } }

                // Check if seek is complete
                if (seekInfo.completed.load()) {
                    seekInfo.completed.store(false);
                    seek_performed.store(false);
                }

                // --- Determine highResWindowSize and ringBufferCapacity ---
                 // Note: These might be better calculated once during loading or dynamically based on needs
                 int highResWindowSize = 700; // Default or recalculate based on fps if needed here
                 double fps_local = original_fps.load();
                 if (fps_local > 55.0) { highResWindowSize = 1400; } else if (fps_local > 45.0) { highResWindowSize = 1200; } else if (fps_local > 28.0) { highResWindowSize = 700; } else { highResWindowSize = 600; }
                 const size_t ringBufferCapacity = 2000; // Could also be adaptive


                displayFrame(renderer, frameIndex, newCurrentFrame, frameToDisplay, frameTypeToDisplay,
                            true, // enableHighResDecode - Assuming true for now
                             playback_rate.load(), current_audio_time.load(), total_duration.load(),
                            showIndex, showOSD, font, isPlaying, is_reverse.load(), waiting_for_timecode,
                            input_timecode, original_fps.load(), jog_forward, jog_backward,
                            ringBufferCapacity, highResWindowSize, 950); // Pass calculated sizes

                // Limit frame rate
                Uint32 frameTime = SDL_GetTicks() - frameStart;
                if (frameTime < FRAME_DELAY) {
                    SDL_Delay(FRAME_DELAY - frameTime);
                }

                // Handle seek request
                if (seek_performed.load()) {
                    seekInfo.time.store(current_audio_time.load());
                    seekInfo.requested.store(true);
                    seekInfo.completed.store(false);
                    // Notify managers immediately on seek request
                     if (fullResManagerPtr) fullResManagerPtr->notifyFrameChange();
                     if (lowCachedManagerPtr) lowCachedManagerPtr->notifyFrameChange();
                     if (cachedManagerPtr) cachedManagerPtr->notifyFrameChange();
                }

                // Exit condition check for the inner loop
                if (shouldExit.load()) {
                    break; 
                } 

            } // --- End of the inner playback loop ---


            // --- Cleanup after inner loop exits (either by shouldExit or reaching end of file naturally?) ---
            std::cout << "[Cleanup] Stopping managers..." << std::endl; // Debug log
            if (fullResManagerPtr) fullResManagerPtr->stop();
            if (lowCachedManagerPtr) lowCachedManagerPtr->stop();
            if (cachedManagerPtr) cachedManagerPtr->stop();
            std::cout << "[Cleanup] Managers stopped." << std::endl; // Debug log
            
            std::cout << "[Cleanup] Joining speed change thread..." << std::endl; // Debug log
            if (speed_change_thread.joinable()) {
                 // Ensure smooth_speed_change loop checks shouldExit/quit
                try {
                speed_change_thread.join();
                    std::cout << "[Cleanup] Speed change thread joined." << std::endl; // Debug log
                } catch (const std::system_error& e) {
                     std::cerr << "[Cleanup] Error joining speed_change_thread: " << e.what() << " (" << e.code() << ")" << std::endl;
                     // Consider if detaching is an option here as a last resort
                 }
            }
            

            // Cleanup audio resources
            std::cout << "[Cleanup] Cleaning audio..." << std::endl; // Debug log
            cleanup_audio();
            
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            // Clean up temporary files before potentially loading new file
            std::cout << "[Cleanup] Cleaning temp files..." << std::endl; // Debug log
            cleanupTempFiles();
            
            // If there was a request to exit the program (not reload file), break the outer loop
            if (!reload_file_requested.load() && !restart_requested) {
                 // Ensure shouldExit is true so the outer loop terminates correctly
                 shouldExit = true; // Set explicitly for clarity
                 break; // Break the outer while(true) loop
            }
            // If reload or restart was requested, the outer loop will continue,
            // and reload_file_requested will be handled at the beginning.
        } // End if (shouldLoadFile)
    } // End outer while(true) loop

    // Save final window settings before closing
    saveWindowSettings(window);

    // Cleanup display resources (including Metal) BEFORE destroying SDL renderer/window
    cleanupDisplayResources();

    // Free TTF resources
    if (font) {
        TTF_CloseFont(font);
        font = nullptr;
    }

    TTF_Quit();

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    // Before exiting
#ifdef __APPLE__
    cleanupMenuSystem();
#endif

    // Before exiting, cleanup remote control
    if (g_remote_control) {
        delete g_remote_control;
        g_remote_control = nullptr;
    }

    // If restart was requested (not file reload), launch program with new file
    if (restart_requested) {
        std::cout << "Performing restart with file: " << restart_filename << std::endl;
        log("Performing restart with file: " + restart_filename);
        
        // Prepare arguments for restart
        std::vector<char*> args;
        args.push_back(const_cast<char*>(argv0.c_str())); // Program path
        args.push_back(const_cast<char*>(restart_filename.c_str())); // New file
        args.push_back(nullptr); // Terminating nullptr
        
        // Perform restart
#ifdef _WIN32
        // Use _execv for Windows
        _execv(argv0.c_str(), args.data());
#else
        // Use execv for UNIX-like systems
        execv(argv0.c_str(), args.data());
#endif
        
        // If execv returns, an error occurred
        std::cerr << "Failed to restart program: " << strerror(errno) << std::endl;
        log("Failed to restart program: " + std::string(strerror(errno)));
    }

    return 0;
}

// Add this check in the appropriate update loop
void check_and_reset_threshold() {
    if (speed_reset_requested.load() && fabs(playback_rate.load() - 1.0) < 0.1) {
        // Speed is close to 1.0x now, reset the threshold back to default
        LowCachedDecoderManager::speed_threshold.store(16.0);
        speed_reset_requested.store(false);
    }
}
