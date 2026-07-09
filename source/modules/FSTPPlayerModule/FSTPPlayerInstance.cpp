#include "FSTPPlayerInstance.h"
#include "../FSTPAudioModule/FSTPAudioModule_wrapper.h"
#include "../FSTPVideoModule/FSTPVideoModule_wrapper.h"
#include "../FSTPMainModule/WSGUI/FSTPOSDSystem.h"
#include "../FSTPMainModule/WSGUI/FSTPWindowManager.h"
#include "../FSTPMainModule/WSGUI/FSTPSettings.h"
#include <iostream>
#include <string>
#include <algorithm>
#include <limits>
#include <cmath>

FSTPPlayerInstance::FSTPPlayerInstance()
    : m_initialized(false), m_file_loaded(false), m_instance_id(-1), m_window_index(-1),
      m_audio_module(std::make_unique<FSTPAudioModuleWrapper>()),
      m_video_module(std::make_unique<FSTPVideoModuleWrapper>()) {  // Enable video module for interface testing
}

FSTPPlayerInstance::~FSTPPlayerInstance() {
    try {
        if (m_initialized) {
            Shutdown();
        }
    } catch (const std::exception& e) {
        std::cerr << "⚠️ [FSTPPlayerInstance] Exception in destructor: " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "⚠️ [FSTPPlayerInstance] Unknown exception in destructor" << std::endl;
    }
}

int FSTPPlayerInstance::Initialize() {
    if (m_initialized) {
        std::cout << "Player instance already initialized" << std::endl;
        return 0;
    }

    std::cout << "Initializing player instance..." << std::endl;

    // Audio module initialization
    if (!m_audio_module->Initialize()) {
        std::cerr << "Failed to initialize audio module" << std::endl;
        return -1;
    }

    // Video module initialization (optional)
    if (m_video_module && !m_video_module->Initialize()) {
        std::cerr << "Warning: Failed to initialize video module (continuing audio-only)" << std::endl;
        // Don't interrupt initialization, continue in audio-only mode
        m_video_module.reset();
    }

    m_initialized = true;
    std::cout << "Player instance initialized successfully" << std::endl;

    return 0;
}

void FSTPPlayerInstance::Shutdown() {
    if (!m_initialized) {
        return;
    }

    std::cout << "Shutting down player instance..." << std::endl;

    // Unload file if loaded
    if (m_file_loaded) {
        UnloadFile();
    }

    // Shutdown video module
    if (m_video_module) {
        m_video_module->Shutdown();
        m_video_module.reset();
    }

    // Shutdown audio module
    if (m_audio_module) {
        m_audio_module->Shutdown();
        m_audio_module.reset();
    }

    m_initialized = false;
    std::cout << "Player instance shutdown complete" << std::endl;
}

int FSTPPlayerInstance::LoadFile(const std::string& filepath, double resume_position) {
    if (!m_initialized) {
        std::cerr << "Player instance not initialized" << std::endl;
        return -1;
    }

    std::cout << "Loading video file into instance: " << filepath << std::endl;

    if (m_file_loaded) {
        UnloadFile();
    }

    if (!m_audio_module->LoadFile(filepath, resume_position)) {
        std::cerr << "Failed to load file into audio module: " << filepath << std::endl;
        return -3;
    }

    // Use the actual audio position as initial_time for the video module.
    // DecodePriorityZone may clamp resume_position if it is near the end of the file,
    // so the audio head may land earlier than the requested resume_position.
    // Passing the raw audio position keeps video in sync with where audio actually starts.
    double actual_initial_time = 0.0;
    if (resume_position > 0.0) {
        actual_initial_time = m_audio_module->GetPosition() - m_audio_module->GetTimecodeOffset();
    }

    if (m_video_module && !m_video_module->LoadFile(filepath, actual_initial_time)) {
        std::cerr << "Warning: Failed to load file into video module (continuing audio-only): " << filepath << std::endl;
    } else if (m_video_module) {
        std::cout << "🎥 [VIDEO] Video file loaded successfully!" << std::endl;
        m_video_module->SetAudioModule(m_audio_module.get());
        std::cout << "🎥 [AUDIO-VIDEO SYNC] Video module linked to audio module for frame synchronization" << std::endl;
    }

    m_file_path = filepath;
    m_file_loaded = true;

    if (m_video_module) {
        std::cout << "🎥 [TEST] Testing video update chain..." << std::endl;
        UpdateVideo();
    }

    std::cout << "Audio file loaded into instance successfully: " << filepath << std::endl;
    return 0;
}

