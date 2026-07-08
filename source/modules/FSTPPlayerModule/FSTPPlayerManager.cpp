#include "FSTPPlayerManager.h"
#include "FSTPPlayerInstance.h"
#include "FSTPRemote.h"
#include "../FSTPMainModule/WSGUI/FSTPOSDSystem.h"
#include "../FSTPMainModule/WSGUI/FSTPSettings.h"
#include "../FSTPMainModule/WSGUI/FSTPWindowManager.h"
#include "../FSTPAudioModule/FSTPAudioModule_wrapper.h"
#include "../FSTPVideoModule/FSTPHardwareDetection.h"
#include <iostream>
#include <memory>
#include <array>
#include <string>
#include <algorithm>
#include <thread>
#include <chrono>
#include <atomic>

// Debug control: set to true to enable verbose logging
static constexpr bool ENABLE_PLAYER_MANAGER_DEBUG = false;

// Structure for managing player instances
struct PlayerInstanceSlot {
    std::unique_ptr<FSTPPlayerInstance> instance;
    bool is_active;
    int instance_id;
};

// Autonomous video update
static std::atomic<bool> g_video_update_running{false};
static std::thread g_video_update_thread;

// MIDI Remote Control (Mackie/X-Touch One)
static std::unique_ptr<FSTPRemote> g_remote_control = nullptr;

// Global player manager state
static struct {
    bool initialized;
    std::array<PlayerInstanceSlot, MAX_PLAYER_INSTANCES> instances;
    int active_player_id; // Active player ID (-1 if none active)
} g_manager_state = { false, {}, -1 };

// Autonomous video update function (ADAPTIVE FPS - synchronized with video file)
static void VideoUpdateLoop() {
    std::cout << "🎥 [VIDEO THREAD] Autonomous video update thread started (ADAPTIVE FPS)" << std::endl;

    // FIXED: Use 50 FPS for better synchronization with 25 FPS PAL video
    // 50 FPS = exactly 2× frames per 25 FPS video frame → minimal judder!
    // This eliminates judder/stuttering when video is 25fps but updates are 60fps (60/25 = 2.4 uneven)
    const auto current_frame_duration = std::chrono::microseconds(20000); // 50 FPS (20ms) - perfect for PAL 25fps

    // Alternative modes (not used yet):
    // const auto max_frame_duration = std::chrono::microseconds(16667); // ~60 FPS (16.67ms)
    // const auto pal_frame_duration = std::chrono::microseconds(40000); // ~25 FPS (40ms) for PAL video
    // const auto ntsc_frame_duration = std::chrono::microseconds(33333); // ~30 FPS (33.33ms) for NTSC video

    while (g_video_update_running.load()) {
        // DISABLED: UpdateAllVideoFrames already called in main render loop (FSTPWindowManager.cpp:465)
        // Double call led to 100+ FPS instead of 60 FPS and CPU overuse!
        // UpdateAllVideoFrames();

        // DISABLED: ProcessCommands now called in UpdateAllVideoFrames (30 FPS)
        // if (g_remote_control && g_remote_control->IsInitialized()) {
        //     g_remote_control->ProcessCommands();
        // }

        // Just sleep, thread needed only for compatibility
        std::this_thread::sleep_for(current_frame_duration);

        // TODO: In future can get real FPS from active player and adapt current_frame_duration
        // For now using 60 FPS for maximum OSD responsiveness
    }

    std::cout << "🎥 [VIDEO THREAD] Autonomous video update thread stopped" << std::endl;
}

// Start autonomous video update
static void StartVideoUpdateThread() {
    if (!g_video_update_running.load()) {
        g_video_update_running.store(true);
        g_video_update_thread = std::thread(VideoUpdateLoop);
        std::cout << "🎥 [VIDEO THREAD] Autonomous video update thread launched" << std::endl;
    }
}

// Stop autonomous video update
static void StopVideoUpdateThread() {
    if (g_video_update_running.load()) {
        g_video_update_running.store(false);
        if (g_video_update_thread.joinable()) {
            g_video_update_thread.join();
        }
        std::cout << "🎥 [VIDEO THREAD] Autonomous video update thread terminated" << std::endl;
    }
}

