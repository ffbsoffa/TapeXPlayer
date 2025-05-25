#include <iostream>
#include <string>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <random>
#include <cmath>
#include <chrono>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <cstring> // For memset and memcpy
#include <memory> // For std::unique_ptr
#include <cmath>   // For sin, M_PI (อาจจะต้องมี _USE_MATH_DEFINES ใน Windows)
#include <random>  // For random number generation

#ifdef __APPLE__ // Include macOS specific headers only on Apple platforms
#include <CoreVideo/CoreVideo.h>
#endif

// Proper inclusion of C libraries
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
#include <libavutil/rational.h>
}

#include "../decode/decode.h"
#include "common.h" // Include common.h for playback_rate access
#include "display.h"
#include "metal_renderer.h"
#include "../audio/mainau.h" // Add this at the top with other includes
#include "../decode/decode.h" // Include for FrameInfo::FrameType

// --- Move static texture dimensions to file scope --- 
static int textureWidth = 0; 
static int textureHeight = 0;
// --- End Move ---

// External zoom variables declared in common.h
extern std::atomic<bool> zoom_enabled;
extern std::atomic<float> zoom_factor;
extern std::atomic<float> zoom_center_x;
extern std::atomic<float> zoom_center_y;
extern std::atomic<bool> show_zoom_thumbnail;
extern std::atomic<double> playback_rate; // Ensure this is declared extern in common.h

// External zoom functions declared in common.h
extern void increase_zoom();
extern void decrease_zoom();
extern void reset_zoom();
extern void set_zoom_center(float x, float y);
extern void toggle_zoom_thumbnail();

// Global variables for texture caching
SDL_Texture* lastTexture = nullptr;
int lastFrameIndex = -1;
FrameInfo::FrameType lastFrameType = FrameInfo::EMPTY;

// Global texture for the entire screen
SDL_Texture* screenTexture = nullptr;
bool screenTextureInitialized = false;
int screenWidth = 0;
int screenHeight = 0;

// --- HSync Loss Effect State Variables ---
static bool hsyncLossActive = false;
static float hsyncEffectProgress = 0.0f; // Not used directly for skew, but for overall conceptual progress
static float hsyncCurrentSkew = 0.0f;
static float hsyncMaxSkewAmount = 0.0f;
static int hsyncEffectDurationFrames = 0;
static int hsyncEffectCurrentFrame = 0;
static auto hsyncLastTriggerTime = std::chrono::steady_clock::now();
// Make HSYNC_MIN_INTERVAL shorter for debugging
static const std::chrono::milliseconds HSYNC_MIN_INTERVAL(1000); // Min 1 second between triggers (was 8000)
static float tearLineNormalized = 0.5f; // Vertical position of the "tear" (0.0 to 1.0)
// --- End HSync State ---

// Forward declarations for zoom functions
void renderZoomedFrame(SDL_Renderer* renderer, SDL_Texture* texture, int frameWidth, int frameHeight, float zoomFactor, float centerX, float centerY);
void renderZoomThumbnail(SDL_Renderer* renderer, SDL_Texture* texture, int frameWidth, int frameHeight, float zoomFactor, float centerX, float centerY);
void handleZoomMouseEvent(SDL_Event& event, int windowWidth, int windowHeight, int frameWidth, int frameHeight);

// Helper function to render the cached software texture
// UPDATED SIGNATURE to accept srcClip, dstClip, and horizontalShift
static void renderSoftwareTexture(SDL_Renderer* renderer, SDL_Texture* texture, int texWidth, int texHeight, const SDL_Rect& fullDestRect, const SDL_Rect* srcClip, const SDL_Rect* dstClip, int horizontalShift) {
    if (!texture || texWidth <= 0 || texHeight <= 0) return; // Safety check

    SDL_Rect finalDst = dstClip ? *dstClip : fullDestRect;
    // Apply horizontalShift if provided (primarily for the non-split hsync render path)
    if (!dstClip && horizontalShift != 0) { // Only apply to fullDestRect if not rendering parts
        finalDst.x += horizontalShift;
        // Basic clipping for the shifted fullDestRect
        // This is a simplified clip; more robust clipping might be needed if shift is large.
        int windowWidth, windowHeight;
        SDL_GetRendererOutputSize(renderer, &windowWidth, &windowHeight);
        if (finalDst.x + finalDst.w < 0) return; // Fully off-screen left
        if (finalDst.x > windowWidth) return;    // Fully off-screen right
        // Further refinement for partial visibility could be added if necessary.
    }


    if (zoom_enabled.load()) {
        // Zoom rendering needs to consider the srcClip and dstClip if they are provided,
        // or apply zoom to the fullDestRect if they are null.
        // A quick fix: If srcClip is provided, it means we're doing a special render (like hsync parts), so skip zoom for these parts.
        if (srcClip) {
             SDL_RenderCopy(renderer, texture, srcClip, &finalDst);
        } else {
            // If not rendering parts and zoom is on, render zoomed to the (potentially shifted) finalDst
            // renderZoomedFrame needs to be aware of the finalDst for correct positioning.
            // For now, renderZoomedFrame uses global window dimensions. This needs adjustment if zoomed hsync is desired.
            // Let's render unzoomed if horizontalShift is active for simplicity to avoid complex zoom + shift interaction for now.
            if (horizontalShift != 0 && !dstClip) { // if it's a full frame render that was shifted
                SDL_RenderCopy(renderer, texture, srcClip, &finalDst); // srcClip is likely nullptr here
            } else {
        renderZoomedFrame(renderer, texture, texWidth, texHeight, zoom_factor.load(), zoom_center_x.load(), zoom_center_y.load());
            }
        }
    } else {
        // Use the pre-calculated destRect which includes jitter offset (fullDestRect here)
        // or the specifically provided dstClip (for hsync parts)
        SDL_RenderCopy(renderer, texture, srcClip, &finalDst);
    }

    // Thumbnail should ideally use the un-distorted full frame.
    // If called for hsync parts, this will draw thumbnail multiple times or with clipped content.
    // This needs to be drawn only once per frame, with the full un-skewed texture.
    // For now, it will be drawn if zoom is enabled, potentially multiple times during hsync.
    if (zoom_enabled.load() && show_zoom_thumbnail.load() && !srcClip) { // Only draw for full frame render (not parts)
        renderZoomThumbnail(renderer, texture, texWidth, texHeight, zoom_factor.load(), zoom_center_x.load(), zoom_center_y.load());
    }
}

// Global MetalRenderer instance
#ifdef __APPLE__
MetalRenderer metalRenderer;
#endif

// Helper function (add declaration to display.h later)
static AVPixelFormat av_pix_fmt_from_sdl_format(SDL_PixelFormatEnum sdlFormat);

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

void updateVisualization(SDL_Renderer* renderer, const std::vector<FrameInfo>& frameIndex, int currentFrame, int bufferStart, int bufferEnd, int highResStart, int highResEnd, bool enableHighResDecode) {
    int totalFrames = frameIndex.size();
    if (totalFrames == 0) return; // Nothing to draw

    int windowWidth, windowHeight;
    SDL_GetRendererOutputSize(renderer, &windowWidth, &windowHeight);
    
    int indexHeight = 5; // Reduce the height of the index bar to 5 pixels
    double frameWidth = static_cast<double>(windowWidth) / totalFrames;

    // Clear only the top part of the screen for the index
    SDL_SetRenderDrawColor(renderer, 10, 10, 10, 255); // Slightly lighter background for index
    SDL_Rect indexRect = {0, 0, windowWidth, indexHeight};
    SDL_RenderFillRect(renderer, &indexRect);

    // --- Optimized Rendering: Iterate through visible pixels --- 
    for (int pixelX = 0; pixelX < windowWidth; ++pixelX) {
        // Determine the representative frame index for this pixel column
        int frameIdx = static_cast<int>(pixelX / frameWidth);
        frameIdx = std::max(0, std::min(frameIdx, totalFrames - 1)); // Clamp index

        // Get the type of the representative frame
        FrameInfo::FrameType type = frameIndex[frameIdx].type;
        bool isInBuffer = (frameIdx >= bufferStart && frameIdx <= bufferEnd);
        
        Uint8 r, g, b;
        
        // Determine color based on type and buffer status
        if (isInBuffer) {
            switch (type) {
                case FrameInfo::LOW_RES: r = 0; g = 128; b = 255; break; // Light blue
                case FrameInfo::FULL_RES: r = 255; g = 255; b = 0; break; // Yellow
                case FrameInfo::CACHED: r = 0; g = 255; b = 128; break; // Light green
                case FrameInfo::EMPTY: default: r = 64; g = 64; b = 64; break; // Dark gray
            }
        } else { // Outside buffer
            if (type == FrameInfo::CACHED) {
                r = 0; g = 128; b = 64; // Darker green
    } else {
                r = 32; g = 32; b = 32; // Very dark gray
            }
        }

        SDL_SetRenderDrawColor(renderer, r, g, b, 255);
        SDL_RenderDrawLine(renderer, pixelX, 0, pixelX, indexHeight - 1);
    }

    // --- Render current frame (playhead) --- 
    int playheadX = static_cast<int>(currentFrame * frameWidth);
    playheadX = std::max(0, std::min(playheadX, windowWidth - 1));
    SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255); // Red
    SDL_RenderDrawLine(renderer, playheadX, 0, playheadX, indexHeight - 1);
    if (frameWidth < 1.0 && playheadX < windowWidth - 1) { 
         SDL_RenderDrawLine(renderer, playheadX + 1, 0, playheadX + 1, indexHeight - 1);
    }
}

