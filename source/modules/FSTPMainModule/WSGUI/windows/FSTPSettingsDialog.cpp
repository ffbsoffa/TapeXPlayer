#ifdef _WIN32

#include <windows.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <portaudio.h>
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <algorithm>
#include "../FSTPSettings.h"
#include "../FSTPWindowManager.h"
#include "../FSTPMemoryLocations.h"
#include "FSTPSettingsDialog.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "dwmapi.lib")

extern std::atomic<bool> g_dialog_open;

// Forward declarations
extern "C" int GetMIDIInputDeviceCount();
extern "C" int GetMIDIOutputDeviceCount();
extern "C" const char* GetMIDIInputDeviceName(int index);
extern "C" const char* GetMIDIOutputDeviceName(int index);
extern "C" void ApplyMIDISettings();

// ── Control IDs ──────────────────────────────────────────────────────────────
#define IDC_SIDEBAR           100
// IDOK=1 and IDCANCEL=2 are used so IsDialogMessageW handles Enter and Esc.
#define IDC_BTN_RESET         103

// Audio page
#define IDC_AUDIO_DEVICE      1010
#define IDC_VOLUME_SLIDER     1011
#define IDC_VOLUME_LABEL      1012
#define IDC_DUCKING_CHECK     1013
#define IDC_BUFFER_COMBO      1014

// Video & Sync page
#define IDC_FRAME_OFFSET      1015
#define IDC_FREEZE_CHECK      1016
#define IDC_BETACAM_CHECK     1017
#define IDC_DECODER_STATUS    1018
#define IDC_REVERSE_STRIPE    1019

// MIDI page
#define IDC_MIDI_ENABLE       1020
#define IDC_MIDI_INPUT        1021
#define IDC_MIDI_OUTPUT       1022

// Cache page
#define IDC_CLEAR_PROXY       1030
#define IDC_CLEAR_MEMORY      1031
#define IDC_PROXY_INFO        1032
#define IDC_MEMORY_INFO       1033

// Extensions page
#define IDC_YTDLP_CHECK       1040

// Hint labels use IDs 2000-2099 — PageProc uses this range to colour them gray
#define IDC_HINT_BASE         2000
#define IDC_HINT_MAX          2099

// ── DPI scaling ────────────────────────────────────────────────────────────────
// The whole dialog is hand-laid-out in logical (96-DPI) pixels. The app is manifest-
// declared DPI-aware, so Windows does NOT auto-scale it — at 125%/150% the fonts
// (created DPI-scaled in CreateFonts) grow but the fixed geometry would not, so text
// clips and controls overlap. We therefore scale EVERY coordinate/size through Dpi().
// g_dpiScale is captured once per dialog open in InitDpiMetrics().
static double g_dpiScale = 1.0;

static inline int Dpi(int logical) {
    return (int)(logical * g_dpiScale + 0.5);
}

// All the layout constants below are LOGICAL (96-DPI) sizes; they get set to their
// DPI-scaled values in InitDpiMetrics() before any control is created.
static const int DLG_W_LOGICAL     = 680;
static const int DLG_H_LOGICAL     = 480;
static const int SIDEBAR_W_LOGICAL = 160;
static const int FOOTER_H_LOGICAL  = 52;
static const int ITEM_H_LOGICAL    = 40;
// Indent for hint text that appears below a checkbox label.
// Aligns hint with the checkbox's text (checkbox square ≈ 13px + 3px gap).
static const int CHECKBOX_INDENT_LOGICAL = 16;

static const int PAGE_COUNT    = 5;

// DPI-scaled at runtime (InitDpiMetrics). Not const: value depends on the monitor DPI.
static int DLG_W          = DLG_W_LOGICAL;
static int DLG_H          = DLG_H_LOGICAL;
static int SIDEBAR_W      = SIDEBAR_W_LOGICAL;
static int FOOTER_H       = FOOTER_H_LOGICAL;
static int ITEM_H         = ITEM_H_LOGICAL;
static int CHECKBOX_INDENT = CHECKBOX_INDENT_LOGICAL;

static void InitDpiMetrics() {
    // Use the same LOGPIXELSY that CreateFonts() uses, so fonts and geometry scale
    // by the exact same factor (96 DPI = 100%, 120 = 125%, 144 = 150%).
    HDC screen = GetDC(NULL);
    int dpiY = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(NULL, screen);
    if (dpiY <= 0) dpiY = 96;
    g_dpiScale = dpiY / 96.0;

    DLG_W          = Dpi(DLG_W_LOGICAL);
    DLG_H          = Dpi(DLG_H_LOGICAL);
    SIDEBAR_W      = Dpi(SIDEBAR_W_LOGICAL);
    FOOTER_H       = Dpi(FOOTER_H_LOGICAL);
    ITEM_H         = Dpi(ITEM_H_LOGICAL);
    CHECKBOX_INDENT = Dpi(CHECKBOX_INDENT_LOGICAL);
}

// ── Sidebar colours ───────────────────────────────────────────────────────────
static const COLORREF CLR_SIDEBAR_BG   = RGB(240, 240, 240);
static const COLORREF CLR_SIDEBAR_SEL  = RGB(0, 120, 215);   // Windows accent blue
static const COLORREF CLR_SIDEBAR_HOT  = RGB(229, 243, 255);
static const COLORREF CLR_SIDEBAR_TXT  = RGB(30,  30,  30);
static const COLORREF CLR_SIDEBAR_STXT = RGB(255, 255, 255);
static const COLORREF CLR_CONTENT_BG   = RGB(255, 255, 255);
static const COLORREF CLR_DIVIDER      = RGB(220, 220, 220);

// ── Fonts ─────────────────────────────────────────────────────────────────────
static HFONT g_fontNormal  = nullptr;
static HFONT g_fontBold    = nullptr;
static HFONT g_fontSmall   = nullptr;

