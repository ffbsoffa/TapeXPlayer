#ifndef FSTPWAYLANDWS_H
#define FSTPWAYLANDWS_H

#ifdef __cplusplus
extern "C" {
#endif

void ShowNativeFileDialog(int target_player_id = -1);  // -1 = auto-detect active player
void LoadFileFromPath(const char* filepath, int target_player_id = -1);  // Load file directly from path
void RequestForceRender();  // Request forced render on UI changes

// Screenshot
void CopyScreenshotToClipboard();

// Memory Location Dialog (unified for add/edit: location_id = -1 for add mode)
void ShowGTKMemoryLocationDialog(int player_id, int location_id, double current_time);
void OnMemoryLocationDialogClosedCallback();

// Settings Dialog
void ShowGTKSettingsDialog();

// Context Menu (right-click menu)
void ShowGTKContextMenu();

#ifdef __cplusplus
}
#endif

#endif // FSTPWAYLANDWS_H
