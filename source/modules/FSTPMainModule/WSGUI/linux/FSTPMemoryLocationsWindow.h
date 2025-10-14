#ifndef FSTP_MEMORY_LOCATIONS_WINDOW_H
#define FSTP_MEMORY_LOCATIONS_WINDOW_H

#ifdef __linux__

#ifdef __cplusplus
extern "C" {
#endif

// Show/Hide Memory Locations Inspector Window
void ShowGTKMemoryLocationsWindow();
void HideGTKMemoryLocationsWindow();
void ToggleGTKMemoryLocationsWindow();

#ifdef __cplusplus
}
#endif

#endif // __linux__

#endif // FSTP_MEMORY_LOCATIONS_WINDOW_H
