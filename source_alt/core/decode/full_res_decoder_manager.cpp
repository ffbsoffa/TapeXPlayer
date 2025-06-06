#include "full_res_decoder_manager.h"
#include "../common/common.h" // For seekInfo, speed_reset_requested etc.
#include <iostream>
#include <chrono>
#include <limits> // For numeric_limits
#include <cmath>    // For std::abs
#include <algorithm> // For std::min, std::max

// Forward declarations for global atomics from common.h (if needed here, better to pass as refs)
// extern SeekInfo seekInfo;
// extern std::atomic<bool> speed_reset_requested;
// extern std::atomic<bool> shouldExit;

FullResDecoderManager::FullResDecoderManager(
    const std::string& filename,
    std::vector<FrameInfo>& frameIndex,
    std::atomic<int>& currentFrame,
    std::atomic<double>& playbackRate,
    const int& highResWindowSizeRef,
    std::atomic<bool>& isPlaying,
    std::atomic<bool>& isReverseRef
) : 
    filename_(filename),
    frameIndex_(frameIndex),
    currentFrame_(currentFrame),
    playbackRate_(playbackRate),
    isPlaying_(isPlaying),
    highResWindowSize_(highResWindowSizeRef),
    isReverse_(isReverseRef),
    stopRequested_(false),
    isRunning_(false),
    lastProcessedFrame_(-1),
    lastDecodeCheckTime_(std::chrono::steady_clock::now()),
    lastHighResUpdateTime_(std::chrono::steady_clock::time_point()),
    nextScheduledHighResTime_(std::chrono::steady_clock::now()),
    highResConditionsMetPreviously_(false)
{
    // --- ADDED: Log for manager constructor ---
    std::cout << "[FRDM CONSTRUCTOR TID:" << std::this_thread::get_id() << "] FullResDecoderManager CREATED. Decoder ptr will be: " << decoder_.get() << " (though not yet assigned)" << std::endl;
    std::cerr << "[FRDM CONSTRUCTOR TID:" << std::this_thread::get_id() << "] FullResDecoderManager CREATED. Decoder ptr will be: " << decoder_.get() << " (though not yet assigned)" << std::endl;

    const std::chrono::milliseconds highResUpdateInterval(18000); // Define interval constant here too
    decoder_ = std::make_unique<FullResDecoder>(filename_);
    if (!decoder_ || !decoder_->isInitialized()) {
        std::cerr << "FullResDecoderManager Error: Failed to initialize FullResDecoder." << std::endl;
        throw std::runtime_error("Failed to initialize FullResDecoder in FullResDecoderManager");
    }
    // std::cout << "FullResDecoderManager: Initialized successfully." << std::endl;

    // --- Perform initial decode during construction --- 
    if (decoder_ && !frameIndex_.empty()) {
        // std::cout << "[FRDM Constructor] Performing initial decode..." << std::endl;
        int initialFrame = 0; // Start centered at frame 0
        int windowSize = highResWindowSize_; // Get window size
        int sizeBehind = static_cast<int>(windowSize * 0.10);
        int sizeAhead = windowSize - sizeBehind;
        int initialStart = std::max(0, initialFrame - sizeBehind); 
        int initialEnd = std::min(static_cast<int>(frameIndex_.size()) - 1, initialFrame + sizeAhead);

        if (initialStart <= initialEnd) {
            bool success = decoder_->decodeFrameRange(frameIndex_, initialStart, initialEnd);
            if (!success) {
                std::cerr << "[FRDM Constructor] Warning: Initial decodeFrameRange failed for [" 
                          << initialStart << "-" << initialEnd << "]" << std::endl;
            }
            // Schedule the next update after the initial decode
            nextScheduledHighResTime_ = std::chrono::steady_clock::now() + highResUpdateInterval;
            // std::cout << "[FRDM Constructor] Initial decode finished. Next decode scheduled for: " << nextScheduledHighResTime_.time_since_epoch().count() << std::endl;
        } else {
            // std::cout << "[FRDM Constructor] Warning: Cannot perform initial decode, invalid range [" << initialStart << "-" << initialEnd << "]" << std::endl;
        }
    }

    // Initialize time point to epoch to ensure the first check passes
    lastHighResUpdateTime_ = std::chrono::steady_clock::time_point(); 
}

