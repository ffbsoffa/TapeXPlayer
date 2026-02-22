#ifndef FSTPLOWCACHEDDECODERMANAGER_H
#define FSTPLOWCACHEDDECODERMANAGER_H

#pragma once

#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <string>
#include <memory> // For std::unique_ptr
#include <chrono> // Added for time point
#include <set>    // Added for segment tracking
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

    // Private methods
    void loadSegment(int segmentIndex);   // Declaration added
    void unloadSegment(int segmentIndex); // Declaration added
};

} // namespace FSTP

#endif // FSTPLOWCACHEDDECODERMANAGER_H 