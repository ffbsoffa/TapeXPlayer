#include "decode.h"
#include <iostream>
#include <unistd.h>
#include <filesystem>
#include <algorithm>
#include <vector>
#include <unistd.h>
#include <sys/stat.h>
#include <openssl/evp.h>
#include <pwd.h>

namespace fs = std::filesystem;

std::string generateFileId(const char* filename) {
    EVP_MD_CTX *mdctx;
    const EVP_MD *md;
    unsigned char md_value[EVP_MAX_MD_SIZE];
    unsigned int md_len;
    
    md = EVP_md5();
    mdctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(mdctx, md, NULL);
    
    FILE *file = fopen(filename, "rb");
    if (!file) return "";
    
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

bool decodeFrameRange(const char* filename, std::vector<FrameInfo>& frameIndex, int startFrame, int endFrame) {
    const int numThreads = 2; 
    std::vector<std::thread> threads;
    std::atomic<bool> success{true};

    auto decodeSegment = [&](int threadId, int threadStartFrame, int threadEndFrame) {
        AVFormatContext* formatContext = nullptr;
        AVCodecContext* codecContext = nullptr;
        const AVCodec* codec = nullptr;
        int videoStream;

        if (avformat_open_input(&formatContext, filename, nullptr, nullptr) != 0) {
            success = false;
            return;
        }

        if (avformat_find_stream_info(formatContext, nullptr) < 0) {
            avformat_close_input(&formatContext);
            success = false;
            return;
        }

        // Try to find VideoToolbox decoder
        videoStream = av_find_best_stream(formatContext, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
        if (videoStream < 0) {
            avformat_close_input(&formatContext);
            success = false;
            return;
        }

        codecContext = avcodec_alloc_context3(codec);
        if (!codecContext) {
            avformat_close_input(&formatContext);
            success = false;
            return;
        }

        if (avcodec_parameters_to_context(codecContext, formatContext->streams[videoStream]->codecpar) < 0) {
            avcodec_free_context(&codecContext);
            avformat_close_input(&formatContext);
            success = false;
            return;
        }

        // Disable hardware acceleration
        codecContext->hw_device_ctx = nullptr;

        if (avcodec_open2(codecContext, codec, nullptr) < 0) {
            std::cerr << "Failed to open codec" << std::endl;
            avcodec_free_context(&codecContext);
            avformat_close_input(&formatContext);
            success = false;
            return;
        }

        AVRational timeBase = formatContext->streams[videoStream]->time_base;
        int64_t startPts = frameIndex[threadStartFrame].pts;
        av_seek_frame(formatContext, videoStream, startPts, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(codecContext);

        AVPacket* packet = av_packet_alloc();
        AVFrame* frame = av_frame_alloc();

        int currentFrame = threadStartFrame;

        while (av_read_frame(formatContext, packet) >= 0 && currentFrame <= threadEndFrame) {
            if (packet->stream_index == videoStream) {
                int ret = avcodec_send_packet(codecContext, packet);
                if (ret < 0) {
                    av_packet_unref(packet);
                    continue;
                }

                while (ret >= 0) {
                    ret = avcodec_receive_frame(codecContext, frame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                        break;
                    } else if (ret < 0) {
                        break;
                    }

                    if (frame->pts >= startPts && currentFrame <= threadEndFrame && currentFrame < frameIndex.size()) {
                        std::lock_guard<std::mutex> lock(frameIndex[currentFrame].mutex);
                        if (!frameIndex[currentFrame].is_decoding) {
                            frameIndex[currentFrame].is_decoding = true;
                            
                            double seconds = frame->pts * av_q2d(timeBase);
                            int64_t milliseconds = seconds * 1000;

                            frameIndex[currentFrame].frame = std::shared_ptr<AVFrame>(av_frame_clone(frame), [](AVFrame* f) { av_frame_free(&f); });
                            frameIndex[currentFrame].pts = frame->pts;
                            frameIndex[currentFrame].relative_pts = frame->pts - formatContext->streams[videoStream]->start_time;
                            frameIndex[currentFrame].time_ms = milliseconds;
                            frameIndex[currentFrame].type = FrameInfo::FULL_RES;
                            frameIndex[currentFrame].is_decoding = false;
                        }
                        currentFrame++;
                    }
                }
            }
            av_packet_unref(packet);
            if (currentFrame > threadEndFrame) break;
        }

        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&codecContext);
        avformat_close_input(&formatContext);
    };

    int framesPerThread = (endFrame - startFrame + 1) / numThreads;
    for (int i = 0; i < numThreads; ++i) {
        int threadStartFrame = startFrame + i * framesPerThread;
        int threadEndFrame = (i == numThreads - 1) ? endFrame : threadStartFrame + framesPerThread - 1;
        threads.emplace_back(decodeSegment, i, threadStartFrame, threadEndFrame);
    }

    for (auto& thread : threads) {
        thread.join();
    }

    return success;
}

std::vector<FrameInfo> createFrameIndex(const char* filename) {
    std::vector<FrameInfo> frameIndex;
    AVFormatContext* formatContext = nullptr;

    if (avformat_open_input(&formatContext, filename, nullptr, nullptr) != 0) {
        std::cerr << "Failed to open file" << std::endl;
        return frameIndex;
    }

    if (avformat_find_stream_info(formatContext, nullptr) < 0) {
        std::cerr << "Failed to get stream information" << std::endl;
        avformat_close_input(&formatContext);
        return frameIndex;
    }

    int videoStream = av_find_best_stream(formatContext, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (videoStream < 0) {
        std::cerr << "Video stream not found" << std::endl;
        avformat_close_input(&formatContext);
        return frameIndex;
    }

    AVRational timeBase = formatContext->streams[videoStream]->time_base;
    int64_t startTime = formatContext->streams[videoStream]->start_time;

    AVPacket packet;
    while (av_read_frame(formatContext, &packet) >= 0) {
        if (packet.stream_index == videoStream) {
            FrameInfo info;
            info.pts = packet.pts;
            info.relative_pts = packet.pts - startTime;
            info.time_ms = av_rescale_q(info.relative_pts, timeBase, {1, 1000});
            info.frame = nullptr;
            info.low_res_frame = nullptr;
            info.type = FrameInfo::EMPTY;
            frameIndex.push_back(info);
        }
        av_packet_unref(&packet);
    }

    avformat_close_input(&formatContext);
    return frameIndex;
}

std::string getCachePath() {
    const char* homeDir;
    if ((homeDir = getenv("HOME")) == NULL) {
        homeDir = getpwuid(getuid())->pw_dir;
    }
    std::string cachePath = std::string(homeDir) + "/Library/Caches/TapeXPlayer";
    return cachePath;
}

bool convertToLowRes(const char* filename, std::string& outputFilename) {
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
        return true;
    }


    char probe_cmd[1024];
    snprintf(probe_cmd, sizeof(probe_cmd),
             "ffprobe -v error -select_streams v:0 -count_packets -show_entries stream=width,height -of csv=p=0 \"%s\"",
             filename);
    
    FILE* pipe = popen(probe_cmd, "r");
    if (!pipe) {
        std::cerr << "Error executing ffprobe" << std::endl;
        return false;
    }
    
    int width, height;
    if (fscanf(pipe, "%d,%d", &width, &height) != 2) {
        std::cerr << "Error reading video dimensions" << std::endl;
        pclose(pipe);
        return false;
    }
    pclose(pipe);


    int new_width = width / 2;
    int new_height = height / 2;


    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -i \"%s\" -c:v libx264 -crf 23 -preset ultrafast "
             "-vf scale=%d:%d "
             "-y \"%s\"",
             filename, new_width, new_height, cachePath.c_str());
    
    std::cout << "Starting conversion to low resolution..." << std::endl;
    int result = system(cmd);
    if (result != 0) {
        std::cerr << "Error converting video to low resolution" << std::endl;
        return false;
    }
    std::cout << "Low resolution conversion completed successfully" << std::endl;


    outputFilename = cachePath;

 
    std::vector<fs::path> cacheFiles;
    for (const auto& entry : fs::directory_iterator(cacheDir)) {
        if (entry.path().extension() == ".mp4") {
            cacheFiles.push_back(entry.path());
        }
    }

    std::sort(cacheFiles.begin(), cacheFiles.end(),
              [](const fs::path& a, const fs::path& b) {
                  return fs::last_write_time(a) < fs::last_write_time(b);
              });

    while (cacheFiles.size() > 3) {
        fs::remove(cacheFiles[0]);
        cacheFiles.erase(cacheFiles.begin());
    }

    return true;
}

bool fillIndexWithLowResFrames(const char* filename, std::vector<FrameInfo>& frameIndex) {
    AVFormatContext* formatContext = nullptr;
    AVCodecContext* codecContext = nullptr;
    const AVCodec* codec = nullptr;
    int videoStream;

    if (avformat_open_input(&formatContext, filename, nullptr, nullptr) != 0) {
        std::cerr << "Failed to open file" << std::endl;
        return false;
    }

    if (avformat_find_stream_info(formatContext, nullptr) < 0) {
        std::cerr << "Failed to find stream information" << std::endl;
        avformat_close_input(&formatContext);
        return false;
    }

    videoStream = av_find_best_stream(formatContext, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (videoStream < 0) {
        std::cerr << "Video stream not found" << std::endl;
        avformat_close_input(&formatContext);
        return false;
    }

    codecContext = avcodec_alloc_context3(codec);
    if (!codecContext) {
        avformat_close_input(&formatContext);
        return false;
    }

    if (avcodec_parameters_to_context(codecContext, formatContext->streams[videoStream]->codecpar) < 0) {
        avcodec_free_context(&codecContext);
        avformat_close_input(&formatContext);
        return false;
    }

    if (avcodec_open2(codecContext, codec, nullptr) < 0) {
        avcodec_free_context(&codecContext);
        avformat_close_input(&formatContext);
        return false;
    }

    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();

    int frameCount = 0;
    while (av_read_frame(formatContext, packet) >= 0) {
        if (packet->stream_index == videoStream) {
            int ret = avcodec_send_packet(codecContext, packet);
            if (ret < 0) {
                av_packet_unref(packet);
                continue;
            }

            while (ret >= 0) {
                ret = avcodec_receive_frame(codecContext, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                } else if (ret < 0) {
                    break;
                }

                if (frameCount < frameIndex.size()) {
                    frameIndex[frameCount].low_res_frame = std::shared_ptr<AVFrame>(av_frame_clone(frame), [](AVFrame* f) { av_frame_free(&f); });
                    frameIndex[frameCount].type = FrameInfo::LOW_RES;
                } else {
                    frameIndex.push_back(FrameInfo{});
                    frameIndex.back().low_res_frame = std::shared_ptr<AVFrame>(av_frame_clone(frame), [](AVFrame* f) { av_frame_free(&f); });
                    frameIndex.back().type = FrameInfo::LOW_RES;
                }

                frameCount++;
                if (frameCount % 100 == 0) {
                    std::cout << "Processed " << frameCount << " frames\r" << std::flush;
                }
            }
        }
        av_packet_unref(packet);
    }

    std::cout << "\nTotal processed " << frameCount << " frames" << std::endl;

    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&codecContext);
    avformat_close_input(&formatContext);

    return true;
}

bool decodeLowResRange(const char* filename, std::vector<FrameInfo>& frameIndex, int startFrame, int endFrame, int highResStart, int highResEnd) {
    const int numThreads = 2; 
    std::vector<std::thread> threads;
    std::atomic<bool> success{true};

    auto decodeSegment = [&](int threadId, int threadStartFrame, int threadEndFrame) {
        AVFormatContext* formatContext = nullptr;
        AVCodecContext* codecContext = nullptr;
        const AVCodec* codec = nullptr;
        int videoStream;

        if (avformat_open_input(&formatContext, filename, nullptr, nullptr) != 0) {
            success = false;
            return;
        }

        if (avformat_find_stream_info(formatContext, nullptr) < 0) {
            avformat_close_input(&formatContext);
            success = false;
            return;
        }

        videoStream = av_find_best_stream(formatContext, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
        if (videoStream < 0) {
            avformat_close_input(&formatContext);
            success = false;
            return;
        }

        codecContext = avcodec_alloc_context3(codec);
        if (!codecContext) {
            avformat_close_input(&formatContext);
            success = false;
            return;
        }

        codecContext->thread_count = std::thread::hardware_concurrency(); 
        codecContext->thread_type = FF_THREAD_FRAME; 

        if (avcodec_parameters_to_context(codecContext, formatContext->streams[videoStream]->codecpar) < 0) {
            avcodec_free_context(&codecContext);
            avformat_close_input(&formatContext);
            success = false;
            return;
        }

        if (avcodec_open2(codecContext, codec, nullptr) < 0) {
            avcodec_free_context(&codecContext);
            avformat_close_input(&formatContext);
            success = false;
            return;
        }

        AVRational timeBase = formatContext->streams[videoStream]->time_base;
        int64_t startPts = frameIndex[threadStartFrame].pts;
        av_seek_frame(formatContext, videoStream, startPts, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(codecContext);

        AVPacket* packet = av_packet_alloc();
        AVFrame* frame = av_frame_alloc();

        int currentFrame = threadStartFrame;

        while (av_read_frame(formatContext, packet) >= 0 && currentFrame <= threadEndFrame) {
            if (packet->stream_index == videoStream) {
                int ret = avcodec_send_packet(codecContext, packet);
                if (ret < 0) {
                    av_packet_unref(packet);
                    continue;
                }

                while (ret >= 0) {
                    ret = avcodec_receive_frame(codecContext, frame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                        break;
                    } else if (ret < 0) {
                        break;
                    }

                    if (frame->pts >= startPts && currentFrame <= threadEndFrame && currentFrame < frameIndex.size()) {
                        if (currentFrame >= highResStart && currentFrame <= highResEnd) {
                            currentFrame++;
                            continue;
                        }

                        if (frameIndex[currentFrame].type == FrameInfo::EMPTY) {
                            double seconds = frame->pts * av_q2d(timeBase);
                            int64_t milliseconds = seconds * 1000;

                            frameIndex[currentFrame].low_res_frame = std::shared_ptr<AVFrame>(av_frame_clone(frame), [](AVFrame* f) { av_frame_free(&f); });
                            frameIndex[currentFrame].pts = frame->pts;
                            frameIndex[currentFrame].relative_pts = frame->pts - formatContext->streams[videoStream]->start_time;
                            frameIndex[currentFrame].time_ms = milliseconds;
                            frameIndex[currentFrame].type = FrameInfo::LOW_RES;
                        }
                        currentFrame++;
                    }
                }
            }
            av_packet_unref(packet);
            if (currentFrame > threadEndFrame) break;
        }

        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&codecContext);
        avformat_close_input(&formatContext);
    };

    int framesPerThread = (endFrame - startFrame + 1) / numThreads;
    for (int i = 0; i < numThreads; ++i) {
        int threadStartFrame = startFrame + i * framesPerThread;
        int threadEndFrame = (i == numThreads - 1) ? endFrame : threadStartFrame + framesPerThread - 1;
        threads.emplace_back(decodeSegment, i, threadStartFrame, threadEndFrame);
    }

    for (auto& thread : threads) {
        thread.join();
    }

    return success;
}

FrameCleaner::FrameCleaner(std::vector<FrameInfo>& fi) : frameIndex(fi) {}

void FrameCleaner::cleanFrames(int startFrame, int endFrame) {
        for (int i = startFrame; i <= endFrame && i < frameIndex.size(); ++i) {
            if (frameIndex[i].frame) {
                frameIndex[i].frame.reset();
            }
            if (frameIndex[i].low_res_frame) {
                frameIndex[i].low_res_frame.reset();
            }
            frameIndex[i].type = FrameInfo::EMPTY;
        }
    }

std::future<void> asyncDecodeLowResRange(const char* filename, std::vector<FrameInfo>& frameIndex, int startFrame, int endFrame, int highResStart, int highResEnd) {
    return std::async(std::launch::async, [=, &frameIndex]() {
        decodeLowResRange(filename, frameIndex, startFrame, endFrame, highResStart, highResEnd);
    });
}

std::future<void> asyncDecodeFrameRange(const char* filename, std::vector<FrameInfo>& frameIndex, int startFrame, int endFrame) {
    return std::async(std::launch::async, [=, &frameIndex]() {
        decodeFrameRange(filename, frameIndex, startFrame, endFrame);
        for (int i = startFrame; i <= endFrame && i < frameIndex.size(); ++i) {
            if (frameIndex[i].frame) {
                frameIndex[i].type = FrameInfo::FULL_RES;
            } else {
            }
        }
    });
}

std::future<void> asyncCleanFrames(FrameCleaner& cleaner, int startFrame, int endFrame) {
    return std::async(std::launch::async, [&cleaner, startFrame, endFrame]() {
        cleaner.cleanFrames(startFrame, endFrame);
    });
}

RingBuffer::RingBuffer(size_t cap) : capacity(cap), start(0), size(0), playhead(0) {
    buffer.resize(capacity);
}

void RingBuffer::push(const FrameInfo& frame) {
    if (size < capacity) {
        buffer[(start + size) % capacity] = frame;
        size++;
    } else {
        buffer[start] = frame;
        start = (start + 1) % capacity;
    }
}

FrameInfo& RingBuffer::at(size_t index) {
    return buffer[(start + index) % capacity];
}

size_t RingBuffer::getStart() const {
    return start;
}

size_t RingBuffer::getSize() const {
    return size;
}

size_t RingBuffer::getPlayheadPosition() const {
    return playhead;
}

void RingBuffer::movePlayhead(int delta) {
    int newPlayhead = static_cast<int>(playhead) + delta;
    if (newPlayhead < 0) {
        playhead = 0;
    } else if (newPlayhead >= static_cast<int>(size)) {
        playhead = size - 1;
    } else {
        playhead = newPlayhead;
    }
}