static void CreateFonts() {
    // Segoe UI 10pt normal
    g_fontNormal = CreateFontW(-MulDiv(10, GetDeviceCaps(GetDC(NULL), LOGPIXELSY), 72),
        0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

    // Segoe UI 10pt bold
    g_fontBold = CreateFontW(-MulDiv(10, GetDeviceCaps(GetDC(NULL), LOGPIXELSY), 72),
        0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

    // Segoe UI 9pt for hints
    g_fontSmall = CreateFontW(-MulDiv(9, GetDeviceCaps(GetDC(NULL), LOGPIXELSY), 72),
        0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
}

static void DestroyFonts() {
    if (g_fontNormal) { DeleteObject(g_fontNormal); g_fontNormal = nullptr; }
    if (g_fontBold)   { DeleteObject(g_fontBold);   g_fontBold   = nullptr; }
    if (g_fontSmall)  { DeleteObject(g_fontSmall);  g_fontSmall  = nullptr; }
}

// ── ComCtl32 v6 activation ────────────────────────────────────────────────────
static HANDLE    g_hActCtx   = INVALID_HANDLE_VALUE;
static ULONG_PTR g_actCookie = 0;

static void ActivateVisualStyles() {
    if (g_hActCtx != INVALID_HANDLE_VALUE) return;
    wchar_t dllPath[MAX_PATH];
    GetSystemDirectoryW(dllPath, MAX_PATH);
    wcscat(dllPath, L"\\comctl32.dll");
    ACTCTXW act = {};
    act.cbSize = sizeof(act);
    act.dwFlags = ACTCTX_FLAG_RESOURCE_NAME_VALID;
    act.lpSource = dllPath;
    act.lpResourceName = MAKEINTRESOURCEW(124);
    g_hActCtx = CreateActCtxW(&act);
    if (g_hActCtx != INVALID_HANDLE_VALUE)
        ActivateActCtx(g_hActCtx, &g_actCookie);
}

static void DeactivateVisualStyles() {
    if (g_hActCtx != INVALID_HANDLE_VALUE) {
        DeactivateActCtx(0, g_actCookie);
        ReleaseActCtx(g_hActCtx);
        g_hActCtx   = INVALID_HANDLE_VALUE;
        g_actCookie = 0;
    }
}

// ── UTF-8 helpers ─────────────────────────────────────────────────────────────
static std::wstring Utf8ToWide(const char* utf8) {
    if (!utf8 || !*utf8) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (len <= 0) return L"";
    std::wstring w(len - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &w[0], len);
    return w;
}

// ── Apply font to all children ────────────────────────────────────────────────
static BOOL CALLBACK SetChildFontCB(HWND child, LPARAM font) {
    SendMessage(child, WM_SETFONT, (WPARAM)font, TRUE);
    return TRUE;
}
static void ApplyFontToChildren(HWND hwnd, HFONT font) {
    EnumChildWindows(hwnd, SetChildFontCB, (LPARAM)font);
}

// Gray brush for hint text background (matches white content area)
static HBRUSH g_hintBrush = nullptr;

// ── Helper: section header + separator ───────────────────────────────────────
// Bold label followed by a 1px gray line. Returns new y position.
static int AddSectionHeader(HWND parent, HINSTANCE hInst, const wchar_t* text, int x, int y, int w) {
    HWND h = CreateWindowExW(0, L"STATIC", text,
        WS_CHILD | WS_VISIBLE, x, y, w, Dpi(19), parent, nullptr, hInst, nullptr);
    SendMessage(h, WM_SETFONT, (WPARAM)g_fontBold, FALSE);
    // Thin separator line below the header text
    CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
        x, y + Dpi(21), w, Dpi(2), parent, nullptr, hInst, nullptr);
    return y + Dpi(30);
}

// ── Helper: hint label (small, gray) ─────────────────────────────────────────
static int g_hintIdCounter = IDC_HINT_BASE;

// Measure how tall `text` renders when word-wrapped to width `w` in the hint font.
// This is the fix for the DPI clipping bug: the old code hardcoded a 32px box and a
// 36px advance, so at 125%/150% the taller, more-wrapped text overflowed its box and
// the next control drew on top of it. We measure the real height with DT_CALCRECT.
// NB: hints are measured and created with g_fontNormal — the same font the page's
// final ApplyFontToChildren(page, g_fontNormal) applies to every child, so the measured
// height matches what actually renders. (Measuring with g_fontSmall would under-measure.)
static int MeasureHintHeight(HWND parent, const wchar_t* text, int w) {
    HDC hdc = GetDC(parent);
    HFONT oldFont = (HFONT)SelectObject(hdc, g_fontNormal);
    RECT rc = { 0, 0, w, 0 };
    DrawTextW(hdc, text, -1, &rc, DT_CALCRECT | DT_WORDBREAK | DT_LEFT | DT_NOPREFIX);
    SelectObject(hdc, oldFont);
    ReleaseDC(parent, hdc);
    int h = rc.bottom - rc.top;
    if (h < Dpi(16)) h = Dpi(16); // at least one line
    return h;
}

static int AddHint(HWND parent, HINSTANCE hInst, const wchar_t* text, int x, int y, int w) {
    int id = g_hintIdCounter;
    if (g_hintIdCounter < IDC_HINT_MAX) g_hintIdCounter++;
    int h = MeasureHintHeight(parent, text, w);
    HWND ctl = CreateWindowExW(0, L"STATIC", text,
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        x, y, w, h, parent, (HMENU)(UINT_PTR)id, hInst, nullptr);
    SendMessage(ctl, WM_SETFONT, (WPARAM)g_fontNormal, FALSE);
    return y + h + Dpi(6); // small gap below the (correctly-sized) hint
}

// ── Content page scroll state ─────────────────────────────────────────────────
struct PageScrollState {
    int contentH = 0;  // total content height (px)
    int scrollY  = 0;  // current scroll offset
};

// Call after building all controls on a page.
static void SetupPageScroll(HWND page, int contentH) {
    RECT rc;
    GetClientRect(page, &rc);
    int clientH = rc.bottom;

    auto* sc = new PageScrollState();
    sc->contentH = contentH + Dpi(16); // bottom padding
    sc->scrollY  = 0;
    SetWindowLongPtrW(page, GWLP_USERDATA, (LONG_PTR)sc);

    if (sc->contentH > clientH) {
        LONG style = GetWindowLongW(page, GWL_STYLE);
        SetWindowLongW(page, GWL_STYLE, style | WS_VSCROLL);
        SetWindowPos(page, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        SCROLLINFO si = {};
        si.cbSize = sizeof(si);
        si.fMask  = SIF_RANGE | SIF_PAGE | SIF_POS;
        si.nMin   = 0;
        si.nMax   = sc->contentH - 1;
        si.nPage  = (UINT)clientH;
        si.nPos   = 0;
        SetScrollInfo(page, SB_VERT, &si, TRUE);
    }
}

static void ScrollPage(HWND page, int delta) {
    auto* sc = (PageScrollState*)GetWindowLongPtrW(page, GWLP_USERDATA);
    if (!sc) return;
    RECT rc;
    GetClientRect(page, &rc);
    int clientH   = rc.bottom;
    int maxScroll = std::max(0, sc->contentH - clientH);
    if (maxScroll == 0) return;

    int oldY = sc->scrollY;
    int newY = std::max(0, std::min(sc->scrollY + delta, maxScroll));
    if (newY == oldY) return;

    sc->scrollY = newY;
    SCROLLINFO si = {};
    si.cbSize = sizeof(si);
    si.fMask  = SIF_POS;
    si.nPos   = newY;
    SetScrollInfo(page, SB_VERT, &si, TRUE);
    ScrollWindowEx(page, 0, oldY - newY, nullptr, nullptr, nullptr, nullptr,
                   SW_SCROLLCHILDREN | SW_ERASE | SW_INVALIDATE);
    UpdateWindow(page);
}

// ── Content page window class ─────────────────────────────────────────────────
static LRESULT CALLBACK PageProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wParam;
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));
        return 1;
    }
    case WM_CTLCOLORSTATIC: {
        HDC hdc  = (HDC)wParam;
        HWND ctl = (HWND)lParam;
        int  id  = GetDlgCtrlID(ctl);
        SetBkColor(hdc, CLR_CONTENT_BG);
        SetTextColor(hdc, (id >= IDC_HINT_BASE && id <= IDC_HINT_MAX)
                          ? RGB(120, 120, 120) : CLR_SIDEBAR_TXT);
        return (LRESULT)GetStockObject(WHITE_BRUSH);
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        return (LRESULT)DefWindowProcW(hwnd, msg, wParam, lParam);

    case WM_VSCROLL: {
        RECT rc; GetClientRect(hwnd, &rc);
        int clientH = rc.bottom;
        auto* sc = (PageScrollState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        if (!sc) return 0;
        int delta = 0;
        switch (LOWORD(wParam)) {
            case SB_LINEUP:   delta = -20;      break;
            case SB_LINEDOWN: delta = +20;      break;
            case SB_PAGEUP:   delta = -clientH; break;
            case SB_PAGEDOWN: delta = +clientH; break;
            case SB_THUMBTRACK: {
                SCROLLINFO si = {}; si.cbSize = sizeof(si); si.fMask = SIF_TRACKPOS;
                GetScrollInfo(hwnd, SB_VERT, &si);
                delta = si.nTrackPos - sc->scrollY;
                break;
            }
            case SB_TOP:    delta = -sc->scrollY; break;
            case SB_BOTTOM: delta = sc->contentH; break;
        }
        if (delta) ScrollPage(hwnd, delta);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        ScrollPage(hwnd, delta > 0 ? -60 : 60);
        return 0;
    }
    case WM_DESTROY: {
        auto* sc = (PageScrollState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        delete sc;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void RegisterPageClass(HINSTANCE hInst) {
    static bool registered = false;
    if (registered) return;
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = PageProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wc.lpszClassName = L"TXP_SettingsPage";
    RegisterClassExW(&wc);
    registered = true;
}

// ── Per-dialog state ──────────────────────────────────────────────────────────
struct SettingsDlgState {
    HWND pages[PAGE_COUNT];   // child windows, one per page
    int  currentPage = 0;
    int  hotItem     = -1;

    // Audio controls (saved here so WM_COMMAND handlers can read them)
    HWND hAudioDevice   = nullptr;
    HWND hVolumeSlider  = nullptr;
    HWND hVolumeLabel   = nullptr;
    HWND hDuckingCheck  = nullptr;
    HWND hBufferCombo   = nullptr;

    // Video controls
    HWND hFrameOffset   = nullptr;
    HWND hFreezeCheck   = nullptr;
    HWND hBetacamCheck  = nullptr;
    HWND hReverseStripe = nullptr;
    HWND hDecoderStatus = nullptr;

    // MIDI controls
    HWND hMidiEnable    = nullptr;
    HWND hMidiInput     = nullptr;
    HWND hMidiOutput    = nullptr;

    // Cache controls
    HWND hClearProxy    = nullptr;
    HWND hClearMemory   = nullptr;
    HWND hProxyInfo     = nullptr;
    HWND hMemoryInfo    = nullptr;

    // Extensions controls
    HWND hYtdlpCheck    = nullptr;
};

static void ShowPage(SettingsDlgState* s, int index) {
    for (int i = 0; i < PAGE_COUNT; i++)
        ShowWindow(s->pages[i], (i == index) ? SW_SHOW : SW_HIDE);
    s->currentPage = index;
    // Repaint sidebar so selection updates
    HWND sidebar = GetDlgItem(GetParent(s->pages[0]), IDC_SIDEBAR);
    if (sidebar) InvalidateRect(sidebar, nullptr, TRUE);
}

// ── Sidebar HWND (owner-drawn) ────────────────────────────────────────────────
static const wchar_t* SIDEBAR_ITEMS[] = {
    L"Audio", L"Video & Sync", L"MIDI", L"Cache & Data", L"Extensions"
};
// ITEM_H is defined above with the other DPI-scaled layout metrics.

static LRESULT CALLBACK SidebarProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    SettingsDlgState* s = (SettingsDlgState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);

        // Background
        HBRUSH bgBrush = CreateSolidBrush(CLR_SIDEBAR_BG);
        FillRect(hdc, &rc, bgBrush);
        DeleteObject(bgBrush);

        SetBkMode(hdc, TRANSPARENT);
        HFONT oldFont = (HFONT)SelectObject(hdc, g_fontNormal);

        for (int i = 0; i < PAGE_COUNT; i++) {
            RECT itemRc = { 0, i * ITEM_H, rc.right, (i + 1) * ITEM_H };
            bool selected = s && (s->currentPage == i);
            bool hot      = s && (s->hotItem     == i);

            if (selected) {
                HBRUSH selBrush = CreateSolidBrush(CLR_SIDEBAR_SEL);
                FillRect(hdc, &itemRc, selBrush);
                DeleteObject(selBrush);
                SetTextColor(hdc, CLR_SIDEBAR_STXT);
            } else if (hot) {
                HBRUSH hotBrush = CreateSolidBrush(CLR_SIDEBAR_HOT);
                FillRect(hdc, &itemRc, hotBrush);
                DeleteObject(hotBrush);
                SetTextColor(hdc, CLR_SIDEBAR_TXT);
            } else {
                SetTextColor(hdc, CLR_SIDEBAR_TXT);
            }

            RECT textRc = { itemRc.left + Dpi(16), itemRc.top, itemRc.right - Dpi(8), itemRc.bottom };
            DrawTextW(hdc, SIDEBAR_ITEMS[i], -1, &textRc, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
        }

        // Right border
        HPEN pen = CreatePen(PS_SOLID, 1, CLR_DIVIDER);
        HPEN oldPen = (HPEN)SelectObject(hdc, pen);
        MoveToEx(hdc, rc.right - 1, 0, nullptr);
        LineTo(hdc, rc.right - 1, rc.bottom);
        SelectObject(hdc, oldPen);
        DeleteObject(pen);

        SelectObject(hdc, oldFont);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_MOUSEMOVE: {
        int y = HIWORD(lParam);
        int item = y / ITEM_H;
        if (item < 0 || item >= PAGE_COUNT) item = -1;
        if (s && s->hotItem != item) {
            s->hotItem = item;
            InvalidateRect(hwnd, nullptr, TRUE);
            // Track mouse leave
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
        }
        return 0;
    }

    case WM_MOUSELEAVE: {
        if (s) { s->hotItem = -1; InvalidateRect(hwnd, nullptr, TRUE); }
        return 0;
    }

    case WM_LBUTTONDOWN: {
        int y = HIWORD(lParam);
        int item = y / ITEM_H;
        if (item >= 0 && item < PAGE_COUNT && s)
            ShowPage(s, item);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1; // handled in WM_PAINT
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ── Page builders ─────────────────────────────────────────────────────────────

static HWND BuildAudioPage(HWND parent, SettingsDlgState* s) {
    HINSTANCE hInst = GetModuleHandle(nullptr);
    const FSTPSettings* settings = GetSettings();
    int pw = DLG_W - SIDEBAR_W - 1;
    int ph = DLG_H - FOOTER_H;

    HWND page = CreateWindowExW(0, L"TXP_SettingsPage", L"",
        WS_CHILD | WS_CLIPCHILDREN,
        SIDEBAR_W + 1, 0, pw, ph, parent, nullptr, hInst, nullptr);

    int x = Dpi(20), y = Dpi(20);
    int cw = pw - Dpi(40); // content width

    // Audio Device
    y = AddSectionHeader(page, hInst, L"Audio Device", x, y, cw);
    s->hAudioDevice = CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        x, y, cw, Dpi(200), page, (HMENU)IDC_AUDIO_DEVICE, hInst, nullptr);
    SendMessageW(s->hAudioDevice, CB_ADDSTRING, 0, (LPARAM)L"Default Audio Device");
    if (settings) {
        int cnt = Pa_GetDeviceCount();
        for (int i = 0; i < cnt; i++) {
            const PaDeviceInfo* di = Pa_GetDeviceInfo(i);
            if (di && di->maxOutputChannels > 0)
                SendMessageW(s->hAudioDevice, CB_ADDSTRING, 0, (LPARAM)Utf8ToWide(di->name).c_str());
        }
        SendMessage(s->hAudioDevice, CB_SETCURSEL, settings->audio_device_index + 1, 0);
    }
    y += Dpi(30);

    // Volume
    y += Dpi(10);
    // Row: label left, value right
    HWND hVolTitle = CreateWindowExW(0, L"STATIC", L"Volume",
        WS_CHILD | WS_VISIBLE, x, y, cw / 2, Dpi(18), page, nullptr, hInst, nullptr);
    SendMessage(hVolTitle, WM_SETFONT, (WPARAM)g_fontBold, FALSE);

    wchar_t vol_buf[32] = L"100%";
    if (settings) swprintf(vol_buf, 32, L"%d%%", (int)(settings->audio_master_volume * 100));
    s->hVolumeLabel = CreateWindowExW(0, L"STATIC", vol_buf,
        WS_CHILD | WS_VISIBLE | SS_RIGHT,
        x + cw / 2, y, cw / 2, Dpi(18), page, (HMENU)IDC_VOLUME_LABEL, hInst, nullptr);
    SendMessage(s->hVolumeLabel, WM_SETFONT, (WPARAM)g_fontNormal, FALSE);
    y += Dpi(22);

    s->hVolumeSlider = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
        WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS | TBS_BOTH,
        x, y, cw, Dpi(28), page, (HMENU)IDC_VOLUME_SLIDER, hInst, nullptr);
    SendMessage(s->hVolumeSlider, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
    if (settings) SendMessage(s->hVolumeSlider, TBM_SETPOS, TRUE, (int)(settings->audio_master_volume * 100));
    y += Dpi(32);

    s->hDuckingCheck = CreateWindowExW(0, L"BUTTON", L"Auto-Reduce Volume at High Speeds",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        x, y, cw, Dpi(20), page, (HMENU)IDC_DUCKING_CHECK, hInst, nullptr);
    if (settings && settings->audio_volume_ducking_enabled)
        SendMessage(s->hDuckingCheck, BM_SETCHECK, BST_CHECKED, 0);
    y += Dpi(22);

    y = AddHint(page, hInst, L"Protects your ears during shuttle (6×: fade, 12×: \u221224 dB, 32×: \u221240 dB)", x + CHECKBOX_INDENT, y, cw - CHECKBOX_INDENT);

    // Buffer size
    y += Dpi(6);
    y = AddSectionHeader(page, hInst, L"Audio Buffer", x, y, cw);
    s->hBufferCombo = CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
        x, y, Dpi(180), Dpi(120), page, (HMENU)IDC_BUFFER_COMBO, hInst, nullptr);
    SendMessageW(s->hBufferCombo, CB_ADDSTRING, 0, (LPARAM)L"512 samples");
    SendMessageW(s->hBufferCombo, CB_ADDSTRING, 0, (LPARAM)L"1024 samples");
    SendMessageW(s->hBufferCombo, CB_ADDSTRING, 0, (LPARAM)L"2048 samples");
    SendMessageW(s->hBufferCombo, CB_ADDSTRING, 0, (LPARAM)L"4096 samples");
    if (settings) {
        int idx = 1;
        if (settings->audio_buffer_size == 512)  idx = 0;
        if (settings->audio_buffer_size == 2048) idx = 2;
        if (settings->audio_buffer_size == 4096) idx = 3;
        SendMessage(s->hBufferCombo, CB_SETCURSEL, idx, 0);
    }
    y += Dpi(30);
    y = AddHint(page, hInst, L"Requires application restart to take effect.", x, y, cw);

    ApplyFontToChildren(page, g_fontNormal);
    SetupPageScroll(page, y);
    return page;
}

static HWND BuildVideoPage(HWND parent, SettingsDlgState* s) {
    HINSTANCE hInst = GetModuleHandle(nullptr);
    const FSTPSettings* settings = GetSettings();
    int pw = DLG_W - SIDEBAR_W - 1;
    int ph = DLG_H - FOOTER_H;

    HWND page = CreateWindowExW(0, L"TXP_SettingsPage", L"",
        WS_CHILD | WS_CLIPCHILDREN,
        SIDEBAR_W + 1, 0, pw, ph, parent, nullptr, hInst, nullptr);

    int x = Dpi(20), y = Dpi(20);
    int cw = pw - Dpi(40);

    // Display Synchronization
    y = AddSectionHeader(page, hInst, L"Display Synchronization", x, y, cw);

    CreateWindowExW(0, L"STATIC", L"Frame Offset:",
        WS_CHILD | WS_VISIBLE, x, y + Dpi(3), Dpi(110), Dpi(20), page, nullptr, hInst, nullptr);

    wchar_t offset_buf[16] = L"0";
    if (settings) swprintf(offset_buf, 16, L"%d", settings->frame_offset);
    s->hFrameOffset = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", offset_buf,
        WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_CENTER,
        x + Dpi(118), y, Dpi(60), Dpi(24), page, (HMENU)IDC_FRAME_OFFSET, hInst, nullptr);

    CreateWindowExW(0, L"STATIC", L"frames  (\u221210 to +10)",
        WS_CHILD | WS_VISIBLE, x + Dpi(186), y + Dpi(3), Dpi(180), Dpi(20), page, nullptr, hInst, nullptr);
    y += Dpi(34);
    y = AddHint(page, hInst, L"Compensate for display lag or sync offset between audio and video.", x, y, cw);

    // Multi-Instance Performance
    y += Dpi(6);
    y = AddSectionHeader(page, hInst, L"Multi-Instance Performance", x, y, cw);
    s->hFreezeCheck = CreateWindowExW(0, L"BUTTON", L"Auto-freeze inactive players",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        x, y, cw, Dpi(20), page, (HMENU)IDC_FREEZE_CHECK, hInst, nullptr);
    if (settings && settings->auto_freeze_inactive)
        SendMessage(s->hFreezeCheck, BM_SETCHECK, BST_CHECKED, 0);
    y += Dpi(22);
    y = AddHint(page, hInst, L"Prevents forgotten players from consuming resources in the background.", x + CHECKBOX_INDENT, y, cw - CHECKBOX_INDENT);

    // Visual Effects
    y += Dpi(6);
    y = AddSectionHeader(page, hInst, L"Visual Effects", x, y, cw);
    s->hBetacamCheck = CreateWindowExW(0, L"BUTTON", L"Enable Betacam tape artefact emulation",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        x, y, cw, Dpi(20), page, (HMENU)IDC_BETACAM_CHECK, hInst, nullptr);
    if (settings && settings->betacam_effect_enabled)
        SendMessage(s->hBetacamCheck, BM_SETCHECK, BST_CHECKED, 0);
    y += Dpi(22);
    y = AddHint(page, hInst, L"Adds rewind/fast-forward tape jitter. May impact performance.", x + CHECKBOX_INDENT, y, cw - CHECKBOX_INDENT);

    // Sub-option of the Betacam effect: authentic tracking stripe at 1x reverse. Off by default —
    // its flicker distracts from frame-by-frame analysis (the product's core use).
    s->hReverseStripe = CreateWindowExW(0, L"BUTTON", L"Show tracking stripe at 1x reverse",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        x + CHECKBOX_INDENT, y, cw - CHECKBOX_INDENT, Dpi(20), page, (HMENU)IDC_REVERSE_STRIPE, hInst, nullptr);
    if (settings && settings->betacam_reverse_stripe)
        SendMessage(s->hReverseStripe, BM_SETCHECK, BST_CHECKED, 0);
    y += Dpi(22);
    y = AddHint(page, hInst, L"Authentic look; flickers, so off by default.", x + CHECKBOX_INDENT, y, cw - CHECKBOX_INDENT);

    // Developer / Debug
    y += Dpi(6);
    y = AddSectionHeader(page, hInst, L"Developer / Debug", x, y, cw);
    s->hDecoderStatus = CreateWindowExW(0, L"BUTTON", L"Show Decoder Status",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        x, y, cw, Dpi(20), page, (HMENU)IDC_DECODER_STATUS, hInst, nullptr);
    if (settings && settings->show_decoder_status)
        SendMessage(s->hDecoderStatus, BM_SETCHECK, BST_CHECKED, 0);
    y += Dpi(22);
    y = AddHint(page, hInst, L"Displays decoded frames indicator. Useful for debugging decoder performance.", x + CHECKBOX_INDENT, y, cw - CHECKBOX_INDENT);

    ApplyFontToChildren(page, g_fontNormal);
    SetupPageScroll(page, y);
    return page;
}

static HWND BuildMidiPage(HWND parent, SettingsDlgState* s) {
    HINSTANCE hInst = GetModuleHandle(nullptr);
    const FSTPSettings* settings = GetSettings();
    int pw = DLG_W - SIDEBAR_W - 1;
    int ph = DLG_H - FOOTER_H;

    HWND page = CreateWindowExW(0, L"TXP_SettingsPage", L"",
        WS_CHILD | WS_CLIPCHILDREN,
        SIDEBAR_W + 1, 0, pw, ph, parent, nullptr, hInst, nullptr);

    int x = Dpi(20), y = Dpi(20);
    int cw = pw - Dpi(40);

    y = AddSectionHeader(page, hInst, L"MIDI Controller", x, y, cw);
    s->hMidiEnable = CreateWindowExW(0, L"BUTTON", L"Enable MIDI Controller",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        x, y, cw, Dpi(20), page, (HMENU)IDC_MIDI_ENABLE, hInst, nullptr);
    if (settings && settings->midi_enabled)
        SendMessage(s->hMidiEnable, BM_SETCHECK, BST_CHECKED, 0);
    y += Dpi(34);

    y = AddSectionHeader(page, hInst, L"MIDI Ports", x, y, cw);

    // Input
    CreateWindowExW(0, L"STATIC", L"Input Port:",
        WS_CHILD | WS_VISIBLE, x, y + Dpi(4), Dpi(90), Dpi(20), page, nullptr, hInst, nullptr);
    s->hMidiInput = CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        x + Dpi(98), y, cw - Dpi(98), Dpi(200), page, (HMENU)IDC_MIDI_INPUT, hInst, nullptr);
    SendMessageW(s->hMidiInput, CB_ADDSTRING, 0, (LPARAM)L"(None)");
    int mic = GetMIDIInputDeviceCount();
    for (int i = 0; i < mic; i++) {
        const char* n = GetMIDIInputDeviceName(i);
        if (n) SendMessageW(s->hMidiInput, CB_ADDSTRING, 0, (LPARAM)Utf8ToWide(n).c_str());
    }
    if (settings) SendMessage(s->hMidiInput, CB_SETCURSEL, settings->midi_input_port + 1, 0);
    bool midiOn = settings && settings->midi_enabled;
    EnableWindow(s->hMidiInput, midiOn);
    y += Dpi(34);

    // Output
    CreateWindowExW(0, L"STATIC", L"Output Port:",
        WS_CHILD | WS_VISIBLE, x, y + Dpi(4), Dpi(90), Dpi(20), page, nullptr, hInst, nullptr);
    s->hMidiOutput = CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        x + Dpi(98), y, cw - Dpi(98), Dpi(200), page, (HMENU)IDC_MIDI_OUTPUT, hInst, nullptr);
    SendMessageW(s->hMidiOutput, CB_ADDSTRING, 0, (LPARAM)L"(None)");
    int moc = GetMIDIOutputDeviceCount();
    for (int i = 0; i < moc; i++) {
        const char* n = GetMIDIOutputDeviceName(i);
        if (n) SendMessageW(s->hMidiOutput, CB_ADDSTRING, 0, (LPARAM)Utf8ToWide(n).c_str());
    }
    if (settings) SendMessage(s->hMidiOutput, CB_SETCURSEL, settings->midi_output_port + 1, 0);
    EnableWindow(s->hMidiOutput, midiOn);
    y += Dpi(34);

    y = AddHint(page, hInst, L"Protocol: Mackie Control. Controller support is in development.", x, y, cw);

    ApplyFontToChildren(page, g_fontNormal);
    SetupPageScroll(page, y);
    return page;
}

static HWND BuildCachePage(HWND parent, SettingsDlgState* s) {
    HINSTANCE hInst = GetModuleHandle(nullptr);
    int pw = DLG_W - SIDEBAR_W - 1;
    int ph = DLG_H - FOOTER_H;

    HWND page = CreateWindowExW(0, L"TXP_SettingsPage", L"",
        WS_CHILD | WS_CLIPCHILDREN,
        SIDEBAR_W + 1, 0, pw, ph, parent, nullptr, hInst, nullptr);

    int x = Dpi(20), y = Dpi(20);
    int cw = pw - Dpi(40);

    // Proxy cache
    y = AddSectionHeader(page, hInst, L"Proxy Video Cache", x, y, cw);

    const char* proxy_path = FSTP_GetProxyCachePath();
    int proxy_size  = FSTP_GetProxyCacheSize();
    int proxy_count = FSTP_GetProxyFilesCount();

    wchar_t proxy_info[512];
    swprintf(proxy_info, 512, L"Location: %ls\nSize: %d MB  \u2022  %d files",
             Utf8ToWide(proxy_path ? proxy_path : "Unknown").c_str(), proxy_size, proxy_count);
    s->hProxyInfo = CreateWindowExW(0, L"STATIC", proxy_info,
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        x, y, cw, Dpi(36), page, (HMENU)IDC_PROXY_INFO, hInst, nullptr);
    y += Dpi(42);

    s->hClearProxy = CreateWindowExW(0, L"BUTTON", L"Clear Proxy Cache\u2026",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        x, y, Dpi(180), Dpi(28), page, (HMENU)IDC_CLEAR_PROXY, hInst, nullptr);
    EnableWindow(s->hClearProxy, proxy_count > 0);
    y += Dpi(44);

    // Memory Locations
    y = AddSectionHeader(page, hInst, L"Memory Locations", x, y, cw);

    const char* mem_path  = FSTP_GetMemoryLocationsCachePath();
    int mem_count = FSTP_GetMemoryLocationsFilesCount();

    wchar_t mem_info[512];
    swprintf(mem_info, 512, L"Location: %ls/memory_locations\nSaved data for %d video files",
             Utf8ToWide(mem_path ? mem_path : "Unknown").c_str(), mem_count);
    s->hMemoryInfo = CreateWindowExW(0, L"STATIC", mem_info,
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        x, y, cw, Dpi(36), page, (HMENU)IDC_MEMORY_INFO, hInst, nullptr);
    y += Dpi(42);

    s->hClearMemory = CreateWindowExW(0, L"BUTTON", L"Clear Memory Locations\u2026",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        x, y, Dpi(200), Dpi(28), page, (HMENU)IDC_CLEAR_MEMORY, hInst, nullptr);
    EnableWindow(s->hClearMemory, mem_count > 0);
    y += Dpi(34);

    y = AddHint(page, hInst, L"Memory Locations are saved per-video, like browser bookmarks.", x, y, cw);

    ApplyFontToChildren(page, g_fontNormal);
    SetupPageScroll(page, y);
    return page;
}

static HWND BuildExtensionsPage(HWND parent, SettingsDlgState* s) {
    HINSTANCE hInst = GetModuleHandle(nullptr);
    const FSTPSettings* settings = GetSettings();
    int pw = DLG_W - SIDEBAR_W - 1;
    int ph = DLG_H - FOOTER_H;

    HWND page = CreateWindowExW(0, L"TXP_SettingsPage", L"",
        WS_CHILD | WS_CLIPCHILDREN,
        SIDEBAR_W + 1, 0, pw, ph, parent, nullptr, hInst, nullptr);

    int x = Dpi(20), y = Dpi(20);
    int cw = pw - Dpi(40);

    y = AddSectionHeader(page, hInst, L"Extensions", x, y, cw);

    bool yt_avail = FSTP_YTDLP_IsAvailable();
    if (yt_avail) {
        s->hYtdlpCheck = CreateWindowExW(0, L"BUTTON", L"Enable yt-dlp network downloader",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            x, y, cw, Dpi(20), page, (HMENU)IDC_YTDLP_CHECK, hInst, nullptr);
        if (settings && settings->yt_dlp_extension_enabled)
            SendMessage(s->hYtdlpCheck, BM_SETCHECK, BST_CHECKED, 0);
        y += Dpi(22);
        y = AddHint(page, hInst, L"Enables opening network streams via yt-dlp.", x + CHECKBOX_INDENT, y, cw - CHECKBOX_INDENT);
    } else {
        y = AddHint(page, hInst, L"yt-dlp not found. Install it to enable network downloads.", x, y, cw);
    }

    y += Dpi(10);
    std::string ext_lang = GetExtensionLanguage();
    std::wstring ext_desc = L"Extensions are scripted using " +
        Utf8ToWide(ext_lang.c_str()) +
        L" (.lua) files. Extension management tools will appear here as the system evolves.";
    y = AddHint(page, hInst, ext_desc.c_str(), x, y, cw);

    ApplyFontToChildren(page, g_fontNormal);
    SetupPageScroll(page, y);
    return page;
}

// Convert a wide (UTF-16) string to UTF-8 (reading device names out of the combo box).
static std::string WideToUtf8(const wchar_t* wide) {
    if (!wide) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    if (len <= 0) return std::string();
    std::string utf8(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, &utf8[0], len, NULL, NULL);
    return utf8;
}

// ── Save settings from controls ───────────────────────────────────────────────
static void CollectAndSave(SettingsDlgState* s) {
    FSTPSettings* settings = GetSettings();
    if (!settings) return;

    // Audio
    if (s->hAudioDevice) {
        int sel = (int)SendMessageW(s->hAudioDevice, CB_GETCURSEL, 0, 0);
        int idx = sel - 1;   // combo item 0 = "Default Audio Device" → -1 (follow system default)
        if (idx < 0) {
            SetAudioDevice(-1, "");
        } else {
            wchar_t wname[256] = {0};
            // Pin by NAME (survives index shifts AND the combo-position ≠ PortAudio-index mismatch).
            if (SendMessageW(s->hAudioDevice, CB_GETLBTEXT, sel, (LPARAM)wname) != CB_ERR)
                SetAudioDevice(idx, WideToUtf8(wname).c_str());
            else
                SetAudioDevice(idx, "");
        }
    }
    if (s->hVolumeSlider)
        settings->audio_master_volume = (float)SendMessage(s->hVolumeSlider, TBM_GETPOS, 0, 0) / 100.0f;
    if (s->hDuckingCheck)
        settings->audio_volume_ducking_enabled = (IsDlgButtonChecked(GetParent(s->hDuckingCheck), IDC_DUCKING_CHECK) == BST_CHECKED) ? 1 : 0;
    if (s->hBufferCombo) {
        static const int buf_sizes[] = { 512, 1024, 2048, 4096 };
        int idx = (int)SendMessage(s->hBufferCombo, CB_GETCURSEL, 0, 0);
        if (idx >= 0 && idx < 4) settings->audio_buffer_size = buf_sizes[idx];
    }

    // Video
    if (s->hFrameOffset) {
        BOOL ok;
        int v = GetDlgItemInt(GetParent(s->hFrameOffset), IDC_FRAME_OFFSET, &ok, TRUE);
        if (ok) settings->frame_offset = std::max(-10, std::min(10, v));
    }
    if (s->hFreezeCheck)
        settings->auto_freeze_inactive = (SendMessage(s->hFreezeCheck, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
    if (s->hBetacamCheck) {
        settings->betacam_effect_enabled = (SendMessage(s->hBetacamCheck, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
        SetBetacamEffectEnabled(settings->betacam_effect_enabled);
    }
    if (s->hReverseStripe) {
        settings->betacam_reverse_stripe = (SendMessage(s->hReverseStripe, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
        SetBetacamReverseStripe(settings->betacam_reverse_stripe);
    }
    if (s->hDecoderStatus)
        settings->show_decoder_status = (SendMessage(s->hDecoderStatus, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;

    // MIDI
    if (s->hMidiEnable)
        settings->midi_enabled = (SendMessage(s->hMidiEnable, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
    if (s->hMidiInput)
        settings->midi_input_port = (int)SendMessage(s->hMidiInput, CB_GETCURSEL, 0, 0) - 1;
    if (s->hMidiOutput)
        settings->midi_output_port = (int)SendMessage(s->hMidiOutput, CB_GETCURSEL, 0, 0) - 1;

    // Extensions
    if (s->hYtdlpCheck) {
        settings->yt_dlp_extension_enabled = (SendMessage(s->hYtdlpCheck, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
        SetYTDLPExtensionEnabled(settings->yt_dlp_extension_enabled);
    }

    SaveSettings();
    ApplyAudioSettings();
    ApplyMIDISettings();
}

// ── Main dialog window proc ───────────────────────────────────────────────────
static LRESULT CALLBACK SettingsDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    SettingsDlgState* s = (SettingsDlgState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {

    case WM_CREATE: {
        HINSTANCE hInst = GetModuleHandle(nullptr);
        s = new SettingsDlgState();
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)s);
        g_hintIdCounter = IDC_HINT_BASE; // reset per dialog open

        // ── Register helper window classes (once) ────────────────────────────
        RegisterPageClass(hInst);

        // ── Register sidebar window class (once) ─────────────────────────────
        static bool sidebarRegistered = false;
        if (!sidebarRegistered) {
            WNDCLASSEXW wc = {};
            wc.cbSize        = sizeof(wc);
            wc.style         = CS_HREDRAW | CS_VREDRAW;
            wc.lpfnWndProc   = SidebarProc;
            wc.hInstance     = hInst;
            wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
            wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
            wc.lpszClassName = L"TXP_Sidebar";
            RegisterClassExW(&wc);
            sidebarRegistered = true;
        }

        // ── Sidebar ──────────────────────────────────────────────────────────
        HWND sidebar = CreateWindowExW(0, L"TXP_Sidebar", L"",
            WS_CHILD | WS_VISIBLE,
            0, 0, SIDEBAR_W, DLG_H - FOOTER_H, hwnd, (HMENU)IDC_SIDEBAR, hInst, nullptr);
        SetWindowLongPtrW(sidebar, GWLP_USERDATA, (LONG_PTR)s);

        // ── Content pages ────────────────────────────────────────────────────
        s->pages[0] = BuildAudioPage(hwnd, s);
        s->pages[1] = BuildVideoPage(hwnd, s);
        s->pages[2] = BuildMidiPage(hwnd, s);
        s->pages[3] = BuildCachePage(hwnd, s);
        s->pages[4] = BuildExtensionsPage(hwnd, s);
        ShowPage(s, 0); // show Audio by default

        // ── Footer separator ─────────────────────────────────────────────────
        CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
            0, DLG_H - FOOTER_H, DLG_W, Dpi(2), hwnd, nullptr, hInst, nullptr);

        // ── Footer buttons ───────────────────────────────────────────────────
        int by = DLG_H - FOOTER_H + Dpi(12);
        CreateWindowExW(0, L"BUTTON", L"Reset to Defaults",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            Dpi(12), by, Dpi(150), Dpi(28), hwnd, (HMENU)IDC_BTN_RESET, hInst, nullptr);

        CreateWindowExW(0, L"BUTTON", L"Cancel",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            DLG_W - Dpi(194), by, Dpi(88), Dpi(28), hwnd, (HMENU)IDCANCEL, hInst, nullptr);

        CreateWindowExW(0, L"BUTTON", L"OK",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            DLG_W - Dpi(98), by, Dpi(86), Dpi(28), hwnd, (HMENU)IDOK, hInst, nullptr);

        // Apply fonts everywhere
        ApplyFontToChildren(hwnd, g_fontNormal);

        return 0;
    }

    case WM_COMMAND: {
        int id   = LOWORD(wParam);
        int code = HIWORD(wParam);

        if (id == IDOK) {
            CollectAndSave(s);
            DestroyWindow(hwnd);
        }
        else if (id == IDCANCEL) {
            DestroyWindow(hwnd);
        }
        else if (id == IDC_BTN_RESET) {
            int r = MessageBoxW(hwnd, L"Reset all settings to defaults?",
                                L"Reset to Defaults", MB_YESNO | MB_ICONQUESTION);
            if (r == IDYES) {
                ResetSettingsToDefault();
                // Re-open with fresh state
                DestroyWindow(hwnd);
                // ShowWin32SettingsDialog() will re-open it from the destroy path
                // but g_dialog_open will be false by then — caller can re-open.
            }
        }
        else if (id == IDC_MIDI_ENABLE && code == BN_CLICKED && s) {
            // Enable/disable MIDI port combos live
            BOOL on = (SendMessage(s->hMidiEnable, BM_GETCHECK, 0, 0) == BST_CHECKED);
            if (s->hMidiInput)  EnableWindow(s->hMidiInput,  on);
            if (s->hMidiOutput) EnableWindow(s->hMidiOutput, on);
        }
        else if (id == IDC_VOLUME_SLIDER && code == TB_THUMBTRACK) {
            // Trackbar sends WM_HSCROLL, not WM_COMMAND — handled below
        }
        else if (id == IDC_CLEAR_PROXY && code == BN_CLICKED) {
            if (MessageBoxW(hwnd, L"Clear all proxy cache files?",
                            L"Clear Proxy Cache", MB_YESNO | MB_ICONWARNING) == IDYES) {
                FSTP_ClearProxyCache(false);
                if (s->hClearProxy) EnableWindow(s->hClearProxy, FALSE);
                if (s->hProxyInfo)  SetWindowTextW(s->hProxyInfo, L"Cache cleared.");
            }
        }
        else if (id == IDC_CLEAR_MEMORY && code == BN_CLICKED) {
            if (MessageBoxW(hwnd, L"Clear all Memory Locations? This cannot be undone.",
                            L"Clear Memory Locations", MB_YESNO | MB_ICONWARNING) == IDYES) {
                FSTP_ClearAllMemoryLocations();
                if (s->hClearMemory) EnableWindow(s->hClearMemory, FALSE);
                if (s->hMemoryInfo)  SetWindowTextW(s->hMemoryInfo, L"Memory Locations cleared.");
            }
        }
        return 0;
    }

    case WM_HSCROLL: {
        // Volume slider
        if (s && s->hVolumeSlider && (HWND)lParam == s->hVolumeSlider) {
            int pos = (int)SendMessage(s->hVolumeSlider, TBM_GETPOS, 0, 0);
            wchar_t buf[16];
            swprintf(buf, 16, L"%d%%", pos);
            if (s->hVolumeLabel) SetWindowTextW(s->hVolumeLabel, buf);
        }
        return 0;
    }

    case WM_ERASEBKGND: {
        // Paint main window background (footer area)
        HDC hdc = (HDC)wParam;
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));
        return 1;
    }

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY: {
        g_dialog_open = false;
        if (s) { delete s; }
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ── Dialog thread ─────────────────────────────────────────────────────────────
static void SettingsDialogThread() {
    ActivateVisualStyles();

    INITCOMMONCONTROLSEX icex = { sizeof(icex), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icex);

    InitDpiMetrics(); // capture monitor DPI and scale all layout constants (before any layout)
    CreateFonts();

    HINSTANCE hInst = GetModuleHandle(nullptr);

    // Register main window class (once)
    static bool mainRegistered = false;
    if (!mainRegistered) {
        WNDCLASSEXW wc = {};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = SettingsDlgProc;
        wc.hInstance     = hInst;
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
        wc.lpszClassName = L"TXP_SettingsWnd";
        RegisterClassExW(&wc);
        mainRegistered = true;
    }

    // Calculate total window size so client area == DLG_W x DLG_H
    DWORD wndStyle   = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    DWORD wndExStyle = WS_EX_DLGMODALFRAME | WS_EX_APPWINDOW;
    RECT  wndRect    = { 0, 0, DLG_W, DLG_H };
    AdjustWindowRectEx(&wndRect, wndStyle, FALSE, wndExStyle);
    int wndW = wndRect.right  - wndRect.left;
    int wndH = wndRect.bottom - wndRect.top;

    // Center on screen
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int wx = (sw - wndW) / 2;
    int wy = (sh - wndH) / 2;

    HWND hwnd = CreateWindowExW(
        wndExStyle,
        L"TXP_SettingsWnd",
        L"Settings \u2014 TapeXPlayer",
        wndStyle,
        wx, wy, wndW, wndH,
        nullptr, nullptr, hInst, nullptr);

    if (!hwnd) {
        std::cerr << "Failed to create settings window, error: " << GetLastError() << std::endl;
        g_dialog_open = false;
        DestroyFonts();
        DeactivateVisualStyles();
        return;
    }

    // Windows 10/11: respect system dark mode for the title bar
    BOOL darkMode = FALSE;
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD val = 1, sz = sizeof(val);
        RegQueryValueExW(hKey, L"AppsUseLightTheme", nullptr, nullptr, (LPBYTE)&val, &sz);
        RegCloseKey(hKey);
        darkMode = (val == 0) ? TRUE : FALSE;
    }
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &darkMode, sizeof(darkMode));

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    // Message loop
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        // Forward mouse wheel to the current page (works even if a child has focus)
        if (msg.message == WM_MOUSEWHEEL) {
            SettingsDlgState* ds = (SettingsDlgState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
            if (ds) {
                SendMessage(ds->pages[ds->currentPage], WM_MOUSEWHEEL, msg.wParam, msg.lParam);
                continue;
            }
        }
        // Up/Down arrows switch sidebar page regardless of focused control
        if (msg.message == WM_KEYDOWN &&
            (msg.wParam == VK_UP || msg.wParam == VK_DOWN)) {
            SettingsDlgState* ds = (SettingsDlgState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
            if (ds) {
                int dir  = (msg.wParam == VK_DOWN) ? 1 : -1;
                int next = std::max(0, std::min(PAGE_COUNT - 1, ds->currentPage + dir));
                ShowPage(ds, next);
                continue;
            }
        }
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    DestroyFonts();
    DeactivateVisualStyles();
    std::cout << "Settings dialog closed" << std::endl;
}

// ── Public entry point ────────────────────────────────────────────────────────
void ShowWin32SettingsDialog() {
    if (g_dialog_open) {
        std::cout << "Settings dialog already open" << std::endl;
        return;
    }
    const FSTPSettings* settings = GetSettings();
    if (!settings) {
        std::cerr << "Failed to get settings" << std::endl;
        return;
    }
    g_dialog_open = true;
    std::thread(SettingsDialogThread).detach();
}

#else
// Stub for non-Windows builds.
#endif
