#include "FSTPVideoFrameIndex.h"
#include <iostream>
#include <algorithm>
#include <cmath>

FSTPVideoFrameIndex::FSTPVideoFrameIndex()
    : m_duration(0.0), m_frame_rate(0.0), m_estimated_total_frames(0), m_current_frame(0),
      m_decode_format_ctx(nullptr), m_decode_codec_ctx(nullptr), m_decode_stream(nullptr), m_decode_stream_index(-1) {
    m_time_base = {0, 1};
}

FSTPVideoFrameIndex::~FSTPVideoFrameIndex() {
    StopBackgroundIndexing();
    CleanupDecoder();
    Clear();
}

void FSTPVideoFrameIndex::Clear() {
    StopBackgroundIndexing();

    std::lock_guard<std::mutex> lock(m_index_mutex);
    m_frame_index.clear();
    m_video_file_path.clear();
    m_duration = 0.0;
    m_frame_rate = 0.0;
    m_estimated_total_frames = 0;
    m_current_frame = 0;
    m_time_base = {0, 1};

    // Reset indexing state
    m_metadata_ready = false;
    m_quick_index_ready = false;
    m_complete_index_ready = false;
    m_indexing_progress = 0.0;
}

// === HYBRID INDEXING ===

// Phase 1: Extract metadata (fast)
bool FSTPVideoFrameIndex::ExtractMetadata(const std::string& video_file_path) {
    Clear();
    m_video_file_path = video_file_path;

    AVFormatContext* format_ctx = nullptr;
    int video_stream_index = -1;

    if (!OpenVideoFile(&format_ctx, &video_stream_index)) {
        return false;
    }

    bool success = ExtractBasicInfo(format_ctx, video_stream_index);
    avformat_close_input(&format_ctx);

    if (success) {
        m_metadata_ready = true;
        std::cout << "Metadata extracted successfully:" << std::endl;
        std::cout << "Estimated frames: " << m_estimated_total_frames << std::endl;
        std::cout << "Duration: " << m_duration << " seconds" << std::endl;
        std::cout << "Frame rate: " << m_frame_rate << " fps" << std::endl;
    }

    return success;
}

// Phase 2: Fast indexing with step (for proxy)
bool FSTPVideoFrameIndex::BuildQuickIndex(int step_size) {
    if (!m_metadata_ready) {
        std::cerr << "Metadata must be extracted first" << std::endl;
        return false;
    }

    AVFormatContext* format_ctx = nullptr;
    int video_stream_index = -1;

    if (!OpenVideoFile(&format_ctx, &video_stream_index)) {
        return false;
    }

    bool success = BuildQuickIndexInternal(format_ctx, video_stream_index, step_size);
    avformat_close_input(&format_ctx);

    if (success) {
        m_quick_index_ready = true;
        std::cout << "Quick index built with step " << step_size << std::endl;
        std::cout << "Indexed frames: " << m_frame_index.size() << std::endl;
    }

    return success;
}

// Phase 3: Complete indexing in background
std::future<bool> FSTPVideoFrameIndex::BuildCompleteIndexAsync() {
    if (!m_quick_index_ready) {
        std::cerr << "Quick index must be built first" << std::endl;
        return std::future<bool>();
    }

    m_should_stop = false;

    return std::async(std::launch::async, [this]() {
        return BuildCompleteIndexInternal();
    });
}

// Legacy method for compatibility
bool FSTPVideoFrameIndex::BuildIndex(const std::string& video_file_path) {
    if (!ExtractMetadata(video_file_path)) {
        return false;
    }

    if (!BuildQuickIndex(16) || m_frame_index.empty()) {
        std::cerr << "[VideoFrameIndex] Quick index failed or empty (block). Building full index on source." << std::endl;
        m_video_file_path = video_file_path;
        return BuildCompleteIndexInternal();
    }

    auto future = BuildCompleteIndexAsync();
    return future.valid();
}

bool FSTPVideoFrameIndex::BlockUntilComplete(const std::string& video_file_path) {
    if (!ExtractMetadata(video_file_path)) {
        return false;
    }

    if (!BuildQuickIndex(16) || m_frame_index.empty()) {
        std::cerr << "[VideoFrameIndex] Quick index failed or empty during block. Building full index synchronously." << std::endl;
        return BuildFullIndexFromScratch();
    }

    return BuildCompleteIndexInternal();
}

