#include "FSTPVideoModule_wrapper.h"

#include "../FSTPMainModule/WSGUI/FSTPOSDSystem.h"
#include "../FSTPMainModule/WSGUI/FSTPWindowManager.h"
#include "../FSTPMainModule/WSGUI/FSTPPixelBufferManager.h"
#include "../FSTPAudioModule/FSTPAudioModule_API.h"
#include "../FSTPAudioModule/FSTPAudioModule_wrapper.h"
#include "../FSTPMainModule/WSGUI/FSTPToolsMenu.h"
#include "FSTPPerformanceProfiler.h"
#include "FSTPCallCounter.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <future>
#include <iomanip>
#include <iostream>
#include <vector>
#include <cstring>
#include <utility>

namespace fs = std::filesystem;

// Debug control: set to true to enable verbose logging
static constexpr bool ENABLE_VIDEO_DEBUG = false;

// Export from WindowManager for updating color metadata renderer
extern "C" void FSTP_UpdatePlayerColorMetadata(int player_id, int colorspace, int color_range, int color_primaries, int color_trc);

FSTPVideoModuleWrapper::FSTPVideoModuleWrapper()
    : m_audio_module(nullptr)
    , m_frame_converter(std::make_unique<FSTPFrameConverter>())
    , m_current_frame_number(-1)
    , m_last_good_frame_number(-1) {
}

FSTPVideoModuleWrapper::~FSTPVideoModuleWrapper() {
    if (m_initialized) {
        Shutdown();
    }
}

bool FSTPVideoModuleWrapper::Initialize() {
    if (m_initialized) {
        return true;
    }

    m_frame_index = std::make_unique<FSTPSimpleVideoIndex>();
    m_initialized = true;
    return true;
}

void FSTPVideoModuleWrapper::Shutdown() {
    if (!m_initialized) {
        return;
    }

    if (m_loaded) {
        UnloadFile();
    }

    m_initialized = false;
}

bool FSTPVideoModuleWrapper::EnsureProxy(const std::string& filepath, const std::function<void(int)>& progressCallback) {
    fs::path sourcePath(filepath);
    if (!fs::exists(sourcePath)) {
        std::cerr << "[VIDEO] Source file does not exist: " << filepath << std::endl;
        return false;
    }

    // Check resolution, codec and GOP of original video
    AVFormatContext* fmt = nullptr;
    int source_height = 0;
    bool is_h264 = false;
    bool use_original_as_proxy = false;
    int max_gop_size = 0;

    if (avformat_open_input(&fmt, filepath.c_str(), nullptr, nullptr) == 0 &&
        avformat_find_stream_info(fmt, nullptr) >= 0) {
        int vindex = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (vindex >= 0) {
            AVCodecParameters* cp = fmt->streams[vindex]->codecpar;
            source_height = cp->height;
            is_h264 = (cp->codec_id == AV_CODEC_ID_H264);

            // CRITICAL: Analyze GOP to determine necessity of proxy
            AVPacket* packet = av_packet_alloc();
            int current_gop = 0;
            int frames_analyzed = 0;
            const int MAX_FRAMES_TO_ANALYZE = 1000;

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

            // Condition: ≤480p AND h264 AND GOP≤100 → use original
            // CRITICAL: When GOP > 100 ALWAYS create proxy for responsiveness!
            if (source_height <= 480 && is_h264 && max_gop_size <= 100 && max_gop_size > 0) {
                use_original_as_proxy = true;
                m_use_original_as_proxy = true;  // Flag to disable Full_Res
                std::cout << "[VIDEO] Video is " << source_height << "p H.264 with GOP=" << max_gop_size
                          << " - using original as proxy (no conversion needed)" << std::endl;
            } else if (max_gop_size > 100) {
                std::cout << "⚠️  [VIDEO] Large GOP detected (" << max_gop_size
                          << " frames) - will create GOP=25 proxy for responsive scrubbing" << std::endl;
            }
        }
        avformat_close_input(&fmt);
    }

    fs::path cacheDir = fs::path(FSTP::LowResDecoder::getCachePath()) / "proxy";
    fs::create_directories(cacheDir);

    m_proxy_path = cacheDir / (FSTP::LowResDecoder::generateFileId(filepath) + "_lowres.mp4");

    // If using original - create symlink or copy
    if (use_original_as_proxy) {
        if (!fs::exists(m_proxy_path)) {
            try {
                // Create symlink instead of copying to save space
                fs::create_symlink(sourcePath, m_proxy_path);
                std::cout << "[VIDEO] Created symlink to original: " << m_proxy_path << std::endl;
            } catch (...) {
                // If symlink failed, copy file
                fs::copy_file(sourcePath, m_proxy_path, fs::copy_options::overwrite_existing);
                std::cout << "[VIDEO] Copied original as proxy: " << m_proxy_path << std::endl;
            }
        } else {
            std::cout << "[VIDEO] Found existing proxy (original): " << m_proxy_path << std::endl;
        }

        if (progressCallback) {
            progressCallback(100);
        }
        return true;
    }

    // Normal logic - proxy conversion
    m_use_original_as_proxy = false;

    // CRITICAL: If old symlink on original (created before GOP check)
    // but GOP > 100, delete it and create real proxy with GOP=25
    if (fs::exists(m_proxy_path) && fs::is_symlink(m_proxy_path) && max_gop_size > 100) {
        std::cout << "[VIDEO] Removing old symlink (GOP > 100 requires real proxy): " << m_proxy_path << std::endl;
        fs::remove(m_proxy_path);
    } else if (fs::exists(m_proxy_path)) {
        std::cout << "[VIDEO] Found existing proxy: " << m_proxy_path << std::endl;
        if (progressCallback) {
            progressCallback(100);
        }
        return true;
    }

    std::cout << "[VIDEO] Proxy not found. Converting " << filepath << " -> " << m_proxy_path << std::endl;
    std::string generatedProxy;
    bool converted = FSTP::LowResDecoder::convertToLowRes(filepath, generatedProxy, progressCallback);
    if (!converted) {
        std::cerr << "[VIDEO] Proxy conversion failed." << std::endl;
        return false;
    }

    m_proxy_path = generatedProxy;
    return true;
}

