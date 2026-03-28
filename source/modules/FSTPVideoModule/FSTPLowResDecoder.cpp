#include "FSTPLowResDecoder.h"

#include "FSTPVideoFrame.h"
#include "FSTPHardwareDetection.h"
#include "FSTPPerformanceProfiler.h"
#include "FSTPSIMDOptimizations.h"

#include <iostream>
#include <filesystem>
#include <thread>
#include <chrono>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <functional>
#include <cmath>
#include <limits>
#include <iomanip>
#ifdef _WIN32
#include <windows.h>
#endif

extern "C" {
#include <libavutil/opt.h>
}

namespace fs = std::filesystem;

// External hardware detection instance
extern FSTPHardwareDetection* g_hardware_detection;

namespace FSTP {

// Debug control: set to true to enable verbose logging
static constexpr bool ENABLE_LOWRES_DECODER_DEBUG = false;

// Static variables for thread-local contexts (survive object lifetime)
std::map<std::string, std::map<int, LowResDecoder::ThreadDecoderContext>> LowResDecoder::allThreadContexts_;

// Per-file mutexes for parallel decoding of multiple instances
std::map<std::string, std::unique_ptr<std::mutex>> LowResDecoder::perFileMutexes_;
std::mutex LowResDecoder::globalMutexForMapAccess_;

// CRITICAL FIX: Global mutex to serialize av_frame_unref() calls during cleanup
// Prevents race condition in FFmpeg's av_buffer_unref() when cleaning up frames with shared buffers
std::mutex LowResDecoder::frameCleanupMutex_;

namespace {

// Hardware format selector for macOS (VideoToolbox), Linux (VA-API) and Windows (D3D11VA)
static enum AVPixelFormat SelectHWFormat(AVCodecContext* ctx, const AVPixelFormat* pix_fmts) {
    const enum AVPixelFormat* p;
    for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
#ifdef __APPLE__
        if (*p == AV_PIX_FMT_VIDEOTOOLBOX) return AV_PIX_FMT_VIDEOTOOLBOX;
#elif defined(__linux__)
        if (*p == AV_PIX_FMT_VAAPI)         return AV_PIX_FMT_VAAPI;
#elif defined(_WIN32)
        if (*p == AV_PIX_FMT_D3D11)         return AV_PIX_FMT_D3D11;
#endif
    }
    return AV_PIX_FMT_NONE;
}

#ifdef _WIN32
// Runs a UTF-8 encoded command via CreateProcessW (Unicode-safe, bypasses cmd.exe ANSI).
// lineCallback is called for each line of combined stdout+stderr output.
// Returns the process exit code, or -1 on failure to launch.
static int RunCommandW(const std::string& utf8Command,
                       const std::function<void(const std::string&)>& lineCallback) {
    // Convert UTF-8 command to wide string
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8Command.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return -1;
    std::wstring wcmd(wlen - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8Command.c_str(), -1, &wcmd[0], wlen);

    // Create a pipe: child writes stdout+stderr, parent reads
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    HANDLE hRead = nullptr, hWrite = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return -1;
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);  // read end not inherited by child

    STARTUPINFOW si{};
    si.cb          = sizeof(STARTUPINFOW);
    si.hStdOutput  = hWrite;
    si.hStdError   = hWrite;  // merge stderr into same pipe
    si.dwFlags     = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(
        nullptr, &wcmd[0],        // exe from command line (mutable wide string required)
        nullptr, nullptr,          // process/thread security attributes
        TRUE,                      // inherit handles (for the pipe)
        CREATE_NO_WINDOW,          // no console window popup
        nullptr, nullptr,          // inherit environment and working directory
        &si, &pi
    );

    CloseHandle(hWrite);  // parent must close its copy of the write end
    if (!ok) {
        CloseHandle(hRead);
        return -1;
    }

    // Read output line by line and forward to callback
    char buf[4096];
    DWORD nRead;
    std::string lineBuffer;
    while (ReadFile(hRead, buf, sizeof(buf) - 1, &nRead, nullptr) && nRead > 0) {
        buf[nRead] = '\0';
        lineBuffer += buf;
        size_t pos;
        while ((pos = lineBuffer.find('\n')) != std::string::npos) {
            lineCallback(lineBuffer.substr(0, pos));
            lineBuffer.erase(0, pos + 1);
        }
    }
    if (!lineBuffer.empty()) lineCallback(lineBuffer);  // flush last partial line

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = static_cast<DWORD>(-1);
    GetExitCodeProcess(pi.hProcess, &exitCode);

    CloseHandle(hRead);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(exitCode);
}
#endif

static double QueryVideoDuration(const std::string& filename) {
    std::string command = "ffprobe -v error -show_entries format=duration -of default=noprint_wrappers=1:nokey=1 \"" + filename + "\"";
    std::string result;

#ifdef _WIN32
    RunCommandW(command, [&](const std::string& line) { result += line; });
#else
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return -1.0;
    char buffer[128];
    while (fgets(buffer, sizeof(buffer), pipe)) result += buffer;
    pclose(pipe);
#endif

    try {
        return std::stod(result);
    } catch (...) {
        return -1.0;
    }
}

// OPTIMIZATION: Binary search for B-frame time matching
// frameIndex is SORTED by time_ms → use binary search instead of linear O(N) → O(log N)
// For GOP=300: 300 checks → ~9 checks (33x speedup!)
static int BinarySearchFrameByTime(const std::vector<FSTP::FrameInfo>& frameIndex,
                                   int searchStart,
                                   int searchEnd,
                                   double targetTimeMs,
                                   double tolerance) {
    int left = searchStart;
    int right = searchEnd;
    int bestMatch = -1;
    double bestTime = -1.0;

    while (left <= right) {
        int mid = left + (right - left) / 2;

        // Safety check: ensure index is valid
        if (mid < 0 || mid >= static_cast<int>(frameIndex.size())) {
            break;
        }

        int64_t indexTimeMs_int = frameIndex[mid].time_ms;

        // Skip invalid entries (time_ms < 0)
        if (indexTimeMs_int < 0) {
            // Try searching right side first (most frames have valid time)
            left = mid + 1;
            continue;
        }

        double indexTimeMs = static_cast<double>(indexTimeMs_int);

        // Check if this is a match within tolerance
        if (indexTimeMs <= (targetTimeMs + tolerance)) {
            // This frame is at or before target time
            // Remember it as potential best match
            if (indexTimeMs >= bestTime) {
                bestTime = indexTimeMs;
                bestMatch = mid;
            }
            // Search right half for potentially better match
            left = mid + 1;
        } else {
            // indexTimeMs > targetTimeMs + tolerance
            // Frame is after target time, search left half
            right = mid - 1;
        }
    }

    return bestMatch;
}

} // namespace

LowResDecoder::LowResDecoder(const std::string& lowResFilename)
    : lowResFilename_(lowResFilename)
    , width_(0)
    , height_(0)
    , pixFmt_(AV_PIX_FMT_NONE)
    , initialized_(false) {
    // OPTIMIZATION: Do NOT call old initialize()!
    // All decoding goes through thread-local contexts (getOrCreateThreadContext)
    // Contexts will be lazily created on first decodeLowResRange call

    // Simple check that file exists
    AVFormatContext* test_ctx = nullptr;
    if (avformat_open_input(&test_ctx, lowResFilename_.c_str(), nullptr, nullptr) == 0) {
        if (avformat_find_stream_info(test_ctx, nullptr) >= 0) {
            int video_stream = av_find_best_stream(test_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
            if (video_stream >= 0) {
                width_ = test_ctx->streams[video_stream]->codecpar->width;
                height_ = test_ctx->streams[video_stream]->codecpar->height;
                pixFmt_ = static_cast<AVPixelFormat>(test_ctx->streams[video_stream]->codecpar->format);
                initialized_ = true;

                std::cout << "✅ [LowResDecoder] File opened: " << width_ << "x" << height_ << std::endl;

                // OPTIMIZATION: Initialize frame pool for CPU frames only
                // Pool size=120 frames (increased from 50 for high-speed playback)
                //
                // NOTE: Only CPU frames (YUV420P) use pool because:
                // - av_frame_copy() works perfectly with YUV420P
                // - av_frame_copy() FAILS with NV12 (returns "Invalid argument")
                // - HW frames (NV12) use av_frame_clone() which is already efficient
                //   (zero-copy via reference counting for GPU-resident buffers)
                //
                // At 60fps @ 32× reverse = 1920 frames/sec:
                // - CPU frames with pool: ~0 MB/sec overhead (buffer reuse)
                // - HW frames with clone: minimal overhead (ref counting, not actual copy)
                frame_pool_ = std::make_unique<AVFramePool>(width_, height_, pixFmt_, 120);

                std::cout << "🏊 [LowResDecoder] Created frame pool for "
                          << av_get_pix_fmt_name(pixFmt_) << " (120 frames)" << std::endl;
            }
        }
        avformat_close_input(&test_ctx);
    }

    if (!initialized_) {
        std::cerr << "❌ [LowResDecoder] Failed to open file: " << lowResFilename_ << std::endl;
    }
}

LowResDecoder::~LowResDecoder() {
    try {
        cleanup();
    } catch (const std::exception& e) {
        std::cerr << "⚠️ [LowResDecoder] Exception in destructor: " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "⚠️ [LowResDecoder] Unknown exception in destructor" << std::endl;
    }
}

bool LowResDecoder::initialize() {
    if (initialized_) {
        return true;
    }

    if (avformat_open_input(&formatCtx_, lowResFilename_.c_str(), nullptr, nullptr) != 0) {
        std::cerr << "LowResDecoder Error: Failed to open file " << lowResFilename_ << std::endl;
        cleanup();
        return false;
    }

    if (avformat_find_stream_info(formatCtx_, nullptr) < 0) {
        std::cerr << "LowResDecoder Error: Failed to find stream information for " << lowResFilename_ << std::endl;
        cleanup();
        return false;
    }

    const AVCodec* codec = nullptr; 
    videoStreamIndex_ = av_find_best_stream(formatCtx_, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (videoStreamIndex_ < 0 || !codec) {
        std::cerr << "LowResDecoder Error: Video stream not found or codec could not be found in " << lowResFilename_ << std::endl;
        cleanup();
        return false;
    }

    videoStream_ = formatCtx_->streams[videoStreamIndex_];
    codecParams_ = videoStream_->codecpar;

    codecCtx_ = avcodec_alloc_context3(codec);
    if (!codecCtx_) {
        std::cerr << "LowResDecoder Error: Failed to allocate codec context" << std::endl;
        cleanup();
        return false;
    }

    if (avcodec_parameters_to_context(codecCtx_, codecParams_) < 0) {
        std::cerr << "LowResDecoder Error: Failed to copy codec parameters to context" << std::endl;
        cleanup();
        return false;
    }

    // Hardware acceleration: VideoToolbox (macOS), VA-API (Linux), D3D11VA (Windows)
    if (codecCtx_->codec_id == AV_CODEC_ID_H264 || codecCtx_->codec_id == AV_CODEC_ID_HEVC) {
        AVBufferRef* hw_device_ctx = nullptr;
        int ret = -1;

#ifdef __APPLE__
        ret = av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_VIDEOTOOLBOX, nullptr, nullptr, 0);
        if (ret >= 0) {
            codecCtx_->hw_device_ctx = av_buffer_ref(hw_device_ctx);
            codecCtx_->get_format = SelectHWFormat;
            av_buffer_unref(&hw_device_ctx);
            std::cout << "[LowResDecoder] ✅ VideoToolbox hardware acceleration enabled" << std::endl;
        } else {
            std::cerr << "[LowResDecoder] ⚠️  VideoToolbox failed, using software decoder" << std::endl;
        }
#elif defined(__linux__)
        ret = av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI, "/dev/dri/renderD128", nullptr, 0);
        if (ret >= 0) {
            codecCtx_->hw_device_ctx = av_buffer_ref(hw_device_ctx);
            codecCtx_->get_format = SelectHWFormat;
            av_buffer_unref(&hw_device_ctx);
            std::cout << "[LowResDecoder] ✅ VA-API hardware acceleration enabled (renderD128)" << std::endl;
        } else {
            std::cerr << "[LowResDecoder] ⚠️  VA-API failed, using software decoder" << std::endl;
        }
#elif defined(_WIN32)
        ret = av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0);
        if (ret >= 0) {
            codecCtx_->hw_device_ctx = av_buffer_ref(hw_device_ctx);
            codecCtx_->get_format = SelectHWFormat;
            av_buffer_unref(&hw_device_ctx);
            std::cout << "[LowResDecoder] ✅ D3D11VA hardware acceleration enabled" << std::endl;
        } else {
            std::cerr << "[LowResDecoder] ⚠️  D3D11VA failed, using software decoder" << std::endl;
        }
#endif
    }

    if (avcodec_open2(codecCtx_, codec, nullptr) < 0) {
        std::cerr << "LowResDecoder Error: Failed to open codec" << std::endl;
        cleanup();
        return false;
    }

    width_ = codecCtx_->width;
    height_ = codecCtx_->height;
    pixFmt_ = codecCtx_->pix_fmt;

    // Read SAR from stream for anamorphic content
    sar_ = videoStream_->sample_aspect_ratio;
    if (sar_.num <= 0 || sar_.den <= 0) {
        sar_ = {1, 1};  // Default to square pixels
    }

    initialized_ = true;
    return true;
}

