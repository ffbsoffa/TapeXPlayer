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
#include "fontdata.h"
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <limits.h>
#include <random>
#include <chrono>
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

// Добавьте эти строки в начало файла, после других глобальных переменных
std::string input_timecode;
bool waiting_for_timecode = false;

// Добавьте это в начало файла, после других глобальных переменных
std::atomic<bool> seek_performed(false);

void updateVisualization(SDL_Renderer* renderer, const std::vector<FrameInfo>& frameIndex, int currentFrame, int bufferStart, int bufferEnd, int highResStart, int highResEnd, bool enableHighResDecode) {
    if (!showIndex) return; // Если индекс не должен отображаться, выходим из функции

    int totalFrames = frameIndex.size();
    int windowWidth, windowHeight;
    SDL_GetRendererOutputSize(renderer, &windowWidth, &windowHeight);
    
    int indexHeight = 5; // Уменьшаем высоту полосы индекса до 5 пикселей
    double frameWidth = static_cast<double>(windowWidth) / totalFrames;

    // Очищаем только верхнюю часть экрана для индекса
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

        // Убираем проверку на высокое разрешение
        // if (enableHighResDecode && i >= highResStart && i <= highResEnd) {
        //     SDL_SetRenderDrawColor(renderer, 0, 255, 0, 255); // Зеленый для кадров высокого разрешения
        // } else 
        if (i >= bufferStart && i <= bufferEnd) {
            switch (frameIndex[i].type) {
                case FrameInfo::EMPTY:
                    SDL_SetRenderDrawColor(renderer, 64, 64, 64, 255); // Темно-серый для пустых кадров
                    break;
                case FrameInfo::LOW_RES:
                    SDL_SetRenderDrawColor(renderer, 0, 128, 255, 255); // Голубой для низкого разрешения
                    break;
                case FrameInfo::FULL_RES:
                    SDL_SetRenderDrawColor(renderer, 255, 255, 0, 255); // Желтый для высокого разрешения
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
        std::max(2, static_cast<int>(frameWidth)), // Минимальная ширина 2 пикселя для видимости
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

void displayCurrentFrame(SDL_Renderer* renderer, const FrameInfo& frameInfo, bool enableHighResDecode, double playbackRate, double currentTime, double totalDuration) {
    std::lock_guard<std::mutex> lock(frameInfo.mutex);
    if (frameInfo.is_decoding) {
        return;
    }

    if ((!enableHighResDecode && !frameInfo.low_res_frame) || (enableHighResDecode && !frameInfo.frame && !frameInfo.low_res_frame)) {
        return;
    }

    AVFrame* frame = (enableHighResDecode && frameInfo.frame) ? frameInfo.frame.get() : frameInfo.low_res_frame.get();
    
    if (!frame) {
        return;
    }

    // Создаем текстуру из AVFrame
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

    // Проверяем, нужно ли преобразовывать изображение в черно-белое
    const double threshold = 0.5; // Порог в секундах
    bool makeBlackAndWhite = std::abs(playbackRate) >= 12.0 && 
                             currentTime >= threshold && 
                             (totalDuration - currentTime) >= threshold;

    if (makeBlackAndWhite) {
        // Создаем временный буфер для Y-компоненты
        std::vector<uint8_t> y_plane(frame->linesize[0] * frame->height);
        
        // Копируем Y-компоненту
        for (int y = 0; y < frame->height; ++y) {
            std::memcpy(y_plane.data() + y * frame->linesize[0], frame->data[0] + y * frame->linesize[0], frame->width);
        }

        // Заполняем U и V компоненты средним значением (128)
        std::vector<uint8_t> uv_plane(frame->linesize[1] * frame->height / 2, 128);

        SDL_UpdateYUVTexture(
            texture,
            NULL,
            y_plane.data(),
            frame->linesize[0],
            uv_plane.data(),
            frame->linesize[1],
            uv_plane.data(),
            frame->linesize[2]
        );
    } else {
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
    }

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
    
    // Добавляем очень мягкий jitter эффект только по вертикали при повышенной скорости воспроизведения или паузе
    if (std::abs(playbackRate) > 1.0 || std::abs(playbackRate) < 0.5) {
        static std::mt19937 generator(std::chrono::system_clock::now().time_since_epoch().count());
        std::uniform_real_distribution<float> distribution(-1.0f, 1.0f);
        
        float jitter = distribution(generator);
        if (std::abs(jitter) > 0.97f) {  // Применяем jitter только в 10% случаев
            dstRect.y += (jitter > 0) ? 1 : -1;
        }
    }
    
    SDL_RenderCopy(renderer, texture, NULL, &dstRect);
    SDL_DestroyTexture(texture);
}

// Объявеия ункци из mainau.cpp
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
        std::cerr << "Не удалось найти вдеопоток" << std::endl;
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
    std::string timecode = "00:00:00:00";
    if (waiting_for_timecode) {
        for (size_t i = 0; i < input_timecode.length() && i < 8; ++i) {
            size_t pos = i + (i / 2);  // Учитываем позиции двоеточий
            timecode[pos] = input_timecode[i];
        }
    } else {
        int hours = static_cast<int>(currentTime / 3600);
        int minutes = static_cast<int>((currentTime - hours * 3600) / 60);
        int seconds = static_cast<int>(currentTime) % 60;
        int frames = static_cast<int>((currentTime - static_cast<int>(currentTime)) * original_fps.load());

        std::stringstream timecodeStream;
        timecodeStream << std::setfill('0') << std::setw(2) << hours << ":"
                       << std::setfill('0') << std::setw(2) << minutes << ":"
                       << std::setfill('0') << std::setw(2) << seconds << ":"
                       << std::setfill('0') << std::setw(2) << frames;
        timecode = timecodeStream.str();
    }

    // Подготавливаем текст для правой части
    std::string rightText = isReverse ? "REV" : "FWD";
    if (jog_forward.load() || jog_backward.load()) {
    } else if (std::abs(playbackRate) > 1.0) {
        int roundedSpeed = std::round(std::abs(playbackRate));
        rightText += " " + std::to_string(roundedSpeed) + "x";
    }

    // Отрисовываем текст
    SDL_Color textColor = {255, 255, 255, 255};  // Белый цвет
    SDL_Color grayColor = {128, 128, 128, 255};  // Серый цвет для неактивных цифр при вводе таймкода

    SDL_Surface* leftSurface = TTF_RenderText_Solid(font, leftText.c_str(), textColor);
    SDL_Texture* leftTexture = SDL_CreateTextureFromSurface(renderer, leftSurface);
    SDL_Rect leftRect = {10, windowHeight - 30 + (30 - leftSurface->h) / 2, leftSurface->w, leftSurface->h};
    SDL_RenderCopy(renderer, leftTexture, NULL, &leftRect);

    // Отрисовка таймкода
    int charWidth = 0;
    int charHeight = 0;
    TTF_SizeText(font, "0", &charWidth, &charHeight);  // Получаем размер одного символа
    int totalWidth = charWidth * 11;  // 8 цифр + 3 двоеточия
    int xPos = (windowWidth - totalWidth) / 2;
    int yPos = windowHeight - 30 + (30 - charHeight) / 2;

    for (size_t i = 0; i < timecode.length(); ++i) {
        char c[2] = {timecode[i], '\0'};
        SDL_Color color;
        if (waiting_for_timecode) {
            if (i % 3 == 2) {  // Двоеточия
                color = grayColor;
            } else {
                // Проверяем, введен ли этот символ
                size_t inputIndex = i - (i / 3);  // Индекс в input_timecode
                color = (inputIndex < input_timecode.length()) ? textColor : grayColor;
            }
        } else {
            color = textColor;
        }
        
        SDL_Surface* charSurface = TTF_RenderText_Solid(font, c, color);
        SDL_Texture* charTexture = SDL_CreateTextureFromSurface(renderer, charSurface);
        SDL_Rect charRect = {xPos, yPos, charSurface->w, charSurface->h};
        SDL_RenderCopy(renderer, charTexture, NULL, &charRect);
        xPos += charWidth;
        SDL_FreeSurface(charSurface);
        SDL_DestroyTexture(charTexture);
    }

    SDL_Surface* rightSurface = TTF_RenderText_Solid(font, rightText.c_str(), textColor);
    SDL_Texture* rightTexture = SDL_CreateTextureFromSurface(renderer, rightSurface);
    SDL_Rect rightRect = {windowWidth - rightSurface->w - 10, windowHeight - 30 + (30 - rightSurface->h) / 2, rightSurface->w, rightSurface->h};
    SDL_RenderCopy(renderer, rightTexture, NULL, &rightRect);

    // Освобождаем ресу��сы
    SDL_FreeSurface(leftSurface);
    SDL_DestroyTexture(leftTexture);
    SDL_FreeSurface(rightSurface);
    SDL_DestroyTexture(rightTexture);
}

// Добавьте эту функцию перед main()
int getUpdateInterval(double playbackRate) {
    if (playbackRate < 0.9) return std::numeric_limits<int>::max(); // Не декодируем
    if (playbackRate <= 1.0) return 5000;
    if (playbackRate <= 2.0) return 2500;
    if (playbackRate <= 4.0) return 1000;
    if (playbackRate <= 8.0) return 500;
    return 100; // Для скорости 16x и выше
}

void initializeBuffer(const char* lowResFilename, std::vector<FrameInfo>& frameIndex, int currentFrame, int bufferSize) {
    int bufferStart = std::max(0, currentFrame - bufferSize / 2);
    int bufferEnd = std::min(bufferStart + bufferSize - 1, static_cast<int>(frameIndex.size()) - 1);
    
    asyncDecodeLowResRange(lowResFilename, frameIndex, bufferStart, bufferEnd, currentFrame, currentFrame).wait();
}

// В функции removeHighResFrames
void removeHighResFrames(std::vector<FrameInfo>& frameIndex, int start, int end, int highResStart, int highResEnd) {
    for (int i = start; i <= end && i < frameIndex.size(); ++i) {
        if (frameIndex[i].type == FrameInfo::FULL_RES) {
            // Добавьте проверку, чтобы не удалять кадры в текущей зоне выскго разрешения
            if (i < highResStart || i > highResEnd) {
                // std::cout << "Удаление кадра высокого разрешения " << i << std::endl;
                frameIndex[i].frame.reset();
                frameIndex[i].type = FrameInfo::LOW_RES;
            }
        }
    }
}

void log(const std::string& message) {
    static std::ofstream logFile("TapeXPlayer.log", std::ios::app);
    logFile << message << std::endl;
}

// Добавьте эту функцию перед main()
void renderRewindEffect(SDL_Renderer* renderer, double playbackRate, double currentTime, double totalDuration) {
    int windowWidth, windowHeight;
    SDL_GetRendererOutputSize(renderer, &windowWidth, &windowHeight);

    // Проверяем, нужно ли рендерить эффект
    if (std::abs(playbackRate) <= 1.1) return;

    // Проверяем, не находимся ли мы в начале или конце файла
    const double threshold = 0.1; // Порог в секундах
    if (currentTime < threshold || (totalDuration - currentTime) < threshold) return;

    // Базовое количество полос при скорости чуть выше 1.1x
    double baseNumStripes = 1.0;
    // Плавно увеличиваем количество полос с ростом скорости
    double speedFactor = std::max(0.0, std::abs(playbackRate) - 1.1);
    double numStripesFloat = baseNumStripes + std::sqrt(speedFactor) * 2.0;
    int numStripes = static_cast<int>(std::round(numStripesFloat));
    numStripes = std::min(numStripes, 10); // Ограничиваем максимальное количество полос

    // Базовая высота полосы (самая толстая при скорости чуть выше 1.1x)
    int baseStripeHeight = windowHeight / baseNumStripes;
    // ��меньшаем высоту полосы с ростом скорости
    int stripeHeight = static_cast<int>(baseStripeHeight / (std::abs(playbackRate) / 0.2));
    stripeHeight = std::max(stripeHeight, 10); // Минимальная высота полосы

    // Скорость движения полос (пикселей за кадр)
    // Увеличиваем базовую скорость и делаем ее зависимой от высоты окна
    int baseSpeed = windowHeight / 3; // Базовая скорость для скорости чуть выше 1.1x
    int speed = static_cast<int>(baseSpeed * (std::abs(playbackRate) / 1.1));

    // Смещение полос (меняется со временем)
    static int offset = 0;
    offset = (offset + speed) % windowHeight;

    // Цвет полос (серый, непрозрачный)
    SDL_SetRenderDrawColor(renderer, 128, 128, 128, 255);

    for (int i = 0; i < numStripes; ++i) {
        SDL_Rect stripeRect;
        stripeRect.x = 0;
        stripeRect.y = windowHeight - (i * (windowHeight / numStripes) + offset) % windowHeight;
        stripeRect.w = windowWidth;
        stripeRect.h = stripeHeight;

        SDL_RenderFillRect(renderer, &stripeRect);
    }
}

int main(int argc, char* argv[]) {
    log("Программа запущена");
    log("Количество аргументов: " + std::to_string(argc));
    for (int i = 0; i < argc; ++i) {
        log("Аргумент " + std::to_string(i) + ": " + argv[i]);
    }
    log("Текущая р��бочая директория: " + std::string(getcwd(NULL, 0)));

    if (argc < 2) {
        std::cerr << "Использование: " << argv[0] << " <путь_к_видеофайлу>" << std::endl;
        return 1;
    }

    std::string filename = argv[1];
    
    // Обработка пути к файлу для macOS
    if (filename.find("/Volumes/") == 0) {
        // Путь уже абсолютный, оставляем как есть
    } else if (filename[0] != '/') {
        // Относительный путь, добавляем текущую директорию
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd)) != NULL) {
            filename = std::string(cwd) + "/" + filename;
        } else {
            std::cerr << "Ошибка олучения текущей директории" << std::endl;
            return 1;
        }
    }

    std::cout << "Получен путь к файлу: " << filename << std::endl;
    log("Обработанный путь к файлу: " + filename);

    if (!std::filesystem::exists(filename)) {
        std::cerr << "Файл не найден: " << filename << std::endl;
        log("Ошибка: файл не найден: " + filename);
        return 1;
    }

    std::string lowResFilename = "low_res_output.mp4";

    std::vector<FrameInfo> frameIndex = createFrameIndex(filename.c_str());
    std::cout << "Создан индекс кадров. Всего кадров: " << frameIndex.size() << std::endl;

    const size_t ringBufferCapacity = 1000;  // или другое подходящее значение

    if (!convertToLowRes(filename.c_str(), lowResFilename)) {
        std::cerr << "Ошибка при конвертации видео в низкое разрешение" << std::endl;
        return 1;
    }

    // Инициализация буфера после конвертации
    initializeBuffer(lowResFilename.c_str(), frameIndex, 0, ringBufferCapacity);

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
SDL_RWops* rw = SDL_RWFromConstMem(font_otf, sizeof(font_otf));
    TTF_Font* font = TTF_OpenFontRW(rw, 1, 24);
    if (!font) {
        std::cerr << "Ошибка загрузки шрифта из памяти: " << TTF_GetError() << std::endl;
        return 1;
    }

    std::future<double> fps_future = std::async(std::launch::async, get_video_fps, filename.c_str());
    std::future<double> duration_future = std::async(std::launch::async, get_file_duration, filename.c_str());

    // Получаем размеры видео
    int videoWidth, videoHeight;
    get_video_dimensions(filename.c_str(), &videoWidth, &videoHeight);

    SDL_Window* window = SDL_CreateWindow("TapeXPlayer", 
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 
        videoWidth, videoHeight, 
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!window) {
        std::cerr << "Ошбка создания окна: " << SDL_GetError() << std::endl;
        SDL_Quit();
        return 1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        std::cerr << "Ошибка создания рендерера с VSync: " << SDL_GetError() << std::endl;
        // Попробуем создать рендерер без VSync
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
        if (!renderer) {
            std::cerr << "шибка создания рендерера без VSync: " << SDL_GetError() << std::endl;
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 1;
        }
        std::cout << "Рендерер создн без VSync" << std::endl;
    } else {
        std::cout << "Рендерер создан с VSync" << std::endl;
    }

    RingBuffer ringBuffer(ringBufferCapacity);
    FrameCleaner frameCleaner(frameIndex);

    int currentFrame = 0;
    bool isPlaying = false;
    bool enableHighResDecode = true;
    std::chrono::steady_clock::time_point lastFrameTime;
    std::chrono::steady_clock::time_point lastBufferUpdateTime = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point now;

    const int highResWindowSize = 500;  // Размер окна высокого разрешения
    std::future<void> lowResFuture;
    std::future<void> highResFuture;
    std::future<void> secondaryCleanFuture;

    const int TARGET_FPS = 60;
    const int FRAME_DELAY = 1000 / TARGET_FPS;

    // Инициализация аудио
    audio_thread = std::thread(start_audio, filename.c_str());
    speed_change_thread = std::thread(smooth_speed_change);

    bool fps_received = false;
    bool duration_received = false;

    std::chrono::steady_clock::time_point lastLowResUpdateTime = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point lastHighResUpdateTime = std::chrono::steady_clock::now();

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

        // Прверяем, готова ли длительность
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
                if (waiting_for_timecode) {
                    // std::cout << "Нажата клавиша: " << SDL_GetKeyName(e.key.keysym.sym) << " (од: " << e.key.keysym.sym << ")" << std::endl;
                    
                    if (e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_KP_ENTER) {
                        try {
                            double target_time = parse_timecode(input_timecode);
                            seek_to_time(target_time);
                            waiting_for_timecode = false;
                            input_timecode.clear();
                        } catch (const std::exception& ex) {
                           // std::cout << "Invalid timecode: " << ex.what() << std::endl;
                        }
                    } else if (e.key.keysym.sym == SDLK_BACKSPACE && !input_timecode.empty()) {
                        input_timecode.pop_back();
                    } else if (input_timecode.length() < 8) {
                        const char* keyName = SDL_GetKeyName(e.key.keysym.sym);
                        char digit = '\0';
                        
                        if (keyName[0] >= '0' && keyName[0] <= '9' && keyName[1] == '\0') {
                            // Обычные цифровые клавиши
                            digit = keyName[0];
                        } else if (strncmp(keyName, "Keypad ", 7) == 0 && keyName[7] >= '0' && keyName[7] <= '9' && keyName[8] == '\0') {
                            // Клавиши NumPad
                            digit = keyName[7];
                        }
                        
                        if (digit != '\0') {
                            input_timecode += digit;
                           //  std::cout << "Добавлена цифра: " << digit << ", текущий таймкод: " << input_timecode << std::endl;
                        }
                    } else if (e.key.keysym.sym == SDLK_ESCAPE) {
                        waiting_for_timecode = false;
                        input_timecode.clear();
                    }
                    
                    // std::cout << "Текущий таймкод: " << input_timecode << std::endl;
                } else {
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
                                // Обычное поведение стрелки впао
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
                                // std::cout << "Playback direction: " << (is_reverse.load() ? "Reverse" : "Forward") << std::endl;
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
                            // std::cout << "Playback direction: " << (is_reverse.load() ? "Reverse" : "Forward") << std::endl;
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
                               // std::cout << "Отображение индекса: " << (showIndex ? "включено" : "выключено") << std::endl;
                            }
                            break;
                        case SDLK_g:
                            waiting_for_timecode = true;
                            // std::cout << "Enter timecode (HHMMSSFF): ";
                            break;
                    }
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

        // Проверка на выполнение операции seek
        if (seek_performed.load()) {
            // Получаем текущее время после seek
            double currentTime = current_audio_time.load();
            currentFrame = static_cast<int>(currentTime * original_fps.load()) % frameIndex.size();

            // Пересоздаем буфер
            initializeBuffer(lowResFilename.c_str(), frameIndex, currentFrame, ringBufferCapacity);

            // Сбрасываем флаг seek
            seek_performed.store(false);

            // Сбрасываем времена последних обновлений
            lastLowResUpdateTime = std::chrono::steady_clock::now();
            lastHighResUpdateTime = std::chrono::steady_clock::now();
        }

        // Обновление буфера
        int bufferStart = std::max(0, currentFrame - static_cast<int>(ringBufferCapacity / 2));
        int bufferEnd = std::min(bufferStart + static_cast<int>(ringBufferCapacity) - 1, static_cast<int>(frameIndex.size()) - 1);

        // Определение границ окна высокого разрешения
        int highResStart = std::max(0, currentFrame - highResWindowSize / 2);
        int highResEnd = std::min(static_cast<int>(frameIndex.size()) - 1, currentFrame + highResWindowSize / 2);

        bool isInHighResZone = (currentFrame >= highResStart && currentFrame <= highResEnd);

        now = std::chrono::steady_clock::now();
        auto timeSinceLastLowResUpdate = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastLowResUpdateTime);
        auto timeSinceLastHighResUpdate = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastHighResUpdateTime);

        double currentPlaybackRate = std::abs(playback_rate.load());
        int lowResUpdateInterval = getUpdateInterval(currentPlaybackRate);
        int highResUpdateInterval = getUpdateInterval(currentPlaybackRate) / 2;

        // Определяем, нужно ли декодировать кадры высокого разрешения
        bool shouldDecodeHighRes = currentPlaybackRate < 2.0 && enableHighResDecode;

        // Цикл декодирования кадров низкого разрешения
        if (timeSinceLastLowResUpdate.count() >= lowResUpdateInterval) {
            bool needLowResUpdate = false;
            for (int i = bufferStart; i <= bufferEnd; ++i) {
                if (frameIndex[i].type == FrameInfo::EMPTY) {
                    needLowResUpdate = true;
                    break;
                }
            }

            if (needLowResUpdate) {
                if (!lowResFuture.valid() || lowResFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                    lowResFuture = asyncDecodeLowResRange(lowResFilename.c_str(), frameIndex, bufferStart, bufferEnd, highResStart, highResEnd);
                    lastLowResUpdateTime = now;
                }
            }
        }

        // Цикл декодирования кадров высокого разрешения
        if (shouldDecodeHighRes && timeSinceLastHighResUpdate.count() >= highResUpdateInterval) {
            bool needHighResUpdate = false;
            for (int i = highResStart; i <= highResEnd; ++i) {
                if (frameIndex[i].type != FrameInfo::FULL_RES) {
                    needHighResUpdate = true;
                    break;
                }
            }

            if (needHighResUpdate) {
                if (!highResFuture.valid() || highResFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                    // std::cout << "Запуск декодироания высокого разрешения для кадров " << highResStart << " - " << highResEnd << std::endl;
                    highResFuture = asyncDecodeFrameRange(filename.c_str(), frameIndex, highResStart, highResEnd);
                    lastHighResUpdateTime = now;
                } else {
                   //  std::cout << "Декодирование высокого разрешения все еще выполняется" << std::endl;
                }
            }
        } else if (currentPlaybackRate >= 2.0) {
            // Если скорость воспроизведения >= 2x, удаляем кадры высокого разрешения
            removeHighResFrames(frameIndex, 0, frameIndex.size() - 1, -1, -1);
        }

        // Удаляем кадры высокого разрешения за пределами окна
        removeHighResFrames(frameIndex, 0, highResStart - 1, highResStart, highResEnd);
        removeHighResFrames(frameIndex, highResEnd + 1, frameIndex.size() - 1, highResStart, highResEnd);

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
        displayCurrentFrame(renderer, frameIndex[currentFrame], isInHighResZone && enableHighResDecode, std::abs(playback_rate.load()), currentTime, total_duration.load());

        // Добавьте эти строки:
        double effectPlaybackRate = playback_rate.load();
        if (std::abs(effectPlaybackRate) >= 2.0) {
            renderRewindEffect(renderer, effectPlaybackRate, currentTime, total_duration.load());
        }

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

        // В главном цикле
        // std::cout << "Текущий кадр: " << currentFrame << ", High Res зона: " << highResStart << " - " << highResEnd << std::endl;
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
    if (font) {
        TTF_CloseFont(font);
        font = nullptr;
    }

    TTF_Quit();

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}