bool FSTPVideoModuleWrapper::LoadFile(const std::string& filepath) {
    if (!m_initialized) {
        std::cerr << "[VIDEO] Module not initialized." << std::endl;
        return false;
    }

    const int osdPlayerId = (m_instance_id >= 0 ? m_instance_id : 0);

    // If file already loaded - set reload flag
    if (m_loaded) {
        m_file_reloading.store(true);
    }

    // Set loading mode before unloading old file
    SetPlayerLoadingState(osdPlayerId, true);
    SetPlayerLoadingProgress(osdPlayerId, 0);
    UpdateOSDDisplayMode(osdPlayerId, OSD_MODE_LOADING);
    SetPlayerLoadingStatus(osdPlayerId, "threading");

    if (m_loaded) {
        UnloadFile();
    }

    m_current_file = filepath;

    // Phase 1: Proxy conversion (0-50%)
    SetPlayerLoadingStatus(osdPlayerId, "proxy");  // FIX: Set status "proxy"

    auto proxyProgress = [playerId = osdPlayerId](int percent) {
        int scaledPercent = percent / 2;  // 0-100 -> 0-50
        SetPlayerLoadingProgress(playerId, scaledPercent);
        UpdateOSDLoadingProgress(playerId, scaledPercent);
    };

    if (!EnsureProxy(filepath, proxyProgress)) {
        SetPlayerLoadingProgress(osdPlayerId, 0);
        SetPlayerLoadingState(osdPlayerId, false);
        m_file_reloading.store(false);
        return false;
    }

    // Phase 2: Indexing (50-75%)
    SetPlayerLoadingStatus(osdPlayerId, "indexing");  // FIX: Set status "indexing"
    SetPlayerLoadingProgress(osdPlayerId, 50);
    UpdateOSDLoadingProgress(osdPlayerId, 50);

    if (!m_frame_index) {
        m_frame_index = std::make_unique<FSTPSimpleVideoIndex>();
    }

    std::cout << "[VIDEO] Building simple index for original file: " << m_current_file << std::endl;
    if (!m_frame_index->BuildIndex(m_current_file)) {
        SetPlayerLoadingState(osdPlayerId, false);
        SetPlayerLoadingProgress(osdPlayerId, 0);
        m_file_reloading.store(false);
        std::cerr << "[VIDEO] Failed to build simple index for source." << std::endl;
        return false;
    }

    SetPlayerLoadingProgress(osdPlayerId, 75);
    UpdateOSDLoadingProgress(osdPlayerId, 75);

    m_frames.clear();
    m_frames.resize(m_frame_index->GetTotalFrames());
    
    // Initialize time_ms and keyframe info from original video index for seeking reference
    // LowResDecoder will update time_ms with actual proxy file timing during decode
    for (size_t i = 0; i < m_frames.size(); ++i) {
        const SimpleFrameInfo* frame_info = m_frame_index->GetFrameInfo(static_cast<int>(i));
        if (frame_info) {
                // CRITICAL: Use ROUNDING instead of truncation (like in old decode.cpp)
                // At 29.97fps (33.367ms per frame) truncation creates jitter, rounding fixes it!
                // Old solution: (relative_us + 500) / 1000 = rounding to nearest
            m_frames[i].time_ms = static_cast<int64_t>(std::round(frame_info->time_seconds * 1000.0));

            // CRITICAL: Copy is_keyframe for GOP-aware decoding
            m_frames[i].is_keyframe = frame_info->is_keyframe;

            static int debug_count = 0;
            if (debug_count++ < 5) {
                std::cout << "[VIDEO] Frame " << i << ": initial time_ms=" << m_frames[i].time_ms
                          << " (from original " << std::fixed << std::setprecision(3) << frame_info->time_seconds << "s)"
                          << (frame_info->is_keyframe ? " [KEYFRAME]" : "") << std::endl;
            }
        } else {
            m_frames[i].time_ms = -1; // Invalid time
            m_frames[i].is_keyframe = false;
        }
    }
    
    m_current_index.store(0);

    m_duration.store(m_frame_index->GetDuration());
    m_frame_rate = m_frame_index->GetFrameRate();
    m_total_frames = static_cast<int>(m_frame_index->GetTotalFrames());
    m_video_width = m_frame_index->GetWidth();
    m_video_height = m_frame_index->GetHeight();

    // Phase 3: Initialize decoders (75-100%)
    if (!InitializeDecoders()) {
        SetPlayerLoadingState(osdPlayerId, false);
        SetPlayerLoadingProgress(osdPlayerId, 0);
        m_file_reloading.store(false);
        std::cerr << "[VIDEO] Failed to initialize decoders" << std::endl;
        return false;
    }

    m_loaded = true;

    SetPlayerLoadingProgress(osdPlayerId, 100);
    UpdateOSDLoadingProgress(osdPlayerId, 100);
    SetPlayerLoadingState(osdPlayerId, false);
    UpdateOSDDisplayMode(osdPlayerId, OSD_MODE_NORMAL);

    // Reset reload flag after successful loading
    m_file_reloading.store(false);

    // Test display immediately after loading
    std::cout << "[VIDEO] Testing immediate display of frame 0..." << std::endl;
    DisplayFrame(0);
    
    return true;
}

void FSTPVideoModuleWrapper::UnloadFile() {
    if (!m_loaded) {
        return;
    }

    ShutdownDecoders();

    m_frames.clear();
    if (m_frame_index) {
        m_frame_index->Clear();
    }

    m_current_file.clear();
    m_proxy_path.clear();
    m_use_original_as_proxy = false; // Reset flag
    m_duration.store(0.0);
    m_frame_rate = 0.0;
    m_total_frames = 0;
    m_last_displayed_frame = -1; // Reset for forced rendering of first frame
    m_loaded = false;

    // Switch OSD to "no file" mode only if it's NOT reload
    // On reload mode is already set to LOADING in LoadFile()
    if (!m_file_reloading.load()) {
        const int osdPlayerId = (m_instance_id >= 0 ? m_instance_id : 0);
        UpdateOSDDisplayMode(osdPlayerId, OSD_MODE_NO_FILE);
    }
}

bool FSTPVideoModuleWrapper::InitializeDecoders() {
    std::cout << "[VIDEO] InitializeDecoders: Using LowCachedDecoderManager (the proven solution)" << std::endl;

    // Use the working LowCachedDecoderManager from old code
    try {
        m_low_cached_manager = std::make_unique<FSTP::LowCachedDecoderManager>(
            m_proxy_path,
            m_frames,
            m_current_index,
            m_low_res_range,
            m_high_res_window,
            m_playing,
            m_speed,
            m_reverse
        );
    } catch (const std::exception& ex) {
        std::cerr << "[VIDEO] Failed to create low cached decoder manager: " << ex.what() << std::endl;
        return false;
    }

    if (m_low_cached_manager) {
        m_low_cached_manager->run();
    }

    // V2: Streaming full-res decoder (only if video > 480p or not h264)
    // For ≤480p h264 video we use only proxy - save resources!
    if (!m_use_original_as_proxy) {
        std::cout << "[VIDEO] InitializeDecoders: Creating FSTPFullResDecoderV2 (streaming mode)" << std::endl;
        try {
            m_full_res_decoder = std::make_unique<FSTPFullResDecoderV2>(m_current_file);

            if (m_full_res_decoder && m_full_res_decoder->IsInitialized()) {
                 std::cout << "✅ [VIDEO] FSTPFullResDecoderV2 initialized successfully" << std::endl;
                 std::cout << "   Display: " << m_full_res_decoder->GetWidth() << "x"
                          << m_full_res_decoder->GetHeight() << " (downscaled from "
                          << m_full_res_decoder->GetNativeWidth() << "x"
                          << m_full_res_decoder->GetNativeHeight() << ")" << std::endl;
            } else {
                std::cerr << "⚠️  [VIDEO] FSTPFullResDecoderV2 initialization failed" << std::endl;
                m_full_res_decoder.reset();
            }
        } catch (const std::exception& ex) {
            std::cerr << "❌ [VIDEO] Failed to create full-res decoder: " << ex.what() << std::endl;
            m_full_res_decoder.reset();
        }
    } else {
        std::cout << "[VIDEO] ⚡ Skipping Full-Res decoder (using original ≤480p h264 as proxy - resource optimization)" << std::endl;
    }

    // Progressive scan functionality removed

    m_decoders_active.store(true);
    return true;
}

