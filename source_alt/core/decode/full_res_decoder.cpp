#include "full_res_decoder.h"
#include "decode.h" // Includes FrameInfo definition
#include <iostream>
#include <thread>
#include <atomic>
#include <vector>
#include <memory>
#include <cmath>
#include <algorithm>
#include <chrono> // Added for timing
#include <CoreVideo/CoreVideo.h> // Include necessary for CVPixelBufferRef type if used directly
#include <libavutil/pixdesc.h> // For av_get_pix_fmt_name if needed later

// --- Implementation of get_hw_format (copied from test) ---
enum AVPixelFormat FullResDecoder::get_hw_format(AVCodecContext *ctx,
                                                 const enum AVPixelFormat *pix_fmts) {
    const enum AVPixelFormat *p;
    for (p = pix_fmts; *p != -1; p++) {
        if (*p == AV_PIX_FMT_VIDEOTOOLBOX) {
            std::cout << "FullResDecoder: Found supported hardware pixel format: AV_PIX_FMT_VIDEOTOOLBOX" << std::endl;
            return *p;
        }
    }
    std::cerr << "FullResDecoder: Failed to get HW surface format." << std::endl;
    return AV_PIX_FMT_NONE;
}

FullResDecoder::FullResDecoder(const std::string& sourceFilename)
    : sourceFilename_(sourceFilename),
      initialized_(false),
      width_(0),
      height_(0),
      pixFmt_(AV_PIX_FMT_NONE),
      formatCtx_(nullptr),
      codecCtx_(nullptr),
      codecParams_(nullptr),
      videoStream_(nullptr),
      swsCtx_(nullptr),
      videoStreamIndex_(-1),
      hw_accel_enabled_(false), // Initialize new members
      hw_device_ctx_(nullptr),
      hw_pix_fmt_(AV_PIX_FMT_NONE),
      stop_requested_(false) // Initialize stop flag
{
    std::cout << "FullResDecoder: Initializing for " << sourceFilename_ << "..." << std::endl;
    initialized_ = initialize();
}

FullResDecoder::~FullResDecoder() {
    std::cout << "FullResDecoder: Destroying for " << sourceFilename_ << "..." << std::endl;
    cleanup();
    std::cout << "FullResDecoder: Destroyed." << std::endl;
}