void FSTPVideoFrameIndex::StopBackgroundIndexing() {
    m_should_stop = true;
    if (m_background_thread.joinable()) {
        m_background_thread.join();
    }
}

// === INTERNAL METHODS ===

bool FSTPVideoFrameIndex::OpenVideoFile(AVFormatContext** format_ctx, int* video_stream_index) {
    int ret = avformat_open_input(format_ctx, m_video_file_path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        std::cerr << "Failed to open video file: " << m_video_file_path << std::endl;
        return false;
    }

    ret = avformat_find_stream_info(*format_ctx, nullptr);
    if (ret < 0) {
        std::cerr << "Failed to find stream info" << std::endl;
        avformat_close_input(format_ctx);
        return false;
    }
    
    // Seek to beginning after stream info analysis
    av_seek_frame(*format_ctx, -1, 0, AVSEEK_FLAG_BACKWARD);

    // Find video stream
    *video_stream_index = -1;
    for (unsigned int i = 0; i < (*format_ctx)->nb_streams; i++) {
        if ((*format_ctx)->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            *video_stream_index = i;
            break;
        }
    }

    if (*video_stream_index == -1) {
        std::cerr << "No video stream found" << std::endl;
        avformat_close_input(format_ctx);
        return false;
    }

    return true;
}

bool FSTPVideoFrameIndex::ExtractBasicInfo(AVFormatContext* format_ctx, int video_stream_index) {
    AVStream* video_stream = format_ctx->streams[video_stream_index];
    m_time_base = video_stream->time_base;

    // Get information about frame rate
    AVRational frame_rate = av_guess_frame_rate(format_ctx, video_stream, nullptr);

    // DIAGNOSTICS: print all available FPS sources
    std::cout << "📊 [FPS DETECTION] av_guess_frame_rate: " << frame_rate.num << "/" << frame_rate.den
              << " = " << (frame_rate.den > 0 ? (double)frame_rate.num / frame_rate.den : 0.0) << " fps" << std::endl;
    std::cout << "📊 [FPS DETECTION] stream->avg_frame_rate: " << video_stream->avg_frame_rate.num << "/" << video_stream->avg_frame_rate.den
              << " = " << (video_stream->avg_frame_rate.den > 0 ? (double)video_stream->avg_frame_rate.num / video_stream->avg_frame_rate.den : 0.0) << " fps" << std::endl;
    std::cout << "📊 [FPS DETECTION] stream->r_frame_rate: " << video_stream->r_frame_rate.num << "/" << video_stream->r_frame_rate.den
              << " = " << (video_stream->r_frame_rate.den > 0 ? (double)video_stream->r_frame_rate.num / video_stream->r_frame_rate.den : 0.0) << " fps" << std::endl;

    if (frame_rate.num > 0 && frame_rate.den > 0) {
        m_frame_rate = av_q2d(frame_rate);
        std::cout << "✅ [FPS DETECTION] Using av_guess_frame_rate: " << m_frame_rate << " fps" << std::endl;
    } else {
        m_frame_rate = 25.0; // Fallback
        std::cout << "⚠️  [FPS DETECTION] Using fallback FPS: " << m_frame_rate << " fps" << std::endl;
    }

    // Estimate total duration and number of frames
    if (format_ctx->duration != AV_NOPTS_VALUE) {
        m_duration = format_ctx->duration / (double)AV_TIME_BASE;
        m_estimated_total_frames = (size_t)(m_duration * m_frame_rate);
    } else if (video_stream->duration != AV_NOPTS_VALUE) {
        m_duration = video_stream->duration * av_q2d(video_stream->time_base);
        m_estimated_total_frames = (size_t)(m_duration * m_frame_rate);
    } else {
        // Fallback: try to estimate by file size and bitrate
        m_duration = 0.0;
        m_estimated_total_frames = 0;
    }

    std::cout << "Extracted metadata:" << std::endl;
    std::cout << "Frame rate: " << m_frame_rate << " fps" << std::endl;
    std::cout << "Time base: " << m_time_base.num << "/" << m_time_base.den << std::endl;

    return true;
}

