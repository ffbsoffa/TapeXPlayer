#ifndef WINDOW_MANAGER_H
#define WINDOW_MANAGER_H

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <string>
#include <memory>
#include <atomic>
#include <vector>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <chrono>
#include "../decode/decode.h"
#include "../main/initmanager.h"

class WindowManager {
public:
    // Constructor and destructor
    WindowManager();
    ~WindowManager();
    
    
    // Initialize SDL and create window
    bool initialize(const std::string& title, int x, int y, int width, int height, bool fullscreen);
    
    // Cleanup resources
    void cleanup();
    
    // Window operations
    void setTitle(const std::string& title);
    void setFullscreen(bool fullscreen);
    void toggleFullscreen();
    bool isFullscreen() const;
    void getWindowSize(int& width, int& height) const;
    bool hasInputFocus() const;
    
    // Frame rendering
    void beginFrame();
    void endFrame();
    void clear(int r = 0, int g = 0, int b = 0, int a = 255);
    
    // Display current frame
    void displayFrame(
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
        float targetDisplayAspectRatio
    );
    
    // OSD rendering
    void renderOSD(
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
        FrameInfo::FrameType frameType
    );
    
    // Loading screen
    void renderLoadingScreen(const LoadingStatus& status);
    
    // No file loaded screen
    void renderNoFileScreen();
    
    // Zoom functionality
    void renderZoomedFrame(SDL_Texture* texture, int frameWidth, int frameHeight, 
                          float zoomFactor, float centerX, float centerY);
    void renderZoomThumbnail(SDL_Texture* texture, int frameWidth, int frameHeight,
                            float zoomFactor, float centerX, float centerY);
    void handleZoomMouseEvent(SDL_Event& event, int frameWidth, int frameHeight);
    
    // Event handling helpers
    void enableDropFile();
    
    // Getters
    SDL_Window* getWindow() const { return window_; }
    SDL_Renderer* getRenderer() const { return renderer_; }
    TTF_Font* getFont() const { return font_; }
    int getLastTextureWidth() const { return lastTextureWidth_; }
    int getLastTextureHeight() const { return lastTextureHeight_; }
    
    // Frame timing helpers
    void setTargetFPS(int fps);
    void beginFrameTiming();
    void endFrameTiming();
    bool shouldSkipFrame() const;
    
    // Event processing helpers
    int processEvents(std::function<void(SDL_Event&)> eventHandler, int maxEvents = 10);
    
    // Frame selection logic
    struct FrameSelection {
        std::shared_ptr<AVFrame> frame;
        FrameInfo::FrameType frameType;
        bool frameFound;
        
        FrameSelection() : frame(nullptr), frameType(FrameInfo::EMPTY), frameFound(false) {}
    };
    
    FrameSelection selectFrame(
        const std::vector<FrameInfo>& frameIndex,
        int currentFrameIndex,
        double playbackRate,
        bool forceUpdate = false
    );
    
    // Reset frame selection state (e.g., after seek)
    void resetFrameSelection();
    
    // Calculate decoder parameters based on FPS
    struct DecoderParams {
        int highResWindowSize;
        size_t ringBufferCapacity;
    };
    
    static DecoderParams calculateDecoderParams(double fps);
    
    
private:
    // SDL resources
    SDL_Window* window_;
    SDL_Renderer* renderer_;
    TTF_Font* font_;
    
    // Metal renderer for macOS
    void* metalRenderer_; // Will be cast to MetalRenderer* in implementation
    
    // Window state
    bool isFullscreen_;
    int windowedX_, windowedY_;
    int windowedWidth_, windowedHeight_;
    
    // Last rendered texture dimensions
    int lastTextureWidth_;
    int lastTextureHeight_;
    
    
    // Prevent copying
    WindowManager(const WindowManager&) = delete;
    WindowManager& operator=(const WindowManager&) = delete;
    
    // Frame timing
    Uint32 frameStartTime_;
    int targetFPS_;
    Uint32 targetFrameTime_;
    bool useAdaptiveDelay_;
    
    // Event processing timing
    mutable std::chrono::steady_clock::time_point lastEventCheck_;
    mutable int consecutiveNoEvents_;
    
    // Frame selection state
    FrameInfo::FrameType lastFrameTypeDisplayed_;
    int frameTypeTransitionCounter_;
};

#endif // WINDOW_MANAGER_H