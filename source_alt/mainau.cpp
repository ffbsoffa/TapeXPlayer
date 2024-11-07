#include "common.h"
extern "C" {
    #include <libavcodec/avcodec.h>
    #include <libavformat/avformat.h>
    #include <libavutil/opt.h>
    #include <libavutil/channel_layout.h>
}
#include <portaudio.h>
#include <iostream>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <gst/gst.h>
#include "common.h"
#include <random>
#include <nfd.hpp>

std::vector<float> audio_buffer;
size_t audio_buffer_index = 0;
std::atomic<bool> decoding_finished{false};
std::atomic<bool> decoding_completed{false};

extern std::atomic<bool> quit;
extern std::atomic<double> playback_rate;
extern std::atomic<bool> is_reverse;
extern std::atomic<double> current_audio_time;
extern std::atomic<double> total_duration;
extern std::atomic<double> original_fps;

std::atomic<bool> jog_forward(false);
std::atomic<bool> jog_backward(false);
const double JOG_SPEED = 0.25;

std::atomic<int> sample_rate{44100};

PaStream* stream = nullptr;

void smooth_speed_change() {
    const double normal_step = 0.3;
    const double pause_step = 1;
    const int normal_interval = 4;
    const int pause_interval = 4;
    const double overshoot_speed = 1.2;
    const int overshoot_duration = 175; 
    static int resume_count = 0;
    static int overshoot_interval = 0;
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(7, 10);

    while (!quit.load()) {
        double current = playback_rate.load();
        double target = target_playback_rate.load();
        float current_volume = volume.load();

        if (current <= 0.15) {
            float volume_multiplier = current / 0.15f;
            volume.store(current_volume * volume_multiplier);
        } else if (current > 0.15 && current_volume < 1.0f) {
            volume.store(1.0f);
        }

        if (current >= 7.0) {
            float volume_multiplier;
            if (current >= 18.0) {
                volume_multiplier = 0.15f;
            } else {
                float t = (current - 7.0f) / (18.0f - 7.0f);
                volume_multiplier = 1.0f - (t * 0.85f); 
            }
            volume.store(volume_multiplier);
        }

        bool is_pausing = (target == 0.0 && current > 0.0);
        bool is_resuming = (current == 0.0 && target > 0.0);
        bool is_jogging = jog_forward.load() || jog_backward.load();

        double max_step = is_pausing ? pause_step : (is_jogging ? JOG_SPEED : normal_step);
        int interval = is_pausing ? pause_interval : normal_interval;

        if (is_resuming) {
            resume_count++;
            bool do_overshoot = (resume_count >= overshoot_interval);

            if (do_overshoot) {
                // Реализация краткой секвенции при снятии паузы
                auto start_time = std::chrono::steady_clock::now();
                while (std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start_time).count() < overshoot_duration) {
                    double progress = static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start_time).count()) / overshoot_duration;
                    
                    if (progress < 0.7) {
                        // Ускорение до overshoot_speed за первые 70% времени
                        playback_rate.store(progress * overshoot_speed / 0.7);
                    } else {
                        // Замедление до 1x за оставшиеся 30% времени
                        playback_rate.store(overshoot_speed + (progress - 0.7) * (1.0 - overshoot_speed) / 0.3);
                    }
                    
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                resume_count = 0;
                overshoot_interval = dis(gen); 
            } else {
                double step = 0.1;
                while (current < 1.0) {
                    current += step;
                    if (current > 1.0) current = 1.0;
                    playback_rate.store(current);
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
            playback_rate.store(1.0);
        } else if (current != target || is_jogging) {
            double diff = target - current;
            double interpolated_step = std::min(max_step, std::abs(diff) * 0.1);
            
            if (is_jogging) {
                playback_rate.store(JOG_SPEED);
            } else if (std::abs(diff) < interpolated_step) {
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

void toggle_pause() {
    if (target_playback_rate.load() == 0.0) {
        target_playback_rate.store(1.0);
    } else {
        target_playback_rate.store(0.0);
    }
}

void check_audio_stream(AVFormatContext* format_ctx, int audio_stream_index) {
    AVStream* audio_stream = format_ctx->streams[audio_stream_index];
    AVCodecParameters* codecpar = audio_stream->codecpar;

    std::cout << "Audio stream details:" << std::endl;
    std::cout << "  Codec: " << avcodec_get_name(codecpar->codec_id) << std::endl;
    std::cout << "  Sample rate: " << codecpar->sample_rate << " Hz" << std::endl;
    std::cout << "  Channels: " << codecpar->ch_layout.nb_channels << std::endl;
    std::cout << "  Bit rate: " << codecpar->bit_rate << " bps" << std::endl;
    std::cout << "  Duration: " << av_rescale_q(audio_stream->duration, audio_stream->time_base, {1, AV_TIME_BASE}) / 1000000.0 << " seconds" << std::endl;
}

static int patestCallback(const void *inputBuffer, void *outputBuffer,
                          unsigned long framesPerBuffer,
                          const PaStreamCallbackTimeInfo* timeInfo,
                          PaStreamCallbackFlags statusFlags,
                          void *userData) {
    float *out = (float*)outputBuffer;
    (void) inputBuffer; 

    double rate = playback_rate.load();
    if (rate == 0.0) {
        for (unsigned int i = 0; i < framesPerBuffer * 2; i++) {
            *out++ = 0.0f;
        }
        return paContinue;
    }

    float current_volume = volume.load();
    double position = audio_buffer_index;
    const int window_size = 4;

    for (unsigned int i = 0; i < framesPerBuffer; i++) {
        if (is_reverse.load()) {
            position = std::max(0.0, position - rate);
        } else {
            position = std::min(static_cast<double>(audio_buffer.size() - window_size), position + rate);
        }

        if (position < 0 || position >= audio_buffer.size() - window_size) {
            *out++ = 0.0f;
            *out++ = 0.0f;
        } else {
            float sum_left = 0.0f;
            float sum_right = 0.0f;
            for (int j = 0; j < window_size; ++j) {
                size_t index = static_cast<size_t>(position) + j;
                float weight = 1.0f - std::abs(j - (window_size / 2.0f - 0.5f)) / (window_size / 2.0f);
                sum_left += audio_buffer[index * 2] * weight;
                sum_right += audio_buffer[index * 2 + 1] * weight;
            }
            *out++ = (sum_left / window_size) * current_volume;
            *out++ = (sum_right / window_size) * current_volume;
        }
    }

    audio_buffer_index = static_cast<size_t>(position);


    double time_per_sample = 1.0 / sample_rate.load();
    double elapsed_time = audio_buffer_index * time_per_sample;
    double total_time = (audio_buffer.size() / 2) * time_per_sample; 
    double adjusted_total_time = total_time - 0.1; 

    elapsed_time = std::max(0.0, std::min(elapsed_time, adjusted_total_time));

    current_audio_time.store(elapsed_time, std::memory_order_release);

    return paContinue;
}

void decode_audio(const char* filename) {
    AVFormatContext* format_ctx = nullptr;
    AVCodecContext* audio_codec_ctx = nullptr;
    const AVCodec* audio_codec = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = av_packet_alloc();
    int audio_stream_index = -1;

    try {
        std::cout << "Starting audio decoding..." << std::endl;
        std::cout << "FFmpeg version: " << av_version_info() << std::endl;

        if (avformat_open_input(&format_ctx, filename, nullptr, nullptr) != 0) {
            throw std::runtime_error("Could not open file");
        }
        if (avformat_find_stream_info(format_ctx, nullptr) < 0) {
            throw std::runtime_error("Could not find stream information");
        }

        for (unsigned int i = 0; i < format_ctx->nb_streams; i++) {
            std::cout << "Stream #" << i << " type: " 
                      << av_get_media_type_string(format_ctx->streams[i]->codecpar->codec_type) 
                      << ", codec: " << avcodec_get_name(format_ctx->streams[i]->codecpar->codec_id) << std::endl;
        }

        audio_stream_index = av_find_best_stream(format_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, &audio_codec, 0);
        if (audio_stream_index < 0) {
            throw std::runtime_error("Could not find audio stream");
        }

        std::cout << "Audio stream index: " << audio_stream_index << std::endl;
        std::cout << "Audio codec: " << avcodec_get_name(audio_codec->id) << std::endl;

        check_audio_stream(format_ctx, audio_stream_index);

        audio_codec_ctx = avcodec_alloc_context3(audio_codec);
        if (!audio_codec_ctx) {
            throw std::runtime_error("Could not allocate audio codec context");
        }

        if (avcodec_parameters_to_context(audio_codec_ctx, format_ctx->streams[audio_stream_index]->codecpar) < 0) {
            throw std::runtime_error("Could not set codec parameters");
        }

        if (avcodec_open2(audio_codec_ctx, audio_codec, nullptr) < 0) {
            throw std::runtime_error("Could not open audio codec");
        }

        frame = av_frame_alloc();
        if (!frame) {
            throw std::runtime_error("Could not allocate frame");
        }

        std::cout << "Audio decoding setup complete, starting to read frames..." << std::endl;

        sample_rate.store(audio_codec_ctx->sample_rate);
        std::cout << "Audio sample rate: " << sample_rate.load() << " Hz" << std::endl;

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        int frame_count = 0;
        int packet_count = 0;
        int ret;
        while ((ret = av_read_frame(format_ctx, packet)) >= 0 && !quit.load()) {
            if (packet->stream_index == audio_stream_index) {
                packet_count++;
                ret = avcodec_send_packet(audio_codec_ctx, packet);
                if (ret < 0) {
                    std::cerr << "Error sending packet for decoding: " << av_err2str(ret) << std::endl;
                    continue;
                }

                while (ret >= 0) {
                    ret = avcodec_receive_frame(audio_codec_ctx, frame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                        break;
                    } else if (ret < 0) {
                        std::cerr << "Error during decoding: " << av_err2str(ret) << std::endl;
                        break;
                    }

                    if (audio_codec_ctx->sample_fmt == AV_SAMPLE_FMT_FLTP) {
                        for (int i = 0; i < frame->nb_samples; ++i) {
                            for (int ch = 0; ch < frame->ch_layout.nb_channels; ++ch) {
                                float sample = reinterpret_cast<float*>(frame->data[ch])[i];
                                audio_buffer.push_back(sample);
                            }
                        }
                    } else if (audio_codec_ctx->sample_fmt == AV_SAMPLE_FMT_S16P) {
                        for (int i = 0; i < frame->nb_samples; ++i) {
                            for (int ch = 0; ch < frame->ch_layout.nb_channels; ++ch) {
                                int16_t sample = reinterpret_cast<int16_t*>(frame->data[ch])[i];
                                audio_buffer.push_back(sample / 32768.0f);
                            }
                        }
                    } else if (audio_codec_ctx->sample_fmt == AV_SAMPLE_FMT_S16) {
                        int16_t* samples = reinterpret_cast<int16_t*>(frame->data[0]);
                        for (int i = 0; i < frame->nb_samples * frame->ch_layout.nb_channels; ++i) {
                            audio_buffer.push_back(samples[i] / 32768.0f);
                        }
                    } else {
                        std::cerr << "Unsupported audio format: " << av_get_sample_fmt_name(audio_codec_ctx->sample_fmt) << std::endl;
                    }

                    frame_count++;
                    if (frame_count % 1000 == 0) {
                        std::cout << "Decoded " << frame_count << " audio frames" << std::endl;
                    }
                }
            }
            av_packet_unref(packet);
            if (packet_count % 1000 == 0) {
                std::cout << "Processed " << packet_count << " packets" << std::endl;
            }
        }

        if (ret < 0 && ret != AVERROR_EOF) {
            std::cerr << "Error reading frame: " << av_err2str(ret) << std::endl;
        }

        std::cout << "Audio decoding finished. Total frames: " << frame_count << ", Total packets: " << packet_count << std::endl;
        decoding_finished.store(true);
        decoding_completed.store(true);  
    }
    catch (const std::exception& e) {
        std::cerr << "Decoding error: " << e.what() << std::endl;
        decoding_finished.store(true);
    }

    if (frame) av_frame_free(&frame);
    if (audio_codec_ctx) avcodec_free_context(&audio_codec_ctx);
    if (format_ctx) avformat_close_input(&format_ctx);
    av_packet_free(&packet);
}

void start_audio(const char* filename) {
    try {
        std::cout << "Starting audio initialization..." << std::endl;

        PaError err;
        err = Pa_Initialize();
        if (err != paNoError) {
            throw std::runtime_error("PortAudio error: " + std::string(Pa_GetErrorText(err)));
        }

        PaStream *stream;
        PaStreamParameters outputParameters;

        outputParameters.device = Pa_GetDefaultOutputDevice();
        if (outputParameters.device == paNoDevice) {
            throw std::runtime_error("No default output device.");
        }
        outputParameters.channelCount = 2;
        outputParameters.sampleFormat = paFloat32;
        outputParameters.suggestedLatency = Pa_GetDeviceInfo(outputParameters.device)->defaultLowOutputLatency;
        outputParameters.hostApiSpecificStreamInfo = NULL;

        // Начинаем декодирование аудио в отдельном потоке
        std::thread decoding_thread([filename]() {
            decode_audio(filename);
        });
        decoding_thread.detach(); 

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        err = Pa_OpenStream(
            &stream,
            NULL, 
            &outputParameters,
            sample_rate.load(), 
            256,   
            paClipOff,
            patestCallback,
            NULL); 

        if (err != paNoError) {
            throw std::runtime_error("PortAudio error: " + std::string(Pa_GetErrorText(err)));
        }

        err = Pa_StartStream(stream);
        if (err != paNoError) {
            throw std::runtime_error("PortAudio error: " + std::string(Pa_GetErrorText(err)));
        }

        std::cout << "Audio device opened successfully" << std::endl;

        audio_buffer_index = 0;
        current_audio_time.store(0.0);
        playback_rate.store(0.0);
        target_playback_rate.store(0.0);

        std::cout << "Audio playback started. Decoding continues in background." << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Audio Error: " << e.what() << std::endl;
        quit.store(true);
    }
}

std::string format_time(double time_in_seconds, int fps) {
    int hours = static_cast<int>(time_in_seconds / 3600);
    int minutes = static_cast<int>((time_in_seconds - hours * 3600) / 60);
    int seconds = static_cast<int>(time_in_seconds) % 60;
    int frames = static_cast<int>((time_in_seconds - static_cast<int>(time_in_seconds)) * fps);

    std::ostringstream oss;
    oss << std::setw(2) << std::setfill('0') << hours << ":"
        << std::setw(2) << std::setfill('0') << minutes << ":"
        << std::setw(2) << std::setfill('0') << seconds << ":"
        << std::setw(2) << std::setfill('0') << frames;
    return oss.str();
}

gint64 get_total_duration() {
    gint64 duration = static_cast<gint64>(total_duration.load() * GST_SECOND);
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
    double total_dur = total_duration.load();

    // Жестко ограничиваем время длительностью файла
    time = std::min(std::max(time, 0.0), total_dur);

    int64_t total_frames = static_cast<int64_t>(std::round(time * fps));
    int64_t max_frames = static_cast<int64_t>(std::floor(total_dur * fps));

    // Жеско ограничиваем количество кадров
    total_frames = std::min(total_frames, max_frames);

    int hours = static_cast<int>(total_frames / (3600 * fps));
    int minutes = static_cast<int>((total_frames / (60 * fps))) % 60;
    int seconds = static_cast<int>((total_frames / fps)) % 60;
    int frames = static_cast<int>(total_frames % static_cast<int64_t>(std::round(fps)));

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
        return 25.0;
    }

    double fps = 25.0;
    for (unsigned int i = 0; i < format_ctx->nb_streams; i++) {
        if (format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            AVRational frame_rate = av_guess_frame_rate(format_ctx, format_ctx->streams[i], NULL);
            if (frame_rate.num && frame_rate.den) {
                double calculated_fps = static_cast<double>(frame_rate.num) / frame_rate.den;
                
                if (std::abs(calculated_fps - 29.97) < 0.01) {
                    fps = 30000.0 / 1001.0;
                } else if (std::abs(calculated_fps - 59.94) < 0.01) {
                    fps = 60000.0 / 1001.0;
                } else {
                    fps = calculated_fps;
                }
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

void increase_volume() {
    float current = volume.load();
    float new_volume = std::min(current + 0.1f, 1.0f);
    volume.store(new_volume);
}

void decrease_volume() {
    float current = volume.load();
    float new_volume = std::max(current - 0.1f, 0.0f);
    volume.store(new_volume);
}

void start_jog_forward() {
    jog_forward.store(true);
    jog_backward.store(false);
    target_playback_rate.store(JOG_SPEED);
    is_reverse.store(false);
}

void start_jog_backward() {
    jog_backward.store(true);
    jog_forward.store(false);
    target_playback_rate.store(JOG_SPEED);
    is_reverse.store(true);
}

void stop_jog() {
    jog_forward.store(false);
    jog_backward.store(false);
    if (target_playback_rate.load() == JOG_SPEED) {
        target_playback_rate.store(0.0);
    }
}

void seek_to_time(double target_time) {
    if (target_time < 0) {
        target_time = 0;
    } else if (target_time > total_duration.load()) {
        target_time = total_duration.load();
    }

    size_t target_index = static_cast<size_t>(target_time * sample_rate.load());
    audio_buffer_index = target_index;
    current_audio_time.store(target_time);
    seek_performed.store(true);
}

double parse_timecode(const std::string& timecode) {
    std::string padded_timecode = timecode + std::string(8 - timecode.length(), '0');

    int hours = std::stoi(padded_timecode.substr(0, 2));
    int minutes = std::stoi(padded_timecode.substr(2, 2));
    int seconds = std::stoi(padded_timecode.substr(4, 2));
    int frames = std::stoi(padded_timecode.substr(6, 2));

    if (hours > 23 || minutes > 59 || seconds > 59) {
        throw std::runtime_error("Invalid timecode: hours, minutes, or seconds out of range");
    }

    double fps = original_fps.load();
    if (frames >= fps) {
        throw std::runtime_error("Invalid timecode: frames exceed FPS");
    }

    return hours * 3600.0 + minutes * 60.0 + seconds + frames / fps;
}

void cleanup_audio() {
    if (stream) {
        Pa_StopStream(stream);
        Pa_CloseStream(stream);
        stream = nullptr;
    }
    Pa_Terminate();
}
