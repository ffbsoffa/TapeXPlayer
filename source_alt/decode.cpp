#include "decode.h"
#include "common.h"
#include "display.h"
#include <iostream>
#include <unistd.h>
#include <filesystem>
#include <algorithm>
#include <vector>
#include <unistd.h>
#include <sys/stat.h>
#include <openssl/evp.h>
#include <pwd.h>
#include <regex>
#include <cstdio>
#include <cstdlib>

namespace fs = std::filesystem;

// Глобальный промежуточный буфер
FrameBuffer frameBuffer;

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

        codecContext->hw_device_ctx = nullptr;

        if (avcodec_open2(codecContext, codec, nullptr) < 0) {
            std::cerr << "Failed to open codec" << std::endl;
            avcodec_free_context(&codecContext);
            avformat_close_input(&formatContext);
            success = false;
            return;
        }

        AVRational timeBase = formatContext->streams[videoStream]->time_base;
        double frameDuration = av_q2d(formatContext->streams[videoStream]->avg_frame_rate);
        frameDuration = 1.0 / frameDuration;

        int64_t startTime = frameIndex[threadStartFrame].time_ms;
        av_seek_frame(formatContext, videoStream, startTime * timeBase.den / (timeBase.num * 1000), AVSEEK_FLAG_BACKWARD);
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

                    double frameTime = frame->pts * av_q2d(timeBase);
                    int64_t frameTimeMs = frameTime * 1000;

                    if (frameTimeMs >= startTime && currentFrame <= threadEndFrame && currentFrame < frameIndex.size()) {
                        std::lock_guard<std::mutex> lock(frameIndex[currentFrame].mutex);
                        if (!frameIndex[currentFrame].is_decoding) {
                            frameIndex[currentFrame].is_decoding = true;
                            
                            frameIndex[currentFrame].frame = std::shared_ptr<AVFrame>(av_frame_clone(frame), [](AVFrame* f) { av_frame_free(&f); });
                            frameIndex[currentFrame].pts = frame->pts;
                            frameIndex[currentFrame].relative_pts = frame->pts - formatContext->streams[videoStream]->start_time;
                            frameIndex[currentFrame].time_ms = frameTimeMs;
                            frameIndex[currentFrame].type = FrameInfo::FULL_RES;
                            frameIndex[currentFrame].time_base = timeBase;
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

bool convertToLowRes(const char* filename, std::string& outputFilename, ProgressCallback progressCallback) {
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
            progressCallback(100); // Indicate completion
        }
        return true;
    }

    // Create FFmpeg command for low-res conversion
    std::string command = "ffmpeg -y -i \"" + std::string(filename) + "\" -vf \"scale=640:-1\" -c:v libx264 -crf 23 -preset veryfast -c:a aac -b:a 128k \"" + cachePath + "\" 2>&1";
    
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        std::cerr << "Error executing FFmpeg command" << std::endl;
        return false;
    }

    char buffer[128];
    std::string result = "";
    
    // Read FFmpeg output to track progress
    while (!feof(pipe)) {
        if (fgets(buffer, 128, pipe) != NULL) {
            result += buffer;
            
            // Parse FFmpeg output to estimate progress
            if (progressCallback) {
                // Look for time= in the output
                std::string output(buffer);
                size_t timePos = output.find("time=");
                if (timePos != std::string::npos) {
                    // Extract time information
                    std::string timeStr = output.substr(timePos + 5, 11); // "00:00:00.00"
                    
                    // Convert time to seconds
                    int hours = 0, minutes = 0, seconds = 0;
                    float milliseconds = 0;
                    sscanf(timeStr.c_str(), "%d:%d:%d.%f", &hours, &minutes, &seconds, &milliseconds);
                    float currentTime = hours * 3600 + minutes * 60 + seconds + milliseconds / 100;
                    
                    // Estimate progress (assuming we know total duration)
                    // Since we don't know the total duration here, we'll use a heuristic
                    // This is a simplified approach - in a real implementation, you might want to 
                    // first determine the total duration of the video
                    int progress = std::min(int(currentTime / 3 * 100), 99); // Cap at 99% until complete
                    progressCallback(progress);
                }
            }
        }
    }
    
    int status = pclose(pipe);
    if (status != 0) {
        std::cerr << "FFmpeg command failed with status " << status << std::endl;
        return false;
    }

    outputFilename = cachePath;
    
    // Register the file for cleanup
    registerTempFileForCleanup(cachePath);
    
    if (progressCallback) {
        progressCallback(100); // Indicate completion
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

    AVRational streamTimeBase = formatContext->streams[videoStream]->time_base;

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
                    frameIndex[frameCount].time_base = streamTimeBase;
                } else {
                    frameIndex.push_back(FrameInfo{});
                    frameIndex.back().low_res_frame = std::shared_ptr<AVFrame>(av_frame_clone(frame), [](AVFrame* f) { av_frame_free(&f); });
                    frameIndex.back().type = FrameInfo::LOW_RES;
                    frameIndex.back().time_base = streamTimeBase;
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

bool decodeLowResRange(const char* filename, std::vector<FrameInfo>& frameIndex, int startFrame, int endFrame, int highResStart, int highResEnd, bool skipHighResWindow) {
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
        double frameDuration = av_q2d(formatContext->streams[videoStream]->avg_frame_rate);
        frameDuration = 1.0 / frameDuration;

        int64_t startTime = frameIndex[threadStartFrame].time_ms;
        av_seek_frame(formatContext, videoStream, startTime * timeBase.den / (timeBase.num * 1000), AVSEEK_FLAG_BACKWARD);
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

                    double frameTime = frame->pts * av_q2d(timeBase);
                    int64_t frameTimeMs = frameTime * 1000;

                    if (frameTimeMs >= startTime && currentFrame <= threadEndFrame && currentFrame < frameIndex.size()) {
                        // Вместо пропуска кадров в окне high-res, проверяем наличие low-res кадра
                        std::lock_guard<std::mutex> lock(frameIndex[currentFrame].mutex);
                        
                        // Декодируем low-res кадр, если его еще нет, независимо от того, находится ли кадр в окне high-res
                        if (!frameIndex[currentFrame].low_res_frame) {
                            frameIndex[currentFrame].low_res_frame = std::shared_ptr<AVFrame>(av_frame_clone(frame), [](AVFrame* f) { av_frame_free(&f); });
                            frameIndex[currentFrame].pts = frame->pts;
                            frameIndex[currentFrame].relative_pts = frame->pts - formatContext->streams[videoStream]->start_time;
                            frameIndex[currentFrame].time_ms = frameTimeMs;
                            frameIndex[currentFrame].time_base = timeBase;
                            
                            // Обновляем тип только если текущий тип EMPTY
                            if (frameIndex[currentFrame].type == FrameInfo::EMPTY) {
                                frameIndex[currentFrame].type = FrameInfo::LOW_RES;
                            }
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

// In removeHighResFrames function
void removeHighResFrames(std::vector<FrameInfo>& frameIndex, int start, int end, int highResStart, int highResEnd) {
    for (int i = start; i <= end && i < frameIndex.size(); ++i) {
        if (frameIndex[i].type == FrameInfo::FULL_RES) {
            // Add check to not remove frames in the current high-res zone
            if (i < highResStart || i > highResEnd) {
                // std::cout << "Removing high-res frame " << i << std::endl;
                frameIndex[i].frame.reset();
                // Проверяем наличие low_res или cached кадров
                if (frameIndex[i].low_res_frame) {
                    frameIndex[i].type = FrameInfo::LOW_RES;
                } else if (frameIndex[i].cached_frame) {
                    frameIndex[i].type = FrameInfo::CACHED;
                } else {
                    frameIndex[i].type = FrameInfo::EMPTY;
                }
            }
        }
    }
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
            // Не удаляем cached_frame
            if (frameIndex[i].cached_frame) {
                frameIndex[i].type = FrameInfo::CACHED;
            } else {
                frameIndex[i].type = FrameInfo::EMPTY;
            }
        }
    }

std::future<void> asyncDecodeLowResRange(const char* filename, std::vector<FrameInfo>& frameIndex, int startFrame, int endFrame, int highResStart, int highResEnd, bool skipHighResWindow) {
    return std::async(std::launch::async, [=, &frameIndex]() {
        decodeLowResRange(filename, frameIndex, startFrame, endFrame, highResStart, highResEnd, skipHighResWindow);
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

bool decodeCachedFrames(const char* filename, std::vector<FrameInfo>& frameIndex, int step) {
    // Проверяем, все ли кадры уже кэшированы
    bool allFramesCached = true;
    int totalFrames = frameIndex.size();
    int cachedCount = 0;
    
    // Подсчитываем количество уже кэшированных кадров
    for (int i = 0; i < totalFrames; i += step) {
        if (i < frameIndex.size()) {
            std::lock_guard<std::mutex> lock(frameIndex[i].mutex);
            if (frameIndex[i].cached_frame) {
                cachedCount++;
            } else {
                allFramesCached = false;
            }
        }
    }
    
    // Если все кадры уже кэшированы, выходим
    if (allFramesCached) {
        std::cout << "All frames already cached (" << cachedCount << " frames). Skipping cache update." << std::endl;
        return true;
    }
    
    // Если кэшировано более 95% кадров, тоже выходим
    int expectedCachedFrames = (totalFrames + step - 1) / step; // Округление вверх
    if (cachedCount >= expectedCachedFrames * 0.95) {
        std::cout << "Most frames already cached (" << cachedCount << " of " << expectedCachedFrames << "). Skipping cache update." << std::endl;
        return true;
    }
    
    // Продолжаем с обычным кэшированием
    AVFormatContext* formatContext = nullptr;
    AVCodecContext* codecContext = nullptr;
    const AVCodec* codec = nullptr;
    int videoStream;

    if (avformat_open_input(&formatContext, filename, nullptr, nullptr) != 0) {
        std::cerr << "Failed to open file for cached frames" << std::endl;
        return false;
    }

    if (avformat_find_stream_info(formatContext, nullptr) < 0) {
        std::cerr << "Failed to find stream information for cached frames" << std::endl;
        avformat_close_input(&formatContext);
        return false;
    }

    videoStream = av_find_best_stream(formatContext, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (videoStream < 0) {
        std::cerr << "Video stream not found for cached frames" << std::endl;
        avformat_close_input(&formatContext);
        return false;
    }

    // Получаем FPS видео
    AVRational frameRate = formatContext->streams[videoStream]->avg_frame_rate;
    double fps = av_q2d(frameRate);
    
    // Адаптируем шаг кэширования в зависимости от FPS
    int adaptedStep = step;
    if (fps > 0) {
        // Устанавливаем шаг в зависимости от FPS
        if (fps >= 59.0 && fps <= 60.0) {
            adaptedStep = 12; // Для 60 fps (и 59.94)
        } else if (fps >= 49.0 && fps <= 50.0) {
            adaptedStep = 10; // Для 50 fps
        } else if (fps >= 29.0 && fps <= 30.0) {
            adaptedStep = 6;  // Для 30 fps (и 29.97)
        } else if (fps >= 24.0 && fps <= 25.0) {
            adaptedStep = 5;  // Для 25 fps и 24 fps
        } else if (fps >= 23.0 && fps < 24.0) {
            adaptedStep = 4;  // Для 23.976 fps
        } else {
            // Для других значений FPS используем формулу
            adaptedStep = static_cast<int>(fps / 5.0);
            adaptedStep = std::max(3, adaptedStep); // Минимум 3 кадра
            adaptedStep = std::min(15, adaptedStep); // Максимум 15 кадров
        }
        
        std::cout << "Video FPS: " << fps << ", adapted cache step: " << adaptedStep << std::endl;
    }

    codecContext = avcodec_alloc_context3(codec);
    if (!codecContext) {
        avformat_close_input(&formatContext);
        return false;
    }

    codecContext->thread_count = std::thread::hardware_concurrency();
    codecContext->thread_type = FF_THREAD_FRAME;

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

    AVRational timeBase = formatContext->streams[videoStream]->time_base;
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();

    int frameCount = 0;
    while (av_read_frame(formatContext, packet) >= 0 && !shouldExit) {
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

                // Используем адаптированный шаг вместо фиксированного
                if (frameCount % adaptedStep == 0 && frameCount < frameIndex.size()) {
                    double frameTime = frame->pts * av_q2d(timeBase);
                    int64_t frameTimeMs = frameTime * 1000;

                    std::lock_guard<std::mutex> lock(frameIndex[frameCount].mutex);
                    if (!frameIndex[frameCount].cached_frame) {
                        frameIndex[frameCount].cached_frame = std::shared_ptr<AVFrame>(av_frame_clone(frame), [](AVFrame* f) { av_frame_free(&f); });
                        frameIndex[frameCount].pts = frame->pts;
                        frameIndex[frameCount].relative_pts = frame->pts - formatContext->streams[videoStream]->start_time;
                        frameIndex[frameCount].time_ms = frameTimeMs;
                        frameIndex[frameCount].time_base = timeBase;
                        
                        // Устанавливаем тип CACHED только если нет кадра более высокого качества
                        if (frameIndex[frameCount].type == FrameInfo::EMPTY) {
                            frameIndex[frameCount].type = FrameInfo::CACHED;
                        }
                    }
                }
                frameCount++;
                if (frameCount % 100 == 0) {
                    std::cout << "Processed " << frameCount << " frames for caching\r" << std::flush;
                }
            }
        }
        av_packet_unref(packet);
    }

    std::cout << "\nTotal processed " << frameCount << " frames for caching" << std::endl;

    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&codecContext);
    avformat_close_input(&formatContext);

    return true;
}

std::future<void> asyncDecodeCachedFrames(const char* filename, std::vector<FrameInfo>& frameIndex, int step) {
    return std::async(std::launch::async, [=, &frameIndex]() {
        decodeCachedFrames(filename, frameIndex, step);
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

// Add this new function after the main function
void manageVideoDecoding(const std::string& filename, const std::string& lowResFilename, 
                         std::vector<FrameInfo>& frameIndex, std::atomic<int>& currentFrame,
                         const size_t ringBufferCapacity, const int highResWindowSize,
                         std::atomic<bool>& isPlaying) {


    std::chrono::steady_clock::time_point lastFrameTime;
    std::chrono::steady_clock::time_point lastBufferUpdateTime = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point now;

    std::future<void> lowResFuture;
    std::future<void> highResFuture;
    std::future<void> cachedFramesFuture;
    std::future<void> secondaryCleanFuture;

    std::chrono::steady_clock::time_point lastLowResUpdateTime = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point lastHighResUpdateTime = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point lastCachedUpdateTime = std::chrono::steady_clock::now();
    bool cachedFramesInitialized = false;

    bool was_speed_reset_requested = false;

    auto getUpdateInterval = [](double playbackRate) {
        if (playbackRate < 0.9) return std::numeric_limits<int>::max(); // Don't decode
        if (playbackRate >= 12.0) return std::numeric_limits<int>::max(); // Don't decode at high speeds (changed from 10.0 to 12.0)
        if (playbackRate <= 1.0) return 8000; // Еще больше уменьшаем интервал для нормальной скорости
        if (playbackRate <= 2.0) return 6000;  // Уменьшаем интервал для скорости до 2x
        if (playbackRate <= 4.0) return 3000;  // Уменьшаем интервал для скорости до 4x
        if (playbackRate <= 8.0) return 1250;  // Уменьшаем интервал для скорости до 8x
        return 1000; // Уменьшаем интервал для скорости 8x-12x
    };

    // Инициализация кэшированных кадров при запуске
    if (!cachedFramesFuture.valid() || cachedFramesFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        std::cout << "Initializing cached frames..." << std::endl;
        // Используем адаптивный шаг, основанный на FPS видео
        int adaptiveStep = 10;  // Значение по умолчанию
        double fps = original_fps.load();
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
        cachedFramesFuture = asyncDecodeCachedFrames(lowResFilename.c_str(), frameIndex, adaptiveStep);
    }

    // Добавим флаг, который будет отслеживать, были ли кадры полностью кэшированы
    bool fullyCached = false;

    while (!shouldExit) {
        int currentFrameValue = currentFrame.load();
        double currentTime = current_audio_time.load();
        double currentPlaybackRate = std::abs(playback_rate.load());

        if (isPlaying.load()) {
            now = std::chrono::steady_clock::now();
            auto frameDuration = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastFrameTime);
            double fps = 1000.0 / frameDuration.count();
        }

        // Обработка запроса на поиск
        if (seekInfo.requested.load()) {
            double seekTime = seekInfo.time.load();
            int seekFrame = static_cast<int>(seekTime * original_fps.load()) % frameIndex.size();
            
            // Очистка буфера
            int clearStart = std::max(0, seekFrame - static_cast<int>(ringBufferCapacity / 2));
            int clearEnd = std::min(clearStart + static_cast<int>(ringBufferCapacity) - 1, static_cast<int>(frameIndex.size()) - 1);
            for (int i = clearStart; i <= clearEnd; ++i) {
                frameIndex[i].frame.reset();
                frameIndex[i].low_res_frame.reset();
                // Не очищаем cached_frame при поиске
                if (frameIndex[i].type != FrameInfo::CACHED) {
                    frameIndex[i].type = FrameInfo::EMPTY;
                }
            }

            currentFrame.store(seekFrame);
            current_audio_time.store(seekTime);
            seekInfo.requested.store(false);
            seekInfo.completed.store(true);

            // Форсируем декодирование кадров после seek
            // Отключаем декодирование low-res кадров при скорости 10x и выше
            if (currentPlaybackRate < 10.0) {
                // Всегда декодируем low-res кадры, даже в окне high-res
                lowResFuture = asyncDecodeLowResRange(lowResFilename.c_str(), frameIndex, clearStart, clearEnd, seekFrame, seekFrame, false);
            }
            if (currentPlaybackRate < 2.0) {
                highResFuture = asyncDecodeFrameRange(filename.c_str(), frameIndex, seekFrame, std::min(seekFrame + highResWindowSize, static_cast<int>(frameIndex.size()) - 1));
            }
        }

        // Обработка запроса на сброс скорости
        if (speed_reset_requested.load()) {
            was_speed_reset_requested = true;
            speed_reset_requested.store(false);
            
            // Принудительное обновление low-res буфера для текущего кадра
            int currentFrameValue = currentFrame.load();
            std::cout << "Speed reset requested, forcing low-res decoding around frame " << currentFrameValue << std::endl;
            
            // Расширенный диапазон буфера при сбросе скорости
            int resetBufferStart = std::max(0, currentFrameValue - static_cast<int>(ringBufferCapacity * 0.5));
            int resetBufferEnd = std::min(static_cast<int>(frameIndex.size()) - 1, 
                                       currentFrameValue + static_cast<int>(ringBufferCapacity * 0.5));
            
            // Форсировать декодирование в фоновом режиме
            lowResFuture = asyncDecodeLowResRange(lowResFilename.c_str(), frameIndex, 
                                                 resetBufferStart, resetBufferEnd, 
                                                 currentFrameValue, currentFrameValue, true);
            
            // Также запускаем декодирование high-res для более плавного перехода
            int highResStart = std::max(0, currentFrameValue - highResWindowSize / 2);
            int highResEnd = std::min(static_cast<int>(frameIndex.size()) - 1, 
                                   currentFrameValue + highResWindowSize / 2);
            
            highResFuture = asyncDecodeFrameRange(filename.c_str(), frameIndex, highResStart, highResEnd);
            
            lastLowResUpdateTime = std::chrono::steady_clock::now();
            lastHighResUpdateTime = std::chrono::steady_clock::now();
        }

        // После цикла if (was_speed_reset_requested) с небольшой задержкой сбрасываем флаг
        if (was_speed_reset_requested) {
            static auto reset_start_time = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - reset_start_time).count();
            
            if (elapsed > 2000) { // 2 секунды для декодирования
                was_speed_reset_requested = false;
                reset_start_time = std::chrono::steady_clock::now();
            }
        }

        // Обновляем currentFrame на основе текущего времени аудио
        int newCurrentFrame = static_cast<int>(currentTime * original_fps.load()) % frameIndex.size();
        currentFrame.store(newCurrentFrame);

        // Обновление интервалов декодирования
        int lowResUpdateInterval = getUpdateInterval(currentPlaybackRate);
        int highResUpdateInterval = lowResUpdateInterval;
        int cachedUpdateInterval = 60000; // Обновление кэшированных кадров каждую минуту

        // Буфер обновления
        int bufferStart = std::max(0, currentFrameValue - static_cast<int>(ringBufferCapacity / 2));
        int bufferEnd = std::min(bufferStart + static_cast<int>(ringBufferCapacity) - 1, static_cast<int>(frameIndex.size()) - 1);

        // Определение границ окна высокого разршения
        int highResStart = std::max(0, currentFrameValue - highResWindowSize / 2);
        int highResEnd = std::min(static_cast<int>(frameIndex.size()) - 1, currentFrameValue + highResWindowSize / 2);

        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        auto timeSinceLastLowResUpdate = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastLowResUpdateTime);
        auto timeSinceLastHighResUpdate = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastHighResUpdateTime);
        auto timeSinceLastCachedUpdate = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastCachedUpdateTime);

        // Проверяем, нужно ли очистить low-res кадры при высокой скорости
        static bool wasHighSpeed = false;
        bool isHighSpeed = currentPlaybackRate >= 12.0;
        
        if (isHighSpeed && !wasHighSpeed) {
            // Переход на высокую скорость - очищаем low-res кадры
            std::cout << "Switching to high speed mode, clearing low-res frames..." << std::endl;
            for (int i = 0; i < frameIndex.size(); ++i) {
                if (frameIndex[i].type == FrameInfo::LOW_RES && !frameIndex[i].is_decoding) {
                    frameIndex[i].low_res_frame.reset();
                    // Проверяем наличие cached кадров
                    if (frameIndex[i].cached_frame) {
                        frameIndex[i].type = FrameInfo::CACHED;
                    } else {
                        frameIndex[i].type = FrameInfo::EMPTY;
                    }
                }
            }
        }
        wasHighSpeed = isHighSpeed;

        // Проверка и инициализация кэшированных кадров
        if (!cachedFramesInitialized) {
            if (cachedFramesFuture.valid() && cachedFramesFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                std::cout << "Cached frames initialization completed" << std::endl;
                cachedFramesInitialized = true;
            }
        }

        // Периодическое обновление кэшированных кадров
        if (cachedFramesInitialized && !fullyCached && timeSinceLastCachedUpdate.count() >= cachedUpdateInterval) {
            if (!cachedFramesFuture.valid() || cachedFramesFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                // Проверяем, нужно ли обновлять кэш
                bool needsUpdate = false;
                
                // Проверяем, есть ли некэшированные кадры
                int step = 10; // Базовый шаг
                double fps = original_fps.load();
                if (fps > 0) {
                    // Устанавливаем шаг в зависимости от FPS
                    if (fps >= 59.0 && fps <= 60.0) {
                        step = 12;
                    } else if (fps >= 49.0 && fps <= 50.0) {
                        step = 10;
                    } else if (fps >= 29.0 && fps <= 30.0) {
                        step = 6;
                    } else if (fps >= 24.0 && fps <= 25.0) {
                        step = 5;
                    } else if (fps >= 23.0 && fps < 24.0) {
                        step = 4;
                    } else {
                        step = static_cast<int>(fps / 5.0);
                        step = std::max(3, step);
                        step = std::min(15, step);
                    }
                }
                
                // Проверяем выборочно кадры
                int sampleSize = std::min(100, static_cast<int>(frameIndex.size()));
                int uncachedCount = 0;
                
                for (int i = 0; i < sampleSize; i++) {
                    int frameIdx = (i * frameIndex.size()) / sampleSize;
                    if (frameIdx % step == 0) {
                        std::lock_guard<std::mutex> lock(frameIndex[frameIdx].mutex);
                        if (!frameIndex[frameIdx].cached_frame) {
                            uncachedCount++;
                        }
                    }
                }
                
                // Если более 5% выборки не кэшировано, обновляем кэш
                if (uncachedCount > sampleSize * 0.05) {
                    std::cout << "Updating cached frames... (" << uncachedCount << " uncached frames in sample)" << std::endl;
                    
                    int adaptiveStep = step;
                    cachedFramesFuture = asyncDecodeCachedFrames(lowResFilename.c_str(), frameIndex, adaptiveStep);
                    lastCachedUpdateTime = now;
                } else {
                    std::cout << "Cache is up to date. No update needed." << std::endl;
                    fullyCached = true; // Помечаем, что кэш полный
                }
            }
        }

        // Low-res frame decoding
        if (timeSinceLastLowResUpdate.count() >= lowResUpdateInterval) {
            // Отключаем декодирование low-res кадров при скорости 12x и выше (изменено с 10x на 12x)
            if (currentPlaybackRate < 12.0) {
                if (!lowResFuture.valid() || lowResFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                    // Расширяем буфер для low-res кадров при нормальной скорости
                    int extendedBufferStart = bufferStart;
                    int extendedBufferEnd = bufferEnd;
                    
                    // Увеличиваем размер буфера для всех скоростей до 8x
                    if (currentPlaybackRate <= 8.0) {
                        // Чем ниже скорость, тем больше буфер
                        float bufferMultiplier = 1.0f;
                        if (currentPlaybackRate <= 1.0) {
                            bufferMultiplier = 0.5f; // 50% от ringBufferCapacity в каждую сторону
                        } else if (currentPlaybackRate <= 2.0) {
                            bufferMultiplier = 0.4f; // 40% от ringBufferCapacity в каждую сторону
                        } else if (currentPlaybackRate <= 4.0) {
                            bufferMultiplier = 0.3f; // 30% от ringBufferCapacity в каждую сторону
                        } else {
                            bufferMultiplier = 0.2f; // 20% от ringBufferCapacity в каждую сторону
                        }
                        
                        // Учитываем направление движения для асимметричного буфера
                        bool isForward = playback_rate.load() >= 0;
                        if (isForward) {
                            // Больше кадров впереди, чем позади
                            extendedBufferStart = std::max(0, bufferStart - static_cast<int>(ringBufferCapacity * bufferMultiplier * 0.3f));
                            extendedBufferEnd = std::min(static_cast<int>(frameIndex.size()) - 1, bufferEnd + static_cast<int>(ringBufferCapacity * bufferMultiplier * 0.7f));
                        } else {
                            // Больше кадров позади, чем впереди
                            extendedBufferStart = std::max(0, bufferStart - static_cast<int>(ringBufferCapacity * bufferMultiplier * 0.7f));
                            extendedBufferEnd = std::min(static_cast<int>(frameIndex.size()) - 1, bufferEnd + static_cast<int>(ringBufferCapacity * bufferMultiplier * 0.3f));
                        }
                    }
                    
                    // Всегда декодируем low-res кадры, даже в окне high-res
                    lowResFuture = asyncDecodeLowResRange(lowResFilename.c_str(), frameIndex, extendedBufferStart, extendedBufferEnd, highResStart, highResEnd, false);
                    lastLowResUpdateTime = now;
                }
            }
        }

        // High-res frame decoding
        bool shouldDecodeHighRes = currentPlaybackRate < 2.0;
        if (shouldDecodeHighRes && timeSinceLastHighResUpdate.count() >= highResUpdateInterval) {
            bool needHighResUpdate = false;
            for (int i = highResStart; i <= highResEnd; ++i) {
                if (frameIndex[i].type != FrameInfo::FULL_RES) {
                    needHighResUpdate = true;
                    break;
                }
            }

            if (needHighResUpdate) {
                if (!highResFuture.valid() || highResFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                    highResFuture = asyncDecodeFrameRange(filename.c_str(), frameIndex, highResStart, highResEnd);
                    lastHighResUpdateTime = now;
                }
            }
        } else if (currentPlaybackRate >= 2.0) {
            removeHighResFrames(frameIndex, 0, frameIndex.size() - 1, -1, -1);
        }

        // Remove high-res frames outside the window
        removeHighResFrames(frameIndex, 0, highResStart - 1, highResStart, highResEnd);
        removeHighResFrames(frameIndex, highResEnd + 1, frameIndex.size() - 1, highResStart, highResEnd);

        // Check and reset frame type if it's outside the high-res window
        for (int i = 0; i < frameIndex.size(); ++i) {
            if (i < highResStart || i > highResEnd) {
                if (frameIndex[i].type == FrameInfo::FULL_RES && !frameIndex[i].is_decoding) {
                    frameIndex[i].frame.reset();
                    // Проверяем наличие low_res или cached кадров
                    if (frameIndex[i].low_res_frame) {
                        frameIndex[i].type = FrameInfo::LOW_RES;
                    } else if (frameIndex[i].cached_frame) {
                        frameIndex[i].type = FrameInfo::CACHED;
                    } else {
                        frameIndex[i].type = FrameInfo::EMPTY;
                    }
                }
            }
        }

        // Clear low-res frames outside the buffer
        for (int i = 0; i < frameIndex.size(); ++i) {
            if (i < bufferStart || i > bufferEnd) {
                if (frameIndex[i].type == FrameInfo::LOW_RES && !frameIndex[i].is_decoding) {
                    frameIndex[i].low_res_frame.reset();
                    // Проверяем наличие cached кадров
                    if (frameIndex[i].cached_frame) {
                        frameIndex[i].type = FrameInfo::CACHED;
                    } else {
                        frameIndex[i].type = FrameInfo::EMPTY;
                    }
                }
            }
        }

        // Обновляем промежуточный буфер с текущим кадром
        if (currentFrameValue >= 0 && currentFrameValue < frameIndex.size()) {
            // При высоких скоростях (>= 10x) ищем ближайший кэшированный кадр
            if (currentPlaybackRate >= 10.0) {
                // Определяем направление движения
                bool isForward = playback_rate.load() >= 0;
                int step = isForward ? 1 : -1;
                int searchRange = 20; // Диапазон поиска кэшированных кадров
                int foundCachedFrame = -1;
                
                // Ищем ближайший кэшированный кадр в направлении движения
                for (int i = 0; i < searchRange; ++i) {
                    int checkFrame = currentFrameValue + (i * step);
                    
                    // Проверяем границы
                    if (checkFrame < 0 || checkFrame >= frameIndex.size()) {
                        continue;
                    }
                    
                    std::lock_guard<std::mutex> lock(frameIndex[checkFrame].mutex);
                    if (!frameIndex[checkFrame].is_decoding && 
                        frameIndex[checkFrame].cached_frame && 
                        (frameIndex[checkFrame].type == FrameInfo::CACHED || 
                         frameIndex[checkFrame].type == FrameInfo::LOW_RES || 
                         frameIndex[checkFrame].type == FrameInfo::FULL_RES)) {
                        foundCachedFrame = checkFrame;
                        break;
                    }
                }
                
                // Если нашли кэшированный кадр, используем его
                if (foundCachedFrame != -1) {
                    std::lock_guard<std::mutex> lock(frameIndex[foundCachedFrame].mutex);
                    if (frameIndex[foundCachedFrame].cached_frame) {
                        frameBuffer.updateFrame(
                            frameIndex[foundCachedFrame].cached_frame, 
                            foundCachedFrame, 
                            FrameInfo::CACHED, 
                            frameIndex[foundCachedFrame].time_base
                        );
                    }
                } else {
                    // Если не нашли кэшированный кадр, используем текущий кадр
                    const FrameInfo& currentFrameInfo = frameIndex[currentFrameValue];
                    std::lock_guard<std::mutex> lock(currentFrameInfo.mutex);
                    
                    if (!currentFrameInfo.is_decoding) {
                        // Сохраняем текущий тип кадра для отслеживания изменений
                        static FrameInfo::FrameType lastType = FrameInfo::EMPTY;
                        FrameInfo::FrameType currentType = currentFrameInfo.type;
                        
                        // Приоритет выбора кадра зависит от скорости
                        if (currentPlaybackRate <= 2.0) {
                            // При низких скоростях предпочитаем full-res, затем low-res, затем cached
                            if (currentFrameInfo.type == FrameInfo::FULL_RES && currentFrameInfo.frame) {
                                frameBuffer.updateFrame(currentFrameInfo.frame, currentFrameValue, FrameInfo::FULL_RES, currentFrameInfo.time_base);
                                lastType = FrameInfo::FULL_RES;
                            } else if (currentFrameInfo.type == FrameInfo::LOW_RES && currentFrameInfo.low_res_frame) {
                                frameBuffer.updateFrame(currentFrameInfo.low_res_frame, currentFrameValue, FrameInfo::LOW_RES, currentFrameInfo.time_base);
                                lastType = FrameInfo::LOW_RES;
                            }
                        } else if (currentPlaybackRate <= 8.0) {
                            // При средних скоростях предпочитаем low-res, затем cached, затем full-res
                            if (currentFrameInfo.type == FrameInfo::LOW_RES && currentFrameInfo.low_res_frame) {
                                frameBuffer.updateFrame(currentFrameInfo.low_res_frame, currentFrameValue, FrameInfo::LOW_RES, currentFrameInfo.time_base);
                                lastType = FrameInfo::LOW_RES;
                            }
                        } else {
                            // При высоких скоростях (8x-10x) предпочитаем cached, затем low-res
                            if (currentFrameInfo.type == FrameInfo::CACHED && currentFrameInfo.cached_frame) {
                                frameBuffer.updateFrame(currentFrameInfo.cached_frame, currentFrameValue, FrameInfo::CACHED, currentFrameInfo.time_base);
                                lastType = FrameInfo::CACHED;
                            }
                        }
                        
                        // Если произошло изменение типа кадра, выводим информацию в консоль для отладки
                        if (lastType != currentType && currentType != FrameInfo::EMPTY) {
                            std::string typeStr;
                            switch (currentType) {
                                case FrameInfo::FULL_RES: typeStr = "FULL_RES"; break;
                                case FrameInfo::LOW_RES: typeStr = "LOW_RES"; break;
                                case FrameInfo::CACHED: typeStr = "CACHED"; break;
                                default: typeStr = "UNKNOWN"; break;
                            }
                            std::cout << "Frame type changed to: " << typeStr << " at frame " << currentFrameValue << std::endl;
                        }
                    }
                }
            } else {
                // Улучшенная логика выбора кадра для средних скоростей (1x-10x)
                const FrameInfo& currentFrameInfo = frameIndex[currentFrameValue];
                std::lock_guard<std::mutex> lock(currentFrameInfo.mutex);
                
                if (!currentFrameInfo.is_decoding) {
                    // Сохраняем текущий тип кадра для отслеживания изменений
                    static FrameInfo::FrameType lastType = FrameInfo::EMPTY;
                    FrameInfo::FrameType currentType = currentFrameInfo.type;
                    
                    // Приоритет выбора кадра зависит от скорости
                    if (currentPlaybackRate <= 2.0) {
                        // При низких скоростях предпочитаем full-res, затем low-res, затем cached
                        if (currentFrameInfo.type == FrameInfo::FULL_RES && currentFrameInfo.frame) {
                            frameBuffer.updateFrame(currentFrameInfo.frame, currentFrameValue, FrameInfo::FULL_RES, currentFrameInfo.time_base);
                            lastType = FrameInfo::FULL_RES;
                        } else if (currentFrameInfo.type == FrameInfo::LOW_RES && currentFrameInfo.low_res_frame) {
                            frameBuffer.updateFrame(currentFrameInfo.low_res_frame, currentFrameValue, FrameInfo::LOW_RES, currentFrameInfo.time_base);
                            lastType = FrameInfo::LOW_RES;
                        }
                    } else if (currentPlaybackRate <= 8.0) {
                        // При средних скоростях предпочитаем low-res, затем cached, затем full-res
                        if (currentFrameInfo.type == FrameInfo::LOW_RES && currentFrameInfo.low_res_frame) {
                            frameBuffer.updateFrame(currentFrameInfo.low_res_frame, currentFrameValue, FrameInfo::LOW_RES, currentFrameInfo.time_base);
                            lastType = FrameInfo::LOW_RES;
                        }
                    } else {
                        // При высоких скоростях (8x-10x) предпочитаем cached, затем low-res
                        if (currentFrameInfo.type == FrameInfo::CACHED && currentFrameInfo.cached_frame) {
                            frameBuffer.updateFrame(currentFrameInfo.cached_frame, currentFrameValue, FrameInfo::CACHED, currentFrameInfo.time_base);
                            lastType = FrameInfo::CACHED;
                        }
                    }
                    
                    // Если произошло изменение типа кадра, выводим информацию в консоль для отладки
                    if (lastType != currentType && currentType != FrameInfo::EMPTY) {
                        std::string typeStr;
                        switch (currentType) {
                            case FrameInfo::FULL_RES: typeStr = "FULL_RES"; break;
                            case FrameInfo::LOW_RES: typeStr = "LOW_RES"; break;
                            case FrameInfo::CACHED: typeStr = "CACHED"; break;
                            default: typeStr = "UNKNOWN"; break;
                        }
                        std::cout << "Frame type changed to: " << typeStr << " at frame " << currentFrameValue << std::endl;
                    }
                }
            }
        }

        // Небольшая задержка для предотвращения активного ожидания
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Wait for any ongoing decoding to finish before exiting
    if (lowResFuture.valid()) lowResFuture.wait();
    if (highResFuture.valid()) highResFuture.wait();
    if (cachedFramesFuture.valid()) cachedFramesFuture.wait();
    
    // Очищаем ресурсы отображения
    cleanupDisplayResources();
}

// Функция для проверки, является ли строка URL
bool isURL(const std::string& str) {
    std::regex url_regex(
        R"(https?:\/\/(www\.)?[-a-zA-Z0-9@:%._\+~#=]{1,256}\.[a-zA-Z0-9()]{1,6}\b([-a-zA-Z0-9()@:%_\+.~#?&//=]*))",
        std::regex::icase
    );
    return std::regex_match(str, url_regex);
}

// Добавляем функцию для генерации ID для URL
std::string generateURLId(const std::string& url) {
    EVP_MD_CTX *mdctx;
    const EVP_MD *md;
    unsigned char md_value[EVP_MAX_MD_SIZE];
    unsigned int md_len;
    
    md = EVP_md5();
    mdctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(mdctx, md, NULL);
    
    // Используем саму строку URL как входные данные для хеша
    EVP_DigestUpdate(mdctx, url.c_str(), url.length());
    
    EVP_DigestFinal_ex(mdctx, md_value, &md_len);
    EVP_MD_CTX_free(mdctx);
    
    char md5string[33];
    for(unsigned int i = 0; i < md_len; i++)
        snprintf(&md5string[i*2], 3, "%02x", (unsigned int)md_value[i]);
    
    return std::string(md5string);
}

// Модифицируем функцию загрузки видео
bool downloadVideoFromURL(const std::string& url, std::string& outputFilename) {
    std::string tempDir = getCachePath() + "/temp_downloads";
    fs::create_directories(tempDir);
    
    // Используем новую функцию для URL
    std::string fileId = generateURLId(url);
    
    std::string outputPath = tempDir + "/" + fileId + ".mp4";
    
    // Проверяем, существует ли уже загруженный файл
    if (fs::exists(outputPath)) {
        std::cout << "Found previously downloaded file: " << outputPath << std::endl;
        outputFilename = outputPath;
        return true;
    }
    
    // Формируем команду для yt-dlp
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "yt-dlp -f \"bestvideo[ext=mp4][height<=1080]+bestaudio[ext=m4a]/best[ext=mp4]/best\" "
             "--merge-output-format mp4 "
             "--no-playlist "
             "--no-mtime "
             "-o \"%s\" "
             "\"%s\"",
             outputPath.c_str(), url.c_str());
    
    std::cout << "Starting video download from URL: " << url << std::endl;
    int result = system(cmd);
    if (result != 0) {
        std::cerr << "Error downloading video from URL" << std::endl;
        return false;
    }
    
    std::cout << "Video download completed successfully" << std::endl;
    outputFilename = outputPath;
    
    // Регистрируем файл для удаления при завершении программы
    registerTempFileForCleanup(outputPath);
    
    return true;
}

// Функция для регистрации временных файлов для удаления
std::vector<std::string> tempFilesToCleanup;

void registerTempFileForCleanup(const std::string& filePath) {
    tempFilesToCleanup.push_back(filePath);
}

// Функция для очистки временных файлов
void cleanupTempFiles() {
    for (const auto& filePath : tempFilesToCleanup) {
        if (fs::exists(filePath)) {
            try {
                fs::remove(filePath);
                std::cout << "Removed temporary file: " << filePath << std::endl;
            } catch (const std::exception& e) {
                std::cerr << "Error removing temporary file: " << filePath << " - " << e.what() << std::endl;
            }
        }
    }
    tempFilesToCleanup.clear();
}

// Модифицированная функция для обработки как файлов, так и URL с поддержкой отслеживания прогресса
bool processMediaSource(const std::string& source, std::string& processedFilePath, ProgressCallback progressCallback) {
    // Проверяем, является ли источник URL
    if (isURL(source)) {
        // Если это URL, загружаем видео с отслеживанием прогресса
        if (!downloadVideoFromURL(source, processedFilePath)) {
            return false;
        }
        
        // Если загрузка успешна и есть callback, сообщаем о завершении
        if (progressCallback) {
            progressCallback(100);
        }
    } else {
        // Если это локальный файл, просто используем его путь
        processedFilePath = source;
        
        // Для локальных файлов прогресс мгновенный
        if (progressCallback) {
            progressCallback(100);
        }
    }
    
    return true;
}

// Original version without progress callback for backward compatibility
bool convertToLowRes(const char* filename, std::string& outputFilename) {
    // Call the version with progress callback but pass nullptr
    return convertToLowRes(filename, outputFilename, nullptr);
}

// Original version without progress callback for backward compatibility
bool processMediaSource(const std::string& source, std::string& processedFilePath) {
    // Call the version with progress callback but pass nullptr
    return processMediaSource(source, processedFilePath, nullptr);
}
