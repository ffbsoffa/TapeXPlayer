#ifndef FSTP_MEMORY_LOCATIONS_WINDOW_H
#define FSTP_MEMORY_LOCATIONS_WINDOW_H

#include <windows.h>
#include <SDL.h>
#include "../FSTPMemoryLocations.h"

class FSTPMemoryLocationsWindow {
public:
    FSTPMemoryLocationsWindow(HWND parentWindow);
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

#endif // FSTP_MEMORY_LOCATIONS_WINDOW_H