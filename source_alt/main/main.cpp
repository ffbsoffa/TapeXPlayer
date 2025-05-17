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
#include <cstring> // For strerror
#include "core/decode/decode.h"
#include "core/decode/low_cached_decoder_manager.h"
#include "core/decode/full_res_decoder_manager.h"
#include "core/decode/low_res_decoder.h"
#include "common/common.h"
#include "core/display/display.h"
#include "common/fontdata.h"
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <limits.h>
#include <random>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <algorithm> // For std::transform
#include "nfd.hpp"
#include "core/remote/remote_control.h"
#include "core/menu/menu_system.h"
#include "core/decode/cached_decoder.h"
#include "../core/decode/cached_decoder_manager.h"
#include "main.h"
#include "initmanager.h"
#include "core/remote/url_handler.h"

// Pending FSTP URL processing globals
static std::string g_pendingFstpUrlPath;
static double g_pendingFstpUrlTime = -1.0;
static std::atomic<bool> g_hasPendingFstpUrl(false);

// Forward declaration for zoom event handler from display.mm
void handleZoomMouseEvent(SDL_Event& event, int windowWidth, int windowHeight, int frameWidth, int frameHeight);

// Global RemoteControl instance
RemoteControl* g_remote_control = nullptr;

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
std::atomic<bool> toggle_fullscreen_requested(false);
std::array<double, 5> memoryMarkers = {-1, -1, -1, -1, -1}; // -1 means marker is not set
bool showIndex = false;
bool showOSD = true;
std::atomic<double> previous_playback_rate(1.0);
std::string input_timecode;
bool waiting_for_timecode = false;
std::atomic<bool> seek_performed(false);
std::atomic<bool> is_force_decoding(false);
std::future<void> force_decode_future;
std::mutex frameIndexMutex;
void start_audio(const char* filename);
std::string generateTXTimecode(double time);
void smooth_speed_change();
void check_and_reset_threshold();
SeekInfo seekInfo;
std::atomic<bool> audio_initialized(false);
std::thread audio_thread;
std::thread speed_change_thread;
extern void cleanup_audio();
std::atomic<bool> speed_reset_requested(false);
float currentDisplayAspectRatio = 16.0f / 9.0f;

// Variables for deep pause
std::atomic<bool> is_deep_pause_active(false);
std::chrono::steady_clock::time_point deep_pause_timer_start;
const std::chrono::seconds DEEP_PAUSE_THRESHOLD{15};
// --- End Deep Pause Variables ---

// Variables for zoom control
std::atomic<bool> zoom_enabled(false);
std::atomic<float> zoom_factor(1.0f);
std::atomic<float> zoom_center_x(0.5f);
std::atomic<float> zoom_center_y(0.5f);
std::atomic<bool> show_zoom_thumbnail(true);

// Zoom constants
const float MAX_ZOOM_FACTOR = 10.0f;
const float MIN_ZOOM_FACTOR = 1.0f;
const float ZOOM_STEP = 1.2f;

// External variables from mainau.cpp
// extern std::vector<int16_t> audio_buffer; // REMOVED: No longer using vector buffer
extern double audio_buffer_index; // Keep this, it's now the fractional mmap index
extern std::atomic<bool> decoding_finished;
extern std::atomic<bool> decoding_completed;

// Constants for frame rate limiting (Restored)
const int TARGET_FPS = 60;
const int FRAME_DELAY = 1000 / TARGET_FPS;
const int DEEP_PAUSE_RENDER_FPS = 2; // FPS for deep pause
const int DEEP_PAUSE_FRAME_DELAY = 1000 / DEEP_PAUSE_RENDER_FPS;

// Add this at the beginning of the file with other global variables
static std::string argv0;
static std::vector<std::string> restartArgs;
bool restart_requested = false;
std::string restart_filename;
std::atomic<bool> reload_file_requested = false; // Use atomic consistent with extern declaration

// Добавляем глобальную переменную для пути текущего открытого файла
static std::string g_currentOpenFilePath = ""; // ОСТАВЛЕНО
static double g_seek_after_next_load_time = -1.0; // For URL-triggered loads with seek time

// Functions to access playback rate variables
std::atomic<double>& get_playback_rate() {
    return playback_rate;
}

std::atomic<double>& get_target_playback_rate() {
    return target_playback_rate;
}

void log(const std::string& message) {
    static std::ofstream logFile("/tmp/TapeXPlayer.log", std::ios::app);
    if (logFile.is_open()) {
    logFile << message << std::endl;
    } else {
        std::cerr << "[LOG ERROR] Could not open /tmp/TapeXPlayer.log. Message: " << message << std::endl;
    }
}

std::string get_current_timecode() {
    static char timecode[12];  // HH:MM:SS:FF\0
    
    // Get current playback time in seconds
    double current_time = current_audio_time.load();
    
    // Convert to HH:MM:SS:FF format
    int total_seconds = static_cast<int>(current_time);
    int hours = total_seconds / 3600;
    int minutes = (total_seconds % 3600) / 60;
    int seconds = total_seconds % 60;
    
    // Calculate frames (FF) from fractional part of time
    // Use real video FPS
    double fps = original_fps.load();
    if (fps <= 0) fps = 30.0;  // Use 30 as default value if FPS is not yet defined
    
    int frames = static_cast<int>((current_time - total_seconds) * fps);
    
    // Make sure frame number doesn't exceed fps-1
    frames = std::min(frames, static_cast<int>(fps) - 1);
    
    // Format string
    snprintf(timecode, sizeof(timecode), "%02d:%02d:%02d:%02d", 
             hours, minutes, seconds, frames);
    
    return std::string(timecode);
}

// Function to reset speed
void reset_to_normal_speed() {
    // First set flag for decoder
    speed_reset_requested.store(true);
    
    // Temporarily increase speed threshold to 24x during reset
    LowCachedDecoderManager::speed_threshold.store(24.0);
    
    // Then reset speed to 1x and direction forward
    target_playback_rate.store(1.0);
    is_reverse.store(false);
    
    // Set pause depending on requirements
    // is_paused.store(false); // uncomment if auto-play needed
    
    // Start a timer or trigger to restore the threshold back to 16x
    // This could be done via a separate thread, timer, or check in the main loop
    
    // Example of a check that could be placed in the main loop:
    // if speed_reset_requested is true and current_playback_rate is close to 1.0,
    // then reset speed_threshold back to 16.0 and set speed_reset_requested to false
}

// --- Вспомогательные функции для генерации ссылки --- 

// Простая функция для URL-кодирования пути
// Заменяет пробелы на %20 и некоторые другие базовые символы, если необходимо.
// Для более полной реализации можно использовать библиотеку, но для путей обычно этого достаточно.
std::string urlEncodePath(const std::string& path) {
    std::ostringstream encoded;
    // encoded << std::hex; // БЫЛО ЗАКОММЕНТИРОВАНО, НУЖНО РАСКОММЕНТИРОВАТЬ И ИСПОЛЬЗОВАТЬ ПРАВИЛЬНО

    for (char c : path) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
            encoded << c;
        } else {
            // Используем std::hex для вывода в шестнадцатеричном формате
            encoded << '%' << std::setw(2) << std::setfill('0') << std::hex << std::uppercase << static_cast<int>(static_cast<unsigned char>(c));
        }
    }
    return encoded.str();
}

