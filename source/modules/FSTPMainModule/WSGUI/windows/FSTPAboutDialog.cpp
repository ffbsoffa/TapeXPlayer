#ifdef _WIN32
#include "FSTPAboutDialog.h"
#include "BuildInfo.h"
#include <string>

FSTPAboutDialog::FSTPAboutDialog(HWND parentWindow) 
    : hwndParent(parentWindow), hwndDialog(NULL), isVisible(false) {
}

FSTPAboutDialog::~FSTPAboutDialog() {
    Hide();
}

void FSTPAboutDialog::Show() {
    if (!hwndDialog) {
        // TODO: Create dialog using Windows API
        hwndDialog = CreateWindowEx(
            WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
            L"FSTPAboutDialogClass",
            L"About TapeXPlayer",
            WS_VISIBLE | WS_SYSMENU | WS_CAPTION,
            CW_USEDEFAULT, CW_USEDEFAULT,
            400, 300,
            hwndParent,
            NULL,
            GetModuleHandle(NULL),
            this
        );
        
        if (hwndDialog) {
            isVisible = true;
            ShowWindow(hwndDialog, SW_SHOW);
            UpdateWindow(hwndDialog);
        }
    }
}

void FSTPAboutDialog::Hide() {
    if (hwndDialog) {
        DestroyWindow(hwndDialog);
        hwndDialog = NULL;
        isVisible = false;
    }
}

bool FSTPAboutDialog::IsVisible() const {
    return isVisible;
}

INT_PTR CALLBACK FSTPAboutDialog::DialogProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_INITDIALOG:
            return TRUE;

        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
                EndDialog(hwnd, LOWORD(wParam));
                return TRUE;
            }
            break;

        case WM_CLOSE:
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
    }
    return FALSE;
}
#else
// Stub translation unit for non-Windows builds.
#endif
