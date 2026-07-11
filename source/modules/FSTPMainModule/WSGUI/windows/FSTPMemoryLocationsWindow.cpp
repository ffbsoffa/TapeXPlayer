#ifdef _WIN32

#include <windows.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <iostream>
#include <string>
#include <cmath>
#include "../FSTPMemoryLocations.h"
#include "FSTPMemoryLocationsWindow.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "dwmapi.lib")

#define IDI_APPICON 101

// Forward declarations
extern "C" int GetActivePlayerID();
extern "C" double GetInstancePosition(int player_id);
extern "C" double GetInstanceVideoFPS(int player_id);

// Global window state
static HWND g_memory_window = NULL;
static HWND g_listview = NULL;
static HWND g_subtitle_label = NULL;   // "Player N · X markers" (as on macOS)
static UINT_PTR g_refresh_timer = 0;

// ── Styling in the spirit of FSTPSettingsDialog: Segoe UI, white content, grey
//    hints, a divider above the bottom bar. Without this the window looked like
//    Win95: DEFAULT_GUI_FONT + gridlines + a sunken 3D border.
static HFONT g_mlFontNormal = NULL;
static HFONT g_mlFontSmall  = NULL;
static HBRUSH g_mlBgBrush   = NULL;
static const COLORREF ML_CLR_BG      = RGB(255, 255, 255);
static const COLORREF ML_CLR_TEXT    = RGB(30, 30, 30);
static const COLORREF ML_CLR_HINT    = RGB(120, 120, 120);
static const COLORREF ML_CLR_DIVIDER = RGB(220, 220, 220);
static const int ML_HEADER_H = 34;   // subtitle strip above the table
static const int ML_FOOTER_H = 46;   // bottom bar with buttons

// ── DPI scaling ──────────────────────────────────────────────────────────────
// The add/edit dialog is hand-laid-out in logical (96-DPI) pixels. The fonts already
// scale with DPI (CreateFontW via -MulDiv(pt, dpiY, 72)); the GEOMETRY must scale by the
// same factor or, at 125%/150%, the larger text clips its fixed boxes and rows overlap —
// exactly what FSTPSettingsDialog fixes with its Dpi() helper. Captured in
// EnsureMLFontsAndBrush() so both the fonts and this factor come from the same LOGPIXELSY.
static double g_mlDpiScale = 1.0;
static inline int MlDpi(int logical) { return (int)(logical * g_mlDpiScale + 0.5); }

// Control IDs
#define IDC_LISTVIEW       2001
#define IDC_EXPORT_BTN     2002
#define IDC_INFO_LABEL     2003
#define IDC_IMPORT_BTN     2004
#define IDC_SUBTITLE_LABEL 2005
#define IDM_TIMER_REFRESH  3001

// ListView columns (mirror the macOS table: # / Timecode / Name / Comments)
enum {
    COL_ID = 0,
    COL_TIMECODE,
    COL_NAME,
    COL_COMMENTS,
    NUM_COLS
};

