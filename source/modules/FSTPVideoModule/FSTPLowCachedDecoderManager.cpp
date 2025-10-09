#include "FSTPLowCachedDecoderManager.h"

#include <iostream>
#include <chrono>
#include <algorithm>
#include <limits>
#include <cmath>

// Debug control: set to true to enable verbose logging
static constexpr bool ENABLE_LOWCACHED_DEBUG = false;

namespace FSTP {

std::atomic<double> LowCachedDecoderManager::speed_threshold(16.0);

LowCachedDecoderManager::LowCachedDecoderManager(const std::string& lowResFilename,
                                                 std::vector<FrameInfo>& frameIndex,
                                                 std::atomic<int>& currentFrame,
                                                 int ringBufferCapacity,
                                                 int highResWindowSize,
                                                 std::atomic<bool>& isPlaying,
                                                 std::atomic<double>& playbackRate,
                                                 std::atomic<bool>& isReverseRef)
    : decoder_(std::make_unique<LowResDecoder>(lowResFilename))
    , frameIndex_(frameIndex)
    , currentFrame_(currentFrame)
    , isPlaying_(isPlaying)
    , playbackRate_(playbackRate)
    , isReverse_(isReverseRef)
    , ringBufferCapacity_(ringBufferCapacity)
    , highResWindowSize_(highResWindowSize)
    , segmentSize_(750) {  // Reduced from 2750 to 750: 750 frames = ~30 seconds @ 25fps
                           // Golden middle: enough for efficiency, but faster to decode
                           // At GOP=300 this is only 2.5 GOP - decoder manages to load
                           // At GOP=25 this is 30 GOP - also good
    if (!decoder_ || !decoder_->isInitialized()) {
        throw std::runtime_error("Failed to initialize LowResDecoder in LowCachedDecoderManager");
    }

    int initialSegment = currentFrame_.load() / segmentSize_;
    int startFrame = initialSegment * segmentSize_;
    int endFrame = std::min(startFrame + segmentSize_ - 1, static_cast<int>(frameIndex_.size()) - 1);
    if (!frameIndex_.empty() && startFrame <= endFrame) {
        int dummyHighStart = 0;
        int dummyHighEnd = -1;
        bool isReverse = isReverse_.load();
        decoder_->decodeLowResRange(frameIndex_, startFrame, endFrame, dummyHighStart, dummyHighEnd, false, isReverse);
        loadedSegments_.insert(initialSegment);
        lastLowResUpdateTime_ = std::chrono::steady_clock::now();
    }
}

LowCachedDecoderManager::~LowCachedDecoderManager() {
    try {
        stop();
    } catch (const std::exception& e) {
        std::cerr << "⚠️ [LowCachedDecoderManager] Exception in destructor: " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "⚠️ [LowCachedDecoderManager] Unknown exception in destructor" << std::endl;
    }
}

void LowCachedDecoderManager::run() {
    if (isRunning_) {
        return;
    }
    stopRequested_.store(false);
    isRunning_.store(true);
    managerThread_ = std::thread(&LowCachedDecoderManager::decodingLoop, this);
}

void LowCachedDecoderManager::stop() {
    if (!isRunning_ && !managerThread_.joinable()) {
        return;
    }

    stopRequested_.store(true);
    if (decoder_) {
        decoder_->requestStop();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    cv_.notify_one();

    if (managerThread_.joinable()) {
        managerThread_.join();
    }

    isRunning_.store(false);
}

void LowCachedDecoderManager::notifyFrameChange() {
    cv_.notify_one();
}

void LowCachedDecoderManager::decodingLoop() {
    static int wakeup_count = 0;
    static auto last_report = std::chrono::steady_clock::now();

    while (!stopRequested_.load()) {
        wakeup_count++;

        // Every second output statistics
        auto now = std::chrono::steady_clock::now();
        if (ENABLE_LOWCACHED_DEBUG && std::chrono::duration_cast<std::chrono::seconds>(now - last_report).count() >= 1) {
            std::cout << "🔄 [LowCached] Wakeups per second: " << wakeup_count << std::endl;
            wakeup_count = 0;
            last_report = now;
        }

        int current = currentFrame_.load();
        double currentRate = std::abs(playbackRate_.load());
        double rateDiff = std::abs(currentRate - previousPlaybackRate_);
        const double rateThreshold = 0.5;
        bool needsUpdate = false;

        // OPTIMIZATION: adaptive check interval depending on speed
        // Balance between CPU load and responsiveness at high speeds
        // Now wake up only when segment changes → can increase intervals for CPU savings
        auto wait_duration = (currentRate == 0.0) ? std::chrono::milliseconds(500) :  // Pause
                             (currentRate <= 1.0) ? std::chrono::milliseconds(250) :  // 1x: rarely (segment = 110 sec!)
                             (currentRate < 4.0)  ? std::chrono::milliseconds(100) :  // 1-4x
                             (currentRate < 10.0) ? std::chrono::milliseconds(50) :   // 4-10x
                                                    std::chrono::milliseconds(25);    // 10x+ (fast reverse!)

        {
            std::unique_lock<std::mutex> lock(mtx_);

            // OPTIMIZATION: Wake up only when segment changes, not on every frame!
            // Segment changes every ~110 seconds @ 1x, no need to wake up on every frame (25/sec)
            if (!cv_.wait_for(lock, wait_duration, [&] {
                    if (stopRequested_.load()) return true;

                    int newFrame = currentFrame_.load();
                    int newSegment = newFrame / segmentSize_;
                    int lastSegment = lastNotifiedFrame_ / segmentSize_;

                    // Wake up if:
                    // 1. Segment changed (main condition - not on every frame!)
                    // 2. OR direction changed (need preload in other direction)
                    // 3. OR speed changed (need adjust preload count)
                    bool segmentChanged = (newSegment != lastSegment);
                    bool directionChanged = (isReverse_.load() != previousIsReverse_);
                    bool speedChanged = (std::abs(std::abs(playbackRate_.load()) - previousPlaybackRate_) > 0.5);

                    return segmentChanged || directionChanged || speedChanged;
                })) {
                // Timeout - check periodically even if segment didn't change
                current = currentFrame_.load();
                int currentSegment = current / segmentSize_;
                int lastSegment = lastNotifiedFrame_ / segmentSize_;

                if (stopRequested_.load() || currentSegment == lastSegment) {
                    continue;  // Same segment - do nothing
                }
            }

            if (stopRequested_.load()) {
                break;
            }

            current = currentFrame_.load();
            if (current != lastNotifiedFrame_) {
                needsUpdate = true;
                lastNotifiedFrame_ = current;
            }
        }

        if (currentRate >= speed_threshold.load()) {
            std::set<int> segmentsToClear;
            bool hasSegments = false;

            {
                std::lock_guard<std::mutex> lock(mtx_);
                if (!loadedSegments_.empty()) {
                    hasSegments = true;
                    segmentsToClear = loadedSegments_;
                    loadedSegments_.clear();
                    previousSegment_ = -1;
                }
            }

            if (hasSegments) {
                for (int segIdx : segmentsToClear) {
                    int startFrame = segIdx * segmentSize_;
                    int endFrame = std::min(startFrame + segmentSize_ - 1, static_cast<int>(frameIndex_.size()) - 1);
                    if (startFrame <= endFrame) {
                        LowResDecoder::removeLowResFrames(frameIndex_, startFrame, endFrame);
                    }
                }
            }

            continue;
        }

        if (!decoder_ || frameIndex_.empty() || segmentSize_ <= 0) {
            continue;
        }

        int currentSegment = current / segmentSize_;
        int numSegmentsTotal = (frameIndex_.size() + segmentSize_ - 1) / segmentSize_;
        bool segmentChanged = (currentSegment != previousSegment_);
        bool directionChanged = (isReverse_.load() != previousIsReverse_);

        if (segmentChanged || directionChanged) {
            std::set<int> targetSegments;
            targetSegments.insert(currentSegment);

            if (isReverse_.load()) {
                // REVERSE PRELOAD: ALWAYS backward (to smaller segment numbers)
                // When reverse head moves: frame N → N-1 → N-2 → 0 (DECREASE!)
                // So segments also decrease: segment 10 → 9 → 8 → 0

                // AGGRESSIVE PRELOAD adapted for segment=750 frames (~30 seconds @ 25fps)
                // Goal: cover 2-3 minutes buffer for smooth reverse with GOP=300
                // 1x:   6 segments (180 seconds = 3 minutes buffer)
                // 2-4x: 8 segments (240 seconds = 4 minutes)
                // 4-8x: 10 segments (300 seconds = 5 minutes)
                // 9x+:  12 segments (360 seconds = 6 minutes, maximum)
                int preloadCount = 6; // default increased for smaller segments
                if (currentRate >= 9.0) {
                    preloadCount = 12;  // Maximum for high speeds
                } else if (currentRate >= 4.0) {
                    preloadCount = 10;  // Increased for medium-high speeds
                } else if (currentRate >= 2.0) {
                    preloadCount = 8;   // Medium speeds
                } else {
                    preloadCount = 6;   // 1x smooth reverse (CRITICAL for GOP=300!)
                }

                for (int i = 1; i <= preloadCount; i++) {
                    if (currentSegment - i >= 0) {
                        targetSegments.insert(currentSegment - i);
                    }
                }

                if (directionChanged) {
                    int minSegment = std::max(0, currentSegment - preloadCount);
                    std::cout << "⏪ [REVERSE PRELOAD] Direction changed to REVERSE at segment " << currentSegment
                              << " (speed=" << currentRate << "x) → preloading " << preloadCount
                              << " segments BACKWARD [" << minSegment
                              << " to " << (currentSegment - 1) << "]" << std::endl;
                }
            } else {
                // Forward preloading (leave as is - works perfectly)
                if (currentSegment < numSegmentsTotal - 1) targetSegments.insert(currentSegment + 1);
                if (currentRate >= 1.8 && currentSegment < numSegmentsTotal - 2) targetSegments.insert(currentSegment + 2);
                if (directionChanged && currentSegment < numSegmentsTotal - 3) {
                    targetSegments.insert(currentSegment + 3);
                    std::cout << "[LowCachedManager] Direction changed to FORWARD: preloading segment "
                              << (currentSegment + 3) << std::endl;
                }
            }

            std::set<int> segmentsToLoad;
            std::set<int> segmentsToUnload = loadedSegments_;
            for (int seg : targetSegments) {
                if (seg >= 0 && seg < numSegmentsTotal) {
                    if (loadedSegments_.find(seg) == loadedSegments_.end()) {
                        segmentsToLoad.insert(seg);
                    }
                    segmentsToUnload.erase(seg);
                }
            }

            // CRITICAL: HYSTERESIS for unload - don't remove segments close to current position!
            // Problem: When reverse crosses segment boundary, but frames from "old" segment are still shown
            // Solution: Leave "safety buffer" ±1 segment from currentSegment
            // This prevents FREEZE FRAME when fast movement crosses boundaries
                int safetyMargin = 1;  // Leave 1 segment on each side
            for (int offset = -safetyMargin; offset <= safetyMargin; offset++) {
                int protectedSegment = currentSegment + offset;
                if (protectedSegment >= 0 && protectedSegment < numSegmentsTotal) {
                    segmentsToUnload.erase(protectedSegment);
                }
            }

            // Unload only segments that are far from current position
            for (int segIdx : segmentsToUnload) {
                int startFrame = segIdx * segmentSize_;
                int endFrame = startFrame + segmentSize_ - 1;
                LowResDecoder::removeLowResFrames(frameIndex_, startFrame, endFrame);
                loadedSegments_.erase(segIdx);
            }

            if (segmentsToLoad.count(currentSegment)) {
                loadSegment(currentSegment);
                segmentsToLoad.erase(currentSegment);
            }

            for (int segIdx : segmentsToLoad) {
                loadSegment(segIdx);
            }

            lastLowResUpdateTime_ = now;
            previousSegment_ = currentSegment;
        } else if (needsUpdate) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastLowResUpdateTime_);
            auto intervalForRate = [](double rate) {
                if (rate < 0.9) return std::numeric_limits<int>::max();
                if (rate <= 1.0) return 10000;    // 1x: 10s
                if (rate <= 1.8) return 5000;     // 1-2x: 5s
                if (rate <= 3.8) return 2500;     // 2-4x: 2.5s
                if (rate <= 7.8) return 1000;     // 4-8x: 1s (was 1250ms)
                if (rate <= 12.0) return 500;     // 8-12x: 500ms (CRITICAL for reverse!)
                return 250;                       // 12x+: 250ms (aggressive)
            };
            std::chrono::milliseconds updateInterval(intervalForRate(currentRate));
            bool forceUpdate = (rateDiff > rateThreshold);

            std::set<int> targetSegments;
            targetSegments.insert(currentSegment);

            if (isReverse_.load()) {
                // REVERSE: adaptive preload by speed (for needsUpdate - slightly less than directionChanged)
                // Segment=750, less segments but faster to decode
                int preloadCount = 4; // default increased from 2 to 4
                if (currentRate >= 9.0) {
                    preloadCount = 8;  // Increased from 4 to 8
                } else if (currentRate >= 4.0) {
                    preloadCount = 6;  // Increased from 3 to 6
                } else if (currentRate >= 2.0) {
                    preloadCount = 5;  // New tier
                } else {
                    preloadCount = 4;  // Increased from 2 to 4 for smooth 1x
                }

                for (int i = 1; i <= preloadCount; i++) {
                    if (currentSegment - i >= 0) {
                        targetSegments.insert(currentSegment - i);
                    }
                }
            } else {
                // Forward playback (leave as is - works perfectly)
                if (currentSegment < numSegmentsTotal - 1) targetSegments.insert(currentSegment + 1);
                if (currentRate >= 1.8 && currentSegment < numSegmentsTotal - 2) targetSegments.insert(currentSegment + 2);
            }

            std::set<int> segmentsToLoad;
            for (int seg : targetSegments) {
                if (seg >= 0 && seg < numSegmentsTotal) {
                    if (loadedSegments_.find(seg) == loadedSegments_.end()) {
                        segmentsToLoad.insert(seg);
                    }
                }
            }

            if (!segmentsToLoad.empty() && (elapsed >= updateInterval || forceUpdate)) {
                if (segmentsToLoad.count(currentSegment)) {
                    loadSegment(currentSegment);
                    segmentsToLoad.erase(currentSegment);
                }
                for (int segIdx : segmentsToLoad) {
                    loadSegment(segIdx);
                }
                lastLowResUpdateTime_ = now;
            }

            previousSegment_ = currentSegment;
        }

        if (!needsUpdate && !isPlaying_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        previousPlaybackRate_ = currentRate;
        previousIsReverse_ = isReverse_.load();
    }

    isRunning_.store(false);
}

void LowCachedDecoderManager::loadSegment(int segmentIndex) {
    if (!decoder_ || !decoder_->isInitialized()) {
        return;
    }
    if (frameIndex_.empty() || segmentSize_ <= 0) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (loadedSegments_.count(segmentIndex)) {
            return;
        }
    }

