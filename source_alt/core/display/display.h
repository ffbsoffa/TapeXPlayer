#pragma once

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libavutil/pixfmt.h>
}

#include "../decode/decode.h"
#include "../main/initmanager.h"
#include <vector>
#include <atomic>
#include <string>

void get_video_dimensions(const char* filename, int* width, int* height);

void updateVisualization(SDL_Renderer* renderer, const std::vector<FrameInfo>& frameIndex, int currentFrame, int bufferStart, int bufferEnd, int highResStart, int highResEnd, bool enableHighResDecode);

void displayCurrentFrame(SDL_Renderer* renderer, const FrameInfo& frameInfo, bool enableHighResDecode, double playbackRate, double currentTime, double totalDuration);

void renderOSD(SDL_Renderer* renderer, TTF_Font* font, bool isPlaying, double playbackRate, bool isReverse, double currentTime, int frameNumber, bool showOSD = true, bool waiting_for_timecode = false, const std::string& input_timecode = "", double original_fps = 25.0, bool jog_forward = false, bool jog_backward = false);

void displayFrame(
    SDL_Renderer* renderer,
    const std::vector<FrameInfo>& frameIndex,
    int newCurrentFrame,
    std::shared_ptr<AVFrame> frameToDisplay,
    FrameInfo::FrameType frameTypeToDisplay,
    bool enableHighResDecode,
    double playbackRate,
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
);

void renderLoadingScreen(SDL_Renderer* renderer, TTF_Font* font, const LoadingStatus& status);

void cleanupDisplayResources();

void renderZoomedFrame(SDL_Renderer* renderer, SDL_Texture* texture, int frameWidth, int frameHeight, float zoomFactor, float centerX, float centerY);

void renderZoomThumbnail(SDL_Renderer* renderer, SDL_Texture* texture, int frameWidth, int frameHeight, float zoomFactor, float centerX, float centerY);

void handleZoomMouseEvent(SDL_Event& event, int windowWidth, int windowHeight, int frameWidth, int frameHeight);

static AVPixelFormat av_pix_fmt_from_sdl_format(SDL_PixelFormatEnum sdlFormat);

// Getters for last rendered texture dimensions
int get_last_texture_width();
int get_last_texture_height();
