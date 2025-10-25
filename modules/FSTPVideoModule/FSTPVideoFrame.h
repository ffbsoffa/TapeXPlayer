#pragma once

#include <memory>
#include <atomic>
#include <mutex>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace FSTP {

// Structure for frame information (compatible with old system)
struct FrameInfo {
    enum FrameType {
        EMPTY,
        LOW_RES,
        CACHED,
        FULL_RES
    };

    std::shared_ptr<AVFrame> frame;         // Full resolution frame
    std::shared_ptr<AVFrame> low_res_frame; // Low resolution frame
    std::shared_ptr<AVFrame> cached_frame;  // Cached frame

    FrameType type = EMPTY;                 // Frame type
    AVPixelFormat format = AV_PIX_FMT_NONE; // Pixel format
    int64_t pts = AV_NOPTS_VALUE;           // Presentation timestamp
    int64_t relative_pts = AV_NOPTS_VALUE;  // PTS relative to stream start
    AVRational time_base = {0, 1};          // Time base for PTS
    double time_ms = -1.0;                  // Time in milliseconds
    bool is_keyframe = false;               // Key frame (for GOP-aware decoding)

    std::atomic<bool> is_decoding{false};   // Currently decoding
    std::atomic<bool> is_ready{false};      // Ready for display
    mutable std::mutex mutex;               // Mutex for frame protection

    // Constructors and operators
    FrameInfo() = default;
    
    // Copy constructor (needed for vector resizing, properly handle atomic variables and mutex)
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
        is_decoding(other.is_decoding.load()),
        is_ready(other.is_ready.load())
    {
        // Mutex is not copied, each instance gets its own
    }

    // Copy assignment operator
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
        is_decoding.store(other.is_decoding.load());
        is_ready.store(other.is_ready.load());
        return *this;
    }

    // Move constructor
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
        is_decoding(other.is_decoding.load()),
        is_ready(other.is_ready.load())
    {
        // Reset original object
        other.type = EMPTY;
        other.format = AV_PIX_FMT_NONE;
        other.pts = AV_NOPTS_VALUE;
        other.relative_pts = AV_NOPTS_VALUE;
        other.time_base = {0, 1};
        other.time_ms = -1.0;
        other.is_decoding.store(false);
        other.is_ready.store(false);
    }

    // Move assignment operator
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
        is_decoding.store(other.is_decoding.load());
        is_ready.store(other.is_ready.load());
        
        // Reset original object
        other.type = EMPTY;
        other.format = AV_PIX_FMT_NONE;
        other.pts = AV_NOPTS_VALUE;
        other.relative_pts = AV_NOPTS_VALUE;
        other.time_base = {0, 1};
        other.time_ms = -1.0;
        other.is_decoding.store(false);
        other.is_ready.store(false);
        
        return *this;
    }

    ~FrameInfo() = default;
};

// Buffer for frames
class FrameBuffer {
private:
    std::vector<FrameInfo> m_frames;
    mutable std::mutex m_mutex;

public:
    FrameBuffer() = default;
    ~FrameBuffer() = default;

    void Resize(size_t size);
    size_t Size() const;
    FrameInfo& GetFrame(size_t index);
    const FrameInfo& GetFrame(size_t index) const;
    void Clear();
};

// Frame cleaner
class FrameCleaner {
public:
    static void CleanFrames(std::vector<FrameInfo>& frames, int start, int end);
};

// Ring buffer
class RingBuffer {
private:
    std::vector<FrameInfo> m_buffer;
    size_t m_capacity;
    size_t m_start;
    size_t m_size;
    mutable std::mutex m_mutex;

public:
    explicit RingBuffer(size_t capacity);
    ~RingBuffer() = default;

    bool Push(const FrameInfo& frame);
    bool Pop(FrameInfo& frame);
    bool IsEmpty() const;
    bool IsFull() const;
    size_t Size() const;
    void Clear();
};

} // namespace FSTP