void FSTPPlayerInstance::UnloadFile() {
    if (!m_file_loaded) {
        return;
    }

    std::cout << "Unloading video file from instance: " << m_file_path << std::endl;

    // Save resume position BEFORE Stop() — Stop() resets playback_position to 0
    if (m_audio_module && m_audio_module->IsLoaded() && !m_file_path.empty()) {
        double raw_pos = m_audio_module->GetPosition() - m_audio_module->GetTimecodeOffset();
        if (raw_pos > 1.0) {
            std::cout << "[RESUME] Saving position " << raw_pos << "s for: " << m_file_path << std::endl;
            SaveResumePosition(m_file_path.c_str(), raw_pos);
        }
    }

    // Stop playback
    if (m_audio_module && m_audio_module->IsPlaying()) {
        m_audio_module->Stop();
    }

    // Unload file from video module
    if (m_video_module) {
        m_video_module->UnloadFile();
    }

    // Unload file from audio module
    if (m_audio_module) {
        m_audio_module->UnloadFile();
    }

    m_file_path.clear();
    m_file_loaded = false;

    std::cout << "Video file unloaded from instance" << std::endl;
}

// === Playback Control Methods ===

int FSTPPlayerInstance::Play() {
    if (!m_initialized) {
        std::cerr << "Player instance not initialized" << std::endl;
        return -1;
    }

    if (!m_file_loaded) {
        std::cerr << "No file loaded in player instance" << std::endl;
        return -2;
    }

    if (!m_audio_module) {
        std::cerr << "Audio module not available" << std::endl;
        return -3;
    }

    std::cout << "Starting playback..." << std::endl;

    if (m_audio_module->Play()) {
        std::cout << "Playback started successfully" << std::endl;
        return 0;
    } else {
        std::cerr << "Failed to start playback" << std::endl;
        return -4;
    }
}

int FSTPPlayerInstance::Pause() {
    if (!m_initialized || !m_file_loaded || !m_audio_module) {
        std::cerr << "Player instance not ready for pause operation" << std::endl;
        return -1;
    }

    std::cout << "Pausing playback..." << std::endl;

    if (m_audio_module->Pause()) {
        std::cout << "Playback paused successfully" << std::endl;
        return 0;
    } else {
        std::cerr << "Failed to pause playback" << std::endl;
        return -2;
    }
}

int FSTPPlayerInstance::Stop() {
    if (!m_initialized || !m_file_loaded || !m_audio_module) {
        std::cerr << "Player instance not ready for stop operation" << std::endl;
        return -1;
    }

    std::cout << "Stopping playback..." << std::endl;

    // Save resume position BEFORE audio Stop() — Stop() resets playback_position to 0
    if (!m_file_path.empty()) {
        double raw_pos = m_audio_module->GetPosition() - m_audio_module->GetTimecodeOffset();
        if (raw_pos > 1.0) {
            std::cout << "[RESUME] Saving position " << raw_pos << "s on Stop() for: " << m_file_path << std::endl;
            SaveResumePosition(m_file_path.c_str(), raw_pos);
        }
    }

    if (m_audio_module->Stop()) {
        std::cout << "Playback stopped successfully" << std::endl;
        return 0;
    } else {
        std::cerr << "Failed to stop playback" << std::endl;
        return -2;
    }
}

int FSTPPlayerInstance::Seek(double position) {
    if (!m_initialized || !m_file_loaded || !m_audio_module) {
        std::cerr << "Player instance not ready for seek operation" << std::endl;
        return -1;
    }

    if (position < 0.0) {
        std::cerr << "Invalid seek position: " << position << std::endl;
        return -2;
    }

    double duration = m_audio_module->GetDuration();
    if (position > duration) {
        std::cerr << "Seek position exceeds file duration: " << position << " > " << duration << std::endl;
        return -3;
    }

    std::cout << "Seeking to position: " << position << " seconds" << std::endl;

    m_audio_module->SetPosition(position);
    std::cout << "Seek completed successfully" << std::endl;
    return 0;
}

int FSTPPlayerInstance::SetSpeed(double speed) {
    if (!m_initialized || !m_audio_module) {
        std::cerr << "Player instance not ready for speed change" << std::endl;
        return -1;
    }

    if (speed <= 0.0) {
        std::cerr << "Invalid speed value: " << speed << " (must be > 0)" << std::endl;
        return -2;
    }

    // TAPE THREADING: while the proxy is still converting in the background, the transport
    // is limited to ≤1× — the full-res V2 decoder reads the original forward, but shuttle
    // needs the GOP=4 proxy. Like a real deck: no shuttle until the tape is threaded.
    if (speed > 1.0 && m_video_module && m_video_module->IsLoaded() &&
        !m_video_module->IsShuttleReady()) {
        std::cout << "[THREADING] Shuttle not ready — clamping speed " << speed << "x → 1x" << std::endl;
        speed = 1.0;
    }

    static double last_logged_speed = std::numeric_limits<double>::quiet_NaN();
    bool should_log = std::isnan(last_logged_speed) || std::abs(last_logged_speed - speed) >= 0.5;
    if (should_log) {
        std::cout << "Setting playback speed to: " << speed << "x" << std::endl;
    }
    last_logged_speed = speed;

    m_audio_module->SetSpeed(speed);

    // CRITICAL: Also notify video module to enable Full-Res decoder optimization
    if (m_video_module) {
        m_video_module->SetSpeed(speed);
    }

    if (should_log) {
        std::cout << "Speed changed successfully" << std::endl;
    }
    return 0;
}

