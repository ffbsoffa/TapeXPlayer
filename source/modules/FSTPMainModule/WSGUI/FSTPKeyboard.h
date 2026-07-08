#ifndef FSTPKEYBOARD_H
#define FSTPKEYBOARD_H

#include <SDL2/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

bool HandleKeyboardEvents(SDL_Event& event);
void HandleNativeEvents();

// Mouse Shuttle functions
void StartMouseShuttle(int x, int y);
void UpdateMouseShuttle(int x);
void StopMouseShuttle();

// Zoom Panning (for improved FPS during zoom movement)
bool IsZoomPanningActive();

// Mouse Shuttle state (for adaptive event loop timing)
bool IsMouseShuttleActive();

// Function declaration for window event handling
void HandleWindowEvents(SDL_Event* event);

#ifdef __cplusplus
}
#endif

#endif // FSTPKEYBOARD_H