void FSTPVideoModuleWrapper::ShutdownDecoders() {
    m_decoders_active.store(false);

    // V2: Shutdown streaming decoder
    if (m_full_res_decoder) {
        m_full_res_decoder->RequestStop();
        m_full_res_decoder->ClearBuffer();
        m_full_res_decoder.reset();
    }

    if (m_low_cached_manager) {
        m_low_cached_manager->stop();
        m_low_cached_manager.reset();
    }
}

void FSTPVideoModuleWrapper::NotifyDecodersOfFrameChange(int frame_number) {
    m_current_index.store(frame_number);
    if (m_low_cached_manager) {
        m_low_cached_manager->notifyFrameChange();
    }
    // V2: Streaming decoder updates itself based on current time
    // No need to notify frame change
}

bool FSTPVideoModuleWrapper::Play() {
    if (!m_loaded) {
        return false;
    }
    m_playing.store(true);
    return true;
}

bool FSTPVideoModuleWrapper::Pause() {
    m_playing.store(false);
    return true;
}

bool FSTPVideoModuleWrapper::Stop() {
    m_playing.store(false);
    return true;
}

void FSTPVideoModuleWrapper::SetSpeed(double speed) {
    m_speed.store(speed);
    NotifyDecodersOfFrameChange(m_current_index.load());
}

void FSTPVideoModuleWrapper::SetSpeedInstant(double speed) {
    SetSpeed(speed);
}

void FSTPVideoModuleWrapper::SetReverse(bool reverse) {
    m_reverse.store(reverse);
    NotifyDecodersOfFrameChange(m_current_index.load());
}

double FSTPVideoModuleWrapper::GetDuration() const {
    return m_duration.load();
}

bool FSTPVideoModuleWrapper::IsPlaying() const {
    return m_playing.load();
}

bool FSTPVideoModuleWrapper::IsLoaded() const {
    return m_loaded;
}

double FSTPVideoModuleWrapper::GetSpeed() const {
    return m_speed.load();
}

double FSTPVideoModuleWrapper::GetActualSpeed() const {
    return m_speed.load();
}

bool FSTPVideoModuleWrapper::IsReverse() const {
    return m_reverse.load();
}

int FSTPVideoModuleWrapper::GetWidth() const {
    return m_video_width;
}

int FSTPVideoModuleWrapper::GetHeight() const {
    return m_video_height;
}

double FSTPVideoModuleWrapper::GetFrameRate() const {
    return m_frame_rate;
}

int FSTPVideoModuleWrapper::GetTotalFrames() const {
    return m_total_frames;
}

std::string FSTPVideoModuleWrapper::GetVideoCodecName() const {
    if (m_frame_index) {
        return m_frame_index->GetCodecName();
    }
    return "Unknown";
}

// Functions for developer visualization
const std::vector<FSTP::FrameInfo>& FSTPVideoModuleWrapper::GetFrameIndex() const {
    return m_frames;
}

int FSTPVideoModuleWrapper::GetCurrentFrameIndex() const {
    return m_current_index.load();
}

void FSTPVideoModuleWrapper::GetBufferRanges(int& bufferStart, int& bufferEnd, int& highResStart, int& highResEnd) const {
    int current_frame = m_current_index.load();
    int total_frames = static_cast<int>(m_frames.size());

    if (total_frames == 0) {
        bufferStart = bufferEnd = highResStart = highResEnd = 0;
        return;
    }

    // Low resolution range (large buffer around current frame)
    bufferStart = std::max(0, current_frame - m_low_res_range / 2);
    bufferEnd = std::min(total_frames - 1, current_frame + m_low_res_range / 2);

    // High resolution range (smaller buffer around current frame)
    highResStart = std::max(0, current_frame - m_high_res_window / 2);
    highResEnd = std::min(total_frames - 1, current_frame + m_high_res_window / 2);
}

bool FSTPVideoModuleWrapper::IsFastBufferReady() const {
    return m_decoders_active.load();
}

bool FSTPVideoModuleWrapper::IsFullBufferReady() const {
    return false;
}

void FSTPVideoModuleWrapper::SetAudioModule(FSTPAudioModuleWrapper* audio_module) {
    m_audio_module = audio_module;
}

int FSTPVideoModuleWrapper::GetCurrentAudioFrame() const {
    // Use real audio synchronization
    if (!m_audio_module) {
        // Fallback to simulation only if no audio module
        static auto start_time = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time);

        double elapsed_seconds = elapsed.count() / 1000.0;
        double frame_rate = (m_frame_rate > 0.0) ? m_frame_rate : 25.0;
        int simulated_frame = static_cast<int>(elapsed_seconds * frame_rate);

        // Clamp to available frames
        int total_frames = static_cast<int>(m_frame_index ? m_frame_index->GetTotalFrames() : 0);
        if (total_frames > 0) {
            simulated_frame = simulated_frame % total_frames; // Loop for testing
        }

        static int log_counter = 0;
        log_counter++;
        if (log_counter <= 5 || log_counter % 100 == 0) {
            std::cout << "[VIDEO] No audio module - simulated time: " << std::fixed << std::setprecision(2)
                      << elapsed_seconds << "s → frame " << simulated_frame << std::endl;
        }

        return simulated_frame;
    }

    // FIXED: Use audio time directly as in old code
    // Remove double conversion time→frame→time which lost precision
    // IMPORTANT: Video module is leader, so request position every frame (60 FPS)
    // NO SMOOTHING - old code worked directly, return to proven approach
    double audio_time = FSTPAudioModule_API::GetPosition(m_audio_module);

    // Find corresponding video frame by time (supports variable FPS)
    if (audio_time >= 0.0 && m_frame_index && m_frame_index->IsReady()) {
        // Direct search by time - exactly like in old findClosestFrameIndexByTime
        int video_frame = m_frame_index->FindFrameByTime(audio_time);
        
        // Clamp to valid range - but first check boundaries
        int total_frames = static_cast<int>(m_frame_index->GetTotalFrames());
        if (total_frames > 0) {
            // If FindFrameByTime returned -1 or invalid value, DO NOT reset to 0!
            if (video_frame < 0) {
                // Use last frame instead of resetting to beginning
                video_frame = total_frames - 1;
                std::cout << " ⚠️  FindFrameByTime returned " << video_frame << " for time " << audio_time
                          << "s, using last frame " << (total_frames - 1) << std::endl;
            } else if (video_frame >= total_frames) {
                video_frame = total_frames - 1;
            }
        }
        
        if (ENABLE_VIDEO_DEBUG) {
            static int log_counter = 0;
            log_counter++;
            if (log_counter <= 10 || log_counter % 50 == 0 || video_frame == 0) { // Always log when video_frame is 0!
                std::cout << "[VIDEO] Direct audio sync: audio_time=" << std::fixed << std::setprecision(4) << audio_time << "s"
                          << " → video_frame=" << video_frame << " (no precision loss)";

                // Additional debug when video_frame is 0 but audio_time is large
                if (video_frame == 0 && audio_time > 4.0) {
                    std::cout << " ⚠️  PROBLEM: Large audio time but video frame is 0!";
                }
                std::cout << std::endl;
            }
        }

        return video_frame;
    }

    // Fallback: if index not ready, try simple time to frame conversion
    if (audio_time > 0.0) {
        double frame_rate = (m_frame_rate > 0.0) ? m_frame_rate : 25.0;
        return static_cast<int>(audio_time * frame_rate);
    }

    return 0;
}