void renderOSD(SDL_Renderer* renderer, TTF_Font* font, bool isPlaying, double playbackRate, bool isReverse, double currentTime, int frameNumber, bool showOSD, bool waiting_for_timecode, const std::string& input_timecode, double original_fps, bool jog_forward, bool jog_backward, FrameInfo::FrameType frameTypeToDisplay) {
    int windowWidth, windowHeight;
    SDL_GetRendererOutputSize(renderer, &windowWidth, &windowHeight);
    SDL_Color textColor = {255, 255, 255, 255}; // White color
    SDL_Color grayColor = {128, 128, 128, 255}; // Gray color for timecode input mode

    // Draw semi-transparent background for OSD area
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 150);
    SDL_Rect osdBackgroundRect = {0, windowHeight - 30, windowWidth, 30};
    SDL_RenderFillRect(renderer, &osdBackgroundRect);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

    // Left Text
    std::string leftText;
    if (jog_forward || jog_backward) {
        leftText = "JOG";
    } else if (std::abs(playbackRate) < 0.01) {
        leftText = "STILL";
    } else if (std::abs(playbackRate) > 1.0) {
        leftText = "SHUTTLE";
    } else {
        leftText = "PLAY";
    }

    SDL_Surface* leftSurface = TTF_RenderText_Blended(font, leftText.c_str(), textColor);
    if (leftSurface) {
        SDL_Texture* leftTexture = SDL_CreateTextureFromSurface(renderer, leftSurface);
        if (leftTexture) {
            SDL_Rect leftRect = {10, windowHeight - 30 + (30 - leftSurface->h) / 2, leftSurface->w, leftSurface->h};
            SDL_RenderCopy(renderer, leftTexture, NULL, &leftRect);
            SDL_DestroyTexture(leftTexture);
        }
        SDL_FreeSurface(leftSurface);
    }

    // Timecode
    if (waiting_for_timecode) {
        // First render the full template in gray to get the total width
        std::string template_timecode = "00:00:00:00";
        SDL_Surface* templateSurface = TTF_RenderText_Blended(font, template_timecode.c_str(), grayColor);
        if (!templateSurface) return;
        
        int totalWidth = templateSurface->w;
        int totalHeight = templateSurface->h;
        SDL_FreeSurface(templateSurface);
        
        // Calculate center position
        int startX = (windowWidth - totalWidth) / 2;
        int startY = windowHeight - 30 + (30 - totalHeight) / 2;
        
        // Get the width of a single digit for positioning
        SDL_Surface* digitSurface = TTF_RenderText_Blended(font, "0", grayColor);
        if (!digitSurface) return;
        int digitWidth = digitSurface->w;
        SDL_FreeSurface(digitSurface);
        
        // Get the width of a colon for positioning
        SDL_Surface* colonSurface = TTF_RenderText_Blended(font, ":", textColor);
        if (!colonSurface) return;
        int colonWidth = colonSurface->w;
        SDL_FreeSurface(colonSurface);
        
        // Render each character
        std::string displayTimecode = "00:00:00:00";
        size_t inputLength = input_timecode.length();
        int currentX = startX;
        
        for (size_t i = 0; i < displayTimecode.length(); ++i) {
            bool isColon = (displayTimecode[i] == ':');
            size_t inputPos = i - (i > 7 ? 3 : (i > 5 ? 2 : (i > 2 ? 1 : 0)));
            
            // Determine character and color
            char currentChar = displayTimecode[i];
            SDL_Color* currentColor = &grayColor;
            
            if (isColon) {
                currentColor = &textColor; // Colons always white
            } else if (inputPos < inputLength) {
                currentChar = input_timecode[inputPos];
                currentColor = &textColor; // Entered digits white
            }
            
            // Render character
            char charStr[2] = {currentChar, '\0'};
            SDL_Surface* charSurface = TTF_RenderText_Blended(font, charStr, *currentColor);
            if (charSurface) {
                SDL_Texture* charTexture = SDL_CreateTextureFromSurface(renderer, charSurface);
                if (charTexture) {
                    SDL_Rect charRect = {
                        currentX,
                        startY,
                        charSurface->w,
                        charSurface->h
                    };
                    SDL_RenderCopy(renderer, charTexture, NULL, &charRect);
                    SDL_DestroyTexture(charTexture);
                }
                currentX += isColon ? colonWidth : digitWidth;
                SDL_FreeSurface(charSurface);
            }
        }
    } else {
        // Normal timecode display (not in input mode)
        std::string timecode = generateTXTimecode(currentTime);
        SDL_Surface* timecodeSurface = TTF_RenderText_Blended(font, timecode.c_str(), textColor);
        if (timecodeSurface) {
            SDL_Texture* timecodeTexture = SDL_CreateTextureFromSurface(renderer, timecodeSurface);
            if (timecodeTexture) {
                SDL_Rect timecodeRect = {
                    (windowWidth - timecodeSurface->w) / 2,
                    windowHeight - 30 + (30 - timecodeSurface->h) / 2,
                    timecodeSurface->w,
                    timecodeSurface->h
                };
                SDL_RenderCopy(renderer, timecodeTexture, NULL, &timecodeRect);
                SDL_DestroyTexture(timecodeTexture);
            }
            SDL_FreeSurface(timecodeSurface);
        }
    }

    // Right Text
    std::string rightText;
    if (frameTypeToDisplay == FrameInfo::FULL_RES && std::abs(playbackRate - 1.0) < 0.01) {
        rightText = "LOCK";
    } else {
        rightText = isReverse ? "REV " : "FWD ";
        char rateBuffer[10];
        if (std::abs(playbackRate) < 0.01) snprintf(rateBuffer, sizeof(rateBuffer), "0");
        else if (std::abs(playbackRate - 1.0) < 0.01) snprintf(rateBuffer, sizeof(rateBuffer), "1");
        else snprintf(rateBuffer, sizeof(rateBuffer), "%.2f", std::abs(playbackRate));
        rightText += rateBuffer;
        rightText += "x";
    }
    
    SDL_Surface* rightSurface = TTF_RenderText_Blended(font, rightText.c_str(), textColor);
    if (rightSurface) {
        SDL_Texture* rightTexture = SDL_CreateTextureFromSurface(renderer, rightSurface);
        if (rightTexture) {
            SDL_Rect rightRect = {windowWidth - rightSurface->w - 10, windowHeight - 30 + (30 - rightSurface->h) / 2, rightSurface->w, rightSurface->h};
            SDL_RenderCopy(renderer, rightTexture, NULL, &rightRect);
            SDL_DestroyTexture(rightTexture);
        }
        SDL_FreeSurface(rightSurface);
    }
}


