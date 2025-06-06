#include "window_manager.h"
#include "display.h"
#include "../common/common.h"
#include "../common/fontdata.h"
#include <iostream>
#include <thread>

#ifdef __APPLE__
// Forward declaration instead of including metal_renderer.h
void* createMetalRenderer(SDL_Window* window);
void releaseMetalRenderer(void* renderer);
#endif

WindowManager::WindowManager()
    : window_(nullptr)
    , renderer_(nullptr)
    , font_(nullptr)
    , metalRenderer_(nullptr)
    , isFullscreen_(false)
    , windowedX_(SDL_WINDOWPOS_CENTERED)
    , windowedY_(SDL_WINDOWPOS_CENTERED)
    , windowedWidth_(1280)
    , windowedHeight_(720)
    , lastTextureWidth_(0)
    , lastTextureHeight_(0)
    , frameStartTime_(0)
    , targetFPS_(60)
    , targetFrameTime_(1000 / 60)
    , useAdaptiveDelay_(true)
    , lastEventCheck_(std::chrono::steady_clock::now())
    , consecutiveNoEvents_(0)
    , lastFrameTypeDisplayed_(FrameInfo::EMPTY)
    , frameTypeTransitionCounter_(0) {
}

WindowManager::~WindowManager() {
    cleanup();
}

bool WindowManager::initialize(const std::string& title, int x, int y, int width, int height, bool fullscreen) {
    // Initialize SDL
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        std::cerr << "SDL initialization error: " << SDL_GetError() << std::endl;
        return false;
    }
    
    // Initialize SDL_ttf
    if (TTF_Init() < 0) {
        std::cerr << "SDL_ttf initialization error: " << TTF_GetError() << std::endl;
        SDL_Quit();
        return false;
    }
    
    // Load embedded font
    SDL_RWops* rw = SDL_RWFromConstMem(font_otf, sizeof(font_otf));
    if (!rw) {
        std::cerr << "Failed to create RWops from embedded font" << std::endl;
        TTF_Quit();
        SDL_Quit();
        return false;
    }
    
    font_ = TTF_OpenFontRW(rw, 1, 16);
    if (!font_) {
        std::cerr << "Failed to load embedded font: " << TTF_GetError() << std::endl;
        TTF_Quit();
        SDL_Quit();
        return false;
    }
    
    // Store windowed position and size
    windowedX_ = x;
    windowedY_ = y;
    windowedWidth_ = width;
    windowedHeight_ = height;
    isFullscreen_ = fullscreen;
    
    // Create window
    Uint32 windowFlags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
#ifdef __APPLE__
    windowFlags |= SDL_WINDOW_METAL;
#endif
    
    if (fullscreen) {
        windowFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    }
    
    window_ = SDL_CreateWindow(title.c_str(), x, y, width, height, windowFlags);
    if (!window_) {
        std::cerr << "Window creation error: " << SDL_GetError() << std::endl;
        TTF_CloseFont(font_);
        TTF_Quit();
        SDL_Quit();
        return false;
    }
    
    // Enable drop file events
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
    
    // Create renderer
    renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer_) {
        std::cerr << "Renderer creation error with VSync: " << SDL_GetError() << std::endl;
        // Try without VSync
        renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED);
        if (!renderer_) {
            std::cerr << "Renderer creation error: " << SDL_GetError() << std::endl;
            SDL_DestroyWindow(window_);
            TTF_CloseFont(font_);
            TTF_Quit();
            SDL_Quit();
            return false;
        }
    }
    
#ifdef __APPLE__
    // TODO: Initialize Metal renderer when functions are available
    // metalRenderer_ = createMetalRenderer(window_);
    // if (!metalRenderer_) {
    //     std::cerr << "Failed to create Metal renderer, falling back to SDL" << std::endl;
    // }
    metalRenderer_ = nullptr;
#endif
    
    return true;
}

