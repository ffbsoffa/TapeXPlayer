#include "FSTPVideoModule_wrapper.h"

#include "../FSTPMainModule/WSGUI/FSTPOSDSystem.h"
#include "../FSTPMainModule/WSGUI/FSTPWindowManager.h"
#include "../FSTPMainModule/WSGUI/FSTPPixelBufferManager.h"
#include "../FSTPAudioModule/FSTPAudioModule_API.h"
#include "../FSTPAudioModule/FSTPAudioModule_wrapper.h"
#include "../FSTPMainModule/WSGUI/darwin/sdl/FSTPToolsMenu.h"
#include "FSTPPerformanceProfiler.h"
#include "FSTPCallCounter.h"
#include "FSTPHardwareDetection.h"

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
#include <map>
#include <mutex>
#include <thread>

extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

// External hardware detection
extern FSTPHardwareDetection* g_hardware_detection;

// Global registry for video module instances (for frame update notifications)
// Heap-allocated to prevent static destruction order fiasco:
// LowCachedDecoderManager decode threads may still be running when static
// globals are destroyed during program exit, causing "mutex lock failed: EINVAL".
static std::map<int, FSTPVideoModuleWrapper*>& GetVideoInstances() {
    static std::map<int, FSTPVideoModuleWrapper*>* instances =
        new std::map<int, FSTPVideoModuleWrapper*>();
    return *instances;
}
static std::mutex& GetInstancesMutex() {
    static std::mutex* mtx = new std::mutex();
    return *mtx;
}

// Global function to notify video module about frame update
void NotifyVideoFrameUpdate(int instance_id) {
    std::lock_guard<std::mutex> lock(GetInstancesMutex());
    auto it = GetVideoInstances().find(instance_id);
    if (it != GetVideoInstances().end() && it->second) {
        it->second->RequestFrameUpdate();
    }
}

// Platform-specific memory headers
#ifdef __linux__
    #include <malloc.h>
#else
    #include <stdlib.h>  // macOS uses stdlib.h for malloc functions
#endif

namespace fs = std::filesystem;

// Debug control: set to true to enable verbose logging
static constexpr bool ENABLE_VIDEO_DEBUG = true;   // DIAG BUILD ONLY (feat/shuttle-diag) — render/UpdateVideoFrame rate + breakdown

// Export from WindowManager for updating color metadata renderer
extern "C" void FSTP_UpdatePlayerColorMetadata(int player_id, int colorspace, int color_range, int color_primaries, int color_trc);

// Request force render from window system
extern "C" void RequestForceRender();

FSTPVideoModuleWrapper::FSTPVideoModuleWrapper()
    : m_audio_module(nullptr)
    , m_frame_converter(std::make_unique<FSTPFrameConverter>())
    , m_current_frame_number(-1)
    , m_last_good_frame_number(-1) {
}

FSTPVideoModuleWrapper::~FSTPVideoModuleWrapper() {
    // Unregister from global instance map
    if (m_instance_id >= 0) {
        std::lock_guard<std::mutex> lock(GetInstancesMutex());
        GetVideoInstances().erase(m_instance_id);
    }

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

FSTPVideoModuleWrapper::ProxyState FSTPVideoModuleWrapper::ProbeProxy(const std::string& filepath) {
    fs::path sourcePath(filepath);
    if (!fs::exists(sourcePath)) {
        std::cerr << "[VIDEO] Source file does not exist: " << filepath << std::endl;
        return ProxyState::Error;
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

            // NEW STRATEGY: ≤480p AND h264 → ALWAYS use original (any GOP!)
            // Adaptive segment sizing handles any GOP structure (25, 100, 300, etc.)
            if (source_height <= 480 && is_h264 && max_gop_size > 0) {
                use_original_as_proxy = true;
                m_use_original_as_proxy = true;  // Flag to disable Full_Res
                std::cout << "[VIDEO] ✅ Video is " << source_height << "p H.264 with GOP=" << max_gop_size
                          << " - using original as proxy (adaptive segments will handle any GOP)" << std::endl;
            }
        }
        avformat_close_input(&fmt);
    }

    fs::path cacheDir = fs::path(FSTP::LowResDecoder::getCachePath()) / "proxy";
    fs::create_directories(cacheDir);

    m_proxy_path = (cacheDir / (FSTP::LowResDecoder::generateFileId(filepath) + "_lowres.mp4")).string();

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

        return ProxyState::Ready;
    }

    // Normal logic - proxy conversion
    m_use_original_as_proxy = false;

    // Check if proxy already exists — but only trust it if its manifest still matches the
    // source. A bare existence check reused stale/gappy proxies from older builds (the gaps
    // showed as "empty slots" and jumped on playback). Invalid → delete and regenerate.
    if (fs::exists(m_proxy_path)) {
        if (FSTP::LowResDecoder::isCachedProxyValid(m_proxy_path, filepath)) {
            std::cout << "[VIDEO] Found existing proxy: " << m_proxy_path << std::endl;
            return ProxyState::Ready;
        }
        std::cout << "♻️  [VIDEO] Cached proxy failed manifest check — regenerating: "
                  << m_proxy_path << std::endl;
        std::error_code ec;
        fs::remove(m_proxy_path, ec);
        fs::remove(m_proxy_path + ".meta", ec);
    }

    return ProxyState::NeedsConversion;
}

