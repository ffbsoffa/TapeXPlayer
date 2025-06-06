#ifndef KEYBOARD_MANAGER_H
#define KEYBOARD_MANAGER_H

#include <SDL2/SDL.h>
#include "core/menu/menu_system.h"

class KeyboardManager {
public:
    KeyboardManager();
    ~KeyboardManager();
    
    void handleMenuCommand(int command);
    void handleKeyboardEvent(const SDL_Event& event);
    void handleMouseEvent(const SDL_Event& event);
    
private:
    void openFileDialog();
    void copyFstpUrlMarkdown();
    void copyScreenshot();
    void enableTimecodeInput();
    
    void handleTimecodeInput(const SDL_Event& event);
    void handleNormalKeyInput(const SDL_Event& event);
    void handleKeyUp(const SDL_Event& event);
    void handleEscape();
    void handleMarkerKeys(const SDL_Event& event);
    void handleZoomToggle(const SDL_Event& event);
    void handleMenuKey(const SDL_Event& event);
    
    void startMouseShuttle(int x);
    void updateMouseShuttle(int x);
    void stopMouseShuttle();
};

#endif // KEYBOARD_MANAGER_H