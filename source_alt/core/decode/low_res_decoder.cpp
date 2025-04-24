#include "low_res_decoder.h"
#include "decode.h"
#include <iostream>
#include <filesystem>
#include <thread>
#include <openssl/evp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cmath> // For std::abs in timestamp comparison
#include <atomic> // Include atomic header
#include <cstdio> // For popen, pclose, fgets
#include <string>
#include <algorithm> // For std::min
#include <vector> // For storing ffprobe output lines
#include <cstdlib> // For atof
#include <functional> // For std::function

namespace fs = std::filesystem;

// --- Constructor and Destructor --- 
LowResDecoder::LowResDecoder(const std::string& lowResFilename) 
    : lowResFilename_(lowResFilename), 
      formatCtx_(nullptr), 
      codecCtx_(nullptr), 
      codecParams_(nullptr), 
      videoStream_(nullptr), 
      swsCtx_(nullptr), 
      videoStreamIndex_(-1), 
      initialized_(false), 
      width_(0), 
      height_(0), 
      pixFmt_(AV_PIX_FMT_NONE), 
      stop_requested_(false) 
{
    std::cout << "LowResDecoder created for: " << lowResFilename_ << std::endl;
    initialized_ = initialize(); // Call initialize on construction
}

LowResDecoder::~LowResDecoder() {
    std::cout << "LowResDecoder destroyed for: " << lowResFilename_ << std::endl;
    cleanup(); // Call cleanup on destruction
}

// --- Private Initialization and Cleanup ---

