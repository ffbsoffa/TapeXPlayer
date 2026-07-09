#include "FSTPZoom.h"
#include <iostream>
#include <cmath>

#ifdef __APPLE__
#include "darwin/sdl/FSTPDarwinWS.h"
#endif

// Maximum number of windows (must match FSTPWindowManager.h)
#define MAX_WINDOWS 16

// Zoom state for each window
static FSTPZoomState g_zoom_states[MAX_WINDOWS];

void InitZoomForWindow(int window_index) {
    if (window_index < 0 || window_index >= MAX_WINDOWS) {
        return;
    }

    g_zoom_states[window_index].enabled = false;
    g_zoom_states[window_index].factor = MIN_ZOOM_FACTOR;
    g_zoom_states[window_index].center_x = 0.5f;
    g_zoom_states[window_index].center_y = 0.5f;
    g_zoom_states[window_index].show_thumbnail = false;

    std::cout << "🔍 [ZOOM] Initialized for window " << window_index << std::endl;
}

void IncreaseZoom(int window_index) {
    if (window_index < 0 || window_index >= MAX_WINDOWS) {
        return;
    }

    FSTPZoomState* state = &g_zoom_states[window_index];
    float new_factor = state->factor * ZOOM_STEP;

    if (new_factor > MAX_ZOOM_FACTOR) {
        new_factor = MAX_ZOOM_FACTOR;
    }

    state->factor = new_factor;

    // Enable zoom if it was disabled
    if (!state->enabled && new_factor > MIN_ZOOM_FACTOR) {
        state->enabled = true;
        state->show_thumbnail = true;  // Auto-enable thumbnail when zoom starts
    }

    std::cout << "🔍 [ZOOM] Window " << window_index << " increased to "
              << state->factor << "x" << std::endl;
}

void DecreaseZoom(int window_index) {
    if (window_index < 0 || window_index >= MAX_WINDOWS) {
        return;
    }

    FSTPZoomState* state = &g_zoom_states[window_index];
    float new_factor = state->factor / ZOOM_STEP;

    if (new_factor < MIN_ZOOM_FACTOR) {
        new_factor = MIN_ZOOM_FACTOR;
        state->enabled = false;
    }

    state->factor = new_factor;

    std::cout << "🔍 [ZOOM] Window " << window_index << " decreased to "
              << state->factor << "x" << std::endl;
}

void ResetZoom(int window_index) {
    if (window_index < 0 || window_index >= MAX_WINDOWS) {
        return;
    }

    FSTPZoomState* state = &g_zoom_states[window_index];
    state->factor = MIN_ZOOM_FACTOR;
    state->center_x = 0.5f;
    state->center_y = 0.5f;
    state->enabled = false;

    std::cout << "🔍 [ZOOM] Window " << window_index << " reset" << std::endl;
}

void SetZoomCenter(int window_index, float x, float y) {
    if (window_index < 0 || window_index >= MAX_WINDOWS) {
        return;
    }

    // Clamp to 0.0-1.0 range
    if (x < 0.0f) x = 0.0f;
    if (x > 1.0f) x = 1.0f;
    if (y < 0.0f) y = 0.0f;
    if (y > 1.0f) y = 1.0f;

    FSTPZoomState* state = &g_zoom_states[window_index];
    state->center_x = x;
    state->center_y = y;

    // Request forced render to display changes
#ifdef __APPLE__
    RequestForceRender();
#endif

    std::cout << "🔍 [ZOOM] Window " << window_index << " center set to ("
              << x << ", " << y << ")" << std::endl;
}

void ToggleZoomThumbnail(int window_index) {
    if (window_index < 0 || window_index >= MAX_WINDOWS) {
        return;
    }

    FSTPZoomState* state = &g_zoom_states[window_index];
    state->show_thumbnail = !state->show_thumbnail;

    std::cout << "🔍 [ZOOM] Window " << window_index << " thumbnail "
              << (state->show_thumbnail ? "enabled" : "disabled") << std::endl;
}

FSTPZoomState* GetZoomState(int window_index) {
    if (window_index < 0 || window_index >= MAX_WINDOWS) {
        return nullptr;
    }

    return &g_zoom_states[window_index];
}

SDL_Rect ApplyZoomToRect(int window_index, SDL_Rect video_rect, int window_width, int window_height) {
    if (window_index < 0 || window_index >= MAX_WINDOWS) {
        return video_rect;
    }

    FSTPZoomState* state = &g_zoom_states[window_index];

    if (!state->enabled || state->factor <= MIN_ZOOM_FACTOR) {
        return video_rect;  // No zoom applied
    }

    // Calculate zoomed rectangle
    int new_width = (int)(video_rect.w / state->factor);
    int new_height = (int)(video_rect.h / state->factor);

    // Calculate center point in video coordinates
    int center_x = video_rect.x + (int)(video_rect.w * state->center_x);
    int center_y = video_rect.y + (int)(video_rect.h * state->center_y);

    // Calculate new position (centered on zoom center)
    int new_x = center_x - new_width / 2;
    int new_y = center_y - new_height / 2;

    // Clamp to video bounds
    if (new_x < video_rect.x) new_x = video_rect.x;
    if (new_y < video_rect.y) new_y = video_rect.y;
    if (new_x + new_width > video_rect.x + video_rect.w) {
        new_x = video_rect.x + video_rect.w - new_width;
    }
    if (new_y + new_height > video_rect.y + video_rect.h) {
        new_y = video_rect.y + video_rect.h - new_height;
    }

    SDL_Rect zoomed_rect = {new_x, new_y, new_width, new_height};
    return zoomed_rect;
}
