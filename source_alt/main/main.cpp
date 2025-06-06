#include "includes.h"

// All global variables and declarations are now in globals.h and globals.cpp

// Functions to access playback rate variables
std::atomic<double>& get_playback_rate() {
    return playback_rate;
}

std::atomic<double>& get_target_playback_rate() {
    return target_playback_rate;
}

void log(const std::string& message) {
    static std::ofstream logFile("/tmp/TapeXPlayer.log", std::ios::app);
    if (logFile.is_open()) {
    logFile << message << std::endl;
    } else {
        std::cerr << "[LOG ERROR] Could not open /tmp/TapeXPlayer.log. Message: " << message << std::endl;
    }
}

std::string get_current_timecode() {
    static char timecode[12];  // HH:MM:SS:FF\0
    
    // Get current playback time in seconds
    double current_time = current_audio_time.load();
    
    // Use real video FPS
    double fps = original_fps.load();
    if (fps <= 0) fps = 30.0;  // Use 30 as default value if FPS is not yet defined
    
    // Direct calculation from time to components (same as generateTXTimecode)
    int hours = static_cast<int>(current_time / 3600.0);
    current_time -= hours * 3600.0;
    
    int minutes = static_cast<int>(current_time / 60.0);
    current_time -= minutes * 60.0;
    
    int seconds = static_cast<int>(current_time);
    double fractional_seconds = current_time - seconds;
    
    // Calculate frames from fractional seconds
    int frames = static_cast<int>(fractional_seconds * fps);
    
    // Ensure frames are within valid range [0, fps-1]
    if (frames >= static_cast<int>(fps)) {
        frames = static_cast<int>(fps) - 1;
    }
    
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

// --- Helper functions for link generation --- 

// Simple function for URL-encoding a path
// Replaces spaces with %20 and some other basic characters if necessary.
// For a more complete implementation, a library could be used, but this is usually sufficient for paths.
std::string urlEncodePath(const std::string& path) {
    std::ostringstream encoded;
    // encoded << std::hex; // WAS COMMENTED OUT, NEEDS TO BE UNCOMMENTED AND USED CORRECTLY

    for (char c : path) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
            encoded << c;
        } else {
            // Use std::hex for hexadecimal output
            encoded << '%' << std::setw(2) << std::setfill('0') << std::hex << std::uppercase << static_cast<int>(static_cast<unsigned char>(c));
        }
    }
    return encoded.str();
}

// Formats time as HHMMSSFF
std::string formatTimeForFstpUrl(double time_in_seconds, double fps_val) {
    if (time_in_seconds < 0) time_in_seconds = 0;
    if (fps_val <= 0) fps_val = 25.0; // Safe default value

    // Direct calculation from time to components (same as generateTXTimecode)
    int hours = static_cast<int>(time_in_seconds / 3600.0);
    time_in_seconds -= hours * 3600.0;
    
    int minutes = static_cast<int>(time_in_seconds / 60.0);
    time_in_seconds -= minutes * 60.0;
    
    int seconds = static_cast<int>(time_in_seconds);
    double fractional_seconds = time_in_seconds - seconds;
    
    // Calculate frames from fractional seconds
    int frames = static_cast<int>(fractional_seconds * fps_val);
    
    // Ensure frames are within valid range [0, fps-1]
    if (frames >= static_cast<int>(fps_val)) {
        frames = static_cast<int>(fps_val) - 1;
    }

    std::ostringstream time_stream;
    time_stream << std::setw(2) << std::setfill('0') << hours
                << std::setw(2) << std::setfill('0') << minutes
                << std::setw(2) << std::setfill('0') << seconds
                << std::setw(2) << std::setfill('0') << frames;
    return time_stream.str();
}

