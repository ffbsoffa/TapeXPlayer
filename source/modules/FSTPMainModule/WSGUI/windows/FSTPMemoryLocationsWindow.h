#ifndef FSTP_MEMORY_LOCATIONS_WINDOW_H
#define FSTP_MEMORY_LOCATIONS_WINDOW_H

#ifdef _WIN32

#ifdef __cplusplus
extern "C" {
#endif

// Show/Hide Memory Locations Inspector Window
void ShowWin32MemoryLocationsWindow();
void HideWin32MemoryLocationsWindow();
void ToggleWin32MemoryLocationsWindow();

// Show unified Add/Edit dialog for Memory Locations
// Use location_id = -1 for "Add mode", otherwise "Edit mode"
void ShowWin32MemoryLocationDialog(int player_id, int location_id, double current_time);

#ifdef __cplusplus
}
#endif

#endif // _WIN32

#endif // FSTP_MEMORY_LOCATIONS_WINDOW_H