// Форматирует время в HHMMSSFF
std::string formatTimeForFstpUrl(double time_in_seconds, double fps_val) {
    if (time_in_seconds < 0) time_in_seconds = 0;
    if (fps_val <= 0) fps_val = 25.0; // Безопасное значение по умолчанию

    int total_seconds_int = static_cast<int>(time_in_seconds);
    int hours = total_seconds_int / 3600;
    int minutes = (total_seconds_int % 3600) / 60;
    int seconds = total_seconds_int % 60;
    int frames = static_cast<int>((time_in_seconds - total_seconds_int) * fps_val);
    frames = std::min(frames, static_cast<int>(fps_val) -1); // Убедимся, что не превышает FPS
    if (frames < 0) frames = 0;

    std::ostringstream time_stream;
    time_stream << std::setw(2) << std::setfill('0') << hours
                << std::setw(2) << std::setfill('0') << minutes
                << std::setw(2) << std::setfill('0') << seconds
                << std::setw(2) << std::setfill('0') << frames;
    return time_stream.str();
}

// Основная функция генерации и копирования
void generateAndCopyFstpMarkdownLink() {
    if (g_currentOpenFilePath.empty()) {
        std::cout << "Cannot copy fstp link: No file is currently open." << std::endl;
        // Можно добавить OSD сообщение об ошибке позже
        return;
    }

    std::string filePath = g_currentOpenFilePath;
    double currentTime = current_audio_time.load();
    double fps = original_fps.load();

    std::cout << "[DEBUG URL Encode] Path BEFORE encoding: " << filePath << std::endl; // <--- НОВЫЙ ЛОГ
    std::string encodedPath = urlEncodePath(filePath);
    std::cout << "[DEBUG URL Encode] Path AFTER encoding: " << encodedPath << std::endl; // <--- НОВЫЙ ЛОГ

    std::string timeParam = formatTimeForFstpUrl(currentTime, fps);
    std::string displayTime = get_current_timecode(); // Используем существующую HH:MM:SS:FF

    // Собираем URL. Убедимся, что абсолютные пути начинаются с ///, если они абсолютные.
    std::string fstpUrl;
    if (!encodedPath.empty() && encodedPath[0] == '/') {
        fstpUrl = "fstp:///" + encodedPath.substr(1); // Заменяем первый / на ///
    } else {
        // Для относительных путей или если путь уже как-то отформатирован (маловероятно для g_currentOpenFilePath)
        fstpUrl = "fstp://" + encodedPath; 
    }
    fstpUrl += "&t=" + timeParam;

    // std::string markdownLink = "[" + fstpUrl + "|" + displayTime + "]"; // Old format
    std::string markdownLink = "[" + displayTime + "](" + fstpUrl + ")"; // New format

    if (SDL_SetClipboardText(markdownLink.c_str()) == 0) {
        std::cout << "Copied to clipboard: " << markdownLink << std::endl;
        // Можно добавить OSD сообщение об успехе
    } else {
        std::cerr << "Error copying to clipboard: " << SDL_GetError() << std::endl;
        // Можно добавить OSD сообщение об ошибке
    }
}

void handleMenuCommand(int command) {
    switch (command) {
        case MENU_FILE_OPEN: {
            NFD::UniquePath outPath;
            nfdfilteritem_t filterItem[1] = { { "Video files", "mp4,mov,avi,mkv,wmv,flv,webm" } };
            nfdresult_t result = NFD::OpenDialog(outPath, filterItem, 1);
            if (result == NFD_OKAY) {
                std::string filename = outPath.get();
                std::cout << "Selected file: " << filename << std::endl;
                
                // Restart player with new file
                restartPlayerWithFile(filename, -1.0);
            }
            break;
        }
        case MENU_INTERFACE_SELECT: {
            // TODO: Implement interface selection logic
            break;
        }
        case MENU_FILE_COPY_FSTP_URL_MARKDOWN: {
            generateAndCopyFstpMarkdownLink();
            break;
        }
    }
}

// Functions for zoom control
void increase_zoom() {
    float current = zoom_factor.load();
    float new_factor = current * ZOOM_STEP;
    if (new_factor > MAX_ZOOM_FACTOR) new_factor = MAX_ZOOM_FACTOR;
    zoom_factor.store(new_factor);
    
    // Enable zoom if it was disabled
    if (!zoom_enabled.load() && new_factor > MIN_ZOOM_FACTOR) {
        zoom_enabled.store(true);
    }
}

void decrease_zoom() {
    float current = zoom_factor.load();
    float new_factor = current / ZOOM_STEP;
    if (new_factor < MIN_ZOOM_FACTOR) {
        new_factor = MIN_ZOOM_FACTOR;
        zoom_enabled.store(false);
    }
    zoom_factor.store(new_factor);
}

void reset_zoom() {
    zoom_factor.store(MIN_ZOOM_FACTOR);
    zoom_center_x.store(0.5f);
    zoom_center_y.store(0.5f);
    zoom_enabled.store(false);
}

void set_zoom_center(float x, float y) {
    zoom_center_x.store(x);
    zoom_center_y.store(y);
}

void toggle_zoom_thumbnail() {
    show_zoom_thumbnail.store(!show_zoom_thumbnail.load());
}

void restartPlayerWithFile(const std::string& filename, double seek_after_load_time) {
    if (g_currentOpenFilePath == filename && !g_currentOpenFilePath.empty()) {
        std::cout << "Request to open the same file that is already open: " << filename << ". No action." << std::endl;
        // If a seek time is provided for the *same* currently open file, we should handle it here directly
        // This case is now primarily handled by processIncomingFstpUrl, but good to be aware.
        // If seek_after_load_time >= 0, perhaps we should call seek_to_time(seek_after_load_time) ?
        // For now, the logic in processIncomingFstpUrl will handle immediate seek for same file.
        // This function is more about *reloading*.
        return; 
    }
    std::cout << "Restarting player with file: " << filename 
              << (seek_after_load_time >=0 ? " and will seek to " + std::to_string(seek_after_load_time) +"s after load" : "") << std::endl;
    log("Restarting player with file: " + filename);
    
#ifdef __APPLE__
    std::cout << "[main.cpp] Before updateCopyLinkMenuState(false) in restartPlayerWithFile" << std::endl;
    updateCopyLinkMenuState(false); // Деактивируем перед перезагрузкой
    std::cout << "[main.cpp] After updateCopyLinkMenuState(false) in restartPlayerWithFile" << std::endl;
#endif
    restart_filename = filename;
    g_seek_after_next_load_time = seek_after_load_time; // Store the seek time
    reload_file_requested.store(true); 
    shouldExit.store(true); // Signal inner loop to exit
    restart_requested = false; 
}

