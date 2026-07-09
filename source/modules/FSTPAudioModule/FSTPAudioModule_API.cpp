#include "FSTPAudioModule_API.h"
#include "FSTPAudioModule_wrapper.h"
#include <algorithm>

// Need to declare FSTPAudioModuleImpl as friend or get access through public methods
// But since we have friend class, need to include full definition
// Add declaration here for access to impl
extern "C" {
    #include <atomic>
}

// For access to impl need to declare it here or create accessor methods

FSTPAudioModule_API::FSTPAudioModule_API() {
}

FSTPAudioModule_API::~FSTPAudioModule_API() {
}

// Moved from FSTPAudioModule_wrapper.cpp:966-971
double FSTPAudioModule_API::GetPosition(FSTPAudioModuleWrapper* wrapper) {
    int sample_rate = wrapper->GetSampleRateInternal();
    int channels = wrapper->GetChannelsInternal();
    if (sample_rate > 0 && channels > 0) {
        return wrapper->GetPlaybackPositionInternal() / (sample_rate * channels);
    }
    return 0.0;
}

// Moved from FSTPAudioModule_wrapper.cpp:958-964
void FSTPAudioModule_API::SetPosition(FSTPAudioModuleWrapper* wrapper, double position_seconds) {
    int sample_rate = wrapper->GetSampleRateInternal();
    int channels = wrapper->GetChannelsInternal();
    if (sample_rate > 0 && channels > 0) {
        double sample_pos = position_seconds * sample_rate * channels;
        sample_pos = std::max(0.0, std::min(sample_pos, static_cast<double>(wrapper->GetDecodedSamplesInternal())));
        wrapper->SetPlaybackPositionInternal(sample_pos);
    }
}

// Moved from FSTPAudioModule_wrapper.cpp:973-975
double FSTPAudioModule_API::GetDuration(FSTPAudioModuleWrapper* wrapper) {
    return wrapper->m_duration.load();
}

// Time to frame conversion functions
int FSTPAudioModule_API::GetCurrentFrame(FSTPAudioModuleWrapper* wrapper, double fps) {
    if (fps <= 0.0) return 0;
    double current_time = GetPosition(wrapper);
    return TimeToFrame(current_time, fps);
}

int FSTPAudioModule_API::GetTotalFrames(FSTPAudioModuleWrapper* wrapper, double fps) {
    if (fps <= 0.0) return 0;
    double total_time = GetDuration(wrapper);
    return TimeToFrame(total_time, fps);
}

double FSTPAudioModule_API::FrameToTime(int frame_number, double fps) {
    if (fps <= 0.0) return 0.0;
    return static_cast<double>(frame_number) / fps;
}

int FSTPAudioModule_API::TimeToFrame(double time_seconds, double fps) {
    if (fps <= 0.0) return 0;
    return static_cast<int>(time_seconds * fps);
}