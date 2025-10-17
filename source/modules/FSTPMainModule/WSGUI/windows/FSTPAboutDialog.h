#ifndef FSTP_ABOUT_DIALOG_H
#define FSTP_ABOUT_DIALOG_H

#include <windows.h>
#include <SDL.h>
#include "../FSTPWindowManager.h"

class FSTPAboutDialog {
public:
    FSTPAboutDialog(HWND parentWindow);
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

#endif // FSTP_ABOUT_DIALOG_H