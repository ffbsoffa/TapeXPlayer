#include "FSTPSimpleVideoIndex.h"
#include <iostream>
#include <algorithm>
#include <cctype>
#include <iomanip>
#include <cmath>
#include <limits>

extern "C" {
#include <libavformat/version.h>
#include <libavcodec/avcodec.h>
}

// Debug control: set to true to enable verbose logging
static constexpr bool ENABLE_INDEX_DEBUG = false;
static constexpr int kMaxSamplePackets = 20000;
static constexpr int kMaxSampleFrames = 15000;
static constexpr int kMaxIntraSamplePackets = 2000;
static constexpr int kMaxIntraSampleFrames = 1500;

void FSTPSimpleVideoIndex::Clear() {
    m_frames.clear();
    m_duration = 0.0;
    m_frame_rate = 25.0;
    m_time_base = {1, 25};
    m_start_time = 0;
    m_max_gop_size = 0;
    m_intraframe_codec = false;
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
    
    if (ENABLE_INDEX_DEBUG) {
        std::cout << "[SimpleIndex] File opened successfully, video stream: " << *video_stream_idx << std::endl;
    }
    return true;
}

void FSTPSimpleVideoIndex::ExtractMetadata(AVFormatContext* fmt_ctx, int video_stream_idx) {
    AVStream* video_stream = fmt_ctx->streams[video_stream_idx];
    m_time_base = video_stream->time_base;

    // CRITICAL: Get start_time of stream for calculating RELATIVE time
    // Like in old decode.cpp - audio gives relative time, video should too
    m_start_time = (video_stream->start_time != AV_NOPTS_VALUE) ? video_stream->start_time : 0;

    if (ENABLE_INDEX_DEBUG && m_start_time != 0) {
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

        m_intraframe_codec = false;
        switch (codec_params->codec_id) {
            case AV_CODEC_ID_DNXHD:  // includes DNxHR variants
            case AV_CODEC_ID_PRORES:
                m_intraframe_codec = true;
                break;
            default:
                break;
        }

        if (!m_intraframe_codec) {
            std::string codec_name_lower = m_codec_name;
            for (char& ch : codec_name_lower) {
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            }
            if (codec_name_lower.find("dnx") != std::string::npos ||
                codec_name_lower.find("prores") != std::string::npos) {
                m_intraframe_codec = true;
            }
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

    if (ENABLE_INDEX_DEBUG) {
        std::cout << "[SimpleIndex] Metadata: " << m_width << "x" << m_height
                  << ", " << m_frame_rate << " fps, " << m_duration << " seconds"
                  << ", codec: " << m_codec_name << std::endl;
    }
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

    // Fast path: try to build index using stream metadata without full packet scan
    if (BuildIndexFromStreamInfo(fmt_ctx, video_stream_idx)) {
        avformat_close_input(&fmt_ctx);
        return true;
    }

    if (m_intraframe_codec) {
        bool ok = BuildIndexIntraframe(fmt_ctx, video_stream_idx);
        avformat_close_input(&fmt_ctx);
        return ok;
    }
    
    // Reserve space for frames
    size_t estimated_frames = (size_t)(m_duration * m_frame_rate);
    if (estimated_frames > 0) {
        m_frames.reserve(estimated_frames);
    }
    
    if (ENABLE_INDEX_DEBUG) {
        std::cout << "[SimpleIndex] Starting to read packets..." << std::endl;
    }
    
    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        std::cerr << "[SimpleIndex] Failed to allocate packet" << std::endl;
        avformat_close_input(&fmt_ctx);
        return false;
    }
    
    int frame_count = 0;
    int packet_count = 0;
    const double time_base_seconds = av_q2d(m_time_base);
    const double fallback_frame_step = (m_frame_rate > 0.0) ? (1.0 / m_frame_rate) : 0.0;
    double last_frame_time = -std::numeric_limits<double>::infinity();
    bool needs_sort = false;
    constexpr double kTimeEpsilon = 1e-9;
    
    // Read all packets
    const int frame_limit = m_intraframe_codec ? kMaxIntraSampleFrames : kMaxSampleFrames;
    const int packet_limit = m_intraframe_codec ? kMaxIntraSamplePackets : kMaxSamplePackets;

    bool sample_limit_reached = false;
    int ret;
    while ((ret = av_read_frame(fmt_ctx, packet)) >= 0) {
        packet_count++;
        
        if (ENABLE_INDEX_DEBUG && packet_count <= 5) {
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
            bool packet_is_keyframe = (packet->flags & AV_PKT_FLAG_KEY) != 0;
            if (m_intraframe_codec) {
                packet_is_keyframe = true;
            }
            frame_info.is_keyframe = packet_is_keyframe;

            // CRITICAL: Calculate RELATIVE time (like in old decode.cpp)
            // Audio gives relative time from 0, video should match!
            if (packet->pts != AV_NOPTS_VALUE) {
                frame_info.time_seconds = (packet->pts - m_start_time) * time_base_seconds;
            } else if (fallback_frame_step > 0.0) {
                frame_info.time_seconds = frame_count * fallback_frame_step;
            } else {
                frame_info.time_seconds = static_cast<double>(frame_count);
            }

            if (!needs_sort) {
                if (frame_count == 0) {
                    last_frame_time = frame_info.time_seconds;
                } else if (frame_info.time_seconds + kTimeEpsilon < last_frame_time) {
                    needs_sort = true;
                } else {
                    last_frame_time = frame_info.time_seconds;
                }
            }
            
            // BuildIndex() called only when loading file
            // in one thread, when object is not used by other threads
            m_frames.push_back(frame_info);
            
            frame_count++;
            
            if (ENABLE_INDEX_DEBUG && frame_count % 1000 == 0) {
                std::cout << "[SimpleIndex] Processed " << frame_count << " video frames..." << std::endl;
            }

            if (!sample_limit_reached && (frame_count >= frame_limit || packet_count >= packet_limit)) {
                sample_limit_reached = true;
                av_packet_unref(packet);
                break;
            }
        }

        av_packet_unref(packet);

        if (sample_limit_reached) {
            break;
        }
    }

    av_packet_free(&packet);
    if (sample_limit_reached) {
        const double observed_span = (frame_count > 1)
            ? (m_frames.back().time_seconds - m_frames.front().time_seconds)
            : 0.0;
        const double avg_step_from_frames = (frame_count > 1)
            ? observed_span / static_cast<double>(frame_count - 1)
            : 0.0;
        const double fallback_step = (m_frame_rate > 0.0) ? (1.0 / m_frame_rate) : (1.0 / 25.0);
        const double average_step = (avg_step_from_frames > 1e-9) ? avg_step_from_frames : fallback_step;
        const double time_base_seconds = av_q2d(m_time_base);

        int estimated_total_frames = static_cast<int>(std::llround(m_duration * (m_frame_rate > 0.0 ? m_frame_rate : 25.0)));
        if (estimated_total_frames <= frame_count) {
            estimated_total_frames = frame_count;
        }

        std::vector<int> keyframe_indices;
        keyframe_indices.reserve(frame_count);
        for (size_t i = 0; i < m_frames.size(); ++i) {
            if (m_frames[i].is_keyframe) {
                keyframe_indices.push_back(static_cast<int>(i));
            }
        }

        int gop_interval = 0;
        if (m_intraframe_codec) {
            gop_interval = 1;
        } else if (keyframe_indices.size() >= 2) {
            long long interval_sum = 0;
            for (size_t i = 1; i < keyframe_indices.size(); ++i) {
                interval_sum += keyframe_indices[i] - keyframe_indices[i - 1];
            }
            gop_interval = static_cast<int>(std::llround(static_cast<double>(interval_sum) / (keyframe_indices.size() - 1)));
        }
        if (gop_interval <= 0) {
            gop_interval = static_cast<int>(std::max(1.0, m_frame_rate > 0.0 ? std::round(m_frame_rate) : 1.0));
        }

        const size_t original_size = m_frames.size();
        if (estimated_total_frames > frame_count) {
            m_frames.resize(static_cast<size_t>(estimated_total_frames));
        }

        double current_time = (frame_count > 0) ? m_frames.back().time_seconds : 0.0;
        int last_keyframe_idx = !keyframe_indices.empty() ? keyframe_indices.back() : (frame_count > 0 ? frame_count - 1 : 0);

        for (size_t i = original_size; i < m_frames.size(); ++i) {
            current_time += average_step;
            auto& frame = m_frames[i];
            frame.frame_number = static_cast<int>(i);
            frame.time_seconds = std::min(current_time, m_duration);
            frame.file_position = -1;
            frame.pts = m_start_time + static_cast<int64_t>(std::llround(frame.time_seconds / time_base_seconds));

            if (m_intraframe_codec) {
                frame.is_keyframe = true;
                last_keyframe_idx = static_cast<int>(i);
            } else {
                const int delta_from_last_key = static_cast<int>(i) - last_keyframe_idx;
                if (delta_from_last_key >= gop_interval) {
                    frame.is_keyframe = true;
                    last_keyframe_idx = static_cast<int>(i);
                } else {
                    frame.is_keyframe = false;
                }
            }
        }

        frame_count = static_cast<int>(m_frames.size());
    }
    
    avformat_close_input(&fmt_ctx);
    
    // Check result
    if (!sample_limit_reached && ret < 0 && ret != AVERROR_EOF) {
        char error_buf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, error_buf, sizeof(error_buf));
        std::cerr << "[SimpleIndex] Read error: " << error_buf << std::endl;
        return false;
    }
    
    if (ENABLE_INDEX_DEBUG) {
        std::cout << "[SimpleIndex] Completed: " << packet_count << " total packets, " 
                  << frame_count << " video frames indexed" << std::endl;
    }
    
    if (frame_count > 0) {
        if (needs_sort) {
            if (ENABLE_INDEX_DEBUG) {
                std::cout << "[SimpleIndex] Sorting " << m_frames.size() << " frames by presentation time..." << std::endl;
            }

            std::sort(m_frames.begin(), m_frames.end(), [](const SimpleFrameInfo& a, const SimpleFrameInfo& b) {
                return a.time_seconds < b.time_seconds;
            });

            for (size_t i = 0; i < m_frames.size(); ++i) {
                m_frames[i].frame_number = static_cast<int>(i);
            }

            if (ENABLE_INDEX_DEBUG) {
                std::cout << "[SimpleIndex] Frames sorted by presentation time for proper B-frame handling" << std::endl;
            }
        }

        // GOP STRUCTURE DIAGNOSIS: Analyze GOP size to identify problematic files
        int keyframe_count = 0;
        int current_gop_size = 0;
        int min_gop_size = std::numeric_limits<int>::max();
        m_max_gop_size = 0;
        int gop_sample_count = 0;
        double gop_mean = 0.0;
        double gop_m2 = 0.0;

        for (const auto& frame : m_frames) {
            current_gop_size++;
            if (frame.is_keyframe) {
                keyframe_count++;
                if (current_gop_size > 1) {  // Skip first GOP (may be incomplete)
                    const int gop_size = current_gop_size;
                    m_max_gop_size = std::max(m_max_gop_size, gop_size);
                    min_gop_size = std::min(min_gop_size, gop_size);

                    gop_sample_count++;
                    const double delta = gop_size - gop_mean;
                    gop_mean += delta / gop_sample_count;
                    const double delta2 = gop_size - gop_mean;
                    gop_m2 += delta * delta2;
                }
                current_gop_size = 0;
            }
        }

        if (ENABLE_INDEX_DEBUG && gop_sample_count > 0) {
            const double stddev = (gop_sample_count > 0) ? std::sqrt(gop_m2 / gop_sample_count) : 0.0;

            std::cout << "[SimpleIndex] GOP analysis: " << keyframe_count << " keyframes, "
                      << gop_sample_count << " GOPs" << std::endl;
            std::cout << "              GOP size: min=" << min_gop_size
                      << ", max=" << m_max_gop_size
                      << ", avg=" << std::fixed << std::setprecision(1) << gop_mean
                      << ", stddev=" << std::setprecision(1) << stddev << std::endl;

            if (m_max_gop_size > 100) {
                std::cout << "              ⚠️  CRITICAL: Very large GOP detected (" << m_max_gop_size
                          << " frames) - decoder will use adaptive margins" << std::endl;
            } else if (m_max_gop_size > 50) {
                std::cout << "              ⚠️  WARNING: Large GOP detected (" << m_max_gop_size
                          << " frames) - decoder will use extended margins" << std::endl;
            }

            if (stddev > gop_mean * 0.5) {
                std::cout << "              ⚠️  WARNING: Irregular GOP structure detected (high variance)"
                          << " - frame timing may be unpredictable" << std::endl;
            }
        }

        if (ENABLE_INDEX_DEBUG) {
            std::cout << "[SimpleIndex] After sorting: ";
            for (int i = 0; i < std::min(5, static_cast<int>(m_frames.size())); i++) {
                std::cout << std::fixed << std::setprecision(3) << m_frames[i].time_seconds << "s";
                if (i < 4 && i < static_cast<int>(m_frames.size()) - 1) std::cout << " → ";
            }
            std::cout << " (display order)" << std::endl;
        }

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

bool FSTPSimpleVideoIndex::BuildIndexFromStreamInfo(AVFormatContext* fmt_ctx, int video_stream_idx) {
    AVStream* video_stream = fmt_ctx->streams[video_stream_idx];
    if (!video_stream) {
        return false;
    }

    if (m_frame_rate <= 0.0) {
        return false;
    }

    int64_t total_frames = video_stream->nb_frames;
    if (total_frames <= 0) {
        total_frames = static_cast<int64_t>(std::llround(m_duration * m_frame_rate));
    }

    if (total_frames <= 0) {
        return false;
    }

    // Guard against unreasonable estimates (e.g., corrupted metadata)
    constexpr int64_t kMaxReasonableFrames = 10'000'000;
    if (total_frames > kMaxReasonableFrames) {
        return false;
    }

    m_frames.clear();
    m_frames.resize(static_cast<size_t>(total_frames));

    const double time_step = 1.0 / m_frame_rate;
    const double time_base_seconds = av_q2d(m_time_base);
    if (!(time_base_seconds > 0.0)) {
        m_frames.clear();
        return false;
    }

    for (int64_t i = 0; i < total_frames; ++i) {
        auto& frame = m_frames[static_cast<size_t>(i)];
        frame.frame_number = static_cast<int>(i);
        frame.time_seconds = std::min(i * time_step, m_duration);
        frame.file_position = -1;
        frame.is_keyframe = false;
        frame.pts = m_start_time + static_cast<int64_t>(std::llround(frame.time_seconds / time_base_seconds));
    }

    bool has_keyframe_info = false;
#if LIBAVFORMAT_VERSION_MAJOR < 59
    if (video_stream->index_entries && video_stream->nb_index_entries > 0) {
        has_keyframe_info = true;
        const AVIndexEntry* entries = video_stream->index_entries;
        const int entry_count = video_stream->nb_index_entries;

        for (int i = 0; i < entry_count; ++i) {
            const int64_t timestamp = entries[i].timestamp;
            const double relative_seconds = (timestamp - m_start_time) * time_base_seconds;
            int frame_idx = static_cast<int>(std::llround(relative_seconds * m_frame_rate));
            frame_idx = std::clamp(frame_idx, 0, static_cast<int>(total_frames) - 1);
            m_frames[static_cast<size_t>(frame_idx)].is_keyframe = true;
        }
    }
#else
    const int entry_count = avformat_index_get_entries_count(video_stream);
    if (entry_count > 0) {
        has_keyframe_info = true;
        for (int i = 0; i < entry_count; ++i) {
            const AVIndexEntry* entry = avformat_index_get_entry(video_stream, i);
            if (!entry) {
                continue;
            }

            const int64_t timestamp = entry->timestamp;
            const double relative_seconds = (timestamp - m_start_time) * time_base_seconds;
            int frame_idx = static_cast<int>(std::llround(relative_seconds * m_frame_rate));
            frame_idx = std::clamp(frame_idx, 0, static_cast<int>(total_frames) - 1);
            m_frames[static_cast<size_t>(frame_idx)].is_keyframe = true;
        }
    }
#endif

    if (!has_keyframe_info) {
        const AVCodecParameters* codecpar = video_stream->codecpar;
        bool assume_all_i_frames = false;
        if (codecpar) {
            switch (codecpar->codec_id) {
                case AV_CODEC_ID_DNXHD:      // includes DNxHR family
                case AV_CODEC_ID_PRORES:
                    assume_all_i_frames = true;
                    break;
                default:
                    break;
            }
        }

        if (assume_all_i_frames) {
            for (auto& frame : m_frames) {
                frame.is_keyframe = true;
            }
            has_keyframe_info = true;
        } else {
            m_frames.clear();
            return false;
        }
    }

    if (!m_frames.empty() && !m_frames.front().is_keyframe) {
        m_frames.front().is_keyframe = true;
    }

    m_max_gop_size = 0;
    int current_gop = 0;
    for (const auto& frame : m_frames) {
        current_gop++;
        if (frame.is_keyframe) {
            if (current_gop > 1) {
                m_max_gop_size = std::max(m_max_gop_size, current_gop);
            }
            current_gop = 0;
        }
    }
    if (current_gop > 0) {
        m_max_gop_size = std::max(m_max_gop_size, current_gop);
    }

    if (ENABLE_INDEX_DEBUG) {
        std::cout << "[SimpleIndex] Fast index build succeeded using stream metadata: "
                  << m_frames.size() << " frames" << std::endl;
    }

    m_ready = true;
    return true;
}

bool FSTPSimpleVideoIndex::BuildIndexIntraframe(AVFormatContext* fmt_ctx, int video_stream_idx) {
    const double time_base_seconds = av_q2d(m_time_base);
    double fps = (m_frame_rate > 0.0) ? m_frame_rate : 25.0;
    if (fps <= 0.0) {
        fps = 25.0;
    }

    int64_t estimated_frames = static_cast<int64_t>(std::llround(m_duration * fps));
    if (estimated_frames <= 0) {
        estimated_frames = static_cast<int64_t>(fps * std::max(1.0, m_duration));
        if (estimated_frames <= 0) {
            estimated_frames = static_cast<int64_t>(fps * 60.0); // fallback to 1 minute
        }
    }
    const int64_t kMaxFrames = 2'000'000;
    estimated_frames = std::clamp<int64_t>(estimated_frames, 1, kMaxFrames);

    m_frames.clear();
    m_frames.resize(static_cast<size_t>(estimated_frames));

    const double time_step = 1.0 / fps;
    for (size_t i = 0; i < m_frames.size(); ++i) {
        auto& frame = m_frames[i];
        frame.frame_number = static_cast<int>(i);
        frame.time_seconds = std::min(i * time_step, m_duration);
        frame.file_position = -1;
        frame.is_keyframe = true;
        frame.pts = m_start_time + static_cast<int64_t>(std::llround(frame.time_seconds / time_base_seconds));
    }

    int sample_count = static_cast<int>(std::min<size_t>(m_frames.size(), 256));
    if (sample_count <= 0) {
        sample_count = 1;
    }

    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        return false;
    }

    for (int s = 0; s < sample_count; ++s) {
        double ratio = (sample_count > 1) ? static_cast<double>(s) / static_cast<double>(sample_count - 1) : 0.0;
        size_t frame_idx = std::min<size_t>(m_frames.size() - 1, static_cast<size_t>(std::llround(ratio * (m_frames.size() - 1))));
        double sample_time = m_frames[frame_idx].time_seconds;
        int64_t target_pts = m_start_time + static_cast<int64_t>(std::llround(sample_time / time_base_seconds));

        if (av_seek_frame(fmt_ctx, video_stream_idx, target_pts, AVSEEK_FLAG_BACKWARD) < 0) {
            continue;
        }

        int tries = 0;
        while (tries < 50 && av_read_frame(fmt_ctx, packet) >= 0) {
            ++tries;
            if (packet->stream_index != video_stream_idx) {
                av_packet_unref(packet);
                continue;
            }

            double actual_time = sample_time;
            if (packet->pts != AV_NOPTS_VALUE) {
                actual_time = (packet->pts - m_start_time) * time_base_seconds;
            } else if (packet->dts != AV_NOPTS_VALUE) {
                actual_time = (packet->dts - m_start_time) * time_base_seconds;
            }

            actual_time = std::clamp(actual_time, 0.0, (m_duration > 0.0) ? m_duration : actual_time);

            m_frames[frame_idx].time_seconds = actual_time;
            m_frames[frame_idx].pts = packet->pts;
            m_frames[frame_idx].file_position = packet->pos;
            m_frames[frame_idx].is_keyframe = true;

            break;
        }

        av_packet_unref(packet);
    }

    av_packet_free(&packet);

    double last_time = 0.0;
    for (size_t i = 0; i < m_frames.size(); ++i) {
        auto& frame = m_frames[i];
        if (i == 0) {
            last_time = frame.time_seconds;
        } else {
            if (frame.time_seconds + 1e-9 < last_time) {
                frame.time_seconds = last_time;
            } else {
                last_time = frame.time_seconds;
            }
        }
    }

    m_max_gop_size = 1;
    m_ready = true;
    return true;
}
