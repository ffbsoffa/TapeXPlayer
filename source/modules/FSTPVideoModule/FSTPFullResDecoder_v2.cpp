#include "FSTPFullResDecoder_v2.h"
#include "FSTPPerformanceProfiler.h"
#include "FSTPVideoOptimizations.h"
#include "FSTPHardwareDetection.h"  // NEW: Unified hardware detection
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <chrono>
#include <atomic>
#include <thread>
#include <pthread.h>
#ifndef _WIN32
#include <sys/resource.h>
#endif

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/thread_info.h>
#elif defined(__linux__)
#include <time.h>
#endif

// Debug control: set to true to enable verbose logging
static constexpr bool ENABLE_FULLRES_V2_DEBUG = false;
static constexpr bool ENABLE_FULLRES_V2_PROFILING = false;  // CPU/Buffer statistics every 100 iterations

// Global counter of active V2 decoders (for multi-instance adaptation)
static std::atomic<int> g_active_decoder_count{0};

// Helper: Get real CPU time of current thread (user + system) in microseconds
static uint64_t GetThreadCPUTimeMicroseconds() {
#ifdef __APPLE__
    mach_port_t thread = mach_thread_self();
    thread_basic_info_data_t info;
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;

    if (thread_info(thread, THREAD_BASIC_INFO, (thread_info_t)&info, &count) == KERN_SUCCESS) {
        uint64_t user_us = info.user_time.seconds * 1000000ULL + info.user_time.microseconds;
        uint64_t sys_us = info.system_time.seconds * 1000000ULL + info.system_time.microseconds;
        mach_port_deallocate(mach_task_self(), thread);
        return user_us + sys_us;
    }

    mach_port_deallocate(mach_task_self(), thread);
    return 0;
#elif defined(__linux__)
    // Linux implementation using clock_gettime
    struct timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0) {
        return ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL;
    }
    return 0;
#else
    // Fallback for other platforms
    return 0;
#endif
}

FSTPFullResDecoderV2::FSTPFullResDecoderV2(const std::string& sourceFilename, double initial_time)
    : source_filename_(sourceFilename)
    , initialized_(false)
    , stop_requested_(false)
    , thread_running_(false)
    , native_width_(0)
    , native_height_(0)
    , display_width_(0)
    , display_height_(0)
    , duration_(0.0)
    , frame_rate_(0.0)
    , pix_fmt_(AV_PIX_FMT_NONE)
    , format_ctx_(nullptr)
    , codec_ctx_(nullptr)
    , codec_params_(nullptr)
    , video_stream_(nullptr)
    , video_stream_index_(-1)
    , hw_accel_enabled_(false)
    , hw_device_ctx_(nullptr)
    , hw_pix_fmt_(AV_PIX_FMT_NONE)
    , downscale_ctx_(nullptr)
    , current_playback_time_(0.0)
{
    // Increment counter of active decoders
    int decoder_count = ++g_active_decoder_count;

    std::cout << "🎬 [FULL-RES V2] Creating streaming decoder for: " << sourceFilename
              << " (active decoders: " << decoder_count << ")" << std::endl;

    // MULTI-INSTANCE ADAPTATION: reduce buffer when multiple players
    if (decoder_count >= 2) {
        buffer_size_target_.store(12);  // Reduced buffer for 2+ players (~0.5 sec)
        std::cout << "📉 [FULL-RES V2] Reduced buffer size to 12 frames (multi-instance mode)" << std::endl;
    }

    initialized_ = Initialize();

    if (initialized_) {
        // ADAPTIVE FPS: Adjust buffer windows based on video frame rate
        UpdateBufferWindowsForFPS();
        
        // Start background decoding thread
        thread_running_.store(true);
        decoding_thread_ = std::thread(&FSTPFullResDecoderV2::DecodingThreadLoop, this);
        std::cout << "✅ [FULL-RES V2] Background decoding thread started" << std::endl;

        // IMPORTANT: Decode first frame immediately on load!
        // Start at initial_time (resume position) instead of always 0.0
        SetPlaybackTime(initial_time);
        std::cout << "📸 [FULL-RES V2] Triggered initial frame decoding at time " << initial_time << std::endl;

        // OPTIMIZATION: Yield to give the decoding thread a chance to run immediately
        std::this_thread::yield();

        // WAITING: Wait until minimum buffer is ready (maximum 500ms)
        // Wait for at least 3 frames to prevent jitter when switching from proxy to full-res
        auto wait_start = std::chrono::steady_clock::now();
        bool buffer_ready = false;
        const int max_wait_ms = (frame_rate_ >= 50.0) ? 800 : 800;  // Extra margin for software-decoded codecs (AV1, VP9)
        const size_t min_frames_for_smooth_start = (frame_rate_ >= 50.0) ? 5 : 10;  // 10 frames (~333ms at 30fps) before starting

        while (!buffer_ready) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - wait_start).count();

            if (elapsed > max_wait_ms) {
                std::cout << "⚠️  [FULL-RES V2] Buffer not ready after " << max_wait_ms
                          << "ms, continuing anyway" << std::endl;
                break;
            }

            // Check buffer has minimum frames
            {
                std::lock_guard<std::mutex> lock(buffer_mutex_);
                if (stream_buffer_.size() >= min_frames_for_smooth_start) {
                    buffer_ready = true;
                    std::cout << "✅ [FULL-RES V2] Buffer ready with " << stream_buffer_.size()
                              << " frames after " << elapsed << "ms" << std::endl;
                    break;
                }
            }

            // OPTIMIZATION: Check frequently for responsive startup
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    }
}

FSTPFullResDecoderV2::~FSTPFullResDecoderV2() {
    // Decrement counter of active decoders
    int decoder_count = --g_active_decoder_count;
    std::cout << "🎬 [FULL-RES V2] Destroying streaming decoder (remaining: "
              << decoder_count << ")" << std::endl;
    Cleanup();
}

bool FSTPFullResDecoderV2::Initialize() {
    if (initialized_) return true;

    std::cout << "🎬 [FULL-RES V2] Initializing streaming decoder..." << std::endl;

    // Open video file
    if (avformat_open_input(&format_ctx_, source_filename_.c_str(), nullptr, nullptr) < 0) {
        std::cerr << "❌ [FULL-RES V2] Failed to open file: " << source_filename_ << std::endl;
        return false;
    }

    if (avformat_find_stream_info(format_ctx_, nullptr) < 0) {
        std::cerr << "❌ [FULL-RES V2] Failed to find stream info" << std::endl;
        avformat_close_input(&format_ctx_);
        return false;
    }

    // Find video stream
    video_stream_index_ = av_find_best_stream(format_ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_stream_index_ < 0) {
        std::cerr << "❌ [FULL-RES V2] No video stream found" << std::endl;
        avformat_close_input(&format_ctx_);
        return false;
    }

    video_stream_ = format_ctx_->streams[video_stream_index_];
    codec_params_ = video_stream_->codecpar;

    // Get metadata
    native_width_ = codec_params_->width;
    native_height_ = codec_params_->height;
    pix_fmt_ = static_cast<AVPixelFormat>(codec_params_->format);

    // Calculate duration
    if (format_ctx_->duration != AV_NOPTS_VALUE) {
        duration_ = static_cast<double>(format_ctx_->duration) / AV_TIME_BASE;
    } else if (video_stream_->duration != AV_NOPTS_VALUE) {
        duration_ = static_cast<double>(video_stream_->duration) * av_q2d(video_stream_->time_base);
    }

    // Calculate frame rate
    AVRational fr = av_guess_frame_rate(format_ctx_, video_stream_, nullptr);
    if (fr.num > 0 && fr.den > 0) {
        frame_rate_ = av_q2d(fr);
    } else {
        frame_rate_ = 25.0; // Fallback
    }

    // FFPLAY STYLE: Keep original resolution (downscale does renderer)
    display_width_ = native_width_;
    display_height_ = native_height_;
    std::cout << "📐 [FULL-RES V2] Native resolution (no CPU downscale): " << display_width_ << "x" << display_height_ << std::endl;

    // Find decoder
    const AVCodec* codec = avcodec_find_decoder(codec_params_->codec_id);
    if (!codec) {
        std::cerr << "❌ [FULL-RES V2] Codec not found" << std::endl;
        avformat_close_input(&format_ctx_);
        return false;
    }

    // Create codec context
    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) {
        std::cerr << "❌ [FULL-RES V2] Failed to allocate codec context" << std::endl;
        avformat_close_input(&format_ctx_);
        return false;
    }

    if (avcodec_parameters_to_context(codec_ctx_, codec_params_) < 0) {
        std::cerr << "❌ [FULL-RES V2] Failed to copy codec parameters" << std::endl;
        avcodec_free_context(&codec_ctx_);
        avformat_close_input(&format_ctx_);
        return false;
    }

    // Try hardware acceleration
    hw_accel_enabled_ = InitializeHardwareAcceleration(codec);

    // Open codec
    if (avcodec_open2(codec_ctx_, codec, nullptr) < 0) {
        std::cerr << "❌ [FULL-RES V2] Failed to open codec" << std::endl;
        avcodec_free_context(&codec_ctx_);
        avformat_close_input(&format_ctx_);
        return false;
    }

    std::cout << "✅ [FULL-RES V2] Initialized successfully" << std::endl;
    std::cout << "   Native: " << native_width_ << "x" << native_height_ << std::endl;
    std::cout << "   Display: " << display_width_ << "x" << display_height_ << std::endl;
    std::cout << "   Duration: " << duration_ << "s, FPS: " << frame_rate_ << std::endl;
    std::cout << "   HW Accel: " << (hw_accel_enabled_ ? "YES" : "NO") << std::endl;

    return true;
}

