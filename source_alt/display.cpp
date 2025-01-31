#include "display.h"
#include "decode.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <random>
#include <chrono>
#include <libswscale/swscale.h>
#include <libswscale/swscale.h>
#include <libavutil/rational.h>

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
                    // SDL_SetRenderDrawColor(renderer, 255, 255, 0, 255); // Yellow for high-res
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

void displayCurrentFrame(SDL_Renderer* renderer, const FrameInfo& frameInfo, bool enableHighResDecode, double playbackRate, double currentTime, double totalDuration) {
    std::lock_guard<std::mutex> lock(frameInfo.mutex);
    if (frameInfo.is_decoding) {
        return;
    }

    AVFrame* frame = (enableHighResDecode && frameInfo.frame) ? frameInfo.frame.get() : frameInfo.low_res_frame.get();
    
    if (!frame) {
        return;
    }

    static SDL_Texture* texture = nullptr;
    static SwsContext* swsContext = nullptr;
    static int textureWidth = 0, textureHeight = 0;
    static AVPixelFormat lastPixFormat = AV_PIX_FMT_NONE;

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

    if (std::abs(playbackRate) < 18.0) {
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
            uint8_t* yPlane = destData[0];
            uint8_t* uPlane = destData[1];
            uint8_t* vPlane = destData[2];

            for (int y = 0; y < frame->height; ++y) {
                for (int x = 0; x < frame->width; ++x) {
                    int uvIndex = (y / 2) * destLinesize[1] + (x / 2);

                    // Set U and V to 128 (neutral) for black and white effect
                    if (uPlane) uPlane[uvIndex] = 128;
                    if (vPlane) vPlane[uvIndex] = 128;
                }
            }
        }

        SDL_UnlockTexture(texture);
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

    // Добавляем эффект дрожания (jitter)
    static std::mt19937 rng(std::random_device{}());
    std::uniform_real_distribution<> jitterDist(-1.0, 1.0);

    // Рассчитываем амплитуду дрожания в зависимости от скорости воспроизведения
    double jitterAmplitude = 0;
    double absPlaybackRate = std::abs(playbackRate);
    
    if (absPlaybackRate >= 0.20 && absPlaybackRate < 0.30) {
        // Очень слабое дергание при скорости около 0.25
        double t = (absPlaybackRate - 0.20) / 0.10;
        jitterAmplitude = 1.0 + t * 2.0; // Линейная интерполяция от 1 до 3 пикселей
    } else if (absPlaybackRate >= 0.30 && absPlaybackRate < 0.90) {
        // Такое же дрожание, как и для скоростей от 1.3 до 1.9
        double t = (absPlaybackRate - 0.30) / 0.60;
        jitterAmplitude = 3.0;
    } else if (absPlaybackRate >= 1.3 && absPlaybackRate < 1.9) {
        // Более резкое дрожание для скоростей от 1.3 до 1.9
        double t = (absPlaybackRate - 1.3) / 0.6;
        jitterAmplitude = 10.0 + t * 9.0; // Линейная интерполяция от 10 до 19 пикселей
    } else if (absPlaybackRate >= 1.9 && absPlaybackRate < 4.0) {
        jitterAmplitude = 4.0;
    } else if (absPlaybackRate >= 4.0 && absPlaybackRate < 16.0) {
        // Линейная интерполяция от 2 до 6 пикселей
        double t = (absPlaybackRate - 4.0) / 12.0;
        jitterAmplitude = 1.4 + t * 4.0;
    } else if (absPlaybackRate >= 20.0) {
        jitterAmplitude = 6.0;
    }

    if (jitterAmplitude > 0) {
        // Используем более резкое распределение для скоростей от 0.30 до 0.90 и от 1.3 до 2.0
        if ((absPlaybackRate >= 0.30 && absPlaybackRate < 0.90) || (absPlaybackRate >= 1.3 && absPlaybackRate < 2.0)) {
            std::uniform_real_distribution<> sharpJitterDist(-1.0, 1.0);
            int jitterY = static_cast<int>(sharpJitterDist(rng) * jitterAmplitude);
            destRect.y += jitterY;
        } else {
            int jitterY = static_cast<int>(jitterDist(rng) * jitterAmplitude);
            destRect.y += jitterY;
        }
    }

    SDL_RenderCopy(renderer, texture, NULL, &destRect);
}