// Fonts/brush are created lazily: the add-marker dialog can open from the
// keyboard (Enter) before the main Memory Locations window.
static void EnsureMLFontsAndBrush() {
    HDC screen = GetDC(NULL);
    int dpiY = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(NULL, screen);
    if (dpiY <= 0) dpiY = 96;
    g_mlDpiScale = dpiY / 96.0;   // keep geometry in step with the DPI-scaled fonts below
    if (!g_mlFontNormal) {
        g_mlFontNormal = CreateFontW(-MulDiv(10, dpiY, 72), 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        g_mlFontSmall = CreateFontW(-MulDiv(9, dpiY, 72), 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    }
    if (!g_mlBgBrush) g_mlBgBrush = CreateSolidBrush(ML_CLR_BG);
}

// Helper: Convert UTF-8 to wide string
static std::wstring Utf8ToWide(const char* utf8) {
    if (!utf8) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    std::wstring wide(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &wide[0], len);
    return wide;
}

// Helper: Convert wide string to UTF-8
static std::string WideToUtf8(const wchar_t* wide) {
    if (!wide) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    std::string utf8(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, &utf8[0], len, NULL, NULL);
    return utf8;
}

// Refresh table data from backend
static void RefreshMemoryLocationsTable() {
    if (!g_listview) return;

    ListView_DeleteAllItems(g_listview);

    int count = FSTP_GetMemoryLocationsCount();

    // Subtitle: player + count (also the empty-state hint).
    if (g_subtitle_label) {
        wchar_t sub[160];
        if (count == 0) {
            swprintf(sub, 160, L"No markers — press Enter during playback to add one");
        } else {
            int player = GetActivePlayerID();
            swprintf(sub, 160, L"Player %d  ·  %d %s",
                     (player >= 0 ? player + 1 : 1), count,
                     count == 1 ? L"marker" : L"markers");
        }
        SetWindowTextW(g_subtitle_label, sub);
    }

    for (int i = 0; i < count; i++) {
        FSTP_MemoryLocationData data;
        if (FSTP_GetMemoryLocationData(i, &data)) {
            // ID
            wchar_t id_buf[16];
            swprintf(id_buf, 16, L"%d", data.id);

            LVITEMW lvi = {};
            lvi.mask = LVIF_TEXT;
            lvi.iItem = i;
            lvi.iSubItem = COL_ID;
            lvi.pszText = id_buf;
            SendMessageW(g_listview, LVM_INSERTITEMW, 0, (LPARAM)&lvi);

            // Timecode
            std::wstring tc = Utf8ToWide(data.timecode_display);
            lvi.iSubItem = COL_TIMECODE;
            lvi.pszText = (LPWSTR)tc.c_str();
            SendMessageW(g_listview, LVM_SETITEMW, 0, (LPARAM)&lvi);

            // Name
            std::wstring name = Utf8ToWide(data.name);
            lvi.iSubItem = COL_NAME;
            lvi.pszText = (LPWSTR)name.c_str();
            SendMessageW(g_listview, LVM_SETITEMW, 0, (LPARAM)&lvi);

            // Comments
            std::wstring comments = Utf8ToWide(data.comments);
            lvi.iSubItem = COL_COMMENTS;
            lvi.pszText = (LPWSTR)comments.c_str();
            SendMessageW(g_listview, LVM_SETITEMW, 0, (LPARAM)&lvi);
        }
    }
}

// Forward declaration
static void ShowAddEditMemoryLocationDialog(int player_id, int location_id, double current_time);

// Export to CSV
static void OnExportToCSV() {
    std::cout << "Exporting Memory Locations to CSV..." << std::endl;

    wchar_t filepath[MAX_PATH] = L"memory_locations.csv";

    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_memory_window;
    ofn.lpstrFilter = L"CSV files (*.csv)\0*.csv\0All Files\0*.*\0";
    ofn.lpstrFile = filepath;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Export Memory Locations to CSV";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    ofn.lpstrDefExt = L"csv";

    if (GetSaveFileNameW(&ofn)) {
        std::string utf8_path = WideToUtf8(filepath);

        if (FSTP_ExportMemoryLocationsToCSV(utf8_path.c_str())) {
            std::cout << "Memory Locations exported to: " << utf8_path << std::endl;
            MessageBoxW(g_memory_window, L"Memory Locations exported successfully!",
                        L"Export Successful", MB_OK | MB_ICONINFORMATION);
        } else {
            std::cerr << "Failed to export Memory Locations to CSV" << std::endl;
            MessageBoxW(g_memory_window, L"Failed to export Memory Locations.",
                        L"Export Failed", MB_OK | MB_ICONERROR);
        }
    }
}

// Import from a Memory Locations file
static void OnImport() {
    wchar_t filepath[MAX_PATH] = L"";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_memory_window;
    ofn.lpstrFilter = L"Memory Locations (*.csv;*.json;*.txt)\0*.csv;*.json;*.txt\0All Files\0*.*\0";
    ofn.lpstrFile = filepath;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Import Memory Locations";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) {
        std::string utf8_path = WideToUtf8(filepath);
        FSTP_LoadMemoryLocations(utf8_path.c_str());
        RefreshMemoryLocationsTable();
    }
}

// Get location ID from a specific ListView row (by index, not selection state)
static int GetLocationIDFromRow(int row) {
    if (row < 0 || !g_listview) return -1;
    wchar_t buf[16] = {};
    LVITEMW lvi = {};
    lvi.mask = LVIF_TEXT;
    lvi.iItem = row;
    lvi.iSubItem = COL_ID;
    lvi.pszText = buf;
    lvi.cchTextMax = 16;
    SendMessageW(g_listview, LVM_GETITEMW, 0, (LPARAM)&lvi);
    return _wtoi(buf);
}

// Memory Locations window procedure
static LRESULT CALLBACK MemoryLocationsProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_SIZE: {
            int width = LOWORD(lParam);
            int height = HIWORD(lParam);
            // Subtitle on top, table in the middle, button bar at the bottom.
            if (g_subtitle_label) {
                SetWindowPos(g_subtitle_label, NULL, 12, 9, width - 24, 18, SWP_NOZORDER);
            }
            if (g_listview) {
                SetWindowPos(g_listview, NULL, 0, ML_HEADER_H,
                             width, height - ML_HEADER_H - ML_FOOTER_H, SWP_NOZORDER);
            }
            HWND hExport = GetDlgItem(hwnd, IDC_EXPORT_BTN);
            if (hExport) {
                SetWindowPos(hExport, NULL, width - 142, height - ML_FOOTER_H + 9, 130, 28, SWP_NOZORDER);
            }
            HWND hImport = GetDlgItem(hwnd, IDC_IMPORT_BTN);
            if (hImport) {
                SetWindowPos(hImport, NULL, width - 282, height - ML_FOOTER_H + 9, 130, 28, SWP_NOZORDER);
            }
            HWND hInfo = GetDlgItem(hwnd, IDC_INFO_LABEL);
            if (hInfo) {
                SetWindowPos(hInfo, NULL, 12, height - ML_FOOTER_H + 14, width - 300, 18, SWP_NOZORDER);
            }
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            // Thin divider above the bottom bar (like CLR_DIVIDER in Settings).
            RECT div = { 0, rc.bottom - ML_FOOTER_H, rc.right, rc.bottom - ML_FOOTER_H + 1 };
            HBRUSH divBrush = CreateSolidBrush(ML_CLR_DIVIDER);
            FillRect(hdc, &div, divBrush);
            DeleteObject(divBrush);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_CTLCOLORSTATIC: {
            // White background under statics; subtitle and hint are grey.
            HDC hdc = (HDC)wParam;
            SetBkColor(hdc, ML_CLR_BG);
            SetTextColor(hdc, ((HWND)lParam == g_subtitle_label) ? ML_CLR_TEXT : ML_CLR_HINT);
            if (!g_mlBgBrush) g_mlBgBrush = CreateSolidBrush(ML_CLR_BG);
            return (LRESULT)g_mlBgBrush;
        }

        case WM_TIMER:
            if (wParam == IDM_TIMER_REFRESH) {
                RefreshMemoryLocationsTable();
            }
            return 0;

        case WM_NOTIFY: {
            NMHDR* pnmh = (NMHDR*)lParam;
            if (pnmh->idFrom == IDC_LISTVIEW) {
                if (pnmh->code == NM_DBLCLK) {
                    // Double-click: recall location
                    // Use iItem from notification directly — do not rely on selection state.
                    NMITEMACTIVATE* pnmia = (NMITEMACTIVATE*)lParam;
                    int id = GetLocationIDFromRow(pnmia->iItem);
                    if (id > 0) {
                        int active_player = GetActivePlayerID();
                        if (active_player >= 0) {
                            FSTP_RecallMemoryLocation(id, active_player);
                            std::cout << "Recalled Memory Location #" << id << std::endl;
                        }
                    }
                    return 0;
                }

                if (pnmh->code == NM_CLICK) {
                    NMITEMACTIVATE* pnmia = (NMITEMACTIVATE*)lParam;
                    if (pnmia->iItem < 0) break;  // click on empty area

                    // Read ID directly from the clicked row — not from selection state,
                    // which may lag behind on the first click into an unfocused window.
                    int id = GetLocationIDFromRow(pnmia->iItem);
                    if (id <= 0) break;

                    // Ctrl+Click: edit
                    bool ctrl_held = (pnmia->uKeyFlags & LVKF_CONTROL) ||
                                     (GetKeyState(VK_CONTROL) & 0x8000);
                    if (ctrl_held) {
                        std::cout << "Ctrl+Click - Editing Memory Location #" << id << std::endl;
                        ShowAddEditMemoryLocationDialog(0, id, 0.0);
                        return 0;
                    }

                    // Alt+Click: delete
                    bool alt_held = (pnmia->uKeyFlags & LVKF_ALT) ||
                                    (GetKeyState(VK_MENU) & 0x8000);
                    if (alt_held) {
                        std::cout << "Alt+Click - Deleting Memory Location #" << id << std::endl;
                        if (FSTP_DeleteMemoryLocation(id)) {
                            RefreshMemoryLocationsTable();
                            std::cout << "Memory Location #" << id << " deleted" << std::endl;
                        }
                        return 0;
                    }
                }
            }
            break;
        }

        case WM_COMMAND:
            if (HIWORD(wParam) == BN_CLICKED) {
                if (LOWORD(wParam) == IDC_EXPORT_BTN) OnExportToCSV();
                else if (LOWORD(wParam) == IDC_IMPORT_BTN) OnImport();
            }
            break;

        case WM_CLOSE:
            // Hide instead of destroy (window can be shown again)
            ShowWindow(hwnd, SW_HIDE);
            return 0;

        case WM_DESTROY:
            if (g_refresh_timer) {
                KillTimer(hwnd, IDM_TIMER_REFRESH);
                g_refresh_timer = 0;
            }
            g_memory_window = NULL;
            g_listview = NULL;
            g_subtitle_label = NULL;
            return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Create the Memory Locations window
static void CreateMemoryLocationsWindow() {
    if (g_memory_window) return;

    std::cout << "Creating Memory Locations window..." << std::endl;

    // Segoe UI fonts (like FSTPSettingsDialog) — instead of DEFAULT_GUI_FONT
    EnsureMLFontsAndBrush();

    // Register window class
    static bool class_registered = false;
    if (!class_registered) {
        HINSTANCE hInst = GetModuleHandle(NULL);
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.lpfnWndProc = MemoryLocationsProc;
        wc.hInstance = hInst;
        wc.lpszClassName = L"FSTPMemoryLocationsClass";
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = g_mlBgBrush;  // white content instead of grey COLOR_BTNFACE
        wc.hIcon   = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON),
                                       IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
        wc.hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON),
                                       IMAGE_ICON, 16, 16, 0);
        RegisterClassExW(&wc);
        class_registered = true;
    }

    // Create window
    g_memory_window = CreateWindowExW(
        0,
        L"FSTPMemoryLocationsClass",
        L"Memory Locations",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        720, 430,
        NULL, NULL,
        GetModuleHandle(NULL), NULL
    );

    if (!g_memory_window) {
        std::cerr << "Failed to create Memory Locations window" << std::endl;
        return;
    }

    // Initialize Common Controls
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icex);

    // Subtitle above the table ("Player N · X markers")
    g_subtitle_label = CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_ENDELLIPSIS,
        12, 9, 690, 18,
        g_memory_window, (HMENU)IDC_SUBTITLE_LABEL, NULL, NULL);
    SendMessage(g_subtitle_label, WM_SETFONT, (WPARAM)g_mlFontNormal, TRUE);

    // Create ListView — flat, no 3D border or gridlines, with the Explorer theme
    g_listview = CreateWindowExW(
        0,
        WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        0, ML_HEADER_H, 720, 430 - ML_HEADER_H - ML_FOOTER_H,
        g_memory_window,
        (HMENU)IDC_LISTVIEW,
        GetModuleHandle(NULL), NULL
    );
    SendMessage(g_listview, WM_SETFONT, (WPARAM)g_mlFontNormal, TRUE);
    ListView_SetExtendedListViewStyle(g_listview,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    SetWindowTheme(g_listview, L"Explorer", NULL);  // modern row selection
    ListView_SetBkColor(g_listview, ML_CLR_BG);
    ListView_SetTextBkColor(g_listview, ML_CLR_BG);
    ListView_SetTextColor(g_listview, ML_CLR_TEXT);

    // Add columns
    LVCOLUMNW lvc = {};
    lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;

    // ID column
    lvc.fmt = LVCFMT_LEFT;
    lvc.cx = 50;
    lvc.pszText = (LPWSTR)L"#";
    SendMessageW(g_listview, LVM_INSERTCOLUMNW, COL_ID, (LPARAM)&lvc);

    // Timecode column
    lvc.cx = 120;
    lvc.pszText = (LPWSTR)L"Timecode";
    SendMessageW(g_listview, LVM_INSERTCOLUMNW, COL_TIMECODE, (LPARAM)&lvc);

    // Name column
    lvc.cx = 200;
    lvc.pszText = (LPWSTR)L"Name";
    SendMessageW(g_listview, LVM_INSERTCOLUMNW, COL_NAME, (LPARAM)&lvc);

    // Comments column
    lvc.cx = 300;
    lvc.pszText = (LPWSTR)L"Comments";
    SendMessageW(g_listview, LVM_INSERTCOLUMNW, COL_COMMENTS, (LPARAM)&lvc);

    // Bottom bar: Import + Export buttons
    HWND hExport = CreateWindowExW(0, L"BUTTON", L"Export to CSV",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        578, 393, 130, 28,
        g_memory_window, (HMENU)IDC_EXPORT_BTN, NULL, NULL);
    SendMessage(hExport, WM_SETFONT, (WPARAM)g_mlFontNormal, TRUE);

    HWND hImport = CreateWindowExW(0, L"BUTTON", L"Import…",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        438, 393, 130, 28,
        g_memory_window, (HMENU)IDC_IMPORT_BTN, NULL, NULL);
    SendMessage(hImport, WM_SETFONT, (WPARAM)g_mlFontNormal, TRUE);

    // Info label (grey hint in the bottom bar)
    HWND hInfo = CreateWindowExW(0, L"STATIC",
        L"Double-click: Recall   ·   Ctrl+Click: Edit   ·   Alt+Click: Delete",
        WS_CHILD | WS_VISIBLE | SS_ENDELLIPSIS,
        12, 398, 410, 18,
        g_memory_window, (HMENU)IDC_INFO_LABEL, NULL, NULL);
    SendMessage(hInfo, WM_SETFONT, (WPARAM)g_mlFontSmall, TRUE);

    // Start auto-refresh timer (100ms interval, like macOS/Linux)
    g_refresh_timer = SetTimer(g_memory_window, IDM_TIMER_REFRESH, 100, NULL);

    std::cout << "Memory Locations window created" << std::endl;
}