void FSTPFullResDecoderV2::UpdateBufferWindowsForFPS() {
    // ADAPTIVE FPS: Scale buffer windows based on frame rate
    // High FPS videos need larger buffers to handle frame density
    if (frame_rate_ >= 55.0) {
        // 60 FPS: Need 4.0s ahead (~240 frames) and 2.5s behind (~150 frames)
        buffer_window_ahead_ = 4.0;
        buffer_window_behind_ = 2.5;
        std::cout << "🎬 [FULL-RES V2] High FPS detected (" << frame_rate_ 
                  << "), buffer windows: ahead=" << buffer_window_ahead_ 
                  << "s, behind=" << buffer_window_behind_ << "s" << std::endl;
    } else if (frame_rate_ >= 45.0) {
        // ~50 FPS: Moderate increase
        buffer_window_ahead_ = 3.5;
        buffer_window_behind_ = 2.2;
        std::cout << "🎬 [FULL-RES V2] Medium-high FPS (" << frame_rate_ 
                  << "), buffer windows: ahead=" << buffer_window_ahead_ 
                  << "s, behind=" << buffer_window_behind_ << "s" << std::endl;
    } else {
        // Standard 24-30 FPS: default values are fine
        std::cout << "🎬 [FULL-RES V2] Standard FPS (" << frame_rate_ 
                  << "), using default buffer windows" << std::endl;
    }
}

#ifdef _WIN32
// Which Windows format was actually created: D3D11 (default) or DXVA2_VLD (fallback).
// GetHWFormat is static (no this), so the choice is stored at file scope; on one
// machine every decoder instance uses the same API.
static AVPixelFormat g_win_hw_pix_fmt = AV_PIX_FMT_D3D11;
#endif

bool FSTPFullResDecoderV2::InitializeHardwareAcceleration(const AVCodec* codec) {
    // ========================================
    // UNIFIED HARDWARE DETECTION (NEW!)
    // ========================================
    // Use centralized FSTPHardwareDetection instead of inline detection

    if (codec_params_->codec_id != AV_CODEC_ID_H264 && codec_params_->codec_id != AV_CODEC_ID_HEVC) {
        std::cout << "ℹ️  [FULL-RES V2] Codec doesn't support hardware acceleration, using software" << std::endl;
        return false;
    }

    if (!g_hardware_detection) {
        std::cerr << "⚠️  [FULL-RES V2] g_hardware_detection not initialized, using software" << std::endl;
        return false;
    }

    // Get decoder strategy from unified detection
    FSTPDecoderStrategy strategy = g_hardware_detection->GetDecoderStrategy(
        codec_params_->width,
        codec_params_->height,
        codec_params_->codec_id,
        1.0  // Normal playback speed
    );

    std::cout << "🎯 [FULL-RES V2] Strategy: " << strategy.strategy_reason << std::endl;

    if (!strategy.use_hw_accel) {
        std::cout << "ℹ️  [FULL-RES V2] Strategy recommends software decode" << std::endl;
        return false;
    }

    // Initialize hardware acceleration based on strategy
    int ret = -1;

    if (strategy.hw_accel_type == FSTPHWAccelType::VIDEOTOOLBOX) {
        // macOS VideoToolbox
        ret = av_hwdevice_ctx_create(&hw_device_ctx_, AV_HWDEVICE_TYPE_VIDEOTOOLBOX, nullptr, nullptr, 0);
        if (ret >= 0) {
            hw_pix_fmt_ = AV_PIX_FMT_VIDEOTOOLBOX;
            std::cout << "✅ [FULL-RES V2] VideoToolbox hardware acceleration enabled" << std::endl;
        }
    } else if (strategy.hw_accel_type == FSTPHWAccelType::VAAPI) {
        // Linux VA-API
        const char* device = strategy.hw_device_path.empty() ? "/dev/dri/renderD128" : strategy.hw_device_path.c_str();
        ret = av_hwdevice_ctx_create(&hw_device_ctx_, AV_HWDEVICE_TYPE_VAAPI, device, nullptr, 0);
        if (ret >= 0) {
            hw_pix_fmt_ = AV_PIX_FMT_VAAPI;
            std::cout << "✅ [FULL-RES V2] VA-API hardware acceleration enabled (" << device << ")" << std::endl;
        }
    } else if (strategy.hw_accel_type == FSTPHWAccelType::D3D11VA ||
               strategy.hw_accel_type == FSTPHWAccelType::DXVA2) {
        // Windows Direct3D 11 Video Acceleration
        ret = av_hwdevice_ctx_create(&hw_device_ctx_, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0);
        if (ret >= 0) {
            hw_pix_fmt_ = AV_PIX_FMT_D3D11;
            std::cout << "✅ [FULL-RES V2] D3D11VA hardware acceleration enabled" << std::endl;
        } else {
            // DXVA2 fallback: old drivers and remote sessions where a D3D11
            // device won't create but DXVA2 (D3D9) still works.
            ret = av_hwdevice_ctx_create(&hw_device_ctx_, AV_HWDEVICE_TYPE_DXVA2, nullptr, nullptr, 0);
            if (ret >= 0) {
                hw_pix_fmt_ = AV_PIX_FMT_DXVA2_VLD;
                std::cout << "✅ [FULL-RES V2] DXVA2 fallback enabled (D3D11VA unavailable)" << std::endl;
            }
        }
#ifdef _WIN32
        if (ret >= 0) g_win_hw_pix_fmt = hw_pix_fmt_;
#endif
    }

    if (ret < 0) {
        std::cerr << "⚠️  [FULL-RES V2] Hardware acceleration init failed, using software" << std::endl;
        char av_errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_make_error_string(av_errbuf, AV_ERROR_MAX_STRING_SIZE, ret);
        std::cerr << "      Error code: " << ret << " (" << av_errbuf << ")" << std::endl;
        return false;
    }

    // Apply decoder strategy settings
    codec_ctx_->hw_device_ctx = av_buffer_ref(hw_device_ctx_);
    codec_ctx_->get_format = FSTPFullResDecoderV2::GetHWFormat;
    codec_ctx_->thread_count = strategy.thread_count;
    codec_ctx_->thread_type = strategy.thread_type;
    codec_ctx_->flags |= strategy.codec_flags;
    codec_ctx_->flags2 |= strategy.codec_flags2;

    return true;
}

AVPixelFormat FSTPFullResDecoderV2::GetHWFormat(AVCodecContext* ctx, const enum AVPixelFormat* pix_fmts) {
    for (const enum AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
#ifdef __APPLE__
        if (*p == AV_PIX_FMT_VIDEOTOOLBOX) return *p;
#elif defined(__linux__)
        if (*p == AV_PIX_FMT_VAAPI)         return *p;
#elif defined(_WIN32)
        // D3D11 or DXVA2_VLD — whichever device was actually created (see init).
        if (*p == g_win_hw_pix_fmt)         return *p;
#endif
    }
    return AV_PIX_FMT_NONE;
}

