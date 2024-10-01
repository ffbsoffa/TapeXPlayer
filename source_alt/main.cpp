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
#include "fontdata.h"
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <limits.h>
#include <random>
#include <chrono>
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

// Declaration of visualizeFrameIndex function
void visualizeFrameIndex(const std::vector<FrameInfo>& frameIndex);

// Add this at the beginning of the file, after other global variables
bool showIndex = false;
bool showOSD = true;

std::atomic<double> previous_playback_rate(1.0);

// Add these lines at the beginning of the file, after other global variables
std::string input_timecode;
bool waiting_for_timecode = false;

// Add this at the beginning of the file, after other global variables
std::atomic<bool> seek_performed(false);

void updateVisualization(SDL_Renderer* renderer, const std::vector<FrameInfo>& frameIndex, int currentFrame, int bufferStart, int bufferEnd, int highResStart, int highResEnd, bool enableHighResDecode) {
    if (!showIndex) return; // If index should not be displayed, exit the function

    int totalFrames = frameIndex.size();
    int windowWidth, windowHeight;
    SDL_GetRendererOutputSize(renderer, &windowWidth, &windowHeight);
    
    int indexHeight = 5; // Reduce the height of the index bar to 5 pixels
    double frameWidth = static_cast<double>(windowWidth) / totalFrames;

    // Clear only the top part of the screen for the index
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_Rect indexRect = {0, 0, windowWidth, indexHeight};
    SDL_RenderFillRect(renderer, &indexRect);

    // Render frame index
    for (int i = 0; i < totalFrames; ++i) {
        SDL_Rect rect;
        rect.x = static_cast<int>(i * frameWidth);
        rect.y = 0; // Draw at the top of the screen
        rect.w = std::max(1, static_cast<int>(frameWidth));
        rect.h = indexHeight;

        // Remove high resolution check
        // if (enableHighResDecode && i >= highResStart && i <= highResEnd) {
        //     SDL_SetRenderDrawColor(renderer, 0, 255, 0, 255); // Green for high-res frames
        // } else 
        if (i >= bufferStart && i <= bufferEnd) {
            switch (frameIndex[i].type) {
                case FrameInfo::EMPTY:
                    SDL_SetRenderDrawColor(renderer, 64, 64, 64, 255); // Dark gray for empty frames
                    break;
                case FrameInfo::LOW_RES:
                    SDL_SetRenderDrawColor(renderer, 0, 128, 255, 255); // Light blue for low-res
                    break;
                case FrameInfo::FULL_RES:
                    SDL_SetRenderDrawColor(renderer, 255, 255, 0, 255); // Yellow for high-res
                    break;
            }
        } else {
            SDL_SetRenderDrawColor(renderer, 32, 32, 32, 255); // Very dark gray for frames outside the buffer
        }

        SDL_RenderFillRect(renderer, &rect);
    }

    // Render current frame (playhead)
    SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255); // Red color for the playhead
    SDL_Rect currentFrameRect = {
        static_cast<int>(currentFrame * frameWidth),
        0,
        std::max(2, static_cast<int>(frameWidth)), // Minimum width of 2 pixels for visibility
        indexHeight
    };
    SDL_RenderFillRect(renderer, &currentFrameRect);
}

void printMemoryUsage() {
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        std::cout << "Memory usage: " << usage.ru_maxrss << " KB" << std::endl;
    }
}