void displayFrame(
    SDL_Renderer* renderer,
    const std::vector<FrameInfo>& frameIndex,
    int newCurrentFrame,
    std::shared_ptr<AVFrame> frameToDisplay,     // Added
    FrameInfo::FrameType frameTypeToDisplay,   // Added
    bool enableHighResDecode,
    double currentPlaybackRate, // Renamed for clarity within function scope
    double currentTime,
    double totalDuration,
    bool showIndex,
    bool showOSD,
    TTF_Font* font,
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
    // Add the new parameter to the definition
    float targetDisplayAspectRatio
) {
    auto request_time = std::chrono::high_resolution_clock::now();
    auto ms_since_epoch = std::chrono::duration_cast<std::chrono::milliseconds>(request_time.time_since_epoch()).count();
    // std::cout << "[TimingDebug:" << ms_since_epoch << "] RENDER frame request: " << newCurrentFrame << " (Segment: " << (segmentSize > 0 ? newCurrentFrame / segmentSize : -1) << ")" << std::endl; // Optional log

    bool renderedWithMetal = false; // Declare here for wider scope

    int windowWidth, windowHeight;
    SDL_GetRendererOutputSize(renderer, &windowWidth, &windowHeight);
    static bool firstFrameRendered = false; // Track if at least one frame (Metal or SW) has been shown
    // Use unique_ptr with custom deleter for automatic cleanup of SwsContext
    struct SwsContextDeleter {
        void operator()(SwsContext* ctx) const {
            if (ctx) { sws_freeContext(ctx); }
        }
    };
    static std::unique_ptr<SwsContext, SwsContextDeleter> swsContextPtr = nullptr;
    static AVPixelFormat lastSrcPixFormat = AV_PIX_FMT_NONE;
    static SDL_PixelFormatEnum lastSdlPixFormat = SDL_PIXELFORMAT_UNKNOWN;
    bool newFrameProcessed = (frameToDisplay != nullptr); // Flag: True if a new frame ptr was provided

    // --- Инициализация MetalRenderer при первом вызове (ленивая) ---
#ifdef __APPLE__
    if (!metalRenderer.isInitialized()) {
        if (!metalRenderer.initialize(renderer)) {
            std::cerr << "[Display] Failed to initialize Metal Renderer. Falling back to software rendering." << std::endl;
        }
    }
#endif

    // Clear background *before* rendering anything for this frame
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);

    AVFrame* frame = frameToDisplay ? frameToDisplay.get() : nullptr; // Get raw pointer or nullptr
    SDL_Rect destRect = {0,0,0,0}; // Destination rect for rendering

    if (frame) { // --- Start Processing if a valid frame is provided ---
        // --- Дополнительная проверка валидности полученного кадра ---
        if (frame) { // This inner check might seem redundant now but keeping for safety
            bool isPotentiallyValidHardwareFrame = (static_cast<AVPixelFormat>(frame->format) == AV_PIX_FMT_VIDEOTOOLBOX);
            // Для VideoToolbox кадров, data[0..2] могут быть NULL, data[3] содержит CVPixelBufferRef.
            // Проверка frame->width > 0 и frame->height > 0 остается важной для всех типов.
            if (!isPotentiallyValidHardwareFrame && (!frame->data[0] || frame->linesize[0] <= 0)) {
                std::cerr << "[Display] Warning: SW Frame " << newCurrentFrame
                          << " (type: " << frameTypeToDisplay << ") data or linesize is invalid "
                          << "(data[0]=" << (void*)frame->data[0]
                          << ", linesize[0]=" << frame->linesize[0]
                          << "). Marking as invalid." << std::endl;
                frame = nullptr;
            }
            if (frame && (frame->width <= 0 || frame->height <= 0)) { // Check width/height for all frames
                 std::cerr << "[Display] Warning: Frame " << newCurrentFrame
                          << " (type: " << frameTypeToDisplay << ") width/height is invalid "
                          << "(w=" << frame->width << ", h=" << frame->height
                          << "). Marking as invalid." << std::endl;
                frame = nullptr;
            }
        }
    // --- Конец дополнительной проверки ---

    // --- HSync Loss Effect Logic ---
    double absPlaybackRateForEffect = std::abs(currentPlaybackRate); // Use a local copy for this block
    bool hsyncConditionMet = (absPlaybackRateForEffect >= 1.5 && absPlaybackRateForEffect <= 2.2);

    if (hsyncConditionMet && !hsyncLossActive) {
        auto now = std::chrono::steady_clock::now();
        if (now - hsyncLastTriggerTime > HSYNC_MIN_INTERVAL) {
            // Use a thread-local random engine for better randomness if called from multiple threads,
            // but displayFrame is likely called from a single render thread.
            static std::mt19937 gen(std::random_device{}());
            // Increase probability for debugging
            std::uniform_int_distribution<> distrib(1, 30); // Chance: 1 in 30 calls (was 250)

            if (distrib(gen) == 1) {
                hsyncLossActive = true;
                hsyncEffectCurrentFrame = 0;
                // Duration 0.4 to 0.8 seconds based on typical FPS (e.g., 25-60)
                float effectDurationSec = 0.4f + (static_cast<float>(gen() % 201) / 1000.0f); // Changed to 0.4 to 0.6 sec
                hsyncEffectDurationFrames = static_cast<int>( (originalFps > 0 ? originalFps : 30.0) * effectDurationSec );
                if (hsyncEffectDurationFrames < 5) hsyncEffectDurationFrames = 5; // Minimum duration

                // Max skew amount will be calculated based on destRect.w later, if effect triggers
                // For now, set a placeholder or calculate if destRect.w is known (it's not yet)
                // hsyncMaxSkewAmount = (destRect.w * 0.03f) + (static_cast<float>(gen() % (int)(destRect.w * 0.05f)));
                hsyncLastTriggerTime = now;
            }
        }
    }

    if (hsyncLossActive) {
        hsyncEffectCurrentFrame++;
        float overallProgress = static_cast<float>(hsyncEffectCurrentFrame) / hsyncEffectDurationFrames;
        overallProgress = std::min(1.0f, std::max(0.0f, overallProgress));

        // Skew amount (0 -> max -> 0) using sine wave
        hsyncCurrentSkew = hsyncMaxSkewAmount * sin(overallProgress * M_PI);

        // Tear line animates (e.g., moves from 0.2 to 0.8 of screen height and back, over 2 cycles)
        tearLineNormalized = 0.5f + 0.3f * sin(overallProgress * M_PI * 4.0f); // Oscillates between 0.2 and 0.8

        if (hsyncEffectCurrentFrame >= hsyncEffectDurationFrames) {
            hsyncLossActive = false;
            hsyncCurrentSkew = 0.0f;
        }
    } else {
        hsyncCurrentSkew = 0.0f; // Ensure skew is zero if not active
    }
    // --- End HSync Effect Logic ---

    // --- Max Skew Amount Calculation (deferred until destRect.w is known) ---
    // Moved the actual calculation of hsyncMaxSkewAmount after destRect is defined.
    // --- End Max Skew Amount Calculation ---


    // This entire block processes the *new* frame if available
    // if (frame) { // This 'frame' is the one potentially nullified by deep pause check

        AVPixelFormat currentSrcFormat = static_cast<AVPixelFormat>(frame->format);

        // --- Попытка рендеринга через Metal ---
        // Disable Metal rendering path by adding "false &&"
        if (false && currentSrcFormat == AV_PIX_FMT_VIDEOTOOLBOX && metalRenderer.isInitialized()) {
            CVPixelBufferRef cv_pix_buf = (CVPixelBufferRef)frame->data[3];
            // std::cout << "[Display] Frame " << newCurrentFrame << ": Attempting Metal render with CVPixelBufferRef: " << cv_pix_buf << std::endl; // Log buffer address
            if (cv_pix_buf) {
                // std::cout << "[Display] Frame " << newCurrentFrame << ": Calling metalRenderer.render..." << std::endl; // Log before call
                 if (metalRenderer.render(cv_pix_buf, renderer)) {
                     renderedWithMetal = true;
                    // std::cout << "[Display] Frame " << newCurrentFrame << ": metalRenderer.render returned true." << std::endl; // Log success
                 } else {
                    std::cerr << "[Display] MetalRenderer::render failed for frame " << newCurrentFrame << std::endl;
                    renderedWithMetal = false; 
                    // Metal rendering handles its own drawing or error indication
                 }
            } else {
                std::cerr << "[Display] Error: CVPixelBufferRef is null in VideoToolbox frame " << newCurrentFrame << "!" << std::endl;
                renderedWithMetal = false; // Ensure flag is false if buffer is null
                // Metal rendering handles its own error indication
            }
            if (renderedWithMetal) {
                textureWidth = frame->width; // Update dimensions for potential thumbnail
                textureHeight = frame->height;
                firstFrameRendered = true;
            }
        }

        // --- Если не рендерили через Metal, используем старый SW путь ---
        if (!renderedWithMetal) {
             // --- Add Hardware Frame Transfer Logic ---
             AVFrame* original_frame = frame; // Keep original pointer
             AVFrame* sw_frame = nullptr;     // Pointer for potential software frame
             bool frame_transferred = false;

             if (currentSrcFormat == AV_PIX_FMT_VIDEOTOOLBOX) {
                 // std::cout << "[Display SW] Detected VideoToolbox frame " << newCurrentFrame << ". Attempting transfer..." << std::endl;
                 sw_frame = av_frame_alloc();
                 if (!sw_frame) {
                     std::cerr << "Error allocating SW frame for transfer." << std::endl;
                 } else {
                     // Choose a software format (NV12 is often efficient)
                     sw_frame->format = AV_PIX_FMT_NV12;
                     sw_frame->width = frame->width;
                     sw_frame->height = frame->height;

                     if (av_frame_get_buffer(sw_frame, 0) < 0) {
                         std::cerr << "Error allocating buffer for SW frame." << std::endl;
                         av_frame_free(&sw_frame); // Free the frame if buffer allocation failed
                         sw_frame = nullptr;
                     } else {
                         if (av_hwframe_transfer_data(sw_frame, frame, 0) < 0) {
                             std::cerr << "Error transferring hardware frame data." << std::endl;
                             av_frame_free(&sw_frame); // Free frame and buffers
                             sw_frame = nullptr;
                         } else {
                             // std::cout << "[Display SW] Successfully transferred VideoToolbox frame to SW format " << sw_frame->format << std::endl;
                             frame = sw_frame; // Use the transferred software frame from now on
                             currentSrcFormat = static_cast<AVPixelFormat>(sw_frame->format); // Update format
                             frame_transferred = true;
                         }
                     }
                 }
                 // If transfer failed, 'frame' remains the original VT frame,
                 // and the switch below might hit default/error.
             }
             // --- End Hardware Frame Transfer Logic ---


             SDL_PixelFormatEnum currentSdlFormat = SDL_PIXELFORMAT_UNKNOWN;
             bool recreateTexture = false;

             // Determine SDL format for SW path
             switch (currentSrcFormat) {
                 case AV_PIX_FMT_YUV420P: case AV_PIX_FMT_YUVJ420P: currentSdlFormat = SDL_PIXELFORMAT_IYUV; break;
                 // --- Force NV12 (from HW transfer) to be converted to IYUV for SW path ---
                 case AV_PIX_FMT_NV12: currentSdlFormat = SDL_PIXELFORMAT_IYUV; break; 
                 case AV_PIX_FMT_YUV422P: case AV_PIX_FMT_YUVJ422P: currentSdlFormat = SDL_PIXELFORMAT_UYVY; break; // Note: YUV422P might be better converted to UYVY for SDL
                 case AV_PIX_FMT_RGB24: currentSdlFormat = SDL_PIXELFORMAT_RGB24; break;
                 case AV_PIX_FMT_BGR24: currentSdlFormat = SDL_PIXELFORMAT_BGR24; break;
                 default:
                      if(currentSrcFormat != AV_PIX_FMT_NONE) { std::cerr << "Unsupported SW pixel format: " << currentSrcFormat << ". Falling back to IYUV." << std::endl; }
                      currentSdlFormat = SDL_PIXELFORMAT_IYUV;
                      currentSrcFormat = AV_PIX_FMT_YUV420P;
                      break;
             }

             // Check if texture needs recreation
             if (!lastTexture || textureWidth != frame->width || textureHeight != frame->height || lastSdlPixFormat != currentSdlFormat) {
                 if (lastTexture) { SDL_DestroyTexture(lastTexture); lastTexture = nullptr; }
                 lastTexture = SDL_CreateTexture(renderer, currentSdlFormat, SDL_TEXTUREACCESS_STREAMING, frame->width, frame->height);
                 if (lastTexture) {
                     textureWidth = frame->width;
                     textureHeight = frame->height;
                     lastSdlPixFormat = currentSdlFormat;
                     SDL_SetTextureScaleMode(lastTexture, SDL_ScaleModeBest);
                     recreateTexture = true;
                     // std::cout << "[Display SW] Created SDL Texture: " << textureWidth << "x" << textureHeight << " Format: " << SDL_GetPixelFormatName(currentSdlFormat) << std::endl; // Optional log
                 } else {
                     std::cerr << "Error creating SDL texture for SW path: " << SDL_GetError() << std::endl;
                     lastTexture = nullptr;
                 }
             }

             // Check SwsContext
             AVPixelFormat targetAvFormat = av_pix_fmt_from_sdl_format(currentSdlFormat); // Determine target AV format for sws_scale
             bool conversionNeeded = (targetAvFormat != currentSrcFormat && targetAvFormat != AV_PIX_FMT_NONE);

             // --- Explicitly force conversion if source was NV12 (from VT) and target is IYUV ---
             if (currentSrcFormat == AV_PIX_FMT_NV12 && targetAvFormat == AV_PIX_FMT_YUV420P) {
                 conversionNeeded = true;
             }

             if (conversionNeeded && (!swsContextPtr || lastSrcPixFormat != currentSrcFormat || recreateTexture)) {
                 // unique_ptr.reset() will automatically free the old context if it exists
                 if (targetAvFormat != AV_PIX_FMT_NONE) {
                     SwsContext* newCtx = sws_getContext(frame->width, frame->height, currentSrcFormat, frame->width, frame->height, targetAvFormat, SWS_BILINEAR, nullptr, nullptr, nullptr);
                     if (!newCtx) { std::cerr << "Error creating SwsContext for " << currentSrcFormat << " -> " << SDL_GetPixelFormatName(currentSdlFormat) << std::endl; }
                     swsContextPtr.reset(newCtx);
                     // else { std::cout << "[Display] Created SwsContext: " << currentSrcFormat << " -> " << SDL_GetPixelFormatName(currentSdlFormat) << std::endl; } // Optional log
                     } else {
                    std::cerr << "Error: Cannot determine target AVFormat for SDL format " << SDL_GetPixelFormatName(currentSdlFormat) << std::endl;
                    swsContextPtr.reset(); // Ensure ptr is null if context creation fails
                     }
                 }
             lastSrcPixFormat = currentSrcFormat; // Update last source format processed

             // Update SW texture data
             if (lastTexture) {
                          uint8_t* pixels = nullptr;
                          int pitch = 0;
                          if (SDL_LockTexture(lastTexture, nullptr, (void**)&pixels, &pitch) == 0) {
                               uint8_t* dst_data[4] = { pixels, nullptr, nullptr, nullptr };
                               int dst_linesize[4] = { pitch, 0, 0, 0 };
                        // Setup destData/linesize based on the *target* AV format for sws_scale
                               if (targetAvFormat == AV_PIX_FMT_NV12) { dst_data[1] = pixels + pitch * frame->height; dst_linesize[1] = pitch; }
                       else if (targetAvFormat == AV_PIX_FMT_UYVY422) { /* single plane pitch ok */ }
                               else if (targetAvFormat == AV_PIX_FMT_YUV420P) { dst_data[1] = pixels + pitch * frame->height; dst_data[2] = pixels + pitch * frame->height * 5 / 4; dst_linesize[1] = pitch / 2; dst_linesize[2] = pitch / 2; }
                       else if (targetAvFormat == AV_PIX_FMT_RGB24 || targetAvFormat == AV_PIX_FMT_BGR24) { /* single plane pitch ok */ }
                       // Add more cases if other target formats are supported

                       if (conversionNeeded) {
                           if (swsContextPtr) {
                               sws_scale(swsContextPtr.get(), (const uint8_t* const*)frame->data, frame->linesize, 0, frame->height, dst_data, dst_linesize);
                      } else { std::cerr << "Error: SwsContext is null for needed SW conversion!" << std::endl; }
                  } else {
                           // Direct copy if formats match
                           // Safer row-by-row copy, respecting linesizes
                           if (currentSdlFormat == SDL_PIXELFORMAT_IYUV && currentSrcFormat == AV_PIX_FMT_YUV420P) {
                               // Copy Y plane row by row
                               for (int y = 0; y < frame->height; ++y) {
                                   if (dst_data[0] && frame->data[0]) { // Safety check pointers
                                       memcpy(dst_data[0] + y * dst_linesize[0], // Dest: Start of row y in texture Y plane
                                              frame->data[0] + y * frame->linesize[0], // Src: Start of row y in frame Y plane
                                              frame->width); // Copy width bytes (actual data width)
                                   }
                               }
                               // Copy U plane row by row (height/2 rows, width/2 bytes per row)
                               for (int y = 0; y < frame->height / 2; ++y) {
                                   if (dst_data[1] && frame->data[1]) { // Safety check pointers
                                       memcpy(dst_data[1] + y * dst_linesize[1], // Dest: Start of row y in texture U plane
                                              frame->data[1] + y * frame->linesize[1], // Src: Start of row y in frame U plane
                                              frame->width / 2); // Copy width/2 bytes
                                   }
                               }
                               // Copy V plane row by row (height/2 rows, width/2 bytes per row)
                               for (int y = 0; y < frame->height / 2; ++y) {
                                   if (dst_data[2] && frame->data[2]) { // Safety check pointers
                                        memcpy(dst_data[2] + y * dst_linesize[2], // Dest: Start of row y in texture V plane
                                               frame->data[2] + y * frame->linesize[2], // Src: Start of row y in frame V plane
                                               frame->width / 2); // Copy width/2 bytes
                                   }
                               }
                           } else if (currentSdlFormat == SDL_PIXELFORMAT_UYVY && currentSrcFormat == AV_PIX_FMT_UYVY422) {
                               // Example for packed format (can still use row-by-row for safety)
                               for (int y = 0; y < frame->height; ++y) {
                                    if (dst_data[0] && frame->data[0]) { // Safety check pointers
                                        memcpy(dst_data[0] + y * dst_linesize[0],
                                               frame->data[0] + y * frame->linesize[0],
                                               frame->width * 2); // UYVY is 2 bytes per pixel
                                    }
                               }
                           } // Add other direct copy cases if needed (e.g., RGB24)
                       }

                       // --- Apply Betacam Effects ---
                       // These effects are applied to the new frame data being put into lastTexture.
                       double absPlaybackRate = std::abs(currentPlaybackRate);
                       const double effectThreshold = 1.2; // Slightly above 1x to trigger effects
                       if (absPlaybackRate >= effectThreshold && currentTime > 0.1 && (totalDuration - currentTime) > 0.1) {
                           // Apply B&W effect for high speeds (>= 10.0x)
                           if (absPlaybackRate >= 10.0) {
                               if (lastSdlPixFormat == SDL_PIXELFORMAT_IYUV) {
                                   memset(dst_data[1], 128, dst_linesize[1] * textureHeight / 2); // U
                                   memset(dst_data[2], 128, dst_linesize[2] * textureHeight / 2); // V
                               } else if (lastSdlPixFormat == SDL_PIXELFORMAT_NV12) {
                                   for (int y = 0; y < textureHeight / 2; ++y) {
                                       uint8_t* uvPlane = dst_data[1] + y * dst_linesize[1];
                                       for (int x = 0; x < textureWidth / 2; ++x) { uvPlane[x*2]=128; uvPlane[x*2+1]=128; }
                                   }
                               }
                           }

                           // Calculate stripe parameters
                           const double resolutionScale = static_cast<double>(textureHeight) / 1080.0;
                           const int maxStripeHeight = static_cast<int>(720 * resolutionScale);
                           const int baseStripeHeight = static_cast<int>(85 * resolutionScale);
                           const int baseStripeSpacing = static_cast<int>(450 * resolutionScale);
                           const int minStripeSpacing = static_cast<int>(62 * resolutionScale);
                           int currentMinStripeHeight = (absPlaybackRate >= 16.0) ? 1 : static_cast<int>(14 * resolutionScale); // Set min height to 1 for >= 16x
                           const int midStripeHeight = static_cast<int>(50 * resolutionScale);
                           int stripeHeight, stripeSpacing;
                           // ... (rest of stripe parameter calculation based on absPlaybackRate, identical to old code) ...
                            if ((absPlaybackRate >= 0.2 && absPlaybackRate < 0.9) || (absPlaybackRate >= 1.1 && absPlaybackRate < 2.0)) {
                                double t = (absPlaybackRate < 0.9) ? (absPlaybackRate - 0.2) / 0.7 : (absPlaybackRate - 1.2) / 0.8;
                                t = t * t * (3 - 2 * t); stripeHeight = static_cast<int>(maxStripeHeight * (1 - t) + midStripeHeight * t); stripeSpacing = baseStripeSpacing;
                            } else if (absPlaybackRate >= 2.0 && absPlaybackRate < 4.0) {
                                stripeHeight = baseStripeHeight; stripeSpacing = baseStripeSpacing;
                            } else if (absPlaybackRate >= 3.5 && absPlaybackRate < 14.0) {
                                double t = (absPlaybackRate - 4.0) / 10.0; t = std::pow(t, 0.7);
                                stripeHeight = static_cast<int>(baseStripeHeight * (1.0 - t) + currentMinStripeHeight * t);
                                stripeSpacing = static_cast<int>(baseStripeSpacing * (1.0 - t) + minStripeSpacing * t);
                            } else { // Covers >= 14.0x
                                 stripeHeight = currentMinStripeHeight; 
                                 stripeSpacing = minStripeSpacing; 
                                 // --- Set very low min height for 20x+ specifically ---
                                 if (absPlaybackRate >= 20.0) {
                                     currentMinStripeHeight = static_cast<int>(12.0 * resolutionScale); // Set to 1px scaled
                                     stripeHeight = currentMinStripeHeight; // Ensure stripeHeight uses this new minimum
                                 }
                                 // --- End Set low min height ---
                            }


                           // Calculate cycle progress for animation
                           double cycleProgress;
                           const double baseDuration = 1.5;
                           const int fps = static_cast<int>(originalFps > 0 ? originalFps : 30); // Use actual FPS if available
                           const double minCycleDuration = 0.05; // 50ms minimum cycle time
                           
                           if (absPlaybackRate >= 12.0) { // New logic for high speeds (12x+)
                                double cycleDuration = (baseDuration / absPlaybackRate) * 3.0; // Simple inverse relation + factor
                                cycleDuration = std::max(minCycleDuration, cycleDuration); // Apply minimum duration
                                cycleProgress = std::fmod(currentTime, cycleDuration) / cycleDuration;
                           } else if (absPlaybackRate >= 3.5) { // Covers 3.5x up to 12.0x (Old medium speed logic)
                                double normalizedSpeed = (absPlaybackRate - 3.5) / 8.5; // Normalize in 3.5-12 range
                                double speedMultiplier = 0.08 + (std::pow(normalizedSpeed, 3) * 0.15);
                                if (absPlaybackRate < 8.0) speedMultiplier *= 0.7; // Keep the old adjustment for < 8x
                                double mediumSpeedDuration = baseDuration / (absPlaybackRate * speedMultiplier);
                                mediumSpeedDuration = std::max(minCycleDuration, mediumSpeedDuration); // Apply minimum duration
                                cycleProgress = std::fmod(currentTime, mediumSpeedDuration) / mediumSpeedDuration;
                           } else { // Covers >= effectThreshold (1.1x) up to 3.5x (unchanged)
                                double speedFactor = std::min(absPlaybackRate / 2.0, 2.0) * 0.5; 
                                double adjustedDuration = baseDuration / speedFactor; 
                                if (adjustedDuration <= 0) adjustedDuration = 1.0; // Fallback
                                cycleProgress = std::fmod(currentTime, adjustedDuration) / adjustedDuration; 
                           }


                           // --- Generate Stripe Positions with Randomized Spacing ---
                           std::vector<std::pair<int, int>> stripePositions; // Store {startY, height}
                           std::vector<int> stripeSpacings; // Store spacing *after* each stripe

                           // Use the calculated stripeSpacing for the current speed as the *base* for randomization
                           int baseSpacingForSpeed; // Declared here, calculated below
                           // --- Adjust base spacing based on specific speed ranges --- 
                           double currentSpacingVariationFactor = 0.001; // Keep default variation low initially
                           
                           // Define target spacings for interpolation points
                           const double spacingAt12x = 100.0 * resolutionScale;
                           const double spacingAt24x = 18.0 * resolutionScale; // New target spacing

                           if (absPlaybackRate >= 2.0 && absPlaybackRate < 3.5) {
                                // Add 86 pixels for ~3x range to make stripes sparse (applied to base stripeSpacing)
                                baseSpacingForSpeed = stripeSpacing + static_cast<int>((10.0 + 76.0) * resolutionScale); 
                           } else if (absPlaybackRate >= 8.0 && absPlaybackRate < 12.0) { // Fixed range for 10x spacing
                                // Force smaller spacing for ~10x range
                                baseSpacingForSpeed = static_cast<int>(spacingAt12x); 
                           } else if (absPlaybackRate >= 12.0) { // Interpolate from 12x up to 24x, then hold
                               double t = std::min(1.0, (absPlaybackRate - 12.0) / (24.0 - 12.0)); // Normalize 0 to 1 between 12x and 24x
                               baseSpacingForSpeed = static_cast<int>(spacingAt12x * (1.0 - t) + spacingAt24x * t);
                           } else { // Covers < 8x (excluding 2.0-3.5 range handled above)
                                // Use the originally calculated stripeSpacing which interpolates height/spacing
                               baseSpacingForSpeed = stripeSpacing; 
                           }
                           // --- End Adjust base spacing --- 

                           std::mt19937 rng(std::random_device{}()); // Random number generator
                           // Use the potentially modified spacing variation factor
                           double spacingVariationFactor = currentSpacingVariationFactor; 
                           int minAllowedSpacing = std::max(1, minStripeSpacing / 4); // Reduce min allowed spacing for high speeds

                           // Calculate the initial vertical offset based on cycleProgress and *average* spacing/height
                           // Ensures the pattern starts scrolling from off-screen smoothly
                           int averagePatternUnitHeight = baseStripeHeight + baseSpacingForSpeed;
                           // Ensure averagePatternUnitHeight is positive to avoid division by zero or negative fmod issues
                           if (averagePatternUnitHeight <= 0) averagePatternUnitHeight = 1; 
                           double initialOffset = std::fmod(cycleProgress * averagePatternUnitHeight, averagePatternUnitHeight);
                           int currentY = static_cast<int>(initialOffset) - averagePatternUnitHeight; 


                           // Now start the loop to calculate and store stripes, starting from the potentially negative currentY
                           while (currentY < textureHeight) {
                               // Calculate height for this potential stripe (variable random variation)
                               int finalStripeHeight; // Use a new variable for the final calculated height                               
                               if (absPlaybackRate >= 16.0) {
                                   // For >= 16x, vary height slightly randomly from 1 to 3 pixels
                                   finalStripeHeight = 1 + (rand() % 3); // Generates 1, 2, 3
                               } else {
                                   // Minimal height variation for other speeds (+/- 1 pixel before scaling)
                                   double heightVariation = ((rand() % 3 - 1) * resolutionScale); 
                                   // Use the potentially reduced currentMinStripeHeight for clamping
                                   finalStripeHeight = std::max(static_cast<int>(stripeHeight + heightVariation), currentMinStripeHeight);
                               }
                               // Ensure height is at least 1 pixel if scale is very small
                               finalStripeHeight = std::max(1, finalStripeHeight); 

                               // Calculate randomized spacing for the *next* gap (using current variation factor)
                               int currentStripeSpacing;
                               if (absPlaybackRate >= 16.0) {
                                   // For >= 16x, apply subtle +/- 2 pixel random offset to base spacing
                                   int randomOffset = (rand() % 5) - 2; // Generates -2, -1, 0, 1, 2
                                   currentStripeSpacing = std::max(1, static_cast<int>(baseSpacingForSpeed + randomOffset)); // Ensure spacing is at least 1
                               } else {
                                   // Apply smaller variation for lower speeds
                                   int randomSpacingOffset = static_cast<int>(baseSpacingForSpeed * spacingVariationFactor * 2.0 * ((double)rand() / RAND_MAX - 0.5)); // +/- variation
                                   currentStripeSpacing = std::max(minAllowedSpacing, baseSpacingForSpeed + randomSpacingOffset);
                               }

                               // Determine the actual start and end Y for drawing, clamping to texture bounds
                               // This will handle cases where currentY is negative
                               int startY = std::max(0, currentY);
                               int endY = std::min(textureHeight, currentY + finalStripeHeight);

                               // Store position only if the stripe is at least partially visible
                               if (endY > startY && startY < textureHeight) {
                                   stripePositions.push_back({startY, endY - startY}); // Store {start, actual_height}
                               }

                               stripeSpacings.push_back(currentStripeSpacing); // Store spacing for consistency if needed later

                               // Advance Y position using the *final* calculated height
                               currentY += finalStripeHeight + currentStripeSpacing;
                               
                               // Safety break to prevent infinite loops in case of calculation issues
                               if (stripePositions.size() > textureHeight) { // Unlikely, but safe
                                    std::cerr << "Warning: Excessive stripe calculation, breaking loop." << std::endl;
                                    break;
                               }
                           }
                           // --- End Stripe Position Generation ---


                           // Apply B&W zones under stripes (2x-10x) - USE STORED POSITIONS
                           if (absPlaybackRate >= 2.0 && absPlaybackRate < 10.0) {
                               for (const auto& pos : stripePositions) {
                                   int startY = pos.first;
                                   int currentStripeHeight = pos.second; // Use stored actual height
                                   if (currentStripeHeight <= 0) continue;

                                   int bwZoneHeight = static_cast<int>(currentStripeHeight * 1.75);
                                   int heightDifference = bwZoneHeight - currentStripeHeight;
                                   int y_bw = startY - heightDifference / 2;

                                    if (y_bw < textureHeight && y_bw + bwZoneHeight > 0) {
                                       int bwStartY = std::max(0, y_bw);
                                       int bwEndY = std::min(textureHeight, y_bw + bwZoneHeight);
                                        for (int yPos = bwStartY; yPos < bwEndY; ++yPos) {
                                            uint8_t* rowStart = dst_data[0] + yPos * pitch;
                                            if (lastSdlPixFormat == SDL_PIXELFORMAT_UYVY) {
                                                for (int x = 0; x < textureWidth; ++x) { // Iterate per pixel
                                                    // Darken Y component
                                                    rowStart[x * 2 + 1] = static_cast<uint8_t>(rowStart[x * 2 + 1] * 0.85f);
                                                    // Set U and V to 128 for B&W
                                                    if (x % 2 == 0) rowStart[x * 2] = 128; // U for even pixels
                                                    else rowStart[x * 2] = 128;           // V for odd pixels (actually U for next pair, V for current if x*2-1)
                                                                                         // Correct way for UYVY: U is at (pixel/2)*4, V is at (pixel/2)*4+2
                                                }
                                                // Simpler: Iterate by pairs for UYVY
                                                for (int i = 0; i < textureWidth / 2; ++i) { // Iterate per UYVY group
                                                    uint8_t* uyvy_group = rowStart + i * 4;
                                                    uyvy_group[0] = 128; // U
                                                    uyvy_group[1] = static_cast<uint8_t>(uyvy_group[1] * 0.85f); // Y0
                                                    uyvy_group[2] = 128; // V
                                                    uyvy_group[3] = static_cast<uint8_t>(uyvy_group[3] * 0.85f); // Y1
                                                }
                                            } else { // Existing logic for IYUV and NV12
                                                for (int x = 0; x < textureWidth; ++x) { rowStart[x] = static_cast<uint8_t>(rowStart[x] * 0.85); } // Darken Y (for planar Y)
                                                if (lastSdlPixFormat == SDL_PIXELFORMAT_IYUV) {
                                                    int uvY = yPos / 2; if (uvY < textureHeight / 2) { memset(dst_data[1] + uvY * dst_linesize[1], 128, textureWidth / 2); memset(dst_data[2] + uvY * dst_linesize[2], 128, textureWidth / 2); }
                                                } else if (lastSdlPixFormat == SDL_PIXELFORMAT_NV12) {
                                                    int uvY = yPos / 2; if (uvY < textureHeight / 2) { uint8_t* uvPlane = dst_data[1] + uvY * dst_linesize[1]; for (int x = 0; x < textureWidth / 2; ++x) { uvPlane[x*2]=128; uvPlane[x*2+1]=128; } }
                                                }
                                            }
                                        }
                                    }
                               }
                           }

                           // Draw grey stripes - USE STORED POSITIONS
                           for (const auto& pos : stripePositions) {
                               int startY = pos.first;
                               int currentStripeHeight = pos.second;
                               int endY = startY + currentStripeHeight;

                               if (startY < textureHeight && endY > 0 && currentStripeHeight > 0) {
                                   startY = std::max(0, startY);
                                   endY = std::min(textureHeight, endY);

                                   for (int yPos = startY; yPos < endY; ++yPos) {
                                       uint8_t* rowStart = dst_data[0] + yPos * pitch;
                                       if (lastSdlPixFormat == SDL_PIXELFORMAT_UYVY) {
                                           for (int i = 0; i < textureWidth / 2; ++i) { // Iterate per UYVY group
                                               uint8_t* uyvy_group = rowStart + i * 4;
                                               uyvy_group[0] = 128; // U
                                               uyvy_group[1] = 128; // Y0
                                               uyvy_group[2] = 128; // V
                                               uyvy_group[3] = 128; // Y1
                                           }
                                       } else {
                                           memset(rowStart, 128, textureWidth); // Y plane for planar formats
                                           if (lastSdlPixFormat == SDL_PIXELFORMAT_IYUV) {
                                               int uvY = yPos / 2; if (uvY < textureHeight / 2) { memset(dst_data[1] + uvY * dst_linesize[1], 128, textureWidth / 2); memset(dst_data[2] + uvY * dst_linesize[2], 128, textureWidth / 2); }
                                           } else if (lastSdlPixFormat == SDL_PIXELFORMAT_NV12) {
                                               int uvY = yPos / 2; if (uvY < textureHeight / 2) { uint8_t* uvPlane = dst_data[1] + uvY * dst_linesize[1]; for (int x = 0; x < textureWidth / 2; ++x) { uvPlane[x*2]=128; uvPlane[x*2+1]=128; } }
                                           }
                                       }
                                   }
                                   // --- Restore stripe drawing logic --- 
                                   for (int yPos = startY; yPos < endY; ++yPos) {
                                       memset(dst_data[0] + yPos * pitch, 128, textureWidth); // Y
                                       if (lastSdlPixFormat == SDL_PIXELFORMAT_IYUV) {
                                           int uvY = yPos / 2; if (uvY < textureHeight / 2) { memset(dst_data[1] + uvY * dst_linesize[1], 128, textureWidth / 2); memset(dst_data[2] + uvY * dst_linesize[2], 128, textureWidth / 2); }
                                       } else if (lastSdlPixFormat == SDL_PIXELFORMAT_NV12) {
                                           int uvY = yPos / 2; if (uvY < textureHeight / 2) { uint8_t* uvPlane = dst_data[1] + uvY * dst_linesize[1]; for (int x = 0; x < textureWidth / 2; ++x) { uvPlane[x*2]=128; uvPlane[x*2+1]=128; } }
                                       }
                                   }
                                   // --- End of restored stripe drawing logic --- 

                                   // Add black outline below the stripe - USES endY from current stripe
                                   if (absPlaybackRate >= 8.0) {
                                       const double outlineStartSpeed = 8.0;
                                       const double outlineFullSpeed = 14.0;
                                       double t = std::max(0.0, std::min(1.0, (absPlaybackRate - outlineStartSpeed) / (outlineFullSpeed - outlineStartSpeed)));
                                       uint8_t targetYValue = 16 + static_cast<uint8_t>((128 - 16) * (1.0 - t));
                                       int currentOutlineHeight = 1;
                                       for (int h = 0; h < currentOutlineHeight; ++h) {
                                           int outlineY = endY + h;
                                           if (outlineY < textureHeight) {
                                               uint8_t* rowStart = dst_data[0] + outlineY * pitch;
                                               if (lastSdlPixFormat == SDL_PIXELFORMAT_UYVY) {
                                                   for (int i = 0; i < textureWidth / 2; ++i) { // Iterate per UYVY group
                                                       uint8_t* uyvy_group = rowStart + i * 4;
                                                       // For outline, usually only Y is set, U/V remain (or set to neutral grey)
                                                       uyvy_group[0] = 128; // U (neutral grey)
                                                       uyvy_group[1] = targetYValue; // Y0
                                                       uyvy_group[2] = 128; // V (neutral grey)
                                                       uyvy_group[3] = targetYValue; // Y1
                                                   }
                                               } else {
                                                   memset(rowStart, targetYValue, textureWidth); // Y plane for planar
                                                   // UV handling for outline (existing code for IYUV/NV12)
                                                   if (lastSdlPixFormat == SDL_PIXELFORMAT_IYUV) {
                                                        int uvY = outlineY / 2; if (uvY < textureHeight / 2) { memset(dst_data[1] + uvY * dst_linesize[1], 128, textureWidth / 2); memset(dst_data[2] + uvY * dst_linesize[2], 128, textureWidth / 2); }
                                                   } else if (lastSdlPixFormat == SDL_PIXELFORMAT_NV12) {
                                                        int uvY = outlineY / 2; if (uvY < textureHeight / 2) { uint8_t* uvPlane = dst_data[1] + uvY * dst_linesize[1]; for (int x = 0; x < textureWidth / 2; ++x) { uvPlane[x*2]=128; uvPlane[x*2+1]=128; } }
                                                   }
                                               }
                                           }
                                       }
                                   }

                                   // Add snow effect (>= 4x) - USES startY from current stripe
                                   if (absPlaybackRate >= 4.0 && startY < textureHeight) {
                                       int snowY = startY;
                                       int snowCount = std::max(8, textureWidth / 80);
                                       if (absPlaybackRate > 10.0) snowCount = static_cast<int>(snowCount * 1.5);

                                       for (int j = 0; j < snowCount; ++j) {
                                           int snowX = rand() % textureWidth; // snowX is pixel index
                                           double speedFactor_snow = std::sqrt(absPlaybackRate);
                                           int tailBase = 10 + static_cast<int>(rand() % 20);
                                           int tailLength = tailBase + static_cast<int>(speedFactor_snow * 5.0);

                                           if (snowY >= 0 && snowY < textureHeight && snowX >= 0 && snowX < textureWidth) {
                                               uint8_t* rowStart = dst_data[0] + snowY * pitch;
                                               if (lastSdlPixFormat == SDL_PIXELFORMAT_UYVY) {
                                                   // Snow is white, set Y to bright, U/V to neutral
                                                   // snowX is the pixel index from 0 to textureWidth-1
                                                   int baseByteOffset = snowX * 2;
                                                   if (baseByteOffset + 1 < pitch) { // Check boundary for Y
                                                      rowStart[baseByteOffset + 1] = 235; // Y
                                                      if (snowX % 2 == 0) { // This Y is Y0, U is at baseByteOffset
                                                          if (baseByteOffset < pitch) rowStart[baseByteOffset] = 128; // U
                                                          if (baseByteOffset + 2 < pitch) rowStart[baseByteOffset+2] = 128; // V
                                                      } else { // This Y is Y1, V is at baseByteOffset
                                                          // U is at baseByteOffset - 1, V is at baseByteOffset
                                                          if (baseByteOffset -1 >= 0) rowStart[baseByteOffset-1] = 128; // U
                                                          if (baseByteOffset < pitch) rowStart[baseByteOffset] = 128; // V
                                                      }
                                                      // Correct UYVY snow (white):
                                                      // For pixel snowX: U at (snowX/2)*4, Y0 at (snowX/2)*4+1, V at (snowX/2)*4+2, Y1 at (snowX/2)*4+3
                                                      int group_idx = snowX / 2;
                                                      int in_group_idx = snowX % 2; // 0 for Y0, 1 for Y1
                                                      uint8_t* uyvy_pixel_group = rowStart + group_idx * 4;
                                                      if (group_idx * 4 + (1 + in_group_idx*2) < pitch) { // Check Y component
                                                          uyvy_pixel_group[0] = 128; // U
                                                          uyvy_pixel_group[1 + in_group_idx*2] = 235; // Y (Y0 or Y1)
                                                          uyvy_pixel_group[2] = 128; // V
                                                      }
                                                   }
                                               } else {
                                                   rowStart[snowX] = 235; // Y plane for planar
                                               }
                                           }

                                           for (int k = 1; k < tailLength; k++) {
                                               int xPos = (snowX + k);
                                               if (xPos >= textureWidth) continue;

                                               double fadeFactor = exp(-0.15 * k);
                                               int brightness = 128 + static_cast<int>(107 * fadeFactor);

                                               if (snowY >= 0 && snowY < textureHeight && xPos >= 0 && xPos < textureWidth) {
                                                   uint8_t* rowStart = dst_data[0] + snowY * pitch;
                                                    if (lastSdlPixFormat == SDL_PIXELFORMAT_UYVY) {
                                                       int group_idx = xPos / 2;
                                                       int in_group_idx = xPos % 2;
                                                       uint8_t* uyvy_pixel_group = rowStart + group_idx * 4;
                                                       if (group_idx * 4 + (1 + in_group_idx*2) < pitch) { // Check Y component
                                                            uyvy_pixel_group[0] = 128; // U
                                                            uyvy_pixel_group[1 + in_group_idx*2] = brightness; // Y (Y0 or Y1)
                                                            uyvy_pixel_group[2] = 128; // V
                                                       }
                                                    } else {
                                                       rowStart[xPos] = brightness; // Y plane for planar
                                                    }
                                               }
                                           }
                                       }
                                   }
                               }
                           }

                           // Scanline duplication effect (>= 16x) - NEEDS UPDATED STRIPE MASK
                            if (absPlaybackRate >= 16.0) {
                                // Reduce maximum intensity (existing code)
                                double effectIntensity = std::min(0.85, (absPlaybackRate - 16.0) / 8.0);
                                std::vector<bool> stripeMask(textureHeight, false);

                                // --- Re-calculate stripeMask based on STORED POSITIONS ---
                                // Mark B&W zones first (if active)
                                    if (absPlaybackRate >= 2.0 && absPlaybackRate < 10.0) {
                                    for (const auto& pos : stripePositions) {
                                        int startY = pos.first;
                                        int currentStripeHeight = pos.second;
                                        if (currentStripeHeight <= 0) continue;
                                        int bwZoneHeight = static_cast<int>(currentStripeHeight * 1.75);
                                        int heightDifference = bwZoneHeight - currentStripeHeight;
                                        int y_bw = startY - heightDifference / 2;
                                        if (y_bw < textureHeight && y_bw + bwZoneHeight > 0) {
                                            int bwStartY = std::max(0, y_bw);
                                            int bwEndY = std::min(textureHeight, y_bw + bwZoneHeight);
                                            for (int yy = bwStartY; yy < bwEndY; ++yy) stripeMask[yy] = true;
                                        }
                                    }
                                }
                                
                                // Mark actual grey stripes
                                for (const auto& pos : stripePositions) {
                                    int startY = pos.first;
                                    int currentStripeHeight = pos.second;
                                    int endY = startY + currentStripeHeight;
                                    if (startY < textureHeight && endY > 0 && currentStripeHeight > 0) {
                                        startY = std::max(0, startY);
                                        endY = std::min(textureHeight, endY);
                                        for (int yy = startY; yy < endY; ++yy) stripeMask[yy] = true;

                                        // Also mark the black outline in the mask if it exists
                                        if (absPlaybackRate >= 8.0) {
                                            int currentOutlineHeight = 1;
                                            for (int h = 0; h < currentOutlineHeight; ++h) {
                                                int outlineY = endY + h;
                                                if (outlineY < textureHeight) {
                                                    stripeMask[outlineY] = true;
                                                }
                                            }
                                        }
                                    }
                                }
                                // --- End Stripe Mask Calculation ---


                                // Find clear areas and apply duplication (existing logic using the new mask)
                                bool inClearArea = false; int clearStart = 0;
                                for (int y = 0; y < textureHeight; ++y) {
                                    if (!stripeMask[y] && !inClearArea) { clearStart = y; inClearArea = true; }
                                    else if ((stripeMask[y] || y == textureHeight - 1) && inClearArea) {
                                        int clearEnd = stripeMask[y] ? y : y + 1;
                                        if (clearEnd - clearStart > 1) {
                                            int areaStart = clearStart;
                                            int areaEnd = clearEnd;
                                            int sourceLineY = areaStart;

                                            if (sourceLineY >= 0 && sourceLineY < textureHeight && pitch > 0) {
                                                uint8_t* srcLine = dst_data[0] + sourceLineY * pitch;
                                                for (int destY = areaStart + 1; destY < areaEnd; destY++) {
                                                    uint8_t* dstLine = dst_data[0] + destY * pitch;
                                                    if (lastSdlPixFormat == SDL_PIXELFORMAT_UYVY) {
                                                        memcpy(dstLine, srcLine, textureWidth * 2); // UYVY is 2 bytes per pixel
                                                    } else {
                                                        memcpy(dstLine, srcLine, textureWidth); // Y plane for planar
                                                    }

                                                    if ((sourceLineY/2 != destY/2) && (destY/2 < textureHeight/2)) {
                                                        if (lastSdlPixFormat == SDL_PIXELFORMAT_IYUV) {
                                                            if (dst_linesize[1] > 0) memcpy(dst_data[1] + destY/2 * dst_linesize[1], dst_data[1] + sourceLineY/2 * dst_linesize[1], textureWidth/2);
                                                            if (dst_linesize[2] > 0) memcpy(dst_data[2] + destY/2 * dst_linesize[2], dst_data[2] + sourceLineY/2 * dst_linesize[2], textureWidth/2);
                                                        } else if (lastSdlPixFormat == SDL_PIXELFORMAT_NV12) {
                                                            if (dst_linesize[1] > 0) memcpy(dst_data[1] + destY/2 * dst_linesize[1], dst_data[1] + sourceLineY/2 * dst_linesize[1], textureWidth);
                                                        }
                                                        // No specific U/V copy for UYVY here as memcpy of dstLine should cover it.
                                                    }
                                                }
                                            }
                                        }
                                        inClearArea = false;
                                    }
                                }
                            }
                       } // End if absPlaybackRate >= effectThreshold

                       // --- Apply Edge Fade ---
                       const int leftEdgeFadeWidth = 3;
                       const int rightEdgeFadeWidth = 2;
                       if ((leftEdgeFadeWidth > 0 || rightEdgeFadeWidth > 0) && textureWidth > (leftEdgeFadeWidth + rightEdgeFadeWidth) && pitch > 0) {
                            for (int y = 0; y < textureHeight; ++y) {
                                uint8_t* rowStart = dst_data[0] + y * pitch;
                                // Left edge fade
                                if (leftEdgeFadeWidth > 0) {
                                    for (int x = 0; x < leftEdgeFadeWidth; ++x) { // x is pixel index
                                        float fade = static_cast<float>(x) / (leftEdgeFadeWidth > 1 ? (leftEdgeFadeWidth - 1) : 1);
                                        if (lastSdlPixFormat == SDL_PIXELFORMAT_UYVY) {
                                            int group_idx = x / 2;
                                            int in_group_idx = x % 2;
                                            uint8_t* uyvy_pixel_group = rowStart + group_idx * 4;
                                            // Fade only Y component, leave U/V or set to neutral for strong fade
                                            if (group_idx * 4 + (1 + in_group_idx*2) < pitch) {
                                                uint8_t currentY = uyvy_pixel_group[1 + in_group_idx*2];
                                                uyvy_pixel_group[1 + in_group_idx*2] = static_cast<uint8_t>(currentY * fade + 16.0f * (1.0f - fade));
                                                // Optionally fade U/V to grey:
                                                // uyvy_pixel_group[0] = static_cast<uint8_t>(uyvy_pixel_group[0] * fade + 128.0f * (1.0f - fade));
                                                // uyvy_pixel_group[2] = static_cast<uint8_t>(uyvy_pixel_group[2] * fade + 128.0f * (1.0f - fade));
                                            }
                                        } else {
                                            rowStart[x] = static_cast<uint8_t>(rowStart[x] * fade + 16.0f * (1.0f - fade));
                                        }
                                    }
                                }
                                // Right edge fade
                                if (rightEdgeFadeWidth > 0) {
                                    for (int x = 0; x < rightEdgeFadeWidth; ++x) {
                                        int realX = textureWidth - 1 - x; // pixel index
                                        float fade = static_cast<float>(x) / (rightEdgeFadeWidth > 1 ? (rightEdgeFadeWidth - 1) : 1);
                                        if (lastSdlPixFormat == SDL_PIXELFORMAT_UYVY) {
                                            int group_idx = realX / 2;
                                            int in_group_idx = realX % 2;
                                            uint8_t* uyvy_pixel_group = rowStart + group_idx * 4;
                                            if (group_idx * 4 + (1 + in_group_idx*2) < pitch) {
                                                uint8_t currentY = uyvy_pixel_group[1 + in_group_idx*2];
                                                uyvy_pixel_group[1 + in_group_idx*2] = static_cast<uint8_t>(currentY * fade + 16.0f * (1.0f - fade));
                                            }
                                        } else {
                                            rowStart[realX] = static_cast<uint8_t>(rowStart[realX] * fade + 16.0f * (1.0f - fade));
                                        }
                                    }
                                }
                            }
                       }
                       // --- End Edge Fade ---

                       // Apply film grain dithering for LOW_RES frames
                       if (frameTypeToDisplay == FrameInfo::LOW_RES) {
                           // Use thread-local random for better performance
                           static thread_local std::mt19937 rng(std::random_device{}());
                           std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

                           // Grain intensity (very subtle)
                           const float grainIntensity = 0.035f; // 3% intensity

                           for (int y = 0; y < textureHeight; ++y) {
                               uint8_t* rowStart = dst_data[0] + y * pitch;
                               
                               if (lastSdlPixFormat == SDL_PIXELFORMAT_UYVY) {
                                   // For UYVY, only modify Y values (every other byte starting at index 1)
                                   for (int x = 0; x < textureWidth * 2; x += 2) {
                                       float noise = dist(rng) * grainIntensity;
                                       int yValue = rowStart[x + 1];
                                       yValue = std::min(235, std::max(16, static_cast<int>(yValue + yValue * noise)));
                                       rowStart[x + 1] = static_cast<uint8_t>(yValue);
                                   }
                               } else {
                                   // For planar formats (IYUV/NV12), modify only Y plane
                                   for (int x = 0; x < textureWidth; ++x) {
                                       float noise = dist(rng) * grainIntensity;
                                       int yValue = rowStart[x];
                                       yValue = std::min(235, std::max(16, static_cast<int>(yValue + yValue * noise)));
                                       rowStart[x] = static_cast<uint8_t>(yValue);
                                   }
                               }
                           }
                       }

                       SDL_UnlockTexture(lastTexture);
                  } else {
                      std::cerr << "Error locking texture for SW update/effects: " << SDL_GetError() << std::endl;
                      // Skip rendering this texture if lock failed
                      if (lastTexture) { SDL_DestroyTexture(lastTexture); lastTexture = nullptr; }
                  }

                  // Mark frame as rendered *only if* texture update was successful
                  if (lastTexture) {
                      firstFrameRendered = true;
                  }

             // --- Free transferred frame if created ---
             if (frame_transferred && sw_frame) {
                 av_frame_free(&sw_frame); // Frees both frame and its buffers
             }
             // --- End Free transferred frame ---

             } // end if(lastTexture exists after creation/check)
        } // End if (!renderedWithMetal)

    } else { // No frameToDisplay provided
        // std::cout << "[TimingDebug:" << ms_since_epoch << "] >> No AVFrame provided for frame " << newCurrentFrame << "! Trying redraw." << std::endl; // Optional log
        // Attempt to redraw the last SW texture only if it exists and something was rendered before
        // Do nothing if Metal was used previously or no SW texture exists
    }

     // --- Final Rendering Stage ---
     // Calculate destRect based on current texture dimensions (could be from Metal or SW path)
     // Ensure textureWidth and textureHeight are up-to-date
    if (firstFrameRendered && textureWidth > 0 && textureHeight > 0) {
        // Use the targetDisplayAspectRatio passed from the caller
        float aspectRatio = targetDisplayAspectRatio; 
        // Ensure aspect ratio is positive to avoid division by zero or weird behavior
        if (aspectRatio <= 0) { aspectRatio = 16.0f / 9.0f; } // Fallback

                      if (windowWidth / aspectRatio <= windowHeight) {
                          destRect.w = windowWidth;
                          destRect.h = static_cast<int>(windowWidth / aspectRatio);
                      } else {
                          destRect.h = windowHeight;
                          destRect.w = static_cast<int>(windowHeight * aspectRatio);
                      }
                      destRect.x = (windowWidth - destRect.w) / 2;
                      destRect.y = (windowHeight - destRect.h) / 2;
        
        // --- Calculate hsyncMaxSkewAmount now that destRect.w is known, if effect just started ---
        if (hsyncLossActive && hsyncEffectCurrentFrame == 1) { // First frame of an active effect
             static std::mt19937 gen_skew(std::random_device{}()); // Separate generator for amount
             // Max skew is a percentage of the *destination rect width*
             float baseMaxSkew = destRect.w * (0.02f + (static_cast<float>(gen_skew() % 31) / 1000.0f)); // 2% to 5% of width
             hsyncMaxSkewAmount = baseMaxSkew;
             if (gen_skew() % 2 == 0) hsyncMaxSkewAmount *= -1; // Random direction
        }


        // Apply Jitter Offset just before rendering (conceptually)
        // Jitter calculated earlier within the SW path is used here
        { // Jitter scope
             double absPlaybackRate = std::abs(currentPlaybackRate); // Use current rate again
             static std::random_device rd_jitter_render; // Separate RNG for render stage jitter
             static std::mt19937 rng_jitter_render(rd_jitter_render());

             double jitterAmplitude = 0;
             // --- Jitter calculations for various speeds (0.2x to 1.0x excluded) ---
             if (absPlaybackRate >= 1.3 && absPlaybackRate < 1.9) { double t = (absPlaybackRate - 1.3) / 0.6; jitterAmplitude = 10.0 + t * 9.0; }
             else if (absPlaybackRate >= 1.9 && absPlaybackRate < 4.0) jitterAmplitude = 2.0;
             else if (absPlaybackRate >= 4.0 && absPlaybackRate < 16.0) { double t = (absPlaybackRate - 4.0) / 12.0; jitterAmplitude = 1.4 + t * 2.2; }
             else if (absPlaybackRate >= 20.0) jitterAmplitude = 2.0;

             if (jitterAmplitude > 0) {
                 if (newFrameProcessed) { // Apply jitter ONLY if a new frame was processed
                     if (absPlaybackRate >= 0.20 && absPlaybackRate < 1.0) {
                         int baseOffset = static_cast<int>(floor(jitterAmplitude));
                         int offset = (newCurrentFrame % 2 == 0) ? baseOffset : -baseOffset;
                         destRect.y += offset;
                     }
                     // --- Existing Random Jitter for other speeds ---
                     else {
                         // Sharp random jitter for specific higher speed range
                         if (absPlaybackRate >= 1.3 && absPlaybackRate < 2.0) {
                              std::uniform_real_distribution<> sharpJitterDist(-1.0, 1.0); destRect.y += static_cast<int>(sharpJitterDist(rng_jitter_render) * jitterAmplitude);
                         }
                         // Normal random jitter for other speeds >= 1.0x (and specifically >= 4.0x based on amplitude calc)
                         else { // This covers >=1.0x, excluding 1.3-2.0, but amplitude is only > 0 for >= 1.9, 4.0-16.0, >=20.0
                              // Redundant check since we are inside if(jitterAmplitude > 0), but safe.
                              std::normal_distribution<> normalJitterDist(0.0, 1.0); destRect.y += static_cast<int>(normalJitterDist(rng_jitter_render) * jitterAmplitude);
                         }
                     }
                 }
             }
        } // End jitter scope


        // Render the final texture (either Metal's output implicitly, or our SW texture)
        // In deep pause, renderedWithMetal will be false, so it will try to render lastTexture.
        if (!renderedWithMetal && lastTexture) { // Render SW texture if it exists and wasn't Metal
            // --- HSync Loss Effect Rendering ---
            if (hsyncLossActive && hsyncMaxSkewAmount != 0.0f && destRect.h > 0 && textureHeight > 0) {
                int actualTearLineScreenY = destRect.y + static_cast<int>(tearLineNormalized * destRect.h);
                actualTearLineScreenY = std::max(destRect.y, std::min(actualTearLineScreenY, destRect.y + destRect.h -1)); // -1 to ensure min 1px for below part

                // Part 1: Below the tear (renders normally, unchanged)
                SDL_Rect srcBelow, dstBelow;
                dstBelow.x = destRect.x;
                dstBelow.y = actualTearLineScreenY;
                dstBelow.w = destRect.w;
                dstBelow.h = (destRect.y + destRect.h) - actualTearLineScreenY;

                srcBelow.x = 0;
                srcBelow.y = static_cast<int>((static_cast<float>(dstBelow.y - destRect.y) / destRect.h) * textureHeight);
                srcBelow.w = textureWidth;
                srcBelow.h = static_cast<int>((static_cast<float>(dstBelow.h) / destRect.h) * textureHeight);
                
                if (srcBelow.y + srcBelow.h > textureHeight) { // Adjust srcBelow.h if it exceeds texture bounds
                    srcBelow.h = textureHeight - srcBelow.y;
                }

                if (srcBelow.h > 0 && dstBelow.h > 0) {
                    renderSoftwareTexture(renderer, lastTexture, textureWidth, textureHeight, destRect, &srcBelow, &dstBelow, 0);
                }

                // Part 2: Above the tear (this part is skewed, rendered line by line)
                int topPartScreenHeight = actualTearLineScreenY - destRect.y;
                if (topPartScreenHeight > 0) {
                    for (int y_screen = destRect.y; y_screen < actualTearLineScreenY; ++y_screen) {
                        SDL_Rect srcLine, dstLine;
                        
                        // Normalize y_screen within the skewed region (0 at top of tear, 1 at top of screen)
                        // Inverse: 0 at top of screen, 1 at tear line
                        float normalizedYInSkewArea = static_cast<float>(actualTearLineScreenY - 1 - y_screen) / std::max(1, topPartScreenHeight -1) ;
                        
                        // Skew for this specific line (max skew at the very top, 0 at tear line)
                        // hsyncMaxSkewAmount is already animated (0->peak->0)
                        float currentLineSkew = hsyncMaxSkewAmount * normalizedYInSkewArea;

                        dstLine.x = destRect.x + static_cast<int>(currentLineSkew);
                        dstLine.y = y_screen;
                        dstLine.w = destRect.w; // We'll clip this via srcLine.w adjustment if needed
                        dstLine.h = 1;

                        srcLine.x = 0; // Default source X
                        // Calculate corresponding Y in source texture
                        srcLine.y = static_cast<int>((static_cast<float>(y_screen - destRect.y) / destRect.h) * textureHeight);
                        srcLine.w = textureWidth; // Default source width
                        srcLine.h = 1;
                        
                        // Adjust srcLine.x and srcLine.w based on how much dstLine.x is shifted
                        // and how much of dstLine.w would be outside destRect.x / destRect.x + destRect.w
                        // This is a simplified clipping, more advanced would be needed for perfect results
                        if (dstLine.x < destRect.x) {
                            int offset = destRect.x - dstLine.x;
                            srcLine.x += static_cast<int>((float)offset / dstLine.w * srcLine.w) ; 
                            srcLine.w -= static_cast<int>((float)offset / dstLine.w * srcLine.w) ; 
                            dstLine.w -= offset;
                            dstLine.x = destRect.x;
                        }
                        if (dstLine.x + dstLine.w > destRect.x + destRect.w) {
                            int overflow = (dstLine.x + dstLine.w) - (destRect.x + destRect.w);
                            srcLine.w -= static_cast<int>((float)overflow / dstLine.w * srcLine.w) ; 
                            dstLine.w -= overflow;
                        }

                        if (srcLine.y >= 0 && srcLine.y < textureHeight && srcLine.w > 0 && dstLine.w > 0) {
                             SDL_RenderCopy(renderer, lastTexture, &srcLine, &dstLine);
                        }
                    }
                }
            } else {
                 renderSoftwareTexture(renderer, lastTexture, textureWidth, textureHeight, destRect, nullptr, &destRect, 0); // Pass 0 for horizontalShift when no effect
            }
            // --- End HSync Loss Effect Rendering ---
        } else if (!renderedWithMetal && !lastTexture && firstFrameRendered) {
            // This case should ideally not happen if logic is correct,
            // but redraw the last known good texture if needed (maybe from a previous Metal frame?)
            // Need a way to cache/access the *result* of Metal rendering if we want to redraw it.
            // For now, we might just show black if the SW texture is gone.
             SDL_SetRenderDrawColor(renderer, 10, 0, 10, 255); SDL_RenderClear(renderer); // Purple to indicate issue
        } else if (renderedWithMetal) {
             // Metal already rendered to the screen implicitly or via metalRenderer.render()
             // But render the zoom thumbnail if needed, using the Metal frame dimensions
                  if (zoom_enabled.load() && show_zoom_thumbnail.load()) {
                 // We need a way to get an SDL_Texture representation of the Metal frame
                 // or render the thumbnail using Metal directly. This is complex.
                 // Placeholder: Render a box indicating thumbnail area.
                 int thumbW = std::min(300, static_cast<int>(windowWidth * 0.2f));
                 int thumbH = static_cast<int>(thumbW / (static_cast<float>(textureWidth) / textureHeight));
                 SDL_Rect thumbRect = {windowWidth - thumbW - 10, 10, thumbW, thumbH};
                 SDL_SetRenderDrawColor(renderer, 255, 255, 0, 100); // Yellow placeholder
                 SDL_RenderDrawRect(renderer, &thumbRect);
             }
        }
    } else if (!firstFrameRendered) { // Only show black if not first frame
         // Nothing rendered yet, ensure screen is black
         // SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255); SDL_RenderClear(renderer); // Already cleared at the beginning
    }


    // Render frame index visualization (if enabled)
    if (showIndex) {
        // Ensure frameIndex is not empty before accessing size()
        if (!frameIndex.empty()) {
        int bufferStart = std::max(0, newCurrentFrame - static_cast<int>(ringBufferCapacity / 2));
        int bufferEnd = std::min(static_cast<int>(frameIndex.size()) - 1, bufferStart + static_cast<int>(ringBufferCapacity) - 1);
        int highResStart = std::max(0, newCurrentFrame - highResWindowSize / 2);
        int highResEnd = std::min(static_cast<int>(frameIndex.size()) - 1, newCurrentFrame + highResWindowSize / 2);
        updateVisualization(renderer, frameIndex, newCurrentFrame, bufferStart, bufferEnd, highResStart, highResEnd, enableHighResDecode);
        }
    }

    // Render OSD (if enabled)
    if (showOSD && font) {
        renderOSD(renderer, font, isPlaying.load(), currentPlaybackRate, isReverse, currentTime, newCurrentFrame, showOSD, waitingForTimecode, inputTimecode, originalFps, jog_forward.load(), jog_backward.load(), frameTypeToDisplay);
    }

    // Update the screen
    SDL_RenderPresent(renderer);
}

