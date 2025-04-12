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
extern std::vector<float> audio_buffer;
extern size_t audio_buffer_index;
extern std::atomic<bool> decoding_finished;
extern std::atomic<bool> decoding_completed;

// Constants for frame rate limiting (Restored)
const int TARGET_FPS = 60;
const int FRAME_DELAY = 1000 / TARGET_FPS;

// Structure for storing window settings
struct WindowSettings {
    int x = SDL_WINDOWPOS_CENTERED;
    int y = SDL_WINDOWPOS_CENTERED;
    int width = 1280;
    int height = 720;
    bool isFullscreen = false;
    bool isValid = false;
};

// Add this at the beginning of the file with other global variables
static std::string argv0;
static std::vector<std::string> restartArgs;
static bool restart_requested = false;
static std::string restart_filename;
static bool reload_file_requested = false; // New flag for reloading file without restarting program

// Function to get config file path depending on OS
std::string getConfigFilePath() {
    std::string configPath;
    
#if defined(_WIN32) || defined(_WIN64)
    // Windows: use AppData
    char* appDataPath = nullptr;
    size_t pathLen;
    _dupenv_s(&appDataPath, &pathLen, "APPDATA");
    if (appDataPath) {
        configPath = std::string(appDataPath) + "\\TapeXPlayer\\";
        free(appDataPath);
    } else {
        configPath = ".\\";
    }
#elif defined(__APPLE__)
    // macOS: use ~/Library/Application Support/
    const char* homeDir = getenv("HOME");
    if (homeDir) {
        configPath = std::string(homeDir) + "/Library/Application Support/TapeXPlayer/";
    } else {
        configPath = "./";
    }
#else
    // Linux and other UNIX-like systems: use ~/.config/
    const char* homeDir = getenv("HOME");
    if (homeDir) {
        configPath = std::string(homeDir) + "/.config/TapeXPlayer/";
    } else {
        configPath = "./";
    }
#endif

    // Create directory if it doesn't exist
    std::filesystem::create_directories(configPath);
    
    return configPath + "window_settings.conf";
}

// Function to load window settings
WindowSettings loadWindowSettings() {
    WindowSettings settings;
    std::string configFile = getConfigFilePath();
    std::ifstream file(configFile);
    
    if (file.is_open()) {
        file >> settings.x >> settings.y >> settings.width >> settings.height >> settings.isFullscreen;
        settings.isValid = true;
        file.close();
    }
    
    return settings;
}

// Function to save window settings - updated
void saveWindowSettings(SDL_Window* window) {
    int x, y, width, height;
    SDL_GetWindowPosition(window, &x, &y);
    SDL_GetWindowSize(window, &width, &height);
    Uint32 flags = SDL_GetWindowFlags(window);
    bool isFullscreen = (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
    
    std::string configFile = getConfigFilePath();
    std::ofstream file(configFile);
    
    if (file.is_open()) {
        file << x << " " << y << " " << width << " " << height << " " << (isFullscreen ? 1 : 0);
        file.close();
    }
}

// Replace old function with this one
void saveWindowSettings(int x, int y, int width, int height, bool isFullscreen) {
    std::string configFile = getConfigFilePath();
    std::ofstream file(configFile);
    
    if (file.is_open()) {
        file << x << " " << y << " " << width << " " << height << " " << (isFullscreen ? 1 : 0);
        file.close();
    }
}

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
    
    // Then reset speed to 1x and direction forward
    target_playback_rate.store(1.0);
    is_reverse.store(false);
    
    // Set pause depending on requirements
    // is_paused.store(false); // uncomment if auto-play needed
}

// Function declaration at start of file
bool processMediaSource(const std::string& source, std::string& processedFilePath, ProgressCallback progressCallback = nullptr);
void cleanupTempFiles();

// Function to reset player state before loading new file
void resetPlayerState() {
    // Reset flags and states
    quit = false;
    shouldExit = false;
    current_audio_time.store(0.0);
    playback_rate.store(0.0); // Start with playback rate at 0 to prevent audio playing immediately
    target_playback_rate.store(0.0); // Also set target to 0
    is_reverse.store(false);
    is_seeking.store(false);
    total_duration.store(0.0);
    original_fps.store(0.0);
    
    // Reset timecode input state
    waiting_for_timecode = false;
    input_timecode.clear();
    
    // Reset seek state
    seekInfo.requested.store(false);
    seekInfo.completed.store(false);
    seek_performed.store(false);
    
    // Clear audio buffer
    if (!audio_buffer.empty()) {
        audio_buffer.clear();
    }
    audio_buffer_index = 0;
    
    // Reset decoding flags
    decoding_finished.store(false);
    decoding_completed.store(false);
}

