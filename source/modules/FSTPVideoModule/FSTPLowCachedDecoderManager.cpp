#include "FSTPLowCachedDecoderManager.h"
#include "FSTPThreadName.h"

#include <iostream>
#include <chrono>
#include <algorithm>
#include <iomanip>
#include <limits>
#include <cmath>
#include <cstdlib>
#include <cstdio>

// Request force render from window system (defined in platform-specific WS)
extern "C" void RequestForceRender();

// Request frame update from video module (defined in FSTPVideoModule_wrapper.cpp)
extern void NotifyVideoFrameUpdate(int instance_id);

// Debug control: set to true to enable verbose logging
static constexpr bool ENABLE_LOWCACHED_DEBUG = false;

// Shuttle on-demand decode (see shuttleStep)
static constexpr double kShuttleRate = 2.0;        // from here up: decode what is shown
// Leave the tape-head mode only after the tape has run below ~1× for a while. A mouse/jog
// reversal or a stop passes through 0×; dropping straight into segment prefetch there started a
// synchronous 300-frame segment decode (up to ~1 s on a Pentium Gold 7505) that the head mode
// then had to wait out — the picture froze on every reversal and stop.
static constexpr double kShuttleHoldRate = 1.05;
static constexpr auto   kShuttleExitGrace = std::chrono::milliseconds(400);
// ...and only once the playhead has stood still this long. After a sharp shuttle the tape runs
// down for longer than the grace above (the requested speed is already 0, the tape still moves);
// leaving the head mode then left its last frame 6–8 frames short of where the tape stopped, and
// that frame stood on screen until the full-res one replaced it — the visible jump at the
// proxy→full-res switch. Kept while the playhead moves, the head reads the stop frame itself.
static constexpr auto   kShuttleSettle = std::chrono::milliseconds(150);
static constexpr size_t kShuttleKeepOnExit = 8;    // last head frames kept to bridge the stop
static constexpr int    kShuttleLookahead = 0;     // display ticks predicted ahead of the playhead
static constexpr double kShuttleDisplayRate = 60.0;
static constexpr size_t kShuttleKeep = 48;         // on-demand frames kept behind the playhead

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

    // Only the CURRENT frame is decoded here, synchronously, so there is a picture straight away
    // even without the full-res decoder. The segments around it are filled by the manager thread's
    // first pass (decodingLoop) once run() starts. This used to decode a whole default-size
    // segment (2400 frames, from frame 0 even when the file resumes elsewhere) on the loading
    // thread — 4.3 s before the first picture on a Pentium Gold 7505 in power-saver mode — and
    // SetupProxyDecoder then resized the segments and dropped it from loadedSegments_ unfreed.
    decodeFrameNow(currentFrame_.load());
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
    FSTPSetThreadName("fstp-proxy");
    static int wakeup_count = 0;
    static auto last_report = std::chrono::steady_clock::now();
    // First pass: fill the window around the current frame right away instead of waiting for
    // the position to move (at frame 0, paused, the wait below would never fire).
    bool firstPass = true;

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

        // SHUTTLE: decode what the display shows, not the whole stretch of tape (see shuttleStep).
        // FSTP_SHUTTLE_SEGMENTS=1 keeps the old segment prefetch at shuttle speed (A/B comparison).
        static const bool shuttle_on_demand = std::getenv("FSTP_SHUTTLE_SEGMENTS") == nullptr;
        const auto loopNow = std::chrono::steady_clock::now();
        if (inShuttleMode_) {
            const int playheadNow = currentFrame_.load();
            if (playheadNow != shuttleLastPlayhead_) {
                shuttleLastPlayhead_ = playheadNow;
                shuttleLastMove_ = loopNow;
            }
        }
        const bool keepHead = inShuttleMode_ &&
            (currentRate >= kShuttleHoldRate || loopNow - shuttleLastFast_ < kShuttleExitGrace ||
             loopNow - shuttleLastMove_ < kShuttleSettle);
        if (shuttle_on_demand && (currentRate >= kShuttleRate || keepHead) &&
            currentRate < speed_threshold.load() && decoder_) {
            if (!inShuttleMode_) enterShuttleMode();
            if (currentRate >= kShuttleHoldRate) shuttleLastFast_ = loopNow;
            const int playhead = currentFrame_.load();
            shuttleStep(playhead);
            previousPlaybackRate_ = currentRate;
            previousIsReverse_ = isReverse_.load();
            std::unique_lock<std::mutex> lock(mtx_);
            cv_.wait_for(lock, std::chrono::milliseconds(4), [&] {
                return stopRequested_.load() || currentFrame_.load() != playhead;
            });
            continue;
        }
        if (inShuttleMode_) {
            exitShuttleMode();
            firstPass = true;   // skip the wait once: refill the window where the shuttle stopped
        }
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
            if (!firstPass && !cv_.wait_for(lock, wait_duration, [&] {
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
            firstPass = false;

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

        // Restore the segment decoder: the ≥2× skip that idled the manager during shuttle and
        // pushed synchronous decodeFrameNow onto the display thread is REMOVED. The manager now
        // segment-prefetches at ALL speeds (as it did pre-"mega commit"/build1563). With the light
        // CPU proxy (540p) it keeps up, so the display no longer decodes — it only reads. Improves
        // shuttle dynamics + input responsiveness on slower HW (verified better on macOS).

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
            // GUARANTEE THE CURRENT FRAME FIRST. The 300-frame segment fill below takes time, and the
            // segment-decoder restore REMOVED the display-side decodeFrameNow that used to supply the
            // exact displayed frame — so on a seek/direction-reversal nothing put the current frame in
            // the buffer until the segment decode reached it. Meanwhile the display held its last
            // full-res frame (anti-flicker hold, FSTPVideoModule_wrapper.cpp) → a frozen field / frozen
            // Betacam "snow" = the visible stutter at pause→play-after-seek and shuttle direction
            // changes (owner-reported; confirmed by diag: full-res buffer emptied every jump, proxy
            // segment not yet at the new position). Decode JUST the current frame now — GOP-aware,
            // GOP=2 so ~1-2 frames, and decodeFrameNow no-ops if it's already present — on THIS manager
            // thread (decode contexts 2/3, never the display thread, so it can't stall present). This
            // restores the guarantee without the old display-thread block.
            decodeFrameNow(currentFrame_.load());
            decodeLeadIn(currentFrame_.load());

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

            loadSegmentsInOrder(segmentsToLoad, currentSegment);

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

            loadSegmentsInOrder(segmentsToLoad, currentSegment);

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

void LowCachedDecoderManager::enterShuttleMode() {
    inShuttleMode_ = true;
    shuttleLastPlayhead_ = currentFrame_.load();
    shuttleLastMove_ = std::chrono::steady_clock::now();
    shuttleLastFrame_ = -1;
    shuttleVelocity_ = 0.0;
    shuttleLastFast_ = std::chrono::steady_clock::now();
    // Prefetched segments stay put: the segment window unloads whatever it no longer needs when
    // the head mode ends (freeing them here thrashed when the speed hovered around kShuttleRate).
}

void LowCachedDecoderManager::exitShuttleMode() {
    inShuttleMode_ = false;
    // Keep the last few head frames — the picture at the stop point. The display keeps showing
    // the head frame (shuttleSlot) until the exact frame or the full-res one is there, instead
    // of freezing on a stale picture while the segment and full-res buffers refill.
    while (shuttleSlots_.size() > kShuttleKeepOnExit) {
        LowResDecoder::removeLowResFrames(frameIndex_, shuttleSlots_.front(), shuttleSlots_.front());
        shuttleSlots_.pop_front();
    }
    std::lock_guard<std::mutex> lock(mtx_);
    previousSegment_ = -1;   // the segment window reloads around the stop point
}

// One step of the shuttle "tape head": the frame on screen now, then where the next display tick
// will land (from the playhead's observed velocity). Each is a GOP-aware single-frame decode —
// on the GOP-4 proxy ~2.5 frames, ~7 ms on a Pentium Gold 7505 in power-saver mode — so the cost
// follows the ~60 pictures/s the display can show. Segment prefetch at 24× decoded ~1200 frames/s
// there, two cores' worth, to show the same ~54 pictures.
void LowCachedDecoderManager::shuttleStep(int current) {
    if (frameIndex_.empty()) return;
    const int lastFrame = isHalfFps_ ? static_cast<int>(frameIndex_.size()) * 2 - 1
                                     : static_cast<int>(frameIndex_.size()) - 1;
    auto now = std::chrono::steady_clock::now();
    if (shuttleLastFrame_ < 0) {
        shuttleLastFrame_ = current;
        shuttleLastTime_ = now;
    } else if (current != shuttleLastFrame_) {
        double dt = std::chrono::duration<double>(now - shuttleLastTime_).count();
        if (dt > 0.0) {
            double v = (current - shuttleLastFrame_) / dt;
            shuttleVelocity_ = (shuttleVelocity_ == 0.0) ? v : 0.6 * shuttleVelocity_ + 0.4 * v;
        }
        shuttleLastFrame_ = current;
        shuttleLastTime_ = now;
    }

    for (int k = 0; k <= kShuttleLookahead && !stopRequested_.load(); ++k) {
        int f = current + static_cast<int>(std::lround(shuttleVelocity_ * k / kShuttleDisplayRate));
        f = std::clamp(f, 0, lastFrame);
        if (k > 0 && f == current) break;   // no velocity yet
        const int slot = toSlot(f);
        if (std::find(shuttleSlots_.begin(), shuttleSlots_.end(), slot) != shuttleSlots_.end()) continue;
        auto t0 = std::chrono::steady_clock::now();
        bool ok = decodeFrameNow(f);
        static const bool stats = std::getenv("FSTP_RENDER_STATS") != nullptr;
        if (stats) {
            static int calls = 0; static double total_ms = 0, max_ms = 0;
            static auto window = std::chrono::steady_clock::now();
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            ++calls; total_ms += ms; max_ms = std::max(max_ms, ms);
            if (std::chrono::steady_clock::now() - window > std::chrono::seconds(1)) {
                std::printf("🎞️ [SHUTTLE STATS] decodes=%d avg=%.1fms max=%.1fms velocity=%.0f f/s\n",
                            calls, total_ms / calls, max_ms, shuttleVelocity_);
                calls = 0; total_ms = 0; max_ms = 0; window = std::chrono::steady_clock::now();
            }
        }
        if (ok) {
            shuttleSlots_.push_back(slot);
            shuttleShownSlot_.store(slot, std::memory_order_release);
            if (k == 0 && instanceId_ >= 0) NotifyVideoFrameUpdate(instanceId_);   // show it now
        }
    }

    while (shuttleSlots_.size() > kShuttleKeep) {
        const int slot = shuttleSlots_.front();
        shuttleSlots_.pop_front();
        LowResDecoder::removeLowResFrames(frameIndex_, slot, slot);
    }
}

void LowCachedDecoderManager::decodeLeadIn(int frame) {
    if (!decoder_ || frameIndex_.empty()) return;
    const int n = static_cast<int>(frameIndex_.size());
    const int idx = toSlot(frame);
    if (idx < 0 || idx >= n) return;
    constexpr int kLeadInFrames = 48;   // ~1 s at 50 fps, 2 s at 25
    int first = idx, last = idx;
    if (isReverse_.load()) first = std::max(0, idx - kLeadInFrames);
    else                   last  = std::min(n - 1, idx + kLeadInFrames);
    // Contexts 2/3 (the on-demand ones): the segment loads below use 0/1.
    decoder_->decodeLowResRange(frameIndex_, first, last, 0, -1, false, /*isReverse=*/false, false,
                                /*threadIdBase=*/2);
    if (instanceId_ >= 0) NotifyVideoFrameUpdate(instanceId_);
}

void LowCachedDecoderManager::loadSegmentsInOrder(const std::set<int>& segments, int currentSegment) {
    const int ahead = isReverse_.load() ? currentSegment - 1 : currentSegment + 1;
    const int behind = isReverse_.load() ? currentSegment + 1 : currentSegment - 1;
    for (int seg : {currentSegment, ahead, behind}) {
        if (segments.count(seg)) loadSegment(seg);
    }
    for (int seg : segments) {   // anything else (wider windows), in index order
        if (seg != currentSegment && seg != ahead && seg != behind) loadSegment(seg);
    }
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
    segmentLoadInProgress_.store(true, std::memory_order_release);
    bool ok = decoder_->decodeLowResRange(frameIndex_, startFrame, endFrame, highResStart, highResEnd, false, isReverse, false);
    segmentLoadInProgress_.store(false, std::memory_order_release);
    if (ok && decoder_->isStopRequested()) {
        ok = false;   // interrupted for shuttle: partially filled, don't mark it loaded
    }
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
