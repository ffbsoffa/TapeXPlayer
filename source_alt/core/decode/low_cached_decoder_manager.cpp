#include "low_cached_decoder_manager.h"
#include <iostream>
#include <chrono>   // For std::chrono::milliseconds
#include <algorithm> // For std::min, std::max
#include <limits>    // For std::numeric_limits
#include <cmath>     // For std::abs
#include <iomanip> // For std::setw, std::setfill

// Forward declare AVFrame if needed, or include relevant header if getFrame returns it
struct AVFrame;

LowCachedDecoderManager::LowCachedDecoderManager(
    const std::string& lowResFilename,
    std::vector<FrameInfo>& frameIndex, 
    std::atomic<int>& currentFrame,
    int ringBufferCapacity,
    int highResWindowSize,
    std::atomic<bool>& isPlaying,
    std::atomic<double>& playbackRate,
    std::atomic<bool>& isReverseRef
) : 
    frameIndex_(frameIndex),
    currentFrame_(currentFrame),
    isPlaying_(isPlaying),
    playbackRate_(playbackRate),
    isReverse_(isReverseRef),
    ringBufferCapacity_(ringBufferCapacity),
    highResWindowSize_(highResWindowSize),
    segmentSize_(1250), // User requested segment size
    stopRequested_(false),
    isRunning_(false),
    lastNotifiedFrame_(-1),
    lastLowResUpdateTime_(std::chrono::steady_clock::time_point()), // Init to epoch
    previousPlaybackRate_(std::abs(playbackRate_.load())), // Init with current rate
    loadedSegments_(),
    previousSegment_(-1),
    previousIsReverse_(isReverse_.load()) // Initialize previous direction
{
    // std::cout << "LowCachedDecoderManager: Initializing..." << std::endl;
    decoder_ = std::make_unique<LowResDecoder>(lowResFilename);
    if (!decoder_ || !decoder_->isInitialized()) {
        std::cerr << "LowCachedDecoderManager Error: Failed to initialize LowResDecoder." << std::endl;
        // Handle initialization failure, maybe throw an exception or set an error state
        throw std::runtime_error("Failed to initialize LowResDecoder in LowCachedDecoderManager");
    }
    // std::cout << "LowCachedDecoderManager: Initialized successfully." << std::endl;
    // lastLowResUpdateTime_ = std::chrono::steady_clock::time_point(); // Already initialized

    // Optional: Preload the initial segment(s)
    int initialSegment = currentFrame_.load() / segmentSize_;
    int startFrame = initialSegment * segmentSize_;
    int endFrame = std::min(startFrame + segmentSize_ - 1, static_cast<int>(frameIndex_.size()) - 1);
    if (decoder_ && !frameIndex_.empty() && startFrame <= endFrame) {
        // std::cout << "LowCachedDecoderManager: Preloading initial segment " << initialSegment << " [" << startFrame << "-" << endFrame << "]" << std::endl;
        int dummyHsStart = 0, dummyHsEnd = -1; // No high-res window initially
        decoder_->decodeLowResRange(frameIndex_, startFrame, endFrame, dummyHsStart, dummyHsEnd, false);
        loadedSegments_.insert(initialSegment);
        lastLowResUpdateTime_ = std::chrono::steady_clock::now(); // Update time after preload
    }
}

LowCachedDecoderManager::~LowCachedDecoderManager() {
    stop(); // Ensure thread is stopped and joined
    // std::cout << "LowCachedDecoderManager: Destroyed." << std::endl;
}

void LowCachedDecoderManager::run() {
    if (isRunning_) {
        // std::cout << "LowCachedDecoderManager: Already running." << std::endl;
        return;
    }
    // std::cout << "LowCachedDecoderManager: Starting manager thread." << std::endl;
    stopRequested_ = false;
    isRunning_ = true; // Set isRunning before starting the thread
    managerThread_ = std::thread(&LowCachedDecoderManager::decodingLoop, this);
}

void LowCachedDecoderManager::stop() {
    if (!isRunning_ && !managerThread_.joinable()) {
        // Already stopped or never started
        return;
    }
    // std::cout << "LowCachedDecoderManager: Stopping manager thread..." << std::endl;
    stopRequested_ = true;
    cv_.notify_one(); // Wake up the thread if it's waiting
    if (managerThread_.joinable()) {
        managerThread_.join();
    }
    isRunning_ = false; // Mark as not running after thread finishes
    // std::cout << "LowCachedDecoderManager: Manager thread stopped." << std::endl;
}