bool FullResDecoder::initialize() {
    if (initialized_) return true; // Already initialized

    const AVCodec* codec = nullptr;
    int ret = 0;
    bool hw_init_success = false;

    // Open input
    if (avformat_open_input(&formatCtx_, sourceFilename_.c_str(), nullptr, nullptr) != 0) {
        std::cerr << "FullResDecoder Error: Could not open input file: " << sourceFilename_ << std::endl;
        return false; // No need for cleanup() here, nothing allocated yet
    }

    // Find stream info
    if (avformat_find_stream_info(formatCtx_, nullptr) < 0) {
        std::cerr << "FullResDecoder Error: Could not find stream info for: " << sourceFilename_ << std::endl;
        cleanup(); // formatCtx_ is open, need cleanup
        return false;
    }

    // Find best video stream
    videoStreamIndex_ = av_find_best_stream(formatCtx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0); // Find stream first
    if (videoStreamIndex_ < 0) {
         std::cerr << "FullResDecoder Error: Could not find video stream in: " << sourceFilename_ << std::endl;
         cleanup();
         return false;
    }
    videoStream_ = formatCtx_->streams[videoStreamIndex_];
    codecParams_ = videoStream_->codecpar;

    // Find the decoder for the stream
    codec = avcodec_find_decoder(codecParams_->codec_id);
    if (!codec) {
        std::cerr << "FullResDecoder Error: Could not find decoder for codec id " << codecParams_->codec_id << std::endl;
        cleanup();
        return false;
    }
    std::cout << "FullResDecoder: Found decoder: " << codec->name << std::endl;

    // --- Try to initialize Hardware Acceleration (VideoToolbox) ---
#ifdef __APPLE__
    enum AVHWDeviceType hw_type = AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
    for (int i = 0; ; i++) {
        const AVCodecHWConfig *config = avcodec_get_hw_config(codec, i);
        if (!config) {
             std::cout << "FullResDecoder: Decoder " << codec->name << " does not support VideoToolbox (no more HW configs)." << std::endl;
            break;
        }
        if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX &&
            config->device_type == hw_type) {
            std::cout << "FullResDecoder: Decoder supports VideoToolbox HW acceleration method." << std::endl;
            hw_pix_fmt_ = AV_PIX_FMT_VIDEOTOOLBOX; // We expect this format

            // Allocate codec context specifically for HW attempt
            AVCodecContext* hwCodecCtx = avcodec_alloc_context3(codec);
            if (!hwCodecCtx) {
                 std::cerr << "FullResDecoder Warning: Failed to allocate HW codec context attempt." << std::endl;
                 break; // Stop trying HW if allocation fails
            }

            // Create HW device context and store it in hw_device_ctx_
            ret = av_hwdevice_ctx_create(&hw_device_ctx_, hw_type, nullptr, nullptr, 0);
            if (ret < 0) {
                std::cerr << "FullResDecoder Warning: Failed to create HW device context: " << av_err2str(ret) << std::endl;
                avcodec_free_context(&hwCodecCtx); // Clean up attempted context
                hw_device_ctx_ = nullptr; // Ensure it's null
                break; // Stop trying HW acceleration
            }

            // Assign the created HW device context to the codec context
            hwCodecCtx->hw_device_ctx = av_buffer_ref(hw_device_ctx_); // Assign created context
            if (!hwCodecCtx->hw_device_ctx) {
                std::cerr << "FullResDecoder Warning: Failed to ref HW device context." << std::endl;
                avcodec_free_context(&hwCodecCtx);
                // hw_device_ctx_ needs unref since ref failed
                av_buffer_unref(&hw_device_ctx_);
                hw_device_ctx_ = nullptr;
                break;
            }

            // Set the get_format callback
            hwCodecCtx->get_format = get_hw_format;

             // Copy parameters and try to open codec with HW acceleration
            if (avcodec_parameters_to_context(hwCodecCtx, codecParams_) < 0) {
                 std::cerr << "FullResDecoder Warning: Failed to copy params to HW context attempt." << std::endl;
                 avcodec_free_context(&hwCodecCtx); // Cleanup attempt (will unref hw_device_ctx_)
                 // hw_device_ctx_ was already unreffed after creation, no extra unref needed here
                 break;
            }

            if (avcodec_open2(hwCodecCtx, codec, nullptr) < 0) {
                std::cerr << "FullResDecoder Warning: Failed to open codec with HW acceleration." << std::endl;
                avcodec_free_context(&hwCodecCtx); // Cleanup attempt (will unref hw_device_ctx_)
                 // hw_device_ctx_ was already unreffed after creation, no extra unref needed here
                break;
            }

            // Success! Use this context.
            std::cout << "FullResDecoder: Successfully initialized with VideoToolbox HW Acceleration." << std::endl;
            codecCtx_ = hwCodecCtx; // Assign the successfully opened HW context
            hw_accel_enabled_ = true;
            hw_init_success = true;
            // Unref the initially created hw_device_ctx_ as codecCtx_ now holds a reference
            av_buffer_unref(&hw_device_ctx_);
            hw_device_ctx_ = nullptr; // Mark our direct pointer as null
            break; // Found working HW config, stop searching
        }
    }
