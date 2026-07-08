#ifndef FSTP_SUBTITLES_H
#define FSTP_SUBTITLES_H

// ---------------------------------------------------------------------------
// FSTPSubtitles — sidecar subtitle support, drawn directly in SDL.
//
// This is the first concrete TapeXPlayer extension (see FSTPExtensions). It is
// implemented natively in C++ for performance and registered through the
// extension registry so it appears in the extensions list and can be toggled.
//
// Behaviour:
//   * Parses SubRip (.srt) files (the most common sidecar format).
//   * Auto-loads "<video-basename>.srt" lazily: FSTPSubtitles_Render notices
//     when a player's loaded file changes and looks for a matching sidecar —
//     so there is no per-platform file-open hook to maintain.
//   * Renders the active cue centred near the bottom of the window, scaled to
//     the window height, with a semi-transparent backing for readability.
//   * Cross-platform: the render call lives in the shared FSTPWindowManager
//     loop, so macOS / Linux / Windows all get it from one code path.
//
// Tracks are per player_id (each window's video can carry its own subtitles),
// mirroring how Memory Locations are kept per player. The on/off state is
// global (one toggle for the whole app).
// ---------------------------------------------------------------------------

#include <SDL2/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

// Global show/hide. When enabled, a loaded track is drawn; when no track is
// loaded for the player, nothing is shown. Default: enabled (so a sidecar that
// exists is shown automatically).
void FSTPSubtitles_SetEnabled(bool enabled);
bool FSTPSubtitles_IsEnabled(void);
void FSTPSubtitles_Toggle(void);

// Load / clear a track for a player explicitly (e.g. from a menu). Returns true
// if the file parsed into at least one cue.
bool FSTPSubtitles_LoadForPlayer(int player_id, const char* srt_path);
void FSTPSubtitles_ClearForPlayer(int player_id);
bool FSTPSubtitles_HasTrack(int player_id);

// Draw the active cue for player_id at time_seconds (the player's 0-based
// position). win_w/win_h are the renderer output size in pixels. No-op when
// disabled or no cue is active. Also performs the lazy sidecar auto-load.
void FSTPSubtitles_Render(SDL_Renderer* renderer, int player_id,
                          double time_seconds, int win_w, int win_h);

#ifdef __cplusplus
}
#endif

#endif // FSTP_SUBTITLES_H
