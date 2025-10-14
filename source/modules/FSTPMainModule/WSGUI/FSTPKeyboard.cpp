#include "FSTPKeyboard.h"
#include "FSTPPlayerManager.h"
#include "FSTPOSDSystem.h"
#include "FSTPWindowManager.h"
#include "FSTPPixelBufferManager.h"
#include "FSTPZoom.h"
#include "FSTPScreenshot.h"
#include "FSTPToolsMenu.h"
#include "FSTPMemoryLocations.h"
#include "FSTPHardwareDetection.h"
#include <iostream>
#include <cmath>

extern "C" {
#include <libavutil/frame.h>
}

#ifdef __APPLE__
#include "darwin/sdl/FSTPDarwinWS.h"
#endif

#ifdef __linux__
#include "linux/FSTPWaylandWS.h"
#endif

// Mouse Shuttle variables
static bool mouse_shuttle_active = false;
static int mouse_shuttle_start_x = 0;
static int mouse_shuttle_start_y = 0;
static auto mouse_shuttle_click_time = std::chrono::steady_clock::now();

// Zoom Panning variables (for increased FPS during active panning)
static bool zoom_panning_active = false;

// Timecode Seek variables
static bool timecode_seek_active = false;
static std::string timecode_input = "";
static int timecode_seek_player_id = -1;

// Parse timecode (HH:MM:SS:FF or MMSSFF format) to seconds
static double ParseTimecode(const std::string& input, double fps) {
    if (fps <= 0) fps = 25.0;

    // Remove all non-digit characters
    std::string digits;
    for (char c : input) {
        if (c >= '0' && c <= '9') {
            digits += c;
        }
    }

    if (digits.empty()) return 0.0;

    // Pad with zeros to 8 digits (HHMMSSFF)
    while (digits.length() < 8) {
        digits = "0" + digits;
    }

    // Parse HHMMSSFF
    int hours = std::stoi(digits.substr(0, 2));
    int minutes = std::stoi(digits.substr(2, 2));
    int seconds = std::stoi(digits.substr(4, 2));
    int frames = std::stoi(digits.substr(6, 2));

    double total_seconds = hours * 3600.0 + minutes * 60.0 + seconds + (frames / fps);
    return total_seconds;
}

// Generate timecode string from seconds (HH:MM:SS:FF format)
static std::string GenerateTimecode(double time_seconds, double fps) {
    if (fps <= 0) fps = 25.0;  // Default to 25 FPS

    int64_t total_frames = static_cast<int64_t>(std::round(time_seconds * fps));
    int hours = static_cast<int>(total_frames / (3600 * fps));
    int minutes = static_cast<int>((total_frames / (60 * fps))) % 60;
    int seconds = static_cast<int>((total_frames / fps)) % 60;
    int frames = static_cast<int>(total_frames % static_cast<int64_t>(std::round(fps)));

    char timecode[12];
    snprintf(timecode, sizeof(timecode), "%02d:%02d:%02d:%02d", hours, minutes, seconds, frames);
    return std::string(timecode);
}

// Helper function to get window index by player_id
static int GetWindowIndexByPlayerID(int player_id) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        FSTPWindow* window = GetWindowByIndex(i);
        if (window && window->is_active && window->player_instance_id == player_id) {
            return i;
        }
    }
    return -1;
}

// Helper functions for player control
static void HandlePlaybackControl(int player_id, const char* action) {
    if (player_id < 0) {
        std::cout << "No active player for " << action << std::endl;
        return;
    }

    std::cout << "Player " << player_id << ": " << action << std::endl;

    // Get window index for OSD update
    int window_index = GetWindowIndexByPlayerID(player_id);
    if (window_index < 0) {
        std::cout << "No window found for player " << player_id << std::endl;
        return;
    }

    // Update OSD with current player state
    bool is_playing = IsInstancePlaying(player_id);
    double current_position = GetInstancePosition(player_id);
    double duration = GetInstanceDuration(player_id);
    double speed = GetInstanceSpeed(player_id);
    bool is_reverse = IsInstanceReverse(player_id);

    // Update OSD for specific window
    UpdateWindowOSD(window_index, current_position, duration, is_playing, speed, is_reverse);
}

