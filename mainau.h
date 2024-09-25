#ifndef MAINAU_H
#define MAINAU_H

#include <atomic>
#include <string>

extern std::atomic<bool> quit;
extern std::atomic<double> playback_rate;  // Раскомментировать эту строку
extern std::atomic<double> target_playback_rate;  // Добавить эту строку
extern std::atomic<bool> is_reverse;
extern std::atomic<double> current_audio_time;
extern std::atomic<double> total_duration;
extern std::atomic<double> original_fps;
extern std::atomic<float> volume;  // Добавить объявление новой глобальной переменной

// Объявления функций
void start_audio(const char* filename);
void toggle_pause();
double get_current_audio_time();
double get_original_fps();
std::string format_time(double time, double fps);
double get_video_fps(const char* filename);
double get_file_duration(const char* filename);
void set_target_playback_rate(double rate);  // Добавить точку с запятой
void smooth_speed_change();  // Добавить объявление новой функции
void increase_volume();
void decrease_volume();
double get_precise_audio_time();

#endif // MAINAU_H