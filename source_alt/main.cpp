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
#include "decode.h"
#include "common.h"
#include "display.h"
#include "fontdata.h"
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <limits.h>
#include <random>
#include <chrono>
#include <sstream>
#include <iomanip>
#include "nfd.hpp"
#include "remote_control.h"
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

// Functions to access playback rate variables
std::atomic<double>& get_playback_rate() {
    return playback_rate;
}

std::atomic<double>& get_target_playback_rate() {
    return target_playback_rate;
}

void initializeBuffer(const char* lowResFilename, std::vector<FrameInfo>& frameIndex, int currentFrame, int bufferSize) {
    int bufferStart = std::max(0, currentFrame - bufferSize / 2);
    int bufferEnd = std::min(bufferStart + bufferSize - 1, static_cast<int>(frameIndex.size()) - 1);
    
    asyncDecodeLowResRange(lowResFilename, frameIndex, bufferStart, bufferEnd, currentFrame, currentFrame).wait();
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

int main(int argc, char* argv[]) {
    log("Program started");
    log("Number of arguments: " + std::to_string(argc));
    for (int i = 0; i < argc; ++i) {
        log("Argument " + std::to_string(i) + ": " + argv[i]);
    }
    log("Current working directory: " + std::string(getcwd(NULL, 0)));

    // Initialize remote control
    RemoteControl remote_control;
    if (!remote_control.initialize()) {
        std::cerr << "Warning: Failed to initialize remote control" << std::endl;
        log("Warning: Failed to initialize remote control");
    }

    std::string filename;
    bool fileSelected = false;

    if (argc < 2) {
        // No file provided as argument, show blue screen and then file dialog
        if (SDL_Init(SDL_INIT_VIDEO) < 0) {
            std::cerr << "SDL initialization error: " << SDL_GetError() << std::endl;
            return 1;
        }

        // Initialize TTF
        if (TTF_Init() == -1) {
            std::cerr << "SDL_ttf initialization error: " << TTF_GetError() << std::endl;
            return 1;
        }

        SDL_Window* window = SDL_CreateWindow("TapeXPlayer", 
            SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 
            1280, 720,  // Default size, can be adjusted
            SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
        if (!window) {
            std::cerr << "Window creation error: " << SDL_GetError() << std::endl;
            SDL_Quit();
            return 1;
        }

        SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!renderer) {
            std::cerr << "Renderer creation error: " << SDL_GetError() << std::endl;
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 1;
        }

        // Load font
        SDL_RWops* rw = SDL_RWFromConstMem(font_otf, sizeof(font_otf));
        TTF_Font* font = TTF_OpenFontRW(rw, 1, 24);
        if (!font) {
            std::cerr << "Error loading font from memory: " << TTF_GetError() << std::endl;
            return 1;
        }

        // Render blue background
        SDL_SetRenderDrawColor(renderer, 0, 0, 255, 255);
        SDL_RenderClear(renderer);

        // Render OSD
        renderOSD(renderer, font, false, 0.0, false, 0.0, 0, true, false, "", 0.0, false, false);

        SDL_RenderPresent(renderer);

        // Wait for a short time to show the blue screen
        SDL_Delay(1000);  // Wait for 1 second

        // Open file dialog
        NFD::Guard nfdGuard;
        if (NFD::Init() != NFD_OKAY) {
            std::cerr << "Error initializing NFD: " << NFD::GetError() << std::endl;
            log("Error initializing NFD: " + std::string(NFD::GetError()));
        } else {
            NFD::UniquePath outPath;
            nfdfilteritem_t filterItem[1] = { { "Video files", "mp4,avi,mov,mkv" } };
            nfdresult_t result = NFD::OpenDialog(outPath, filterItem, 1);
            
            if (result == NFD_OKAY) {
                filename = outPath.get();
                std::cout << "File selected: " << filename << std::endl;
                log("File selected via dialog: " + filename);
                fileSelected = true;
            } else if (result == NFD_CANCEL) {
                std::cout << "User canceled file selection." << std::endl;
                log("User canceled file selection");
            } else {
                std::cerr << "Error: " << NFD::GetError() << std::endl;
                log("Error in file dialog: " + std::string(NFD::GetError()));
            }
        }

        // Clean up
        TTF_CloseFont(font);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        TTF_Quit();
        SDL_Quit();

        if (!fileSelected) {
            return 0;
        }
    } else {
        filename = argv[1];
    }

    if (filename.empty()) {
        std::cerr << "No file selected. Exiting." << std::endl;
        log("No file selected. Exiting.");
        return 1;
    }

    // Handle file path for macOS
    if (filename.find("/Volumes/") == 0) {
        // Path is already absolute, leave as is
    } else if (filename[0] != '/') {
        // Relative path, add current directory
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd)) != NULL) {
            filename = std::string(cwd) + "/" + filename;
        } else {
            std::cerr << "Error getting current directory" << std::endl;
            return 1;
        }
    }

    std::cout << "Received file path: " << filename << std::endl;
    log("Processed file path: " + filename);

    if (!std::filesystem::exists(filename)) {
        std::cerr << "File not found: " << filename << std::endl;
        log("Error: file not found: " + filename);
        return 1;
    }

    std::string lowResFilename = "low_res_output.mp4";

    std::vector<FrameInfo> frameIndex = createFrameIndex(filename.c_str());
    std::cout << "Frame index created. Total frames: " << frameIndex.size() << std::endl;

    const size_t ringBufferCapacity = 1000;  // or another suitable value

    if (!convertToLowRes(filename.c_str(), lowResFilename)) {
        std::cerr << "Error converting video to low resolution" << std::endl;
        return 1;
    }

    // Initialize buffer after conversion
    initializeBuffer(lowResFilename.c_str(), frameIndex, 0, ringBufferCapacity);

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

    std::future<double> fps_future = std::async(std::launch::async, get_video_fps, filename.c_str());
    std::future<double> duration_future = std::async(std::launch::async, get_file_duration, filename.c_str());

    // Get video dimensions
    int videoWidth, videoHeight;
    get_video_dimensions(filename.c_str(), &videoWidth, &videoHeight);

    SDL_Window* window = SDL_CreateWindow("TapeXPlayer", 
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 
        videoWidth, videoHeight, 
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!window) {
        std::cerr << "Window creation error: " << SDL_GetError() << std::endl;
        SDL_Quit();
        return 1;
    }

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

    bool enableHighResDecode = true;

    const int highResWindowSize = 500;  // Size of high-res window

    const int TARGET_FPS = 60;
    const int FRAME_DELAY = 1000 / TARGET_FPS;

    // Initialize audio
    bool audio_started = false;
    for (int attempt = 0; attempt < 3 && !audio_started; ++attempt) {
        std::cout << "Attempting to start audio (attempt " << attempt + 1 << " of 3)" << std::endl;
        audio_thread = std::thread([&filename, &audio_started]() {
            start_audio(filename.c_str());
        });
        audio_thread.join();  // Ждем завершения потока
        
        audio_started = !quit.load();  // Если quit не установлен, считаем, чт аудио запустилось успешно
        
        if (!audio_started) {
            std::cout << "Audio failed to start, retrying..." << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(1));  // Небольшая пауза перед следующей попыткой
        }
    }

    if (!audio_started) {
        std::cerr << "Failed to start audio after 3 attempts. Exiting." << std::endl;
        return 1;
    }

    speed_change_thread = std::thread(smooth_speed_change);

    bool fps_received = false;
    bool duration_received = false;

    previous_playback_rate.store(playback_rate.load());

    std::atomic<int> currentFrame(0);
    std::atomic<bool> isPlaying(false);
    std::thread decodingThread(manageVideoDecoding, std::ref(filename), std::ref(lowResFilename), 
                               std::ref(frameIndex), std::ref(currentFrame),
                               ringBufferCapacity, highResWindowSize,
                               std::ref(isPlaying));

    while (!shouldExit) {
        // Process remote commands at the start of each frame
        if (remote_control.is_initialized()) {
            remote_control.process_commands();
        }

        // Check if FPS value is ready
        if (!fps_received && fps_future.valid()) {
            std::future_status status = fps_future.wait_for(std::chrono::seconds(0));
            if (status == std::future_status::ready) {
                original_fps.store(fps_future.get());
                std::cout << "Video FPS: " << original_fps.load() << std::endl;
                fps_received = true;
            }
        }

        // Check if duration is ready
        if (!duration_received && duration_future.valid()) {
            std::future_status status = duration_future.wait_for(std::chrono::seconds(0));
            if (status == std::future_status::ready) {
                total_duration.store(duration_future.get());
                std::cout << "Total duration: " << total_duration.load() << " seconds" << std::endl;
                duration_received = true;
            }
        }

        Uint32 frameStart = SDL_GetTicks();

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                quit = true;
                shouldExit = true;
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
                            toggle_pause();
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
                            shouldExit = true;
                            break;
                        case SDLK_PLUS:
                        case SDLK_EQUALS:
                            increase_volume();
                            break;
                        case SDLK_MINUS:
                            decrease_volume();
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
                                // Проверяем, не находимся ли мы в режиме ввода таймкода
                                if (!waiting_for_timecode) {
                                    int markerIndex = e.key.keysym.sym - SDLK_1;
                                    if (e.key.keysym.mod & KMOD_ALT) {
                                        // Установка маркера
                                        memoryMarkers[markerIndex] = current_audio_time.load();
                                        std::cout << "Marker " << (markerIndex + 1) << " set at " 
                                                << generateTXTimecode(memoryMarkers[markerIndex]) << std::endl;
                                    } else {
                                        // Переход к маркеру
                                        if (memoryMarkers[markerIndex] >= 0) {
                                            seek_to_time(memoryMarkers[markerIndex]);
                                        }
                                    }
                                }
                            }
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
            }
        }

        // Обновляем currentFrame на основе текущего ремени аудио
        double currentTime = current_audio_time.load();
        int newCurrentFrame = static_cast<int>(currentTime * original_fps.load()) % frameIndex.size();
        currentFrame.store(newCurrentFrame);

        // Проверяем завершение seek
        if (seekInfo.completed.load()) {
            seekInfo.completed.store(false);
            seek_performed.store(false);
        }