static void HandleSpeedControl(int player_id, double speed_change) {
    if (player_id < 0) return;

    double current_speed = GetInstanceSpeed(player_id);
    double new_speed = current_speed + speed_change;

    // Fixed speed values: 1x, 3x, 10x, 18x, 24x, 32x
    double speed_steps[] = {1.0, 3.0, 10.0, 18.0, 24.0, 32.0};
    int num_steps = sizeof(speed_steps) / sizeof(speed_steps[0]);

    // Apply CPU-specific speed limit
    if (g_hardware_detection) {
        double max_speed = g_hardware_detection->GetMaxRecommendedSpeed();
        // Filter out speeds above CPU limit
        int filtered_steps = 0;
        for (int i = 0; i < num_steps; i++) {
            if (speed_steps[i] <= max_speed) {
                filtered_steps = i + 1;
            }
        }
        if (filtered_steps > 0 && filtered_steps < num_steps) {
            num_steps = filtered_steps;
        }
    }

    // Find nearest speed value
    double closest_speed = speed_steps[0];
    double min_diff = std::abs(new_speed - speed_steps[0]);
    
    for (int i = 1; i < num_steps; i++) {
        double diff = std::abs(new_speed - speed_steps[i]);
        if (diff < min_diff) {
            min_diff = diff;
            closest_speed = speed_steps[i];
        }
    }
    
    if (SetInstanceSpeed(player_id, closest_speed) == 0) {
        std::cout << "Player " << player_id << ": Speed changed to " << closest_speed << "x" << std::endl;

        // Get window index for OSD update
        int window_index = GetWindowIndexByPlayerID(player_id);
        if (window_index >= 0) {
            // Update OSD with new speed
            bool is_playing = IsInstancePlaying(player_id);
            double current_position = GetInstancePosition(player_id);
            double duration = GetInstanceDuration(player_id);
            bool is_reverse = IsInstanceReverse(player_id);

            UpdateWindowOSD(window_index, current_position, duration, is_playing, closest_speed, is_reverse);
        }
    }
}

static void HandleSeekControl(int player_id, double seek_offset) {
    if (player_id < 0) return;
    
    double current_position = GetInstancePosition(player_id);
    double new_position = current_position + seek_offset;
    
    // Clamp position within file bounds
    double duration = GetInstanceDuration(player_id);
    if (new_position < 0) new_position = 0;
    if (new_position > duration) new_position = duration;

    if (SeekInstance(player_id, new_position) == 0) {
        std::cout << "Player " << player_id << ": Seeked to " << new_position << " seconds" << std::endl;

        // Get window index for OSD update
        int window_index = GetWindowIndexByPlayerID(player_id);
        if (window_index >= 0) {
            // Update OSD with new position
            bool is_playing = IsInstancePlaying(player_id);
            double speed = GetInstanceSpeed(player_id);
            bool is_reverse = IsInstanceReverse(player_id);

            UpdateWindowOSD(window_index, new_position, duration, is_playing, speed, is_reverse);
        }
    }
}

static void HandleVolumeControl(int player_id, double volume_change) {
    if (player_id < 0) return;
    
    // TODO: Implement volume control through audio module
    std::cout << "Player " << player_id << ": Volume change " << volume_change << std::endl;
}

static bool ShouldResetSpeedBeforePlay(int player_id) {
    if (player_id < 0) return false;
    
    double current_speed = GetInstanceSpeed(player_id);
    return current_speed > 1.0;
}

static void HandleSpeedStepUp(int player_id) {
    if (player_id < 0) return;

    double current_speed = GetInstanceSpeed(player_id);

    // Fixed speed values: 1x, 3x, 10x, 18x, 24x, 32x
    double speed_steps[] = {1.0, 3.0, 10.0, 18.0, 24.0, 32.0};
    int num_steps = sizeof(speed_steps) / sizeof(speed_steps[0]);

    // Apply CPU-specific speed limit
    if (g_hardware_detection) {
        double max_speed = g_hardware_detection->GetMaxRecommendedSpeed();
        // Filter out speeds above CPU limit
        int filtered_steps = 0;
        for (int i = 0; i < num_steps; i++) {
            if (speed_steps[i] <= max_speed) {
                filtered_steps = i + 1;
            }
        }
        if (filtered_steps > 0 && filtered_steps < num_steps) {
            num_steps = filtered_steps;
        }
    }

    // Find next speed step
    for (int i = 0; i < num_steps; i++) {
        if (speed_steps[i] > current_speed) {
            if (SetInstanceSpeed(player_id, speed_steps[i]) == 0) {
                std::cout << "Player " << player_id << ": Speed increased to " << speed_steps[i] << "x" << std::endl;

                // Get window index for OSD update
                int window_index = GetWindowIndexByPlayerID(player_id);
                if (window_index >= 0) {
                    // Update OSD with new speed
                    bool is_playing = IsInstancePlaying(player_id);
                    double current_position = GetInstancePosition(player_id);
                    double duration = GetInstanceDuration(player_id);
                    bool is_reverse = IsInstanceReverse(player_id);

                    UpdateWindowOSD(window_index, current_position, duration, is_playing, speed_steps[i], is_reverse);
                }
            }
            return;
        }
    }

    // If already at maximum speed, stay there
    std::cout << "Player " << player_id << ": Already at maximum speed " << current_speed << "x" << std::endl;
}

