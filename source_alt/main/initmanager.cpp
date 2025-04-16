// Includes for functionality within this file
#include <string>
#include <fstream>
#include <filesystem>
#include <iostream> // For std::cout
#include <cstdlib> // For getenv
#include <atomic>
#include <vector>   // Potentially needed if SeekInfo uses vectors, include defensively
#include <memory>   // Potentially needed if SeekInfo uses smart pointers
#include <SDL2/SDL.h> // Added because WindowSettings uses SDL_WINDOWPOS_CENTERED
#include "initmanager.h"  // Include the header for function declarations in this file
#include "../common/common.h" // Still needed for SeekInfo definition
#include "main.h"       // Include main header for global variables and types
#include "core/decode/decode.h" // Needed for createFrameIndex, FrameInfo, get_video_dimensions, get_video_fps, get_file_duration
#include "core/decode/low_res_decoder.h" // Needed for LowResDecoder::convertToLowRes
#include "core/decode/full_res_decoder_manager.h" // Needed for FullResDecoderManager
#include "core/decode/low_cached_decoder_manager.h" // Needed for LowCachedDecoderManager
#include "core/decode/cached_decoder_manager.h" // Needed for CachedDecoderManager
#include <future> // Needed for std::async, std::future
#include <thread> // Needed for std::thread
#include <chrono> // Needed for std::chrono
#include <unistd.h> // Needed for getcwd
#include <limits.h> // Needed for PATH_MAX

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

// Function declaration at start of file
bool processMediaSource(const std::string& source, std::string& processedFilePath);
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
    
    // Clear audio buffer - REMOVED Block
    // if (!audio_buffer.empty()) { 
    //     audio_buffer.clear();
    // }
    audio_buffer_index = 0.0; // Reset the double index
    
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