// Function to clean up resources when program exits
void cleanupDisplayResources() {
    // Corrected SwsContext cleanup
#ifdef __APPLE__
    metalRenderer.cleanup();
#endif
    if (lastTexture) {
        SDL_DestroyTexture(lastTexture);
        lastTexture = nullptr;
    }
    // Clean up global screen texture
    if (screenTexture) {
        SDL_DestroyTexture(screenTexture);
        screenTexture = nullptr;
        screenTextureInitialized = false;
    }
    // Access the static SwsContext declared within displayFrame's scope (or make it global/member if needed)
    // No explicit cleanup needed here anymore for SwsContext,
    // static unique_ptr in displayFrame will handle it at program exit.
}

// Function to render the loading screen
void renderLoadingScreen(SDL_Renderer* renderer, TTF_Font* font, const LoadingStatus& status) {
    static Uint32 lastBlinkTime = 0;
    static bool showDashes = true;

    // Clear the screen with black background
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    
    int windowWidth, windowHeight;
    SDL_GetRendererOutputSize(renderer, &windowWidth, &windowHeight);
    
    // Create semi-transparent black background for OSD
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 150);
    SDL_Rect osdRect = {0, windowHeight - 30, windowWidth, 30};
    SDL_RenderFillRect(renderer, &osdRect);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

    // Prepare OSD text based on LoadingStatus
    SDL_Color textColor = {255, 255, 255, 255};
    
    // Left Text (Fixed during loading)
    std::string leftText = "THREADING"; // More accurate VCR term for loading
    SDL_Surface* leftSurface = TTF_RenderText_Blended(font, leftText.c_str(), textColor);
    if (leftSurface) {
        SDL_Texture* leftTexture = SDL_CreateTextureFromSurface(renderer, leftSurface);
        if (leftTexture) {
            SDL_Rect leftRect = {10, windowHeight - 30 + (30 - leftSurface->h) / 2, leftSurface->w, leftSurface->h};
            SDL_RenderCopy(renderer, leftTexture, NULL, &leftRect);
            SDL_DestroyTexture(leftTexture);
        }
        SDL_FreeSurface(leftSurface);
    }

    // Center Text (Stage) -> Blinking Timecode Placeholder
    // Update blink state
    Uint32 currentTicks = SDL_GetTicks();
    if (currentTicks - lastBlinkTime > 500) { // Blink every 500ms
        showDashes = !showDashes;
        lastBlinkTime = currentTicks;
    }
    std::string timecodeText = showDashes ? "--:--:--:--" : "  :  :  :  ";
    SDL_Surface* timecodeSurface = TTF_RenderText_Blended(font, timecodeText.c_str(), textColor);
    if (timecodeSurface) {
        SDL_Texture* timecodeTexture = SDL_CreateTextureFromSurface(renderer, timecodeSurface);
        if (timecodeTexture) {
            SDL_Rect timecodeRect = {(windowWidth - timecodeSurface->w) / 2, windowHeight - 30 + (30 - timecodeSurface->h) / 2, timecodeSurface->w, timecodeSurface->h};
            SDL_RenderCopy(renderer, timecodeTexture, NULL, &timecodeRect);
            SDL_DestroyTexture(timecodeTexture);
        }
        SDL_FreeSurface(timecodeSurface);
    }

    // Right Text (Progress Percentage) -> Show as 3-digit number
    int progressPercent = status.percent.load();
    char progressBuffer[4]; // 3 digits + null terminator
    snprintf(progressBuffer, sizeof(progressBuffer), "%03d", progressPercent);
    std::string rightText = progressBuffer;
    SDL_Surface* rightSurface = TTF_RenderText_Blended(font, rightText.c_str(), textColor);
    if (rightSurface) {
        SDL_Texture* rightTexture = SDL_CreateTextureFromSurface(renderer, rightSurface);
        if (rightTexture) {
            SDL_Rect rightRect = {windowWidth - rightSurface->w - 10, windowHeight - 30 + (30 - rightSurface->h) / 2, rightSurface->w, rightSurface->h};
            SDL_RenderCopy(renderer, rightTexture, NULL, &rightRect);
            SDL_DestroyTexture(rightTexture);
        }
        SDL_FreeSurface(rightSurface);
    }
    
    // Present the renderer
    SDL_RenderPresent(renderer);
}

