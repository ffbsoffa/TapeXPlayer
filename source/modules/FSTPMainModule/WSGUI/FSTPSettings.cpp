#include "FSTPSettings.h"
#include <cstring>
#include <fstream>
#include <iostream>
#include <filesystem>
#include <vector>
#include <set>
#include <unordered_map>
#include <atomic>
#include <algorithm>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#ifndef _WIN32
#include <unistd.h>
#include <sys/wait.h>
#endif

#ifdef __APPLE__
#include <portaudio.h>
#endif

// Global settings structure
static FSTPSettings g_settings;
static bool g_settings_initialized = false;
static std::atomic<bool> g_settings_shutting_down{false};

// Path to settings file (will be set during initialization)
static std::string g_settings_path;

// Get path to settings file (platform-dependent)
static std::string GetSettingsFilePath() {
#ifdef __APPLE__
    // macOS: ~/Library/Preferences/com.tapexplayer.settings
    const char* home = getenv("HOME");
    if (home) {
        return std::string(home) + "/Library/Preferences/com.tapexplayer.settings";
    }
#elif defined(_WIN32)
    // Windows: %APPDATA%/TapeXPlayer/settings.ini
    const char* appdata = getenv("APPDATA");
    if (appdata) {
        return std::string(appdata) + "/TapeXPlayer/settings.ini";
    }
#else
    // Linux: ~/.config/tapexplayer/settings
    const char* home = getenv("HOME");
    if (home) {
        return std::string(home) + "/.config/tapexplayer/settings";
    }
#endif

    // Fallback: current directory
    return "./tapexplayer_settings.ini";
}

// Set default settings
void ResetSettingsToDefault() {
    memset(&g_settings, 0, sizeof(FSTPSettings));

    // Audio settings
    g_settings.audio_device_index = -1;        // -1 means use Pa_GetDefaultOutputDevice()
    g_settings.audio_device_name[0] = '\0';    // no pinned device yet (resolved by name when set)
    g_settings.audio_follow_default = 1;       // follow the OS default output until the user pins a device
    g_settings.audio_master_volume = 1.0f;     // 100%
    g_settings.audio_buffer_size = 1024;       // 1024 samples
    g_settings.audio_volume_ducking_enabled = 1; // ENABLED BY DEFAULT (ear protection at high shuttle speeds)

    // A/V sync settings
    g_settings.frame_offset = 0;              // No offset by default

    // Multi-instance settings
    g_settings.auto_freeze_inactive = 1;       // ENABLED BY DEFAULT (protection from forgotten players)
    g_settings.betacam_effect_enabled = 0;     // Disabled by default
    g_settings.betacam_reverse_stripe = 0;     // Off by default (opt-in; distracts frame analysis)
    g_settings.yt_dlp_extension_enabled = 0;   // Disabled by default

    // MIDI settings
    g_settings.midi_enabled = 0;              // Disabled by default
    g_settings.midi_input_port = -1;          // Not selected
    g_settings.midi_output_port = -1;         // Not selected

    // Developer/Debug settings
    g_settings.show_decoder_status = 0;       // Hidden by default (debug feature)

    // Presentation mode settings
    g_settings.presentation_display_index = -1; // -1 = auto (first external display)
    g_settings.presentation_output_mode   = 0;  // 0 = external display (with windowed fallback)
    g_settings.presentation_follow_focus  = 1;  // follow the focused player by default
    g_settings.presentation_pinned_player = 0;  // used only when follow_focus == 0

    // Onboarding / first-run
    g_settings.welcome_version = 0;             // 0 = welcome screen never shown
}

// Initialize settings system
int InitSettings() {
    if (g_settings_shutting_down) return 0; // don't re-init during shutdown
    if (g_settings_initialized) {
        return 0; // Already initialized
    }

    g_settings_path = GetSettingsFilePath();
    ResetSettingsToDefault();

    // Try to load settings from file
    if (LoadSettings() != 0) {
        std::cout << "Settings file not found or corrupted, using defaults" << std::endl;
    } else {
        std::cout << "Settings loaded from: " << g_settings_path << std::endl;
    }

    if (g_settings.yt_dlp_extension_enabled && !FSTP_YTDLP_IsAvailable()) {
        g_settings.yt_dlp_extension_enabled = 0;
    }

    g_settings_initialized = true;
    return 0;
}