int FSTPPlayerInstance::SetSpeedInstant(double speed) {
    if (!m_initialized || !m_audio_module) {
        std::cerr << "Player instance not ready for instant speed change" << std::endl;
        return -1;
    }

    if (speed <= 0.0) {
        std::cerr << "Invalid instant speed value: " << speed << " (must be > 0)" << std::endl;
        return -2;
    }

    // TAPE THREADING: same ≤1× clamp as SetSpeed (see comment there)
    if (speed > 1.0 && m_video_module && m_video_module->IsLoaded() &&
        !m_video_module->IsShuttleReady()) {
        std::cout << "[THREADING] Shuttle not ready — clamping instant speed " << speed << "x → 1x" << std::endl;
        speed = 1.0;
    }

    m_audio_module->SetSpeedInstant(speed);

    // CRITICAL: Also notify video module to enable Full-Res decoder optimization
    if (m_video_module) {
        m_video_module->SetSpeedInstant(speed);
    }

    return 0;
}

int FSTPPlayerInstance::SetReverse(bool reverse) {
    if (!m_initialized || !m_audio_module) {
        std::cerr << "Player instance not ready for reverse change" << std::endl;
        return -1;
    }
    // TAPE THREADING: reverse needs the proxy (V2 streams the original forward only);
    // ignore the request until the background conversion publishes the proxy decoder.
    if (reverse && m_video_module && m_video_module->IsLoaded() &&
        !m_video_module->IsShuttleReady()) {
        std::cout << "[THREADING] Shuttle not ready — reverse ignored" << std::endl;
        return 0;
    }
    // Audio module runs direction-change sequencer; video module follows immediately
    // (video will reflect new direction via IsReverse() once sequencer flips the flag)
    m_audio_module->SetReverse(reverse);
    if (m_video_module) {
        m_video_module->SetReverse(reverse);
    }
    return 0;
}

int FSTPPlayerInstance::SetReverseInstant(bool reverse) {
    if (!m_initialized || !m_audio_module) {
        std::cerr << "Player instance not ready for instant reverse change" << std::endl;
        return -1;
    }
    // TAPE THREADING: same reverse gate as SetReverse (see comment there)
    if (reverse && m_video_module && m_video_module->IsLoaded() &&
        !m_video_module->IsShuttleReady()) {
        std::cout << "[THREADING] Shuttle not ready — instant reverse ignored" << std::endl;
        return 0;
    }
    m_audio_module->SetReverseInstant(reverse);
    if (m_video_module) {
        m_video_module->SetReverse(reverse);
    }
    return 0;
}

void FSTPPlayerInstance::SetBackgrounded(bool backgrounded) {
    // Resource courtesy: a backgrounded (unfocused, not-simultaneously-needed) instance frees its
    // heavy full-res decoder. Pure video-side; audio/playback state untouched.
    if (m_video_module) {
        m_video_module->SetBackgrounded(backgrounded);
    }
}

// === State Retrieval Methods ===

double FSTPPlayerInstance::GetPosition() const {
    if (!m_initialized || !m_file_loaded || !m_audio_module) {
        return 0.0;
    }
    return m_audio_module->GetPosition();
}

double FSTPPlayerInstance::GetDuration() const {
    if (!m_initialized || !m_file_loaded || !m_audio_module) {
        return 0.0;
    }
    return m_audio_module->GetDuration();
}

double FSTPPlayerInstance::GetTimecodeOffset() const {
    if (!m_initialized || !m_file_loaded || !m_audio_module) {
        return 0.0;
    }
    return m_audio_module->GetTimecodeOffset();
}

bool FSTPPlayerInstance::IsPlaying() const {
    if (!m_initialized || !m_file_loaded || !m_audio_module) {
        return false;
    }
    return m_audio_module->IsPlaying();
}

double FSTPPlayerInstance::GetSpeed() const {
    if (!m_initialized || !m_audio_module) {
        return 1.0;
    }
    return m_audio_module->GetSpeed();
}

