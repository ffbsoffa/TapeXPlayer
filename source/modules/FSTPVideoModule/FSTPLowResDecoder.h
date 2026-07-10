#pragma once

#include <vector>
#include <memory>
#include <atomic>
#include <future>
#include <string>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include "FSTPVideoFrame.h"
#include "FSTPAVFramePool.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#ifdef __APPLE__
#include <libavutil/hwcontext.h>
#endif
}

#include "FSTPVideoFrame.h"

namespace FSTP {

class LowResDecoder {
public:
    explicit LowResDecoder(const std::string& lowResFilename);
    ~LowResDecoder();

    static bool convertToLowRes(const std::string& filename,
                                std::string& outputFilename,
                                const std::function<void(int)>& progressCallback = nullptr);

    static std::string getCachePath();
    static std::string generateFileId(const std::string& filename);

    // Validate a cached proxy against its source via the sidecar manifest (size+mtime+schema).
    // Returns false for a missing/old-schema/mismatched manifest → caller should rebuild.
    // Use this anywhere a cached proxy is reused, so stale/gappy proxies never get trusted.
    static bool isCachedProxyValid(const std::string& proxyPath,
                                   const std::string& sourceFilename);
    static void removeLowResFrames(std::vector<FSTP::FrameInfo>& frameIndex, int start, int end);

    bool decodeLowResRange(std::vector<FSTP::FrameInfo>& frameIndex,
                           int startFrame,
                           int endFrame,
                           int highResStart,
                           int highResEnd,
                           bool skipHighResWindow = false,
                           bool isReverse = false,
                           bool keyframesOnly = false,    // shuttle: decode keyframes only (skip_frame)
                           int threadIdBase = 0);          // base id for the 2 internal decode contexts
                                                           // (on-demand decode uses a separate base so it
                                                           //  never shares an AVCodecContext with the manager)

    bool isInitialized() const;
    int getWidth() const;
    int getHeight() const;
    AVPixelFormat getPixelFormat() const;

    void requestStop();

private:
    bool initialize();
    void cleanup();

    // Thread-local decoder contexts for reuse (avoid overhead open/close)
    struct ThreadDecoderContext {
        AVFormatContext* formatCtx = nullptr;
        AVCodecContext* codecCtx = nullptr;
        int videoStreamIndex = -1;
        bool initialized = false;
        bool hw_accel_enabled = false;
        AVBufferRef* hw_device_ctx = nullptr;
        AVRational sar = {1, 1};  // Sample Aspect Ratio from stream

        void cleanup() {
            if (codecCtx) {
                avcodec_free_context(&codecCtx);
                codecCtx = nullptr;
            }
            if (formatCtx) {
                avformat_close_input(&formatCtx);
                formatCtx = nullptr;
            }
            if (hw_device_ctx) {
                av_buffer_unref(&hw_device_ctx);
                hw_device_ctx = nullptr;
            }
            initialized = false;
        }
    };

    // Get or create thread-local context for current thread
    ThreadDecoderContext* getOrCreateThreadContext(int threadId, bool useHardwareAccel);

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
    AVRational sar_ = {1, 1};  // Sample Aspect Ratio from stream (for anamorphic content)
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> is_decoding_{false};
    std::atomic<bool> cleanup_called_{false};  // Protection from double cleanup
    AVBufferRef* global_hw_device_ctx_ = nullptr;
    bool hw_accel_available_ = false;

    // Thread-local contexts for reuse (avoid overhead 70-300ms per segment)
    // STATIC to survive object lifetime (threads can work after destructor!)
    static std::map<std::string, std::map<int, ThreadDecoderContext>> allThreadContexts_;

    // CRITICAL: Per-file mutex instead of global for parallel decoding multiple instances!
    // Global mutex created serialization bottleneck when 2+ players
    static std::map<std::string, std::unique_ptr<std::mutex>> perFileMutexes_;
    static std::mutex globalMutexForMapAccess_;  // Only for map access, not for decoding!

    // CRITICAL FIX FOR RACE CONDITION: Global mutex to serialize av_frame_unref() calls
    // When using av_frame_ref() (zero-copy), frames share buffers via FFmpeg refcounting
    // During batch cleanup of 156K+ frames, simultaneous av_frame_unref() calls cause
    // race condition in av_buffer_unref() leading to double-free corruption
    // This mutex serializes cleanup to prevent the race
    static std::mutex frameCleanupMutex_;

    // OPTIMIZATION: Frame pool for buffer reuse (avoid av_frame_clone overhead)
    // Per-file pool (each LowResDecoder instance gets its own pool)
    // 480x360 YUV420P = ~259 KB per frame (after anamorphic optimization)
    // Pool saves 155 MB copying per 600-frame segment!
    //
    // NOTE: Only CPU frames (YUV420P) use pool.
    // HW frames (NV12) can't use pool because av_frame_copy() fails with NV12.
    // av_frame_clone() for HW frames is already efficient (zero-copy via refcounting).
    //
    // shared_ptr (not unique_ptr) so a decoded frame's shared_ptr<AVFrame> deleter can hold a
    // weak_ptr to the pool. When this decoder is destroyed on a file switch, any frames still
    // held elsewhere (e.g. the pixel buffer's last displayed frame) see an EXPIRED weak_ptr and
    // free themselves directly instead of calling Release() on a destroyed pool — which locked a
    // destroyed mutex and aborted with "mutex lock failed: Invalid argument".
    std::shared_ptr<AVFramePool> frame_pool_;
};

} // namespace FSTP