void FSTPVideoModuleWrapper::SetInstanceID(int instance_id) {
    m_instance_id = instance_id;
}

int FSTPVideoModuleWrapper::GetInstanceID() const {
    return m_instance_id;
}

bool FSTPVideoModuleWrapper::SubmitFrameToTexture(const std::shared_ptr<AVFrame>& frame,
                                                   int frame_number,
                                                   double timestamp) {
    // ZERO-COPY: No profiling memcpy - it's gone!

    if (!frame) {
        return false;
    }

    // ZERO-COPY ARCHITECTURE: pass AVFrame directly without copying!
    // No thread_local buffers, no memcpy - only refcount increment

    // DIAGNOSTICS: Log format and color metadata (commented)
    // static int format_log = 0;
    // if (++format_log % 100 == 1 || format_log <= 5) {
    //     const char* format_name = av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format));
    //     const char* range_str = (frame->color_range == 1) ? "LIMITED(16-235)" :
    //                            (frame->color_range == 2) ? "FULL(0-255)" : "UNSPECIFIED";
    //     std::cout << "🚀 [ZERO-COPY SUBMIT] Frame " << frame_number
    //               << ": format=" << (format_name ? format_name : "UNKNOWN")
    //               << " (" << frame->format << "), range=" << frame->color_range << " (" << range_str << ")"
    //               << ", resolution=" << frame->width << "x" << frame->height
    //               << ", refcount=" << frame.use_count() << std::endl;
    // }

    // Support YUV420P and NV12 formats (zero-copy!)
    if (frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUVJ420P ||
        frame->format == AV_PIX_FMT_NV12) {
        // Pass color metadata of current frame (1:1)
        auto norm = [](int v, int unspecified_code) { return (v == unspecified_code) ? -1 : v; };
        int cs = norm(frame->colorspace, 2);        // AVCOL_SPC_UNSPECIFIED == 2
        int pr = norm(frame->color_primaries, 2);   // AVCOL_PRI_UNSPECIFIED == 2
        int tr = norm(frame->color_trc, 2);         // AVCOL_TRC_UNSPECIFIED == 2
        int rg = (frame->color_range == 0 /* AVCOL_RANGE_UNSPECIFIED */) ? -1 : frame->color_range;

        FSTP_UpdatePlayerColorMetadata(m_instance_id, cs, rg, pr, tr);

        // ZERO-COPY: Send shared_ptr directly to pixel buffer manager
        // No memcpy! Only increment refcount on AVFrame
        SubmitAVFrame(m_instance_id, frame, timestamp, frame_number);

        if (ENABLE_VIDEO_DEBUG) {
            static int zero_copy_log = 0;
            if (++zero_copy_log % 100 == 1) {
                const char* format_name = av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format));
                std::cout << "🚀 [ZERO-COPY] Submitted AVFrame " << frame_number
                          << " format=" << (format_name ? format_name : "UNKNOWN")
                          << " (" << frame->width << "x" << frame->height << ")"
                          << ", Y_linesize=" << frame->linesize[0]
                          << ", UV_linesize=" << frame->linesize[1]
                          << ", refcount=" << frame.use_count() << std::endl;
            }
        }

        return true;
    }

    // For other formats return false
    std::cerr << "❌ [VIDEO] Unsupported frame format: " << frame->format << std::endl;
    return false;
}

