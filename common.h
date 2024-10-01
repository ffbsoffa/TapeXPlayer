#ifndef COMMON_H
#define COMMON_H

#include <atomic>
#include <SDL2/SDL.h>
#include <mutex>

extern std::mutex cout_mutex;
extern std::atomic<bool> quit;
extern std::atomic<double> current_audio_time;
extern std::atomic<int> current_video_frame;
extern SDL_Window* window;
extern SDL_Renderer* renderer;
extern std::atomic<bool> seek_performed;
extern std::atomic<double> playback_rate;
extern std::atomic<double> target_playback_rate; 
extern std::atomic<double> previous_playback_rate;
extern std::atomic<bool> is_reverse;
extern std::atomic<bool> is_seeking;
void toggle_pause();

extern std::atomic<double> total_duration;
extern std::atomic<double> original_fps;

// Add these function declarations
double get_video_fps(const char* filename);
double get_file_duration(const char* filename);
void smooth_speed_change(); 

extern std::atomic<float> volume;

void increase_volume();
void decrease_volume();
extern std::atomic<bool> shouldExit;

extern std::atomic<bool> jog_forward;
extern std::atomic<bool> jog_backward;
extern const double JOG_SPEED;

void start_jog_forward();
void start_jog_backward();
void stop_jog();

void seek_to_time(double target_time);
double parse_timecode(const std::string& timecode);

#endif // COMMON_H