#endif

    // --- Fallback to Software Decoder if HW failed (or not Apple) ---
    if (!hw_init_success) {
        std::cout << "FullResDecoder: Initializing Software Decoder." << std::endl;
         // Ensure hw_device_ctx_ is cleaned up if created but not assigned/opened
         if (hw_device_ctx_) {
            av_buffer_unref(&hw_device_ctx_);
            hw_device_ctx_ = nullptr;
         }
         hw_accel_enabled_ = false;
         hw_pix_fmt_ = AV_PIX_FMT_NONE;

        codecCtx_ = avcodec_alloc_context3(codec);
        if (!codecCtx_) {
            std::cerr << "FullResDecoder Error: Could not alloc SW codec context." << std::endl;
            cleanup();
            return false;
        }
        if (avcodec_parameters_to_context(codecCtx_, codecParams_) < 0) {
            std::cerr << "FullResDecoder Error: Could not copy codec params to SW context." << std::endl;
            cleanup();
            return false;
        }
        // Optional: Enable multi-threading hint for SW decoder
        // codecCtx_->thread_count = std::thread::hardware_concurrency();
        // codecCtx_->thread_type = FF_THREAD_FRAME; // or FF_THREAD_SLICE
        if (avcodec_open2(codecCtx_, codec, nullptr) < 0) {
            std::cerr << "FullResDecoder Error: Could not open SW codec." << std::endl;
            cleanup();
            return false;
        }
         std::cout << "FullResDecoder: Initialized with Software Decoder." << std::endl;
    }

    // Store common properties
    width_ = codecCtx_->width;
    height_ = codecCtx_->height;
    // Use the actual pixel format reported by the opened context
    pixFmt_ = codecCtx_->pix_fmt;

    initialized_ = true;
    std::cout << "FullResDecoder: Initialization successful." << std::endl;
    std::cout << "  Mode: " << (hw_accel_enabled_ ? "Hardware (VideoToolbox)" : "Software") << std::endl;
    std::cout << "  Resolution: " << width_ << "x" << height_ << std::endl;
    // Safely get pixel format name
    const char* fmt_name = av_get_pix_fmt_name(pixFmt_);
    std::cout << "  Context Pixel Format: " << (fmt_name ? fmt_name : "N/A") << std::endl;
    std::cout << "  Time Base: " << videoStream_->time_base.num << "/" << videoStream_->time_base.den << std::endl;

    return true;
}

void FullResDecoder::cleanup() {
    if (swsCtx_) {
        sws_freeContext(swsCtx_);
        swsCtx_ = nullptr;
    }
    if (codecCtx_) {
        avcodec_free_context(&codecCtx_); // This also unrefs codecCtx_->hw_device_ctx if set
        codecCtx_ = nullptr;
    }
    // Our direct pointer hw_device_ctx_ should be null by now if initialization logic is correct.
    // No need to explicitly unref hw_device_ctx_ here as its lifetime is tied to codecCtx_->hw_device_ctx
    hw_device_ctx_ = nullptr;

    codecParams_ = nullptr; // Belongs to formatCtx_
    if (formatCtx_) {
        avformat_close_input(&formatCtx_);
        formatCtx_ = nullptr;
    }
    videoStream_ = nullptr; // Belongs to formatCtx_
    videoStreamIndex_ = -1;
    initialized_ = false;
    hw_accel_enabled_ = false;
    hw_pix_fmt_ = AV_PIX_FMT_NONE;
    width_ = 0;
    height_ = 0;
    pixFmt_ = AV_PIX_FMT_NONE;
    stop_requested_ = false; // Reset stop flag on cleanup
    // std::cout << "FullResDecoder cleaned up." << std::endl; // Optional log
}


bool FullResDecoder::isHardwareAccelerated() const {
    return hw_accel_enabled_;
}