void WindowManager::cleanup() {
#ifdef __APPLE__
    // TODO: Release Metal renderer when functions are available
    // if (metalRenderer_) {
    //     releaseMetalRenderer(metalRenderer_);
    //     metalRenderer_ = nullptr;
    // }
#endif
    
    if (renderer_) {
        SDL_DestroyRenderer(renderer_);
        renderer_ = nullptr;
    }
    
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    
    if (font_) {
        TTF_CloseFont(font_);
        font_ = nullptr;
    }
    
    TTF_Quit();
    SDL_Quit();
}

void WindowManager::setTitle(const std::string& title) {
    if (window_) {
        SDL_SetWindowTitle(window_, title.c_str());
    }
}

void WindowManager::setFullscreen(bool fullscreen) {
    if (!window_ || isFullscreen_ == fullscreen) return;
    
#ifdef __APPLE__
    // Use native macOS fullscreen
    // Note: toggleNativeFullscreen is defined in menu_system.mm
    // For now, fall back to SDL fullscreen
    SDL_SetWindowFullscreen(window_, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    isFullscreen_ = fullscreen;
#else
    if (fullscreen) {
        // Save current windowed position and size
        SDL_GetWindowPosition(window_, &windowedX_, &windowedY_);
        SDL_GetWindowSize(window_, &windowedWidth_, &windowedHeight_);
        
        SDL_SetWindowFullscreen(window_, SDL_WINDOW_FULLSCREEN_DESKTOP);
    } else {
        SDL_SetWindowFullscreen(window_, 0);
        
        // Restore windowed position and size
        SDL_SetWindowPosition(window_, windowedX_, windowedY_);
        SDL_SetWindowSize(window_, windowedWidth_, windowedHeight_);
    }
    
    isFullscreen_ = fullscreen;
#endif
}

void WindowManager::toggleFullscreen() {
    setFullscreen(!isFullscreen());
}

bool WindowManager::isFullscreen() const {
    if (!window_) return false;
    return (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
}

void WindowManager::getWindowSize(int& width, int& height) const {
    if (window_) {
        SDL_GetWindowSize(window_, &width, &height);
    } else {
        width = 0;
        height = 0;
    }
}

bool WindowManager::hasInputFocus() const {
    if (!window_) return false;
    return (SDL_GetWindowFlags(window_) & SDL_WINDOW_INPUT_FOCUS) != 0;
}

void WindowManager::beginFrame() {
    // Nothing needed for SDL renderer
}

void WindowManager::endFrame() {
    if (renderer_) {
        SDL_RenderPresent(renderer_);
    }
}

void WindowManager::clear(int r, int g, int b, int a) {
    if (renderer_) {
        SDL_SetRenderDrawColor(renderer_, r, g, b, a);
        SDL_RenderClear(renderer_);
    }
}

void WindowManager::displayFrame(
    const std::vector<FrameInfo>& frameIndex,
    int currentFrame,
    std::shared_ptr<AVFrame> frameToDisplay,
    FrameInfo::FrameType frameTypeToDisplay,
    bool enableHighResDecode,
    double playbackRate,
    double currentTime,
    double totalDuration,
    bool showIndex,
    bool showOSD,
    std::atomic<bool>& isPlaying,
    bool isReverse,
    bool waitingForTimecode,
    const std::string& inputTimecode,
    double originalFps,
    std::atomic<bool>& jog_forward,
    std::atomic<bool>& jog_backward,
    size_t ringBufferCapacity,
    int highResWindowSize,
    int segmentSize,
    float targetDisplayAspectRatio) {
    
    if (!renderer_) return;
    
    // Call the existing displayFrame function from display.cpp
    ::displayFrame(renderer_, frameIndex, currentFrame, frameToDisplay, frameTypeToDisplay,
                   enableHighResDecode, playbackRate, currentTime, totalDuration,
                   showIndex, showOSD, font_, isPlaying, isReverse,
                   waitingForTimecode, inputTimecode, originalFps,
                   jog_forward, jog_backward, ringBufferCapacity,
                   highResWindowSize, segmentSize, targetDisplayAspectRatio);
    
    // Update last texture dimensions
    lastTextureWidth_ = get_last_texture_width();
    lastTextureHeight_ = get_last_texture_height();
}

void WindowManager::renderOSD(
    bool isPlaying,
    double playbackRate,
    bool isReverse,
    double currentTime,
    int frameNumber,
    bool showOSD,
    bool waitingForTimecode,
    const std::string& inputTimecode,
    double originalFps,
    bool jogForward,
    bool jogBackward,
    FrameInfo::FrameType frameType) {
    
    if (!renderer_ || !font_) return;
    
    ::renderOSD(renderer_, font_, isPlaying, playbackRate, isReverse,
                currentTime, frameNumber, showOSD, waitingForTimecode,
                inputTimecode, originalFps, jogForward, jogBackward, frameType);
}

void WindowManager::renderLoadingScreen(const LoadingStatus& status) {
    if (!renderer_ || !font_) return;
    
    ::renderLoadingScreen(renderer_, font_, status);
}

void WindowManager::renderNoFileScreen() {
    if (!renderer_ || !font_) return;
    
    clear(0, 0, 0, 255);
    
    // Get window dimensions
    int windowWidth, windowHeight;
    getWindowSize(windowWidth, windowHeight);
    
    // Render hint text
    SDL_Color textColor = {200, 200, 200, 200};
    SDL_Surface* messageSurface = TTF_RenderText_Blended(font_, "Press Ctrl+O to open a file", textColor);
    if (messageSurface) {
        SDL_Texture* messageTexture = SDL_CreateTextureFromSurface(renderer_, messageSurface);
        if (messageTexture) {
            SDL_Rect messageRect = {
                windowWidth / 2 - messageSurface->w / 2,
                windowHeight / 2 - messageSurface->h / 2,
                messageSurface->w,
                messageSurface->h
            };
            
            SDL_RenderCopy(renderer_, messageTexture, NULL, &messageRect);
            SDL_DestroyTexture(messageTexture);
        }
        SDL_FreeSurface(messageSurface);
    }
}

void WindowManager::renderZoomedFrame(SDL_Texture* texture, int frameWidth, int frameHeight,
                                     float zoomFactor, float centerX, float centerY) {
    if (!renderer_) return;
    
    ::renderZoomedFrame(renderer_, texture, frameWidth, frameHeight, zoomFactor, centerX, centerY);
}

void WindowManager::renderZoomThumbnail(SDL_Texture* texture, int frameWidth, int frameHeight,
                                       float zoomFactor, float centerX, float centerY) {
    if (!renderer_) return;
    
    ::renderZoomThumbnail(renderer_, texture, frameWidth, frameHeight, zoomFactor, centerX, centerY);
}

void WindowManager::handleZoomMouseEvent(SDL_Event& event, int frameWidth, int frameHeight) {
    int windowWidth, windowHeight;
    getWindowSize(windowWidth, windowHeight);
    
    ::handleZoomMouseEvent(event, windowWidth, windowHeight, frameWidth, frameHeight);
}

void WindowManager::enableDropFile() {
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
}

// Frame timing methods
void WindowManager::setTargetFPS(int fps) {
    targetFPS_ = fps;
    targetFrameTime_ = (fps > 0) ? (1000 / fps) : 16; // Default to ~60 FPS if invalid
}

void WindowManager::beginFrameTiming() {
    frameStartTime_ = SDL_GetTicks();
}

void WindowManager::endFrameTiming() {
    if (!useAdaptiveDelay_) return;
    
    Uint32 frameTime = SDL_GetTicks() - frameStartTime_;
    if (frameTime < targetFrameTime_) {
        // Use a more responsive delay approach
        Uint32 delayTime = targetFrameTime_ - frameTime;
        
        // For short delays, use busy wait for better precision
        if (delayTime < 5) {
            Uint32 targetTime = frameStartTime_ + targetFrameTime_;
            while (SDL_GetTicks() < targetTime) {
                // Busy wait for precision
            }
        } else {
            // For longer delays, use SDL_Delay but wake up a bit early
            // to avoid overshooting
            SDL_Delay(delayTime - 1);
            
            // Then busy wait for the remaining time
            Uint32 targetTime = frameStartTime_ + targetFrameTime_;
            while (SDL_GetTicks() < targetTime) {
                // Busy wait for precision
            }
        }
    }
}

bool WindowManager::shouldSkipFrame() const {
    // Can be used to implement frame skipping if running behind
    Uint32 currentTime = SDL_GetTicks();
    return (currentTime - frameStartTime_) > (targetFrameTime_ * 2);
}

int WindowManager::processEvents(std::function<void(SDL_Event&)> eventHandler, int maxEvents) {
    SDL_Event event;
    int eventsProcessed = 0;
    auto currentTime = std::chrono::steady_clock::now();
    
    // Use adaptive strategy based on recent event activity
    bool hasRecentActivity = (currentTime - lastEventCheck_) < std::chrono::milliseconds(100);
    lastEventCheck_ = currentTime;
    
    // Process events with adaptive limit and quick exit strategy
    while (eventsProcessed < maxEvents) {
        // Use SDL_PollEvent for standard non-blocking check
        if (SDL_PollEvent(&event)) {
            eventHandler(event);
            eventsProcessed++;
            consecutiveNoEvents_ = 0;
            
            // Yield periodically during heavy event processing
            if (eventsProcessed % 5 == 0) {
                std::this_thread::yield();
            }
        } else {
            // No events available
            consecutiveNoEvents_++;
            break;
        }
    }
    
    // If we haven't had events for a while and we're not in active use,
    // do a tiny sleep to reduce CPU usage
    if (consecutiveNoEvents_ > 10 && !hasRecentActivity) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    
    return eventsProcessed;
}

void WindowManager::resetFrameSelection() {
    lastFrameTypeDisplayed_ = FrameInfo::EMPTY;
    frameTypeTransitionCounter_ = 0;
}

WindowManager::FrameSelection WindowManager::selectFrame(
    const std::vector<FrameInfo>& frameIndex,
    int currentFrameIndex,
    double playbackRate,
    bool forceUpdate) {
    
    FrameSelection result;
    const int TRANSITION_THRESHOLD = 1; // Reduced threshold for faster type switching
    
    if (currentFrameIndex < 0 || currentFrameIndex >= frameIndex.size()) {
        return result; // frameFound = false
    }
    
    const FrameInfo& currentFrameInfo = frameIndex[currentFrameIndex];
    std::unique_lock<std::mutex> lock(currentFrameInfo.mutex);
    
    // Skip if frame is currently being decoded
    if (currentFrameInfo.is_decoding) {
        return result; // frameFound = false
    }
    
    double currentPlaybackRate = std::abs(playbackRate);
    
    if (currentPlaybackRate <= 1.1) {
        // At normal speed, prefer highest quality available, but with smooth transitions
        bool shouldTransition = false;
        
        // If force update after seek, immediately choose best available
        if (forceUpdate) {
            shouldTransition = true;
            frameTypeTransitionCounter_ = TRANSITION_THRESHOLD; // Skip transition delay
        }
        // Otherwise try to maintain the last frame type if available
        else if (lastFrameTypeDisplayed_ != FrameInfo::EMPTY) {
            switch (lastFrameTypeDisplayed_) {
                case FrameInfo::FULL_RES:
                    if (currentFrameInfo.frame) {
                        result.frame = currentFrameInfo.frame;
                        result.frameType = FrameInfo::FULL_RES;
                        result.frameFound = true;
                    } else {
                        shouldTransition = true;
                    }
                    break;
                case FrameInfo::LOW_RES:
                    if (currentFrameInfo.low_res_frame) {
                        result.frame = currentFrameInfo.low_res_frame;
                        result.frameType = FrameInfo::LOW_RES;
                        result.frameFound = true;
                        // Always try to transition to FULL_RES if available
                        if (currentFrameInfo.frame) shouldTransition = true;
                    } else {
                        shouldTransition = true;
                    }
                    break;
                case FrameInfo::CACHED:
                    // Immediately transition if better quality is available
                    if (currentFrameInfo.frame || currentFrameInfo.low_res_frame) {
                        shouldTransition = true;
                        frameTypeTransitionCounter_ = TRANSITION_THRESHOLD; // Force immediate transition
                    } else if (currentFrameInfo.cached_frame) {
                        result.frame = currentFrameInfo.cached_frame;
                        result.frameType = FrameInfo::CACHED;
                        result.frameFound = true;
                    } else {
                        shouldTransition = true;
                    }
                    break;
                default:
                    shouldTransition = true;
                    break;
            }
        } else {
            shouldTransition = true;
        }

        // Handle transitions
        if (shouldTransition) {
            frameTypeTransitionCounter_++;
            if (frameTypeTransitionCounter_ >= TRANSITION_THRESHOLD) {
                // Reset counter and switch to best available quality
                frameTypeTransitionCounter_ = 0;
                if (currentFrameInfo.frame) {
                    result.frame = currentFrameInfo.frame;
                    result.frameType = FrameInfo::FULL_RES;
                    result.frameFound = true;
                } else if (currentFrameInfo.low_res_frame) {
                    result.frame = currentFrameInfo.low_res_frame;
                    result.frameType = FrameInfo::LOW_RES;
                    result.frameFound = true;
                } else if (currentFrameInfo.cached_frame) {
                    result.frame = currentFrameInfo.cached_frame;
                    result.frameType = FrameInfo::CACHED;
                    result.frameFound = true;
                }
            }
        } else {
            // If we're maintaining the same type, reset transition counter
            frameTypeTransitionCounter_ = 0;
        }
    } else {
        // At high speed, prefer low_res/cached, but try to maintain consistency if possible
        bool foundFrame = false;
        
        // Always try LOW_RES first at high speed
        if (currentFrameInfo.low_res_frame) {
            result.frame = currentFrameInfo.low_res_frame;
            result.frameType = FrameInfo::LOW_RES;
            result.frameFound = true;
            foundFrame = true;
        } else if (lastFrameTypeDisplayed_ == FrameInfo::CACHED && currentFrameInfo.cached_frame) {
            result.frame = currentFrameInfo.cached_frame;
            result.frameType = FrameInfo::CACHED;
            result.frameFound = true;
            foundFrame = true;
        }
        
        if (!foundFrame) {
            lock.unlock();
            // Search for nearby frames
            bool isForward = playbackRate >= 0;
            int step = isForward ? 1 : -1;
            int searchRange = 15;
            
            for (int i = 1; i <= searchRange; ++i) {
                int checkFrameIdx = currentFrameIndex + (i * step);
                if (checkFrameIdx >= 0 && checkFrameIdx < frameIndex.size()) {
                    std::lock_guard<std::mutex> search_lock(frameIndex[checkFrameIdx].mutex);
                    if (!frameIndex[checkFrameIdx].is_decoding) {
                        if (frameIndex[checkFrameIdx].low_res_frame) {
                            result.frame = frameIndex[checkFrameIdx].low_res_frame;
                            result.frameType = FrameInfo::LOW_RES;
                            result.frameFound = true;
                            break;
                        } else if (frameIndex[checkFrameIdx].cached_frame) {
                            result.frame = frameIndex[checkFrameIdx].cached_frame;
                            result.frameType = FrameInfo::CACHED;
                            result.frameFound = true;
                            break;
                        }
                    }
                } else {
                    break;
                }
            }
        }
    }
    
    // Update last frame type for next iteration
    if (result.frameFound) {
        lastFrameTypeDisplayed_ = result.frameType;
    } else {
        // If no frame was found, reset transition counter
        frameTypeTransitionCounter_ = 0;
    }
    
    return result;
}

WindowManager::DecoderParams WindowManager::calculateDecoderParams(double fps) {
    DecoderParams params;
    
    // Calculate highResWindowSize based on FPS
    if (fps > 55.0) {
        params.highResWindowSize = 1400;
    } else if (fps > 45.0) {
        params.highResWindowSize = 1200;
    } else if (fps > 28.0) {
        params.highResWindowSize = 700;
    } else {
        params.highResWindowSize = 600;
    }
    
    // Ring buffer capacity - could also be adaptive based on FPS
    params.ringBufferCapacity = 2000;
    
    return params;
}