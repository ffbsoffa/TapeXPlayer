#include "FSTPWindowsWS.h"
#include "BuildInfo.h"
#include <SDL.h>
#include <SDL_syswm.h>

// Global window manager instance
static FSTPWindowsWS* g_windowManager = nullptr;

// Implementation of the main UI loop (called from main.cpp)
extern "C" int RunMainUILoop() {
    g_windowManager = new FSTPWindowsWS();
    
    if (!g_windowManager->CreateMainWindow("TapeXPlayer", 800, 600)) {
        delete g_windowManager;
        return -1;
    }

    g_windowManager->ShowWindow();

    // Main event loop
    bool running = true;
    while (running) {
        running = g_windowManager->ProcessEvents();
        g_windowManager->UpdateWindow();
        SDL_Delay(16); // ~60 FPS
    }

    delete g_windowManager;
    return 0;
}

FSTPWindowsWS::FSTPWindowsWS()
    : sdlWindow(nullptr)
    , sdlRenderer(nullptr)
    , isFullscreen(false)
    , hwnd(NULL)
    , hInstance(GetModuleHandle(NULL)) {
}

FSTPWindowsWS::~FSTPWindowsWS() {
    DestroyMainWindow();
}

bool FSTPWindowsWS::CreateMainWindow(const char* title, int width, int height) {
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        return false;
    }

    sdlWindow = SDL_CreateWindow(
        title,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        width, height,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
    );

    if (!sdlWindow) {
        return false;
    }

    // Get native window handle
    SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);
    if (SDL_GetWindowWMInfo(sdlWindow, &wmInfo)) {
        hwnd = wmInfo.info.win.window;
    }

    sdlRenderer = SDL_CreateRenderer(sdlWindow, -1, SDL_RENDERER_ACCELERATED);
    if (!sdlRenderer) {
        DestroyMainWindow();
        return false;
    }

    return true;
}

void FSTPWindowsWS::DestroyMainWindow() {
    if (sdlRenderer) {
        SDL_DestroyRenderer(sdlRenderer);
        sdlRenderer = nullptr;
    }
    if (sdlWindow) {
        SDL_DestroyWindow(sdlWindow);
        sdlWindow = nullptr;
    }
    hwnd = NULL;
    SDL_Quit();
}

void FSTPWindowsWS::ShowWindow() {
    if (sdlWindow) {
        SDL_ShowWindow(sdlWindow);
    }
}

void FSTPWindowsWS::HideWindow() {
    if (sdlWindow) {
        SDL_HideWindow(sdlWindow);
    }
}

void FSTPWindowsWS::SetFullscreen(bool fullscreen) {
    if (sdlWindow && fullscreen != isFullscreen) {
        SDL_SetWindowFullscreen(sdlWindow, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
        isFullscreen = fullscreen;
    }
}

bool FSTPWindowsWS::ProcessEvents() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_QUIT:
                return false;
            // Add more event handling as needed
        }
    }
    return true;
}

void FSTPWindowsWS::UpdateWindow() {
    if (sdlRenderer) {
        SDL_RenderPresent(sdlRenderer);
    }
}

bool FSTPWindowsWS::CopyToClipboard(const char* text) {
    if (!text || !OpenClipboard(hwnd)) {
        return false;
    }

    EmptyClipboard();
    size_t len = strlen(text) + 1;
    HGLOBAL hGlob = GlobalAlloc(GMEM_MOVEABLE, len);
    if (!hGlob) {
        CloseClipboard();
        return false;
    }

    memcpy(GlobalLock(hGlob), text, len);
    GlobalUnlock(hGlob);
    SetClipboardData(CF_TEXT, hGlob);
    CloseClipboard();
    return true;
}

char* FSTPWindowsWS::GetClipboardText() {
    if (!OpenClipboard(hwnd)) {
        return nullptr;
    }

    HANDLE hData = GetClipboardData(CF_TEXT);
    if (!hData) {
        CloseClipboard();
        return nullptr;
    }

    char* text = static_cast<char*>(GlobalLock(hData));
    if (text) {
        char* result = SDL_strdup(text);
        GlobalUnlock(hData);
        CloseClipboard();
        return result;
    }

    CloseClipboard();
    return nullptr;
}

bool FSTPWindowsWS::IsFullscreen() const {
    return isFullscreen;
}

void FSTPWindowsWS::GetWindowSize(int& width, int& height) const {
    if (sdlWindow) {
        SDL_GetWindowSize(sdlWindow, &width, &height);
    } else {
        width = height = 0;
    }
}

void FSTPWindowsWS::SetWindowSize(int width, int height) {
    if (sdlWindow) {
        SDL_SetWindowSize(sdlWindow, width, height);
    }
}