#ifndef FSTP_TOOLS_MENU_H
#define FSTP_TOOLS_MENU_H

#ifdef __cplusplus
extern "C" {
#endif

// Initialize tools menu (only on macOS)
void InitToolsMenu();

// Show/hide inspector properties file
void ShowInspector();
void HideInspector();
void ToggleInspector();

// Check if inspector is active
bool IsInspectorActive();

// Copy screenshot of current frame to clipboard
void CopyScreenshotToClipboard();

// Show Memory Locations window
void ShowMemoryLocations();

// Create new Memory Location (called from hotkeys)
void CreateMemoryLocationAtCurrentTime();

// Check if text field is active (for player key blocking)
bool IsTextFieldActive();

// Check if Memory Locations window is open (for player key blocking)
bool IsMemoryLocationsWindowActive();

// Check if Memory Location dialog is open (for player key blocking)
bool IsMemoryLocationDialogActive();

#ifdef __cplusplus
}
#endif

#endif // FSTP_TOOLS_MENU_H