void FSTPFullResDecoderV2::Cleanup() {
    // Stop background thread
    if (thread_running_.load()) {
        std::cout << "🛑 [FULL-RES V2] Stopping background thread..." << std::endl;
        thread_running_.store(false);
        cv_.notify_all();

        if (decoding_thread_.joinable()) {
            decoding_thread_.join();
            std::cout << "✅ [FULL-RES V2] Background thread stopped" << std::endl;
        }
    }

    // Clear buffer
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        stream_buffer_.clear();
    }

    // Clear cache
    {
        std::lock_guard<std::mutex> cache_lock(cache_mutex_);
        cached_frame_.reset();
        cached_frame_time_.store(-1.0);
    }

    // Free downscale context
    if (downscale_ctx_) {
        sws_freeContext(downscale_ctx_);
        downscale_ctx_ = nullptr;
    }


    // Free codec context
    if (codec_ctx_) {
        avcodec_free_context(&codec_ctx_);
        codec_ctx_ = nullptr;
    }

    // Free hardware device context
    if (hw_device_ctx_) {
        av_buffer_unref(&hw_device_ctx_);
        hw_device_ctx_ = nullptr;
    }

    // Close format context
    if (format_ctx_) {
        avformat_close_input(&format_ctx_);
        format_ctx_ = nullptr;
    }

    initialized_ = false;
}

// UpdateBuffer removed - now using background thread DecodingThreadLoop()

std::shared_ptr<AVFrame> FSTPFullResDecoderV2::GetFrameForTime(double time_seconds) {
    // CORRECTION: Use dynamic tolerance based on frame_rate
    const double ONE_FRAME_MS = 1000.0 / (frame_rate_ > 0 ? frame_rate_ : 25.0);

    // ADAPTIVE FPS: High FPS needs tighter cache tolerance for smoother playback
    const double CACHE_TOLERANCE_FRAMES = (frame_rate_ >= 55.0) ? 0.5 : 0.7;
    
    // FAST PATH: Check cache without lock (optimization for 60 FPS)
    double cached_time = cached_frame_time_.load(std::memory_order_relaxed);
    const double TOLERANCE = (ONE_FRAME_MS * CACHE_TOLERANCE_FRAMES) / 1000.0;

    if (cached_time >= 0.0 && std::abs(cached_time - time_seconds) < TOLERANCE) {
        // Cache hit - return without lock!
        return cached_frame_;
    }

    // SLOW PATH: Search in buffer (only when frame changes)
    std::lock_guard<std::mutex> lock(buffer_mutex_);

    if (stream_buffer_.empty()) {
        return nullptr;
    }

    // CORRECTION: Search CLOSEST frame to requested_time (not necessarily <=)
    // This allows taking a frame slightly FORWARD if it's closer, avoiding "stuttering"
    
    // LOW FPS FIX (25fps): Search for best frame <= requested_time to avoid 1-frame jitter
    // At 25fps, each frame = 40ms. "Closest" can jump between N and N+1 causing visible stutter.
    // Instead: prefer frame <= requested_time (already displayed or current)
    StreamFrame* best_match = nullptr;
    double best_diff = 1e9;
    // AT-OR-BEFORE (floor) at ALL fps: the displayed base frame must be floor(audio*fps) = N, the
    // SAME the proxy shows (slot[floor]) and the same the Betacam helical model assumes below the
    // stripe. "Nearest" would jump the base to N+1 once the audio phase passes 0.5 → a 1-frame jump
    // at the proxy→full-res switch. (Re-enabled: the earlier spike this caused was because the N+1
    // adjacent frame was ALSO fetched via this nearest/at-or-before path; adjacents now come from
    // GetFrameTriplet straight off the buffer, so capping the BASE at <= time is safe.)
    bool is_low_fps = true;

    for (auto& sf : stream_buffer_) {
        double diff = std::abs(sf.time_seconds - time_seconds);
        
        if (is_low_fps) {
            // PREFER frames <= requested_time to avoid jumping ahead
            // Only accept frame > requested_time if no earlier frame exists
            if (sf.time_seconds <= time_seconds + 0.001) {  // <= requested (with tiny tolerance)
                if (diff < best_diff) {
                    best_diff = diff;
                    best_match = &sf;
                }
            } else if (!best_match) {
                // Fallback: accept next frame only if nothing earlier available
                if (diff < best_diff) {
                    best_diff = diff;
                    best_match = &sf;
                }
            }
        } else {
            // High FPS: use standard closest-frame logic
            if (diff < best_diff) {
                best_diff = diff;
                best_match = &sf;
            }
        }
    }

    // ADAPTIVE FPS: Tolerance for frame search scales with frame rate
    // 60 FPS: 4.0 frames = ~66.7ms (more lenient to catch up)
    // 30 FPS: 3.1 frames = ~103ms (standard)
    const double SEARCH_TOLERANCE_FRAMES = (frame_rate_ >= 55.0) ? 4.0 : 3.1;
    const double MAX_DIFF = (ONE_FRAME_MS * SEARCH_TOLERANCE_FRAMES) / 1000.0;

    // Check that found frame is within tolerance
    if (best_match && best_diff <= MAX_DIFF) {
        // Update cache for next calls (atomically)
        {
            std::lock_guard<std::mutex> cache_lock(cache_mutex_);
            cached_frame_ = best_match->frame;
            cached_frame_time_.store(best_match->time_seconds, std::memory_order_release);
        }

        if (ENABLE_FULLRES_V2_DEBUG) {
            static int hit_log = 0;
            if (++hit_log % 60 == 1) {
                std::cout << std::fixed << std::setprecision(4);
                std::cout << "✅ [V2 HIT] requested=" << time_seconds
                          << "s, found=" << best_match->time_seconds
                          << "s, diff=" << (best_diff * 1000.0) << "ms"
                          << ", buf_size=" << stream_buffer_.size()
                          << " [" << stream_buffer_.front().time_seconds
                          << "s.." << stream_buffer_.back().time_seconds << "s]"
                          << std::defaultfloat << std::endl;
            }
        }

        return best_match->frame;
    }

    if (ENABLE_FULLRES_V2_DEBUG) {
        double diff_ms = best_match ? (best_diff * 1000.0) : 9999.0;
        std::cout << std::fixed << std::setprecision(4);
        std::cout << "⚠️ [V2 MISS] requested=" << time_seconds
                  << "s, best=" << (best_match ? best_match->time_seconds : -1.0)
                  << "s, diff=" << diff_ms << "ms"
                  << ", tol=" << (MAX_DIFF * 1000.0) << "ms"
                  << ", buf_size=" << stream_buffer_.size();
        if (!stream_buffer_.empty()) {
            std::cout << " [" << stream_buffer_.front().time_seconds
                      << "s.." << stream_buffer_.back().time_seconds << "s]";
        }
        std::cout << std::defaultfloat << std::endl;
    }

    return nullptr;
}

void FSTPFullResDecoderV2::GetFrameTriplet(double now_time,
                                           std::shared_ptr<AVFrame>& prev,
                                           std::shared_ptr<AVFrame>& now,
                                           std::shared_ptr<AVFrame>& next) {
    prev = now = next = nullptr;

    std::lock_guard<std::mutex> lock(buffer_mutex_);
    if (stream_buffer_.empty()) return;

    // AT-OR-BEFORE (floor) `now`: the last buffered frame whose time <= now_time. This matches
    // GetFrameForTime's at-or-before base selection, so `now` here is the SAME frame the base shows
    // — and then the buffer-contiguous neighbours are exactly its N-1 / N+1. (The buffer holds
    // frames in ascending time order, so the highest-index qualifying entry is the floor frame.)
    const double tol = 0.001;
    size_t best = SIZE_MAX;
    for (size_t i = 0; i < stream_buffer_.size(); ++i) {
        if (stream_buffer_[i].time_seconds <= now_time + tol) best = i;  // keep last → at-or-before
    }

    const double one_frame = (frame_rate_ > 0.0) ? (1.0 / frame_rate_) : 0.04;
    if (best == SIZE_MAX) {
        // now_time is before the whole buffer → use the first frame only if it's close enough.
        if (std::abs(stream_buffer_.front().time_seconds - now_time) > one_frame * 3.1) return;
        best = 0;
    } else if (now_time - stream_buffer_[best].time_seconds > one_frame * 3.1) {
        return;  // nearest at-or-before is too far back → "now" not really buffered
    }

    now = stream_buffer_[best].frame;
    // The buffer holds contiguous decoded frames in time order, so the immediate neighbours ARE
    // N-1 and N+1 — a guaranteed-consistent triplet, no second time-query needed.
    if (best > 0)                              prev = stream_buffer_[best - 1].frame;
    if (best + 1 < stream_buffer_.size())      next = stream_buffer_[best + 1].frame;
}