// Blocking probe + convert. Only used when instant start is impossible (no V2 decoder on this
// hardware profile) — the normal path defers the conversion to BackgroundProxyConversion.
bool FSTPVideoModuleWrapper::EnsureProxy(const std::string& filepath, const std::function<void(int)>& progressCallback) {
    switch (ProbeProxy(filepath)) {
        case ProxyState::Ready:
            if (progressCallback) progressCallback(100);
            return true;
        case ProxyState::Error:
            return false;
        case ProxyState::NeedsConversion:
            break;
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

// TAPE THREADING: runs on a detached thread. Converts the proxy, then — if the instance
// still exists and hasn't loaded a different file since (generation check) — publishes the
// proxy decoder. Deliberately static and without `this`: the wrapper may be unloaded,
// reloaded or destroyed while ffmpeg runs; the instance registry + load generation decide
// whether the result is still wanted. A dropped result is not wasted — the converted proxy
// stays in the cache and the next open of this file picks it up instantly.
void FSTPVideoModuleWrapper::BackgroundProxyConversion(int instance_id, uint32_t generation,
                                                       std::string filepath) {
    std::cout << "[VIDEO] 🧵 Background proxy conversion started (instance " << instance_id
              << "): " << filepath << std::endl;

    auto progress = [instance_id](int percent) {
        UpdateOSDProxyThreading(instance_id, percent);
    };

    std::string generatedProxy;
    bool converted = FSTP::LowResDecoder::convertToLowRes(filepath, generatedProxy, progress);

    // Publication (or disposal) happens under the registry mutex: UnloadFile bumps the
    // generation under this same mutex, so a stale conversion can never install itself
    // into a wrapper that has moved on to another file.
    std::lock_guard<std::mutex> lock(GetInstancesMutex());
    UpdateOSDProxyThreading(instance_id, -1);

    auto it = GetVideoInstances().find(instance_id);
    if (it == GetVideoInstances().end() || !it->second) {
        std::cout << "[VIDEO] 🧵 Instance " << instance_id << " gone — dropping proxy result" << std::endl;
        return;
    }
    FSTPVideoModuleWrapper* self = it->second;
    if (self->m_load_generation.load() != generation) {
        std::cout << "[VIDEO] 🧵 Load generation changed — dropping proxy result" << std::endl;
        return;
    }
    if (!converted) {
        std::cerr << "[VIDEO] 🧵 Background proxy conversion FAILED — staying on full-res decoder "
                     "(shuttle remains limited)" << std::endl;
        return;
    }

    self->m_proxy_path = generatedProxy;
    if (!self->SetupProxyDecoder()) {
        std::cerr << "[VIDEO] 🧵 Proxy decoder setup failed after conversion — staying on full-res" << std::endl;
        return;
    }
    self->m_proxy_ready.store(true, std::memory_order_release);
    self->RequestFrameUpdate();
    std::cout << "[VIDEO] 🧵 ✅ Tape threaded — proxy decoder online, shuttle unlocked (instance "
              << instance_id << ")" << std::endl;
}

bool FSTPVideoModuleWrapper::LoadFile(const std::string& filepath, double initial_time) {
    if (!m_initialized) {
        std::cerr << "[VIDEO] Module not initialized." << std::endl;
        return false;
    }

    const int osdPlayerId = (m_instance_id >= 0 ? m_instance_id : 0);

    // Proxy↔original frame offset is now structurally 0: the proxy is frame-exact and Stage-2
    // on-demand decode shows the EXACT current frame. Force offset 0 and mark "calibrated" so the
    // old luma-signature auto-calibration never runs — it could mis-lock ±1 (especially when V2 was
    // shown a frame off via the loose sync tolerance) and that lock was the proxy→full-res shift.
    m_proxy_frame_offset.store(0);
    m_proxy_offset_calibrated = true;   // disabled: offset is exact by construction
    m_proxy_calib_candidate = 99;
    m_proxy_calib_count = 0;

    // If file already loaded - set reload flag
    if (m_loaded) {
        m_file_reloading.store(true);
    }

    // Set loading mode before unloading old file
    SetPlayerLoadingState(osdPlayerId, true);
    SetPlayerLoadingProgress(osdPlayerId, 0);
    UpdateOSDDisplayMode(osdPlayerId, OSD_MODE_LOADING);
    // REMOVED: SetPlayerLoadingStatus("threading") - moved to Phase 3 where it actually happens

    if (m_loaded) {
        UnloadFile();
    }

    m_current_file = filepath;
    // Fresh load: no proxy decoder published yet (UnloadFile also clears this; the first
    // load and failed-load retries land here without an unload).
    m_proxy_ready.store(false, std::memory_order_release);

    // Phase 1: Indexing (0-25%) — built from the ORIGINAL file, no proxy required.
    // (Used to run AFTER the proxy conversion; moved first so playback can start
    // before the proxy exists — see TAPE THREADING below.)
    SetPlayerLoadingStatus(osdPlayerId, "indexing");
    SetPlayerLoadingProgress(osdPlayerId, 0);
    UpdateOSDLoadingProgress(osdPlayerId, 0);

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

    SetPlayerLoadingProgress(osdPlayerId, 25);
    UpdateOSDLoadingProgress(osdPlayerId, 25);

    // CRITICAL: Initialize m_frames pointer if needed (leak on exit acceptable)
    if (!m_frames) {
        m_frames = new std::vector<FSTP::FrameInfo>();
    }

    m_frames->clear();
    m_frames->resize(m_frame_index->GetTotalFrames());


    // Initialize time_ms and keyframe info from original video index for seeking reference
    // LowResDecoder will update time_ms with actual proxy file timing during decode
    for (size_t i = 0; i < m_frames->size(); ++i) {
        const SimpleFrameInfo* frame_info = m_frame_index->GetFrameInfo(static_cast<int>(i));
        if (frame_info) {
                // CRITICAL: Use ROUNDING instead of truncation (like in old decode.cpp)
                // At 29.97fps (33.367ms per frame) truncation creates jitter, rounding fixes it!
                // Old solution: (relative_us + 500) / 1000 = rounding to nearest
            (*m_frames)[i].time_ms = static_cast<int64_t>(std::round(frame_info->time_seconds * 1000.0));

            // CRITICAL: Copy is_keyframe for GOP-aware decoding
            (*m_frames)[i].is_keyframe = frame_info->is_keyframe;

            static int debug_count = 0;
            if (debug_count++ < 5) {
                std::cout << "[VIDEO] Frame " << i << ": initial time_ms=" << (*m_frames)[i].time_ms
                          << " (from original " << std::fixed << std::setprecision(3) << frame_info->time_seconds << "s)"
                          << (frame_info->is_keyframe ? " [KEYFRAME]" : "") << std::endl;
            }
        } else {
            (*m_frames)[i].time_ms = -1; // Invalid time
            (*m_frames)[i].is_keyframe = false;
        }
    }

    m_current_index.store(0);

    m_duration.store(m_frame_index->GetDuration());
    m_frame_rate = m_frame_index->GetFrameRate();
    m_total_frames = static_cast<int>(m_frame_index->GetTotalFrames());
    m_video_width = m_frame_index->GetWidth();
    m_video_height = m_frame_index->GetHeight();

    // Phase 2: Proxy probe (cheap — GOP scan / cache check, never a transcode).
    // TAPE THREADING: when a full conversion is needed and the full-res V2 decoder is
    // available, don't block on it — playback starts from the ORIGINAL right away and
    // BackgroundProxyConversion publishes the proxy decoder when the tape is "threaded".
    SetPlayerLoadingStatus(osdPlayerId, "proxy");
    ProxyState proxy_state = ProbeProxy(filepath);
    if (proxy_state == ProxyState::Error) {
        SetPlayerLoadingProgress(osdPlayerId, 0);
        SetPlayerLoadingState(osdPlayerId, false);
        m_file_reloading.store(false);
        return false;
    }

    bool defer_proxy = false;
    if (proxy_state == ProxyState::NeedsConversion) {
        // On this CPU profile the V2 decoder is skipped entirely — without a proxy there
        // would be no picture at all, so keep the old blocking conversion (25-60%).
        bool v2_unavailable = (g_hardware_detection &&
                               g_hardware_detection->GetCPUInfo().is_pentium_gold_7505);
        if (v2_unavailable) {
            auto proxyProgress = [playerId = osdPlayerId](int percent) {
                int scaled = 25 + percent * 35 / 100;
                SetPlayerLoadingProgress(playerId, scaled);
                UpdateOSDLoadingProgress(playerId, scaled);
            };
            if (!EnsureProxy(filepath, proxyProgress)) {
                SetPlayerLoadingProgress(osdPlayerId, 0);
                SetPlayerLoadingState(osdPlayerId, false);
                m_file_reloading.store(false);
                return false;
            }
        } else {
            defer_proxy = true;
            std::cout << "[VIDEO] 🧵 TAPE THREADING: instant start from original, "
                         "proxy conversion deferred to background" << std::endl;
        }
    }

    SetPlayerLoadingProgress(osdPlayerId, 60);
    UpdateOSDLoadingProgress(osdPlayerId, 60);

    // Phase 3: Initialize decoders (60-100%) - THREADING
    SetPlayerLoadingStatus(osdPlayerId, "threading");

    if (!InitializeDecoders(initial_time, !defer_proxy)) {
        SetPlayerLoadingState(osdPlayerId, false);
        SetPlayerLoadingProgress(osdPlayerId, 0);
        m_file_reloading.store(false);
        std::cerr << "[VIDEO] Failed to initialize decoders" << std::endl;
        return false;
    }

    m_loaded = true;

    // Notify audio module of video frame rate for frame alignment feature
    if (m_audio_module && m_frame_rate > 0.0) {
        m_audio_module->SetVideoFrameRate(m_frame_rate);
    }

    SetPlayerLoadingProgress(osdPlayerId, 100);
    UpdateOSDLoadingProgress(osdPlayerId, 100);

    // Reset reload flag after successful loading
    m_file_reloading.store(false);

    // Submit the new clip's first frame to the pixel buffer BEFORE clearing the loading state.
    // While loading, the window render blanks the video (clears to black, skips the texture), so
    // doing this first closes the gap where the render would otherwise briefly reveal the PREVIOUS
    // clip's last frame — still sitting in the pixel buffer / window texture — before frame 0 of the
    // new clip arrives. (Previously the loading state was cleared first, exposing that stale frame.)
    if (initial_time > 0.0 && m_frame_index) {
        int initial_frame = m_frame_index->FindFrameByTime(initial_time);
        initial_frame = std::max(0, std::min(initial_frame, static_cast<int>(m_frames->size()) - 1));
        std::cout << "[VIDEO] Initial display at " << initial_time << "s (frame " << initial_frame << ")" << std::endl;
        DisplayFrame(initial_frame);
    } else {
        std::cout << "[VIDEO] Testing immediate display of frame 0..." << std::endl;
        DisplayFrame(0);
    }

    // First frame of the new clip is now in the buffer — reveal the video.
    SetPlayerLoadingState(osdPlayerId, false);
    UpdateOSDDisplayMode(osdPlayerId, OSD_MODE_NORMAL);

    // TAPE THREADING: kick off the background conversion AFTER the video is revealed.
    // The thread is detached — it must not reference `this` directly; it re-finds the
    // instance through the registry (and the load generation) when it finishes.
    if (defer_proxy) {
        UpdateOSDProxyThreading(osdPlayerId, 0);
        std::thread(&FSTPVideoModuleWrapper::BackgroundProxyConversion,
                    m_instance_id, m_load_generation.load(), filepath).detach();
    }

    return true;
}

void FSTPVideoModuleWrapper::UnloadFile() {
    if (!m_loaded) {
        return;
    }
    // NOTE: V2 caches (m_v2_cached_frame, m_cached_next_adjacent, m_cached_prev_adjacent)
    // are cleared in DisplayFrame when m_file_reloading=true, from the render thread.
    // Clearing them here (load thread) while render thread might be reading = data race.

    const int osdPlayerId = (m_instance_id >= 0 ? m_instance_id : 0);

    // Show "unthreading" progress during unload
    // If reloading, LOADING mode is already set by LoadFile
    if (!m_file_reloading.load()) {
        SetPlayerLoadingState(osdPlayerId, true);
        UpdateOSDDisplayMode(osdPlayerId, OSD_MODE_LOADING);
    }
    SetPlayerLoadingStatus(osdPlayerId, "unthreading");
    SetPlayerLoadingProgress(osdPlayerId, 0);
    UpdateOSDLoadingProgress(osdPlayerId, 0);

    // CRITICAL FIX: CORRECT cleanup order to prevent heap corruption!
    // AVFrames have internal references to decoder contexts.
    // MUST free frames BEFORE destroying contexts!
    //
    // WRONG ORDER (causes heap corruption):
    //   1. ShutdownDecoders() - destroys contexts
    //   2. Free AVFrames - tries to access destroyed contexts → CRASH
    //
    // CORRECT ORDER:
    //   1. Free AVFrames first
    //   2. Then ShutdownDecoders() - safe to destroy contexts

    // STEP -1 (TAPE THREADING): retire any in-flight background proxy conversion FIRST.
    // The generation bump under the registry mutex guarantees that a conversion finishing
    // right now either publishes before we proceed (and is torn down normally below) or
    // sees the stale generation and drops its result. After this block no new proxy
    // decoder can appear during the teardown.
    {
        std::lock_guard<std::mutex> reg_lock(GetInstancesMutex());
        m_load_generation.fetch_add(1);
        m_proxy_ready.store(false, std::memory_order_release);
    }
    UpdateOSDProxyThreading(osdPlayerId, -1);

    // STEP 0: Stop the decoder THREADS before touching m_frames. The proxy manager thread and the
    // display's on-demand decode (decodeFrameNow) both write into m_frames; if they run while the
    // cleanup below resets/clears it, that's a use-after-free → segfault (seen when switching to a
    // different clip). stop() joins the manager thread (and aborts a mid-decode via requestStop), so
    // after this no one else touches m_frames. Contexts stay ALIVE here — the AVFrames still
    // reference them; they're destroyed later in ShutdownDecoders, AFTER the frames are freed.
    m_decoders_active.store(false);
    if (m_low_cached_manager) m_low_cached_manager->stop();
    if (m_full_res_decoder)   m_full_res_decoder->RequestStop();

    // STEP 1: Free AVFrames first (while decoder contexts are still valid)
    // CAREFUL GRADUAL CLEANUP: Clean frames in small batches with pauses
    // Now that Audio uses direct ALSA (no PipeWire crash), we can try careful cleanup
    // Strategy: Clear 100 frames at a time with 2ms pause between batches
    // This prevents heap allocator overload while avoiding 200MB leak
    size_t frame_count = m_frames->size();
    if (frame_count > 0) {
        std::cout << "[VIDEO] Starting careful gradual frame cleanup ("
                  << frame_count << " frames in batches of 100)" << std::endl;

        const size_t BATCH_SIZE = 100;  // Clean 100 frames at a time
        const int PAUSE_MS = 2;         // 2ms pause between batches

        size_t cleaned = 0;
        auto cleanup_start = std::chrono::steady_clock::now();

        // Clear frames in small batches
        for (size_t i = 0; i < frame_count; i += BATCH_SIZE) {
            size_t batch_end = std::min(i + BATCH_SIZE, frame_count);

            // Reset shared_ptrs in this batch
            for (size_t j = i; j < batch_end; j++) {
                // Lock mutex to safely reset frame pointers
                try {
                    std::lock_guard<std::mutex> lock((*m_frames)[j].mutex);
                    (*m_frames)[j].low_res_frame.reset();
                    (*m_frames)[j].frame.reset();
                    (*m_frames)[j].cached_frame.reset();
                } catch (...) {
                    // Mutex might be in bad state, skip this frame
                }
            }

            cleaned += (batch_end - i);

            // Update OSD progress (0-80% for frame cleanup)
            int progress = static_cast<int>((cleaned * 80) / frame_count);
            SetPlayerLoadingProgress(osdPlayerId, progress);
            UpdateOSDLoadingProgress(osdPlayerId, progress);

            // NOTE: Don't call RenderAllWindows() here - causes race condition!
            // Render thread will automatically pick up OSD updates on next VSync

            // Progress report every 1000 frames
            if (cleaned % 1000 == 0 || cleaned == frame_count) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - cleanup_start).count();
                std::cout << "[VIDEO] Cleanup progress: " << cleaned << "/" << frame_count
                          << " frames (" << (cleaned * 100 / frame_count) << "%, "
                          << elapsed_ms << "ms elapsed)" << std::endl;
            }

            // Pause between batches to let heap allocator breathe
            if (batch_end < frame_count) {
                std::this_thread::sleep_for(std::chrono::milliseconds(PAUSE_MS));
            }
        }

        // Finally clear the vector itself
        m_frames->clear();

        auto cleanup_end = std::chrono::steady_clock::now();
        auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(cleanup_end - cleanup_start).count();
        std::cout << "[VIDEO] Frame cleanup completed: " << frame_count
                  << " frames in " << total_ms << "ms" << std::endl;
    }

    // HALF-FPS PROXY: Clear half-fps index if it exists
    if (m_half_fps_frames && !m_half_fps_frames->empty()) {
        std::cout << "🎯 [HALF-FPS] Clearing half-fps index (" << m_half_fps_frames->size() << " slots)" << std::endl;
        m_half_fps_frames->clear();
    }

    // STEP 2: Now safe to shutdown decoders (all AVFrames freed) - 80-100%
    SetPlayerLoadingProgress(osdPlayerId, 85);
    UpdateOSDLoadingProgress(osdPlayerId, 85);

    std::cout << "[VIDEO] All frames freed, now shutting down decoder contexts..." << std::endl;
    ShutdownDecoders();
    std::cout << "[VIDEO] Decoder contexts shutdown complete" << std::endl;

    SetPlayerLoadingProgress(osdPlayerId, 95);
    UpdateOSDLoadingProgress(osdPlayerId, 95);

    if (m_frame_index) {
        m_frame_index->Clear();
    }

    SetPlayerLoadingProgress(osdPlayerId, 98);
    UpdateOSDLoadingProgress(osdPlayerId, 98);

    m_current_file.clear();
    m_proxy_path.clear();
    m_use_original_as_proxy = false; // Reset flag
    m_is_half_fps_proxy = false; // Reset half-fps flag
    m_duration.store(0.0);
    m_frame_rate = 0.0;
    m_total_frames = 0;
    m_last_displayed_frame = -1; // Reset for forced rendering of first frame
    m_last_good_frame_number = -1; // Reset to prevent stale frame reference on reload
    m_loaded = false;

    // Complete unthreading process
    SetPlayerLoadingProgress(osdPlayerId, 100);
    UpdateOSDLoadingProgress(osdPlayerId, 100);

    // NOTE: Don't call RenderAllWindows() - render thread handles it
    // Small delay to allow render thread to show 100% completion
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // If reloading, keep LOADING mode (LoadFile will continue with "proxy")
    // If just unloading, keep LOADING state with "unthreading 100%" until window closes
    if (m_file_reloading.load()) {
        std::cout << "[VIDEO] File unloaded, continuing with reload..." << std::endl;
    } else {
        // DON'T call SetPlayerLoadingState(false) - keep "unthreading 100%" visible
        // until window actually closes (prevents NO_FILE flash)
        std::cout << "[VIDEO] File unloaded (unthreading complete, keeping LOADING state)" << std::endl;
    }
}

