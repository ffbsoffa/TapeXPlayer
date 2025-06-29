#include "url_handler.h"
#include <iostream> // For std::cerr, std::cout
#include <algorithm> // For std::transform (if needed for path normalization)
// Other necessary standard headers are already included via url_handler.h

// Helper function for URL decoding
std::string urlDecode(const std::string& encoded_string) {
    std::string result;
    result.reserve(encoded_string.length());
    for (std::size_t i = 0; i < encoded_string.length(); ++i) {
        if (encoded_string[i] == '%') {
            if (i + 2 < encoded_string.length()) {
                std::string hex = encoded_string.substr(i + 1, 2);
                try {
                    char decoded_char = static_cast<char>(std::stoi(hex, nullptr, 16));
                    result += decoded_char;
                } catch (const std::invalid_argument& /*ia*/) {
                    result += encoded_string[i]; // Error, add as is
                    result += encoded_string[i+1];
                    result += encoded_string[i+2];
                } catch (const std::out_of_range& /*oor*/) {
                    result += encoded_string[i]; // Error, add as is
                    result += encoded_string[i+1];
                    result += encoded_string[i+2];
                }
                i += 2;
            } else {
                result += encoded_string[i]; // Incomplete %-code
            }
        } else if (encoded_string[i] == '+') {
            result += ' ';
        } else {
            result += encoded_string[i];
        }
    }
    return result;
}

// Function for parsing fstp:// URL
ParsedFstpUrl parseFstpUrl(const std::string& url_arg, double current_original_fps) {
    ParsedFstpUrl result;
    result.originalUrl = url_arg;
    const std::string prefix = "fstp://";

    if (url_arg.rfind(prefix, 0) != 0) {
        return result; // Not our scheme
    }

    std::string full_content = url_arg.substr(prefix.length());

    // Processing path in URL
    if (full_content.rfind("///", 0) == 0) {
        full_content = full_content.substr(2); // Keep one / at the beginning: "/absolute/path"
    } 
    // Otherwise path is considered as is (can be relative or absolute without triple slash)

    std::string encoded_path;
    std::string time_param_str;

    size_t time_marker_pos = full_content.find("&t=");
    if (time_marker_pos != std::string::npos) {
        encoded_path = full_content.substr(0, time_marker_pos);
        time_param_str = full_content.substr(time_marker_pos + 3); // Length of "&t="
    } else {
        encoded_path = full_content;
    }

    result.videoPath = urlDecode(encoded_path);
    // Small cleanup for path if it starts with "//" after decoding
    if (result.videoPath.rfind("//", 0) == 0 && result.videoPath.length() > 2 && result.videoPath[2] != '/') {
        result.videoPath = result.videoPath.substr(1);
    }
    // Remove "file://" prefix if present
    const std::string file_scheme = "file://";
    if (result.videoPath.rfind(file_scheme, 0) == 0) {
        result.videoPath = result.videoPath.substr(file_scheme.length());
    }

    if (!time_param_str.empty()) {
        int h = 0, m = 0, s = 0, f = 0;
        bool time_parsed = false;

        if (time_param_str.find(':') != std::string::npos) {
            std::vector<std::string> parts;
            std::string current_part;
            std::stringstream ss_time(time_param_str);
            while(std::getline(ss_time, current_part, ':')) {
                parts.push_back(current_part);
            }

            try {
                if (parts.size() == 4) { // HH:MM:SS:FF
                    h = std::stoi(parts[0]);
                    m = std::stoi(parts[1]);
                    s = std::stoi(parts[2]);
                    f = std::stoi(parts[3]);
                    time_parsed = true;
                } else if (parts.size() == 3) { // HH:MM:SS
                    h = std::stoi(parts[0]);
                    m = std::stoi(parts[1]);
                    s = std::stoi(parts[2]);
                    time_parsed = true;
                } else if (parts.size() == 2) { // MM:SS
                    m = std::stoi(parts[0]);
                    s = std::stoi(parts[1]);
                    time_parsed = true;
                } else if (parts.size() == 1) { // SS 
                     s = std::stoi(parts[0]);
                     time_parsed = true;
                }
            } catch (const std::exception& /*e*/) { /* Parsing error */ }
        } else { // Format without colons: HHMMSSFF, HHMMSS, MMSS, SS
            try {
                if (time_param_str.length() == 8) { // HHMMSSFF
                    h = std::stoi(time_param_str.substr(0, 2));
                    m = std::stoi(time_param_str.substr(2, 2));
                    s = std::stoi(time_param_str.substr(4, 2));
                    f = std::stoi(time_param_str.substr(6, 2));
                    time_parsed = true;
                } else if (time_param_str.length() == 6) { // HHMMSS
                    h = std::stoi(time_param_str.substr(0, 2));
                    m = std::stoi(time_param_str.substr(2, 2));
                    s = std::stoi(time_param_str.substr(4, 2));
                    time_parsed = true;
                } else if (time_param_str.length() == 4) { // MMSS
                    m = std::stoi(time_param_str.substr(0, 2));
                    s = std::stoi(time_param_str.substr(2, 2));
                    time_parsed = true;
                } else if (time_param_str.length() == 2) { // SS
                    s = std::stoi(time_param_str.substr(0, 2));
                    time_parsed = true;
                }
            } catch (const std::exception& /*e*/) { /* Parsing error */ }
        }

        if (time_parsed) {
            result.timeInSeconds = static_cast<double>(h * 3600 + m * 60 + s);
            if (f > 0 && current_original_fps > 0) {
                 result.timeInSeconds += static_cast<double>(f) / current_original_fps;
            }
        } else {
            std::cerr << "Warning: Failed to parse time string: " << time_param_str << " from URL " << url_arg << std::endl;
        }
    }
    result.isValid = !result.videoPath.empty();
    if (!result.isValid) {
        std::cerr << "Warning: Failed to parse video path from URL: " << url_arg << std::endl;
    }
    return result;
}

