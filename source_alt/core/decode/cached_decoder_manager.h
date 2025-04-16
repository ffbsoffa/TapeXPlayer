#pragma once

#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <set>
#include <chrono>
#include <memory> // For unique_ptr if needed

// Forward declarations
struct FrameInfo;
class CachedDecoder; // If we need an instance, otherwise static calls

class CachedDecoderManager {
public:
    CachedDecoderManager(
        const std::string& lowResFilename, // Still needed to pass to decoder function
        std::vector<FrameInfo>& frameIndex,
        std::atomic<int>& currentFrame,
        std::atomic<bool>& isReverseRef, // Needed for direction
        int segmentSize = 10500 // Default segment size (adjust as needed)
    );

    ~CachedDecoderManager();

    void run();  // Start the manager thread
    void stop(); // Stop the manager thread
    void notifyFrameChange(); // Notify the manager about frame changes

private:
    // Configuration & State
    std::string lowResFilename_;
    std::vector<FrameInfo>& frameIndex_;
    std::atomic<int>& currentFrame_;
    std::atomic<bool>& isReverse_;
    int segmentSize_;
    int preloadThreshold_; // Threshold within a segment to trigger next load (e.g., 0.75 * segmentSize)

    // Decoder Instance
    std::unique_ptr<CachedDecoder> decoder_; // Instance of the decoder

    // Threading
    std::thread managerThread_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::atomic<bool> stopRequested_;
    std::atomic<bool> isRunning_;

    // Segment Management
    std::set<int> loadedSegments_;
    int previousSegment_;
    bool previousIsReverse_;
    int lastNotifiedFrame_; // To avoid unnecessary work if frame hasn't changed

    // Background Task Management (Optional: track decoding futures)
    // std::vector<std::future<void>> activeDecodingTasks_; 

    // Private methods
    void decodingLoop();
    void loadSegment(int segmentIndex);
    void unloadSegment(int segmentIndex);
    // Helper to remove cached frames similar to LowResDecoder::removeLowResFrames
    static void removeCachedFrames(std::vector<FrameInfo>& frameIndex, int startIndex, int endIndex); 
}; 