bool FSTPPlayerInstance::IsReverse() const {
    if (!m_initialized || !m_audio_module) {
        return false;
    }
    return m_audio_module->IsReverse();
}

bool FSTPPlayerInstance::IsFrameAligned() const {
    if (!m_initialized || !m_audio_module) {
        return false;
    }
    return m_audio_module->IsFrameAligned();
}

double FSTPPlayerInstance::GetActualSpeed() const {
    if (!m_initialized || !m_file_loaded || !m_audio_module) {
        return 0.0;
    }
    return m_audio_module->GetActualSpeed();
}

// === Additional Methods for Working with Audio Module ===

bool FSTPPlayerInstance::IsFastBufferReady() const {
    if (!m_initialized || !m_file_loaded || !m_audio_module) {
        return false;
    }
    return m_audio_module->IsFastBufferReady();
}

bool FSTPPlayerInstance::IsFullBufferReady() const {
    if (!m_initialized || !m_file_loaded || !m_audio_module) {
        return false;
    }
    return m_audio_module->IsFullBufferReady();
}

int FSTPPlayerInstance::GetSampleRate() const {
    if (!m_initialized || !m_file_loaded || !m_audio_module) {
        return 0;
    }
    return m_audio_module->GetSampleRate();
}

int FSTPPlayerInstance::GetChannels() const {
    if (!m_initialized || !m_file_loaded || !m_audio_module) {
        return 0;
    }
    return m_audio_module->GetChannels();
}

float FSTPPlayerInstance::GetAudioLevelLeft() const {
    if (!m_initialized || !m_file_loaded || !m_audio_module) {
        return 0.0f;
    }
    return m_audio_module->GetAudioLevelLeft();
}

float FSTPPlayerInstance::GetAudioLevelRight() const {
    if (!m_initialized || !m_file_loaded || !m_audio_module) {
        return 0.0f;
    }
    return m_audio_module->GetAudioLevelRight();
}

float FSTPPlayerInstance::GetAudioPeakLeft() const {
    if (!m_initialized || !m_file_loaded || !m_audio_module) {
        return 0.0f;
    }
    return m_audio_module->GetAudioPeakLeft();
}

float FSTPPlayerInstance::GetAudioPeakRight() const {
    if (!m_initialized || !m_file_loaded || !m_audio_module) {
        return 0.0f;
    }
    return m_audio_module->GetAudioPeakRight();
}

// === Methods for Working with Video Module ===

int FSTPPlayerInstance::GetVideoWidth() const {
    if (m_video_module && m_file_loaded) {
        return m_video_module->GetWidth();
    }
    return 0;
}

int FSTPPlayerInstance::GetVideoHeight() const {
    if (m_video_module && m_file_loaded) {
        return m_video_module->GetHeight();
    }
    return 0;
}

double FSTPPlayerInstance::GetFrameRate() const {
    // FIXED: Return REAL FPS from video module!
    if (m_video_module && m_file_loaded) {
        double fps = m_video_module->GetFrameRate();
        if (fps > 0.0) {
            return fps;
        }
    }
    // Fallback only if video module not initialized
    return 25.0; // PAL standard framerate (fallback)
}

int FSTPPlayerInstance::GetTotalFrames() const {
    if (!m_initialized || !m_file_loaded || !m_audio_module) {
        return 0;
    }
    // Calculate frames based on audio duration and standard framerate
    double duration = m_audio_module->GetDuration();
    double fps = GetFrameRate();
    return static_cast<int>(duration * fps);
}

std::string FSTPPlayerInstance::GetVideoCodecName() const {
    if (m_video_module && m_file_loaded) {
        return m_video_module->GetVideoCodecName();
    }
    return "Unknown";
}

// Access to audio module for frame number calculation
FSTPAudioModuleWrapper* FSTPPlayerInstance::GetAudioModule() const {
    return m_audio_module.get();
}

// Video frame update (slave mode - follows audio time)
void FSTPPlayerInstance::UpdateVideo() const {
    if (m_video_module && m_file_loaded) {
        m_video_module->UpdateVideoFrame();
    }
}

// Instance ID management
void FSTPPlayerInstance::SetInstanceID(int instance_id) {
    m_instance_id = instance_id;
    m_window_index = instance_id; // Window N is bound to player N
    std::cout << "🎬 [PLAYER INSTANCE] Setting instance ID to: " << instance_id << " (window: " << m_window_index << ")" << std::endl;
    
    // Pass ID to video module
    if (m_video_module) {
        m_video_module->SetInstanceID(instance_id);
    }
}

int FSTPPlayerInstance::GetInstanceID() const {
    return m_instance_id;
}