void LowResDecoder::cleanup() {
    // Protection from double cleanup (CRITICAL: avoid double-free crashes)
    bool expected = false;
    if (!cleanup_called_.compare_exchange_strong(expected, true)) {
        // cleanup() already called, skip to avoid double-free
        return;
    }

    // Cleanup thread-local contexts ONLY for current file (avoid leaks)
    // CRITICAL: Wrap ALL mutex operations in try-catch to handle static destruction order issues
    try {
        // Use per-file mutex for safe cleanup
        std::mutex* fileMutex = nullptr;

        // STEP 1: Get file mutex (wrapped in try-catch for static destruction safety)
        try {
            std::lock_guard<std::mutex> mapLock(globalMutexForMapAccess_);
            auto mutexIt = perFileMutexes_.find(lowResFilename_);
            if (mutexIt != perFileMutexes_.end()) {
                fileMutex = mutexIt->second.get();
            }
        } catch (const std::system_error& e) {
            // Global mutex already destroyed (static destruction order), skip cleanup
            std::cerr << "⚠️ [LowResDecoder] Global mutex destroyed during cleanup, skipping: " << e.what() << std::endl;
            return;  // Cannot proceed safely
        }

        // STEP 2: Cleanup thread contexts using file mutex
        if (fileMutex) {
            try {
                std::lock_guard<std::mutex> lock(*fileMutex);
                auto it = allThreadContexts_.find(lowResFilename_);
                if (it != allThreadContexts_.end()) {
                    for (auto& [threadId, ctx] : it->second) {
                        ctx.cleanup();
                    }
                    allThreadContexts_.erase(it);
                }

                // STEP 3: Remove per-file mutex after cleanup
                try {
                    std::lock_guard<std::mutex> mapLock(globalMutexForMapAccess_);
                    perFileMutexes_.erase(lowResFilename_);
                } catch (const std::system_error& e) {
                    // Global mutex destroyed, cannot clean up map
                    std::cerr << "⚠️ [LowResDecoder] Cannot remove file mutex (global mutex destroyed): " << e.what() << std::endl;
                }
            } catch (const std::system_error& e) {
                // File mutex lock failed (already destroyed?), skip thread context cleanup
                std::cerr << "⚠️ [LowResDecoder] File mutex lock failed during cleanup: " << e.what() << std::endl;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "⚠️ [LowResDecoder] Exception during thread context cleanup: " << e.what() << std::endl;
        // Continue to main decoder cleanup below
    }

    // Cleanup main decoder resources (safe even if thread context cleanup failed)
    if (swsCtx_) {
        sws_freeContext(swsCtx_);
        swsCtx_ = nullptr;
    }
    if (codecCtx_) {
        avcodec_free_context(&codecCtx_);
        codecCtx_ = nullptr;
    }
    codecParams_ = nullptr;
    if (formatCtx_) {
        avformat_close_input(&formatCtx_);
        formatCtx_ = nullptr;
    }
    videoStream_ = nullptr;
    videoStreamIndex_ = -1;
    initialized_ = false;
    width_ = 0;
    height_ = 0;
    pixFmt_ = AV_PIX_FMT_NONE;
    stop_requested_.store(false);
    is_decoding_.store(false);
}

void LowResDecoder::requestStop() {
    stop_requested_.store(true);
}

std::string LowResDecoder::getCachePath() {
#ifdef _WIN32
    // Windows: %LOCALAPPDATA%\TapeXPlayer\cache
    // C:\Users\<user>\AppData\Local\TapeXPlayer\cache — local, not synced, easy to delete
    const char* localAppData = std::getenv("LOCALAPPDATA");
    if (localAppData) {
        fs::path cacheDir = fs::path(localAppData) / "TapeXPlayer" / "cache";
        return cacheDir.string();
    }
    const char* temp = std::getenv("TEMP");
    fs::path cacheDir = fs::path(temp ? temp : "C:/Temp") / "TapeXPlayer" / "cache";
    return cacheDir.string();
#else
    const char* homeDir = std::getenv("HOME");
    if (!homeDir) {
        homeDir = "/tmp";
    }
    fs::path cacheDir = fs::path(homeDir) / ".fstp" / "cache";
    return cacheDir.string();
#endif
}

std::string LowResDecoder::generateFileId(const std::string& filename) {
    std::hash<std::string> hasher;
    return std::to_string(hasher(filename));
}

void LowResDecoder::removeLowResFrames(std::vector<FrameInfo>& frameIndex, int start, int end) {
    start = std::max(0, start);
    end = std::min(static_cast<int>(frameIndex.size()) - 1, end);

    for (int i = start; i <= end; ++i) {
        FrameInfo& info = frameIndex[i];
        std::lock_guard<std::mutex> lock(info.mutex);
        if (info.low_res_frame) {
            // CRITICAL FIX: Do NOT call av_frame_unref on shared_ptr managed AVFrame!
            // shared_ptr will automatically call av_frame_free when refcount reaches 0
            // Calling av_frame_unref here causes double free corruption
            info.low_res_frame.reset();
            if (info.type == FrameInfo::LOW_RES) {
                if (info.frame) {
                    info.type = FrameInfo::FULL_RES;
                } else if (info.cached_frame) {
                    info.type = FrameInfo::CACHED;
                } else {
                    info.type = FrameInfo::EMPTY;
                }
            }
        }
    }
}

// Get or create thread-local context (REUSE avoids overhead 70-300ms)
LowResDecoder::ThreadDecoderContext* LowResDecoder::getOrCreateThreadContext(int threadId, bool useHardwareAccel) {
    // CRITICAL: Use per-file mutex for parallel decoding!
    // Different files can be decoded simultaneously (multiple instances)

    // Step 1: Quickly get/create per-file mutex
    std::mutex* fileMutex = nullptr;
    {
        std::lock_guard<std::mutex> mapLock(globalMutexForMapAccess_);
        auto& mutexPtr = perFileMutexes_[lowResFilename_];
        if (!mutexPtr) {
            mutexPtr = std::make_unique<std::mutex>();
        }
        fileMutex = mutexPtr.get();
    }

    // Step 2: Acquire per-file mutex (does NOT block other files!)
    std::lock_guard<std::mutex> lock(*fileMutex);

    // Get contexts for current file
    auto& fileContexts = allThreadContexts_[lowResFilename_];

    auto it = fileContexts.find(threadId);
    if (it != fileContexts.end() && it->second.initialized) {
        // Context already exists - reuse it!
        return &it->second;
    }

    // Create new context for current file
    ThreadDecoderContext& ctx = fileContexts[threadId];

    std::cout << "🔧 [Thread " << threadId << "] Initializing reusable decoder context..."  << std::endl;

    // Open file
    if (avformat_open_input(&ctx.formatCtx, lowResFilename_.c_str(), nullptr, nullptr) != 0) {
        std::cerr << "❌ [Thread " << threadId << "] Failed to open file" << std::endl;
        return nullptr;
    }

    if (avformat_find_stream_info(ctx.formatCtx, nullptr) < 0) {
        avformat_close_input(&ctx.formatCtx);
        std::cerr << "❌ [Thread " << threadId << "] Failed to find stream info" << std::endl;
        return nullptr;
    }

    // Find video stream
    const AVCodec* codec = nullptr;
    ctx.videoStreamIndex = av_find_best_stream(ctx.formatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (ctx.videoStreamIndex < 0 || !codec) {
        avformat_close_input(&ctx.formatCtx);
        std::cerr << "❌ [Thread " << threadId << "] Video stream not found" << std::endl;
        return nullptr;
    }

    AVCodecParameters* codecParams = ctx.formatCtx->streams[ctx.videoStreamIndex]->codecpar;
    AVStream* videoStream = ctx.formatCtx->streams[ctx.videoStreamIndex];

    // Read SAR from stream for anamorphic content
    ctx.sar = videoStream->sample_aspect_ratio;
    if (ctx.sar.num <= 0 || ctx.sar.den <= 0) {
        ctx.sar = {1, 1};  // Default to square pixels
    }

    // Allocate codec context
    ctx.codecCtx = avcodec_alloc_context3(codec);
    if (!ctx.codecCtx) {
        avformat_close_input(&ctx.formatCtx);
        std::cerr << "❌ [Thread " << threadId << "] Failed to allocate codec context" << std::endl;
        return nullptr;
    }

    if (avcodec_parameters_to_context(ctx.codecCtx, codecParams) < 0) {
        avcodec_free_context(&ctx.codecCtx);
        avformat_close_input(&ctx.formatCtx);
        std::cerr << "❌ [Thread " << threadId << "] Failed to copy codec params" << std::endl;
        return nullptr;
    }

    // ========================================
    // UNIFIED HARDWARE DETECTION (NEW!)
    // ========================================
    // Use centralized FSTPHardwareDetection instead of inline detection

    if (useHardwareAccel && (ctx.codecCtx->codec_id == AV_CODEC_ID_H264 || ctx.codecCtx->codec_id == AV_CODEC_ID_HEVC)) {
        // Get decoder strategy from unified hardware detection
        if (g_hardware_detection) {
            FSTPDecoderStrategy strategy = g_hardware_detection->GetDecoderStrategy(
                ctx.codecCtx->width,
                ctx.codecCtx->height,
                ctx.codecCtx->codec_id,
                1.0  // playback_speed (normal for context creation)
            );

            std::cout << "🎯 [Thread " << threadId << "] Strategy: " << strategy.strategy_reason << std::endl;

            // Apply hardware acceleration strategy
            if (strategy.use_hw_accel) {
                int ret = -1;

                if (strategy.hw_accel_type == FSTPHWAccelType::VIDEOTOOLBOX) {
                    // macOS VideoToolbox
                    ret = av_hwdevice_ctx_create(&ctx.hw_device_ctx, AV_HWDEVICE_TYPE_VIDEOTOOLBOX, nullptr, nullptr, 0);
                    if (ret >= 0) {
                        ctx.codecCtx->hw_device_ctx = av_buffer_ref(ctx.hw_device_ctx);
                        ctx.codecCtx->get_format = SelectHWFormat;
                        ctx.hw_accel_enabled = true;
                        std::cout << "✅ [Thread " << threadId << "] VideoToolbox HW accel enabled" << std::endl;
                    }
                } else if (strategy.hw_accel_type == FSTPHWAccelType::VAAPI) {
                    // Linux VA-API
                    const char* device = strategy.hw_device_path.empty() ? "/dev/dri/renderD128" : strategy.hw_device_path.c_str();
                    ret = av_hwdevice_ctx_create(&ctx.hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI, device, nullptr, 0);
                    if (ret >= 0) {
                        ctx.codecCtx->hw_device_ctx = av_buffer_ref(ctx.hw_device_ctx);
                        ctx.codecCtx->get_format = SelectHWFormat;
                        ctx.hw_accel_enabled = true;
                        std::cout << "✅ [Thread " << threadId << "] VA-API HW accel enabled (" << device << ")" << std::endl;
                    }
                } else if (strategy.hw_accel_type == FSTPHWAccelType::D3D11VA ||
                           strategy.hw_accel_type == FSTPHWAccelType::DXVA2) {
                    // Windows D3D11VA
                    ret = av_hwdevice_ctx_create(&ctx.hw_device_ctx, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0);
                    if (ret >= 0) {
                        ctx.codecCtx->hw_device_ctx = av_buffer_ref(ctx.hw_device_ctx);
                        ctx.codecCtx->get_format = SelectHWFormat;
                        ctx.hw_accel_enabled = true;
                        std::cout << "✅ [Thread " << threadId << "] D3D11VA HW accel enabled" << std::endl;
                    }
                }

                if (ret < 0) {
                    std::cout << "⚠️  [Thread " << threadId << "] HW accel init failed, using CPU" << std::endl;
                    strategy.use_hw_accel = false;  // Fallback to CPU
                }
            }

            // Apply threading configuration
            ctx.codecCtx->thread_count = strategy.thread_count;
            ctx.codecCtx->thread_type = strategy.thread_type;

            // Apply codec flags
            ctx.codecCtx->flags |= strategy.codec_flags;
            ctx.codecCtx->flags2 |= strategy.codec_flags2;

            // Apply performance tuning
            ctx.codecCtx->skip_frame = static_cast<AVDiscard>(strategy.skip_frame);
            ctx.codecCtx->skip_idct = static_cast<AVDiscard>(strategy.skip_idct);
            ctx.codecCtx->skip_loop_filter = static_cast<AVDiscard>(strategy.skip_loop_filter);

            std::cout << "✅ [Thread " << threadId << "] Decoder configured: "
                      << (strategy.use_hw_accel ? "HW" : "CPU") << " decode, "
                      << strategy.thread_count << " threads, SIMD level " << strategy.simd_level << std::endl;
        } else {
            std::cerr << "⚠️  [Thread " << threadId << "] g_hardware_detection not initialized, using defaults" << std::endl;
        }
    } else {
        (void)useHardwareAccel; // Unused if not H264/HEVC
    }

    // Open codec
    if (avcodec_open2(ctx.codecCtx, codec, nullptr) < 0) {
        if (ctx.hw_device_ctx) av_buffer_unref(&ctx.hw_device_ctx);
        avcodec_free_context(&ctx.codecCtx);
        avformat_close_input(&ctx.formatCtx);
        std::cerr << "❌ [Thread " << threadId << "] Failed to open codec" << std::endl;
        return nullptr;
    }

    ctx.initialized = true;
    std::cout << "✅ [Thread " << threadId << "] Decoder context initialized" << std::endl;
    return &ctx;
}

bool LowResDecoder::decodeLowResRange(std::vector<FrameInfo>& frameIndex,
                                      int startFrame,
                                      int endFrame,
                                      int highResStart,
                                      int highResEnd,
                                      bool skipHighResWindow,
                                      bool isReverse) {
    (void)highResStart;
    (void)highResEnd;
    (void)skipHighResWindow;

    stop_requested_ = false;
    is_decoding_ = true;

    if (!initialized_) {
        std::cerr << "LowResDecoder::decodeLowResRange Error: Decoder object not initialized." << std::endl;
        is_decoding_ = false;
        return false;
    }

    if (frameIndex.empty()) {
        is_decoding_ = false;
        return true;
    }

    startFrame = std::max(0, startFrame);
    endFrame = std::min(static_cast<int>(frameIndex.size()) - 1, endFrame);

    if (startFrame > endFrame) {
        is_decoding_ = false;
        return false;
    }

    // PRIORITY DECODING for reverse 32x optimization
    // Problem: Linear decoding is too slow, playhead overtakes decoder
    // Solution: Decode in priority order - where playhead is NOW, then rest
    //
    // REVERSE STRATEGY (3-part split):
    // 1. Decode Part 3 (last third) FIRST with 2 threads ← playhead is here!
    // 2. Decode Part 1+2 (first two thirds) with 2 threads ← less urgent
    //
    // This ensures playhead always has decoded frames ahead
    const int numThreads = 2;  // Always use 2 threads for speed
    std::vector<std::thread> threads;
    std::atomic<bool> success{true};

    auto decodeSegment = [&](int threadId, int threadStartFrame, int threadEndFrame) {
        // Intel Celeron + PowerSaver + VA-API optimization:
        // ALL threads use hardware acceleration for maximum performance
        // PowerSaver + VA-API needs aggressive hardware usage to avoid dropout
        // CPU fallback only if VA-API fails
        bool useHardwareAccel = true; // Force hardware acceleration for all threads
        
        // ADDITIONAL: Intel Celeron optimization - reduce decoder overhead
        // Use more aggressive settings for PowerSaver mode
        if (isReverse) {
            // REVERSE: Ultra-aggressive settings for Intel Celeron
            // Reduce memory allocations and context switching
            useHardwareAccel = true;
        }
        ThreadDecoderContext* ctx = getOrCreateThreadContext(threadId, useHardwareAccel);

        if (!ctx || !ctx->initialized) {
            std::cerr << "❌ [Thread " << threadId << "] Failed to get decoder context" << std::endl;
            success = false;
            return;
        }

        // Use reusable context
        AVFormatContext* formatContext = ctx->formatCtx;
        AVCodecContext* codecContext = ctx->codecCtx;
        int videoStream = ctx->videoStreamIndex;

        AVPacket* packet = av_packet_alloc();
        AVFrame* frame = av_frame_alloc();
        if (!packet || !frame) {
            if (packet) av_packet_free(&packet);
            if (frame) av_frame_free(&frame);
            success = false;
            return;
        }

        AVRational timeBase = formatContext->streams[videoStream]->time_base;
        int64_t startTime = formatContext->streams[videoStream]->start_time;

        // --- GOP-AWARE SEEKING: Find nearest keyframe for fast start ---
        // PROBLEM: With GOP=300 seek to threadStartFrame may be in middle of GOP
        // SOLUTION: Find keyframe BEFORE threadStartFrame for correct decoding
        int64_t seekTargetTimeMs = -1;
        int seekFrame = threadStartFrame;

        // Find nearest keyframe backwards (up to 400 frames - covers GOP=300 with margin)
        int searchLimit = std::max(0, threadStartFrame - 400);
        for (int i = threadStartFrame; i >= searchLimit; i--) {
            if (i < static_cast<int>(frameIndex.size()) && frameIndex[i].is_keyframe) {
                seekFrame = i;
                if (ENABLE_LOWRES_DECODER_DEBUG) {
                    static int keyframe_log = 0;
                    if (keyframe_log++ < 10) {
                        std::cout << "🔑 [GOP-AWARE SEEK] Thread " << threadId
                                  << " found keyframe at " << i
                                  << " (requested segment start: " << threadStartFrame
                                  << ", distance: " << (threadStartFrame - i) << " frames)" << std::endl;
                    }
                }
                break;
            }
        }

        // Use found keyframe for seek
        int validTimeFrame = seekFrame;
        while (validTimeFrame <= threadEndFrame && validTimeFrame < static_cast<int>(frameIndex.size())
               && frameIndex[validTimeFrame].time_ms < 0) {
            validTimeFrame++;
        }

        if (validTimeFrame <= threadEndFrame && validTimeFrame < static_cast<int>(frameIndex.size())) {
            seekTargetTimeMs = frameIndex[validTimeFrame].time_ms;
            int64_t seek_target_ts = av_rescale_q(seekTargetTimeMs, {1, 1000}, timeBase);
            int seek_flags = AVSEEK_FLAG_BACKWARD; // Seek to nearest keyframe before target
            int seek_ret = av_seek_frame(formatContext, videoStream, seek_target_ts, seek_flags);
            if (seek_ret < 0) {
                 char av_errbuf_seek[AV_ERROR_MAX_STRING_SIZE];
                 av_make_error_string(av_errbuf_seek, AV_ERROR_MAX_STRING_SIZE, seek_ret);
                 std::cerr << "[Thread " << threadId << "] Warning: Seek to ts " << seek_target_ts
                           << " (ms " << seekTargetTimeMs << ") failed: " << av_errbuf_seek << std::endl;
                 avcodec_flush_buffers(codecContext);
            } else {
                 avcodec_flush_buffers(codecContext); // Flush after successful seek too
                if (ENABLE_LOWRES_DECODER_DEBUG) {
                    static int debug_count = 0;
                    if (debug_count++ < 5) {
                        std::cout << "[Thread " << threadId << "] GOP-aware seek successful to "
                                  << seekTargetTimeMs << " ms (frame " << seekFrame << ")" << std::endl;
                    }
                }
            }
        } else {
            std::cerr << "[Thread " << threadId << "] Warning: No valid timestamp found in range ["
                      << threadStartFrame << ", " << threadEndFrame << "] for seeking. "
                      << "Starting decode from beginning of stream for this thread." << std::endl;
            avcodec_flush_buffers(codecContext);
        }
        // --- End GOP-Aware Seeking Logic ---

        int currentFrame = threadStartFrame; // Frame index counter for this thread

        // DECODER PROFILING (thread_local for safety in multithreaded environment)
        thread_local static uint64_t total_read_frame_us = 0;
        thread_local static uint64_t total_send_packet_us = 0;
        thread_local static uint64_t total_receive_frame_us = 0;
        thread_local static uint64_t total_frame_ref_us = 0;
        thread_local static uint64_t total_hw_transfer_us = 0;
        thread_local static uint64_t total_frame_processing_us = 0;
        thread_local static int perf_samples = 0;
        thread_local static int perf_report_counter = 0;
        thread_local static auto segment_start_time = std::chrono::high_resolution_clock::now();

        // --- Decoding Loop (like original) ---
        while (!stop_requested_.load()) {
            auto t_before_read = std::chrono::high_resolution_clock::now();
            int read_result = av_read_frame(formatContext, packet);
            auto t_after_read = std::chrono::high_resolution_clock::now();
            total_read_frame_us += std::chrono::duration_cast<std::chrono::microseconds>(t_after_read - t_before_read).count();

            if (read_result < 0) break;

            FSTP_SIGNPOST_EVENT(fstp_log_decoder, "av_read_frame_lowres", "size=%d", packet->size);

            if (packet->stream_index == videoStream) {
                FSTP_PROFILE_DECODE_BEGIN("lowres");

                auto t_before_send = std::chrono::high_resolution_clock::now();
                int send_result = avcodec_send_packet(codecContext, packet);
                auto t_after_send = std::chrono::high_resolution_clock::now();
                total_send_packet_us += std::chrono::duration_cast<std::chrono::microseconds>(t_after_send - t_before_send).count();

                if (send_result < 0) {
                    av_packet_unref(packet);
                    FSTP_PROFILE_DECODE_END("lowres");
                    continue;
                }

                while (!stop_requested_.load()) {
                    auto t_before_receive = std::chrono::high_resolution_clock::now();
                    int receive_result = avcodec_receive_frame(codecContext, frame);
                    auto t_after_receive = std::chrono::high_resolution_clock::now();
                    total_receive_frame_us += std::chrono::duration_cast<std::chrono::microseconds>(t_after_receive - t_before_receive).count();

                    if (receive_result == 0) {
                        // Start of frame processing (end-to-end timing)
                        auto t_frame_start = std::chrono::high_resolution_clock::now();

                        // --- Frame Timing Calculation (like original) ---
                    int64_t framePts = frame->best_effort_timestamp;
                    if (framePts == AV_NOPTS_VALUE) framePts = frame->pts;

                    // FIXED: Use ABSOLUTE PTS for stability between segments
                    // Previously used relative PTS (framePts - startTime), which caused jitter
                    // when switching segments due to startTime changes after seek
                    // CRITICAL: Calculate RELATIVE time (as in old low_res_decoder.cpp)
                    // Must match SimpleVideoIndex which also uses relative time
                    double frameTimeMs = -1.0;
                    if (framePts != AV_NOPTS_VALUE) {
                        // Convert to microseconds for precision
                        int64_t pts_us = av_rescale_q(framePts, timeBase, {1, 1000000});
                        int64_t start_us = (startTime != AV_NOPTS_VALUE) ?
                            av_rescale_q(startTime, timeBase, {1, 1000000}) : 0;

                        // RELATIVE time from start_time (matches audio and SimpleVideoIndex)
                        int64_t relative_us = pts_us - start_us;

                        // Convert to milliseconds with fractional precision (critical for 29.94 fps)
                        frameTimeMs = static_cast<double>(relative_us) / 1000.0;
                    }
                    // --- End Frame Timing Calculation ---

                    // CRITICAL FOR B-FRAMES: Find correct slot by time, not just currentFrame++
                    // With B-frames decoded order ≠ display order, need to find slot in sorted frameIndex
                    // Logic as in old cached_decoder.cpp (lines 296-320)
                    int targetFrameIndex = currentFrame; // Default fallback
                    if (framePts != AV_NOPTS_VALUE && frameTimeMs >= 0) {
                        double maxTimeMsLessOrEqual = -1.0;  // FIXED: double for precision

                        // ADAPTIVE WINDOW: Cover 1-2 GOPs in both directions
                        // Increased to support EXTREME irregular GOPs (up to 300 frames!)
                        // Based on real analysis: max_gop=300, avg=107, stddev=80
                        int searchWindowBefore = isReverse ? 150 : 80;   // Increased 3x for GOP=300
                        int searchWindowAfter = isReverse ? 300 : 150;   // Enough for max GOP

                        // Soft margin: allow "peeking" beyond segment boundaries for B-frames from adjacent GOPs
                        // This does NOT break segmentation, just covers overlap between segments
                        // Increased for extreme GOPs up to 150 frames (~6 seconds @ 25fps)
                        int softMarginBefore = 150;  // For GOP=300 need larger margin
                        int softMarginAfter = 150;

                        int searchStart = std::max(threadStartFrame - softMarginBefore,
                                                   currentFrame - searchWindowBefore);
                        searchStart = std::max(0, searchStart);

                        int searchEnd = std::min(threadEndFrame + softMarginAfter,
                                                currentFrame + searchWindowAfter);
                        searchEnd = std::min(searchEnd, static_cast<int>(frameIndex.size()) - 1);

                        // CRITICAL: Tolerance for float comparison (important for 29.97fps and irregular GOPs)
                        // At 29.97fps frames come every 33.367ms - fractional precision is critical!
                        const double TIME_TOLERANCE_MS = 0.5;  // 0.5ms tolerance for float errors

                        // OPTIMIZATION: Use binary search instead of linear (O(log N) vs O(N))
                        // For GOP=300: 9 checks instead of ~150 (16x speedup!)
                        bool found = false;
                        int foundIndex = BinarySearchFrameByTime(frameIndex, searchStart, searchEnd,
                                                                 frameTimeMs, TIME_TOLERANCE_MS);
                        if (foundIndex >= 0) {
                            targetFrameIndex = foundIndex;
                            maxTimeMsLessOrEqual = static_cast<double>(frameIndex[foundIndex].time_ms);
                            found = true;
                        }

                        // EXTENDED FALLBACK LEVEL 1: If not found in window with margins, search entire segment
                        // This is for irregular GOPs where B-frames can be far from their position
                        // OPTIMIZATION: Use binary search here too
                        if (!found) {
                            int segmentEnd = std::min(threadEndFrame, static_cast<int>(frameIndex.size()) - 1);
                            foundIndex = BinarySearchFrameByTime(frameIndex, threadStartFrame, segmentEnd,
                                                                frameTimeMs, TIME_TOLERANCE_MS);
                            if (foundIndex >= 0) {
                                targetFrameIndex = foundIndex;
                                maxTimeMsLessOrEqual = static_cast<double>(frameIndex[foundIndex].time_ms);
                                found = true;

                                if (ENABLE_LOWRES_DECODER_DEBUG) {
                                    static int fallback_log = 0;
                                    if (fallback_log++ < 10) {
                                        std::cout << "⚠️  [FALLBACK L1] Frame " << targetFrameIndex
                                                  << " found in segment (irregular GOP, time="
                                                  << std::fixed << std::setprecision(3) << frameTimeMs << "ms)" << std::endl;
                                    }
                                }
                            }
                        }

                        // EXTREME FALLBACK LEVEL 2: For VERY large GOPs (>100 frames)
                        // Search in adjacent segments within ±GOP_size from current position
                        // This is critical for GOP=300 when B-frame can be in completely different segment!
                        // OPTIMIZATION: Use binary search for extreme range too
                        if (!found) {
                            // Expand search to ±400 frames (~16 seconds for GOP=300+margin)
                            int extremeSearchStart = std::max(0, currentFrame - 400);
                            int extremeSearchEnd = std::min(static_cast<int>(frameIndex.size()) - 1,
                                                            currentFrame + 400);

                            foundIndex = BinarySearchFrameByTime(frameIndex, extremeSearchStart, extremeSearchEnd,
                                                                frameTimeMs, TIME_TOLERANCE_MS);
                            if (foundIndex >= 0) {
                                targetFrameIndex = foundIndex;
                                maxTimeMsLessOrEqual = static_cast<double>(frameIndex[foundIndex].time_ms);
                                found = true;

                                static int extreme_fallback_log = 0;
                                if (extreme_fallback_log++ < 5) {
                                    std::cout << "🔴 [FALLBACK L2 EXTREME] Frame " << targetFrameIndex
                                              << " found in ±400 range (CRITICAL: GOP > 100 frames, time="
                                              << std::fixed << std::setprecision(3) << frameTimeMs << "ms)" << std::endl;
                                }
                            }
                        }

                        // DIAGNOSTICS: Log unsuccessful searches for debugging "broken" files
                        if (!found && ENABLE_LOWRES_DECODER_DEBUG) {
                            static int not_found_log = 0;
                            if (not_found_log++ < 5) {
                                std::cout << "❌ [B-FRAME NOT FOUND] Thread " << threadId
                                          << " decoded frame time=" << std::fixed << std::setprecision(3) << frameTimeMs
                                          << "ms, but NO slot found even in extreme range ±400 frames" << std::endl;
                                std::cout << "   searchStart=" << searchStart << ", searchEnd=" << searchEnd
                                          << ", segment=[" << threadStartFrame << "-" << threadEndFrame << "]" << std::endl;
                            }
                        }
                    }

                        // --- Frame Storage Logic (like original) ---
                    bool stored_this_frame_attempt = false;

                    // FIXED: use targetFrameIndex instead of currentFrame
                        if (targetFrameIndex >= threadStartFrame && targetFrameIndex <= threadEndFrame && targetFrameIndex < static_cast<int>(frameIndex.size()) &&
                        (frameTimeMs >= seekTargetTimeMs - 100 || seekTargetTimeMs < 0))
                    {
                        stored_this_frame_attempt = true; // We will attempt to process this slot

                        // ZERO-COPY: av_frame_ref instead of av_frame_clone (increment refcount, NOT data copy!)
                        auto t_before_ref = std::chrono::high_resolution_clock::now();

                        // Handle hardware frames (VideoToolbox on macOS, VA-API on Linux)
                        AVFrame* source_frame = frame;
                        AVFrame* temp_hw_frame = nullptr;

                        // Check for hardware frame formats
                        bool is_hw_frame = (frame->format == AV_PIX_FMT_VIDEOTOOLBOX) ||
                                          (frame->format == AV_PIX_FMT_VAAPI);

                        if (is_hw_frame) {
                            auto t_before_hw_transfer = std::chrono::high_resolution_clock::now();
                            temp_hw_frame = av_frame_alloc();
                            if (temp_hw_frame && av_hwframe_transfer_data(temp_hw_frame, frame, 0) >= 0) {
                                auto t_after_hw_transfer = std::chrono::high_resolution_clock::now();
                                total_hw_transfer_us += std::chrono::duration_cast<std::chrono::microseconds>(t_after_hw_transfer - t_before_hw_transfer).count();

                                // CRITICAL: Copy ALL timing fields from original frame!
                                temp_hw_frame->pts = framePts;
                                temp_hw_frame->best_effort_timestamp = framePts;
                                temp_hw_frame->pkt_dts = frame->pkt_dts;
                                source_frame = temp_hw_frame;

                                if (ENABLE_LOWRES_DECODER_DEBUG) {
                                    static int hw_transfer_log = 0;
                                    if (++hw_transfer_log % 100 == 1) {
                                        const char* hw_name = (frame->format == AV_PIX_FMT_VIDEOTOOLBOX) ? "VideoToolbox" : "VA-API";
                                        std::cout << "🎬 [LowResDecoder] " << hw_name << " frame transferred to CPU: "
                                                  << source_frame->width << "x" << source_frame->height
                                                  << ", format=" << source_frame->format << std::endl;
                                    }
                                }
                            } else {
                                std::cerr << "❌ [LowResDecoder] av_hwframe_transfer_data failed" << std::endl;
                                if (temp_hw_frame) av_frame_free(&temp_hw_frame);
                                source_frame = frame; // Fallback to original
                            }
                        }

                        // OPTIMIZATION: Use frame pool instead of av_frame_clone
                        // OLD: av_frame_clone() → 345 KB copy per frame (207 MB per 600-frame segment!)
                        // NEW: Frame pool → Zero allocation, buffer reuse, ~50% faster!
                        //
                        // DUAL-FORMAT POOL SUPPORT:
                        // - CPU decode → YUV420P → use frame_pool_
                        // - HW decode (VideoToolbox/QSV) → NV12 → use hw_frame_pool_
                        // This preserves hardware acceleration performance on Intel Pentium 7505!
                        AVFrame* ref_frame = nullptr;
                        AVFramePool* selected_pool = nullptr;
                        bool from_pool = false;

                        // Detect actual source format
                        AVPixelFormat source_format = static_cast<AVPixelFormat>(source_frame->format);

                        // STRATEGY: Use pool ONLY for CPU frames (YUV420P)
                        // - CPU frames (YUV420P) → use frame_pool_ + av_frame_copy()
                        // - HW frames (NV12) → use av_frame_clone() (zero-copy for HW buffers)
                        //
                        // IMPORTANT: av_frame_copy() does NOT work with NV12!
                        // Returns "Invalid argument" error, probably due to alignment requirements.
                        // HW frames (VideoToolbox/VA-API) are GPU-resident anyway, so av_frame_clone()
                        // is already efficient (uses reference counting, not actual copy).

                        if (source_format == pixFmt_) {
                            // CPU-decoded frame (YUV420P from proxy)
                            selected_pool = frame_pool_.get();
                        } else {
                            // HW-decoded frame (NV12) or unknown format
                            // Don't use pool - av_frame_copy() fails with NV12
                            selected_pool = nullptr;
                        }

                        if (selected_pool) {
                            // FAST PATH: Use frame pool (CPU or HW)
                            ref_frame = selected_pool->Acquire();
                            if (ref_frame) {
                                // Copy frame data from source to pooled frame
                                int copy_ret = av_frame_copy(ref_frame, source_frame);
                                if (copy_ret < 0) {
                                    char errbuf[256];
                                    av_strerror(copy_ret, errbuf, sizeof(errbuf));
                                    std::cerr << "❌ [LowResDecoder] av_frame_copy failed: " << errbuf
                                              << " (format=" << av_get_pix_fmt_name(source_format) << ")" << std::endl;
                                    selected_pool->Release(ref_frame);
                                    ref_frame = nullptr;
                                } else {
                                    // Copy metadata (PTS, timebase, etc.)
                                    av_frame_copy_props(ref_frame, source_frame);
                                    // Apply SAR from stream (for anamorphic content)
                                    ref_frame->sample_aspect_ratio = ctx->sar;
                                    from_pool = true;
                                }
                            }
                        }

                        // Pool unavailable or failed → fallback to av_frame_clone
                        if (!ref_frame) {
                            ref_frame = av_frame_clone(source_frame);
                            if (!ref_frame) {
                                std::cerr << "❌ [LowResDecoder] av_frame_clone failed!" << std::endl;
                            } else {
                                // Apply SAR from stream (for anamorphic content)
                                ref_frame->sample_aspect_ratio = ctx->sar;
                            }
                            from_pool = false;
                        }

                        // Clean up hardware frame if created
                        if (temp_hw_frame) {
                            av_frame_free(&temp_hw_frame);
                        }

                        auto t_after_ref = std::chrono::high_resolution_clock::now();
                        total_frame_ref_us += std::chrono::duration_cast<std::chrono::microseconds>(t_after_ref - t_before_ref).count();

                        if (ref_frame) {
                            // MEMORY SAFETY: Validate frame before storing
                            if (!ref_frame->data[0] || ref_frame->width <= 0 || ref_frame->height <= 0) {
                                std::cerr << "❌ [LowResDecoder] Invalid frame data detected, skipping frame" << std::endl;
                                av_frame_free(&ref_frame);
                                ref_frame = nullptr;
                            }
                            
                            // CRITICAL FIX: DISABLE ALL SIMD optimizations that modify shared buffers
                            // These functions modify FFmpeg-managed buffers from av_frame_ref() which causes heap corruption!
                            // - OptimizeFrameAlignment() calls av_frame_get_buffer() and reallocates buffers
                            // - SanitizeFrameData() modifies frame data directly
                            // AVFrame buffers from av_frame_ref() are SHARED and must NOT be modified
                            // (void)ref_frame; // All SIMD frame modifications completely disabled

                            if (ref_frame) {
                                // CRITICAL FOR METAL: Check linesize alignment for ALL frames
                                // Metal on macOS requires 64-byte alignment to avoid "AGX: bytes_per_row assertion"
                                const int METAL_LINESIZE_ALIGNMENT = 64;
                                bool needs_realignment = false;

                                // TEMPORARILY DISABLED: Realignment creates too much overhead (23000+ frames)
                                // and green frames still appear. Problem is somewhere else.
                                // TODO: Find source of AGX assertion (possibly FullResDecoder or other component)

                                // Log only first 5 frames for diagnostics
                                static int linesize_log = 0;
                                if (linesize_log++ < 5) {
                                    std::cout << "🔍 [LINESIZE] Thread " << threadId << " (HW=" << useHardwareAccel << "): "
                                              << "format=" << ref_frame->format
                                              << ", Y=" << ref_frame->linesize[0]
                                              << ", U=" << ref_frame->linesize[1]
                                              << ", V=" << ref_frame->linesize[2] << std::endl;
                                }

                                needs_realignment = false;  // TEMPORARILY DISABLED

                                // REALIGNMENT BLOCK COMPLETELY DISABLED (see needs_realignment = false above)
                                if (needs_realignment) {  // Never executes
                                // AGX FIX: Recreate frame with proper alignment
                                AVFrame* aligned_frame = av_frame_alloc();
                                if (!aligned_frame) {
                                    std::cerr << "❌ [METAL FIX] av_frame_alloc failed for alignment" << std::endl;
                                } else {
                                    aligned_frame->width = ref_frame->width;
                                    aligned_frame->height = ref_frame->height;
                                    aligned_frame->format = ref_frame->format;

                                    std::cout << "🔧 [REALIGN] Calling av_frame_get_buffer with alignment=" << METAL_LINESIZE_ALIGNMENT
                                              << ", format=" << aligned_frame->format
                                              << ", size=" << aligned_frame->width << "x" << aligned_frame->height << std::endl;

                                    // Set alignment to 64 bytes for Metal
                                    int align_ret = av_frame_get_buffer(aligned_frame, METAL_LINESIZE_ALIGNMENT);
                                    if (align_ret < 0) {
                                        char av_errbuf_align[AV_ERROR_MAX_STRING_SIZE];
                                        av_make_error_string(av_errbuf_align, AV_ERROR_MAX_STRING_SIZE, align_ret);
                                        std::cerr << "❌ [METAL FIX] av_frame_get_buffer failed: " << av_errbuf_align << std::endl;
                                        av_frame_free(&aligned_frame);
                                    } else {
                                        std::cout << "🔧 [REALIGN] Buffer allocated: Y=" << aligned_frame->linesize[0]
                                                  << ", U=" << aligned_frame->linesize[1]
                                                  << ", V=" << aligned_frame->linesize[2] << std::endl;

                                        // CRITICAL: Initialize new buffer + copy line by line
                                        // 1. Padding may contain garbage after av_frame_get_buffer
                                        // 2. av_frame_copy() copies entire linesize including garbage → green frames
                                        // SOLUTION: Fill buffer with safe values, then copy only width
                                        bool copy_success = true;

                                        // Initialize planes with safe values (for YUV: Y=0 black, UV=128 neutral)
                                        if (ref_frame->format == AV_PIX_FMT_NV12) {
                                            // NV12: Y plane (0 = black) + UV plane (128 = neutral gray)
                                            memset(aligned_frame->data[0], 0, aligned_frame->linesize[0] * ref_frame->height);
                                            memset(aligned_frame->data[1], 128, aligned_frame->linesize[1] * ref_frame->height / 2);
                                        } else if (ref_frame->format == AV_PIX_FMT_YUV420P) {
                                            // YUV420P: Y=0, U=128, V=128
                                            memset(aligned_frame->data[0], 0, aligned_frame->linesize[0] * ref_frame->height);
                                            memset(aligned_frame->data[1], 128, aligned_frame->linesize[1] * ref_frame->height / 2);
                                            memset(aligned_frame->data[2], 128, aligned_frame->linesize[2] * ref_frame->height / 2);
                                        }

                                        // Now copy real data line by line
                                        if (ref_frame->format == AV_PIX_FMT_NV12) {
                                            // NV12: Y plane + interleaved UV plane
                                            // Y plane: copy width bytes per line
                                            for (int y = 0; y < ref_frame->height; y++) {
                                                memcpy(aligned_frame->data[0] + y * aligned_frame->linesize[0],
                                                       ref_frame->data[0] + y * ref_frame->linesize[0],
                                                       ref_frame->width);
                                            }
                                            // UV plane: interleaved (UVUV...), copy width bytes (width/2 pixels × 2 bytes)
                                            for (int y = 0; y < ref_frame->height / 2; y++) {
                                                memcpy(aligned_frame->data[1] + y * aligned_frame->linesize[1],
                                                       ref_frame->data[1] + y * ref_frame->linesize[1],
                                                       ref_frame->width);  // width bytes (not width/2!)
                                            }
                                        } else if (ref_frame->format == AV_PIX_FMT_YUV420P) {
                                            // YUV420P: Y + U + V separate planes
                                            // Y plane
                                            for (int y = 0; y < ref_frame->height; y++) {
                                                memcpy(aligned_frame->data[0] + y * aligned_frame->linesize[0],
                                                       ref_frame->data[0] + y * ref_frame->linesize[0],
                                                       ref_frame->width);
                                            }
                                            // U plane (width/2 × height/2)
                                            for (int y = 0; y < ref_frame->height / 2; y++) {
                                                memcpy(aligned_frame->data[1] + y * aligned_frame->linesize[1],
                                                       ref_frame->data[1] + y * ref_frame->linesize[1],
                                                       ref_frame->width / 2);
                                            }
                                            // V plane (width/2 × height/2)
                                            for (int y = 0; y < ref_frame->height / 2; y++) {
                                                memcpy(aligned_frame->data[2] + y * aligned_frame->linesize[2],
                                                       ref_frame->data[2] + y * ref_frame->linesize[2],
                                                       ref_frame->width / 2);
                                            }
                                        } else {
                                            std::cerr << "❌ [METAL FIX] Unsupported format for line-by-line copy: " << ref_frame->format << std::endl;
                                            copy_success = false;
                                        }

                                        if (!copy_success) {
                                            av_frame_free(&aligned_frame);
                                        } else {
                                            av_frame_copy_props(aligned_frame, ref_frame);

                                            // Free old frame and use aligned one
                                            av_frame_free(&ref_frame);
                                            ref_frame = aligned_frame;

                                            static int realign_success = 0;
                                            std::cout << "✅ [METAL FIX #" << (++realign_success) << "] Realigned: "
                                                      << "Y=" << ref_frame->linesize[0]
                                                      << ", U=" << ref_frame->linesize[1]
                                                      << ", V=" << ref_frame->linesize[2]
                                                      << " (aligned to " << METAL_LINESIZE_ALIGNMENT << " bytes)" << std::endl;
                                        }
                                    }
                                }
                            }

                            // Convert yuvj420p (MJPEG full range) → yuv420p
                            // IMPORTANT: yuvj420p is FULL range variant, so preserve FULL range
                            // Metadata modification is safe with av_frame_ref (don't touch pixel data)
                            if (ref_frame->format == AV_PIX_FMT_YUVJ420P) {
                                ref_frame->format = AV_PIX_FMT_YUV420P;
                                ref_frame->color_range = AVCOL_RANGE_JPEG; // FULL (0-255) - yuvj == full range

                                // CRITICAL: Don't override colorspace/primaries/trc if they're already correct
                                // MJPEG may have correct BT709 metadata from proxy encoder
                                if (ref_frame->colorspace == AVCOL_SPC_UNSPECIFIED) {
                                    ref_frame->colorspace = AVCOL_SPC_BT709;
                                }
                                if (ref_frame->color_primaries == AVCOL_PRI_UNSPECIFIED) {
                                    ref_frame->color_primaries = AVCOL_PRI_BT709;
                                }
                                if (ref_frame->color_trc == AVCOL_TRC_UNSPECIFIED) {
                                    ref_frame->color_trc = AVCOL_TRC_BT709;
                                }
                            }
                            
                            // CRITICAL FIX: DISABLE ProcessYUV/ProcessNV12 SIMD functions
                            // These functions modify FFmpeg-managed shared buffers which causes heap corruption!
                            // - ProcessYUV420P_* and ProcessNV12_* call ProcessPlane_* which modifies ref_frame->data[]
                            // - ref_frame is created by av_frame_ref() with SHARED buffer references
                            // - Modifying shared buffers corrupts FFmpeg's memory management
                            // All SIMD processing completely disabled for stability
                            (void)FSTP::SIMDOptimizations::GetOptimalSIMDLevel(); // Suppress unused warning

                            // Log color_range for all proxy frames (first 5)
                            static int metadata_log = 0;
                            if (++metadata_log <= 5) {
                                std::cout << "🎨 [LowResDecoder] Decoded frame: "
                                         << "format=" << ref_frame->format
                                         << ", range=" << ref_frame->color_range
                                         << " (0=unspec, 1=MPEG/TV, 2=JPEG/PC)"
                                         << ", cs=" << ref_frame->colorspace
                                         << ", prim=" << ref_frame->color_primaries
                                         << ", trc=" << ref_frame->color_trc << std::endl;
                            }

                            // FIXED: Lock for assignments - use targetFrameIndex
                            std::lock_guard<std::mutex> lock(frameIndex[targetFrameIndex].mutex);

                            // MEMORY SAFETY: Additional validation before storing in shared_ptr
                            if (ref_frame && ref_frame->data[0] && ref_frame->width > 0 && ref_frame->height > 0) {
                                // OPTIMIZATION: Use appropriate deleter based on frame source
                                // - Pool frames (CPU or HW) → Release back to appropriate pool
                                // - Cloned frames → av_frame_free (standard cleanup)

                                if (from_pool) {
                                    // FAST PATH: Frame from pool → return to correct pool
                                    // CRITICAL: Capture selected_pool (not frame_pool_!) so HW frames
                                    // return to hw_frame_pool_ and CPU frames to frame_pool_
                                    AVFramePool* pool_ptr = selected_pool;

                                    frameIndex[targetFrameIndex].low_res_frame = std::shared_ptr<AVFrame>(
                                        ref_frame,
                                        [pool_ptr](AVFrame* f) {
                                            if (pool_ptr) {
                                                pool_ptr->Release(f);
                                            } else {
                                                // Fallback if pool destroyed (shutdown scenario)
                                                // CRITICAL: Direct free without mutex - mutex may be invalid during shutdown
                                                av_frame_free(&f);
                                            }
                                        }
                                    );
                                } else {
                                    // SLOW PATH: Frame from av_frame_clone → normal free
                                    frameIndex[targetFrameIndex].low_res_frame = std::shared_ptr<AVFrame>(
                                        ref_frame,
                                        [](AVFrame* f) {
                                            // CRITICAL: Direct free without mutex during shutdown
                                            // Mutex may be destroyed before shared_ptr deleters run
                                            av_frame_free(&f);
                                        }
                                    );
                                }
                                frameIndex[targetFrameIndex].pts = framePts;
                                // CRITICAL: relative_pts must be RELATIVE (as in old code)
                                // Subtract startTime for consistency with SimpleVideoIndex and audio
                                frameIndex[targetFrameIndex].relative_pts = framePts - startTime;
                                frameIndex[targetFrameIndex].time_base = timeBase;
                                frameIndex[targetFrameIndex].type = FrameInfo::LOW_RES;
                                frameIndex[targetFrameIndex].format = static_cast<AVPixelFormat>(frame->format);
                                // CRITICAL: Use ROUNDING for time_ms (as in old decode.cpp)
                                // frameTimeMs is double with fractional precision, round to nearest int64_t
                                // This eliminates jitter at 29.97fps and irregular GOPs
                                if (frameTimeMs >= 0) {
                                    frameIndex[targetFrameIndex].time_ms = static_cast<int64_t>(std::round(frameTimeMs));
                                }
                                frameIndex[targetFrameIndex].is_ready.store(true);
                                frameIndex[targetFrameIndex].is_decoding.store(false);
                            } else {
                                std::cerr << "❌ [LowResDecoder] Invalid frame data, skipping storage" << std::endl;
                                if (ref_frame) {
                                    // Direct free on error path (no mutex needed - synchronous cleanup)
                                    av_frame_free(&ref_frame);
                                }
                                ref_frame = nullptr;
                                // Reset frame info to empty state
                                frameIndex[targetFrameIndex].type = FrameInfo::EMPTY;
                                frameIndex[targetFrameIndex].is_ready.store(false);
                                frameIndex[targetFrameIndex].is_decoding.store(false);
                            }

                            // DIAGNOSTICS: Output first 20 frames for timing debugging
                            if (ENABLE_LOWRES_DECODER_DEBUG) {
                                static int timing_debug_count = 0;
                                if (timing_debug_count < 20) {
                                    double time_seconds = frameTimeMs / 1000.0;
                                    std::cout << "🔍 [LowRes TIMING] Frame " << currentFrame
                                              << ": frame->pts=" << frame->pts
                                              << ", frame->best_effort=" << frame->best_effort_timestamp
                                              << ", framePts=" << framePts
                                              << ", frameTimeMs=" << frameTimeMs
                                              << " (" << time_seconds << "s)"
                                              << ", timeBase=" << timeBase.num << "/" << timeBase.den
                                              << ", startTime=" << startTime << std::endl;
                                    timing_debug_count++;
                                }
                            }

                            // End-to-end timing for frame
                            auto t_frame_end = std::chrono::high_resolution_clock::now();
                            total_frame_processing_us += std::chrono::duration_cast<std::chrono::microseconds>(t_frame_end - t_frame_start).count();

                            perf_samples++;

                            // Output profiling every 100 frames
                            if (ENABLE_LOWRES_DECODER_DEBUG) {
                                if (++perf_report_counter >= 100) {
                                    int samples = perf_samples;  // Local copy for safety
                                    if (samples > 0) {
                                        auto now = std::chrono::high_resolution_clock::now();
                                        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - segment_start_time).count();
                                        double fps = (elapsed_ms > 0) ? (samples * 1000.0 / elapsed_ms) : 0.0;

                                        uint64_t avg_read = total_read_frame_us / samples;
                                        uint64_t avg_send = total_send_packet_us / samples;
                                        uint64_t avg_receive = total_receive_frame_us / samples;
                                        uint64_t avg_ref = total_frame_ref_us / samples;
                                        uint64_t avg_hw_transfer = (total_hw_transfer_us > 0) ? (total_hw_transfer_us / samples) : 0;
                                        uint64_t avg_total = total_frame_processing_us / samples;

                                        std::cout << "⏱️  [LowResDecoder PERF] Thread " << std::this_thread::get_id() << std::endl;
                                        std::cout << "    📊 Breakdown per frame (avg over " << samples << " frames):" << std::endl;
                                        std::cout << "       • Read packet:     " << avg_read << "μs" << std::endl;
                                        std::cout << "       • Send (decode):   " << avg_send << "μs ⬅️ BOTTLENECK" << std::endl;
                                        std::cout << "       • Receive:         " << avg_receive << "μs" << std::endl;
                                        if (avg_hw_transfer > 0) {
                                            std::cout << "       • GPU→CPU copy:    " << avg_hw_transfer << "μs" << std::endl;
                                        }
                                        std::cout << "       • Frame ref:       " << avg_ref << "μs (zero-copy)" << std::endl;
                                        std::cout << "       • TOTAL per frame: " << avg_total << "μs ("
                                                  << (avg_total / 1000.0) << "ms)" << std::endl;
                                        std::cout << "    🚀 Throughput:         " << std::fixed << std::setprecision(1)
                                                  << fps << " fps" << std::endl;
                                    }
                                    perf_report_counter = 0;
                                    total_read_frame_us = 0;
                                    total_send_packet_us = 0;
                                    total_receive_frame_us = 0;
                                    total_frame_ref_us = 0;
                                    total_hw_transfer_us = 0;
                                    total_frame_processing_us = 0;
                                    perf_samples = 0;
                                    segment_start_time = std::chrono::high_resolution_clock::now();
                                }
                            }

                            if (ENABLE_LOWRES_DECODER_DEBUG) {
                                static int debug_count = 0;
                                if (debug_count++ < 10) {
                                    std::cout << "[LowResDecoder] Thread " << threadId << " stored frame " << currentFrame
                                              << " (frameTimeMs=" << frameTimeMs << "ms, seekTarget=" << seekTargetTimeMs << "ms)" << std::endl;
                                }
                            }
                            } // Close if (ref_frame) block
                        } else {
                            // Ref failed (av_frame_alloc or av_frame_ref)
                            std::cerr << "[Thread " << threadId << "] LowResDecoder: Failed to ref AVFrame for index " << currentFrame << ". Resetting slot." << std::endl;
                            std::lock_guard<std::mutex> lock(frameIndex[currentFrame].mutex); // Lock to reset
                            frameIndex[currentFrame].low_res_frame.reset();
                            if (frameIndex[currentFrame].type != FrameInfo::FULL_RES) {
                                frameIndex[currentFrame].type = FrameInfo::EMPTY;
                            }
                            frameIndex[currentFrame].is_decoding.store(false);
                        }
                    }

                    // FIXED FOR B-FRAMES: Increment considering targetFrameIndex
                    // With B-frames targetFrameIndex can differ from currentFrame (frames arrive out of order)
                    // Therefore after processing frame currentFrame should continue from targetFrameIndex + 1
                    if (stored_this_frame_attempt) {
                        currentFrame = targetFrameIndex + 1;
                    }
                    // --- End Frame Storage Logic ---

                    av_frame_unref(frame); // Unref frame inside receive loop

                    // Check if this thread's work is done
                    if (currentFrame > threadEndFrame) {
                        FSTP_PROFILE_DECODE_END("lowres");
                        goto thread_decode_loop_end;
                        }
                    } else {
                        break;
                    }
                }
                FSTP_PROFILE_DECODE_END("lowres");
            }
            av_packet_unref(packet);
        }

    thread_decode_loop_end:

        // OPTIMIZATION: DO NOT close context - it will be reused!
        // Cleanup only temporary packet/frame
        av_frame_free(&frame);
        av_packet_free(&packet);

        // Flush decoder buffers for next segment (cleanup state)
        avcodec_flush_buffers(codecContext);

        static int thread_done_count = 0;
        if (++thread_done_count % 10 == 0) {
            std::cout << "🔄 [Thread " << threadId << "] Segment done, context REUSED (no overhead!)" << std::endl;
        }
    };

    int totalFramesInRange = endFrame - startFrame + 1;

        if (isReverse) {
            // ============================================================
            // REVERSE: 3-PART PRIORITY DECODING FOR 32X OPTIMIZATION
            // ============================================================
            // Problem: Playhead moves backward fast, linear decoding is too slow
            // Solution: Decode where playhead IS first, then rest
            //
            // Split segment into 3 parts:
            // [-- Part 1 --][-- Part 2 --][-- Part 3 --]
            //                                ↑ playhead here!
            //
            // STEP 1: Decode Part 3 (last third) with 2 threads ← URGENT!
            // STEP 2: Decode Part 1+2 (first two thirds) with 2 threads
            // ============================================================

            int segmentSize = totalFramesInRange;
            int part1End = startFrame + (segmentSize / 3) - 1;
            int part2End = startFrame + (2 * segmentSize / 3) - 1;
            int part3Start = part2End + 1;

            static int reverse_log = 0;
            if (reverse_log++ < 3) {
                std::cout << "⏪ [REVERSE 3-PART] Segment [" << startFrame << "-" << endFrame << "]" << std::endl;
                std::cout << "   Part 1: [" << startFrame << "-" << part1End << "]" << std::endl;
                std::cout << "   Part 2: [" << (part1End + 1) << "-" << part2End << "]" << std::endl;
                std::cout << "   Part 3: [" << part3Start << "-" << endFrame << "] ← DECODE FIRST!" << std::endl;
            }

            // STEP 1: Decode Part 3 (high priority - where playhead is)
            int part3Mid = part3Start + (endFrame - part3Start) / 2;
            threads.emplace_back(decodeSegment, 0, part3Start, part3Mid);
            threads.emplace_back(decodeSegment, 1, part3Mid + 1, endFrame);

            // Wait for Part 3 to complete
            for (auto& t : threads) {
                if (t.joinable()) t.join();
            }
            threads.clear();

            // STEP 2: Decode Part 1+2 (lower priority)
            threads.emplace_back(decodeSegment, 0, startFrame, part1End);
            threads.emplace_back(decodeSegment, 1, part1End + 1, part2End);

    } else {
        // ========== FORWARD: Normal decoding (works great) ==========
        int framesPerThread = std::max(1, totalFramesInRange / numThreads);
        int start = startFrame;

        for (int i = 0; i < numThreads; ++i) {
            int threadStart = start;
            int threadEnd = (i == numThreads - 1) ? endFrame : std::min(endFrame, start + framesPerThread - 1);
            if (threadStart > threadEnd) break;

            threads.emplace_back(decodeSegment, i, threadStart, threadEnd);
            start = threadEnd + 1;
            if (start > endFrame) break;
        }
    }

    for (auto& t : threads) {
        if (t.joinable()) {
            t.join();
        }
    }

    // DEBUG: Count filled vs empty slots to verify 30fps proxy behavior
    static int segment_count = 0;
    if (++segment_count <= 5) {
        int filled = 0, empty = 0;
        for (int i = startFrame; i <= endFrame && i < static_cast<int>(frameIndex.size()); ++i) {
            if (frameIndex[i].type == FrameInfo::LOW_RES) filled++;
            else if (frameIndex[i].type == FrameInfo::EMPTY) empty++;
        }
        std::cout << "📊 [LowResDecoder] Segment [" << startFrame << "-" << endFrame << "]: "
                  << filled << " filled, " << empty << " empty slots"
                  << " (" << (filled * 100 / (endFrame - startFrame + 1)) << "% coverage)" << std::endl;
    }

    is_decoding_ = false;
    return success.load();
}

bool LowResDecoder::convertToLowRes(const std::string& filename,
                                    std::string& outputFilename,
                                    const std::function<void(int)>& progressCallback) {
    fs::path cacheDir = fs::path(getCachePath()) / "proxy";
    fs::create_directories(cacheDir);

    std::string fileId = generateFileId(filename);
    if (fileId.empty()) {
        return false;
    }

    // CRITICAL: ALWAYS analyze GOP regardless of resolution/codec
    // When GOP > 100 ALWAYS create proxy with GOP=25 for responsive scrubbing
    int max_gop_size = 0;
    int width = 0;
    int height = 0;
    bool is_h264 = false;
    int64_t video_bitrate = 0;  // Video bitrate in bps

    {
        AVFormatContext* fmt = nullptr;
        if (avformat_open_input(&fmt, filename.c_str(), nullptr, nullptr) == 0 &&
            avformat_find_stream_info(fmt, nullptr) >= 0) {
            int vindex = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
            if (vindex >= 0) {
                AVCodecParameters* cp = fmt->streams[vindex]->codecpar;
                width = cp->width;
                height = cp->height;
                is_h264 = (cp->codec_id == AV_CODEC_ID_H264);

                // Get video bitrate (for detecting low-bitrate surveillance cameras)
                video_bitrate = cp->bit_rate;
                if (video_bitrate <= 0) {
                    // Fallback: estimate from file size and duration
                    if (fmt->duration > 0 && fmt->bit_rate > 0) {
                        video_bitrate = fmt->bit_rate * 0.8;  // Assume 80% is video
                    }
                }

                // CRITICAL: Analyze GOP for ANY file
                AVPacket* packet = av_packet_alloc();
                int current_gop = 0;
                int frames_analyzed = 0;
                const int MAX_FRAMES_TO_ANALYZE = 1000; // Analyze first 1000 frames

                while (frames_analyzed < MAX_FRAMES_TO_ANALYZE && av_read_frame(fmt, packet) >= 0) {
                    if (packet->stream_index == vindex) {
                        if (packet->flags & AV_PKT_FLAG_KEY) {
                            if (current_gop > 0) {
                                max_gop_size = std::max(max_gop_size, current_gop);
                            }
                            current_gop = 0;
                        }
                        current_gop++;
                        frames_analyzed++;
                    }
                    av_packet_unref(packet);
                }
                av_packet_free(&packet);
            }
            avformat_close_input(&fmt);
        }
    }

    // CRITICAL: Check GOP BEFORE checking resolution/codec
    // PENTIUM GOLD 7505 OPTIMIZATION: ALWAYS create proxy (never use original)
    bool is_pentium_7505 = false;
    if (g_hardware_detection != nullptr) {
        is_pentium_7505 = g_hardware_detection->GetCPUInfo().is_pentium_gold_7505;
    }

    if (is_pentium_7505) {
        std::cout << "🎯 [Pentium 7505] ALWAYS creating 360p GOP=25 proxy (original disabled)" << std::endl;
        // Continue creating proxy below
    } else if (max_gop_size > 50) {
        std::cout << "⚠️  [LowResDecoder] Large GOP detected (" << max_gop_size
                  << " frames) - creating GOP=25 proxy for responsive scrubbing" << std::endl;
        // Continue creating proxy below
    } else {
        // GOP is adequate (≤50) - check if we can use original
        bool is_low_res = (width * height) <= (720 * 480);

        // macOS OPTIMIZATION: Use low-bitrate Full HD videos directly (surveillance cameras)
        // VideoToolbox can decode these efficiently, no need to create smaller proxy
#ifdef __APPLE__
        bool is_low_bitrate_hd = false;
        if (is_h264 && video_bitrate > 0 && video_bitrate <= 2000000) {  // ≤2 Mbps
            is_low_bitrate_hd = true;
            std::cout << "📹 [macOS] Low-bitrate surveillance video detected: "
                      << width << "x" << height << " @ " << (video_bitrate / 1000) << " kbps" << std::endl;
        }

        if (is_h264 && max_gop_size > 0 && (is_low_res || is_low_bitrate_hd)) {
#else
        if (is_h264 && is_low_res && max_gop_size > 0) {
#endif
            outputFilename = filename;
            if (progressCallback) {
                progressCallback(100);
            }

#ifdef __APPLE__
            if (is_low_bitrate_hd && !is_low_res) {
                std::cout << "✅ [macOS/VideoToolbox] Using low-bitrate original as proxy (" << width << "x" << height
                          << ", " << (video_bitrate / 1000) << " kbps, GOP=" << max_gop_size << ")" << std::endl;
            } else {
#endif
                std::cout << "✅ [LowResDecoder] Original is already ≤480p H.264 (" << width << "x" << height
                          << ", GOP=" << max_gop_size << ") - using directly as proxy" << std::endl;
#ifdef __APPLE__
            }
#endif
            return true;
        }
        // Otherwise create proxy for scaling
    }

    // H.264 proxy ONLY (hardware decoding VideoToolbox)
    fs::path h264Path = cacheDir / (fileId + "_lowres.mp4");

    if (fs::exists(h264Path)) {
        outputFilename = h264Path.string();
        if (progressCallback) {
            progressCallback(100);
        }
        std::cout << "✅ Using H.264 proxy (GPU): " << h264Path << std::endl;
        return true;
    }

    double totalDuration = QueryVideoDuration(filename);

    auto reportProgress = [&](int percent) {
        static int lastReportedPercent = -1;
        // Output progress only on 5% change to reduce spam
        if (percent - lastReportedPercent >= 5 || percent == 0 || percent == 100) {
            std::cout << "🎬 [Proxy] Creating GOP=25 proxy: " << percent << "%" << std::endl;
            lastReportedPercent = percent;
        }
        if (progressCallback) {
            progressCallback(percent);
        }
    };

    reportProgress(0);

    // Parse one output line from ffmpeg -progress pipe:1 and update progress bar
    auto processLine = [&](const std::string& line) {
        if (!progressCallback) return;
        size_t timePos = line.find("out_time_ms=");
        if (timePos == std::string::npos) return;
        try {
            int64_t time_us = std::stoll(line.substr(timePos + 12));
            double currentTime = time_us / 1000000.0;
            if (totalDuration > 0.0 && currentTime > 0.0) {
                int percent = static_cast<int>((currentTime / totalDuration) * 100.0);
                percent = std::max(0, std::min(100, percent));
                reportProgress(percent);
            }
        } catch (...) {}
    };

    auto executeConversion = [&](const std::string& command, const fs::path& destination) -> bool {
        int status = 0;

#ifdef _WIN32
        // Use CreateProcessW: handles Unicode paths natively, no cmd.exe ANSI conversion
        status = RunCommandW(command, processLine);
        if (status == -1) {
            std::cerr << "[LowResDecoder] Failed to launch command (CreateProcessW): " << command << std::endl;
            return false;
        }
#else
        FILE* pipe = popen(command.c_str(), "r");
        if (!pipe) {
            std::cerr << "[LowResDecoder] Failed to execute command: " << command << std::endl;
            return false;
        }
        char buffer[512];
        std::string lineBuffer;
        while (fgets(buffer, sizeof(buffer), pipe)) {
            lineBuffer += buffer;
            size_t pos;
            while ((pos = lineBuffer.find('\n')) != std::string::npos) {
                processLine(lineBuffer.substr(0, pos));
                lineBuffer.erase(0, pos + 1);
            }
        }
        status = pclose(pipe);
#endif

        if (status != 0) {
            std::cerr << "[LowResDecoder] FFmpeg command failed with status " << status << std::endl;
            if (fs::exists(destination)) fs::remove(destination);
            return false;
        }

        reportProgress(100);
        return true;
    };

    // IMPORTANT: fix color matrix/range in proxy to match main stream (TV range, BT.709)
    // Determine source color metadata via libav so proxy matches exactly
    std::string cs_flag = "bt709";
    std::string prim_flag = "bt709";
    std::string trc_flag = "bt709";
    std::string range_flag = "tv"; // limited by default

    // OPTIMIZATION: Anamorphic proxy for 25% less pixels to decode
    // Store original DAR in SAR to maintain correct aspect ratio
    int orig_width = 0;
    int orig_height = 0;
    AVRational orig_sar = {1, 1};
    AVRational orig_dar = {16, 9};  // Default 16:9

    // Get original FPS for logging
    double orig_fps = 0.0;

    {
        AVFormatContext* fmt = nullptr;
        if (avformat_open_input(&fmt, filename.c_str(), nullptr, nullptr) == 0 &&
            avformat_find_stream_info(fmt, nullptr) >= 0) {
            int vindex = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
            if (vindex >= 0) {
                AVCodecParameters* cp = fmt->streams[vindex]->codecpar;
                AVStream* stream = fmt->streams[vindex];

                // Get original FPS from r_frame_rate (more reliable than avg_frame_rate)
                if (stream->r_frame_rate.den > 0) {
                    orig_fps = static_cast<double>(stream->r_frame_rate.num) / stream->r_frame_rate.den;
                }

                // Capture original dimensions and aspect ratio
                orig_width = cp->width;
                orig_height = cp->height;
                orig_sar = cp->sample_aspect_ratio;

                // If SAR is invalid, assume square pixels
                if (orig_sar.num <= 0 || orig_sar.den <= 0) {
                    orig_sar = {1, 1};
                }

                // Calculate DAR = (width/height) × SAR
                // Using rational math: DAR = (width × SAR.num) / (height × SAR.den)
                orig_dar.num = orig_width * orig_sar.num;
                orig_dar.den = orig_height * orig_sar.den;

                // Simplify the rational using simple GCD
                int dar_gcd = 1;
                for (int i = 1; i <= orig_dar.num && i <= orig_dar.den; ++i) {
                    if (orig_dar.num % i == 0 && orig_dar.den % i == 0) {
                        dar_gcd = i;
                    }
                }
                orig_dar.num /= dar_gcd;
                orig_dar.den /= dar_gcd;
                // colorspace
                switch (cp->color_space) {
                    case AVCOL_SPC_BT470BG:
                    case AVCOL_SPC_SMPTE170M:
                        cs_flag = "bt470bg"; break; // 601
                    case AVCOL_SPC_BT709:
                        cs_flag = "bt709"; break;
                    case AVCOL_SPC_BT2020_NCL:
                    case AVCOL_SPC_BT2020_CL:
                        cs_flag = "bt2020nc"; break;
                    default: break;
                }
                // primaries
                switch (cp->color_primaries) {
                    case AVCOL_PRI_BT470BG:
                    case AVCOL_PRI_SMPTE170M:
                        prim_flag = "bt470bg"; break;
                    case AVCOL_PRI_BT709:
                        prim_flag = "bt709"; break;
                    case AVCOL_PRI_BT2020:
                        prim_flag = "bt2020"; break;
                    default: break;
                }
                // transfer
                switch (cp->color_trc) {
                    case AVCOL_TRC_BT709:
                        trc_flag = "bt709"; break;
                    case AVCOL_TRC_SMPTE170M:
                        trc_flag = "smpte170m"; break;
                    case AVCOL_TRC_IEC61966_2_1:
                        trc_flag = "iec61966-2-1"; break; // sRGB
                    case AVCOL_TRC_BT2020_10:
                    case AVCOL_TRC_BT2020_12:
                        trc_flag = "bt2020-10"; break;
                    default: break;
                }
                // range
                // CRITICAL: Use same range as original for identical brightness/contrast
                // Most videos are in TV/limited range (16-235)
                if (cp->color_range == AVCOL_RANGE_JPEG) {
                    range_flag = "pc"; // full range (0-255)
                } else {
                    range_flag = "tv"; // limited range (16-235) - like original
                }
            }
            avformat_close_input(&fmt);
        }
    }

    // Calculate proxy dimensions maintaining original aspect ratio
    // Target height: 360p, width adjusted to preserve DAR with square pixels (SAR 1:1)
    //
    // DAR = (orig_width / orig_height) × (orig_sar_num / orig_sar_den)
    // proxy_width = proxy_height × DAR
    //
    // Examples:
    // - 1920×1080 SAR 1:1 (16:9) → 640×360 (normal)
    // - 1920×1080 SAR 1:1 (16:9) → 426×240 (half-fps anamorphic, Pentium 7505)
    // - 1440×1080 SAR 1:1 (4:3) → 480×360 (normal)
    // - 1920×1080 SAR 32:27 (anamorphic) → calculated from DAR

    // STANDARD MODE: 360p with square pixels
    int proxy_height = 360;
    int proxy_width;

    // Calculate display aspect ratio (DAR) from original dimensions and SAR
    double dar = (static_cast<double>(orig_width) / static_cast<double>(orig_height)) *
                 (static_cast<double>(orig_sar.num) / static_cast<double>(orig_sar.den));

    // Calculate proxy width to maintain DAR with square pixels
    proxy_width = static_cast<int>(std::round(proxy_height * dar));

    // Ensure even dimensions (required for H.264)
    if (proxy_width % 2 != 0) proxy_width++;

    std::cout << "📐 [Proxy] Original: " << orig_width << "x" << orig_height
              << " (SAR " << orig_sar.num << ":" << orig_sar.den
              << " → DAR " << orig_dar.num << ":" << orig_dar.den << ")" << std::endl;
    std::cout << "📐 [Proxy] Target: " << proxy_width << "x" << proxy_height
              << " (360p square pixels, DAR " << std::fixed << std::setprecision(2) << dar << ":1, GOP=25)" << std::endl;

    // H.264 generation ONLY (hardware decoding VideoToolbox)
    // CRITICAL: Fix GOP=25 for responsive reverse playback!
    // Use Baseline profile WITHOUT B-frames for fastest decode (matches working 50fps proxy)
    std::string x264_params = "colorprimaries=" + prim_flag + ":transfer=" + trc_flag + ":colormatrix=" +
                              (cs_flag == "bt2020nc" ? std::string("bt2020nc") : cs_flag) +
                              (range_flag == "pc" ? ":fullrange=on" : ":fullrange=off") +
                              ":keyint=25:min-keyint=25";  // GOP=25, no B-frames for simplicity

    // Build filter chain: scale with square pixels, convert to yuv420p
    std::string vfilter = "scale=" + std::to_string(proxy_width) + ":" + std::to_string(proxy_height) +
                          ",format=yuv420p";

    // AUDIO: Copy audio stream if exists, otherwise create silent track
    // This allows proxy to be used standalone (without original file)
    // -progress pipe:1: Output progress to stdout for parsing (out_time_ms format)
    // CRITICAL FIX: On macOS, use AudioToolbox AAC decoder for AAC input (handles AAC-ELD)
#ifdef __APPLE__
    std::string audio_decoder = "-c:a aac_at";  // AudioToolbox AAC decoder on macOS
#else
    std::string audio_decoder = "";  // Use default decoder on other platforms
#endif

#ifdef _WIN32
    // On Windows: CreateProcessW handles stdout+stderr via pipe — no shell, no "2>&1" needed
    std::string h264Command = "ffmpeg " + audio_decoder + " -nostdin -y -progress pipe:1 -i \"" + filename +
                              "\" -vf \"" + vfilter + "\" -colorspace " + cs_flag +
                              " -color_primaries " + prim_flag + " -color_trc " + trc_flag +
                              " -color_range " + range_flag +
                              " -c:v libx264 -profile:v baseline -preset medium -g 25 -b:v 600k -x264-params \"" +
                              x264_params + "\" -c:a aac -b:a 128k \"" + h264Path.string() + "\"";
#else
    // On Mac/Linux: popen() uses a shell, "2>&1" merges stderr into stdout for progress parsing
    std::string h264Command = "ffmpeg " + audio_decoder + " -nostdin -y -progress pipe:1 -i \"" + filename +
                              "\" -vf \"" + vfilter + "\" -colorspace " + cs_flag +
                              " -color_primaries " + prim_flag + " -color_trc " + trc_flag +
                              " -color_range " + range_flag +
                              " -c:v libx264 -profile:v baseline -preset medium -g 25 -b:v 600k -x264-params \"" +
                              x264_params + "\" -c:a aac -b:a 128k \"" + h264Path.string() + "\" 2>&1";
#endif

    std::cout << "🎬 [Proxy] Starting conversion with GOP=25 for responsive scrubbing..." << std::endl;
    std::cout << "    Duration: " << std::fixed << std::setprecision(1) << totalDuration << "s" << std::endl;

    if (executeConversion(h264Command, h264Path)) {
        outputFilename = h264Path.string();
        std::cout << "✅ [Proxy] Generated H.264 GOP=25 proxy: " << h264Path << std::endl;
        return true;
    }

    return false;
}

bool LowResDecoder::isInitialized() const {
    return initialized_;
}

int LowResDecoder::getWidth() const {
    return width_;
}

int LowResDecoder::getHeight() const {
    return height_;
}

AVPixelFormat LowResDecoder::getPixelFormat() const {
    return pixFmt_;
}

} // namespace FSTP