void FSTPFullResDecoderV2::ClearBuffer() {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    stream_buffer_.clear();

    // Invalidate cache
    {
        std::lock_guard<std::mutex> cache_lock(cache_mutex_);
        cached_frame_.reset();
        cached_frame_time_.store(-1.0, std::memory_order_release);
    }

    // std::cout << "🗑️  [FULL-RES V2] Buffer cleared" << std::endl;
}

bool FSTPFullResDecoderV2::SeekToTime(double time_seconds) {
    if (!format_ctx_ || !video_stream_) return false;

    // Convert time to timestamp
    int64_t seek_target = static_cast<int64_t>(time_seconds / av_q2d(video_stream_->time_base));

    if (video_stream_->start_time != AV_NOPTS_VALUE) {
        seek_target += video_stream_->start_time;
    }

    // Seek with BACKWARD flag to get keyframe before target
    if (av_seek_frame(format_ctx_, video_stream_index_, seek_target, AVSEEK_FLAG_BACKWARD) < 0) {
        std::cerr << "⚠️  [FULL-RES V2] Seek failed for time " << time_seconds << std::endl;
        return false;
    }

    // Flush codec buffers
    avcodec_flush_buffers(codec_ctx_);
    return true;
}

bool FSTPFullResDecoderV2::DownscaleFrame(AVFrame* src, AVFrame* dst) {
    if (!src || !dst) return false;

    AVPixelFormat src_format = static_cast<AVPixelFormat>(src->format);

    // Handle hardware frame transfer first (VideoToolbox on macOS, VA-API on Linux, D3D11 on Windows)
    AVFrame* sw_src = src;
    AVFrame* temp_hw_frame = nullptr;

    bool is_hw_frame = (src_format == AV_PIX_FMT_VIDEOTOOLBOX) ||
                       (src_format == AV_PIX_FMT_VAAPI) ||
                       (src_format == AV_PIX_FMT_D3D11) ||
                       (src_format == AV_PIX_FMT_DXVA2_VLD);

    if (is_hw_frame) {
        temp_hw_frame = av_frame_alloc();
        if (!temp_hw_frame) return false;

        if (av_hwframe_transfer_data(temp_hw_frame, src, 0) < 0) {
            av_frame_free(&temp_hw_frame);
            return false;
        }

        temp_hw_frame->pts = src->pts;
        sw_src = temp_hw_frame;
        src_format = static_cast<AVPixelFormat>(sw_src->format);
    }

    // Check if we need to recreate downscale context
    if (!downscale_ctx_ ||
        src->width != native_width_ ||
        src->height != native_height_) {

        if (downscale_ctx_) {
            sws_freeContext(downscale_ctx_);
        }

        downscale_ctx_ = sws_getContext(
            sw_src->width, sw_src->height, src_format,
            display_width_, display_height_, AV_PIX_FMT_YUV420P,
            SWS_BILINEAR, nullptr, nullptr, nullptr
        );

        if (!downscale_ctx_) {
            if (temp_hw_frame) av_frame_free(&temp_hw_frame);
            return false;
        }
    }

    // Allocate destination frame
    dst->format = AV_PIX_FMT_YUV420P;
    dst->width = display_width_;
    dst->height = display_height_;
    dst->pts = sw_src->pts;

    if (av_frame_get_buffer(dst, 0) < 0) {
        if (temp_hw_frame) av_frame_free(&temp_hw_frame);
        return false;
    }

    // Perform downscaling
    int result = sws_scale(
        downscale_ctx_,
        sw_src->data, sw_src->linesize, 0, sw_src->height,
        dst->data, dst->linesize
    );

    if (temp_hw_frame) av_frame_free(&temp_hw_frame);

    if (result > 0) {
        // Save color metadata from original frame for further conversion
        dst->colorspace = sw_src->colorspace;
        dst->color_range = sw_src->color_range;
        dst->color_primaries = sw_src->color_primaries;
        dst->color_trc = sw_src->color_trc;
        return true;
    }
    return false;
}

