#ifndef DEEP_PAUSE_MANAGER_H
#define DEEP_PAUSE_MANAGER_H

#include <atomic>
#include <chrono>

class DeepPauseManager {
public:
    DeepPauseManager();
    ~DeepPauseManager();
    
    // Main update function - call this every frame
    void update(
        double playbackRate,
        double targetPlaybackRate,
        bool windowHasFocus
    );
    
    // Check if currently in deep pause
    bool isActive() const { return isActive_.load(); }
    
    // Force exit from deep pause (e.g., on user interaction)
    void forceExit() { 
        isActive_.store(false);
        shouldInterrupt_ = true;
    }
    
    // Check if there should be an interrupt for immediate refresh
    bool shouldInterruptForRefresh() {
        bool result = shouldInterrupt_;
        shouldInterrupt_ = false; // Reset flag after reading
        return result;
    }
    
    // Called when pause/unpause happens
    void onPauseToggle(double newTargetRate);
    
    // Get recommended sleep time for deep pause (in milliseconds)
    int getDeepPauseSleepTime() const { return 100; } // 100ms = 10 updates per second
    
    // Configuration
    void setThreshold(std::chrono::milliseconds threshold) { threshold_ = threshold; }
    
private:
    std::atomic<bool> isActive_;
    std::chrono::steady_clock::time_point pauseStartTime_;
    std::chrono::milliseconds threshold_;
    bool shouldInterrupt_;
};

#endif // DEEP_PAUSE_MANAGER_H