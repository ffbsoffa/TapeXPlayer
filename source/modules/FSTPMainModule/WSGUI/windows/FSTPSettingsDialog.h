#ifndef FSTP_SETTINGS_DIALOG_H
#define FSTP_SETTINGS_DIALOG_H

#include <windows.h>
#include <SDL.h>
#include "../FSTPSettings.h"

class FSTPSettingsDialog {
public:
    FSTPSettingsDialog(HWND parentWindow);
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

#endif // FSTP_SETTINGS_DIALOG_H