// Shutdown settings system
void ShutdownSettings() {
    g_settings_shutting_down = true; // block re-init from background threads
    if (g_settings_initialized) {
        SaveSettings(); // Automatic save on exit
        g_settings_initialized = false;
    }
}

// Get pointer to settings
FSTPSettings* GetSettings() {
    if (!g_settings_initialized) {
        InitSettings();
    }
    return &g_settings;
}

// Simple settings save (text format)
int SaveSettings() {
    if (!g_settings_initialized) {
        return -1;
    }

    // Ensure parent directory exists
    std::filesystem::path settings_dir = std::filesystem::path(g_settings_path).parent_path();
    if (!settings_dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(settings_dir, ec);
    }

    std::ofstream file{std::filesystem::path(g_settings_path)};
    if (!file.is_open()) {
        std::cerr << "Failed to open settings file for writing: " << g_settings_path << std::endl;
        return -1;
    }

    file << "# TapeXPlayer Settings File\n";
    file << "# Generated automatically - do not edit manually\n\n";

    // Audio settings
    file << "[Audio]\n";
    file << "device_index=" << g_settings.audio_device_index << "\n";
    file << "device_name=" << g_settings.audio_device_name << "\n";
    file << "follow_default=" << g_settings.audio_follow_default << "\n";
    file << "master_volume=" << g_settings.audio_master_volume << "\n";
    file << "buffer_size=" << g_settings.audio_buffer_size << "\n";
    file << "volume_ducking_enabled=" << g_settings.audio_volume_ducking_enabled << "\n";
    file << "\n";

    // Sync settings
    file << "[Sync]\n";
    file << "frame_offset=" << g_settings.frame_offset << "\n";

    // Multi-instance settings
    file << "[MultiInstance]\n";
    file << "auto_freeze_inactive=" << g_settings.auto_freeze_inactive << "\n";
    file << "\n";

    // Video settings
    file << "[Video]\n";
    file << "betacam_effect_enabled=" << g_settings.betacam_effect_enabled << "\n";
    file << "betacam_reverse_stripe=" << g_settings.betacam_reverse_stripe << "\n";
    file << "presentation_display=" << g_settings.presentation_display_index << "\n";
    file << "presentation_output_mode=" << g_settings.presentation_output_mode << "\n";
    file << "presentation_follow_focus=" << g_settings.presentation_follow_focus << "\n";
    file << "presentation_pinned_player=" << g_settings.presentation_pinned_player << "\n";
    file << "\n";

    file << "[Extensions]\n";
    file << "yt_dlp_enabled=" << g_settings.yt_dlp_extension_enabled << "\n";
    file << "\n";

    // MIDI settings
    file << "[MIDI]\n";
    file << "enabled=" << g_settings.midi_enabled << "\n";
    file << "input_port=" << g_settings.midi_input_port << "\n";
    file << "output_port=" << g_settings.midi_output_port << "\n";
    file << "\n";

    // Developer/Debug settings
    file << "[Debug]\n";
    file << "show_decoder_status=" << g_settings.show_decoder_status << "\n";
    file << "\n";

    // UI / onboarding
    file << "[UI]\n";
    file << "welcome_version=" << g_settings.welcome_version << "\n";

    file.close();
    std::cout << "Settings saved to: " << g_settings_path << std::endl;
    return 0;
}

