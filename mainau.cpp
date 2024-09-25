extern "C" {
    #include <libavcodec/avcodec.h>
    #include <libavformat/avformat.h>
    #include <libavutil/opt.h>
    #include <libavutil/channel_layout.h>
}
#include <SDL2/SDL.h>
#include <iostream>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <gst/gst.h>

#include "mainau.h"

std::atomic<bool> quit(false);
std::atomic<double> playback_rate(1.0);
std::atomic<bool> is_reverse(false);
std::atomic<double> current_audio_time(0.0);
std::atomic<double> total_duration(0.0);
std::atomic<double> original_fps(0.0);
std::atomic<float> volume(1.0f);  // Начальная громкость 100%

std::vector<float> audio_buffer;
size_t audio_buffer_index = 0;
SDL_AudioDeviceID audioDevice;
std::atomic<bool> decoding_finished{false};  // Добавьте эту строку

// Используем глобальные переменные из common.h
extern std::atomic<bool> quit;
extern std::atomic<double> playback_rate;
extern std::atomic<bool> is_reverse;
extern std::atomic<double> current_audio_time;
extern std::atomic<double> total_duration;
extern std::atomic<double> original_fps;

std::atomic<double> target_playback_rate(1.0);

void audio_callback(void*, Uint8* stream, int len) {
    float* float_stream = reinterpret_cast<float*>(stream);
    int float_len = len / sizeof(float);
    SDL_setenv("SDL_AUDIODRIVER", "coreaudio", 1);

    SDL_memset(stream, 0, len);

    if (audio_buffer_index >= audio_buffer.size() || audio_buffer_index < 0) {
        quit.store(true);
        return;
    }

    double rate = playback_rate.load();
    if (rate == 0.0) {
        SDL_memset(stream, 0, len);
        return;
    }

    float current_volume = volume.load();
    double position = audio_buffer_index;
    const int window_size = 4; // Размер окна сглаживания

    for (int i = 0; i < float_len; ++i) {
        if (is_reverse.load()) {
            position -= rate;
        } else {
            position += rate;
        }

        if (position < 0 || position >= audio_buffer.size() - window_size) {
            float_stream[i] = 0.0f;
        } else {
            float sum = 0.0f;
            for (int j = 0; j < window_size; ++j) {
                size_t index = static_cast<size_t>(position) + j;
                float weight = 1.0f - std::abs(j - (window_size / 2.0f - 0.5f)) / (window_size / 2.0f);
                sum += audio_buffer[index] * weight;
            }
            float_stream[i] = (sum / window_size) * current_volume;
        }
    }

    audio_buffer_index = static_cast<size_t>(position);
    if (audio_buffer_index >= audio_buffer.size() || audio_buffer_index < 0) {
        audio_buffer_index = 0;
    }

    current_audio_time.store(static_cast<double>(audio_buffer_index) / audio_buffer.size() * total_duration.load(), std::memory_order_release);
}

void decode_audio(const char* filename) {
    AVFormatContext* format_ctx = nullptr;
    AVCodecContext* audio_codec_ctx = nullptr;
    const AVCodec* audio_codec = nullptr;
    AVFrame* frame = nullptr;
    AVPacket packet;
    int audio_stream_index = -1;

    try {
        if (avformat_open_input(&format_ctx, filename, nullptr, nullptr) != 0) {
            throw std::runtime_error("Не удалось открыть файл");
        }
        if (avformat_find_stream_info(format_ctx, nullptr) < 0) {
            throw std::runtime_error("Не удалось получить информацию о потоке");
        }

        audio_stream_index = av_find_best_stream(format_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, &audio_codec, 0);
        if (audio_stream_index < 0) {
            throw std::runtime_error("Не удалось найти аудиопоток");
        }

        audio_codec_ctx = avcodec_alloc_context3(audio_codec);
        if (!audio_codec_ctx) {
            throw std::runtime_error("Не удалось выделить память для контекста кодека");
        }

        if (avcodec_parameters_to_context(audio_codec_ctx, format_ctx->streams[audio_stream_index]->codecpar) < 0) {
            throw std::runtime_error("Не удалось установить параметры кодека");
        }

        if (avcodec_open2(audio_codec_ctx, audio_codec, nullptr) < 0) {
            throw std::runtime_error("Не удалось открыть аудиокодек");
        }

        frame = av_frame_alloc();
        if (!frame) {
            throw std::runtime_error("Не удалось выделить память для фрейма");
        }

        while (av_read_frame(format_ctx, &packet) >= 0 && !quit.load()) {
            if (packet.stream_index == audio_stream_index) {
                if (avcodec_send_packet(audio_codec_ctx, &packet) == 0) {
                    while (avcodec_receive_frame(audio_codec_ctx, frame) == 0) {
                        for (int i = 0; i < frame->nb_samples; ++i) {
                            for (int ch = 0; ch < frame->ch_layout.nb_channels; ++ch) {
                                audio_buffer.push_back(reinterpret_cast<float*>(frame->data[ch])[i]);
                            }
                        }
                    }
                }
            }
            av_packet_unref(&packet);
        }

        decoding_finished.store(true);
    }
    catch (const std::exception& e) {
        std::cerr << "Ошибка декодирования: " << e.what() << std::endl;
        decoding_finished.store(true);  // Устанавливаем флаг даже в случае ошибки
    }

    if (frame) av_frame_free(&frame);
    if (audio_codec_ctx) avcodec_free_context(&audio_codec_ctx);
    if (format_ctx) avformat_close_input(&format_ctx);
}