void displayCurrentFrame(SDL_Renderer* renderer, const FrameInfo& frameInfo, bool enableHighResDecode, double playbackRate, double currentTime, double totalDuration) {
    std::lock_guard<std::mutex> lock(frameInfo.mutex);
    if (frameInfo.is_decoding) {
        return;
    }

    if ((!enableHighResDecode && !frameInfo.low_res_frame) || (enableHighResDecode && !frameInfo.frame && !frameInfo.low_res_frame)) {
        return;
    }

    AVFrame* frame = (enableHighResDecode && frameInfo.frame) ? frameInfo.frame.get() : frameInfo.low_res_frame.get();
    
    if (!frame) {
        return;
    }

    // Create texture from AVFrame
    SDL_Texture* texture = SDL_CreateTexture(
        renderer,
        SDL_PIXELFORMAT_YV12,
        SDL_TEXTUREACCESS_STREAMING,
        frame->width,
        frame->height
    );

    if (!texture) {
        std::cout << "Error creating texture: " << SDL_GetError() << std::endl;
        return;
    }

    // Check if we need to convert the image to black and white
    const double threshold = 0.5; // Threshold in seconds
    bool makeBlackAndWhite = std::abs(playbackRate) >= 12.0 && 
                             currentTime >= threshold && 
                             (totalDuration - currentTime) >= threshold;

    if (makeBlackAndWhite) {
        // Create temporary buffer for Y-component
        std::vector<uint8_t> y_plane(frame->linesize[0] * frame->height);
        
        // Copy Y-component
        for (int y = 0; y < frame->height; ++y) {
            std::memcpy(y_plane.data() + y * frame->linesize[0], frame->data[0] + y * frame->linesize[0], frame->width);
        }

        // Fill U and V components with average value (128)
        std::vector<uint8_t> uv_plane(frame->linesize[1] * frame->height / 2, 128);

        SDL_UpdateYUVTexture(
            texture,
            NULL,
            y_plane.data(),
            frame->linesize[0],
            uv_plane.data(),
            frame->linesize[1],
            uv_plane.data(),
            frame->linesize[2]
        );
    } else {
        SDL_UpdateYUVTexture(
            texture,
            NULL,
            frame->data[0],
            frame->linesize[0],
            frame->data[1],
            frame->linesize[1],
            frame->data[2],
            frame->linesize[2]
        );
    }

    // Display texture while maintaining aspect ratio
    int windowWidth, windowHeight;
    SDL_GetRendererOutputSize(renderer, &windowWidth, &windowHeight);
    
    float videoAspectRatio = static_cast<float>(frame->width) / frame->height;
    float windowAspectRatio = static_cast<float>(windowWidth) / windowHeight;
    
    SDL_Rect dstRect;
    if (videoAspectRatio > windowAspectRatio) {
        dstRect.w = windowWidth;
        dstRect.h = static_cast<int>(windowWidth / videoAspectRatio);
        dstRect.x = 0;
        dstRect.y = (windowHeight - dstRect.h) / 2;
    } else {
        dstRect.h = windowHeight;
        dstRect.w = static_cast<int>(windowHeight * videoAspectRatio);
        dstRect.y = 0;
        dstRect.x = (windowWidth - dstRect.w) / 2;
    }
    
    // Add very soft jitter effect only vertically at increased playback speed or pause
    if (std::abs(playbackRate) > 1.0 || std::abs(playbackRate) < 0.5) {
        static std::mt19937 generator(std::chrono::system_clock::now().time_since_epoch().count());
        std::uniform_real_distribution<float> distribution(-1.0f, 1.0f);
        
        float jitter = distribution(generator);
        if (std::abs(jitter) > 0.97f) {  // Apply jitter only in 10% of cases
            dstRect.y += (jitter > 0) ? 1 : -1;
        }
    }
    
    SDL_RenderCopy(renderer, texture, NULL, &dstRect);
    SDL_DestroyTexture(texture);
}

// Function declarations from mainau.cpp
void start_audio(const char* filename);
std::string generateTXTimecode(double time);

void smooth_speed_change();

// Global variables for playback control
std::atomic<bool> audio_initialized(false);
std::thread audio_thread;
std::thread speed_change_thread;

void get_video_dimensions(const char* filename, int* width, int* height) {
    AVFormatContext* formatContext = avformat_alloc_context();
    if (avformat_open_input(&formatContext, filename, NULL, NULL) != 0) {
        std::cerr << "Could not open file" << std::endl;
        return;
    }
    
    if (avformat_find_stream_info(formatContext, NULL) < 0) {
        std::cerr << "Could not find stream information" << std::endl;
        avformat_close_input(&formatContext);
        return;
    }
    
    int videoStream = -1;
    for (unsigned int i = 0; i < formatContext->nb_streams; i++) {
        if (formatContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoStream = i;
            break;
        }
    }
    
    if (videoStream == -1) {
        std::cerr << "Could not find video stream" << std::endl;
        avformat_close_input(&formatContext);
        return;
    }
    
    *width = formatContext->streams[videoStream]->codecpar->width;
    *height = formatContext->streams[videoStream]->codecpar->height;
    
    avformat_close_input(&formatContext);
}