bool LowResDecoder::initialize() {
    if (initialized_) return true; // Already initialized

    // Open input file
    if (avformat_open_input(&formatCtx_, lowResFilename_.c_str(), nullptr, nullptr) != 0) {
        std::cerr << "LowResDecoder Error: Failed to open file " << lowResFilename_ << std::endl;
        cleanup();
        return false;
    }

    // Find stream info
    if (avformat_find_stream_info(formatCtx_, nullptr) < 0) {
        std::cerr << "LowResDecoder Error: Failed to find stream information for " << lowResFilename_ << std::endl;
        cleanup();
        return false;
    }

    // Find video stream
    const AVCodec* codec = nullptr; 
    videoStreamIndex_ = av_find_best_stream(formatCtx_, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (videoStreamIndex_ < 0 || !codec) {
        std::cerr << "LowResDecoder Error: Video stream not found or codec could not be found in " << lowResFilename_ << std::endl;
        cleanup();
        return false;
    }
    videoStream_ = formatCtx_->streams[videoStreamIndex_];
    codecParams_ = videoStream_->codecpar; // Store codec parameters

    // Allocate codec context
    codecCtx_ = avcodec_alloc_context3(codec);
    if (!codecCtx_) {
        std::cerr << "LowResDecoder Error: Failed to allocate codec context" << std::endl;
        cleanup();
        return false;
    }

    // Copy codec parameters to context
    if (avcodec_parameters_to_context(codecCtx_, codecParams_) < 0) {
        std::cerr << "LowResDecoder Error: Failed to copy codec parameters to context" << std::endl;
        cleanup();
        return false;
    }
    
    // Enable multi-threading hint (actual threading depends on codec implementation)
    // codecCtx_->thread_count = std::thread::hardware_concurrency(); // Use available cores
    // codecCtx_->thread_type = FF_THREAD_FRAME; // Enable frame-level threading

    // Open codec
    if (avcodec_open2(codecCtx_, codec, nullptr) < 0) {
        std::cerr << "LowResDecoder Error: Failed to open codec" << std::endl;
        cleanup();
        return false;
    }

    // Store video properties
    width_ = codecCtx_->width;
    height_ = codecCtx_->height;
    pixFmt_ = codecCtx_->pix_fmt;

    // SwsContext for potential future format conversions (e.g., to RGB)
    // Initialize only if needed, or consider initializing it on demand later.
    /*
    swsCtx_ = sws_getContext(width_, height_, pixFmt_, 
                             width_, height_, AV_PIX_FMT_RGB24, // Example target format
                             SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!swsCtx_) {
        std::cerr << "LowResDecoder Warning: Failed to create SwsContext" << std::endl;
        // This might not be fatal if conversion isn't strictly required initially
    }
    */

    std::cout << "LowResDecoder initialized successfully for " << lowResFilename_ << std::endl;
    std::cout << "  Resolution: " << width_ << "x" << height_ << std::endl;
    std::cout << "  Pixel Format: " << (pixFmt_ != AV_PIX_FMT_NONE ? av_get_pix_fmt_name(pixFmt_) : "N/A") << std::endl;
    std::cout << "  Time Base: " << videoStream_->time_base.num << "/" << videoStream_->time_base.den << std::endl;
    
    initialized_ = true;
    return true;
}

void LowResDecoder::cleanup() {
    if (swsCtx_) {
        sws_freeContext(swsCtx_);
        swsCtx_ = nullptr;
    }
    if (codecCtx_) {
        avcodec_free_context(&codecCtx_);
        codecCtx_ = nullptr;
    }
    // codecParams_ points into formatCtx_->streams, no need to free separately.
    codecParams_ = nullptr; 
    if (formatCtx_) {
        avformat_close_input(&formatCtx_);
        formatCtx_ = nullptr;
    }
    videoStream_ = nullptr; // Belongs to formatCtx_
    videoStreamIndex_ = -1;
    initialized_ = false;
    width_ = 0;
    height_ = 0;
    pixFmt_ = AV_PIX_FMT_NONE;
    stop_requested_ = false; // Reset stop flag on cleanup? (Consider lifecycle)
    std::cout << "LowResDecoder cleaned up." << std::endl;
}

// --- Public method to request stop --- 
void LowResDecoder::requestStop() {
    stop_requested_ = true;
}

// --- Static Methods --- 

std::string LowResDecoder::getCachePath() {
    const char* homeDir = getenv("HOME");
    if (!homeDir) {
        struct passwd* pwd = getpwuid(getuid());
        if (pwd)
            homeDir = pwd->pw_dir;
    }
    
    if (!homeDir) {
        std::cerr << "Could not determine home directory" << std::endl;
        return "";
    }
    
    std::string cachePath = std::string(homeDir) + "/.cache/tapexplayer";
    return cachePath;
}

std::string LowResDecoder::generateFileId(const std::string& filename) {
    EVP_MD_CTX *mdctx;
    const EVP_MD *md;
    unsigned char md_value[EVP_MAX_MD_SIZE];
    unsigned int md_len;
    
    md = EVP_md5();
    mdctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(mdctx, md, NULL);
    
    FILE *file = fopen(filename.c_str(), "rb");
    if (!file) {
        std::cerr << "Error opening file for hashing: " << filename << std::endl;
        EVP_MD_CTX_free(mdctx); // Clean up context on error
        return "";
    }
    
    const int bufSize = 32768;
    unsigned char buffer[bufSize];
    int bytesRead = 0;
    while ((bytesRead = fread(buffer, 1, bufSize, file)) != 0) {
        EVP_DigestUpdate(mdctx, buffer, bytesRead);
    }
    fclose(file);
    
    EVP_DigestFinal_ex(mdctx, md_value, &md_len);
    EVP_MD_CTX_free(mdctx);
    
    char md5string[33];
    for(unsigned int i = 0; i < md_len; i++)
        snprintf(&md5string[i*2], 3, "%02x", (unsigned int)md_value[i]);
    
    return std::string(md5string);
}

// Function to get video duration using ffprobe
double getVideoDuration(const std::string& filename) {
    // Use standard double quotes for the filename, which is generally safer on Unix-like systems
    std::string command = "ffprobe -v error -show_entries format=duration -of default=noprint_wrappers=1:nokey=1 \"" + filename + "\"";
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        std::cerr << "Error executing ffprobe command to get duration." << std::endl;
        return -1.0;
    }

    char buffer[128];
    std::string result = "";
    while (!feof(pipe)) {
        if (fgets(buffer, 128, pipe) != NULL) {
            result += buffer;
        }
    }

    int status = pclose(pipe);
    if (status != 0 || result.empty()) {
        std::cerr << "ffprobe command failed or returned empty result for duration. Status: " << status << std::endl;
         std::cerr << "ffprobe output: " << result << std::endl; // Log ffprobe output on error
        return -1.0;
    }

    try {
        // Remove trailing newline if present
        if (!result.empty() && result.back() == '\n') {
            result.pop_back();
        }
        return std::stod(result); // Use stod for better precision and error handling
    } catch (const std::invalid_argument& ia) {
        std::cerr << "Invalid argument: Could not convert ffprobe duration output '" << result << "' to double." << std::endl;
        return -1.0;
    } catch (const std::out_of_range& oor) {
        std::cerr << "Out of range: Could not convert ffprobe duration output '" << result << "' to double." << std::endl;
        return -1.0;
    }
}