bool FullResDecoder::decodeFrameRange(std::vector<FrameInfo>& frameIndex, int startFrame, int endFrame) {
    auto function_start_time = std::chrono::high_resolution_clock::now();
    if (!initialized_ || !formatCtx_ || !codecCtx_ || videoStreamIndex_ < 0) {
        std::cerr << "FullResDecoder::decodeFrameRange Error: Decoder not initialized." << std::endl;
        return false;
    }

    if (frameIndex.empty()) {
        std::cerr << "FullResDecoder::decodeFrameRange Warning: Frame index is empty." << std::endl;
        return true; // Nothing to do
    }

    // Clamp range to valid bounds
    startFrame = std::max(0, startFrame);
    endFrame = std::min(static_cast<int>(frameIndex.size()) - 1, endFrame);

    if (startFrame > endFrame) {
         std::cerr << "FullResDecoder::decodeFrameRange Error: Invalid frame range requested after clamping (" << startFrame << " - " << endFrame << ")" << std::endl;
         return false;
    }

    std::cout << "[Timing] FullResDecoder::decodeFrameRange: Decoding range [" << startFrame << " - " << endFrame << "] (HW: " << hw_accel_enabled_ << ")" << std::endl;

    // --- Seeking (using member contexts) ---
    AVRational timeBase = videoStream_->time_base;
    int64_t seek_target_ts = -1;
    int64_t startTimeMs = -1; // Store start time of the segment

    // Attempt to seek to the startFrame's time_ms if valid
    if (startFrame >= 0 && startFrame < frameIndex.size() && frameIndex[startFrame].time_ms >= 0) {
        startTimeMs = frameIndex[startFrame].time_ms;
        seek_target_ts = av_rescale_q(startTimeMs, {1, 1000}, timeBase);

        auto seek_start_time = std::chrono::high_resolution_clock::now(); // Timing: Seek start
        // std::cout << "[Timing] FullResDecoder::decodeFrameRange: Seeking to frame " << startFrame << " (target_ms=" << startTimeMs << ", target_ts=" << seek_target_ts << ")" << std::endl;

        int seek_flags = AVSEEK_FLAG_BACKWARD; // Seek to the keyframe <= target_ts
        int seek_ret = av_seek_frame(formatCtx_, videoStreamIndex_, seek_target_ts, seek_flags);

        auto seek_end_time = std::chrono::high_resolution_clock::now(); // Timing: Seek end
        std::chrono::duration<double, std::milli> seek_duration = seek_end_time - seek_start_time;

        if (seek_ret < 0) {
            std::cerr << "[Timing] FullResDecoder::decodeFrameRange Warning: Seek failed in " << seek_duration.count() << " ms (Error: " << av_err2str(seek_ret) << "). Will attempt decode sequentially." << std::endl;
            startTimeMs = -1; // Reset startTimeMs if seek failed, rely only on counter
        } else {
            avcodec_flush_buffers(codecCtx_);
            // std::cout << "[Timing] FullResDecoder::decodeFrameRange: Seek successful in " << seek_duration.count() << " ms. Buffers flushed." << std::endl;
        }
    } else {
        std::cerr << "[Timing] FullResDecoder::decodeFrameRange Warning: Invalid time_ms for startFrame " << startFrame << ". Will attempt decode sequentially." << std::endl;
        startTimeMs = -1; // No valid start time, rely only on counter
    }

    // --- Decoding Loop (Single-threaded, using member contexts) ---
        AVPacket* packet = av_packet_alloc();
        AVFrame* frame = av_frame_alloc(); // This frame will receive the final data (HW or SW)
    if (!packet || !frame) {
        std::cerr << "FullResDecoder::decodeFrameRange Error: Failed to allocate packet or frame." << std::endl;
        av_packet_free(&packet);
        av_frame_free(&frame);
        return false;
    }

    int currentOutputFrameIndex = startFrame; // Index where the next decoded frame should be stored
    int decoded_frame_count = 0; // Counter for timing log
    bool success = true; // Flag to track overall success

    auto loop_start_time = std::chrono::high_resolution_clock::now(); // Timing: Loop start
    while (!stop_requested_.load() && av_read_frame(formatCtx_, packet) >= 0) {
        if (packet->stream_index == videoStreamIndex_) {
            int ret = avcodec_send_packet(codecCtx_, packet);
                if (ret < 0) {
                if (ret != AVERROR(EAGAIN)) {
                    std::cerr << "FullResDecoder::decodeFrameRange Warning: Error sending packet: " << av_err2str(ret) << std::endl;
                }
                    av_packet_unref(packet);
                    continue;
                }

                while (!stop_requested_.load() && ret >= 0) {
                ret = avcodec_receive_frame(codecCtx_, frame); // Receive into 'frame'
                if (ret == AVERROR(EAGAIN)) {
                        break;
                } else if (ret == AVERROR_EOF) {
                    // std::cout << "FullResDecoder::decodeFrameRange: End of stream reached while decoding." << std::endl;
                    av_packet_unref(packet);
                    goto decode_loop_end;
                    } else if (ret < 0) {
                    std::cerr << "FullResDecoder::decodeFrameRange Error: Error receiving frame: " << av_err2str(ret) << std::endl;
                        success = false; // Mark failure
                        goto decode_loop_end;
                }

                // --- Frame Identification & Storage ---
                int64_t framePts = frame->best_effort_timestamp;
                if (framePts == AV_NOPTS_VALUE) framePts = frame->pts;

                int64_t frameTimeMs = (framePts != AV_NOPTS_VALUE)
                                        ? av_rescale_q(framePts, timeBase, {1, 1000})
                                        : -1; // Assign -1 if no valid PTS

                // Check if frame time is reasonable and index is within range
                if (currentOutputFrameIndex <= endFrame &&
                    (startTimeMs < 0 || frameTimeMs >= startTimeMs - 50)) // Check time only if seek was attempted/successful
                {
                    // Store the frame at the currentOutputFrameIndex
                    std::lock_guard<std::mutex> lock(frameIndex[currentOutputFrameIndex].mutex);

                    // frameIndex[currentOutputFrameIndex].is_decoding = true; // Optional mark

                    // Clone the received frame (could be HW surface or SW data)
                    frameIndex[currentOutputFrameIndex].frame = std::shared_ptr<AVFrame>(av_frame_clone(frame), [](AVFrame* f) { av_frame_free(&f); });
                    if (!frameIndex[currentOutputFrameIndex].frame) {
                        std::cerr << "FullResDecoder::decodeFrameRange Error: Failed to clone frame for index " << currentOutputFrameIndex << std::endl;
                        success = false;
                        goto decode_loop_end; // Stop processing on critical error
                    } else {
                        // Store frame metadata
                        frameIndex[currentOutputFrameIndex].pts = framePts;
                        int64_t streamStartTime = (videoStream_->start_time != AV_NOPTS_VALUE) ? videoStream_->start_time : 0;
                        frameIndex[currentOutputFrameIndex].relative_pts = framePts - streamStartTime;
                        frameIndex[currentOutputFrameIndex].time_ms = frameTimeMs;
                        frameIndex[currentOutputFrameIndex].type = FrameInfo::FULL_RES; // Mark as full-res attempt
                        frameIndex[currentOutputFrameIndex].time_base = timeBase;
                        frameIndex[currentOutputFrameIndex].format = (AVPixelFormat)frame->format; // <<< STORE THE ACTUAL FORMAT

                        // Debug log for HW frame
                        if (frame->format == AV_PIX_FMT_VIDEOTOOLBOX) {
                             CVPixelBufferRef cv_pix_buf = (CVPixelBufferRef)frame->data[3];
                             // std::cout << "[Decoded HW Frame " << currentOutputFrameIndex << "] Format: VT, CVPixelBufferRef: " << (void*)cv_pix_buf << std::endl;
                        } else {
                             // const char* sw_fmt_name = av_get_pix_fmt_name((AVPixelFormat)frame->format);
                             // std::cout << "[Decoded SW Frame " << currentOutputFrameIndex << "] Format: " << (sw_fmt_name ? sw_fmt_name : "Unknown") << std::endl;
                        }
                        decoded_frame_count++; // Increment counter only when frame is stored
                    }

                    // frameIndex[currentOutputFrameIndex].is_decoding = false;

                    // Move to the next output index
                    currentOutputFrameIndex++;

                    // Check if we have processed beyond the end frame
                     if (currentOutputFrameIndex > endFrame) {
                        // std::cout << "[Timing] FullResDecoder::decodeFrameRange: Reached end frame index " << endFrame << ". Stopping decode loop." << std::endl;
                        av_frame_unref(frame); // Unref the last received frame before breaking
                        av_packet_unref(packet);
                        goto decode_loop_end;
                     }
                }

                av_frame_unref(frame); // Unref frame inside receive loop
            } // end while(receive_frame)
        } // end if(packet->stream_index == videoStreamIndex_)
            av_packet_unref(packet);
    } // end while(av_read_frame)

decode_loop_end:
    auto loop_end_time = std::chrono::high_resolution_clock::now(); // Timing: Loop end
    std::chrono::duration<double, std::milli> loop_duration = loop_end_time - loop_start_time;
    std::cout << "[Timing] FullResDecoder::decodeFrameRange: Decode loop finished in " << loop_duration.count() << " ms. Decoded frames: " << decoded_frame_count << std::endl;

    if (stop_requested_.load()) { // Optional: Log if stopped due to request
        std::cout << "[FullResDecoder] Exiting decode loop due to stop request." << std::endl;
    }
    av_frame_free(&frame);
    av_packet_free(&packet);

    auto function_end_time = std::chrono::high_resolution_clock::now(); // Timing: Function end
    std::chrono::duration<double, std::milli> function_duration = function_end_time - function_start_time;
    // std::cout << "[Timing] FullResDecoder::decodeFrameRange: Finished processing range [" << startFrame << " - " << endFrame << "] in " << function_duration.count() << " ms." << std::endl;

    return success; // Indicate operation completed (or if errors occurred)
}