// Function to restart player with new file
void restartPlayerWithFile(const std::string& filename) {
    std::cout << "Loading new file: " << filename << std::endl;
    log("Loading new file: " + filename);
    
    // Save filename for loading
    restart_filename = filename;
    
    // Set file reload flag
    reload_file_requested = true;
    
    // Signal to exit main loop
    shouldExit = true;
}

void handleMenuCommand(int command) {
    switch (command) {
        case MENU_FILE_OPEN: {
            NFD::UniquePath outPath;
            nfdfilteritem_t filterItem[1] = { { "Video files", "mp4,mov,avi,mkv" } };
            nfdresult_t result = NFD::OpenDialog(outPath, filterItem, 1);
            if (result == NFD_OKAY) {
                std::string filename = outPath.get();
                std::cout << "Selected file: " << filename << std::endl;
                
                // Show loading screen before restarting player
                SDL_Window* window = SDL_GetWindowFromID(1); // Assuming window ID 1
                if (window) {
                    SDL_Renderer* renderer = SDL_GetRenderer(window);
                    if (renderer) {
                        // Get font
                        SDL_RWops* rw = SDL_RWFromConstMem(font_otf, sizeof(font_otf));
                        TTF_Font* font = TTF_OpenFontRW(rw, 1, 24);
                        if (font) {
                            renderLoadingScreen(renderer, font, "file", 0);
                            TTF_CloseFont(font);
                        }
                    }
                }
                
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

// Progress callback functions
void convertProgressCallback(int progress) {
    static SDL_Renderer* rendererPtr = nullptr;
    static TTF_Font* fontPtr = nullptr;
    
    // Store renderer and font pointers on first call
    if (rendererPtr == nullptr) {
        SDL_Window* window = SDL_GetWindowFromID(1); // Assuming window ID 1
        if (window) {
            rendererPtr = SDL_GetRenderer(window);
        }
    }
    
    if (fontPtr == nullptr) {
        // We need to recreate the font for this thread
        SDL_RWops* rw = SDL_RWFromConstMem(font_otf, sizeof(font_otf));
        fontPtr = TTF_OpenFontRW(rw, 1, 24);
    }
    
    if (rendererPtr && fontPtr) {
        renderLoadingScreen(rendererPtr, fontPtr, "convert", progress);
    }
}

void bufferProgressCallback(int progress) {
    static SDL_Renderer* rendererPtr = nullptr;
    static TTF_Font* fontPtr = nullptr;
    
    // Store renderer and font pointers on first call
    if (rendererPtr == nullptr) {
        SDL_Window* window = SDL_GetWindowFromID(1); // Assuming window ID 1
        if (window) {
            rendererPtr = SDL_GetRenderer(window);
        }
    }
    
    if (fontPtr == nullptr) {
        // We need to recreate the font for this thread
        SDL_RWops* rw = SDL_RWFromConstMem(font_otf, sizeof(font_otf));
        fontPtr = TTF_OpenFontRW(rw, 1, 24);
    }
    
    if (rendererPtr && fontPtr) {
        renderLoadingScreen(rendererPtr, fontPtr, "file", 50 + progress / 2); // Scale from 50-100%
    }
}

void youtubeProgressCallback(int progress) {
    static SDL_Renderer* rendererPtr = nullptr;
    static TTF_Font* fontPtr = nullptr;
    
    // Store renderer and font pointers on first call
    if (rendererPtr == nullptr) {
        SDL_Window* window = SDL_GetWindowFromID(1); // Assuming window ID 1
        if (window) {
            rendererPtr = SDL_GetRenderer(window);
        }
    }
    
    if (fontPtr == nullptr) {
        // We need to recreate the font for this thread
        SDL_RWops* rw = SDL_RWFromConstMem(font_otf, sizeof(font_otf));
        fontPtr = TTF_OpenFontRW(rw, 1, 24);
    }
    
    if (rendererPtr && fontPtr) {
        renderLoadingScreen(rendererPtr, fontPtr, "youtube", progress);
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

    while (true) {
        // Determine which file to load
        std::string fileToLoad;
        bool shouldLoadFile = false;
        
        if (firstRun && fileProvided) {
            // On first run, use command line argument if provided
            fileToLoad = argv[1];
            shouldLoadFile = true;
            firstRun = false;
        } else if (reload_file_requested) {
            // --- Show Loading Screen BEFORE processing --- 
            renderLoadingScreen(renderer, font, "file", 0);
            // --- End Show Loading Screen ---
            // On reload, use filename from reload request
            fileToLoad = restart_filename;
            shouldLoadFile = true;
            reload_file_requested = false;
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
                                    nfdfilteritem_t filterItem[1] = { { "Video files", "mp4,mov,avi,mkv" } };
                                    nfdresult_t result = NFD::OpenDialog(outPath, filterItem, 1);
                                    if (result == NFD_OKAY) {
                                        std::string filename = outPath.get();
                                        std::cout << "Selected file: " << filename << std::endl;
                                        
                                        // Show loading screen before restarting player
                                        renderLoadingScreen(renderer, font, "file", 0);
                                        
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
                // isLoading, loadingType, loadingProgress
                renderOSD(renderer, font, false, 0.0, false, 0.0, 0, true, false, "", 25.0, false, false, true, "", 0);
                
                // Render a small hint at the center of the screen
                SDL_Color textColor = {200, 200, 200, 200};
                SDL_Surface* messageSurface = TTF_RenderText_Blended(font, "Press Ctrl+O to open a file", textColor);
                if (messageSurface) {
                    SDL_Texture* messageTexture = SDL_CreateTextureFromSurface(renderer, messageSurface);
                    if (messageTexture) {
                        int windowWidth, windowHeight;
                        SDL_GetRendererOutputSize(renderer, &windowWidth, &windowHeight);
                        
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
        }
        
        if (shouldLoadFile) {
            // Reset flags and states
            resetPlayerState();
            
            // --- Ensure window is visible and show loading screen if loading from command line --- 
            if (firstRun && fileProvided) { 
                 SDL_SetWindowTitle(window, "TapeXPlayer - Loading...");
                 renderLoadingScreen(renderer, font, "file", 0);
                 SDL_RenderPresent(renderer); // Force immediate display
                 firstRun = false; // Mark first run as handled
            }
            // --- End Loading Screen for Cmd Line --- 

            // Ensure audio is completely stopped
            cleanup_audio();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            
            // Process media source (file or URL)
            std::string processedFilePath;
            if (!processMediaSource(fileToLoad, processedFilePath, youtubeProgressCallback)) {
                std::cerr << "Failed to process media source: " << fileToLoad << std::endl;
                break;
            }
            
            // Use processed file path for further processing
            currentFilename = processedFilePath;

            if (currentFilename.empty()) {
                std::cerr << "No file selected. Exiting." << std::endl;
                log("No file selected. Exiting.");
                break;
            }

            // Handle file path for macOS
            if (currentFilename.find("/Volumes/") == 0) {
                // Path is already absolute, leave as is
            } else if (currentFilename[0] != '/') {
                // Relative path, add current directory
                char cwd[PATH_MAX];
                if (getcwd(cwd, sizeof(cwd)) != NULL) {
                    currentFilename = std::string(cwd) + "/" + currentFilename;
                } else {
                    std::cerr << "Error getting current directory" << std::endl;
                    break;
                }
            }

            std::cout << "Loading file: " << currentFilename << std::endl;
            log("Loading file: " + currentFilename);

            if (!std::filesystem::exists(currentFilename)) {
                std::cerr << "File not found: " << currentFilename << std::endl;
                log("Error: file not found: " + currentFilename);
                break;
            }

            // Update window title with filename
            std::string filename_only = std::filesystem::path(currentFilename).filename().string();
            std::string windowTitle = "TapeXPlayer - " + filename_only;
            SDL_SetWindowTitle(window, windowTitle.c_str());

            // Show loading indicator
            renderLoadingScreen(renderer, font, "file", 0);

            std::string lowResFilename = "low_res_output.mp4";

            std::vector<FrameInfo> frameIndex = createFrameIndex(currentFilename.c_str());
            std::cout << "Frame index created. Total frames: " << frameIndex.size() << std::endl;

            // --- Show Loading Screen: Indexing --- 
            renderLoadingScreen(renderer, font, "index", 0); // Using "index" type

            // --- Show Loading Screen: Converting --- 
            renderLoadingScreen(renderer, font, "convert", 0); // Show before starting conversion

            if (!LowResDecoder::convertToLowRes(currentFilename.c_str(), lowResFilename, convertProgressCallback)) {
                std::cerr << "Error converting video to low resolution" << std::endl;
                break;
            }
            
            // Update loading indicator for buffer initialization
            renderLoadingScreen(renderer, font, "buffer", 0); // Show before manager creation

            // Get video dimensions
            int videoWidth, videoHeight;
            get_video_dimensions(currentFilename.c_str(), &videoWidth, &videoHeight);

            // We no longer resize the window to match video dimensions
            // This ensures the window maintains its current size when loading a new file

            std::future<double> fps_future = std::async(std::launch::async, get_video_fps, currentFilename.c_str());
            std::future<double> duration_future = std::async(std::launch::async, get_file_duration, currentFilename.c_str());

            // --- Get FPS and Set Adaptive Window Size BEFORE manager creation ---
            original_fps.store(fps_future.get()); // Block until FPS is available
            double fps = original_fps.load();

            // int highResWindowSize = 600; // Default value (fallback) - Declare without default now
            int highResWindowSize;
            if (fps > 55.0) { // 60 FPS range (e.g., 59.94)
                // highResWindowSize = 2400; // Old value
                highResWindowSize = 1400; // New value
            } else if (fps > 45.0) { // 50 FPS range
                // highResWindowSize = 2000; // Old value
                highResWindowSize = 1200; // New value
            } else if (fps > 28.0) { // 30 FPS range (e.g., 29.97)
                // highResWindowSize = 1200; // Old value
                highResWindowSize = 700;  // New value
            } else { // <= 28 FPS (includes 24, 25)
                // highResWindowSize = 1000; // Old value
                highResWindowSize = 600;  // New value
            }
            std::cout << "[DEBUG] Set highResWindowSize to: " << highResWindowSize << std::endl; 

            // --- Calculate Adaptive Cached Segment Size based on FPS ---
            int adaptiveCachedSegmentSize;
            if (fps > 55.0) { // ~60 FPS
                adaptiveCachedSegmentSize = 3000;
            } else if (fps > 45.0) { // ~50 FPS
                adaptiveCachedSegmentSize = 2500;
            } else if (fps > 28.0) { // ~30 FPS
                adaptiveCachedSegmentSize = 1500;
            } else if (fps > 0) { // ~24/25 FPS or other
                adaptiveCachedSegmentSize = 1250; 
            } else { // Fallback if FPS is unknown
                adaptiveCachedSegmentSize = 2000; // Default fallback size
            }
            std::cout << "[DEBUG] Set adaptiveCachedSegmentSize to: " << adaptiveCachedSegmentSize << std::endl;

            bool enableHighResDecode = true;

            // Initialize audio
            bool audio_started = false;
            for (int attempt = 0; attempt < 3 && !audio_started; ++attempt) {
                std::cout << "Attempting to start audio (attempt " << attempt + 1 << " of 3)" << std::endl;
                
                // Make sure audio variables are reset
                decoding_finished.store(false);
                decoding_completed.store(false);
                audio_buffer.clear();
                audio_buffer_index = 0;
                
                audio_thread = std::thread([&currentFilename, &audio_started]() {
                    start_audio(currentFilename.c_str());
                });
                audio_thread.join();  // Wait for thread completion
                
                audio_started = !quit.load();  // If quit is not set, consider audio started successfully
                
                if (!audio_started) {
                    std::cout << "Audio failed to start, retrying..." << std::endl;
                    std::this_thread::sleep_for(std::chrono::seconds(1));  // Small pause before next attempt
                }
            }

            if (!audio_started) {
                std::cerr << "Failed to start audio after 3 attempts. Exiting." << std::endl;
                break;
            }

            speed_change_thread = std::thread(smooth_speed_change);

            bool fps_received = false;
            bool duration_received = false;

            previous_playback_rate.store(playback_rate.load());
            
            // Initialize atomics needed by managers
            std::atomic<int> currentFrame(0);
            std::atomic<bool> isPlaying(false); // Initialize as not playing

            // Define ring buffer capacity before creating managers
            const size_t ringBufferCapacity = 2000;

            // Pointers for managers, to handle potential exceptions during creation
            std::unique_ptr<FullResDecoderManager> fullResManagerPtr;
            std::unique_ptr<LowCachedDecoderManager> lowCachedManagerPtr;
            std::unique_ptr<CachedDecoderManager> cachedManagerPtr;

            try {
                // Create manager instances using unique_ptr
                
                fullResManagerPtr = std::make_unique<FullResDecoderManager>(
                    currentFilename, 
                    frameIndex, 
                    currentFrame, 
                    playback_rate, 
                    highResWindowSize, 
                    isPlaying,
                    is_reverse
                );
            
                lowCachedManagerPtr = std::make_unique<LowCachedDecoderManager>(
                    lowResFilename, 
                    frameIndex, 
                    currentFrame, 
                    ringBufferCapacity, 
                    highResWindowSize, 
                    isPlaying,
                    playback_rate,
                    is_reverse
                );
                
                // Create the CachedDecoderManager instance with adaptive size
                cachedManagerPtr = std::make_unique<CachedDecoderManager>(
                    lowResFilename, 
                    frameIndex,     
                    currentFrame,   
                    is_reverse,     
                    adaptiveCachedSegmentSize // Pass the calculated adaptive size
                );

                // Start the managers' internal threads
                if (fullResManagerPtr) fullResManagerPtr->run(); 
                if (lowCachedManagerPtr) lowCachedManagerPtr->run(); 
                if (cachedManagerPtr) cachedManagerPtr->run(); // Start the new manager

            } catch (const std::exception& e) {
                std::cerr << "Error creating decoder managers: " << e.what() << std::endl;
                log("Error creating decoder managers: " + std::string(e.what()));
                shouldExit = true; // Signal exit if managers fail
            } catch (...) {
                 std::cerr << "Unknown error creating decoder managers." << std::endl;
                 log("Unknown error creating decoder managers.");
                 shouldExit = true;
            }

            // Start playback after a short delay and manager setup
            std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Shorter delay maybe?
            // Ensure playback starts paused
            target_playback_rate.store(0.0); 
            playback_rate.store(0.0); // Ensure current rate is also 0
            isPlaying.store(false); // Ensure isPlaying is false

            // Variables for periodic timecode logging
            // auto lastTimecodeLogTime = std::chrono::steady_clock::now(); // Commented out
            // const std::chrono::seconds timecodeLogInterval(1); // Commented out

            while (!shouldExit) {
                // --- Periodic Timecode Logging --- 
                // auto currentTimePoint = std::chrono::steady_clock::now(); // Commented out
                // if (currentTimePoint - lastTimecodeLogTime >= timecodeLogInterval) { // Commented out
                //     std::cout << "[Main Loop] Current Timecode: " << get_current_timecode() // Commented out
                //               << ", Playback Rate: " << playback_rate.load() // Commented out
                //               << ", Is Reverse: " << is_reverse.load() << std::endl; // Commented out
                //     lastTimecodeLogTime = currentTimePoint; // Commented out
                // } // Commented out
                
                // Process remote commands at the start of each frame
                if (g_remote_control->is_initialized()) {
                    g_remote_control->process_commands();
                }

                // Check if duration is ready - Moved earlier
                if (!duration_received && duration_future.valid()) {
                    std::future_status status = duration_future.wait_for(std::chrono::seconds(0));
                    if (status == std::future_status::ready) {
                        total_duration.store(duration_future.get());
                        std::cout << "Total duration: " << total_duration.load() << " seconds" << std::endl;
                        duration_received = true;
                    }
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
                                    target_playback_rate.store(std::min(target_playback_rate.load() * 2.0, 18.0));
                                    break;
                                case SDLK_DOWN:
                                    target_playback_rate.store(std::max(target_playback_rate.load() / 2.0, 0.125));
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
                                        nfdfilteritem_t filterItem[1] = { { "Video files", "mp4,mov,avi,mkv" } };
                                        nfdresult_t result = NFD::OpenDialog(outPath, filterItem, 1);
                                        if (result == NFD_OKAY) {
                                            std::string filename = outPath.get();
                                            std::cout << "Selected file: " << filename << std::endl;
                                            
                                            // Show loading screen before restarting player
                                            renderLoadingScreen(renderer, font, "file", 0);
                                            
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
                                    if (zoom_enabled.load()) {
                                        reset_zoom();
                                    } else {
                                        zoom_enabled.store(true);
                                        zoom_factor.store(2.0f); // Initial zoom factor
                                    }
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
                            
                            if (extension == "mp4" || extension == "mov" || extension == "avi" || extension == "mkv") {
                                // Show loading screen before restarting player
                                renderLoadingScreen(renderer, font, "file", 0);
                                
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
                // int newCurrentFrame = 0; // Default to 0 - OLD CALCULATION
                // if (original_fps.load() > 0 && !frameIndex.empty()) { 
                //     newCurrentFrame = static_cast<int>(currentTime * original_fps.load());
                //     // Ensure frame index stays within bounds
                //     if (newCurrentFrame >= frameIndex.size()) {
                //         newCurrentFrame = frameIndex.size() - 1;
                //     }
                //     if (newCurrentFrame < 0) {
                //         newCurrentFrame = 0;
                //     }
                // }
                
                // --- NEW CALCULATION using time_ms --- 
                int64_t target_time_ms = static_cast<int64_t>(currentTime * 1000.0);
                int newCurrentFrame = findClosestFrameIndexByTime(frameIndex, target_time_ms);
                // Ensure frame index stays within bounds (redundant if findClosestFrameIndexByTime handles empty index)
                if (!frameIndex.empty()) {
                    newCurrentFrame = std::max(0, std::min(newCurrentFrame, static_cast<int>(frameIndex.size()) - 1));
                } else {
                    newCurrentFrame = 0;
                }
                // --- END NEW CALCULATION ---
                 
                 // Notify managers ONLY if the frame has actually changed
                 if (newCurrentFrame != currentFrame.load()) {
                     currentFrame.store(newCurrentFrame);
                     if (fullResManagerPtr) fullResManagerPtr->notifyFrameChange();
                     if (lowCachedManagerPtr) lowCachedManagerPtr->notifyFrameChange();
                     if (cachedManagerPtr) cachedManagerPtr->notifyFrameChange();
                 }

                // --- Frame Selection Logic --- (Moved from decoding threads)
                std::shared_ptr<AVFrame> frameToDisplay = nullptr;
                FrameInfo::FrameType frameTypeToDisplay = FrameInfo::EMPTY;
                double currentPlaybackRate = std::abs(playback_rate.load()); // Get current speed

                if (newCurrentFrame >= 0 && newCurrentFrame < frameIndex.size()) {
                    const FrameInfo& currentFrameInfo = frameIndex[newCurrentFrame];
                    // Use std::unique_lock for more control over locking
                    std::unique_lock<std::mutex> lock(currentFrameInfo.mutex);

                    if (!currentFrameInfo.is_decoding) { 
                        if (currentPlaybackRate <= 1.1) { // Speed <= 1.1x: Prefer Full_Res
                            if (currentFrameInfo.frame) { frameToDisplay = currentFrameInfo.frame; frameTypeToDisplay = FrameInfo::FULL_RES; }
                            else if (currentFrameInfo.low_res_frame) { frameToDisplay = currentFrameInfo.low_res_frame; frameTypeToDisplay = FrameInfo::LOW_RES; }
                            else if (currentFrameInfo.cached_frame) { frameToDisplay = currentFrameInfo.cached_frame; frameTypeToDisplay = FrameInfo::CACHED; }
                            lock.unlock(); // Unlock after accessing
                        } else { // Speed > 1.1x: NEVER use Full_Res. Prefer Low > Cached
                            if (currentFrameInfo.low_res_frame) { frameToDisplay = currentFrameInfo.low_res_frame; frameTypeToDisplay = FrameInfo::LOW_RES; }
                            else if (currentFrameInfo.cached_frame) { frameToDisplay = currentFrameInfo.cached_frame; frameTypeToDisplay = FrameInfo::CACHED; }
                            else { // Fallback: Search neighbors if Low/Cached not available
                                lock.unlock(); // Release lock *before* searching neighbors
                                bool isForward = playback_rate.load() >= 0;
                                int step = isForward ? 1 : -1;
                                int searchRange = 15; // Search range

                                for (int i = 1; i <= searchRange; ++i) { // Start from neighbor
                                    int checkFrameIdx = newCurrentFrame + (i * step);
                                    if (checkFrameIdx >= 0 && checkFrameIdx < frameIndex.size()) {
                                        // Lock neighbor briefly just to check
                                        std::lock_guard<std::mutex> search_lock(frameIndex[checkFrameIdx].mutex);
                                        if (!frameIndex[checkFrameIdx].is_decoding) { // Check if neighbor is not being decoded
                                            // Check low_res_frame first (preferred)
                                            if (frameIndex[checkFrameIdx].low_res_frame) {
                                                frameToDisplay = frameIndex[checkFrameIdx].low_res_frame;
                                                frameTypeToDisplay = FrameInfo::LOW_RES;
                                                break; // Found one
                                            } 
                                            // Then check cached_frame (fallback)
                                            else if (frameIndex[checkFrameIdx].cached_frame) {
                                                frameToDisplay = frameIndex[checkFrameIdx].cached_frame;
                                                frameTypeToDisplay = FrameInfo::CACHED;
                                                break; // Found one
                                            }
                                        }
                                    } else {
                                        break; // Out of bounds
                                    }
                                }
                                // frameToDisplay might still be nullptr if no suitable neighbor found
                            }
                            // If Low/Cached found initially, the lock was released implicitly. 
                            // Ensure lock is released if we fell through the neighbor search without finding anything.
                            if (lock.owns_lock()) { 
                                lock.unlock();
                            }
                        }
                    } else { // Frame is currently being decoded
                        lock.unlock(); // Unlock if frame is decoding
                        // frameToDisplay remains nullptr
                    }
                } else { // frameToDisplay remains nullptr if index is out of bounds
                    // --- End Frame Selection Logic ---
                    // frameToDisplay remains nullptr
                }

                // Check if seek is complete
                if (seekInfo.completed.load()) {
                    seekInfo.completed.store(false);
                    seek_performed.store(false);
                }
                displayFrame(renderer, frameIndex, newCurrentFrame, frameToDisplay, frameTypeToDisplay, // Pass selected frame and type
                            enableHighResDecode, playback_rate.load(), current_audio_time, total_duration,
                            showIndex, showOSD, font, isPlaying, is_reverse.load(), waiting_for_timecode,
                            input_timecode, original_fps, jog_forward, jog_backward,
                            ringBufferCapacity, highResWindowSize, 950);

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
                    // seek_performed.store(false); // Moved completion check earlier
                    // Notify managers immediately on seek request
                     if (fullResManagerPtr) fullResManagerPtr->notifyFrameChange();
                     if (lowCachedManagerPtr) lowCachedManagerPtr->notifyFrameChange();
                     if (cachedManagerPtr) cachedManagerPtr->notifyFrameChange();
                }

                // Make sure to stop and join the threads before exiting
                if (shouldExit) {
                    // Stop managers first (this will join their threads)
                    if (fullResManagerPtr) fullResManagerPtr->stop();
                    if (lowCachedManagerPtr) lowCachedManagerPtr->stop();
                    if (cachedManagerPtr) cachedManagerPtr->stop();
                    
                    // Also join other threads like speed_change_thread if they exist
                    if (speed_change_thread.joinable()) {
                        speed_change_thread.join();
                    }
                    break; // Exit the main loop
                }
            }

            // Ensure threads are joined on normal exit as well
            shouldExit = true; // Signal threads to stop (redundant, but safe)
             // Stop managers (this will join their threads)
            if (fullResManagerPtr) fullResManagerPtr->stop();
            if (lowCachedManagerPtr) lowCachedManagerPtr->stop();
            if (cachedManagerPtr) cachedManagerPtr->stop();
            
            if (speed_change_thread.joinable()) {
                speed_change_thread.join();
            }

            // Cleanup audio resources
            cleanup_audio();
            
            // Ensure audio is completely stopped and resources are released
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            // Clean up temporary files before loading new file
            cleanupTempFiles();
            
            // If there was a request to exit the program (not reload file), break the loop
            if (!reload_file_requested && !restart_requested) {
                break;
            }
        }
    }

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
