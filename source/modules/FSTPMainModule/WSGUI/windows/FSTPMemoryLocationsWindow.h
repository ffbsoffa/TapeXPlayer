#ifndef FSTP_MEMORY_LOCATIONS_WINDOW_H
#define FSTP_MEMORY_LOCATIONS_WINDOW_H

#ifdef _WIN32
#include <windows.h>
#include <SDL.h>
#include "../FSTPMemoryLocations.h"

class FSTPMemoryLocationsWindow {
public:
    explicit FSTPMemoryLocationsWindow(HWND parentWindow);
    ~FSTPMemoryLocationsWindow();

    void Show();
    void Hide();
    void Update();
    bool IsVisible() const;
    
private:
    HWND hwndWindow;
    HWND hwndParent;
    HWND hwndList;
    bool isVisible;

    void CreateControls();
    void PopulateList();
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
};

#else

class FSTPMemoryLocationsWindow {
public:
    explicit FSTPMemoryLocationsWindow(void*) {}
    void Show() {}
    void Hide() {}
    void Update() {}
    bool IsVisible() const { return false; }
};

#endif

#endif // FSTP_MEMORY_LOCATIONS_WINDOW_H
