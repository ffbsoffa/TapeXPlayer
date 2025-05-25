#include "decode.h"
#include "common.h"
#include "../display/display.h"
#include "full_res_decoder.h"
#include "low_cached_decoder_manager.h"
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
#include <memory>
#include <atomic>
#include <future>
#include <chrono>
#include <mutex>
#include <limits>
#include <cmath>
#include <iomanip>

namespace fs = std::filesystem;

// Global intermediate buffer
FrameBuffer frameBuffer;

extern std::atomic<bool> speed_reset_requested;

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

    // Check for HEVC/H.265 codec
    AVCodecParameters* codecParams = formatContext->streams[videoStream]->codecpar;
    if (codecParams->codec_id == AV_CODEC_ID_HEVC) {
        std::cerr << "HEVC/H.265 video format is not supported due to high CPU usage." << std::endl;
        std::cerr << "Please convert the video to H.264 format before loading." << std::endl;
        avformat_close_input(&formatContext);
        return frameIndex;
    }

    AVRational timeBase = formatContext->streams[videoStream]->time_base;
    int64_t startTime = formatContext->streams[videoStream]->start_time;

    // Diagnostic info (reduced logging)
    std::cout << "[TIMESTAMP] Time base: " << timeBase.num << "/" << timeBase.den;
    if (startTime != AV_NOPTS_VALUE) {
        double startTimeMs = av_rescale_q(startTime, timeBase, {1, 1000});
        std::cout << ", Start: " << startTimeMs << "ms";
    }
    std::cout << std::endl;

    AVPacket packet;
    int packetCount = 0;
    int64_t firstPts = AV_NOPTS_VALUE, lastPts = AV_NOPTS_VALUE;
    int64_t minPts = INT64_MAX, maxPts = INT64_MIN;
    
    // Temporary vector to collect all frames before sorting
    std::vector<FrameInfo> tempFrameIndex;
    
    while (av_read_frame(formatContext, &packet) >= 0) {
        if (packet.stream_index == videoStream) {
            FrameInfo info;
            // Use best_effort_timestamp first, fallback to pts (consistent with decoders)
            int64_t framePts = packet.pts; // For packets, pts is typically available
            if (framePts == AV_NOPTS_VALUE) {
                // Skip frames without valid PTS to avoid negative time_ms
                av_packet_unref(&packet);
                continue;
            }
            
            // Collect statistics
            if (firstPts == AV_NOPTS_VALUE) firstPts = framePts;
            lastPts = framePts;
            minPts = std::min(minPts, framePts);
            maxPts = std::max(maxPts, framePts);
            
            info.pts = framePts;
            info.relative_pts = framePts - startTime;
            
            // More precise frame timing calculation
            if (framePts != AV_NOPTS_VALUE) {
                // First convert to a common timebase (1/1000000 microseconds) for maximum precision
                int64_t pts_us = av_rescale_q(framePts, timeBase, {1, 1000000});
                int64_t start_us = (startTime != AV_NOPTS_VALUE) ? 
                    av_rescale_q(startTime, timeBase, {1, 1000000}) : 0;
                
                // Calculate relative time in microseconds
                int64_t relative_us = pts_us - start_us;
                
                // Convert to milliseconds with rounding
                info.time_ms = (relative_us + 500) / 1000;
            } else {
                info.time_ms = -1;
            }
            
            info.frame = nullptr;
            info.low_res_frame = nullptr;
            info.type = FrameInfo::EMPTY;
            info.time_base = timeBase;
            tempFrameIndex.push_back(info);
            
            // Log first few frames for diagnostic (before sorting) - reduced
            if (packetCount < 5) {
                double relativeTimeMs = av_rescale_q(info.relative_pts, timeBase, {1, 1000});
                std::cout << "  Frame " << packetCount << ": " << relativeTimeMs << "ms";
                if (packetCount == 4) std::cout << " (decode order)";
                std::cout << std::endl;
            }
            packetCount++;
        }
        av_packet_unref(&packet);
    }

    // Sort frames by presentation timestamp (time_ms) to handle B-frames correctly
    std::cout << "[TIMESTAMP FIX] Sorting " << tempFrameIndex.size() << " frames..." << std::endl;
    std::sort(tempFrameIndex.begin(), tempFrameIndex.end(), [](const FrameInfo& a, const FrameInfo& b) {
        return a.time_ms < b.time_ms;
    });
    
    // Move sorted frames to final frameIndex
    frameIndex = std::move(tempFrameIndex);
    
    // Log first few frames after sorting (reduced)
    std::cout << "[TIMESTAMP FIX] After sorting: ";
    for (int i = 0; i < std::min(5, (int)frameIndex.size()); i++) {
        std::cout << frameIndex[i].time_ms << "ms";
        if (i < 4 && i < frameIndex.size() - 1) std::cout << " → ";
    }
    std::cout << " (display order)" << std::endl;

    // Print summary (reduced)
    if (firstPts != AV_NOPTS_VALUE && lastPts != AV_NOPTS_VALUE) {
        double firstTimeMs = av_rescale_q(firstPts, timeBase, {1, 1000});
        double lastTimeMs = av_rescale_q(lastPts, timeBase, {1, 1000});
        double durationSec = (lastTimeMs - firstTimeMs) / 1000.0;
        std::cout << "[TIMESTAMP] " << frameIndex.size() << " frames, " 
                  << std::fixed << std::setprecision(1) << durationSec << "s duration" << std::endl;
    }
    
    // Check for timestamp consistency after sorting
    int inconsistentCount = 0;
    for (int i = 1; i < frameIndex.size(); i++) {
        if (frameIndex[i].time_ms < frameIndex[i-1].time_ms) {
            inconsistentCount++;
            if (inconsistentCount <= 5) { // Log first 5 inconsistencies
                std::cout << "  WARNING: Frame " << i << " still out of order: " 
                          << frameIndex[i].time_ms << "ms < " << frameIndex[i-1].time_ms << "ms" << std::endl;
            }
        }
    }
    if (inconsistentCount > 0) {
        std::cout << "  Total timestamp inconsistencies after sorting: " << inconsistentCount << std::endl;
    } else {
        std::cout << "  ✓ All timestamps are now in correct order!" << std::endl;
    }

    avformat_close_input(&formatContext);
    return frameIndex;
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