void FSTPVideoModuleWrapper::DisplayFrame(int frame_number) {
    static int debug_call_count = 0;
    debug_call_count++;

    // Profiling DisplayFrame
    static uint64_t total_checks_us = 0;
    static uint64_t total_v2_get_us = 0;
    static uint64_t total_mutex_us = 0;
    static uint64_t total_submit_us = 0;
    static int df_perf_samples = 0;

    auto t_df_start = std::chrono::high_resolution_clock::now();

    if (debug_call_count <= 3) { // Minimal logging to avoid Metal conflicts
        std::cout << "[VIDEO] DisplayFrame called #" << debug_call_count
                  << " with frame_number=" << frame_number << std::endl;
    }

    // DisplayFrame profiling every 100 calls
    if (ENABLE_VIDEO_DEBUG) {
        static int df_report_counter = 0;
        if (++df_report_counter >= 100) {
            if (df_perf_samples > 0) {
                std::cout << "⏱️  [DisplayFrame PERF] Checks: " << (total_checks_us / df_perf_samples)
                          << "μs, V2Get: " << (total_v2_get_us / df_perf_samples)
                          << "μs, Mutex: " << (total_mutex_us / df_perf_samples)
                          << "μs, Submit: " << (total_submit_us / df_perf_samples) << "μs" << std::endl;
            }
            df_report_counter = 0;
            total_checks_us = 0;
            total_v2_get_us = 0;
            total_mutex_us = 0;
            total_submit_us = 0;
            df_perf_samples = 0;
        }
    }
    
    // PROTECTION AGAINST RACE CONDITION: exit if file is reloading
    if (m_file_reloading.load()) {
        if (debug_call_count <= 3) {
            std::cout << "[VIDEO] DisplayFrame: file is reloading, skipping" << std::endl;
        }
        return;
    }

    if (!m_loaded || !m_frame_index || !m_frame_index->IsReady()) {
        if (debug_call_count <= 3) {
            std::cout << "[VIDEO] DisplayFrame: waiting for index (ready="
                      << (m_frame_index ? m_frame_index->IsReady() : false) << ")" << std::endl;
        }
        return;
    }

    int clamped = std::max(0, std::min(frame_number, static_cast<int>(m_frame_index->GetTotalFrames()) - 1));
    m_current_index.store(clamped);

    // Get frame info from simple index
    const SimpleFrameInfo* frame_info = m_frame_index->GetFrameInfo(clamped);
    if (!frame_info) {
        if (debug_call_count <= 3) {
            std::cout << "[VIDEO] No frame info for frame " << clamped << std::endl;
        }
        return;
    }
    // Use frames decoded by LowCachedDecoderManager (the proven solution)
    if (clamped >= static_cast<int>(m_frames.size())) {
        if (debug_call_count <= 3) {
            std::cout << "[VIDEO] Frame index " << clamped << " out of range (size=" << m_frames.size() << ")" << std::endl;
        }
        return;
    }

    // SAFE CHECK: make sure m_frames was not cleared between checks
    if (!m_loaded || clamped >= static_cast<int>(m_frames.size())) {
        if (debug_call_count <= 3) {
            std::cout << "[VIDEO] Race condition detected: frames cleared during DisplayFrame" << std::endl;
        }
        return;
    }

    auto t_after_checks = std::chrono::high_resolution_clock::now();
    total_checks_us += std::chrono::duration_cast<std::chrono::microseconds>(t_after_checks - t_df_start).count();

    FSTP::FrameInfo& info = m_frames[clamped];
    std::shared_ptr<AVFrame> frame;

    // CRITICAL: Use DECODED time (info.time_ms) if available,
    // otherwise fallback to calculated time from SimpleIndex
    // This eliminates frame drift when seeking (problem "2 frames forward")
    double timestamp = frame_info->time_seconds;  // Fallback
    if (info.time_ms >= 0) {
        timestamp = info.time_ms / 1000.0;  // Use REAL DECODED time

        if (ENABLE_VIDEO_DEBUG) {
            static int time_source_log = 0;
            if (time_source_log++ < 10) {
                double diff_ms = (frame_info->time_seconds - timestamp) * 1000.0;
                std::cout << "🎯 [TIME SOURCE] Frame " << clamped
                          << ": Using DECODED time=" << timestamp << "s"
                          << " (SimpleIndex would be " << frame_info->time_seconds << "s"
                          << ", diff=" << diff_ms << "ms)" << std::endl;
            }
        }
    }

    // V2: Try streaming full-res decoder at slow speeds (≤1x including pause)
    // Get actual playback speed from audio module (not target speed)
    double actual_speed = 0.0;
    if (m_audio_module) {
        actual_speed = m_audio_module->GetActualSpeed();
    }

    // Use V2 at speeds ≤ 1x: pause (0x), slow motion (0.5x), normal (1x)
    bool slow_or_normal_speed = (actual_speed <= 1.0);

    // DEBUG: Log V2 decoder state per player
    if (ENABLE_VIDEO_DEBUG) {
        static int v2_state_log = 0;
        if (v2_state_log++ % 300 == 0) {
            std::cout << "🎞️ [V2 DEBUG] Player " << this
                      << ": decoder=" << (m_full_res_decoder ? "YES" : "NO")
                      << ", speed=" << actual_speed << "x"
                      << ", condition=" << (slow_or_normal_speed ? "ACTIVE" : "INACTIVE") << std::endl;
        }
    }

    auto t_before_v2_get = std::chrono::high_resolution_clock::now();

    // Use full-res decoder when at slow/normal speed (including pause!)
    if (m_full_res_decoder && slow_or_normal_speed) {
        // Inform background thread of current playback time (lightweight operation)
        m_full_res_decoder->SetPlaybackTime(timestamp);
        m_full_res_decoder->ClearStopRequest();

        // Get shared_ptr copy from buffer (safe - extends frame lifetime)
        frame = m_full_res_decoder->GetFrameForTime(timestamp);

        if (frame) {
            if (ENABLE_VIDEO_DEBUG) {
                static int v2_use_counter = 0;
                v2_use_counter++;
                if (v2_use_counter % 30 == 1 || v2_use_counter <= 5) {
                    // Calculate frame number from PTS
                    double frame_time = 0.0;
                    if (frame->time_base.den > 0) {
                        frame_time = (frame->pts * av_q2d(frame->time_base));
                    }
                    int estimated_frame = static_cast<int>(frame_time * 25.0 + 0.5); // Assume 25 FPS

                    std::cout << "🎬 [VIDEO] V2 frame: requested=" << clamped
                              << ", estimated=" << estimated_frame
                              << " (offset=" << (estimated_frame - clamped) << ")"
                              << ", req_time=" << timestamp << "s"
                              << ", frame_time=" << frame_time << "s"
                              << ", resolution=" << frame->width << "x" << frame->height << std::endl;
                }
            }
        } else if (ENABLE_VIDEO_DEBUG && debug_call_count <= 5) {
            std::cout << "⚠️  [VIDEO] V2 GetFrameForTime returned nullptr for time=" << timestamp << "s" << std::endl;
        }
    } else if (m_full_res_decoder && actual_speed > 1.4) {
        // Clear buffer ONLY when speed > 1x (fast forward / seeking)
        static bool buffer_cleared = false;
        static double last_actual_speed = -1.0;

        // Reset flag if speed changed significantly
        if (std::abs(actual_speed - last_actual_speed) > 0.1) {
            buffer_cleared = false;
            last_actual_speed = actual_speed;
        }

        if (!buffer_cleared) {
            m_full_res_decoder->ClearBuffer();
            buffer_cleared = true;
            if (debug_call_count <= 3) {
                std::cout << "[VIDEO] Cleared V2 buffer (speed=" << actual_speed
                          << "x > 1.0, fast forward mode)" << std::endl;
            }
        }
    }

    auto t_after_v2_get = std::chrono::high_resolution_clock::now();
    total_v2_get_us += std::chrono::duration_cast<std::chrono::microseconds>(t_after_v2_get - t_before_v2_get).count();

    // SAFE MUTEX: check that object is still valid
    // auto t_before_mutex = std::chrono::high_resolution_clock::now();  // Unused - performance timing disabled
    try {
        std::lock_guard<std::mutex> lock(info.mutex);

        // DEBUG: Check frame state at mutex entry
        static int mutex_entry_counter = 0;
        mutex_entry_counter++;
        if (mutex_entry_counter % 30 == 1 || mutex_entry_counter <= 10) {
            // std::cout << "🔒 [MUTEX ENTRY] clamped=" << clamped
                      // << ", frame=" << (frame ? "SET" : "NULL")
                      // << (frame ? (", resolution=" + std::to_string(frame->width) + "x" + std::to_string(frame->height)) : "")
                      // << (frame ? (", ptr=" + std::to_string((size_t)frame.get())) : "")
                      // << std::endl;
        }

        // If V2 didn't provide a frame, check legacy sources
        // PRIORITY 1: Legacy full-resolution frame (from old decoder, if still present)
        if (!frame && info.is_ready.load() && info.frame) {
            frame = info.frame;
            if (debug_call_count <= 3) {
                std::cout << "🎬 [VIDEO] Using LEGACY full-res frame for frame " << clamped
                          << " (original quality at 1x speed)" << std::endl;
            }
        }
        // PRIORITY 2: Low-res proxy frame (always available)
        else if (!frame && info.low_res_frame) {
            // CRITICAL: ALWAYS check PTS before using low_res_frame
            // This prevents jittering when fast-rewinding 24-32x
            bool is_seek_mode = false;
            int frame_jump = abs(clamped - m_last_good_frame_number);
            if (m_last_good_frame_number >= 0 && frame_jump > 30) {
                is_seek_mode = true;
            }

            bool pts_valid = true;
            // ALWAYS check PTS (not only when seeking!)
            double frame_time = -1.0;
            if (info.low_res_frame->best_effort_timestamp != AV_NOPTS_VALUE) {
                if (info.time_base.den > 0) {
                    frame_time = info.low_res_frame->best_effort_timestamp * av_q2d(info.time_base);
                }
            } else if (info.low_res_frame->pts != AV_NOPTS_VALUE) {
                if (info.time_base.den > 0) {
                    frame_time = info.low_res_frame->pts * av_q2d(info.time_base);
                }
            }

            if (frame_time >= 0.0) {
                double time_diff = fabs(frame_time - timestamp);
                // Strict check: ±0.1s when seeking, ±0.5s when playing
                double max_diff = is_seek_mode ? 0.1 : 0.5;
                if (time_diff > max_diff) {
                    pts_valid = false;
                    if (debug_call_count <= 5) {
                        std::cout << "⚠️  [PTS CHECK] Frame " << clamped << " has time "
                                  << std::fixed << std::setprecision(3) << frame_time << "s"
                                  << " but requested " << timestamp << "s (diff=" << (time_diff * 1000.0) << "ms > "
                                  << (max_diff * 1000.0) << "ms)"
                                  << " - SKIP to avoid jitter!" << std::endl;
                    }
                }
            }

            if (pts_valid) {
                frame = info.low_res_frame;
                if (debug_call_count <= 5) {
                    std::cout << "[VIDEO] Using low_res_frame (proxy) for frame " << clamped
                              << ", resolution=" << frame->width << "x" << frame->height << std::endl;
                }
            }
        }
        else if (frame) {
            static int v2_keep_counter = 0;
            v2_keep_counter++;
            if (v2_keep_counter % 30 == 1 || v2_keep_counter <= 10) {
                //std::cout << "✅ [VIDEO] Keeping V2 frame, skipping proxy. clamped=" << clamped
                          //<< ", resolution=" << frame->width << "x" << frame->height
                          //<< ", ptr=" << (void*)frame.get() << std::endl;
            }
        }
        // PRIORITY 3: Cached frame (legacy fallback)
        else if (info.cached_frame) {
            frame = info.cached_frame;
            if (debug_call_count <= 3) {
                std::cout << "[VIDEO] Using cached_frame for frame " << clamped << std::endl;
            }
        } else {
            // No decoded frame available yet - try to find nearest ready frame
            static int fallback_search_counter = 0;
            fallback_search_counter++;

            // CRITICAL: Detect seek (large jump in time)
            // If jump > 30 frames, this is explicit seek - need freeze frame
            bool is_seek = false;
            int frame_distance = abs(clamped - m_last_good_frame_number);
            if (m_last_good_frame_number >= 0 && frame_distance > 30) {
                is_seek = true;
                if (debug_call_count <= 3 || fallback_search_counter % 30 == 1) {
                    std::cout << "🔍 [SEEK DETECTED] Jump from frame " << m_last_good_frame_number
                              << " to " << clamped << " (distance: " << frame_distance << " frames)" << std::endl;
                }
            }

            if (debug_call_count <= 3 || fallback_search_counter % 30 == 1) {
                std::cout << "⚠️  [FALLBACK SEARCH #" << fallback_search_counter << "] No decoded frame available for frame " << clamped
                          << " (is_decoding=" << info.is_decoding.load()
                          << ", is_ready=" << info.is_ready.load()
                          << ", is_seek=" << is_seek << "), searching for nearest (SLOW!)" << std::endl;
            }

            // FAST EXPONENTIAL SEARCH: Check frames at exponentially increasing distances
            // Instead of checking 1,2,3,4,5... (linear O(n) with n mutex locks)
            // Check 1,2,4,8,16,32... (logarithmic O(log n) with much fewer mutex locks)
            //
            // IMPORTANT: When seeking, do not use fallback search - instead use freeze frame
            std::shared_ptr<AVFrame> nearest_frame = nullptr;
            int found_idx = -1;
            int search_radius = is_seek ? 0 : 64; // Disable search when seeking!

            // Exponential backward search (prefer backward - more likely to be decoded)
            // Try: -1, -2, -4, -8, -16, -32, -64
            for (int step = 1; step <= search_radius; step *= 2) {
                int idx = clamped - step;
                if (idx >= 0 && idx < static_cast<int>(m_frames.size())) {
                    // Quick check without lock first (peek at atomic state)
                    // This avoids expensive mutex locks on empty frames
                    std::lock_guard<std::mutex> search_lock(m_frames[idx].mutex);
                    if (m_frames[idx].low_res_frame &&
                        m_frames[idx].low_res_frame->data[0]) {
                        nearest_frame = m_frames[idx].low_res_frame;
                        found_idx = idx;
                        if (debug_call_count <= 3) {
                            std::cout << "⚡ [FAST SEARCH] Found backward frame " << idx
                                      << " (offset -" << step << ") for " << clamped << std::endl;
                        }
                        break;
                    }
                }
            }

            // If not found backward, try exponential forward search
            // Try: +1, +2, +4, +8, +16, +32, +64
            if (!nearest_frame) {
                for (int step = 1; step <= search_radius; step *= 2) {
                    int idx = clamped + step;
                    if (idx >= 0 && idx < static_cast<int>(m_frames.size())) {
                        std::lock_guard<std::mutex> search_lock(m_frames[idx].mutex);
                        if (m_frames[idx].low_res_frame &&
                            m_frames[idx].low_res_frame->data[0]) {
                            nearest_frame = m_frames[idx].low_res_frame;
                            found_idx = idx;
                            if (debug_call_count <= 3) {
                                std::cout << "⚡ [FAST SEARCH] Found forward frame " << idx
                                          << " (offset +" << step << ") for " << clamped << std::endl;
                            }
                            break;
                        }
                    }
                }
            }

            // If exponential search found nothing, try immediate neighbors (linear search within small range)
            // This catches frames that were skipped by exponential steps (e.g., -3, -5, -6, -7)
            if (!nearest_frame) {
                for (int offset = 1; offset <= 8 && (clamped - offset) >= 0; offset++) {
                    int idx = clamped - offset;
                    if (idx < static_cast<int>(m_frames.size())) {
                        std::lock_guard<std::mutex> search_lock(m_frames[idx].mutex);
                        if (m_frames[idx].low_res_frame &&
                            m_frames[idx].low_res_frame->data[0]) {
                            nearest_frame = m_frames[idx].low_res_frame;
                            found_idx = idx;
                            if (debug_call_count <= 3) {
                                std::cout << "⚡ [NEIGHBOR SEARCH] Found nearby backward frame " << idx
                                          << " for " << clamped << std::endl;
                            }
                            break;
                        }
                    }
                }
            }

            // If found a nearby frame, use it ONLY if timestamp is reasonable
            if (nearest_frame && found_idx >= 0) {
                // CRITICAL: Check REAL TIMESTAMP via PTS and timebase!
                // This is much more accurate than just frame number

                // 1. Calculate requested time
                double requested_time = timestamp;

                // 2. Get REAL time of found frame via PTS
                // CRITICAL: use time_base from FrameInfo, not from AVFrame!
                double found_frame_time = -1.0;

                // Get FrameInfo for found frame
                const FSTP::FrameInfo& found_info = m_frames[found_idx];

                // Try best_effort_timestamp (most accurate)
                if (nearest_frame->best_effort_timestamp != AV_NOPTS_VALUE) {
                    if (found_info.time_base.den > 0) {
                        found_frame_time = nearest_frame->best_effort_timestamp * av_q2d(found_info.time_base);
                    }
                }
                // Fallback to pts
                else if (nearest_frame->pts != AV_NOPTS_VALUE) {
                    if (found_info.time_base.den > 0) {
                        found_frame_time = nearest_frame->pts * av_q2d(found_info.time_base);
                    }
                }
                // Fallback to calculation by frame rate
                else if (m_frame_rate > 0.0) {
                    found_frame_time = found_idx / m_frame_rate;
                }

                // 3. Check difference by REAL TIME
                // IMPORTANT:
                // - When seeking: STRICT match ±0.1s (3 frames at 30fps)
                // - When playing: allow ±0.5s (15 frames) - for fast rewind 24-32x
                // - Old value 10s was too lenient and caused jittering
                // Goal: block visible desyncs (>0.5s is noticeable), freeze frame until needed frame is decoded
                const double MAX_TIME_DIFFERENCE = is_seek ? 0.1 : 0.5; // STRICT validation!

                if (found_frame_time >= 0.0) {
                    double time_diff = fabs(found_frame_time - requested_time);

                    if (time_diff <= MAX_TIME_DIFFERENCE) {
                        frame = nearest_frame;
                        if (debug_call_count <= 3 || fallback_search_counter % 10 == 1) {
                            std::cout << "✅ [PTS VALIDATED] Using frame " << found_idx
                                      << " (PTS: " << std::fixed << std::setprecision(3) << found_frame_time << "s)"
                                      << " for requested time " << requested_time << "s"
                                      << " (diff: " << (time_diff * 1000.0) << "ms)" << std::endl;
                        }
                    } else {
                        // REAL time differs too much - DESYNC!
                        if (debug_call_count <= 3 || fallback_search_counter % 10 == 1) {
                            std::cout << "❌ [PTS MISMATCH] Found frame " << found_idx
                                      << " at time " << std::fixed << std::setprecision(3) << found_frame_time << "s"
                                      << " but requested " << requested_time << "s"
                                      << " (diff: " << std::setprecision(2) << time_diff << "s > " << MAX_TIME_DIFFERENCE << "s)"
                                      << " - FREEZE FRAME to avoid desync!" << std::endl;
                        }
                        nearest_frame = nullptr; // Reset - freeze frame
                    }
                } else {
                    // No PTS information - use old check by frame number
                    int frame_distance = abs(found_idx - clamped);
                    const int MAX_FRAME_DISTANCE = 30; // More strict: 30 frames (~1.25 sec)

                    if (frame_distance <= MAX_FRAME_DISTANCE) {
                        frame = nearest_frame;
                        if (debug_call_count <= 3) {
                            std::cout << "⚠️  [NO PTS] Using frame " << found_idx
                                      << " (distance: " << frame_distance << " frames, no PTS validation)" << std::endl;
                        }
                    } else {
                        if (debug_call_count <= 3) {
                            std::cout << "❌ [FRAME DISTANCE TOO FAR] " << frame_distance
                                      << " > " << MAX_FRAME_DISTANCE << " - FREEZE FRAME" << std::endl;
                        }
                        nearest_frame = nullptr;
                    }
                }
            }

            // If no suitable frame found or seek detected, use freeze frame
            if (!nearest_frame) {
                // FREEZE FRAME: Hold last good frame while needed segment is being decoded
                static int freeze_frame_counter = 0;
                freeze_frame_counter++;

                if (is_seek) {
                    // When seeking - this is expected behavior
                    if (freeze_frame_counter % 10 == 1 || debug_call_count <= 3) {
                        std::cout << "🧊 [FREEZE FRAME] Seek detected, holding last good frame "
                                  << m_last_good_frame_number
                                  << " while decoding segment for frame " << clamped << std::endl;
                    }
                } else {
                    // If not seek but frame unavailable - decoder is lagging
                    if (freeze_frame_counter % 10 == 1 || debug_call_count <= 3) {
                        std::cout << "🧊 [FREEZE FRAME #" << freeze_frame_counter
                                  << "] No suitable frame for " << clamped
                                  << ", holding last good frame " << m_last_good_frame_number
                                  << " (decoder catching up...)" << std::endl;
                    }
                }

                // Try to use the last good frame if available
                if (m_last_good_frame_number >= 0 && m_last_good_frame_number < static_cast<int>(m_frames.size())) {
                    std::lock_guard<std::mutex> last_lock(m_frames[m_last_good_frame_number].mutex);
                    if (m_frames[m_last_good_frame_number].low_res_frame) {
                        frame = m_frames[m_last_good_frame_number].low_res_frame;
                    }
                }

                // If still no frame, keep showing last good frame (sticky frame for pause)
                if (!frame && m_last_good_frame_number >= 0 && m_last_good_frame_number < static_cast<int>(m_frames.size())) {
                    // Force re-submit last good frame to prevent black screen during pause
                    std::lock_guard<std::mutex> last_lock(m_frames[m_last_good_frame_number].mutex);
                    if (m_frames[m_last_good_frame_number].low_res_frame) {
                        if (SubmitFrameToTexture(m_frames[m_last_good_frame_number].low_res_frame,
                                               m_last_good_frame_number, timestamp)) {
                            if (debug_call_count <= 3) {
                                std::cout << "[VIDEO] Re-submitted sticky frame " << m_last_good_frame_number
                                          << " to prevent black screen" << std::endl;
                            }
                        }
                    }
                }
                return;
            }
        }
    } catch (const std::system_error& e) {
        // Mutex was destroyed during UnloadFile - this is normal
        if (debug_call_count <= 3) {
            std::cout << "[VIDEO] Mutex access failed during file reload: " << e.what() << std::endl;
        }
        return;
    } catch (...) {
        // Any other exceptions when accessing frames
        if (debug_call_count <= 3) {
            std::cout << "[VIDEO] Exception during frame access (likely during file reload)" << std::endl;
        }
        return;
    }

    if (!frame) {
        return;
    }

    // DEBUG: Log final frame resolution before submission
    static int submit_counter = 0;
    submit_counter++;
    if (submit_counter % 30 == 1 || submit_counter <= 10) {
        // std::cout << "🖼️  [VIDEO] About to submit clamped=" << clamped
                 // << ", resolution=" << frame->width << "x" << frame->height
                 // << ", format=" << frame->format
                  // << ", frame ptr=" << (void*)frame.get()
                  //<< " (submit #" << submit_counter << ")" << std::endl;
    }

    auto t_before_submit = std::chrono::high_resolution_clock::now();

    // Submit real decoded frame to texture interface
    if (SubmitFrameToTexture(frame, clamped, timestamp)) {
        m_last_good_frame_number = clamped;
        if (debug_call_count <= 3) {
            std::cout << "[VIDEO] Submitted real decoded frame " << clamped
                      << " (time=" << std::fixed << std::setprecision(3) << timestamp << "s)" << std::endl;
        }
    }

    auto t_after_submit = std::chrono::high_resolution_clock::now();
    total_submit_us += std::chrono::duration_cast<std::chrono::microseconds>(t_after_submit - t_before_submit).count();
    df_perf_samples++;
}

