#ifdef _WIN32
#include "FSTPMemoryLocationsWindow.h"
#include <commctrl.h>

FSTPMemoryLocationsWindow::FSTPMemoryLocationsWindow(HWND parentWindow)
    : hwndParent(parentWindow), hwndWindow(NULL), hwndList(NULL), isVisible(false) {
}

FSTPMemoryLocationsWindow::~FSTPMemoryLocationsWindow() {
    Hide();
}

void FSTPMemoryLocationsWindow::Show() {
    if (!hwndWindow) {
        // Register window class
        WNDCLASSEX wc = {0};
        wc.cbSize = sizeof(WNDCLASSEX);
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.lpszClassName = L"FSTPMemoryLocationsWindowClass";
        RegisterClassEx(&wc);

        // Create window
        hwndWindow = CreateWindowEx(
            0,
            L"FSTPMemoryLocationsWindowClass",
            L"Memory Locations",
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT,
            500, 400,
            hwndParent,
            NULL,
            GetModuleHandle(NULL),
            this
        );

        if (hwndWindow) {
            CreateControls();
            isVisible = true;
            ShowWindow(hwndWindow, SW_SHOW);
            UpdateWindow(hwndWindow);
        }
    }
}

void FSTPMemoryLocationsWindow::CreateControls() {
    // Initialize Common Controls
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icex);

    // Create ListView
    hwndList = CreateWindowEx(
        0,
        WC_LISTVIEW,
        L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT,
        0, 0, 0, 0,  // Will be sized in WM_SIZE
        hwndWindow,
        (HMENU)1,
        GetModuleHandle(NULL),
        NULL
    );

    // Add columns
    LVCOLUMN lvc;
    lvc.mask = LVCF_TEXT | LVCF_WIDTH;
    
    lvc.pszText = (LPWSTR)L"Address";
    lvc.cx = 100;
    ListView_InsertColumn(hwndList, 0, &lvc);
    
    lvc.pszText = (LPWSTR)L"Value";
    lvc.cx = 100;
    ListView_InsertColumn(hwndList, 1, &lvc);
    
    lvc.pszText = (LPWSTR)L"Description";
    lvc.cx = 200;
    ListView_InsertColumn(hwndList, 2, &lvc);

    PopulateList();
}

void FSTPMemoryLocationsWindow::PopulateList() {
    if (!hwndList) return;

    ListView_DeleteAllItems(hwndList);
    
    // TODO: Get actual memory locations from FSTPMemoryLocations
    // This is a placeholder implementation
    LVITEM lvi = {0};
    lvi.mask = LVIF_TEXT;
    lvi.iItem = 0;
    
    // Example item
    lvi.iSubItem = 0;
    lvi.pszText = (LPWSTR)L"0x1000";
    ListView_InsertItem(hwndList, &lvi);
    
    lvi.iSubItem = 1;
    lvi.pszText = (LPWSTR)L"0xFF";
    ListView_SetItem(hwndList, &lvi);
    
    lvi.iSubItem = 2;
    lvi.pszText = (LPWSTR)L"Example Memory Location";
    ListView_SetItem(hwndList, &lvi);
}

void FSTPMemoryLocationsWindow::Hide() {
    if (hwndWindow) {
        DestroyWindow(hwndWindow);
        hwndWindow = NULL;
        hwndList = NULL;
        isVisible = false;
    }
}

void FSTPMemoryLocationsWindow::Update() {
    if (isVisible) {
        PopulateList();
    }
}

bool FSTPMemoryLocationsWindow::IsVisible() const {
    return isVisible;
}

LRESULT CALLBACK FSTPMemoryLocationsWindow::WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_SIZE:
            if (HWND hwndList = GetDlgItem(hwnd, 1)) {
                SetWindowPos(hwndList, NULL,
                    0, 0, LOWORD(lParam), HIWORD(lParam),
                    SWP_NOZORDER);
            }
            return 0;

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            return 0;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}
#else
// Stub implementation for non-Windows builds.
#endif
