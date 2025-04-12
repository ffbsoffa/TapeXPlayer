#include "cached_decoder_manager.h"
#include "cached_decoder.h" // Needed for decoder instance and static methods
#include "decode.h"         // For FrameInfo struct definition
#include <iostream>
#include <algorithm> // For std::min, std::max
#include <future>    // For std::async if used in loadSegment
#include <memory>    // For std::make_unique

// Constructor
CachedDecoderManager::CachedDecoderManager(
    const std::string& lowResFilename,
    std::vector<FrameInfo>& frameIndex,
    std::atomic<int>& currentFrame,
    std::atomic<bool>& isReverseRef,
    int segmentSize
) :
    lowResFilename_(lowResFilename),
    frameIndex_(frameIndex),
    currentFrame_(currentFrame),
    isReverse_(isReverseRef),
    segmentSize_(segmentSize > 0 ? segmentSize : 2500),
    stopRequested_(false),
    isRunning_(false),
    previousSegment_(-1),
    previousIsReverse_(isReverse_.load()),
    lastNotifiedFrame_(-1)
{
    // std::cout << "CachedDecoderManager: Initializing..." << std::endl;
    // Calculate preload threshold (e.g., 75% of segment size)
    preloadThreshold_ = static_cast<int>(segmentSize_ * 0.75);

    // Create the CachedDecoder instance
    try {
        decoder_ = std::make_unique<CachedDecoder>(lowResFilename_, frameIndex_);
        if (!decoder_ || !decoder_->isInitialized()) {
             throw std::runtime_error("CachedDecoder instance failed to initialize.");
        }
    } catch (const std::exception& e) {
        std::cerr << "CachedDecoderManager Error: Failed to create CachedDecoder: " << e.what() << std::endl;
        // Rethrow or handle appropriately - maybe set an error state for the manager?
        throw; // Rethrow for now
    }

    // std::cout << "CachedDecoderManager: Segment Size = " << segmentSize_ << ", Preload Threshold = " << preloadThreshold_ << std::endl;
    // std::cout << "CachedDecoderManager: Initialized." << std::endl;
}

// Destructor
CachedDecoderManager::~CachedDecoderManager() {
    stop(); // Ensure thread is stopped and joined
    // std::cout << "CachedDecoderManager: Destroyed." << std::endl;
}

// Start the manager thread
void CachedDecoderManager::run() {
    if (isRunning_) {
        // std::cout << "CachedDecoderManager: Already running." << std::endl;
        return;
    }
    // std::cout << "CachedDecoderManager: Starting manager thread." << std::endl;
    stopRequested_ = false;
    isRunning_ = true;
    managerThread_ = std::thread(&CachedDecoderManager::decodingLoop, this);
}

// Stop the manager thread
void CachedDecoderManager::stop() {
    if (!isRunning_ && !managerThread_.joinable()) {
        return;
    }
    // std::cout << "CachedDecoderManager: Stopping manager thread..." << std::endl;
    stopRequested_ = true;
    cv_.notify_one(); // Wake up the thread if it's waiting
    if (managerThread_.joinable()) {
        managerThread_.join();
    }
    isRunning_ = false;
    // std::cout << "CachedDecoderManager: Manager thread stopped." << std::endl;
}

// Notify the manager about frame changes
void CachedDecoderManager::notifyFrameChange() {
    // Simply notify the condition variable to potentially wake up the loop
    cv_.notify_one();
}

