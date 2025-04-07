#pragma once

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include "decode.h"
#include <vector>
#include <atomic>
#include <string>

void get_video_dimensions(const char* filename, int* width, int* height);

void updateVisualization(SDL_Renderer* renderer, const std::vector<FrameInfo>& frameIndex, int currentFrame, int bufferStart, int bufferEnd, int highResStart, int highResEnd, bool enableHighResDecode);

void displayCurrentFrame(SDL_Renderer* renderer, const FrameInfo& frameInfo, bool enableHighResDecode, double playbackRate, double currentTime, double totalDuration);

void renderOSD(SDL_Renderer* renderer, TTF_Font* font, bool isPlaying, double playbackRate, bool isReverse, double currentTime, int frameNumber, bool showOSD = true, bool waiting_for_timecode = false, const std::string& input_timecode = "", double original_fps = 25.0, bool jog_forward = false, bool jog_backward = false, bool isLoading = false, const std::string& loadingType = "", int loadingProgress = 0);

void displayFrame(SDL_Renderer* renderer, const std::vector<FrameInfo>& frameIndex, int newCurrentFrame, bool enableHighResDecode, double playbackRate, double currentTime, double totalDuration, bool showIndex, bool showOSD, TTF_Font* font, bool isPlaying, bool isReverse, bool waitingForTimecode, const std::string& inputTimecode, double originalFps, bool jogForward, bool jogBackward, size_t ringBufferCapacity, int highResWindowSize);

void renderLoadingScreen(SDL_Renderer* renderer, TTF_Font* font, const std::string& loadingType, int loadingProgress);

void cleanupDisplayResources();

void renderZoomedFrame(SDL_Renderer* renderer, SDL_Texture* texture, int frameWidth, int frameHeight, float zoomFactor, float centerX, float centerY);

void renderZoomThumbnail(SDL_Renderer* renderer, SDL_Texture* texture, int frameWidth, int frameHeight, float zoomFactor, float centerX, float centerY);

void handleZoomMouseEvent(SDL_Event& event, int windowWidth, int windowHeight, int frameWidth, int frameHeight);
