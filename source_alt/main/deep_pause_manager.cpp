#include "deep_pause_manager.h"

DeepPauseManager::DeepPauseManager()
    : isActive_(false)
    , pauseStartTime_(std::chrono::steady_clock::now())
    , threshold_(std::chrono::seconds(2)) // Default 2 second threshold
    , shouldInterrupt_(false) {
}

DeepPauseManager::~DeepPauseManager() {
}

void DeepPauseManager::update(
    double playbackRate,
    double targetPlaybackRate,
    bool windowHasFocus) {
    
    // Check for deep pause activation
    if (playbackRate == 0.0 && targetPlaybackRate == 0.0 && !isActive_.load()) {
        // Only activate deep pause if window doesn't have focus
        if (!windowHasFocus && std::chrono::steady_clock::now() - pauseStartTime_ > threshold_) {
            isActive_.store(true);
            // Potentially set a flag here to force one OSD update if needed
        }
    }
    
    // If playback starts or window gets focus, ensure deep pause is off
    if ((playbackRate != 0.0 || windowHasFocus) && isActive_.load()) {
        isActive_.store(false);
        shouldInterrupt_ = true; // Force refresh when exiting deep pause
    }
}

void DeepPauseManager::onPauseToggle(double newTargetRate) {
    if (newTargetRate == 0.0) {
        // Just paused - reset timer
        pauseStartTime_ = std::chrono::steady_clock::now();
        isActive_.store(false); // Ensure not in deep pause when toggling to pause
    } else {
        // Just unpaused
        isActive_.store(false);
        shouldInterrupt_ = true; // Force refresh on unpause
    }
}