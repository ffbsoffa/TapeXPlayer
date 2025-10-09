#include "FSTPSimpleVideoIndex.h"
#include <iostream>
#include <algorithm>
#include <iomanip>
#include <cmath>

// Debug control: set to true to enable verbose logging
static constexpr bool ENABLE_INDEX_DEBUG = false;

void FSTPSimpleVideoIndex::Clear() {
    m_frames.clear();
    m_duration = 0.0;
    m_frame_rate = 25.0;
    m_time_base = {1, 25};
    m_start_time = 0;
    m_max_gop_size = 0;
    m_ready = false;
}

bool FSTPSimpleVideoIndex::OpenFile(AVFormatContext** fmt_ctx, int* video_stream_idx) {
    *fmt_ctx = nullptr;
    *video_stream_idx = -1;
    
    // Open file
    int ret = avformat_open_input(fmt_ctx, m_file_path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        char error_buf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, error_buf, sizeof(error_buf));
        // std::cerr << "[SimpleIndex] Failed to open file: " << error_buf << std::endl;
        return false;
    }
    
    // Analyze streams
    ret = avformat_find_stream_info(*fmt_ctx, nullptr);
    if (ret < 0) {
        char error_buf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, error_buf, sizeof(error_buf));
        std::cerr << "[SimpleIndex] Failed to find stream info: " << error_buf << std::endl;
        avformat_close_input(fmt_ctx);
        return false;
    }
    
    // Find video stream
    for (unsigned int i = 0; i < (*fmt_ctx)->nb_streams; i++) {
        if ((*fmt_ctx)->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            *video_stream_idx = i;
            break;
        }
    }
    
    if (*video_stream_idx == -1) {
        std::cerr << "[SimpleIndex] No video stream found" << std::endl;
        avformat_close_input(fmt_ctx);
        return false;
    }
    
    std::cout << "[SimpleIndex] File opened successfully, video stream: " << *video_stream_idx << std::endl;
    return true;
}

void FSTPSimpleVideoIndex::ExtractMetadata(AVFormatContext* fmt_ctx, int video_stream_idx) {
    AVStream* video_stream = fmt_ctx->streams[video_stream_idx];
    m_time_base = video_stream->time_base;

    // CRITICAL: Get start_time of stream for calculating RELATIVE time
    // Like in old decode.cpp - audio gives relative time, video should too
    m_start_time = (video_stream->start_time != AV_NOPTS_VALUE) ? video_stream->start_time : 0;

    if (m_start_time != 0) {
        double start_time_sec = m_start_time * av_q2d(m_time_base);
        std::cout << "[SimpleIndex] Video stream start_time: " << m_start_time
                  << " (" << start_time_sec << " seconds) - will use RELATIVE timestamps" << std::endl;
    }

    // Get frame rate
    AVRational frame_rate = av_guess_frame_rate(fmt_ctx, video_stream, nullptr);
    if (frame_rate.num > 0 && frame_rate.den > 0) {
        m_frame_rate = av_q2d(frame_rate);
    } else {
        m_frame_rate = 25.0; // fallback
    }
    
    // Get video size and codec info
    AVCodecParameters* codec_params = video_stream->codecpar;
    if (codec_params) {
        m_width = codec_params->width;
        m_height = codec_params->height;

        // Get codec name
        const AVCodec* codec = avcodec_find_decoder(codec_params->codec_id);
        if (codec && codec->long_name) {
            m_codec_name = codec->long_name;
        } else if (codec && codec->name) {
            m_codec_name = codec->name;
        } else {
            m_codec_name = "Unknown";
        }
    }

    // Get duration
    if (fmt_ctx->duration != AV_NOPTS_VALUE) {
        m_duration = fmt_ctx->duration / (double)AV_TIME_BASE;
    } else if (video_stream->duration != AV_NOPTS_VALUE) {
        m_duration = video_stream->duration * av_q2d(video_stream->time_base);
    } else {
        m_duration = 0.0;
    }

    std::cout << "[SimpleIndex] Metadata: " << m_width << "x" << m_height
              << ", " << m_frame_rate << " fps, " << m_duration << " seconds"
              << ", codec: " << m_codec_name << std::endl;
}