bool FSTPVideoFrameIndex::BuildQuickIndexInternal(AVFormatContext* format_ctx, int video_stream_index, int step) {
    std::cout << "Building quick index with step " << step << "..." << std::endl;
    std::cout << "[QuickIndex] Format context: " << (format_ctx ? "valid" : "null") << std::endl;
    std::cout << "[QuickIndex] Video stream index: " << video_stream_index << std::endl;
    std::cout << "[QuickIndex] Number of streams: " << (format_ctx ? format_ctx->nb_streams : 0) << std::endl;
    std::cout << "[QuickIndex] Estimated frames: " << m_estimated_total_frames << std::endl;

    if (!format_ctx) {
        std::cerr << "[QuickIndex] Format context is null!" << std::endl;
        return false;
    }

    if (video_stream_index < 0 || video_stream_index >= static_cast<int>(format_ctx->nb_streams)) {
        std::cerr << "[QuickIndex] Invalid video stream index: " << video_stream_index << std::endl;
        return false;
    }

    // Try to seek to beginning - DISABLED for testing
    // int seek_result = av_seek_frame(format_ctx, -1, 0, AVSEEK_FLAG_BACKWARD);
    // std::cout << "[QuickIndex] Seek to beginning result: " << seek_result << std::endl;
    std::cout << "[QuickIndex] Skipping seek - reading from current position" << std::endl;

    if (m_estimated_total_frames == 0) {
        ResizeIndexSafely(step * 1024); // initial reserve
    } else {
        ResizeIndexSafely(m_estimated_total_frames);
    }

    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        std::cerr << "Failed to allocate packet" << std::endl;
        return false;
    }

    int frame_count = 0;
    int indexed_frames = 0;
    int total_packets = 0;

    std::cout << "[QuickIndex] Starting packet reading loop..." << std::endl;

    int read_result;
    while ((read_result = av_read_frame(format_ctx, packet)) >= 0 && !m_should_stop) {
        total_packets++;
        if (total_packets <= 20) { // Log first 20 packets for debugging
            std::cout << "[QuickIndex] Packet " << total_packets << ": stream=" << packet->stream_index 
                      << ", pts=" << packet->pts << ", size=" << packet->size << std::endl;
        }
        
        if (packet->stream_index == video_stream_index) {
            std::cout << "[QuickIndex] Packet frame=" << frame_count
                      << " stream=" << packet->stream_index
                      << " pts=" << packet->pts
                      << " key=" << ((packet->flags & AV_PKT_FLAG_KEY) != 0)
                      << std::endl;

            {
                std::lock_guard<std::mutex> lock(m_index_mutex);
                if (frame_count >= static_cast<int>(m_frame_index.size())) {
                    m_frame_index.emplace_back();
                }
                VideoFrameInfo& frame_info = m_frame_index[frame_count];
                frame_info.frame_number = frame_count;
                frame_info.original_pts = packet->pts;
                frame_info.file_position = packet->pos;
                frame_info.is_keyframe = (packet->flags & AV_PKT_FLAG_KEY) != 0;
                frame_info.frame_type = DetermineFrameType(packet);
                frame_info.stream_index = video_stream_index;

                if (frame_count % step == 0 || frame_info.is_keyframe) {
                    frame_info.state = INDEX_QUICK;
                } else {
                    frame_info.state = INDEX_METADATA;
                }

                if (packet->pts != AV_NOPTS_VALUE) {
                    frame_info.calculated_time = packet->pts * av_q2d(m_time_base);
                } else {
                    frame_info.calculated_time = frame_count / m_frame_rate;
                }

                indexed_frames++;
                std::cout << "[QuickIndex] Added frame_info for frame " << frame_count
                          << " state=" << static_cast<int>(frame_info.state)
                          << " calc_time=" << frame_info.calculated_time
                          << std::endl;
            }

            frame_count++;

            if (frame_count % 5000 == 0) {
                SetIndexingProgress((double)frame_count / std::max<size_t>(m_estimated_total_frames, 1) * 0.5);
                std::cout << "Quick indexed: " << indexed_frames << " / " << frame_count << " frames..." << std::endl;
            }
        } else {
            std::cout << "[QuickIndex] Skipping packet stream=" << packet->stream_index << std::endl;
        }
        av_packet_unref(packet);
    }

    if (total_packets == 0 && read_result < 0) {
        char error_buf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(read_result, error_buf, sizeof(error_buf));
        std::cout << "[QuickIndex] av_read_frame failed with code " << read_result << ": " << error_buf << std::endl;
    } else if (total_packets == 0) {
        std::cout << "[QuickIndex] No packets read (reached end of file immediately)" << std::endl;
    }
    
    std::cout << "[QuickIndex] Total packets read: " << total_packets << std::endl;
    std::cout << "[QuickIndex] Video frames processed: " << frame_count << std::endl;

    av_packet_free(&packet);

    {
        std::lock_guard<std::mutex> lock(m_index_mutex);
        if (frame_count < static_cast<int>(m_frame_index.size())) {
            m_frame_index.resize(frame_count);
        }
        m_estimated_total_frames = frame_count;
        std::cout << "[QuickIndex] Resized frame index to " << m_frame_index.size() << std::endl;
    }

    std::cout << "Quick index complete: " << indexed_frames << " total frames indexed" << std::endl;
    std::cout << "Frame index size: " << m_frame_index.size() << ", estimated frames: " << m_estimated_total_frames << std::endl;
    return true;
}