void LowCachedDecoderManager::notifyFrameChange() {
    // No need for mutex here, just notify
    // std::cout << "LowCachedDecoderManager: Frame change notification received." << std::endl; 
    cv_.notify_one(); 
}

void LowCachedDecoderManager::decodingLoop() {
    // std::cout << "LowCachedDecoderManager: Decoding loop started." << std::endl;
    
    while (!stopRequested_) {
        int currentFrame = currentFrame_.load();
        double currentPlaybackRateAbs = std::abs(playbackRate_.load());
        double rateDifference = std::abs(currentPlaybackRateAbs - previousPlaybackRate_);
        const double significantRateChangeThreshold = 0.5; // Threshold for forcing update
        bool needsUpdate = false;
        auto now = std::chrono::steady_clock::now(); // Get current time

        { // Scope for lock - Wait for notification or timeout
            std::unique_lock<std::mutex> lock(mtx_);

            // Wait only if playing or if the frame hasn't changed significantly since last check
            // Add a timeout to periodically check even if not notified
            if (!cv_.wait_for(lock, std::chrono::milliseconds(100), [&] {
                return stopRequested_.load() || currentFrame_.load() != lastNotifiedFrame_;
            })) {
                // Timeout occurred, re-check conditions
                currentFrame = currentFrame_.load(); // Get latest frame
                if (stopRequested_ || currentFrame == lastNotifiedFrame_) {
                    // Still no change or stop requested, continue waiting
                    continue; 
                }
            }

            // Check stop condition again after waking up
            if (stopRequested_) break;

            // Frame has changed or was notified
            currentFrame = currentFrame_.load(); // Get latest frame after wait
            // // std::cout << "LowCachedDecoderManager: Woke up. Current Frame: " << currentFrame << ", Last Frame: " << lastNotifiedFrame_ << std::endl;

            if (currentFrame != lastNotifiedFrame_) {
                 needsUpdate = true;
                 lastNotifiedFrame_ = currentFrame; // Update last processed frame *under lock*
            }
        } // Lock released here

        // --- Speed Check: Disable and Clear if Speed >= 12.0x ---
        if (currentPlaybackRateAbs >= 16.0) {
            std::set<int> segmentsToClear; // Temporary set to hold segments for clearing
            bool hadSegmentsToClear = false;

            // Lock mutex to safely access and modify loadedSegments_
            {
                std::lock_guard<std::mutex> lock(mtx_);
                if (!loadedSegments_.empty()) {
                    hadSegmentsToClear = true;
                    // std::cout << "LowCachedDecoderManager: Speed >= 12.0x. Clearing "
                              // << loadedSegments_.size() << " loaded low-res segments." << std::endl;
                    segmentsToClear = loadedSegments_;
                    loadedSegments_.clear();          // Clear the original set immediately
                    previousSegment_ = -1;            // Reset segment tracking
                }
            } // Mutex released

            // Perform actual frame removal outside the lock, iterating over the copy
            if (hadSegmentsToClear) {
                for (int segIdx : segmentsToClear) {
                    int startFrame = segIdx * segmentSize_;
                    // Clamp endFrame safely using frameIndex_.size()
                    int endFrame = std::min(startFrame + segmentSize_ - 1, 
                                            static_cast<int>(frameIndex_.size()) - 1); 
                    if (startFrame <= endFrame) { // Ensure valid range before calling remove
                       LowResDecoder::removeLowResFrames(frameIndex_, startFrame, endFrame);
                       // std::cout << "LowCachedDecoderManager: Cleared low-res segment " << segIdx << std::endl; // Optional log
                    }
                }
                 // Optional: Log completion of clearing
                 // std::cout << "LowCachedDecoderManager: Finished clearing low-res segments." << std::endl;
            }

            // Skip the rest of the segment logic for this iteration
            continue; 

        } else {
            // --- Normal Segment-based Decoding Logic (Speed < 12.0x) ---
            if (decoder_ && !frameIndex_.empty() && segmentSize_ > 0) {
                int currentSegment = currentFrame / segmentSize_;
                int numSegmentsTotal = (frameIndex_.size() + segmentSize_ - 1) / segmentSize_;
                bool segmentChanged = (currentSegment != previousSegment_);
                bool directionChanged = (isReverse_.load() != previousIsReverse_);

                // --- Force immediate update if segment changed OR direction changed --- 
                if (segmentChanged || directionChanged) {
                     // if (segmentChanged) {
                         // std::cout << "LowCachedDecoderManager: Segment changed to " << currentSegment << ". Forcing update." << std::endl;
                     // } else { // directionChanged
                         // std::cout << "LowCachedDecoderManager: Direction changed. Forcing update." << std::endl;
                     // }
                      std::set<int> targetSegments; 
                      targetSegments.insert(currentSegment);
                      if (isReverse_.load()) {
                          if (currentSegment > 0) targetSegments.insert(currentSegment - 1);
                      } else {
                          if (currentSegment < numSegmentsTotal - 1) targetSegments.insert(currentSegment + 1);
                          if (currentPlaybackRateAbs >= 1.8 && currentSegment < numSegmentsTotal - 2) targetSegments.insert(currentSegment + 2);
                      }

                      std::set<int> segmentsToLoad;
                      std::set<int> segmentsToUnload = loadedSegments_;
                      for (int targetSeg : targetSegments) {
                          if (targetSeg >= 0 && targetSeg < numSegmentsTotal) {
                              if (loadedSegments_.find(targetSeg) == loadedSegments_.end()) { segmentsToLoad.insert(targetSeg); }
                              segmentsToUnload.erase(targetSeg);
                          }
                      }

                      // Unload immediately
                      for (int segIdx : segmentsToUnload) {
                          int startFrame = segIdx * segmentSize_;
                          int endFrame = startFrame + segmentSize_ - 1;
                          // std::cout << "LowCachedDecoderManager: Unloading segment (forced) " << segIdx << " [" << startFrame << "-" << endFrame << "]" << std::endl;
                          LowResDecoder::removeLowResFrames(frameIndex_, startFrame, endFrame);
                          loadedSegments_.erase(segIdx);
                      }

                      // Load immediately
                      for (int segIdx : segmentsToLoad) {
                          int startFrame = segIdx * segmentSize_;
                          int endFrame = std::min(startFrame + segmentSize_ - 1, static_cast<int>(frameIndex_.size()) - 1);
                          int highResHalfSize = highResWindowSize_ / 2;
                          int highResStart = std::max(0, currentFrame - highResHalfSize);
                          int highResEnd = std::min(static_cast<int>(frameIndex_.size()) - 1, currentFrame + highResHalfSize);
                          auto start_time = std::chrono::high_resolution_clock::now();
                          auto ms_since_epoch = std::chrono::duration_cast<std::chrono::milliseconds>(start_time.time_since_epoch()).count();
                          // std::cout << "[TimingDebug:" << ms_since_epoch << "] START decode segment (forced): " << segIdx << " [" << startFrame << "-" << endFrame << "] for currentFrame: " << currentFrame << std::endl;
                          bool success = decoder_->decodeLowResRange(frameIndex_, startFrame, endFrame, highResStart, highResEnd, false);
                          auto end_time = std::chrono::high_resolution_clock::now();
                          auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
                          ms_since_epoch = std::chrono::duration_cast<std::chrono::milliseconds>(end_time.time_since_epoch()).count();
                          // std::cout << "[TimingDebug:" << ms_since_epoch << "] END decode segment (forced):   " << segIdx << " Success: " << (success ? "Yes" : "No") << ". Duration: " << duration << "ms." << std::endl;
                          if (success) { loadedSegments_.insert(segIdx); }
                          else { std::cerr << "LowCachedDecoderManager Warning: Failed to load segment (forced) " << segIdx << std::endl; }
                      }
                      lastLowResUpdateTime_ = now; // Update time after forced load/unload
                      previousSegment_ = currentSegment; // Update segment tracker
                      // The interval check below might be skipped if forced update happened
                }
                
                // --- Normal Update Check (if segment/direction didn't change or after forced update) ---
                // Only check for loading *new* target segments if interval passed or speed changed significantly.
                else if (needsUpdate) { // Only check if frame actually moved within the segment
                    auto timeSinceLastUpdate = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastLowResUpdateTime_);
                    auto getUpdateInterval = [](double rate) {
                        if (rate < 0.9) return std::numeric_limits<int>::max();
                        // Speed check >= 10.0 is handled above, no need to return max here
                        // if (rate >= 10.0) return std::numeric_limits<int>::max(); 
                        if (rate <= 1.0) return 8000;
                        if (rate <= 1.8) return 5000;
                        if (rate <= 3.8) return 2500;
                        if (rate <= 7.8) return 900;
                        return 900; // Default for rates between 7.8 and 10.0
                    };
                    std::chrono::milliseconds lowResUpdateInterval(getUpdateInterval(currentPlaybackRateAbs));
                    bool forceUpdateDueToRateChange = (rateDifference > significantRateChangeThreshold);

                    // Recalculate target segments for potential loading
                    std::set<int> targetSegments; 
                    targetSegments.insert(currentSegment);
                    if (isReverse_.load()) {
                        if (currentSegment > 0) targetSegments.insert(currentSegment - 1);
                    } else {
                        if (currentSegment < numSegmentsTotal - 1) targetSegments.insert(currentSegment + 1);
                        if (currentPlaybackRateAbs >= 1.8 && currentSegment < numSegmentsTotal - 2) targetSegments.insert(currentSegment + 2);
                    }

                    std::set<int> segmentsToLoad;
                    for (int targetSeg : targetSegments) {
                        if (targetSeg >= 0 && targetSeg < numSegmentsTotal) { // Check bounds
                            if (loadedSegments_.find(targetSeg) == loadedSegments_.end()) {
                                segmentsToLoad.insert(targetSeg); // Not loaded, needs loading
                            }
                        }
                    }

                    if (!segmentsToLoad.empty() && (timeSinceLastUpdate >= lowResUpdateInterval || forceUpdateDueToRateChange)) {
                        for (int segIdx : segmentsToLoad) {
                            int startFrame = segIdx * segmentSize_;
                            int endFrame = std::min(startFrame + segmentSize_ - 1, static_cast<int>(frameIndex_.size()) - 1);
                            int highResHalfSize = highResWindowSize_ / 2;
                            int highResStart = std::max(0, currentFrame - highResHalfSize);
                            int highResEnd = std::min(static_cast<int>(frameIndex_.size()) - 1, currentFrame + highResHalfSize);
                            auto start_time = std::chrono::high_resolution_clock::now();
                            auto ms_since_epoch = std::chrono::duration_cast<std::chrono::milliseconds>(start_time.time_since_epoch()).count();
                            // std::cout << "[TimingDebug:" << ms_since_epoch << "] START decode segment (interval/rate): " << segIdx << " [" << startFrame << "-" << endFrame << "] for currentFrame: " << currentFrame << std::endl;
                            bool success = decoder_->decodeLowResRange(frameIndex_, startFrame, endFrame, highResStart, highResEnd, false);
                            auto end_time = std::chrono::high_resolution_clock::now();
                            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
                            ms_since_epoch = std::chrono::duration_cast<std::chrono::milliseconds>(end_time.time_since_epoch()).count();
                            // std::cout << "[TimingDebug:" << ms_since_epoch << "] END decode segment (interval/rate):   " << segIdx << " Success: " << (success ? "Yes" : "No") << ". Duration: " << duration << "ms." << std::endl;
                            if (success) { loadedSegments_.insert(segIdx); }
                            else { std::cerr << "LowCachedDecoderManager Warning: Failed to load segment (interval/rate) " << segIdx << std::endl; }
                        }
                        lastLowResUpdateTime_ = now; // Update time after load attempt
                    }
                    // Update segment tracker even if no load happened, as frame moved within segment
                    previousSegment_ = currentSegment; 
                }
            } // end if(decoder_ && !frameIndex_.empty() ...)
        } // end else (speed < 12.0x)

        // --- Idle Sleep (if paused and no frame change) ---
        // Placed outside the main speed check to allow sleeping even at high speeds if paused.
        if (!needsUpdate && !isPlaying_.load()) {
             std::this_thread::sleep_for(std::chrono::milliseconds(50)); 
        }

        // Update previous rate and direction at the end of the iteration
        previousPlaybackRate_ = currentPlaybackRateAbs;
        previousIsReverse_ = isReverse_.load();

    } // end while loop

    // std::cout << "LowCachedDecoderManager: Decoding loop finished." << std::endl;
    isRunning_ = false; // Ensure isRunning is false when loop exits
}