void FSTPVideoModuleWrapper::UpdateVideoFrame() {
    FSTP_COUNT_CALL("UpdateVideoFrame");

    // Diagnostics: call frequency with timestamps
    if (ENABLE_VIDEO_DEBUG) {
        static auto last_report_time = std::chrono::steady_clock::now();
        static int actual_calls_since_report = 0;
        actual_calls_since_report++;

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_report_time).count();
        if (elapsed >= 1) {
            std::cout << "⏱️  [REAL CALLS] UpdateVideoFrame actually called "
                      << actual_calls_since_report << " times in last second" << std::endl;
            actual_calls_since_report = 0;
            last_report_time = now;
        }
    }

    if (!m_loaded) {
        return;
    }

    // Profiling inside UpdateVideoFrame
    static uint64_t total_get_audio_frame_us = 0;
    static uint64_t total_display_frame_us = 0;
    static uint64_t total_notify_decoders_us = 0;
    static uint64_t total_visualization_us = 0;
    static int perf_samples = 0;

    // auto t_start = std::chrono::high_resolution_clock::now();  // Unused - performance timing disabled

    // Every 100 calls report hot spots
    if (ENABLE_VIDEO_DEBUG) {
        static int report_counter = 0;
        if (++report_counter >= 100) {
            FSTP_REPORT_HOTSPOTS();

            // Report UpdateVideoFrame profiling
            if (perf_samples > 0) {
                std::cout << "⏱️  [UpdateVideoFrame PERF] GetAudioFrame: " << (total_get_audio_frame_us / perf_samples)
                          << "μs, DisplayFrame: " << (total_display_frame_us / perf_samples)
                          << "μs, NotifyDecoders: " << (total_notify_decoders_us / perf_samples)
                          << "μs, Visualization: " << (total_visualization_us / perf_samples) << "μs" << std::endl;
            }

            report_counter = 0;
            total_get_audio_frame_us = 0;
            total_display_frame_us = 0;
            total_notify_decoders_us = 0;
            total_visualization_us = 0;
            perf_samples = 0;
        }
    }

    // Always update video frame regardless of playing state for now
    // This ensures we see video even when m_playing flag is not synchronized

    auto t_before_audio = std::chrono::high_resolution_clock::now();
    int audioFrame = GetCurrentAudioFrame();
    auto t_after_audio = std::chrono::high_resolution_clock::now();
    total_get_audio_frame_us += std::chrono::duration_cast<std::chrono::microseconds>(t_after_audio - t_before_audio).count();

    if (audioFrame < 0) {
        audioFrame = 0;
    }

    // OPTIMIZATION: SKIP RENDERING IDENTICAL FRAMES
    // If frame hasn't changed since last render - skip
    if (audioFrame == m_last_displayed_frame) {
        // Frame unchanged - skip all rendering and notifications
        return;
    }

    // Frame changed - perform full rendering cycle
    auto t_before_display = std::chrono::high_resolution_clock::now();
    DisplayFrame(audioFrame);
    auto t_after_display = std::chrono::high_resolution_clock::now();
    total_display_frame_us += std::chrono::duration_cast<std::chrono::microseconds>(t_after_display - t_before_display).count();

    auto t_before_notify = std::chrono::high_resolution_clock::now();
    NotifyDecodersOfFrameChange(audioFrame);
    auto t_after_notify = std::chrono::high_resolution_clock::now();
    total_notify_decoders_us += std::chrono::duration_cast<std::chrono::microseconds>(t_after_notify - t_before_notify).count();

    // Save rendered frame number
    m_last_displayed_frame = audioFrame;

    // NOTE: Decoder visualization removed - replaced with SwiftUI Inspector
    // The inspector updates automatically via its own timer (FSTPInspectorView.swift)
    // No need to push updates from here anymore

    perf_samples++;

    // Progressive scan removed
}

