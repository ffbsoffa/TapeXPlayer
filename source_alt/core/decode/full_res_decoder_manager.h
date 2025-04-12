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
        const std::string& filename,            // Source filename
        std::vector<FrameInfo>& frameIndex,     // Shared frame index
        std::atomic<int>& currentFrame,         // Shared current frame
        std::atomic<double>& playbackRate,      // Shared playback rate
        const int& highResWindowSizeRef,         // Use const reference
        std::atomic<bool>& isPlaying,           // Shared playing status
        std::atomic<bool>& isReverseRef         // Added: Shared reverse status
    );

    ~FullResDecoderManager();

    void run(); // Start the manager thread
    void stop(); // Stop the manager thread
    void notifyFrameChange(); // Notify about frame changes

private:
    void decodingLoop(); // The actual loop logic

    std::string filename_;                     // Store filename copy
    std::vector<FrameInfo>& frameIndex_;       // Reference to shared data
    std::atomic<int>& currentFrame_;           // Reference to shared data
    std::atomic<double>& playbackRate_;        // Reference to shared data
    std::atomic<bool>& isPlaying_;             // Reference to shared data
    std::atomic<bool>& isReverse_;             // Added: Reference to shared data
    
    const int& highResWindowSize_;             // Store as const reference

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

    // REMOVED: size_t ringBufferCapacity; (Less relevant here)
    // REMOVED: std::future<void> highResFuture;
};

#endif // FULL_RES_DECODER_MANAGER_H 