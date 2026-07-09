#pragma once

#include <vector>
#include <string>
#include <atomic>
#include <mutex>
#include <thread>
#include <future>
#include <memory>  // std::shared_ptr — libc++ (clang) does not pull it in transitively

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

// Frame type for better structure understanding
enum FrameType {
    FRAME_I = 0,    // I-frame (key frame)
    FRAME_P = 1,    // P-frame (predicted)
    FRAME_B = 2,    // B-frame (bidirectionally predicted)
    FRAME_UNKNOWN = 3
};

// Frame indexing state
enum IndexingState {
    INDEX_EMPTY = 0,        // Frame not indexed
    INDEX_METADATA = 1,     // Basic metadata available
    INDEX_QUICK = 2,        // Quick indexing (every N-th frame)
    INDEX_COMPLETE = 3      // Complete indexing
};

// Structure for storing frame information
struct VideoFrameInfo {
    int frame_number;           // Sequential frame number (0, 1, 2...)
    int64_t original_pts;       // Original PTS from file
    int64_t display_pts;        // PTS for display (after B-frame sorting)
    double calculated_time;     // Calculated precise time (in seconds)
    int64_t file_position;      // Packet position in file
    bool is_keyframe;          // Whether it's a key frame
    FrameType frame_type;      // Frame type (I/P/B)
    IndexingState state;       // Indexing state
    int stream_index;          // Video stream index

    VideoFrameInfo() : frame_number(-1), original_pts(AV_NOPTS_VALUE),
                      display_pts(AV_NOPTS_VALUE), calculated_time(-1.0),
                      file_position(-1), is_keyframe(false),
                      frame_type(FRAME_UNKNOWN), state(INDEX_EMPTY),
                      stream_index(-1) {}
};

class FSTPVideoFrameIndex {
public:
    FSTPVideoFrameIndex();
    ~FSTPVideoFrameIndex();

    // === HYBRID INDEXING ===
    // Phase 1: Extracting metadata (fast)
    bool ExtractMetadata(const std::string& video_file_path);

    // Phase 2: Fast indexing with step (for proxy)
    bool BuildQuickIndex(int step_size = 16);

    // Phase 3: Complete indexing in background
    std::future<bool> BuildCompleteIndexAsync();

    // Legacy method for compatibility
    bool BuildIndex(const std::string& video_file_path);
    bool BlockUntilComplete(const std::string& video_file_path);

    // Stop background process
    void StopBackgroundIndexing();

    // Check indexing state
    bool IsMetadataReady() const { return m_metadata_ready; }
    bool IsQuickIndexReady() const { return m_quick_index_ready; }
    bool IsCompleteIndexReady() const { return m_complete_index_ready; }
    double GetIndexingProgress() const { return m_indexing_progress; }

    // Clear index
    void Clear();

    // Get information
    size_t GetTotalFrames() const { return m_estimated_total_frames; }
    double GetDuration() const { return m_duration; }
    double GetFrameRate() const { return m_frame_rate; }

    // === SEARCH AND NAVIGATION ===
    // Find frame by time
    int FindFrameByTime(double time_seconds) const;

    // Find nearest keyframe
    int FindNearestKeyframe(int frame_number, bool search_backward = true) const;

    // Get information about frame
    const VideoFrameInfo* GetFrameInfo(int frame_number) const;

    // Get time of frame
    double GetFrameTime(int frame_number) const;

    // Simple binding: audio frame → video index
    void SetCurrentFrame(int frame_number);
    int GetCurrentFrame() const { return m_current_frame; }

    // === B-FRAME SORTING ===
    // Getting correct display order
    std::vector<int> GetDisplayOrder(int start_frame, int end_frame) const;

    // === DECODING FRAMES ===
    // Decoding frame by number
    std::shared_ptr<AVFrame> DecodeFrame(int frame_number);

    // Decode frame by time
    std::shared_ptr<AVFrame> DecodeFrameByTime(double time_seconds);

    // Check correspondence of frame time
    bool VerifyFrameTime(int frame_number, double expected_time, double tolerance = 0.04); // 1 frame at 25fps