void renderOSD(SDL_Renderer* renderer, TTF_Font* font, bool isPlaying, double playbackRate, bool isReverse, double currentTime, int frameNumber, bool showOSD, bool waiting_for_timecode, const std::string& input_timecode, double original_fps, bool jog_forward, bool jog_backward) {
    if (!showOSD) return;

    int windowWidth, windowHeight;
    SDL_GetRendererOutputSize(renderer, &windowWidth, &windowHeight);

    // Create semi-transparent black background for OSD
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 150); // Полупрозрачный черный
    SDL_Rect osdRect = {0, windowHeight - 30, windowWidth, 30};
    SDL_RenderFillRect(renderer, &osdRect);

    // Prepare text color
    SDL_Color textColor = {255, 255, 255, 255};  // White color
    SDL_Color grayColor = {128, 128, 128, 255};  // Gray color for inactive digits during timecode input

    // Render left text
    std::string leftText;
    if (jog_forward || jog_backward) {
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
    SDL_Surface* leftSurface = TTF_RenderText_Blended(font, leftText.c_str(), textColor);
    SDL_Texture* leftTexture = SDL_CreateTextureFromSurface(renderer, leftSurface);
    SDL_Rect leftRect = {10, windowHeight - 30 + (30 - leftSurface->h) / 2, leftSurface->w, leftSurface->h};
    SDL_RenderCopy(renderer, leftTexture, NULL, &leftRect);

    // Render timecode
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
        int frames = static_cast<int>((currentTime - static_cast<int>(currentTime)) * original_fps);

        std::stringstream timecodeStream;
        timecodeStream << std::setfill('0') << std::setw(2) << hours << ":"
                       << std::setfill('0') << std::setw(2) << minutes << ":"
                       << std::setfill('0') << std::setw(2) << seconds << ":"
                       << std::setfill('0') << std::setw(2) << frames;
        timecode = timecodeStream.str();
    }

    // Render timecode with individual characters
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
        
        SDL_Surface* charSurface = TTF_RenderText_Blended(font, c, color);
        SDL_Texture* charTexture = SDL_CreateTextureFromSurface(renderer, charSurface);
        SDL_Rect charRect = {xPos, yPos, charSurface->w, charSurface->h};
        SDL_RenderCopy(renderer, charTexture, NULL, &charRect);
        xPos += charWidth;
        SDL_FreeSurface(charSurface);
        SDL_DestroyTexture(charTexture);
    }

    // Render right text
    std::string rightText = isReverse ? "REV" : "FWD";
    if (jog_forward || jog_backward) {
    } else if (std::abs(playbackRate) > 1.0) {
        int roundedSpeed = std::round(std::abs(playbackRate));
        rightText += " " + std::to_string(roundedSpeed) + "x";
    }
    SDL_Surface* rightSurface = TTF_RenderText_Blended(font, rightText.c_str(), textColor);
    SDL_Texture* rightTexture = SDL_CreateTextureFromSurface(renderer, rightSurface);
    SDL_Rect rightRect = {windowWidth - rightSurface->w - 10, windowHeight - 30 + (30 - rightSurface->h) / 2, rightSurface->w, rightSurface->h};
    SDL_RenderCopy(renderer, rightTexture, NULL, &rightRect);

    // Free resources
    SDL_FreeSurface(leftSurface);
    SDL_DestroyTexture(leftTexture);
    SDL_FreeSurface(rightSurface);
    SDL_DestroyTexture(rightTexture);
}

