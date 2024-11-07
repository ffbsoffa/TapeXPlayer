#pragma once

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include "decode.h"
#include <vector>
#include <atomic>
#include <string>

void get_video_dimensions(const char* filename, int* width, int* height);

void updateVisualization(SDL_Renderer* renderer, const std::vector<FrameInfo>& frameIndex, int currentFrame, int bufferStart, int bufferEnd, int highResStart, int highResEnd, bool enableHighResDecode);

void displayCurrentFrame(SDL_Renderer* renderer, const FrameInfo& frameInfo, bool enableHighResDecode, double playbackRate, double currentTime, double totalDuration);

void renderOSD(SDL_Renderer* renderer, TTF_Font* font, bool isPlaying, double playbackRate, bool isReverse, double currentTime, int frameNumber, bool showOSD = true, bool waiting_for_timecode = false, const std::string& input_timecode = "", double original_fps = 25.0, bool jog_forward = false, bool jog_backward = false);

void renderRewindEffect(SDL_Renderer* renderer, double playbackRate, double currentTime, double totalDuration);

void displayFrame(SDL_Renderer* renderer, const std::vector<FrameInfo>& frameIndex, int newCurrentFrame, bool enableHighResDecode, double playbackRate, double currentTime, double totalDuration, bool showIndex, bool showOSD, TTF_Font* font, bool isPlaying, bool isReverse, bool waitingForTimecode, const std::string& inputTimecode, double originalFps, bool jogForward, bool jogBackward, size_t ringBufferCapacity, int highResWindowSize);