bool FSTPVideoFrameIndex::BuildCompleteIndexInternal() {
    if (!m_quick_index_ready) {
        return false;
    }

    // Skip complete indexing if quick index already contains all frames
    if (m_frame_index.size() == m_estimated_total_frames && m_estimated_total_frames > 0) {
        std::cout << "Quick index is already complete, marking as complete index" << std::endl;

        // Update state of all frames to INDEX_COMPLETE
        {
            std::lock_guard<std::mutex> lock(m_index_mutex);
            for (auto& frame : m_frame_index) {
                frame.state = INDEX_COMPLETE;
            }
        }

        m_complete_index_ready = true;
        SetIndexingProgress(1.0);
        return true;
    }

    AVFormatContext* format_ctx = nullptr;
    int video_stream_index = -1;

    if (!OpenVideoFile(&format_ctx, &video_stream_index)) {
        return false;
    }

    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        std::cerr << "Failed to allocate packet" << std::endl;
        avformat_close_input(&format_ctx);
        return false;
    }

    int frame_count = 0;
    std::cout << "Building complete index..." << std::endl;

    while (av_read_frame(format_ctx, packet) >= 0 && !m_should_stop) {
        if (packet->stream_index == video_stream_index) {
            std::cout << "[CompleteIndex] Packet frame=" << frame_count
                      << " stream=" << packet->stream_index
                      << " pts=" << packet->pts
                      << " key=" << ((packet->flags & AV_PKT_FLAG_KEY) != 0)
                      << std::endl;

            {
                std::lock_guard<std::mutex> lock(m_index_mutex);
                if (frame_count < (int)m_frame_index.size()) {
                    VideoFrameInfo& frame_info = m_frame_index[frame_count];

                    // Update frame if it is not indexed or partially indexed
                    if (frame_info.state < INDEX_COMPLETE) {
                        frame_info.frame_number = frame_count;
                        frame_info.original_pts = packet->pts;
                        frame_info.file_position = packet->pos;
                        frame_info.is_keyframe = (packet->flags & AV_PKT_FLAG_KEY) != 0;
                        frame_info.frame_type = DetermineFrameType(packet);
                        frame_info.stream_index = video_stream_index;
                        frame_info.state = INDEX_COMPLETE;

                        // Exact time
                        if (packet->pts != AV_NOPTS_VALUE) {
                            frame_info.calculated_time = packet->pts * av_q2d(m_time_base);
                        } else {
                            frame_info.calculated_time = frame_count / m_frame_rate;
                        }
                    }
                }
            }

            frame_count++;

            if (frame_count % 2000 == 0) {
                SetIndexingProgress(0.5 + (double)frame_count / m_estimated_total_frames * 0.5); // 50-100%
                std::cout << "Complete indexed: " << frame_count << " frames..." << std::endl;
            }
        }
        av_packet_unref(packet);
    }

    av_packet_free(&packet);
    avformat_close_input(&format_ctx);

    if (!m_should_stop) {
        // Sort B-frames by correct display order
        SortFramesByDisplayOrder();

        m_complete_index_ready = true;
        SetIndexingProgress(1.0);
        std::cout << "Complete index finished: " << frame_count << " total frames" << std::endl;
    }

    return !m_should_stop;
}


int FSTPVideoFrameIndex::CountKeyframes() const {
    int count = 0;
    for (const auto& frame : m_frame_index) {
        if (frame.is_keyframe) count++;
    }
    return count;
}

