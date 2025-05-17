#ifndef FULL_RES_DECODER_MANAGER_H
#define FULL_RES_DECODER_MANAGER_H

#include <vector>
#include <string>
#include <atomic>
#include <thread>
#include <mutex>             // Added
#include <condition_variable> // Added
#include <memory>            // Added for unique_ptr
#include <chrono>            // Added for std::chrono
#include "decode.h" // Includes FrameInfo definition
#include "full_res_decoder.h"

class FullResDecoderManager {
public:
    FullResDecoderManager(
        const std::string& filename,            
        std::vector<FrameInfo>& frameIndex,     
        std::atomic<int>& currentFrame,         
        std::atomic<double>& playbackRate,      
        const int& highResWindowSizeRef,         
        std::atomic<bool>& isPlaying,           
        std::atomic<bool>& isReverseRef         
    );

    ~FullResDecoderManager();

    void run(); // Start the manager thread
    void stop(); // Stop the manager thread
    void notifyFrameChange(); // Notify about frame changes

    // Add getter for the decoder instance
    FullResDecoder* getDecoder() const;

    // New methods for window size activity control
    void checkWindowSizeAndToggleActivity(int windowWidth, int windowHeight);
    bool isCurrentlyActive() const;

private:
    void decodingLoop(); // The actual loop logic

    std::string filename_;                     // Store filename copy
    std::vector<FrameInfo>& frameIndex_;       // Reference to shared data
    std::atomic<int>& currentFrame_;           // Reference to shared data
    std::atomic<double>& playbackRate_;        // Reference to shared data
    std::atomic<bool>& isPlaying_;             // Reference to shared data
    std::atomic<bool>& isReverse_;             // Added: Reference to shared data
    
    const int highResWindowSize_;             // Store as const reference

    // --- ADDED: Flag for permanent HW failure of the current decoder instance ---
    bool current_decoder_hw_failed_permanently_ = false;

    std::unique_ptr<FullResDecoder> decoder_;  // Owns the specific decoder instance

    // Thread management
    std::thread managerThread_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::atomic<bool> isRunning_{false};
    std::atomic<bool> stopRequested_{false};
    int lastProcessedFrame_ = -1; // Track last frame processed
    std::chrono::steady_clock::time_point lastDecodeCheckTime_; // Time of last decode check/initiation
    std::chrono::steady_clock::time_point lastHighResUpdateTime_; // Added to throttle updates

    // State for improved interval timing
    std::chrono::steady_clock::time_point nextScheduledHighResTime_;
    bool highResConditionsMetPreviously_ = false;

    // New members for activity control
    std::atomic<bool> isHighResActive_{true}; // Default to active
    std::mutex activityCheckMutex_; // For protecting decoder access during activity check

    // REMOVED: size_t ringBufferCapacity; (Less relevant here)
    // REMOVED: std::future<void> highResFuture;
};

#endif // FULL_RES_DECODER_MANAGER_H 