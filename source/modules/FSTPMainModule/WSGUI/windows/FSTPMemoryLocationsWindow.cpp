#ifdef _WIN32

#include <windows.h>
#include <commctrl.h>
#include <iostream>
#include <string>
#include <cmath>
#include "../FSTPMemoryLocations.h"
#include "FSTPMemoryLocationsWindow.h"

#pragma comment(lib, "comctl32.lib")

#define IDI_APPICON 101

// Forward declarations
extern "C" int GetActivePlayerID();
extern "C" double GetInstancePosition(int player_id);
extern "C" double GetInstanceVideoFPS(int player_id);

// Global window state
static HWND g_memory_window = NULL;
static HWND g_listview = NULL;
static UINT_PTR g_refresh_timer = 0;

// Control IDs
#define IDC_LISTVIEW       2001
#define IDC_EXPORT_BTN     2002
#define IDC_INFO_LABEL     2003
#define IDM_TIMER_REFRESH  3001

// ListView columns
enum {
    COL_ID = 0,
    COL_TIMECODE,
    COL_NAME,
    NUM_COLS
};

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
            // Resize ListView to fill window (leaving space for bottom bar)
            if (g_listview) {
                SetWindowPos(g_listview, NULL, 0, 0, width, height - 40, SWP_NOZORDER);
            }
            // Reposition bottom bar controls
            HWND hExport = GetDlgItem(hwnd, IDC_EXPORT_BTN);
            if (hExport) {
                SetWindowPos(hExport, NULL, width - 140, height - 35, 130, 28, SWP_NOZORDER);
            }
            HWND hInfo = GetDlgItem(hwnd, IDC_INFO_LABEL);
            if (hInfo) {
                SetWindowPos(hInfo, NULL, 5, height - 30, width - 160, 20, SWP_NOZORDER);
            }
            return 0;
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
            if (LOWORD(wParam) == IDC_EXPORT_BTN && HIWORD(wParam) == BN_CLICKED) {
                OnExportToCSV();
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
            return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Create the Memory Locations window
static void CreateMemoryLocationsWindow() {
    if (g_memory_window) return;

    std::cout << "Creating Memory Locations window..." << std::endl;

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
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
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
        700, 400,
        NULL, NULL,
        GetModuleHandle(NULL), NULL
    );

    if (!g_memory_window) {
        std::cerr << "Failed to create Memory Locations window" << std::endl;
        return;
    }

    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    // Initialize Common Controls
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icex);

    // Create ListView
    g_listview = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        0, 0, 700, 360,
        g_memory_window,
        (HMENU)IDC_LISTVIEW,
        GetModuleHandle(NULL), NULL
    );
    SendMessage(g_listview, WM_SETFONT, (WPARAM)hFont, TRUE);
    ListView_SetExtendedListViewStyle(g_listview, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

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

    // Name column (auto-expand)
    lvc.cx = 500;
    lvc.pszText = (LPWSTR)L"Name";
    SendMessageW(g_listview, LVM_INSERTCOLUMNW, COL_NAME, (LPARAM)&lvc);

    // Bottom bar: Export button
    HWND hExport = CreateWindowExW(0, L"BUTTON", L"Export to CSV",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        560, 365, 130, 28,
        g_memory_window, (HMENU)IDC_EXPORT_BTN, NULL, NULL);
    SendMessage(hExport, WM_SETFONT, (WPARAM)hFont, TRUE);

    // Info label
    HWND hInfo = CreateWindowExW(0, L"STATIC",
        L"Double-click: Recall | Ctrl+Click: Edit | Alt+Click: Delete",
        WS_CHILD | WS_VISIBLE,
        5, 370, 550, 20,
        g_memory_window, (HMENU)IDC_INFO_LABEL, NULL, NULL);
    SendMessage(hInfo, WM_SETFONT, (WPARAM)hFont, TRUE);

    // Start auto-refresh timer (100ms interval, like macOS/Linux)
    g_refresh_timer = SetTimer(g_memory_window, IDM_TIMER_REFRESH, 100, NULL);

    std::cout << "Memory Locations window created" << std::endl;
}