int InitPlayerManager() {
    if (g_manager_state.initialized) {
        std::cout << "Player manager already initialized" << std::endl;
        return 0;
    }

    std::cout << "Initializing player manager for multiple instances..." << std::endl;

    // Initialize instances array
    for (int i = 0; i < MAX_PLAYER_INSTANCES; i++) {
        g_manager_state.instances[i].instance = nullptr;
        g_manager_state.instances[i].is_active = false;
        g_manager_state.instances[i].instance_id = i;
    }

    g_manager_state.initialized = true;

    // DISABLED: Autonomous video thread no longer needed!
    // Video now updates synchronously in RenderAllWindows() like in old code
    // StartVideoUpdateThread();
    std::cout << "🎥 [VIDEO SYNC] Video updates now synchronized with render loop (no separate thread)" << std::endl;

    // Initialize MIDI Remote Control (Mackie/X-Touch One)
    try {
        g_remote_control = std::make_unique<FSTPRemote>();
        if (g_remote_control->Initialize()) {
            std::cout << "🎹 [MIDI REMOTE] MIDI Remote Control initialized successfully" << std::endl;

            // Apply saved MIDI settings
            if (GetMIDIEnabled()) {
                ApplyMIDISettings();
            } else {
                std::cout << "🎹 [MIDI REMOTE] MIDI disabled in settings" << std::endl;
            }
        } else {
            std::cerr << "⚠️  [MIDI REMOTE] Failed to initialize MIDI Remote Control (will continue without it)" << std::endl;
            g_remote_control.reset();
        }
    } catch (const std::exception& e) {
        std::cerr << "⚠️  [MIDI REMOTE] Exception during initialization: " << e.what() << std::endl;
        g_remote_control.reset();
    }

    std::cout << "Player manager initialized successfully (supports " << MAX_PLAYER_INSTANCES << " instances)" << std::endl;

    return 0;
}

void ShutdownPlayerManager() {
    if (!g_manager_state.initialized) {
        return;
    }

    std::cout << "Shutting down player manager..." << std::endl;

    // Shutdown MIDI Remote Control
    if (g_remote_control) {
        std::cout << "🎹 [MIDI REMOTE] Shutting down MIDI Remote Control..." << std::endl;
        g_remote_control->Shutdown();
        g_remote_control.reset();
    }

    // DISABLED: Autonomous video thread no longer used
    // StopVideoUpdateThread();

    // Destroy all active instances
    for (int i = 0; i < MAX_PLAYER_INSTANCES; i++) {
        if (g_manager_state.instances[i].is_active) {
            DestroyPlayerInstance(i);
        }
    }

    g_manager_state.initialized = false;
    std::cout << "Player manager shutdown complete" << std::endl;
}

