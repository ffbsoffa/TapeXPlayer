#ifndef MAINVID_H
#define MAINVID_H

#include <vector>
#include <atomic>
#include <thread>
#include <memory>
#include <SDL2/SDL_ttf.h>
#include <sstream>
#include <iomanip>

extern "C" {
#include <libavcodec/avcodec.h>
}

// В начале файла добавьте объявление функции generateTXTimecode
std::string generateTXTimecode(double time);

// Определение структуры FrameInfo
struct FrameInfo {
    int64_t pts;
    int64_t relative_pts;
    int64_t time_ms;
    std::shared_ptr<AVFrame> frame;
    std::shared_ptr<AVFrame> low_res_frame;
    enum FrameType {
        EMPTY,
        LOW_RES,
        FULL_RES
    } type;
};

// Предварительные объявления классов
class PlaybackHead;
class FrameCleaner;

// Объявления функций
bool convertToLowRes(const char* filename, const char* outputFilename);
bool fillIndexWithLowResFrames(const char* filename, std::vector<FrameInfo>& frameIndex);
void decodeAndClean(const char* filename, std::vector<FrameInfo>& frameIndex, PlaybackHead& playbackHead, FrameCleaner& frameCleaner, std::atomic<bool>& isPlaying);

double get_precise_audio_time();
extern std::atomic<double> playback_rate;
extern std::atomic<bool> debug_mode;

// Объявление функций и классов, перенесенных из main.cpp
class FrameIndexer {
public:
    FrameIndexer(const char* filename) : filename(filename) {}

