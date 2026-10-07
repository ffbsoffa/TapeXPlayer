#ifndef FSTP_FULL_RES_DECODER_V2_H
#define FSTP_FULL_RES_DECODER_V2_H

#include <string>
#include <atomic>
#include <memory>
#include <deque>
#include <mutex>
#include <thread>
#include <condition_variable>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>
}

/**
 * FSTPFullResDecoder - Streaming high-resolution video decoder
 *
 * NEW APPROACH (v2):
 * - Independent buffer (NOT bound to frameIndex)
 * - Streaming decoding (10-20 frames in memory)
 * - Downscaling to 720p to save memory
 * - Works by time (time_seconds), not by index
 */

// Frame structure in buffer
struct StreamFrame {
    std::shared_ptr<AVFrame> frame;  // Decoded frame (already YUV420P + downscaled)
    double time_seconds;              // Frame time
    int64_t pts;                      // PTS for synchronization

    StreamFrame() : time_seconds(0.0), pts(AV_NOPTS_VALUE) {}
};

class FSTPFullResDecoderV2 {
public:
    explicit FSTPFullResDecoderV2(const std::string& sourceFilename, double initial_time = 0.0);
    ~FSTPFullResDecoderV2();

    // Initialization
    bool IsInitialized() const { return initialized_; }

    // STREAMING API (new!)
    // Set current playback time (background thread will update buffer)
    void SetPlaybackTime(double time_seconds);

    // While paused only a short stretch ahead is kept decoded (kPausedAhead): enough for frame
    // steps and the start of playback. The window behind (backward frame steps) is unchanged.
    void SetPaused(bool paused) { paused_.store(paused, std::memory_order_relaxed); }

    // Get frame for given time (or nullptr if not in buffer)
    std::shared_ptr<AVFrame> GetFrameForTime(double time_seconds);

    // Get a CONSISTENT N-1 / N / N+1 triplet in one atomic buffer read for Betacam compositing.
    // `now` is the buffered frame nearest now_time; `prev`/`next` are its IMMEDIATE buffer
    // neighbours — and since the buffer holds contiguous decoded frames, those are exactly the
    // N-1 / N+1 pictures. This replaces three independent time-queries (which, with the per-call
    // cache + nearest matching, could return a mismatched triplet → seam spike on each new frame).
    // Any of the three may be null at a buffer edge.
    void GetFrameTriplet(double now_time,
                         std::shared_ptr<AVFrame>& prev,
                         std::shared_ptr<AVFrame>& now,
                         std::shared_ptr<AVFrame>& next);

    // Get actual timestamp of last frame returned by GetFrameForTime (-1 if none)
    double GetLastFrameTime() const {
        return cached_frame_time_.load(std::memory_order_relaxed);
    }

    // Clear buffer
    void ClearBuffer();

    // Stop decoding
    void RequestStop();
    void ClearStopRequest();

    // Adaptive buffer size (for multi-instance)
    void SetBufferSize(int size) { buffer_size_target_.store(size); }

    // Getting metadata
    int GetWidth() const { return display_width_; }    // After downscale
    int GetHeight() const { return display_height_; }  // After downscale
    int GetNativeWidth() const { return native_width_; }
    int GetNativeHeight() const { return native_height_; }
    double GetDuration() const { return duration_; }
    double GetFrameRate() const { return frame_rate_; }

    // MEASURED, not guessed: frames this decoder actually produces per second of its own busy
    // time on this file and this machine (decode + GPU→RAM copy included). The wrapper keeps the
    // full-res decoder only if this clears realtime with margin — one rule on every platform,
    // instead of recognising the CPU by name.
    double MeasuredDecodeFps() const;
    int MeasuredFrames() const { return all_frames_.load(std::memory_order_relaxed); }
    // Frames measured after the start-up frames (see kWarmupFrames in the .cpp).
    int SteadyFrames() const { return steady_frames_.load(std::memory_order_relaxed); }
    // Whether the frames come off a hardware decoder (VideoToolbox / D3D11VA / VA-API surface).
    bool DecodesOnGPU() const { return gpu_frames_.load(std::memory_order_relaxed) > 0; }

private:
    // Initialization
    bool Initialize();
    void Cleanup();

