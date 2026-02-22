#ifdef _WIN32

#include <windows.h>
#include <commctrl.h>
#include <prsht.h>
#include <uxtheme.h>
#include <portaudio.h>
#include <iostream>
#include <cmath>
#include <string>
#include <vector>
#include <thread>
#include "../FSTPSettings.h"
#include "../FSTPWindowManager.h"
#include "../FSTPMemoryLocations.h"
#include "FSTPSettingsDialog.h"

#pragma comment(lib, "comctl32.lib")

// Dialog state (atomic — accessed from main thread and dialog threads)
#include <atomic>
extern std::atomic<bool> g_dialog_open;

// RAII guard to ensure dialog flag is reset
class SettingsDialogGuard {
public:
    SettingsDialogGuard() {
        g_dialog_open = true;
    }
    ~SettingsDialogGuard() {
        g_dialog_open = false;
        std::cout << "Settings dialog closed (flag reset)" << std::endl;
    }
};

// Forward declarations for MIDI functions
extern "C" int GetMIDIInputDeviceCount();
extern "C" int GetMIDIOutputDeviceCount();
extern "C" const char* GetMIDIInputDeviceName(int index);
extern "C" const char* GetMIDIOutputDeviceName(int index);
extern "C" void ApplyMIDISettings();

// Cache/memory functions declared in FSTPMemoryLocations.h (included above)
// Settings reset declared in FSTPSettings.h (included above)

// Control IDs
#define IDC_AUDIO_DEVICE      1010
#define IDC_VOLUME_SLIDER     1011
#define IDC_VOLUME_LABEL      1012
#define IDC_DUCKING_CHECK     1013
#define IDC_BUFFER_COMBO      1014
#define IDC_FRAME_OFFSET      1015
#define IDC_FREEZE_CHECK      1016
#define IDC_BETACAM_CHECK     1017
#define IDC_DECODER_STATUS    1018
#define IDC_MIDI_ENABLE       1020
#define IDC_MIDI_INPUT        1021
#define IDC_MIDI_OUTPUT       1022
#define IDC_CLEAR_PROXY       1030
#define IDC_CLEAR_MEMORY      1031
#define IDC_PROXY_INFO        1032
#define IDC_MEMORY_INFO       1033
#define IDC_YTDLP_CHECK       1040

// Helper: Convert UTF-8 to wide string
static std::wstring Utf8ToWide(const char* utf8) {
    if (!utf8) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (len <= 0) return L"";
    std::wstring wide(len - 1, 0);  // len includes null terminator, exclude it
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &wide[0], len);
    return wide;
}

// Apply the dialog's font to all dynamically created child controls.
// Call at the end of WM_INITDIALOG after all CreateWindowExW calls.
static BOOL CALLBACK SetChildFont(HWND child, LPARAM lParam) {
    SendMessage(child, WM_SETFONT, (WPARAM)lParam, TRUE);
    return TRUE;
}
static void ApplyDialogFont(HWND hwnd) {
    HFONT hFont = (HFONT)SendMessage(hwnd, WM_GETFONT, 0, 0);
    if (hFont)
        EnumChildWindows(hwnd, SetChildFont, (LPARAM)hFont);
}

// Build an in-memory DLGTEMPLATE with DS_SETFONT ("Segoe UI", 9pt)
static std::vector<BYTE> BuildEmptyDialogTemplate(int width, int height) {
    std::vector<BYTE> buf;
    buf.resize(256, 0);
    BYTE* p = buf.data();

    // DLGTEMPLATE
    DLGTEMPLATE* dlg = (DLGTEMPLATE*)p;
    dlg->style = DS_SETFONT | DS_CONTROL | WS_CHILD;
    dlg->dwExtendedStyle = 0;
    dlg->cdit = 0;
    dlg->x = 0;
    dlg->y = 0;
    dlg->cx = (short)width;
    dlg->cy = (short)height;
    p += sizeof(DLGTEMPLATE);

    // Menu (none)
    *(WORD*)p = 0; p += sizeof(WORD);
    // Class (default)
    *(WORD*)p = 0; p += sizeof(WORD);
    // Title (empty)
    *(WORD*)p = 0; p += sizeof(WORD);

    // DS_SETFONT: point size + font name
    *(WORD*)p = 9; p += sizeof(WORD);
    const wchar_t* font = L"Segoe UI";
    size_t font_bytes = (wcslen(font) + 1) * sizeof(wchar_t);
    memcpy(p, font, font_bytes);
    p += font_bytes;

    buf.resize(p - buf.data());
    return buf;
}

