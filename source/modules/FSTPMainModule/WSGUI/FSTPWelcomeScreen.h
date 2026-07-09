#ifndef FSTP_WELCOME_SCREEN_H
#define FSTP_WELCOME_SCREEN_H

// ---------------------------------------------------------------------------
// FSTPWelcomeScreen — first-run "Welcome" overlay, drawn directly in SDL.
//
// This replaces the macOS-only SwiftUI welcome window (FSTPWelcomeView.swift)
// with one cross-platform implementation that renders on top of the player
// window using the same SDL renderer the rest of the UI uses. The same code
// therefore serves macOS, Linux and Windows.
//
// Integration is two calls per platform:
//   * FSTPWelcome_Render() — once per frame, after the OSD and before present
//     (done once, in the shared FSTPWindowManager render loop).
//   * FSTPWelcome_HandleEvent() — at the top of each platform's SDL event
//     pump; returns true when it consumed the event (so the host loop skips it).
//
// Text comes through FSTP_Tr() (see FSTPLang.h): English defaults are built in,
// a language pack can translate it. The version gate (show once per
// FSTP_WELCOME_VERSION) lives in FSTPSettings and stays platform-shared.
// ---------------------------------------------------------------------------

#include <SDL2/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

// Show / hide the overlay. Show() is idempotent.
void FSTPWelcome_Show();
void FSTPWelcome_Hide();
bool FSTPWelcome_IsActive();

// Draw the overlay if active. win_w/win_h are the renderer's output size in
// pixels (SDL_GetRendererOutputSize). No-op when inactive.
void FSTPWelcome_Render(SDL_Renderer* renderer, int win_w, int win_h);

// Feed an SDL event while the overlay may be active. Returns true if the event
// was consumed (clicks on buttons, Esc/Enter to dismiss, modal swallowing of
// player input). Returns false for everything else (quit, window, Cmd/Ctrl+Q),
// so the host loop keeps handling those.
bool FSTPWelcome_HandleEvent(const SDL_Event* event);

#ifdef __cplusplus
}
#endif

#endif // FSTP_WELCOME_SCREEN_H
