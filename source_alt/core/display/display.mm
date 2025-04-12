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

// Forward declarations for zoom functions
void renderZoomedFrame(SDL_Renderer* renderer, SDL_Texture* texture, int frameWidth, int frameHeight, float zoomFactor, float centerX, float centerY);
void renderZoomThumbnail(SDL_Renderer* renderer, SDL_Texture* texture, int frameWidth, int frameHeight, float zoomFactor, float centerX, float centerY);
void handleZoomMouseEvent(SDL_Event& event, int windowWidth, int windowHeight, int frameWidth, int frameHeight);

// Helper function to render the cached software texture
static void renderSoftwareTexture(SDL_Renderer* renderer, SDL_Texture* texture, int texWidth, int texHeight, const SDL_Rect& destRect) { // Pass destRect
    if (!texture || texWidth <= 0 || texHeight <= 0) return; // Safety check

    if (zoom_enabled.load()) {
        renderZoomedFrame(renderer, texture, texWidth, texHeight, zoom_factor.load(), zoom_center_x.load(), zoom_center_y.load());
    } else {
        // Use the pre-calculated destRect which includes jitter offset
        SDL_RenderCopy(renderer, texture, nullptr, &destRect);
    }
    if (zoom_enabled.load() && show_zoom_thumbnail.load()) {
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

void renderOSD(SDL_Renderer* renderer, TTF_Font* font, bool isPlaying, double playbackRate, bool isReverse, double currentTime, int frameNumber, bool showOSD, bool waiting_for_timecode, const std::string& input_timecode, double original_fps, bool jog_forward, bool jog_backward, bool isLoading, const std::string& loadingType, int loadingProgress) {
    int windowWidth, windowHeight;
    SDL_GetRendererOutputSize(renderer, &windowWidth, &windowHeight);
    SDL_Color textColor = {255, 255, 255, 255};

    // Draw semi-transparent background for OSD area
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND); // Enable blending
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 150); // Black with alpha ~60%
    SDL_Rect osdBackgroundRect = {0, windowHeight - 30, windowWidth, 30};
    SDL_RenderFillRect(renderer, &osdBackgroundRect);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE); // Disable blending for text

    // Left Text
    std::string leftText;
    if (isLoading) leftText = "UNTHREAD";
    else if (jog_forward || jog_backward) leftText = "JOG";
    else if (std::abs(playbackRate) < 0.01) leftText = "STILL";
    else if (std::abs(playbackRate) > 1.0) leftText = "SHUTTLE";
    else leftText = "PLAY";

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
    std::string timecode;
    if (isLoading) {
        static Uint32 lastBlinkTime = 0;
        static bool showDashes = true;
         Uint32 currentTicks = SDL_GetTicks();
         if (currentTicks - lastBlinkTime > 500) { showDashes = !showDashes; lastBlinkTime = currentTicks; }
        timecode = showDashes ? "--:--:--:--" : "  :  :  :  ";
    } else if (waiting_for_timecode) {
        timecode = "00:00:00:00"; // Start with placeholder
        std::string formatted_input = input_timecode;
        formatted_input.resize(8, '0'); // Pad with zeros if needed
        size_t tc_idx = 0;
        for(size_t i = 0; i < formatted_input.length() && tc_idx < timecode.length(); ++i) {
            timecode[tc_idx++] = formatted_input[i];
            if (i == 1 || i == 3 || i == 5) { // After HH, MM, SS
                tc_idx++; // Skip colon
            }
        }
    } else {
        int hours = static_cast<int>(currentTime / 3600);
        int minutes = static_cast<int>(fmod(currentTime, 3600) / 60);
        int seconds = static_cast<int>(fmod(currentTime, 60));
        int frames = static_cast<int>(fmod(currentTime, 1.0) * original_fps);
        char buffer[12];
        snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d:%02d", hours, minutes, seconds, frames);
        timecode = buffer;
    }
    
    SDL_Surface* timecodeSurface = TTF_RenderText_Blended(font, timecode.c_str(), textColor);
    if (timecodeSurface) {
        SDL_Texture* timecodeTexture = SDL_CreateTextureFromSurface(renderer, timecodeSurface);
        if (timecodeTexture) {
            SDL_Rect timecodeRect = {(windowWidth - timecodeSurface->w) / 2, windowHeight - 30 + (30 - timecodeSurface->h) / 2, timecodeSurface->w, timecodeSurface->h};
            SDL_RenderCopy(renderer, timecodeTexture, NULL, &timecodeRect);
            SDL_DestroyTexture(timecodeTexture);
        }
        SDL_FreeSurface(timecodeSurface);
    }

    // Right Text
    std::string rightText;
    if (isLoading) {
        std::string prefix = (loadingType == "youtube") ? "DL" : (loadingType == "file" ? "LD" : (loadingType == "convert" ? "LS" : "LD"));
        char progressBuffer[5];
        snprintf(progressBuffer, sizeof(progressBuffer), "%03d", loadingProgress);
        rightText = prefix + std::string(progressBuffer);
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
    int segmentSize
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
                           // Direct copy if formats match (e.g., YUV420P -> IYUV)
                           // This assumes sws_scale is bypassed only if src AVFmt == target AVFmt
                           // We still need to copy data to the texture planes
                           if (currentSdlFormat == SDL_PIXELFORMAT_IYUV && currentSrcFormat == AV_PIX_FMT_YUV420P) {
                               memcpy(dst_data[0], frame->data[0], frame->linesize[0] * frame->height); // Y
                               memcpy(dst_data[1], frame->data[1], frame->linesize[1] * frame->height / 2); // U
                               memcpy(dst_data[2], frame->data[2], frame->linesize[2] * frame->height / 2); // V
                           } else if (currentSdlFormat == SDL_PIXELFORMAT_UYVY && currentSrcFormat == AV_PIX_FMT_UYVY422) {
                                memcpy(dst_data[0], frame->data[0], frame->linesize[0] * frame->height); // UYVY
                           } // Add other direct copy cases if needed
                       }

                       // --- Apply Betacam Effects ---
                       double absPlaybackRate = std::abs(currentPlaybackRate);
                       const double effectThreshold = 1.1; // Slightly above 1x to trigger effects
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
                           const int minStripeHeight = static_cast<int>(14 * resolutionScale);
                           const int midStripeHeight = static_cast<int>(50 * resolutionScale);
                           int stripeHeight, stripeSpacing;
                           // ... (rest of stripe parameter calculation based on absPlaybackRate, identical to old code) ...
                            if ((absPlaybackRate >= 0.2 && absPlaybackRate < 0.9) || (absPlaybackRate >= 1.2 && absPlaybackRate < 2.0)) {
                                double t = (absPlaybackRate < 0.9) ? (absPlaybackRate - 0.2) / 0.7 : (absPlaybackRate - 1.2) / 0.8;
                                t = t * t * (3 - 2 * t); stripeHeight = static_cast<int>(maxStripeHeight * (1 - t) + midStripeHeight * t); stripeSpacing = baseStripeSpacing;
                            } else if (absPlaybackRate >= 2.0 && absPlaybackRate < 4.0) {
                                stripeHeight = baseStripeHeight; stripeSpacing = baseStripeSpacing;
                            } else if (absPlaybackRate >= 3.5 && absPlaybackRate < 14.0) {
                                double t = (absPlaybackRate - 4.0) / 10.0; t = std::pow(t, 0.7);
                                stripeHeight = static_cast<int>(baseStripeHeight * (1.0 - t) + minStripeHeight * t); stripeSpacing = static_cast<int>(baseStripeSpacing * (1.0 - t) + minStripeSpacing * t);
                            } else { stripeHeight = minStripeHeight; stripeSpacing = minStripeSpacing; }


                           // Calculate cycle progress for animation
                           double cycleProgress;
                           const double baseDuration = 1.5;
                           const int fps = static_cast<int>(originalFps > 0 ? originalFps : 30); // Use actual FPS if available
                           // ... (rest of cycleProgress calculation based on absPlaybackRate, identical to old code) ...
                            if (absPlaybackRate >= 14.0) {
                                double speedFactor = std::pow(absPlaybackRate/14.0, 1.2) * 4.02; if (absPlaybackRate >= 16.0) speedFactor *= 1.0 + (absPlaybackRate - 16.0) * 0.417;
                                double highSpeedCycleDuration = 1.0 / (fps * speedFactor); cycleProgress = std::fmod(currentTime, highSpeedCycleDuration) / highSpeedCycleDuration;
                            } else if (absPlaybackRate >= 12.0) {
                                double t = (absPlaybackRate - 12.0) / 2.0; double speedMultiplier = 0.4 + t * 0.6; double transitionDuration = baseDuration / (absPlaybackRate * speedMultiplier); cycleProgress = std::fmod(currentTime, transitionDuration) / transitionDuration;
                            } else if (absPlaybackRate >= 3.5) {
                                double normalizedSpeed = (absPlaybackRate - 3.5) / 8.5; double speedMultiplier = 0.08 + (std::pow(normalizedSpeed, 3) * 0.15); if (absPlaybackRate < 8.0) speedMultiplier *= 0.7;
                                double mediumSpeedDuration = baseDuration / (absPlaybackRate * speedMultiplier); cycleProgress = std::fmod(currentTime, mediumSpeedDuration) / mediumSpeedDuration;
                            } else { double speedFactor = std::min(absPlaybackRate / 2.0, 2.0); double adjustedDuration = baseDuration / speedFactor; cycleProgress = std::fmod(currentTime, adjustedDuration) / adjustedDuration; }


                           // Apply B&W zones under stripes (2x-10x)
                           if (absPlaybackRate >= 2.0 && absPlaybackRate < 10.0) {
                               int stripeCount_bw = (textureHeight + stripeSpacing) / stripeSpacing;
                               for (int i = 0; i < stripeCount_bw; ++i) {
                                   // ... (calculations for bwZoneHeight, y_bw, bwStartY, bwEndY identical to old code) ...
                                    double baseOffset = cycleProgress * stripeSpacing + i * stripeSpacing; double heightVariation = (absPlaybackRate >= 14.0) ? 0 : ((rand() % 21 - 10) * resolutionScale);
                                    int currentStripeHeight = std::max(static_cast<int>(stripeHeight + heightVariation), minStripeHeight); int bwZoneHeight = static_cast<int>(currentStripeHeight * 1.75);
                                    int heightDifference = bwZoneHeight - currentStripeHeight; int y_stripe = static_cast<int>(std::fmod(baseOffset, textureHeight + stripeSpacing)) - currentStripeHeight;
                                    int y_bw = y_stripe - heightDifference / 2;

                                    if (y_bw < textureHeight && y_bw + bwZoneHeight > 0) { // Check overlap
                                        int bwStartY = std::max(0, y_bw); int bwEndY = std::min(textureHeight, y_bw + bwZoneHeight);
                                        for (int yPos = bwStartY; yPos < bwEndY; ++yPos) {
                                            uint8_t* yPlane = dst_data[0] + yPos * pitch;
                                            for (int x = 0; x < textureWidth; ++x) { yPlane[x] = static_cast<uint8_t>(yPlane[x] * 0.85); } // Darken Y
                                            if (lastSdlPixFormat == SDL_PIXELFORMAT_IYUV) {
                                                int uvY = yPos / 2; if (uvY < textureHeight / 2) { memset(dst_data[1] + uvY * dst_linesize[1], 128, textureWidth / 2); memset(dst_data[2] + uvY * dst_linesize[2], 128, textureWidth / 2); }
                                            } else if (lastSdlPixFormat == SDL_PIXELFORMAT_NV12) {
                                                int uvY = yPos / 2; if (uvY < textureHeight / 2) { uint8_t* uvPlane = dst_data[1] + uvY * dst_linesize[1]; for (int x = 0; x < textureWidth / 2; ++x) { uvPlane[x*2]=128; uvPlane[x*2+1]=128; } }
                                            }
                                        }
                                    }
                               }
                           }

                           // Draw grey stripes
                           int stripeCount = (textureHeight + stripeSpacing) / stripeSpacing;
                           for (int i = 0; i < stripeCount; ++i) {
                               double baseOffset = cycleProgress * stripeSpacing + i * stripeSpacing;
                               // Debug: Print index and base stripe height
                               // if (i < 5) std::cout << "Stripe[" << i << "] baseHeight: " << stripeHeight; // COMMENTED OUT
 
                               // Apply random height variation per stripe (+/- 12 pixels before scaling)
                               // double heightVariation = ((rand() % 11 - 5) * resolutionScale); // Old +/- 5 code
                               // --- DEBUG: Use much larger variation to test visibility ---
                               double heightVariation = ((rand() % 25 - 12) * resolutionScale); // Final range +/- 12
                               int currentStripeHeight = std::max(static_cast<int>(stripeHeight + heightVariation), minStripeHeight);
                               int y = static_cast<int>(std::fmod(baseOffset, textureHeight + stripeSpacing)) - currentStripeHeight;
 
                               // Debug: Print variation and final height
                               // if (i < 5) { // Log only first 5 stripes to avoid spam
                               //     std::cout << "  Stripe[" << i << "] variation: " << heightVariation << ", finalHeight: " << currentStripeHeight << std::endl;
                               // }

                               if (y < textureHeight && y + currentStripeHeight > 0) { // Check overlap
                                   int startY = std::max(0, y);
                                   int endY = std::min(textureHeight, y + currentStripeHeight);
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

                                   // Add black outline below the stripe
                                   if (absPlaybackRate >= 8.0) {
                                       // Calculate intensity based on speed (8x to 14x)
                                       const double outlineStartSpeed = 8.0;
                                       const double outlineFullSpeed = 14.0; // Range for intensity
                                       double t = std::max(0.0, std::min(1.0, (absPlaybackRate - outlineStartSpeed) / (outlineFullSpeed - outlineStartSpeed)));

                                       // Calculate target Y value (128 -> 16)
                                       uint8_t targetY = 16 + static_cast<uint8_t>((128 - 16) * (1.0 - t));

                                       // Set outline height to fixed 1 pixel when active
                                       int currentOutlineHeight = 1; 

                                       // Draw the outline rows
                                       for (int h = 0; h < currentOutlineHeight; ++h) {
                                           int outlineY = endY + h;
                                           if (outlineY < textureHeight) { // Check bounds for each row
                                               memset(dst_data[0] + outlineY * pitch, targetY, textureWidth);
                                               // Ensure UV is neutral for the outline rows
                                               if (lastSdlPixFormat == SDL_PIXELFORMAT_IYUV) {
                                                   int uvY = outlineY / 2;
                                                   if (uvY < textureHeight / 2) {
                                                       if (dst_linesize[1] > 0) memset(dst_data[1] + uvY * dst_linesize[1], 128, textureWidth / 2);
                                                       if (dst_linesize[2] > 0) memset(dst_data[2] + uvY * dst_linesize[2], 128, textureWidth / 2);
                                                   }
                                               } else if (lastSdlPixFormat == SDL_PIXELFORMAT_NV12) {
                                                   int uvY = outlineY / 2;
                                                   if (uvY < textureHeight / 2) {
                                                       uint8_t* uvPlane = dst_data[1] + uvY * dst_linesize[1];
                                                       for (int x_uv = 0; x_uv < textureWidth / 2; ++x_uv) { uvPlane[x_uv*2]=128; uvPlane[x_uv*2+1]=128; }
                                                   }
                                               }
                                           }
                                       }
                                   }

                                   // Add snow effect (>= 4x)
                                   if (absPlaybackRate >= 4.0 && startY < textureHeight) { // Ensure snowY is valid
                                       int snowY = startY; // Snow on the first line of the stripe
                                       int snowCount = std::max(8, textureWidth / 80); if (absPlaybackRate > 10.0) snowCount = static_cast<int>(snowCount * 1.5);
                                       for (int j = 0; j < snowCount; ++j) {
                                           int snowX = rand() % textureWidth;
                                           double speedFactor_snow = std::sqrt(absPlaybackRate); int tailBase = 10 + static_cast<int>(rand() % 20); int tailLength = tailBase + static_cast<int>(speedFactor_snow * 5.0);
                                           // Use pitch * textureHeight as rough upper bound check, needs refinement if possible
                                           size_t max_offset = static_cast<size_t>(pitch) * textureHeight;
                                           if (static_cast<size_t>(snowY * pitch + snowX) < max_offset) dst_data[0][snowY * pitch + snowX] = 235; 
                                           for (int k = 1; k < tailLength; k++) {
                                               int xPos = (snowX + k); 
                                               if (xPos >= textureWidth) continue; // Skip pixels beyond texture width
                                               double fadeFactor = exp(-0.15 * k); int brightness = 128 + static_cast<int>(107 * fadeFactor);
                                               if (static_cast<size_t>(snowY * pitch + xPos) < max_offset) dst_data[0][snowY * pitch + xPos] = brightness; 
                                           }
                                       }
                                   }
                               }
                           }

                           // Scanline duplication effect (>= 16x)
                            if (absPlaybackRate >= 16.0) {
                                double effectIntensity = std::min(1.0, (absPlaybackRate - 16.0) / 6.0);
                                std::vector<bool> stripeMask(textureHeight, false);
                                // Re-calculate stripeMask based on current parameters
                                int stripeCount_dup = (textureHeight + stripeSpacing) / stripeSpacing;
                                for (int i = 0; i < stripeCount_dup; ++i) {
                                     // ... (calculations for currentStripeHeight_dup, y_bw/y, bwStartY/startY, bwEndY/endY identical to old code) ...
                                    double baseOffset = cycleProgress * stripeSpacing + i * stripeSpacing; double heightVariation = (absPlaybackRate >= 14.0) ? 0 : ((rand() % 21 - 10) * resolutionScale);
                                    int currentStripeHeight_dup = std::max(static_cast<int>(stripeHeight + heightVariation), minStripeHeight);
                                    if (absPlaybackRate >= 2.0 && absPlaybackRate < 10.0) {
                                        int bwZoneHeight = static_cast<int>(currentStripeHeight_dup * 1.75); int heightDifference = bwZoneHeight - currentStripeHeight_dup;
                                        int y_stripe = static_cast<int>(std::fmod(baseOffset, textureHeight + stripeSpacing)) - currentStripeHeight_dup; int y_bw = y_stripe - heightDifference / 2;
                                        if (y_bw < textureHeight && y_bw + bwZoneHeight > 0) { int bwStartY = std::max(0, y_bw); int bwEndY = std::min(textureHeight, y_bw + bwZoneHeight); for (int yy = bwStartY; yy < bwEndY; ++yy) stripeMask[yy] = true; }
                      } else {
                                        int y = static_cast<int>(std::fmod(baseOffset, textureHeight + stripeSpacing)) - currentStripeHeight_dup;
                                        if (y < textureHeight && y + currentStripeHeight_dup > 0) { int startY = std::max(0, y); int endY = std::min(textureHeight, y + currentStripeHeight_dup); for (int yy = startY; yy < endY; ++yy) stripeMask[yy] = true; }
                                    }
                                }

                                bool inClearArea = false; int clearStart = 0;
                                for (int y = 0; y < textureHeight; ++y) {
                                    if (!stripeMask[y] && !inClearArea) { clearStart = y; inClearArea = true; }
                                    else if ((stripeMask[y] || y == textureHeight - 1) && inClearArea) {
                                        int clearEnd = stripeMask[y] ? y : y + 1;
                                        if (clearEnd - clearStart > 4) { // Area has enough height
                                            int areaStart = clearStart; int areaEnd = clearEnd;
                                            int skipLines = (effectIntensity > 0.7) ? 1 : ((effectIntensity > 0.4) ? 2 : 3);
                                            int baseDuplication = static_cast<int>(1 + 4 * effectIntensity);
                                            for (int lineY = areaStart; lineY < areaEnd; lineY += skipLines) { // Iterate through source lines
                                                if (rand() % 100 < effectIntensity * 100) { // Probabilistic duplication
                                                     int duplication = std::min(areaEnd - lineY - 1, baseDuplication); // How many lines to overwrite
                                                     for (int dup = 1; dup <= duplication; ++dup) {
                                                         if (lineY + dup >= areaEnd) break;
                                                         // Copy Y plane (scanline)
                                                         memcpy(dst_data[0] + (lineY + dup) * pitch, dst_data[0] + lineY * pitch, textureWidth);
                                                         // Copy UV planes (handling subsampling)
                                                         if ((lineY/2 != (lineY+dup)/2) && ((lineY+dup)/2 < textureHeight/2)) { // Only copy UV if the destination line is different in the UV plane
                                                            if (lastSdlPixFormat == SDL_PIXELFORMAT_IYUV) {
                                                                if (dst_linesize[1] > 0) memcpy(dst_data[1] + (lineY+dup)/2 * dst_linesize[1], dst_data[1] + lineY/2 * dst_linesize[1], textureWidth/2);
                                                                if (dst_linesize[2] > 0) memcpy(dst_data[2] + (lineY+dup)/2 * dst_linesize[2], dst_data[2] + lineY/2 * dst_linesize[2], textureWidth/2);
                                                             } else if (lastSdlPixFormat == SDL_PIXELFORMAT_NV12) {
                                                                if (dst_linesize[1] > 0) memcpy(dst_data[1] + (lineY+dup)/2 * dst_linesize[1], dst_data[1] + lineY/2 * dst_linesize[1], textureWidth); // NV12 UV plane is full width
                                                             }
                                                         }
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
                       if ((leftEdgeFadeWidth > 0 || rightEdgeFadeWidth > 0) && textureWidth > (leftEdgeFadeWidth + rightEdgeFadeWidth) && pitch > 0) { // Check if fade is enabled and texture is wide enough
                            for (int y = 0; y < textureHeight; ++y) {
                                uint8_t* yPlane = dst_data[0] + y * pitch;
                                // Left edge fade
                                if (leftEdgeFadeWidth > 0) { // Apply only if width > 0
                                    for (int x = 0; x < leftEdgeFadeWidth; ++x) {
                                        float fade = static_cast<float>(x) / (leftEdgeFadeWidth > 1 ? (leftEdgeFadeWidth - 1) : 1);
                                        yPlane[x] = static_cast<uint8_t>(yPlane[x] * fade + 16.0f * (1.0f - fade));
                                    }
                                }
                                // Right edge fade
                                if (rightEdgeFadeWidth > 0) { // Apply only if width > 0
                                    for (int x = 0; x < rightEdgeFadeWidth; ++x) {
                                        int realX = textureWidth - 1 - x;
                                        float fade = static_cast<float>(x) / (rightEdgeFadeWidth > 1 ? (rightEdgeFadeWidth - 1) : 1);
                                        // Boundary check for realX (already covered by loop limit and textureWidth check)
                                        yPlane[realX] = static_cast<uint8_t>(yPlane[realX] * fade + 16.0f * (1.0f - fade));
                                    }
                                }
                            }
                       }
                       // --- End Edge Fade --- 

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

    } else { // --- No frameToDisplay provided ---
        // std::cout << "[TimingDebug:" << ms_since_epoch << "] >> No AVFrame provided for frame " << newCurrentFrame << "! Trying redraw." << std::endl; // Optional log
        // Attempt to redraw the last SW texture only if it exists and something was rendered before
        // Do nothing if Metal was used previously or no SW texture exists
    }

     // --- Final Rendering Stage ---
     // Calculate destRect based on current texture dimensions (could be from Metal or SW path)
     // Ensure textureWidth and textureHeight are up-to-date
    if (firstFrameRendered && textureWidth > 0 && textureHeight > 0) {
        float aspectRatio = static_cast<float>(textureWidth) / textureHeight;
                      if (windowWidth / aspectRatio <= windowHeight) {
                          destRect.w = windowWidth;
                          destRect.h = static_cast<int>(windowWidth / aspectRatio);
                      } else {
                          destRect.h = windowHeight;
                          destRect.w = static_cast<int>(windowHeight * aspectRatio);
                      }
                      destRect.x = (windowWidth - destRect.w) / 2;
                      destRect.y = (windowHeight - destRect.h) / 2;

        // Apply Jitter Offset just before rendering (conceptually)
        // Jitter calculated earlier within the SW path is used here
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


        // Render the final texture (either Metal's output implicitly, or our SW texture)
        if (!renderedWithMetal && lastTexture) { // Render SW texture if it exists and wasn't Metal
             renderSoftwareTexture(renderer, lastTexture, textureWidth, textureHeight, destRect); // Pass final destRect
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
    } else if (!firstFrameRendered) {
         // Nothing rendered yet, ensure screen is black
         SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255); SDL_RenderClear(renderer);
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
        renderOSD(renderer, font, isPlaying.load(), currentPlaybackRate, isReverse, currentTime, newCurrentFrame, showOSD, waitingForTimecode, inputTimecode, originalFps, jog_forward.load(), jog_backward.load(), false, "", 0);
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

void renderLoadingScreen(SDL_Renderer* renderer, TTF_Font* font, const std::string& loadingType, int loadingProgress) {
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
    
    // Render the OSD with loading information
    if (font) renderOSD(renderer, font, false, 0.0, false, 0.0, 0, true, false, "", 25.0, false, false, true, loadingType, loadingProgress);
    
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