std::shared_ptr<AVFrame> FSTPFullResDecoderV2::DecodeAndProcessFrame(double targetTime) {
    if (!format_ctx_ || !codec_ctx_) return nullptr;

    // PROFILING: Separate measurement of each operation
    static uint64_t total_read_us = 0;
    static uint64_t total_send_packet_us = 0;
    static uint64_t total_receive_frame_us = 0;
    static uint64_t total_hw_alloc_us = 0;
    static uint64_t total_hw_transfer_us = 0;
    static uint64_t total_clone_us = 0;
    static int profile_samples = 0;

    AVPacket* packet = av_packet_alloc();
    AVFrame* decoded_frame = av_frame_alloc();

    if (!packet || !decoded_frame) {
        av_packet_free(&packet);
        av_frame_free(&decoded_frame);
        return nullptr;
    }

    std::shared_ptr<AVFrame> result = nullptr;
    int read_result = 0;

    auto read_start = std::chrono::high_resolution_clock::now();
    while ((read_result = av_read_frame(format_ctx_, packet)) >= 0) {
        auto read_end = std::chrono::high_resolution_clock::now();
        total_read_us += std::chrono::duration_cast<std::chrono::microseconds>(read_end - read_start).count();
        FSTP_SIGNPOST_EVENT(fstp_log_decoder, "av_read_frame", "size=%d", packet->size);

        if (stop_requested_.load()) break;

        if (packet->stream_index == video_stream_index_) {
            FSTP_PROFILE_DECODE_BEGIN("fullres_v2");

            // PROFILING: Measurement of avcodec_send_packet
            auto send_start = std::chrono::high_resolution_clock::now();
            int send_result = avcodec_send_packet(codec_ctx_, packet);
            auto send_end = std::chrono::high_resolution_clock::now();
            total_send_packet_us += std::chrono::duration_cast<std::chrono::microseconds>(send_end - send_start).count();

            if (send_result < 0) {
                av_packet_unref(packet);
                FSTP_PROFILE_DECODE_END("fullres_v2");
                read_start = std::chrono::high_resolution_clock::now();
                continue;
            }

            while (true) {
                // PROFILING: Measurement of avcodec_receive_frame (VideoToolbox decode)
                auto receive_start = std::chrono::high_resolution_clock::now();
                int receive_result = avcodec_receive_frame(codec_ctx_, decoded_frame);
                auto receive_end = std::chrono::high_resolution_clock::now();

                if (receive_result != 0) break;

                total_receive_frame_us += std::chrono::duration_cast<std::chrono::microseconds>(receive_end - receive_start).count();
                // FFPLAY STYLE: Keep original resolution (without downscale)
                // Handle hardware frame transfer if needed (VideoToolbox on macOS, VA-API on Linux)
                AVFrame* final_frame = decoded_frame;
                AVFrame* temp_hw_frame = nullptr;

                bool is_hw_frame = (decoded_frame->format == AV_PIX_FMT_VIDEOTOOLBOX) ||
                                   (decoded_frame->format == AV_PIX_FMT_VAAPI) ||
                                   (decoded_frame->format == AV_PIX_FMT_D3D11) ||
                                   (decoded_frame->format == AV_PIX_FMT_DXVA2_VLD);

                if (is_hw_frame) {
                    // PROFILING: Measurement of av_frame_alloc
                    auto alloc_start = std::chrono::high_resolution_clock::now();
                    temp_hw_frame = av_frame_alloc();
                    auto alloc_end = std::chrono::high_resolution_clock::now();
                    total_hw_alloc_us += std::chrono::duration_cast<std::chrono::microseconds>(alloc_end - alloc_start).count();

                    if (temp_hw_frame) {
                        // PROFILING: Measurement of av_hwframe_transfer_data (GPU→CPU copy)
                        auto transfer_start = std::chrono::high_resolution_clock::now();
                        int transfer_result = av_hwframe_transfer_data(temp_hw_frame, decoded_frame, 0);
                        auto transfer_end = std::chrono::high_resolution_clock::now();
                        total_hw_transfer_us += std::chrono::duration_cast<std::chrono::microseconds>(transfer_end - transfer_start).count();

                        if (transfer_result >= 0) {
                            // CRITICAL: Copy ALL properties from hardware frame
                            av_frame_copy_props(temp_hw_frame, decoded_frame);
                            temp_hw_frame->pts = decoded_frame->pts;
                            temp_hw_frame->time_base = video_stream_->time_base;
                            final_frame = temp_hw_frame;
                        } else {
                            av_frame_free(&temp_hw_frame);
                            av_frame_unref(decoded_frame);
                            continue;
                        }
                    } else {
                        av_frame_unref(decoded_frame);
                        continue;
                    }
                }

                // ZERO-COPY: prefer native YUV formats; other formats handled later in pipeline
                // (format conversion handled downstream if required)

                // Clone to shared_ptr (like in old working version)
                auto clone_start = std::chrono::high_resolution_clock::now();
                result = std::shared_ptr<AVFrame>(
                    av_frame_clone(final_frame),
                    [](AVFrame* f) { av_frame_free(&f); }
                );
                auto clone_end = std::chrono::high_resolution_clock::now();
                total_clone_us += std::chrono::duration_cast<std::chrono::microseconds>(clone_end - clone_start).count();

                if (temp_hw_frame) av_frame_free(&temp_hw_frame);

                if (result) {
                    // Preserve PTS for time calculation
                    result->pts = decoded_frame->best_effort_timestamp;
                    if (result->pts == AV_NOPTS_VALUE) {
                        result->pts = decoded_frame->pts;
                    }
                    // time_base already copied through av_frame_clone

                    // Logging color_range for full-res (first 5 frames)
                    static int fullres_metadata_log = 0;
                    if (++fullres_metadata_log <= 5) {
                        std::cout << "🎬 [FULL-RES V2] Decoded frame: "
                                 << "format=" << result->format
                                 << ", range=" << result->color_range
                                 << " (0=unspec, 1=MPEG/TV, 2=JPEG/PC)"
                                 << ", cs=" << result->colorspace
                                 << ", prim=" << result->color_primaries
                                 << ", trc=" << result->color_trc << std::endl;
                    }

                    // PROFILING: Output detailed statistics every 60 frames (gated by flag)
                    if (ENABLE_FULLRES_V2_PROFILING) {
                        profile_samples++;
                        if (profile_samples >= 60) {
                            uint64_t avg_read = total_read_us / profile_samples;
                            uint64_t avg_send = total_send_packet_us / profile_samples;
                            uint64_t avg_receive = total_receive_frame_us / profile_samples;
                            uint64_t avg_alloc = total_hw_alloc_us / profile_samples;
                            uint64_t avg_transfer = total_hw_transfer_us / profile_samples;
                            uint64_t avg_clone = total_clone_us / profile_samples;
                            uint64_t total_avg = avg_read + avg_send + avg_receive + avg_alloc + avg_transfer + avg_clone;

                            std::cout << "📊 [DECODE PROFILE] Read: " << avg_read << "μs"
                                      << ", Send: " << avg_send << "μs"
                                      << ", Receive(VT): " << avg_receive << "μs"
                                      << ", Alloc: " << avg_alloc << "μs"
                                      << ", Transfer: " << avg_transfer << "μs"
                                      << ", Clone: " << avg_clone << "μs"
                                      << " | TOTAL: " << total_avg << "μs/frame" << std::endl;

                            // Reset
                            total_read_us = 0;
                            total_send_packet_us = 0;
                            total_receive_frame_us = 0;
                            total_hw_alloc_us = 0;
                            total_hw_transfer_us = 0;
                            total_clone_us = 0;
                            profile_samples = 0;
                        }
                    }

                    // Return frame immediately
                    av_frame_unref(decoded_frame);
                    av_packet_unref(packet);
                    FSTP_PROFILE_DECODE_END("fullres_v2");
                    goto decode_end;
                }

                av_frame_unref(decoded_frame);
            }
            FSTP_PROFILE_DECODE_END("fullres_v2");
        }
        av_packet_unref(packet);
        read_start = std::chrono::high_resolution_clock::now();
    }

    // EOF reached - flush decoder to get buffered frames
    if (read_result < 0) {
        avcodec_send_packet(codec_ctx_, nullptr);  // Flush signal

        while (avcodec_receive_frame(codec_ctx_, decoded_frame) == 0) {
            AVFrame* final_frame = decoded_frame;
            AVFrame* temp_hw_frame = nullptr;

            bool is_hw_frame = (decoded_frame->format == AV_PIX_FMT_VIDEOTOOLBOX) ||
                               (decoded_frame->format == AV_PIX_FMT_VAAPI) ||
                               (decoded_frame->format == AV_PIX_FMT_D3D11) ||
                               (decoded_frame->format == AV_PIX_FMT_DXVA2_VLD);

            if (is_hw_frame) {
                temp_hw_frame = av_frame_alloc();
                if (temp_hw_frame && av_hwframe_transfer_data(temp_hw_frame, decoded_frame, 0) >= 0) {
                    // CRITICAL: Copy ALL properties from hardware frame
                    av_frame_copy_props(temp_hw_frame, decoded_frame);
                    temp_hw_frame->pts = decoded_frame->pts;
                    temp_hw_frame->time_base = video_stream_->time_base;
                    final_frame = temp_hw_frame;
                } else {
                    if (temp_hw_frame) av_frame_free(&temp_hw_frame);
                    av_frame_unref(decoded_frame);
                    continue;
                }
            }

            // ZERO-COPY: Accept NV12/YUV420P directly without conversion!
            // SDL supports both formats through SDL_UpdateNVTexture/SDL_UpdateYUVTexture
            // (format conversion handled downstream if required)

            // Clone to shared_ptr (like in old working version)
            result = std::shared_ptr<AVFrame>(
                av_frame_clone(final_frame),
                [](AVFrame* f) { av_frame_free(&f); }
            );

            if (temp_hw_frame) av_frame_free(&temp_hw_frame);

            if (result) {
                result->pts = decoded_frame->best_effort_timestamp;
                if (result->pts == AV_NOPTS_VALUE) {
                    result->pts = decoded_frame->pts;
                }
                // time_base already copied through av_frame_clone

                av_frame_unref(decoded_frame);
                goto decode_end;
            }

            av_frame_unref(decoded_frame);
        }
    }

decode_end:
    av_packet_free(&packet);
    av_frame_free(&decoded_frame);

    return result;
}

// ============================================================================
// BACKGROUND DECODING THREAD
// ============================================================================

void FSTPFullResDecoderV2::SetPlaybackTime(double time_seconds) {
    double prev_time = current_playback_time_.exchange(time_seconds);

    // CORRECTION: If there is a significant time change (seek/shuttle) - clear buffer and cache
    // This prevents showing old frames when switching from proxy → Full-res
    const double SEEK_THRESHOLD = 0.5; // 0.5s = ~12 frames at 25 FPS
    if (std::abs(time_seconds - prev_time) > SEEK_THRESHOLD) {
        // Clear buffer - old frames are no longer needed
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            stream_buffer_.clear();
        }

        // Invalidate cache - force GetFrameForTime to search in buffer
        {
            std::lock_guard<std::mutex> cache_lock(cache_mutex_);
            cached_frame_.reset();
            cached_frame_time_.store(-1.0, std::memory_order_release);
        }

        // Log ALL large seeks (not just 1 in 5) for better debugging
        std::cout << "🔄 [V2 SEEK] Large time jump detected: " << std::fixed << std::setprecision(1)
                  << prev_time << "s → " << time_seconds << "s (Δ="
                  << std::abs(time_seconds - prev_time) << "s), buffer+cache cleared, waking thread"
                  << std::defaultfloat << std::endl;
    }

    // CRITICAL: Wake up thread immediately after ANY playback time change
    // This ensures rapid response after shuttle/seek
    cv_.notify_one();
}

