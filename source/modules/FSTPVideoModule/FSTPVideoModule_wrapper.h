#pragma once

#include <string>
#include <memory>
#include <atomic>
#include <vector>
#include <chrono>

#include "FSTPFrameConverter.h"
#include "FSTPVideoFrame.h"
#include "FSTPLowResDecoder.h"
#include "FSTPLowCachedDecoderManager.h"
#include "FSTPSimpleVideoIndex.h"
#include "FSTPFullResDecoder_v2.h"  // V2: Streaming decoder

extern "C" {
#include <libavcodec/avcodec.h>
}

struct SwsContext;

// Forward declaration
class FSTPAudioModuleWrapper;

class FSTPVideoModuleWrapper {
private:
    bool m_initialized = false;
    bool m_loaded = false;
    std::string m_current_file;
    std::string m_proxy_path;
    bool m_use_original_as_proxy = false; // Flag: use original as proxy (≤480p h264)

    int m_instance_id = -1;

    std::atomic<double> m_duration{0.0};
    std::atomic<bool> m_playing{false};
    std::atomic<double> m_speed{1.0};
    std::atomic<bool> m_reverse{false};

    FSTPAudioModuleWrapper* m_audio_module;

    std::unique_ptr<FSTPFrameConverter> m_frame_converter;

    mutable int m_current_frame_number;
    mutable int m_last_good_frame_number;

    // Optimization: track last rendered frame to skip identical ones
    mutable int m_last_displayed_frame = -1;
    mutable std::atomic<bool> m_force_frame_update{false}; // Force update after segment decode
    mutable bool m_last_frame_aligned = false; // Previous IsFrameAligned() for transition detection
    // Phase-accumulator deadline for the shuttle/slow-mo forced re-render (see UpdateVideoFrame).
    // During shuttle we re-render every present to animate the Betacam stripe and step the proxy;
    // that scales with display refresh, so on a 165 Hz panel it ran 2.75× more often than on 60 Hz
    // — and each present now also does an on-demand proxy decode (decodeFrameNow). Advancing this by
    // one 60 Hz period per render (not resetting to "now") caps the long-run rate to exactly 60 fps
    // on any panel — visually identical during a scrub, at a fraction of the high-refresh cost.
    mutable std::chrono::steady_clock::time_point m_last_shuttle_render{};

    std::unique_ptr<FSTPSimpleVideoIndex> m_frame_index;
    // CRITICAL FIX: Use raw pointer to prevent destructor cleanup
    // Problem: vector destructor calls shared_ptr destructors → heap corruption
    // Solution: Allocate dynamically and NEVER free (leak on exit acceptable)
    std::vector<FSTP::FrameInfo>* m_frames = nullptr;

    // HALF-FPS PROXY OPTIMIZATION: Separate index for 30fps proxy from 60fps original
    // When proxy is 30fps from 60fps source, create dedicated half-size index
    // This avoids 50% EMPTY slots in main index which trigger expensive fallback search
    // Mapping: original_frame_idx → half_fps_idx = original_frame_idx / 2
    std::vector<FSTP::FrameInfo>* m_half_fps_frames = nullptr;
    bool m_is_half_fps_proxy = false; // Flag: proxy is 30fps from 60fps original

    std::atomic<int> m_current_index{0};
    std::atomic<bool> m_decoders_active{false};
    std::atomic<bool> m_file_reloading{false}; // Protection from race condition when changing file

    // TAPE THREADING (instant start): when the proxy needs a full conversion, LoadFile no longer
    // blocks on it. Playback starts immediately from the ORIGINAL via the full-res V2 decoder
    // (pause + forward ≤1×), while convertToLowRes runs on a background thread. When it finishes,
    // the LowCachedDecoderManager is created and published — only then is shuttle/reverse allowed.
    //
    // m_proxy_ready gates EVERY read of m_low_cached_manager (and the half-fps state): the
    // background thread fully builds the manager, then release-stores true; readers acquire-load
    // it first, so the unique_ptr itself is never read concurrently with its assignment.
    // m_load_generation (bumped in UnloadFile under the instance-registry mutex) lets a finishing
    // conversion detect that its file was unloaded/replaced and drop its result silently.
    std::atomic<bool> m_proxy_ready{false};
    std::atomic<uint32_t> m_load_generation{0};

    std::atomic<bool> m_backgrounded{false};   // Instance lost focus & not needed simultaneously
                                               // (presentation off) → full-res V2 freed to save RAM

    std::unique_ptr<FSTP::LowCachedDecoderManager> m_low_cached_manager;
    std::unique_ptr<FSTPFullResDecoderV2> m_full_res_decoder;  // V2: Streaming decoder
    bool m_full_res_stopped = false;  // SPEED OPT state (per-instance; hysteresis in SetSpeed)

    // Pause-frame cache: avoid re-querying decoder when position hasn't changed
    std::shared_ptr<AVFrame> m_v2_cached_frame;
    double m_v2_cached_timestamp = -1.0;

    // Adjacent-frame cache for Betacam compositing.
    // try_to_lock may intermittently fail when decoder threads briefly hold the mutex.
    // Caching the last valid N-1 / N+1 frames prevents compositing from flickering off.
    std::shared_ptr<AVFrame> m_cached_next_adjacent;
    std::shared_ptr<AVFrame> m_cached_prev_adjacent;
    int m_cached_adjacent_frame_number = -1;
    bool m_cached_adjacent_was_v2 = false;  // source of the cached neighbours (proxy vs full-res)

