#ifdef _WIN32

#include <SDL.h>
#include <SDL_syswm.h>
#include <windows.h>
#include <mmsystem.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <portaudio.h>
#include <commctrl.h>
#include <cmath>
#include <atomic>
#include <thread>
#include <chrono>
#include <iostream>
#include <vector>
#include <string>
#include "../../main.h"
#include "../FSTPOSDSystem.h"
#include "../FSTPSettings.h"
#include "../../FSTPPlayerModule/FSTPPlayerManager.h"
#include "../FSTPWindowManager.h"
#include "../FSTPPixelBufferManager.h"
#include "../FSTPKeyboard.h"
#include "../FSTPMemoryLocations.h"
#include "../FSTPWelcomeScreen.h"
#include "FSTPWindowsWS.h"
#include "FSTPSettingsDialog.h"
#include "FSTPMemoryLocationsWindow.h"
#include "FSTPAboutDialog.h"
#include "../FSTPScreenshot.h"
#include "../FSTPZoom.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

// Debug control: set to true to enable verbose logging
static constexpr bool ENABLE_WIN_WS_DEBUG = false;

// Window system implementation for Windows (Win32 + SDL2)

// Rendering state (read by the render thread, written by the main thread — atomic to avoid a
// data race under the C++ memory model).
static std::atomic<bool> g_renderingActive{false};

// Dedicated render thread (Windows): moves the VSync-blocking SDL_RenderPresent OFF the event
// thread so input (seek / play / shuttle) is handled with no ~16ms present latency. Renderer
// access is serialised against window create/close by g_render_mutex inside FSTPWindowManager.
static std::thread g_renderThread;
static std::atomic<bool> g_renderThreadRunning{false};
// Set while the window is in a live move/resize loop (WM_ENTERSIZEMOVE..WM_EXITSIZEMOVE). D3D11 is
// far less forgiving than Metal about presenting during a swapchain resize, so the render thread
// pauses while this is true.
//
// CRITICAL: these WM_*SIZEMOVE messages are SENT (not posted), so Windows delivers them straight to
// the window's WndProc — they never appear in the thread message queue that PeekMessage/SDL's pump
// drain. So we observe them via an HWND SUBCLASS (SetWindowSubclass below), which IS invoked for
// sent messages. The old code toggled this flag from the PeekMessage loop, where the messages never
// arrive → the flag stayed false → the render thread kept calling SDL_RenderPresent while SDL
// resized the D3D11 swapchain on the event thread → concurrent ResizeBuffers+Present on the
// non-thread-safe D3D11 context → 0xC0000005 crash on resize (macOS/Metal tolerates this; D3D11
// does not — a classic macOS-first-port gap).
static std::atomic<bool> g_isLiveResizing{false};

// Set by the render thread once it has OBSERVED g_isLiveResizing and parked itself (i.e. it is NOT
// inside SDL_RenderPresent). The resize-enter handler waits for this ack before letting the modal
// resize loop proceed, so an in-flight present can't overlap the swapchain resize.
static std::atomic<bool> g_renderPausedAck{false};

// Asynchronous shutdown variables (same as Linux/macOS)
static std::atomic<bool> g_shutdownRequested{false};
static std::atomic<bool> g_shutdownComplete{false};
static std::atomic<bool> g_cleanupThreadStarted{false};

// Dialog state (atomic — accessed from main thread and dialog threads)
std::atomic<bool> g_dialog_open{false};

// Initial file to load from command line
static const char* g_initial_file_to_load = nullptr;

// Windows-specific: stored HWND for Win32 dialogs
static HWND g_main_hwnd = NULL;

// Autonomous rendering function - works independently of events (same as Linux/macOS)
void AutoRenderFrame() {
    if (!g_renderingActive) return;

    // FIRST: Render all active windows through WindowManager
    RenderAllWindows();

    // SECOND: Update OSD data for all active windows with real player data
    for (int i = 0; i < MAX_WINDOWS; i++) {
        FSTPWindow* window = GetWindowByIndex(i);
        if (window && window->is_active) {
            int player_id = window->player_instance_id;

            if (player_id >= 0 && IsPlayerInstanceActive(player_id)) {
                // Get real player data
                bool is_playing = IsInstancePlaying(player_id);
                double current_position = GetInstancePosition(player_id);
                double duration = GetInstanceDuration(player_id);
                double speed = GetInstanceSpeed(player_id);
                double actual_speed = GetInstanceActualSpeed(player_id);
                bool is_reverse = IsInstanceReverse(player_id);

                // Get audio signal levels
                float audio_left = GetInstanceAudioLevelLeft(player_id);
                float audio_right = GetInstanceAudioLevelRight(player_id);
                float peak_left = GetInstanceAudioPeakLeft(player_id);
                float peak_right = GetInstanceAudioPeakRight(player_id);

                // Update OSD for window
                UpdateWindowOSD(i, current_position, duration, is_playing, speed, is_reverse);
                UpdateOSDActualSpeed(player_id, actual_speed);
                UpdateOSDAudioLevels(player_id, audio_left, audio_right, peak_left, peak_right);
            } else {
                // Even without loaded file, update OSD to show NO_FILE state
                UpdateWindowOSD(i, 0.0, 0.0, false, 1.0, false);
            }
        }
    }
}