static void HandleSpeedStepDown(int player_id) {
    if (player_id < 0) return;

    double current_speed = GetInstanceSpeed(player_id);

    // Fixed speed values: 1x, 3x, 10x, 18x, 24x, 32x
    double speed_steps[] = {1.0, 3.0, 10.0, 18.0, 24.0, 32.0};
    int num_steps = sizeof(speed_steps) / sizeof(speed_steps[0]);

    // Apply CPU-specific speed limit
    if (g_hardware_detection) {
        double max_speed = g_hardware_detection->GetMaxRecommendedSpeed();
        // Filter out speeds above CPU limit
        int filtered_steps = 0;
        for (int i = 0; i < num_steps; i++) {
            if (speed_steps[i] <= max_speed) {
                filtered_steps = i + 1;
            }
        }
        if (filtered_steps > 0 && filtered_steps < num_steps) {
            num_steps = filtered_steps;
        }
    }

    // Find previous speed step
    for (int i = num_steps - 1; i >= 0; i--) {
        if (speed_steps[i] < current_speed) {
            if (SetInstanceSpeed(player_id, speed_steps[i]) == 0) {
                std::cout << "Player " << player_id << ": Speed decreased to " << speed_steps[i] << "x" << std::endl;

                // Get window index for OSD update
                int window_index = GetWindowIndexByPlayerID(player_id);
                if (window_index >= 0) {
                    // Update OSD with new speed
                    bool is_playing = IsInstancePlaying(player_id);
                    double current_position = GetInstancePosition(player_id);
                    double duration = GetInstanceDuration(player_id);
                    bool is_reverse = IsInstanceReverse(player_id);

                    UpdateWindowOSD(window_index, current_position, duration, is_playing, speed_steps[i], is_reverse);
                }
            }
            return;
        }
    }

    // If already at minimum speed, stay there
    std::cout << "Player " << player_id << ": Already at minimum speed " << current_speed << "x" << std::endl;
}