bool FSTPSimpleVideoIndex::BuildIndex(const std::string& video_file) {
    Clear();
    m_file_path = video_file;
    
    AVFormatContext* fmt_ctx = nullptr;
    int video_stream_idx = -1;
    
    if (!OpenFile(&fmt_ctx, &video_stream_idx)) {
        return false;
    }
    
    ExtractMetadata(fmt_ctx, video_stream_idx);
    
    // Reserve space for frames
    size_t estimated_frames = (size_t)(m_duration * m_frame_rate);
    if (estimated_frames > 0) {
        m_frames.reserve(estimated_frames);
    }
    
    std::cout << "[SimpleIndex] Starting to read packets..." << std::endl;
    
    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        std::cerr << "[SimpleIndex] Failed to allocate packet" << std::endl;
        avformat_close_input(&fmt_ctx);
        return false;
    }
    
    int frame_count = 0;
    int packet_count = 0;
    
    // Read all packets
    int ret;
    while ((ret = av_read_frame(fmt_ctx, packet)) >= 0) {
        packet_count++;
        
        if (packet_count <= 5) {
            std::cout << "[SimpleIndex] Packet " << packet_count 
                      << ": stream=" << packet->stream_index 
                      << ", pts=" << packet->pts 
                      << ", size=" << packet->size << std::endl;
        }
        
        // Process only video packets
        if (packet->stream_index == video_stream_idx) {
            SimpleFrameInfo frame_info;
            frame_info.frame_number = frame_count;
            frame_info.pts = packet->pts;
            frame_info.file_position = packet->pos;
            frame_info.is_keyframe = (packet->flags & AV_PKT_FLAG_KEY) != 0;

            // CRITICAL: Calculate RELATIVE time (like in old decode.cpp)
            // Audio gives relative time from 0, video should match!
            if (packet->pts != AV_NOPTS_VALUE) {
                // Convert to microseconds for accuracy (like in old code)
                int64_t pts_us = av_rescale_q(packet->pts, m_time_base, {1, 1000000});
                int64_t start_us = av_rescale_q(m_start_time, m_time_base, {1, 1000000});
                int64_t relative_us = pts_us - start_us;  // RELATIVE time!

                // Convert to seconds with full precision
                frame_info.time_seconds = relative_us / 1000000.0;
            } else {
                frame_info.time_seconds = frame_count / m_frame_rate;
            }
            
            // BuildIndex() called only when loading file
            // in one thread, when object is not used by other threads
            m_frames.push_back(frame_info);
            
            frame_count++;
            
            if (frame_count % 1000 == 0) {
                std::cout << "[SimpleIndex] Processed " << frame_count << " video frames..." << std::endl;
            }
        }
        
        av_packet_unref(packet);
    }
    
    av_packet_free(&packet);
    avformat_close_input(&fmt_ctx);
    
    // Check result
    if (ret < 0 && ret != AVERROR_EOF) {
        char error_buf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, error_buf, sizeof(error_buf));
        std::cerr << "[SimpleIndex] Read error: " << error_buf << std::endl;
        return false;
    }
    
    std::cout << "[SimpleIndex] Completed: " << packet_count << " total packets, " 
              << frame_count << " video frames indexed" << std::endl;
    
    if (frame_count > 0) {
        // Sort frames by presentation time (like old decode.cpp)
        std::cout << "[SimpleIndex] Sorting " << m_frames.size() << " frames by presentation time..." << std::endl;

        // BuildIndex() works in one thread when loading file
        std::sort(m_frames.begin(), m_frames.end(), [](const SimpleFrameInfo& a, const SimpleFrameInfo& b) {
            return a.time_seconds < b.time_seconds;
        });

        // Update frame numbers after sorting
        for (int i = 0; i < static_cast<int>(m_frames.size()); ++i) {
            m_frames[i].frame_number = i;
        }

        std::cout << "[SimpleIndex] Frames sorted by presentation time for proper B-frame handling" << std::endl;

        // GOP STRUCTURE DIAGNOSIS: Analyze GOP size to identify problematic files
        int keyframe_count = 0;
        m_max_gop_size = 0;  // Save in class member for use by decoder
        int current_gop_size = 0;
        std::vector<int> gop_sizes;

        for (size_t i = 0; i < m_frames.size(); ++i) {
            current_gop_size++;
            if (m_frames[i].is_keyframe) {
                keyframe_count++;
                if (current_gop_size > 1) {  // Skip first GOP (may be incomplete)
                    gop_sizes.push_back(current_gop_size);
                    m_max_gop_size = std::max(m_max_gop_size, current_gop_size);
                }
                current_gop_size = 0;
            }
        }

        // Calculate GOP statistics to identify irregularity
        if (!gop_sizes.empty()) {
            int min_gop = *std::min_element(gop_sizes.begin(), gop_sizes.end());
            int total_gop = 0;
            for (int size : gop_sizes) total_gop += size;
            double avg_gop = static_cast<double>(total_gop) / gop_sizes.size();

            // Calculate standard deviation to determine "irregularity"
            double variance = 0.0;
            for (int size : gop_sizes) {
                variance += (size - avg_gop) * (size - avg_gop);
            }
            double stddev = std::sqrt(variance / gop_sizes.size());

            std::cout << "[SimpleIndex] GOP analysis: " << keyframe_count << " keyframes, "
                      << gop_sizes.size() << " GOPs" << std::endl;
            std::cout << "              GOP size: min=" << min_gop
                      << ", max=" << m_max_gop_size
                      << ", avg=" << std::fixed << std::setprecision(1) << avg_gop
                      << ", stddev=" << std::setprecision(1) << stddev << std::endl;

            // WARNING about problematic GOP structures
            if (m_max_gop_size > 100) {
                std::cout << "              ⚠️  CRITICAL: Very large GOP detected (" << m_max_gop_size
                          << " frames) - decoder will use adaptive margins" << std::endl;
            } else if (m_max_gop_size > 50) {
                std::cout << "              ⚠️  WARNING: Large GOP detected (" << m_max_gop_size
                          << " frames) - decoder will use extended margins" << std::endl;
            }

            // Irregularity: stddev > 50% from average
            if (stddev > avg_gop * 0.5) {
                std::cout << "              ⚠️  WARNING: Irregular GOP structure detected (high variance)"
                          << " - frame timing may be unpredictable" << std::endl;
            }
        }

        // Log first few frames after sorting
        std::cout << "[SimpleIndex] After sorting: ";
        for (int i = 0; i < std::min(5, static_cast<int>(m_frames.size())); i++) {
            std::cout << std::fixed << std::setprecision(3) << m_frames[i].time_seconds << "s";
            if (i < 4 && i < static_cast<int>(m_frames.size()) - 1) std::cout << " → ";
        }
        std::cout << " (display order)" << std::endl;

        m_ready = true;
        return true;
    }
    
    std::cerr << "[SimpleIndex] No video frames found" << std::endl;
    return false;
}

