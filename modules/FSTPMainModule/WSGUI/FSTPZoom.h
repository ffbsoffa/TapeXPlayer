#ifndef FSTP_ZOOM_H
#define FSTP_ZOOM_H

#include <SDL2/SDL.h>

// Zoom constants
#define MIN_ZOOM_FACTOR 1.0f
#define MAX_ZOOM_FACTOR 8.0f
#define ZOOM_STEP 1.2f  // 20% increase/decrease per step

// Zoom state per window
struct FSTPZoomState {
    bool enabled;
    float factor;       // 1.0 = no zoom, 8.0 = max zoom
    float center_x;     // 0.0-1.0 (normalized coordinates)
    float center_y;     // 0.0-1.0
    bool show_thumbnail;  // Show mini thumbnail of full frame when zoomed
};

#ifdef __cplusplus
extern "C" {
#endif

// Initialize zoom for a window (called when window is created)
void InitZoomForWindow(int window_index);

// Zoom control functions (per window)
void IncreaseZoom(int window_index);
void DecreaseZoom(int window_index);
void ResetZoom(int window_index);
void SetZoomCenter(int window_index, float x, float y);
void ToggleZoomThumbnail(int window_index);

// Get zoom state
FSTPZoomState* GetZoomState(int window_index);

// Apply zoom to SDL_Rect (for rendering)
SDL_Rect ApplyZoomToRect(int window_index, SDL_Rect video_rect, int window_width, int window_height);

#ifdef __cplusplus
}
#endif

#endif // FSTP_ZOOM_H