void FSTPVideoModuleWrapper::UpdateVideoFrame() const {
    // Don't add FSTP_COUNT_CALL here, as this function just delegates to non-const version
    // which already has the counter. Otherwise we get double counting!
    const_cast<FSTPVideoModuleWrapper*>(this)->UpdateVideoFrame();
}

bool FSTPVideoModuleWrapper::GetCurrentFrame(uint8_t** frame_data, int& width, int& height) const {
    (void)frame_data;
    width = m_video_width;
    height = m_video_height;
    return false;
}

void FSTPVideoModuleWrapper::ReleaseFrame(uint8_t* frame_data) const {
    (void)frame_data;
}

void FSTPVideoModuleWrapper::SetPosition(double position_seconds) {
    if (!m_loaded || !m_frame_index) {
        return;
    }

    int frameNumber = m_frame_index->FindFrameByTime(position_seconds);
    frameNumber = std::max(0, std::min(frameNumber, static_cast<int>(m_frames.size()) - 1));
    NotifyDecodersOfFrameChange(frameNumber);
}

double FSTPVideoModuleWrapper::GetPosition() const {
    if (!m_loaded || !m_frame_index) {
        return 0.0;
    }
    const SimpleFrameInfo* frame = m_frame_index->GetFrameInfo(m_current_index.load());
    return frame ? frame->time_seconds : 0.0;
}

// Progressive scan functions removed