void FullResDecoder::removeHighResFrames(std::vector<FrameInfo>& frameIndex,
                                       int start, int end,
                                       int highResStart, int highResEnd) {
    // Clamp indices
    start = std::max(0, start);
    end = std::min(static_cast<int>(frameIndex.size()) - 1, end);
    if (start > end) return;

    for (int i = start; i <= end; ++i) {
        // Lock only the frame being modified
        std::lock_guard<std::mutex> lock(frameIndex[i].mutex);

        // Check if it's outside the current high-res window
        if (i < highResStart || i > highResEnd) {
            // Check if it currently has a full-res frame (could be HW or SW)
            if (frameIndex[i].frame) {
                frameIndex[i].frame.reset(); // Release the shared_ptr

                // Update type based on whether low_res exists
                if (frameIndex[i].low_res_frame) {
                    frameIndex[i].type = FrameInfo::LOW_RES;
                    // Keep the format of the low_res frame (should be SW)
                    frameIndex[i].format = static_cast<AVPixelFormat>(frameIndex[i].low_res_frame->format);
                } else {
                    frameIndex[i].type = FrameInfo::EMPTY;
                    frameIndex[i].format = AV_PIX_FMT_NONE;
                }
                 // std::cout << "Removed full-res frame outside window at index " << i << std::endl;
            }
        }
    }
}