// Add these includes at the beginning of the file
#include <sstream>
#include <iomanip>

// Add this function before main()
void renderOSD(SDL_Renderer* renderer, TTF_Font* font, bool isPlaying, double playbackRate, bool isReverse, double currentTime, int frameNumber) {
    if (!showOSD) return;

    int windowWidth, windowHeight;
    SDL_GetRendererOutputSize(renderer, &windowWidth, &windowHeight);

    // Create black background for OSD
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_Rect osdRect = {0, windowHeight - 30, windowWidth, 30};
    SDL_RenderFillRect(renderer, &osdRect);

    // Prepare text for left part
    std::string leftText;
    if (jog_forward.load() || jog_backward.load()) {
        leftText = "JOG";
    } else if (std::abs(playbackRate) < 0.01) {
        leftText = "STILL";
    } else if (std::abs(playbackRate) > 1.0) {
        leftText = "SHUTTLE";
    } else if (std::abs(playbackRate) > 0.0) {
        leftText = "PLAY";
    } else {
        leftText = "STILL";
    }

    // Prepare timecode
    std::string timecode = "00:00:00:00";
    if (waiting_for_timecode) {
        for (size_t i = 0; i < input_timecode.length() && i < 8; ++i) {
            size_t pos = i + (i / 2);  // Account for colon positions
            timecode[pos] = input_timecode[i];
        }
    } else {
        int hours = static_cast<int>(currentTime / 3600);
        int minutes = static_cast<int>((currentTime - hours * 3600) / 60);
        int seconds = static_cast<int>(currentTime) % 60;
        int frames = static_cast<int>((currentTime - static_cast<int>(currentTime)) * original_fps.load());

        std::stringstream timecodeStream;
        timecodeStream << std::setfill('0') << std::setw(2) << hours << ":"
                       << std::setfill('0') << std::setw(2) << minutes << ":"
                       << std::setfill('0') << std::setw(2) << seconds << ":"
                       << std::setfill('0') << std::setw(2) << frames;
        timecode = timecodeStream.str();
    }

    // Prepare text for right part
    std::string rightText = isReverse ? "REV" : "FWD";
    if (jog_forward.load() || jog_backward.load()) {
    } else if (std::abs(playbackRate) > 1.0) {
        int roundedSpeed = std::round(std::abs(playbackRate));
        rightText += " " + std::to_string(roundedSpeed) + "x";
    }

    // Render text
    SDL_Color textColor = {255, 255, 255, 255};  // White color
    SDL_Color grayColor = {128, 128, 128, 255};  // Gray color for inactive digits during timecode input

    SDL_Surface* leftSurface = TTF_RenderText_Solid(font, leftText.c_str(), textColor);
    SDL_Texture* leftTexture = SDL_CreateTextureFromSurface(renderer, leftSurface);
    SDL_Rect leftRect = {10, windowHeight - 30 + (30 - leftSurface->h) / 2, leftSurface->w, leftSurface->h};
    SDL_RenderCopy(renderer, leftTexture, NULL, &leftRect);

    // Render timecode
    int charWidth = 0;
    int charHeight = 0;
    TTF_SizeText(font, "0", &charWidth, &charHeight);  // Get size of one character
    int totalWidth = charWidth * 11;  // 8 digits + 3 colons
    int xPos = (windowWidth - totalWidth) / 2;
    int yPos = windowHeight - 30 + (30 - charHeight) / 2;

    for (size_t i = 0; i < timecode.length(); ++i) {
        char c[2] = {timecode[i], '\0'};
        SDL_Color color;
        if (waiting_for_timecode) {
            if (i % 3 == 2) {  // Colons
                color = grayColor;
            } else {
                // Check if this character has been entered
                size_t inputIndex = i - (i / 3);  // Index in input_timecode
                color = (inputIndex < input_timecode.length()) ? textColor : grayColor;
            }
        } else {
            color = textColor;
        }
        
        SDL_Surface* charSurface = TTF_RenderText_Solid(font, c, color);
        SDL_Texture* charTexture = SDL_CreateTextureFromSurface(renderer, charSurface);
        SDL_Rect charRect = {xPos, yPos, charSurface->w, charSurface->h};
        SDL_RenderCopy(renderer, charTexture, NULL, &charRect);
        xPos += charWidth;
        SDL_FreeSurface(charSurface);
        SDL_DestroyTexture(charTexture);
    }

    SDL_Surface* rightSurface = TTF_RenderText_Solid(font, rightText.c_str(), textColor);
    SDL_Texture* rightTexture = SDL_CreateTextureFromSurface(renderer, rightSurface);
    SDL_Rect rightRect = {windowWidth - rightSurface->w - 10, windowHeight - 30 + (30 - rightSurface->h) / 2, rightSurface->w, rightSurface->h};
    SDL_RenderCopy(renderer, rightTexture, NULL, &rightRect);

    // Free resources
    SDL_FreeSurface(leftSurface);
    SDL_DestroyTexture(leftTexture);
    SDL_FreeSurface(rightSurface);
    SDL_DestroyTexture(rightTexture);
}