// Start the dedicated render thread. VSync is enabled, so SDL_RenderPresent inside AutoRenderFrame
// blocks on this thread until the next vblank — pacing us to the display refresh WITHOUT stalling
// the event loop. Renderer/window lifetime is serialised via g_render_mutex in the window manager,
// so create/close on the event thread cannot race this thread's rendering.
void StartAutonomousRendering() {
    if (g_renderThreadRunning.load()) return;
    g_renderingActive = true;
    g_renderThreadRunning = true;

    g_renderThread = std::thread([]() {
        // Raise the render thread's priority so the OS scheduler doesn't let other
        // threads preempt it between vblanks. macOS runs this thread at
        // QOS_CLASS_USER_INTERACTIVE (FSTPDarwinWS.mm) for <0.5ms jitter; on Windows
        // the plain std::thread ran at NORMAL priority, so during mouse-shuttle the
        // frame that reflects the new scrub position could be delayed behind other
        // work — the UI lagged the cursor. TIME_CRITICAL matches macOS's intent
        // (kernel32 only, no extra link deps like avrt/MMCSS).
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

        std::cout << "[RENDER THREAD] Windows render thread started (VSync-paced, TIME_CRITICAL)" << std::endl;
        while (g_renderThreadRunning.load()) {
            // Pause presenting during a live window move/resize — D3D11 swapchain resize and a
            // concurrent Present don't mix. The event thread drives the resize; we idle briefly.
            // Publish g_renderPausedAck so the resize-enter handler knows we're parked OUTSIDE
            // SDL_RenderPresent before it lets the swapchain resize begin.
            if (g_isLiveResizing.load()) {
                g_renderPausedAck.store(true);
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
                continue;
            }
            g_renderPausedAck.store(false);
            // AutoRenderFrame → RenderAllWindows takes g_render_mutex and (with VSync on) blocks in
            // SDL_RenderPresent until vblank, so this loop self-paces to the refresh rate.
            AutoRenderFrame();
        }
        std::cout << "[RENDER THREAD] Windows render thread stopped" << std::endl;
    });
}

// Stop the render thread and join it. MUST be called before tearing down windows / SDL so the
// render thread is guaranteed not to touch the renderer during shutdown.
void StopAutonomousRendering() {
    g_renderThreadRunning = false;
    if (g_renderThread.joinable()) {
        g_renderThread.join();
        std::cout << "[RENDER THREAD] Windows render thread joined" << std::endl;
    }
    g_renderingActive = false;
}

