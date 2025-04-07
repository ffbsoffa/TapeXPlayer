#pragma once

#include <string>
#include <vector>
#include <atomic>

// Audio device management functions
extern std::vector<std::string> get_audio_output_devices();
extern int get_current_audio_device_index();
extern bool switch_audio_device(int deviceIndex);
extern int get_audio_device_count();
extern std::string get_audio_device_name(int index);

// Audio playback functions
extern void toggle_pause();
extern void start_jog_forward();
extern void start_jog_backward();
extern void stop_jog();
extern void start_audio(const char* filename);
extern void cleanup_audio();
extern void seek_to_time(double target_time);
extern void increase_volume();
extern void decrease_volume();

// Global variables
extern std::atomic<int> selected_audio_device_index; 