void FSTPFullResDecoderV2::RequestStop() {
    stop_requested_.store(true);
}

void FSTPFullResDecoderV2::ClearStopRequest() {
    stop_requested_.store(false);
}

void FSTPFullResDecoderV2::DecodingThreadLoop() {
    std::cout << "🧵 [FULL-RES V2 THREAD] Decoding thread started" << std::endl;

    // Lower priority of decoding thread for multi-instance
    int decoder_count = g_active_decoder_count.load();
    if (decoder_count >= 2) {
#ifdef __APPLE__
        // macOS: lower priority using pthread
        struct sched_param param;
        param.sched_priority = sched_get_priority_min(SCHED_OTHER);
        pthread_setschedparam(pthread_self(), SCHED_OTHER, &param);
        std::cout << "⬇️ [FULL-RES V2 THREAD] Lowered thread priority (multi-instance)" << std::endl;
#endif
    }

    // PROFILING: Measurement of time in different parts of loop
    uint64_t total_buffer_check_us = 0;
    uint64_t total_seek_us = 0;
    uint64_t total_decode_us = 0;
    uint64_t total_timestamp_us = 0;
    uint64_t total_duplicate_check_us = 0;
    uint64_t total_buffer_mgmt_us = 0;
    int profile_iterations = 0;
    int total_frames_decoded = 0;  // Counter of REAL decoded frames
    int total_frames_duplicates = 0;  // Counter of duplicates (frames decoded again)
    int total_seeks = 0;  // Counter of seek operations
    auto profile_start_time = std::chrono::high_resolution_clock::now();
    uint64_t profile_start_cpu_us = GetThreadCPUTimeMicroseconds();

    while (thread_running_.load()) {
        auto iteration_start = std::chrono::high_resolution_clock::now();
        double playback_time = current_playback_time_.load();
        bool is_high_fps = (frame_rate_ >= 55.0);  // ADAPTIVE FPS helper

        // PROFILING: Buffer check
        auto buffer_check_start = std::chrono::high_resolution_clock::now();

        // Check if buffer needs to be updated
        bool needs_update = false;
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);

            if (stream_buffer_.empty()) {
                needs_update = true;
            } else {
                double bufferMinTime = stream_buffer_.front().time_seconds;
                double bufferMaxTime = stream_buffer_.back().time_seconds;

                // CPU OPTIMIZATION: Balance between performance and smoothness
                // Update buffer ONLY if:
                // 1. Playback outside buffer range (need seek) - PRIORITY!
                // 2. Playback approaching end of buffer (need to decode forward)
                // 3. Buffer too short forward
                bool playback_outside_buffer = (playback_time < bufferMinTime - 0.1) ||
                                               (playback_time > bufferMaxTime + 0.1);

                // CRITICAL: Buffer must be ahead of playback by at least 1.5s for high FPS!
                // Otherwise it will stutter due to showing old frames
                // Adaptive thresholds: high FPS needs larger buffers
                double ahead_threshold = (frame_rate_ >= 55.0) ? 2.5 : 1.0;
                double decode_start_threshold = (frame_rate_ >= 55.0) ? 3.0 : 2.0;
                
                bool playback_near_end = (playback_time > bufferMaxTime - decode_start_threshold);  // Start decoding earlier for high FPS

                bool buffer_too_short = (bufferMaxTime < playback_time + ahead_threshold);  // Buffer MUST be ahead

                needs_update = playback_outside_buffer || playback_near_end || buffer_too_short;

                // DO NOT decode if buffer is full AND playback is inside range AND buffer is long enough forward
                // CRITICAL: Allow decoding even if buffer is full if buffer is behind!
                int target_size = buffer_size_target_.load();
                // High FPS needs larger buffer (3x instead of 3x)
                int buffer_full_multiplier = (frame_rate_ >= 55.0) ? 4 : 3;
                if (stream_buffer_.size() >= static_cast<size_t>(target_size * buffer_full_multiplier) &&
                    !playback_outside_buffer &&
                    !buffer_too_short) {
                    needs_update = false;  // Buffer is full, playback is normal, AND buffer is ahead
                }
            }
        }

        auto buffer_check_end = std::chrono::high_resolution_clock::now();
        total_buffer_check_us += std::chrono::duration_cast<std::chrono::microseconds>(buffer_check_end - buffer_check_start).count();

        // PENTIUM 7505 OPTIMIZATION: Sleep when stopped to free CPU
        if (stop_requested_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;  // Skip decoding, check again after sleep
        }

        if (needs_update) {
            // Decode frames to fill buffer
            double minTime = playback_time - buffer_window_behind_;
            double maxTime = playback_time + buffer_window_ahead_;

            // CRITICAL CPU OPTIMIZATION: DO NOT SEEK if buffer exists!
            // Seek ALWAYS goes to keyframe back → duplicates 90%
            // Instead: continue decoding from current decoder position
            bool need_seek = false;
            double bufferMinTime = 0.0;
            double bufferMaxTime = 0.0;

            {
                std::lock_guard<std::mutex> lock(buffer_mutex_);
                if (stream_buffer_.empty()) {
                    // Buffer is empty - need seek to start decoding
                    need_seek = true;
                } else {
                    bufferMinTime = stream_buffer_.front().time_seconds;
                    bufferMaxTime = stream_buffer_.back().time_seconds;

                    // Seek needed if:
                    // 1. Playback jumped BACK (user rewound)
                    // 2. Playback is VERY FAR ahead (shuttle/scrub forward)
                    // ADAPTIVE FPS: High FPS needs tighter seek thresholds
                    double seek_backward_threshold = is_high_fps ? 0.8 : 0.5;
                    double shuttle_ahead_threshold = is_high_fps ? 8.0 : 10.0;
                    
                    if (playback_time < bufferMinTime - seek_backward_threshold) {
                        need_seek = true;
                    }
                    // SHUTTLE FIX: Seek forward if playback is >Xs ahead of buffer
                    // Without this, decoder gets stuck decoding from wrong position during shuttle
                    // High FPS: reduce threshold to catch up faster
                    else if (playback_time > bufferMaxTime + shuttle_ahead_threshold) {
                        need_seek = true;
                        static int shuttle_seek_log = 0;
                        if (++shuttle_seek_log % 10 == 1) {
                            std::cout << "🔄 [V2 SHUTTLE] Seek forward: buffer=[" << bufferMinTime
                                      << ".." << bufferMaxTime << "]s, playback=" << playback_time
                                      << "s (diff=" << (playback_time - bufferMaxTime) << "s)" << std::endl;
                        }
                    }
                }
            }

            bool seek_success = true;  // By default assume successful (if not seeking)

            if (need_seek) {
                // PROFILING: SeekToTime
                double seekTime = playback_time - 0.5;
                auto seek_start = std::chrono::high_resolution_clock::now();
                seek_success = SeekToTime(seekTime);
                auto seek_end = std::chrono::high_resolution_clock::now();
                total_seek_us += std::chrono::duration_cast<std::chrono::microseconds>(seek_end - seek_start).count();
                if (seek_success) total_seeks++;  // Counter of seek operations
            }

            if (seek_success) {
                int decoded_count = 0;

                // Check if close to end of file
                double file_duration = video_stream_->duration * av_q2d(video_stream_->time_base);
                bool near_eof = (playback_time > file_duration - 3.0);

                // Determine current buffer span for adaptive decode window
                double bufferMinForDecode = playback_time;
                double bufferMaxForDecode = playback_time;
                bool bufferEmpty = true;
                {
                    std::lock_guard<std::mutex> lock(buffer_mutex_);
                    if (!stream_buffer_.empty()) {
                        bufferEmpty = false;
                        bufferMinForDecode = stream_buffer_.front().time_seconds;
                        bufferMaxForDecode = stream_buffer_.back().time_seconds;
                    }
                }

                const double frameDuration = (frame_rate_ > 0.0) ? (1.0 / frame_rate_) : (1.0 / 25.0);
                int target_size = buffer_size_target_.load();

                // High FPS needs larger initial decode batch
                // 60 FPS: 20 frames = only 0.33s, need at least 0.5-1.0s
                int min_decode_batch = (frame_rate_ >= 55.0) ? 60 : 20;
                int max_decode_batch = (frame_rate_ >= 55.0) ? 600 : 400;  // Increased max for 60fps
                
                int max_decode = near_eof ? 200 : min_decode_batch;
                double desiredAheadTime = playback_time + buffer_window_ahead_;

                if (bufferEmpty) {
                    // CRITICAL: After seek/shuttle, decode many frames immediately to minimize jitter
                    // High FPS needs more frames to fill the same time window
                    int desired_frames = static_cast<int>(std::ceil((buffer_window_ahead_ + buffer_window_behind_ + 2.0) / frameDuration));
                    desired_frames = std::max(desired_frames, target_size * 3);
                    max_decode = std::min(max_decode_batch, std::max(max_decode, desired_frames));
                } else {
                    if (bufferMaxForDecode < desiredAheadTime) {
                        double missing = desiredAheadTime - bufferMaxForDecode;
                        int extra_frames = static_cast<int>(std::ceil(missing / frameDuration)) + (target_size * 2);
                        max_decode = std::min(max_decode_batch, std::max(max_decode, extra_frames));
                    }

                    if (playback_time - bufferMinForDecode > 1.0) {
                        max_decode = std::min(max_decode_batch, std::max(max_decode, target_size * 3));
                    }
                }

                bool reached_eof = false;
                
                // CRITICAL: Track initial playback time to detect seeks during decoding
                double initial_playback_time = playback_time;
                const double SEEK_ABORT_THRESHOLD = is_high_fps ? 1.0 : 0.5;  // High FPS: more sensitive

                while (decoded_count < max_decode && !stop_requested_.load() && thread_running_.load()) {
                    // CRITICAL CHECK: Detect if playback time changed significantly (seek/scrub)
                    // Abort current decoding batch and re-evaluate buffer needs
                    double current_playback = current_playback_time_.load();
                    if (std::abs(current_playback - initial_playback_time) > SEEK_ABORT_THRESHOLD) {
                        static int seek_abort_log = 0;
                        if (++seek_abort_log % 20 == 1) {
                            std::cout << "⚡ [V2 THREAD] Seek detected during decoding - aborting batch "
                                      << "(decoded=" << decoded_count << "/" << max_decode 
                                      << ", old=" << std::fixed << std::setprecision(2) << initial_playback_time
                                      << "s, new=" << current_playback << "s)" << std::defaultfloat << std::endl;
                        }
                        break;  // Abort this decoding batch, re-evaluate at loop start
                    }
                    
                    // PROFILING: DecodeAndProcessFrame
                    auto decode_start = std::chrono::high_resolution_clock::now();
                    auto decoded_frame = DecodeAndProcessFrame(playback_time);
                    auto decode_end = std::chrono::high_resolution_clock::now();
                    total_decode_us += std::chrono::duration_cast<std::chrono::microseconds>(decode_end - decode_start).count();

                    // Counter of REAL decoded frames (including nullptr - decoding attempts)
                    if (decoded_frame) total_frames_decoded++;

                    if (!decoded_frame) {
                        reached_eof = true;

                        // Logging for diagnostics
                        static int eof_null_log = 0;
                        if (near_eof && eof_null_log++ < 3) {
                            std::cout << "⚠️ [V2 THREAD] DecodeAndProcessFrame returned nullptr at EOF"
                                     << " (decoded=" << decoded_count << "/" << max_decode
                                     << ", playback_time=" << playback_time << "s)" << std::endl;
                        }
                        break;
                    }

                    // PROFILING: Timestamp calculation
                    auto timestamp_start = std::chrono::high_resolution_clock::now();

                    // Calculate frame time with MICROSECOND precision (like LowResDecoder).
                    // Use best_effort_timestamp (canonical DISPLAY time) — NOT raw .pts. On B-frame
                    // sources (the original is long-GOP w/ B-frames) raw .pts can sit a frame off the
                    // display order, which time-stamped V2 frames a frame early → GetFrameForTime's
                    // "nearest" returned clamped+1 → the stable +1 proxy→full-res shift. The proxy
                    // decoder (LowResDecoder) already uses best_effort_timestamp, so this matches it.
                    int64_t framePts = decoded_frame->best_effort_timestamp;
                    if (framePts == AV_NOPTS_VALUE) framePts = decoded_frame->pts;

                    double frame_time = 0.0;
                    if (framePts != AV_NOPTS_VALUE) {
                        // CRITICAL: Use time_base from STREAM, like in LowResDecoder!
                        AVRational time_base = video_stream_->time_base;
                        int64_t start_time = video_stream_->start_time != AV_NOPTS_VALUE ? video_stream_->start_time : 0;

                        // Convert to microseconds for maximum precision
                        int64_t pts_us = av_rescale_q(framePts, time_base, {1, 1000000});
                        int64_t start_us = av_rescale_q(start_time, time_base, {1, 1000000});

                        // Calculate relative time in microseconds, then convert to seconds
                        int64_t relative_us = pts_us - start_us;
                        frame_time = static_cast<double>(relative_us) / 1000000.0;
                    }

                    auto timestamp_end = std::chrono::high_resolution_clock::now();
                    total_timestamp_us += std::chrono::duration_cast<std::chrono::microseconds>(timestamp_end - timestamp_start).count();

                    // Skip old frames, BUT during initial fill (buffer empty), accept frames closer to target
                    // This prevents wasting decoded frames after a long seek
                    double effective_minTime = minTime;
                    if (bufferEmpty && decoded_count < 50) {
                        // During initial catch-up after seek, accept frames from seek point forward
                        // This allows filling buffer even if we landed at a distant keyframe
                        effective_minTime = playback_time - 10.0;  // Accept frames up to 10s before target
                    }
                    if (frame_time < effective_minTime) continue;

                    // DO NOT stop by maxTime - decode as long as there are frames
                    // Limitation only by buffer size (buffer_size_target * 2)

                    // Add to buffer
                    {
                        auto buffer_mgmt_start = std::chrono::high_resolution_clock::now();
                        std::lock_guard<std::mutex> lock(buffer_mutex_);

                        // PROFILING: Duplicate check
                        auto dup_start = std::chrono::high_resolution_clock::now();
                        bool duplicate = false;
                        for (const auto& sf : stream_buffer_) {
                            if (std::abs(sf.time_seconds - frame_time) < 0.001) {
                                duplicate = true;
                                break;
                            }
                        }
                        auto dup_end = std::chrono::high_resolution_clock::now();
                        total_duplicate_check_us += std::chrono::duration_cast<std::chrono::microseconds>(dup_end - dup_start).count();

                        if (duplicate) {
                            total_frames_duplicates++;  // Frame was decoded, but already in buffer!
                        }

                        if (!duplicate) {
                            StreamFrame sf;
                            sf.frame = decoded_frame;
                            sf.time_seconds = frame_time;
                            sf.pts = framePts;
                            stream_buffer_.push_back(sf);
                            decoded_count++;
                            bufferMinForDecode = stream_buffer_.front().time_seconds;
                            bufferMaxForDecode = stream_buffer_.back().time_seconds;

                            // DIAGNOSTICS: Output first 20 frames for timing debug
                            if (ENABLE_FULLRES_V2_DEBUG) {
                                static int fullres_timing_debug = 0;
                                if (fullres_timing_debug < 20) {
                                    int est_frame = static_cast<int>(frame_time * 25.0 + 0.5);
                                    std::cout << "🔍 [FullRes TIMING] Frame " << est_frame
                                              << ": decoded_frame->pts=" << decoded_frame->pts
                                              << ", framePts=" << framePts
                                              << ", frame_time=" << frame_time << "s"
                                              << ", time_base=" << video_stream_->time_base.num << "/" << video_stream_->time_base.den
                                              << ", start_time=" << video_stream_->start_time << std::endl;
                                    fullres_timing_debug++;
                                }
                            }
                        }

                        // Remove old frames (outside window)
                        while (!stream_buffer_.empty() && stream_buffer_.front().time_seconds < minTime) {
                            stream_buffer_.pop_front();
                            if (!stream_buffer_.empty()) {
                                bufferMinForDecode = stream_buffer_.front().time_seconds;
                                bufferMaxForDecode = stream_buffer_.back().time_seconds;
                            } else {
                                bufferMinForDecode = playback_time;
                                bufferMaxForDecode = playback_time;
                            }
                        }

                        if (!stream_buffer_.empty()) {
                            bufferMinForDecode = stream_buffer_.front().time_seconds;
                            bufferMaxForDecode = stream_buffer_.back().time_seconds;
                        } else {
                            bufferMinForDecode = playback_time;
                            bufferMaxForDecode = playback_time;
                        }

                        // Check buffer overflow
                        // High FPS needs larger buffer capacity (60fps = 2x more frames than 30fps)
                        int target_size = buffer_size_target_.load();
                        int buffer_overflow_multiplier = (frame_rate_ >= 55.0) ? 5 : 3;
                        
                        if (stream_buffer_.size() >= static_cast<size_t>(target_size * buffer_overflow_multiplier)) {
                            // If close to end of file (last 3 sec) - continue decoding
                            double file_duration = video_stream_->duration * av_q2d(video_stream_->time_base);
                            bool near_eof = (frame_time > file_duration - 3.0);
                            double currentBufferMax = stream_buffer_.empty() ? frame_time : stream_buffer_.back().time_seconds;

                            // CRITICAL: During catch-up (buffer behind playback), keep decoding!
                            // Only stop if buffer is actually AHEAD of playback time
                            bool still_catching_up = (currentBufferMax < playback_time - 0.5);

                            if (!near_eof && !still_catching_up && currentBufferMax >= desiredAheadTime - (frameDuration * 0.5)) {
                                break;  // Buffer is full enough ahead and NOT end of file - stop
                            }
                        }

                        auto buffer_mgmt_end = std::chrono::high_resolution_clock::now();
                        total_buffer_mgmt_us += std::chrono::duration_cast<std::chrono::microseconds>(buffer_mgmt_end - buffer_mgmt_start).count();
                    }
                }

                if (decoded_count > 0 && ENABLE_FULLRES_V2_DEBUG) {
                    double bufferMin = stream_buffer_.empty() ? -1 : stream_buffer_.front().time_seconds;
                    double bufferMax = stream_buffer_.empty() ? -1 : stream_buffer_.back().time_seconds;

                    static int log_counter = 0;
                    if (++log_counter % 10 == 1) {  // Log every 10 updates
                        std::cout << "🧵 [V2 THREAD] Decoded " << decoded_count << " frames for playback_time=" << playback_time
                                  << "s, buffer now has " << stream_buffer_.size() << " frames ["
                                  << bufferMin << "s.." << bufferMax << "s]" << std::endl;
                    }
                }
            }
        }

        // CPU OPTIMIZATION: Balance between CPU and responsiveness
        // Adaptive sleep times based on FPS - high FPS needs faster response
        // (is_high_fps already defined at loop start)
        // NOTE: Software-decoded codecs (AV1, VP9) have no hw pipeline — short sleeps
        // are critical so the buffer stays ahead of the render thread.
        int sleep_ms = 40;  // By default 40ms

        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            bool bufferBehind = stream_buffer_.empty();

            if (!stream_buffer_.empty()) {
                double currentMax = stream_buffer_.back().time_seconds;
                double desiredAheadTime = playback_time + std::max(buffer_window_ahead_, 1.5);
                bufferBehind = (currentMax < desiredAheadTime - 0.5);
            }

            // Adaptive sleep depending on buffer fill
            if (bufferBehind) {
                // CRITICAL: When buffer is empty (after seek/shuttle), decode at max speed
                // High FPS: no sleep at all for faster catch-up
                // Low FPS: minimal 10ms sleep to avoid CPU hogging
                sleep_ms = (stream_buffer_.empty() || is_high_fps) ? 0 : 10;
            } else if (stream_buffer_.size() >= static_cast<size_t>(is_high_fps ? 30 : 18)) {
                // 300ms was too long for software codecs: buffer drains while thread sleeps.
                // 80ms ≈ 2.4 frames at 30fps — enough CPU savings, safe margin.
                sleep_ms = is_high_fps ? 66 : 80;
            } else if (stream_buffer_.size() >= static_cast<size_t>(is_high_fps ? 20 : 12)) {
                sleep_ms = is_high_fps ? 33 : 50;
            } else if (stream_buffer_.size() < static_cast<size_t>(is_high_fps ? 12 : 8)) {
                sleep_ms = is_high_fps ? 16 : 20;
            }
        }

        // PROFILING: Output statistics thread loop every 100 iterations
        profile_iterations++;
        if (ENABLE_FULLRES_V2_PROFILING && profile_iterations >= 100) {
            auto profile_end_time = std::chrono::high_resolution_clock::now();
            uint64_t wall_clock_us = std::chrono::duration_cast<std::chrono::microseconds>(profile_end_time - profile_start_time).count();
            uint64_t profile_end_cpu_us = GetThreadCPUTimeMicroseconds();
            uint64_t actual_cpu_us = profile_end_cpu_us - profile_start_cpu_us;

            uint64_t avg_decode = total_decode_us / profile_iterations;
            uint64_t avg_seek = total_seek_us / profile_iterations;
            uint64_t avg_timestamp = total_timestamp_us / profile_iterations;
            uint64_t total_work_us = total_buffer_check_us + total_seek_us + total_decode_us + total_timestamp_us + total_duplicate_check_us + total_buffer_mgmt_us;

            // REAL CPU usage: (actual CPU time / wall-clock time) × 100%
            double real_cpu_percent = (static_cast<double>(actual_cpu_us) / static_cast<double>(wall_clock_us)) * 100.0;

            // Wall-clock CPU (for comparison)
            double wall_cpu_percent = (static_cast<double>(total_work_us) / static_cast<double>(wall_clock_us)) * 100.0;

            // Calculating decode FPS
            double wall_seconds = static_cast<double>(wall_clock_us) / 1000000.0;
            double decode_fps = static_cast<double>(total_frames_decoded) / wall_seconds;

            int active_decoders = g_active_decoder_count.load();
            double duplicate_percent = (total_frames_decoded > 0)
                ? (static_cast<double>(total_frames_duplicates) / static_cast<double>(total_frames_decoded)) * 100.0
                : 0.0;

            // Buffer diagnostics
            int buffer_size = 0;
            double buffer_min = -1, buffer_max = -1;
            {
                std::lock_guard<std::mutex> lock(buffer_mutex_);
                buffer_size = stream_buffer_.size();
                if (!stream_buffer_.empty()) {
                    buffer_min = stream_buffer_.front().time_seconds;
                    buffer_max = stream_buffer_.back().time_seconds;
                }
            }

            std::cout << "🧵 [THREAD CPU] REAL: " << std::fixed << std::setprecision(1) << real_cpu_percent << "%"
                      << " (WallClock: " << wall_cpu_percent << "%)"
                      << " | ActualCPU: " << (actual_cpu_us / 1000) << "ms / Wall: " << (wall_clock_us / 1000) << "ms"
                      << "\n   📊 Frames decoded: " << total_frames_decoded
                      << " (" << std::setprecision(1) << decode_fps << " fps)"
                      << ", Duplicates: " << total_frames_duplicates << " (" << std::setprecision(0) << duplicate_percent << "%)"
                      << ", Seeks: " << total_seeks
                      << " | Buffer: " << buffer_size << " frames ["
                      << std::setprecision(1) << buffer_min << "s.." << buffer_max << "s]"
                      << " | Decoders: " << active_decoders << std::endl;

            // Reset
            total_buffer_check_us = 0;
            total_seek_us = 0;
            total_decode_us = 0;
            total_timestamp_us = 0;
            total_duplicate_check_us = 0;
            total_buffer_mgmt_us = 0;
            total_frames_decoded = 0;
            total_frames_duplicates = 0;
            total_seeks = 0;
            profile_iterations = 0;
            profile_start_time = std::chrono::high_resolution_clock::now();
            profile_start_cpu_us = GetThreadCPUTimeMicroseconds();
        }

        // Wait before next iteration or until woken up
        std::unique_lock<std::mutex> lock(cv_mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(sleep_ms));
    }

    std::cout << "🧵 [FULL-RES V2 THREAD] Decoding thread exiting" << std::endl;
}