// HWND subclass that brackets the live move/resize loop. Runs INSIDE the window's WndProc, so it
// sees WM_ENTERSIZEMOVE/WM_EXITSIZEMOVE (sent messages the PeekMessage pump never receives) as well
// as the maximize/snap path (WM_SIZE with SIZE_MAXIMIZED/RESTORED, which have no ENTERSIZEMOVE
// bracket). While bracketed, g_isLiveResizing pauses the render thread so it can't Present into a
// swapchain SDL is resizing on this same thread.
static LRESULT CALLBACK ResizeSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                           UINT_PTR /*uIdSubclass*/, DWORD_PTR /*dwRefData*/) {
    switch (msg) {
        case WM_ENTERSIZEMOVE:
            g_isLiveResizing.store(true);
            // Wait (bounded) until the render thread confirms it has parked OUTSIDE
            // SDL_RenderPresent, so no present overlaps the swapchain resize about to start.
            for (int i = 0; i < 100 && !g_renderPausedAck.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            break;
        case WM_EXITSIZEMOVE:
            g_isLiveResizing.store(false);
            break;
        case WM_SIZE:
            // Maximize / restore / snap change size WITHOUT an ENTERSIZEMOVE..EXITSIZEMOVE bracket.
            // Pause for this single message so the implicit swapchain resize inside DefWindowProc
            // doesn't race the render thread. (A live drag sends many WM_SIZE inside the bracket;
            // those are already covered by g_isLiveResizing staying true across them.)
            if (!g_isLiveResizing.load() &&
                (wParam == SIZE_MAXIMIZED || wParam == SIZE_RESTORED)) {
                g_isLiveResizing.store(true);
                for (int i = 0; i < 100 && !g_renderPausedAck.load(); ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                LRESULT r = DefSubclassProc(hwnd, msg, wParam, lParam);
                g_isLiveResizing.store(false);
                return r;
            }
            break;
        default:
            break;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

// Install the resize subclass on the SDL window's HWND. Safe to call once after the HWND exists.
static void InstallResizeSubclass(HWND hwnd) {
    if (!hwnd) return;
    if (SetWindowSubclass(hwnd, ResizeSubclassProc, 1 /*id*/, 0 /*refdata*/)) {
        std::cout << "[RESIZE] Installed live-resize subclass — render thread will pause during "
                     "swapchain resize (fixes D3D11 resize crash)" << std::endl;
    } else {
        std::cout << "[RESIZE] WARNING: SetWindowSubclass failed; resize crash guard inactive"
                  << std::endl;
    }
}

// RAII guard to ensure dialog flag is always reset
class DialogGuard {
public:
    DialogGuard() {
        g_dialog_open = true;
    }
    ~DialogGuard() {
        g_dialog_open = false;
        std::cout << "Dialog closed (flag reset)" << std::endl;
    }
};

// Helper: Get HWND from SDL window
static HWND GetHWNDFromSDLWindow(SDL_Window* sdl_window) {
    SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);
    if (SDL_GetWindowWMInfo(sdl_window, &wmInfo)) {
        return wmInfo.info.win.window;
    }
    return NULL;
}

// Install the D3D11 live-resize crash guard on ANY window. Called by the shared CreateNewWindow
// (FSTPWindowManager.cpp) for EVERY window it creates — main, menu (IDM_NEW_WINDOW /
// IDM_OPEN_NEW_INST) and the Cmd+N keyboard path all funnel through there. The render thread is
// process-global, so a window without the subclass would still let a Present race a swapchain
// resize and crash; installing at the single creation point covers all paths at once. (macOS gets
// the same "all windows" coverage from one global NSNotification observer in FSTPDarwinWS.mm.)
void FSTP_InstallWindowResizeGuard(SDL_Window* win) {
    if (!win) return;
    InstallResizeSubclass(GetHWNDFromSDLWindow(win));
}

// Native file dialog for Windows using IFileOpenDialog (COM)
void ShowNativeFileDialog(int target_player_id) {
    std::cout << "ShowNativeFileDialog called with target_player_id=" << target_player_id << std::endl;

    // Prevent multiple dialog invocations
    if (g_dialog_open) {
        std::cout << "Dialog already open, skipping call" << std::endl;
        return;
    }

    // RAII guard - automatically resets flag when function exits
    DialogGuard guard;
    std::cout << "Opening file dialog..." << std::endl;

    // Initialize COM for IFileOpenDialog
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) {
        std::cerr << "COM initialization failed" << std::endl;
        return;
    }

    IFileOpenDialog* pFileOpen = nullptr;
    hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_ALL,
                          IID_IFileOpenDialog, reinterpret_cast<void**>(&pFileOpen));

    if (FAILED(hr)) {
        std::cerr << "Failed to create IFileOpenDialog" << std::endl;
        CoUninitialize();
        return;
    }

    // Set file type filters
    COMDLG_FILTERSPEC fileTypes[] = {
        { L"Video Files", L"*.mp4;*.mov;*.avi;*.mkv;*.m4v;*.webm;*.mxf;*.ts" },
        { L"Audio Files", L"*.mp3;*.wav;*.flac;*.aac;*.m4a;*.ogg" },
        { L"All Files",   L"*.*" }
    };
    pFileOpen->SetFileTypes(ARRAYSIZE(fileTypes), fileTypes);
    pFileOpen->SetTitle(L"Open File");

    // Show the dialog
    std::cout << "Running file dialog..." << std::endl;
    hr = pFileOpen->Show(g_main_hwnd);

    if (SUCCEEDED(hr)) {
        IShellItem* pItem = nullptr;
        hr = pFileOpen->GetResult(&pItem);

        if (SUCCEEDED(hr)) {
            PWSTR pszFilePath = nullptr;
            hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath);

            if (SUCCEEDED(hr) && pszFilePath) {
                // Convert wide string to UTF-8
                int utf8_len = WideCharToMultiByte(CP_UTF8, 0, pszFilePath, -1, NULL, 0, NULL, NULL);
                char* filename = new char[utf8_len];
                WideCharToMultiByte(CP_UTF8, 0, pszFilePath, -1, filename, utf8_len, NULL, NULL);

                std::cout << "Selected file: " << filename << std::endl;

                // Determine player_id based on parameter
                int active_player_id;
                if (target_player_id >= 0) {
                    active_player_id = target_player_id;
                    std::cout << "Opening file for specific player instance: " << target_player_id << std::endl;
                } else {
                    active_player_id = GetActivePlayerID();
                    std::cout << "Opening file for active player: " << active_player_id << std::endl;
                }

                // Set initial loading state IMMEDIATELY
                UpdateOSDPosition(active_player_id, 0.0, 0.0);
                UpdateOSDPlayState(active_player_id, false, false, false);
                SetPlayerLoadingState(active_player_id, true);
                UpdateOSDDisplayMode(active_player_id, OSD_MODE_LOADING);
                SetPlayerLoadingProgress(active_player_id, 0);
                SetPlayerLoadingStatus(active_player_id, "threading");

                // Clear old video texture
                FSTPPixelBufferManager* pixel_mgr = GetPixelBufferManager();
                if (pixel_mgr) {
                    pixel_mgr->ClearPlayerBuffers(active_player_id);
                }

                // Load file asynchronously
                std::thread([filename, active_player_id]() {
                    std::cout << "Loading file: " << filename << std::endl;

                    int instance_id = -1;

                    // Check if player instance is already active
                    if (active_player_id >= 0 && IsPlayerInstanceActive(active_player_id)) {
                        std::cout << "Loading file into existing player instance " << active_player_id << std::endl;
                        instance_id = LoadFileIntoPlayerInstance(filename, active_player_id);
                    } else {
                        std::cout << "Creating new player instance for player " << active_player_id << std::endl;
                        instance_id = CreatePlayerInstance(filename, active_player_id);
                    }

                    if (instance_id >= 0) {
                        std::cout << "File successfully loaded into player instance " << instance_id << std::endl;

                        // CRITICAL: Stop loading mode and switch to NORMAL
                        SetPlayerLoadingState(active_player_id, false);
                        UpdateOSDDisplayMode(active_player_id, OSD_MODE_NORMAL);
                        std::cout << "Loading complete, switched to NORMAL mode" << std::endl;

                        // Find window index for OSD update
                        int window_index = -1;
                        for (int i = 0; i < MAX_WINDOWS; i++) {
                            FSTPWindow* window = GetWindowByIndex(i);
                            if (window && window->player_instance_id == active_player_id) {
                                window_index = i;
                                break;
                            }
                        }

                        if (window_index >= 0) {
                            double duration = GetInstanceDuration(instance_id);
                            UpdateWindowOSD(window_index, 0.0, duration, false, 1.0, false);
                            std::cout << "OSD updated for window " << window_index
                                     << " (player " << instance_id << "), duration: "
                                     << duration << " sec" << std::endl;
                        }
                    } else {
                        std::cout << "File loading error: " << instance_id << std::endl;
                        SetPlayerLoadingState(active_player_id, false);
                        UpdateOSDDisplayMode(active_player_id, OSD_MODE_NO_FILE);
                    }

                    delete[] filename;
                }).detach();

                CoTaskMemFree(pszFilePath);
            }
            pItem->Release();
        }
    }

    pFileOpen->Release();
    CoUninitialize();
    std::cout << "File dialog closed" << std::endl;
}