// The main loop for managing cached segments
void CachedDecoderManager::decodingLoop() {
    // std::cout << "CachedDecoderManager: Decoding loop started." << std::endl;
    while (!stopRequested_) {
        int currentFrame = -1; // Initialize with invalid value
        bool needsUpdate = false;
        bool directionChanged = false;

        { // Scope for lock
            std::unique_lock<std::mutex> lock(mtx_);
            if (!cv_.wait_for(lock, std::chrono::milliseconds(200), [&] {
                return stopRequested_.load() || currentFrame_.load() != lastNotifiedFrame_;
            })) {
                if (stopRequested_ || currentFrame_.load() == lastNotifiedFrame_) {
                    continue; 
                }
            }

            if (stopRequested_) break; 

            currentFrame = currentFrame_.load(); 
            
            if (currentFrame != lastNotifiedFrame_) {
                needsUpdate = true;
                // Don't update lastNotifiedFrame_ here, do it after processing the frame change
            }
            
             bool currentIsReverse = isReverse_.load();
             if (currentIsReverse != previousIsReverse_) {
                 // std::cout << "CachedDecoderManager: Direction changed! New direction: " << (currentIsReverse ? "Reverse" : "Forward") << std::endl;
                 directionChanged = true;
                 previousIsReverse_ = currentIsReverse; 
             }
            
        } // Lock released

        if (!needsUpdate && !directionChanged) {
            continue;
        }

        // Update last notified frame *after* processing the change
        lastNotifiedFrame_ = currentFrame; 

        // --- Segment Management Logic --- 
        if (frameIndex_.empty() || segmentSize_ <= 0 || currentFrame < 0) {
            continue; 
        }

        int currentSegment = currentFrame / segmentSize_;
        int frameInSegment = currentFrame % segmentSize_;
        int numSegmentsTotal = (frameIndex_.size() + segmentSize_ - 1) / segmentSize_;\
        bool currentIsReverse = isReverse_.load(); // Get potentially updated value

        if(currentSegment != previousSegment_ || directionChanged) {
            // std::cout << "CachedDecoderManager: Current Frame=" << currentFrame << ", Current Segment=" << currentSegment << std::endl;
        }

        // 1. Determine Target Segments
        std::set<int> targetSegments;
        targetSegments.insert(currentSegment); 
        if (currentIsReverse) {
            if (currentSegment > 0) targetSegments.insert(currentSegment - 1);
        } else {
            if (currentSegment < numSegmentsTotal - 1) targetSegments.insert(currentSegment + 1);
            // Optional: if (currentSegment < numSegmentsTotal - 2) { targetSegments.insert(currentSegment + 2); }
        }
        
        // --- DEBUG: Log Target Segments ---
        if(currentSegment != previousSegment_ || directionChanged) {
            // std::cout << "  Target Segments: { ";
            // for(int ts : targetSegments) std::cout << ts << " ";
            // std::cout << "}" << std::endl;
        }
        // --- END DEBUG ---

        // 2. Identify Segments to Load/Unload
        std::set<int> segmentsToLoad;
        std::set<int> segmentsToUnload;
        size_t loadedCountBefore = 0; // Debug

        { 
            std::lock_guard<std::mutex> lock(mtx_); 
            loadedCountBefore = loadedSegments_.size(); // Debug

            for (int loadedSeg : loadedSegments_) {
                if (targetSegments.find(loadedSeg) == targetSegments.end()) {
                    segmentsToUnload.insert(loadedSeg);
                }
            }
            
            for (int targetSeg : targetSegments) {
                 if (targetSeg >= 0 && targetSeg < numSegmentsTotal) { 
                     if (loadedSegments_.find(targetSeg) == loadedSegments_.end()) {
                         segmentsToLoad.insert(targetSeg);
                     }
                 }
            }

            // Add preload segments based on threshold
            bool preloadTriggered = false; // Debug
            if (currentIsReverse) {
                if (frameInSegment < (segmentSize_ - preloadThreshold_) && currentSegment > 0) {
                    int preloadSegment = currentSegment - 1;
                    if (loadedSegments_.find(preloadSegment) == loadedSegments_.end() && targetSegments.find(preloadSegment) == targetSegments.end()) { // Ensure not already target
                        segmentsToLoad.insert(preloadSegment);
                        preloadTriggered = true; // Debug
                    }
                }
            } else {
                if (frameInSegment >= preloadThreshold_ && currentSegment < numSegmentsTotal - 1) {
                    int preloadSegment = currentSegment + 1;
                     if (loadedSegments_.find(preloadSegment) == loadedSegments_.end() && targetSegments.find(preloadSegment) == targetSegments.end()) { // Ensure not already target
                        segmentsToLoad.insert(preloadSegment);
                        preloadTriggered = true; // Debug
                    }
                }
                // Optional: Preload segment + 2
            }
             // --- DEBUG: Log Segments To Load/Unload ---
             if (!segmentsToLoad.empty() || !segmentsToUnload.empty()) {
                 // std::cout << "  Segments To Load: { "; for(int sl : segmentsToLoad) std::cout << sl << " "; std::cout << "}" << (preloadTriggered ? " (Preload Triggered)" : "") << std::endl;
                 // std::cout << "  Segments To Unload: { "; for(int su : segmentsToUnload) std::cout << su << " "; std::cout << "}" << std::endl;
             }
             // --- END DEBUG ---

        } // Lock released
        

        // 3. Unload segments 
        for (int segIdx : segmentsToUnload) {
            unloadSegment(segIdx); 
        }

        // 4. Load segments 
        for (int segIdx : segmentsToLoad) {
            loadSegment(segIdx); 
        }

        // 5. Update state trackers
        previousSegment_ = currentSegment;

        // --- DEBUG: Log loaded count after changes ---
        {
            std::lock_guard<std::mutex> lock(mtx_);
             // if (!segmentsToLoad.empty() || !segmentsToUnload.empty()) { // Only log if changes happened
                // std::cout << "  Loaded Segments (" << loadedSegments_.size() << "): { ";
                // for(int ls : loadedSegments_) std::cout << ls << " ";
                // std::cout << "}" << std::endl;
            // }
        }
        // --- END DEBUG ---

    } // End while loop
    // std::cout << "CachedDecoderManager: Decoding loop finished." << std::endl;
    isRunning_ = false;
}