int CreatePlayerInstance(const char* filepath, int player_id) {
    if (!g_manager_state.initialized) {
        std::cerr << "Player manager not initialized" << std::endl;
        return -1;
    }

    if (!filepath) {
        std::cerr << "Invalid file path" << std::endl;
        return -2;
    }

    // CPU-based instance limiting: Check if we can create new instance on Pentium CPUs
    if (g_hardware_detection) {
        const FSTPCPUInfo& cpu_info = g_hardware_detection->GetCPUInfo();

        // Check if CPU is a Pentium processor
        if (cpu_info.model_name.find("Pentium") != std::string::npos) {
            int active_count = GetActiveInstanceCount();

            // Limit to 1 instance on Pentium CPUs
            if (active_count >= 1 && (player_id == -1 || !g_manager_state.instances[player_id].is_active)) {
                std::cerr << "⚠️  Instance limit reached for " << cpu_info.model_name << std::endl;
                std::cerr << "    Pentium CPUs are limited to 1 player instance for optimal performance." << std::endl;
                std::cerr << "    Please close the existing instance before opening a new file." << std::endl;
                return -4;
            }
        }
    }

    int instance_id = -1;
    
    if (player_id >= 0 && player_id < MAX_PLAYER_INSTANCES) {
        // If specific player ID is specified, use it
        if (!g_manager_state.instances[player_id].is_active) {
            instance_id = player_id;
        } else {
            std::cerr << "Player instance " << player_id << " is already active" << std::endl;
            return -3;
        }
    } else {
        // If ID not specified, find free slot for instance
        for (int i = 0; i < MAX_PLAYER_INSTANCES; i++) {
            if (!g_manager_state.instances[i].is_active) {
                instance_id = i;
                break;
            }
        }
    }

    if (instance_id == -1) {
        std::cerr << "Maximum number of player instances reached (" << MAX_PLAYER_INSTANCES << ")" << std::endl;
        return -3;
    }

    std::cout << "Creating player instance " << instance_id << " for file: " << filepath << std::endl;

    // Create new player instance
    auto new_instance = std::make_unique<FSTPPlayerInstance>();

    // Set instance ID
    new_instance->SetInstanceID(instance_id);

    // Initialize instance
    int init_result = new_instance->Initialize();
    if (init_result != 0) {
        std::cerr << "Failed to initialize player instance " << instance_id << std::endl;
        return init_result;
    }

    // Restore resume position for this file (if any)
    double resume_pos = LoadResumePosition(filepath);
    if (resume_pos > 0.0) {
        std::cout << "[RESUME] Restoring position " << resume_pos << "s for: " << filepath << std::endl;
    }

    // Load file into instance
    int load_result = new_instance->LoadFile(filepath, resume_pos > 0.0 ? resume_pos : 0.0);
    if (load_result != 0) {
        std::cerr << "Failed to load file into player instance " << instance_id << std::endl;
        return load_result;
    }

    // Determine file type by extension
    std::string file_str(filepath);
    std::transform(file_str.begin(), file_str.end(), file_str.begin(), ::tolower);

    bool is_audio_file = (file_str.find(".mp3") != std::string::npos ||
                         file_str.find(".wav") != std::string::npos ||
                         file_str.find(".m4a") != std::string::npos ||
                         file_str.find(".aac") != std::string::npos ||
                         file_str.find(".flac") != std::string::npos ||
                         file_str.find(".ogg") != std::string::npos);

    // Set file type in OSD
    SetOSDFileType(instance_id, is_audio_file);

    // Set timecode offset from file metadata (professional video files)
    auto* audio_mod = new_instance->GetAudioModule();
    if (audio_mod) {
        double tc_offset = audio_mod->GetTimecodeOffset();
        SetOSDTimecodeOffset(instance_id, tc_offset);
    }

    // Save instance in slot
    g_manager_state.instances[instance_id].instance = std::move(new_instance);
    g_manager_state.instances[instance_id].is_active = true;

    // Set real file FPS in OSD for correct timecode display
    if (!is_audio_file) {
        double fps = g_manager_state.instances[instance_id].instance->GetFrameRate();
        SetOSDFrameRate(instance_id, fps);
    }

    // Update window title with instance number and filename
    UpdateWindowTitle(instance_id, instance_id, filepath);

    // FIX: Don't set OSD_MODE_NORMAL here - FSTPVideoModule_wrapper.cpp::LoadFile() already does this
    // UpdateOSDDisplayMode(instance_id, OSD_MODE_NORMAL); // Commented to avoid duplication

    std::cout << "Player instance " << instance_id << " created successfully" << std::endl;
    return instance_id;
}

int LoadFileIntoPlayerInstance(const char* filepath, int player_id) {
    if (!g_manager_state.initialized) {
        std::cerr << "Player manager not initialized" << std::endl;
        return -1;
    }

    if (!filepath) {
        std::cerr << "Invalid file path" << std::endl;
        return -2;
    }

    if (player_id < 0 || player_id >= MAX_PLAYER_INSTANCES) {
        std::cerr << "Invalid player ID: " << player_id << std::endl;
        return -3;
    }

    // Check if instance is active
    if (!g_manager_state.instances[player_id].is_active) {
        std::cerr << "Player instance " << player_id << " is not active" << std::endl;
        return -4;
    }

    std::cout << "Loading file into existing player instance " << player_id << ": " << filepath << std::endl;

    // Note: save of old file position is handled inside FSTPPlayerInstance::UnloadFile()
    // which runs BEFORE Stop() resets playback_position to 0.

    // Restore resume position for the new file (if any)
    double resume_pos = LoadResumePosition(filepath);
    if (resume_pos > 0.0) {
        std::cout << "[RESUME] Restoring position " << resume_pos << "s for: " << filepath << std::endl;
    }

    // Load file into existing instance
    std::cout << "Calling LoadFile on player instance " << player_id << " with file: " << filepath << std::endl;
    int load_result = g_manager_state.instances[player_id].instance->LoadFile(filepath, resume_pos > 0.0 ? resume_pos : 0.0);
    if (load_result != 0) {
        std::cerr << "Failed to load file into player instance " << player_id << " - LoadFile returned " << load_result << std::endl;
        return -5;
    }

    // Determine file type by extension
    std::string file_str(filepath);
    std::transform(file_str.begin(), file_str.end(), file_str.begin(), ::tolower);

    bool is_audio_file = (file_str.find(".mp3") != std::string::npos ||
                         file_str.find(".wav") != std::string::npos ||
                         file_str.find(".m4a") != std::string::npos ||
                         file_str.find(".aac") != std::string::npos ||
                         file_str.find(".flac") != std::string::npos ||
                         file_str.find(".ogg") != std::string::npos);

    // Set file type in OSD (assuming OSD is bound to player_id)
    SetOSDFileType(player_id, is_audio_file);

    // Set timecode offset from file metadata
    auto* audio_mod = g_manager_state.instances[player_id].instance->GetAudioModule();
    if (audio_mod) {
        double tc_offset = audio_mod->GetTimecodeOffset();
        SetOSDTimecodeOffset(player_id, tc_offset);
    }

    // Set real file FPS in OSD for correct timecode display
    if (!is_audio_file) {
        double fps = g_manager_state.instances[player_id].instance->GetFrameRate();
        SetOSDFrameRate(player_id, fps);
    }

    // Update window title with instance number and filename
    UpdateWindowTitle(player_id, player_id, filepath);

    // FIX: Don't set OSD_MODE_NORMAL here - FSTPVideoModule_wrapper.cpp::LoadFile() already does this
    // UpdateOSDDisplayMode(player_id, OSD_MODE_NORMAL); // Commented to avoid duplication

    std::cout << "File loaded successfully into player instance " << player_id
              << " (type: " << (is_audio_file ? "audio" : "video") << ")" << std::endl;
    return player_id;
}