// --- Main Loading Sequence Implementation (Asynchronous) ---
std::future<bool> mainLoadingSequence(
    SDL_Renderer* renderer, // renderer и window теперь параметры
    SDL_Window* window,
    LoadingStatus& loading_status_ref, // Added loading status reference
    const std::string& fileToLoad,
    std::string& currentFilename_out,
    std::vector<FrameInfo>& frameIndex_out,
    std::unique_ptr<FullResDecoderManager>& fullResMgr_out,
    std::unique_ptr<LowCachedDecoderManager>& lowCachedMgr_out,
    std::unique_ptr<CachedDecoderManager>& cachedMgr_out,
    std::atomic<int>& currentFrame_ref, // Используем ссылки на атомики
    std::atomic<bool>& isPlaying_ref)
{
    // Launch the loading sequence asynchronously
    return std::async(std::launch::async, 
        [&]() -> bool { // Capture all parameters by reference

            // Update status: Initializing
            { std::lock_guard<std::mutex> lock(loading_status_ref.stage_mutex); loading_status_ref.stage = "Initializing..."; }
            loading_status_ref.percent.store(0);

            // Ensure audio is completely stopped (using global function cleanup_audio)
            cleanup_audio();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            // Process media source (file or URL)
            std::string processedFilePath;
            { std::lock_guard<std::mutex> lock(loading_status_ref.stage_mutex); loading_status_ref.stage = "Processing source..."; }
            loading_status_ref.percent.store(5);
            if (!processMediaSource(fileToLoad, processedFilePath)) { // Assumes processMediaSource is accessible
                std::cerr << "Failed to process media source: " << fileToLoad << std::endl;
                return false; 
            }

            // Use processed file path for further processing
            currentFilename_out = processedFilePath; 

            if (currentFilename_out.empty()) {
                std::cerr << "No file selected. Exiting." << std::endl;
                log("No file selected. Exiting.");
                return false;
            }

            // Handle file path for macOS
            if (currentFilename_out.find("/Volumes/") == 0) { 
                /* aabsolute path */
            } else if (currentFilename_out[0] != '/') {
                char cwd[PATH_MAX];
                if (getcwd(cwd, sizeof(cwd)) != NULL) {
                    currentFilename_out = std::string(cwd) + "/" + currentFilename_out;
                } else {
                    std::cerr << "Error getting current directory" << std::endl;
                    return false;
                }
            }

            std::cout << "Loading file: " << currentFilename_out << std::endl;
            log("Loading file: " + currentFilename_out);

            if (!std::filesystem::exists(currentFilename_out)) {
                std::cerr << "File not found: " << currentFilename_out << std::endl;
                log("Error: file not found: " + currentFilename_out);
                return false;
            }

            // Update window title 
            std::string filename_only = std::filesystem::path(currentFilename_out).filename().string();
            std::string windowTitle = "TapeXPlayer - " + filename_only;
            // NOTE: SDL calls should ideally be on the main thread.
            // Consider passing title back or using SDL_CreateThreadSafeRenderer if needed.
            // For now, we risk potential issues calling this from a different thread.
            // SDL_SetWindowTitle(window, windowTitle.c_str()); // Moved to main thread after load

            std::string lowResFilename = "low_res_output.mp4"; 

            // Create frame index
            { std::lock_guard<std::mutex> lock(loading_status_ref.stage_mutex); loading_status_ref.stage = "Creating frame index..."; }
            loading_status_ref.percent.store(15);
            frameIndex_out = createFrameIndex(currentFilename_out.c_str());
            std::cout << "Frame index created. Total frames: " << frameIndex_out.size() << std::endl;

            // Convert to low-res
            { std::lock_guard<std::mutex> lock(loading_status_ref.stage_mutex); loading_status_ref.stage = "Converting to low-res..."; }
            loading_status_ref.percent.store(25);
            // Create callback lambda for convertToLowRes progress
            auto convertProgressCallback = [&](int progress) {
                loading_status_ref.percent.store(25 + (progress * 40 / 100)); // Scale 0-100 to 25-65%
            };
            if (!LowResDecoder::convertToLowRes(currentFilename_out.c_str(), lowResFilename, convertProgressCallback)) {
                std::cerr << "Error converting video to low resolution" << std::endl;
                return false;
            }

            // Get video dimensions
            int videoWidth, videoHeight;
            get_video_dimensions(currentFilename_out.c_str(), &videoWidth, &videoHeight);

            // Async get FPS and duration
            std::future<double> fps_future = std::async(std::launch::async, get_video_fps, currentFilename_out.c_str());
            // Duration future is handled in main loop now

            // Store FPS
            original_fps.store(fps_future.get());
            double fps = original_fps.load();

            // Calculate adaptive sizes
            int highResWindowSize;
             if (fps > 55.0) { highResWindowSize = 1400; }
             else if (fps > 45.0) { highResWindowSize = 1200; }
             else if (fps > 28.0) { highResWindowSize = 700; }
             else { highResWindowSize = 600; }
            std::cout << "[DEBUG] Set highResWindowSize to: " << highResWindowSize << std::endl;

            int adaptiveCachedSegmentSize;
            if (fps > 55.0) { adaptiveCachedSegmentSize = 3000; }
            else if (fps > 45.0) { adaptiveCachedSegmentSize = 2500; }
            else if (fps > 28.0) { adaptiveCachedSegmentSize = 1500; }
            else if (fps > 0) { adaptiveCachedSegmentSize = 1250; }
            else { adaptiveCachedSegmentSize = 2000; }
            std::cout << "[DEBUG] Set adaptiveCachedSegmentSize to: " << adaptiveCachedSegmentSize << std::endl;

            // Initialize audio
            bool audio_started = false;
            for (int attempt = 0; attempt < 3 && !audio_started; ++attempt) {
                std::cout << "Attempting to start audio (attempt " << attempt + 1 << " of 3)" << std::endl;
                decoding_finished.store(false);
                decoding_completed.store(false);
                audio_buffer_index = 0.0;
                // Running start_audio in its own thread within the async lambda
                std::thread local_audio_thread = std::thread([&]() {
                    start_audio(currentFilename_out.c_str());
                });
                local_audio_thread.join(); // Wait for this specific audio start attempt
                audio_started = !quit.load();
                if (!audio_started) {
                    std::cout << "Audio failed to start, retrying..." << std::endl;
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
            }

            if (!audio_started) {
                std::cerr << "Failed to start audio after 3 attempts. Exiting." << std::endl;
                return false;
            }

            // Start speed change thread
            speed_change_thread = std::thread(smooth_speed_change);

            { std::lock_guard<std::mutex> lock(loading_status_ref.stage_mutex); loading_status_ref.stage = "Initializing decoders..."; }
            loading_status_ref.percent.store(85);
            // Initialize managers
            previous_playback_rate.store(playback_rate.load());
            currentFrame_ref.store(0);   
            isPlaying_ref.store(false);
            const size_t ringBufferCapacity = 2000;

            try {
                fullResMgr_out = std::make_unique<FullResDecoderManager>(
                    currentFilename_out, frameIndex_out, currentFrame_ref, playback_rate,
                    highResWindowSize, isPlaying_ref, is_reverse
                );
                lowCachedMgr_out = std::make_unique<LowCachedDecoderManager>(
                    lowResFilename, frameIndex_out, currentFrame_ref, ringBufferCapacity,
                    highResWindowSize, isPlaying_ref, playback_rate, is_reverse
                );
                cachedMgr_out = std::make_unique<CachedDecoderManager>(
                    lowResFilename, frameIndex_out, currentFrame_ref, is_reverse,
                    adaptiveCachedSegmentSize
                );

                // Start the managers' internal threads - MOVED TO MAIN THREAD
                // if (fullResMgr_out) fullResMgr_out->run(); 
                // if (lowCachedMgr_out) lowCachedMgr_out->run(); 
                // if (cachedMgr_out) cachedMgr_out->run();

            } catch (const std::exception& e) {
                std::cerr << "Error creating decoder managers: " << e.what() << std::endl;
                log("Error creating decoder managers: " + std::string(e.what()));
                return false;
            } catch (...) {
                 std::cerr << "Unknown error creating decoder managers." << std::endl;
                 log("Unknown error creating decoder managers.");
                 return false;
            }

            { std::lock_guard<std::mutex> lock(loading_status_ref.stage_mutex); loading_status_ref.stage = "Finalizing..."; }
            loading_status_ref.percent.store(100);
            return true; // Loading successful
        }
    );
}