    std::vector<FrameInfo> createIndex() {
        std::vector<FrameInfo> frameIndex;
        AVFormatContext* formatContext = nullptr;

        if (avformat_open_input(&formatContext, filename, nullptr, nullptr) != 0) {
            std::cerr << "Не удалось открыть файл" << std::endl;
            return frameIndex;
        }

        if (avformat_find_stream_info(formatContext, nullptr) < 0) {
            std::cerr << "Не удалось получить информацию о потоках" << std::endl;
            avformat_close_input(&formatContext);
            return frameIndex;
        }

        int videoStream = av_find_best_stream(formatContext, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (videoStream < 0) {
            std::cerr << "Видеопоток не найден" << std::endl;
            avformat_close_input(&formatContext);
            return frameIndex;
        }

        AVRational timeBase = formatContext->streams[videoStream]->time_base;
        int64_t startTime = formatContext->streams[videoStream]->start_time;

        AVPacket packet;
        while (av_read_frame(formatContext, &packet) >= 0) {
            if (packet.stream_index == videoStream) {
                FrameInfo info;
                info.pts = packet.pts;
                info.relative_pts = packet.pts - startTime;
                info.time_ms = av_rescale_q(info.relative_pts, timeBase, {1, 1000});
                info.frame = nullptr;
                frameIndex.push_back(info);
            }
            av_packet_unref(&packet);
        }

        avformat_close_input(&formatContext);

        return frameIndex;
    }

private:
    const char* filename;
};

extern std::atomic<bool> isDecodingActive;

// Предварительное объявление функции decodeFrameRange
bool decodeFrameRange(const char* filename, std::vector<FrameInfo>& frameIndex, int startFrame, int endFrame, FrameInfo::FrameType targetType);

void asyncDecodeFrameRange(const char* filename, std::vector<FrameInfo>& frameIndex, int startFrame, int endFrame) {
    if (isDecodingActive.exchange(true)) {
        std::cout << "Декодирование уже выполняется. Пропускаем новый запрос." << std::endl;
        return;
    }

    std::thread decodingThread([filename, &frameIndex, startFrame, endFrame]() {
        decodeFrameRange(filename, frameIndex, startFrame, endFrame, FrameInfo::LOW_RES);
        isDecodingActive.store(false);
    });

    decodingThread.detach();
}

// Полное определение функции decodeFrameRange
bool decodeFrameRange(const char* filename, std::vector<FrameInfo>& frameIndex, int startFrame, int endFrame, FrameInfo::FrameType targetType) {
    const int numThreads = 2; // Фиксируем количество потоков
    std::vector<std::thread> threads;
    std::atomic<bool> success{true};

    auto decodeSegment = [&](int threadId, int threadStartFrame, int threadEndFrame) {
        AVFormatContext* formatContext = nullptr;
        AVCodecContext* codecContext = nullptr;
        const AVCodec* codec = nullptr;
        int videoStream;

        if (avformat_open_input(&formatContext, filename, nullptr, nullptr) != 0) {
            success = false;
            return;
        }

        if (avformat_find_stream_info(formatContext, nullptr) < 0) {
            avformat_close_input(&formatContext);
            success = false;
            return;
        }

        videoStream = av_find_best_stream(formatContext, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
        if (videoStream < 0) {
            avformat_close_input(&formatContext);
            success = false;
            return;
        }

        // Попытка найти декодер VideoToolbox
        const AVCodec* hwCodec = avcodec_find_decoder_by_name("h264_videotoolbox");
        if (hwCodec) {
            codec = hwCodec;
        }

        codecContext = avcodec_alloc_context3(codec);
        if (!codecContext) {
            avformat_close_input(&formatContext);
            success = false;
            return;
        }

        codecContext->thread_count = std::thread::hardware_concurrency(); // Используем все доступные ядра
        codecContext->thread_type = FF_THREAD_FRAME; // Или FF_THREAD_SLICE для некоторых кодеков
        codecContext->flags2 |= AV_CODEC_FLAG2_FAST; // Используем быстрое декодироание
        codecContext->skip_loop_filter = AVDISCARD_ALL; // Пропускаем фильтр деблокировки
        codecContext->flags |= AV_CODEC_FLAG_LOW_DELAY;
        codecContext->strict_std_compliance = FF_COMPLIANCE_UNOFFICIAL;

        if (avcodec_parameters_to_context(codecContext, formatContext->streams[videoStream]->codecpar) < 0) {
            avcodec_free_context(&codecContext);
            avformat_close_input(&formatContext);
            success = false;
            return;
        }

        if (avcodec_open2(codecContext, codec, nullptr) < 0) {
            avcodec_free_context(&codecContext);
            avformat_close_input(&formatContext);
            success = false;
            return;
        }

        AVRational timeBase = formatContext->streams[videoStream]->time_base;
        int64_t startPts = frameIndex[threadStartFrame].pts;
        av_seek_frame(formatContext, videoStream, startPts, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(codecContext);

        AVPacket* packet = av_packet_alloc();
        AVFrame* frame = av_frame_alloc();

        int currentFrame = threadStartFrame;

while (av_read_frame(formatContext, packet) >= 0 && currentFrame <= threadEndFrame) {
            if (packet->stream_index == videoStream) {
                int ret = avcodec_send_packet(codecContext, packet);
                if (ret < 0) {
                    av_packet_unref(packet);
                    continue;
                }

                while (ret >= 0) {
                    ret = avcodec_receive_frame(codecContext, frame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                        break;
                    } else if (ret < 0) {
                        break;
                    }

                    if (frame->pts >= startPts && currentFrame <= threadEndFrame) {
                        double seconds = frame->pts * av_q2d(timeBase);
                        int64_t milliseconds = seconds * 1000;

                        if (targetType == FrameInfo::FULL_RES) {
                            frameIndex[currentFrame].frame = std::shared_ptr<AVFrame>(av_frame_clone(frame), 
                                                            [](AVFrame* f) { av_frame_free(&f); });
                            frameIndex[currentFrame].type = FrameInfo::FULL_RES;
                        } else {
                            frameIndex[currentFrame].low_res_frame = std::shared_ptr<AVFrame>(av_frame_clone(frame), 
                                                                    [](AVFrame* f) { av_frame_free(&f); });
                            frameIndex[currentFrame].type = FrameInfo::LOW_RES;
                        }

                        frameIndex[currentFrame].pts = frame->pts;
                        frameIndex[currentFrame].relative_pts = frame->pts - formatContext->streams[videoStream]->start_time;
                        frameIndex[currentFrame].time_ms = milliseconds;

                        currentFrame++;
                    }
                }
            }
            av_packet_unref(packet);
            if (currentFrame > threadEndFrame) break;
        }

        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&codecContext);
        avformat_close_input(&formatContext);
    };

    int framesPerThread = (endFrame - startFrame + 1) / numThreads;
    for (int i = 0; i < numThreads; ++i) {
        int threadStartFrame = startFrame + i * framesPerThread;
        int threadEndFrame = (i == numThreads - 1) ? endFrame : threadStartFrame + framesPerThread - 1;
        threads.emplace_back(decodeSegment, i, threadStartFrame, threadEndFrame);
    }

    for (auto& thread : threads) {
        thread.join();
    }

    return success;
}

bool convertToLowRes(const char* filename, const char* outputFilename) {
    // Получаем размеры оригинального видео
    char probe_cmd[512];
    snprintf(probe_cmd, sizeof(probe_cmd),
             "ffprobe -v error -select_streams v:0 -count_packets -show_entries stream=width,height -of csv=p=0 %s",
             filename);
    
    FILE* pipe = popen(probe_cmd, "r");
    if (!pipe) {
        std::cerr << "Ошибка при выполнении ffprobe" << std::endl;
        return false;
    }
    
    int width, height;
    if (fscanf(pipe, "%d,%d", &width, &height) != 2) {
        std::cerr << "Ошибка при чтении размеров видео" << std::endl;
        pclose(pipe);
        return false;
    }
    pclose(pipe);

    // Если высота видео меньше 720, просто копируем файл без изменений
    if (height < 720) {
        char copy_cmd[1024];
        snprintf(copy_cmd, sizeof(copy_cmd), "cp %s %s", filename, outputFilename);
        int result = system(copy_cmd);
        if (result != 0) {
            std::cerr << "Ошибка при копировании видео" << std::endl;
            return false;
        }
        return true;
    }

    // Для видео с высотой 720 и больше, выполняем уменьшение разрешения
    int new_width = std::max(640, width / 2);
    int new_height = height * new_width / width; // Сохраняем пропорции

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -i %s -c:v libx264 -crf 23 -preset ultrafast "
             "-vf scale=%d:%d "
             "-y %s",
             filename, new_width, new_height, outputFilename);
    
    int result = system(cmd);
    if (result != 0) {
        std::cerr << "Ошибка при конвертации видео в низкое разрешение" << std::endl;
        return false;
    }
    return true;
}

bool fillIndexWithLowResFrames(const char* filename, std::vector<FrameInfo>& frameIndex) {
    AVFormatContext* formatContext = nullptr;
    AVCodecContext* codecContext = nullptr;
    const AVCodec* codec = nullptr;
    int videoStream;

    if (avformat_open_input(&formatContext, filename, nullptr, nullptr) != 0) {
        std::cerr << "Не удалось открыть файл" << std::endl;
        return false;
    }

    if (avformat_find_stream_info(formatContext, nullptr) < 0) {
        std::cerr << "Не удалось найти информацию о потоках" << std::endl;
        avformat_close_input(&formatContext);
        return false;
    }

    videoStream = av_find_best_stream(formatContext, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (videoStream < 0) {
        std::cerr << "Видеопоток не найден" << std::endl;
        avformat_close_input(&formatContext);
        return false;
    }

    codecContext = avcodec_alloc_context3(codec);
    if (!codecContext) {
        avformat_close_input(&formatContext);
        return false;
    }

    if (avcodec_parameters_to_context(codecContext, formatContext->streams[videoStream]->codecpar) < 0) {
        avcodec_free_context(&codecContext);
        avformat_close_input(&formatContext);
        return false;
    }

    if (avcodec_open2(codecContext, codec, nullptr) < 0) {
        avcodec_free_context(&codecContext);
        avformat_close_input(&formatContext);
        return false;
    }

    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();

    int frameCount = 0;
    while (av_read_frame(formatContext, packet) >= 0) {
        if (packet->stream_index == videoStream) {
            int ret = avcodec_send_packet(codecContext, packet);
            if (ret < 0) {
                av_packet_unref(packet);
                continue;
            }

            while (ret >= 0) {
                ret = avcodec_receive_frame(codecContext, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                } else if (ret < 0) {
                    break;
                }

                if (frameCount < frameIndex.size()) {
                    frameIndex[frameCount].low_res_frame = std::shared_ptr<AVFrame>(av_frame_clone(frame), [](AVFrame* f) { av_frame_free(&f); });
                    frameIndex[frameCount].type = FrameInfo::LOW_RES;
                } else {
                    frameIndex.push_back(FrameInfo{});
                    frameIndex.back().low_res_frame = std::shared_ptr<AVFrame>(av_frame_clone(frame), [](AVFrame* f) { av_frame_free(&f); });
                    frameIndex.back().type = FrameInfo::LOW_RES;
                }

                frameCount++;
                if (frameCount % 100 == 0) {
                    std::cout << "Обработано " << frameCount << " кадров\r" << std::flush;
                }
            }
        }
        av_packet_unref(packet);
    }

    std::cout << "\nВсего обработано " << frameCount << " кадров" << std::endl;

    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&codecContext);
    avformat_close_input(&formatContext);

    return true;
}



class FrameCleaner {
private:
    std::vector<FrameInfo>& frameIndex;

public:
    FrameCleaner(std::vector<FrameInfo>& fi) : frameIndex(fi) {}

