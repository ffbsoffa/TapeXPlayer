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
    static void removeLowResFrames(std::vector<FSTP::FrameInfo>& frameIndex, int start, int end);

    bool decodeLowResRange(std::vector<FSTP::FrameInfo>& frameIndex,
                           int startFrame,
                           int endFrame,
                           int highResStart,
                           int highResEnd,
                           bool skipHighResWindow = false,
                           bool isReverse = false);

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
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> is_decoding_{false};
    AVBufferRef* global_hw_device_ctx_ = nullptr;
    bool hw_accel_available_ = false;

    // Thread-local contexts for reuse (avoid overhead 70-300ms per segment)
    // STATIC to survive object lifetime (threads can work after destructor!)
    static std::map<std::string, std::map<int, ThreadDecoderContext>> allThreadContexts_;

    // CRITICAL: Per-file mutex instead of global for parallel decoding multiple instances!
    // Global mutex created serialization bottleneck when 2+ players
    static std::map<std::string, std::unique_ptr<std::mutex>> perFileMutexes_;
    static std::mutex globalMutexForMapAccess_;  // Only for map access, not for decoding!
};

} // namespace FSTP


