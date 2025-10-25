#ifndef FSTPDARWINWS_H
#define FSTPDARWINWS_H

#ifdef __cplusplus
extern "C" {
#endif

void ShowNativeFileDialog(int target_player_id = -1);  // -1 = auto-detect active player
void ShowNativeSettingsDialog();
void ShowSwiftUISettingsDialog();  // Modern SwiftUI settings (macOS 10.15+)
void ShowSwiftUIAboutWindow();     // Modern SwiftUI about window (macOS 13.0+)
void ShowSwiftUIMemoryLocationDialog(int player_id, double current_time);  // Modern SwiftUI memory location dialog (macOS 13.0+)
void CreateNativeMenu();
void HandleNativeAppEvents();
void InitializeNativeApp();
void RequestForceRender();  // Request forced render on UI changes (zoom, panels)

#ifdef __cplusplus
}
#endif

#endif // FSTPDARWINWS_H