    void cleanFrames(int startFrame, int endFrame) {
        for (int i = startFrame; i <= endFrame && i < frameIndex.size(); ++i) {
            frameIndex[i].frame.reset();
            frameIndex[i].low_res_frame.reset();
            frameIndex[i].type = FrameInfo::EMPTY;
            std::cout << "\rОчищен кадр " << i << std::flush;
        }
    }

    void downgradeResolution(int startFrame, int endFrame) {
        for (int i = startFrame; i <= endFrame && i < frameIndex.size(); ++i) {
            if (frameIndex[i].type == FrameInfo::FULL_RES) {
                frameIndex[i].frame.reset();
                frameIndex[i].type = FrameInfo::LOW_RES;
                std::cout << "\rПонижено разрешение кадра " << i << std::flush;
            }
        }
    }
};

class SDLDisplay {
public:
    SDLDisplay(int videoWidth, int videoHeight) 
        : window(nullptr), renderer(nullptr), texture(nullptr),
          videoWidth(videoWidth), videoHeight(videoHeight), font(nullptr) {
        if (SDL_Init(SDL_INIT_VIDEO) < 0) {
            std::cerr << "SDL не смог инициализироваться. SDL_Error: " << SDL_GetError() << std::endl;
            return;
        }

        window = SDL_CreateWindow("TapeXPlayer", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 
                                  videoWidth, videoHeight, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
        if (window == nullptr) {
            std::cerr << "Окно не может быть создано. SDL_Error: " << SDL_GetError() << std::endl;
            return;
        }

        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
        if (renderer == nullptr) {
            std::cerr << "Рендерер не может быть создан. SDL_Error: " << SDL_GetError() << std::endl;
            return;
        }

        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, 
                                    videoWidth, videoHeight);
        if (texture == nullptr) {
            std::cerr << "Текстура не может быть создана. SDL_Error: " << SDL_GetError() << std::endl;
        }

        srand(static_cast<unsigned int>(time(nullptr)));  // Инициализация генератора случайных чисел

        // Установка цвета фона на черный
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);