// Load file directly from path (for command line arguments)
void LoadFileFromPath(const char* filepath, int target_player_id) {
    std::cout << "LoadFileFromPath called: " << filepath << std::endl;

    if (!filepath) {
        std::cerr << "No filepath provided" << std::endl;
        return;
    }

    // Determine player_id based on parameter
    int active_player_id;
    if (target_player_id >= 0) {
        active_player_id = target_player_id;
        std::cout << "Loading file for specific player instance: " << target_player_id << std::endl;
    } else {
        active_player_id = GetActivePlayerID();
        std::cout << "Loading file for active player: " << active_player_id << std::endl;
    }

    // Set initial loading state
    UpdateOSDPosition(active_player_id, 0.0, 0.0);
    UpdateOSDPlayState(active_player_id, false, false, false);
    SetPlayerLoadingState(active_player_id, true);
    UpdateOSDDisplayMode(active_player_id, OSD_MODE_LOADING);
    SetPlayerLoadingProgress(active_player_id, 0);
    SetPlayerLoadingStatus(active_player_id, "threading");

    // Clear old video texture
    FSTPPixelBufferManager* pixel_mgr = GetPixelBufferManager();
    if (pixel_mgr) {
        pixel_mgr->ClearPlayerBuffers(active_player_id);
    }

    // Copy filepath to heap for thread
    char* filename_copy = _strdup(filepath);

    // Load file asynchronously
    std::thread([filename_copy, active_player_id]() {
        std::cout << "Loading file from command line: " << filename_copy << std::endl;

        int instance_id = -1;

        // Check if player instance is already active
        if (active_player_id >= 0 && IsPlayerInstanceActive(active_player_id)) {
            std::cout << "Loading file into existing player instance " << active_player_id << std::endl;
            instance_id = LoadFileIntoPlayerInstance(filename_copy, active_player_id);
        } else {
            std::cout << "Creating new player instance for player " << active_player_id << std::endl;
            instance_id = CreatePlayerInstance(filename_copy, active_player_id);
        }

        if (instance_id >= 0) {
            std::cout << "File successfully loaded into player instance " << instance_id << std::endl;

            // Stop loading mode and switch to NORMAL
            SetPlayerLoadingState(active_player_id, false);
            UpdateOSDDisplayMode(active_player_id, OSD_MODE_NORMAL);
            std::cout << "Loading complete, switched to NORMAL mode" << std::endl;

            // Find window index for OSD update
            int window_index = -1;
            for (int i = 0; i < MAX_WINDOWS; i++) {
                FSTPWindow* window = GetWindowByIndex(i);
                if (window && window->player_instance_id == active_player_id) {
                    window_index = i;
                    break;
                }
            }

            if (window_index >= 0) {
                double duration = GetInstanceDuration(instance_id);
                UpdateWindowOSD(window_index, 0.0, duration, false, 1.0, false);
                std::cout << "OSD updated for window " << window_index
                         << " (player " << instance_id << "), duration: "
                         << duration << " sec" << std::endl;
            }
        } else {
            std::cout << "File loading error: " << instance_id << std::endl;
            SetPlayerLoadingState(active_player_id, false);
            UpdateOSDDisplayMode(active_player_id, OSD_MODE_NO_FILE);
        }

        free(filename_copy);
    }).detach();
}

// Forward declarations for helper functions
extern "C" double GetInstanceVideoFPS(int player_id);

// Win32 Context Menu (right-click menu using TrackPopupMenu)
#define IDM_OPEN_FILE       40001
#define IDM_SCREENSHOT      40002
#define IDM_MEMORY_LOCS     40003
#define IDM_SETTINGS        40004
#define IDM_ABOUT           40005
#define IDM_NEW_WINDOW      40006
#define IDM_OPEN_NEW_INST   40007
#define IDM_CLEAR_RECENT    40008
// Recent-file entries occupy a contiguous command range: IDM_RECENT_BASE + i
// selects the i-th recent file. Keep this above the fixed IDs and wide enough
// for FSTP_RECENT_MAX (12) entries.
#define IDM_RECENT_BASE     40100
#define IDM_RECENT_MAX      40199

// Build the "Open Recent" submenu from the persisted MRU list. Returns a popup
// HMENU owned by the caller's menu (freed when the parent menu is destroyed),
// or NULL if there are no recent files. Labels show just the filename; the full
// path is recovered from the recent list by index when the command fires.
static HMENU BuildRecentSubmenu() {
    int count = GetRecentFileCount();
    if (count <= 0) return NULL;

    HMENU hSub = CreatePopupMenu();
    if (!hSub) return NULL;

    for (int i = 0; i < count && i < (IDM_RECENT_MAX - IDM_RECENT_BASE); i++) {
        const char* path = GetRecentFile(i);
        if (!path) continue;
        // Show the filename (basename), not the whole path, to keep the menu tidy.
        std::string p(path);
        size_t slash = p.find_last_of("/\\");
        std::string name = (slash == std::string::npos) ? p : p.substr(slash + 1);

        int wlen = MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, NULL, 0);
        std::wstring wname(wlen > 0 ? wlen - 1 : 0, L'\0');
        if (wlen > 0) MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, &wname[0], wlen);

        AppendMenuW(hSub, MF_STRING, IDM_RECENT_BASE + i, wname.c_str());
    }
    AppendMenuW(hSub, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hSub, MF_STRING, IDM_CLEAR_RECENT, L"Clear Recent");
    return hSub;
}