void start_audio(const char* filename) {
    try {
        // Получаем частоту дискретизации из файла
        AVFormatContext* format_ctx = nullptr;
        if (avformat_open_input(&format_ctx, filename, nullptr, nullptr) != 0) {
            throw std::runtime_error("Не удалось открыть файл");
        }

        int audio_stream_index = av_find_best_stream(format_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (audio_stream_index < 0) {
            avformat_close_input(&format_ctx);
            throw std::runtime_error("Не удалось найти аудиопоток");
        }

        int sample_rate = format_ctx->streams[audio_stream_index]->codecpar->sample_rate;
        avformat_close_input(&format_ctx);

        SDL_AudioSpec want, have;
        SDL_zero(want);
        want.freq = sample_rate;  // Используем частоту дискретизации из файла
        want.format = AUDIO_F32SYS;
        want.channels = 2;
        want.samples = 1024;
        want.callback = audio_callback;

        audioDevice = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (audioDevice == 0) {
            throw std::runtime_error("Failed to open audio device");
        }

        // Проверяем, соответствует ли полученная конфигурация запрошенной
        if (have.freq != want.freq || have.format != want.format || have.channels != want.channels) {
            std::cerr << "Warning: Audio device couldn't provide exact requested format." << std::endl;
            std::cerr << "Requested: " << want.freq << "Hz, " << want.channels << " channels" << std::endl;
            std::cerr << "Obtained: " << have.freq << "Hz, " << have.channels << " channels" << std::endl;
        }

        audio_buffer_index = 0;
        current_audio_time.store(0.0);

        // Запускаем асинхронное декодирование
        std::thread decoding_thread(decode_audio, filename);
        decoding_thread.detach();  // Отсоединяем поток, чтобы он работал независимо

        std::thread speed_change_thread(smooth_speed_change);
        speed_change_thread.detach();

        SDL_PauseAudioDevice(audioDevice, 0);  // Запускаем устройство, но аудио на паузе

        std::cout << "Аудио готово к воспроизведению. Нажмите 'p' для воспроизведения/паузы." << std::endl;

        while (!quit.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        SDL_CloseAudioDevice(audioDevice);
    } catch (const std::exception& e) {
        std::cerr << "Audio Error: " << e.what() << std::endl;
        quit.store(true);
    }
}

void toggle_pause() {
    if (target_playback_rate.load() == 0.0) {
        set_target_playback_rate(1.0);  // или последняя активная скорость
        std::cout << "Воспроизведение" << std::endl;
    } else {
        set_target_playback_rate(0.0);
        std::cout << "Пауза" << std::endl;
    }
}

std::string format_time(double time_in_seconds, double fps) {
    int total_frames = static_cast<int>(std::round(time_in_seconds * fps));
    int hours = total_frames / (3600 * static_cast<int>(std::round(fps)));
    int minutes = (total_frames / (60 * static_cast<int>(std::round(fps)))) % 60;
    int seconds = (total_frames / static_cast<int>(std::round(fps))) % 60;
    int frames = total_frames % static_cast<int>(std::round(fps));

    std::ostringstream oss;
    oss << std::setw(2) << std::setfill('0') << hours << ":"
        << std::setw(2) << std::setfill('0') << minutes << ":"
        << std::setw(2) << std::setfill('0') << seconds << ":"
        << std::setw(2) << std::setfill('0') << frames;
    return oss.str();
}

gint64 get_total_duration() {
    gint64 duration = static_cast<gint64>(total_duration.load() * GST_SECOND);
    std::cout << "get_total_duration called, returning: " << duration << " nanoseconds" << std::endl;
    return duration;
}

double get_current_audio_time() {
    return current_audio_time.load();
}

double get_original_fps() {
    return original_fps.load();
}

std::string generateTXTimecode(double time) {
    double fps = original_fps.load();
    int total_frames = static_cast<int>(std::round(time * fps));
    int hours = total_frames / (3600 * static_cast<int>(std::round(fps)));
    int minutes = (total_frames / (60 * static_cast<int>(std::round(fps)))) % 60;
    int seconds = (total_frames / static_cast<int>(std::round(fps))) % 60;
    int frames = total_frames % static_cast<int>(std::round(fps));

    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(2) << hours << ":"
        << std::setfill('0') << std::setw(2) << minutes << ":"
        << std::setfill('0') << std::setw(2) << seconds << ":"
        << std::setfill('0') << std::setw(2) << frames;
    return oss.str();
}

double get_video_fps(const char* filename) {
    AVFormatContext* format_ctx = avformat_alloc_context();
    if (avformat_open_input(&format_ctx, filename, nullptr, nullptr) != 0) {
        return 25.0; // Возвращаем значение по умолчанию в случае ошибки
    }

    double fps = 25.0; // Значение по умолчанию
    for (unsigned int i = 0; i < format_ctx->nb_streams; i++) {
        if (format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            AVRational frame_rate = av_guess_frame_rate(format_ctx, format_ctx->streams[i], NULL);
            if (frame_rate.num && frame_rate.den) {
                fps = av_q2d(frame_rate);
                // Обработка специальных случаев
                if (std::abs(fps - 29.97) < 0.01) fps = 29.97;
                if (std::abs(fps - 59.94) < 0.01) fps = 59.94;
                break;
            }
        }
    }

    avformat_close_input(&format_ctx);
    return fps;
}

double get_file_duration(const char* filename) {
    AVFormatContext* format_ctx = avformat_alloc_context();
    double duration = 0.0;

    if (avformat_open_input(&format_ctx, filename, nullptr, nullptr) == 0) {
        if (avformat_find_stream_info(format_ctx, nullptr) >= 0) {
            duration = static_cast<double>(format_ctx->duration) / AV_TIME_BASE;
        }
        avformat_close_input(&format_ctx);
    }

    return duration;
}

void smooth_speed_change() {
    const double normal_step = 0.3;  // Максимальный шаг изменения скорости
    const double pause_step = 0.8;   // Шаг для быстрой остановки
    const int normal_interval = 4;  // Обычный интервал обновления в миллисекундах
    const int pause_interval = 6;    // Интервал для быстрой остановки

    while (!quit.load()) {
        double current = playback_rate.load();
        double target = target_playback_rate.load();

        // Определяем, входим ли мы в режим паузы
        bool is_pausing = (target == 0.0 && current > 0.0);

        double max_step = is_pausing ? pause_step : normal_step;
        int interval = is_pausing ? pause_interval : normal_interval;

        if (current != target) {
            // Вычисляем интерполированный шаг
            double diff = target - current;
            double interpolated_step = std::min(max_step, std::abs(diff) * 0.1);
            
            if (std::abs(diff) < interpolated_step) {
                playback_rate.store(target);
            } else if (diff > 0) {
                playback_rate.store(current + interpolated_step);
            } else {
                playback_rate.store(current - interpolated_step);
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(interval));
    }
}

// Функция для установки целевой скорости воспроизведения
void set_target_playback_rate(double rate) {
    target_playback_rate.store(rate);
}

void increase_volume() {
    float current = volume.load();
    float new_volume = std::min(current + 0.1f, 1.0f);
    volume.store(new_volume);
    std::cout << "Volume: " << static_cast<int>(new_volume * 100) << "%" << std::endl;
}

void decrease_volume() {
    float current = volume.load();
    float new_volume = std::max(current - 0.1f, 0.0f);
    volume.store(new_volume);
    std::cout << "Volume: " << static_cast<int>(new_volume * 100) << "%" << std::endl;
}

double get_precise_audio_time() {
    return current_audio_time.load();
}
