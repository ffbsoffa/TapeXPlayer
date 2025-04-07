#include <iostream>
#include <string>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <random>
#include <cmath>
#include <chrono>
#include <SDL.h>
#include <SDL_ttf.h>

// Proper inclusion of C libraries
extern "C" {
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
#include <libavutil/rational.h>
}

#include "display.h"
#include "decode.h"
#include "common.h"

// External zoom variables declared in common.h
extern std::atomic<bool> zoom_enabled;
extern std::atomic<float> zoom_factor;
extern std::atomic<float> zoom_center_x;
extern std::atomic<float> zoom_center_y;
extern std::atomic<bool> show_zoom_thumbnail;

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
                case FrameInfo::CACHED:
                    SDL_SetRenderDrawColor(renderer, 0, 255, 128, 255); // Light green for cached frames
                    break;
            }
        } else {
            // For frames outside buffer, check for cached frames
            if (frameIndex[i].type == FrameInfo::CACHED) {
                SDL_SetRenderDrawColor(renderer, 0, 128, 64, 255); // Darker green for cached frames outside buffer
            } else {
                SDL_SetRenderDrawColor(renderer, 32, 32, 32, 255); // Very dark gray for frames outside the buffer
            }
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

void displayCurrentFrame(SDL_Renderer* renderer, const FrameInfo& frameInfo, bool enableHighResDecode, double playbackRate, double currentTime, double totalDuration) {
    // Get frame from intermediate buffer
    int frameIndex;
    FrameInfo::FrameType frameType;
    AVRational timeBase;
    std::shared_ptr<AVFrame> framePtr = frameBuffer.getFrame(frameIndex, frameType, timeBase);
    
    if (!framePtr) {
        // If no frame in buffer, exit
        return;
    }
    
    AVFrame* frame = framePtr.get();
    
    // Optimization - use global variables for caching
    static SDL_Texture* texture = nullptr;
    static SwsContext* swsContext = nullptr;
    static int textureWidth = 0, textureHeight = 0;
    static AVPixelFormat lastPixFormat = AV_PIX_FMT_NONE;

    // Recreate texture only if size or format changed
    if (!texture || textureWidth != frame->width || textureHeight != frame->height || lastPixFormat != frame->format) {
        if (texture) {
            SDL_DestroyTexture(texture);
        }
        if (swsContext) {
            sws_freeContext(swsContext);
            swsContext = nullptr;
        }

        SDL_PixelFormatEnum sdlFormat = SDL_PIXELFORMAT_IYUV;
        AVPixelFormat targetFormat = AV_PIX_FMT_YUV420P;

        switch (frame->format) {
            case AV_PIX_FMT_YUV420P:
                sdlFormat = SDL_PIXELFORMAT_IYUV;
                targetFormat = AV_PIX_FMT_YUV420P;
                break;
            case AV_PIX_FMT_NV12:
                sdlFormat = SDL_PIXELFORMAT_NV12;
                targetFormat = AV_PIX_FMT_NV12;
                break;
            case AV_PIX_FMT_YUV422P:
            case AV_PIX_FMT_YUV422P10LE:
                sdlFormat = SDL_PIXELFORMAT_IYUV;
                targetFormat = AV_PIX_FMT_YUV420P;
                break;
            default:
                std::cerr << "Unsupported pixel format: " << frame->format << std::endl;
                return;
            }

        texture = SDL_CreateTexture(
            renderer,
            sdlFormat,
            SDL_TEXTUREACCESS_STREAMING,
            frame->width,
            frame->height
        );
        textureWidth = frame->width;
        textureHeight = frame->height;
        lastPixFormat = static_cast<AVPixelFormat>(frame->format);

        SDL_SetTextureScaleMode(texture, SDL_ScaleModeBest);

        swsContext = sws_getContext(
            frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
            frame->width, frame->height, targetFormat,
            SWS_BILINEAR, nullptr, nullptr, nullptr
        );
    }

    if (!texture || !swsContext) {
        std::cerr << "Error creating texture or SwsContext" << std::endl;
        return;
    }

    int windowWidth, windowHeight;
    SDL_GetRendererOutputSize(renderer, &windowWidth, &windowHeight);

    SDL_Rect destRect;
    float aspectRatio = static_cast<float>(frame->width) / frame->height;
    if (windowWidth / aspectRatio <= windowHeight) {
        destRect.w = windowWidth;
        destRect.h = static_cast<int>(windowWidth / aspectRatio);
    } else {
        destRect.h = windowHeight;
        destRect.w = static_cast<int>(windowHeight * aspectRatio);
    }
    destRect.x = (windowWidth - destRect.w) / 2;
    destRect.y = (windowHeight - destRect.h) / 2;

    // Add jitter effect based on playback speed
    static std::random_device rd;
    static std::mt19937 rng(rd());
    static std::normal_distribution<> jitterDist(0.0, 1.0);
    
    // Calculate jitter amplitude based on playback speed
    double jitterAmplitude = 0;
    double absPlaybackRate = std::abs(playbackRate);
    
    if (absPlaybackRate >= 0.20 && absPlaybackRate < 0.30) {
        // Very slight jitter at around 0.25x speed
        double t = (absPlaybackRate - 0.20) / 0.10;
        jitterAmplitude = 1.0 + t * 2.0; // Linear interpolation from 1 to 3 pixels
    } else if (absPlaybackRate >= 0.30 && absPlaybackRate < 0.90) {
        // Same jitter as speeds from 1.3 to 1.9
        double t = (absPlaybackRate - 0.30) / 0.60;
        jitterAmplitude = 3.0;
    } else if (absPlaybackRate >= 1.3 && absPlaybackRate < 1.9) {
        // More pronounced jitter for speeds from 1.3 to 1.9
        double t = (absPlaybackRate - 1.3) / 0.6;
        jitterAmplitude = 10.0 + t * 9.0; // Linear interpolation from 10 to 19 pixels
    } else if (absPlaybackRate >= 1.9 && absPlaybackRate < 4.0) {
        jitterAmplitude = 4.0;
    } else if (absPlaybackRate >= 4.0 && absPlaybackRate < 16.0) {
        // Linear interpolation from 2 to 6 pixels
        double t = (absPlaybackRate - 4.0) / 12.0;
        jitterAmplitude = 1.4 + t * 4.0;
    } else if (absPlaybackRate >= 20.0) {
        jitterAmplitude = 6.0;
    }

    if (jitterAmplitude > 0) {
        // Use sharper distribution for speeds from 0.30 to 0.90 and 1.3 to 2.0
        if ((absPlaybackRate >= 0.30 && absPlaybackRate < 0.90) || (absPlaybackRate >= 1.3 && absPlaybackRate < 2.0)) {
            std::uniform_real_distribution<> sharpJitterDist(-1.0, 1.0);
            int jitterY = static_cast<int>(sharpJitterDist(rng) * jitterAmplitude);
            destRect.y += jitterY;
        } else {
            int jitterY = static_cast<int>(jitterDist(rng) * jitterAmplitude);
            destRect.y += jitterY;
        }
    }

    // Apply rewind effect for high speeds
    const double threshold = 0.1;
    if (std::abs(playbackRate) >= 1.1 && currentTime > threshold && (totalDuration - currentTime) > threshold) {
        // Lock texture for updating
        void* pixels;
        int pitch;
        SDL_LockTexture(texture, NULL, &pixels, &pitch);

        uint8_t* destData[4] = { static_cast<uint8_t*>(pixels), nullptr, nullptr, nullptr };
        int destLinesize[4] = { pitch, 0, 0, 0 };

        if (frame->format == AV_PIX_FMT_NV12) {
            destData[1] = destData[0] + pitch * frame->height;
            destLinesize[1] = pitch;
        } else {
            destData[1] = destData[0] + pitch * frame->height;
            destData[2] = destData[1] + (pitch / 2) * (frame->height / 2);
            destLinesize[1] = pitch / 2;
            destLinesize[2] = pitch / 2;
        }

        sws_scale(swsContext, frame->data, frame->linesize, 0, frame->height, destData, destLinesize);
        
        // Apply black and white effect for high speeds
        if (std::abs(playbackRate) >= 10.0) {
            // Full black and white mode for entire frame
            for (int y = 0; y < frame->height / 2; ++y) {
                memset(destData[1] + y * destLinesize[1], 128, frame->width / 2);
                memset(destData[2] + y * destLinesize[2], 128, frame->width / 2);
            }
        }

        // Calculate parameters for stripe effect
        const double resolutionScale = static_cast<double>(frame->height) / 1080.0;
        const int maxStripeHeight = static_cast<int>(720 * resolutionScale);
        const int baseStripeHeight = static_cast<int>(85 * resolutionScale);
        const int baseStripeSpacing = static_cast<int>(450 * resolutionScale);
        const int minStripeSpacing = static_cast<int>(48 * resolutionScale);
        const int minStripeHeight = static_cast<int>(11 * resolutionScale);
        const int midStripeHeight = static_cast<int>(50 * resolutionScale);

        // Calculate stripe parameters based on speed
        int stripeHeight, stripeSpacing;
        if ((playbackRate >= 0.2 && playbackRate < 0.9) || (playbackRate >= 1.2 && playbackRate < 2.0)) {
            double t = (playbackRate < 0.9) ? 
                      (playbackRate - 0.2) / 0.7 : 
                      (playbackRate - 1.2) / 0.8;
            t = t * t * (3 - 2 * t); // smooth interpolation
            stripeHeight = static_cast<int>(maxStripeHeight * (1 - t) + midStripeHeight * t);
            stripeSpacing = baseStripeSpacing;
        } else if (playbackRate >= 2.0 && playbackRate < 4.0) {
            stripeHeight = baseStripeHeight;
            stripeSpacing = baseStripeSpacing;
        } else if (playbackRate >= 3.5 && playbackRate < 14.0) {
            double t = (playbackRate - 4.0) / 10.0;
            t = std::pow(t, 0.7);
            stripeHeight = static_cast<int>(baseStripeHeight * (1.0 - t) + minStripeHeight * t);
            stripeSpacing = static_cast<int>(baseStripeSpacing * (1.0 - t) + minStripeSpacing * t);
        } else {
            stripeHeight = minStripeHeight;
            stripeSpacing = minStripeSpacing;
        }

        // Calculate cycle progress for stripe animation
        double cycleProgress;
        const double baseDuration = 1.5;
        const int fps = 30; // Assume 30 fps, adjust if needed

        if (playbackRate >= 14.0) {
            double speedFactor = std::pow(playbackRate/14.0, 1.2) * 4.02;
            if (playbackRate >= 16.0) {
                speedFactor *= 1.0 + (playbackRate - 16.0) * 0.417;
            }
            double highSpeedCycleDuration = 1.0 / (fps * speedFactor);
            cycleProgress = std::fmod(currentTime, highSpeedCycleDuration) / highSpeedCycleDuration;
        } else if (playbackRate >= 12.0) {
            double t = (playbackRate - 12.0) / 2.0;
            double speedMultiplier = 0.4 + t * 0.6;
            double transitionDuration = baseDuration / (playbackRate * speedMultiplier);
            cycleProgress = std::fmod(currentTime, transitionDuration) / transitionDuration;
        } else if (playbackRate >= 3.5) {
            double normalizedSpeed = (playbackRate - 3.5) / 8.5;
            double speedMultiplier = 0.08 + (std::pow(normalizedSpeed, 3) * 0.15);
            if (playbackRate < 8.0) {
                speedMultiplier *= 0.7;
            }
            double mediumSpeedDuration = baseDuration / (playbackRate * speedMultiplier);
            cycleProgress = std::fmod(currentTime, mediumSpeedDuration) / mediumSpeedDuration;
        } else {
            double speedFactor = std::min(playbackRate / 2.0, 2.0);
            double adjustedDuration = baseDuration / speedFactor;
            cycleProgress = std::fmod(currentTime, adjustedDuration) / adjustedDuration;
        }

        // Add black and white zones under gray stripes for speeds 2x-10x
        if (std::abs(playbackRate) >= 2.0 && std::abs(playbackRate) < 10.0) {
            int stripeCount = (frame->height + stripeSpacing) / stripeSpacing;
            for (int i = 0; i < stripeCount; ++i) {
                double baseOffset = cycleProgress * stripeSpacing + i * stripeSpacing;
                double heightVariation = (playbackRate >= 14.0) ? 0 : ((rand() % 21 - 10) * resolutionScale);
                int currentStripeHeight = std::max(static_cast<int>(stripeHeight + heightVariation), minStripeHeight);
                
                // Black and white zone 75% larger than gray stripe
                int bwZoneHeight = static_cast<int>(currentStripeHeight * 1.75);
                
                // Center black and white zone relative to gray stripe
                // Gray stripe starts at y with height currentStripeHeight
                // Need to shift black and white zone start up by half the height difference
                int heightDifference = bwZoneHeight - currentStripeHeight;
                int y_stripe = static_cast<int>(std::fmod(baseOffset, frame->height + stripeSpacing)) - currentStripeHeight;
                int y_bw = y_stripe - heightDifference / 2;
                
                if (y_bw >= 0 && y_bw < frame->height) {
                    // Define black and white zone boundaries
                    int bwStartY = std::max(0, y_bw);
                    int bwEndY = std::min(frame->height, y_bw + bwZoneHeight);
                    
                    // Zero out color information within black and white zone
                    for (int yPos = bwStartY; yPos < bwEndY; ++yPos) {
                        // Reduce brightness (Y) - make image slightly darker
                        uint8_t* yPlane = destData[0] + yPos * pitch;
                        for (int x = 0; x < frame->width; ++x) {
                            // Reduce brightness by about 15%
                            int currentY = yPlane[x];
                            int newY = static_cast<int>(currentY * 0.85); // Multiplier to get roughly 112 from 128
                            yPlane[x] = static_cast<uint8_t>(newY);
                        }
                        
                        // Zero out color information (U and V)
                        if (frame->format == AV_PIX_FMT_YUV420P) {
                            int uvY = yPos / 2;
                            if (uvY < frame->height / 2) {
                                memset(destData[1] + uvY * destLinesize[1], 128, frame->width / 2); // U
                                memset(destData[2] + uvY * destLinesize[2], 128, frame->width / 2); // V
                            }
                        } else if (frame->format == AV_PIX_FMT_NV12) {
                            int uvY = yPos / 2;
                            if (uvY < frame->height / 2) {
                                uint8_t* uvPlane = destData[1] + uvY * destLinesize[1];
                                for (int x = 0; x < frame->width / 2; ++x) {
                                    uvPlane[x * 2] = 128;     // U
                                    uvPlane[x * 2 + 1] = 128; // V
                                }
                            }
                        }
                    }
                }
            }
        }

        // Main loop for drawing gray stripes
        int stripeCount = (frame->height + stripeSpacing) / stripeSpacing;
        for (int i = 0; i < stripeCount; ++i) {
            double baseOffset = cycleProgress * stripeSpacing + i * stripeSpacing;
            double heightVariation = (playbackRate >= 14.0) ? 0 : ((rand() % 21 - 10) * resolutionScale);
            int currentStripeHeight = std::max(static_cast<int>(stripeHeight + heightVariation), minStripeHeight);
            int y = static_cast<int>(std::fmod(baseOffset, frame->height + stripeSpacing)) - currentStripeHeight;
            
            if (y >= 0 && y < frame->height) {
                // Draw gray stripe without blending
                int startY = std::max(0, y);
                int endY = std::min(frame->height, y + currentStripeHeight);
                
                for (int yPos = startY; yPos < endY; ++yPos) {
                    // Fill Y channel with gray
                    memset(destData[0] + yPos * pitch, 128, frame->width);
                    
                    // Fill U and V channels with neutral values for pure gray
                    if (frame->format == AV_PIX_FMT_YUV420P) {
                        int uvPitch = destLinesize[1];
                        int uvY = yPos / 2;
                        if (uvY < frame->height / 2) {
                            memset(destData[1] + uvY * uvPitch, 128, frame->width / 2); // U-channel
                            memset(destData[2] + uvY * uvPitch, 128, frame->width / 2); // V-channel
                        }
                    } else if (frame->format == AV_PIX_FMT_NV12) {
                        int uvPitch = destLinesize[1];
                        int uvY = yPos / 2;
                        if (uvY < frame->height / 2) {
                            // In NV12 format U and V alternate
                            uint8_t* uvPlane = destData[1] + uvY * uvPitch;
                            for (int x = 0; x < frame->width / 2; ++x) {
                                uvPlane[x * 2] = 128;     // U-component
                                uvPlane[x * 2 + 1] = 128; // V-component
                            }
                        }
                    }
                }
                
                // Add snow effect on first pixel of stripe (enhanced version)
                if (playbackRate >= 4.0) {
                    // Snow only on first line of stripe
                    int snowY = startY;
                    
                    // Return to moderate number of snowflakes
                    int snowCount = frame->width / 80; // Roughly 1 snowflake per 80 pixels width
                    
                    // Adjust count for high speeds
                    if (playbackRate > 10.0) {
                        snowCount = static_cast<int>(snowCount * 1.5);
                    }
                    
                    snowCount = std::max(8, snowCount); // Minimum 8 snowflakes
                    
                    for (int j = 0; j < snowCount; ++j) {
                        // Random snowflake position on X
                        int snowX = rand() % frame->width;
                        
                        // Different tail lengths for different snowflakes
                        // Use quadratic speed dependency for more natural look
                        double speedFactor = std::sqrt(playbackRate);
                        int tailBase = 10 + static_cast<int>(rand() % 20); // Base random length
                        int tailLength = tailBase + static_cast<int>(speedFactor * 5.0);
                        
                        // Initial point brightness (more pronounced)
                        destData[0][snowY * pitch + snowX] = 235; // Very bright initial point
                        
                        // Draw snowflake tail (back to sharper fade)
                        for (int i = 1; i < tailLength; i++) {
                            int xPos = (snowX + i) % frame->width;
                            
                            // Exponential fade with sharper drop
                            double fadeFactor = exp(-0.15 * i);
                            int brightness = 128 + static_cast<int>(107 * fadeFactor); // From 235 to 128
                            
                            destData[0][snowY * pitch + xPos] = brightness;
                        }
                    }
                }
            }
        }

        // After processing black and white zones and gray stripes, add scanline duplication effect
        // for very high speeds (18x and above) with smooth ramp up to 24x
        if (std::abs(playbackRate) >= 18.0) {
            // Calculate effect intensity coefficient (from 0.0 at 18x to 1.0 at 24x)
            double effectIntensity = std::min(1.0, (std::abs(playbackRate) - 18.0) / 6.0);
            
            // Find areas between stripes
            std::vector<std::pair<int, int>> clearAreas; // pairs of (start, end) for areas between stripes
            
            // Create map of areas occupied by stripes
            std::vector<bool> stripeMask(frame->height, false);
            
            // Mark areas occupied by gray stripes and black and white zones
            int stripeCount = (frame->height + stripeSpacing) / stripeSpacing;
            for (int i = 0; i < stripeCount; ++i) {
                double baseOffset = cycleProgress * stripeSpacing + i * stripeSpacing;
                double heightVariation = (playbackRate >= 14.0) ? 0 : ((rand() % 21 - 10) * resolutionScale);
                int currentStripeHeight = std::max(static_cast<int>(stripeHeight + heightVariation), minStripeHeight);
                
                // Account for extended black and white zones
                if (std::abs(playbackRate) >= 2.0 && std::abs(playbackRate) < 10.0) {
                    int bwZoneHeight = static_cast<int>(currentStripeHeight * 1.75);
                    int heightDifference = bwZoneHeight - currentStripeHeight;
                    int y_stripe = static_cast<int>(std::fmod(baseOffset, frame->height + stripeSpacing)) - currentStripeHeight;
                    int y_bw = y_stripe - heightDifference / 2;
                    
                    if (y_bw >= 0 && y_bw < frame->height) {
                        int bwStartY = std::max(0, y_bw);
                        int bwEndY = std::min(frame->height, y_bw + bwZoneHeight);
                        
                        for (int yy = bwStartY; yy < bwEndY; ++yy) {
                            stripeMask[yy] = true;
                        }
                    }
                } else {
                    // Gray stripes only
                    int y = static_cast<int>(std::fmod(baseOffset, frame->height + stripeSpacing)) - currentStripeHeight;
                    
                    if (y >= 0 && y < frame->height) {
                        int startY = std::max(0, y);
                        int endY = std::min(frame->height, y + currentStripeHeight);
                        
                        for (int yy = startY; yy < endY; ++yy) {
                            stripeMask[yy] = true;
                        }
                    }
                }
            }
            
            // Find continuous areas between stripes
            bool inClearArea = false;
            int clearStart = 0;
            
            for (int y = 0; y < frame->height; ++y) {
                if (!stripeMask[y] && !inClearArea) {
                    // Start of area between stripes
                    clearStart = y;
                    inClearArea = true;
                } else if ((stripeMask[y] || y == frame->height - 1) && inClearArea) {
                    // End of area between stripes
                    int clearEnd = stripeMask[y] ? y : y + 1;
                    
                    // Add only if area has sufficient height
                    if (clearEnd - clearStart > 4) {
                        clearAreas.push_back(std::make_pair(clearStart, clearEnd));
                    }
                    
                    inClearArea = false;
                }
            }
            
            // For each found area between stripes, apply scanline duplication effect
            for (const auto& area : clearAreas) {
                int areaStart = area.first;
                int areaEnd = area.second;
                int areaHeight = areaEnd - areaStart;
                
                // Define base duplication step and maximum duplication
                // Apply more aggressive duplication based on intensity
                int skipLines = 1; // How many lines to skip between duplications
                int baseDuplication = static_cast<int>(1 + 4 * effectIntensity); // 1 to 5 duplications
                
                // At high intensity decrease step (duplicate more frequently)
                if (effectIntensity > 0.7) skipLines = 1;
                else if (effectIntensity > 0.4) skipLines = 2;
                else skipLines = 3;
                
                // Apply additional random variation for more uneven effect
                for (int y = areaStart; y < areaEnd - baseDuplication; y += skipLines) {
                    // Random number of duplications for each line within base range
                    int duplication = std::min(areaEnd - y - 1, baseDuplication);
                    
                    // Duplication probability depends on intensity
                    if (rand() % 100 < effectIntensity * 100) {
                        // Copy line multiple times
                        for (int dup = 1; dup <= duplication; ++dup) {
                            if (y + dup >= areaEnd) break;
                            
                            // Copy Y channel with possible small shift for more "damaged" look
                            int shiftX = 0;
                            if (effectIntensity > 0.8 && (rand() % 100 < 30)) {
                                shiftX = (rand() % 3) - 1; // Random shift -1, 0 or 1
                            }
                            
                            if (shiftX == 0) {
                                // Direct copy without shift
                                memcpy(destData[0] + (y + dup) * pitch, 
                                       destData[0] + y * pitch, 
                                       frame->width);
                            } else {
                                // Copy with shift
                                if (shiftX > 0) {
                                    memcpy(destData[0] + (y + dup) * pitch + shiftX, 
                                           destData[0] + y * pitch, 
                                           frame->width - shiftX);
                                } else {
                                    memcpy(destData[0] + (y + dup) * pitch, 
                                           destData[0] + y * pitch - shiftX, 
                                           frame->width + shiftX);
                                }
                            }
                            
                            // Copy U and V channels (if they exist)
                            if (y/2 != (y+dup)/2 && (y+dup)/2 < frame->height/2) {
                                if (frame->format == AV_PIX_FMT_YUV420P) {
                                    memcpy(destData[1] + (y+dup)/2 * destLinesize[1],
                                           destData[1] + y/2 * destLinesize[1],
                                           frame->width/2);
                                    memcpy(destData[2] + (y+dup)/2 * destLinesize[2],
                                           destData[2] + y/2 * destLinesize[2],
                                           frame->width/2);
                                } else if (frame->format == AV_PIX_FMT_NV12) {
                                    memcpy(destData[1] + (y+dup)/2 * destLinesize[1],
                                           destData[1] + y/2 * destLinesize[1],
                                           frame->width);
                                }
                            }
                        }
                    }
                }
            }
        }

        SDL_UnlockTexture(texture);
    } else {
        // Normal display without effects
        void* pixels;
        int pitch;
        SDL_LockTexture(texture, NULL, &pixels, &pitch);

        uint8_t* destData[4] = { static_cast<uint8_t*>(pixels), nullptr, nullptr, nullptr };
        int destLinesize[4] = { pitch, 0, 0, 0 };

        if (frame->format == AV_PIX_FMT_NV12) {
            destData[1] = destData[0] + pitch * frame->height;
            destLinesize[1] = pitch;
        } else {
            destData[1] = destData[0] + pitch * frame->height;
            destData[2] = destData[1] + (pitch / 2) * (frame->height / 2);
            destLinesize[1] = pitch / 2;
            destLinesize[2] = pitch / 2;
        }

        sws_scale(swsContext, frame->data, frame->linesize, 0, frame->height, destData, destLinesize);
        SDL_UnlockTexture(texture);
    }

    // Check if zoom is enabled
    if (zoom_enabled.load()) {
        // Render zoomed frame
        renderZoomedFrame(renderer, texture, frame->width, frame->height, 
                         zoom_factor.load(), zoom_center_x.load(), zoom_center_y.load());
        
        // If thumbnail display is enabled, render it
        if (show_zoom_thumbnail.load()) {
            renderZoomThumbnail(renderer, texture, frame->width, frame->height, 
                               zoom_factor.load(), zoom_center_x.load(), zoom_center_y.load());
        }
    } else {
        // Normal rendering without zoom
        SDL_RenderCopy(renderer, texture, NULL, &destRect);
    }
}

void renderOSD(SDL_Renderer* renderer, TTF_Font* font, bool isPlaying, double playbackRate, bool isReverse, double currentTime, int frameNumber, bool showOSD, bool waiting_for_timecode, const std::string& input_timecode, double original_fps, bool jog_forward, bool jog_backward, bool isLoading, const std::string& loadingType, int loadingProgress) {
    // showOSD parameter is no longer used as check is done in displayFrame

    int windowWidth, windowHeight;
    SDL_GetRendererOutputSize(renderer, &windowWidth, &windowHeight);

    // Prepare text color
    SDL_Color textColor = {255, 255, 255, 255};  // White color
    SDL_Color grayColor = {128, 128, 128, 255};  // Gray color for inactive digits during timecode input

    // Render left text
    std::string leftText;
    if (isLoading) {
        leftText = "UNTHREAD";
    } else if (jog_forward || jog_backward) {
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
    
    // Create surface and texture for left text
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

    // Render timecode
    std::string timecode;
    if (isLoading) {
        // Blinking timecode during loading
        static Uint32 lastBlinkTime = 0;
        static bool showDashes = true;
        
        Uint32 currentTime = SDL_GetTicks();
        if (currentTime - lastBlinkTime > 500) { // Blink every 500ms
            showDashes = !showDashes;
            lastBlinkTime = currentTime;
        }
        
        timecode = showDashes ? "--:--:--:--" : "  :  :  :  ";
    } else if (waiting_for_timecode) {
        timecode = "00:00:00:00";
        for (size_t i = 0; i < input_timecode.length() && i < 8; ++i) {
            size_t pos = i + (i / 2);  // Account for colon positions
            timecode[pos] = input_timecode[i];
        }
    } else {
        int hours = static_cast<int>(currentTime / 3600);
        int minutes = static_cast<int>((currentTime - hours * 3600) / 60);
        int seconds = static_cast<int>(currentTime) % 60;
        int frames = static_cast<int>((currentTime - static_cast<int>(currentTime)) * original_fps);

        // Format timecode
        char buffer[12];
        snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d:%02d", hours, minutes, seconds, frames);
        timecode = buffer;
    }
    
    // Create surface and texture for timecode
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

    // Render right text (playback rate or loading progress)
    std::string rightText;
    if (isLoading) {
        // Format: XX000 where XX is the loading type and 000 is the progress percentage
        std::string prefix;
        if (loadingType == "youtube") {
            prefix = "DL";
        } else if (loadingType == "file") {
            prefix = "LD";
        } else if (loadingType == "convert") {
            prefix = "LS";
        } else {
            prefix = "LD"; // Default to LD if type not specified
        }
        
        char progressBuffer[5];
        snprintf(progressBuffer, sizeof(progressBuffer), "%03d", loadingProgress);
        rightText = prefix + std::string(progressBuffer);
    } else {
        if (isReverse) {
            rightText = "REV ";
        } else {
            rightText = "FWD ";
        }
        
        // Add playback rate
        char rateBuffer[10];
        if (std::abs(playbackRate) < 0.01) {
            snprintf(rateBuffer, sizeof(rateBuffer), "0");
        } else if (std::abs(playbackRate - 1.0) < 0.01) {
            snprintf(rateBuffer, sizeof(rateBuffer), "1");
        } else {
            snprintf(rateBuffer, sizeof(rateBuffer), "%.2f", std::abs(playbackRate));
        }
        rightText += rateBuffer;
        rightText += "x";
    }
    
    // Create surface and texture for right text
    SDL_Surface* rightSurface = TTF_RenderText_Blended(font, rightText.c_str(), textColor);
    if (rightSurface) {
        SDL_Texture* rightTexture = SDL_CreateTextureFromSurface(renderer, rightSurface);
        if (rightTexture) {
            SDL_Rect rightRect = {
                windowWidth - rightSurface->w - 10, 
                windowHeight - 30 + (30 - rightSurface->h) / 2, 
                rightSurface->w, 
                rightSurface->h
            };
            SDL_RenderCopy(renderer, rightTexture, NULL, &rightRect);
            SDL_DestroyTexture(rightTexture);
        }
        SDL_FreeSurface(rightSurface);
    }
}

void displayFrame(SDL_Renderer* renderer, const std::vector<FrameInfo>& frameIndex, int newCurrentFrame, bool enableHighResDecode, double playbackRate, double currentTime, double totalDuration, bool showIndex, bool showOSD, TTF_Font* font, bool isPlaying, bool isReverse, bool waitingForTimecode, const std::string& inputTimecode, double originalFps, bool jogForward, bool jogBackward, size_t ringBufferCapacity, int highResWindowSize) {
    // Limit refresh rate for smoothness
    static std::chrono::steady_clock::time_point lastFrameTime = std::chrono::steady_clock::now();
    static const int targetFPS = 60; // Target frame rate
    static const std::chrono::milliseconds frameInterval(1000 / targetFPS);
    
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastFrameTime);
    
    // Skip rendering if not enough time has passed since last frame
    // But at high speeds (>= 10x) always update to not miss cached frames
    if (elapsed < frameInterval && std::abs(playbackRate) < 10.0) {
        return;
    }
    
    lastFrameTime = now;
    
    // Get window dimensions
    int windowWidth, windowHeight;
    SDL_GetRendererOutputSize(renderer, &windowWidth, &windowHeight);
    
    // COMPLETELY CLEAR SCREEN BEFORE RENDERING
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    
    // Save video dimensions in static variables
    static int lastVideoWidth = 0;
    static int lastVideoHeight = 0;
    static float lastAspectRatio = 16.0f/9.0f; // Default 16:9

    // Get current frame from intermediate buffer
    int bufferFrameIndex;
    FrameInfo::FrameType bufferFrameType;
    AVRational bufferTimeBase;
    std::shared_ptr<AVFrame> framePtr = frameBuffer.getFrame(bufferFrameIndex, bufferFrameType, bufferTimeBase);
    
    // Update saved dimensions if frame exists
    if (framePtr) {
        AVFrame* frame = framePtr.get();
        lastVideoWidth = frame->width;
        lastVideoHeight = frame->height;
        lastAspectRatio = static_cast<float>(frame->width) / frame->height;
    }

    // Calculate video dimensions using saved aspect ratio
    SDL_Rect videoRect;
    if (windowWidth / lastAspectRatio <= windowHeight) {
        videoRect.w = windowWidth;
        videoRect.h = static_cast<int>(windowWidth / lastAspectRatio);
    } else {
        videoRect.h = windowHeight;
        videoRect.w = static_cast<int>(windowHeight * lastAspectRatio);
    }
    videoRect.x = (windowWidth - videoRect.w) / 2;
    videoRect.y = (windowHeight - videoRect.h) / 2;
    
    // Display current frame from intermediate buffer
    if (framePtr) {
        // Use empty FrameInfo since displayCurrentFrame now uses intermediate buffer
        FrameInfo dummyFrameInfo;
        displayCurrentFrame(renderer, dummyFrameInfo, enableHighResDecode, std::abs(playbackRate), currentTime, totalDuration);
    } else {
        // If no frame, draw blue background
        SDL_SetRenderDrawColor(renderer, 0, 0, 128, 255);
        SDL_RenderFillRect(renderer, &videoRect);
    }
    
    // Display index if enabled
    if (showIndex) {
        // Calculate buffer boundaries
        int bufferStart = std::max(0, newCurrentFrame - static_cast<int>(ringBufferCapacity / 2));
        int bufferEnd = std::min(static_cast<int>(frameIndex.size()) - 1, bufferStart + static_cast<int>(ringBufferCapacity) - 1);
        
        // Calculate high-res window boundaries
        int highResStart = std::max(0, newCurrentFrame - highResWindowSize / 2);
        int highResEnd = std::min(static_cast<int>(frameIndex.size()) - 1, highResStart + highResWindowSize - 1);
        
        // Update visualization
        updateVisualization(renderer, frameIndex, newCurrentFrame, bufferStart, bufferEnd, highResStart, highResEnd, enableHighResDecode);
    }
    
    // RENDER OSD DIRECTLY TO MAIN RENDERER
    if (showOSD) {
        // Clear only OSD area
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 150); // Semi-transparent black
        SDL_Rect osdRect = {0, windowHeight - 30, windowWidth, 30};
        SDL_RenderFillRect(renderer, &osdRect);
        
        // Render OSD directly using renamed function
        renderOSD(renderer, font, isPlaying, playbackRate, isReverse, currentTime, newCurrentFrame, 
                true, waitingForTimecode, inputTimecode, originalFps, jogForward, jogBackward, false, "", 0);
        
        // If zoom enabled, display zoom info
        if (zoom_enabled.load()) {
            std::string zoomInfo = "Zoom: " + std::to_string(static_cast<int>(zoom_factor.load() * 100)) + "%";
            SDL_Color textColor = {255, 255, 255, 255};
            SDL_Surface* zoomSurface = TTF_RenderText_Blended(font, zoomInfo.c_str(), textColor);
            if (zoomSurface) {
                SDL_Texture* zoomTexture = SDL_CreateTextureFromSurface(renderer, zoomSurface);
                if (zoomTexture) {
                    SDL_Rect zoomRect = {
                        windowWidth - zoomSurface->w - 10, 
                        windowHeight - 60, // Above main OSD
                        zoomSurface->w, 
                        zoomSurface->h
                    };
                    SDL_RenderCopy(renderer, zoomTexture, NULL, &zoomRect);
                    SDL_DestroyTexture(zoomTexture);
                }
                SDL_FreeSurface(zoomSurface);
            }
        }
    }
    
    // Update screen
    SDL_RenderPresent(renderer);
}

