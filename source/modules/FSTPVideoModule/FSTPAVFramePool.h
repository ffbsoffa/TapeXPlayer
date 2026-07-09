#pragma once

#include <vector>
#include <mutex>
#include <memory>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
}

namespace FSTP {

/**
 * AVFramePool - Memory pool for AVFrame reuse
 *
 * OPTIMIZATION: Avoid av_frame_clone() overhead (345 KB copy per frame)
 * For 600-frame segment: 207 MB → ~0 MB copying!
 */
class AVFramePool {
public:
    AVFramePool(int width, int height, AVPixelFormat format, int initial_size = 120);
    ~AVFramePool();

    AVFramePool(const AVFramePool&) = delete;
    AVFramePool& operator=(const AVFramePool&) = delete;

    AVFrame* Acquire();
    void Release(AVFrame* frame);

    size_t GetAvailableCount() const;
    size_t GetTotalCount() const;

private:
    int width_;
    int height_;
    AVPixelFormat format_;
    std::vector<AVFrame*> available_frames_;
    size_t total_allocated_;
    mutable std::mutex mutex_;

    AVFrame* CreateFrame();
};

// RAII wrapper for automatic return to pool
class PooledFrameDeleter {
public:
    explicit PooledFrameDeleter(AVFramePool* pool) : pool_(pool) {}
    void operator()(AVFrame* frame) const {
        if (frame && pool_) {
            pool_->Release(frame);
        }
    }
private:
    AVFramePool* pool_;
};

inline std::shared_ptr<AVFrame> MakePooledFrame(AVFramePool* pool) {
    AVFrame* frame = pool->Acquire();
    return std::shared_ptr<AVFrame>(frame, PooledFrameDeleter(pool));
}

} // namespace FSTP
