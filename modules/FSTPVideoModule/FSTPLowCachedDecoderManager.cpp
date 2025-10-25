#include "FSTPLowCachedDecoderManager.h"

#include <iostream>
#include <chrono>
#include <algorithm>
#include <iomanip>
#include <limits>
#include <cmath>

// Debug control: set to true to enable verbose logging
static constexpr bool ENABLE_LOWCACHED_DEBUG = false;

namespace FSTP {

// CRITICAL: Increased threshold from 16.0 to 64.0 for 32x reverse support
// Old: ≥16x would DELETE ALL segments (catastrophic for reverse playback!)
// New: Only at ≥64x we clear segments (extreme forward scanning only)
// This allows 32x reverse to work smoothly with segment buffering
std::atomic<double> LowCachedDecoderManager::speed_threshold(64.0);

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
    , segmentSize_(600) {  // Default 600 frames, will be updated from GOP
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
        // DYNAMIC SPEED ADAPTATION for Intel Celeron + PowerSaver
        // Goal: 32x smooth reverse in PowerSaver mode
        // Adapt check frequency based on current speed and frame position
        auto wait_duration = (currentRate == 0.0) ? std::chrono::milliseconds(500) :  // Pause
                             (currentRate <= 1.0) ? std::chrono::milliseconds(50) :   // 1x: Very frequent
                             (currentRate <= 4.0) ? std::chrono::milliseconds(25) :   // 1-4x: Ultra frequent
                             (currentRate <= 8.0) ? std::chrono::milliseconds(15) :   // 4-8x: Extreme frequent
                             (currentRate <= 16.0) ? std::chrono::milliseconds(10) :  // 8-16x: Maximum frequent
                             (currentRate <= 32.0) ? std::chrono::milliseconds(5) :   // 16-32x: Critical frequent
                                                    std::chrono::milliseconds(2);     // 32x+: Ultra critical

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

                // REVERSE-SPECIFIC OPTIMIZATION for Intel Celeron + PowerSaver
                // Problem: Reverse playback needs different segment loading strategy
                // Forward: Load segments ahead of current position
                // Reverse: Load segments BEHIND current position (already passed)
                
                // CRITICAL: For reverse, we need to preload segments that we've ALREADY passed
                // This prevents dropouts when playback head moves backward faster than decoder
                
                // OPTIMIZED PRELOADING for 24x max speed (500 frame segments)
                // Test results: 500 frames = 1221 fps throughput (enough for 24x)
                // With 500 frame segments: fewer segments needed, less memory
                // Segment = 500 frames = 20 seconds @ 25fps
                int preloadCount;
                if (currentRate >= 16.0) {
                    preloadCount = 8;   // 16x+: 8 segments (160 seconds = 2.7 minutes)
                } else if (currentRate >= 8.0) {
                    preloadCount = 6;   // 8-16x: 6 segments (120 seconds = 2 minutes)
                } else if (currentRate >= 4.0) {
                    preloadCount = 5;   // 4-8x: 5 segments (100 seconds = 1.7 minutes)
                } else if (currentRate >= 2.0) {
                    preloadCount = 4;   // 2-4x: 4 segments (80 seconds = 1.3 minutes)
                } else {
                    preloadCount = 3;   // 1-2x: 3 segments (60 seconds = 1 minute)
                }
                
                // ADDITIONAL: Current frame position adaptation
                // If we're near the beginning, reduce preloading to avoid waste
                int currentFrame = currentFrame_.load();
                int totalFrames = frameIndex_.size();
                if (currentFrame < totalFrames * 0.1) {  // First 10% of video
                    preloadCount = std::min(preloadCount, 30);  // Limit preloading
                } else if (currentFrame > totalFrames * 0.9) {  // Last 10% of video
                    preloadCount = std::min(preloadCount, 20);  // Limit preloading
                }

                // REVERSE STRATEGY: Load segments BEHIND current position
                // This ensures smooth reverse playback without dropouts
                for (int i = 1; i <= preloadCount; i++) {
                    if (currentSegment - i >= 0) {
                        targetSegments.insert(currentSegment - i);
                    }
                }
                
                // ADDITIONAL: Also preload some segments AHEAD for direction changes
                // This helps when user switches from reverse to forward
                int forwardBuffer = std::min(5, preloadCount / 4);
                for (int i = 1; i <= forwardBuffer; i++) {
                    targetSegments.insert(currentSegment + i);
                }

                if (directionChanged) {
                    int minSegment = std::max(0, currentSegment - preloadCount);
                    std::cout << "⏪ [REVERSE PRELOAD] Direction changed to REVERSE at segment " << currentSegment
                              << " (speed=" << currentRate << "x) → preloading " << preloadCount
                              << " segments BACKWARD [" << minSegment
                              << " to " << (currentSegment - 1) << "]" << std::endl;
                }
                
                // REALTIME MONITORING for Intel Celeron + PowerSaver
                // Log performance metrics for 32x reverse optimization
                static auto lastLogTime = std::chrono::steady_clock::now();
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(now - lastLogTime).count() >= 2) {
                    int currentFrame = currentFrame_.load();
                    int totalFrames = frameIndex_.size();
                    double progress = totalFrames > 0 ? (double)currentFrame / totalFrames * 100.0 : 0.0;
                    
                    std::cout << "📊 [REALTIME] Speed: " << currentRate << "x | Frame: " << currentFrame 
                              << "/" << totalFrames << " (" << std::fixed << std::setprecision(1) << progress << "%)"
                              << " | Segment: " << currentSegment << " | Preload: " << preloadCount << " segments" << std::endl;
                    
                    lastLogTime = now;
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

            // OPTIMIZED BUFFER: With 500-frame segments, need fewer segments in memory
            // 500 frames/segment × 17MB/segment = reasonable memory usage
            // Tests show: 8 segments sufficient for 24x reverse (160 seconds buffer)
            // Safety margin keeps segments longer to avoid re-decode
            int safetyMargin = 10;  // Keep 10 segments on each side (±10 = 21 total = ~7 minutes)
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

            // REVERSE-SPECIFIC PRIORITY LOADING for Intel Pentium Gold 7505
            if (isReverse_.load()) {
                // REVERSE: Load segments in reverse order of priority
                // Current segment is most critical, then segments immediately behind it
                std::vector<int> prioritySegments;

                // EMERGENCY CHECK: Is currentFrame in undecoded zone?
                // If YES - IMMEDIATE decoding to prevent dropout
                int current = currentFrame_.load();
                int frameSegment = current / segmentSize_;
                bool currentFrameHasLowRes = false;

                if (frameSegment >= 0 && frameSegment < numSegmentsTotal) {
                    int frameInSegment = current % segmentSize_;
                    int absoluteFrameIndex = frameSegment * segmentSize_ + frameInSegment;

                    if (absoluteFrameIndex < static_cast<int>(frameIndex_.size())) {
                        std::lock_guard<std::mutex> lock(frameIndex_[absoluteFrameIndex].mutex);
                        currentFrameHasLowRes = (frameIndex_[absoluteFrameIndex].low_res_frame != nullptr);
                    }
                }

                // CRITICAL: If current frame has NO low_res_frame - EMERGENCY DECODE!
                if (!currentFrameHasLowRes && frameSegment >= 0 && frameSegment < numSegmentsTotal) {
                    std::cout << "🚨 [EMERGENCY] Frame " << current << " in segment " << frameSegment
                              << " has no low_res - IMMEDIATE decode!" << std::endl;
                    // Load current segment IMMEDIATELY (synchronously)
                    loadSegment(frameSegment);
                    // Also preload segments behind (for reverse playback continuity)
                    for (int i = 1; i <= 3; i++) {
                        if (frameSegment - i >= 0) {
                            loadSegment(frameSegment - i);
                        }
                    }
                }

                // 1. Current segment (highest priority)
                if (segmentsToLoad.count(currentSegment)) {
                    prioritySegments.push_back(currentSegment);
                    segmentsToLoad.erase(currentSegment);
                } else if (loadedSegments_.find(currentSegment) == loadedSegments_.end()) {
                    // EMERGENCY: Load current segment immediately if not already loaded
                    loadSegment(currentSegment);
                }
                
                // 2. Segments immediately behind current (for reverse playback)
                for (int i = 1; i <= 3; i++) {
                    int behindSegment = currentSegment - i;
                    if (behindSegment >= 0 && segmentsToLoad.count(behindSegment)) {
                        prioritySegments.push_back(behindSegment);
                        segmentsToLoad.erase(behindSegment);
                    }
                }
                
                // 3. Load priority segments first
                for (int segIdx : prioritySegments) {
                    loadSegment(segIdx);
                }
                
                // 4. Load remaining segments in background
                for (int segIdx : segmentsToLoad) {
                    loadSegment(segIdx);
                }
            } else {
                // FORWARD: Normal priority loading
                if (segmentsToLoad.count(currentSegment)) {
                    loadSegment(currentSegment);
                    segmentsToLoad.erase(currentSegment);
                } else {
                    loadSegment(currentSegment);
                }
                
                for (int segIdx : segmentsToLoad) {
                    loadSegment(segIdx);
                }
            }

            lastLowResUpdateTime_ = now;
            previousSegment_ = currentSegment;
        } else if (needsUpdate) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastLowResUpdateTime_);
            // DYNAMIC INTERVALS for Intel Celeron + PowerSaver
            // Goal: 32x smooth reverse - ultra-aggressive updates
            auto intervalForRate = [](double rate) {
                if (rate < 0.9) return std::numeric_limits<int>::max();
                if (rate <= 1.0) return 2000;     // 1x: 2s
                if (rate <= 2.0) return 1000;     // 1-2x: 1s
                if (rate <= 4.0) return 500;      // 2-4x: 500ms
                if (rate <= 8.0) return 250;      // 4-8x: 250ms
                if (rate <= 16.0) return 125;     // 8-16x: 125ms
                if (rate <= 32.0) return 50;      // 16-32x: 50ms - CRITICAL for PowerSaver
                return 25;                        // 32x+: 25ms - ULTRA CRITICAL
            };
            std::chrono::milliseconds updateInterval(intervalForRate(currentRate));
            bool forceUpdate = (rateDiff > rateThreshold);

            std::set<int> targetSegments;
            targetSegments.insert(currentSegment);

            if (isReverse_.load()) {
                // REVERSE: EXTREME preload for Intel Celeron + PowerSaver (for needsUpdate)
                // PowerSaver + VA-API needs MASSIVE preloading even for regular updates
                // Segment=300 frames (~12 seconds each), PowerSaver + VA-API needs huge buffer
                int preloadCount = 12; // Increased from 8 to 12 for PowerSaver
                if (currentRate >= 9.0) {
                    preloadCount = 18;  // Increased from 12 to 18
                } else if (currentRate >= 4.0) {
                    preloadCount = 15;  // Increased from 10 to 15
                } else if (currentRate >= 2.0) {
                    preloadCount = 13;  // Increased from 9 to 13
                } else {
                    preloadCount = 12;  // Increased from 8 to 12 for smooth 1x
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
                // EMERGENCY BUFFER: Load current segment FIRST for immediate playback
                if (segmentsToLoad.count(currentSegment)) {
                    loadSegment(currentSegment);
                    segmentsToLoad.erase(currentSegment);
                } else {
                    // EMERGENCY: Current segment not in load list - load it immediately!
                    // This prevents dropout when PowerSaver + VA-API is too slow
                    loadSegment(currentSegment);
                }
                
                // Then load other segments in background
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

int LowCachedDecoderManager::calculateOptimalSegmentSize(int gopSize) {
    // ADAPTIVE SEGMENT SIZING based on video GOP structure
    // Goal: Align segments with GOP boundaries for minimal seek overhead

    if (gopSize <= 0) {
        // Unknown or invalid GOP - use safe default
        std::cout << "[LowCachedManager] GOP unknown, using default 600 frames" << std::endl;
        return 600;
    }

    // Based on real tests with Pentium Gold 7505:
    // - 300 frames (GOP 300): 1220 fps, 1.0% seek overhead
    // - 600 frames (2×GOP 300): 1411 fps, 0.5% seek overhead
    // - 900 frames (3×GOP 300): 1473 fps, 0.4% seek overhead (best)

    // Strategy: Use 2× GOP size for optimal balance
    // This gives good performance while keeping memory reasonable

    int targetSize = gopSize * 2;

    // Clamp to reasonable range: 300-1200 frames
    // Too small (<300): Excessive seek overhead
    // Too large (>1200): Excessive memory usage
    if (targetSize < 300) {
        targetSize = 300;
    } else if (targetSize > 1200) {
        targetSize = 1200;
    }

    std::cout << "[LowCachedManager] 🎯 Adaptive segment size: " << targetSize
              << " frames (GOP=" << gopSize << ", " << (targetSize / gopSize) << "× GOP)"
              << std::endl;
    std::cout << "                    Memory per segment: ~"
              << std::fixed << std::setprecision(1) << (targetSize * 35.0 / 1024.0)
              << " MB" << std::endl;

    return targetSize;
}

void LowCachedDecoderManager::setSegmentSizeFromGOP(int gopSize) {
    int newSize = calculateOptimalSegmentSize(gopSize);

    if (newSize != segmentSize_) {
        std::cout << "[LowCachedManager] Updating segment size: " << segmentSize_
                  << " → " << newSize << " frames" << std::endl;

        std::lock_guard<std::mutex> lock(mtx_);
        segmentSize_ = newSize;

        // Clear loaded segments to force reload with new size
        // This is safe because decoder will reload segments as needed
        if (!loadedSegments_.empty()) {
            std::cout << "[LowCachedManager] Clearing " << loadedSegments_.size()
                      << " segments to reload with new size" << std::endl;
            loadedSegments_.clear();
            previousSegment_ = -1;
        }
    }
}

} // namespace FSTP