void FullResDecoder::clearHighResFrames(std::vector<FrameInfo>& frameIndex) {
    for (auto& frameInfo : frameIndex) {
        // Lock the frame being modified
        std::lock_guard<std::mutex> lock(frameInfo.mutex);

        if (frameInfo.frame) {
            frameInfo.frame.reset(); // Release the full-res frame

            // Update type based on low_res frame presence
            if (frameInfo.low_res_frame) {
                frameInfo.type = FrameInfo::LOW_RES;
                frameInfo.format = static_cast<AVPixelFormat>(frameInfo.low_res_frame->format);
            } else {
                frameInfo.type = FrameInfo::EMPTY;
                frameInfo.format = AV_PIX_FMT_NONE;
            }
        }
    }
    // std::cout << "Cleared all high-res frames." << std::endl;
}


bool FullResDecoder::shouldProcessFrame(const FrameInfo& frame) {
    // Reverted logic: Let's focus on processing if NO full-res exists yet
    // No need to lock here as we are just reading atomic/simple types
    return !frame.is_decoding && !frame.frame; // Process if not decoding and no full-res frame exists
}

// --- Add back the missing isInitialized implementation ---
bool FullResDecoder::isInitialized() const {
    return initialized_;
}

// --- Getter implementations --- (getWidth, getHeight, getPixelFormat) should be fine
int FullResDecoder::getWidth() const { return width_; }
int FullResDecoder::getHeight() const { return height_; }
AVPixelFormat FullResDecoder::getPixelFormat() const { return pixFmt_; }

// --- Public method to request stop --- 
void FullResDecoder::requestStop() {
    stop_requested_ = true;
} 