void ShowWin32ContextMenu() {
    std::cout << "ShowWin32ContextMenu called" << std::endl;

    if (!g_main_hwnd) {
        std::cerr << "No HWND available for context menu" << std::endl;
        return;
    }

    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    AppendMenuW(hMenu, MF_STRING, IDM_OPEN_FILE,     L"Open File...\tCtrl+O");
    AppendMenuW(hMenu, MF_STRING, IDM_OPEN_NEW_INST, L"Open in New Instance...");
    // Open Recent submenu (grayed out when there is no history yet).
    HMENU hRecent = BuildRecentSubmenu();
    if (hRecent) {
        AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hRecent, L"Open Recent");
    } else {
        AppendMenuW(hMenu, MF_STRING | MF_GRAYED, 0, L"Open Recent");
    }
    AppendMenuW(hMenu, MF_STRING, IDM_NEW_WINDOW,    L"New Window\tCtrl+Shift+N");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, IDM_SCREENSHOT,    L"Copy Screenshot\tCtrl+C");
    AppendMenuW(hMenu, MF_STRING, IDM_MEMORY_LOCS,   L"Memory Locations");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, IDM_SETTINGS,      L"Settings...\tCtrl+,");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, IDM_ABOUT,         L"About TapeXPlayer");

    // Get cursor position
    POINT pt;
    GetCursorPos(&pt);

    // Show menu
    UINT cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                               pt.x, pt.y, 0, g_main_hwnd, NULL);

    DestroyMenu(hMenu);

    // Recent-file entries: load the selected path into the active player.
    if (cmd >= IDM_RECENT_BASE && cmd <= IDM_RECENT_MAX) {
        const char* path = GetRecentFile(cmd - IDM_RECENT_BASE);
        if (path) {
            // Copy before loading: LoadFileFromPath is async and AddRecentFile
            // (called on load) mutates the list, invalidating this pointer.
            std::string path_copy(path);
            LoadFileFromPath(path_copy.c_str(), -1);
        }
        std::cout << "Context menu closed" << std::endl;
        return;
    }

    // Handle menu command
    switch (cmd) {
        case IDM_OPEN_FILE:
            ShowNativeFileDialog(-1);
            break;
        case IDM_CLEAR_RECENT:
            ClearRecentFiles();
            break;
        case IDM_NEW_WINDOW: {
            int active_count = 0;
            for (int i = 0; i < MAX_WINDOWS; i++) {
                FSTPWindow* w = GetWindowByIndex(i);
                if (w && w->is_active) active_count++;
            }
            char title[128];
            snprintf(title, sizeof(title), "TapeXPlayer 2026 - Player %d", active_count);
            int idx = CreateNewWindow(title, 1280, 720);
            if (idx >= 0) {
                std::cout << "New window created with index " << idx << std::endl;
            } else {
                std::cerr << "Failed to create new window: error " << idx << std::endl;
            }
            break;
        }
        case IDM_OPEN_NEW_INST: {
            int active_count = 0;
            for (int i = 0; i < MAX_WINDOWS; i++) {
                FSTPWindow* w = GetWindowByIndex(i);
                if (w && w->is_active) active_count++;
            }
            char title[128];
            snprintf(title, sizeof(title), "TapeXPlayer 2026 - Player %d", active_count);
            int idx = CreateNewWindow(title, 1280, 720);
            if (idx >= 0) {
                FSTPWindow* new_win = GetWindowByIndex(idx);
                int player_id = new_win ? new_win->player_instance_id : -1;
                ShowNativeFileDialog(player_id);
            }
            break;
        }
        case IDM_SCREENSHOT:
            CopyScreenshotToClipboard();
            break;
        case IDM_MEMORY_LOCS:
            ShowWin32MemoryLocationsWindow();
            break;
        case IDM_SETTINGS:
            ShowWin32SettingsDialog();
            break;
        case IDM_ABOUT:
            ShowWin32AboutDialog();
            break;
    }

    std::cout << "Context menu closed" << std::endl;
}