// Function to clean up resources when program exits
void cleanupDisplayResources() {
    // Clean up static resources from displayCurrentFrame
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
    renderOSD(renderer, font, false, 0.0, false, 0.0, 0, true, false, "", 25.0, false, false, true, loadingType, loadingProgress);
    
    // Present the renderer
    SDL_RenderPresent(renderer);
}

// Implementation of zoom functions
void renderZoomedFrame(SDL_Renderer* renderer, SDL_Texture* texture, int frameWidth, int frameHeight, float zoomFactor, float centerX, float centerY) {
    int windowWidth, windowHeight;
    SDL_GetRendererOutputSize(renderer, &windowWidth, &windowHeight);
    
    // Calculate video dimensions maintaining aspect ratio
    SDL_Rect destRect;
    float aspectRatio = static_cast<float>(frameWidth) / frameHeight;
    if (windowWidth / aspectRatio <= windowHeight) {
        destRect.w = windowWidth;
        destRect.h = static_cast<int>(windowWidth / aspectRatio);
    } else {
        destRect.h = windowHeight;
        destRect.w = static_cast<int>(windowHeight * aspectRatio);
    }
    destRect.x = (windowWidth - destRect.w) / 2;
    destRect.y = (windowHeight - destRect.h) / 2;
    
    // Add jitter effect based on playback speed
    static std::random_device rd;
    static std::mt19937 rng(rd());
    static std::normal_distribution<> jitterDist(0.0, 1.0);
    
    // Get current playback speed
    double playbackRate = std::abs(playback_rate.load());
    
    // Calculate jitter amplitude based on playback speed
    double jitterAmplitude = 0;
    double absPlaybackRate = playbackRate;
    
    if (absPlaybackRate >= 0.20 && absPlaybackRate < 0.30) {
        // Very slight jitter around 0.25x speed
        double t = (absPlaybackRate - 0.20) / 0.10;
        jitterAmplitude = 1.0 + t * 2.0; // Linear interpolation from 1 to 3 pixels
    } else if (absPlaybackRate >= 0.30 && absPlaybackRate < 0.90) {
        // Same jitter as speeds from 1.3 to 1.9
        double t = (absPlaybackRate - 0.30) / 0.60;
        jitterAmplitude = 3.0;
    } else if (absPlaybackRate >= 1.3 && absPlaybackRate < 1.9) {
        // Sharper jitter for speeds from 1.3 to 1.9
        double t = (absPlaybackRate - 1.3) / 0.6;
        jitterAmplitude = 10.0 + t * 9.0; // Linear interpolation from 10 to 19 pixels
    } else if (absPlaybackRate >= 1.9 && absPlaybackRate < 4.0) {
        jitterAmplitude = 4.0;
    } else if (absPlaybackRate >= 4.0 && absPlaybackRate < 16.0) {
        // Linear interpolation from 2 to 6 pixels
        double t = (absPlaybackRate - 4.0) / 12.0;
        jitterAmplitude = 1.4 + t * 4.0;
    } else if (absPlaybackRate >= 20.0) {
        jitterAmplitude = 6.0;
    }

    if (jitterAmplitude > 0) {
        // Use sharper distribution for speeds 0.30-0.90 and 1.3-2.0
        if ((absPlaybackRate >= 0.30 && absPlaybackRate < 0.90) || (absPlaybackRate >= 1.3 && absPlaybackRate < 2.0)) {
            std::uniform_real_distribution<> sharpJitterDist(-1.0, 1.0);
            int jitterY = static_cast<int>(sharpJitterDist(rng) * jitterAmplitude);
            destRect.y += jitterY;
        } else {
            int jitterY = static_cast<int>(jitterDist(rng) * jitterAmplitude);
            destRect.y += jitterY;
        }
    }
    
    // Calculate zoomed area dimensions and position
    SDL_Rect srcRect;
    srcRect.w = static_cast<int>(frameWidth / zoomFactor);
    srcRect.h = static_cast<int>(frameHeight / zoomFactor);
    
    // Center zoomed area around specified point
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
    
    // Thumbnail size (20% of screen width, max 300px)
    int thumbnailWidth = std::min(300, static_cast<int>(windowWidth * 0.2f));
    int thumbnailHeight = static_cast<int>(thumbnailWidth / (static_cast<float>(frameWidth) / frameHeight));
    
    // Thumbnail position (top-right corner with padding)
    int padding = 10;
    SDL_Rect thumbnailRect = {
        windowWidth - thumbnailWidth - padding,
        padding,
        thumbnailWidth,
        thumbnailHeight
    };
    
    // Render full frame thumbnail
    SDL_RenderCopy(renderer, texture, NULL, &thumbnailRect);
    
    // Draw border around thumbnail
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    SDL_RenderDrawRect(renderer, &thumbnailRect);
    
    // Calculate and draw rectangle showing zoomed area
    SDL_Rect zoomRect;
    zoomRect.w = static_cast<int>(thumbnailWidth / zoomFactor);
    zoomRect.h = static_cast<int>(thumbnailHeight / zoomFactor);
    zoomRect.x = thumbnailRect.x + static_cast<int>(centerX * thumbnailWidth - zoomRect.w / 2);
    zoomRect.y = thumbnailRect.y + static_cast<int>(centerY * thumbnailHeight - zoomRect.h / 2);
    
    // Constrain area to thumbnail boundaries
    if (zoomRect.x < thumbnailRect.x) zoomRect.x = thumbnailRect.x;
    if (zoomRect.y < thumbnailRect.y) zoomRect.y = thumbnailRect.y;
    if (zoomRect.x + zoomRect.w > thumbnailRect.x + thumbnailRect.w) 
        zoomRect.x = thumbnailRect.x + thumbnailRect.w - zoomRect.w;
    if (zoomRect.y + zoomRect.h > thumbnailRect.y + thumbnailRect.h) 
        zoomRect.y = thumbnailRect.y + thumbnailRect.h - zoomRect.h;
    
    // Draw rectangle showing zoomed area
    SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255);
    SDL_RenderDrawRect(renderer, &zoomRect);
}

