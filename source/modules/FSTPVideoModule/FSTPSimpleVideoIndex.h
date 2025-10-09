#pragma once

#include <vector>
#include <string>
#include <memory>
#include <atomic>
#include <mutex>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/rational.h>
}

// Simple frame structure
struct SimpleFrameInfo {
    int64_t pts = AV_NOPTS_VALUE;
    int64_t file_position = -1;
    double time_seconds = 0.0;
    bool is_keyframe = false;
    int frame_number = 0;
};

// Simple video indexer without complications
class FSTPSimpleVideoIndex {
public:
    FSTPSimpleVideoIndex() = default;
    ~FSTPSimpleVideoIndex() = default;

    // Main methods
    bool BuildIndex(const std::string& video_file);
    void Clear();
    
    // Information retrieval
    size_t GetTotalFrames() const { return m_frames.size(); }
    double GetDuration() const { return m_duration; }
    double GetFrameRate() const { return m_frame_rate; }
    int GetWidth() const { return m_width; }
    int GetHeight() const { return m_height; }
    std::string GetCodecName() const { return m_codec_name; }
    int GetMaxGopSize() const { return m_max_gop_size; }  // For decoder adaptation to irregular GOPs
    
    // Navigation
    const SimpleFrameInfo* GetFrameInfo(int frame_number) const;
    int FindFrameByTime(double time_seconds) const;

    // Time synchronization with decoded frames
    void UpdateFrameTime(int frame_number, double time_seconds);

    // Readiness check
    bool IsReady() const { return m_ready; }

private:
    std::vector<SimpleFrameInfo> m_frames;
    std::string m_file_path;
    double m_duration = 0.0;
    double m_frame_rate = 25.0;
    int m_width = 0;
    int m_height = 0;
    std::string m_codec_name;
    AVRational m_time_base = {1, 25};
    int64_t m_start_time = 0;  // Start time thread for calculating relative time
    int m_max_gop_size = 0;    // Maximum GOP size for decoder adaptation
    std::atomic<bool> m_ready{false};
    // Mutex removed: BuildIndex works in one thread, after m_ready=true data is read-only
    
    // Helper methods
    bool OpenFile(AVFormatContext** fmt_ctx, int* video_stream_idx);
    void ExtractMetadata(AVFormatContext* fmt_ctx, int video_stream_idx);
};
