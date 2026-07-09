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

// Create new Memory Location (called from hotkeys) — opens naming dialog
void CreateMemoryLocationAtCurrentTime();

// Instantly create an auto-named Memory Location on the focused player (no dialog).
// Pro Tools "Auto-Name Memory Locations" style — the fast path bound to Enter.
void CreateMemoryLocationInstant();

// Reload the Memory Locations window's table if it is open (after a keyboard
// create/delete or a focus change, so it reflects the focused player's set).
void RefreshMemoryLocationsWindowIfOpen();

// Check if text field is active (for player key blocking)
bool IsTextFieldActive();

// Check if Memory Locations window is open (for player key blocking)
bool IsMemoryLocationsWindowActive();

// Check if Memory Location dialog is open (for player key blocking)
bool IsMemoryLocationDialogActive();

// Presentation mode
void ShowPresentationMode();
void TogglePresentationMode();
bool IsPresentationModeActive();

#ifdef __cplusplus
}
#endif

#endif // FSTP_TOOLS_MENU_H