int FSTPVideoFrameIndex::FindFrameByTime(double time_seconds) const {
    std::lock_guard<std::mutex> lock(m_index_mutex);

    std::cout << "FindFrameByTime: time=" << time_seconds << ", index_size=" << m_frame_index.size() << ", duration=" << m_duration << std::endl;

    if (m_frame_index.empty() || time_seconds < 0.0) {
        std::cout << "FindFrameByTime: empty index or negative time" << std::endl;
        return -1;
    }

    if (time_seconds >= m_duration) {
        return m_frame_index.size() - 1;
    }

    // FIXED: Use logic as in old code - find last frame where time <= target
    // This solves the problem of "dance" frames and synchronization with OSD
    int bestMatchIndex = 0;
    double maxTimeLessOrEqual = -1.0;

    for (size_t i = 0; i < m_frame_index.size(); i++) {
        double frame_time = m_frame_index[i].calculated_time;

        // Find last frame where time <= target_time (as in old findClosestFrameIndexByTime)
        if (frame_time <= time_seconds) {
            if (frame_time >= maxTimeLessOrEqual) {
                maxTimeLessOrEqual = frame_time;
                bestMatchIndex = i;
            }
        } else {
            // If frames are in order and we exceeded target_time, we can stop
            break;
        }
    }

    std::cout << "FindFrameByTime: target=" << time_seconds << "s → frame " << bestMatchIndex
              << " (time=" << maxTimeLessOrEqual << "s)" << std::endl;

    return bestMatchIndex;
}

int FSTPVideoFrameIndex::FindNearestKeyframe(int frame_number, bool search_backward) const {
    std::lock_guard<std::mutex> lock(m_index_mutex);

    if (frame_number < 0 || frame_number >= (int)m_frame_index.size()) {
        return -1;
    }

    if (search_backward) {
        // Search backward
        for (int i = frame_number; i >= 0; i--) {
            if (m_frame_index[i].is_keyframe) {
                return i;
            }
        }
    } else {
        // Search forward
        for (int i = frame_number; i < (int)m_frame_index.size(); i++) {
            if (m_frame_index[i].is_keyframe) {
                return i;
            }
        }
    }

    return -1;
}

const VideoFrameInfo* FSTPVideoFrameIndex::GetFrameInfo(int frame_number) const {
    std::lock_guard<std::mutex> lock(m_index_mutex);

    if (frame_number < 0 || frame_number >= (int)m_frame_index.size()) {
        return nullptr;
    }
    return &m_frame_index[frame_number];
}

double FSTPVideoFrameIndex::GetFrameTime(int frame_number) const {
    const VideoFrameInfo* info = GetFrameInfo(frame_number);
    return info ? info->calculated_time : 0.0;
}

// Simple audio frame → video index binding
void FSTPVideoFrameIndex::SetCurrentFrame(int frame_number) {
    m_current_frame = frame_number;
}

// === NEW METHODS ===

FrameType FSTPVideoFrameIndex::DetermineFrameType(AVPacket* packet) {
    // Simple determination of frame type by packet flags
    if (packet->flags & AV_PKT_FLAG_KEY) {
        return FRAME_I;
    }

    // For more accurate determination, we need to analyze NAL units (H.264/H.265)
    // For now, we use simple heuristics
    return FRAME_UNKNOWN;
}

void FSTPVideoFrameIndex::SortFramesByDisplayOrder() {
    std::lock_guard<std::mutex> lock(m_index_mutex);

    // For correct sorting of B-frames, we need to analyze PTS and DTS
    // For now, we leave the base implementation, which copies original_pts to display_pts
    for (auto& frame : m_frame_index) {
        frame.display_pts = frame.original_pts;
    }

    std::cout << "B-frame sorting completed" << std::endl;
}

std::vector<int> FSTPVideoFrameIndex::GetDisplayOrder(int start_frame, int end_frame) const {
    std::vector<int> order;
    std::lock_guard<std::mutex> lock(m_index_mutex);

    for (int i = start_frame; i <= end_frame && i < (int)m_frame_index.size(); i++) {
        order.push_back(i);
    }

    // TODO: Real sorting by display_pts for B-frames
    return order;
}

void FSTPVideoFrameIndex::SetIndexingProgress(double progress) {
    m_indexing_progress = progress;
}