void DestroyPlayerInstance(int instance_id) {
    if (instance_id < 0 || instance_id >= MAX_PLAYER_INSTANCES ||
        !g_manager_state.instances[instance_id].is_active) {
        return;
    }

    std::cout << "Destroying player instance " << instance_id << std::endl;

    // Note: save of position is handled inside FSTPPlayerInstance::UnloadFile()
    // which is called from the destructor BEFORE Stop() resets playback_position to 0.

    // CRITICAL: clear is_active BEFORE destroying the instance. The autonomous
    // render thread keeps rendering this window during teardown (is_closing is set
    // only afterwards, to show "unthreading" OSD progress), so accessors like
    // GetInstanceFilePath()/GetInstancePosition() can run concurrently. They guard
    // on is_active, so flipping it first makes them bail out instead of
    // dereferencing the half-/fully-destroyed instance (null-deref on close).
    g_manager_state.instances[instance_id].is_active = false;
    g_manager_state.instances[instance_id].instance.reset();

    std::cout << "Player instance " << instance_id << " destroyed" << std::endl;
}

int IsPlayerInstanceActive(int instance_id) {
    if (instance_id < 0 || instance_id >= MAX_PLAYER_INSTANCES) {
        return 0;
    }
    return g_manager_state.instances[instance_id].is_active ? 1 : 0;
}

int IsVideoLoadedInInstance(int instance_id) {
    if (instance_id < 0 || instance_id >= MAX_PLAYER_INSTANCES ||
        !g_manager_state.instances[instance_id].is_active) {
        return 0;
    }

    return g_manager_state.instances[instance_id].instance->IsFileLoaded() ? 1 : 0;
}

const char* GetInstanceFilePath(int instance_id) {
    if (instance_id < 0 || instance_id >= MAX_PLAYER_INSTANCES ||
        !g_manager_state.instances[instance_id].is_active) {
        return nullptr;
    }

    if (!g_manager_state.instances[instance_id].instance->IsFileLoaded()) {
        return nullptr;
    }

    return g_manager_state.instances[instance_id].instance->GetFilePath().c_str();
}

int GetActiveInstanceCount() {
    int count = 0;
    for (int i = 0; i < MAX_PLAYER_INSTANCES; i++) {
        if (g_manager_state.instances[i].is_active) {
            count++;
        }
    }
    return count;
}

void GetActiveInstanceIDs(int* instance_ids, int* count) {
    if (!instance_ids || !count) {
        return;
    }

    *count = 0;
    for (int i = 0; i < MAX_PLAYER_INSTANCES; i++) {
        if (g_manager_state.instances[i].is_active) {
            instance_ids[*count] = i;
            (*count)++;
        }
    }
}

int FindInstanceByFilePath(const char* filepath) {
    if (!filepath) {
        return -1;
    }

    for (int i = 0; i < MAX_PLAYER_INSTANCES; i++) {
        if (g_manager_state.instances[i].is_active &&
            g_manager_state.instances[i].instance->IsFileLoaded()) {

            const std::string& instance_path = g_manager_state.instances[i].instance->GetFilePath();
            if (instance_path == filepath) {
                return i;
            }
        }
    }

    return -1;
}

// === Old functions for backward compatibility ===
// TODO: Can be removed when all parts of code transition to new API

int OpenVideoFile(const char* filepath) {
    // Check if file is already opened
    int existing_instance = FindInstanceByFilePath(filepath);
    if (existing_instance != -1) {
        std::cout << "File already loaded in instance " << existing_instance << std::endl;
        return existing_instance;
    }

    // Create new instance
    return CreatePlayerInstance(filepath);
}

