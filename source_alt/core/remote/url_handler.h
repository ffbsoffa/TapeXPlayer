#ifndef URL_HANDLER_H
#define URL_HANDLER_H

#include <string>
#include <vector> // Required for parseFstpUrl (used by std::stringstream in its typical implementation)
#include <sstream> // Required for parseFstpUrl
#include <iomanip> // Required for parseFstpUrl (potentially for logging or advanced parsing)
#include <stdexcept> // Required for parseFstpUrl (std::stoi exceptions)

// Структура для распарсенного URL
struct ParsedFstpUrl {
    std::string videoPath;
    double timeInSeconds = -1.0; // -1.0 если время не указано
    bool isValid = false;
    std::string originalUrl; // Для логирования
};

// Вспомогательная функция для URL-декодирования
std::string urlDecode(const std::string& encoded_string);

// Функция парсинга URL fstp://
// Примечание: original_fps используется для расчета времени из кадров. 
// Если он недоступен в момент парсинга, передайте <= 0.
ParsedFstpUrl parseFstpUrl(const std::string& url_arg, double current_original_fps);

// Основная функция для обработки URL-аргумента
// Возвращает true, если URL-аргумент был fstp схемой и был обработан (даже если парсинг не удался).
// Возвращает false, если это не fstp URL.
// 
// @param url_arg: Полная строка URL (например, "fstp:///path/to/video&t=00102000")
// @param currentOpenFilePath: Полный путь к текущему открытому видеофайлу. Пустая строка, если файл не открыт.
// @param current_original_fps: FPS текущего видео (для парсинга времени из кадров в URL).
// @param outVideoPath: Сюда будет записан декодированный путь к видео из URL.
// @param outTimeToSeek: Сюда будет записано время для перемотки в секундах (-1.0 если нет).
// @param shouldOpenFile: Установится в true, если нужно открыть файл (путь отличается от currentOpenFilePath или currentOpenFilePath пуст).
// @param shouldSeek: Установится в true, если в URL указано валидное время и либо открывается новый файл, либо текущий файл совпадает.
bool handleFstpUrlArgument(const std::string& url_arg,
                           const std::string& currentOpenFilePath,
                           double current_original_fps,
                           std::string& outVideoPath,
                           double& outTimeToSeek,
                           bool& shouldOpenFile,
                           bool& shouldSeek);

#endif // URL_HANDLER_H 