void FSTPVideoFrameIndex::ResizeIndexSafely(size_t new_size) {
    std::lock_guard<std::mutex> lock(m_index_mutex);
    m_frame_index.resize(new_size);
}

void FSTPVideoFrameIndex::CalculateAccurateTiming() {
    std::lock_guard<std::mutex> lock(m_index_mutex);

    // New implementation of exact time calculation based on PTS
    if (m_frame_index.empty()) return;

    int64_t first_pts = AV_NOPTS_VALUE;
    int64_t last_pts = AV_NOPTS_VALUE;

    // Find first and last valid PTS
    for (const auto& frame : m_frame_index) {
        if (frame.original_pts != AV_NOPTS_VALUE) {
            if (first_pts == AV_NOPTS_VALUE) {
                first_pts = frame.original_pts;
            }
            last_pts = frame.original_pts;
        }
    }

    if (first_pts != AV_NOPTS_VALUE && last_pts != AV_NOPTS_VALUE) {
        double pts_duration = (last_pts - first_pts) * av_q2d(m_time_base);
        double real_frame_rate = (m_frame_index.size() - 1) / pts_duration;

        // Correct frame rate if needed
        if (std::abs(real_frame_rate - m_frame_rate) > 0.5) {
            m_frame_rate = real_frame_rate;
        }

        m_duration = pts_duration;
    }
}

// === DECODING FRAMES ===

std::shared_ptr<AVFrame> FSTPVideoFrameIndex::DecodeFrame(int frame_number) {
    if (!InitializeDecoder()) {
        return nullptr;
    }

    return DecodeFrameInternal(frame_number);
}

std::shared_ptr<AVFrame> FSTPVideoFrameIndex::DecodeFrameByTime(double time_seconds) {
    int frame_number = FindFrameByTime(time_seconds);
    if (frame_number < 0) {
        return nullptr;
    }

    return DecodeFrame(frame_number);
}

bool FSTPVideoFrameIndex::VerifyFrameTime(int frame_number, double expected_time, double tolerance) {
    const VideoFrameInfo* frame_info = GetFrameInfo(frame_number);
    if (!frame_info) {
        return false;
    }

    double actual_time = frame_info->calculated_time;
    double diff = std::abs(actual_time - expected_time);

    std::cout << "Frame #" << frame_number << ": expected time=" << expected_time
              << "sec, actual=" << actual_time << "sec, difference=" << diff << "sec" << std::endl;

    return diff <= tolerance;
}

bool FSTPVideoFrameIndex::InitializeDecoder() {
    std::lock_guard<std::mutex> lock(m_decode_mutex);

    if (m_decode_codec_ctx) {
        return true; // Already initialized
    }

    // Open file for decoding
    int ret = avformat_open_input(&m_decode_format_ctx, m_video_file_path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        std::cerr << "Failed to open video file for decoding: " << m_video_file_path << std::endl;
        return false;
    }

    ret = avformat_find_stream_info(m_decode_format_ctx, nullptr);
    if (ret < 0) {
        std::cerr << "Failed to find stream info for decoding" << std::endl;
        CleanupDecoder();
        return false;
    }

    // Find video stream
    for (unsigned int i = 0; i < m_decode_format_ctx->nb_streams; i++) {
        if (m_decode_format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            m_decode_stream_index = i;
            m_decode_stream = m_decode_format_ctx->streams[i];
            break;
        }
    }

    if (m_decode_stream_index == -1) {
        std::cerr << "No video stream found for decoding" << std::endl;
        CleanupDecoder();
        return false;
    }

    // Find codec
    const AVCodec* codec = avcodec_find_decoder(m_decode_stream->codecpar->codec_id);
    if (!codec) {
        std::cerr << "Decoder not found" << std::endl;
        CleanupDecoder();
        return false;
    }

    // Create codec context
    m_decode_codec_ctx = avcodec_alloc_context3(codec);
    if (!m_decode_codec_ctx) {
        std::cerr << "Failed to allocate codec context" << std::endl;
        CleanupDecoder();
        return false;
    }

    // Copy parameters
    ret = avcodec_parameters_to_context(m_decode_codec_ctx, m_decode_stream->codecpar);
    if (ret < 0) {
        std::cerr << "Failed to copy codec parameters" << std::endl;
        CleanupDecoder();
        return false;
    }

    // Open codec
    ret = avcodec_open2(m_decode_codec_ctx, codec, nullptr);
    if (ret < 0) {
        std::cerr << "Failed to open codec" << std::endl;
        CleanupDecoder();
        return false;
    }

    std::cout << "Decoder initialized successfully" << std::endl;
    return true;
}

