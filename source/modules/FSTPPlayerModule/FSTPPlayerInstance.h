#ifndef FSTP_PLAYER_INSTANCE_H
#define FSTP_PLAYER_INSTANCE_H

#include <string>
#include <memory>

// Forward declarations
class FSTPAudioModuleWrapper;
class FSTPVideoModuleWrapper;

// Class for individual player instance for one video file
class FSTPPlayerInstance {
private:
    bool m_initialized;
    bool m_file_loaded;
    std::string m_file_path;
    int m_instance_id; // Unique ID for this instance
    int m_window_index; // Index of window bound to this instance

    // Audio and video modules
    std::unique_ptr<FSTPAudioModuleWrapper> m_audio_module;
    std::unique_ptr<FSTPVideoModuleWrapper> m_video_module;

public:
    FSTPPlayerInstance();
    ~FSTPPlayerInstance();

    // Player instance initialization
    int Initialize();

    // Instance shutdown
    void Shutdown();

    // Load specific video file into this instance
    int LoadFile(const std::string& filepath, double resume_position = 0.0);

    // Unload file from instance
    void UnloadFile();

    // State checks
    bool IsInitialized() const { return m_initialized; }
    bool IsFileLoaded() const { return m_file_loaded; }
    const std::string& GetFilePath() const { return m_file_path; }

    // Playback control methods
    int Play();
    int Pause();
    int Stop();
    int Seek(double position);

    // Speed and direction control
    int SetSpeed(double speed);
    int SetSpeedInstant(double speed);    // For Mouse Shuttle - without animation
    int SetReverse(bool reverse);         // With direction-change sequencer
    int SetReverseInstant(bool reverse);  // For Mouse Shuttle - instant, no sequencer
    void SetBackgrounded(bool backgrounded); // Free/restore the full-res decoder when unfocused

    // State retrieval
    double GetPosition() const;
    double GetDuration() const;
    double GetTimecodeOffset() const;
    bool IsPlaying() const;
    double GetSpeed() const;            // Target speed
    double GetActualSpeed() const;      // Actual animated speed
    bool IsReverse() const;

    // Additional methods for working with audio module
    bool IsFastBufferReady() const;
    bool IsFullBufferReady() const;
    bool IsFrameAligned() const;
    
    // Get information about loaded file
    int GetSampleRate() const;
    int GetChannels() const;

    // Get audio signal levels
    float GetAudioLevelLeft() const;
    float GetAudioLevelRight() const;
    float GetAudioPeakLeft() const;
    float GetAudioPeakRight() const;
    
    // Methods for working with video module
    int GetVideoWidth() const;
    int GetVideoHeight() const;
    double GetFrameRate() const;
    int GetTotalFrames() const;
    std::string GetVideoCodecName() const;
    
    // Access to audio module for frame number calculation
    class FSTPAudioModuleWrapper* GetAudioModule() const;

    // Video frame update (called 60 FPS from main loop)
    void UpdateVideo() const;
    
    // Instance ID management
    void SetInstanceID(int instance_id);
    int GetInstanceID() const;
};

#endif // FSTP_PLAYER_INSTANCE_H