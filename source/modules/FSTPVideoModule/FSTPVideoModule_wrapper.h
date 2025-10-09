#pragma once

#include <string>
#include <memory>
#include <atomic>
#include <vector>

#include "FSTPFrameConverter.h"
#include "FSTPVideoFrame.h"
#include "FSTPLowResDecoder.h"
#include "FSTPLowCachedDecoderManager.h"
#include "FSTPSimpleVideoIndex.h"
#include "FSTPFullResDecoder_v2.h"  // V2: Streaming decoder

extern "C" {
#include <libavcodec/avcodec.h>
}

// Forward declaration
class FSTPAudioModuleWrapper;

class FSTPVideoModuleWrapper {
private:
    bool m_initialized = false;
    bool m_loaded = false;
    std::string m_current_file;
    std::string m_proxy_path;
    bool m_use_original_as_proxy = false; // Flag: use original as proxy (≤480p h264)

    int m_instance_id = -1;

    std::atomic<double> m_duration{0.0};
    std::atomic<bool> m_playing{false};
    std::atomic<double> m_speed{1.0};
    std::atomic<bool> m_reverse{false};

    FSTPAudioModuleWrapper* m_audio_module;

    std::unique_ptr<FSTPFrameConverter> m_frame_converter;

    mutable int m_current_frame_number;
    mutable int m_last_good_frame_number;

    // Optimization: track last rendered frame to skip identical ones
    mutable int m_last_displayed_frame = -1;

    std::unique_ptr<FSTPSimpleVideoIndex> m_frame_index;
    std::vector<FSTP::FrameInfo> m_frames;
    std::atomic<int> m_current_index{0};
    std::atomic<bool> m_decoders_active{false};
    std::atomic<bool> m_file_reloading{false}; // Protection from race condition when changing file

    std::unique_ptr<FSTP::LowCachedDecoderManager> m_low_cached_manager;
    std::unique_ptr<FSTPFullResDecoderV2> m_full_res_decoder;  // V2: Streaming decoder

    mutable FSTP::FrameBuffer m_display_buffer;

    int m_low_res_range = 1200;
    int m_high_res_window = 100;  // Reduced from 500 to 100 to reduce load

    int m_video_width = 0;
    int m_video_height = 0;
    double m_frame_rate = 0.0;
    int m_total_frames = 0;

    bool EnsureProxy(const std::string& filepath, const std::function<void(int)>& progressCallback);
    bool InitializeDecoders();
    void ShutdownDecoders();
    void NotifyDecodersOfFrameChange(int frame_number);
    bool SubmitFrameToTexture(const std::shared_ptr<AVFrame>& frame,
                              int frame_number,
                              double timestamp);

public:
    FSTPVideoModuleWrapper();
    ~FSTPVideoModuleWrapper();

    bool Initialize();
    void Shutdown();

    bool LoadFile(const std::string& filepath);
    void UnloadFile();

    bool Play();
    bool Pause();
    bool Stop();

    void SetSpeed(double speed);
    void SetSpeedInstant(double speed);
    void SetReverse(bool reverse);

    void SetPosition(double position_seconds);
    double GetPosition() const;
    double GetDuration() const;

    bool IsPlaying() const;
    bool IsLoaded() const;
    double GetSpeed() const;
    double GetActualSpeed() const;
    bool IsReverse() const;

    int GetWidth() const;
    int GetHeight() const;
    double GetFrameRate() const;
    int GetTotalFrames() const;
    std::string GetVideoCodecName() const;

    // Functions for developer visualization
    const std::vector<FSTP::FrameInfo>& GetFrameIndex() const;
    int GetCurrentFrameIndex() const;
    void GetBufferRanges(int& bufferStart, int& bufferEnd, int& highResStart, int& highResEnd) const;

    bool IsFastBufferReady() const;
    bool IsFullBufferReady() const;

    bool GetCurrentFrame(uint8_t** frame_data, int& width, int& height) const;
    void ReleaseFrame(uint8_t* frame_data) const;

    void SetAudioModule(FSTPAudioModuleWrapper* audio_module);
    int GetCurrentAudioFrame() const;

    void SetInstanceID(int instance_id);
    int GetInstanceID() const;

    void DisplayFrame(int frame_number);
    void UpdateVideoFrame();
    void UpdateVideoFrame() const;
};