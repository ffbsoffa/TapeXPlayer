#ifndef FSTP_VIDEO_OPTIMIZATIONS_H
#define FSTP_VIDEO_OPTIMIZATIONS_H

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

// Class for caching and reusing SwsContext
class SwsContextCache {
public:
    static SwsContextCache& getInstance() {
        static SwsContextCache instance;
        return instance;
    }

    // Get or create SwsContext with caching
    SwsContext* getContext(int src_width, int src_height, AVPixelFormat src_format,
                          int dst_width, int dst_height, AVPixelFormat dst_format,
                          int flags = SWS_FAST_BILINEAR) {

        std::lock_guard<std::mutex> lock(mutex_);

        // Create cache key
        uint64_t key = createKey(src_width, src_height, src_format, dst_width, dst_height, dst_format, flags);

        // Check cache
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            return it->second.get();
        }

        // Create new context
        SwsContext* ctx = sws_getContext(
            src_width, src_height, src_format,
            dst_width, dst_height, dst_format,
            flags, nullptr, nullptr, nullptr
        );

        if (ctx) {
            // Add to cache
            cache_[key] = std::unique_ptr<SwsContext, SwsContextDeleter>(ctx);
            return ctx;
        }

        return nullptr;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        cache_.clear();
    }

private:
    SwsContextCache() = default;
    ~SwsContextCache() = default;

    uint64_t createKey(int sw, int sh, int sf, int dw, int dh, int df, int flags) {
        // Simple hash function for creating key
        uint64_t key = 0;
        key = (key * 31) ^ sw;
        key = (key * 31) ^ sh;
        key = (key * 31) ^ sf;
        key = (key * 31) ^ dw;
        key = (key * 31) ^ dh;
        key = (key * 31) ^ df;
        key = (key * 31) ^ flags;
        return key;
    }

    struct SwsContextDeleter {
        void operator()(SwsContext* ctx) const {
            if (ctx) sws_freeContext(ctx);
        }
    };

    std::mutex mutex_;
    std::unordered_map<uint64_t, std::unique_ptr<SwsContext, SwsContextDeleter>> cache_;
};

// Optimized flags for swscale depending on load
class SwsOptimizer {
public:
    static int getOptimalFlags(int active_decoders, int width, int height) {
        // Adaptive algorithm selection depending on load
        if (active_decoders >= 3) {
            // Maximum speed optimization for 3+ players
            return SWS_POINT;  // Nearest neighbor - fastest
        } else if (active_decoders >= 2) {
            // Medium balance for 2 players
            return SWS_FAST_BILINEAR;  // Fast bilinear interpolation
        } else if (width * height > 1920 * 1080) {
            // For 4K and above use faster algorithm
            return SWS_FAST_BILINEAR;
        } else {
            // Maximum quality for single player and HD
            return SWS_BILINEAR;  // Quality bilinear interpolation
        }
    }

    // Check if conversion is needed
    static bool needsConversion(AVPixelFormat format) {
        // Formats that can be used directly without conversion
        return format != AV_PIX_FMT_YUV420P &&
               format != AV_PIX_FMT_YUVJ420P &&
               format != AV_PIX_FMT_NV12;  // NV12 can be used directly on macOS
    }

    // VideoToolbox optimization
    static bool canUseDirectVideoToolbox(AVPixelFormat format) {
        #ifdef __APPLE__
        // VideoToolbox on macOS can directly output these formats
        return format == AV_PIX_FMT_VIDEOTOOLBOX ||
               format == AV_PIX_FMT_NV12;
        #else
        return false;
        #endif
    }
};

// Class for AVFrame pool to reduce allocations
class AVFramePool {
public:
    static AVFramePool& getInstance() {
        static AVFramePool instance;
        return instance;
    }

    std::shared_ptr<AVFrame> getFrame(int width, int height, AVPixelFormat format) {
        std::lock_guard<std::mutex> lock(mutex_);

        // Look for suitable frame in pool
        for (auto it = pool_.begin(); it != pool_.end(); ++it) {
            AVFrame* frame = it->get();
            if (frame && frame->width == width && frame->height == height &&
                frame->format == format) {
                auto result = *it;
                pool_.erase(it);
                return result;
            }
        }

        // Create new frame
        AVFrame* frame = av_frame_alloc();
        if (!frame) return nullptr;

        frame->width = width;
        frame->height = height;
        frame->format = format;

        if (av_frame_get_buffer(frame, 32) < 0) {  // 32-byte alignment for SIMD
            av_frame_free(&frame);
            return nullptr;
        }

        return std::shared_ptr<AVFrame>(frame, [this](AVFrame* f) {
            returnFrame(f);
        });
    }

private:
    void returnFrame(AVFrame* frame) {
        if (!frame) return;

        std::lock_guard<std::mutex> lock(mutex_);

        // Limit pool size
        if (pool_.size() < 10) {
            av_frame_unref(frame);
            pool_.push_back(std::shared_ptr<AVFrame>(frame, [](AVFrame* f) { av_frame_free(&f); }));
        } else {
            av_frame_free(&frame);
        }
    }

    std::mutex mutex_;
    std::vector<std::shared_ptr<AVFrame>> pool_;
};

#endif // FSTP_VIDEO_OPTIMIZATIONS_H