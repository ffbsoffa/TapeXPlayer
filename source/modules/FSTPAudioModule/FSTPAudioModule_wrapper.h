#ifndef FSTP_AUDIO_MODULE_WRAPPER_H
#define FSTP_AUDIO_MODULE_WRAPPER_H

#include <string>
#include <atomic>
#include <memory>

class FSTPAudioModuleWrapper {
public:
    FSTPAudioModuleWrapper();
    ~FSTPAudioModuleWrapper();

    // Main methods
    bool Initialize();
    void Shutdown();

    bool LoadFile(const std::string& filepath);
    void UnloadFile();

    // Playback control
    bool Play();
    bool Pause();
    bool Stop();

    // Speed and direction control
    void SetSpeed(double speed);
    void SetSpeedInstant(double speed);  // For Mouse Shuttle - without animation
    void SetReverse(bool reverse);
    void SetPosition(double position_seconds);

    // State retrieval
    double GetPosition() const;
    double GetDuration() const;
    bool IsPlaying() const;
    bool IsLoaded() const;
    double GetSpeed() const;            // Target speed
    double GetActualSpeed() const;      // Actual animated speed
    bool IsReverse() const;

    // Buffer status
    bool IsFastBufferReady() const;
    bool IsFullBufferReady() const;

    // Information about loaded file
    int GetSampleRate() const;
    int GetChannels() const;
    std::string GetAudioCodecName() const;

    // Audio signal levels for VU meters
    float GetAudioLevelLeft() const;
    float GetAudioLevelRight() const;
    float GetAudioPeakLeft() const;
    float GetAudioPeakRight() const;

    // Restart audio stream with new settings
    bool RestartAudioStream();

    // Video sync: set frame rate for frame alignment on prolonged pause
    void SetVideoFrameRate(double fps);

    // Friend class for API
    friend class FSTPAudioModule_API;

    // Accessor methods for API (only for friend class)
    int GetSampleRateInternal() const;
    int GetChannelsInternal() const;
    double GetPlaybackPositionInternal() const;
    void SetPlaybackPositionInternal(double sample_position);
    size_t GetDecodedSamplesInternal() const;

private:
    class FSTPAudioModuleImpl;
    std::unique_ptr<FSTPAudioModuleImpl> m_impl;

    bool m_initialized = false;
    bool m_loaded = false;
    std::string m_current_file;

    std::atomic<double> m_duration{0.0};
    std::atomic<double> m_speed{1.0};
    std::atomic<bool> m_reverse{false};
    std::atomic<bool> m_playing{false};
};

#endif // FSTP_AUDIO_MODULE_WRAPPER_H