void handleZoomMouseEvent(SDL_Event& event, int windowWidth, int windowHeight, int frameWidth, int frameHeight) {
    static int lastMouseX = 0;
    static int lastMouseY = 0;
    
    // Calculate video dimensions maintaining aspect ratio
    SDL_Rect videoRect;
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
        
        // If mouse is within video area and zoom enabled, update zoom center
        if (zoom_enabled.load() && 
            lastMouseX >= videoRect.x && lastMouseX < videoRect.x + videoRect.w &&
            lastMouseY >= videoRect.y && lastMouseY < videoRect.y + videoRect.h) {
            
            // Convert mouse coordinates to normalized frame coordinates (0.0-1.0)
            float normalizedX = static_cast<float>(lastMouseX - videoRect.x) / videoRect.w;
            float normalizedY = static_cast<float>(lastMouseY - videoRect.y) / videoRect.h;
            
            // Set zoom center
            set_zoom_center(normalizedX, normalizedY);
        }
    } else if (event.type == SDL_MOUSEWHEEL) {
        // If mouse is within video area, change zoom with mouse wheel
        if (lastMouseX >= videoRect.x && lastMouseX < videoRect.x + videoRect.w &&
            lastMouseY >= videoRect.y && lastMouseY < videoRect.y + videoRect.h) {
            
            if (event.wheel.y > 0) {
                // Scroll up - increase zoom
                increase_zoom();
            } else if (event.wheel.y < 0) {
                // Scroll down - decrease zoom
                decrease_zoom();
            }
        }
    } else if (event.type == SDL_MOUSEBUTTONDOWN) {
        if (event.button.button == SDL_BUTTON_RIGHT) {
            // Right mouse button - reset zoom
            reset_zoom();
        } else if (event.button.button == SDL_BUTTON_MIDDLE) {
            // Middle mouse button - toggle thumbnail display
            toggle_zoom_thumbnail();
        }
    }
}