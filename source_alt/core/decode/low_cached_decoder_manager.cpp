#include "low_cached_decoder_manager.h"
#include <iostream>
#include <chrono>   // For std::chrono::milliseconds
#include <algorithm> // For std::min, std::max
#include <limits>    // For std::numeric_limits
#include <cmath>     // For std::abs
#include <iomanip> // For std::setw, std::setfill

// Initialize static member
std::atomic<double> LowCachedDecoderManager::speed_threshold(16.0);

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
    segmentSize_(2750), // User requested segment size
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

        // --- Speed Check: Disable and Clear if Speed >= threshold ---
        if (currentPlaybackRateAbs >= speed_threshold.load()) {
            std::set<int> segmentsToClear; // Temporary set to hold segments for clearing
            bool hadSegmentsToClear = false;

            // Lock mutex to safely access and modify loadedSegments_
            {
                std::lock_guard<std::mutex> lock(mtx_);
                if (!loadedSegments_.empty()) {
                    hadSegmentsToClear = true;
                    // std::cout << "LowCachedDecoderManager: Speed >= threshold. Clearing "
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
            // --- Normal Segment-based Decoding Logic (Speed < threshold) ---
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

                      // Load immediately, prioritizing current segment
                      if (segmentsToLoad.count(currentSegment)) {
                          // std::cout << "  Prioritizing load of current segment: " << currentSegment << std::endl;
                          loadSegment(currentSegment);
                          segmentsToLoad.erase(currentSegment); // Remove from set after loading
                      }
                      // Load remaining needed segments
                      for (int segIdx : segmentsToLoad) {
                         loadSegment(segIdx);
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
                        if (rate <= 1.0) return 10000;
                        if (rate <= 1.8) return 5000;
                        if (rate <= 3.8) return 2500;
                        if (rate <= 7.8) return 1250;
                        return 1250; // Default for rates between 7.8 and 10.0
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
                         // Prioritize loading current segment if needed within interval/rate check
                         if (segmentsToLoad.count(currentSegment)) {
                             // std::cout << "  Prioritizing load of current segment (interval/rate): " << currentSegment << std::endl;
                             loadSegment(currentSegment);
                             segmentsToLoad.erase(currentSegment);
                         }
                         // Load remaining needed segments
                        for (int segIdx : segmentsToLoad) {
                            loadSegment(segIdx);
                        }
                        lastLowResUpdateTime_ = now; // Update time after load attempt
                    }
                    // Update segment tracker even if no load happened, as frame moved within segment
                    previousSegment_ = currentSegment; 
                }
            } // end if(decoder_ && !frameIndex_.empty() ...)
        } // end else (speed < threshold)

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

// --- Helper function to load a segment --- 
void LowCachedDecoderManager::loadSegment(int segmentIndex) {
    if (!decoder_ || !decoder_->isInitialized()) { 
         std::cerr << "LowCachedDecoderManager Error: Decoder not initialized in loadSegment." << std::endl;
         return;
    }
    if (frameIndex_.empty() || segmentSize_ <= 0) return;

    // Check if already loaded (needs lock)
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (loadedSegments_.count(segmentIndex)) {
            // std::cout << "LowCachedDecoderManager: Segment " << segmentIndex << " already loaded or loading." << std::endl;
            return; // Already loaded or being loaded by another thread check
        }
        // Mark as loading (optional, depends on if multiple threads could call this)
        // loadedSegments_.insert(segmentIndex); // Or use a separate loading set
    }

    int startFrame = segmentIndex * segmentSize_;
    int endFrame = std::min(startFrame + segmentSize_ - 1, static_cast<int>(frameIndex_.size()) - 1);

    if (startFrame > endFrame) return;

    // std::cout << "LowCachedDecoderManager: Loading segment " << segmentIndex << " [" << startFrame << "-" << endFrame << "]" << std::endl;

    int currentFrame = currentFrame_.load(); // Get current frame for context
    int highResHalfSize = highResWindowSize_ / 2;
    int highResStart = std::max(0, currentFrame - highResHalfSize);
    int highResEnd = std::min(static_cast<int>(frameIndex_.size()) - 1, currentFrame + highResHalfSize);

    bool success = decoder_->decodeLowResRange(frameIndex_, startFrame, endFrame, highResStart, highResEnd, false);

    if (success) {
        std::lock_guard<std::mutex> lock(mtx_);
        loadedSegments_.insert(segmentIndex); // Add to set *after* successful load
        // std::cout << "LowCachedDecoderManager: Successfully loaded segment " << segmentIndex << ". Total loaded: " << loadedSegments_.size() << std::endl;
    } else {
        std::cerr << "LowCachedDecoderManager Warning: Failed to load segment " << segmentIndex << std::endl;
        // Optional: remove from loading set if used
    }
}

// --- Helper function to unload a segment --- 
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
            // std::cout << "LowCachedDecoderManager: Unloading segment " << segmentIndex << " [" << startFrame << "-" << endFrame << "]" << std::endl;
            LowResDecoder::removeLowResFrames(frameIndex_, startFrame, endFrame);
        }
    }
}