// Show the Memory Locations window
void ShowWin32MemoryLocationsWindow() {
    std::cout << "ShowWin32MemoryLocationsWindow called" << std::endl;

    // Initialize Memory Locations system
    FSTP_InitMemoryLocations();

    // Create window if it doesn't exist
    if (!g_memory_window) {
        CreateMemoryLocationsWindow();
    }

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
    static bool class_registered = false;
    if (!class_registered) {
        HINSTANCE hInst = GetModuleHandle(NULL);
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.lpfnWndProc = AddEditDialogProc;
        wc.hInstance = hInst;
        wc.lpszClassName = L"FSTPMemLocEditClass";
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.hIcon   = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON),
                                       IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
        wc.hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON),
                                       IMAGE_ICON, 16, 16, 0);
        RegisterClassExW(&wc);
        class_registered = true;
    }

    // Create dialog window
    HWND hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        L"FSTPMemLocEditClass",
        is_add_mode ? L"New Memory Location" : L"Edit Memory Location",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT,
        500, 400,
        g_memory_window, NULL,
        GetModuleHandle(NULL), NULL
    );

    if (!hwnd) return;

    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    int y = 15;

    // Number/ID field
    HWND h = CreateWindowExW(0, L"STATIC", is_add_mode ? L"Number:" : L"ID:",
        WS_CHILD | WS_VISIBLE, 15, y, 80, 20, hwnd, (HMENU)IDC_LOC_ID_LABEL, NULL, NULL);
    SendMessage(h, WM_SETFONT, (WPARAM)hFont, TRUE);

    if (is_add_mode) {
        wchar_t id_buf[16];
        swprintf(id_buf, 16, L"%d", g_edit_state.data.id);
        h = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", id_buf,
            WS_CHILD | WS_VISIBLE | ES_NUMBER,
            100, y, 80, 22, hwnd, (HMENU)IDC_LOC_ID, NULL, NULL);
        SendMessage(h, WM_SETFONT, (WPARAM)hFont, TRUE);
    } else {
        wchar_t id_buf[16];
        swprintf(id_buf, 16, L"%d", g_edit_state.data.id);
        h = CreateWindowExW(0, L"STATIC", id_buf,
            WS_CHILD | WS_VISIBLE, 100, y, 80, 20, hwnd, (HMENU)IDC_LOC_ID, NULL, NULL);
        SendMessage(h, WM_SETFONT, (WPARAM)hFont, TRUE);
    }
    y += 30;

    // Timecode field
    h = CreateWindowExW(0, L"STATIC", L"Timecode:",
        WS_CHILD | WS_VISIBLE, 15, y, 80, 20, hwnd, (HMENU)IDC_LOC_TC_LABEL, NULL, NULL);
    SendMessage(h, WM_SETFONT, (WPARAM)hFont, TRUE);

    std::wstring tc_wide = Utf8ToWide(g_edit_state.data.timecode_display);
    if (is_add_mode) {
        h = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", tc_wide.c_str(),
            WS_CHILD | WS_VISIBLE,
            100, y, 120, 22, hwnd, (HMENU)IDC_LOC_TC, NULL, NULL);
        SendMessage(h, WM_SETFONT, (WPARAM)hFont, TRUE);
    } else {
        h = CreateWindowExW(0, L"STATIC", tc_wide.c_str(),
            WS_CHILD | WS_VISIBLE, 100, y, 120, 20, hwnd, (HMENU)IDC_LOC_TC, NULL, NULL);
        SendMessage(h, WM_SETFONT, (WPARAM)hFont, TRUE);
    }
    y += 30;

    // Name
    h = CreateWindowExW(0, L"STATIC", L"Name:",
        WS_CHILD | WS_VISIBLE, 15, y, 80, 20, hwnd, (HMENU)IDC_LOC_NAME_LABEL, NULL, NULL);
    SendMessage(h, WM_SETFONT, (WPARAM)hFont, TRUE);

    std::wstring name_wide = Utf8ToWide(g_edit_state.data.name);
    h = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", name_wide.c_str(),
        WS_CHILD | WS_VISIBLE,
        100, y, 370, 22, hwnd, (HMENU)IDC_LOC_NAME, NULL, NULL);
    SendMessage(h, WM_SETFONT, (WPARAM)hFont, TRUE);
    y += 30;

    // Comments
    h = CreateWindowExW(0, L"STATIC", L"Comments:",
        WS_CHILD | WS_VISIBLE, 15, y, 80, 20, hwnd, (HMENU)IDC_LOC_COMMENT_LBL, NULL, NULL);
    SendMessage(h, WM_SETFONT, (WPARAM)hFont, TRUE);

    std::wstring comments_wide = Utf8ToWide(g_edit_state.data.comments);
    h = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", comments_wide.c_str(),
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_WANTRETURN | WS_VSCROLL,
        100, y, 370, 120, hwnd, (HMENU)IDC_LOC_COMMENT, NULL, NULL);
    SendMessage(h, WM_SETFONT, (WPARAM)hFont, TRUE);
    y += 130;

    // Recall zoom checkbox (only in add mode)
    if (is_add_mode) {
        h = CreateWindowExW(0, L"BUTTON", L"Recall zoom settings",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            15, y, 250, 20, hwnd, (HMENU)IDC_LOC_ZOOM_CHECK, NULL, NULL);
        SendMessage(h, WM_SETFONT, (WPARAM)hFont, TRUE);
        y += 30;
    }

    // Buttons
    y = 330;
    h = CreateWindowExW(0, L"BUTTON", is_add_mode ? L"Add" : L"Save",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        290, y, 90, 28, hwnd, (HMENU)IDC_LOC_OK, NULL, NULL);
    SendMessage(h, WM_SETFONT, (WPARAM)hFont, TRUE);

    h = CreateWindowExW(0, L"BUTTON", L"Cancel",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        390, y, 90, 28, hwnd, (HMENU)IDC_LOC_CANCEL, NULL, NULL);
    SendMessage(h, WM_SETFONT, (WPARAM)hFont, TRUE);

    // Focus name field
    SetFocus(GetDlgItem(hwnd, IDC_LOC_NAME));

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
}

// C API: Show unified Add/Edit dialog for Memory Locations
void ShowWin32MemoryLocationDialog(int player_id, int location_id, double current_time) {
    ShowAddEditMemoryLocationDialog(player_id, location_id, current_time);
}

#else
// Stub implementation for non-Windows builds.
#endif