bool LowResDecoder::convertToLowRes(const std::string& filename, std::string& outputFilename, const std::function<void(int)>& progressCallback) {
    std::string cacheDir = getCachePath();
    fs::create_directories(cacheDir);

    std::string fileId = generateFileId(filename);
    if (fileId.empty()) {
        std::cerr << "Error generating file ID" << std::endl;
        return false;
    }

    std::string cachePath = cacheDir + "/" + fileId + "_lowres.mp4";

    if (fs::exists(cachePath)) {
        std::cout << "Found cached low-resolution file: " << cachePath << std::endl;
        outputFilename = cachePath;
        if (progressCallback) {
            progressCallback(100);
        }
        return true;
    }

    // --- Get Video Duration ---
    double totalDuration = getVideoDuration(filename);
    if (totalDuration <= 0) {
        std::cerr << "Could not determine video duration. Progress reporting will be inaccurate." << std::endl;
        // Proceed without accurate progress, or return false? For now, proceed.
    } else {
         std::cout << "Total video duration: " << totalDuration << " seconds." << std::endl;
    }
    // --- End Get Video Duration ---


    // Create FFmpeg command for low-res conversion without audio (-an)
    std::string command = "ffmpeg -y -i \"" + filename + "\" -vf \"scale=640:-2\" -c:v libx264 -crf 23 -preset veryfast -an \"" + cachePath + "\" 2>&1";

    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        std::cerr << "Error executing FFmpeg command" << std::endl;
        return false;
    }

    char buffer[256]; // Increased buffer size slightly
    std::string result = "";
    std::string line_buffer = ""; // Buffer to accumulate lines

    // Read FFmpeg output to track progress
    while (true) {
        if (fgets(buffer, sizeof(buffer), pipe) == NULL) {
             if (feof(pipe)) {
                 // End of file reached
                 break;
             } else {
                 // Error reading from pipe
                 std::cerr << "Error reading FFmpeg output pipe." << std::endl;
                 pclose(pipe); // Close the pipe before returning
                 return false; // Indicate failure
             }
         }
        
        result += buffer; // Append to the complete result string
        line_buffer += buffer; // Append to the current line buffer

        // Process line by line (check for newline)
        size_t newline_pos;
        while ((newline_pos = line_buffer.find('\n')) != std::string::npos || (newline_pos = line_buffer.find('\r')) != std::string::npos) {
             std::string current_line = line_buffer.substr(0, newline_pos);
             line_buffer.erase(0, newline_pos + 1); // Remove processed line (and newline)

            // Parse FFmpeg output to estimate progress
            if (progressCallback) {
                size_t timePos = current_line.find("time=");
                 if (timePos != std::string::npos) {
                    // Look for the end of the time string (space or end of string)
                     size_t timeEndPos = current_line.find_first_of(" \t", timePos + 5);
                     std::string timeStr;
                     if (timeEndPos != std::string::npos) {
                         timeStr = current_line.substr(timePos + 5, timeEndPos - (timePos + 5));
                     } else {
                         timeStr = current_line.substr(timePos + 5); // Take rest of string
                     }


                    // Convert time to seconds
                    int hours = 0, minutes = 0;
                    double seconds = 0.0; // Use double for seconds
                    // Use sscanf which is readily available
                    int scanned_items = sscanf(timeStr.c_str(), "%d:%d:%lf", &hours, &minutes, &seconds);

                    if (scanned_items == 3) { // Ensure all parts were scanned
                         double currentTime = hours * 3600.0 + minutes * 60.0 + seconds;

                        // Calculate progress if duration is known
                        if (totalDuration > 0) {
                            int progress = std::min(static_cast<int>((currentTime / totalDuration) * 100.0), 99); // Cap at 99% until complete
                            if (progressCallback) { // Call callback if it's valid (not null)
                                progressCallback(progress);
                            }
                            // Also print progress to stdout
                            std::cout << "Conversion Progress: " << progress << "%\r" << std::flush; 
                         } else {
                             // Optional: Provide some basic indication even without duration?
                             // e.g., progressCallback(-1); // Indicate unknown progress
                         }
                    } else {
                        // std::cerr << "Warning: Could not parse time string: " << timeStr << std::endl;
                    }
                }
            }
        } // end while processing lines
    } // end while reading pipe

    int status = pclose(pipe);
    std::cout << std::endl; // Move to next line after progress updates
    if (status != 0) {
        std::cerr << "FFmpeg command failed with status " << status << std::endl;
        std::cerr << "FFmpeg output:\n" << result << std::endl;
        // Consider removing the partially created cache file on failure
        fs::remove(cachePath); 
        return false;
    }

    outputFilename = cachePath;

    if (progressCallback) {
        progressCallback(100); // Indicate completion via callback
    }
    std::cout << "Conversion Progress: 100%" << std::endl; // Final progress output to stdout

    return true;
}

