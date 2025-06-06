#include "keyboard_manager.h"
#include "globals.h"
#include "nfd.h"
#include "deep_pause_manager.h"
#include "core/display/window_manager.h"
#include "core/menu/menu_system.h"
#include <iostream>
#include <string>
#include <cstring>
#include <cstdlib>

// Forward declarations for functions used by KeyboardManager
extern void restartPlayerWithFile(const std::string& filename, double seekTime);
extern void generateAndCopyFstpMarkdownLink();
extern void takeCurrentFrameScreenshot();
extern void seek_to_time(double time);
extern void reset_to_normal_speed();
extern void toggle_pause();
extern void start_jog_forward();
extern void start_jog_backward();
extern void stop_jog();
extern void increase_volume();
extern void decrease_volume();
extern void reset_zoom();
extern void toggle_zoom_thumbnail();
extern void saveWindowSettings(SDL_Window* window);
extern void showMenuBarTemporarily();
extern double parse_timecode(const std::string& timecode);
extern std::string generateTXTimecode(double time);

// External global variables and objects
extern WindowManager* g_windowManager;
extern DeepPauseManager* g_deepPauseManager;
extern SDL_Window* window;
extern std::vector<double> speed_steps;
extern int current_speed_index;

KeyboardManager::KeyboardManager() {
}

KeyboardManager::~KeyboardManager() {
}

void KeyboardManager::handleMenuCommand(int command) {
    switch (command) {
        case MENU_FILE_OPEN: {
            openFileDialog();
            break;
        }
        case MENU_INTERFACE_SELECT: {
            // TODO: Implement interface selection logic
            break;
        }
        case MENU_FILE_COPY_FSTP_URL_MARKDOWN: {
            copyFstpUrlMarkdown();
            break;
        }
        case MENU_EDIT_COPY_SCREENSHOT: {
            copyScreenshot();
            break;
        }
        case MENU_EDIT_GOTO_TIMECODE: {
            enableTimecodeInput();
            break;
        }
        case MENU_VIEW_TOGGLE_BETACAM_EFFECT: {
            betacam_effect_enabled.store(!betacam_effect_enabled.load());
            updateBetacamEffectMenuState(betacam_effect_enabled.load());
            break;
        }
    }
}

void KeyboardManager::openFileDialog() {
    char *outPath = nullptr;
    nfdfilteritem_t filterItem[1] = { { "Video files", "mp4,mov,avi,mkv,wmv,flv,webm" } };
    nfdresult_t result = NFD_OpenDialog(&outPath, filterItem, 1, nullptr);
    if (result == NFD_OKAY) {
        std::string filename = outPath;
        std::cout << "Selected file: " << filename << std::endl;
        
        // Restart player with new file
        restartPlayerWithFile(filename, -1.0);
        free(outPath);
    }
}

void KeyboardManager::copyFstpUrlMarkdown() {
    generateAndCopyFstpMarkdownLink();
}

void KeyboardManager::copyScreenshot() {
    // Take screenshot using the same function as the S key
    takeCurrentFrameScreenshot();
}

void KeyboardManager::enableTimecodeInput() {
    waiting_for_timecode = true;
}

void KeyboardManager::handleKeyboardEvent(const SDL_Event& event) {
    if (event.type == SDL_KEYDOWN) {
        if (waiting_for_timecode) {
            handleTimecodeInput(event);
        } else {
            handleNormalKeyInput(event);
        }
    } else if (event.type == SDL_KEYUP) {
        handleKeyUp(event);
    }
}

void KeyboardManager::handleTimecodeInput(const SDL_Event& event) {
    if (event.key.keysym.sym == SDLK_RETURN || event.key.keysym.sym == SDLK_KP_ENTER) {
        try {
            double target_time = parse_timecode(input_timecode);
            seek_to_time(target_time);
            waiting_for_timecode = false;
            input_timecode.clear();
        } catch (const std::exception& ex) {
        }
    } else if (event.key.keysym.sym == SDLK_BACKSPACE && !input_timecode.empty()) {
        input_timecode.pop_back();
    } else if (input_timecode.length() < 8) {
        const char* keyName = SDL_GetKeyName(event.key.keysym.sym);
        char digit = '\0';
        
        if (keyName[0] >= '0' && keyName[0] <= '9' && keyName[1] == '\0') {
            digit = keyName[0];
        } else if (strncmp(keyName, "Keypad ", 7) == 0 && keyName[7] >= '0' && keyName[7] <= '9' && keyName[8] == '\0') {
            digit = keyName[7];
        }
        
        if (digit != '\0') {
            input_timecode += digit;
        }
    } else if (event.key.keysym.sym == SDLK_ESCAPE) {
        waiting_for_timecode = false;
        input_timecode.clear();
    }
}

