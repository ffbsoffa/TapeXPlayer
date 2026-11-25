#ifndef FSTPLOWCACHEDDECODERMANAGER_H
#define FSTPLOWCACHEDDECODERMANAGER_H

#pragma once

#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>
#include <string>
#include <memory> // For std::unique_ptr
#include <chrono> // Added for time point
#include <set>    // Added for segment tracking
#include <deque>
#include "FSTPVideoFrame.h"

#include "FSTPLowResDecoder.h"
#include "FSTPVideoFrame.h"

namespace FSTP {

class LowCachedDecoderManager {
public:
    static std::atomic<double> speed_threshold; // New static variable for speed threshold
    
    LowCachedDecoderManager(
        const std::string& lowResFilename,
        std::vector<FSTP::FrameInfo>& frameIndex,
        std::atomic<int>& currentFrame,
        int ringBufferCapacity,      // How many low-res frames to keep around the current frame
        int highResWindowSize,       // Size of the high-res window (to potentially skip decoding low-res)
        std::atomic<bool>& isPlaying, // Shared playback status
        std::atomic<double>& playbackRate, // Added
        std::atomic<bool>& isReverseRef, // Added
        int instanceId = -1, // Instance ID for frame update notification
        bool isHalfFps = false // HALF-FPS: true if currentFrame is in original coords but frameIndex is half-size
    );

    ~LowCachedDecoderManager();

    // Delete copy constructor and assignment operators (due to atomic members and thread management)
    LowCachedDecoderManager(const LowCachedDecoderManager&) = delete;
    LowCachedDecoderManager& operator=(const LowCachedDecoderManager&) = delete;

    // Start the manager's background thread
    void run();

    // Stop the manager's background thread
    void stop();

    // Notify the manager about a potential seek or change in current frame
    void notifyFrameChange();

    // STAGE 2 (shuttle seek-per-frame): decode exactly one frame on the CALLING thread, into the
    // shared index. Used by the display at high speed where segment prefetch is skipped — gives a
    // "tape head" response (decode only what's shown, ~1-2ms on GOP=4). No-op if already decoded.
    bool decodeFrameNow(int frame);

    // SHUTTLE: the slot the "tape head" read last (-1 outside shuttle). The display shows it when
    // the exact frame under the playhead isn't decoded yet — like a VTR in shuttle shows whatever
    // the heads pick up — instead of holding a stale picture until an exact match turns up.
    int shuttleSlot() const { return shuttleShownSlot_.load(std::memory_order_acquire); }

    // True while the "tape head" mode runs (frames are read on demand under the playhead)
    bool headActive() const { return headActive_.load(std::memory_order_acquire); }
    // A new playhead: wake the head at once instead of at its next 4 ms poll
    void wakeHead() { headWake_.store(true, std::memory_order_release); cv_.notify_all(); }
    // The display, slow, found the exact frame not read yet: wake the head and wait up to
    // `timeout` for `ready` (the frame is in its slot). The head reads one frame in ~1 ms (GPD),
    // a few on the Mac. True when it came in time.
    bool waitHead(const std::function<bool()>& ready, std::chrono::milliseconds timeout);

    // Called when the tape speeds up into shuttle: aborts a segment prefetch in flight so the
    // tape-head mode can take over at once (the aborted segment is not marked loaded).
    void interruptPrefetchForShuttle() {
        if (segmentLoadInProgress_.load(std::memory_order_acquire) && decoder_) decoder_->requestStop();
    }

    // Adaptive segment sizing based on GOP structure
    void setSegmentSizeFromGOP(int gopSize);
    static int calculateOptimalSegmentSize(int gopSize); 

private:
    // The main loop running on the background thread
    void decodingLoop();

    // The decoder instance responsible for low-res decoding
    std::unique_ptr<LowResDecoder> decoder_;

    // Shared data references
    std::vector<FSTP::FrameInfo>& frameIndex_;
    std::atomic<int>& currentFrame_;
    std::atomic<bool>& isPlaying_;
    std::atomic<double>& playbackRate_; // Added
    std::atomic<bool>& isReverse_;      // Added

    // Configuration
    int ringBufferCapacity_;
    int highResWindowSize_;
    int segmentSize_ = 750;    // Golden middle: balance between CPU and responsiveness reverse
    int instanceId_ = -1;      // Instance ID for frame update notifications
    bool isHalfFps_ = false;   // HALF-FPS: currentFrame in original coords, frameIndex is half-size

    // Thread management
    std::thread managerThread_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::atomic<bool> isRunning_{false};
    std::atomic<bool> stopRequested_{false};
    
    // Internal state for the decoding loop
    int lastNotifiedFrame_ = -1; // To track changes in currentFrame_
    std::chrono::steady_clock::time_point lastLowResUpdateTime_; // Added
    double previousPlaybackRate_ = 0.0; // Added
    std::set<int> loadedSegments_; // Added - tracks loaded segment indices
    int previousSegment_ = -1; // Added - tracks last processed segment index
    bool previousIsReverse_ = false; // Added - tracks last direction state

    // SHUTTLE (≥ kShuttleRate): decode only the frames the display is about to show — the current
    // one plus a few predicted from the observed playhead velocity — instead of whole segments.
    // The work then follows the display rate (~60 pictures/s), not the tape speed: 8×, 24× and
    // 32× cost the same. Segment prefetch is kept below kShuttleRate, where neighbouring frames
    // are what gets shown.
    void shuttleStep(int current);
    // ~1 s of proxy frames from the playhead in the direction of travel, decoded before the
    // 300-frame segments, so playback that starts here (after a seek or a shuttle) has pictures
    // at once instead of waiting for the segment that contains the playhead.
    void decodeLeadIn(int frame);
    // Loads segments current-first, then the one ahead in the direction of travel, then behind.
    void loadSegmentsInOrder(const std::set<int>& segments, int currentSegment);
    void enterShuttleMode();
    void exitShuttleMode();
    int toSlot(int frame) const { return isHalfFps_ ? frame / 2 : frame; }
    bool inShuttleMode_ = false;
    std::deque<int> shuttleSlots_;          // slots decoded on demand, oldest first
    int shuttleLastFrame_ = -1;             // playhead at the last velocity sample
    std::chrono::steady_clock::time_point shuttleLastTime_{};
    double shuttleVelocity_ = 0.0;          // frames/s, signed (smoothed from observed motion)
    double shuttleInstVelocity_ = 0.0;      // frames/s, signed, the last sample alone (no lag)
    std::atomic<int> shuttleShownSlot_{-1};
    std::atomic<bool> headActive_{false};
    std::atomic<bool> headWake_{false};     // run a head step now, even if the playhead has not moved
    std::mutex headMtx_;                    // with headCv_: "a head frame was read"
    std::condition_variable headCv_;
    std::atomic<bool> segmentLoadInProgress_{false};
    std::chrono::steady_clock::time_point shuttleLastFast_{};   // last time the tape ran ≥ hold rate
    int shuttleLastPlayhead_ = -1;                               // playhead at the last loop pass
    std::chrono::steady_clock::time_point shuttleLastMove_{};   // last time the playhead moved

    // Private methods
    void loadSegment(int segmentIndex);   // Declaration added
    void unloadSegment(int segmentIndex); // Declaration added
};

} // namespace FSTP

#endif // FSTPLOWCACHEDDECODERMANAGER_H 