// Function to load a specific segment
void CachedDecoderManager::loadSegment(int segmentIndex) {
    if (!decoder_ || !decoder_->isInitialized()) { // Check if decoder is valid
         std::cerr << "CachedDecoderManager Error: Decoder not initialized in loadSegment." << std::endl;
         return;
    }
    if (frameIndex_.empty() || segmentSize_ <= 0) return;

    int startFrame = segmentIndex * segmentSize_;
    int endFrame = std::min(startFrame + segmentSize_ - 1, static_cast<int>(frameIndex_.size()) - 1);

    // Check if already loaded (check before potentially expensive lock)
    // Need lock here to check loadedSegments_ safely
    { 
        std::lock_guard<std::mutex> lock(mtx_);
        if (loadedSegments_.count(segmentIndex)) {
            // std::cout << "CachedDecoderManager: Segment " << segmentIndex << " already loaded." << std::endl;
            return;
        }
    }

    // Check invalid range
    if (startFrame > endFrame) {
        return;
    }

    // std::cout << "CachedDecoderManager: Requesting load for segment " << segmentIndex << " [" << startFrame << "-" << endFrame << "]" << std::endl;

    // --- Call the actual decoding function --- 
    bool success = decoder_->decodeRange(startFrame, endFrame); // Call instance method

    // Placeholder removed
    // std::this_thread::sleep_for(std::chrono::milliseconds(10)); 
    // bool success = true;

    if (success) {
        std::lock_guard<std::mutex> lock(mtx_); // Protect access to loadedSegments_
        loadedSegments_.insert(segmentIndex);
        // --- DEBUG ---
        // std::cout << "CachedDecoderManager: Successfully loaded segment " << segmentIndex << ". Total loaded: " << loadedSegments_.size() << std::endl;
    } else {
        std::cerr << "CachedDecoderManager Warning: Failed to load segment " << segmentIndex << std::endl;
    }

    // Optional: Use std::async for true background loading
    /*
    auto future = std::async(std::launch::async, [&](){
        return decoder_->decodeRange(startFrame, endFrame);
    });
    // Store future if you need to track completion?
    */
}


// Function to unload a specific segment
void CachedDecoderManager::unloadSegment(int segmentIndex) {
    bool shouldRemove = false;
    // Check if segment is currently loaded and remove it from the set atomically
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (loadedSegments_.count(segmentIndex)) { // Check if it exists
            loadedSegments_.erase(segmentIndex);   // Remove it immediately
            shouldRemove = true;                  // Mark that we need to clean frames
        }
    } // Lock released

    // If the segment was in the set, proceed to clean its frames
    if (shouldRemove) {
        int startFrame = segmentIndex * segmentSize_;
        int endFrame = std::min(startFrame + segmentSize_ - 1, static_cast<int>(frameIndex_.size()) - 1);

        if (startFrame <= endFrame) { // Check for valid range before cleaning
            // --- DEBUG ---
            // std::cout << "CachedDecoderManager: Unloading segment " << segmentIndex << " [" << startFrame << "-" << endFrame << "]" << std::endl;
            removeCachedFrames(frameIndex_, startFrame, endFrame);
        }
    }
    // else: Segment was already removed concurrently or never loaded, do nothing.
}

// Static helper to remove cached frames (similar to LowResDecoder)
void CachedDecoderManager::removeCachedFrames(std::vector<FrameInfo>& frameIndex, int startIndex, int endIndex) {
    startIndex = std::max(0, startIndex);
    endIndex = std::min(static_cast<int>(frameIndex.size()) - 1, endIndex);

    if (startIndex > endIndex) {
        return;
    }

    int removedCount = 0; // Debug counter

    for (int i = startIndex; i <= endIndex; ++i) {
        // Check if frame index is valid before accessing
        if (i < 0 || i >= frameIndex.size()) continue; 

        std::lock_guard<std::mutex> lock(frameIndex[i].mutex); // Lock the specific frame

        if (frameIndex[i].cached_frame) {
            // --- Explicitly unreference frame data buffers BEFORE reset --- 
            av_frame_unref(frameIndex[i].cached_frame.get()); 
            // --- End Explicit Unref ---
            
            frameIndex[i].cached_frame.reset(); // Release the shared_ptr
            removedCount++; // Increment debug counter

            // Reset type only if it was CACHED
            if (frameIndex[i].type == FrameInfo::CACHED) {
                 if (frameIndex[i].frame) {
                     frameIndex[i].type = FrameInfo::FULL_RES;
                 } else if (frameIndex[i].low_res_frame){
                    frameIndex[i].type = FrameInfo::LOW_RES;
                 } else {
                    frameIndex[i].type = FrameInfo::EMPTY;
                 }
            }
        }
    }
}