void FSTPVideoFrameIndex::CleanupDecoder() {
    if (m_decode_codec_ctx) {
        avcodec_free_context(&m_decode_codec_ctx);
        m_decode_codec_ctx = nullptr;
    }

    if (m_decode_format_ctx) {
        avformat_close_input(&m_decode_format_ctx);
        m_decode_format_ctx = nullptr;
    }

    m_decode_stream = nullptr;
    m_decode_stream_index = -1;
}

bool FSTPVideoFrameIndex::SeekToFrame(int frame_number) {
    const VideoFrameInfo* frame_info = GetFrameInfo(frame_number);
    if (!frame_info) {
        return false;
    }

    // Find nearest keyframe
    int keyframe = FindNearestKeyframe(frame_number, true);
    if (keyframe < 0) {
        keyframe = 0; // Start from the beginning if keyframe not found
    }

    const VideoFrameInfo* keyframe_info = GetFrameInfo(keyframe);
    if (!keyframe_info) {
        return false;
    }

    // Seek to keyframe
    int64_t timestamp = keyframe_info->original_pts;
    if (timestamp == AV_NOPTS_VALUE) {
        timestamp = (int64_t)(keyframe_info->calculated_time / av_q2d(m_time_base));
    }

    int ret = av_seek_frame(m_decode_format_ctx, m_decode_stream_index, timestamp, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        std::cerr << "Failed to seek to timestamp " << timestamp << std::endl;
        return false;
    }

    // Clear decoder buffers
    avcodec_flush_buffers(m_decode_codec_ctx);

    return true;
}

std::shared_ptr<AVFrame> FSTPVideoFrameIndex::DecodeFrameInternal(int frame_number) {
    std::lock_guard<std::mutex> lock(m_decode_mutex);

    // Check validity of frame number
    if (frame_number < 0 || frame_number >= (int)m_estimated_total_frames) {
        std::cerr << "Invalid frame number: " << frame_number << " (total: " << m_estimated_total_frames << ")" << std::endl;
        return nullptr;
    }

    if (!SeekToFrame(frame_number)) {
        return nullptr;
    }

    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        return nullptr;
    }

    std::shared_ptr<AVFrame> target_frame = nullptr;
    int current_frame = FindNearestKeyframe(frame_number, true);
    if (current_frame < 0) current_frame = 0;

    while (av_read_frame(m_decode_format_ctx, packet) >= 0) {
        if (packet->stream_index == m_decode_stream_index) {
            // Send packet to decoder
            int ret = avcodec_send_packet(m_decode_codec_ctx, packet);
            if (ret < 0) {
                av_packet_unref(packet);
                continue;
            }

            // Get frame
            AVFrame* frame = av_frame_alloc();
            while (avcodec_receive_frame(m_decode_codec_ctx, frame) == 0) {
                if (current_frame == frame_number) {
                    // Found the desired frame!
                    target_frame = std::shared_ptr<AVFrame>(frame, [](AVFrame* f) { av_frame_free(&f); });
                    av_packet_unref(packet);
                    av_packet_free(&packet);
                    return target_frame;
                }
                current_frame++;
                av_frame_unref(frame);
            }
            av_frame_free(&frame);
        }
        av_packet_unref(packet);

    // Protection against infinite loop and attempt to reach the end of the file
        if (current_frame > frame_number + 50) {
            break;
        }
    }

    // Attempt to get remaining frames from decoder (for last frames)
    if (current_frame <= frame_number + 10) {
        // Send NULL packet for flush
        avcodec_send_packet(m_decode_codec_ctx, nullptr);

        AVFrame* frame = av_frame_alloc();
        while (avcodec_receive_frame(m_decode_codec_ctx, frame) == 0) {
            if (current_frame == frame_number) {
                target_frame = std::shared_ptr<AVFrame>(frame, [](AVFrame* f) { av_frame_free(&f); });
                av_packet_free(&packet);
                return target_frame;
            }
            current_frame++;
            av_frame_unref(frame);
        }
        av_frame_free(&frame);
    }

    av_packet_free(&packet);
    std::cerr << "Failed to decode frame " << frame_number << " (reached frame " << current_frame << ")" << std::endl;
    return nullptr;
}