// Implementation of zoom functions (Jitter removed)
void renderZoomedFrame(SDL_Renderer* renderer, SDL_Texture* texture, int frameWidth, int frameHeight, float zoomFactor, float centerX, float centerY) {
    int windowWidth, windowHeight;
    SDL_GetRendererOutputSize(renderer, &windowWidth, &windowHeight);
    float aspectRatio = static_cast<float>(frameWidth) / frameHeight;
    SDL_Rect destRect; // Keep destRect calculation for positioning
    if (windowWidth / aspectRatio <= windowHeight) {
        destRect.w = windowWidth;
        destRect.h = static_cast<int>(windowWidth / aspectRatio);
    } else {
        destRect.h = windowHeight;
        destRect.w = static_cast<int>(windowHeight * aspectRatio);
    }
    destRect.x = (windowWidth - destRect.w) / 2;
    destRect.y = (windowHeight - destRect.h) / 2;
    // Jitter logic removed
    
    // Calculate zoomed area dimensions and position
    SDL_Rect srcRect;
    srcRect.w = static_cast<int>(frameWidth / zoomFactor);
    srcRect.h = static_cast<int>(frameHeight / zoomFactor);
    srcRect.x = static_cast<int>(centerX * frameWidth - srcRect.w / 2);
    srcRect.y = static_cast<int>(centerY * frameHeight - srcRect.h / 2);
    
    // Constrain area to frame boundaries
    if (srcRect.x < 0) srcRect.x = 0;
    if (srcRect.y < 0) srcRect.y = 0;
    if (srcRect.x + srcRect.w > frameWidth) srcRect.x = frameWidth - srcRect.w;
    if (srcRect.y + srcRect.h > frameHeight) srcRect.y = frameHeight - srcRect.h;
    
    // Render zoomed area to full screen
    SDL_RenderCopy(renderer, texture, &srcRect, &destRect);
}