int main(int argc, char* argv[]) {
    // Store the program path for potential relaunch
    argv0 = argv[0];
    
    restartArgs.clear();
    for (int i = 0; i < argc; ++i) {
        restartArgs.push_back(argv[i]);
    }

    // ParsedFstpUrl urlToOpen; // <--- ЗАМЕНЕНО на переменные ниже
    std::string pathFromUrl;
    double timeFromUrl = -1.0;
    bool openFileFromUrl = false;
    bool seekFromFileUrl = false;
    bool fstpUrlProcessed = false;

    std::string initialPathFromArgs; 

    if (argc > 1) {
        for (int i = 1; i < argc; ++i) {
            std::string arg_str = argv[i];
            // Используем g_currentOpenFilePath, но он еще пустой на старте.
            // original_fps.load() также будет 0.0 на старте.
            if (handleFstpUrlArgument(arg_str, g_currentOpenFilePath, original_fps.load(), pathFromUrl, timeFromUrl, openFileFromUrl, seekFromFileUrl)) {
                fstpUrlProcessed = true;
                // Log the direct outputs of handleFstpUrlArgument
                log("[FSTP DEBUG] handleFstpUrlArgument returned true. Outputs: pathFromUrl='" + pathFromUrl +
                    "', timeFromUrl=" + std::to_string(timeFromUrl) +
                    ", openFileFromUrl=" + (openFileFromUrl ? "true" : "false") +
                    ", seekFromFileUrl=" + (seekFromFileUrl ? "true" : "false") +
                    ", for input arg_str='" + arg_str + "'");
                // Если URL обработан (даже если невалидный), выходим из цикла, 
                // т.к. мы не хотим, чтобы он интерпретировался как обычный путь к файлу.
                break; 
            } else {
                // Это не fstp URL, проверяем, не обычный ли это путь
                if (arg_str.length() > 0 && arg_str[0] != '-' && initialPathFromArgs.empty()) {
                     initialPathFromArgs = arg_str;
                     std::cout << "Found potential file path argument (non-fstp): " << initialPathFromArgs << std::endl;
                }
            }
        }
    }
    
    std::string video_path_to_load;
    double time_to_seek = -1.0;
    bool should_load_new_file_from_args = false;

    if (fstpUrlProcessed) {
        // If an FSTP URL was processed and it provided a file path, prioritize loading it.
        if (!pathFromUrl.empty()) {
            video_path_to_load = pathFromUrl;
            should_load_new_file_from_args = true; // Force loading if path is present
            if (seekFromFileUrl) { // If a seek is also requested for this file
                time_to_seek = timeFromUrl;
            }
            std::cout << "[FSTP Startup] Preparing to load from fstp URL: " << video_path_to_load
                      << (time_to_seek >= 0 ? " and seek to " + std::to_string(time_to_seek) + "s" : "") << std::endl;
        } else if (seekFromFileUrl) {
            // FSTP URL provided NO path, but asked for a seek (e.g., fstp:&t=...). 
            // At startup, no file is loaded yet from the URL itself, so this seek is for an unspecified context.
            // We won't load a file based on this, but we'll store the seek time if it becomes relevant later.
            time_to_seek = timeFromUrl;
            std::cout << "[FSTP Startup] URL requests seek to: " << time_to_seek
                      << "s, but no file path was specified in the URL. No file will be loaded based on this URL." << std::endl;
        } else {
            // FSTP URL was processed, but it's invalid or has no path and no seek.
            std::cout << "[FSTP Startup] URL processed, but no file path or seek action required from it." << std::endl;
        }
    } else if (!initialPathFromArgs.empty()) {
        // This handles the case where a non-FSTP command line argument (a direct file path) was given.
        video_path_to_load = initialPathFromArgs;
        should_load_new_file_from_args = true;
        std::cout << "[Argument Startup] Preparing to load from command line argument: " << video_path_to_load << std::endl;
    }
    
    log("Program started");
    log("Number of arguments: " + std::to_string(argc));
    for (int i = 0; i < argc; ++i) {
        log("Argument " + std::to_string(i) + ": " + argv[i]);
    }
    log("Current working directory: " + std::string(getcwd(NULL, 0)));

    // Initialize remote control
    g_remote_control = new RemoteControl();
    if (!g_remote_control->initialize()) {
        std::cerr << "Warning: Failed to initialize remote control" << std::endl;
        log("Warning: Failed to initialize remote control");
    }

#ifdef __APPLE__
    // Initialize menu system
    initializeMenuSystem();
    // Изначально пункт меню неактивен, если не будет загрузки из аргументов
    // Это будет переопределено ниже, если should_load_new_file_from_args станет true
    std::cout << "[main.cpp] Before initial updateCopyLinkMenuState(false) in main()" << std::endl;
    updateCopyLinkMenuState(false); 
    std::cout << "[main.cpp] After initial updateCopyLinkMenuState(false) in main()" << std::endl;
#endif

    // SDL initialization and window creation is done once
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        std::cerr << "SDL initialization error: " << SDL_GetError() << std::endl;
        return 1;
    }

    // Initialize TTF
    if (TTF_Init() == -1) {
        std::cerr << "SDL_ttf initialization error: " << TTF_GetError() << std::endl;
        return 1;
    }

    // Load font
    SDL_RWops* rw = SDL_RWFromConstMem(font_otf, sizeof(font_otf));
    TTF_Font* font = TTF_OpenFontRW(rw, 1, 24);
    if (!font) {
        std::cerr << "Error loading font from memory: " << TTF_GetError() << std::endl;
        return 1;
    }

    // Load saved window settings
    WindowSettings windowSettings = loadWindowSettings();
    
    // Use saved settings if available, otherwise use default dimensions
    int windowWidth = windowSettings.isValid ? windowSettings.width : 1280;
    int windowHeight = windowSettings.isValid ? windowSettings.height : 720;
    int windowX = windowSettings.isValid ? windowSettings.x : SDL_WINDOWPOS_CENTERED;
    int windowY = windowSettings.isValid ? windowSettings.y : SDL_WINDOWPOS_CENTERED;

    // Set window creation flags
    Uint32 windowFlags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_METAL; // Removed HIGHDPI
    if (windowSettings.isValid && windowSettings.isFullscreen) {
        windowFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    }

    SDL_Window* window = SDL_CreateWindow("TapeXPlayer", 
        windowX, windowY, 
        windowWidth, windowHeight, 
        windowFlags);
    if (!window) {
        std::cerr << "Window creation error: " << SDL_GetError() << std::endl;
        SDL_Quit();
        return 1;
    }

    // Enable file drag and drop support
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        std::cerr << "Renderer creation error with VSync: " << SDL_GetError() << std::endl;
        // Try to create renderer without VSync
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
        if (!renderer) {
            std::cerr << "Renderer creation error without VSync: " << SDL_GetError() << std::endl;
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 1;
        }
        std::cout << "Renderer created without VSync" << std::endl;
    } else {
        std::cout << "Renderer created with VSync" << std::endl;
    }

    // Main file loading loop
    std::string currentFilename; // Используется для передачи имени файла в/из mainLoadingSequence
    // g_currentOpenFilePath будет обновляться после успешной загрузки
    
    bool firstRun = true;
    // bool fileProvided = (argc > 1); // Эта логика теперь сложнее из-за fstp
    bool fileArgProcessed = false; // Флаг, что аргумент командной строки (URL или путь) обработан

    // --- Define fixed speed steps ---
    const std::vector<double> speed_steps = {0.5, 1.0, 3.0, 10.0, 24.0};
    static int current_speed_index = 1; // Start at 1.0x (index 1)

    while (true) {
        std::string fileToLoadPath; // Путь к файлу для загрузки в этой итерации
        bool shouldAttemptFileLoadThisIteration = false;
        double initialSeekTimeForThisLoad = -1.0; // Время для перемотки *после* загрузки этого файла

        if (!fileArgProcessed && should_load_new_file_from_args) {
            fileToLoadPath = video_path_to_load; // Из URL или обычного аргумента
            initialSeekTimeForThisLoad = time_to_seek; // Запомненное время из URL
            shouldAttemptFileLoadThisIteration = true;
            fileArgProcessed = true; // Аргументы командной строки обработаны
            firstRun = false;
        } else if (firstRun && fstpUrlProcessed && !openFileFromUrl && seekFromFileUrl) {
            // Особый случай: URL при запуске указывал на текущий (гипотетически) файл и требовал перемотки.
            // Файл еще не загружен, поэтому fileToLoadPath должен быть пустым, чтобы показать "no file".
            // Перемотка будет применена, если пользователь откроет *этот* файл позже вручную.
            // Или, если бы мы знали заранее какой файл "текущий" до первой загрузки.
            // Пока что, если файл не открывается по URL, но есть seek, мы его не применяем на пустом плеере.
            // Этот блок может потребовать доработки, если нужно открывать *последний* открытый файл и мотать.
            // Текущая логика: если URL не привел к открытию файла, то на старте ничего не делаем.
            fileArgProcessed = true; // URL обработан.
            firstRun = false;
        }
        else if (reload_file_requested.load()) {
            fileToLoadPath = restart_filename; // Имя файла из запроса на перезагрузку
            initialSeekTimeForThisLoad = g_seek_after_next_load_time; // Use the stored time for next load
            g_seek_after_next_load_time = -1.0; // Reset it after use
            shouldAttemptFileLoadThisIteration = true;
            reload_file_requested.store(false);
            firstRun = false; // Если была перезагрузка, это уже не первый запуск в контексте аргументов
        } else if (firstRun) {
            // Первый запуск, но не было валидных аргументов для автоматической загрузки
            firstRun = false;
            // shouldAttemptFileLoadThisIteration остается false
            
            SDL_SetWindowTitle(window, "TapeXPlayer - No File Loaded");
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderClear(renderer);
            SDL_RenderPresent(renderer);
        }
        
        if (!shouldAttemptFileLoadThisIteration) {
#ifdef __APPLE__
            std::cout << "[main.cpp] Before updateCopyLinkMenuState(false) in no-file loop" << std::endl;
            updateCopyLinkMenuState(false); // <--- ВЫКЛЮЧИТЬ в цикле "нет файла"
            std::cout << "[main.cpp] After updateCopyLinkMenuState(false) in no-file loop" << std::endl;
#endif
            bool noFileLoaded = true;
            
            while (noFileLoaded && !shouldExit) {
                // Check for pending FSTP URL at the beginning of the loop
                if (g_hasPendingFstpUrl.load()) {
                    log("[main.cpp] Detected pending FSTP URL in noFileLoaded loop: " + g_pendingFstpUrlPath);
                    if (!g_pendingFstpUrlPath.empty()) {
                        restart_filename = g_pendingFstpUrlPath;
                        g_seek_after_next_load_time = g_pendingFstpUrlTime;
                        
                        reload_file_requested.store(true); // Signal the outer loop to load this file
                        noFileLoaded = false; // Exit this "no file loaded" loop
                    }
                    // Reset pending URL state
                    g_hasPendingFstpUrl.store(false);
                    g_pendingFstpUrlPath.clear();
                    g_pendingFstpUrlTime = -1.0;
                    continue; // Re-evaluate while condition; should exit this loop and proceed to load
                }

                Uint32 frameStart = SDL_GetTicks();
                
                // Process remote commands at the start of each frame
                if (g_remote_control->is_initialized()) {
                    g_remote_control->process_commands();
                }
                
                // Handle fullscreen toggle request from menu
                if (toggle_fullscreen_requested.load()) {
                    toggle_fullscreen_requested.store(false);
                    Uint32 flags = SDL_GetWindowFlags(window);
                    if (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) {
                        SDL_SetWindowFullscreen(window, 0);
                    } else {
                        SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
                    }
                }
                
                SDL_Event e;
                while (SDL_PollEvent(&e)) {
                    if (e.type == SDL_QUIT) {
                        saveWindowSettings(window);
                        quit = true;
                        shouldExit = true;
                        restart_requested = false; // Cancel restart on window close
                    } else if (e.type == SDL_KEYDOWN) {
                        switch (e.key.keysym.sym) {
                            case SDLK_o:
                                // Ctrl+O to open file
                                if (e.key.keysym.mod & KMOD_CTRL) {
                                    NFD::UniquePath outPath;
                                    nfdfilteritem_t filterItem[1] = { { "Video files", "mp4,mov,avi,mkv,wmv,flv,webm" } };
                                    nfdresult_t result = NFD::OpenDialog(outPath, filterItem, 1);
                                    if (result == NFD_OKAY) {
                                        std::string filename = outPath.get();
                                        std::cout << "Selected file: " << filename << std::endl;
                                        
                                        // Restart player with new file
                                        restartPlayerWithFile(filename, -1.0);
                                    }
                                }
                                break;
                            case SDLK_ESCAPE:
                                saveWindowSettings(window);
                                shouldExit = true;
                                restart_requested = false; // Cancel restart on Escape
                                break;
                        }
                    }
                }
                
                // Clear the screen with a black background
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
                SDL_RenderClear(renderer);
                
                // Use renderOSD to display placeholder values for timecode and playback direction
                // Parameters: renderer, font, isPlaying, playbackRate, isReverse, currentTime, frameNumber, 
                // showOSD, waiting_for_timecode, input_timecode, original_fps, jog_forward, jog_backward, 
                // Removed isLoading, loadingType, loadingProgress
                renderOSD(renderer, font, false, 0.0, false, 0.0, 0, true, false, "", 25.0, false, false);
                
                // Render a small hint at the center of the screen
                SDL_Color textColor = {200, 200, 200, 200};
                SDL_Surface* messageSurface = TTF_RenderText_Blended(font, "Press Ctrl+O to open a file", textColor);
                if (messageSurface) {
                    SDL_Texture* messageTexture = SDL_CreateTextureFromSurface(renderer, messageSurface);
                    if (messageTexture) {
                        SDL_Rect messageRect = {
                            windowWidth / 2 - messageSurface->w / 2,
                            windowHeight / 2 - messageSurface->h / 2,
                            messageSurface->w,
                            messageSurface->h
                        };
                        
                        SDL_RenderCopy(renderer, messageTexture, NULL, &messageRect);
                        SDL_DestroyTexture(messageTexture);
                    }
                    SDL_FreeSurface(messageSurface);
                }
                
                SDL_RenderPresent(renderer);
                
                // Cap the frame rate
                Uint32 frameTime = SDL_GetTicks() - frameStart;
                if (frameTime < FRAME_DELAY) {
                    SDL_Delay(FRAME_DELAY - frameTime);
                }
            }

            // If the user requested to exit from the 'no file loaded' state,
            // AND it wasn't a request to reload a file, break the main loop
            if (shouldExit && !reload_file_requested.load()) { 
                break;
            }
        }
        
        if (shouldAttemptFileLoadThisIteration) {
            resetPlayerState(); // Сброс состояния перед загрузкой нового файла
            
            // currentFilename здесь будет обновлен функцией mainLoadingSequence
            // g_currentOpenFilePath будет обновлен *после* успешной загрузки
            
            // Initialize variables for the playback loop that need to be passed to/from loading sequence
            std::vector<FrameInfo> frameIndex; // Needs to be populated by loading sequence
            std::unique_ptr<FullResDecoderManager> fullResManagerPtr;
            std::unique_ptr<LowCachedDecoderManager> lowCachedManagerPtr;
            std::unique_ptr<CachedDecoderManager> cachedManagerPtr;
            std::atomic<int> currentFrame(0); // Will be reset inside loading sequence
            std::atomic<bool> isPlaying(false); // Will be set inside loading sequence

            // Create LoadingStatus object for this loading operation
            LoadingStatus loadingStatus;

            // Call the loading sequence function asynchronously
            std::future<bool> loading_future = mainLoadingSequence(
                renderer, window,
                loadingStatus, 
                fileToLoadPath, // Используем fileToLoadPath
                currentFilename, // currentFilename будет ЗАПОЛНЕН функцией
                frameIndex, fullResManagerPtr, lowCachedManagerPtr, cachedManagerPtr,
                currentFrame, isPlaying
            );

            // --- Show Loading Screen while waiting for the future --- 
            SDL_SetWindowTitle(window, "TapeXPlayer - Loading..."); // Set loading title
            while (loading_future.wait_for(std::chrono::milliseconds(10)) != std::future_status::ready) {
                // Render loading screen
                renderLoadingScreen(renderer, font, const_cast<const LoadingStatus&>(loadingStatus));

                // Handle essential events (like SDL_QUIT) during loading
                SDL_Event e;
                while (SDL_PollEvent(&e)) {
                    if (e.type == SDL_QUIT) {
                        // Need to signal the loading thread to stop if possible
                        // This requires adding stop logic to mainLoadingSequence/its sub-tasks
                        // For now, just set quit flags and break loops
                        quit = true;
                        shouldExit = true; 
                        // Optionally try to cancel the future if the task supports cancellation
                        goto loading_exit; // Exit loading loop immediately
                    } 
                    // Handle other minimal events if necessary
                }
                if (quit.load()) goto loading_exit; // Exit if quit was set

                // Small delay to prevent busy-waiting
                 // SDL_Delay(10); // wait_for already provides a delay
            }
            loading_exit:; // Label for exiting the loading loop

            // Check if we exited due to quit signal
            if (quit.load()) {
                // Cleanup resources acquired before loading started if necessary
                continue; // Go to the start of the outer loop, which will handle exit
            }

            // Loading finished, get the result
            bool loading_success = loading_future.get();

             if (!loading_success) {
                  // Loading failed, let the outer loop handle showing the "Press Ctrl+O" screen
                   continue; // Go to the start of the outer while(true) loop
              }

            // --- Loading successful, update window title --- 
            g_currentOpenFilePath = currentFilename; // <--- ОБНОВЛЯЕМ путь к текущему открытому файлу
            std::string filename_only = std::filesystem::path(g_currentOpenFilePath).filename().string();
            std::string windowTitle = "TapeXPlayer - " + filename_only;
            SDL_SetWindowTitle(window, windowTitle.c_str());

#ifdef __APPLE__
            std::cout << "[main.cpp] Before updateCopyLinkMenuState(true) after successful load" << std::endl;
            updateCopyLinkMenuState(true); // <--- Enable the menu item
            std::cout << "[main.cpp] After updateCopyLinkMenuState(true) after successful load" << std::endl;
#endif

             // --- Start Decoder Manager Threads (after successful load and no quit signal) ---
                if (fullResManagerPtr) {
                    fullResManagerPtr->run(); 
                    // Initial check based on starting window size
                    int initialWidth, initialHeight;
                    SDL_GetWindowSize(window, &initialWidth, &initialHeight);
                    fullResManagerPtr->checkWindowSizeAndToggleActivity(initialWidth, initialHeight);
                }
                if (lowCachedManagerPtr) lowCachedManagerPtr->run(); 
             if (cachedManagerPtr) cachedManagerPtr->run();
             // -------------------------------------------------------------------------

             // --- Duration handling moved outside loading sequence for simplicity ---
             // Let's get duration after successful load if needed immediately
             double duration = get_file_duration(currentFilename.c_str());
             total_duration.store(duration);
             std::cout << "Total duration: " << total_duration.load() << " seconds" << std::endl;

            // Start playback after a short delay and manager setup
            std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Shorter delay maybe?
             // Ensure playback starts paused (already handled by isPlaying init and loading func)
            target_playback_rate.store(0.0); 
            playback_rate.store(0.0); // Ensure current rate is also 0

            // Perform initial seek if requested (e.g., from URL)
            if (initialSeekTimeForThisLoad >= 0.0) {
                std::cout << "[main.cpp] Performing initial seek to: " << initialSeekTimeForThisLoad << "s after successful load." << std::endl;
                log("[main.cpp] Performing initial seek to: " + std::to_string(initialSeekTimeForThisLoad) + "s after successful load.");
                seek_to_time(initialSeekTimeForThisLoad);
                // Важно дождаться завершения перемотки перед тем, как начнется основной цикл,
                // чтобы current_audio_time успел обновиться, и первый кадр был правильным.
                // Цикл ожидания seekInfo.completed можно добавить здесь, если необходимо.
                // Однако, seek_to_time уже должен обновить current_audio_time достаточно быстро.
            }

             // --- Start of the inner playback loop ---
            while (!shouldExit) {
                // Process remote commands at the start of each frame
                if (g_remote_control->is_initialized()) {
                    g_remote_control->process_commands();
                }
                
                bool deep_pause_interrupt_for_immediate_refresh = false; // Flag for this iteration

                // Check if we need to reset the speed threshold
                check_and_reset_threshold();

                // Handle fullscreen toggle request from menu
                if (toggle_fullscreen_requested.load()) {
                    toggle_fullscreen_requested.store(false);
                    Uint32 flags = SDL_GetWindowFlags(window);
                    if (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) {
                        SDL_SetWindowFullscreen(window, 0);
                    } else {
                        SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
                    }
                }

                Uint32 frameStart = SDL_GetTicks();

                SDL_Event e;
                while (SDL_PollEvent(&e)) {
                    if (e.type == SDL_QUIT) {
                        saveWindowSettings(window);
                        quit = true;
                        shouldExit = true;
                        restart_requested = false; // Cancel restart on window close
                        if(is_deep_pause_active.load()) deep_pause_interrupt_for_immediate_refresh = true;
                    } else if (e.type == SDL_KEYDOWN) {
                        if (waiting_for_timecode) {
                            if (e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_KP_ENTER) {
                                try {
                                    double target_time = parse_timecode(input_timecode);
                                    seek_to_time(target_time);
                                    waiting_for_timecode = false;
                                    input_timecode.clear();
                                } catch (const std::exception& ex) {
                                }
                            } else if (e.key.keysym.sym == SDLK_BACKSPACE && !input_timecode.empty()) {
                                input_timecode.pop_back();
                            } else if (input_timecode.length() < 8) {
                                const char* keyName = SDL_GetKeyName(e.key.keysym.sym);
                                char digit = '\0';
                                
                                if (keyName[0] >= '0' && keyName[0] <= '9' && keyName[1] == '\0') {
                                    // Regular number keys
                                    digit = keyName[0];
                                } else if (strncmp(keyName, "Keypad ", 7) == 0 && keyName[7] >= '0' && keyName[7] <= '9' && keyName[8] == '\0') {
                                    // NumPad keys
                                    digit = keyName[7];
                                }
                                
                                if (digit != '\0') {
                                    input_timecode += digit;
                                }
                            } else if (e.key.keysym.sym == SDLK_ESCAPE) {
                                waiting_for_timecode = false;
                                input_timecode.clear();
                            }
                        } else {
                            switch (e.key.keysym.sym) {
                                case SDLK_SPACE:
                                    // Regular pause/play
                                    if (std::abs(playback_rate.load()) > 1.1) {
                                        // If speed is above 1.1x, first reset speed
                                        reset_to_normal_speed();
                                        // --- Reset speed index to 1.0x --- 
                                        current_speed_index = 1; // Index for 1.0x in speed_steps
                                        // --- End Reset speed index --- 
                                    } else {
                                        toggle_pause();
                                        // --- Deep Pause Logic on Toggle ---
                                        if (target_playback_rate.load() == 0.0) { // Just paused
                                            deep_pause_timer_start = std::chrono::steady_clock::now();
                                            is_deep_pause_active.store(false); // Ensure not in deep pause when toggling to pause
                                        } else { // Just unpaused
                                            is_deep_pause_active.store(false);
                                            deep_pause_interrupt_for_immediate_refresh = true; // Force refresh on unpause
                                        }
                                        // --- End Deep Pause Logic on Toggle ---
                                    }
                                    break;
                                case SDLK_RIGHT:
                                    if (e.key.keysym.mod & KMOD_SHIFT) {
                                        if (e.key.repeat == 0) { // Check if it's not a repeat event
                                            start_jog_forward();
                                        }
                                    } else {
                                        // Regular right arrow behavior
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
                                        // Regular left arrow behavior
                                        is_reverse.store(!is_reverse.load());
                                    }
                                    break;
                                case SDLK_UP:
                                    if (current_speed_index < speed_steps.size() - 1) {
                                        current_speed_index++;
                                    }
                                    target_playback_rate.store(speed_steps[current_speed_index]);
                                    break;
                                case SDLK_DOWN:
                                    if (current_speed_index > 0) {
                                        current_speed_index--;
                                    }
                                    target_playback_rate.store(speed_steps[current_speed_index]);
                                    break;
                                case SDLK_r:
                                    is_reverse.store(!is_reverse.load());
                                    break;
                                case SDLK_ESCAPE:
                                    saveWindowSettings(window);
                                    shouldExit = true;
                                    restart_requested = false; // Cancel restart on Escape
                                    if(is_deep_pause_active.load()) deep_pause_interrupt_for_immediate_refresh = true;
                                    break;
                                case SDLK_PLUS:
                                case SDLK_EQUALS:
                                    increase_volume();
                                    break;
                                case SDLK_MINUS:
                                    decrease_volume();
                                    break;
                                case SDLK_o:
                                    // Ctrl+O to open file
                                    if (e.key.keysym.mod & KMOD_CTRL) {
                                        NFD::UniquePath outPath;
                                        nfdfilteritem_t filterItem[1] = { { "Video files", "mp4,mov,avi,mkv,wmv,flv,webm" } };
                                        nfdresult_t result = NFD::OpenDialog(outPath, filterItem, 1);
                                        if (result == NFD_OKAY) {
                                            std::string filename = outPath.get();
                                            std::cout << "Selected file: " << filename << std::endl;
                                            
                                            // Restart player with new file
                                            restartPlayerWithFile(filename, -1.0);
                                            if(is_deep_pause_active.load()) deep_pause_interrupt_for_immediate_refresh = true; 
                                        }
                                    }
                                    break;
                                case SDLK_d:
                                    if (e.key.keysym.mod & KMOD_ALT) {
                                        showOSD = !showOSD;
                                    } else if (SDL_GetModState() & KMOD_SHIFT) {
                                        showIndex = !showIndex;
                                    }
                                    break;
                                case SDLK_g:
                                    waiting_for_timecode = true;
                                    break;
                                case SDLK_1:
                                case SDLK_2:
                                case SDLK_3:
                                case SDLK_4:
                                case SDLK_5:
                                    {
                                        // Check if we're not in timecode input mode
                                        if (!waiting_for_timecode) {
                                            int markerIndex = e.key.keysym.sym - SDLK_1;
                                            if (e.key.keysym.mod & KMOD_ALT) {
                                                // Set marker
                                                memoryMarkers[markerIndex] = current_audio_time.load();
                                                std::cout << "Marker " << (markerIndex + 1) << " set at " 
                                                        << generateTXTimecode(memoryMarkers[markerIndex]) << std::endl;
                                            } else {
                                                // Jump to marker
                                                if (memoryMarkers[markerIndex] >= 0) {
                                                    seek_to_time(memoryMarkers[markerIndex]);
                                                }
                                            }
                                        }
                                    }
                                    break;
                                case SDLK_f:
                                    // Toggle fullscreen mode
                                    {
                                        Uint32 flags = SDL_GetWindowFlags(window);
                                        if (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) {
                                            SDL_SetWindowFullscreen(window, 0);
                                        } else {
                                            SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
                                        }
                                    }
                                    break;
                                case SDLK_z:
                                    // Toggle zoom mode
                                    if (!zoom_enabled.load()) {
                                        int mouseX, mouseY;
                                        SDL_GetMouseState(&mouseX, &mouseY);
                                        // Pass texture dimensions to the function
                                        handleZoomMouseEvent(e, windowWidth, windowHeight, get_last_texture_width(), get_last_texture_height()); 
                                    } else {
                                        reset_zoom(); // Disable zoom if it was enabled
                                    }
                                    zoom_enabled.store(!zoom_enabled.load());
                                    break;
                                case SDLK_t:
                                    // Toggle thumbnail display
                                    toggle_zoom_thumbnail();
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
                            // Notify FullResDecoderManager about the resize
                            if (fullResManagerPtr) {
                                fullResManagerPtr->checkWindowSizeAndToggleActivity(windowWidth, windowHeight);
                            }
                        }
                    } else if (e.type == SDL_DROPFILE) {
                        // Handle file drop
                        char* droppedFile = e.drop.file;
                        if (droppedFile) {
                            std::string filename = droppedFile;
                            std::cout << "File dropped: " << filename << std::endl;
                            log("File dropped: " + filename);
                            
                            // Free memory allocated by SDL
                            SDL_free(droppedFile);
                            
                            // Check file extension
                            std::string extension = filename.substr(filename.find_last_of(".") + 1);
                            std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
                            
                            if (extension == "mp4" || extension == "mov" || extension == "avi" || extension == "mkv" || extension == "wmv" || extension == "flv" || extension == "webm") {
                                // Restart player with new file
                                restartPlayerWithFile(filename, -1.0);
                            } else {
                                std::cerr << "Unsupported file format: " << extension << std::endl;
                                log("Unsupported file format: " + extension);
                            }
                        }
                    } else if (e.type == SDL_MOUSEMOTION || e.type == SDL_MOUSEWHEEL || e.type == SDL_MOUSEBUTTONDOWN) { // Re-enabled Mouse Events
                        // Handle mouse events for zooming
                        int windowWidth, windowHeight;
                        SDL_GetRendererOutputSize(renderer, &windowWidth, &windowHeight);
                        
                        // Get current frame dimensions from frame buffer
                        // --- Get frame dimensions using getters from display.mm --- 
                        int frameWidth = get_last_texture_width(); 
                        int frameHeight = get_last_texture_height();
                        // --- End Get frame dimensions ---
                        
                        // If frame dimensions are known, handle mouse events
                        if (frameWidth > 0 && frameHeight > 0) {
                            handleZoomMouseEvent(e, windowWidth, windowHeight, frameWidth, frameHeight);
                        }
                    }
                }

                // Update currentFrame based on current audio time
                double currentTime = current_audio_time.load();
                int64_t target_time_ms = static_cast<int64_t>(currentTime * 1000.0);
                int newCurrentFrame = findClosestFrameIndexByTime(frameIndex, target_time_ms);
                 if (!frameIndex.empty()) { newCurrentFrame = std::max(0, std::min(newCurrentFrame, static_cast<int>(frameIndex.size()) - 1)); } else { newCurrentFrame = 0; }
                 
                 // Notify managers ONLY if the frame has actually changed
                 if (newCurrentFrame != currentFrame.load()) {
                     currentFrame.store(newCurrentFrame);
                     if (fullResManagerPtr) fullResManagerPtr->notifyFrameChange();
                     if (lowCachedManagerPtr) lowCachedManagerPtr->notifyFrameChange();
                     if (cachedManagerPtr) cachedManagerPtr->notifyFrameChange();
                 }

                // --- Deep Pause Activation Check ---
                if (playback_rate.load() == 0.0 && target_playback_rate.load() == 0.0 && !is_deep_pause_active.load()) {
                    if (std::chrono::steady_clock::now() - deep_pause_timer_start > DEEP_PAUSE_THRESHOLD) {
                        is_deep_pause_active.store(true);
                        // Potentially set a flag here to force one OSD update if needed
                    }
                }
                // If playback starts, ensure deep pause is off
                if (playback_rate.load() != 0.0 && is_deep_pause_active.load()) {
                    is_deep_pause_active.store(false);
                }
                // --- End Deep Pause Activation Check ---

                // --- Frame Selection Logic ---
                std::shared_ptr<AVFrame> frameToDisplay = nullptr;
                FrameInfo::FrameType frameTypeToDisplay = FrameInfo::EMPTY;
                double currentPlaybackRate = std::abs(playback_rate.load());
                if (newCurrentFrame >= 0 && newCurrentFrame < frameIndex.size())
                {
                    const FrameInfo &currentFrameInfo = frameIndex[newCurrentFrame];
                    std::unique_lock<std::mutex> lock(currentFrameInfo.mutex);
                    if (!currentFrameInfo.is_decoding)
                    {
                        if (currentPlaybackRate <= 1.1)
                        {
                            if (currentFrameInfo.frame)
                            {
                                frameToDisplay = currentFrameInfo.frame;
                                frameTypeToDisplay = FrameInfo::FULL_RES;
                            }
                            else if (currentFrameInfo.low_res_frame)
                            {
                                frameToDisplay = currentFrameInfo.low_res_frame;
                                frameTypeToDisplay = FrameInfo::LOW_RES;
                            }
                            else if (currentFrameInfo.cached_frame)
                            {
                                frameToDisplay = currentFrameInfo.cached_frame;
                                frameTypeToDisplay = FrameInfo::CACHED;
                            }
                            lock.unlock();
                        }
                        else
                        {
                            if (currentFrameInfo.low_res_frame)
                            {
                                frameToDisplay = currentFrameInfo.low_res_frame;
                                frameTypeToDisplay = FrameInfo::LOW_RES;
                            }
                            else if (currentFrameInfo.cached_frame)
                            {
                                frameToDisplay = currentFrameInfo.cached_frame;
                                frameTypeToDisplay = FrameInfo::CACHED;
                            }
                            else
                            {
                                lock.unlock();
                                bool isForward = playback_rate.load() >= 0;
                                int step = isForward ? 1 : 1;
                                int searchRange = 15;
                                for (int i = 1; i <= searchRange; ++i)
                                {
                                    int checkFrameIdx = newCurrentFrame + (i * step);
                                    if (checkFrameIdx >= 0 && checkFrameIdx < frameIndex.size())
                                    {
                                        std::lock_guard<std::mutex> search_lock(frameIndex[checkFrameIdx].mutex);
                                        if (!frameIndex[checkFrameIdx].is_decoding)
                                        {
                                            if (frameIndex[checkFrameIdx].low_res_frame)
                                            {
                                                frameToDisplay = frameIndex[checkFrameIdx].low_res_frame;
                                                frameTypeToDisplay = FrameInfo::LOW_RES;
                                                break;
                                            }
                                            else if (frameIndex[checkFrameIdx].cached_frame)
                                            {
                                                frameToDisplay = frameIndex[checkFrameIdx].cached_frame;
                                                frameTypeToDisplay = FrameInfo::CACHED;
                                                break;
                                            }
                                        }
                                    }
                                    else
                                    {
                                        break;
                                    }
                                }
                            }
                            if (lock.owns_lock())
                            {
                                lock.unlock();
                            }
                        }
                    }
                    else
                    {
                        lock.unlock();
                    }
                }

                // Check if seek is complete
                if (seekInfo.completed.load()) {
                    seekInfo.completed.store(false);
                    seek_performed.store(false);
                }

                // --- Determine highResWindowSize and ringBufferCapacity ---
                 // Note: These might be better calculated once during loading or dynamically based on needs
                 int highResWindowSize = 700; // Default or recalculate based on fps if needed here
                 double fps_local = original_fps.load();
                 if (fps_local > 55.0) { highResWindowSize = 1400; } else if (fps_local > 45.0) { highResWindowSize = 1200; } else if (fps_local > 28.0) { highResWindowSize = 700; } else { highResWindowSize = 600; }
                 const size_t ringBufferCapacity = 2000; // Could also be adaptive

                // --- Update currentDisplayAspectRatio from the decoder ---
                if (fullResManagerPtr && fullResManagerPtr->getDecoder()) {
                    currentDisplayAspectRatio = fullResManagerPtr->getDecoder()->getDisplayAspectRatio();
                } else {
                    // Fallback or if no decoder (e.g. no file loaded), keep default or set a sane default
                    // For instance, if no file is loaded, this part of the loop might not even be reached,
                    // but as a safety, we can ensure it's a common aspect ratio.
                    // The global currentDisplayAspectRatio is already 16.0f/9.0f by default.
                }

                displayFrame(renderer, frameIndex, newCurrentFrame, frameToDisplay, frameTypeToDisplay,
                            true, // enableHighResDecode - Assuming true for now
                             playback_rate.load(), current_audio_time.load(), total_duration.load(),
                            showIndex, showOSD, font, isPlaying, is_reverse.load(), waiting_for_timecode,
                            input_timecode, original_fps.load(), jog_forward, jog_backward,
                            ringBufferCapacity, highResWindowSize, 950, // Pass calculated sizes
                            currentDisplayAspectRatio,
                            is_deep_pause_active.load()); // Pass deep pause state

                // Limit frame rate
                Uint32 frameTime = SDL_GetTicks() - frameStart;
                int currentTargetFrameDelay = FRAME_DELAY; // Default delay
                if (is_deep_pause_active.load() && !deep_pause_interrupt_for_immediate_refresh) {
                    currentTargetFrameDelay = DEEP_PAUSE_FRAME_DELAY;
                }

                if (frameTime < currentTargetFrameDelay) {
                    SDL_Delay(currentTargetFrameDelay - frameTime);
                }

                // Handle seek request
                if (seek_performed.load()) {
                    seekInfo.time.store(current_audio_time.load());
                    seekInfo.requested.store(true);
                    seekInfo.completed.store(false);
                    // Notify managers immediately on seek request
                     if (fullResManagerPtr) fullResManagerPtr->notifyFrameChange();
                     if (lowCachedManagerPtr) lowCachedManagerPtr->notifyFrameChange();
                     if (cachedManagerPtr) cachedManagerPtr->notifyFrameChange();
                }

                // Exit condition check for the inner loop
                if (shouldExit.load()) {
                    break; 
                } 

            } // --- End of the inner playback loop ---


            // --- Cleanup after inner loop exits (either by shouldExit or reaching end of file naturally?) ---
#ifdef __APPLE__
            std::cout << "[main.cpp] Before updateCopyLinkMenuState(false) in playback loop cleanup" << std::endl;
            updateCopyLinkMenuState(false); // <--- ВЫКЛЮЧИТЬ перед очисткой и выходом из этого цикла загрузки
            std::cout << "[main.cpp] After updateCopyLinkMenuState(false) in playback loop cleanup" << std::endl;
#endif
            std::cout << "[Cleanup] Stopping managers..." << std::endl;
            if (fullResManagerPtr) fullResManagerPtr->stop();
            if (lowCachedManagerPtr) lowCachedManagerPtr->stop();
            if (cachedManagerPtr) cachedManagerPtr->stop();
            std::cout << "[Cleanup] Managers stopped." << std::endl; // Debug log
            
            std::cout << "[Cleanup] Joining speed change thread..." << std::endl; // Debug log
            if (speed_change_thread.joinable()) {
                 // Ensure smooth_speed_change loop checks shouldExit/quit
                try {
                speed_change_thread.join();
                    std::cout << "[Cleanup] Speed change thread joined." << std::endl; // Debug log
                } catch (const std::system_error& e) {
                     std::cerr << "[Cleanup] Error joining speed_change_thread: " << e.what() << " (" << e.code() << ")" << std::endl;
                     // Consider if detaching is an option here as a last resort
                 }
            }
            

            // Cleanup audio resources
            std::cout << "[Cleanup] Cleaning audio..." << std::endl; // Debug log
            cleanup_audio();
            
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            // Clean up temporary files before potentially loading new file
            std::cout << "[Cleanup] Cleaning temp files..." << std::endl; // Debug log
            cleanupTempFiles();
            
            // If there was a request to exit the program (not reload file), break the outer loop
            if (!reload_file_requested.load() && !restart_requested) {
                 // Ensure shouldExit is true so the outer loop terminates correctly
                 shouldExit = true; // Set explicitly for clarity
                 break; // Break the outer while(true) loop
            }
            // If reload or restart was requested, the outer loop will continue,
            // and reload_file_requested will be handled at the beginning.
        } // End if (shouldAttemptFileLoadThisIteration)
    } // End outer while(true) loop

    // Save final window settings before closing
    saveWindowSettings(window);

    // Cleanup display resources (including Metal) BEFORE destroying SDL renderer/window
    cleanupDisplayResources();

    // Free TTF resources
    if (font) {
        TTF_CloseFont(font);
        font = nullptr;
    }

    TTF_Quit();

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    // Before exiting
#ifdef __APPLE__
    cleanupMenuSystem();
#endif

    // Before exiting, cleanup remote control
    if (g_remote_control) {
        delete g_remote_control;
        g_remote_control = nullptr;
    }

    // If restart was requested (not file reload), launch program with new file
    if (restart_requested) {
        std::cout << "Performing restart with file: " << restart_filename << std::endl;
        log("Performing restart with file: " + restart_filename);
        
        // Prepare arguments for restart
        std::vector<char*> args;
        args.push_back(const_cast<char*>(argv0.c_str())); // Program path
        args.push_back(const_cast<char*>(restart_filename.c_str())); // New file
        args.push_back(nullptr); // Terminating nullptr
        
        // Perform restart
#ifdef _WIN32
        // Use _execv for Windows
        _execv(argv0.c_str(), args.data());
#else
        // Use execv for UNIX-like systems
        execv(argv0.c_str(), args.data());
#endif
        
        // If execv returns, an error occurred
        std::cerr << "Failed to restart program: " << strerror(errno) << std::endl;
        log("Failed to restart program: " + std::string(strerror(errno)));
    }

    return 0;
}