// Simple settings load (primitive parser)
int LoadSettings() {
    std::ifstream file{std::filesystem::path(g_settings_path)};
    if (!file.is_open()) {
        return -1; // File not found
    }

    std::string line;
    std::string current_section = "";

    while (std::getline(file, line)) {
        // Skip comments and empty lines
        if (line.empty() || line[0] == '#') {
            continue;
        }

        // Determine section
        if (line[0] == '[' && line.back() == ']') {
            current_section = line.substr(1, line.length() - 2);
            continue;
        }

        // Parse key=value pairs
        size_t eq_pos = line.find('=');
        if (eq_pos == std::string::npos) {
            continue;
        }

        std::string key = line.substr(0, eq_pos);
        std::string value = line.substr(eq_pos + 1);

        // Simple section-based parser
        if (current_section == "Audio") {
            if (key == "device_index") g_settings.audio_device_index = std::stoi(value);
            else if (key == "device_name") {
                strncpy(g_settings.audio_device_name, value.c_str(), sizeof(g_settings.audio_device_name) - 1);
                g_settings.audio_device_name[sizeof(g_settings.audio_device_name) - 1] = '\0';
            }
            else if (key == "follow_default") g_settings.audio_follow_default = std::stoi(value);
            else if (key == "master_volume") g_settings.audio_master_volume = std::stof(value);
            else if (key == "buffer_size") g_settings.audio_buffer_size = std::stoi(value);
            else if (key == "volume_ducking_enabled") g_settings.audio_volume_ducking_enabled = std::stoi(value);
        }
        else if (current_section == "Sync") {
            if (key == "frame_offset") g_settings.frame_offset = std::stoi(value);
        }
        else if (current_section == "MultiInstance") {
            if (key == "auto_freeze_inactive") g_settings.auto_freeze_inactive = std::stoi(value);
        }
        else if (current_section == "Video") {
            if (key == "betacam_effect_enabled") g_settings.betacam_effect_enabled = std::stoi(value);
            else if (key == "betacam_reverse_stripe") g_settings.betacam_reverse_stripe = std::stoi(value);
            else if (key == "presentation_display") g_settings.presentation_display_index = std::stoi(value);
            else if (key == "presentation_output_mode") g_settings.presentation_output_mode = std::stoi(value);
            else if (key == "presentation_follow_focus") g_settings.presentation_follow_focus = std::stoi(value);
            else if (key == "presentation_pinned_player") g_settings.presentation_pinned_player = std::stoi(value);
        }
        else if (current_section == "Extensions") {
            if (key == "yt_dlp_enabled") g_settings.yt_dlp_extension_enabled = std::stoi(value);
        }
        else if (current_section == "MIDI") {
            if (key == "enabled") g_settings.midi_enabled = std::stoi(value);
            else if (key == "input_port") g_settings.midi_input_port = std::stoi(value);
            else if (key == "output_port") g_settings.midi_output_port = std::stoi(value);
        }
        else if (current_section == "Debug") {
            if (key == "show_decoder_status") g_settings.show_decoder_status = std::stoi(value);
        }
        else if (current_section == "UI") {
            if (key == "welcome_version") g_settings.welcome_version = std::stoi(value);
        }
    }

    file.close();
    return 0;
}

// Forward declarations for platform-specific implementations
#ifdef __linux__
extern "C" void ShowGTKSettingsDialog();
#endif

// Show settings window (platform-specific implementation)
void ShowSettingsDialog() {
#ifdef __APPLE__
    // Function implemented in FSTPDarwinWS.mm as ShowNativeSettingsDialog()
    std::cout << "ShowSettingsDialog called - implementation in FSTPDarwinWS.mm" << std::endl;
#elif defined(__linux__)
    ShowGTKSettingsDialog();
#else
    std::cout << "Settings dialog not implemented for this platform" << std::endl;
#endif
}

// Apply settings to audio system (will be called from audio module)
void ApplyAudioSettings() {
    if (!g_settings_initialized) {
        InitSettings();
    }

    std::cout << "Applying audio settings: device=" << g_settings.audio_device_index
              << ", buffer=" << g_settings.audio_buffer_size
              << ", volume=" << g_settings.audio_master_volume << std::endl;

    // Apply only volume and device (buffer size requires restart)
    // Main application happens in audio module via get functions
    std::cout << "Note: Buffer size changes require application restart" << std::endl;
}

// Convenience functions for getting settings
int GetAudioDeviceIndex() {
    if (!g_settings_initialized) {
        InitSettings();
    }
    return g_settings.audio_device_index;
}

const char* GetAudioDeviceName() {
    if (!g_settings_initialized) InitSettings();
    return g_settings.audio_device_name;
}

int GetAudioFollowDefault() {
    if (!g_settings_initialized) InitSettings();
    return g_settings.audio_follow_default;
}

