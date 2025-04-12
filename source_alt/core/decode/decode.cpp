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

namespace fs = std::filesystem;

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
            info.time_base = timeBase;
            frameIndex.push_back(info);
        }
        av_packet_unref(&packet);
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
    return LowResDecoder::convertToLowRes(filename, outputFilename, nullptr);
}

// Версия processMediaSource без колбэка прогресса
bool processMediaSource(const std::string& source, std::string& processedFilePath) {
    bool result = false;
    
    // Проверяем, является ли источник URL
    if (isURL(source)) {
        // Если это URL, загружаем видео
        result = downloadVideoFromURL(source, processedFilePath);
    } else {
        // Если это локальный файл, просто используем его путь
        processedFilePath = source;
        result = true;
    }
    
    return result;
}

// Helper function to find the frame index closest to a given timestamp
// Performs a limited linear search around the estimated position
int findClosestFrameIndexByTime(const std::vector<FrameInfo>& frameIndex, int64_t target_ms) {
    if (frameIndex.empty() || target_ms < 0) {
        // std::cout << "[SyncDebug] Frame index empty or target_ms < 0. Returning 0." << std::endl; // Optional: Log edge case
        return 0; // Return first frame if index empty or time invalid
    }

    int bestMatchIndex = 0; // Default to first frame
    int64_t maxTimeMsLessOrEqual = -1; // Track the largest timestamp <= target_ms

    // Estimate initial position based on average frame duration if possible
    // This assumes relatively constant frame rate, which might not hold true
    // A simple estimate: guess index based on target time / average time per frame
    // Requires knowing total duration and frame count, or average FPS.
    // For now, we'll just search linearly from the start, but keep this in mind.
    
    // Simple linear search for now (can be optimized later)
    int searchStart = 0;
    int searchEnd = frameIndex.size();

    // --- Start Debug Logging ---
    // static int logCounter = 0; // Log every N calls to avoid spam
    // logCounter++;
    bool shouldLog = true; // (logCounter % 10 == 0); // Log every 10th call
    // --- End Debug Logging ---

    for (int i = searchStart; i < searchEnd; ++i) {
        int64_t current_time_ms = frameIndex[i].time_ms;
        if (current_time_ms >= 0) { // Check if the frame has a valid timestamp
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
                // We already found the best match in the previous iteration (or default 0).
                 if (shouldLog) {
                     // std::cout << "[SyncDebug] Optimization break at index " << i << " (time_ms: " << current_time_ms << " > target_ms: " << target_ms << ")" << std::endl;
                 }
                break;
            }
        }
    }

    // --- More Debug Logging ---
    if (shouldLog) {
        // std::cout << "[SyncDebug] Target: " << target_ms << "ms. Found Index: " << bestMatchIndex << " (time_ms=" << frameIndex[bestMatchIndex].time_ms << ")" << std::endl;
        // Optional: Log surrounding frames for context
        // if (bestMatchIndex > 0) {
        //     std::cout << "  Prev Frame (" << bestMatchIndex - 1 << "): " << frameIndex[bestMatchIndex - 1].time_ms << "ms" << std::endl;
        // }
        // if (bestMatchIndex < frameIndex.size() - 1) {
        //      std::cout << "  Next Frame (" << bestMatchIndex + 1 << "): " << frameIndex[bestMatchIndex + 1].time_ms << "ms" << std::endl;
        // }
        //  std::cout << "----" << std::endl;
    }
    // --- End More Debug Logging ---

    return bestMatchIndex;
}