    // Auto-calibrated proxy↔original frame offset. On some sources the proxy's content lags
    // its index by an integer number of frames (e.g. start_time != 0 makes AVAssetReader add
    // a gap-fill frame), which showed as a 1-frame "jump" at the proxy→full-res handoff.
    // Measured once per file from a clear-motion frame by comparing V2 (ground truth) against
    // proxy[N-1..N+1] 4x4 luma-shape signatures, then applied when reading proxy frames.
    std::atomic<int> m_proxy_frame_offset{0};   // proxy slot to read for logical frame N = N + offset
    bool m_proxy_offset_calibrated = false;
    int  m_proxy_calib_candidate = 99;          // consensus tracking during calibration
    int  m_proxy_calib_count = 0;


    mutable FSTP::FrameBuffer m_display_buffer;

    int m_low_res_range = 1200;
    int m_high_res_window = 100;  // Reduced from 500 to 100 to reduce load

    int m_video_width = 0;
    int m_video_height = 0;
    double m_frame_rate = 0.0;
    int m_total_frames = 0;

    // Proxy probe result: what LoadFile must do about the proxy.
    enum class ProxyState {
        Ready,            // usable proxy exists (original-as-proxy symlink or valid cache)
        NeedsConversion,  // full convertToLowRes required
        Error             // source unreadable
    };

    bool EnsureProxy(const std::string& filepath, const std::function<void(int)>& progressCallback);
    // Cheap part of EnsureProxy: GOP analysis (≤1000 packets), original-as-proxy symlink,
    // cache manifest validation. Sets m_proxy_path / m_use_original_as_proxy. Never transcodes.
    ProxyState ProbeProxy(const std::string& filepath);
    // Proxy-side decoder setup shared by the synchronous path and the background completion:
    // half-fps proxy detection (+ half index init) and LowCachedDecoderManager creation + run().
    // Does NOT publish m_proxy_ready — the caller does, once everything is in place.
    bool SetupProxyDecoder();
    // Runs on a detached thread when a conversion is needed: converts, then (if this load is
    // still current — checked under the instance-registry mutex) publishes the proxy decoder.
    static void BackgroundProxyConversion(int instance_id, uint32_t generation,
                                          std::string filepath);
    // Gate for every m_low_cached_manager access (see m_proxy_ready above).
    FSTP::LowCachedDecoderManager* ProxyManager() const {
        return m_proxy_ready.load(std::memory_order_acquire) ? m_low_cached_manager.get() : nullptr;
    }
    // with_proxy=false → V2-only start (tape threading); proxy decoder arrives later
    // via BackgroundProxyConversion → SetupProxyDecoder → m_proxy_ready publication.
    bool InitializeDecoders(double initial_time = 0.0, bool with_proxy = true);
    void ShutdownDecoders();
    void NotifyDecodersOfFrameChange(int frame_number);
    bool SubmitFrameToTexture(const std::shared_ptr<AVFrame>& frame,
                              int frame_number,
                              double timestamp,
                              const std::shared_ptr<AVFrame>& prev_frame = nullptr,
                              const std::shared_ptr<AVFrame>& next_frame = nullptr);

public:
    FSTPVideoModuleWrapper();
    ~FSTPVideoModuleWrapper();

    bool Initialize();
    void Shutdown();

    bool LoadFile(const std::string& filepath, double initial_time = 0.0);
    void UnloadFile();

    bool Play();
    bool Pause();
    bool Stop();

    void SetSpeed(double speed);
    void SetSpeedInstant(double speed);
    // Backgrounded = this instance lost focus and isn't needed at the same time as the active one.
    // Frees the heavy full-res V2 decoder buffer to respect the machine's resources; the still frame
    // stays (cached texture). Resumed (V2 rebuilt, proxy bridges) on refocus. NOT used while a
    // presentation/conference window is up — there both materials must stay instantly ready.
    void SetBackgrounded(bool backgrounded);
    void SetReverse(bool reverse);

    void SetPosition(double position_seconds);
    double GetPosition() const;
    double GetDuration() const;

    bool IsPlaying() const;
    bool IsLoaded() const;
    double GetSpeed() const;
    double GetActualSpeed() const;
    bool IsReverse() const;

    int GetWidth() const;
    int GetHeight() const;
    double GetFrameRate() const;
    int GetTotalFrames() const;
    std::string GetVideoCodecName() const;

    // Functions for developer visualization
    const std::vector<FSTP::FrameInfo>& GetFrameIndex() const;
    int GetCurrentFrameIndex() const;
    void GetBufferRanges(int& bufferStart, int& bufferEnd, int& highResStart, int& highResEnd) const;

    bool IsFastBufferReady() const;
    bool IsFullBufferReady() const;

    // TAPE THREADING: false while the proxy is still being converted in the background —
    // the transport must stay limited to pause/forward ≤1× (V2 reads the original; reverse
    // and shuttle need the GOP=4 proxy). True for audio-only wrappers is irrelevant: the
    // player instance only consults this when video is loaded.
    bool IsShuttleReady() const { return m_proxy_ready.load(std::memory_order_acquire); }

    bool GetCurrentFrame(uint8_t** frame_data, int& width, int& height) const;
    void ReleaseFrame(uint8_t* frame_data) const;

    void SetAudioModule(FSTPAudioModuleWrapper* audio_module);
    int GetCurrentAudioFrame() const;

    void SetInstanceID(int instance_id);

    // Force frame update after segment decode (for seek responsiveness)
    void RequestFrameUpdate();
    int GetInstanceID() const;

    void DisplayFrame(int frame_number);
    void UpdateVideoFrame();
    void UpdateVideoFrame() const;

    void UpdateOSDDecodedFramesMap() const;
};
