#pragma once

#include <string>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

// Function to take a screenshot of the current frame with timecode overlay
bool takeScreenshotWithTimecode(
    void* renderer, // Changed from SDL_Renderer* to void* to avoid SDL dependency in header
    AVFrame* frame,
    const std::string& timecode,
    const std::string& outputPath,
    int windowWidth,
    int windowHeight
);

// Advanced function to take a screenshot with zoom and thumbnail support
bool takeAdvancedScreenshotWithTimecode(
    AVFrame* frame,
    const std::string& timecode,
    int windowWidth,
    int windowHeight,
    bool isZoomEnabled,
    float zoomFactor,
    float zoomCenterX,
    float zoomCenterY,
    bool showThumbnail
);

// Function to render bitmap text overlay on AVFrame using built-in bitmap font
bool renderTimecodeOnFrame(
    AVFrame* frame,
    const std::string& timecode,
    int x,
    int y
);

// Function to save AVFrame as PNG with timecode overlay
bool saveFrameAsPNGWithTimecode(
    AVFrame* frame,
    const std::string& timecode,
    const std::string& outputPath
);

// Generate filename with timestamp (PNG format)
std::string generateScreenshotFilename(const std::string& timecode); 