// Proxy-side decoder setup shared by the synchronous load path and the background
// conversion completion. Precondition: m_frame_index built, m_frames initialized from it,
// m_proxy_path points at a usable proxy. Does NOT publish m_proxy_ready — the caller does.
bool FSTPVideoModuleWrapper::SetupProxyDecoder() {
    // HALF-FPS PROXY DETECTION: Check if proxy is 30fps from 60fps original.
    // (Moved out of LoadFile: on the instant-start path the proxy file only exists
    // once the background conversion finishes.)
    double original_fps = m_frame_index ? m_frame_index->GetFrameRate() : 0.0;
    m_is_half_fps_proxy = false;

    if (!m_use_original_as_proxy && !m_proxy_path.empty()) {
        AVFormatContext* proxy_fmt = nullptr;
        if (avformat_open_input(&proxy_fmt, m_proxy_path.c_str(), nullptr, nullptr) == 0) {
            if (avformat_find_stream_info(proxy_fmt, nullptr) >= 0) {
                int video_stream_idx = av_find_best_stream(proxy_fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
                if (video_stream_idx >= 0) {
                    AVStream* stream = proxy_fmt->streams[video_stream_idx];
                    if (stream->r_frame_rate.den > 0) {
                        double proxy_fps = static_cast<double>(stream->r_frame_rate.num) / stream->r_frame_rate.den;

                        // Detect half-FPS by the ~2:1 ratio (covers 50→25 AND 60→30, 59.94→29.97).
                        double fps_ratio = (proxy_fps > 0.1) ? (original_fps / proxy_fps) : 0.0;
                        if (original_fps > 45.0 && fps_ratio > 1.7 && fps_ratio < 2.3) {
                            m_is_half_fps_proxy = true;
                            std::cout << "🎯 [HALF-FPS PROXY] Detected: original " << std::fixed << std::setprecision(2)
                                      << original_fps << " fps, proxy " << proxy_fps << " fps" << std::endl;
                        }
                    }
                }
            }
            avformat_close_input(&proxy_fmt);
        }
    }

    // HALF-FPS PROXY: Create separate half-size index and fill it with every second
    // frame's metadata (mapping: half-fps slot i ← original frame 2*i).
    if (m_is_half_fps_proxy) {
        if (!m_half_fps_frames) {
            m_half_fps_frames = new std::vector<FSTP::FrameInfo>();
        }
        size_t half_size = (m_frames->size() + 1) / 2; // Round up
        m_half_fps_frames->clear();
        m_half_fps_frames->resize(half_size);
        std::cout << "🎯 [HALF-FPS INDEX] Created " << half_size << " slots (main index: "
                  << m_frames->size() << " slots)" << std::endl;

        for (size_t i = 0; i < m_half_fps_frames->size(); ++i) {
            size_t orig_idx = i * 2;
            if (orig_idx < m_frames->size()) {
                const SimpleFrameInfo* frame_info = m_frame_index->GetFrameInfo(static_cast<int>(orig_idx));
                if (frame_info) {
                    (*m_half_fps_frames)[i].time_ms = static_cast<int64_t>(std::round(frame_info->time_seconds * 1000.0));
                    (*m_half_fps_frames)[i].is_keyframe = frame_info->is_keyframe;
                } else {
                    (*m_half_fps_frames)[i].time_ms = -1;
                    (*m_half_fps_frames)[i].is_keyframe = false;
                }
            }
        }
    }

    // Use the working LowCachedDecoderManager from old code
    try {
        std::vector<FSTP::FrameInfo>& frame_index_ref = m_is_half_fps_proxy ? *m_half_fps_frames : *m_frames;

        if (m_is_half_fps_proxy) {
            std::cout << "🎯 [HALF-FPS] Using half-fps index (" << m_half_fps_frames->size()
                      << " slots) for LowCachedDecoderManager" << std::endl;
        }

        m_low_cached_manager = std::make_unique<FSTP::LowCachedDecoderManager>(
            m_proxy_path,
            frame_index_ref,  // Use half-fps index if available, otherwise main index
            m_current_index,
            m_low_res_range,
            m_high_res_window,
            m_playing,
            m_speed,
            m_reverse,
            m_instance_id,  // Pass instance ID for frame update notifications
            m_is_half_fps_proxy  // HALF-FPS: Tell manager to map frame indices /2
        );
    } catch (const std::exception& ex) {
        std::cerr << "[VIDEO] Failed to create low cached decoder manager: " << ex.what() << std::endl;
        return false;
    }

    // ADAPTIVE SEGMENT SIZING: Use GOP information from index
    // This optimizes performance for videos with different GOP structures (GOP 25, GOP 300, etc.)
    // Strategy: Use 2× GOP size for optimal balance between CPU and responsiveness
    if (m_low_cached_manager && m_frame_index) {
        int gopSize = m_frame_index->GetMaxGopSize();
        if (gopSize > 0) {
            std::cout << "[VIDEO] 🎯 Detected GOP size: " << gopSize << " frames" << std::endl;
            m_low_cached_manager->setSegmentSizeFromGOP(gopSize);
        } else {
            std::cout << "[VIDEO] ⚠️  GOP size unknown, using default segment size" << std::endl;
        }
    }

    if (m_low_cached_manager) {
        m_low_cached_manager->run();
    }

    return true;
}

bool FSTPVideoModuleWrapper::InitializeDecoders(double initial_time, bool with_proxy) {
    std::cout << "[VIDEO] InitializeDecoders: Using LowCachedDecoderManager (the proven solution)" << std::endl;

    const int osdPlayerId = (m_instance_id >= 0 ? m_instance_id : 0);

    // Progress: 60% → 85% (Low-res decoder setup)
    SetPlayerLoadingProgress(osdPlayerId, 78);
    UpdateOSDLoadingProgress(osdPlayerId, 78);

    if (with_proxy) {
        if (!SetupProxyDecoder()) {
            return false;
        }
        // Same-thread publication; the release store pairs with ProxyManager()'s acquire.
        m_proxy_ready.store(true, std::memory_order_release);
    } else {
        std::cout << "[VIDEO] 🧵 Proxy decoder deferred — transport limited to pause/forward ≤1× "
                     "until the background conversion completes" << std::endl;
    }

    SetPlayerLoadingProgress(osdPlayerId, 88);
    UpdateOSDLoadingProgress(osdPlayerId, 88);

    // V2: Streaming full-res decoder (only if video > 480p or not h264)
    // For ≤480p h264 video we use only proxy - save resources!
    // PENTIUM GOLD 7505 OPTIMIZATION: Always disable Full-Res decoder on this CPU
    bool skip_fullres = m_use_original_as_proxy;
    if (g_hardware_detection && g_hardware_detection->GetCPUInfo().is_pentium_gold_7505) {
        skip_fullres = true;
        std::cout << "[VIDEO] 🎯 Pentium Gold 7505: Full-Res decoder DISABLED (always use optimized proxy)" << std::endl;
    }

    if (!skip_fullres) {
        std::cout << "[VIDEO] InitializeDecoders: Creating FSTPFullResDecoderV2 (streaming mode)" << std::endl;

        SetPlayerLoadingProgress(osdPlayerId, 92);
        UpdateOSDLoadingProgress(osdPlayerId, 92);

        try {
            m_full_res_decoder = std::make_unique<FSTPFullResDecoderV2>(m_current_file, initial_time);

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
        std::cout << "[VIDEO] ⚡ Skipping Full-Res decoder (resource optimization)" << std::endl;
        // Still update progress for consistency
        SetPlayerLoadingProgress(osdPlayerId, 92);
        UpdateOSDLoadingProgress(osdPlayerId, 92);
    }

    // Progressive scan functionality removed

    // Final progress before completion
    SetPlayerLoadingProgress(osdPlayerId, 98);
    UpdateOSDLoadingProgress(osdPlayerId, 98);

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
    // ProxyManager() (not the raw pointer): during tape threading the manager is published
    // from the background thread; the acquire-gated accessor makes that publication safe.
    if (auto* proxy_mgr = ProxyManager()) {
        proxy_mgr->notifyFrameChange();
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

    // PENTIUM 7505 OPTIMIZATION: Disable Full-Res decoder at speeds >= 2x
    // At any speed > 1x, only low-res preview is shown, so full-res wastes CPU/memory
    // Freeing these resources allows low-res decoder to keep up at 24x
    if (m_full_res_decoder) {
        // Per-instance state (was a function-level static shared across ALL
        // players — a latent multi-window bug). Hysteresis avoids thrashing the
        // full-res decoder near the threshold: stop at >=2.0x, resume only once
        // speed drops back to <=1.5x. Without the dead-band, tiny speed jitter
        // around 2x toggled the decoder every event, which forced the frame
        // source to flip proxy<->full-res and recreate the YUV texture each time
        // — cheap on Metal, very expensive on Windows/D3D11 (the scrub lag).
        double abs_speed = std::abs(speed);
        constexpr double STOP_THRESHOLD   = 2.0;
        constexpr double RESUME_THRESHOLD = 1.5;

        if (!m_full_res_stopped && abs_speed >= STOP_THRESHOLD) {
            std::cout << "⚡ [SPEED OPT] Speed >= 2x (" << abs_speed << "x), stopping Full-Res decoder to free resources" << std::endl;
            m_full_res_decoder->RequestStop();
            m_full_res_stopped = true;
        } else if (m_full_res_stopped && abs_speed <= RESUME_THRESHOLD) {
            std::cout << "✅ [SPEED OPT] Speed <= 1.5x (" << abs_speed << "x), resuming Full-Res decoder" << std::endl;
            m_full_res_decoder->ClearStopRequest();
            m_full_res_stopped = false;
        }
    }

    NotifyDecodersOfFrameChange(m_current_index.load());
}

void FSTPVideoModuleWrapper::SetSpeedInstant(double speed) {
    SetSpeed(speed);
}

void FSTPVideoModuleWrapper::SetBackgrounded(bool backgrounded) {
    if (backgrounded == m_backgrounded.load()) return;
    m_backgrounded.store(backgrounded);

    if (!m_full_res_decoder) return;
    if (backgrounded) {
        // Idle the full-res V2 thread and DROP its 1080p frame buffer to free RAM — the window keeps
        // showing the cached still texture (the instance is paused/settled), so V2 isn't needed.
        std::cout << "💤 [BACKGROUND] Player " << m_instance_id
                  << ": freeing full-res V2 buffer (backgrounded)" << std::endl;
        m_full_res_decoder->RequestStop();
        m_full_res_decoder->ClearBuffer();
    } else {
        // Refocused — resume V2; it rebuilds its buffer (proxy bridges the brief catch-up).
        std::cout << "☀️ [BACKGROUND] Player " << m_instance_id
                  << ": resuming full-res V2 (foregrounded)" << std::endl;
        m_full_res_decoder->ClearStopRequest();
    }
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
    static std::vector<FSTP::FrameInfo> empty;
    return m_frames ? *m_frames : empty;
}

int FSTPVideoModuleWrapper::GetCurrentFrameIndex() const {
    return m_current_index.load();
}

void FSTPVideoModuleWrapper::GetBufferRanges(int& bufferStart, int& bufferEnd, int& highResStart, int& highResEnd) const {
    int current_frame = m_current_index.load();
    int total_frames = static_cast<int>(m_frames->size());

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

    // Notify audio module of video frame rate for frame alignment feature
    if (m_audio_module && m_frame_rate > 0.0) {
        m_audio_module->SetVideoFrameRate(m_frame_rate);
    }
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

    // Register in global instance map for frame update notifications
    if (instance_id >= 0) {
        std::lock_guard<std::mutex> lock(GetInstancesMutex());
        GetVideoInstances()[instance_id] = this;
    }
}

int FSTPVideoModuleWrapper::GetInstanceID() const {
    return m_instance_id;
}

void FSTPVideoModuleWrapper::RequestFrameUpdate() {
    m_force_frame_update.store(true);
    m_last_displayed_frame = -1; // Invalidate cache to force re-render
    // Also trigger render update
    RequestForceRender();
}

// Convert an AVFrame to YUV420P if it's not already in a directly supported format.
// Used to normalize adjacent (prev/next) frames before passing to the Betacam compositor,
// which expects 8-bit planar YUV matching the current frame's format.
static std::shared_ptr<AVFrame> ConvertFrameToYUV420P(const std::shared_ptr<AVFrame>& src) {
    if (!src) return nullptr;
    AVPixelFormat fmt = static_cast<AVPixelFormat>(src->format);
    if (fmt == AV_PIX_FMT_YUV420P || fmt == AV_PIX_FMT_YUVJ420P || fmt == AV_PIX_FMT_NV12) {
        return src; // already in a supported 8-bit format
    }
    SwsContext* ctx = sws_getContext(src->width, src->height, fmt,
                                     src->width, src->height, AV_PIX_FMT_YUV420P,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!ctx) return nullptr;
    AVFrame* dst = av_frame_alloc();
    if (!dst) { sws_freeContext(ctx); return nullptr; }
    dst->format = AV_PIX_FMT_YUV420P;
    dst->width  = src->width;
    dst->height = src->height;
    if (av_frame_get_buffer(dst, 32) < 0) {
        av_frame_free(&dst);
        sws_freeContext(ctx);
        return nullptr;
    }
    sws_scale(ctx, src->data, src->linesize, 0, src->height, dst->data, dst->linesize);
    av_frame_copy_props(dst, src.get());
    dst->pts = src->pts;
    sws_freeContext(ctx);
    return std::shared_ptr<AVFrame>(dst, [](AVFrame* f){ av_frame_free(&f); });
}

bool FSTPVideoModuleWrapper::SubmitFrameToTexture(const std::shared_ptr<AVFrame>& frame,
                                                   int frame_number,
                                                   double timestamp,
                                                   const std::shared_ptr<AVFrame>& prev_frame,
                                                   const std::shared_ptr<AVFrame>& next_frame) {
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

    std::shared_ptr<AVFrame> frame_to_submit = frame;
    AVPixelFormat src_format = static_cast<AVPixelFormat>(frame->format);
    bool supported_format = (src_format == AV_PIX_FMT_YUV420P ||
                             src_format == AV_PIX_FMT_YUVJ420P ||
                             src_format == AV_PIX_FMT_NV12);
    bool format_was_native = supported_format; // track before potential conversion

    if (!supported_format) {
        SwsContext* convert_ctx = sws_getContext(
            frame->width,
            frame->height,
            src_format,
            frame->width,
            frame->height,
            AV_PIX_FMT_YUV420P,
            SWS_BILINEAR,
            nullptr,
            nullptr,
            nullptr);

        if (convert_ctx) {
            AVFrame* converted = av_frame_alloc();
            if (converted) {
                converted->format = AV_PIX_FMT_YUV420P;
                converted->width = frame->width;
                converted->height = frame->height;

                if (av_frame_get_buffer(converted, 32) >= 0) {
                    int conv = sws_scale(
                        convert_ctx,
                        frame->data,
                        frame->linesize,
                        0,
                        frame->height,
                        converted->data,
                        converted->linesize);

                    if (conv > 0) {
                        av_frame_copy_props(converted, frame.get());
                        converted->pts = frame->pts;
                        converted->colorspace = frame->colorspace;
                        converted->color_primaries = frame->color_primaries;
                        converted->color_trc = frame->color_trc;
                        converted->color_range = frame->color_range;
                        frame_to_submit.reset(converted, [](AVFrame* f){ av_frame_free(&f); });
                        src_format = AV_PIX_FMT_YUV420P;
                        supported_format = true;
                    } else {
                        av_frame_free(&converted);
                    }
                } else {
                    av_frame_free(&converted);
                }
            }
            sws_freeContext(convert_ctx);
        }

        if (!supported_format) {
            std::cerr << "❌ [VIDEO] Unsupported frame format: " << frame->format << std::endl;
            return false;
        }
    }

    // Support YUV420P and NV12 formats (zero-copy!)
    if (src_format == AV_PIX_FMT_YUV420P || src_format == AV_PIX_FMT_YUVJ420P ||
        src_format == AV_PIX_FMT_NV12) {
        // Pass color metadata of current frame (1:1)
        auto norm = [](int v, int unspecified_code) { return (v == unspecified_code) ? -1 : v; };
        int cs = norm(frame_to_submit->colorspace, 2);        // AVCOL_SPC_UNSPECIFIED == 2
        int pr = norm(frame_to_submit->color_primaries, 2);   // AVCOL_PRI_UNSPECIFIED == 2
        int tr = norm(frame_to_submit->color_trc, 2);         // AVCOL_TRC_UNSPECIFIED == 2
        int rg = (frame_to_submit->color_range == 0 /* AVCOL_RANGE_UNSPECIFIED */) ? -1 : frame_to_submit->color_range;

        FSTP_UpdatePlayerColorMetadata(m_instance_id, cs, rg, pr, tr);

        // Adjacent frames must be in the same 8-bit YUV format as frame_to_submit.
        // If the original frame needed format conversion (e.g. ProRes 10-bit yuv422p10le),
        // adjacent frames from the same decoder have the same raw format and must also
        // be converted — otherwise the Betacam compositor reads 10-bit data as 8-bit.
        std::shared_ptr<AVFrame> adj_prev = format_was_native ? prev_frame : ConvertFrameToYUV420P(prev_frame);
        std::shared_ptr<AVFrame> adj_next = format_was_native ? next_frame : ConvertFrameToYUV420P(next_frame);

        // ZERO-COPY: Send shared_ptr directly to pixel buffer manager
        // No memcpy! Only increment refcount on AVFrame
        // Also pass adjacent frames for Betacam slow-motion compositing
        SubmitAVFrame(m_instance_id, frame_to_submit, timestamp, frame_number, adj_prev, adj_next);

        if (ENABLE_VIDEO_DEBUG) {
            static int zero_copy_log = 0;
            if (++zero_copy_log % 100 == 1) {
                const char* format_name = av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame_to_submit->format));
                std::cout << "🚀 [ZERO-COPY] Submitted AVFrame " << frame_number
                          << " format=" << (format_name ? format_name : "UNKNOWN")
                          << " (" << frame_to_submit->width << "x" << frame_to_submit->height << ")"
                          << ", Y_linesize=" << frame_to_submit->linesize[0]
                          << ", UV_linesize=" << frame_to_submit->linesize[1]
                          << ", refcount=" << frame_to_submit.use_count() << std::endl;
            }
        }

        return true;
    }

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
        // Drop all cached frame references from THIS thread (render thread).
        // UnloadFile/ShutdownDecoders runs on the load thread — clearing shared_ptrs here
        // avoids a data race that would occur if UnloadFile reset them directly while
        // this thread was reading them. After this point, ShutdownDecoders can safely
        // destroy V2 decoder buffers with no outstanding references.
        m_v2_cached_frame = nullptr;
        m_v2_cached_timestamp = -1.0;
        m_cached_next_adjacent = nullptr;
        m_cached_prev_adjacent = nullptr;
        m_cached_adjacent_frame_number = -1;
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

    // TAPE THREADING: all proxy-side state (manager, half-fps flag + vector) is published
    // together by the background conversion; the acquire load in ProxyManager() is the gate.
    // While it returns null (proxy still converting), this function runs V2-only.
    FSTP::LowCachedDecoderManager* proxy_mgr = ProxyManager();
    const bool half_fps_active = (proxy_mgr != nullptr) && m_is_half_fps_proxy;

    // HALF-FPS PROXY: Map original frame index to half-fps index
    // Example: original frames 0,1,2,3,4,5... → half-fps frames 0,0,1,1,2,2...
    int actual_index = clamped;
    std::vector<FSTP::FrameInfo>* frame_vector = m_frames;

    if (half_fps_active && m_half_fps_frames) {
        actual_index = clamped / 2; // Integer division maps pairs to same index
        frame_vector = m_half_fps_frames;

        static int mapping_log = 0;
        if (mapping_log++ < 10) {
            std::cout << "🎯 [HALF-FPS MAPPING] Original frame " << clamped
                      << " → half-fps index " << actual_index << std::endl;
        }
    }

    // Use frames decoded by LowCachedDecoderManager (the proven solution)
    if (actual_index >= static_cast<int>(frame_vector->size())) {
        if (debug_call_count <= 3) {
            std::cout << "[VIDEO] Frame index " << actual_index << " out of range (size=" << frame_vector->size() << ")" << std::endl;
        }
        return;
    }

    // SAFE CHECK: make sure frame vector was not cleared between checks
    if (!m_loaded || actual_index >= static_cast<int>(frame_vector->size())) {
        if (debug_call_count <= 3) {
            std::cout << "[VIDEO] Race condition detected: frames cleared during DisplayFrame" << std::endl;
        }
        return;
    }

    auto t_after_checks = std::chrono::high_resolution_clock::now();
    total_checks_us += std::chrono::duration_cast<std::chrono::microseconds>(t_after_checks - t_df_start).count();

    // STAGE 2 (shuttle seek-per-frame): decode exactly this displayed frame on demand — the "tape
    // head" reads what's under it now (decode = display rate, not speed). Done BEFORE any per-slot
    // lock below to avoid self-deadlock with the decoder's per-slot lock. GOP=4 → ~1-2ms; no-op if
    // already decoded.
    //
    // Active down to a low speed (not just ≥2×): below 2× the proxy is the FALLBACK while the
    // full-res V2 decoder catches up after the shuttle's big seek (~370ms). Without this bridge the
    // picture froze crossing 2× and through zero on deceleration. The "already decoded" peek makes
    // this nearly free where the manager's prefetch already filled the slot (<2×); it only fills the
    // gaps V2/manager haven't yet. Below ~0.25× we're effectively paused — V2 owns the still frame.
    // No speed gate: keep slot[clamped] filled at ALL times (incl. PAUSE) so the proxy is always
    // a ready fallback. Without this, a seek-then-pause showed a stale "sticky" frame while V2
    // rebuilt its buffer (its big seek clears it), then snapped to the right frame — the reported
    // "one frame, then a closer one". The "already decoded" peek makes steady-state nearly free
    // (the manager's prefetch usually already filled the slot below 2×); on-demand only fills the
    // gaps after a seek. Decode contexts 2/3 keep it off the manager's 0/1 and full-res's own.
    if (proxy_mgr && m_decoders_active.load()) {
        proxy_mgr->decodeFrameNow(clamped);

        // Betacam SLOW-MO compositing reads the N-1 and N+1 NEIGHBOURS, so when the proxy is the
        // base right after a shuttle seek those neighbour slots must be present too (otherwise the
        // adjacent read picks up a stale slot → N+1 spike). But this only matters in SLOW MOTION
        // (<0.9×) — at 1× there is no compositing, so the triplet there was pure wasted work that
        // tripled the on-demand decode and stalled the display thread (~50ms blocks at 1×). Gate it
        // to slow-mo only.
        double sp_now = m_audio_module ? std::abs(m_audio_module->GetActualSpeed()) : 0.0;
        if (sp_now < 0.9) {
            proxy_mgr->decodeFrameNow(clamped - 1);  // bounds-checked + no-op if decoded
            proxy_mgr->decodeFrameNow(clamped + 1);
        }
    }

    FSTP::FrameInfo& info = (*frame_vector)[actual_index];
    std::shared_ptr<AVFrame> frame;

    // Whether the displayed frame came from the full-res V2 decoder (the ground truth used
    // by the proxy-offset auto-calibration further down).
    bool shown_is_v2 = false;

    // Use the NOMINAL time of frame `clamped` (frame_number/fps from SimpleIndex) for the V2
    // query — the SAME reference the V2 sync-check validates against, and the same the proxy is
    // addressed by (index). Previously the query used the proxy's DECODED time (info.time_ms) but
    // validated against nominal — a sub-frame mismatch let V2 return clamped±1 → the residual
    // proxy→full-res shift in motion. That info.time_ms path was a workaround for the OLD lagged
    // proxy ("2 frames forward"); the proxy is frame-exact now, so nominal is authoritative.
    double timestamp = frame_info->time_seconds;

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
    // Guard: m_decoders_active is set to false as FIRST action in ShutdownDecoders,
    // so checking it here prevents accessing a partially-destroyed decoder.
    if (m_full_res_decoder && slow_or_normal_speed && m_decoders_active.load()) {
        // Inform background thread of current playback time (lightweight operation)
        m_full_res_decoder->SetPlaybackTime(timestamp);
        m_full_res_decoder->ClearStopRequest();

        // PAUSE CACHE: during pause the same timestamp is requested at render rate (60fps).
        // Return cached frame immediately — no decoder query, no V2 HIT spam.
        const double ONE_FRAME_S = (m_frame_rate > 0.0) ? (1.0 / m_frame_rate) : 0.04;
        bool same_timestamp = (std::abs(timestamp - m_v2_cached_timestamp) < ONE_FRAME_S * 0.5);
        if (actual_speed < 0.05 && same_timestamp && m_v2_cached_frame) {
            frame = m_v2_cached_frame;
            shown_is_v2 = true;
        } else {
            // Get shared_ptr copy from buffer (safe - extends frame lifetime)
            frame = m_full_res_decoder->GetFrameForTime(timestamp);
            bool v2_held = false;   // true if we held the last full-res frame (anti-flicker)
            if (frame) {
                // SYNC CHECK: Reject V2 frame if too far from requested timestamp.
                // Prevents visible frame jump (proxy→full-res) after seek/shuttle.
                // GetFrameForTime tolerates ±3.1 frames, but visual switch must be ≤1 frame.
                double v2_actual_time = m_full_res_decoder->GetLastFrameTime();
                if (v2_actual_time >= 0.0) {
                    // Compare against NOMINAL time (frame_number/fps from SimpleIndex),
                    // NOT against proxy decoded PTS. Proxy and original can have different
                    // PTS bases (systematic offset of up to 1 frame), so comparing against
                    // proxy PTS causes systematic V2 rejection → visible "frame deviation".
                    double nominal_time = frame_info->time_seconds;
                    
                    // EXACT handoff: accept the V2 (full-res) frame only when it is the SAME frame
                    // the proxy is showing (within ½ frame). The proxy shows EXACTLY frame `clamped`
                    // (Stage-2 on-demand), so a looser tol let V2 come up 1 frame off → the visible
                    // proxy→full-res shift. The anti-flicker hold below keeps the last full-res frame
                    // during V2 catch-up instead of flickering, so a tight tol no longer causes the
                    // flicker that the old 1.5-frame value was working around.
                    double frame_tolerance_frames = 0.5;
                    double half_frame = (m_frame_rate > 0.0) ? (frame_tolerance_frames / m_frame_rate) : 0.02;
                    
                    if (std::fabs(v2_actual_time - nominal_time) > half_frame) {
                        // DIAGNOSTICS: Log rejections to understand sync issues
                        static int reject_log = 0;
                        if (++reject_log % 30 == 1) {  // Log every 30 rejections
                            double diff_ms = std::fabs(v2_actual_time - nominal_time) * 1000.0;
                            double tol_ms = half_frame * 1000.0;
                            std::cout << "⚠️  [VIDEO] V2 frame off: diff=" << std::fixed << std::setprecision(2)
                                      << diff_ms << "ms > tol=" << tol_ms << "ms"
                                      << " (fps=" << std::setprecision(1) << m_frame_rate
                                      << ", tolerance=" << frame_tolerance_frames << " frames)"
                                      << std::defaultfloat << std::endl;
                        }
                        // ANTI-FLICKER: while V2 catches up (a frame or two), keep showing the last
                        // accepted full-res frame instead of dropping to the softer proxy — toggling
                        // full-res↔proxy every frame is very visible (worsened by the 432p/half-fps
                        // proxy). Only a large gap (a real seek) falls through to the proxy. The cache
                        // timestamp is NOT refreshed while holding, so the window stays anchored to the
                        // last genuine V2 frame and a truly stuck V2 still yields to the proxy.
                        double holdWindow = 6.0 / (m_frame_rate > 0.0 ? m_frame_rate : 25.0);
                        if (m_v2_cached_frame && m_v2_cached_timestamp >= 0.0 &&
                            std::abs(timestamp - m_v2_cached_timestamp) < holdWindow) {
                            frame = m_v2_cached_frame;
                            v2_held = true;
                            shown_is_v2 = true;
                        } else {
                            frame = nullptr;
                            m_v2_cached_frame = nullptr;
                            m_v2_cached_timestamp = -1.0;
                        }
                    }
                }
                // Cache only genuinely-accepted V2 frames (not held ones), so the hold window
                // stays anchored to the last real V2 hit.
                if (frame && !v2_held) {
                    m_v2_cached_frame = frame;
                    m_v2_cached_timestamp = timestamp;
                    shown_is_v2 = true;
                }
            }
        }

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
    // Use try_lock to avoid deadlock with UnloadFile during file reload
    // auto t_before_mutex = std::chrono::high_resolution_clock::now();  // Unused - performance timing disabled
    try {
        std::unique_lock<std::mutex> lock(info.mutex, std::try_to_lock);

        // If we couldn't get the lock, skip this frame (UnloadFile might be cleaning it)
        if (!lock.owns_lock()) {
            if (debug_call_count <= 5) {
                std::cout << "⚠️  [MUTEX] Couldn't lock frame " << clamped << " - skipping (file may be reloading)" << std::endl;
            }
            return;
        }

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
        // Read the proxy from the auto-calibrated slot (actual_index + offset): proxy content
        // can lag its index by an integer on some sources, so the correct picture for logical
        // frame N lives at slot N+offset. offset==0 → original behaviour (use the locked info).
        else if (!frame) {
            const int proxy_off = m_proxy_frame_offset.load();
            int pidx = actual_index + proxy_off;
            pidx = std::max(0, std::min(pidx, static_cast<int>(frame_vector->size()) - 1));

            std::shared_ptr<AVFrame> proxyFrame;
            if (proxy_off == 0) {
                proxyFrame = info.low_res_frame;          // same slot — already locked above
            } else {
                // Different slot → grab its frame under try_lock; the shared_ptr copy keeps it
                // alive after the lock is released (no nested-lock lifetime issue).
                std::unique_lock<std::mutex> off_lock((*frame_vector)[pidx].mutex, std::try_to_lock);
                if (off_lock.owns_lock()) proxyFrame = (*frame_vector)[pidx].low_res_frame;
            }

            if (proxyFrame && proxyFrame->data[0]) {
                // CRITICAL: ALWAYS check PTS before using the proxy frame (prevents jitter on
                // fast rewind 24-32x).
                bool is_seek_mode = false;
                int frame_jump = abs(clamped - m_last_good_frame_number);
                if (m_last_good_frame_number >= 0 && frame_jump > 30) {
                    is_seek_mode = true;
                }

                bool pts_valid = true;
                double frame_time = -1.0;
                if (proxyFrame->best_effort_timestamp != AV_NOPTS_VALUE && info.time_base.den > 0) {
                    frame_time = proxyFrame->best_effort_timestamp * av_q2d(info.time_base);
                } else if (proxyFrame->pts != AV_NOPTS_VALUE && info.time_base.den > 0) {
                    frame_time = proxyFrame->pts * av_q2d(info.time_base);
                }

                if (frame_time >= 0.0) {
                    // The offset shifts the proxy frame's own PTS by |offset| frames vs the
                    // requested time, so widen the tolerance by that much (otherwise the
                    // correct shifted frame would be wrongly rejected).
                    double one_frame = (m_frame_rate > 0.0) ? (1.0 / m_frame_rate) : 0.04;
                    double off_secs = std::abs(proxy_off) * one_frame;
                    double time_diff = fabs(frame_time - timestamp);
                    double max_diff = (is_seek_mode ? 0.1 : 0.5) + off_secs;
                    if (time_diff > max_diff) {
                        pts_valid = false;
                        if (debug_call_count <= 5) {
                            std::cout << "⚠️  [PTS CHECK] Frame " << clamped << " has time "
                                      << std::fixed << std::setprecision(3) << frame_time << "s"
                                      << " but requested " << timestamp << "s (diff=" << (time_diff * 1000.0) << "ms > "
                                      << (max_diff * 1000.0) << "ms) - SKIP to avoid jitter!" << std::endl;
                        }
                    }
                }

                if (pts_valid) {
                    frame = proxyFrame;
                    if (debug_call_count <= 5) {
                        std::cout << "[VIDEO] Using low_res_frame (proxy) for frame " << clamped
                                  << " (slot " << pidx << ", offset " << proxy_off << ")"
                                  << ", resolution=" << frame->width << "x" << frame->height << std::endl;
                    }
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
            // If jump > 30 frames, this is explicit seek - wake up background decoder
            bool is_seek = false;
            int frame_distance = abs(clamped - m_last_good_frame_number);
            if (m_last_good_frame_number >= 0 && frame_distance > 30) {
                is_seek = true;
                // Wake up background thread to prioritize this segment
                if (proxy_mgr) {
                    proxy_mgr->notifyFrameChange();
                }
            }

            // Fallback search for nearest available frame (silent during seek)
            if (!is_seek && (debug_call_count <= 3 || fallback_search_counter % 30 == 1)) {
                std::cout << "⚠️  [FALLBACK SEARCH #" << fallback_search_counter << "] No decoded frame available for frame " << clamped
                          << " (is_decoding=" << info.is_decoding.load()
                          << ", is_ready=" << info.is_ready.load()
                          << "), searching for nearest..." << std::endl;
            }

            // FAST EXPONENTIAL SEARCH: Check frames at exponentially increasing distances
            // Instead of checking 1,2,3,4,5... (linear O(n) with n mutex locks)
            // Check 1,2,4,8,16,32... (logarithmic O(log n) with much fewer mutex locks)
            //
            // IMPROVED: Allow wider search even when seeking to avoid freeze frame
            // HALF-FPS AWARE: Search in actual_index space (half-fps if applicable)
            std::shared_ptr<AVFrame> nearest_frame = nullptr;
            int found_idx = -1;
            int search_radius = is_seek ? 256 : 64; // Allow wider search when seeking to find ANY frame

            // Apply the calibrated proxy offset so the freeze/fallback frame stays aligned with
            // PRIORITY 2. Without this the offset correction is "lost" during post-seek decode
            // gaps → the proxy→full-res +1 jump returns until the exact frame finishes decoding
            // (which is exactly the "temporary" behaviour after a seek).
            const int proxyOff = m_proxy_frame_offset.load();

            // Exponential backward search (prefer backward - more likely to be decoded)
            // Try: -1, -2, -4, -8, -16, -32, -64
            // CRITICAL: Use try_lock to avoid deadlock with UnloadFile during file reload
            for (int step = 1; step <= search_radius; step *= 2) {
                // Check file_reloading before each lock attempt to avoid race with UnloadFile
                if (m_file_reloading.load()) break;

                int search_idx = actual_index + proxyOff - step;
                if (search_idx >= 0 && search_idx < static_cast<int>(frame_vector->size())) {
                    // Use try_lock to avoid blocking if UnloadFile is clearing this frame
                    std::unique_lock<std::mutex> search_lock((*frame_vector)[search_idx].mutex, std::try_to_lock);
                    if (!search_lock.owns_lock()) continue;  // Skip if can't get lock

                    if ((*frame_vector)[search_idx].low_res_frame &&
                        (*frame_vector)[search_idx].low_res_frame->data[0]) {
                        nearest_frame = (*frame_vector)[search_idx].low_res_frame;
                        found_idx = search_idx;
                        if (debug_call_count <= 3) {
                            std::cout << "⚡ [FAST SEARCH] Found backward frame " << search_idx
                                      << " (offset -" << step << ") for " << actual_index << std::endl;
                        }
                        break;
                    }
                }
            }

            // If not found backward, try exponential forward search
            // Try: +1, +2, +4, +8, +16, +32, +64
            // CRITICAL: Use try_lock to avoid deadlock with UnloadFile during file reload
            if (!nearest_frame && !m_file_reloading.load()) {
                for (int step = 1; step <= search_radius; step *= 2) {
                    // Check file_reloading before each lock attempt
                    if (m_file_reloading.load()) break;

                    int search_idx = actual_index + proxyOff + step;
                    if (search_idx >= 0 && search_idx < static_cast<int>(frame_vector->size())) {
                        std::unique_lock<std::mutex> search_lock((*frame_vector)[search_idx].mutex, std::try_to_lock);
                        if (!search_lock.owns_lock()) continue;  // Skip if can't get lock

                        if ((*frame_vector)[search_idx].low_res_frame &&
                            (*frame_vector)[search_idx].low_res_frame->data[0]) {
                            nearest_frame = (*frame_vector)[search_idx].low_res_frame;
                            found_idx = search_idx;
                            if (debug_call_count <= 3) {
                                std::cout << "⚡ [FAST SEARCH] Found forward frame " << search_idx
                                          << " (offset +" << step << ") for " << actual_index << std::endl;
                            }
                            break;
                        }
                    }
                }
            }

            // If exponential search found nothing, try immediate neighbors (linear search within small range)
            // This catches frames that were skipped by exponential steps (e.g., -3, -5, -6, -7)
            // HALF-FPS AWARE: Search in actual_index space
            // CRITICAL: Use try_lock to avoid deadlock with UnloadFile during file reload
            if (!nearest_frame && !m_file_reloading.load()) {
                for (int offset = 1; offset <= 8 && (actual_index - offset) >= 0; offset++) {
                    // Check file_reloading before each lock attempt
                    if (m_file_reloading.load()) break;

                    int search_idx = actual_index + proxyOff - offset;
                    if (search_idx >= 0 && search_idx < static_cast<int>(frame_vector->size())) {
                        std::unique_lock<std::mutex> search_lock((*frame_vector)[search_idx].mutex, std::try_to_lock);
                        if (!search_lock.owns_lock()) continue;  // Skip if can't get lock

                        if ((*frame_vector)[search_idx].low_res_frame &&
                            (*frame_vector)[search_idx].low_res_frame->data[0]) {
                            nearest_frame = (*frame_vector)[search_idx].low_res_frame;
                            found_idx = search_idx;
                            if (debug_call_count <= 3) {
                                std::cout << "⚡ [NEIGHBOR SEARCH] Found nearby backward frame " << search_idx
                                          << " for " << actual_index << std::endl;
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

                // Get FrameInfo for found frame (use correct vector)
                const FSTP::FrameInfo& found_info = (*frame_vector)[found_idx];

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
            // CRITICAL: Check file_reloading to avoid race with UnloadFile
            if (!nearest_frame && !m_file_reloading.load()) {
                // FREEZE FRAME: Hold last good frame while needed segment is being decoded
                static int freeze_frame_counter = 0;
                freeze_frame_counter++;

                // Silently hold last good frame while decoder loads the segment
                // (freeze frame is expected during seek, logging disabled to reduce noise)

                // HALF-FPS AWARE: Calculate last good frame index in current coordinate space
                int last_good_idx = m_last_good_frame_number;
                if (half_fps_active && m_half_fps_frames) {
                    last_good_idx = m_last_good_frame_number / 2;
                }

                // Try to use the last good frame if available
                // Use try_lock to avoid deadlock with UnloadFile during file reload
                if (!m_file_reloading.load() && last_good_idx >= 0 && last_good_idx < static_cast<int>(frame_vector->size())) {
                    std::unique_lock<std::mutex> last_lock((*frame_vector)[last_good_idx].mutex, std::try_to_lock);
                    if (last_lock.owns_lock() && (*frame_vector)[last_good_idx].low_res_frame) {
                        frame = (*frame_vector)[last_good_idx].low_res_frame;
                    }
                }

                // If still no frame, keep showing last good frame (sticky frame for pause)
                // Use try_lock to avoid deadlock with UnloadFile during file reload
                if (!frame && !m_file_reloading.load() && last_good_idx >= 0 && last_good_idx < static_cast<int>(frame_vector->size())) {
                    // Force re-submit last good frame to prevent black screen during pause
                    std::unique_lock<std::mutex> last_lock((*frame_vector)[last_good_idx].mutex, std::try_to_lock);
                    if (last_lock.owns_lock() && (*frame_vector)[last_good_idx].low_res_frame) {
                        if (SubmitFrameToTexture((*frame_vector)[last_good_idx].low_res_frame,
                                               m_last_good_frame_number, timestamp)) {
                            if (debug_call_count <= 3) {
                                std::cout << "[VIDEO] Re-submitted sticky frame " << m_last_good_frame_number
                                          << " (mapped to idx " << last_good_idx << ") to prevent black screen" << std::endl;
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

    // === Proxy↔original frame-offset auto-calibration ===
    // The proxy's content can lag its index by an integer on some sources (e.g. start_time!=0
    // makes AVAssetReader insert a gap-fill frame), which showed as a 1-frame jump at the
    // proxy→full-res handoff. While the full-res V2 frame is on screen (ground truth) at
    // <=1.5x, compare its 4x4 luma "shape" (per-cell mean minus the global mean — robust to
    // resolution and to a constant brightness offset, sensitive to motion) against
    // proxy[N-1..N+1]. Once a clear-motion frame makes the match unambiguous, lock the integer
    // offset; it is then applied wherever proxy frames are read (PRIORITY 2 + Betacam
    // neighbours). The whole computation is skipped after calibration.
    if (shown_is_v2 && !m_proxy_offset_calibrated) {
        double sp_l = m_audio_module ? m_audio_module->GetActualSpeed() : 0.0;
        if (std::abs(sp_l) <= 1.5) {
            auto shapeSig = [](const std::shared_ptr<AVFrame>& f, double out[16]) -> bool {
                for (int i = 0; i < 16; ++i) out[i] = 0.0;
                if (!f || !f->data[0] || f->width <= 0 || f->height <= 0) return false;
                const uint8_t* y = f->data[0]; int pitch = f->linesize[0];
                long cellSum[16] = {0}; int cellCnt[16] = {0};
                for (int yy = 0; yy < f->height; yy += 2) {
                    int cy = (yy * 4) / f->height; if (cy > 3) cy = 3;
                    const uint8_t* row = y + static_cast<size_t>(yy) * pitch;
                    for (int xx = 0; xx < f->width; xx += 2) {
                        int cx = (xx * 4) / f->width; if (cx > 3) cx = 3;
                        cellSum[cy * 4 + cx] += row[xx]; cellCnt[cy * 4 + cx]++;
                    }
                }
                double g = 0.0; int gc = 0;
                for (int i = 0; i < 16; ++i) if (cellCnt[i] > 0) { out[i] = (double)cellSum[i] / cellCnt[i]; g += out[i]; gc++; }
                g /= (gc > 0 ? gc : 1);
                for (int i = 0; i < 16; ++i) out[i] -= g;   // remove global brightness offset
                return true;
            };
            auto sigDist = [](const double a[16], const double b[16]) {
                double d = 0.0; for (int i = 0; i < 16; ++i) d += std::abs(a[i] - b[i]); return d;
            };
            double sv2[16]; bool okv2 = shapeSig(frame, sv2);
            double dist[3] = {-1.0, -1.0, -1.0};
            for (int k = -1; k <= 1; ++k) {
                int idx = actual_index + k;
                if (okv2 && idx >= 0 && idx < static_cast<int>(frame_vector->size())) {
                    std::unique_lock<std::mutex> lk((*frame_vector)[idx].mutex, std::try_to_lock);
                    if (lk.owns_lock()) {
                        double sp[16];
                        if (shapeSig((*frame_vector)[idx].low_res_frame, sp)) dist[k + 1] = sigDist(sv2, sp);
                    }
                }
            }
            int bestk = 99; double bd = 1e18;
            for (int k = -1; k <= 1; ++k) if (dist[k + 1] >= 0.0 && dist[k + 1] < bd) { bd = dist[k + 1]; bestk = k; }

            // Lock the offset once a clear-motion frame makes the winner unambiguous;
            // consensus guards against a single noisy frame mis-calibrating.
            if (bestk != 99 && dist[0] >= 0.0 && dist[1] >= 0.0 && dist[2] >= 0.0) {
                double second = 1e18;
                for (int k = -1; k <= 1; ++k) { if (k != bestk && dist[k + 1] < second) second = dist[k + 1]; }
                // Decisive = real motion + clear winner (skips static/ambiguous frames).
                bool decisive = (second > bd * 1.6) && ((second - bd) > 3.0);
                // Very strong = unambiguous motion → safe to lock from a single frame, so even
                // the FIRST proxy→full-res handoff is clean (no "first jerk" before calibration).
                bool veryStrong = (second > bd * 3.0) && ((second - bd) > 10.0);
                if (decisive) {
                    if (bestk == m_proxy_calib_candidate) {
                        m_proxy_calib_count++;
                    } else {
                        m_proxy_calib_candidate = bestk;
                        m_proxy_calib_count = 1;
                    }
                    int needed = veryStrong ? 1 : 2;   // instant on clear motion, else confirm with 2
                    if (m_proxy_calib_count >= needed) {
                        m_proxy_frame_offset.store(bestk);
                        m_proxy_offset_calibrated = true;
                        std::cout << "🎯 [PROXY CALIB] Locked proxy frame offset = " << bestk << std::endl;
                    }
                }
            }
        }
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

    // Get adjacent frames for Betacam slow-motion compositing
    // These are frames at index N-1 (prev) and N+1 (next)
    std::shared_ptr<AVFrame> prev_frame = nullptr;
    std::shared_ptr<AVFrame> next_frame = nullptr;

    // Reset the adjacent-frame sticky cache up-front when the displayed frame changed, when the
    // SOURCE changed (proxy↔full-res), or during reload. Doing it BEFORE the fetch lets us skip the
    // decoder query while a frame is held (pause / slow-mo on the same frame). The source check is
    // CRITICAL: on a held frame (clamped unchanged) the proxy→full-res handoff kept serving the
    // stale PROXY neighbours to a full-res base — 540p prev/next under a 1080p now → the N+1 spike.
    if (m_file_reloading.load()) {
        m_cached_adjacent_frame_number = -1;
        m_cached_next_adjacent = nullptr;
        m_cached_prev_adjacent = nullptr;
    } else if (clamped != m_cached_adjacent_frame_number || shown_is_v2 != m_cached_adjacent_was_v2) {
        m_cached_adjacent_frame_number = clamped;
        m_cached_adjacent_was_v2 = shown_is_v2;
        m_cached_next_adjacent = nullptr;
        m_cached_prev_adjacent = nullptr;
    }

    // Only query the decoder while the cache for this frame is still incomplete.
    // Once both N-1 and N+1 are cached, repeated calls on a held frame reuse them
    // instead of hammering GetFrameForTime / try_lock every render frame.
    bool adjacent_cache_incomplete = !m_cached_prev_adjacent || !m_cached_next_adjacent;

    // Only fetch adjacent frames when at slow speed (compositing is needed)
    // NOTE: Uses try_lock to avoid deadlock with UnloadFile during file reload
    double actual_speed_for_adjacent = 0.0;
    if (m_audio_module) {
        actual_speed_for_adjacent = m_audio_module->GetActualSpeed();
    }

    if (actual_speed_for_adjacent <= 1.0 && !m_file_reloading.load() && adjacent_cache_incomplete) {
        // Calculate frame duration for adjacent timestamp calculation
        double fps = (m_frame_rate > 0) ? m_frame_rate : 25.0;
        double frame_duration = 1.0 / fps;

        // Source the adjacent frames from the SAME decoder that produced the base frame. Use the
        // explicit shown_is_v2 flag — NOT a height threshold: the proxy is now 540p (>480), so the
        // old `height > 480` test mis-classified a PROXY base as full-res and fetched V2 neighbours
        // for it. Right after a shuttle seek (V2 buffer still rebuilding) those V2 neighbours were
        // missing/wrong → the N+1 spike exactly at the proxy→full-res transition.
        bool using_full_res = shown_is_v2;

        if (using_full_res && m_full_res_decoder && m_decoders_active.load() && !m_file_reloading.load()) {
            // FULL-RES MODE: get the N-1/N/N+1 triplet in ONE atomic buffer read. The neighbours are
            // the decoder buffer's immediate contiguous frames around `now` — a guaranteed-consistent
            // triplet, unlike three separate time-queries (which, via the per-call cache + nearest
            // match, could return a mismatched set → the seam spike on each new frame).
            try {
                if (!m_file_reloading.load()) {
                    std::shared_ptr<AVFrame> triplet_now;
                    m_full_res_decoder->GetFrameTriplet(timestamp, prev_frame, triplet_now, next_frame);
                }

                // Guard against a buffer-edge duplicate (compositing a copy of the current frame
                // would do nothing useful and can shimmer).
                if (prev_frame && frame &&
                    (prev_frame.get() == frame.get() || prev_frame->data[0] == frame->data[0])) {
                    prev_frame = nullptr;
                }
                if (next_frame && frame &&
                    (next_frame.get() == frame.get() || next_frame->data[0] == frame->data[0])) {
                    next_frame = nullptr;
                }
            } catch (...) {
                prev_frame = nullptr;
                next_frame = nullptr;
            }
        } else if (frame_vector && actual_index >= 0 && !m_file_reloading.load()) {
            // PROXY MODE: Get adjacent frames from frame_vector
            // CRITICAL: Use try_lock to avoid deadlock with UnloadFile
            // If mutex is held by UnloadFile, we just skip the adjacent frame
            try {
                // Apply the same calibrated proxy offset so the Betacam compositing neighbours
                // are the correct pictures (slot N+offset holds logical frame N).
                const int proxy_off_adj = m_proxy_frame_offset.load();
                // Get previous frame (N-1)
                int prev_idx = actual_index + proxy_off_adj - 1;
                if (prev_idx >= 0 && prev_idx < static_cast<int>(frame_vector->size()) && !m_file_reloading.load()) {
                    std::unique_lock<std::mutex> prev_lock((*frame_vector)[prev_idx].mutex, std::try_to_lock);
                    if (prev_lock.owns_lock()) {
                        if ((*frame_vector)[prev_idx].low_res_frame &&
                            (*frame_vector)[prev_idx].low_res_frame->data[0]) {
                            prev_frame = (*frame_vector)[prev_idx].low_res_frame;
                        }
                    }
                    // If lock failed, skip this frame (UnloadFile might be holding it)
                }

                // Get next frame (N+1) - use try_lock to avoid blocking
                int next_idx = actual_index + proxy_off_adj + 1;
                if (next_idx >= 0 && next_idx < static_cast<int>(frame_vector->size()) && !m_file_reloading.load()) {
                    std::unique_lock<std::mutex> next_lock((*frame_vector)[next_idx].mutex, std::try_to_lock);
                    if (next_lock.owns_lock()) {
                        if ((*frame_vector)[next_idx].low_res_frame &&
                            (*frame_vector)[next_idx].low_res_frame->data[0]) {
                            next_frame = (*frame_vector)[next_idx].low_res_frame;
                        }
                    }
                    // If lock failed, skip this frame
                }
            } catch (...) {
                prev_frame = nullptr;
                next_frame = nullptr;
            }
        }
    }

    // Adjacent-frame sticky cache update (the per-frame reset is handled above,
    // before the fetch). Store any freshly obtained frames, then fall back to the
    // cache for any we couldn't obtain this call — whether because try_to_lock lost
    // the race with the decoder thread, or because the fetch was skipped entirely
    // (cache already complete for this held frame). This keeps compositing stable
    // and prevents flicker while eliminating redundant decoder queries.
    if (!m_file_reloading.load()) {
        if (next_frame) m_cached_next_adjacent = next_frame;
        if (prev_frame) m_cached_prev_adjacent = prev_frame;
        if (!next_frame) next_frame = m_cached_next_adjacent;
        if (!prev_frame) prev_frame = m_cached_prev_adjacent;
    }

    // Submit real decoded frame to texture interface (with adjacent frames for compositing)
    if (SubmitFrameToTexture(frame, clamped, timestamp, prev_frame, next_frame)) {
        m_last_good_frame_number = clamped;
        if (debug_call_count <= 3) {
            std::cout << "[VIDEO] Submitted real decoded frame " << clamped
                      << " (time=" << std::fixed << std::setprecision(3) << timestamp << "s)"
                      << ", prev=" << (prev_frame ? "YES" : "NO")
                      << ", next=" << (next_frame ? "YES" : "NO") << std::endl;
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
    // If frame hasn't changed since last render - skip (unless forced update)
    bool force_update = m_force_frame_update.exchange(false); // Reset flag atomically

    // BETACAM EFFECT: Don't skip frames at pause/slow motion or shuttle speeds
    // The Betacam effect needs continuous rendering to show the noise stripe
    // even when the video frame itself doesn't change
    // Use ACTUAL speed from audio module, not target speed (m_speed)
    double actual_speed = 0.0;
    if (m_audio_module) {
        actual_speed = m_audio_module->GetActualSpeed();
    }
    double abs_speed = std::abs(actual_speed);
    bool betacam_speed_range = (abs_speed < 0.9 || abs_speed > 1.1);
    if (betacam_speed_range) {
        bool is_pure_pause = (abs_speed < 0.05);
        if (!is_pure_pause) {
            // Slow motion or shuttle: re-render continuously so the Betacam stripe animates and the
            // proxy steps, even when the audio frame index hasn't advanced. This forced re-render
            // runs once per PRESENT, so it scales with display refresh — on a 165 Hz panel it fired
            // 2.75× as often as on 60 Hz, and each present also pays the megacommit's on-demand
            // proxy decode (decodeFrameNow + per-slot lock) → visible drops on high-refresh screens
            // (reported by a 165 Hz user, effect on OR off). Cap it to 60 fps by wall clock: ~60
            // proxy frames/sec during a scrub is visually identical and the stripe animates just as
            // smoothly, at a fraction of the cost.
            //
            // Render is vblank-locked, so on a panel whose refresh isn't a multiple of 60 (165 Hz)
            // we can only skip WHOLE presents. Advancing the deadline by one 60 Hz period (instead
            // of resetting it to "now") makes the long-run count land on exactly 60 renders/sec:
            // the skips alternate 2/3 vblanks (≈82/≈55 fps instantaneous) and average to 60. Panels
            // at 60/120/144 Hz render on every Nth present cleanly; a small tolerance keeps 60 Hz
            // itself rendering every present despite present-time jitter. An explicit force
            // (segment-decode ready, already in `force_update`) is never throttled away.
            constexpr double kTargetFrameMs   = 1000.0 / 60.0;  // 16.667 ms → 60 fps
            constexpr double kJitterToleranceMs = 2.0;          // don't let jitter halve a 60 Hz panel
            auto now = std::chrono::steady_clock::now();
            double since_ms =
                std::chrono::duration<double, std::milli>(now - m_last_shuttle_render).count();
            if (!force_update && since_ms + kJitterToleranceMs < kTargetFrameMs) {
                return;  // too soon since the last shuttle render — skip this present entirely
            }
            if (since_ms > kTargetFrameMs * 4.0) {
                // Big gap (just entered shuttle, or was idle): resync to avoid a catch-up burst.
                m_last_shuttle_render = now;
            } else {
                // Advance by a whole 60 Hz period so the average stays exactly 60 fps.
                m_last_shuttle_render += std::chrono::microseconds(16667);
            }
            force_update = true;
        } else {
            // Pure pause: force until audio aligns to frame boundary (stripe disappears).
            // On the transition frame (false→true): force ONE more DisplayFrame so the
            // betacam effect renders a clean frame (no prev/next compositing).
            bool stripe_settled = m_audio_module && m_audio_module->IsFrameAligned();
            bool just_aligned = stripe_settled && !m_last_frame_aligned;
            m_last_frame_aligned = stripe_settled;
            if (!stripe_settled || just_aligned) {
                force_update = true;
            }
        }
    }

    if (audioFrame == m_last_displayed_frame && !force_update) {
        // Frame unchanged and Betacam stripe settled — skip all rendering and notifications
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

    // Update OSD decoded frames indicator (top bar)
    UpdateOSDDecodedFramesMap();

    perf_samples++;

    // Progressive scan removed
}

void FSTPVideoModuleWrapper::UpdateVideoFrame() const {
    // Don't add FSTP_COUNT_CALL here, as this function just delegates to non-const version
    // which already has the counter. Otherwise we get double counting!
    const_cast<FSTPVideoModuleWrapper*>(this)->UpdateVideoFrame();
}

void FSTPVideoModuleWrapper::UpdateOSDDecodedFramesMap() const {
    if (!m_loaded || !m_frames) {
        return;
    }

    // Throttle updates to once per 100ms
    static auto last_update = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_update);
    if (elapsed.count() < 100) {
        return;
    }
    last_update = now;

    // Build decoded frames map
    int total_frames = static_cast<int>(m_frames->size());
    std::vector<bool> decoded_map(total_frames, false);

    for (int i = 0; i < total_frames; ++i) {
        const auto& frame_info = (*m_frames)[i];
        decoded_map[i] = (frame_info.low_res_frame != nullptr);
    }

    // Update OSD
    UpdateOSDDecodedFrames(m_instance_id, decoded_map, total_frames);
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
    // Invalidate pause-frame cache — position changed, need a fresh decode
    m_v2_cached_frame.reset();
    m_v2_cached_timestamp = -1.0;

    int frameNumber = m_frame_index->FindFrameByTime(position_seconds);
    frameNumber = std::max(0, std::min(frameNumber, static_cast<int>(m_frames->size()) - 1));
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