// Programmatic ComCtl32 v6 activation.
// Uses the manifest embedded inside comctl32.dll (resource 124).
// This guarantees visual styles even if .rc/.manifest not embedded in .exe.
static HANDLE g_hActCtx = INVALID_HANDLE_VALUE;
static ULONG_PTR g_actCookie = 0;

static void ActivateVisualStyles() {
    if (g_hActCtx != INVALID_HANDLE_VALUE) return; // already active

    wchar_t dllPath[MAX_PATH];
    GetSystemDirectoryW(dllPath, MAX_PATH);
    wcscat(dllPath, L"\\comctl32.dll");

    ACTCTXW act = {};
    act.cbSize = sizeof(act);
    act.dwFlags = ACTCTX_FLAG_RESOURCE_NAME_VALID;
    act.lpSource = dllPath;
    act.lpResourceName = MAKEINTRESOURCEW(124);

    g_hActCtx = CreateActCtxW(&act);
    if (g_hActCtx != INVALID_HANDLE_VALUE) {
        ActivateActCtx(g_hActCtx, &g_actCookie);
        std::cout << "Visual styles activated (ComCtl32 v6)" << std::endl;
    } else {
        std::cerr << "Failed to activate visual styles, error: " << GetLastError() << std::endl;
    }
}

static void DeactivateVisualStyles() {
    if (g_hActCtx != INVALID_HANDLE_VALUE) {
        DeactivateActCtx(0, g_actCookie);
        ReleaseActCtx(g_hActCtx);
        g_hActCtx = INVALID_HANDLE_VALUE;
        g_actCookie = 0;
    }
}

// Theme handling is done automatically by EnableThemeDialogTexture(ETDT_ENABLETAB)
// called in each page's WM_INITDIALOG. DefDlgProc returns the correct themed
// brush for WM_CTLCOLORSTATIC/WM_CTLCOLORBTN — no manual override needed.