    // === FFPLAY-STYLE FAST NAVIGATION ===
    // Asynchronous positioning (like in ffplay)
    bool SeekAsync(double time_seconds, bool keyframe_only = true);
    bool SeekAsyncByFrame(int frame_number, bool keyframe_only = true);

    // Check state of asynchronous seek
    bool IsSeekInProgress() const { return m_seek_in_progress; }
    double GetSeekTarget() const { return m_seek_target; }

    // Frame buffer (minimum like in ffplay - 3 frames)
    std::shared_ptr<AVFrame> GetBufferedFrame(int frame_number);
    void FlushFrameBuffer(); // Clear when seek

private:
    // === INDEX DATA ===
    std::vector<VideoFrameInfo> m_frame_index;
    std::string m_video_file_path;

    // === METADATA ===
    double m_duration;
    double m_frame_rate;
    AVRational m_time_base;
    size_t m_estimated_total_frames;

    // === INDEXING STATE ===
    std::atomic<bool> m_metadata_ready{false};
    std::atomic<bool> m_quick_index_ready{false};
    std::atomic<bool> m_complete_index_ready{false};
    std::atomic<double> m_indexing_progress{0.0};
    std::atomic<bool> m_should_stop{false};

    // === FFPLAY-STYLE SEEKING ===
    std::atomic<bool> m_seek_in_progress{false};
    std::atomic<double> m_seek_target{-1.0};
    std::atomic<int> m_seek_serial{0}; // For discarding outdated frames

    // === BACKGROUND INDEXING ===
    std::thread m_background_thread;
    mutable std::mutex m_index_mutex;

    // Simple binding: audio → video
    int m_current_frame;

    // === DECODING ===
    AVFormatContext* m_decode_format_ctx;
    AVCodecContext* m_decode_codec_ctx;
    AVStream* m_decode_stream;
    int m_decode_stream_index;
    mutable std::mutex m_decode_mutex;

    // === FFPLAY-STYLE FRAME BUFFER (small buffer for responsiveness) ===
    struct FrameBufferEntry {
        std::shared_ptr<AVFrame> frame;
        int frame_number;
        int serial;
        double timestamp;

        FrameBufferEntry() : frame_number(-1), serial(-1), timestamp(-1.0) {}
    };

    static const int FRAME_BUFFER_SIZE = 3; // Like in ffplay - minimum buffer
    FrameBufferEntry m_frame_buffer[FRAME_BUFFER_SIZE];
    int m_buffer_read_index;
    int m_buffer_write_index;
    int m_buffer_size;
    mutable std::mutex m_buffer_mutex;

    // === INTERNAL METHODS ===
    // Phase 1: Metadata
    bool ExtractBasicInfo(AVFormatContext* format_ctx, int video_stream_index);

    // Phase 2: Fast indexing
    bool BuildQuickIndexInternal(AVFormatContext* format_ctx, int video_stream_index, int step);

    // Phase 3: Complete indexing
    bool BuildCompleteIndexInternal();
    
    // Alternative complete index from scratch (if fast one failed)
    bool BuildFullIndexFromScratch();

    // B-frame sorting
    void SortFramesByDisplayOrder();
    FrameType DetermineFrameType(AVPacket* packet);

    // Service methods
    bool OpenVideoFile(AVFormatContext** format_ctx, int* video_stream_index);
    void CalculateAccurateTiming();
    int CountKeyframes() const;

    // Thread-safe methods
    void SetIndexingProgress(double progress);
    void ResizeIndexSafely(size_t new_size);

    // Decoding
    bool InitializeDecoder();
    void CleanupDecoder();
    std::shared_ptr<AVFrame> DecodeFrameInternal(int frame_number);
    bool SeekToFrame(int frame_number);

    // FFPLAY-style methods
    void InitializeFrameBuffer();
    void AddToFrameBuffer(std::shared_ptr<AVFrame> frame, int frame_number, double timestamp);
    std::shared_ptr<AVFrame> GetFromFrameBuffer(int frame_number);
    void SeekInternal(double time_seconds, bool keyframe_only);
};