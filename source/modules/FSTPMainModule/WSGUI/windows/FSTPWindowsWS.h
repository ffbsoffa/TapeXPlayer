#ifndef FSTP_WINDOWS_WS_H
#define FSTP_WINDOWS_WS_H

#ifdef _WIN32
#include <windows.h>
#include <SDL.h>
#include "../FSTPWindowManager.h"

class FSTPWindowsWS : public FSTPWindowManager {
public:
    FSTPWindowsWS();
    ~FSTPWindowsWS() override;

    // Window management
    bool CreateMainWindow(const char* title, int width, int height) override;
    void DestroyMainWindow() override;
    void ShowWindow() override;
    void HideWindow() override;
    void SetFullscreen(bool fullscreen) override;
    
    // Event handling
    bool ProcessEvents() override;
    void UpdateWindow() override;
    
    // Clipboard operations
    bool CopyToClipboard(const char* text) override;
    char* GetClipboardText() override;
    
    // Window state
    bool IsFullscreen() const override;
    void GetWindowSize(int& width, int& height) const override;
    void SetWindowSize(int width, int height) override;
    
private:
    SDL_Window* sdlWindow;
    SDL_Renderer* sdlRenderer;
    bool isFullscreen;
    
    // Windows-specific members
    HWND hwnd;  // Native window handle
    HINSTANCE hInstance;
};

#else
#include "../FSTPWindowManager.h"

class FSTPWindowsWS : public FSTPWindowManager {
public:
    FSTPWindowsWS() = default;
    ~FSTPWindowsWS() override = default;

    bool CreateMainWindow(const char*, int, int) override { return false; }
    void DestroyMainWindow() override {}
    void ShowWindow() override {}
    void HideWindow() override {}
    void SetFullscreen(bool) override {}

    bool ProcessEvents() override { return false; }
    void UpdateWindow() override {}

    bool CopyToClipboard(const char*) override { return false; }
    char* GetClipboardText() override { return nullptr; }

    bool IsFullscreen() const override { return false; }
    void GetWindowSize(int& width, int& height) const override { width = 0; height = 0; }
    void SetWindowSize(int, int) override {}
};

#endif

#endif // FSTP_WINDOWS_WS_H