void CloseVideoFile() {
    // Close first active instance (for backward compatibility)
    for (int i = 0; i < MAX_PLAYER_INSTANCES; i++) {
        if (g_manager_state.instances[i].is_active) {
            DestroyPlayerInstance(i);
            break;
        }
    }
}

int IsVideoOpen() {
    return GetActiveInstanceCount() > 0 ? 1 : 0;
}

const char* GetCurrentFilePath() {
    // Return path of first active instance (for backward compatibility)
    for (int i = 0; i < MAX_PLAYER_INSTANCES; i++) {
        if (g_manager_state.instances[i].is_active) {
            return GetInstanceFilePath(i);
        }
    }
    return nullptr;
}

// === Management of playback for specific instance ===

int PlayInstance(int instance_id) {
    if (instance_id < 0 || instance_id >= MAX_PLAYER_INSTANCES ||
        !g_manager_state.instances[instance_id].is_active) {
        return -1;
    }
    
    return g_manager_state.instances[instance_id].instance->Play();
}

int PauseInstance(int instance_id) {
    if (instance_id < 0 || instance_id >= MAX_PLAYER_INSTANCES ||
        !g_manager_state.instances[instance_id].is_active) {
        return -1;
    }

    return g_manager_state.instances[instance_id].instance->Pause();
}

void SetInstanceBackgrounded(int instance_id, int backgrounded) {
    if (instance_id < 0 || instance_id >= MAX_PLAYER_INSTANCES ||
        !g_manager_state.instances[instance_id].is_active) {
        return;
    }
    g_manager_state.instances[instance_id].instance->SetBackgrounded(backgrounded != 0);
}

int StopInstance(int instance_id) {
    if (instance_id < 0 || instance_id >= MAX_PLAYER_INSTANCES ||
        !g_manager_state.instances[instance_id].is_active) {
        return -1;
    }
    
    return g_manager_state.instances[instance_id].instance->Stop();
}

int SeekInstance(int instance_id, double position) {
    if (instance_id < 0 || instance_id >= MAX_PLAYER_INSTANCES ||
        !g_manager_state.instances[instance_id].is_active) {
        return -1;
    }
    
    return g_manager_state.instances[instance_id].instance->Seek(position);
}

// === Management of speed and direction ===

int SetInstanceSpeed(int instance_id, double speed) {
    if (instance_id < 0 || instance_id >= MAX_PLAYER_INSTANCES ||
        !g_manager_state.instances[instance_id].is_active) {
        return -1;
    }

    return g_manager_state.instances[instance_id].instance->SetSpeed(speed);
}

int SetInstanceSpeedInstant(int instance_id, double speed) {
    if (instance_id < 0 || instance_id >= MAX_PLAYER_INSTANCES ||
        !g_manager_state.instances[instance_id].is_active) {
        return -1;
    }

    return g_manager_state.instances[instance_id].instance->SetSpeedInstant(speed);
}

int SetInstanceReverse(int instance_id, bool reverse) {
    if (instance_id < 0 || instance_id >= MAX_PLAYER_INSTANCES ||
        !g_manager_state.instances[instance_id].is_active) {
        return -1;
    }
    return g_manager_state.instances[instance_id].instance->SetReverse(reverse);
}

int SetInstanceReverseInstant(int instance_id, bool reverse) {
    if (instance_id < 0 || instance_id >= MAX_PLAYER_INSTANCES ||
        !g_manager_state.instances[instance_id].is_active) {
        return -1;
    }
    return g_manager_state.instances[instance_id].instance->SetReverseInstant(reverse);
}

// === Getting the state of the instance ===

double GetInstancePosition(int instance_id) {
    if (instance_id < 0 || instance_id >= MAX_PLAYER_INSTANCES ||
        !g_manager_state.instances[instance_id].is_active) {
        return 0.0;
    }

    // Additional check for nullptr for thread safety
    auto& instance = g_manager_state.instances[instance_id].instance;
    if (!instance) {
        return 0.0;
    }

    return instance->GetPosition();
}

double GetInstanceDuration(int instance_id) {
    if (instance_id < 0 || instance_id >= MAX_PLAYER_INSTANCES ||
        !g_manager_state.instances[instance_id].is_active) {
        return 0.0;
    }

    // Additional check for nullptr for thread safety
    auto& instance = g_manager_state.instances[instance_id].instance;
    if (!instance) {
        return 0.0;
    }

    return instance->GetDuration();
}