FullResDecoderManager::~FullResDecoderManager() {
    stop(); // Ensure thread is stopped and joined
    
    // Ensure any remaining async operation is finished
    {
        std::lock_guard<std::mutex> lock(decodingFutureMutex_);
        if (decodingFuture_.valid()) {
            try {
                decodingFuture_.wait(); // Wait for completion
                decodingFuture_.get();  // Get result to clear state
            } catch (...) {
                // Ignore exceptions during cleanup
            }
        }
    }
    
    // std::cout << "FullResDecoderManager: Destroyed." << std::endl;
}

void FullResDecoderManager::run() {
    if (isRunning_) {
        // std::cout << "FullResDecoderManager: Already running." << std::endl;
        return;
    }
    if (!decoder_ || !decoder_->isInitialized()) {
        std::cerr << "FullResDecoderManager Error: Decoder not initialized. Cannot run." << std::endl;
        return;
    }
    // std::cout << "FullResDecoderManager: Starting manager thread." << std::endl;
    stopRequested_ = false;
    isRunning_ = true;
    managerThread_ = std::thread(&FullResDecoderManager::decodingLoop, this);
}

void FullResDecoderManager::stop() {
    if (!isRunning_ && !managerThread_.joinable()) {
        return;
    }
    
    stopRequested_ = true;
    
    // Cancel any ongoing async decode
    cancelOngoingDecode();
    
    cv_.notify_one();
    
    if (managerThread_.joinable()) {
        managerThread_.join();
    }
    
    isRunning_ = false;
    
    // Clear any remaining high-res frames after stop
    if (decoder_) {
        decoder_->clearHighResFrames(frameIndex_);
    }
}

void FullResDecoderManager::notifyFrameChange() {
    // std::cout << "FullResDecoderManager: Frame change notification received." << std::endl; 
    cv_.notify_one(); 
}

// Determines if high-res should be active based on playback rate and direction
bool shouldDecodeHighRes(double playbackRateAbs, bool isReverse) {
    // Check if playbackRate is very close to 1.0 (within a small tolerance)
    const double epsilon = 0.01;
    return std::abs(playbackRateAbs - 1.0) < epsilon && !isReverse;
}

// Helper function to get update interval based on speed and direction
int getHighResUpdateIntervalMs(double playbackRateAbs, bool isReverse) {
    // If high-res is enabled (1x forward), use 18 seconds interval
    if (shouldDecodeHighRes(playbackRateAbs, isReverse)) {
        return 18000; 
    } else {
        // Otherwise, disable decoding by interval
        return std::numeric_limits<int>::max();
    }
}

