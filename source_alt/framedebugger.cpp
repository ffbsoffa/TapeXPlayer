#include <iostream>
#include <vector>
#include <memory>
#include <chrono>
#include <thread>
#include <mutex>
#include <atomic>
#include <future>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <cstdlib>
#include <sys/resource.h>
#include "decode.h"
#include "common.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

std::mutex cout_mutex;
std::atomic<bool> quit(false);
std::atomic<double> current_audio_time(0.0);

std::atomic<double> playback_rate(1.0);
std::atomic<double> target_playback_rate(1.0);
std::atomic<bool> is_reverse(false);
std::atomic<bool> is_seeking(false);

std::atomic<double> total_duration(0.0);
std::atomic<double> original_fps(0.0);
std::atomic<bool> shouldExit(false);
std::atomic<float> volume(1.0f);

// Объявление функции visualizeFrameIndex
void visualizeFrameIndex(const std::vector<FrameInfo>& frameIndex);

// Добавьте это в начало файла, после других глобальных переменных
bool showIndex = false;
bool showOSD = true;

std::atomic<double> previous_playback_rate(1.0);

void updateVisualization(SDL_Renderer* renderer, const std::vector<FrameInfo>& frameIndex, int currentFrame, int bufferStart, int bufferEnd, int highResStart, int highResEnd, bool enableHighResDecode) {
    if (!showIndex) return; // Если индекс не должен отображаться, выходим из функции

    int totalFrames = frameIndex.size();
    int windowWidth, windowHeight;
    SDL_GetRendererOutputSize(renderer, &windowWidth, &windowHeight);
    
    int indexHeight = 5; // Уменьшаем высоту полосы индекса до 5 пикселей
    double frameWidth = static_cast<double>(windowWidth) / totalFrames;

    // Очищаем только вехнюю часть экран для индекса
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_Rect indexRect = {0, 0, windowWidth, indexHeight};
    SDL_RenderFillRect(renderer, &indexRect);

    // Отрисовка индекса кадров
    for (int i = 0; i < totalFrames; ++i) {
        SDL_Rect rect;
        rect.x = static_cast<int>(i * frameWidth);
        rect.y = 0; // Рисуем в верхней части экрана
        rect.w = std::max(1, static_cast<int>(frameWidth));
        rect.h = indexHeight;

        if (enableHighResDecode && i >= highResStart && i <= highResEnd) {
            SDL_SetRenderDrawColor(renderer, 0, 255, 0, 255); // Зеленый для кадров высокого разрешения
        } else if (i >= bufferStart && i <= bufferEnd) {
            switch (frameIndex[i].type) {
                case FrameInfo::EMPTY:
                    SDL_SetRenderDrawColor(renderer, 64, 64, 64, 255); // Темно-серый для пустых кадров
                    break;
                case FrameInfo::LOW_RES:
                    SDL_SetRenderDrawColor(renderer, 0, 128, 255, 255); // Голубой для низкого разрешения
                    break;
                case FrameInfo::FULL_RES:
                    SDL_SetRenderDrawColor(renderer, 0, 255, 0, 255); // Зеленый для высокого разрешения
                    break;
            }
        } else {
            SDL_SetRenderDrawColor(renderer, 32, 32, 32, 255); // Очень темно-серый для кадров вне буфера
        }

        SDL_RenderFillRect(renderer, &rect);
    }

    // Отрисовка текущего кадра (воспроизводящей головки)
    SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255); // Красный цвет для головки
    SDL_Rect currentFrameRect = {
        static_cast<int>(currentFrame * frameWidth),
        0,
        std::max(2, static_cast<int>(frameWidth)), // Минимальная ширина 2 пикселя для виимости
        indexHeight
    };
    SDL_RenderFillRect(renderer, &currentFrameRect);
}

void printMemoryUsage() {
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        std::cout << "Использование памяти: " << usage.ru_maxrss << " KB" << std::endl;
    }
}

