#ifndef FSTP_WINDOWS_WS_H
#define FSTP_WINDOWS_WS_H

#ifdef _WIN32

#ifdef __cplusplus
extern "C" {
#endif

void ShowNativeFileDialog(int target_player_id);
void LoadFileFromPath(const char* filepath, int target_player_id);
void RequestForceRender();

// Screenshot
void CopyScreenshotToClipboard();

// Memory Location Dialog (unified for add/edit: location_id = -1 for add mode)
void ShowWin32MemoryLocationDialog(int player_id, int location_id, double current_time);

// Settings Dialog
void ShowWin32SettingsDialog();

// About Dialog
void ShowWin32AboutDialog();

// Context Menu (right-click menu)
void ShowWin32ContextMenu();

// Memory Locations Window
void ShowWin32MemoryLocationsWindow();
void HideWin32MemoryLocationsWindow();
void ToggleWin32MemoryLocationsWindow();

#ifdef __cplusplus
}
#endif

#endif // _WIN32

#endif // FSTP_WINDOWS_WS_H
