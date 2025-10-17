#ifndef FSTP_WINDOWS_WS_H
#define FSTP_WINDOWS_WS_H

#include <windows.h>
#include <SDL.h>
#include "../FSTPWindowManager.h"

class FSTPWindowsWS : public FSTPWindowManager {
public:
    FSTPWindowsWS();
    virtual ~FSTPWindowsWS();

    // Window management
    virtual bool CreateMainWindow(const char* title, int width, int height) override;
    virtual void DestroyMainWindow() override;
    virtual void ShowWindow() override;
    virtual void HideWindow() override;
    virtual void SetFullscreen(bool fullscreen) override;
    
    // Event handling
    virtual bool ProcessEvents() override;
    virtual void UpdateWindow() override;
    
    // Clipboard operations
    virtual bool CopyToClipboard(const char* text) override;
    virtual char* GetClipboardText() override;
    
    // Window state
    virtual bool IsFullscreen() const override;
    virtual void GetWindowSize(int& width, int& height) const override;
    virtual void SetWindowSize(int width, int height) override;
    
private:
    SDL_Window* sdlWindow;
    SDL_Renderer* sdlRenderer;
    bool isFullscreen;
    
    // Windows-specific members
    HWND hwnd;  // Native window handle
    HINSTANCE hInstance;
};

#endif // FSTP_WINDOWS_WS_H