// Full index from scratch when quick index fails
bool FSTPVideoFrameIndex::BuildFullIndexFromScratch() {
    std::cout << "Building full index from scratch..." << std::endl;
    
    AVFormatContext* format_ctx = nullptr;
    int video_stream_index = -1;

    if (!OpenVideoFile(&format_ctx, &video_stream_index)) {
        std::cerr << "[FullIndex] Failed to open video file" << std::endl;
        return false;
    }

    std::cout << "[FullIndex] Format context: valid" << std::endl;
    std::cout << "[FullIndex] Video stream index: " << video_stream_index << std::endl;
    std::cout << "[FullIndex] Number of streams: " << format_ctx->nb_streams << std::endl;
    
    // Force seek to the beginning
    int seek_result = av_seek_frame(format_ctx, -1, 0, AVSEEK_FLAG_BACKWARD);
    std::cout << "[FullIndex] Force seek to beginning result: " << seek_result << std::endl;

    // Clear and prepare index
    {
        std::lock_guard<std::mutex> lock(m_index_mutex);
        m_frame_index.clear();
        if (m_estimated_total_frames > 0) {
            m_frame_index.reserve(m_estimated_total_frames);
        }
    }

    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        std::cerr << "[FullIndex] Failed to allocate packet" << std::endl;
        avformat_close_input(&format_ctx);
        return false;
    }

    int frame_count = 0;
    int total_packets = 0;
    int read_result;

    std::cout << "[FullIndex] Starting packet reading..." << std::endl;

    while ((read_result = av_read_frame(format_ctx, packet)) >= 0 && !m_should_stop) {
        total_packets++;
        
        if (total_packets <= 10) {
            std::cout << "[FullIndex] Packet " << total_packets << ": stream=" << packet->stream_index 
                      << ", pts=" << packet->pts << ", size=" << packet->size << std::endl;
        }

        if (packet->stream_index == video_stream_index) {
            VideoFrameInfo frame_info;
            frame_info.frame_number = frame_count;
            frame_info.original_pts = packet->pts;
            frame_info.file_position = packet->pos;
            frame_info.is_keyframe = (packet->flags & AV_PKT_FLAG_KEY) != 0;
            frame_info.frame_type = DetermineFrameType(packet);
            frame_info.stream_index = video_stream_index;
            frame_info.state = INDEX_COMPLETE;

            if (packet->pts != AV_NOPTS_VALUE) {
                frame_info.calculated_time = packet->pts * av_q2d(m_time_base);
            } else {
                frame_info.calculated_time = frame_count / m_frame_rate;
            }

            {
                std::lock_guard<std::mutex> lock(m_index_mutex);
                m_frame_index.push_back(frame_info);
            }

            frame_count++;

            if (frame_count % 1000 == 0) {
                std::cout << "[FullIndex] Processed " << frame_count << " frames..." << std::endl;
                SetIndexingProgress((double)frame_count / std::max<size_t>(m_estimated_total_frames, 1));
            }
        }
        
        av_packet_unref(packet);
    }

    if (total_packets == 0 && read_result < 0) {
        char error_buf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(read_result, error_buf, sizeof(error_buf));
        std::cout << "[FullIndex] av_read_frame failed with code " << read_result << ": " << error_buf << std::endl;
    } else if (total_packets == 0) {
        std::cout << "[FullIndex] No packets read (reached end of file immediately)" << std::endl;
    }

    av_packet_free(&packet);
    avformat_close_input(&format_ctx);

    std::cout << "[FullIndex] Total packets: " << total_packets << ", video frames: " << frame_count << std::endl;

    if (frame_count > 0) {
        std::lock_guard<std::mutex> lock(m_index_mutex);
        m_estimated_total_frames = frame_count;
        m_quick_index_ready = true;  // Mark as ready since we have a working index
        m_complete_index_ready = true;
        SetIndexingProgress(1.0);
        std::cout << "[FullIndex] Successfully indexed " << frame_count << " frames" << std::endl;
        return true;
    }

    std::cerr << "[FullIndex] No frames indexed" << std::endl;
    return false;
}