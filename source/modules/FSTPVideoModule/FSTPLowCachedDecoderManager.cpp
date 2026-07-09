#include "FSTPLowCachedDecoderManager.h"

#include <iostream>
#include <chrono>
#include <algorithm>
#include <iomanip>
#include <limits>
#include <cmath>

// Request force render from window system (defined in platform-specific WS)
extern "C" void RequestForceRender();

// Request frame update from video module (defined in FSTPVideoModule_wrapper.cpp)
extern void NotifyVideoFrameUpdate(int instance_id);

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
                                                 std::atomic<bool>& isReverseRef,
                                                 int instanceId,
                                                 bool isHalfFps)
    : decoder_(std::make_unique<LowResDecoder>(lowResFilename))
    , frameIndex_(frameIndex)
    , currentFrame_(currentFrame)
    , isPlaying_(isPlaying)
    , playbackRate_(playbackRate)
    , isReverse_(isReverseRef)
    , ringBufferCapacity_(ringBufferCapacity)
    , highResWindowSize_(highResWindowSize)
    , segmentSize_(2400)  // Large window for 32x reverse support (3 seconds @ 800 fps)
    , instanceId_(instanceId)
    , isHalfFps_(isHalfFps) {
    if (!decoder_ || !decoder_->isInitialized()) {
        throw std::runtime_error("Failed to initialize LowResDecoder in LowCachedDecoderManager");
    }

    if (isHalfFps_) {
        std::cout << "🎯 [HALF-FPS MANAGER] Initialized with half-fps mode (will map frame indices /2)" << std::endl;
    }

    // HALF-FPS: Convert currentFrame to half-fps coordinates if needed
    int current = currentFrame_.load();
    if (isHalfFps_) {
        current = current / 2;
    }

    int initialSegment = current / segmentSize_;
    int startFrame = initialSegment * segmentSize_;
    int endFrame = std::min(startFrame + segmentSize_ - 1, static_cast<int>(frameIndex_.size()) - 1);
    if (!frameIndex_.empty() && startFrame <= endFrame) {
        int dummyHighStart = 0;
        int dummyHighEnd = -1;
        bool isReverse = isReverse_.load();
        // Stage-1 keyframe-only-at-shuttle was REVERTED: sparse fill + the display's empty-slot
        // fallback produced visible jumps above the threshold. Full decode here; the real shuttle
        // fix is Stage-2 seek-per-frame (decode the exact displayed frame).
        decoder_->decodeLowResRange(frameIndex_, startFrame, endFrame, dummyHighStart, dummyHighEnd, false, isReverse, false);
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

bool LowCachedDecoderManager::decodeFrameNow(int frame) {
    if (!decoder_ || frameIndex_.empty()) return false;

    int idx = frame;
    if (isHalfFps_) idx = idx / 2;
    if (idx < 0 || idx >= static_cast<int>(frameIndex_.size())) return false;

    // Skip if this slot already holds a decoded proxy frame (cheap non-blocking peek).
    {
        std::unique_lock<std::mutex> lk(frameIndex_[idx].mutex, std::try_to_lock);
        if (lk.owns_lock() &&
            frameIndex_[idx].type == FSTP::FrameInfo::LOW_RES &&
            frameIndex_[idx].low_res_frame && frameIndex_[idx].low_res_frame->data[0]) {
            return true;
        }
    }

    // Decode exactly this frame (GOP-aware seek to its keyframe + decode forward). No high-res
    // window (0,-1), full decode (keyframesOnly=false). Runs on the caller's thread.
    // threadIdBase=2 → uses decode contexts 2/3, NEVER 0/1 — so this on-demand decode can run
    // concurrently with the manager's segment decode (contexts 0/1) without sharing an
    // AVCodecContext (that sharing tripped libavcodec's frame->private_ref assertion → crash).
    //
    // isReverse=FALSE always: for a single frame there is no decode-order to optimise, and the
    // reverse "3-part" split degenerates a 1-frame range into [N, N-1] → seek finds no timestamp
    // → falls back to "decode from beginning of stream" (huge) + VideoToolbox -12909 errors →
    // dropped frames on reverse shuttle. Just decode the one frame forward from its keyframe.
    return decoder_->decodeLowResRange(frameIndex_, idx, idx, 0, -1, false, /*isReverse=*/false, false, /*threadIdBase=*/2);
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

        // HALF-FPS: Convert to half-fps coordinates if needed
        if (isHalfFps_) {
            current = current / 2;
        }

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
                    // HALF-FPS: Convert to half-fps coordinates
                    if (isHalfFps_) {
                        newFrame = newFrame / 2;
                    }
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
                // HALF-FPS: Convert to half-fps coordinates
                if (isHalfFps_) {
                    current = current / 2;
                }
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
            // HALF-FPS: Convert to half-fps coordinates
            if (isHalfFps_) {
                current = current / 2;
            }
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

        // STAGE 2: across the WHOLE shuttle range (≥2× — same point where full-res hands off to
        // the proxy) the display decodes exactly the shown frame on demand (decodeFrameNow), so
        // skip segment PREFETCH here. Threshold matches the on-demand gate in DisplayFrame so there
        // is only ONE seam, and it coincides with the existing full-res↔proxy handoff at 2× — no
        // separate mid-shuttle (was 12×) seam that froze on deceleration. We don't unload here;
        // the manager resumes full prefetch + cleanup once speed drops below 2×.
        if (currentRate >= 2.0) {
            previousPlaybackRate_ = currentRate;
            previousIsReverse_ = isReverse_.load();
            continue;
        }

        int currentSegment = current / segmentSize_;
        int numSegmentsTotal = (frameIndex_.size() + segmentSize_ - 1) / segmentSize_;
        bool segmentChanged = (currentSegment != previousSegment_);
        bool directionChanged = (isReverse_.load() != previousIsReverse_);

        // Just dropped out of shuttle (≥2× → <2×)? The manager skipped all prefetch during the
        // shuttle, and the position jumped far, so a naive ±1 window would BURST up to 3 segments
        // (3×750 = 2250 frames) of proxy decode at once — that CPU spike (alongside the full-res V2
        // re-decode) is the hitch felt when stopping a fast rewind onto pause. Load ONLY the current
        // segment on that first resume frame (the on-demand decodeFrameNow already supplies the exact
        // displayed frame + neighbours); the ±1 segments fill lazily as the user scrubs into them.
        bool justLeftShuttle = (previousPlaybackRate_ >= 2.0 && currentRate < 2.0);

        if (segmentChanged || directionChanged) {
            // SIMPLE SLIDING WINDOW APPROACH
            // Decode segments around current position regardless of direction
            // Window size: ±1 segment (total 3 segments: before, current, after) — but only the
            // current segment right after leaving shuttle (see justLeftShuttle above).

            std::set<int> targetSegments;

            // Always keep current segment + neighbors
            targetSegments.insert(currentSegment);
            if (!justLeftShuttle) {
                if (currentSegment > 0) {
                    targetSegments.insert(currentSegment - 1);  // Previous segment
                }
                if (currentSegment < numSegmentsTotal - 1) {
                    targetSegments.insert(currentSegment + 1);  // Next segment
                }
            }

            if (directionChanged) {
                std::cout << "🔄 [DIRECTION CHANGE] " << (isReverse_.load() ? "REVERSE" : "FORWARD")
                          << " at segment " << currentSegment << std::endl;
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

            // Unload segments not in target window
            for (int segIdx : segmentsToUnload) {
                int startFrame = segIdx * segmentSize_;
                int endFrame = startFrame + segmentSize_ - 1;
                LowResDecoder::removeLowResFrames(frameIndex_, startFrame, endFrame);
                loadedSegments_.erase(segIdx);
            }

            // Simple loading - no priority, no emergency checks
            for (int segIdx : segmentsToLoad) {
                loadSegment(segIdx);
            }

            lastLowResUpdateTime_ = now;
            previousSegment_ = currentSegment;
        } else if (needsUpdate) {
            // Same simple window logic as segment change
            std::set<int> targetSegments;
            targetSegments.insert(currentSegment);
            if (currentSegment > 0) {
                targetSegments.insert(currentSegment - 1);
            }
            if (currentSegment < numSegmentsTotal - 1) {
                targetSegments.insert(currentSegment + 1);
            }

            std::set<int> segmentsToLoad;
            for (int seg : targetSegments) {
                if (seg >= 0 && seg < numSegmentsTotal) {
                    if (loadedSegments_.find(seg) == loadedSegments_.end()) {
                        segmentsToLoad.insert(seg);
                    }
                }
            }

            for (int segIdx : segmentsToLoad) {
                loadSegment(segIdx);
            }

            lastLowResUpdateTime_ = now;
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

    // HALF-FPS: Convert to half-fps coordinates for highRes window
    if (isHalfFps_) {
        current = current / 2;
    }

    int highResStart = std::max(0, current - highResHalf);
    int highResEnd = std::min(static_cast<int>(frameIndex_.size()) - 1, current + highResHalf);

    // Pass isReverse to decoder for prioritized decoding.
    // keyframesOnly=false: Stage-1 keyframe-skip reverted (caused jumps via sparse fill); the
    // real shuttle fix is Stage-2 seek-per-frame.
    bool isReverse = isReverse_.load();
    bool ok = decoder_->decodeLowResRange(frameIndex_, startFrame, endFrame, highResStart, highResEnd, false, isReverse, false);
    if (ok) {
        std::lock_guard<std::mutex> lock(mtx_);
        loadedSegments_.insert(segmentIndex);

        // Force immediate frame update for seek responsiveness
        if (instanceId_ >= 0) {
            NotifyVideoFrameUpdate(instanceId_);
        }
        RequestForceRender();
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
    // ADAPTIVE SEGMENT SIZING based on video GOP structure + playback speed
    // Goal: Large segments for high-speed playback to reduce decoding frequency

    if (gopSize <= 0) {
        // Unknown or invalid GOP - use safe default
        std::cout << "[LowCachedManager] GOP unknown, using default 600 frames" << std::endl;
        return 600;
    }

    // Strategy: Use larger multiples of GOP for 32x support
    // Old: 2× GOP (600 frames max) - too small for 32x reverse
    // New: 8× GOP (2400 frames) - provides 3 second buffer @ 32x

    int targetSize = gopSize * 8;  // 8× GOP for 32x reverse support

    // Clamp to reasonable range: 300-2400 frames
    // Minimum 300 to avoid excessive seek overhead
    // Maximum 2400 for 32x reverse (3 seconds buffer @ 800 fps consumption)
    if (targetSize < 300) {
        targetSize = 300;
    } else if (targetSize > 2400) {
        targetSize = 2400;
    }

    std::cout << "[LowCachedManager] 🎯 Fixed large segment size: " << targetSize
              << " frames (GOP=" << gopSize << ", " << (targetSize / gopSize) << "× GOP)"
              << std::endl;
    std::cout << "                    Memory per segment: ~"
              << std::fixed << std::setprecision(1) << (targetSize * 0.345)
              << " MB (640×360 YUV420P/NV12)" << std::endl;

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