double GetInstanceTimecodeOffset(int instance_id) {
    if (instance_id < 0 || instance_id >= MAX_PLAYER_INSTANCES ||
        !g_manager_state.instances[instance_id].is_active) {
        return 0.0;
    }
    auto& instance = g_manager_state.instances[instance_id].instance;
    if (!instance) {
        return 0.0;
    }
    return instance->GetTimecodeOffset();
}

int IsInstancePlaying(int instance_id) {
    if (instance_id < 0 || instance_id >= MAX_PLAYER_INSTANCES ||
        !g_manager_state.instances[instance_id].is_active) {
        return 0;
    }

    // Additional check for nullptr for thread safety
    auto& instance = g_manager_state.instances[instance_id].instance;
    if (!instance) {
        return 0;
    }

    return instance->IsPlaying() ? 1 : 0;
}

double GetInstanceSpeed(int instance_id) {
    if (instance_id < 0 || instance_id >= MAX_PLAYER_INSTANCES ||
        !g_manager_state.instances[instance_id].is_active) {
        return 1.0;
    }

    // Additional check for nullptr for thread safety
    auto& instance = g_manager_state.instances[instance_id].instance;
    if (!instance) {
        return 1.0;
    }

    return instance->GetSpeed();
}

double GetInstanceActualSpeed(int instance_id) {
    if (instance_id < 0 || instance_id >= MAX_PLAYER_INSTANCES ||
        !g_manager_state.instances[instance_id].is_active) {
        return 0.0;
    }

    // Additional check for nullptr for thread safety
    auto& instance = g_manager_state.instances[instance_id].instance;
    if (!instance) {
        return 0.0;
    }

    return instance->GetActualSpeed();
}

int IsInstanceReverse(int instance_id) {
    if (instance_id < 0 || instance_id >= MAX_PLAYER_INSTANCES ||
        !g_manager_state.instances[instance_id].is_active) {
        return 0;
    }

    // Additional check for nullptr for thread safety
    auto& instance = g_manager_state.instances[instance_id].instance;
    if (!instance) {
        return 0;
    }

    return instance->IsReverse() ? 1 : 0;
}

// === Management of active player ===
// Note: GetActivePlayerID() implemented in FSTPWindowManager.cpp
// because it is related to the active window, not the player manager

void SetActivePlayerID(int player_id) {
    if (player_id >= -1 && player_id < MAX_PLAYER_INSTANCES) {
        g_manager_state.active_player_id = player_id;
    }
}

bool GetInstanceFrameAligned(int instance_id) {
    if (instance_id < 0 || instance_id >= MAX_PLAYER_INSTANCES ||
        !g_manager_state.instances[instance_id].is_active) {
        return false;
    }
    auto& instance = g_manager_state.instances[instance_id].instance;
    if (!instance) return false;
    return instance->IsFrameAligned();
}

// === Getting audio signal levels ===

float GetInstanceAudioLevelLeft(int instance_id) {
    if (instance_id < 0 || instance_id >= MAX_PLAYER_INSTANCES ||
        !g_manager_state.instances[instance_id].is_active) {
        return 0.0f;
    }

    // Additional check for nullptr for thread safety
    auto& instance = g_manager_state.instances[instance_id].instance;
    if (!instance) {
        return 0.0f;
    }

    return instance->GetAudioLevelLeft();
}

float GetInstanceAudioLevelRight(int instance_id) {
    if (instance_id < 0 || instance_id >= MAX_PLAYER_INSTANCES ||
        !g_manager_state.instances[instance_id].is_active) {
        return 0.0f;
    }

    // Additional check for nullptr for thread safety
    auto& instance = g_manager_state.instances[instance_id].instance;
    if (!instance) {
        return 0.0f;
    }

    return instance->GetAudioLevelRight();
}

float GetInstanceAudioPeakLeft(int instance_id) {
    if (instance_id < 0 || instance_id >= MAX_PLAYER_INSTANCES ||
        !g_manager_state.instances[instance_id].is_active) {
        return 0.0f;
    }

    // Additional check for nullptr for thread safety
    auto& instance = g_manager_state.instances[instance_id].instance;
    if (!instance) {
        return 0.0f;
    }

    return instance->GetAudioPeakLeft();
}

float GetInstanceAudioPeakRight(int instance_id) {
    if (instance_id < 0 || instance_id >= MAX_PLAYER_INSTANCES ||
        !g_manager_state.instances[instance_id].is_active) {
        return 0.0f;
    }

    // Additional check for nullptr for thread safety
    auto& instance = g_manager_state.instances[instance_id].instance;
    if (!instance) {
        return 0.0f;
    }

    return instance->GetAudioPeakRight();
}