// Show the Memory Locations window
void ShowWin32MemoryLocationsWindow() {
    std::cout << "ShowWin32MemoryLocationsWindow called" << std::endl;

    // Don't create the window without a loaded file: markers are tied to the
    // material, an empty window is just confusing.
    if (!FSTP_IsAnyPlayerActive()) {
        std::cout << "Memory Locations: no file loaded, window not shown" << std::endl;
        return;
    }

    // Initialize Memory Locations system
    FSTP_InitMemoryLocations();

    // Create window if it doesn't exist
    if (!g_memory_window) {
        CreateMemoryLocationsWindow();
    }

    // Dark title bar following the system theme — same trick as FSTPSettingsDialog.
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
    DwmSetWindowAttribute(g_memory_window, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */,
                          &darkMode, sizeof(darkMode));

    // Show window
    ShowWindow(g_memory_window, SW_SHOW);
    SetForegroundWindow(g_memory_window);

    // Refresh data
    RefreshMemoryLocationsTable();
}

// Hide the Memory Locations window
void HideWin32MemoryLocationsWindow() {
    if (g_memory_window) {
        ShowWindow(g_memory_window, SW_HIDE);
    }
}

// Toggle the Memory Locations window
void ToggleWin32MemoryLocationsWindow() {
    if (g_memory_window && IsWindowVisible(g_memory_window)) {
        HideWin32MemoryLocationsWindow();
    } else {
        ShowWin32MemoryLocationsWindow();
    }
}