// Pin output to a specific device: index for the immediate open, name for stable restore.
void SetAudioDevice(int index, const char* name) {
    if (!g_settings_initialized) InitSettings();
    g_settings.audio_device_index = index;
    if (name) {
        strncpy(g_settings.audio_device_name, name, sizeof(g_settings.audio_device_name) - 1);
        g_settings.audio_device_name[sizeof(g_settings.audio_device_name) - 1] = '\0';
    } else {
        g_settings.audio_device_name[0] = '\0';
    }
    g_settings.audio_follow_default = 0;   // pinning implies not following the system default
}

void SetAudioFollowDefault(int follow) {
    if (!g_settings_initialized) InitSettings();
    g_settings.audio_follow_default = follow ? 1 : 0;
}

float GetMasterVolume() {
    if (!g_settings_initialized) {
        InitSettings();
    }
    return g_settings.audio_master_volume;
}

int GetAudioBufferSize() {
    if (!g_settings_initialized) {
        InitSettings();
    }
    return g_settings.audio_buffer_size;
}

int GetAudioVolumeDuckingEnabled() {
    if (!g_settings_initialized) {
        InitSettings();
    }
    return g_settings.audio_volume_ducking_enabled;
}

int GetFrameOffset() {
    if (!g_settings_initialized) {
        InitSettings();
    }
    return g_settings.frame_offset;
}

int GetAutoFreezeInactive() {
    if (!g_settings_initialized) {
        InitSettings();
    }
    return g_settings.auto_freeze_inactive;
}

int GetBetacamEffectEnabled() {
    if (!g_settings_initialized) {
        InitSettings();
    }
    return g_settings.betacam_effect_enabled;
}

int GetBetacamReverseStripe() {
    if (!g_settings_initialized) {
        InitSettings();
    }
    return g_settings.betacam_reverse_stripe;
}

int GetYTDLPExtensionEnabled() {
    if (!g_settings_initialized) {
        InitSettings();
    }
    return g_settings.yt_dlp_extension_enabled;
}

void SetYTDLPExtensionEnabled(int enabled) {
    if (!g_settings_initialized) {
        InitSettings();
    }
    if (enabled && !FSTP_YTDLP_IsAvailable()) {
        enabled = 0;
    }
    g_settings.yt_dlp_extension_enabled = enabled ? 1 : 0;
}

int GetMIDIEnabled() {
    if (!g_settings_initialized) {
        InitSettings();
    }
    return g_settings.midi_enabled;
}

int GetMIDIInputPort() {
    if (!g_settings_initialized) {
        InitSettings();
    }
    return g_settings.midi_input_port;
}

int GetMIDIOutputPort() {
    if (!g_settings_initialized) {
        InitSettings();
    }
    return g_settings.midi_output_port;
}

const char* GetExtensionLanguage() {
    return FSTP_EXTENSION_SCRIPT_LANGUAGE;
}

int GetShowDecoderStatus() {
    if (!g_settings_initialized) {
        InitSettings();
    }
    return g_settings.show_decoder_status;
}

int GetPresentationDisplayIndex() {
    if (!g_settings_initialized) InitSettings();
    return g_settings.presentation_display_index;
}

void SetPresentationDisplayIndex(int idx) {
    if (!g_settings_initialized) InitSettings();
    g_settings.presentation_display_index = idx;
    SaveSettings();
}

int GetPresentationOutputMode() {
    if (!g_settings_initialized) InitSettings();
    return g_settings.presentation_output_mode;
}

void SetPresentationOutputMode(int mode) {
    if (!g_settings_initialized) InitSettings();
    g_settings.presentation_output_mode = mode;
    SaveSettings();
}

int GetPresentationFollowFocus() {
    if (!g_settings_initialized) InitSettings();
    return g_settings.presentation_follow_focus;
}

void SetPresentationFollowFocus(int follow) {
    if (!g_settings_initialized) InitSettings();
    g_settings.presentation_follow_focus = follow;
    SaveSettings();
}

int GetPresentationPinnedPlayer() {
    if (!g_settings_initialized) InitSettings();
    return g_settings.presentation_pinned_player;
}

