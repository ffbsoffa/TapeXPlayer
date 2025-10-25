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
    explicit FSTPFullResDecoderV2(const std::string& sourceFilename);
    ~FSTPFullResDecoderV2();

    // Initialization
    bool IsInitialized() const { return initialized_; }

    // STREAMING API (new!)
    // Set current playback time (background thread will update buffer)
    void SetPlaybackTime(double time_seconds);

    // Get frame for given time (or nullptr if not in buffer)
    std::shared_ptr<AVFrame> GetFrameForTime(double time_seconds);

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

private:
    // Initialization
    bool Initialize();
    void Cleanup();

    // Hardware acceleration
    bool InitializeHardwareAcceleration(const AVCodec* codec);
    static AVPixelFormat GetHWFormat(AVCodecContext* ctx, const enum AVPixelFormat* pix_fmts);

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

    double buffer_window_ahead_ = 3.0;  // Decode at least 3.0 sec ahead
    double buffer_window_behind_ = 2.0; // Keep 2.0 sec behind (DO NOT aggressively remove frames!)
};

#endif // FSTP_FULL_RES_DECODER_V2_H