void displayCurrentFrame(SDL_Renderer* renderer, const FrameInfo& frameInfo, bool enableHighResDecode) {
    std::lock_guard<std::mutex> lock(frameInfo.mutex);
    if (frameInfo.is_decoding) {
        // Если кадр все еще декодируется, пропускаем его отображение
        return;
    }

    if ((!enableHighResDecode && !frameInfo.low_res_frame) || (enableHighResDecode && !frameInfo.frame && !frameInfo.low_res_frame)) {
        std::cout << "Кадр отсутствует" << std::endl;
        return;
    }

    AVFrame* frame = (enableHighResDecode && frameInfo.frame) ? frameInfo.frame.get() : frameInfo.low_res_frame.get();
    
    if (!frame) {
        std::cout << "Ошибка: frame is null" << std::endl;
        return;
    }

    // Создаем текстру из AVFrame
    SDL_Texture* texture = SDL_CreateTexture(
        renderer,
        SDL_PIXELFORMAT_YV12,
        SDL_TEXTUREACCESS_STREAMING,
        frame->width,
        frame->height
    );

    if (!texture) {
        std::cout << "Ошибка создания текстуры: " << SDL_GetError() << std::endl;
        return;
    }

    SDL_UpdateYUVTexture(
        texture,
        NULL,
        frame->data[0],
        frame->linesize[0],
        frame->data[1],
        frame->linesize[1],
        frame->data[2],
        frame->linesize[2]
    );

    // Отображаем текстуру, сохраняя соотношение сторон
    int windowWidth, windowHeight;
    SDL_GetRendererOutputSize(renderer, &windowWidth, &windowHeight);
    
    float videoAspectRatio = static_cast<float>(frame->width) / frame->height;
    float windowAspectRatio = static_cast<float>(windowWidth) / windowHeight;
    
    SDL_Rect dstRect;
    if (videoAspectRatio > windowAspectRatio) {
        dstRect.w = windowWidth;
        dstRect.h = static_cast<int>(windowWidth / videoAspectRatio);
        dstRect.x = 0;
        dstRect.y = (windowHeight - dstRect.h) / 2;
    } else {
        dstRect.h = windowHeight;
        dstRect.w = static_cast<int>(windowHeight * videoAspectRatio);
        dstRect.y = 0;
        dstRect.x = (windowWidth - dstRect.w) / 2;
    }
    
    SDL_RenderCopy(renderer, texture, NULL, &dstRect);
    SDL_DestroyTexture(texture);

    // std::cout << "Отображен кадр: " << ((enableHighResDecode && frameInfo.frame) ? "высокое разрешение" : "низкое разрешение") << std::endl;
}

// Объявления функций из mainau.cpp
void start_audio(const char* filename);
std::string generateTXTimecode(double time);

void smooth_speed_change();

// Глобальные переменные для управления воспроизведением
std::atomic<bool> audio_initialized(false);
std::thread audio_thread;
std::thread speed_change_thread;

void get_video_dimensions(const char* filename, int* width, int* height) {
    AVFormatContext* formatContext = avformat_alloc_context();
    if (avformat_open_input(&formatContext, filename, NULL, NULL) != 0) {
        std::cerr << "Не удалось открыть файл" << std::endl;
        return;
    }
    
    if (avformat_find_stream_info(formatContext, NULL) < 0) {
        std::cerr << "Не удалось найти информацию о потоках" << std::endl;
        avformat_close_input(&formatContext);
        return;
    }
    
    int videoStream = -1;
    for (unsigned int i = 0; i < formatContext->nb_streams; i++) {
        if (formatContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoStream = i;
            break;
        }
    }
    
    if (videoStream == -1) {
        std::cerr << "Не удалось найти видеопоток" << std::endl;
        avformat_close_input(&formatContext);
        return;
    }
    
    *width = formatContext->streams[videoStream]->codecpar->width;
    *height = formatContext->streams[videoStream]->codecpar->height;
    
    avformat_close_input(&formatContext);
}

