#include "cached_decoder.h"
#include "decode.h"
#include <iostream>
#include <thread>
#include <algorithm> // For std::max, std::min
#include <functional> // Ensure std::function is included (needed for callbacks later implicitly)

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
    
    // Enable multi-threading hint for software decoding
    codecCtx_->thread_count = std::thread::hardware_concurrency();
    codecCtx_->thread_type = FF_THREAD_FRAME;

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
    } else {
        avcodec_flush_buffers(codecCtx_); // Flush decoder after successful seek
        // std::cout << "CachedDecoder Seek successful to ~" << seekTargetTimeMs << " ms" << std::endl;

        // --- "Warm-up" decoder: Decode and discard a couple of frames after seek --- 
        int warmup_frames_to_discard = 4; // Increased from 2 to potentially avoid initial green frames
        AVFrame* warmup_frame = av_frame_alloc();
        AVPacket* warmup_packet = av_packet_alloc(); // Need a packet for warm-up too
        if (warmup_frame && warmup_packet) {
            for (int i = 0; i < warmup_frames_to_discard; ++i) {
                if (av_read_frame(formatCtx_, warmup_packet) < 0) break; // EOF or error
                if (warmup_packet->stream_index == videoStreamIndex_) {
                    if (avcodec_send_packet(codecCtx_, warmup_packet) >= 0) {
                        if (avcodec_receive_frame(codecCtx_, warmup_frame) >= 0) {
                            // Successfully received a warmup frame, do nothing with it
                            av_frame_unref(warmup_frame);
                        } // else: EAGAIN or error, will be handled by main loop or next warmup iter
                    }
                }
                av_packet_unref(warmup_packet); // Unref packet in warmup loop
            }
        }
        if(warmup_frame) av_frame_free(&warmup_frame);
        if(warmup_packet) av_packet_free(&warmup_packet);
        // --- End Warm-up ---
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

    int decodedFramesSinceFirstStore = 0; // Счетчик для шага
    bool firstFrameStoredInRange = false;  // Флаг для первого кадра
    int firstStoredFrameIndex = -1;       // Индекс первого сохраненного кадра в этом вызове

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

                // --- Check for decode errors in the frame itself ---
                if (frame->decode_error_flags) {
                    std::cerr << "CachedDecoder Warning: Frame (PTS: " << frame->pts << ") has decode_error_flags: " << frame->decode_error_flags << ". Skipping." << std::endl;
                    av_frame_unref(frame); // Unref problematic frame
                    continue; // Skip this frame
                }
                // --- End Check for decode errors ---

                // --- Frame Identification and Storage --- 
                int64_t framePts = frame->best_effort_timestamp;
                int currentFrameIndex = -1; 
                int64_t frameTimeMs = -1; // Declare frameTimeMs here, default to -1

                if (framePts == AV_NOPTS_VALUE) framePts = frame->pts;

                // Estimate current frame index based on PTS
                // This requires frameIndex_ to have valid time_ms
                if (framePts != AV_NOPTS_VALUE) {
                    // Calculate actual frameTimeMs if PTS is valid
                    frameTimeMs = av_rescale_q(framePts - videoStream_->start_time, timeBase_, {1, 1000});
                    
                    // Binary search for the closest frame index
                    if (!frameIndex_.empty()) {
                        int64_t targetTimeMs = frameTimeMs;
                        
                        auto it = std::lower_bound(frameIndex_.begin(), frameIndex_.end(), targetTimeMs,
                            [](const FrameInfo& info, int64_t val) {
                                // Ensure comparison is safe even if some time_ms might be uninitialized (-1)
                                // However, createFrameIndex should initialize all time_ms.
                                // For lower_bound, elements considered "less" if their time_ms is less.
                                if (info.time_ms < 0 && val >= 0) return true; // Uninitialized considered less than initialized
                                if (info.time_ms >= 0 && val < 0) return false; // Initialized considered not less than uninitialized
                                return info.time_ms < val; 
                            });

                        int64_t minAbsoluteDifference = INT64_MAX;

                        // Candidate 1: Element at or after targetTimeMs (from std::lower_bound)
                        if (it != frameIndex_.end()) {
                            if (it->time_ms >= 0) { // Consider only if time_ms is valid
                                int64_t diff = std::abs(it->time_ms - targetTimeMs);
                                if (diff < minAbsoluteDifference) {
                                    minAbsoluteDifference = diff;
                                    currentFrameIndex = std::distance(frameIndex_.begin(), it);
                                }
                            }
                        }

                        // Candidate 2: Element before targetTimeMs (if 'it' is not the beginning)
                        if (it != frameIndex_.begin()) {
                            auto prev_it = std::prev(it);
                            if (prev_it->time_ms >= 0) { // Consider only if time_ms is valid
                                int64_t diff = std::abs(prev_it->time_ms - targetTimeMs);
                                if (diff < minAbsoluteDifference) {
                                    minAbsoluteDifference = diff;
                                    currentFrameIndex = std::distance(frameIndex_.begin(), prev_it);
                                } else if (diff == minAbsoluteDifference) {
                                    // If differences are equal, std::lower_bound gives the element >= target.
                                    // If 'it' was valid, currentFrameIndex is already set to it.
                                    // If 'it' was not valid (e.g. end() or invalid time_ms) and currentFrameIndex is still -1,
                                    // then prev_it (if equally close) becomes the candidate.
                                    if (currentFrameIndex == -1) {
                                         currentFrameIndex = std::distance(frameIndex_.begin(), prev_it);
                }
                                }
                            }
                        }
                    }
                }
                // End of new binary search logic

                // --- Logic for deciding whether to store the frame (Counter Based Step) ---
                if (currentFrameIndex != -1 && currentFrameIndex >= startFrame && currentFrameIndex <= endFrame) {
                    bool shouldStoreThisFrame = false;

                    // Ищем и сохраняем первый подходящий кадр в диапазоне
                    if (!firstFrameStoredInRange) {
                        // Ensure frameTimeMs is valid for this check
                        // Store the first frame ONLY if it\'s a key frame AND its timestamp is valid and near the target
                        if (frame->key_frame == 1 && frameTimeMs != -1 && frameTimeMs >= seekTargetTimeMs - 50) { // Re-added key_frame check
                            shouldStoreThisFrame = true;
                            firstFrameStoredInRange = true;
                            firstStoredFrameIndex = currentFrameIndex; // Запоминаем, где сохранили первый
                            decodedFramesSinceFirstStore = 0; // Сбрасываем счетчик для первого кадра
                        }
                        // If the first frame isn\'t a key frame meeting the time condition,
                        // we keep looking (firstFrameStoredInRange remains false).
                    }
                    // Для последующих кадров используем счетчик
                    else {
                        decodedFramesSinceFirstStore++; // Увеличиваем счетчик для КАЖДОГО кадра после первого
                        if (decodedFramesSinceFirstStore >= adaptedStep_) { // Сравниваем с шагом
                           shouldStoreThisFrame = true;
                           decodedFramesSinceFirstStore = 0; // Сбрасываем счетчик после решения о сохранении
                        }
                    }

                    if (shouldStoreThisFrame) {
                         // Дополнительная проверка: не пытаемся сохранить по индексу меньшему, чем первый сохраненный
                         if (firstStoredFrameIndex == -1 || currentFrameIndex >= firstStoredFrameIndex) {
                             std::lock_guard<std::mutex> lock(frameIndex_[currentFrameIndex].mutex);
                             // Сохраняем, только если слот пуст
                             if (!frameIndex_[currentFrameIndex].cached_frame) {
                                AVFrame* temp_clone = av_frame_clone(frame);
                                if (temp_clone) { // If clone succeeded structually
                                    // Restore check for software formats (assuming YUV planar)
                                    bool is_frame_valid = temp_clone->width > 0 && temp_clone->height > 0 &&
                                                          temp_clone->data[0] != nullptr && temp_clone->linesize[0] > 0 &&
                                                          temp_clone->data[1] != nullptr && temp_clone->linesize[1] > 0 &&
                                                          temp_clone->data[2] != nullptr && temp_clone->linesize[2] > 0;

                                    if (!is_frame_valid) {
                                        std::cerr << "CachedDecoder Warning: Cloned frame for index " << currentFrameIndex
                                                   << " appears invalid or incomplete (w:" << temp_clone->width << " h:" << temp_clone->height
                                                   << " data[0]:" << (void*)temp_clone->data[0] << " ls[0]:" << temp_clone->linesize[0]
                                                   << " data[1]:" << (void*)temp_clone->data[1] << " ls[1]:" << temp_clone->linesize[1]
                                                   << " data[2]:" << (void*)temp_clone->data[2] << " ls[2]:" << temp_clone->linesize[2]
                                                   << "). Discarding clone." << std::endl;
                                        av_frame_free(&temp_clone); // Free the problematic clone, do not store it
                                    } else {
                                        // Clone is sane, store it
                                        frameIndex_[currentFrameIndex].cached_frame = std::shared_ptr<AVFrame>(temp_clone, [](AVFrame* f){ av_frame_free(&f); });
                                        
                                        // Update info
                                        frameIndex_[currentFrameIndex].pts = framePts;
                                        frameIndex_[currentFrameIndex].relative_pts = framePts - videoStream_->start_time;
                                        // frameIndex_[currentFrameIndex].time_ms = av_rescale_q(frameIndex_[currentFrameIndex].relative_pts, timeBase_, {1, 1000});
                                        frameIndex_[currentFrameIndex].time_base = timeBase_;

                                        // Set type
                                        if (frameIndex_[currentFrameIndex].type == FrameInfo::EMPTY || frameIndex_[currentFrameIndex].type == FrameInfo::CACHED) {
                                            frameIndex_[currentFrameIndex].type = FrameInfo::CACHED;
                                        }
                                    }
                                } else {
                                     std::cerr << "CachedDecoder Error: av_frame_clone returned nullptr for index " << currentFrameIndex << std::endl;
                                }
                            } // End if !cached_frame
                         } // else: Индекс "прыгнул" назад? Пропускаем сохранение.
                    }
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