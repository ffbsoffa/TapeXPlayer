extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

#include <SDL2/SDL.h>
#include <iostream>
#include <vector>
#include <fstream>
#include <iomanip>
#include <chrono>
#include <thread>
#include <memory>
#include <mutex>
#include <future>
#include <atomic>
#include "mainvid.h"
#include "mainau.h"

// Определение глобальных переменных, если они используются
std::atomic<bool> isDecodingActive{false};

// Объявление функций, которые используются в main, но определены в других файлах
void toggle_pause();
double get_current_audio_time();
double get_original_fps();
std::string format_time(double time, double fps);
std::atomic<bool> debug_mode(false);



bool get_video_dimensions(const char* filename, int& width, int& height) {
    AVFormatContext* formatContext = nullptr;
    if (avformat_open_input(&formatContext, filename, nullptr, nullptr) != 0) {
        return false;
    }

    if (avformat_find_stream_info(formatContext, nullptr) < 0) {
        avformat_close_input(&formatContext);
        return false;
    }

    int videoStream = av_find_best_stream(formatContext, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (videoStream < 0) {
        avformat_close_input(&formatContext);
        return false;
    }

    AVCodecParameters* codecParams = formatContext->streams[videoStream]->codecpar;
    width = codecParams->width;
    height = codecParams->height;

    avformat_close_input(&formatContext);
    return true;
}

void handleEvents(bool& running, PlaybackHead& playbackHead) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) {
            running = false;
            quit.store(true);
        } else if (event.type == SDL_KEYDOWN) {
            switch (event.key.keysym.sym) {
                case SDLK_SPACE: toggle_pause(); break;
                case SDLK_LEFT: is_reverse.store(!is_reverse.load()); break;
                case SDLK_UP: {
                    double current_rate = target_playback_rate.load();
                    set_target_playback_rate(std::min(current_rate * 2.0, 16.0));
                    break;
                }
                case SDLK_DOWN: {
                    double current_rate = target_playback_rate.load();
                    set_target_playback_rate(std::max(current_rate / 2.0, 0.25));
                    break;
                }
                case SDLK_PLUS:
                case SDLK_KP_PLUS:
                    increase_volume();
                    break;
                case SDLK_MINUS:
                case SDLK_KP_MINUS:
                    decrease_volume();
                    break;
                case SDLK_q: running = false; quit.store(true); break;
                case SDLK_d:
                    debug_mode.store(!debug_mode.load());
                    std::cout << "Debug mode: " << (debug_mode.load() ? "ON" : "OFF") << std::endl;
                    break;
            }
        }
    }
}

void updatePlaybackHead(PlaybackHead& playbackHead) {
    double currentTime = get_current_audio_time();
    int targetFrame = static_cast<int>(currentTime * get_original_fps());
    playbackHead.seekTo(targetFrame);
}

void displayInfo(int targetFrame) {
    std::cout << "\rTime: " << format_time(get_current_audio_time(), get_original_fps()) 
              << " | Speed: " << playback_rate.load() 
              << "x | Direction: " << (is_reverse.load() ? "Reverse" : "Forward") 
              << " | Frame: " << targetFrame 
              << " | " << (playback_rate.load() == 0.0 ? "Paused" : "Playing")
              << " | Debug: " << (debug_mode.load() ? "ON" : "OFF")
              << "    " << std::flush;
}

void runAudioVideoTest(const char* filename, std::vector<FrameInfo>& frameIndex, PlaybackHead& playbackHead) {
    FrameCleaner frameCleaner(frameIndex);
    std::thread audioThread(start_audio, filename);
    toggle_pause();
    std::cout << "Нажмите пробел, чтобы начать воспроизведение." << std::endl;

    std::atomic<bool> isPlaying(true);
    std::thread decodeThread([&]() {
        decodeAndClean(filename, frameIndex, playbackHead, frameCleaner, isPlaying);
    });

    bool running = true;
    while (running && !quit.load()) {
        handleEvents(running, playbackHead);
        updatePlaybackHead(playbackHead);
        playbackHead.displayCurrentFrame(); // Добавьте эту строку
        displayInfo(playbackHead.getCurrentFrame());
        SDL_Delay(16); // Примерно 60 FPS для обновления информации
    }

    isPlaying.store(false);
    decodeThread.join();
    audioThread.join();
}