// Main function for generation and copying
void generateAndCopyFstpMarkdownLink() {
    if (g_currentOpenFilePath.empty()) {
        std::cout << "Cannot copy fstp link: No file is currently open." << std::endl;
        // Could add OSD error message later
        return;
    }

    std::string filePath = g_currentOpenFilePath;
    double currentTime = current_audio_time.load();
    double fps = original_fps.load();

    std::cout << "[DEBUG URL Encode] Path BEFORE encoding: " << filePath << std::endl; // <--- NEW LOG
    std::string encodedPath = urlEncodePath(filePath);
    std::cout << "[DEBUG URL Encode] Path AFTER encoding: " << encodedPath << std::endl; // <--- NEW LOG

    std::string timeParam = formatTimeForFstpUrl(currentTime, fps);
    std::string displayTime = get_current_timecode(); // Use existing HH:MM:SS:FF

    // Build URL. Ensure absolute paths start with /// if they are absolute.
    std::string fstpUrl;
    if (!encodedPath.empty() && encodedPath[0] == '/') {
        fstpUrl = "fstp:///" + encodedPath.substr(1); // Replace first / with ///
    } else {
        // For relative paths or if path is already formatted somehow (unlikely for g_currentOpenFilePath)
        fstpUrl = "fstp://" + encodedPath; 
    }
    fstpUrl += "&t=" + timeParam;

    // std::string markdownLink = "[" + fstpUrl + "|" + displayTime + "]"; // Old format
    std::string markdownLink = "[" + displayTime + "](" + fstpUrl + ")"; // New format

    if (SDL_SetClipboardText(markdownLink.c_str()) == 0) {
        std::cout << "Copied to clipboard: " << markdownLink << std::endl;
        // Could add OSD success message
    } else {
        std::cerr << "Error copying to clipboard: " << SDL_GetError() << std::endl;
        // Could add OSD error message
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

void restartPlayerWithFile(const std::string& filename, double seek_after_load_time) {
    if (g_currentOpenFilePath == filename && !g_currentOpenFilePath.empty()) {
        std::cout << "Request to open the same file that is already open: " << filename << ". No action." << std::endl;
        // If a seek time is provided for the *same* currently open file, we should handle it here directly
        // This case is now primarily handled by processIncomingFstpUrl, but good to be aware.
        // If seek_after_load_time >= 0, perhaps we should call seek_to_time(seek_after_load_time) ?
        // For now, the logic in processIncomingFstpUrl will handle immediate seek for same file.
        // This function is more about *reloading*.
        return; 
    }
    std::cout << "Restarting player with file: " << filename 
              << (seek_after_load_time >=0 ? " and will seek to " + std::to_string(seek_after_load_time) +"s after load" : "") << std::endl;
    log("Restarting player with file: " + filename);
    
#ifdef __APPLE__
    std::cout << "[main.cpp] Before updateCopyLinkMenuState(false) in restartPlayerWithFile" << std::endl;
    updateCopyLinkMenuState(false); // Deactivate before reload
    updateCopyScreenshotMenuState(false); // Deactivate screenshot before reload
    std::cout << "[main.cpp] After updateCopyLinkMenuState(false) in restartPlayerWithFile" << std::endl;
#endif
    restart_filename = filename;
    g_seek_after_next_load_time = seek_after_load_time; // Store the seek time
    reload_file_requested.store(true); 
    shouldExit.store(true); // Signal inner loop to exit
    restart_requested = false; 
}

int main(int argc, char* argv[]) {
    // Store the program path for potential relaunch
    argv0 = argv[0];
    
    restartArgs.clear();
    for (int i = 0; i < argc; ++i) {
        restartArgs.push_back(argv[i]);
    }

    // ParsedFstpUrl urlToOpen; // <--- REPLACED with variables below
    std::string pathFromUrl;
    double timeFromUrl = -1.0;
    bool openFileFromUrl = false;
    bool seekFromFileUrl = false;
    bool fstpUrlProcessed = false;

    std::string initialPathFromArgs; 

    if (argc > 1) {
        for (int i = 1; i < argc; ++i) {
            std::string arg_str = argv[i];
            // Using g_currentOpenFilePath, but it's still empty at startup.
            // original_fps.load() will also be 0.0 at startup.
            if (handleFstpUrlArgument(arg_str, g_currentOpenFilePath, original_fps.load(), pathFromUrl, timeFromUrl, openFileFromUrl, seekFromFileUrl)) {
                fstpUrlProcessed = true;
                // Log the direct outputs of handleFstpUrlArgument
                log("[FSTP DEBUG] handleFstpUrlArgument returned true. Outputs: pathFromUrl='" + pathFromUrl +
                    "', timeFromUrl=" + std::to_string(timeFromUrl) +
                    ", openFileFromUrl=" + (openFileFromUrl ? "true" : "false") +
                    ", seekFromFileUrl=" + (seekFromFileUrl ? "true" : "false") +
                    ", for input arg_str='" + arg_str + "'");
                // If URL is processed (even if invalid), exit the loop,
                // as we don't want it to be interpreted as a regular file path.
                break; 
            } else {
                // This is not an fstp URL, check if it's a regular path
                if (arg_str.length() > 0 && arg_str[0] != '-' && initialPathFromArgs.empty()) {
                     initialPathFromArgs = arg_str;
                     std::cout << "Found potential file path argument (non-fstp): " << initialPathFromArgs << std::endl;
                }
            }
        }
    }
    
    std::string video_path_to_load;
    double time_to_seek = -1.0;
    bool should_load_new_file_from_args = false;

    if (fstpUrlProcessed) {
        // If an FSTP URL was processed and it provided a file path, prioritize loading it.
        if (!pathFromUrl.empty()) {
            video_path_to_load = pathFromUrl;
            should_load_new_file_from_args = true; // Force loading if path is present
            if (seekFromFileUrl) { // If a seek is also requested for this file
                time_to_seek = timeFromUrl;
            }
            std::cout << "[FSTP Startup] Preparing to load from fstp URL: " << video_path_to_load
                      << (time_to_seek >= 0 ? " and seek to " + std::to_string(time_to_seek) + "s" : "") << std::endl;
        } else if (seekFromFileUrl) {
            // FSTP URL provided NO path, but asked for a seek (e.g., fstp:&t=...). 
            // At startup, no file is loaded yet from the URL itself, so this seek is for an unspecified context.
            // We won't load a file based on this, but we'll store the seek time if it becomes relevant later.
            time_to_seek = timeFromUrl;
            std::cout << "[FSTP Startup] URL requests seek to: " << time_to_seek
                      << "s, but no file path was specified in the URL. No file will be loaded based on this URL." << std::endl;
        } else {
            // FSTP URL was processed, but it's invalid or has no path and no seek.
            std::cout << "[FSTP Startup] URL processed, but no file path or seek action required from it." << std::endl;
        }
    } else if (!initialPathFromArgs.empty()) {
        // This handles the case where a non-FSTP command line argument (a direct file path) was given.
        video_path_to_load = initialPathFromArgs;
        should_load_new_file_from_args = true;
        std::cout << "[Argument Startup] Preparing to load from command line argument: " << video_path_to_load << std::endl;
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
    // Изначально пункт меню неактивен, если не будет загрузки из аргументов
    // Это будет переопределено ниже, если should_load_new_file_from_args станет true
    std::cout << "[main.cpp] Before initial updateCopyLinkMenuState(false) in main()" << std::endl;
    updateCopyLinkMenuState(false); 
    updateCopyScreenshotMenuState(false);
    std::cout << "[main.cpp] After initial updateCopyLinkMenuState(false) in main()" << std::endl;
#endif

    // Create managers
    WindowManager windowManager;
    KeyboardManager keyboardManager;
    DeepPauseManager deepPauseManager;
    
    // Set global pointers
    g_keyboardManager = &keyboardManager;
    g_windowManager = &windowManager;
    g_deepPauseManager = &deepPauseManager;
    
    // Load saved window settings
    WindowSettings windowSettings = loadWindowSettings();
    
    // Use saved settings if available, otherwise use default dimensions
    int windowWidth = windowSettings.isValid ? windowSettings.width : 1280;
    int windowHeight = windowSettings.isValid ? windowSettings.height : 720;
    int windowX = windowSettings.isValid ? windowSettings.x : SDL_WINDOWPOS_CENTERED;
    int windowY = windowSettings.isValid ? windowSettings.y : SDL_WINDOWPOS_CENTERED;
    bool isFullscreen = windowSettings.isValid && windowSettings.isFullscreen;
    
    // Initialize window manager
    if (!windowManager.initialize("TapeXPlayer", windowX, windowY, windowWidth, windowHeight, isFullscreen)) {
        std::cerr << "Failed to initialize window manager" << std::endl;
        return 1;
    }
    
    // Initialize deep pause manager
    deepPauseManager.setThreshold(std::chrono::seconds(5)); // 5 second threshold
    
    // Check initial window focus state
    window_has_focus.store(windowManager.hasInputFocus());
    
    // Get SDL objects from window manager for backward compatibility
    window = windowManager.getWindow(); // Set global window pointer
    SDL_Renderer* renderer = windowManager.getRenderer();
    TTF_Font* font = windowManager.getFont();

    // Main file loading loop
    std::string currentFilename; // Используется для передачи имени файла в/из mainLoadingSequence
    // g_currentOpenFilePath будет обновляться после успешной загрузки
    
    bool firstRun = true;
    // bool fileProvided = (argc > 1); // Эта логика теперь сложнее из-за fstp
    bool fileArgProcessed = false; // Флаг, что аргумент командной строки (URL или путь) обработан

    // --- Define fixed speed steps ---
    const std::vector<double> speed_steps = {0.5, 1.0, 3.0, 10.0, 24.0};
    static int current_speed_index = 1; // Start at 1.0x (index 1)

    while (true) {
        std::string fileToLoadPath; // Путь к файлу для загрузки в этой итерации
        bool shouldAttemptFileLoadThisIteration = false;
        double initialSeekTimeForThisLoad = -1.0; // Время для перемотки *после* загрузки этого файла

        if (!fileArgProcessed && should_load_new_file_from_args) {
            fileToLoadPath = video_path_to_load; // From URL or regular argument
            initialSeekTimeForThisLoad = time_to_seek; // Stored time from URL
            shouldAttemptFileLoadThisIteration = true;
            fileArgProcessed = true; // Command line arguments processed
            firstRun = false;
        } else if (firstRun && fstpUrlProcessed && !openFileFromUrl && seekFromFileUrl) {
            // Special case: URL at startup pointed to current (hypothetically) file and requested seek.
            // File is not loaded yet, so fileToLoadPath should be empty to show "no file".
            // Seek will be applied if user opens *this* file later manually.
            // Or if we knew beforehand which file is "current" before first load.
            // For now, if file is not opened by URL but there's seek, we don't apply it on empty player.
            // This block may need refinement if we need to open *last* opened file and seek.
            // Current logic: if URL didn't lead to opening a file, we do nothing at startup.
            fileArgProcessed = true; // URL обработан.
            firstRun = false;
        }
        else if (reload_file_requested.load()) {
            fileToLoadPath = restart_filename; // File name from reload request
            initialSeekTimeForThisLoad = g_seek_after_next_load_time; // Use the stored time for next load
            g_seek_after_next_load_time = -1.0; // Reset it after use
            shouldAttemptFileLoadThisIteration = true;
            reload_file_requested.store(false);
            firstRun = false; // If there was a reload, this is no longer the first run in the context of arguments
        } else if (firstRun) {
            // First run, but there were no valid arguments for automatic loading
            firstRun = false;
            // shouldAttemptFileLoadThisIteration остается false
            
            windowManager.setTitle("TapeXPlayer - No File Loaded");
            windowManager.clear(0, 0, 0, 255);
            windowManager.endFrame();
        }
        
        if (!shouldAttemptFileLoadThisIteration) {
#ifdef __APPLE__
            std::cout << "[main.cpp] Before updateCopyLinkMenuState(false) in no-file loop" << std::endl;
            updateCopyLinkMenuState(false); // <--- ВЫКЛЮЧИТЬ в цикле "нет файла"
            updateCopyScreenshotMenuState(false); // <--- ВЫКЛЮЧИТЬ скриншот в цикле "нет файла"
            std::cout << "[main.cpp] After updateCopyLinkMenuState(false) in no-file loop" << std::endl;
#endif
            bool noFileLoaded = true;
            
            while (noFileLoaded && !shouldExit) {
                // Check for pending FSTP URL at the beginning of the loop
                if (g_hasPendingFstpUrl.load()) {
                    log("[main.cpp] Detected pending FSTP URL in noFileLoaded loop: " + g_pendingFstpUrlPath);
                    if (!g_pendingFstpUrlPath.empty()) {
                        restart_filename = g_pendingFstpUrlPath;
                        g_seek_after_next_load_time = g_pendingFstpUrlTime;
                        
                        reload_file_requested.store(true); // Signal the outer loop to load this file
                        noFileLoaded = false; // Exit this "no file loaded" loop
                    }
                    // Reset pending URL state
                    g_hasPendingFstpUrl.store(false);
                    g_pendingFstpUrlPath.clear();
                    g_pendingFstpUrlTime = -1.0;
                    continue; // Re-evaluate while condition; should exit this loop and proceed to load
                }

                windowManager.beginFrameTiming();
                Uint32 frameStart = SDL_GetTicks(); // Keep for compatibility
                
                // Process remote commands at the start of each frame
                if (g_remote_control->is_initialized()) {
                    g_remote_control->process_commands();
                }
                
                // Handle fullscreen toggle request from menu
                if (toggle_fullscreen_requested.load()) {
                    toggle_fullscreen_requested.store(false);
                    windowManager.toggleFullscreen();
                }
                
                windowManager.processEvents([&](SDL_Event& e) {
                    if (e.type == SDL_QUIT) {
                        saveWindowSettings(window);
                        quit = true;
                        shouldExit = true;
                        restart_requested = false; // Cancel restart on window close
                    } else if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
                        keyboardManager.handleKeyboardEvent(e);
                    } else if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP || e.type == SDL_MOUSEMOTION) {
                        keyboardManager.handleMouseEvent(e);
                    }
                });
                
                // Render no file screen
                windowManager.renderNoFileScreen();
                windowManager.renderOSD(false, 0.0, false, 0.0, 0, true, false, "", 25.0, false, false, FrameInfo::EMPTY);
                windowManager.endFrame();
                
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
        
        if (shouldAttemptFileLoadThisIteration) {
            resetPlayerState(); // Сброс состояния перед загрузкой нового файла
            
            // currentFilename здесь будет обновлен функцией mainLoadingSequence
            // g_currentOpenFilePath будет обновлен *после* успешной загрузки
            
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
                loadingStatus, 
                fileToLoadPath, // Используем fileToLoadPath
                currentFilename, // currentFilename будет ЗАПОЛНЕН функцией
                frameIndex, fullResManagerPtr, lowCachedManagerPtr, cachedManagerPtr,
                currentFrame, isPlaying
            );

            // --- Show Loading Screen while waiting for the future --- 
            windowManager.setTitle("TapeXPlayer - Loading..."); // Set loading title
            while (loading_future.wait_for(std::chrono::milliseconds(10)) != std::future_status::ready) {
                // Render loading screen
                windowManager.renderLoadingScreen(const_cast<const LoadingStatus&>(loadingStatus));

                // Handle essential events (like SDL_QUIT) during loading
                windowManager.processEvents([&](SDL_Event& e) {
                    if (e.type == SDL_QUIT) {
                        // Need to signal the loading thread to stop if possible
                        // This requires adding stop logic to mainLoadingSequence/its sub-tasks
                        // For now, just set quit flags and break loops
                        quit = true;
                        shouldExit = true; 
                        // Optionally try to cancel the future if the task supports cancellation
                    } 
                    // Handle other minimal events if necessary
                });
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
            g_currentOpenFilePath = currentFilename; // <--- ОБНОВЛЯЕМ путь к текущему открытому файлу
            std::string filename_only = std::filesystem::path(g_currentOpenFilePath).filename().string();
            std::string windowTitle = "TapeXPlayer - " + filename_only;
            windowManager.setTitle(windowTitle);

#ifdef __APPLE__
            std::cout << "[main.cpp] Before updateCopyLinkMenuState(true) after successful load" << std::endl;
            updateCopyLinkMenuState(true); // <--- Enable the menu item
            updateCopyScreenshotMenuState(true); // <--- Enable the screenshot menu item
            std::cout << "[main.cpp] After updateCopyLinkMenuState(true) after successful load" << std::endl;
#endif

             // --- Start Decoder Manager Threads (after successful load and no quit signal) ---
                if (fullResManagerPtr) {
                    fullResManagerPtr->run(); 
                    // Initial check based on starting window size
                    int initialWidth, initialHeight;
                    windowManager.getWindowSize(initialWidth, initialHeight);
                    fullResManagerPtr->checkWindowSizeAndToggleActivity(initialWidth, initialHeight);
                }
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

            // Perform initial seek if requested (e.g., from URL)
            if (initialSeekTimeForThisLoad >= 0.0) {
                std::cout << "[main.cpp] Performing initial seek to: " << initialSeekTimeForThisLoad << "s after successful load." << std::endl;
                log("[main.cpp] Performing initial seek to: " + std::to_string(initialSeekTimeForThisLoad) + "s after successful load.");
                seek_to_time(initialSeekTimeForThisLoad);
            }

             // Frame selection state
            bool forceFrameUpdate = false; // Force frame selection after seek
            
             // --- Start of the inner playback loop ---
            while (!shouldExit) {
                // Process remote commands at the start of each frame
                if (g_remote_control->is_initialized()) {
                    g_remote_control->process_commands();
                }
                
                // Update deep pause manager
                deepPauseManager.update(playback_rate.load(), target_playback_rate.load(), window_has_focus.load());
                
                // Add delay when in Deep Pause to reduce CPU usage
                if (deepPauseManager.isActive() && !deepPauseManager.shouldInterruptForRefresh()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(deepPauseManager.getDeepPauseSleepTime()));
                }
                
                // Check if we need to reset the speed threshold
                check_and_reset_threshold();

                // Handle fullscreen toggle request from menu
                if (toggle_fullscreen_requested.load()) {
                    toggle_fullscreen_requested.store(false);
                    windowManager.toggleFullscreen();
                }

                windowManager.beginFrameTiming();
                Uint32 frameStart = SDL_GetTicks(); // Keep for compatibility

                windowManager.processEvents([&](SDL_Event& e) {
                    if (e.type == SDL_QUIT) {
                        saveWindowSettings(window);
                        quit = true;
                        shouldExit = true;
                        restart_requested = false; // Cancel restart on window close
                        if(deepPauseManager.isActive()) deepPauseManager.forceExit();
                    } else if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
                        keyboardManager.handleKeyboardEvent(e);
                    } else if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP || e.type == SDL_MOUSEMOTION) {
                        keyboardManager.handleMouseEvent(e);
                    } else if (e.type == SDL_WINDOWEVENT) {
                        if (e.window.event == SDL_WINDOWEVENT_RESIZED) {
                            int windowWidth, windowHeight;
                            windowManager.getWindowSize(windowWidth, windowHeight);
                            // Notify FullResDecoderManager about the resize
                            if (fullResManagerPtr) {
                                fullResManagerPtr->checkWindowSizeAndToggleActivity(windowWidth, windowHeight);
                            }
                        } else if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                            window_has_focus.store(true);
                            // Exit deep pause when window gets focus
                            if (deepPauseManager.isActive()) {
                                deepPauseManager.forceExit();
                            }
                        } else if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                            window_has_focus.store(false);
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
                                restartPlayerWithFile(filename, -1.0);
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
                            windowManager.handleZoomMouseEvent(e, frameWidth, frameHeight);
                        }
                    }
                });

                // Update currentFrame based on current audio time
                double currentTime = current_audio_time.load();
                int64_t target_time_ms = static_cast<int64_t>(currentTime * 1000.0);
                int newCurrentFrame = findClosestFrameIndexByTime(frameIndex, target_time_ms);
                 if (!frameIndex.empty()) { newCurrentFrame = std::max(0, std::min(newCurrentFrame, static_cast<int>(frameIndex.size()) - 1)); } else { newCurrentFrame = 0; }
                 
                 // REMOVED: Frame synchronization - let the natural timestamps work as intended
                 
                 // Notify managers ONLY if the frame has actually changed
                 if (newCurrentFrame != currentFrame.load()) {
                     currentFrame.store(newCurrentFrame);
                     if (fullResManagerPtr) fullResManagerPtr->notifyFrameChange();
                     if (lowCachedManagerPtr) lowCachedManagerPtr->notifyFrameChange();
                     if (cachedManagerPtr) cachedManagerPtr->notifyFrameChange();
                 }

                // Deep pause is now handled by deepPauseManager.update() call above

                // --- Frame Selection Logic ---
                auto frameSelection = windowManager.selectFrame(frameIndex, newCurrentFrame, playback_rate.load(), forceFrameUpdate);
                std::shared_ptr<AVFrame> frameToDisplay = frameSelection.frame;
                FrameInfo::FrameType frameTypeToDisplay = frameSelection.frameType;
                
                // Reset force update flag after successful frame selection
                if (forceFrameUpdate && frameSelection.frameFound) {
                    forceFrameUpdate = false;
                }

                // Check if seek is complete
                if (seekInfo.completed.load()) {
                    seekInfo.completed.store(false);
                    seek_performed.store(false);
                }

                // --- Determine highResWindowSize and ringBufferCapacity ---
                auto decoderParams = WindowManager::calculateDecoderParams(original_fps.load());
                int highResWindowSize = decoderParams.highResWindowSize;
                const size_t ringBufferCapacity = decoderParams.ringBufferCapacity;

                // --- Update currentDisplayAspectRatio from the decoder ---
                if (fullResManagerPtr && fullResManagerPtr->getDecoder()) {
                    currentDisplayAspectRatio = fullResManagerPtr->getDecoder()->getDisplayAspectRatio();
                } else {
                    // Fallback or if no decoder (e.g. no file loaded), keep default or set a sane default
                    // For instance, if no file is loaded, this part of the loop might not even be reached,
                    // but as a safety, we can ensure it's a common aspect ratio.
                    // The global currentDisplayAspectRatio is already 16.0f/9.0f by default.
                }

                windowManager.displayFrame(frameIndex, newCurrentFrame, frameToDisplay, frameTypeToDisplay,
                            true, // enableHighResDecode - Assuming true for now
                             playback_rate.load(), current_audio_time.load(), total_duration.load(),
                            showIndex, showOSD, isPlaying, is_reverse.load(), waiting_for_timecode,
                            input_timecode, original_fps.load(), jog_forward, jog_backward,
                            ringBufferCapacity, highResWindowSize, 950, // Pass calculated sizes
                            currentDisplayAspectRatio);

                // Handle screenshot request
                if (screenshot_requested.load() && frameToDisplay) {
                    screenshot_requested.store(false);
                    
                    // Generate timecode for current frame
                    std::string currentTimecode = get_current_timecode();
                    
                    // Get window dimensions
                    int windowWidth, windowHeight;
                    SDL_GetRendererOutputSize(renderer, &windowWidth, &windowHeight);
                    
                    // Take advanced screenshot with zoom and thumbnail support
                    bool success = takeAdvancedScreenshotWithTimecode(
                        frameToDisplay.get(), 
                        currentTimecode, 
                        windowWidth, 
                        windowHeight,
                        zoom_enabled.load(),
                        zoom_factor.load(),
                        zoom_center_x.load(),
                        zoom_center_y.load(),
                        show_zoom_thumbnail.load()
                    );
                    
                    if (success) {
                        std::cout << "[Screenshot] Advanced screenshot copied to clipboard successfully!" << std::endl;
                        std::cout << "[Screenshot] Frame: " << newCurrentFrame << ", Timecode: " << currentTimecode << std::endl;
                        if (zoom_enabled.load()) {
                            std::cout << "[Screenshot] Zoom: " << zoom_factor.load() << "x at (" 
                                      << zoom_center_x.load() << "," << zoom_center_y.load() << ")" << std::endl;
                        }
                    } else {
                        std::cerr << "[Screenshot] Failed to copy advanced screenshot to clipboard" << std::endl;
                    }
                }

                // Calculate frame delay based on mode
                int currentTargetFPS = TARGET_FPS;
                if (deepPauseManager.isActive() && !deepPauseManager.shouldInterruptForRefresh() && !zoom_enabled.load()) {
                    currentTargetFPS = DEEP_PAUSE_RENDER_FPS;
                }
                
                // Set target FPS in window manager
                windowManager.setTargetFPS(currentTargetFPS);

                // Skip frame delay if zoom is enabled or if we just performed a seek
                if (!zoom_enabled.load() && !seek_performed.load()) {
                    windowManager.endFrameTiming();
                }

                // Handle seek request
                if (seek_performed.load()) {
                    seekInfo.time.store(current_audio_time.load());
                    seekInfo.requested.store(true);
                    seekInfo.completed.store(false);
                    // Reset frame transition state after seek to ensure best quality frame is selected
                    windowManager.resetFrameSelection();
                    forceFrameUpdate = true;
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
#ifdef __APPLE__
            std::cout << "[main.cpp] Before updateCopyLinkMenuState(false) in playback loop cleanup" << std::endl;
            updateCopyLinkMenuState(false); // <--- ВЫКЛЮЧИТЬ перед очисткой и выходом из этого цикла загрузки
            updateCopyScreenshotMenuState(false); // <--- ВЫКЛЮЧИТЬ скриншот перед очисткой
            std::cout << "[main.cpp] After updateCopyLinkMenuState(false) in playback loop cleanup" << std::endl;
#endif
            std::cout << "[Cleanup] Stopping managers..." << std::endl;
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
            
            // If there was a request to exit the program (not reload), break the outer loop
            if (!reload_file_requested.load() && !restart_requested) {
                 // Ensure shouldExit is true so the outer loop terminates correctly
                 shouldExit = true; // Set explicitly for clarity
                 break; // Break the outer while(true) loop
            }
            // If reload or restart was requested, the outer loop will continue,
            // and reload_file_requested will be handled at the beginning.
        } // End if (shouldAttemptFileLoadThisIteration)
    } // End outer while(true) loop

    // Save final window settings before closing
    saveWindowSettings(window);

    // Cleanup display resources (including Metal) BEFORE destroying SDL renderer/window
    cleanupDisplayResources();

    // Free TTF resources
    // WindowManager will cleanup SDL resources in its destructor
    // Including font cleanup

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

// Function called from Objective-C (via menu_system.h bridge) when app opens an fstp:// URL
void processIncomingFstpUrl(const char* url_c_str) {
    log("[FSTP Event] processIncomingFstpUrl called with URL: " + std::string(url_c_str));

    std::string urlString(url_c_str);
    std::cout << "[main.cpp] processIncomingFstpUrl received: " << urlString << std::endl;
    log("[main.cpp] processIncomingFstpUrl received: " + urlString);

    std::string pathFromUrl;
    double timeFromUrl = -1.0;
    bool openFileFromUrl = false;
    bool seekFromFileUrl = false;

    // Use original_fps.load() which should be valid if a file is already playing.
    // If no file is playing, it'll be 0, and handleFstpUrlArgument should cope or use a default.
    if (handleFstpUrlArgument(urlString, g_currentOpenFilePath, original_fps.load(), 
                              pathFromUrl, timeFromUrl, openFileFromUrl, seekFromFileUrl)) {
        
        log("[FSTP Event DEBUG] handleFstpUrlArgument (from processIncomingFstpUrl) returned true. Outputs: pathFromUrl='" + pathFromUrl +
            "', timeFromUrl=" + std::to_string(timeFromUrl) +
            ", openFileFromUrl=" + (openFileFromUrl ? "true" : "false") +
            ", seekFromFileUrl=" + (seekFromFileUrl ? "true" : "false") +
            ", for input urlString='" + urlString + "'");

        std::cout << "[main.cpp] Parsed FSTP URL: path='" << pathFromUrl 
                  << "', time=" << timeFromUrl 
                  << ", openFile=" << openFileFromUrl 
                  << ", seekFile=" << seekFromFileUrl << std::endl;

        if (!openFileFromUrl && seekFromFileUrl && !pathFromUrl.empty() && pathFromUrl == g_currentOpenFilePath) {
            std::cout << "[main.cpp] URL: Same file already open ('" << g_currentOpenFilePath << "'), seeking to " << timeFromUrl << "s." << std::endl;
            log("[main.cpp] URL: Same file, seeking to " + std::to_string(timeFromUrl));
            seek_to_time(timeFromUrl);
        } else if (openFileFromUrl && !pathFromUrl.empty()) {
            std::cout << "[main.cpp] URL: Request to open file '" << pathFromUrl << "'." 
                      << (seekFromFileUrl ? " Will seek to " + std::to_string(timeFromUrl) + "s after load." : " No initial seek specified.") << std::endl;
            log("[FSTP Event] Storing pending FSTP URL for main loop: " + pathFromUrl + (seekFromFileUrl ? " and seeking to " + std::to_string(timeFromUrl) : ""));
            
            // Store for the main loop to pick up if it's in the "no file loaded" state or if it's already running.
            g_pendingFstpUrlPath = pathFromUrl;
            g_pendingFstpUrlTime = seekFromFileUrl ? timeFromUrl : -1.0;
            g_hasPendingFstpUrl.store(true);

            // DO NOT call restartPlayerWithFile here. Let the main loop handle it via the flags.
            // This is crucial for the "no file loaded" state to reliably pick up the pending URL.

        } else if (seekFromFileUrl && (pathFromUrl.empty() || pathFromUrl != g_currentOpenFilePath)) {
            // This case implies a URL like fstp:&t=xxx or fstp://DIFFERENT_FILE&t=xxx 
            // where we are not opening the file but a seek is requested.
            // If pathFromUrl is empty, it was likely just fstp:&t=... which is ambiguous without a current file context from the URL itself.
            // If pathFromUrl is different, we shouldn't seek in the *current* g_currentOpenFilePath.
            std::cout << "[main.cpp] URL: Seek requested for a file ('" << pathFromUrl 
                      << "') not currently open or ambiguous. Current file: '" << g_currentOpenFilePath << "'. Ignoring seek." << std::endl;
            log("[main.cpp] URL: Seek requested for a file not currently open or ambiguous. Ignoring seek.");
        } else {
            std::cout << "[main.cpp] URL: Processed, but no specific action (open/seek) taken based on current state or URL content." << std::endl;
            log("[main.cpp] URL: No specific open/seek action taken.");
        }
    } else {
        std::cout << "[main.cpp] URL: Not a valid fstp URL or failed to parse: " << urlString << std::endl;
        log("[main.cpp] URL: Not a valid fstp URL or failed to parse: " + urlString);
    }
}

// Function to take screenshot of current frame
void takeCurrentFrameScreenshot() {
    screenshot_requested.store(true);
}