displayFrame(renderer, frameIndex, newCurrentFrame, enableHighResDecode, playback_rate.load(), currentTime, total_duration.load(), showIndex, showOSD, font, isPlaying, is_reverse.load(), waiting_for_timecode, input_timecode, original_fps.load(), jog_forward, jog_backward, ringBufferCapacity, highResWindowSize);
        SDL_RenderPresent(renderer);

        // Limit frame rate
        Uint32 frameTime = SDL_GetTicks() - frameStart;
        if (frameTime < FRAME_DELAY) {
            SDL_Delay(FRAME_DELAY - frameTime);
        }

        // Обработка запроса на поиск
        if (seek_performed.load()) {
            seekInfo.time.store(current_audio_time.load());
            seekInfo.requested.store(true);
            seekInfo.completed.store(false);
            seek_performed.store(false);
        }
    }

    // Finish work
    shouldExit = true;
    quit = true;

    // Cleanup audio before joining threads
    cleanup_audio();

    // Finish threads
    if (decodingThread.joinable()) {
        decodingThread.join();
    }
    if (audio_thread.joinable()) {
        audio_thread.join();
    }
    if (speed_change_thread.joinable()) {
        speed_change_thread.join();
    }

    // Free TTF resources
    if (font) {
        TTF_CloseFont(font);
        font = nullptr;
    }

    TTF_Quit();

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}