        if (TTF_Init() == -1) {
            std::cerr << "SDL_ttf не смог инициализироваться. SDL_ttf Error: " << TTF_GetError() << std::endl;
            return;
        }

        font = TTF_OpenFont("font.ttf", 24);
        if (font == nullptr) {
            std::cerr << "Не удалось загрузить шрифт. SDL_ttf Error: " << TTF_GetError() << std::endl;
        }

        textColor = {255, 255, 255, 255}; // Белый цвет
    }

    ~SDLDisplay() {
        if (texture) SDL_DestroyTexture(texture);
        if (renderer) SDL_DestroyRenderer(renderer);
        if (window) SDL_DestroyWindow(window);
        if (font) TTF_CloseFont(font);
        TTF_Quit();
        SDL_Quit();
    }

void displayFrame(const uint8_t* yPlane, const uint8_t* uPlane, const uint8_t* vPlane, 
                  int yPitch, int uPitch, int vPitch, 
                  int frameWidth, int frameHeight,
                  const std::vector<FrameInfo>& frameIndex, int currentFrame) {
    // Получаем текущие размеры окна
    int windowWidth, windowHeight;
    SDL_GetWindowSize(window, &windowWidth, &windowHeight);

    // Объявляем переменные для размеров текстуры
    int texWidth, texHeight;

    // Если текстура не существует или её размеры не совпадают с размерами кадра, пересоздаем текстуру
    if (!texture || SDL_QueryTexture(texture, NULL, NULL, &texWidth, &texHeight) < 0 ||
        texWidth != frameWidth || texHeight != frameHeight) {
        if (texture) {
            SDL_DestroyTexture(texture);
        }
        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, 
                                    frameWidth, frameHeight);
        if (texture == nullptr) {
            std::cerr << "Не удалось создать новую текстуру. SDL_Error: " << SDL_GetError() << std::endl;
            return;
        }
    }