// --- Static Methods ---

// Static method to remove low-res frames outside the active buffer range
// Changed to remove frames *inside* the specified range [startIndex, endIndex]
void LowResDecoder::removeLowResFrames(std::vector<FrameInfo>& frameIndex, int startIndex, int endIndex) {
    // Clamp indices to valid range
    startIndex = std::max(0, startIndex);
    endIndex = std::min(static_cast<int>(frameIndex.size()) - 1, endIndex);

    if (startIndex > endIndex) {
        return; // Invalid range after clamping
    }

    // std::cout << "LowResDecoder::removeLowResFrames: Cleaning *inside* range [" << startIndex << "-" << endIndex << "]" << std::endl;

    for (int i = startIndex; i <= endIndex; ++i) {
        // Lock only the frame being modified
        std::lock_guard<std::mutex> lock(frameIndex[i].mutex);

        // Check if it currently has a low-res frame
        if (frameIndex[i].low_res_frame) {
            // --- Explicitly unreference frame data buffers BEFORE reset --- 
            av_frame_unref(frameIndex[i].low_res_frame.get());
            // --- End Explicit Unref ---
            
            frameIndex[i].low_res_frame.reset(); // Release the shared_ptr

            // Reset type only if it was LOW_RES
            if (frameIndex[i].type == FrameInfo::LOW_RES) {
                // If a full-res frame exists, keep that type, otherwise set to EMPTY
                if (frameIndex[i].frame) {
                    frameIndex[i].type = FrameInfo::FULL_RES;
                } else {
                    frameIndex[i].type = FrameInfo::EMPTY;
                }
            }
            // std::cout << "Removed low-res frame at index " << i << std::endl;
        }
    }
}

// --- Instance Methods ---