void KeyboardManager::handleNormalKeyInput(const SDL_Event& event) {
    switch (event.key.keysym.sym) {
        case SDLK_SPACE:
            if (std::abs(playback_rate.load()) > 1.1) {
                reset_to_normal_speed();
                current_speed_index = 0; // Reset to 1.0x speed (index 0)
            } else {
                toggle_pause();
                if (g_deepPauseManager) {
                    g_deepPauseManager->onPauseToggle(target_playback_rate.load());
                }
            }
            break;
        case SDLK_RIGHT:
            if (event.key.keysym.mod & KMOD_SHIFT) {
                if (event.key.repeat == 0) {
                    start_jog_forward();
                }
            } else {
                target_playback_rate.store(1.0);
                is_reverse.store(false);
            }
            break;
        case SDLK_LEFT:
            if (event.key.keysym.mod & KMOD_SHIFT) {
                if (event.key.repeat == 0) {
                    start_jog_backward();
                }
            } else {
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
            handleEscape();
            break;
        case SDLK_PLUS:
        case SDLK_EQUALS:
            increase_volume();
            break;
        case SDLK_MINUS:
            decrease_volume();
            break;
        case SDLK_o:
            if (event.key.keysym.mod & KMOD_CTRL) {
                openFileDialog();
                if(g_deepPauseManager && g_deepPauseManager->isActive()) g_deepPauseManager->forceExit();
            }
            break;
        case SDLK_d:
            if (event.key.keysym.mod & KMOD_ALT) {
                showOSD = !showOSD;
            } else if (SDL_GetModState() & KMOD_SHIFT) {
                showIndex = !showIndex;
            }
            break;
        case SDLK_1:
        case SDLK_2:
        case SDLK_3:
        case SDLK_4:
        case SDLK_5:
            handleMarkerKeys(event);
            break;
        case SDLK_f:
            if (g_windowManager) {
                g_windowManager->toggleFullscreen();
            }
            break;
        case SDLK_z:
            handleZoomToggle(event);
            break;
        case SDLK_t:
            toggle_zoom_thumbnail();
            break;
        case SDLK_c:
            if (event.key.keysym.mod & KMOD_GUI) {
                takeCurrentFrameScreenshot();
            }
            break;
        case SDLK_m:
            handleMenuKey(event);
            break;
    }
}

void KeyboardManager::handleKeyUp(const SDL_Event& event) {
    switch (event.key.keysym.sym) {
        case SDLK_RIGHT:
        case SDLK_LEFT:
            if (event.key.keysym.mod & KMOD_SHIFT) {
                stop_jog();
            }
            break;
    }
}

void KeyboardManager::handleEscape() {
    saveWindowSettings(window);
    shouldExit = true;
    restart_requested = false;
    if(g_deepPauseManager && g_deepPauseManager->isActive()) g_deepPauseManager->forceExit();
}

void KeyboardManager::handleMarkerKeys(const SDL_Event& event) {
    if (!waiting_for_timecode) {
        int markerIndex = event.key.keysym.sym - SDLK_1;
        if (event.key.keysym.mod & KMOD_ALT) {
            memoryMarkers[markerIndex] = current_audio_time.load();
            std::cout << "Marker " << (markerIndex + 1) << " set at " 
                      << generateTXTimecode(memoryMarkers[markerIndex]) << std::endl;
        } else {
            if (memoryMarkers[markerIndex] >= 0) {
                seek_to_time(memoryMarkers[markerIndex]);
            }
        }
    }
}

void KeyboardManager::handleZoomToggle(const SDL_Event& event) {
    if (!zoom_enabled.load()) {
        int mouseX, mouseY;
        SDL_GetMouseState(&mouseX, &mouseY);
        if (g_windowManager) {
            SDL_Event mutableEvent = event;
            g_windowManager->handleZoomMouseEvent(mutableEvent, g_windowManager->getLastTextureWidth(), g_windowManager->getLastTextureHeight()); 
        }
    } else {
        reset_zoom();
    }
    zoom_enabled.store(!zoom_enabled.load());
}

void KeyboardManager::handleMenuKey(const SDL_Event& event) {
    Uint32 flags = SDL_GetWindowFlags(window);
    if (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) {
#ifdef __APPLE__
        showMenuBarTemporarily();
#endif
    }
}

void KeyboardManager::handleMouseEvent(const SDL_Event& event) {
    switch (event.type) {
        case SDL_MOUSEBUTTONDOWN:
            if (event.button.button == SDL_BUTTON_LEFT && (SDL_GetModState() & KMOD_SHIFT)) {
                startMouseShuttle(event.button.x);
            }
            break;
        case SDL_MOUSEBUTTONUP:
            if (event.button.button == SDL_BUTTON_LEFT && mouse_shuttle_active.load()) {
                stopMouseShuttle();
            }
            break;
        case SDL_MOUSEMOTION:
            if (mouse_shuttle_active.load()) {
                updateMouseShuttle(event.motion.x);
            }
            break;
    }
}

void KeyboardManager::startMouseShuttle(int x) {
    mouse_shuttle_active.store(true);
    mouse_shuttle_start_x = x;
    // Start at 0x speed
    target_playback_rate.store(0.0);
    is_reverse.store(false);
}

void KeyboardManager::updateMouseShuttle(int x) {
    if (!mouse_shuttle_active.load()) return;
    
    int delta_x = x - mouse_shuttle_start_x;
    
    // Calculate target speed based on pixel distance
    // Every 10 pixels = 1x speed increase, max 24x
    double target_speed = std::abs(delta_x) / 10.0;
    target_speed = std::min(target_speed, 24.0); // Cap at 24x
    target_speed = std::max(target_speed, 0.0);  // Minimum 0x
    
    // Determine target direction
    bool target_reverse = (delta_x < 0);
    
    double current_rate = playback_rate.load();
    bool current_reverse = is_reverse.load();
    
    // If we want to change direction and current speed > 0, first slow down to 0
    if (target_reverse != current_reverse && current_rate > 0.1) {
        // Slow down to 0 first before changing direction
        target_playback_rate.store(0.0);
        // Keep current direction until we reach 0
    } else if (target_reverse != current_reverse && current_rate <= 0.1) {
        // Now we can change direction since we're at ~0 speed
        is_reverse.store(target_reverse);
        target_playback_rate.store(target_speed);
    } else {
        // Same direction or already at 0, just set target speed
        target_playback_rate.store(target_speed);
    }
}

void KeyboardManager::stopMouseShuttle() {
    mouse_shuttle_active.store(false);
    // Return to normal 1x speed
    target_playback_rate.store(1.0);
    is_reverse.store(false);
}