// Getting audio module for frame number calculation
FSTPAudioModuleWrapper* GetInstanceAudioModule(int instance_id) {
    if (instance_id < 0 || instance_id >= MAX_PLAYER_INSTANCES ||
        !g_manager_state.instances[instance_id].is_active) {
        return nullptr;
    }

    auto& instance = g_manager_state.instances[instance_id].instance;
    if (!instance) {
        return nullptr;
    }

    // Return direct access to audio module through FSTPPlayerInstance
    return instance->GetAudioModule();
}

// Getting video FPS for frame number calculation
double GetInstanceVideoFPS(int instance_id) {
    if (instance_id < 0 || instance_id >= MAX_PLAYER_INSTANCES ||
        !g_manager_state.instances[instance_id].is_active) {
        return 25.0; // Fallback PAL
    }

    auto& instance = g_manager_state.instances[instance_id].instance;
    if (!instance) {
        return 25.0; // Fallback PAL
    }

    return instance->GetFrameRate();
}

// === Updating video (60 FPS) ===

void UpdateAllVideoFrames() {
    if (!g_manager_state.initialized) {
        return;
    }

    // Process MIDI commands from controller (30 FPS - sufficient for responsiveness)
    if (g_remote_control && g_remote_control->IsInitialized()) {
        static int remote_update_counter = 0;
        if (++remote_update_counter >= 2) {  // Every 2nd frame at 60 FPS = 30 FPS
            g_remote_control->ProcessCommands();
            remote_update_counter = 0;
        }
    }

    // Diagnostics: how many players are active?
    if (ENABLE_PLAYER_MANAGER_DEBUG) {
        static int player_count_report = 0;
        if (++player_count_report >= 60) {
            int active_player_count = 0;
            for (int i = 0; i < MAX_PLAYER_INSTANCES; i++) {
                if (g_manager_state.instances[i].is_active && g_manager_state.instances[i].instance) {
                    active_player_count++;
                }
            }
            std::cout << "🎬 [PLAYERS] Active players being updated: " << active_player_count << std::endl;
            player_count_report = 0;
        }
    }

    // Update video for all active players
    for (int i = 0; i < MAX_PLAYER_INSTANCES; i++) {
        if (g_manager_state.instances[i].is_active && g_manager_state.instances[i].instance) {
            g_manager_state.instances[i].instance->UpdateVideo();
        }
    }
}

void UpdateVideoFrameForPlayer(int instance_id) {
    if (!g_manager_state.initialized) {
        return;
    }

    if (instance_id < 0 || instance_id >= MAX_PLAYER_INSTANCES) {
        return;
    }

    if (!g_manager_state.instances[instance_id].is_active || !g_manager_state.instances[instance_id].instance) {
        return;
    }

    // Update video only for this specific player
    g_manager_state.instances[instance_id].instance->UpdateVideo();
}

// === MIDI Remote Control API Implementation ===

int GetMIDIInputDeviceCount() {
    if (!g_remote_control || !g_remote_control->IsInitialized()) {
        return 0;
    }
    return static_cast<int>(g_remote_control->GetInputDevices().size());
}

int GetMIDIOutputDeviceCount() {
    if (!g_remote_control || !g_remote_control->IsInitialized()) {
        return 0;
    }
    return static_cast<int>(g_remote_control->GetOutputDevices().size());
}

const char* GetMIDIInputDeviceName(int index) {
    if (!g_remote_control || !g_remote_control->IsInitialized()) {
        return nullptr;
    }

    auto devices = g_remote_control->GetInputDevices();
    if (index < 0 || index >= static_cast<int>(devices.size())) {
        return nullptr;
    }

    // Use static buffer for each index (up to 16 devices)
    static std::string device_names[16];
    if (index < 16) {
        device_names[index] = devices[index];
        return device_names[index].c_str();
    }

    return nullptr;
}

const char* GetMIDIOutputDeviceName(int index) {
    if (!g_remote_control || !g_remote_control->IsInitialized()) {
        return nullptr;
    }

    auto devices = g_remote_control->GetOutputDevices();
    if (index < 0 || index >= static_cast<int>(devices.size())) {
        return nullptr;
    }

    // Use static buffer for each index (up to 16 devices)
    static std::string device_names[16];
    if (index < 16) {
        device_names[index] = devices[index];
        return device_names[index].c_str();
    }

    return nullptr;
}

