#ifndef FSTP_SETTINGS_DIALOG_H
#define FSTP_SETTINGS_DIALOG_H

#ifdef _WIN32
#include <windows.h>
#include <SDL.h>
#include "../FSTPSettings.h"

class FSTPSettingsDialog {
public:
    explicit FSTPSettingsDialog(HWND parentWindow);
    ~FSTPSettingsDialog();

    void Show();
    void Hide();
    bool IsVisible() const;
    
private:
    HWND hwndDialog;
    HWND hwndParent;
    bool isVisible;

    void SaveSettings();
    void LoadSettings();
    static INT_PTR CALLBACK DialogProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
};

#else

class FSTPSettingsDialog {
public:
    explicit FSTPSettingsDialog(void*) {}
    void Show() {}
    void Hide() {}
    bool IsVisible() const { return false; }
};

#endif

#endif // FSTP_SETTINGS_DIALOG_H
