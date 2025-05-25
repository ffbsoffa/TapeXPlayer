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

struct SeekInfo {
    std::atomic<bool> requested{false};
    std::atomic<double> time{0.0};
    std::atomic<bool> completed{false};
};

extern SeekInfo seekInfo;

// Add signal variable
extern std::atomic<bool> speed_reset_requested;

// Add function declaration for speed reset
void reset_to_normal_speed();

// Variables and functions for zoom control
extern std::atomic<bool> zoom_enabled;
extern std::atomic<float> zoom_factor;
extern std::atomic<float> zoom_center_x;
extern std::atomic<float> zoom_center_y;
extern std::atomic<bool> show_zoom_thumbnail;

// Maximum and minimum zoom factors
extern const float MAX_ZOOM_FACTOR;
extern const float MIN_ZOOM_FACTOR;

// Functions for zoom control
void increase_zoom();
void decrease_zoom();
void reset_zoom();
void set_zoom_center(float x, float y);
void toggle_zoom_thumbnail();

#endif // COMMON_H
