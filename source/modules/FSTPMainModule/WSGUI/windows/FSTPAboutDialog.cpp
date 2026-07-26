#ifdef _WIN32

#include <windows.h>
#include <commctrl.h>
#include <iostream>
#include <string>
#include "FSTPAboutDialog.h"
#include "BuildInfo.h"

// Icon resource ID (must match resources.rc)
#define IDI_APPICON 101

static const wchar_t* ABOUT_CLASS = L"FSTPAboutDialogClass";
static const wchar_t* SCROLL_CLASS = L"FSTPScrollPanelClass";
static HWND g_about_hwnd = NULL;

// Scroll helper — shared by WM_VSCROLL and WM_MOUSEWHEEL
static void DoScroll(HWND hwnd, int delta) {
    SCROLLINFO si = { sizeof(si), SIF_ALL };
    GetScrollInfo(hwnd, SB_VERT, &si);
    int oldPos = si.nPos;
    si.nPos += delta;
    if (si.nPos < si.nMin) si.nPos = si.nMin;
    int maxPos = si.nMax - (int)si.nPage + 1;
    if (maxPos < 0) maxPos = 0;
    if (si.nPos > maxPos) si.nPos = maxPos;
    if (si.nPos != oldPos) {
        SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
        ScrollWindowEx(hwnd, 0, -(si.nPos - oldPos), NULL, NULL, NULL, NULL,
                       SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE);
    }
}

// Scrollable panel window procedure (registered class, not subclass)
static LRESULT CALLBACK ScrollPanelWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_MOUSEWHEEL: {
        int wheelDelta = -GET_WHEEL_DELTA_WPARAM(wParam) / 2;
        DoScroll(hwnd, wheelDelta);
        return 0;
    }
    case WM_VSCROLL: {
        SCROLLINFO si = { sizeof(si), SIF_ALL };
        GetScrollInfo(hwnd, SB_VERT, &si);
        int delta = 0;
        switch (LOWORD(wParam)) {
        case SB_LINEUP:     delta = -20; break;
        case SB_LINEDOWN:   delta = 20; break;
        case SB_PAGEUP:     delta = -(int)si.nPage; break;
        case SB_PAGEDOWN:   delta = (int)si.nPage; break;
        case SB_THUMBTRACK: delta = si.nTrackPos - si.nPos; break;
        }
        DoScroll(hwnd, delta);
        return 0;
    }
    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wParam;
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, (HBRUSH)(COLOR_BTNFACE + 1));
        return 1;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// About dialog window procedure