void renderRewindEffect(SDL_Renderer* renderer, double playbackRate, double currentTime, double totalDuration, int fps, const SDL_Rect* videoRect = nullptr) {
    int windowWidth, windowHeight;
    SDL_GetRendererOutputSize(renderer, &windowWidth, &windowHeight);

    // Check if we need to render the effect
    if (playbackRate < 0.2 || (playbackRate > 0.9 && playbackRate < 1.2)) return;

    // Check if we're not at the beginning or end of the file
    const double threshold = 0.1;
    if (currentTime < threshold || (totalDuration - currentTime) < threshold) return;

    // Use video dimensions if available, otherwise use window dimensions
    int effectWidth = videoRect ? videoRect->w : windowWidth;
    int effectHeight = videoRect ? videoRect->h : windowHeight;
    int effectX = videoRect ? videoRect->x : 0;
    int effectY = videoRect ? videoRect->y : 0;

    // Base parameters scaled to video height instead of window height
    const double resolutionScale = static_cast<double>(effectHeight) / 1080.0;
    const int maxStripeHeight = static_cast<int>(720 * resolutionScale);
    const int baseStripeHeight = static_cast<int>(85 * resolutionScale);
    const int baseStripeSpacing = static_cast<int>(450 * resolutionScale);
    const int minStripeSpacing = static_cast<int>(48 * resolutionScale);
    const int minStripeHeight = static_cast<int>(6 * resolutionScale);
    const int midStripeHeight = static_cast<int>(50 * resolutionScale);

// Calculate stripe parameters based on speed
    int stripeHeight, stripeSpacing;
    
    if ((playbackRate >= 0.2 && playbackRate < 0.9) || (playbackRate >= 1.2 && playbackRate < 2.0)) {
        // Эффект перекрытия как для замедленного, так и для ускоренного воспроизведения
        double t;
        if (playbackRate < 0.9) {
            // Для замедленного воспроизведения
            t = (playbackRate - 0.2) / 0.7; // 0.7 = 0.9 - 0.2
        } else {
            // Для ускоренного воспроизведения
            t = (playbackRate - 1.2) / 0.8; // 0.8 = 2.0 - 1.2
        }
        t = t * t * (3 - 2 * t); // Сглаживание для обоих случаев
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

    // Calculate number of stripes based on spacing
    int stripeCount = (windowHeight + stripeSpacing) / stripeSpacing;

    // Completely reworked cycle progress calculation
    double cycleProgress;
    const double baseDuration = 1.5;

if (playbackRate >= 14.0) {
    // Ускоряем движение для высоких скоростей
    double speedFactor = std::pow(playbackRate/14.0, 1.2) * 4.02; // Увеличили множитель с 2.5 до 4.0
    
    // Добавляем дополнительное ускорение после 16x
    if (playbackRate >= 16.0) {
        speedFactor *= 1.0 + (playbackRate - 16.0) * 0.417; // Ещё больше ускоряем для 16x-18x
    }
    
    double highSpeedCycleDuration = 1.0 / (fps * speedFactor);
    cycleProgress = std::fmod(currentTime, highSpeedCycleDuration) / highSpeedCycleDuration;
} else if (playbackRate >= 12.0) {
        // Переходная зона (12x-14x) - подготовка к ускорению
        double t = (playbackRate - 12.0) / 2.0;
        double speedMultiplier = 0.4 + t * 0.6;
        double transitionDuration = baseDuration / (playbackRate * speedMultiplier);
        cycleProgress = std::fmod(currentTime, transitionDuration) / transitionDuration;
} else if (playbackRate >= 3.5) {
    // Средние скорости (3.5x-12x) - очень медленное увеличение скорости
    double normalizedSpeed = (playbackRate - 3.5) / 8.5; // 0 to 1 в диапазоне 3.5x-12x
    
    // Уменьшаем базовый множитель и делаем более медленную прогрессию
    double speedMultiplier = 0.08 + (std::pow(normalizedSpeed, 3) * 0.15); // Кубическая прогрессия с меньшими значениями
    
    // Дополнительное замедление для нижней части диапазона
    if (playbackRate < 8.0) {
        speedMultiplier *= 0.7; // Замедляем еще больше в начале диапазона
    }
    
    double mediumSpeedDuration = baseDuration / (playbackRate * speedMultiplier);
    cycleProgress = std::fmod(currentTime, mediumSpeedDuration) / mediumSpeedDuration;
} else {
        // Низкие скорости (до 3.5x)
        double speedFactor = std::min(playbackRate / 2.0, 2.0);
        double adjustedDuration = baseDuration / speedFactor;
        cycleProgress = std::fmod(currentTime, adjustedDuration) / adjustedDuration;
    }

    // Enable alpha blending
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer, 128, 128, 128, 255);

    // Create a clip rectangle for the video area
    if (videoRect) {
        SDL_Rect clipRect = *videoRect;
        SDL_RenderSetClipRect(renderer, &clipRect);
    }

    // Render stripes
    for (int i = 0; i < stripeCount; ++i) {
        double baseOffset = cycleProgress * stripeSpacing + i * stripeSpacing;
        
        // Упрощаем вариацию высоты
        double heightVariation = (rand() % 21 - 10) * resolutionScale;
        if (playbackRate >= 14.0) {
            heightVariation = 0; // Убираем вариацию на высоких скоростях для стабильности
        }
        
        int currentStripeHeight = static_cast<int>(stripeHeight + heightVariation);
        currentStripeHeight = std::max(currentStripeHeight, minStripeHeight);

        // Calculate position relative to video area
        double stripeOffset = std::fmod(baseOffset, effectHeight + stripeSpacing);
        int y = effectY + static_cast<int>(stripeOffset) - currentStripeHeight;


        if (y < effectY + effectHeight && y + currentStripeHeight > effectY) {
            // Простой однопроходный рендеринг непрозрачных полос
            SDL_Rect stripeRect = {effectX, y, effectWidth, currentStripeHeight};
            SDL_RenderFillRect(renderer, &stripeRect);
        
            // Для эффекта снега включаем альфа-блендинг
            if (currentStripeHeight < effectHeight) {
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                double snowMultiplier = std::min(2.5, std::abs(playbackRate) / 8.0);
                int snowCount = static_cast<int>(effectWidth / (150.0 / snowMultiplier));
                
                for (int j = 0; j < snowCount; ++j) {
                    int snowX = effectX + (rand() % effectWidth);
                    int snowHeight = (playbackRate >= 14.0) ? 1 : 2;
                    int blurLength = static_cast<int>(15 + (rand() % 16) * (1.0 + std::abs(playbackRate) * 0.05));
                    
                    for (int w = 0; w < blurLength; ++w) {
                        double intensity;
                        if (w == 0) {
                            intensity = 1.0;
                        } else {
                            double blurProgress = static_cast<double>(w) / blurLength;
                            intensity = 1.0 - pow(blurProgress, 0.35);
                        }
                        
                        int snowAlpha = static_cast<int>(255 * intensity);
                        int grayValue = 128 + static_cast<int>(127 * intensity);
                        
                        SDL_SetRenderDrawColor(renderer, grayValue, grayValue, grayValue, snowAlpha);
                        SDL_Rect pixelRect = {snowX + w, y, 1, snowHeight};
                        if (pixelRect.y >= effectY && pixelRect.y < effectY + effectHeight && 
                            pixelRect.x >= effectX && pixelRect.x < effectX + effectWidth) {
                            SDL_RenderFillRect(renderer, &pixelRect);
                        }
                    }
                }
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
            }
        }
    }

    // Reset clip rectangle
    if (videoRect) {
        SDL_RenderSetClipRect(renderer, NULL);
    }
}