void printMemoryUsage() {
    // Implementation... (Keep this if still needed)
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
    std::string tempDir = LowResDecoder::getCachePath() + "/temp_downloads";
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
             "yt-dlp -f \"bestvideo[ext=mp4][height<=1080][vcodec!*=hevc][vcodec!*=h265]+bestaudio[ext=m4a]/best[ext=mp4][vcodec!*=hevc][vcodec!*=h265]/best[vcodec!*=hevc][vcodec!*=h265]\" "
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
    // Check if the source is a URL
    if (isURL(source)) {
        // Add yt-dlp format filter to exclude HEVC/H.265
        std::string formatFilter = "\"bestvideo[ext=mp4][height<=1080][vcodec!*=hevc][vcodec!*=h265]+bestaudio[ext=m4a]/best[ext=mp4][vcodec!*=hevc][vcodec!*=h265]/best[vcodec!*=hevc][vcodec!*=h265]\"";
        
        // If this is URL, download video with progress tracking
        if (!downloadVideoFromURL(source, processedFilePath)) {
            return false;
        }
        
        // If download successful and callback exists, report completion
        if (progressCallback) {
            progressCallback(100);
        }
    } else {
        // For local files, check HEVC before proceeding
        AVFormatContext* formatContext = nullptr;
        if (avformat_open_input(&formatContext, source.c_str(), nullptr, nullptr) == 0) {
            if (avformat_find_stream_info(formatContext, nullptr) >= 0) {
                int videoStream = av_find_best_stream(formatContext, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
                if (videoStream >= 0) {
                    AVCodecParameters* codecParams = formatContext->streams[videoStream]->codecpar;
                    if (codecParams->codec_id == AV_CODEC_ID_HEVC) {
                        std::cerr << "HEVC/H.265 video format is not supported due to high CPU usage." << std::endl;
                        std::cerr << "Please convert the video to H.264 format before loading." << std::endl;
                        avformat_close_input(&formatContext);
                        return false;
                    }
                }
            }
            avformat_close_input(&formatContext);
        }
        
        // If not HEVC, use the file path
        processedFilePath = source;
        
        // For local files progress is instant
        if (progressCallback) {
            progressCallback(100);
        }
    }
    
    return true;
}

// Original version without progress callback for backward compatibility
bool convertToLowRes(const char* filename, std::string& outputFilename) {
    // Call the version with progress callback but pass nullptr
    return LowResDecoder::convertToLowRes(filename, outputFilename, nullptr);
}

// Версия processMediaSource без колбэка прогресса
bool processMediaSource(const std::string& source, std::string& processedFilePath) {
    // Simply call the version with progress callback but pass nullptr
    return processMediaSource(source, processedFilePath, nullptr);
}

// Helper function to find the frame index closest to a given timestamp
// Performs a limited linear search around the estimated position
int findClosestFrameIndexByTime(const std::vector<FrameInfo>& frameIndex, int64_t target_ms) {
    if (frameIndex.empty() || target_ms < 0) {
        // std::cout << "[FRAME_SEARCH] Frame index empty or target_ms < 0. Returning 0." << std::endl;
        return 0; // Return first frame if index empty or time invalid
    }

    int bestMatchIndex = 0; // Default to first frame
    int64_t maxTimeMsLessOrEqual = -1; // Track the largest timestamp <= target_ms

    // Simple linear search for now (can be optimized later)
    int searchStart = 0;
    int searchEnd = frameIndex.size();

    // Count valid timestamps for diagnostic
    int validTimestamps = 0;
    int invalidTimestamps = 0;
    
    static int logCounter = 0;
    logCounter++;
    bool shouldLog = (logCounter % 100 == 0); // Log every 100th call to avoid spam

    for (int i = searchStart; i < searchEnd; ++i) {
        int64_t current_time_ms = frameIndex[i].time_ms;
        if (current_time_ms >= 0) { // Check if the frame has a valid timestamp
            validTimestamps++;
            // Find the latest frame where time_ms <= target_ms
            if (current_time_ms <= target_ms) {
                // Update if this frame is later than the previous best match
                if (current_time_ms >= maxTimeMsLessOrEqual) {
                    maxTimeMsLessOrEqual = current_time_ms;
                    bestMatchIndex = i;
                }
            } else {
                // Optimization: If frame timestamps are generally increasing,
                // and we've passed the target_ms, we can stop searching.
                if (shouldLog) {
                    // std::cout << "[FRAME_SEARCH] Optimization break at index " << i 
                    //          << " (time_ms: " << current_time_ms << " > target_ms: " << target_ms << ")" << std::endl;
                }
                break;
            }
        } else {
            invalidTimestamps++;
        }
    }

    // Enhanced debug logging
    if (shouldLog) {
        // std::cout << "[FRAME_SEARCH] Target: " << target_ms << "ms. Found Index: " << bestMatchIndex 
        //           << " (time_ms=" << frameIndex[bestMatchIndex].time_ms << ")" << std::endl;
        // std::cout << "  Valid timestamps: " << validTimestamps << ", Invalid: " << invalidTimestamps << std::endl;
        
        // Log surrounding frames for context
        if (bestMatchIndex > 0) {
        //    std::cout << "  Prev Frame (" << bestMatchIndex - 1 << "): " << frameIndex[bestMatchIndex - 1].time_ms << "ms" << std::endl;
        }
        if (bestMatchIndex < frameIndex.size() - 1) {
        //    std::cout << "  Next Frame (" << bestMatchIndex + 1 << "): " << frameIndex[bestMatchIndex + 1].time_ms << "ms" << std::endl;
        }
        
        // Check for timestamp consistency issues
        if (bestMatchIndex > 0 && frameIndex[bestMatchIndex].time_ms >= 0 && frameIndex[bestMatchIndex - 1].time_ms >= 0) {
            int64_t timeDiff = frameIndex[bestMatchIndex].time_ms - frameIndex[bestMatchIndex - 1].time_ms;
            if (timeDiff < 0) {
            //    std::cout << "  WARNING: Timestamp goes backwards! Diff: " << timeDiff << "ms" << std::endl;
            } else if (timeDiff > 200) { // More than 200ms between frames
            //    std::cout << "  WARNING: Large timestamp gap: " << timeDiff << "ms" << std::endl;
            }
        }
        // std::cout << "----" << std::endl;
    }

    return bestMatchIndex;
}

// REMOVED: Timestamp synchronization functions
// These functions were causing more issues than they solved because they tried
// to "fix" the original video's timestamps, which may be intentionally offset
// or have specific timing characteristics that shouldn't be altered.