// ===== ADD/EDIT MEMORY LOCATION DIALOG =====

// Dialog control IDs
#define IDC_LOC_ID_LABEL    3010
#define IDC_LOC_ID          3011
#define IDC_LOC_TC_LABEL    3012
#define IDC_LOC_TC          3013
#define IDC_LOC_NAME_LABEL  3014
#define IDC_LOC_NAME        3015
#define IDC_LOC_COMMENT_LBL 3016
#define IDC_LOC_COMMENT     3017
#define IDC_LOC_ZOOM_CHECK  3018
#define IDC_LOC_OK          IDOK
#define IDC_LOC_CANCEL      IDCANCEL

// Dialog state
struct AddEditDialogState {
    int player_id;
    int location_id;  // -1 = add mode
    double current_time;
    FSTP_MemoryLocationData data;
};

static AddEditDialogState g_edit_state;

// Add/Edit dialog procedure
static LRESULT CALLBACK AddEditDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT: {
            // White background + dark text under labels and edit fields — consistent with
            // Settings and the main window (no Win95 grey).
            HDC hdc = (HDC)wParam;
            SetBkColor(hdc, ML_CLR_BG);
            SetTextColor(hdc, ML_CLR_TEXT);
            if (!g_mlBgBrush) g_mlBgBrush = CreateSolidBrush(ML_CLR_BG);
            return (LRESULT)g_mlBgBrush;
        }

        case WM_COMMAND: {
            int id = LOWORD(wParam);

            if (id == IDC_LOC_OK) {
                bool is_add_mode = (g_edit_state.location_id == -1);

                // Get updated values
                wchar_t name_buf[256];
                GetDlgItemTextW(hwnd, IDC_LOC_NAME, name_buf, 256);
                std::string new_name = WideToUtf8(name_buf);

                wchar_t comment_buf[512];
                GetDlgItemTextW(hwnd, IDC_LOC_COMMENT, comment_buf, 512);
                std::string new_comments = WideToUtf8(comment_buf);

                if (is_add_mode) {
                    // Get number
                    int new_id = GetDlgItemInt(hwnd, IDC_LOC_ID, NULL, FALSE);
                    if (new_id <= 0) new_id = g_edit_state.data.id;

                    // Get timecode
                    wchar_t tc_buf[32];
                    GetDlgItemTextW(hwnd, IDC_LOC_TC, tc_buf, 32);
                    std::string tc_str = WideToUtf8(tc_buf);

                    // Recall zoom checkbox
                    bool recall_zoom = (IsDlgButtonChecked(hwnd, IDC_LOC_ZOOM_CHECK) == BST_CHECKED);

                    // Parse timecode
                    double fps = GetInstanceVideoFPS(g_edit_state.player_id);
                    if (fps <= 0) fps = 25.0;

                    int tc_hours = 0, tc_minutes = 0, tc_seconds = 0, tc_frames = 0;
                    if (sscanf(tc_str.c_str(), "%d:%d:%d:%d",
                               &tc_hours, &tc_minutes, &tc_seconds, &tc_frames) == 4) {
                        double timecode_seconds = tc_hours * 3600.0 + tc_minutes * 60.0 + tc_seconds;
                        timecode_seconds += tc_frames / fps;

                        if (FSTP_AddMemoryLocationWithZoom(
                                g_edit_state.player_id, new_id,
                                new_name.c_str(), new_comments.c_str(),
                                timecode_seconds, recall_zoom,
                                1.0f, 0.5f, 0.5f)) {
                            std::cout << "Memory Location added: " << new_name << std::endl;
                            RefreshMemoryLocationsTable();
                        } else {
                            std::cerr << "Failed to add Memory Location" << std::endl;
                        }
                    }
                } else {
                    // Edit mode
                    if (FSTP_UpdateMemoryLocation(g_edit_state.location_id,
                                                   new_name.c_str(), new_comments.c_str())) {
                        std::cout << "Memory Location #" << g_edit_state.location_id << " updated" << std::endl;
                        RefreshMemoryLocationsTable();
                    } else {
                        std::cerr << "Failed to update Memory Location #"
                                  << g_edit_state.location_id << std::endl;
                    }
                }

                DestroyWindow(hwnd);
                return 0;
            }

            if (id == IDC_LOC_CANCEL) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        }

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Unified Add/Edit Memory Location Dialog
static void ShowAddEditMemoryLocationDialog(int player_id, int location_id, double current_time) {
    bool is_add_mode = (location_id == -1);

    std::cout << (is_add_mode ? "Opening add dialog" : "Opening edit dialog")
              << " for Memory Location" << std::endl;

    // Setup state
    g_edit_state.player_id = player_id;
    g_edit_state.location_id = location_id;
    g_edit_state.current_time = current_time;

    if (!is_add_mode) {
        // Edit mode: find existing data
        bool found = false;
        int count = FSTP_GetMemoryLocationsCount();
        for (int i = 0; i < count; i++) {
            FSTP_MemoryLocationData temp;
            if (FSTP_GetMemoryLocationData(i, &temp) && temp.id == location_id) {
                g_edit_state.data = temp;
                found = true;
                break;
            }
        }
        if (!found) {
            std::cerr << "Memory Location #" << location_id << " not found" << std::endl;
            return;
        }
    } else {
        // Add mode: initialize with defaults
        g_edit_state.data.id = FSTP_GetMemoryLocationsCount() + 1;
        g_edit_state.data.timecode_seconds = current_time;
        g_edit_state.data.recall_zoom = false;
        g_edit_state.data.name[0] = '\0';
        g_edit_state.data.comments[0] = '\0';

        // Format timecode
        double fps = GetInstanceVideoFPS(player_id);
        if (fps <= 0) fps = 25.0;
        int hours = (int)(current_time) / 3600;
        int minutes = ((int)(current_time) % 3600) / 60;
        int seconds = (int)(current_time) % 60;
        int frames = (int)((current_time - floor(current_time)) * fps);
        snprintf(g_edit_state.data.timecode_display,
                 sizeof(g_edit_state.data.timecode_display),
                 "%02d:%02d:%02d:%02d", hours, minutes, seconds, frames);
    }

    // Register window class
    EnsureMLFontsAndBrush();
    static bool class_registered = false;
    if (!class_registered) {
        HINSTANCE hInst = GetModuleHandle(NULL);
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.lpfnWndProc = AddEditDialogProc;
        wc.hInstance = hInst;
        wc.lpszClassName = L"FSTPMemLocEditClass";
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = g_mlBgBrush;  // white, Settings style
        wc.hIcon   = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON),
                                       IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
        wc.hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON),
                                       IMAGE_ICON, 16, 16, 0);
        RegisterClassExW(&wc);
        class_registered = true;
    }

    // ── Layout: logical (96-DPI) px; every coordinate goes through MlDpi() so the geometry
    //    scales with the DPI-scaled Segoe UI fonts (mirrors FSTPSettingsDialog's Dpi() model). ──
    const int MARGIN    = 16;
    const int LABEL_X   = 16,  LABEL_W = 90;
    const int FIELD_X   = 112, FIELD_W = 384;
    const int ID_W      = 70,  TC_W    = 130;
    const int ROW       = 30;            // advance per single-line row
    const int EDIT_H    = 23,  LABEL_H = 20;
    const int COMMENT_H = 108;
    const int BTN_W     = 88,  BTN_H = 28, BTN_GAP = 8;
    const int CLIENT_W  = 512;

    // Walk the rows once (same sequence as the layout below) to get the content height —
    // it differs between add and edit mode — so the window is sized to fit exactly.
    int ly = MARGIN;
    ly += ROW;                    // Number / ID
    ly += ROW;                    // Timecode
    ly += ROW;                    // Name
    ly += COMMENT_H + 14;         // Comments (multiline)
    if (is_add_mode) ly += ROW;   // Recall-zoom checkbox
    ly += 10;                     // gap before the button row
    const int BUTTONS_Y = ly;
    const int CLIENT_H  = BUTTONS_Y + BTN_H + MARGIN;

    // Size the window to that client area (DPI-scaled) and centre it on the Memory Locations
    // window, or the screen if the dialog was opened from the keyboard before that window exists.
    const DWORD style   = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE;
    const DWORD exStyle = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;  // CONTROLPARENT: IsDialogMessageW does Tab
    RECT wr = { 0, 0, MlDpi(CLIENT_W), MlDpi(CLIENT_H) };
    AdjustWindowRectEx(&wr, style & ~WS_VISIBLE, FALSE, exStyle);
    int win_w = wr.right - wr.left;
    int win_h = wr.bottom - wr.top;

    RECT pr;
    if (!(g_memory_window && GetWindowRect(g_memory_window, &pr))) {
        pr.left = 0; pr.top = 0;
        pr.right  = GetSystemMetrics(SM_CXSCREEN);
        pr.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    int win_x = pr.left + ((pr.right - pr.left) - win_w) / 2;
    int win_y = pr.top  + ((pr.bottom - pr.top) - win_h) / 2;

    HWND hwnd = CreateWindowExW(
        exStyle, L"FSTPMemLocEditClass",
        is_add_mode ? L"New Memory Location" : L"Edit Memory Location",
        style, win_x, win_y, win_w, win_h,
        g_memory_window, NULL, GetModuleHandle(NULL), NULL);

    if (!hwnd) return;

    HFONT hFont = g_mlFontNormal;  // Segoe UI, not DEFAULT_GUI_FONT
    HWND h;
    ly = MARGIN;

    // Number/ID: label + editable number (add) or read-only value (edit)
    h = CreateWindowExW(0, L"STATIC", is_add_mode ? L"Number:" : L"ID:", WS_CHILD | WS_VISIBLE,
        MlDpi(LABEL_X), MlDpi(ly + 3), MlDpi(LABEL_W), MlDpi(LABEL_H),
        hwnd, (HMENU)IDC_LOC_ID_LABEL, NULL, NULL);
    SendMessage(h, WM_SETFONT, (WPARAM)hFont, TRUE);
    {
        wchar_t id_buf[16];
        swprintf(id_buf, 16, L"%d", g_edit_state.data.id);
        if (is_add_mode) {
            h = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", id_buf,
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER,
                MlDpi(FIELD_X), MlDpi(ly), MlDpi(ID_W), MlDpi(EDIT_H),
                hwnd, (HMENU)IDC_LOC_ID, NULL, NULL);
        } else {
            h = CreateWindowExW(0, L"STATIC", id_buf, WS_CHILD | WS_VISIBLE,
                MlDpi(FIELD_X), MlDpi(ly + 3), MlDpi(ID_W), MlDpi(LABEL_H),
                hwnd, (HMENU)IDC_LOC_ID, NULL, NULL);
        }
        SendMessage(h, WM_SETFONT, (WPARAM)hFont, TRUE);
    }
    ly += ROW;

    // Timecode
    h = CreateWindowExW(0, L"STATIC", L"Timecode:", WS_CHILD | WS_VISIBLE,
        MlDpi(LABEL_X), MlDpi(ly + 3), MlDpi(LABEL_W), MlDpi(LABEL_H),
        hwnd, (HMENU)IDC_LOC_TC_LABEL, NULL, NULL);
    SendMessage(h, WM_SETFONT, (WPARAM)hFont, TRUE);
    std::wstring tc_wide = Utf8ToWide(g_edit_state.data.timecode_display);
    if (is_add_mode) {
        h = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", tc_wide.c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            MlDpi(FIELD_X), MlDpi(ly), MlDpi(TC_W), MlDpi(EDIT_H),
            hwnd, (HMENU)IDC_LOC_TC, NULL, NULL);
    } else {
        h = CreateWindowExW(0, L"STATIC", tc_wide.c_str(), WS_CHILD | WS_VISIBLE,
            MlDpi(FIELD_X), MlDpi(ly + 3), MlDpi(TC_W), MlDpi(LABEL_H),
            hwnd, (HMENU)IDC_LOC_TC, NULL, NULL);
    }
    SendMessage(h, WM_SETFONT, (WPARAM)hFont, TRUE);
    ly += ROW;

    // Name
    h = CreateWindowExW(0, L"STATIC", L"Name:", WS_CHILD | WS_VISIBLE,
        MlDpi(LABEL_X), MlDpi(ly + 3), MlDpi(LABEL_W), MlDpi(LABEL_H),
        hwnd, (HMENU)IDC_LOC_NAME_LABEL, NULL, NULL);
    SendMessage(h, WM_SETFONT, (WPARAM)hFont, TRUE);
    std::wstring name_wide = Utf8ToWide(g_edit_state.data.name);
    h = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", name_wide.c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        MlDpi(FIELD_X), MlDpi(ly), MlDpi(FIELD_W), MlDpi(EDIT_H),
        hwnd, (HMENU)IDC_LOC_NAME, NULL, NULL);
    SendMessage(h, WM_SETFONT, (WPARAM)hFont, TRUE);
    ly += ROW;

    // Comments (multiline)
    h = CreateWindowExW(0, L"STATIC", L"Comments:", WS_CHILD | WS_VISIBLE,
        MlDpi(LABEL_X), MlDpi(ly + 3), MlDpi(LABEL_W), MlDpi(LABEL_H),
        hwnd, (HMENU)IDC_LOC_COMMENT_LBL, NULL, NULL);
    SendMessage(h, WM_SETFONT, (WPARAM)hFont, TRUE);
    std::wstring comments_wide = Utf8ToWide(g_edit_state.data.comments);
    h = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", comments_wide.c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_WANTRETURN | WS_VSCROLL,
        MlDpi(FIELD_X), MlDpi(ly), MlDpi(FIELD_W), MlDpi(COMMENT_H),
        hwnd, (HMENU)IDC_LOC_COMMENT, NULL, NULL);
    SendMessage(h, WM_SETFONT, (WPARAM)hFont, TRUE);
    ly += COMMENT_H + 14;

    // Recall zoom checkbox (add mode only)
    if (is_add_mode) {
        h = CreateWindowExW(0, L"BUTTON", L"Recall zoom settings",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            MlDpi(LABEL_X), MlDpi(ly), MlDpi(260), MlDpi(LABEL_H),
            hwnd, (HMENU)IDC_LOC_ZOOM_CHECK, NULL, NULL);
        SendMessage(h, WM_SETFONT, (WPARAM)hFont, TRUE);
        ly += ROW;
    }

    // Buttons — right-aligned on the button row
    const int cancel_x = CLIENT_W - MARGIN - BTN_W;
    const int ok_x     = cancel_x - BTN_GAP - BTN_W;
    h = CreateWindowExW(0, L"BUTTON", is_add_mode ? L"Add" : L"Save",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        MlDpi(ok_x), MlDpi(BUTTONS_Y), MlDpi(BTN_W), MlDpi(BTN_H),
        hwnd, (HMENU)IDC_LOC_OK, NULL, NULL);
    SendMessage(h, WM_SETFONT, (WPARAM)hFont, TRUE);

    h = CreateWindowExW(0, L"BUTTON", L"Cancel",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        MlDpi(cancel_x), MlDpi(BUTTONS_Y), MlDpi(BTN_W), MlDpi(BTN_H),
        hwnd, (HMENU)IDC_LOC_CANCEL, NULL, NULL);
    SendMessage(h, WM_SETFONT, (WPARAM)hFont, TRUE);

    // Focus name field
    SetFocus(GetDlgItem(hwnd, IDC_LOC_NAME));

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    // Modal keyboard navigation — the Settings dialog's mechanism: a nested GetMessageW loop that
    // routes messages through IsDialogMessageW, which gives Tab (move between fields), Enter
    // (default button = Add/Save; a newline inside the ES_WANTRETURN comments box, as expected) and
    // Esc (Cancel). Before this the dialog was modeless and none of those keys worked. The owner is
    // disabled so it's truly modal; the loop exits when the dialog is destroyed (OK/Cancel/close).
    HWND owner = g_memory_window;
    if (owner) EnableWindow(owner, FALSE);
    MSG msg;
    while (IsWindow(hwnd)) {
        if (!GetMessageW(&msg, nullptr, 0, 0)) {
            // WM_QUIT arrived while we're nested — re-post it so the app's outer loop quits too.
            PostQuitMessage((int)msg.wParam);
            break;
        }
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (owner) {
        EnableWindow(owner, TRUE);
        SetActiveWindow(owner);
    }
}

// C API: Show unified Add/Edit dialog for Memory Locations
void ShowWin32MemoryLocationDialog(int player_id, int location_id, double current_time) {
    ShowAddEditMemoryLocationDialog(player_id, location_id, current_time);
}

#else
// Stub implementation for non-Windows builds.
#endif