// Добавьте эти включения в начало файла
#include <sstream>
#include <iomanip>

// Добавьте эту функцию перед main()
void renderOSD(SDL_Renderer* renderer, TTF_Font* font, bool isPlaying, double playbackRate, bool isReverse, double currentTime, int frameNumber) {
    if (!showOSD) return;

    int windowWidth, windowHeight;
    SDL_GetRendererOutputSize(renderer, &windowWidth, &windowHeight);

    // Создаем черный фон для OSD
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_Rect osdRect = {0, windowHeight - 30, windowWidth, 30};
    SDL_RenderFillRect(renderer, &osdRect);

    // Подготавливаем текст для левой части
    std::string leftText;
    if (jog_forward.load() || jog_backward.load()) {
        leftText = "JOG";
    } else if (std::abs(playbackRate) < 0.01) {
        leftText = "STILL";
    } else if (std::abs(playbackRate) > 1.0) {
        leftText = "SHUTTLE";
    } else if (std::abs(playbackRate) > 0.0) {
        leftText = "PLAY";
    } else {
        leftText = "STILL";
    }

    // Подготавливаем таймкод
    int hours = static_cast<int>(currentTime / 3600);
    int minutes = static_cast<int>((currentTime - hours * 3600) / 60);
    int seconds = static_cast<int>(currentTime) % 60;
    int frames = static_cast<int>((currentTime - static_cast<int>(currentTime)) * original_fps.load());

    std::stringstream timecodeStream;
    timecodeStream << std::setfill('0') << std::setw(2) << hours << ":"
                   << std::setfill('0') << std::setw(2) << minutes << ":"
                   << std::setfill('0') << std::setw(2) << seconds << ":"
                   << std::setfill('0') << std::setw(2) << frames;
    std::string timecode = timecodeStream.str();

    // Подготавливаем текст для правой части
    std::string rightText = isReverse ? "REV" : "FWD";
    if (jog_forward.load() || jog_backward.load()) {
    } else if (std::abs(playbackRate) > 1.0) {
        int roundedSpeed = std::round(std::abs(playbackRate));
        rightText += " " + std::to_string(roundedSpeed) + "x";
    }

    // Отрисовываем текст
    SDL_Color textColor = {255, 255, 255, 255};  // Белый цвет

    SDL_Surface* leftSurface = TTF_RenderText_Solid(font, leftText.c_str(), textColor);
    SDL_Texture* leftTexture = SDL_CreateTextureFromSurface(renderer, leftSurface);
    SDL_Rect leftRect = {10, windowHeight - 30 + (30 - leftSurface->h) / 2, leftSurface->w, leftSurface->h};
    SDL_RenderCopy(renderer, leftTexture, NULL, &leftRect);

    SDL_Surface* timeSurface = TTF_RenderText_Solid(font, timecode.c_str(), textColor);
    SDL_Texture* timeTexture = SDL_CreateTextureFromSurface(renderer, timeSurface);
    SDL_Rect timeRect = {(windowWidth - timeSurface->w) / 2, windowHeight - 30 + (30 - timeSurface->h) / 2, timeSurface->w, timeSurface->h};
    SDL_RenderCopy(renderer, timeTexture, NULL, &timeRect);

    SDL_Surface* rightSurface = TTF_RenderText_Solid(font, rightText.c_str(), textColor);
    SDL_Texture* rightTexture = SDL_CreateTextureFromSurface(renderer, rightSurface);
    SDL_Rect rightRect = {windowWidth - rightSurface->w - 10, windowHeight - 30 + (30 - rightSurface->h) / 2, rightSurface->w, rightSurface->h};
    SDL_RenderCopy(renderer, rightTexture, NULL, &rightRect);

    // Освобождаем ресурсы
    SDL_FreeSurface(leftSurface);
    SDL_DestroyTexture(leftTexture);
    SDL_FreeSurface(timeSurface);
    SDL_DestroyTexture(timeTexture);
    SDL_FreeSurface(rightSurface);
    SDL_DestroyTexture(rightTexture);
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Использование: " << argv[0] << " <имя_видеофайла>" << std::endl;
        return 1;
    }

    const char* filename = argv[1];
    const char* lowResFilename = "low_res_output.mp4";

    std::vector<FrameInfo> frameIndex = createFrameIndex(filename);
    std::cout << "Создан индекс кадров. Всего кадров: " << frameIndex.size() << std::endl;

    if (!convertToLowRes(filename, lowResFilename)) {
        std::cerr << "Ошибка при конвертации видео в низкое разрешение" << std::endl;
        return 1;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        std::cerr << "Ошибка инициализации SDL: " << SDL_GetError() << std::endl;
        return 1;
    }

    // Инициализация TTF
    if (TTF_Init() == -1) {
        std::cerr << "Ошибка инициализации SDL_ttf: " << TTF_GetError() << std::endl;
        return 1;
    }

    // Загрузка шрифта
    TTF_Font* font = TTF_OpenFont("font.otf", 24);
    if (!font) {
        std::cerr << "Ошибка загрузки шрифта: " << TTF_GetError() << std::endl;
        return 1;
    }

    std::future<double> fps_future = std::async(std::launch::async, get_video_fps, filename);
    std::future<double> duration_future = std::async(std::launch::async, get_file_duration, filename);

    // Получаем размеры видео
    int videoWidth, videoHeight;
    get_video_dimensions(filename, &videoWidth, &videoHeight);

    SDL_Window* window = SDL_CreateWindow("TapeXPlayer", 
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 
        videoWidth, videoHeight, 
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!window) {
        std::cerr << "Ошибка создания окна: " << SDL_GetError() << std::endl;
        SDL_Quit();
        return 1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        std::cerr << "Ошибка создания рендерера с VSync: " << SDL_GetError() << std::endl;
        // Попробуем создать рендерер без VSync
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
        if (!renderer) {
            std::cerr << "Ошибка создания рендерера без VSync: " << SDL_GetError() << std::endl;
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 1;
        }
        std::cout << "Рендерер создан без VSync" << std::endl;
    } else {
        std::cout << "Рендерер создан с VSync" << std::endl;
    }

    const size_t ringBufferCapacity = 1000;  // Фиксированный размер буфера
    RingBuffer ringBuffer(ringBufferCapacity);
    FrameCleaner frameCleaner(frameIndex);

    int currentFrame = 0;
    bool isPlaying = false;
    bool enableHighResDecode = true;
    std::chrono::steady_clock::time_point lastFrameTime;
    std::chrono::steady_clock::time_point lastBufferUpdateTime = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point now;

    const int highResWindowSize = 50;  // Размер окна высокого разрешения
    std::future<void> lowResFuture;
    std::future<void> highResFuture;
    std::future<void> secondaryCleanFuture;

    const int TARGET_FPS = 60;
    const int FRAME_DELAY = 1000 / TARGET_FPS;

    // Инициализация аудио
    audio_thread = std::thread(start_audio, filename);
    speed_change_thread = std::thread(smooth_speed_change);

    bool fps_received = false;
    bool duration_received = false;

    while (!shouldExit) {
        // Проверяем, готово ли значение FPS
        if (!fps_received && fps_future.valid()) {
            std::future_status status = fps_future.wait_for(std::chrono::seconds(0));
            if (status == std::future_status::ready) {
                original_fps.store(fps_future.get());
                std::cout << "Video FPS: " << original_fps.load() << std::endl;
                fps_received = true;
            }
        }

        // Проверяем, готова ли длительность
        if (!duration_received && duration_future.valid()) {
            std::future_status status = duration_future.wait_for(std::chrono::seconds(0));
            if (status == std::future_status::ready) {
                total_duration.store(duration_future.get());
                std::cout << "Total duration: " << total_duration.load() << " seconds" << std::endl;
                duration_received = true;
            }
        }

        Uint32 frameStart = SDL_GetTicks();

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                quit = true;
                shouldExit = true;
            } else if (e.type == SDL_KEYDOWN) {
                switch (e.key.keysym.sym) {
                    case SDLK_SPACE:
                            // Обычная пауза/воспроизведение
                            toggle_pause();
                        break;
                    case SDLK_RIGHT:
                        if (e.key.keysym.mod & KMOD_SHIFT) {
                            if (e.key.repeat == 0) { // Проверяем, что это не повторное событие
                                start_jog_forward();
                            }
                        } else {
                            // Обычное поведение стрелки вправо
                            target_playback_rate.store(1.0);
                            is_reverse.store(false);
                        }
                        break;
                    case SDLK_LEFT:
                        if (e.key.keysym.mod & KMOD_SHIFT) {
                            if (e.key.repeat == 0) {
                                start_jog_backward();
                            }
                        } else {
                            // Обычное поведение стрелки влево
                            is_reverse.store(!is_reverse.load());
                            std::cout << "Playback direction: " << (is_reverse.load() ? "Reverse" : "Forward") << std::endl;
                        }
                        break;
                    case SDLK_UP:
                        target_playback_rate.store(std::min(target_playback_rate.load() * 2.0, 16.0));
                        break;
                    case SDLK_DOWN:
                        target_playback_rate.store(std::max(target_playback_rate.load() / 2.0, 0.125));
                        break;
                    case SDLK_r:
                        is_reverse.store(!is_reverse.load());
                        std::cout << "Playback direction: " << (is_reverse.load() ? "Reverse" : "Forward") << std::endl;
                        break;
                    case SDLK_ESCAPE:
                        shouldExit = true;
                        break;
                    case SDLK_PLUS:
                    case SDLK_EQUALS:
                        increase_volume();
                        break;
                    case SDLK_MINUS:
                        decrease_volume();
                        break;
                    case SDLK_d:
                        if (e.key.keysym.mod & KMOD_ALT) {
                            showOSD = !showOSD;
                        } else if (SDL_GetModState() & KMOD_SHIFT) {
                            showIndex = !showIndex;
                            std::cout << "Отображение индекса: " << (showIndex ? "включено" : "выключено") << std::endl;
                        }
                        break;
                }
            } else if (e.type == SDL_KEYUP) {
                switch (e.key.keysym.sym) {
                    case SDLK_RIGHT:
                    case SDLK_LEFT:
                        if (e.key.keysym.mod & KMOD_SHIFT) {
                            stop_jog();
                        }
                        break;
                }
            } else if (e.type == SDL_WINDOWEVENT) {
                if (e.window.event == SDL_WINDOWEVENT_RESIZED) {
                    int windowWidth, windowHeight;
                    SDL_GetWindowSize(window, &windowWidth, &windowHeight);
                }
            }
        }

        // Обновление буфера
        int bufferStart = std::max(0, currentFrame - static_cast<int>(ringBufferCapacity / 2));
        int bufferEnd = std::min(bufferStart + static_cast<int>(ringBufferCapacity) - 1, static_cast<int>(frameIndex.size()) - 1);

        // Определение границ окна высокого разрешения
        int highResStart = std::max(0, currentFrame - highResWindowSize / 2);
        int highResEnd = std::min(static_cast<int>(frameIndex.size()) - 1, currentFrame + highResWindowSize / 2);

        now = std::chrono::steady_clock::now();
        auto timeSinceLastBufferUpdate = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastBufferUpdateTime);

        // Асинхронное заполнение буфера кадрами низкого разрешения каждые 25 мс
        if (timeSinceLastBufferUpdate.count() >= 25) {
            if (!lowResFuture.valid() || lowResFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                lowResFuture = asyncDecodeLowResRange(lowResFilename, frameIndex, bufferStart, bufferEnd, highResStart, highResEnd);
                lastBufferUpdateTime = now;
            }
        }

        // Отключение декодирования кадров высокого разрешения, если скорость воспроизведения больше 1x
        enableHighResDecode = target_playback_rate.load() <= 1.0;

        if (enableHighResDecode) {
            if (!highResFuture.valid() || highResFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                highResFuture = asyncDecodeFrameRange(filename, frameIndex, highResStart, highResEnd);
            }

            // Удаляем кадры высокого разрешения за пределами окна
            removeHighResFrames(frameIndex, 0, highResStart - 1);
            removeHighResFrames(frameIndex, highResEnd + 1, frameIndex.size() - 1);
        } else {
            // Если декодирование высокого разрешения отключено, удаляем все кадры высокого разрешения
            if (highResFuture.valid()) {
                highResFuture.wait();
            }
            removeHighResFrames(frameIndex, 0, frameIndex.size() - 1);
        }

        // Добавим проверку и сброс типа кадра, если он находится вне окна высокого разрешения
        for (int i = 0; i < frameIndex.size(); ++i) {
            if (i < highResStart || i > highResEnd) {
                if (frameIndex[i].type == FrameInfo::FULL_RES && !frameIndex[i].is_decoding) {
                    frameIndex[i].frame.reset();
                    frameIndex[i].type = frameIndex[i].low_res_frame ? FrameInfo::LOW_RES : FrameInfo::EMPTY;
                }
            }
        }

        // Очистка кадров низкого разрешения за пределами буфера
        for (int i = 0; i < frameIndex.size(); ++i) {
            if (i < bufferStart || i > bufferEnd) {
                if (frameIndex[i].type == FrameInfo::LOW_RES && !frameIndex[i].is_decoding) {
                    frameIndex[i].low_res_frame.reset();
                    frameIndex[i].type = FrameInfo::EMPTY;
                }
            }
        }

        // Обновление текущего кадра на основе текущего времени аудио
        double currentTime = current_audio_time.load();
        currentFrame = static_cast<int>(currentTime * original_fps.load()) % frameIndex.size();

        // Очистка всего экрана
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);

        // Отображение текущего кадра
        displayCurrentFrame(renderer, frameIndex[currentFrame], enableHighResDecode);

        // Обновление визуализации индекса кадров
        if (showIndex) {
            updateVisualization(renderer, frameIndex, currentFrame, bufferStart, bufferEnd, highResStart, highResEnd, enableHighResDecode);
        }

        // Отрисовка OSD
        if (showOSD) {
            renderOSD(renderer, font, isPlaying, playback_rate.load(), is_reverse.load(), currentTime, currentFrame);
        }

        SDL_RenderPresent(renderer);

        // printMemoryUsage();

        if (isPlaying) {
            now = std::chrono::steady_clock::now();
            auto frameDuration = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastFrameTime);
            double fps = 1000.0 / frameDuration.count();
            // std::cout << "Воспроизведение: Да" << std::endl;
            // std::cout << "FPS: " << fps << std::endl;
        } else {
            // std::cout << "Воспроизведение: Нет" << std::endl;
        }

        // Ограничение частоты кадров
        Uint32 frameTime = SDL_GetTicks() - frameStart;
        if (frameTime < FRAME_DELAY) {
            SDL_Delay(FRAME_DELAY - frameTime);
        }
    }

    // Завершение работы
    shouldExit = true;

    // Завершаем асинхронные задачи
    if (lowResFuture.valid()) {
        lowResFuture.wait();
    }
    if (highResFuture.valid()) {
        highResFuture.wait();
    }
    if (secondaryCleanFuture.valid()) {
        secondaryCleanFuture.wait();
    }

    // Завершаем потоки
    if (audio_thread.joinable()) {
        audio_thread.join();
    }
    if (speed_change_thread.joinable()) {
        speed_change_thread.join();
    }

    // Освобождение ресурсов TTF
    TTF_CloseFont(font);
    TTF_Quit();

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}