void decodeAndClean(const char* filename, std::vector<FrameInfo>& frameIndex, PlaybackHead& playbackHead, FrameCleaner& frameCleaner, std::atomic<bool>& isPlaying) {
    const int bufferSize = 2000;
    const int highResWindowSize = 200;
    const int updateInterval = 100;
    const int predictionFrames = 60;

    std::atomic<bool> isHighResDecodingActive(false);
    std::future<void> highResDecodingFuture;

    while (isPlaying.load()) {
        int currentFrame = playbackHead.getCurrentFrame();
        double currentPlaybackRate = playback_rate.load();
        int predictedFrame = currentFrame + static_cast<int>(currentPlaybackRate * predictionFrames);

        int bufferStart = std::max(0, predictedFrame - bufferSize / 2);
        int bufferEnd = std::min(static_cast<int>(frameIndex.size()) - 1, predictedFrame + bufferSize / 2);

        // Асинхронное декодирование буфера кадров низкого разрешения
        auto lowResDecodingFuture = std::async(std::launch::async, [&]() {
            decodeFrameRange(filename, frameIndex, bufferStart, bufferEnd, FrameInfo::LOW_RES);
        });

        // Обработка высокого разрешения
        if (std::abs(currentPlaybackRate) <= 1.0) {
            int highResStart = std::max(bufferStart, predictedFrame - highResWindowSize / 2);
            int highResEnd = std::min(bufferEnd, predictedFrame + highResWindowSize / 2);

            // Если предыдущее высокое разрешение все еще декодируется, не начинаем новое
            if (!isHighResDecodingActive.load()) {
                isHighResDecodingActive.store(true);
                highResDecodingFuture = std::async(std::launch::async, [&]() {
                    decodeFrameRange(filename, frameIndex, highResStart, highResEnd, FrameInfo::FULL_RES);
                    isHighResDecodingActive.store(false);
                });
            }
        }

        // Ожидаем завершения декодирования низкого разрешения
        lowResDecodingFuture.wait();

        // Очистка кадров за пределами буфера
        frameCleaner.cleanFrames(0, bufferStart - 1);
        frameCleaner.cleanFrames(bufferEnd + 1, frameIndex.size() - 1);

        // Определение границ для высокого разрешения
        int highResStart = std::max(bufferStart, predictedFrame - highResWindowSize / 2);
        int highResEnd = std::min(bufferEnd, predictedFrame + highResWindowSize / 2);

        // Понижение разрешения только для кадров вне окна высокого разрешения
        if (std::abs(currentPlaybackRate) <= 1.0) {
            // Понижаем разрешение только для кадров между буфером и окном высокого разрешения
            if (bufferStart < highResStart) {
                frameCleaner.downgradeResolution(bufferStart, highResStart - 1);
            }
            if (highResEnd < bufferEnd) {
                frameCleaner.downgradeResolution(highResEnd + 1, bufferEnd);
            }
        } else {
            // При высокой скорости понижаем разрешение всех кадров в буфере, кроме текущего
            frameCleaner.downgradeResolution(bufferStart, currentFrame - 1);
            frameCleaner.downgradeResolution(currentFrame + 1, bufferEnd);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(updateInterval));
    }

    // Ожидаем завершения всех асинхронных операций перед выходом
    if (highResDecodingFuture.valid()) {
        highResDecodingFuture.wait();
    }
}
int main(int argc, char* argv[]) {
    try {
        if (argc != 2) {
            std::cerr << "Использование: " << argv[0] << " <имя_видеофайла>" << std::endl;
            return 1;
        }

        const char* filename = argv[1];

        // Инициализация SDL
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
            throw std::runtime_error("SDL не смог инициализироваться");
        }

        FrameIndexer indexer(filename);
        std::vector<FrameInfo> frameIndex = indexer.createIndex();
        std::cout << "Индекс кадров создан. Начинаем воспроизведение." << std::endl;

        // Получаем реальное разрешение видео
        int videoWidth, videoHeight;
        if (!get_video_dimensions(filename, videoWidth, videoHeight)) {
            throw std::runtime_error("Не удалось получить размеры видео");
        }

        // Создаем экземпляр SDLDisplay с реальным разрешением видео
        SDLDisplay display(videoWidth, videoHeight);

        // Создаем экземпляр PlaybackHead
        PlaybackHead playbackHead(frameIndex, 30, display, filename);

        // Устанавливаем глобальные переменные
        original_fps.store(get_video_fps(filename));
        total_duration.store(get_file_duration(filename));

        // Запускаем тест
        runAudioVideoTest(filename, frameIndex, playbackHead);

        SDL_Quit();
    } catch (const std::exception& e) {
        std::cerr << "Ошибка: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