    // Обновляем текстуру с исходными размерами кадра
    SDL_UpdateYUVTexture(texture, NULL, 
                         yPlane, yPitch, 
                         uPlane, uPitch, 
                         vPlane, vPitch);

    // Очистка экрана черным цветом
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    
    // Вычисляем размеры для отображения, сохраняя пропорции
    SDL_Rect destRect;
    float aspectRatio = (float)frameWidth / frameHeight;
    if (windowWidth / aspectRatio <= windowHeight) {
        destRect.w = windowWidth;
        destRect.h = (int)(windowWidth / aspectRatio);
        destRect.x = 0;
        destRect.y = (windowHeight - destRect.h) / 2;
    } else {
        destRect.h = windowHeight;
        destRect.w = (int)(windowHeight * aspectRatio);
        destRect.x = (windowWidth - destRect.w) / 2;
        destRect.y = 0;
    }

    // Добавляем эффект дрожания (jitter)
    double playbackRate = playback_rate.load();
    double maxJitter = 0.0;
    if (playbackRate == 0.0) {  // Пауза
        maxJitter = 0.5;  // Уменьшено с 2 до 0.5
    } else if (std::abs(playbackRate) > 1.0) {  // Повышенная скорость
        maxJitter = std::min(2.0, std::abs(playbackRate) * 0.3);  // Уменьшен коэффициент и ограничен максимум
    }

    if (maxJitter > 0) {
        // Используем дробные значения для более тонкого контроля
        double jitter = (static_cast<double>(rand()) / RAND_MAX - 0.5) * 2 * maxJitter;
        destRect.y += static_cast<int>(jitter);
    }

    // Отображаем текстуру, масштабируя её ��о нужного размера
    SDL_RenderCopy(renderer, texture, NULL, &destRect);

    // Рисуем полоску индекса кадров только в отладочном режиме
    if (debug_mode.load()) {
        drawFrameIndexBar(frameIndex, currentFrame, windowWidth, windowHeight);
    }

    // Отрисовка индикации
    double currentTime = get_precise_audio_time();  // Изменено здесь
    std::string timeCode = generateTXTimecode(currentTime);
    std::string playStatus = (playback_rate.load() == 0.0) ? "STILL" : "PLAY";

    // Создаем черный фон для текста
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 200);
    SDL_Rect textBgRect = {0, windowHeight - 40, windowWidth, 40};
    SDL_RenderFillRect(renderer, &textBgRect);

    // Отрисовка текста
    int textY = windowHeight - 35;
    renderText(timeCode, (windowWidth - getTextWidth(timeCode)) / 2, textY);
    renderText(playStatus, 10, textY);

    SDL_RenderPresent(renderer);
}

