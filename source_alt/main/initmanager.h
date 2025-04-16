#pragma once

#include <string>
#include <SDL2/SDL.h> // Needed for SDL_Window and SDL_WINDOWPOS_CENTERED
#include <vector>
#include <atomic>
#include <memory> // For std::unique_ptr
#include <future>
#include <mutex> // Needed for mutex in LoadingStatus

// Forward declarations or includes for types used in mainLoadingSequence
#include "../core/decode/decode.h" // Provides FrameInfo definition
// Include atomic for LoadingStatus
#include <atomic>
// Forward declare manager classes to avoid full includes in header if possible
class FullResDecoderManager;
class LowCachedDecoderManager;
class CachedDecoderManager;

// Structure for storing window settings
struct WindowSettings {
    int x = SDL_WINDOWPOS_CENTERED;
    int y = SDL_WINDOWPOS_CENTERED;
    int width = 1280;
    int height = 720;
    bool isFullscreen = false;
    bool isValid = false;
};

// --- Loading Progress Structure ---
struct LoadingStatus {
    std::string stage;              // Current loading stage description
    mutable std::mutex stage_mutex; // Mutex to protect access to stage string
    std::atomic<int> percent;       // Progress percentage (0-100)

    // Constructor to initialize
    LoadingStatus() : stage("Initializing..."), percent(0) {}
};

// Function declarations
std::string getConfigFilePath();
WindowSettings loadWindowSettings();
void saveWindowSettings(SDL_Window* window);
void resetPlayerState();
void restartPlayerWithFile(const std::string& filename);

// Main file loading sequence function
std::future<bool> mainLoadingSequence(
    SDL_Renderer* renderer,
    SDL_Window* window,
    LoadingStatus& loading_status_ref, // Add reference to loading status
    const std::string& fileToLoad,
    std::string& currentFilename_out,          // Output: Actual filename used
    std::vector<FrameInfo>& frameIndex_out,      // Output: Frame index data
    std::unique_ptr<FullResDecoderManager>& fullResMgr_out, // Output: Initialized manager
    std::unique_ptr<LowCachedDecoderManager>& lowCachedMgr_out, // Output: Initialized manager
    std::unique_ptr<CachedDecoderManager>& cachedMgr_out, // Output: Initialized manager
    std::atomic<int>& currentFrame_ref,         // Ref to main's currentFrame
    std::atomic<bool>& isPlaying_ref            // Ref to main's isPlaying
);
