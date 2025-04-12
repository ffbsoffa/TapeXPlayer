#pragma once

#include <vector>
#include <memory>
#include <atomic>
#include <future>
#include <chrono>
#include <mutex>
// #include "full_res_decoder.h" // Removed includes to break circular dependency
// #include "low_res_decoder.h"
// #include "cached_decoder.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libavutil/rational.h> // For AVRational
#include <libavutil/pixfmt.h>   // For AVPixelFormat
}

// External atomic variables
extern std::atomic<bool> shouldExit;
extern std::atomic<double> current_audio_time;
extern std::atomic<double> playback_rate;
extern std::atomic<double> original_fps;

// Frame information structure
struct FrameInfo {
    enum FrameType {
        EMPTY,
        LOW_RES,
        CACHED, // Keep CACHED concept?
        FULL_RES
    };

    std::shared_ptr<AVFrame> frame; // Holds full-res frame (HW or SW)
    std::shared_ptr<AVFrame> low_res_frame;
    std::shared_ptr<AVFrame> cached_frame; // Or maybe remove this?

    FrameType type = EMPTY;
    AVPixelFormat format = AV_PIX_FMT_NONE; // <<< Added: Actual format of the stored frame (in 'frame' or 'low_res_frame')
    int64_t pts = AV_NOPTS_VALUE; // Presentation timestamp
    int64_t relative_pts = AV_NOPTS_VALUE; // PTS relative to stream start time
    AVRational time_base = {0, 1}; // Time base of the PTS
    double time_ms = -1.0;         // Frame time in milliseconds

    std::atomic<bool> is_decoding{false};
    std::atomic<bool> is_ready{false};    // Is frame ready for display?

    // Use std::mutex for exclusive access when modifying frame data
    mutable std::mutex mutex;

    // Default constructor
    FrameInfo() = default;

    // Copy constructor (needed for vector resizing, handle atomics and mutex correctly)
    FrameInfo(const FrameInfo& other) :
        frame(other.frame),
        low_res_frame(other.low_res_frame),
        cached_frame(other.cached_frame),
        type(other.type),
        format(other.format),
        pts(other.pts),
        relative_pts(other.relative_pts),
        time_base(other.time_base),
        time_ms(other.time_ms),
        is_decoding(other.is_decoding.load()), // Copy atomic value
        is_ready(other.is_ready.load())        // Copy atomic value
    {
        // Mutex is not copied, each instance gets its own
    }

    // Copy assignment operator (handle atomics and mutex correctly)
    FrameInfo& operator=(const FrameInfo& other) {
        if (this == &other) {
            return *this;
        }
        frame = other.frame;
        low_res_frame = other.low_res_frame;
        cached_frame = other.cached_frame;
        type = other.type;
        format = other.format;
        pts = other.pts;
        relative_pts = other.relative_pts;
        time_base = other.time_base;
        time_ms = other.time_ms;
        is_decoding.store(other.is_decoding.load()); // Assign atomic value
        is_ready.store(other.is_ready.load());       // Assign atomic value
        // Mutex is not assigned
        return *this;
    }

    // Move constructor (handle atomics and mutex correctly)
    FrameInfo(FrameInfo&& other) noexcept :
        frame(std::move(other.frame)),
        low_res_frame(std::move(other.low_res_frame)),
        cached_frame(std::move(other.cached_frame)),
        type(other.type),
        format(other.format),
        pts(other.pts),
        relative_pts(other.relative_pts),
        time_base(other.time_base),
        time_ms(other.time_ms),
        is_decoding(other.is_decoding.load()), // Move atomic value (by loading)
        is_ready(other.is_ready.load())        // Move atomic value (by loading)
    {
        // Mutex is not moved
        other.type = EMPTY;
        other.format = AV_PIX_FMT_NONE;
        other.pts = AV_NOPTS_VALUE;
        other.relative_pts = AV_NOPTS_VALUE;
        other.time_ms = -1.0;
    }

    // Move assignment operator (handle atomics and mutex correctly)
    FrameInfo& operator=(FrameInfo&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        frame = std::move(other.frame);
        low_res_frame = std::move(other.low_res_frame);
        cached_frame = std::move(other.cached_frame);
        type = other.type;
        format = other.format;
        pts = other.pts;
        relative_pts = other.relative_pts;
        time_base = other.time_base;
        time_ms = other.time_ms;
        is_decoding.store(other.is_decoding.load()); // Assign moved atomic value
        is_ready.store(other.is_ready.load());       // Assign moved atomic value
        // Mutex is not assigned
        other.type = EMPTY;
        other.format = AV_PIX_FMT_NONE;
        other.pts = AV_NOPTS_VALUE;
        other.relative_pts = AV_NOPTS_VALUE;
        other.time_ms = -1.0;
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

// Asynchronous operations
std::future<void> asyncCleanFrames(FrameCleaner& cleaner, int startFrame, int endFrame);

// Memory management
void printMemoryUsage();

// Video decoding management - Updated signature
void manageVideoDecoding(const std::string& filename,
                        std::vector<FrameInfo>& frameIndex,
                        std::atomic<int>& currentFrame,
                        const size_t ringBufferCapacity,
                        const int highResWindowSize,
                        std::atomic<bool>& isPlaying);

// New functions
bool isURL(const std::string& str);
bool downloadVideoFromURL(const std::string& url, std::string& outputFilename);
void registerTempFileForCleanup(const std::string& filePath);
void cleanupTempFiles();
bool processMediaSource(const std::string& source, std::string& processedFilePath);
std::string generateURLId(const std::string& url);

// Function to get video dimensions
void get_video_dimensions(const char* filename, int* width, int* height);

// Function to get video frame rate
double get_video_fps(const char* filename);

// Function to get file duration
double get_file_duration(const char* filename);

// Helper function to parse timecode string (e.g., "01020304")
double parse_timecode(const std::string& timecode);

// Function to toggle pause state
void toggle_pause();

// Functions for volume control
void increase_volume();
void decrease_volume();

// Function to seek to a specific time
void seek_to_time(double time);

// Functions for jog control
void start_jog_forward();
void start_jog_backward();
void stop_jog();

// Helper function to find the frame index closest to a given timestamp
int findClosestFrameIndexByTime(const std::vector<FrameInfo>& frameIndex, int64_t target_ms);