const SimpleFrameInfo* FSTPSimpleVideoIndex::GetFrameInfo(int frame_number) const {
    // NO LOCK: after m_ready=true vector is read-only, safe to read
    if (frame_number >= 0 && frame_number < static_cast<int>(m_frames.size())) {
        return &m_frames[frame_number];
    }
    return nullptr;
}

int FSTPSimpleVideoIndex::FindFrameByTime(double time_seconds) const {
    // NO LOCK: after m_ready=true vector is read-only, safe to read
    if (m_frames.empty()) {
        return 0; // Return first frame if index empty
    }

    // Handle time before first frame
    if (time_seconds <= m_frames[0].time_seconds) {
        return 0;
    }

    // Handle time after last frame
    if (time_seconds >= m_frames.back().time_seconds) {
        return static_cast<int>(m_frames.size()) - 1;
    }

    // SIMPLE LOGIC like in old TapeXPlayer (decode.cpp):
    // Find last frame where time <= target (NO bias, NO hysteresis)
    // This worked stable last year - return to the proven approach
    int bestMatchIndex = 0;
    double maxTimeLessOrEqual = -1.0;

    // Linear search - find last frame with time <= target
    for (int i = 0; i < static_cast<int>(m_frames.size()); ++i) {
        double current_time = m_frames[i].time_seconds;

        if (current_time <= time_seconds) {
            // Found frame where time <= target, update if it's later than previous
            if (current_time >= maxTimeLessOrEqual) {
                maxTimeLessOrEqual = current_time;
                bestMatchIndex = i;
            }
        } else {
            // Optimization: frames are sorted, if we passed target - can stop
            break;
        }
    }

    // Debug logging only if enabled
    if (ENABLE_INDEX_DEBUG) {
        static int log_counter = 0;
        log_counter++;
        bool should_log = (log_counter <= 10) || (log_counter % 100 == 0) ||
                          (bestMatchIndex == 0 && time_seconds > 10.0);

        if (should_log) {
            std::cout << "[SimpleIndex] FindFrameByTime: target=" << std::fixed << std::setprecision(3)
                      << time_seconds << "s, found frame " << bestMatchIndex
                      << " (time=" << m_frames[bestMatchIndex].time_seconds << "s)";

            if (bestMatchIndex == 0 && time_seconds > 10.0) {
                std::cout << " ⚠️  SUSPICIOUS: Large time but returning frame 0!";
                std::cout << "\n[DEBUG] First: " << m_frames[0].time_seconds << "s";
                if (m_frames.size() > 1) {
                    std::cout << ", Second: " << m_frames[1].time_seconds << "s";
                }
                if (m_frames.size() > 2) {
                    std::cout << ", Last: " << m_frames.back().time_seconds << "s";
                }
            }
            std::cout << std::endl;
        }
    }

    return bestMatchIndex;
}

void FSTPSimpleVideoIndex::UpdateFrameTime(int frame_number, double time_seconds) {
    // NO LOCK: this method is not used yet, but if it's used - data is atomic
    if (frame_number >= 0 && frame_number < static_cast<int>(m_frames.size())) {
        // Update frame time with data from decoder for precise synchronization
        m_frames[frame_number].time_seconds = time_seconds;
    }
}