private:
    SDL_Window* window;
    SDL_Renderer* renderer;
    SDL_Texture* texture;
    int videoWidth, videoHeight;
    TTF_Font* font;
    SDL_Color textColor;

    void drawFrameIndexBar(const std::vector<FrameInfo>& frameIndex, int currentFrame, int windowWidth, int windowHeight) {
        const int barHeight = 10;
        const int barY = 0;
        
        // Рисуем фон полоски (пустые кадры)
        SDL_SetRenderDrawColor(renderer, 122, 0, 0, 100); // Красный цвет для пустых кадров
        SDL_Rect barRect = {0, barY, windowWidth, barHeight};
        SDL_RenderFillRect(renderer, &barRect);

        // Рисуем кадры низкого разрешения
        SDL_SetRenderDrawColor(renderer, 0, 0, 122, 122); // Синий цвет для кадров низкого разрешения
        for (size_t i = 0; i < frameIndex.size(); ++i) {
            if (frameIndex[i].type == FrameInfo::LOW_RES) {
                int x = (int)(i * windowWidth / frameIndex.size());
                int width = std::max(1, (int)((i + 1) * windowWidth / frameIndex.size()) - x);
                SDL_Rect lowResRect = {x, barY, width, barHeight};
                SDL_RenderFillRect(renderer, &lowResRect);
            }
        }

        // Рисуем кадры полного разрешения
        SDL_SetRenderDrawColor(renderer, 0, 122, 0, 122); // Зеленый цвет для кадров полного разрешения
        for (size_t i = 0; i < frameIndex.size(); ++i) {
            if (frameIndex[i].type == FrameInfo::FULL_RES) {
                int x = (int)(i * windowWidth / frameIndex.size());
                int width = std::max(1, (int)((i + 1) * windowWidth / frameIndex.size()) - x);
                SDL_Rect fullResRect = {x, barY, width, barHeight};
                SDL_RenderFillRect(renderer, &fullResRect);
            }
        }

        // Рисуем текущий кадр (воспроизводящую головку)
        SDL_SetRenderDrawColor(renderer, 200, 200, 200, 200);
        int headX = (int)(currentFrame * windowWidth / frameIndex.size());
        SDL_Rect headRect = {headX - 1, barY, 3, barHeight}; // Увеличиваем ширину головки до 3 пикселей
        SDL_RenderFillRect(renderer, &headRect);
    }

    void renderText(const std::string& text, int x, int y) {
        if (!font) return;

        SDL_Surface* surface = TTF_RenderText_Solid(font, text.c_str(), textColor);
        if (surface == nullptr) {
            std::cerr << "Не удалось создать поверхность для текста. SDL_ttf Error: " << TTF_GetError() << std::endl;
            return;
        }

        SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
        if (texture == nullptr) {
            std::cerr << "Не удалось создать текстуру из поверхности. SDL Error: " << SDL_GetError() << std::endl;
            SDL_FreeSurface(surface);
            return;
        }

        SDL_Rect renderQuad = {x, y, surface->w, surface->h};
        SDL_RenderCopy(renderer, texture, NULL, &renderQuad);

        SDL_FreeSurface(surface);
        SDL_DestroyTexture(texture);
    }

    int getTextWidth(const std::string& text) {
        if (!font) return 0;
        int w, h;
        TTF_SizeText(font, text.c_str(), &w, &h);
        return w;
    }
};


