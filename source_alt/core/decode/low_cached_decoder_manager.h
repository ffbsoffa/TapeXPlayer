#ifndef LOW_CACHED_DECODER_MANAGER_H
#define LOW_CACHED_DECODER_MANAGER_H

#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <string>
#include <memory> // For std::unique_ptr
#include <chrono> // Added for time point
#include <set>    // Added for segment tracking

#include "low_res_decoder.h" // Include the decoder it manages
#include "decode.h" // Includes FrameInfo definition

class LowCachedDecoderManager {
public:
    LowCachedDecoderManager(
        const std::string& lowResFilename,
        std::vector<FrameInfo>& frameIndex, 
        std::atomic<int>& currentFrame,
        int ringBufferCapacity,      // How many low-res frames to keep around the current frame
        int highResWindowSize,       // Size of the high-res window (to potentially skip decoding low-res)
        std::atomic<bool>& isPlaying, // Shared playback status
        std::atomic<double>& playbackRate, // Added
        std::atomic<bool>& isReverseRef // Added
    );

    ~LowCachedDecoderManager();

    // Start the manager's background thread
    void run();

    // Stop the manager's background thread
    void stop();

    // Notify the manager about a potential seek or change in current frame
    void notifyFrameChange(); 

private:
    // The main loop running on the background thread
    void decodingLoop();

    // The decoder instance responsible for low-res decoding
    std::unique_ptr<LowResDecoder> decoder_;

    // Shared data references
    std::vector<FrameInfo>& frameIndex_;
    std::atomic<int>& currentFrame_;
    std::atomic<bool>& isPlaying_;
    std::atomic<double>& playbackRate_; // Added
    std::atomic<bool>& isReverse_;      // Added

    // Configuration
    int ringBufferCapacity_;
    int highResWindowSize_;
    int segmentSize_ = 1000; // Changed default segment size to 1000

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

};

#endif // LOW_CACHED_DECODER_MANAGER_H 