#include "FSTPAVFramePool.h"
#include <iostream>

namespace FSTP {

AVFramePool::AVFramePool(int width, int height, AVPixelFormat format, int initial_size)
    : width_(width)
    , height_(height)
    , format_(format)
    , total_allocated_(0) {

    // Pre-allocate initial frames
    for (int i = 0; i < initial_size; ++i) {
        AVFrame* frame = CreateFrame();
        if (frame) {
            available_frames_.push_back(frame);
        }
    }

    std::cout << "🏊 [FRAME POOL] Created with " << available_frames_.size()
              << " frames (" << width << "x" << height << ")" << std::endl;
}

AVFramePool::~AVFramePool() {
    std::lock_guard<std::mutex> lock(mutex_);

    // Free all available frames
    for (AVFrame* frame : available_frames_) {
        av_frame_free(&frame);
    }

    std::cout << "🏊 [FRAME POOL] Destroyed, peak allocation: " << total_allocated_ << " frames" << std::endl;
}

AVFrame* AVFramePool::CreateFrame() {
    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        std::cerr << "❌ [FRAME POOL] Failed to allocate AVFrame" << std::endl;
        return nullptr;
    }

    // Set frame parameters
    frame->width = width_;
    frame->height = height_;
    frame->format = format_;

    // Allocate buffer for frame data
    int ret = av_frame_get_buffer(frame, 64);  // 64-byte alignment for SIMD
    if (ret < 0) {
        std::cerr << "❌ [FRAME POOL] Failed to allocate frame buffer" << std::endl;
        av_frame_free(&frame);
        return nullptr;
    }

    total_allocated_++;
    return frame;
}

AVFrame* AVFramePool::Acquire() {
    std::lock_guard<std::mutex> lock(mutex_);

    AVFrame* frame = nullptr;

    if (!available_frames_.empty()) {
        // Reuse frame from pool
        frame = available_frames_.back();
        available_frames_.pop_back();

        // Restore frame properties (cleared by av_frame_unref in Release)
        frame->width = width_;
        frame->height = height_;
        frame->format = format_;

        // Re-allocate buffer if Release()'s av_frame_unref freed it
        if (!frame->data[0]) {
            int ret = av_frame_get_buffer(frame, 64);
            if (ret < 0) {
                std::cerr << "❌ [FRAME POOL] Failed to re-allocate frame buffer" << std::endl;
                av_frame_free(&frame);
                return nullptr;
            }
        }

    } else {
        // Pool empty - create new frame
        frame = CreateFrame();

        static int expansion_log = 0;
        if (expansion_log++ < 5) {
            std::cout << "🏊 [FRAME POOL] Expanded to " << total_allocated_
                      << " frames (high demand)" << std::endl;
        }
    }

    return frame;
}

void AVFramePool::Release(AVFrame* frame) {
    if (!frame) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // Clear frame data (but keep buffer for reuse)
    av_frame_unref(frame);

    // Return to pool
    available_frames_.push_back(frame);
}

size_t AVFramePool::GetAvailableCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return available_frames_.size();
}

size_t AVFramePool::GetTotalCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return total_allocated_;
}

} // namespace FSTP