void FullResDecoderManager::decodingLoop() {
    std::cout << "[FRDM TID:" << std::this_thread::get_id() << "] decodingLoop ENTERED. Decoder ptr: " << decoder_.get() << std::endl;
    std::cerr << "[FRDM TID:" << std::this_thread::get_id() << "] decodingLoop ENTERED. Decoder ptr: " << decoder_.get() << std::endl;

    // std::cout << "FullResDecoderManager: Decoding loop started. Initial next scheduled time: " << nextScheduledHighResTime_.time_since_epoch().count() << std::endl;
    const std::chrono::milliseconds highResUpdateInterval(18000); // Keep interval definition here as well
    
    while (!stopRequested_) {
        // Check if manager should be active based on window size first
        if (!isHighResActive_.load()) {
            // If not active, just wait for conditions to change (e.g., window resize activation)
            // Or for stop request.
            std::unique_lock<std::mutex> lock(mtx_);
            cv_.wait_for(lock, std::chrono::milliseconds(500), [&] {
                return stopRequested_.load() || isHighResActive_.load(); 
            });
            if (stopRequested_.load()) break;
            if (isHighResActive_.load()) {
                // std::cout << "[FRDM] Woke up and now active, re-evaluating..." << std::endl;
            } else {
                // std::cout << "[FRDM] Woke up but still inactive, sleeping again..." << std::endl;
                continue; // Loop back to check stopRequested_ or isHighResActive_
            }
        }

        // Declare variables needed in multiple branches outside the inactive check
        int currentFrame = currentFrame_.load();
        double playbackRateAbs = std::abs(playbackRate_.load());
        bool frameChanged = false;
        auto now = std::chrono::steady_clock::now(); // Get current time
        bool highResConditionsMetNow = false; // Declare and initialize here
        bool justReturnedToHighRes = false; // Initialize here

        if (current_decoder_hw_failed_permanently_) {
            std::cerr << "[FRDM TID:" << std::this_thread::get_id() << "] Loop stopping: Manager_hw_failed_flag IS TRUE. Decoder ptr: " << decoder_.get() << std::endl;
            stopRequested_ = true; 
            continue; 
        }

        { // Scope for lock
            std::unique_lock<std::mutex> lock(mtx_);
            
            // Wait for notification or timeout
            // Timeout allows periodic checks even if paused or no notifications received
            if (!cv_.wait_for(lock, std::chrono::milliseconds(200), [&] {
                 // Wake if stop requested OR if the current frame is different from the last one processed
                return stopRequested_.load() || currentFrame_.load() != lastProcessedFrame_;
            })) {
                // Timeout occurred
                currentFrame = currentFrame_.load(); // Re-check current frame
                 if (stopRequested_.load() || currentFrame == lastProcessedFrame_) {
                     // No change since last check or stop requested, continue waiting
                     continue;
                 }
            }

            // Check stop condition again after waking up
            if (stopRequested_) break;

            // Frame has changed or was notified
            currentFrame = currentFrame_.load(); // Get latest frame
            if (currentFrame != lastProcessedFrame_) {
                frameChanged = true;
                lastProcessedFrame_ = currentFrame; // Update last processed frame HERE, before checks
                // // std::cout << "FullResDecoderManager: Frame changed to " << currentFrame << std::endl;
            } else {
                frameChanged = false;
            }
        } // Lock released

        // Update the previous state for the next iteration
        highResConditionsMetPreviously_ = highResConditionsMetNow;

        // Optional minimal sleep to prevent tight loop if conditions change rapidly
        // std::this_thread::sleep_for(std::chrono::milliseconds(5));

        // --- Check speed and clear high-res frames if > 1.1x --- 
        if (isHighResActive_.load()) { // Only do speed-based clearing if active
            double current_speed = std::abs(playbackRateAbs); 
            if (current_speed > 1.1) { 
                // std::cout << "[FullResManager] Speed > 1.1x. Cancelling async decode and clearing high-res frames." << std::endl;
                
                // Cancel any ongoing async decode
                cancelOngoingDecode();

                if (decoder_) {
                     decoder_->clearHighResFrames(frameIndex_); // Clear existing high-res frames
                }

                highResConditionsMetPreviously_ = false; // Reset state to allow re-triggering decode when speed normalizes
                nextScheduledHighResTime_ = std::chrono::steady_clock::now(); // Allow immediate re-evaluation if speed drops to 1x

                // Sleep briefly to avoid busy-waiting when speed is high
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue; // Skip any scheduled decoding for this iteration
            }
        }
        // --- End speed check ---

        // --- High-Resolution Window Management ---
        if (decoder_ && !frameIndex_.empty()) {
            bool isRev = isReverse_.load();
            highResConditionsMetNow = shouldDecodeHighRes(playbackRateAbs, isRev);
            justReturnedToHighRes = highResConditionsMetNow && !highResConditionsMetPreviously_;

            int windowSize = highResWindowSize_;
            int sizeBehind = static_cast<int>(windowSize * 0.10);
            int sizeAhead = windowSize - sizeBehind;
            int highResStart = std::max(0, currentFrame - sizeBehind);
            int highResEnd = std::min(static_cast<int>(frameIndex_.size()) - 1, currentFrame + sizeAhead);
            
            // REMOVED: Timestamp repair - let original timestamps work naturally
            
            // Decode if high-res conditions are met AND (
            //   current frame changed OR
            //   we just returned to high-res mode OR
            //   the scheduled update time has been reached )
            bool shouldTriggerDecode = highResConditionsMetNow && 
                                       (frameChanged || justReturnedToHighRes || now >= nextScheduledHighResTime_);

            if (shouldTriggerDecode && highResStart <= highResEnd) {
                // Check if there's an ongoing decode
                {
                    std::lock_guard<std::mutex> lock(decodingFutureMutex_);
                    if (decodingFuture_.valid()) {
                        // Check if previous decode finished
                        auto status = decodingFuture_.wait_for(std::chrono::milliseconds(0));
                        if (status != std::future_status::ready) {
                            // Previous decode still running, skip this iteration
                            // Commented out to reduce log spam
                            // std::cout << "[FRDM] Previous decode still running, skipping new decode request" << std::endl;
                            continue;
                        } else {
                            // Get result to clear the future
                            try {
                                bool prevResult = decodingFuture_.get();
                                if (!prevResult && decoder_ && decoder_->isHardwareAccelerated() && decoder_->hasHardwareFailedIrrecoverably()) {
                                    std::cerr << "[FRDM] Previous decode failed with HW error" << std::endl;
                                    current_decoder_hw_failed_permanently_ = true;
                                }
                            } catch (...) {
                                std::cerr << "[FRDM] Exception getting previous decode result" << std::endl;
                            }
                        }
                    }
                }
                
                // Launch async decode
                std::cout << "[FRDM TID:" << std::this_thread::get_id() << "] Launching ASYNC decodeFrameRange. Decoder ptr: " << decoder_.get() 
                          << ", isHW: " << (decoder_ ? decoder_->isHardwareAccelerated() : -1) 
                          << ", hasHWfailed_flag: " << (decoder_ ? decoder_->hasHardwareFailedIrrecoverably() : -1) 
                          << ", Range: [" << highResStart << "-" << highResEnd << "]" << std::endl;
                
                {
                    std::lock_guard<std::mutex> lock(decodingFutureMutex_);
                    // Capture necessary values for lambda
                    auto localDecoder = decoder_.get();
                    auto& localFrameIndex = frameIndex_;
                    
                    decodingFuture_ = std::async(std::launch::async, [localDecoder, &localFrameIndex, highResStart, highResEnd]() {
                        if (!localDecoder) return false;
                        return localDecoder->decodeFrameRange(localFrameIndex, highResStart, highResEnd);
                    });
                }
                
                nextScheduledHighResTime_ = now + highResUpdateInterval; // Schedule next forced update
            }

// --- Cleanup (Restored from 10/04 version) --- 
            // Clean up frames outside the *new* high-res window. 
            // This uses the static method. Needs careful synchronization if frameIndex is heavily shared.
            
            // // std::cout << "FullResDecoderManager: Cleaning frames outside [" << highResStart << "-" << highResEnd << "]" << std::endl;
            // Clean before the window
            if (highResStart > 0) {
                 FullResDecoder::removeHighResFrames(frameIndex_, 0, highResStart - 1, highResStart, highResEnd);
            }
            // Clean after the window
            if (highResEnd < static_cast<int>(frameIndex_.size()) - 1) {
                 FullResDecoder::removeHighResFrames(frameIndex_, highResEnd + 1, frameIndex_.size() - 1, highResStart, highResEnd);
            }
            
             /* --- REMOVED: Aggressive clearing of all high-res frames when not at 1.0x speed ---
            // If high-res decoding is completely disabled (e.g., high speed), clear all high-res frames
            if (!highResConditionsMetNow) { // Use the current condition flag
                // // std::cout << "FullResDecoderManager: High-res disabled, clearing all." << std::endl;
                FullResDecoder::clearHighResFrames(frameIndex_);
            }
            */

            // lastProcessedFrame_ is now updated earlier, before the checks

        } else {
             // Decoder not ready or frame index empty
             // Update last processed frame anyway to avoid re-processing immediately on next loop
             if(frameChanged) {
                  lastProcessedFrame_ = currentFrame;
             }
        }

    } // end while loop

    // std::cout << "FullResDecoderManager: Decoding loop finished." << std::endl;
    isRunning_ = false; // Ensure isRunning is false when loop exits
} 