// Add this check in the appropriate update loop
void check_and_reset_threshold() {
    if (speed_reset_requested.load() && fabs(playback_rate.load() - 1.0) < 0.1) {
        // Speed is close to 1.0x now, reset the threshold back to default
        LowCachedDecoderManager::speed_threshold.store(16.0);
        speed_reset_requested.store(false);
    }
}

// Function called from Objective-C (via menu_system.h bridge) when app opens an fstp:// URL
void processIncomingFstpUrl(const char* url_c_str) {
    log("[FSTP Event] processIncomingFstpUrl called with URL: " + std::string(url_c_str));

    std::string urlString(url_c_str);
    std::cout << "[main.cpp] processIncomingFstpUrl received: " << urlString << std::endl;
    log("[main.cpp] processIncomingFstpUrl received: " + urlString);

    std::string pathFromUrl;
    double timeFromUrl = -1.0;
    bool openFileFromUrl = false;
    bool seekFromFileUrl = false;

    // Use original_fps.load() which should be valid if a file is already playing.
    // If no file is playing, it'll be 0, and handleFstpUrlArgument should cope or use a default.
    if (handleFstpUrlArgument(urlString, g_currentOpenFilePath, original_fps.load(), 
                              pathFromUrl, timeFromUrl, openFileFromUrl, seekFromFileUrl)) {
        
        log("[FSTP Event DEBUG] handleFstpUrlArgument (from processIncomingFstpUrl) returned true. Outputs: pathFromUrl='" + pathFromUrl +
            "', timeFromUrl=" + std::to_string(timeFromUrl) +
            ", openFileFromUrl=" + (openFileFromUrl ? "true" : "false") +
            ", seekFromFileUrl=" + (seekFromFileUrl ? "true" : "false") +
            ", for input urlString='" + urlString + "'");

        std::cout << "[main.cpp] Parsed FSTP URL: path='" << pathFromUrl 
                  << "', time=" << timeFromUrl 
                  << ", openFile=" << openFileFromUrl 
                  << ", seekFile=" << seekFromFileUrl << std::endl;

        if (!openFileFromUrl && seekFromFileUrl && !pathFromUrl.empty() && pathFromUrl == g_currentOpenFilePath) {
            std::cout << "[main.cpp] URL: Same file already open ('" << g_currentOpenFilePath << "'), seeking to " << timeFromUrl << "s." << std::endl;
            log("[main.cpp] URL: Same file, seeking to " + std::to_string(timeFromUrl));
            seek_to_time(timeFromUrl);
        } else if (openFileFromUrl && !pathFromUrl.empty()) {
            std::cout << "[main.cpp] URL: Request to open file '" << pathFromUrl << "'." 
                      << (seekFromFileUrl ? " Will seek to " + std::to_string(timeFromUrl) + "s after load." : " No initial seek specified.") << std::endl;
            log("[FSTP Event] Storing pending FSTP URL for main loop: " + pathFromUrl + (seekFromFileUrl ? " and seeking to " + std::to_string(timeFromUrl) : ""));
            
            // Store for the main loop to pick up if it's in the "no file loaded" state or if it's already running.
            g_pendingFstpUrlPath = pathFromUrl;
            g_pendingFstpUrlTime = seekFromFileUrl ? timeFromUrl : -1.0;
            g_hasPendingFstpUrl.store(true);

            // DO NOT call restartPlayerWithFile here. Let the main loop handle it via the flags.
            // This is crucial for the "no file loaded" state to reliably pick up the pending URL.

        } else if (seekFromFileUrl && (pathFromUrl.empty() || pathFromUrl != g_currentOpenFilePath)) {
            // This case implies a URL like fstp:&t=xxx or fstp://DIFFERENT_FILE&t=xxx 
            // where we are not opening the file but a seek is requested.
            // If pathFromUrl is empty, it was likely just fstp:&t=... which is ambiguous without a current file context from the URL itself.
            // If pathFromUrl is different, we shouldn't seek in the *current* g_currentOpenFilePath.
            std::cout << "[main.cpp] URL: Seek requested for a file ('" << pathFromUrl 
                      << "') not currently open or ambiguous. Current file: '" << g_currentOpenFilePath << "'. Ignoring seek." << std::endl;
            log("[main.cpp] URL: Seek requested for a file not currently open or ambiguous. Ignoring seek.");
        } else {
            std::cout << "[main.cpp] URL: Processed, but no specific action (open/seek) taken based on current state or URL content." << std::endl;
            log("[main.cpp] URL: No specific open/seek action taken.");
        }
    } else {
        std::cout << "[main.cpp] URL: Not a valid fstp URL or failed to parse: " << urlString << std::endl;
        log("[main.cpp] URL: Not a valid fstp URL or failed to parse: " + urlString);
    }
}