static LRESULT CALLBACK AboutDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        g_about_hwnd = NULL;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void ShowWin32AboutDialog() {
    std::cout << "ShowWin32AboutDialog called" << std::endl;

    if (g_about_hwnd && IsWindow(g_about_hwnd)) {
        SetForegroundWindow(g_about_hwnd);
        return;
    }

    HINSTANCE hInst = GetModuleHandle(NULL);

    // Register window classes
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = AboutDialogProc;
    wc.hInstance = hInst;
    wc.lpszClassName = ABOUT_CLASS;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
    wc.hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, 16, 16, 0);
    RegisterClassExW(&wc);

    // Scrollable panel class
    WNDCLASSEXW sc = {};
    sc.cbSize = sizeof(WNDCLASSEXW);
    sc.lpfnWndProc = ScrollPanelWndProc;
    sc.hInstance = hInst;
    sc.lpszClassName = SCROLL_CLASS;
    sc.hCursor = LoadCursor(NULL, IDC_ARROW);
    sc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassExW(&sc);

    // Create window (slightly smaller than before)
    g_about_hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        ABOUT_CLASS,
        L"About TapeXPlayer",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT,
        660, 480,
        NULL, NULL, hInst, NULL
    );

    if (!g_about_hwnd) {
        std::cerr << "Failed to create about window" << std::endl;
        return;
    }

    // Fonts
    HFONT hFont = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    HFONT hFontLarge = CreateFontW(-16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    HFONT hFontTitle = CreateFontW(-22, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    HFONT hFontBold = CreateFontW(-12, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    HFONT hFontSmall = CreateFontW(-11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    // === LEFT PANEL (240px) ===
    int lx = 20, ly = 30;

    // Application icon (loaded from .ico resource, largest available size)
    int iconSize = 48;
    HICON hAppIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                        iconSize, iconSize, 0);
    if (hAppIcon) {
        int iconX = lx + (240 - iconSize) / 2;  // center in 240px panel
        HWND hIconCtrl = CreateWindowExW(0, L"STATIC", NULL,
            WS_CHILD | WS_VISIBLE | SS_ICON,
            iconX, ly, iconSize, iconSize, g_about_hwnd, NULL, hInst, NULL);
        SendMessage(hIconCtrl, STM_SETICON, (WPARAM)hAppIcon, 0);
        ly += iconSize + 10;
    }

    // Application name
    HWND h = CreateWindowExW(0, L"STATIC", L"TapeXPlayer",
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        lx, ly, 240, 28, g_about_hwnd, NULL, NULL, NULL);
    SendMessage(h, WM_SETFONT, (WPARAM)hFontTitle, TRUE);
    ly += 32;

    // Build info
    wchar_t build_info[256];
    const char* code_name = GetTapeXPlayerCodeName();
    if (code_name && code_name[0]) {
        swprintf(build_info, 256, L"Build %hs \"%hs\" (%hs)",
                 GetTapeXPlayerBuildNumber(), code_name, GetTapeXPlayerBuildDate());
    } else {
        swprintf(build_info, 256, L"Build %hs (%hs)",
                 GetTapeXPlayerBuildNumber(), GetTapeXPlayerBuildDate());
    }
    h = CreateWindowExW(0, L"STATIC", build_info,
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        lx, ly, 240, 16, g_about_hwnd, NULL, NULL, NULL);
    SendMessage(h, WM_SETFONT, (WPARAM)hFontSmall, TRUE);
    ly += 20;

    // Version
    wchar_t version_buf[64];
    swprintf(version_buf, 64, L"Version %hs", GetTapeXPlayerVersion());
    h = CreateWindowExW(0, L"STATIC", version_buf,
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        lx, ly, 240, 18, g_about_hwnd, NULL, NULL, NULL);
    SendMessage(h, WM_SETFONT, (WPARAM)hFont, TRUE);
    ly += 25;

    // Build type
    h = CreateWindowExW(0, L"STATIC", L"Windows (SDL2/MinGW)",
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        lx, ly, 240, 16, g_about_hwnd, NULL, NULL, NULL);
    SendMessage(h, WM_SETFONT, (WPARAM)hFontSmall, TRUE);

    // Copyright (bottom area of left panel)
    h = CreateWindowExW(0, L"STATIC",
        L"\xa9 2026 Maksim Maloletkin (FFB_soffa).\r\nLicensed under GPL.",
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        lx, 330, 240, 32, g_about_hwnd, NULL, NULL, NULL);
    SendMessage(h, WM_SETFONT, (WPARAM)hFontSmall, TRUE);

    // GitHub
    h = CreateWindowExW(0, L"STATIC",
        L"github.com/ffbsoffa/TapeXPlayer",
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        lx, 368, 240, 16, g_about_hwnd, NULL, NULL, NULL);
    SendMessage(h, WM_SETFONT, (WPARAM)hFontSmall, TRUE);

    // Website
    h = CreateWindowExW(0, L"STATIC",
        L"ffbsoffa.org",
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        lx, 386, 240, 16, g_about_hwnd, NULL, NULL, NULL);
    SendMessage(h, WM_SETFONT, (WPARAM)hFontSmall, TRUE);

    // === RIGHT PANEL (scrollable) ===
    // Create a scrollable child window for libraries/frameworks list
    RECT rcClient;
    GetClientRect(g_about_hwnd, &rcClient);
    int panelLeft = 275;
    int panelTop = 10;
    int panelWidth = rcClient.right - panelLeft - 5;
    int panelHeight = rcClient.bottom - panelTop - 10;

    HWND hScrollPanel = CreateWindowExW(0, SCROLL_CLASS, NULL,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPCHILDREN,
        panelLeft, panelTop, panelWidth, panelHeight,
        g_about_hwnd, NULL, hInst, NULL);

    // Build content inside scrollable panel
    int ry = 8;
    int contentWidth = panelWidth - 24; // account for scrollbar

    // Header
    h = CreateWindowExW(0, L"STATIC", L"Third-Party Libraries",
        WS_CHILD | WS_VISIBLE,
        8, ry, contentWidth, 20, hScrollPanel, NULL, hInst, NULL);
    SendMessage(h, WM_SETFONT, (WPARAM)hFontLarge, TRUE);
    ry += 26;

    // Library entries
    struct LibraryInfo {
        const wchar_t* name;
        const wchar_t* description;
        const wchar_t* license;
    };

    LibraryInfo libraries[] = {
        { L"FFmpeg",     L"Video and audio codec library",        L"LGPL v2.1+ / GPL v2+" },
        { L"SDL2",       L"Cross-platform multimedia library",    L"zlib License" },
        { L"SDL2_ttf",   L"TrueType font rendering library",     L"zlib License" },
        { L"PortAudio",  L"Cross-platform audio I/O library",    L"MIT-like License" },
        { L"RtMidi",     L"Cross-platform MIDI I/O library",     L"MIT-like License" },
        { L"OpenSSL",    L"Cryptography and SSL/TLS toolkit",    L"Apache License 2.0" },
    };

    for (const auto& lib : libraries) {
        h = CreateWindowExW(0, L"STATIC", lib.name,
            WS_CHILD | WS_VISIBLE,
            8, ry, contentWidth, 16, hScrollPanel, NULL, hInst, NULL);
        SendMessage(h, WM_SETFONT, (WPARAM)hFontBold, TRUE);
        ry += 16;

        h = CreateWindowExW(0, L"STATIC", lib.description,
            WS_CHILD | WS_VISIBLE,
            16, ry, contentWidth, 14, hScrollPanel, NULL, hInst, NULL);
        SendMessage(h, WM_SETFONT, (WPARAM)hFontSmall, TRUE);
        ry += 14;

        wchar_t license_buf[256];
        swprintf(license_buf, 256, L"License: %ls", lib.license);
        h = CreateWindowExW(0, L"STATIC", license_buf,
            WS_CHILD | WS_VISIBLE,
            16, ry, contentWidth, 14, hScrollPanel, NULL, hInst, NULL);
        SendMessage(h, WM_SETFONT, (WPARAM)hFontSmall, TRUE);
        ry += 20;
    }

    // Windows frameworks section
    ry += 6;
    h = CreateWindowExW(0, L"STATIC", L"Windows Frameworks",
        WS_CHILD | WS_VISIBLE,
        8, ry, contentWidth, 20, hScrollPanel, NULL, hInst, NULL);
    SendMessage(h, WM_SETFONT, (WPARAM)hFontLarge, TRUE);
    ry += 24;

    const wchar_t* frameworks[] = {
        L"Win32 API \u2013 Windows native interface",
        L"DirectX / Direct3D \u2013 Graphics rendering",
        L"DXVA2 / D3D11VA \u2013 Hardware video decode",
        L"WinMM \u2013 Multimedia audio/MIDI",
        NULL
    };

    for (int i = 0; frameworks[i] != NULL; i++) {
        h = CreateWindowExW(0, L"STATIC", frameworks[i],
            WS_CHILD | WS_VISIBLE,
            16, ry, contentWidth, 16, hScrollPanel, NULL, hInst, NULL);
        SendMessage(h, WM_SETFONT, (WPARAM)hFontSmall, TRUE);
        ry += 18;
    }

    // Disclaimer
    ry += 10;
    h = CreateWindowExW(0, L"STATIC",
        L"This software uses libraries from the FFmpeg project "
        L"under the LGPLv2.1. FFmpeg is a trademark of Fabrice Bellard. "
        L"All trademarks are property of their respective owners.",
        WS_CHILD | WS_VISIBLE,
        8, ry, contentWidth, 48, hScrollPanel, NULL, hInst, NULL);
    SendMessage(h, WM_SETFONT, (WPARAM)hFontSmall, TRUE);
    ry += 55;

    // Set scroll range based on content height
    int totalContentHeight = ry;
    SCROLLINFO si = { sizeof(si) };
    si.fMask = SIF_RANGE | SIF_PAGE;
    si.nMin = 0;
    si.nMax = totalContentHeight;
    si.nPage = panelHeight;
    SetScrollInfo(hScrollPanel, SB_VERT, &si, TRUE);

    // Hide scrollbar if content fits
    if (totalContentHeight <= panelHeight) {
        ShowScrollBar(hScrollPanel, SB_VERT, FALSE);
    }

    ShowWindow(g_about_hwnd, SW_SHOW);
    UpdateWindow(g_about_hwnd);

    std::cout << "About dialog displayed" << std::endl;
}

#else
// Stub translation unit for non-Windows builds.
#endif