    int startFrame = segmentIndex * segmentSize_;
    int endFrame = std::min(startFrame + segmentSize_ - 1, static_cast<int>(frameIndex_.size()) - 1);
    if (startFrame > endFrame) {
        return;
    }

    int highResHalf = highResWindowSize_ / 2;
    int current = currentFrame_.load();
    int highResStart = std::max(0, current - highResHalf);
    int highResEnd = std::min(static_cast<int>(frameIndex_.size()) - 1, current + highResHalf);

    // Pass isReverse to decoder for prioritized decoding
    bool isReverse = isReverse_.load();
    bool ok = decoder_->decodeLowResRange(frameIndex_, startFrame, endFrame, highResStart, highResEnd, false, isReverse);
    if (ok) {
        std::lock_guard<std::mutex> lock(mtx_);
        loadedSegments_.insert(segmentIndex);
    }
}

void LowCachedDecoderManager::unloadSegment(int segmentIndex) {
    bool shouldRemove = false;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (loadedSegments_.count(segmentIndex)) {
            loadedSegments_.erase(segmentIndex);
            shouldRemove = true;
        }
    }

    if (shouldRemove) {
        int startFrame = segmentIndex * segmentSize_;
        int endFrame = std::min(startFrame + segmentSize_ - 1, static_cast<int>(frameIndex_.size()) - 1);
        if (startFrame <= endFrame) {
            LowResDecoder::removeLowResFrames(frameIndex_, startFrame, endFrame);
        }
    }
}

} // namespace FSTP