bool HandleKeyboardEvents(SDL_Event& event) {
    // Get active player ID outside switch
    int active_player_id = GetActivePlayerID();

    switch (event.type) {
        case SDL_QUIT:
            return false; // Exit application

        case SDL_KEYDOWN:
            // === TIMECODE INPUT MODE ===
            if (timecode_seek_active) {
                // ESC - cancel input
                if (event.key.keysym.sym == SDLK_ESCAPE) {
                    timecode_seek_active = false;
                    timecode_input.clear();
                    UpdateOSDSeekMode(timecode_seek_player_id, false, "");
                    std::cout << "Timecode seek cancelled" << std::endl;
                    break;
                }

                // Return or NumPad Enter - confirm and seek
                if (event.key.keysym.sym == SDLK_RETURN || event.key.keysym.sym == SDLK_KP_ENTER) {
                    if (!timecode_input.empty() && timecode_seek_player_id >= 0) {
                        double fps = GetInstanceVideoFPS(timecode_seek_player_id);
                        if (fps <= 0) fps = 25.0;  // Fallback if FPS not available
                        double seek_time = ParseTimecode(timecode_input, fps);
                        std::cout << "Seeking to timecode: " << timecode_input << " (" << seek_time << " seconds, FPS=" << fps << ")" << std::endl;
                        SeekInstance(timecode_seek_player_id, seek_time);
                    }
                    timecode_seek_active = false;
                    timecode_input.clear();
                    UpdateOSDSeekMode(timecode_seek_player_id, false, "");
                    break;
                }

                // Backspace - delete last digit
                if (event.key.keysym.sym == SDLK_BACKSPACE) {
                    if (!timecode_input.empty()) {
                        timecode_input.pop_back();
                        UpdateOSDSeekMode(timecode_seek_player_id, true, timecode_input);
                    }
                    break;
                }

                // Digits from regular keyboard (0-9)
                if (event.key.keysym.sym >= SDLK_0 && event.key.keysym.sym <= SDLK_9) {
                    if (timecode_input.length() < 8) {  // Maximum 8 digits (HHMMSSFF)
                        timecode_input += (char)('0' + (event.key.keysym.sym - SDLK_0));
                        UpdateOSDSeekMode(timecode_seek_player_id, true, timecode_input);
                    }
                    break;
                }

                // Digits from NumPad (KP_0 - KP_9)
                switch (event.key.keysym.sym) {
                    case SDLK_KP_0: case SDLK_KP_1: case SDLK_KP_2: case SDLK_KP_3: case SDLK_KP_4:
                    case SDLK_KP_5: case SDLK_KP_6: case SDLK_KP_7: case SDLK_KP_8: case SDLK_KP_9:
                        if (timecode_input.length() < 8) {
                            // Convert SDLK_KP_X to digit
                            char digit = '0';
                            switch (event.key.keysym.sym) {
                                case SDLK_KP_0: digit = '0'; break;
                                case SDLK_KP_1: digit = '1'; break;
                                case SDLK_KP_2: digit = '2'; break;
                                case SDLK_KP_3: digit = '3'; break;
                                case SDLK_KP_4: digit = '4'; break;
                                case SDLK_KP_5: digit = '5'; break;
                                case SDLK_KP_6: digit = '6'; break;
                                case SDLK_KP_7: digit = '7'; break;
                                case SDLK_KP_8: digit = '8'; break;
                                case SDLK_KP_9: digit = '9'; break;
                            }
                            timecode_input += digit;
                            UpdateOSDSeekMode(timecode_seek_player_id, true, timecode_input);
                        }
                        break;
                    default:
                        break;
                }

                // Ignore all other keys in input mode
                break;
            }

#ifdef __APPLE__
            // Check if text field is active in any window
            // If yes - ignore all player keys (except Cmd+combinations)
            // IMPORTANT: Always block Space when field is active to avoid
            // conflict with language switching via Command+Space
            if (IsTextFieldActive() || IsMemoryLocationDialogActive()) {
                if (event.key.keysym.sym == SDLK_SPACE) {
                    break; // Always block Space when text field or dialog is active
                }
                if (!(event.key.keysym.mod & KMOD_GUI)) {
                    break; // User is typing or dialog is open - ignore other keys
                }
            }
#endif

            // Cmd+G - activate timecode input mode
            if ((event.key.keysym.mod & KMOD_GUI) && event.key.keysym.sym == SDLK_g) {
                if (active_player_id >= 0) {
                    timecode_seek_active = true;
                    timecode_input.clear();
                    timecode_seek_player_id = active_player_id;
                    UpdateOSDSeekMode(active_player_id, true, timecode_input);
                    std::cout << "Timecode seek mode activated (Cmd+G)" << std::endl;
                }
                break;
            }

            // * on NumPad - activate timecode input mode
            if (event.key.keysym.sym == SDLK_KP_MULTIPLY) {
                if (active_player_id >= 0) {
                    timecode_seek_active = true;
                    timecode_input.clear();
                    timecode_seek_player_id = active_player_id;
                    UpdateOSDSeekMode(active_player_id, true, timecode_input);
                    std::cout << "Timecode seek mode activated (NumPad *)" << std::endl;
                }
                break;
            }

            switch (event.key.keysym.sym) {
                case SDLK_ESCAPE:
                    return false; // ESC to exit

                // === Playback Control ===
                case SDLK_SPACE:
                    // Space - Play/Pause with smart logic
                    if (active_player_id >= 0) {
                        if (IsInstancePlaying(active_player_id)) {
                            // If playing - check speed
                            if (ShouldResetSpeedBeforePlay(active_player_id)) {
                                // If speed > 1x - reset to 1x (first space during fast-forward)
                                SetInstanceSpeed(active_player_id, 1.0);
                                HandleSpeedControl(active_player_id, 1.0);
                                std::cout << "Player " << active_player_id << ": Speed reset to 1x during rewind" << std::endl;
                            } else {
                                // If speed = 1x - pause (second space)
                                PauseInstance(active_player_id);
                                HandlePlaybackControl(active_player_id, "Paused");
                            }
                        } else {
                            // If not playing - check speed
                            if (ShouldResetSpeedBeforePlay(active_player_id)) {
                                // If speed > 1x - reset to 1x (first space when stopped)
                                SetInstanceSpeed(active_player_id, 1.0);
                                HandleSpeedControl(active_player_id, 1.0);
                                std::cout << "Player " << active_player_id << ": Speed reset to 1x before play" << std::endl;
                            } else {
                                // If speed = 1x - start playback
                                PlayInstance(active_player_id);
                                HandlePlaybackControl(active_player_id, "Playing");
                            }
                        }
                    }
                    break;

                case SDLK_p:
                    // P - Play
                    if (active_player_id >= 0) {
                        PlayInstance(active_player_id);
                        HandlePlaybackControl(active_player_id, "Play");
                    }
                    break;

                case SDLK_s:
                    // S - Stop
                    if (active_player_id >= 0) {
                        StopInstance(active_player_id);
                        HandlePlaybackControl(active_player_id, "Stop");
                    }
                    break;

                // === Speed Control ===
                case SDLK_UP:
                    // Arrow up - increase speed
                    HandleSpeedStepUp(active_player_id);
                    break;

                case SDLK_DOWN:
                    // Arrow down - decrease speed
                    HandleSpeedStepDown(active_player_id);
                    break;

                case SDLK_PLUS:
                case SDLK_EQUALS:
                    // + or = - increase speed
                    HandleSpeedControl(active_player_id, 0.25);
                    break;

                case SDLK_MINUS:
                    // - - decrease speed
                    HandleSpeedControl(active_player_id, -0.25);
                    break;

                case SDLK_1:
                    // 1 - 1x speed
                    if (SetInstanceSpeed(active_player_id, 1.0) == 0) {
                        std::cout << "Player " << active_player_id << ": Speed set to 1.0x" << std::endl;

                        // Get window index for OSD update
                        int window_index = GetWindowIndexByPlayerID(active_player_id);
                        if (window_index >= 0) {
                            bool is_playing = IsInstancePlaying(active_player_id);
                            double current_position = GetInstancePosition(active_player_id);
                            double duration = GetInstanceDuration(active_player_id);
                            bool is_reverse = IsInstanceReverse(active_player_id);

                            UpdateWindowOSD(window_index, current_position, duration, is_playing, 1.0, is_reverse);
                        }
                    }
                    break;

                case SDLK_2:
                    // 2 - 3x speed (closest to 2x)
                    if (SetInstanceSpeed(active_player_id, 3.0) == 0) {
                        std::cout << "Player " << active_player_id << ": Speed set to 3.0x" << std::endl;

                        // Get window index for OSD update
                        int window_index = GetWindowIndexByPlayerID(active_player_id);
                        if (window_index >= 0) {
                            bool is_playing = IsInstancePlaying(active_player_id);
                            double current_position = GetInstancePosition(active_player_id);
                            double duration = GetInstanceDuration(active_player_id);
                            bool is_reverse = IsInstanceReverse(active_player_id);

                            UpdateWindowOSD(window_index, current_position, duration, is_playing, 3.0, is_reverse);
                        }
                    }
                    break;

                case SDLK_0:
                    // 0 - 0.5x speed or increase volume
                    if (event.key.keysym.mod & KMOD_SHIFT) {
                        // Shift+0 - increase volume
                        HandleVolumeControl(active_player_id, 0.1);
                    } else {
                        // 0 - 0.5x speed
                        HandleSpeedControl(active_player_id, 0.5);
                    }
                    break;

                // === Position Control ===
                case SDLK_LEFT:
                    // Arrow left - back 10 seconds
                    if (event.key.keysym.mod & KMOD_SHIFT) {
                        HandleSeekControl(active_player_id, -60.0); // Shift+Left = 1 minute back
                    } else {
                        HandleSeekControl(active_player_id, -10.0); // 10 seconds back
                    }
                    break;

                case SDLK_RIGHT:
                    // Arrow right - forward 10 seconds
                    if (event.key.keysym.mod & KMOD_SHIFT) {
                        HandleSeekControl(active_player_id, 60.0); // Shift+Right = 1 minute forward
                    } else {
                        HandleSeekControl(active_player_id, 10.0); // 10 seconds forward
                    }
                    break;

                // SDLK_UP and SDLK_DOWN now used for speed control

                case SDLK_HOME:
                    // Home - to beginning
                    HandleSeekControl(active_player_id, -999999.0); // Large negative value
                    break;

                case SDLK_END:
                    // End - to end
                    HandleSeekControl(active_player_id, 999999.0); // Large positive value
                    break;

                // === Volume Control ===
                case SDLK_9:
                    // 9 - decrease volume
                    HandleVolumeControl(active_player_id, -0.1);
                    break;

                case SDLK_m:
                    // M - Mute/Unmute
                    HandleVolumeControl(active_player_id, 0.0); // Toggle mute
                    break;

                // === Special Functions ===
                case SDLK_r:
                    // R - Reverse toggle
                    if (active_player_id >= 0) {
                        bool current_reverse = IsInstanceReverse(active_player_id);
                        SetInstanceReverse(active_player_id, !current_reverse);
                        std::cout << "Player " << active_player_id << ": Reverse " << (!current_reverse ? "ON" : "OFF") << std::endl;
                    }
                    break;

                case SDLK_t:
                    // T - Toggle Time/Frame display mode
                    if (active_player_id >= 0) {
                        bool current_mode = GetOSDFrameNumberMode(active_player_id);
                        SetOSDFrameNumberMode(active_player_id, !current_mode);
                        std::cout << "Player " << active_player_id << ": Display mode " << (!current_mode ? "Frame Numbers" : "Timecode") << std::endl;
                    }
                    break;

                case SDLK_f:
                    // F - Fullscreen toggle
                    if (active_player_id >= 0) {
                        std::cout << "Player " << active_player_id << ": Toggle Fullscreen" << std::endl;
                        // TODO: Implement fullscreen mode toggle
                    }
                    break;

                case SDLK_i:
                    // I - Show Info
                    if (active_player_id >= 0) {
                        double position = GetInstancePosition(active_player_id);
                        double duration = GetInstanceDuration(active_player_id);
                        double speed = GetInstanceSpeed(active_player_id);
                        bool reverse = IsInstanceReverse(active_player_id);
                        
                        std::cout << "=== Player " << active_player_id << " Info ===" << std::endl;
                        std::cout << "Position: " << position << " / " << duration << " seconds" << std::endl;
                        std::cout << "Speed: " << speed << "x" << std::endl;
                        std::cout << "Reverse: " << (reverse ? "ON" : "OFF") << std::endl;
                        std::cout << "Playing: " << (IsInstancePlaying(active_player_id) ? "YES" : "NO") << std::endl;
                    }
                    break;

                // === System Commands ===
                case SDLK_o:
                    std::cout << "🎹 Key 'O' pressed, modifiers: " << event.key.keysym.mod << std::endl;
#ifdef __APPLE__
                    if (event.key.keysym.mod & KMOD_GUI) { // Cmd+O on macOS
                        std::cout << "📱 macOS: Cmd+O detected" << std::endl;
                        // File open dialog is handled through menu
                        // Don't duplicate call here
                    }
#endif
#ifdef __linux__
                    std::cout << "🐧 Linux: Checking for Ctrl modifier..." << std::endl;
                    if (event.key.keysym.mod & KMOD_CTRL) { // Ctrl+O on Linux
                        std::cout << "⌨️  Ctrl+O pressed, opening file dialog..." << std::endl;
                        ShowNativeFileDialog();
                    } else {
                        std::cout << "⚠️ No Ctrl modifier detected (mod=" << event.key.keysym.mod << ")" << std::endl;
                    }
#endif
                    break;

                case SDLK_COMMA:
#ifdef __APPLE__
                    if (event.key.keysym.mod & KMOD_GUI) { // Cmd+, on macOS
                        ShowNativeSettingsDialog();
                    }
#endif
#ifdef __linux__
                    if (event.key.keysym.mod & KMOD_CTRL) { // Ctrl+, on Linux
                        std::cout << "⚙️  Ctrl+, pressed, opening Settings dialog..." << std::endl;
                        ShowGTKSettingsDialog();
                    }
#endif
                    break;

                case SDLK_q:
                    if (event.key.keysym.mod & KMOD_GUI) { // Cmd+Q on macOS
                        return false;
                    }
                    break;

                // ZOOM CONTROLS
                case SDLK_z:
                    if (event.key.keysym.mod & KMOD_GUI) {
                        // Cmd+Z = Zoom In
                        int active_window = GetActivePlayerID();
                        int window_idx = GetWindowIndexByPlayerID(active_window);
                        if (window_idx >= 0) {
                            IncreaseZoom(window_idx);
                        }
                    } else if (event.key.keysym.mod & KMOD_SHIFT) {
                        // Shift+Z = Zoom Out
                        int active_window = GetActivePlayerID();
                        int window_idx = GetWindowIndexByPlayerID(active_window);
                        if (window_idx >= 0) {
                            DecreaseZoom(window_idx);
                        }
                    }
                    break;

                case SDLK_x:
                    // X = Reset Zoom
                    {
                        int active_window = GetActivePlayerID();
                        int window_idx = GetWindowIndexByPlayerID(active_window);
                        if (window_idx >= 0) {
                            ResetZoom(window_idx);
                        }
                    }
                    break;

                case SDLK_v:
                    // V = Toggle thumbnail
                    {
                        int active_window = GetActivePlayerID();
                        int window_idx = GetWindowIndexByPlayerID(active_window);
                        if (window_idx >= 0) {
                            ToggleZoomThumbnail(window_idx);
                        }
                    }
                    break;

                case SDLK_c:
#ifdef __APPLE__
                    if (event.key.keysym.mod & KMOD_GUI) {
                        // Cmd+C = Take Screenshot
                        CopyScreenshotToClipboard();
                    }
#endif
#ifdef __linux__
                    if (event.key.keysym.mod & KMOD_CTRL) {
                        // Ctrl+C = Take Screenshot
                        std::cout << "📸 Ctrl+C pressed, taking screenshot..." << std::endl;
                        CopyScreenshotToClipboard();
                    }
#endif
                    break;

                case SDLK_n:
                    if (event.key.keysym.mod & KMOD_GUI) {
                        // Cmd+N handled through macOS menu, don't duplicate here
                        std::cout << "Cmd+N: handled by macOS menu" << std::endl;
                    }
                    break;

                // === Memory Locations ===
                case SDLK_RETURN:
                case SDLK_KP_ENTER:
                    // Return or Enter on numpad - create Memory Location
#ifdef __APPLE__
                    CreateMemoryLocationAtCurrentTime();
                    std::cout << "📍 Creating Memory Location via hotkey..." << std::endl;
#endif
#ifdef __linux__
                    if (active_player_id >= 0) {
                        double current_time = GetInstancePosition(active_player_id);
                        std::cout << "📍 Creating Memory Location via hotkey (Linux)..." << std::endl;
                        ShowGTKMemoryLocationDialog(active_player_id, current_time);
                    }
#endif
                    break;
            }
            break;

        case SDL_MOUSEBUTTONDOWN:
            if (event.button.button == SDL_BUTTON_LEFT) {
                // Check if this is a compact device (GPD Pocket, etc.)
                bool is_compact = (g_hardware_detection && g_hardware_detection->IsCompactDevice());

                if (is_compact) {
                    // For compact devices: LEFT CLICK starts mouse shuttle after small delay
                    // Store click position and time - will activate shuttle on motion
                    mouse_shuttle_start_x = event.button.x;
                    mouse_shuttle_start_y = event.button.y;
                    mouse_shuttle_click_time = std::chrono::steady_clock::now();
                    // Don't activate immediately - wait for drag motion
                } else if (SDL_GetModState() & KMOD_SHIFT) {
                    // Standard devices: Shift+Ctrl+Click = Mouse Shuttle
                    if (SDL_GetModState() & KMOD_CTRL) {
                        StartMouseShuttle(event.button.x, event.button.y);
                    } else if (SDL_GetModState() & KMOD_ALT) {
                        // Shift+Alt+Click = Zoom Panning (only if zoom is active!)
                        int active_window = GetActivePlayerID();
                        int window_idx = GetWindowIndexByPlayerID(active_window);
                        if (window_idx >= 0) {
                            FSTPZoomState* zoom = GetZoomState(window_idx);
                            if (zoom && zoom->enabled) {
                                zoom_panning_active = true;
                            }
                        }
                    }
                }
            }
            break;

        case SDL_MOUSEBUTTONUP:
            if (event.button.button == SDL_BUTTON_LEFT) {
                if (mouse_shuttle_active) {
                    StopMouseShuttle();
                }
                if (zoom_panning_active) {
                    zoom_panning_active = false;
                }
            }
            break;

        case SDL_MOUSEMOTION:
            if (mouse_shuttle_active) {
                UpdateMouseShuttle(event.motion.x);
            } else if (event.motion.state & SDL_BUTTON_LMASK) {
                // Left button is pressed
                bool is_compact = (g_hardware_detection && g_hardware_detection->IsCompactDevice());

                if (is_compact) {
                    // For compact devices: activate shuttle after dragging a few pixels
                    // This prevents accidental activation on simple clicks
                    int dx = event.motion.x - mouse_shuttle_start_x;
                    int dy = event.motion.y - mouse_shuttle_start_y;
                    int distance = std::sqrt(dx*dx + dy*dy);

                    // Threshold: 10 pixels of movement to activate
                    if (distance >= 10) {
                        auto now = std::chrono::steady_clock::now();
                        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now - mouse_shuttle_click_time).count();

                        // Also check time: within 500ms to prevent window drag conflict
                        if (elapsed < 500) {
                            StartMouseShuttle(mouse_shuttle_start_x, mouse_shuttle_start_y);
                        }
                    }
                } else if ((SDL_GetModState() & KMOD_SHIFT) &&
                           (SDL_GetModState() & KMOD_ALT) &&
                           !(SDL_GetModState() & KMOD_CTRL)) {
                    // Standard devices: Shift+Alt+Left Mouse Drag = Move zoom center
                    int active_window = GetActivePlayerID();
                    int window_idx = GetWindowIndexByPlayerID(active_window);

                    if (window_idx >= 0) {
                        FSTPWindow* win = GetWindowByIndex(window_idx);
                        if (win && win->window) {
                            int win_w, win_h;
                            SDL_GetWindowSize(win->window, &win_w, &win_h);

                            // Normalize mouse position to 0.0-1.0
                            float norm_x = (float)event.motion.x / (float)win_w;
                            float norm_y = (float)event.motion.y / (float)win_h;

                            SetZoomCenter(window_idx, norm_x, norm_y);
                        }
                    }
                }
            }
            break;

        case SDL_MOUSEWHEEL:
            {
                int active_window = GetActivePlayerID();
                int window_idx = GetWindowIndexByPlayerID(active_window);

                if (window_idx >= 0) {
                    if (event.wheel.y > 0) {
                        // Scroll up = Zoom in
                        IncreaseZoom(window_idx);
                    } else if (event.wheel.y < 0) {
                        // Scroll down = Zoom out
                        DecreaseZoom(window_idx);
                    }
                }
            }
            break;

        case SDL_WINDOWEVENT:
            if (event.window.event == SDL_WINDOWEVENT_CLOSE) {
                // Handle window close through WindowManager
                // Don't exit application, close specific window
                HandleWindowEvents(&event);
                return true; // Continue application
            }
            break;
    }

    return true; // Continue processing
}

