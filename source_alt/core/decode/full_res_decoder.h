#include <vector>
#include <memory>
#include <atomic>
#include <future>
#include <string>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libavutil/hwcontext.h> // Added for HW context
#include <libavutil/hwcontext_videotoolbox.h> // Added for VideoToolbox specifics
}

#include "decode.h"

// Forward declarations
// struct FrameInfo;

class FullResDecoder {
public:
    // Constructor takes the original video filename
    FullResDecoder(const std::string& sourceFilename);
    ~FullResDecoder();

    // --- Instance Methods ---
    // Decode full-res frames in a specific range
    bool decodeFrameRange(std::vector<FrameInfo>& frameIndex, int startFrame, int endFrame);

    // Getters for decoder properties
    bool isInitialized() const;
    bool isHardwareAccelerated() const; // Added getter
    int getWidth() const;
    int getHeight() const;
    AVPixelFormat getPixelFormat() const; // Returns context pix fmt

    // --- Static Utility Methods ---
    static void removeHighResFrames(std::vector<FrameInfo>& frameIndex,
                                  int start, int end,
                                  int highResStart, int highResEnd);
    static void clearHighResFrames(std::vector<FrameInfo>& frameIndex);
    static bool shouldProcessFrame(const FrameInfo& frame);

    void requestStop();

private:
    // Initialization and cleanup
    bool initialize();
    void cleanup();
    static enum AVPixelFormat get_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts); // Moved from test

    // Member variables
    std::string sourceFilename_;
    bool initialized_ = false; // Flag for successful initialization
    int width_ = 0;
    int height_ = 0;
    AVPixelFormat pixFmt_ = AV_PIX_FMT_NONE; // Actual pixel format of the opened context

    // FFmpeg context members
    AVFormatContext* formatCtx_ = nullptr;
    AVCodecContext* codecCtx_ = nullptr;
    AVCodecParameters* codecParams_ = nullptr;
    AVStream* videoStream_ = nullptr;
    SwsContext* swsCtx_ = nullptr; // May still be needed for SW fallback or specific conversions
    int videoStreamIndex_ = -1;

    // Hardware Acceleration Members
    bool hw_accel_enabled_ = false;
    AVBufferRef* hw_device_ctx_ = nullptr; // Holds the ref during init attempt
    AVPixelFormat hw_pix_fmt_ = AV_PIX_FMT_NONE; // Will be AV_PIX_FMT_VIDEOTOOLBOX if enabled

    std::atomic<bool> stop_requested_{false};

};