// Implementation of copy screenshot to clipboard function for Windows
void CopyScreenshotToClipboard() {
    std::cout << "Copying screenshot to clipboard..." << std::endl;

    // Get active player
    int active_player = GetActivePlayerID();
    if (active_player < 0) {
        std::cerr << "No active player for screenshot" << std::endl;
        return;
    }

    // Get current pixel buffer
    FSTPPixelBufferManager* manager = GetPixelBufferManager();
    if (!manager) {
        std::cerr << "Pixel buffer manager not available" << std::endl;
        return;
    }

    const FSTPPixelBufferManager::PixelBuffer* pixel_buffer =
        manager->GetPixelBuffer(active_player);

    if (!pixel_buffer || !pixel_buffer->is_valid || !pixel_buffer->av_frame) {
        std::cerr << "No valid frame for screenshot" << std::endl;
        return;
    }

    int width = pixel_buffer->width;
    int height = pixel_buffer->height;
    double current_time = GetInstancePosition(active_player);

    // Get actual video FPS
    double video_fps = GetInstanceVideoFPS(active_player);
    if (video_fps <= 0) video_fps = 25.0;

    // Format timecode (HH:MM:SS:FF)
    int hours = (int)(current_time / 3600);
    int minutes = (int)((current_time - hours * 3600) / 60);
    int seconds = (int)(current_time - hours * 3600 - minutes * 60);
    int frames = (int)((current_time - (int)current_time) * video_fps);

    char timecode_buf[32];
    snprintf(timecode_buf, sizeof(timecode_buf), "%02d:%02d:%02d:%02d", hours, minutes, seconds, frames);
    std::string timecode(timecode_buf);

    // Determine pixel format
    enum AVPixelFormat pix_fmt = (enum AVPixelFormat)pixel_buffer->av_frame->format;
    bool is_nv12 = (pix_fmt == AV_PIX_FMT_NV12);

    // Validate av_frame data before copying
    if (!pixel_buffer->av_frame->data[0] || !pixel_buffer->av_frame->data[1]) {
        std::cerr << "Screenshot failed: invalid av_frame data pointers" << std::endl;
        return;
    }

    // For YUV420P check V plane
    if (!is_nv12 && !pixel_buffer->av_frame->data[2]) {
        std::cerr << "Screenshot failed: YUV420P format but V plane is null" << std::endl;
        return;
    }

    std::cout << "Screenshot format: " << (is_nv12 ? "NV12 (semi-planar)" : "YUV420P (planar)") << std::endl;

    // Get window index for active player
    int window_idx = -1;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        FSTPWindow* window = GetWindowByIndex(i);
        if (window && window->is_active && window->player_instance_id == active_player) {
            window_idx = i;
            break;
        }
    }

    // Get zoom state
    FSTPZoomState* zoom = nullptr;
    int window_width = width;
    int window_height = height;

    if (window_idx >= 0) {
        zoom = GetZoomState(window_idx);

        FSTPWindow* window = GetWindowByIndex(window_idx);
        if (window && window->window) {
            SDL_GetWindowSize(window->window, &window_width, &window_height);
        }
    }

    // Create temporary contiguous buffer for screenshot
    int y_size = width * height;
    int uv_size = (width / 2) * (height / 2);
    std::vector<uint8_t> temp_yuv_buffer(y_size + uv_size * 2);

    uint8_t* dst_y = temp_yuv_buffer.data();
    uint8_t* dst_u = dst_y + y_size;
    uint8_t* dst_v = dst_u + uv_size;

    // Copy Y plane
    for (int row = 0; row < height; row++) {
        memcpy(dst_y + row * width,
               pixel_buffer->av_frame->data[0] + row * pixel_buffer->av_frame->linesize[0],
               width);
    }

    // Copy U and V planes (NV12 vs YUV420P handling)
    int uv_height = height / 2;
    int uv_width = width / 2;

    if (is_nv12) {
        // NV12: UV interleaved (UVUVUVUV...) in data[1]
        for (int row = 0; row < uv_height; row++) {
            const uint8_t* src_uv = pixel_buffer->av_frame->data[1] +
                                    row * pixel_buffer->av_frame->linesize[1];
            uint8_t* row_dst_u = dst_u + row * uv_width;
            uint8_t* row_dst_v = dst_v + row * uv_width;

            for (int col = 0; col < uv_width; col++) {
                row_dst_u[col] = src_uv[col * 2];     // U (even bytes)
                row_dst_v[col] = src_uv[col * 2 + 1]; // V (odd bytes)
            }
        }
    } else {
        // YUV420P: U and V separate planes
        for (int row = 0; row < uv_height; row++) {
            memcpy(dst_u + row * uv_width,
                   pixel_buffer->av_frame->data[1] + row * pixel_buffer->av_frame->linesize[1],
                   uv_width);
            memcpy(dst_v + row * uv_width,
                   pixel_buffer->av_frame->data[2] + row * pixel_buffer->av_frame->linesize[2],
                   uv_width);
        }
    }

    // For screenshots ALWAYS show thumbnail if zoom is active
    bool show_thumb = zoom && zoom->enabled && zoom->factor > 1.0f;

    bool success = TakeScreenshotFromPixelBuffer(
        temp_yuv_buffer.data(),
        width,
        height,
        timecode,
        window_width,
        window_height,
        zoom ? zoom->enabled : false,
        zoom ? zoom->factor : 1.0f,
        zoom ? zoom->center_x : 0.5f,
        zoom ? zoom->center_y : 0.5f,
        show_thumb
    );

    if (success) {
        std::cout << "Screenshot copied to clipboard (" << width << "x" << height << ")" << std::endl;
    } else {
        std::cerr << "Failed to copy screenshot to clipboard" << std::endl;
    }
}

// Set initial file to load from command line
void SetInitialFileToLoad(const char* filepath) {
    g_initial_file_to_load = filepath;
    std::cout << "Initial file to load set: " << filepath << std::endl;
}

