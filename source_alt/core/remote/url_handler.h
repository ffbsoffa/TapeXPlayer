#ifndef URL_HANDLER_H
#define URL_HANDLER_H

#include <string>
#include <vector> // Required for parseFstpUrl (used by std::stringstream in its typical implementation)
#include <sstream> // Required for parseFstpUrl
#include <iomanip> // Required for parseFstpUrl (potentially for logging or advanced parsing)
#include <stdexcept> // Required for parseFstpUrl (std::stoi exceptions)

// Structure for parsed URL
struct ParsedFstpUrl {
    std::string videoPath;
    double timeInSeconds = -1.0; // -1.0 if time is not specified
    bool isValid = false;
    std::string originalUrl; // For logging
};

// Helper function for URL decoding
std::string urlDecode(const std::string& encoded_string);

// Function for parsing fstp:// URL
// Note: original_fps is used to calculate time from frames.
// If it's not available at parsing time, pass <= 0.
ParsedFstpUrl parseFstpUrl(const std::string& url_arg, double current_original_fps);

// Main function for processing URL argument
// Returns true if URL argument was fstp scheme and was processed (even if parsing failed).
// Returns false if this is not an fstp URL.
// 
// @param url_arg: Full URL string (e.g., "fstp:///path/to/video&t=00102000")
// @param currentOpenFilePath: Full path to currently open video file. Empty string if no file is open.
// @param current_original_fps: FPS of current video (for parsing time from frames in URL).
// @param outVideoPath: Decoded video path from URL will be written here.
// @param outTimeToSeek: Time to seek in seconds will be written here (-1.0 if none).
// @param shouldOpenFile: Will be set to true if file needs to be opened (path differs from currentOpenFilePath or currentOpenFilePath is empty).
// @param shouldSeek: Will be set to true if URL contains valid time and either opening new file or current file matches.
bool handleFstpUrlArgument(const std::string& url_arg,
                           const std::string& currentOpenFilePath,
                           double current_original_fps,
                           std::string& outVideoPath,
                           double& outTimeToSeek,
                           bool& shouldOpenFile,
                           bool& shouldSeek);

#endif // URL_HANDLER_H 