#pragma once

#include <vector>
#include <memory>
#include <atomic>
#include <future>
#include <string>
#include <mutex>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

// Forward declaration
struct FrameInfo;

// Define a progress callback type
typedef void (*ProgressCallback)(int progress);

class CachedDecoder {
public:
    // Constructor takes filename and the frameIndex it will modify
    CachedDecoder(const std::string& filename, std::vector<FrameInfo>& frameIndex);
    ~CachedDecoder();

    // Decode frames within a specific range with adaptive step
    bool decodeRange(int startFrame, int endFrame);

    // Check if decoder is initialized
    bool isInitialized() const { return initialized_; }

    // Static method to get adaptive step based on FPS (remains static)
    static int getAdaptiveStep(double fps);

private:
    // Initialization and state
    bool initialize();
    void cleanup();
    bool initialized_;

    // FFmpeg context members
    std::string sourceFilename_;
    std::vector<FrameInfo>& frameIndex_; // Reference to the main frame index
    AVFormatContext* formatCtx_;
    AVCodecContext* codecCtx_;
    const AVCodecParameters* codecParams_; // Pointer to codec parameters
    AVStream* videoStream_;
    int videoStreamIndex_;
    AVRational timeBase_;
    double fps_;
    int adaptedStep_;

    // No SwsContext needed as we store AVFrame directly
}; 