// Main UI loop for Windows - full implementation (based on Linux version)
int RunMainUILoop() {
    std::cout << "Starting Windows/SDL2 UI main loop..." << std::endl;

    // SDL hints for optimal rendering
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");  // Bilinear filtering
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "1");           // Enable VSync

    // Allow system screen saver and display sleep to work normally
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");

    // Initialize SDL
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        std::cerr << "SDL initialization error: " << SDL_GetError() << std::endl;
        return -1;
    }

    // SDL disables the screensaver by default - re-enable it so the display can sleep normally
    SDL_EnableScreenSaver();

    std::cout << "SDL2 initialized successfully" << std::endl;

    // Initialize window manager
    if (InitWindowManager() != 0) {
        std::cerr << "Window manager initialization error" << std::endl;
        SDL_Quit();
        return -1;
    }

    std::cout << "Window manager initialized" << std::endl;

    // Create main window (automatically bound to player #0)
    int main_window_index = CreateNewWindow("TapeXPlayer 2026 - Player 0", 1280, 720);
    if (main_window_index < 0) {
        std::cerr << "Main window creation error" << std::endl;
        ShutdownWindowManager();
        SDL_Quit();
        return -1;
    }

    // Get main window for OSD system
    FSTPWindow* main_window = GetMainWindow();
    if (main_window == nullptr) {
        std::cerr << "Error getting main window" << std::endl;
        ShutdownWindowManager();
        SDL_Quit();
        return -1;
    }

    std::cout << "Main window created (ID: " << main_window_index << ")" << std::endl;

    // Get native HWND for Win32 dialogs. The live-resize crash guard itself is already installed
    // by CreateNewWindow (above) via FSTP_InstallWindowResizeGuard — for this main window and every
    // other window — so it's live before StartAutonomousRendering() without an explicit call here.
    g_main_hwnd = GetHWNDFromSDLWindow(main_window->window);
    if (g_main_hwnd) {
        std::cout << "Native HWND obtained: " << g_main_hwnd << std::endl;
    }

    // Initialize settings system
    if (InitSettings() != 0) {
        std::cerr << "Settings system initialization warning (non-critical)" << std::endl;
    }

    // Initialize OSD system with main window renderer
    if (InitOSDSystem(main_window->renderer) != 0) {
        std::cerr << "OSD system initialization error" << std::endl;
        ShutdownWindowManager();
        SDL_Quit();
        return -1;
    }

    std::cout << "OSD system initialized" << std::endl;

    // Render on a DEDICATED thread so the VSync-blocking present no longer stalls input handling.
    // (Previously rendering ran inline in the event loop, serialising every keypress behind a
    // ~16ms present.) Renderer lifetime is guarded by g_render_mutex in the window manager.
    StartAutonomousRendering();
    std::cout << "[WINDOWS] Rendering on dedicated thread (input decoupled from VSync)" << std::endl;

    std::cout << "Ready! Press ESC or Ctrl+Q to exit, Ctrl+O to open file" << std::endl;

    // Load initial file from command line if provided
    if (g_initial_file_to_load) {
        std::cout << "Loading initial file from command line: " << g_initial_file_to_load << std::endl;
        LoadFileFromPath(g_initial_file_to_load, 0);
    }

    // First-run onboarding: show the Welcome overlay once per FSTP_WELCOME_VERSION.
    if (GetWelcomeVersion() < FSTP_WELCOME_VERSION) {
        FSTPWelcome_Show();
        SetWelcomeVersion(FSTP_WELCOME_VERSION);
    }

    // Main event loop
    bool running = true;
    SDL_Event event;
    std::thread cleanup_thread;

    // Set Windows timer resolution to 1ms so SDL_WaitEventTimeout is accurate.
    // Without this, Windows rounds delays to its default 15.6ms tick, making the
    // event loop run at ~32Hz instead of 60Hz and causing mouse shuttle sluggishness.
    timeBeginPeriod(1);

    while (running) {
        // NOTE: rendering now runs on the dedicated render thread (StartAutonomousRendering).
        // This loop only pumps events and drives shutdown, so input is no longer serialised
        // behind the VSync-blocking present.

        // CRITICAL: Asynchronous shutdown - destroy player instances in SEPARATE THREAD
        // This allows main thread to continue rendering OSD during cleanup.
        if (g_shutdownRequested.load() && !g_cleanupThreadStarted.load()) {
            std::cout << "[SHUTDOWN] Shutdown requested, starting async cleanup thread..." << std::endl;
            g_cleanupThreadStarted.store(true);

            // Launch cleanup in separate thread while main loop continues rendering
            cleanup_thread = std::thread([]() {
                std::cout << "[CLEANUP THREAD] Started cleanup while main thread renders..." << std::endl;

                FSTPPixelBufferManager* pixel_mgr = GetPixelBufferManager();

                for (int i = 0; i < MAX_WINDOWS; i++) {
                    FSTPWindow* window = GetWindowByIndex(i);
                    if (window && window->is_active) {
                        int player_id = window->player_instance_id;
                        if (player_id >= 0 && IsPlayerInstanceActive(player_id)) {
                            // CRITICAL: Release shared_ptr<AVFrame> references BEFORE
                            // destroying the player. LowResDecoder stores frames via a
                            // pool-based deleter that captures a raw pool_ptr. If the
                            // decoder (and its pool) is destroyed first, the dangling
                            // pool_ptr in the deleter causes a segfault when the
                            // shared_ptr is later released in pixel_mgr->Shutdown().
                            if (pixel_mgr) {
                                pixel_mgr->ClearPlayerBuffers(player_id);
                            }

                            std::cout << "[CLEANUP] Destroying player " << player_id << std::endl;
                            DestroyPlayerInstance(player_id);
                            std::cout << "[CLEANUP] Player " << player_id << " destroyed" << std::endl;
                        }
                    }
                }

                std::cout << "[CLEANUP THREAD] All player instances destroyed" << std::endl;
                g_shutdownComplete.store(true);
            });
        }

        // Exit when cleanup is complete
        if (g_shutdownComplete.load()) {
            std::cout << "[MAIN] Cleanup complete, exiting UI loop" << std::endl;
            running = false;
            break;
        }

        // Adaptive timeout: 1ms during mouse shuttle for fast response, 16ms otherwise.
        // SDL_WaitEventTimeout wakes immediately on any event, then drains all pending
        // events — eliminating the fixed SDL_Delay(16) overhead during active shuttle.
        int timeout_ms = IsMouseShuttleActive() ? 1 : 16;
        if (SDL_WaitEventTimeout(&event, timeout_ms)) {
            do {
                // Welcome overlay (first-run) intercepts its own clicks/keys — and
                // Esc to dismiss — before the loop treats Esc as "quit".
                if (FSTPWelcome_HandleEvent(&event)) { continue; }

                // Intercept exit events and trigger graceful shutdown
                if (event.type == SDL_QUIT) {
                    std::cout << "[EXIT] SDL_QUIT received - requesting asynchronous shutdown" << std::endl;
                    g_shutdownRequested.store(true);
                    break;
                }

                // Check for window close event
                if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE) {
                    std::cout << "[EXIT] Window close requested - requesting asynchronous shutdown" << std::endl;
                    g_shutdownRequested.store(true);
                    break;
                }

                // Check for ESC and Ctrl+Q BEFORE keyboard handler
                if (event.type == SDL_KEYDOWN) {
                    if (event.key.keysym.sym == SDLK_ESCAPE) {
                        std::cout << "[EXIT] ESC pressed - requesting asynchronous shutdown" << std::endl;
                        g_shutdownRequested.store(true);
                        break;
                    }
                    if (event.key.keysym.sym == SDLK_q && (event.key.keysym.mod & KMOD_CTRL)) {
                        std::cout << "[EXIT] Ctrl+Q pressed - requesting asynchronous shutdown" << std::endl;
                        g_shutdownRequested.store(true);
                        break;
                    }
                }

                // Skip other event handling if we're shutting down
                if (g_shutdownRequested.load()) break;

                // Handle mouse events
                if (event.type == SDL_MOUSEBUTTONDOWN) {
                    if (event.button.button == SDL_BUTTON_RIGHT) {
                        // Right click shows context menu
                        std::cout << "Right mouse button clicked, showing context menu..." << std::endl;
                        ShowWin32ContextMenu();
                        continue;
                    }
                }

                // Pass events to window manager
                HandleWindowEvents(&event);

                // Process keyboard events for player control
                HandleKeyboardEvents(event);
            } while (SDL_PollEvent(&event));
        }

        // Process Win32 message pump (required for Win32 dialogs and system integration).
        // NOTE: the live move/resize bracket (WM_ENTERSIZEMOVE..WM_EXITSIZEMOVE) is handled by the
        // HWND subclass ResizeSubclassProc, NOT here — those are SENT messages that never reach this
        // queued PeekMessage loop. (This is exactly the bug that caused the D3D11 resize crash.)
        MSG msg;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    timeEndPeriod(1);

    std::cout << "Shutting down gracefully..." << std::endl;

    // Remove the resize subclass before tearing down the window so the proc can't fire on a
    // half-destroyed window. (No-op if it was never installed.)
    if (g_main_hwnd) {
        RemoveWindowSubclass(g_main_hwnd, ResizeSubclassProc, 1);
    }

    // Step 0: Stop the render thread FIRST and join it. This guarantees no rendering touches a
    // player/renderer while the cleanup thread destroys them below, and that the renderer is idle
    // before ShutdownWindowManager tears down SDL windows. (Mirrors macOS StopAutonomousRendering
    // being called before ShutdownWindowManager.)
    StopAutonomousRendering();
    std::cout << "Rendering stopped (render thread joined)" << std::endl;

    // Step 1: Wait for cleanup thread to finish
    if (cleanup_thread.joinable()) {
        cleanup_thread.join();
        std::cout << "Cleanup thread joined" << std::endl;
    }

    // Step 3: CRITICAL - Clear PixelBufferManager
    // Use Shutdown() which internally iterates over the correct MAX_PLAYERS count.
    // Manual loop with hardcoded 16 caused out-of-bounds access on m_buffer_mutex[5..15]
    // (array size is MAX_PLAYERS=5) → segfault.
    std::cout << "Clearing pixel buffer manager..." << std::endl;
    FSTPPixelBufferManager* pixel_mgr = GetPixelBufferManager();
    if (pixel_mgr) {
        pixel_mgr->Shutdown();
        std::cout << "Pixel buffer manager cleared" << std::endl;
    }

    // Step 4: Shutdown player manager
    std::cout << "Shutting down player manager..." << std::endl;
    ShutdownPlayerManager();
    std::cout << "Player manager shutdown complete" << std::endl;

    // Step 5: Shutdown OSD system
    std::cout << "Shutting down OSD system..." << std::endl;
    ShutdownOSDSystem();
    std::cout << "OSD system shutdown complete" << std::endl;

    // Step 6: Shutdown Settings system
    std::cout << "Shutting down Settings system..." << std::endl;
    ShutdownSettings();
    std::cout << "Settings system shutdown complete" << std::endl;

    // Step 7: Shutdown Window Manager
    std::cout << "Shutting down Window Manager..." << std::endl;
    ShutdownWindowManager();
    std::cout << "Window Manager shutdown complete" << std::endl;

    // Step 8: Clean SDL shutdown (no GTK conflict on Windows!)
    SDL_Quit();
    std::cout << "SDL shutdown complete" << std::endl;

    std::cout << "Clean shutdown complete!" << std::endl;

    // IMPORTANT: Normal return on Windows (no _exit() needed!)
    // Unlike Linux, there is no GTK/SDL X11 conflict on Windows.
    return 0;
}

// Request force render (for UI changes like zoom)
extern "C" void RequestForceRender() {
    // Force render on next frame (simplified, no special flag needed)
}

#else
// Stub translation unit for non-Windows builds.
#endif
