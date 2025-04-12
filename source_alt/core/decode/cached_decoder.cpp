#include "cached_decoder.h"
#include "decode.h"
#include <iostream>
#include <thread>
#include <algorithm> // For std::max, std::min

// --- Constructor and Destructor --- 
CachedDecoder::CachedDecoder(const std::string& filename, std::vector<FrameInfo>& frameIndex) :
    sourceFilename_(filename),
    frameIndex_(frameIndex),
    formatCtx_(nullptr),
    codecCtx_(nullptr),
    codecParams_(nullptr),
    videoStream_(nullptr),
    videoStreamIndex_(-1),
    timeBase_({0, 1}), // Initialize to avoid potential issues
    fps_(0.0),
    adaptedStep_(10), // Default value
    initialized_(false)
{
    std::cout << "CachedDecoder created for: " << sourceFilename_ << std::endl;
    initialized_ = initialize(); // Call initialize on construction
}

CachedDecoder::~CachedDecoder() {
    std::cout << "CachedDecoder destroyed for: " << sourceFilename_ << std::endl;
    cleanup(); // Call cleanup on destruction
}

// --- Private Initialization and Cleanup (Similar to LowResDecoder) ---

bool CachedDecoder::initialize() {
    if (initialized_) return true; // Already initialized

    // Open input file
    if (avformat_open_input(&formatCtx_, sourceFilename_.c_str(), nullptr, nullptr) != 0) {
        std::cerr << "CachedDecoder Error: Failed to open file " << sourceFilename_ << std::endl;
        cleanup();
        return false;
    }

    // Find stream info
    if (avformat_find_stream_info(formatCtx_, nullptr) < 0) {
        std::cerr << "CachedDecoder Error: Failed to find stream information for " << sourceFilename_ << std::endl;
        cleanup();
        return false;
    }

    // Find video stream
    const AVCodec* codec = nullptr; 
    videoStreamIndex_ = av_find_best_stream(formatCtx_, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (videoStreamIndex_ < 0 || !codec) {
        std::cerr << "CachedDecoder Error: Video stream not found or codec could not be found in " << sourceFilename_ << std::endl;
        cleanup();
        return false;
    }
    videoStream_ = formatCtx_->streams[videoStreamIndex_];
    codecParams_ = videoStream_->codecpar; // Store codec parameters
    timeBase_ = videoStream_->time_base; // Store time base

    // Calculate FPS
    if (videoStream_->avg_frame_rate.den != 0) {
        fps_ = av_q2d(videoStream_->avg_frame_rate);
    } else if (videoStream_->r_frame_rate.den != 0) {
        fps_ = av_q2d(videoStream_->r_frame_rate);
    } else {
        fps_ = 25.0; // Fallback FPS
        std::cerr << "CachedDecoder Warning: Could not determine FPS, using default: " << fps_ << std::endl;
    }
    
    // Get adaptive step based on FPS
    adaptedStep_ = getAdaptiveStep(fps_); 

    // Allocate codec context
    codecCtx_ = avcodec_alloc_context3(codec);
    if (!codecCtx_) {
        std::cerr << "CachedDecoder Error: Failed to allocate codec context" << std::endl;
        cleanup();
        return false;
    }

    // Copy codec parameters to context
    if (avcodec_parameters_to_context(codecCtx_, codecParams_) < 0) {
        std::cerr << "CachedDecoder Error: Failed to copy codec parameters to context" << std::endl;
        cleanup();
        return false;
    }
    
    // Enable multi-threading hint (optional)
    codecCtx_->thread_count = std::thread::hardware_concurrency();
    codecCtx_->thread_type = FF_THREAD_FRAME;

    // Open codec
    if (avcodec_open2(codecCtx_, codec, nullptr) < 0) {
        std::cerr << "CachedDecoder Error: Failed to open codec" << std::endl;
        cleanup();
        return false;
    }

    std::cout << "CachedDecoder initialized successfully for " << sourceFilename_ << std::endl;
    std::cout << "  Resolution: " << codecCtx_->width << "x" << codecCtx_->height << std::endl;
    std::cout << "  FPS: " << fps_ << ", Adapted Step: " << adaptedStep_ << std::endl;
    std::cout << "  Time Base: " << timeBase_.num << "/" << timeBase_.den << std::endl;
    
    initialized_ = true;
    return true;
}

void CachedDecoder::cleanup() {
    // Similar to LowResDecoder cleanup
    if (codecCtx_) {
        avcodec_free_context(&codecCtx_);
        codecCtx_ = nullptr;
    }
    codecParams_ = nullptr; // Points into formatCtx_
    if (formatCtx_) {
        avformat_close_input(&formatCtx_);
        formatCtx_ = nullptr;
    }
    videoStream_ = nullptr; // Belongs to formatCtx_
    videoStreamIndex_ = -1;
    initialized_ = false;
    fps_ = 0.0;
    adaptedStep_ = 10;
    timeBase_ = {0, 1};
    // frameIndex_ is a reference, not owned by this class
    std::cout << "CachedDecoder cleaned up." << std::endl;
}

// --- Static Methods --- 

int CachedDecoder::getAdaptiveStep(double fps) {
    int adaptiveStep = 10;  // Значение по умолчанию
    
    if (fps > 0) {
        // Устанавливаем шаг в зависимости от FPS
        if (fps >= 59.0 && fps <= 60.0) {
            adaptiveStep = 12; // Для 60 fps (и 59.94)
        } else if (fps >= 49.0 && fps <= 50.0) {
            adaptiveStep = 10; // Для 50 fps
        } else if (fps >= 29.0 && fps <= 30.0) {
            adaptiveStep = 6;  // Для 30 fps (и 29.97)
        } else if (fps >= 24.0 && fps <= 25.0) {
            adaptiveStep = 5;  // Для 25 fps и 24 fps
        } else if (fps >= 23.0 && fps < 24.0) {
            adaptiveStep = 4;  // Для 23.976 fps
        } else {
            // Для других значений FPS используем формулу
            adaptiveStep = static_cast<int>(fps / 5.0);
            adaptiveStep = std::max(3, adaptiveStep); // Минимум 3 кадра
            adaptiveStep = std::min(15, adaptiveStep); // Максимум 15 кадров
        }
        
        std::cout << "Video FPS: " << fps << ", adapted cache step: " << adaptiveStep << std::endl;
    }
    
    return adaptiveStep;
}

// --- Instance Methods ---

bool CachedDecoder::decodeRange(int startFrame, int endFrame) {
    if (!initialized_) {
        std::cerr << "CachedDecoder::decodeRange Error: Not initialized." << std::endl;
        return false;
    }
    if (frameIndex_.empty() || startFrame > endFrame || startFrame >= frameIndex_.size()) {
         std::cerr << "CachedDecoder::decodeRange Error: Invalid range or empty index." << std::endl;
        return false; // Invalid range or index
    }
    
    // Clamp endFrame to valid index range
    endFrame = std::min(endFrame, static_cast<int>(frameIndex_.size()) - 1);

    // --- Seeking --- 
    // Get the target timestamp for seeking (use time_ms from the startFrame)
    int64_t seekTargetTimeMs = frameIndex_[startFrame].time_ms;
    if (seekTargetTimeMs < 0) {
        std::cerr << "CachedDecoder Warning: Invalid timestamp for seek target frame " << startFrame << ". Seeking near beginning." << std::endl;
        // Fallback: Seek near the beginning or use PTS if time_ms is invalid?
        // For simplicity, let's try seeking to a very small timestamp
        seekTargetTimeMs = 0; 
    }

    int64_t seekTargetPts = av_rescale_q(seekTargetTimeMs, {1, 1000}, timeBase_); 
    // Seek slightly before the target frame to ensure we get it
    seekTargetPts -= av_rescale_q(1000, {1, 1000}, timeBase_); // Seek 1 sec before? Or based on step?
    seekTargetPts = std::max((int64_t)0, seekTargetPts); // Don't seek before 0

    // Use AVSEEK_FLAG_BACKWARD to find the nearest keyframe before or at the target PTS
    int seek_ret = av_seek_frame(formatCtx_, videoStreamIndex_, seekTargetPts, AVSEEK_FLAG_BACKWARD);
    if (seek_ret < 0) {
        std::cerr << "CachedDecoder Warning: Seek to pts " << seekTargetPts << " (ms ~" << seekTargetTimeMs << ") failed: " << av_err2str(seek_ret) << std::endl;
        // Consider trying to seek to beginning if initial seek fails?
        // av_seek_frame(formatCtx_, videoStreamIndex_, 0, AVSEEK_FLAG_BYTE); 
    } else {
        avcodec_flush_buffers(codecCtx_); // Flush decoder after successful seek
        // std::cout << "CachedDecoder Seek successful to ~" << seekTargetTimeMs << " ms" << std::endl;
    }

    // --- Decoding Loop --- 
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    if (!packet || !frame) { 
        std::cerr << "CachedDecoder Error: Failed to allocate packet/frame." << std::endl;
        if(packet) av_packet_free(&packet);
        if(frame) av_frame_free(&frame);
        return false;
    }

    int decodedFrameCountInLoop = 0; // Counter for frames decoded in this loop iteration
    bool firstFrameDecoded = false;
    int currentFrameIndex = -1; // Track the estimated index based on PTS

    while (av_read_frame(formatCtx_, packet) >= 0) {
        if (packet->stream_index == videoStreamIndex_) {
            int ret = avcodec_send_packet(codecCtx_, packet);
            if (ret < 0) {
                if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) { 
                     std::cerr << "CachedDecoder Error sending packet: " << av_err2str(ret) << std::endl;
                }
                av_packet_unref(packet);
                continue;
            }

            while (ret >= 0) {
                ret = avcodec_receive_frame(codecCtx_, frame);
                if (ret == AVERROR(EAGAIN)) {
                    break; // Need more packets
                } else if (ret == AVERROR_EOF) {
                    goto decode_loop_end; // End of stream
                } else if (ret < 0) {
                    std::cerr << "CachedDecoder Error receiving frame: " << av_err2str(ret) << std::endl;
                    break; // Error receiving frame
                }

                // --- Frame Identification and Storage --- 
                int64_t framePts = frame->best_effort_timestamp;
                if (framePts == AV_NOPTS_VALUE) framePts = frame->pts;

                // Estimate current frame index based on PTS
                // This requires frameIndex_ to have valid time_ms
                if (framePts != AV_NOPTS_VALUE) {
                    int64_t frameTimeMs = av_rescale_q(framePts - videoStream_->start_time, timeBase_, {1, 1000});
                    
                    // Find the closest frame index (simple linear search for now)
                    // TODO: Optimize this search if needed (e.g., binary search if frameIndex is sorted by time_ms)
                    int bestMatchIndex = -1;
                    int64_t minDiff = INT64_MAX;
                    int searchStart = std::max(0, startFrame - adaptedStep_ * 2); // Start search a bit before target
                    int searchEnd = std::min(static_cast<int>(frameIndex_.size()) - 1, endFrame + adaptedStep_);

                    for (int idx = searchStart; idx <= searchEnd; ++idx) {
                         if (frameIndex_[idx].time_ms >= 0) { // Check for valid time_ms
                              int64_t diff = std::abs(frameIndex_[idx].time_ms - frameTimeMs);
                              if (diff < minDiff) {
                                   minDiff = diff;
                                   bestMatchIndex = idx;
                              } 
                              // Optimization: If we passed the target time, maybe stop early?
                              // if (frameIndex_[idx].time_ms > frameTimeMs + 50) break; 
                         }
                    }
                    currentFrameIndex = bestMatchIndex;
                }

                // Check if the identified frame index is within the target range and is a cache point
                if (currentFrameIndex != -1 &&
                    currentFrameIndex >= startFrame &&
                    currentFrameIndex <= endFrame &&
                    currentFrameIndex % adaptedStep_ == 0)
                {
                    // Lock the specific frame info
                    std::lock_guard<std::mutex> lock(frameIndex_[currentFrameIndex].mutex);

                    // Store if not already cached
                    // --- ВОССТАНАВЛИВАЕМ ХРАНЕНИЕ КАДРА ---
                    if (!frameIndex_[currentFrameIndex].cached_frame) { // Проверяем, что его еще нет
                        frameIndex_[currentFrameIndex].cached_frame = std::shared_ptr<AVFrame>(av_frame_clone(frame), [](AVFrame* f){ av_frame_free(&f); });
                        if (!frameIndex_[currentFrameIndex].cached_frame) {
                             std::cerr << "CachedDecoder Error: Failed to clone frame for index " << currentFrameIndex << std::endl;
                        } else {
                            // Update frame info (optional)
                            frameIndex_[currentFrameIndex].pts = framePts;
                            frameIndex_[currentFrameIndex].relative_pts = framePts - videoStream_->start_time;
                            frameIndex_[currentFrameIndex].time_ms = av_rescale_q(frameIndex_[currentFrameIndex].relative_pts, timeBase_, {1, 1000});
                            frameIndex_[currentFrameIndex].time_base = timeBase_;

                            // Set type if empty
                            if (frameIndex_[currentFrameIndex].type == FrameInfo::EMPTY) {
                                frameIndex_[currentFrameIndex].type = FrameInfo::CACHED;
                            }
                             // std::cout << "Cached frame at index " << currentFrameIndex << std::endl; // Отладочный вывод
                        }
                    }
                    // --- КОНЕЦ ВОССТАНОВЛЕНИЯ ---
                }

                av_frame_unref(frame); // Unref frame inside receive loop

                // Check if we have processed beyond the end frame for this range
                if (currentFrameIndex > endFrame) {
                    // std::cout << "CachedDecoder: Reached end of range (" << currentFrameIndex << " > " << endFrame << ")." << std::endl;
                    goto decode_loop_end;
                }
            } // end while receive_frame
        } // end if packet is video stream
        av_packet_unref(packet); // Unref packet outside receive loop
    } // end while av_read_frame

decode_loop_end:
    av_frame_free(&frame);
    av_packet_free(&packet);
    // Don't close formatCtx_ or codecCtx_ here, they are managed by the class lifecycle
    return true; // Indicate completion (might need better error handling)
}

// Removed static decodeCachedFrames and asyncDecodeCachedFrames

// --- Non-Static Methods (if any) --- 