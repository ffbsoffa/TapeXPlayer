#include "FSTPSettings.h"
#include <cstring>
#include <fstream>
#include <iostream>

#ifdef __APPLE__
#include <portaudio.h>
#endif

// Global settings structure
static FSTPSettings g_settings;
static bool g_settings_initialized = false;

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
    g_settings.audio_master_volume = 1.0f;     // 100%
    g_settings.audio_buffer_size = 1024;       // 1024 samples

    // A/V sync settings
    g_settings.frame_offset = 0;              // No offset by default

    // Multi-instance settings
    g_settings.auto_freeze_inactive = 1;       // ENABLED BY DEFAULT (protection from forgotten players)

    // MIDI settings
    g_settings.midi_enabled = 0;              // Disabled by default
    g_settings.midi_input_port = -1;          // Not selected
    g_settings.midi_output_port = -1;         // Not selected
}

// Initialize settings system
int InitSettings() {
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

    g_settings_initialized = true;
    return 0;
}

// Shutdown settings system
void ShutdownSettings() {
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

    std::ofstream file(g_settings_path);
    if (!file.is_open()) {
        std::cerr << "Failed to open settings file for writing: " << g_settings_path << std::endl;
        return -1;
    }

    file << "# TapeXPlayer Settings File\n";
    file << "# Generated automatically - do not edit manually\n\n";

    // Audio settings
    file << "[Audio]\n";
    file << "device_index=" << g_settings.audio_device_index << "\n";
    file << "master_volume=" << g_settings.audio_master_volume << "\n";
    file << "buffer_size=" << g_settings.audio_buffer_size << "\n";
    file << "\n";

    // Sync settings
    file << "[Sync]\n";
    file << "frame_offset=" << g_settings.frame_offset << "\n";

    // Multi-instance settings
    file << "[MultiInstance]\n";
    file << "auto_freeze_inactive=" << g_settings.auto_freeze_inactive << "\n";

    // MIDI settings
    file << "[MIDI]\n";
    file << "enabled=" << g_settings.midi_enabled << "\n";
    file << "input_port=" << g_settings.midi_input_port << "\n";
    file << "output_port=" << g_settings.midi_output_port << "\n";

    file.close();
    std::cout << "Settings saved to: " << g_settings_path << std::endl;
    return 0;
}

// Simple settings load (primitive parser)
int LoadSettings() {
    std::ifstream file(g_settings_path);
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
            else if (key == "master_volume") g_settings.audio_master_volume = std::stof(value);
            else if (key == "buffer_size") g_settings.audio_buffer_size = std::stoi(value);
        }
        else if (current_section == "Sync") {
            if (key == "frame_offset") g_settings.frame_offset = std::stoi(value);
        }
        else if (current_section == "MultiInstance") {
            if (key == "auto_freeze_inactive") g_settings.auto_freeze_inactive = std::stoi(value);
        }
        else if (current_section == "MIDI") {
            if (key == "enabled") g_settings.midi_enabled = std::stoi(value);
            else if (key == "input_port") g_settings.midi_input_port = std::stoi(value);
            else if (key == "output_port") g_settings.midi_output_port = std::stoi(value);
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