void SetPresentationPinnedPlayer(int player_id) {
    if (!g_settings_initialized) InitSettings();
    g_settings.presentation_pinned_player = player_id;
    SaveSettings();
}

int GetWelcomeVersion() {
    if (!g_settings_initialized) InitSettings();
    return g_settings.welcome_version;
}

void SetWelcomeVersion(int version) {
    if (!g_settings_initialized) InitSettings();
    g_settings.welcome_version = version;
    SaveSettings();
}

// ============================================================
// Resume positions — persist last playback position per file
// ============================================================

static std::string GetResumeFilePath() {
#ifdef __APPLE__
    const char* home = getenv("HOME");
    if (home) return std::string(home) + "/Library/Preferences/com.tapexplayer.resume";
#elif defined(_WIN32)
    const char* appdata = getenv("APPDATA");
    if (appdata) return std::string(appdata) + "/TapeXPlayer/resume.ini";
#else
    const char* home = getenv("HOME");
    if (home) return std::string(home) + "/.config/tapexplayer/resume";
#endif
    return "./tapexplayer_resume.ini";
}

// In-memory cache: filepath → position_seconds
static std::unordered_map<std::string, double>& GetResumeCache() {
    // Allocated with `new` (intentionally never deleted) so it is never
    // registered with atexit and survives past static destructor phase.
    // Without this, the map can be destroyed before global player instances,
    // causing a crash when UnloadFile() calls SaveResumePosition() at exit.
    static std::unordered_map<std::string, double>* cache =
        new std::unordered_map<std::string, double>();
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        std::ifstream f{std::filesystem::path(GetResumeFilePath())};
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#') continue;
            size_t eq = line.rfind('=');  // rfind: filepath may contain '='
            if (eq == std::string::npos) continue;
            std::string key = line.substr(0, eq);
            try { (*cache)[key] = std::stod(line.substr(eq + 1)); }
            catch (...) {}
        }
    }
    return *cache;
}

static void FlushResumeCache() {
    std::string path = GetResumeFilePath();
    {
        std::error_code ec;
        auto dir = std::filesystem::path(path).parent_path();
        if (!dir.empty()) std::filesystem::create_directories(dir, ec);
    }
    std::ofstream f{std::filesystem::path(path)};
    if (!f.is_open()) return;
    f << "# TapeXPlayer Resume Positions\n";
    constexpr size_t MAX_ENTRIES = 200;
    auto& cache = GetResumeCache();
    size_t count = 0;
    for (auto it = cache.begin(); it != cache.end() && count < MAX_ENTRIES; ++it, ++count) {
        f << it->first << "=" << std::fixed << it->second << "\n";
    }
}

void SaveResumePosition(const char* filepath, double position_seconds) {
    if (!filepath || position_seconds < 1.0) return;  // ignore trivial positions
    GetResumeCache()[filepath] = position_seconds;
    FlushResumeCache();
}

double LoadResumePosition(const char* filepath) {
    if (!filepath) return -1.0;
    auto& cache = GetResumeCache();
    auto it = cache.find(std::string(filepath));
    if (it != cache.end()) return it->second;
    return -1.0;
}

void ClearResumePosition(const char* filepath) {
    if (!filepath) return;
    GetResumeCache().erase(std::string(filepath));
    FlushResumeCache();
}

// ============================================================
// Recent files — MRU list of the last opened files (for "Open Recent")
// ============================================================

// Max entries kept and shown in the Open-Recent menu.
static const size_t FSTP_RECENT_MAX = 12;

static std::string GetRecentFilePath() {
#ifdef __APPLE__
    const char* home = getenv("HOME");
    if (home) return std::string(home) + "/Library/Preferences/com.tapexplayer.recent";
#elif defined(_WIN32)
    const char* appdata = getenv("APPDATA");
    if (appdata) return std::string(appdata) + "/TapeXPlayer/recent.ini";
#else
    const char* home = getenv("HOME");
    if (home) return std::string(home) + "/.config/tapexplayer/recent";
#endif
    return "./tapexplayer_recent.ini";
}

