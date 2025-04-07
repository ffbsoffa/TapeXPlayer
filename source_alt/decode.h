#pragma once

#include <vector>
#include <memory>
#include <atomic>
#include <future>
#include <chrono>
#include <mutex>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

// External atomic variables
extern std::atomic<bool> shouldExit;
extern std::atomic<double> current_audio_time;
extern std::atomic<double> playback_rate;
extern std::atomic<double> original_fps;

// Frame information structure
struct FrameInfo {
    int64_t pts;
    int64_t relative_pts;
    int64_t time_ms;
    std::shared_ptr<AVFrame> frame;
    std::shared_ptr<AVFrame> low_res_frame;
    std::shared_ptr<AVFrame> cached_frame;
    enum FrameType {
        EMPTY,
        LOW_RES,
        FULL_RES,
        CACHED
    } type;
    bool is_decoding;
    mutable std::mutex mutex;
    AVRational time_base;

    FrameInfo() : pts(0), relative_pts(0), time_ms(0), type(EMPTY), is_decoding(false) {}
    
    FrameInfo(const FrameInfo& other) 
        : pts(other.pts), relative_pts(other.relative_pts), time_ms(other.time_ms),
          frame(other.frame), low_res_frame(other.low_res_frame), cached_frame(other.cached_frame), type(other.type) {
        std::lock_guard<std::mutex> lock(other.mutex);
        is_decoding = other.is_decoding;
    }
    
    FrameInfo& operator=(const FrameInfo& other) {
        if (this != &other) {
            std::lock_guard<std::mutex> lock1(mutex);
            std::lock_guard<std::mutex> lock2(other.mutex);
            pts = other.pts;
            relative_pts = other.relative_pts;
            time_ms = other.time_ms;
            frame = other.frame;
            low_res_frame = other.low_res_frame;
            cached_frame = other.cached_frame;
            type = other.type;
            is_decoding = other.is_decoding;
        }
        return *this;
    }
};

// Intermediate buffer for frame transfer between decoder and display
struct FrameBuffer {
    std::shared_ptr<AVFrame> lastFrame;
    int frameIndex;
    FrameInfo::FrameType frameType;
    AVRational timeBase;
    std::mutex mutex;

    FrameBuffer() : frameIndex(-1), frameType(FrameInfo::EMPTY) {}

    void updateFrame(const std::shared_ptr<AVFrame>& newFrame, int index, FrameInfo::FrameType type, const AVRational& tb) {
        std::lock_guard<std::mutex> lock(mutex);
        lastFrame = newFrame;
        frameIndex = index;
        frameType = type;
        timeBase = tb;
    }

    std::shared_ptr<AVFrame> getFrame(int& outIndex, FrameInfo::FrameType& outType, AVRational& outTimeBase) {
        std::lock_guard<std::mutex> lock(mutex);
        outIndex = frameIndex;
        outType = frameType;
        outTimeBase = timeBase;
        return lastFrame;
    }
};

// Global frame index
extern std::vector<FrameInfo> frameIndex;
extern std::vector<FrameInfo> globalFrameIndex;
extern FrameBuffer frameBuffer; // Global intermediate buffer

// Ring buffer for frame management
struct RingBuffer {
    std::vector<FrameInfo> buffer;
    size_t capacity;
    size_t start;
    size_t size;
    size_t playhead;

    RingBuffer(size_t cap);
    void push(const FrameInfo& frame);
    FrameInfo& at(size_t index);
    size_t getStart() const;
    size_t getSize() const;
    size_t getPlayheadPosition() const;
    void movePlayhead(int delta);
};

// Frame cleanup management
class FrameCleaner {
private:
    std::vector<FrameInfo>& frameIndex;

public:
    FrameCleaner(std::vector<FrameInfo>& fi);
    void cleanFrames(int startFrame, int endFrame);
};

// Define a progress callback type
typedef void (*ProgressCallback)(int progress);

// Frame index and decoding functions
std::vector<FrameInfo> createFrameIndex(const char* filename);
bool convertToLowRes(const char* filename, std::string& outputFilename, ProgressCallback progressCallback = nullptr);
bool fillIndexWithLowResFrames(const char* filename, std::vector<FrameInfo>& frameIndex);
bool decodeFrameRange(const char* filename, std::vector<FrameInfo>& frameIndex, int startFrame, int endFrame);
bool decodeLowResRange(const char* filename, std::vector<FrameInfo>& frameIndex, int startFrame, int endFrame, int highResStart, int highResEnd, bool skipHighResWindow = true);
bool decodeCachedFrames(const char* filename, std::vector<FrameInfo>& frameIndex, int step = 10);

// Initialize buffer with progress reporting
void initializeBuffer(const char* lowResFilename, std::vector<FrameInfo>& frameIndex, int currentFrame, int bufferSize, ProgressCallback progressCallback = nullptr);

// Asynchronous operations
std::future<void> asyncDecodeLowResRange(const char* filename, std::vector<FrameInfo>& frameIndex, int startFrame, int endFrame, int highResStart, int highResEnd, bool skipHighResWindow = true);
std::future<void> asyncDecodeFrameRange(const char* filename, std::vector<FrameInfo>& frameIndex, int startFrame, int endFrame);
std::future<void> asyncCleanFrames(FrameCleaner& cleaner, int startFrame, int endFrame);
std::future<void> asyncDecodeCachedFrames(const char* filename, std::vector<FrameInfo>& frameIndex, int step = 10);

// Memory management
void printMemoryUsage();
void clearHighResFrames(std::vector<FrameInfo>& frameIndex);
void removeHighResFrames(std::vector<FrameInfo>& frameIndex, int start, int end, int highResStart, int highResEnd);

// Video decoding management
void manageVideoDecoding(const std::string& filename, const std::string& lowResFilename, 
                        std::vector<FrameInfo>& frameIndex, std::atomic<int>& currentFrame,
                        const size_t ringBufferCapacity, const int highResWindowSize,
                        std::atomic<bool>& isPlaying);

// New functions
bool isURL(const std::string& str);
bool downloadVideoFromURL(const std::string& url, std::string& outputFilename);
void registerTempFileForCleanup(const std::string& filePath);
void cleanupTempFiles();
bool processMediaSource(const std::string& source, std::string& processedFilePath);
std::string generateURLId(const std::string& url);