class PlaybackHead {
private:
    const std::vector<FrameInfo>& frameIndex;
    int currentFrame;
    int delayMs;  // Задержка в миллисекундах между кадрами
    SDLDisplay& display;
    AVFormatContext* formatContext;
    int videoStreamIndex;

public:
    PlaybackHead(const std::vector<FrameInfo>& fi, int fps, SDLDisplay& disp, const char* filename) 
        : frameIndex(fi), currentFrame(0), display(disp) {
        setFPS(fps);
        formatContext = nullptr;
        if (avformat_open_input(&formatContext, filename, nullptr, nullptr) != 0) {
            throw std::runtime_error("Не удалось открыть файл");
        }
        videoStreamIndex = av_find_best_stream(formatContext, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (videoStreamIndex < 0) {
            avformat_close_input(&formatContext);
            throw std::runtime_error("Не удалось найти видеопоток");
        }
    }

    ~PlaybackHead() {
        if (formatContext) {
            avformat_close_input(&formatContext);
        }
    }

    void setFPS(int fps) {
        delayMs = 1000 / fps;
    }

    void setPlaybackSpeed(double speed) {
        delayMs = static_cast<int>(1000 / (speed * 30));  // Предполагаем базовую частоту 30 кадров в секунду
    }

    bool needsDecode(int frame) const {
        return !frameIndex[frame].frame;
    }

void play(int numFrames, bool reverse = false) {
        auto lastFrameTime = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < numFrames; ++i) {
            auto now = std::chrono::high_resolution_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastFrameTime).count();

            if (elapsed < delayMs) {
                std::this_thread::sleep_for(std::chrono::milliseconds(delayMs - elapsed));
            }

            if (reverse) {
                if (currentFrame <= 0) {
                    std::cout << "\rДостигнуто начало видео" << std::flush;
                    break;
                }
                currentFrame--;
            } else {
                if (currentFrame >= frameIndex.size() - 1) {
                    std::cout << "\rДостигнут конец видео" << std::flush;
                    break;
                }
                currentFrame++;
            }

            displayCurrentFrame();
            lastFrameTime = std::chrono::high_resolution_clock::now();

            SDL_Event event;
            if (SDL_PollEvent(&event) && event.type == SDL_QUIT) {
                break;
            }
        }
        std::cout << std::endl;
    }

    void seekTo(int frame) {
        if (frame >= 0 && frame < frameIndex.size()) {
            currentFrame = frame;
            displayCurrentFrame();
            std::cout << "Перемотка к кадру " << frame << std::endl;
        } else {
            std::cout << "Недопустимый номер кадра для перемотки" << std::endl;
        }
    }

    int getCurrentFrame() const {
        return currentFrame;
    }

public:
    void displayCurrentFrame() {
        const FrameInfo& info = frameIndex[currentFrame];
        if (info.type == FrameInfo::FULL_RES && info.frame) {
            display.displayFrame(info.frame->data[0], info.frame->data[1], info.frame->data[2],
                                 info.frame->linesize[0], info.frame->linesize[1], info.frame->linesize[2],
                                 info.frame->width, info.frame->height,
                                 frameIndex, currentFrame);
            std::cout << "\rВоспроизведение кадра полного разрешения " << currentFrame 
                      << " (PTS: " << info.pts << ")" << std::flush;
        } else if (info.type == FrameInfo::LOW_RES && info.low_res_frame) {
            display.displayFrame(info.low_res_frame->data[0], info.low_res_frame->data[1], info.low_res_frame->data[2],
                                 info.low_res_frame->linesize[0], info.low_res_frame->linesize[1], info.low_res_frame->linesize[2],
                                 info.low_res_frame->width, info.low_res_frame->height,
                                 frameIndex, currentFrame);
            std::cout << "\rВоспроизведение кадра низкого разрешения " << currentFrame 
                      << " (PTS: " << info.pts << ")" << std::flush;
        } else {
            // Отображаем черный экран для пустого кадра
            static std::vector<uint8_t> blackY(320 * 240, 0);      // Y плоскость (яркость)
            static std::vector<uint8_t> blackUV(320 * 240 / 4, 128); // U и V плоскости (цветность)

            display.displayFrame(blackY.data(), blackUV.data(), blackUV.data(), 320, 160, 160,
                                 320, 240,
                                 frameIndex, currentFrame);
            std::cout << "\rВоспроизведение пустого кадра " << currentFrame 
                      << " (PTS: " << info.pts << ")" << std::flush;
        }
    }
};

#endif // MAINVID_H