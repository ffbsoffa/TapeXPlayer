#ifndef FSTP_ABOUT_DIALOG_H
#define FSTP_ABOUT_DIALOG_H

#ifdef _WIN32
#include <windows.h>
#include <SDL.h>
#include "../FSTPWindowManager.h"

class FSTPAboutDialog {
public:
    explicit FSTPAboutDialog(HWND parentWindow);
    ~FSTPAboutDialog();

    void Show();
    void Hide();
    bool IsVisible() const;
    
private:
    HWND hwndDialog;
    HWND hwndParent;
    bool isVisible;

    static INT_PTR CALLBACK DialogProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
};

#else

class FSTPAboutDialog {
public:
    explicit FSTPAboutDialog(void*) {}
    void Show() {}
    void Hide() {}
    bool IsVisible() const { return false; }
};

#endif

#endif // FSTP_ABOUT_DIALOG_H