void renderZoomThumbnail(SDL_Renderer* renderer, SDL_Texture* texture, int frameWidth, int frameHeight, float zoomFactor, float centerX, float centerY) {
    int windowWidth, windowHeight;
    SDL_GetRendererOutputSize(renderer, &windowWidth, &windowHeight);
    
    // Thumbnail size
    int thumbnailWidth = std::min(300, static_cast<int>(windowWidth * 0.2f));
    int thumbnailHeight = static_cast<int>(thumbnailWidth / (static_cast<float>(frameWidth) / frameHeight));
    
    // Thumbnail position
    int padding = 10;
    SDL_Rect thumbnailRect = {windowWidth - thumbnailWidth - padding, padding, thumbnailWidth, thumbnailHeight};
    
    // Render full frame thumbnail
    SDL_RenderCopy(renderer, texture, NULL, &thumbnailRect);
    
    // Draw border
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    SDL_RenderDrawRect(renderer, &thumbnailRect);
    
    // Draw rectangle showing zoomed area
    SDL_Rect zoomRect;
    zoomRect.w = static_cast<int>(thumbnailWidth / zoomFactor);
    zoomRect.h = static_cast<int>(thumbnailHeight / zoomFactor);
    zoomRect.x = thumbnailRect.x + static_cast<int>(centerX * thumbnailWidth - zoomRect.w / 2);
    zoomRect.y = thumbnailRect.y + static_cast<int>(centerY * thumbnailHeight - zoomRect.h / 2);
    
    // Constrain area
    if (zoomRect.x < thumbnailRect.x) zoomRect.x = thumbnailRect.x;
    if (zoomRect.y < thumbnailRect.y) zoomRect.y = thumbnailRect.y;
    if (zoomRect.x + zoomRect.w > thumbnailRect.x + thumbnailRect.w) zoomRect.x = thumbnailRect.x + thumbnailRect.w - zoomRect.w;
    if (zoomRect.y + zoomRect.h > thumbnailRect.y + thumbnailRect.h) zoomRect.y = thumbnailRect.y + thumbnailRect.h - zoomRect.h;

    // Draw rectangle
    SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255);
    SDL_RenderDrawRect(renderer, &zoomRect);
}