// ===== PAGE 0: Audio =====
static INT_PTR CALLBACK AudioPageProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_INITDIALOG: {
        EnableThemeDialogTexture(hwnd, ETDT_ENABLETAB);
        const FSTPSettings* settings = GetSettings();
        if (!settings) return TRUE;
        int x = 10, y = 10;
        HWND h;
        HINSTANCE hInst = GetModuleHandle(NULL);

        h = CreateWindowExW(0, L"STATIC", L"Audio Device:",
            WS_CHILD | WS_VISIBLE, x, y, 120, 20, hwnd, NULL, hInst, NULL);
        y += 22;

        h = CreateWindowExW(0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            x, y, 400, 200, hwnd, (HMENU)IDC_AUDIO_DEVICE, hInst, NULL);
        SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)L"Default Audio Device");
        int device_count = Pa_GetDeviceCount();
        for (int i = 0; i < device_count; i++) {
            const PaDeviceInfo* dev_info = Pa_GetDeviceInfo(i);
            if (dev_info && dev_info->maxOutputChannels > 0) {
                std::wstring name = Utf8ToWide(dev_info->name);
                SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)name.c_str());
            }
        }
        SendMessage(h, CB_SETCURSEL, settings->audio_device_index + 1, 0);
        y += 35;

        wchar_t vol_buf[32];
        swprintf(vol_buf, 32, L"Volume: %d%%", (int)(settings->audio_master_volume * 100));
        h = CreateWindowExW(0, L"STATIC", vol_buf,
            WS_CHILD | WS_VISIBLE, x, y, 100, 20,
            hwnd, (HMENU)IDC_VOLUME_LABEL, hInst, NULL);
        y += 20;

        h = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
            WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_AUTOTICKS,
            x, y, 400, 28, hwnd, (HMENU)IDC_VOLUME_SLIDER, hInst, NULL);
        SendMessage(h, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
        SendMessage(h, TBM_SETPOS, TRUE, (int)(settings->audio_master_volume * 100));
        SendMessage(h, TBM_SETTICFREQ, 10, 0);
        y += 30;

        h = CreateWindowExW(0, L"BUTTON", L"Auto-Reduce Volume at High Speeds",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            x, y, 350, 20, hwnd, (HMENU)IDC_DUCKING_CHECK, hInst, NULL);
        if (settings->audio_volume_ducking_enabled)
            SendMessage(h, BM_SETCHECK, BST_CHECKED, 0);
        y += 22;

        h = CreateWindowExW(0, L"STATIC",
            L"Protects ears during shuttle (6x: fade, 12x: -24dB, 32x: -40dB)",
            WS_CHILD | WS_VISIBLE, x + 16, y, 500, 16, hwnd, NULL, hInst, NULL);
        y += 25;

        h = CreateWindowExW(0, L"STATIC", L"Buffer Size:",
            WS_CHILD | WS_VISIBLE, x, y, 80, 20, hwnd, NULL, hInst, NULL);

        h = CreateWindowExW(0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
            x + 80, y - 2, 130, 120, hwnd, (HMENU)IDC_BUFFER_COMBO, hInst, NULL);
        SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)L"512 samples");
        SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)L"1024 samples");
        SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)L"2048 samples");
        SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)L"4096 samples");
        int buffer_index = 1;
        if (settings->audio_buffer_size == 512) buffer_index = 0;
        else if (settings->audio_buffer_size == 1024) buffer_index = 1;
        else if (settings->audio_buffer_size == 2048) buffer_index = 2;
        else if (settings->audio_buffer_size == 4096) buffer_index = 3;
        SendMessage(h, CB_SETCURSEL, buffer_index, 0);

        h = CreateWindowExW(0, L"STATIC", L"(requires restart)",
            WS_CHILD | WS_VISIBLE, x + 215, y + 2, 180, 16, hwnd, NULL, hInst, NULL);

        ApplyDialogFont(hwnd);
        return TRUE;
    }

    case WM_HSCROLL: {
        HWND hSlider = (HWND)lParam;
        if (GetDlgCtrlID(hSlider) == IDC_VOLUME_SLIDER) {
            int pos = (int)SendMessage(hSlider, TBM_GETPOS, 0, 0);
            wchar_t buf[32];
            swprintf(buf, 32, L"Volume: %d%%", pos);
            SetWindowTextW(GetDlgItem(hwnd, IDC_VOLUME_LABEL), buf);
        }
        return TRUE;
    }

    case WM_NOTIFY: {
        NMHDR* pnmh = (NMHDR*)lParam;
        if (pnmh->code == PSN_APPLY) {
            FSTPSettings* settings = GetSettings();
            if (settings) {
                int device_sel = (int)SendDlgItemMessage(hwnd, IDC_AUDIO_DEVICE, CB_GETCURSEL, 0, 0);
                settings->audio_device_index = device_sel - 1;

                int vol_pos = (int)SendDlgItemMessage(hwnd, IDC_VOLUME_SLIDER, TBM_GETPOS, 0, 0);
                settings->audio_master_volume = vol_pos / 100.0f;

                settings->audio_volume_ducking_enabled =
                    (IsDlgButtonChecked(hwnd, IDC_DUCKING_CHECK) == BST_CHECKED) ? 1 : 0;

                int buffer_sizes[] = {512, 1024, 2048, 4096};
                int buf_sel = (int)SendDlgItemMessage(hwnd, IDC_BUFFER_COMBO, CB_GETCURSEL, 0, 0);
                if (buf_sel >= 0 && buf_sel < 4)
                    settings->audio_buffer_size = buffer_sizes[buf_sel];
            }
            SetWindowLongPtr(hwnd, DWLP_MSGRESULT, PSNRET_NOERROR);
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

// ===== PAGE 1: Video & Sync =====
static INT_PTR CALLBACK VideoPageProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_INITDIALOG: {
        EnableThemeDialogTexture(hwnd, ETDT_ENABLETAB);
        const FSTPSettings* settings = GetSettings();
        if (!settings) return TRUE;
        int x = 10, y = 10;
        HWND h;
        HINSTANCE hInst = GetModuleHandle(NULL);

        h = CreateWindowExW(0, L"STATIC", L"Display Synchronization",
            WS_CHILD | WS_VISIBLE, x, y, 200, 20, hwnd, NULL, hInst, NULL);
        y += 25;

        h = CreateWindowExW(0, L"STATIC", L"Frame Offset:",
            WS_CHILD | WS_VISIBLE, x, y, 100, 20, hwnd, NULL, hInst, NULL);

        wchar_t offset_buf[16];
        swprintf(offset_buf, 16, L"%d", settings->frame_offset);
        h = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", offset_buf,
            WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_CENTER,
            x + 110, y, 60, 22, hwnd, (HMENU)IDC_FRAME_OFFSET, hInst, NULL);

        h = CreateWindowExW(0, L"STATIC", L"frames (-10 to +10)",
            WS_CHILD | WS_VISIBLE, x + 180, y, 200, 20, hwnd, NULL, hInst, NULL);
        y += 35;

        h = CreateWindowExW(0, L"STATIC", L"Multi-Instance Performance",
            WS_CHILD | WS_VISIBLE, x, y, 250, 20, hwnd, NULL, hInst, NULL);
        y += 25;

        h = CreateWindowExW(0, L"BUTTON", L"Auto-freeze inactive players",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            x, y, 300, 20, hwnd, (HMENU)IDC_FREEZE_CHECK, hInst, NULL);
        if (settings->auto_freeze_inactive)
            SendMessage(h, BM_SETCHECK, BST_CHECKED, 0);
        y += 22;

        h = CreateWindowExW(0, L"STATIC",
            L"Prevents forgotten players from consuming resources",
            WS_CHILD | WS_VISIBLE, x + 16, y, 500, 16, hwnd, NULL, hInst, NULL);
        y += 35;

        h = CreateWindowExW(0, L"STATIC", L"Visual Effects",
            WS_CHILD | WS_VISIBLE, x, y, 200, 20, hwnd, NULL, hInst, NULL);
        y += 25;

        h = CreateWindowExW(0, L"BUTTON", L"Enable Betacam tape artefact emulation",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            x, y, 350, 20, hwnd, (HMENU)IDC_BETACAM_CHECK, hInst, NULL);
        if (settings->betacam_effect_enabled)
            SendMessage(h, BM_SETCHECK, BST_CHECKED, 0);
        y += 22;

        h = CreateWindowExW(0, L"STATIC",
            L"Adds rewind/fast-forward tape jitter. May impact performance.",
            WS_CHILD | WS_VISIBLE, x + 16, y, 500, 16, hwnd, NULL, hInst, NULL);
        y += 35;

        h = CreateWindowExW(0, L"STATIC", L"Developer/Debug",
            WS_CHILD | WS_VISIBLE, x, y, 200, 20, hwnd, NULL, hInst, NULL);
        y += 25;

        h = CreateWindowExW(0, L"BUTTON", L"Show Decoder Status",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            x, y, 300, 20, hwnd, (HMENU)IDC_DECODER_STATUS, hInst, NULL);
        if (settings->show_decoder_status)
            SendMessage(h, BM_SETCHECK, BST_CHECKED, 0);
        y += 22;

        h = CreateWindowExW(0, L"STATIC",
            L"Displays decoded frames indicator for debugging performance.",
            WS_CHILD | WS_VISIBLE, x + 16, y, 500, 16, hwnd, NULL, hInst, NULL);

        ApplyDialogFont(hwnd);
        return TRUE;
    }

    case WM_NOTIFY: {
        NMHDR* pnmh = (NMHDR*)lParam;
        if (pnmh->code == PSN_APPLY) {
            FSTPSettings* settings = GetSettings();
            if (settings) {
                BOOL success;
                int offset = GetDlgItemInt(hwnd, IDC_FRAME_OFFSET, &success, TRUE);
                if (success) {
                    if (offset < -10) offset = -10;
                    if (offset > 10) offset = 10;
                    settings->frame_offset = offset;
                }

                settings->auto_freeze_inactive =
                    (IsDlgButtonChecked(hwnd, IDC_FREEZE_CHECK) == BST_CHECKED) ? 1 : 0;

                settings->betacam_effect_enabled =
                    (IsDlgButtonChecked(hwnd, IDC_BETACAM_CHECK) == BST_CHECKED) ? 1 : 0;
                SetBetacamEffectEnabled(settings->betacam_effect_enabled);

                settings->show_decoder_status =
                    (IsDlgButtonChecked(hwnd, IDC_DECODER_STATUS) == BST_CHECKED) ? 1 : 0;
            }
            SetWindowLongPtr(hwnd, DWLP_MSGRESULT, PSNRET_NOERROR);
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

// ===== PAGE 2: MIDI =====
static INT_PTR CALLBACK MIDIPageProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_INITDIALOG: {
        EnableThemeDialogTexture(hwnd, ETDT_ENABLETAB);
        const FSTPSettings* settings = GetSettings();
        if (!settings) return TRUE;
        int x = 10, y = 10;
        HWND h;
        HINSTANCE hInst = GetModuleHandle(NULL);

        h = CreateWindowExW(0, L"STATIC", L"MIDI Controller",
            WS_CHILD | WS_VISIBLE, x, y, 200, 20, hwnd, NULL, hInst, NULL);
        y += 25;

        h = CreateWindowExW(0, L"BUTTON", L"Enable MIDI Controller",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            x, y, 250, 20, hwnd, (HMENU)IDC_MIDI_ENABLE, hInst, NULL);
        if (settings->midi_enabled)
            SendMessage(h, BM_SETCHECK, BST_CHECKED, 0);
        y += 35;

        h = CreateWindowExW(0, L"STATIC", L"MIDI Ports",
            WS_CHILD | WS_VISIBLE, x, y, 200, 20, hwnd, NULL, hInst, NULL);
        y += 25;

        h = CreateWindowExW(0, L"STATIC", L"Input Port:",
            WS_CHILD | WS_VISIBLE, x, y, 80, 20, hwnd, NULL, hInst, NULL);

        h = CreateWindowExW(0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            x + 90, y, 350, 200, hwnd, (HMENU)IDC_MIDI_INPUT, hInst, NULL);
        SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)L"(None)");
        int midi_input_count = GetMIDIInputDeviceCount();
        for (int i = 0; i < midi_input_count; i++) {
            const char* name = GetMIDIInputDeviceName(i);
            if (name) {
                std::wstring wname = Utf8ToWide(name);
                SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)wname.c_str());
            }
        }
        SendMessage(h, CB_SETCURSEL, settings->midi_input_port + 1, 0);
        EnableWindow(h, settings->midi_enabled);
        y += 30;

        h = CreateWindowExW(0, L"STATIC", L"Output Port:",
            WS_CHILD | WS_VISIBLE, x, y, 80, 20, hwnd, NULL, hInst, NULL);

        h = CreateWindowExW(0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            x + 90, y, 350, 200, hwnd, (HMENU)IDC_MIDI_OUTPUT, hInst, NULL);
        SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)L"(None)");
        int midi_output_count = GetMIDIOutputDeviceCount();
        for (int i = 0; i < midi_output_count; i++) {
            const char* name = GetMIDIOutputDeviceName(i);
            if (name) {
                std::wstring wname = Utf8ToWide(name);
                SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)wname.c_str());
            }
        }
        SendMessage(h, CB_SETCURSEL, settings->midi_output_port + 1, 0);
        EnableWindow(h, settings->midi_enabled);
        y += 35;

        h = CreateWindowExW(0, L"STATIC", L"Protocol: Mackie Control",
            WS_CHILD | WS_VISIBLE, x, y, 250, 20, hwnd, NULL, hInst, NULL);
        y += 22;

        h = CreateWindowExW(0, L"STATIC", L"Controller support is in development",
            WS_CHILD | WS_VISIBLE, x, y, 350, 16, hwnd, NULL, hInst, NULL);

        ApplyDialogFont(hwnd);
        return TRUE;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        int code = HIWORD(wParam);
        if (id == IDC_MIDI_ENABLE && code == BN_CLICKED) {
            BOOL enabled = IsDlgButtonChecked(hwnd, IDC_MIDI_ENABLE) == BST_CHECKED;
            EnableWindow(GetDlgItem(hwnd, IDC_MIDI_INPUT), enabled);
            EnableWindow(GetDlgItem(hwnd, IDC_MIDI_OUTPUT), enabled);
        }
        break;
    }

    case WM_NOTIFY: {
        NMHDR* pnmh = (NMHDR*)lParam;
        if (pnmh->code == PSN_APPLY) {
            FSTPSettings* settings = GetSettings();
            if (settings) {
                settings->midi_enabled =
                    (IsDlgButtonChecked(hwnd, IDC_MIDI_ENABLE) == BST_CHECKED) ? 1 : 0;
                settings->midi_input_port =
                    (int)SendDlgItemMessage(hwnd, IDC_MIDI_INPUT, CB_GETCURSEL, 0, 0) - 1;
                settings->midi_output_port =
                    (int)SendDlgItemMessage(hwnd, IDC_MIDI_OUTPUT, CB_GETCURSEL, 0, 0) - 1;
            }
            SetWindowLongPtr(hwnd, DWLP_MSGRESULT, PSNRET_NOERROR);
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

// ===== PAGE 3: Cache & Data =====
static INT_PTR CALLBACK CachePageProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_INITDIALOG: {
        EnableThemeDialogTexture(hwnd, ETDT_ENABLETAB);
        int x = 10, y = 10;
        HWND h;
        HINSTANCE hInst = GetModuleHandle(NULL);

        h = CreateWindowExW(0, L"STATIC", L"Proxy Video Cache",
            WS_CHILD | WS_VISIBLE, x, y, 200, 20, hwnd, NULL, hInst, NULL);
        y += 25;

        const char* proxy_path = FSTP_GetProxyCachePath();
        int proxy_size = FSTP_GetProxyCacheSize();
        int proxy_count = FSTP_GetProxyFilesCount();

        std::wstring proxy_path_w = Utf8ToWide(proxy_path ? proxy_path : "Unknown");
        wchar_t proxy_info[512];
        swprintf(proxy_info, 512, L"Location: %ls\nCache Size: %d MB (%d files)",
                 proxy_path_w.c_str(), proxy_size, proxy_count);
        h = CreateWindowExW(0, L"STATIC", proxy_info,
            WS_CHILD | WS_VISIBLE, x, y, 500, 40, hwnd, (HMENU)IDC_PROXY_INFO, hInst, NULL);
        y += 45;

        h = CreateWindowExW(0, L"BUTTON", L"Clear All Proxy Cache",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            x, y, 200, 28, hwnd, (HMENU)IDC_CLEAR_PROXY, hInst, NULL);
        EnableWindow(h, proxy_count > 0);
        y += 45;

        h = CreateWindowExW(0, L"STATIC", L"Memory Locations",
            WS_CHILD | WS_VISIBLE, x, y, 200, 20, hwnd, NULL, hInst, NULL);
        y += 25;

        const char* memory_path = FSTP_GetMemoryLocationsCachePath();
        int memory_count = FSTP_GetMemoryLocationsFilesCount();

        std::wstring memory_path_w = Utf8ToWide(memory_path ? memory_path : "Unknown");
        wchar_t memory_info[512];
        swprintf(memory_info, 512, L"Location: %ls/memory_locations\nSaved Data: %d video files",
                 memory_path_w.c_str(), memory_count);
        h = CreateWindowExW(0, L"STATIC", memory_info,
            WS_CHILD | WS_VISIBLE, x, y, 500, 40, hwnd, (HMENU)IDC_MEMORY_INFO, hInst, NULL);
        y += 45;

        h = CreateWindowExW(0, L"BUTTON", L"Clear All Memory Locations",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            x, y, 200, 28, hwnd, (HMENU)IDC_CLEAR_MEMORY, hInst, NULL);
        EnableWindow(h, memory_count > 0);
        y += 30;

        h = CreateWindowExW(0, L"STATIC",
            L"Memory Locations are saved per-video like browser cookies",
            WS_CHILD | WS_VISIBLE, x, y, 500, 16, hwnd, NULL, hInst, NULL);

        ApplyDialogFont(hwnd);
        return TRUE;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        int code = HIWORD(wParam);

        if (id == IDC_CLEAR_PROXY && code == BN_CLICKED) {
            int result = MessageBoxW(hwnd, L"Clear all proxy cache files?",
                                      L"Clear Proxy Cache", MB_YESNO | MB_ICONWARNING);
            if (result == IDYES) {
                FSTP_ClearProxyCache(false);
                std::cout << "All proxy cache cleared" << std::endl;
            }
        }

        if (id == IDC_CLEAR_MEMORY && code == BN_CLICKED) {
            int result = MessageBoxW(hwnd, L"Clear all Memory Locations? This cannot be undone.",
                                      L"Clear Memory Locations", MB_YESNO | MB_ICONWARNING);
            if (result == IDYES) {
                FSTP_ClearAllMemoryLocations();
                std::cout << "All Memory Locations cleared" << std::endl;
            }
        }
        break;
    }

    case WM_NOTIFY: {
        NMHDR* pnmh = (NMHDR*)lParam;
        if (pnmh->code == PSN_APPLY) {
            // Cache page has no persistent settings to save
            SetWindowLongPtr(hwnd, DWLP_MSGRESULT, PSNRET_NOERROR);
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

// ===== PAGE 4: Extensions =====
static INT_PTR CALLBACK ExtensionsPageProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_INITDIALOG: {
        EnableThemeDialogTexture(hwnd, ETDT_ENABLETAB);
        const FSTPSettings* settings = GetSettings();
        if (!settings) return TRUE;
        int x = 10, y = 10;
        HWND h;
        HINSTANCE hInst = GetModuleHandle(NULL);

        h = CreateWindowExW(0, L"STATIC", L"Extensions",
            WS_CHILD | WS_VISIBLE, x, y, 200, 20, hwnd, NULL, hInst, NULL);
        y += 30;

        bool yt_dlp_available = FSTP_YTDLP_IsAvailable();
        if (yt_dlp_available) {
            h = CreateWindowExW(0, L"BUTTON", L"Enable yt-dlp network downloader",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                x, y, 350, 20, hwnd, (HMENU)IDC_YTDLP_CHECK, hInst, NULL);
            if (settings->yt_dlp_extension_enabled)
                SendMessage(h, BM_SETCHECK, BST_CHECKED, 0);
        } else {
            h = CreateWindowExW(0, L"STATIC",
                L"yt-dlp not found. Install it to enable network downloads.",
                WS_CHILD | WS_VISIBLE, x, y, 500, 20, hwnd, NULL, hInst, NULL);
        }
        y += 30;

        std::string ext_lang = GetExtensionLanguage();
        std::wstring ext_info = L"TapeXPlayer extensions are scripted using " +
            Utf8ToWide(ext_lang.c_str()) +
            L" (.lua) files.\r\n\r\n"
            L"Extension loading is being prepared; this section will expand "
            L"with management tools as the system evolves.";
        h = CreateWindowExW(0, L"STATIC", ext_info.c_str(),
            WS_CHILD | WS_VISIBLE, x, y, 500, 80, hwnd, NULL, hInst, NULL);

        ApplyDialogFont(hwnd);
        return TRUE;
    }

    case WM_NOTIFY: {
        NMHDR* pnmh = (NMHDR*)lParam;
        if (pnmh->code == PSN_APPLY) {
            FSTPSettings* settings = GetSettings();
            if (settings) {
                settings->yt_dlp_extension_enabled =
                    (IsDlgButtonChecked(hwnd, IDC_YTDLP_CHECK) == BST_CHECKED) ? 1 : 0;
                SetYTDLPExtensionEnabled(settings->yt_dlp_extension_enabled);
            }
            SetWindowLongPtr(hwnd, DWLP_MSGRESULT, PSNRET_NOERROR);
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

// Settings dialog thread function — runs PropertySheet in its own thread
// so the main SDL loop continues rendering.
static void SettingsDialogThread() {
    // Enable ComCtl32 v6 visual styles for this thread
    ActivateVisualStyles();

    // Initialize Common Controls
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_TAB_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icex);

    // Build in-memory dialog template (shared by all pages)
    std::vector<BYTE> dlgTemplate = BuildEmptyDialogTemplate(285, 195);

    // Set up 5 property sheet pages
    PROPSHEETPAGEW psp[5] = {};
    const wchar_t* tabNames[] = {
        L"Audio", L"Video && Sync", L"MIDI", L"Cache && Data", L"Extensions"
    };
    DLGPROC procs[] = {
        AudioPageProc, VideoPageProc, MIDIPageProc, CachePageProc, ExtensionsPageProc
    };

    for (int i = 0; i < 5; i++) {
        psp[i].dwSize = sizeof(PROPSHEETPAGEW);
        psp[i].dwFlags = PSP_DLGINDIRECT | PSP_USETITLE;
        psp[i].hInstance = GetModuleHandle(NULL);
        psp[i].pResource = (DLGTEMPLATE*)dlgTemplate.data();
        psp[i].pszTitle = tabNames[i];
        psp[i].pfnDlgProc = procs[i];
    }

    // Set up the property sheet header
    PROPSHEETHEADERW psh = {};
    psh.dwSize = sizeof(PROPSHEETHEADERW);
    psh.dwFlags = PSH_PROPSHEETPAGE | PSH_NOAPPLYNOW;
    psh.hwndParent = NULL;
    psh.hInstance = GetModuleHandle(NULL);
    psh.pszCaption = L"Settings";
    psh.nPages = 5;
    psh.nStartPage = 0;
    psh.ppsp = psp;

    // PropertySheetW blocks until user closes dialog — but only THIS thread
    INT_PTR result = PropertySheetW(&psh);

    if (result > 0) {
        SaveSettings();
        ApplyAudioSettings();
        ApplyMIDISettings();
        std::cout << "Settings saved" << std::endl;
    } else {
        std::cout << "Settings dialog cancelled" << std::endl;
    }

    DeactivateVisualStyles();
    g_dialog_open = false;
    std::cout << "Settings dialog closed (flag reset)" << std::endl;
}

void ShowWin32SettingsDialog() {
    std::cout << "ShowWin32SettingsDialog called" << std::endl;

    if (g_dialog_open) {
        std::cout << "Dialog already open, skipping call" << std::endl;
        return;
    }

    const FSTPSettings* settings = GetSettings();
    if (!settings) {
        std::cerr << "Failed to get settings" << std::endl;
        return;
    }

    g_dialog_open = true;

    // Launch dialog in separate thread so main SDL loop continues rendering
    std::thread dialog_thread(SettingsDialogThread);
    dialog_thread.detach();
}

#else
// Stub translation unit for non-Windows builds.
#endif