void displayFrame(SDL_Renderer* renderer, const std::vector<FrameInfo>& frameIndex, int newCurrentFrame, bool enableHighResDecode, double playbackRate, double currentTime, double totalDuration, bool showIndex, bool showOSD, TTF_Font* font, bool isPlaying, bool isReverse, bool waitingForTimecode, const std::string& inputTimecode, double originalFps, bool jogForward, bool jogBackward, size_t ringBufferCapacity, int highResWindowSize) {
    // чистка всего экрана черным цветом
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);

    // В начале функции после получения размеров окна
    int windowWidth, windowHeight;
    SDL_GetRendererOutputSize(renderer, &windowWidth, &windowHeight);

    // Сохраняем размеры видео в статических переменных
    static int lastVideoWidth = 0;
    static int lastVideoHeight = 0;
    static float lastAspectRatio = 16.0f/9.0f; // По умолчанию 16:9

    SDL_Rect videoRect = {0, 0, windowWidth, windowHeight}; // Инициализация

    // Получаем текущий кадр и обновляем размеры, если он есть
    AVFrame* frame = nullptr;
    if (frameIndex[newCurrentFrame].type != FrameInfo::EMPTY) {
        if (frameIndex[newCurrentFrame].type == FrameInfo::FULL_RES && frameIndex[newCurrentFrame].frame) {
            frame = frameIndex[newCurrentFrame].frame.get();
        } else if (frameIndex[newCurrentFrame].type == FrameInfo::LOW_RES && frameIndex[newCurrentFrame].low_res_frame) {
            frame = frameIndex[newCurrentFrame].low_res_frame.get();
        }
    }

    // Обновляем сохраненные размеры, если есть кадр
    if (frame) {
        lastVideoWidth = frame->width;
        lastVideoHeight = frame->height;
        lastAspectRatio = static_cast<float>(frame->width) / frame->height;
    }

    // Вычисляем размеры видео, используя сохраненное соотношение сторон
    if (windowWidth / lastAspectRatio <= windowHeight) {
        videoRect.w = windowWidth;
        videoRect.h = static_cast<int>(windowWidth / lastAspectRatio);
    } else {
        videoRect.h = windowHeight;
        videoRect.w = static_cast<int>(windowHeight * lastAspectRatio);
    }
    videoRect.x = (windowWidth - videoRect.w) / 2;
    videoRect.y = (windowHeight - videoRect.h) / 2;

    // Отображаем текущий кадр
    if (frameIndex[newCurrentFrame].type != FrameInfo::EMPTY) {
        if ((frameIndex[newCurrentFrame].type == FrameInfo::FULL_RES && frameIndex[newCurrentFrame].frame) ||
            (frameIndex[newCurrentFrame].type == FrameInfo::LOW_RES && frameIndex[newCurrentFrame].low_res_frame)) {
            displayCurrentFrame(renderer, frameIndex[newCurrentFrame], enableHighResDecode, std::abs(playbackRate), currentTime, totalDuration);
        } else {
            // Если кадр помечен как не пустой, но данные отсутствуют, отображаем заглушку
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderFillRect(renderer, NULL);
        }
    } else {
        // Если кадр пустой, отображаем заглушку
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderFillRect(renderer, NULL);
    }

    double effectPlaybackRate = playbackRate;
    if (std::abs(effectPlaybackRate) >= 1.1) {
        renderRewindEffect(renderer, effectPlaybackRate, currentTime, totalDuration, static_cast<int>(originalFps), &videoRect);
    }

    // Update frame index visualization
    if (showIndex) {
        int bufferStart = std::max(0, newCurrentFrame - static_cast<int>(ringBufferCapacity / 2));
        int bufferEnd = std::min(bufferStart + static_cast<int>(ringBufferCapacity) - 1, static_cast<int>(frameIndex.size()) - 1);
        int highResStart = std::max(0, newCurrentFrame - highResWindowSize / 2);
        int highResEnd = std::min(static_cast<int>(frameIndex.size()) - 1, newCurrentFrame + highResWindowSize / 2);
        updateVisualization(renderer, frameIndex, newCurrentFrame, bufferStart, bufferEnd, highResStart, highResEnd, enableHighResDecode);
    }

    // Render OSD
    if (showOSD) {
        renderOSD(renderer, font, isPlaying, playbackRate, isReverse, currentTime, newCurrentFrame, showOSD, waitingForTimecode, inputTimecode, originalFps, jogForward, jogBackward);
    }
}