// Add this function before main()
int getUpdateInterval(double playbackRate) {
    if (playbackRate < 0.9) return std::numeric_limits<int>::max(); // Don't decode
    if (playbackRate <= 1.0) return 5000;
    if (playbackRate <= 2.0) return 2500;
    if (playbackRate <= 4.0) return 1000;
    if (playbackRate <= 8.0) return 500;
    return 100; // For speed 16x and higher
}

void initializeBuffer(const char* lowResFilename, std::vector<FrameInfo>& frameIndex, int currentFrame, int bufferSize) {
    int bufferStart = std::max(0, currentFrame - bufferSize / 2);
    int bufferEnd = std::min(bufferStart + bufferSize - 1, static_cast<int>(frameIndex.size()) - 1);
    
    asyncDecodeLowResRange(lowResFilename, frameIndex, bufferStart, bufferEnd, currentFrame, currentFrame).wait();
}

// In removeHighResFrames function
void removeHighResFrames(std::vector<FrameInfo>& frameIndex, int start, int end, int highResStart, int highResEnd) {
    for (int i = start; i <= end && i < frameIndex.size(); ++i) {
        if (frameIndex[i].type == FrameInfo::FULL_RES) {
            // Add check to not remove frames in the current high-res zone
            if (i < highResStart || i > highResEnd) {
                // std::cout << "Removing high-res frame " << i << std::endl;
                frameIndex[i].frame.reset();
                frameIndex[i].type = FrameInfo::LOW_RES;
            }
        }
    }
}

void log(const std::string& message) {
    static std::ofstream logFile("TapeXPlayer.log", std::ios::app);
    logFile << message << std::endl;
}

// Add this function before main()
void renderRewindEffect(SDL_Renderer* renderer, double playbackRate, double currentTime, double totalDuration) {
    int windowWidth, windowHeight;
    SDL_GetRendererOutputSize(renderer, &windowWidth, &windowHeight);

    // Check if we need to render the effect
    if (std::abs(playbackRate) <= 1.1) return;

    // Check if we're not at the beginning or end of the file
    const double threshold = 0.1; // Threshold in seconds
    if (currentTime < threshold || (totalDuration - currentTime) < threshold) return;

    // Base number of stripes at speed just above 1.1x
    double baseNumStripes = 1.0;
    // Smoothly increase the number of stripes as speed increases
    double speedFactor = std::max(0.0, std::abs(playbackRate) - 1.1);
    double numStripesFloat = baseNumStripes + std::sqrt(speedFactor) * 2.0;
    int numStripes = static_cast<int>(std::round(numStripesFloat));
    numStripes = std::min(numStripes, 10); // Limit maximum number of stripes

    // Base stripe height (thickest at speed just above 1.1x)
    int baseStripeHeight = windowHeight / baseNumStripes;
    // Decrease stripe height as speed increases
    int stripeHeight = static_cast<int>(baseStripeHeight / (std::abs(playbackRate) / 0.2));
    stripeHeight = std::max(stripeHeight, 10); // Minimum stripe height

    // Stripe movement speed (pixels per frame)
    // Increase base speed and make it dependent on window height
    int baseSpeed = windowHeight / 3; // Base speed for speed just above 1.1x
    int speed = static_cast<int>(baseSpeed * (std::abs(playbackRate) / 1.1));

    // Stripe offset (changes over time)
    static int offset = 0;
    offset = (offset + speed) % windowHeight;

    // Stripe color (gray, opaque)
    SDL_SetRenderDrawColor(renderer, 128, 128, 128, 255);

    for (int i = 0; i < numStripes; ++i) {
        SDL_Rect stripeRect;
        stripeRect.x = 0;
        stripeRect.y = windowHeight - (i * (windowHeight / numStripes) + offset) % windowHeight;
        stripeRect.w = windowWidth;
        stripeRect.h = stripeHeight;

        SDL_RenderFillRect(renderer, &stripeRect);
    }
}