    // Hardware acceleration
    bool InitializeHardwareAcceleration(const AVCodec* codec);
    static AVPixelFormat GetHWFormat(AVCodecContext* ctx, const enum AVPixelFormat* pix_fmts);
    bool HardwareTakesThisStream();

    // Decoding frame with downscaling
    std::shared_ptr<AVFrame> DecodeAndProcessFrame(double targetTime);

    // Downscaling (4K → 720p)
    bool DownscaleFrame(AVFrame* src, AVFrame* dst);

    // Seek to time
    bool SeekToTime(double time_seconds);

    // Background decoding thread
    void DecodingThreadLoop();

private:
    std::string source_filename_;
    bool initialized_;
    std::atomic<bool> stop_requested_;
    std::atomic<bool> thread_running_;

    // Video metadata
    int native_width_;       // Original resolution
    int native_height_;
    int display_width_;      // After downscale (max 1280x720)
    int display_height_;
    double duration_;
    double frame_rate_;
    AVPixelFormat pix_fmt_;

    // FFmpeg contexts
    AVFormatContext* format_ctx_;
    AVCodecContext* codec_ctx_;
    const AVCodecParameters* codec_params_;
    AVStream* video_stream_;
    int video_stream_index_;

    // Hardware acceleration
    bool hw_accel_enabled_;
    AVBufferRef* hw_device_ctx_;
    AVPixelFormat hw_pix_fmt_;

    // Downscaling context (cached)
    SwsContext* downscale_ctx_;

    // Output format FFmpeg settled on through GetHWFormat (-2 = not asked yet)
    std::atomic<int> chosen_pix_fmt_{-2};

    // Throughput measurement (see MeasuredDecodeFps): everything, and the steady part after the
    // start-up frames — those carry decoder init and, with frame threading, the pipeline fill,
    // and made a 3×-realtime 4K HEVC decode measure 2.0× on the Pentium Gold 7505.
    std::atomic<int> all_frames_{0};
    std::atomic<int64_t> all_busy_us_{0};
    std::atomic<int> steady_frames_{0};
    std::atomic<int64_t> steady_busy_us_{0};
    std::atomic<int> gpu_frames_{0};

    // STREAMING BUFFER (independent of frameIndex!)
    std::deque<StreamFrame> stream_buffer_;
    std::mutex buffer_mutex_;
    std::atomic<int> buffer_size_target_{20};  // Adaptive buffer size (dynamically changes)

    // LOCK-FREE CACHE for GetFrameForTime (optimization for 60 FPS render)
    std::atomic<double> cached_frame_time_{-1.0};
    std::shared_ptr<AVFrame> cached_frame_;
    std::mutex cache_mutex_;  // Only for updating cache

    // Background decoding thread
    std::thread decoding_thread_;
    std::atomic<double> current_playback_time_;
    std::condition_variable cv_;
    std::mutex cv_mutex_;

    // Adaptive buffer windows (calculated based on FPS)
    // High FPS (>=50): larger windows to handle frame density
    // Low FPS (<50): standard windows
    double buffer_window_ahead_ = 3.0;  // Decode at least 3.0 sec ahead
    double buffer_window_behind_ = 2.0; // Keep 2.0 sec behind (DO NOT aggressively remove frames!)
    std::atomic<bool> paused_{false};
    static constexpr double kPausedAhead = 0.5;   // seconds kept ahead while paused
    
    // FPS-adaptive helper
    void UpdateBufferWindowsForFPS();
};

#endif // FSTP_FULL_RES_DECODER_V2_H