// In-memory MRU list: index 0 = most recent. Ordered, so a vector (not a map).
// Same "new, never deleted" lifetime trick as GetResumeCache(): a file may be
// opened/closed during static teardown, and we must not touch a destroyed list.
static std::vector<std::string>& GetRecentList() {
    static std::vector<std::string>* list = new std::vector<std::string>();
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        std::ifstream f{std::filesystem::path(GetRecentFilePath())};
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#') continue;
            // Strip a trailing CR (files written on Windows / edited by hand).
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty() && list->size() < FSTP_RECENT_MAX)
                list->push_back(line);
        }
    }
    return *list;
}

static void FlushRecentList() {
    std::string path = GetRecentFilePath();
    {
        std::error_code ec;
        auto dir = std::filesystem::path(path).parent_path();
        if (!dir.empty()) std::filesystem::create_directories(dir, ec);
    }
    std::ofstream f{std::filesystem::path(path)};
    if (!f.is_open()) return;
    f << "# TapeXPlayer Recent Files (most recent first)\n";
    auto& list = GetRecentList();
    size_t count = 0;
    for (auto it = list.begin(); it != list.end() && count < FSTP_RECENT_MAX; ++it, ++count)
        f << *it << "\n";
}

void AddRecentFile(const char* filepath) {
    if (!filepath || !*filepath) return;
    std::string path(filepath);
    auto& list = GetRecentList();
    // Remove any existing occurrence, then push to front (move-to-front MRU).
    list.erase(std::remove(list.begin(), list.end(), path), list.end());
    list.insert(list.begin(), path);
    if (list.size() > FSTP_RECENT_MAX) list.resize(FSTP_RECENT_MAX);
    FlushRecentList();
}

int GetRecentFileCount() {
    return (int)GetRecentList().size();
}

const char* GetRecentFile(int index) {
    auto& list = GetRecentList();
    if (index < 0 || index >= (int)list.size()) return nullptr;
    return list[index].c_str();  // valid until the list is next mutated
}

void ClearRecentFiles() {
    GetRecentList().clear();
    FlushRecentList();
}

// ============================================================

namespace {
namespace fs = std::filesystem;

static std::string& GetDownloadsDirectoryInternal() {
    static std::string downloads_dir;
    if (!downloads_dir.empty()) {
        return downloads_dir;
    }

    const char* home = std::getenv("HOME");
    fs::path base = home ? fs::path(home) : fs::temp_directory_path();
#ifdef __APPLE__
    base /= "Movies";
#else
    base /= "Videos";
#endif
    fs::path target = base / "TapeXDownloads";

    std::error_code ec;
    fs::create_directories(target, ec);
    downloads_dir = target.lexically_normal().string();
    return downloads_dir;
}

static bool CopyToBuffer(const std::string& src, char* dest, int capacity) {
    if (!dest || capacity <= 0) {
        return false;
    }
    if ((int)src.size() >= capacity) {
        std::snprintf(dest, capacity, "%s", src.substr(0, capacity - 1).c_str());
        return false;
    }
    std::snprintf(dest, capacity, "%s", src.c_str());
    return true;
}

static std::string ShellQuote(const std::string& value) {
    std::string result = "'";
    for (char c : value) {
        if (c == '\'') {
            result += "'\\''";
        } else {
            result += c;
        }
    }
    result += "'";
    return result;
}

static bool LocateYTDLPBinary(std::string& binary_path) {
    static bool cached = false;
    static std::string cached_path;
    static bool cached_result = false;

    if (cached) {
        binary_path = cached_path;
        return cached_result;
    }

    cached = true;

    std::set<std::string> search_dirs;
    const char* path_env = std::getenv("PATH");
    if (path_env) {
        std::string path_str(path_env);
        size_t start = 0;
        while (start <= path_str.size()) {
            size_t end = path_str.find(':', start);
            std::string part = path_str.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (!part.empty()) {
                search_dirs.insert(part);
            }
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }
#ifdef __APPLE__
    search_dirs.insert("/opt/homebrew/bin");
    search_dirs.insert("/usr/local/bin");
#endif

    for (const auto& dir : search_dirs) {
        fs::path candidate = fs::path(dir) / "yt-dlp";
        std::error_code ec;
        if (fs::exists(candidate, ec) && fs::is_regular_file(candidate, ec)) {
#ifdef _WIN32
            cached_path = candidate.string();
            cached_result = true;
            binary_path = cached_path;
            return true;
#else
            if (::access(candidate.c_str(), X_OK) == 0) {
                cached_path = candidate.string();
                cached_result = true;
                binary_path = cached_path;
                return true;
            }
#endif
        }
    }

    cached_path.clear();
    cached_result = false;
    binary_path.clear();
    return false;
}

static bool DetectDownloadedFile(const fs::path& download_dir,
                                 const std::unordered_map<std::string, fs::file_time_type>& before,
                                 fs::path& out_path) {
    bool found = false;
    fs::file_time_type latest_time = fs::file_time_type::min();

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(download_dir, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file(ec)) {
            continue;
        }

        const std::string name = entry.path().filename().string();
        fs::file_time_type mod_time = entry.last_write_time(ec);
        if (ec) {
            continue;
        }

        auto it = before.find(name);
        bool is_new = false;
        if (it == before.end()) {
            is_new = true;
        } else if (mod_time > it->second) {
            is_new = true;
        }

        if (is_new && (!found || mod_time > latest_time)) {
            found = true;
            latest_time = mod_time;
            out_path = entry.path();
        }
    }

    return found;
}
} // namespace