bool LowResDecoder::decodeLowResRange(std::vector<FrameInfo>& frameIndex, int startFrame, int endFrame, int highResStart, int highResEnd, bool skipHighResWindow) {
    // Reverting to the user-provided multi-threaded logic from the older build
    if (!initialized_) {
        std::cerr << "LowResDecoder::decodeLowResRange Error: Decoder object not initialized (cannot get filename)." << std::endl;
        return false;
    }
    if (frameIndex.empty()) {
        std::cerr << "LowResDecoder::decodeLowResRange Warning: Frame index is empty." << std::endl;
        return true; // Nothing to do
    }

    startFrame = std::max(0, startFrame);
    endFrame = std::min(static_cast<int>(frameIndex.size()) - 1, endFrame);

    if (startFrame > endFrame) {
         std::cerr << "LowResDecoder::decodeLowResRange Error: Invalid frame range requested after clamping (" << startFrame << " - " << endFrame << ")" << std::endl;
        return false; 
    }

    std::cout << "LowResDecoder::decodeLowResRange: Using old 2-thread logic for range [" << startFrame << " - " << endFrame << "]" << std::endl;

    const int numThreads = 3; 
    std::vector<std::thread> threads;
    std::atomic<bool> success{true};

    auto decodeSegment = [&](int threadId, int threadStartFrame, int threadEndFrame) {
        AVFormatContext* formatContext = nullptr;
        AVCodecContext* codecContext = nullptr;
        const AVCodec* codec = nullptr;
        int videoStream;

        // Use the member filename
        const std::string filename = lowResFilename_; 

        if (avformat_open_input(&formatContext, filename.c_str(), nullptr, nullptr) != 0) {
            std::cerr << "[Thread " << threadId << "] Error opening input: " << filename << std::endl;
            success = false;
            return;
        }

        if (avformat_find_stream_info(formatContext, nullptr) < 0) {
            std::cerr << "[Thread " << threadId << "] Error finding stream info." << std::endl;
            avformat_close_input(&formatContext);
            success = false;
            return;
        }

        videoStream = av_find_best_stream(formatContext, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
        if (videoStream < 0 || !codec) { // Added check for codec
            std::cerr << "[Thread " << threadId << "] Error finding video stream or codec." << std::endl;
            avformat_close_input(&formatContext);
            success = false;
            return;
        }

        codecContext = avcodec_alloc_context3(codec);
        if (!codecContext) {
            std::cerr << "[Thread " << threadId << "] Error allocating codec context." << std::endl;
            avformat_close_input(&formatContext);
            success = false;
            return;
        }

        // Optional: Set threading options for the codec context itself (if supported)
        codecContext->thread_count = std::max(1, (int)std::thread::hardware_concurrency() / numThreads); // Пример: распределить ядра
        codecContext->thread_type = FF_THREAD_FRAME; // Или FF_THREAD_SLICE, нужно тестировать

        if (avcodec_parameters_to_context(codecContext, formatContext->streams[videoStream]->codecpar) < 0) {
            std::cerr << "[Thread " << threadId << "] Error copying codec parameters." << std::endl;
            avcodec_free_context(&codecContext);
            avformat_close_input(&formatContext);
            success = false;
            return;
        }

        if (avcodec_open2(codecContext, codec, nullptr) < 0) {
            std::cerr << "[Thread " << threadId << "] Error opening codec." << std::endl;
            avcodec_free_context(&codecContext);
            avformat_close_input(&formatContext);
            success = false;
            return;
        }

        AVRational timeBase = formatContext->streams[videoStream]->time_base;
        // Use member variable if available, otherwise get from format context
        int64_t fileStartTime = (formatContext->streams[videoStream]->start_time != AV_NOPTS_VALUE) ? formatContext->streams[videoStream]->start_time : 0;

        // --- Seeking Logic from User Snippet ---
        int64_t seekTargetTimeMs = -1;
        int validTimeFrame = threadStartFrame;
        while (validTimeFrame <= threadEndFrame && frameIndex[validTimeFrame].time_ms < 0) {
            validTimeFrame++;
        }

        if (validTimeFrame <= threadEndFrame) {
            seekTargetTimeMs = frameIndex[validTimeFrame].time_ms;
            int64_t seek_target_ts = av_rescale_q(seekTargetTimeMs, {1, 1000}, timeBase);
            int seek_flags = AVSEEK_FLAG_BACKWARD;
            int seek_ret = av_seek_frame(formatContext, videoStream, seek_target_ts, seek_flags);
            if (seek_ret < 0) {
                 std::cerr << "[Thread " << threadId << "] Warning: Seek to ts " << seek_target_ts << " (ms " << seekTargetTimeMs << ") failed: " << av_err2str(seek_ret) << std::endl;
            } else {
                 avcodec_flush_buffers(codecContext);
                 // std::cout << "[Thread " << threadId << "] Seek successful to ~" << seekTargetTimeMs << " ms" << std::endl;
            }
        } else {
            std::cerr << "[Thread " << threadId << "] Warning: No valid timestamp found in range [" << threadStartFrame << ", " << threadEndFrame << "] for seeking." << std::endl;
            // Optionally seek to beginning? av_seek_frame(formatContext, videoStream, 0, AVSEEK_FLAG_BYTE);
        }
        // --- End Seeking Logic ---

        AVPacket* packet = av_packet_alloc();
        AVFrame* frame = av_frame_alloc();
        if (!packet || !frame) { 
            std::cerr << "[Thread " << threadId << "] Error allocating packet/frame." << std::endl;
            avcodec_free_context(&codecContext); avformat_close_input(&formatContext); success = false; return;
        }

        int currentFrame = threadStartFrame; // Use the simpler counter from the old logic

        while (success && !stop_requested_.load() && av_read_frame(formatContext, packet) >= 0) { // Check success AND stop_requested_
            if (packet->stream_index == videoStream) {
                int ret = avcodec_send_packet(codecContext, packet);
                if (ret < 0) {
                    if (ret != AVERROR(EAGAIN)) { /* Log warning */ }
                    av_packet_unref(packet);
                    continue;
                }

                while (success && !stop_requested_.load() && ret >= 0) { // Check success AND stop_requested_
                    ret = avcodec_receive_frame(codecContext, frame);
                    if (ret == AVERROR(EAGAIN)) {
                        break;
                    } else if (ret == AVERROR_EOF) {
                        success = false; // Treat EOF as a reason to stop this thread
                        goto thread_decode_loop_end; // Exit outer loop for this thread
                    } else if (ret < 0) {
                        std::cerr << "[Thread " << threadId << "] Error receiving frame: " << av_err2str(ret) << std::endl;
                        break;
                    }

                    int64_t framePts = frame->best_effort_timestamp; // Use best_effort first
                    if (framePts == AV_NOPTS_VALUE) framePts = frame->pts;

                    // Calculate time_ms based on potentially adjusted PTS and stream timebase
                    int64_t frameTimeMs = (framePts != AV_NOPTS_VALUE)
                                          ? av_rescale_q(framePts, timeBase, {1, 1000}) // Scale raw PTS directly
                                          : -1; 

                    // --- Frame Identification & Storage (Old Logic) ---
                    // Check if the current counter is within the thread's bounds AND frame time isn't before the seek target (allowing some slack)
                    if (currentFrame <= threadEndFrame && currentFrame < frameIndex.size() && (frameTimeMs >= seekTargetTimeMs - 50 || seekTargetTimeMs < 0)) 
                    { 
                        std::lock_guard<std::mutex> lock(frameIndex[currentFrame].mutex);
                        
                        // Always store low-res if the slot is empty, ignore skipHighResWindow
                        if (!frameIndex[currentFrame].low_res_frame) {
                            frameIndex[currentFrame].low_res_frame = std::shared_ptr<AVFrame>(av_frame_clone(frame), [](AVFrame* f) { av_frame_free(&f); });
                            if (frameIndex[currentFrame].low_res_frame) {
                                // Update FrameInfo with data from the *decoded* frame
                                frameIndex[currentFrame].pts = framePts;
                                frameIndex[currentFrame].relative_pts = framePts - fileStartTime;
                                frameIndex[currentFrame].time_ms = frameTimeMs;
                                frameIndex[currentFrame].time_base = timeBase;
                                
                                // Update type only if it was empty
                                if (frameIndex[currentFrame].type == FrameInfo::EMPTY) {
                                    frameIndex[currentFrame].type = FrameInfo::LOW_RES;
                                }
                                // std::cout << "[T" << threadId << "] Stored frame at index " << currentFrame << " (time=" << frameTimeMs << ")" << std::endl;
                            } else {
                                std::cerr << "[Thread " << threadId << "] Error cloning frame for index " << currentFrame << std::endl;
                                success = false; // Mark failure if clone fails
                                goto thread_decode_loop_end; // Stop this thread on critical error
                            }
                        } // else: low-res frame already exists, skip
                        
                        // Increment counter *after* successfully processing/storing for this index
                        currentFrame++; 
                    }
                    // --- End Frame Identification & Storage ---

                    av_frame_unref(frame); // Unref frame inside receive loop

                    // Check if we have processed beyond the end frame for this thread
                    if (currentFrame > threadEndFrame) {
                        goto thread_decode_loop_end;
                    }
                } // end while receive_frame
            } // end if packet is video stream
            av_packet_unref(packet); // Unref packet outside receive loop
        } // end while av_read_frame

    thread_decode_loop_end:
        if (stop_requested_.load()) { // Optional: Log if stopped due to request
            std::cout << "[Thread " << threadId << "] Exiting due to stop request." << std::endl;
        }
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&codecContext);
        avformat_close_input(&formatContext);
    };

    int totalFramesInRange = endFrame - startFrame + 1;
    int framesPerThread = totalFramesInRange / numThreads;
    int startOffset = startFrame;

    for (int i = 0; i < numThreads; ++i) {
        int threadStart = startOffset;
        int threadEnd = (i == numThreads - 1) ? endFrame : (startOffset + framesPerThread - 1);
        threadEnd = std::min(threadEnd, endFrame);

        if (threadStart > threadEnd) continue; 

        threads.emplace_back(decodeSegment, i, threadStart, threadEnd);
        startOffset = threadEnd + 1;
    }

    for (auto& thread : threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    return success.load(); // Return the final success status
}

// --- Getters ---

bool LowResDecoder::isInitialized() const {
    return initialized_;
}

int LowResDecoder::getWidth() const {
    return width_;
}

int LowResDecoder::getHeight() const {
    return height_;
}

AVPixelFormat LowResDecoder::getPixelFormat() const {
    return pixFmt_;
}

// --- Non-Static Methods (if any) --- 
// Example:
// bool LowResDecoder::decodeFrame(int frameNumber, AVFrame* outputFrame) {
//     // Implementation using member variables (formatCtx, codecCtx)
// } 