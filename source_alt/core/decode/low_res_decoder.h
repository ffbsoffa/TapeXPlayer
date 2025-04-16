#ifndef LOW_RES_DECODER_H
#define LOW_RES_DECODER_H

#include <vector>
#include <memory>
#include <atomic>
#include <future>
#include <string>
#include <functional>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include "decode.h" // Includes FrameInfo definition

// Forward declaration
struct FrameInfo;

// Define a progress callback type
typedef void (*ProgressCallback)(int progress);

class LowResDecoder {
public:
    // Constructor takes the low-resolution video filename
    LowResDecoder(const std::string& lowResFilename);
    ~LowResDecoder(); // Destructor for cleanup

    // --- Static Utility Methods ---
    // Convert original video to low-res version
    static bool convertToLowRes(const std::string& filename, std::string& outputFilename, 
                              const std::function<void(int)>& progressCallback = nullptr);
    
    // String utilities for file handling
    static std::string getCachePath();
    static std::string generateFileId(const std::string& filename);
    
    // Function to remove low-res frames outside a given window
    static void removeLowResFrames(std::vector<FrameInfo>& frameIndex, int start, int end);

    // --- Instance Methods ---
    // Decode low-res frames in a specific range
    bool decodeLowResRange(std::vector<FrameInfo>& frameIndex, 
                           int startFrame, int endFrame, 
                           int highResStart, int highResEnd, 
                           bool skipHighResWindow = true);

    bool isInitialized() const;
    int getWidth() const;
    int getHeight() const;
    AVPixelFormat getPixelFormat() const;

    void requestStop();

private:
    // Initialization and cleanup
    bool initialize();
    void cleanup();

    // Member variables
    std::string lowResFilename_;
    AVFormatContext* formatCtx_ = nullptr;
    AVCodecContext* codecCtx_ = nullptr;
    AVCodecParameters* codecParams_ = nullptr;
    AVStream* videoStream_ = nullptr;
    SwsContext* swsCtx_ = nullptr;
    int videoStreamIndex_ = -1;
    bool initialized_ = false;
    int width_ = 0;
    int height_ = 0;
    AVPixelFormat pixFmt_ = AV_PIX_FMT_NONE;
    std::atomic<bool> stop_requested_{false};
};

#endif // LOW_RES_DECODER_H 