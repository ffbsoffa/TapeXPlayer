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
    // std::cout << "FullResDecoderManager: Initializing..." << std::endl;
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
        return; // Already stopped or never started
    }
    // std::cout << "[LOG] FullResDecoderManager: Stopping manager thread requested..." << std::endl;
    stopRequested_ = true;
    cv_.notify_one(); // Wake up the thread if it's waiting
    
    // Request the decoder itself to stop its internal loops
    if (decoder_) {
        // std::cout << "[LOG] FullResDecoderManager: Requesting FullResDecoder to stop..." << std::endl;
        decoder_->requestStop();
    }
    
    // std::cout << "[LOG] FullResDecoderManager: Waiting for manager thread to join..." << std::endl;
    if (managerThread_.joinable()) {
        managerThread_.join();
    }
    isRunning_ = false; // Mark as stopped *after* join
    // std::cout << "[LOG] FullResDecoderManager: Manager thread stopped and joined." << std::endl;
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
    // std::cout << "FullResDecoderManager: Decoding loop started. Initial next scheduled time: " << nextScheduledHighResTime_.time_since_epoch().count() << std::endl;
    const std::chrono::milliseconds highResUpdateInterval(18000); // Keep interval definition here as well
    
    while (!stopRequested_) {
        int currentFrame = currentFrame_.load();
        double playbackRateAbs = std::abs(playbackRate_.load());
        bool frameChanged = false;
        auto now = std::chrono::steady_clock::now(); // Get current time
        
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

        // Only proceed if the frame actually changed
        bool highResConditionsMetNow = false; // Declare outside the if block
        if (!frameChanged) {
            // Optional: sleep longer if paused and no frame change
             if (!isPlaying_.load()) {
                  std::this_thread::sleep_for(std::chrono::milliseconds(100));
             }
            continue;
        }

        // --- Calculate Time Since Last Update ---
        auto timeSinceLastUpdate = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastHighResUpdateTime_);

        // --- High-Resolution Window Management ---
        if (decoder_ && !frameIndex_.empty()) {
            bool isRev = isReverse_.load();
            highResConditionsMetNow = shouldDecodeHighRes(playbackRateAbs, isRev); // Assign value inside
            bool justReturnedToHighRes = highResConditionsMetNow && !highResConditionsMetPreviously_;

            // Calculate the target high-res window (shifted 40% right)
            int windowSize = highResWindowSize_; // Get current size via reference
            int sizeBehind = static_cast<int>(windowSize * 0.10); // New 10% behind
            int sizeAhead = windowSize - sizeBehind;             // New 90% ahead

            int highResStart = std::max(0, currentFrame - sizeBehind);
            int highResEnd = std::min(static_cast<int>(frameIndex_.size()) - 1, currentFrame + sizeAhead);
            
            // Log conditions before checking interval
            /* // std::cout << "[FRDM Log] Check Decode: conditionsMetNow=" << highResConditionsMetNow 
                      << ", justReturned=" << justReturnedToHighRes
                      << ", start=" << highResStart << ", end=" << highResEnd 
                      << ", timeNow=" << now.time_since_epoch().count() 
                      << ", scheduledTime=" << nextScheduledHighResTime_.time_since_epoch().count() << std::endl;
            */
            // Decode if: 
            // 1. Conditions are met NOW and we reached the scheduled time OR
            // 2. We JUST returned to the high-res conditions (speed=1x fwd)
            bool shouldTriggerDecode = highResConditionsMetNow && 
                                       (now >= nextScheduledHighResTime_ || justReturnedToHighRes);

            if (shouldTriggerDecode && highResStart <= highResEnd) {
                // Decode the required high-resolution range
                // std::cout << "[FRDM Log] Triggering decode range [" 
                          // << highResStart << "-" << highResEnd 
                          // << "] (Reason: " << (justReturnedToHighRes ? "Returned to 1x" : "Scheduled time") << ")" << std::endl;
                bool success = decoder_->decodeFrameRange(frameIndex_, highResStart, highResEnd);
                if (!success) {
                    std::cerr << "FullResDecoderManager Warning: decodeFrameRange failed for [" 
                              << highResStart << "-" << highResEnd << "]" << std::endl;
                }
                // Schedule the *next* update regardless of success/failure
                nextScheduledHighResTime_ = now + highResUpdateInterval;
                // std::cout << "[FRDM Log] Next decode scheduled for: " << nextScheduledHighResTime_.time_since_epoch().count() << std::endl;
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

        // Update the previous state for the next iteration
        highResConditionsMetPreviously_ = highResConditionsMetNow;

        // Optional minimal sleep to prevent tight loop if conditions change rapidly
        // std::this_thread::sleep_for(std::chrono::milliseconds(5));

    } // end while loop

    // std::cout << "FullResDecoderManager: Decoding loop finished." << std::endl;
    isRunning_ = false; // Ensure isRunning is false when loop exits
} 