int main(int argc, char* argv[]) {
    log("Program started");
    log("Number of arguments: " + std::to_string(argc));
    for (int i = 0; i < argc; ++i) {
        log("Argument " + std::to_string(i) + ": " + argv[i]);
    }
    log("Current working directory: " + std::string(getcwd(NULL, 0)));

    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <path_to_video_file>" << std::endl;
        return 1;
    }

    std::string filename = argv[1];
    
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

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
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

    RingBuffer ringBuffer(ringBufferCapacity);
    FrameCleaner frameCleaner(frameIndex);

    int currentFrame = 0;
    bool isPlaying = false;
    bool enableHighResDecode = true;
    std::chrono::steady_clock::time_point lastFrameTime;
    std::chrono::steady_clock::time_point lastBufferUpdateTime = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point now;

    const int highResWindowSize = 500;  // Size of high-res window
    std::future<void> lowResFuture;
    std::future<void> highResFuture;
    std::future<void> secondaryCleanFuture;

    const int TARGET_FPS = 60;
    const int FRAME_DELAY = 1000 / TARGET_FPS;

    // Initialize audio
    audio_thread = std::thread(start_audio, filename.c_str());
    speed_change_thread = std::thread(smooth_speed_change);

    bool fps_received = false;
    bool duration_received = false;

    std::chrono::steady_clock::time_point lastLowResUpdateTime = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point lastHighResUpdateTime = std::chrono::steady_clock::now();

    while (!shouldExit) {
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
                            target_playback_rate.store(std::min(target_playback_rate.load() * 2.0, 16.0));
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

        // Check if seek operation was performed
        if (seek_performed.load()) {
            // Get current time after seek
            double currentTime = current_audio_time.load();
            currentFrame = static_cast<int>(currentTime * original_fps.load()) % frameIndex.size();

            // Reinitialize buffer
            initializeBuffer(lowResFilename.c_str(), frameIndex, currentFrame, ringBufferCapacity);

            // Reset seek flag
            seek_performed.store(false);

            // Reset last update times
            lastLowResUpdateTime = std::chrono::steady_clock::now();
            lastHighResUpdateTime = std::chrono::steady_clock::now();
        }

        // Buffer update
        int bufferStart = std::max(0, currentFrame - static_cast<int>(ringBufferCapacity / 2));
        int bufferEnd = std::min(bufferStart + static_cast<int>(ringBufferCapacity) - 1, static_cast<int>(frameIndex.size()) - 1);

        // Define high-res window boundaries
        int highResStart = std::max(0, currentFrame - highResWindowSize / 2);
        int highResEnd = std::min(static_cast<int>(frameIndex.size()) - 1, currentFrame + highResWindowSize / 2);

        bool isInHighResZone = (currentFrame >= highResStart && currentFrame <= highResEnd);

        now = std::chrono::steady_clock::now();
        auto timeSinceLastLowResUpdate = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastLowResUpdateTime);
        auto timeSinceLastHighResUpdate = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastHighResUpdateTime);

        double currentPlaybackRate = std::abs(playback_rate.load());
        int lowResUpdateInterval = getUpdateInterval(currentPlaybackRate);
        int highResUpdateInterval = getUpdateInterval(currentPlaybackRate) / 2;

        // Determine if high-res frames should be decoded
        bool shouldDecodeHighRes = currentPlaybackRate < 2.0 && enableHighResDecode;

        // Low-res frame decoding loop
        if (timeSinceLastLowResUpdate.count() >= lowResUpdateInterval) {
            bool needLowResUpdate = false;
            for (int i = bufferStart; i <= bufferEnd; ++i) {
                if (frameIndex[i].type == FrameInfo::EMPTY) {
                    needLowResUpdate = true;
                    break;
                }
            }

            if (needLowResUpdate) {
                if (!lowResFuture.valid() || lowResFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                    lowResFuture = asyncDecodeLowResRange(lowResFilename.c_str(), frameIndex, bufferStart, bufferEnd, highResStart, highResEnd);
                    lastLowResUpdateTime = now;
                }
            }
        }

        // High-res frame decoding loop
        if (shouldDecodeHighRes && timeSinceLastHighResUpdate.count() >= highResUpdateInterval) {
            bool needHighResUpdate = false;
            for (int i = highResStart; i <= highResEnd; ++i) {
                if (frameIndex[i].type != FrameInfo::FULL_RES) {
                    needHighResUpdate = true;
                    break;
                }
            }

            if (needHighResUpdate) {
                if (!highResFuture.valid() || highResFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                    highResFuture = asyncDecodeFrameRange(filename.c_str(), frameIndex, highResStart, highResEnd);
                    lastHighResUpdateTime = now;
                }
            }
        } else if (currentPlaybackRate >= 2.0) {
            // If playback speed >= 2x, remove high-res frames
            removeHighResFrames(frameIndex, 0, frameIndex.size() - 1, -1, -1);
        }

        // Remove high-res frames outside the window
        removeHighResFrames(frameIndex, 0, highResStart - 1, highResStart, highResEnd);
        removeHighResFrames(frameIndex, highResEnd + 1, frameIndex.size() - 1, highResStart, highResEnd);

        // Check and reset frame type if it's outside the high-res window
        for (int i = 0; i < frameIndex.size(); ++i) {
            if (i < highResStart || i > highResEnd) {
                if (frameIndex[i].type == FrameInfo::FULL_RES && !frameIndex[i].is_decoding) {
                    frameIndex[i].frame.reset();
                    frameIndex[i].type = frameIndex[i].low_res_frame ? FrameInfo::LOW_RES : FrameInfo::EMPTY;
                }
            }
        }

        // Clear low-res frames outside the buffer
        for (int i = 0; i < frameIndex.size(); ++i) {
            if (i < bufferStart || i > bufferEnd) {
                if (frameIndex[i].type == FrameInfo::LOW_RES && !frameIndex[i].is_decoding) {
                    frameIndex[i].low_res_frame.reset();
                    frameIndex[i].type = FrameInfo::EMPTY;
                }
            }
        }

        // Update current frame based on current audio time
        double currentTime = current_audio_time.load();
        currentFrame = static_cast<int>(currentTime * original_fps.load()) % frameIndex.size();

        // Clear the entire screen
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);

        // Display current frame
        displayCurrentFrame(renderer, frameIndex[currentFrame], isInHighResZone && enableHighResDecode, std::abs(playback_rate.load()), currentTime, total_duration.load());

        double effectPlaybackRate = playback_rate.load();
        if (std::abs(effectPlaybackRate) >= 2.0) {
            renderRewindEffect(renderer, effectPlaybackRate, currentTime, total_duration.load());
        }

        // Update frame index visualization
        if (showIndex) {
            updateVisualization(renderer, frameIndex, currentFrame, bufferStart, bufferEnd, highResStart, highResEnd, enableHighResDecode);
        }

        // Render OSD
        if (showOSD) {
            renderOSD(renderer, font, isPlaying, playback_rate.load(), is_reverse.load(), currentTime, currentFrame);
        }

        SDL_RenderPresent(renderer);

        if (isPlaying) {
            now = std::chrono::steady_clock::now();
            auto frameDuration = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastFrameTime);
            double fps = 1000.0 / frameDuration.count();
        }

        // Limit frame rate
        Uint32 frameTime = SDL_GetTicks() - frameStart;
        if (frameTime < FRAME_DELAY) {
            SDL_Delay(FRAME_DELAY - frameTime);
        }
    }

    // Finish work
    shouldExit = true;

    // Complete asynchronous tasks
    if (lowResFuture.valid()) {
        lowResFuture.wait();
    }
    if (highResFuture.valid()) {
        highResFuture.wait();
    }
    if (secondaryCleanFuture.valid()) {
        secondaryCleanFuture.wait();
    }

    // Finish threads
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