int FSTP_YTDLP_IsAvailable(void) {
    std::string path;
    return LocateYTDLPBinary(path) ? 1 : 0;
}

const char* FSTP_YTDLP_GetDownloadsDir(void) {
    static std::string dir = GetDownloadsDirectoryInternal();
    return dir.c_str();
}

int FSTP_YTDLP_Download(const char* url,
                        char* out_path,
                        int out_path_size,
                        char* error_buf,
                        int error_buf_size) {
    if (!url || std::strlen(url) == 0) {
        CopyToBuffer("Empty URL", error_buf, error_buf_size);
        return 0;
    }

    std::string binary_path;
    if (!LocateYTDLPBinary(binary_path)) {
        CopyToBuffer("yt-dlp is not installed or not found in PATH", error_buf, error_buf_size);
        return 0;
    }

    fs::path download_dir = fs::path(GetDownloadsDirectoryInternal());

    std::unordered_map<std::string, fs::file_time_type> before;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(download_dir, ec)) {
        if (ec) {
            break;
        }
        if (entry.is_regular_file(ec)) {
            before.emplace(entry.path().filename().string(), entry.last_write_time(ec));
        }
    }

    std::string output_template = (download_dir / "%(title)s.%(ext)s").string();
    std::string command = ShellQuote(binary_path) +
        " --no-progress --no-playlist --merge-output-format mp4" +
        " -o " + ShellQuote(output_template) +
        " " + ShellQuote(url) + " 2>&1";

    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        CopyToBuffer("Failed to spawn yt-dlp process", error_buf, error_buf_size);
        return 0;
    }

    std::string output;
    char buffer[4096];
    while (std::fgets(buffer, sizeof(buffer), pipe)) {
        output += buffer;
    }

    int status = pclose(pipe);
#ifdef _WIN32
    int exit_code = status; // pclose returns exit code directly on Windows
#else
    int exit_code = -1;
    if (WIFEXITED(status)) {
        exit_code = WEXITSTATUS(status);
    }
#endif

    if (exit_code != 0) {
        if (output.empty()) {
            output = "yt-dlp failed with exit code " + std::to_string(exit_code);
        }
        CopyToBuffer(output, error_buf, error_buf_size);
        return 0;
    }

    fs::path downloaded_file;
    if (!DetectDownloadedFile(download_dir, before, downloaded_file)) {
        CopyToBuffer("Download completed but output file was not detected", error_buf, error_buf_size);
        return 0;
    }

    if (!CopyToBuffer(downloaded_file.string(), out_path, out_path_size)) {
        CopyToBuffer("Downloaded file path is too long", error_buf, error_buf_size);
        return 0;
    }

    if (error_buf && error_buf_size > 0) {
        error_buf[0] = '\0';
    }

    return 1;
}