void ApplyMIDISettings() {
    if (!g_remote_control || !g_remote_control->IsInitialized()) {
        std::cerr << "⚠️  [MIDI] Cannot apply settings - remote control not initialized" << std::endl;
        return;
    }

    // Get settings from FSTPSettings
    int input_index = GetMIDIInputPort();
    int output_index = GetMIDIOutputPort();

    std::cout << "🎹 [MIDI] Applying settings: input=" << input_index << ", output=" << output_index << std::endl;

        // Get device names by indices
    auto input_devices = g_remote_control->GetInputDevices();
    auto output_devices = g_remote_control->GetOutputDevices();

    if (input_index >= 0 && input_index < static_cast<int>(input_devices.size())) {
        std::string input_name = input_devices[input_index];
        g_remote_control->SelectDevice(input_name, true);
        std::cout << "🎹 [MIDI] Selected input: " << input_name << std::endl;
    }

    if (output_index >= 0 && output_index < static_cast<int>(output_devices.size())) {
        std::string output_name = output_devices[output_index];
        g_remote_control->SelectDevice(output_name, false);
        std::cout << "🎹 [MIDI] Selected output: " << output_name << std::endl;
    }
}

// === Inspector API - File Properties ===

int GetInstanceVideoWidth(int instance_id) {
    if (instance_id < 0 || instance_id >= MAX_PLAYER_INSTANCES) {
        return 0;
    }

    auto& slot = g_manager_state.instances[instance_id];
    if (!slot.is_active || !slot.instance) {
        return 0;
    }

    return slot.instance->GetVideoWidth();
}

int GetInstanceVideoHeight(int instance_id) {
    if (instance_id < 0 || instance_id >= MAX_PLAYER_INSTANCES) {
        return 0;
    }

    auto& slot = g_manager_state.instances[instance_id];
    if (!slot.is_active || !slot.instance) {
        return 0;
    }

    return slot.instance->GetVideoHeight();
}

int GetInstanceTotalFrames(int instance_id) {
    if (instance_id < 0 || instance_id >= MAX_PLAYER_INSTANCES) {
        return 0;
    }

    auto& slot = g_manager_state.instances[instance_id];
    if (!slot.is_active || !slot.instance) {
        return 0;
    }

    return slot.instance->GetTotalFrames();
}

const char* GetInstanceFileName(int instance_id) {
    const char* full_path = GetInstanceFilePath(instance_id);
    if (!full_path) {
        return nullptr;
    }

    // Use static buffer for storing filename
    static std::string filename_buffer;
    std::string path_str(full_path);

    // Find last slash
    size_t last_slash = path_str.find_last_of("/\\");
    if (last_slash != std::string::npos) {
        filename_buffer = path_str.substr(last_slash + 1);
    } else {
        filename_buffer = path_str;
    }

    return filename_buffer.c_str();
}

int GetInstanceAudioSampleRate(int instance_id) {
    if (instance_id < 0 || instance_id >= MAX_PLAYER_INSTANCES) {
        return 0;
    }

    auto& slot = g_manager_state.instances[instance_id];
    if (!slot.is_active || !slot.instance) {
        return 0;
    }

    auto* audio_module = slot.instance->GetAudioModule();
    if (!audio_module) {
        return 0;
    }

    return audio_module->GetSampleRate();
}

int GetInstanceAudioChannels(int instance_id) {
    if (instance_id < 0 || instance_id >= MAX_PLAYER_INSTANCES) {
        return 0;
    }

    auto& slot = g_manager_state.instances[instance_id];
    if (!slot.is_active || !slot.instance) {
        return 0;
    }

    auto* audio_module = slot.instance->GetAudioModule();
    if (!audio_module) {
        return 0;
    }

    return audio_module->GetChannels();
}

const char* GetInstanceAudioCodecName(int instance_id) {
    if (instance_id < 0 || instance_id >= MAX_PLAYER_INSTANCES) {
        return nullptr;
    }

    auto& slot = g_manager_state.instances[instance_id];
    if (!slot.is_active || !slot.instance) {
        return nullptr;
    }

    auto* audio_module = slot.instance->GetAudioModule();
    if (!audio_module) {
        return nullptr;
    }

    // Use static buffer for storing codec name
    static std::string codec_buffer;
    codec_buffer = audio_module->GetAudioCodecName();
    return codec_buffer.c_str();
}

const char* GetInstanceVideoCodecName(int instance_id) {
    if (instance_id < 0 || instance_id >= MAX_PLAYER_INSTANCES) {
        return nullptr;
    }

    auto& slot = g_manager_state.instances[instance_id];
    if (!slot.is_active || !slot.instance) {
        return nullptr;
    }

    // Use static buffer for storing codec name
    static std::string codec_buffer;
    codec_buffer = slot.instance->GetVideoCodecName();
    return codec_buffer.c_str();
}