// --- ADDED --- Getter implementation for the decoder instance
FullResDecoder* FullResDecoderManager::getDecoder() const {
    return decoder_.get(); // Return raw pointer from unique_ptr
}

void FullResDecoderManager::checkWindowSizeAndToggleActivity(int windowWidth, int windowHeight) {
    std::lock_guard<std::mutex> lock(activityCheckMutex_); // Protect access to decoder_ and isHighResActive_
    if (!decoder_ || !decoder_->isInitialized()) {
        isHighResActive_ = false;
        return;
    }

    int nativeWidth = decoder_->getWidth();
    int nativeHeight = decoder_->getHeight();

    // Condition to deactivate: if window width OR height is less than half of native
    // Consider using AND if both dimensions must be small, or if area is a better metric.
    bool shouldBeActive = true;
    if (nativeWidth > 0 && nativeHeight > 0) { // Ensure native dimensions are valid
        if (windowWidth < nativeWidth / 2 || windowHeight < nativeHeight / 2) {
            shouldBeActive = false;
        }
    }

    if (isHighResActive_ && !shouldBeActive) {
        // Transitioning from active to inactive
        std::cout << "[FRDM] Deactivating FullRes decoding due to small window size (" 
                  << windowWidth << "x" << windowHeight << " vs native " 
                  << nativeWidth << "x" << nativeHeight << ")." << std::endl;
        isHighResActive_ = false;
        
        // Cancel any ongoing async decode
        cancelOngoingDecode();
        
        decoder_->clearHighResFrames(frameIndex_); // Clear frames
    } else if (!isHighResActive_ && shouldBeActive) {
        // Transitioning from inactive to active
        std::cout << "[FRDM] Activating FullRes decoding due to larger window size (" 
                  << windowWidth << "x" << windowHeight << ")." << std::endl;
        isHighResActive_ = true;
        // Notify the loop to re-evaluate decoding needs if it was paused due to inactivity
        cv_.notify_one(); 
    } else if (isHighResActive_ && shouldBeActive) {
        // Remained active, but window might have resized. Re-trigger decode for current frame.
        // This ensures the correct window size is used if aspect ratio changes etc.
        // Only notify if it was already active to avoid redundant notifications if just became active.
        cv_.notify_one();
    }
    // If it remained inactive, do nothing specific here, loop will handle it.
}

bool FullResDecoderManager::isCurrentlyActive() const {
    return isHighResActive_.load();
}

void FullResDecoderManager::cancelOngoingDecode() {
    std::lock_guard<std::mutex> lock(decodingFutureMutex_);
    
    if (decodingFuture_.valid()) {
        // Signal decoder to stop
        if (decoder_) {
            decoder_->requestStop();
        }
        
        // Wait for decode to finish with timeout
        auto status = decodingFuture_.wait_for(std::chrono::milliseconds(100));
        
        if (status == std::future_status::timeout) {
            std::cerr << "[FRDM] Warning: Decode operation did not finish within timeout after stop request" << std::endl;
            // Note: we can't force-terminate the thread, but at least we tried
        } else {
            // Get the result to clear the future state
            try {
                bool result = decodingFuture_.get();
                std::cout << "[FRDM] Async decode cancelled, result was: " << result << std::endl;
            } catch (const std::exception& e) {
                std::cerr << "[FRDM] Exception while getting decode result: " << e.what() << std::endl;
            }
        }
    }
}

// Helper function (could be moved or made static if appropriate)
// ... existing code ... 