void handleZoomMouseEvent(SDL_Event& event, int windowWidth, int windowHeight, int frameWidth, int frameHeight) {
    static int lastMouseX = 0;
    static int lastMouseY = 0;
    
    // Calculate video dimensions maintaining aspect ratio
    SDL_Rect videoRect;
    // Ensure frameHeight is not zero before division
    if (frameHeight == 0) return;
    float aspectRatio = static_cast<float>(frameWidth) / frameHeight;

    if (windowWidth / aspectRatio <= windowHeight) {
        videoRect.w = windowWidth;
        videoRect.h = static_cast<int>(windowWidth / aspectRatio);
    } else {
        videoRect.h = windowHeight;
        videoRect.w = static_cast<int>(windowHeight * aspectRatio);
    }
    videoRect.x = (windowWidth - videoRect.w) / 2;
    videoRect.y = (windowHeight - videoRect.h) / 2;
    
    if (event.type == SDL_MOUSEMOTION) {
        lastMouseX = event.motion.x;
        lastMouseY = event.motion.y;
        
        if (zoom_enabled.load() && videoRect.w > 0 && videoRect.h > 0 && // Ensure rect has dimensions
            lastMouseX >= videoRect.x && lastMouseX < videoRect.x + videoRect.w &&
            lastMouseY >= videoRect.y && lastMouseY < videoRect.y + videoRect.h) {
            float normalizedX = static_cast<float>(lastMouseX - videoRect.x) / videoRect.w;
            float normalizedY = static_cast<float>(lastMouseY - videoRect.y) / videoRect.h;
            set_zoom_center(normalizedX, normalizedY);
        }
    } else if (event.type == SDL_MOUSEWHEEL) {
         if (videoRect.w > 0 && videoRect.h > 0 && // Check rect again
             lastMouseX >= videoRect.x && lastMouseX < videoRect.x + videoRect.w &&
            lastMouseY >= videoRect.y && lastMouseY < videoRect.y + videoRect.h) {
            if (event.wheel.y > 0) increase_zoom();
            else if (event.wheel.y < 0) decrease_zoom();
        }
    } else if (event.type == SDL_MOUSEBUTTONDOWN) {
        if (event.button.button == SDL_BUTTON_RIGHT) reset_zoom();
        else if (event.button.button == SDL_BUTTON_MIDDLE) toggle_zoom_thumbnail();
    }
}

// Helper function implementation
static AVPixelFormat av_pix_fmt_from_sdl_format(SDL_PixelFormatEnum sdlFormat) {
    switch (sdlFormat) {
        case SDL_PIXELFORMAT_IYUV: return AV_PIX_FMT_YUV420P;
        case SDL_PIXELFORMAT_NV12: return AV_PIX_FMT_NV12;
        case SDL_PIXELFORMAT_UYVY: return AV_PIX_FMT_UYVY422;
        case SDL_PIXELFORMAT_YUY2: return AV_PIX_FMT_YUYV422; // Mapping for YUY2
        case SDL_PIXELFORMAT_RGB24: return AV_PIX_FMT_RGB24;
        case SDL_PIXELFORMAT_BGR24: return AV_PIX_FMT_BGR24;
        default: return AV_PIX_FMT_NONE;
    }
}

// --- Implementations for Getters --- 
int get_last_texture_width() {
    return textureWidth; // Return the static variable
}

int get_last_texture_height() {
    return textureHeight; // Return the static variable
}