bool handleFstpUrlArgument(const std::string& url_arg,
                           const std::string& currentOpenFilePath,
                           double current_original_fps,
                           std::string& outVideoPath,
                           double& outTimeToSeek,
                           bool& shouldOpenFile,
                           bool& shouldSeek) {
    
    // --- DETAILED LOGGING (ENTRY) ---
    std::cout << "[url_handler.cpp DEBUG] handleFstpUrlArgument - Entry:" << std::endl;
    std::cout << "[url_handler.cpp DEBUG]   url_arg:               '" << url_arg << "'" << std::endl;
    std::cout << "[url_handler.cpp DEBUG]   currentOpenFilePath:   '" << currentOpenFilePath << "'" << std::endl;
    std::cout << "[url_handler.cpp DEBUG]   current_original_fps:  " << current_original_fps << std::endl;
    // --- END DETAILED LOGGING ---

    const std::string prefix = "fstp://";
    if (url_arg.rfind(prefix, 0) != 0) {
        shouldOpenFile = false;
        shouldSeek = false;
        return false; // Not fstp URL
    }

    ParsedFstpUrl parsed = parseFstpUrl(url_arg, current_original_fps);

    if (parsed.isValid) {
        std::cout << "Parsed fstp:// URL. Path: '" << parsed.videoPath 
                  << "', Time: " << parsed.timeInSeconds << "s (Original URL: " << parsed.originalUrl << ")" << std::endl;
                  
        outVideoPath = parsed.videoPath;
        outTimeToSeek = parsed.timeInSeconds;

        // Normalize paths for comparison (e.g., important for macOS due to case-insensitivity by default)
        // More complex normalization could be added here if needed.
        // For simplicity, direct comparison for now.
        bool pathsMatch = false;
        if (!currentOpenFilePath.empty() && !outVideoPath.empty()) {
            // Simple check. For production, path canonicalization might be needed
             pathsMatch = (currentOpenFilePath == outVideoPath);
             // TODO: Consider std::filesystem::equivalent for more reliable path comparison,
             // if std::filesystem is available and its use is justified.
            std::cout << "[url_handler.cpp DEBUG] Path comparison: currentOpenFilePath == outVideoPath -> " << (pathsMatch ? "true" : "false") << std::endl;
        }

        if (pathsMatch) {
            shouldOpenFile = false;
            std::cout << "URL video path matches currently open file: '" << currentOpenFilePath << "'" << std::endl;
        } else {
            shouldOpenFile = true;
            if (!currentOpenFilePath.empty()) {
                 std::cout << "URL video path '" << outVideoPath << "' differs from currently open '" << currentOpenFilePath << "'. Will open new file." << std::endl;
            } else {
                 std::cout << "No file currently open. Will open from URL: '" << outVideoPath << "'" << std::endl;
            }
        }

        if (outTimeToSeek >= 0.0) {
            shouldSeek = true;
            std::cout << "Seek requested to: " << outTimeToSeek << "s" << std::endl;
        } else {
            shouldSeek = false;
             std::cout << "No seek time specified in URL." << std::endl;
        }
        return true; // fstp URL was processed
    } else {
        std::cerr << "Error: Failed to parse valid data from fstp:// URL: " << url_arg << std::endl;
        shouldOpenFile = false;
        shouldSeek = false;
        // --- DETAILED LOGGING (INVALID PARSE) ---
        std::cout << "[url_handler.cpp DEBUG] handleFstpUrlArgument - Invalid parse/not fstp URL:" << std::endl;
        std::cout << "[url_handler.cpp DEBUG]   shouldOpenFile set to false" << std::endl;
        std::cout << "[url_handler.cpp DEBUG]   shouldSeek set to false" << std::endl;
        // --- END DETAILED LOGGING ---
        return true; // fstp URL was present but failed to parse - still consider it processed so main doesn't look for it as a file
    }
} 