void HandleNativeEvents() {
#ifdef __APPLE__
    HandleNativeAppEvents();
#endif
}

// Mouse Shuttle implementation
void StartMouseShuttle(int x, int y) {
    // std::cout << "StartMouseShuttle called with x=" << x << std::endl;

    mouse_shuttle_active = true;
    mouse_shuttle_start_x = x;
    mouse_shuttle_start_y = y;
    mouse_shuttle_click_time = std::chrono::steady_clock::now();

    int active_player_id = GetActivePlayerID();
    // std::cout << "Active player ID: " << active_player_id << std::endl;

    if (active_player_id >= 0) {
        // Mouse Shuttle automatically enables playback
        PlayInstance(active_player_id);

        // Start with minimum speed (0.1x instead of 0.0x)
        SetInstanceSpeedInstant(active_player_id, 0.1);
        SetInstanceReverse(active_player_id, false);
        std::cout << "Mouse shuttle started at x=" << x << " (auto-play enabled)" << std::endl;
    }
}

void UpdateMouseShuttle(int x) {
    if (!mouse_shuttle_active) return;

    int active_player_id = GetActivePlayerID();
    if (active_player_id < 0) return;

    int delta_x = x - mouse_shuttle_start_x;

    // Smooth transition through zero like real tape
    double target_speed;
    bool target_reverse;

    // Determine target parameters
    if (delta_x >= 0) {
        // Right - forward direction
        target_speed = (double)delta_x / 16.0;
        target_reverse = false;
    } else {
        // Left - reverse direction
        target_speed = (double)std::abs(delta_x) / 16.0;
        target_reverse = true;
    }

    // Apply CPU-specific speed limit for Mouse Shuttle
    double max_speed = 32.0;  // Default max
    if (g_hardware_detection) {
        max_speed = g_hardware_detection->GetMaxRecommendedSpeed();
    }
    target_speed = std::min(target_speed, max_speed);
    target_speed = std::max(target_speed, 0.1); // Minimum 0.1x (audio module rejects 0.0)

    // Simplified logic - always set direction and speed
    SetInstanceReverse(active_player_id, target_reverse);
    SetInstanceSpeedInstant(active_player_id, target_speed);

    // Update OSD
    int window_index = GetWindowIndexByPlayerID(active_player_id);
    if (window_index >= 0) {
        bool is_playing = IsInstancePlaying(active_player_id);
        double current_position = GetInstancePosition(active_player_id);
        double duration = GetInstanceDuration(active_player_id);
        bool is_reverse = IsInstanceReverse(active_player_id);

        UpdateWindowOSD(window_index, current_position, duration, is_playing, target_speed, is_reverse);
    }
}

void StopMouseShuttle() {
    mouse_shuttle_active = false;

    int active_player_id = GetActivePlayerID();
    if (active_player_id >= 0) {
        // Set speed to 1x instantly, then pause
        SetInstanceSpeedInstant(active_player_id, 1.0);
        SetInstanceReverse(active_player_id, false);
        PauseInstance(active_player_id);
        // std::cout << "Mouse shuttle stopped, playback paused" << std::endl;

        // Update OSD
        int window_index = GetWindowIndexByPlayerID(active_player_id);
        if (window_index >= 0) {
            bool is_playing = IsInstancePlaying(active_player_id);
            double current_position = GetInstancePosition(active_player_id);
            double duration = GetInstanceDuration(active_player_id);

            UpdateWindowOSD(window_index, current_position, duration, is_playing, 1.0, false);
        }
    }
}

// Check if zoom panning is active (for increased rendering FPS)
bool IsZoomPanningActive() {
    return zoom_panning_active;
}