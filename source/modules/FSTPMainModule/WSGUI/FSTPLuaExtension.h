#ifndef FSTP_LUA_EXTENSION_H
#define FSTP_LUA_EXTENSION_H

// ---------------------------------------------------------------------------
// FSTPLuaExtension — runs script (type = "lua") extensions.
//
// Each Lua extension gets its own Lua VM with a small host API exposed as a
// global `fstp` table. The extension's entry script registers a render
// callback; the player calls it every frame so the script can draw an overlay.
//
// Host API available to scripts (see FSTPLuaExtension.cpp):
//   -- events
//   fstp.on_render(function() ... end)  -- per-frame draw callback
//   fstp.on_load(function(path) ... end) -- fired when a file is loaded
//   -- read (safe any time)
//   fstp.state([player]) -> table       -- {player, active, file, time, duration,
//                                          fps, frame, timecode, playing, speed, reverse}
//   fstp.active_player() -> id
//   fstp.player_count()  -> n
//   fstp.player_time()   -> seconds     -- current render-frame position
//   fstp.window_size()   -> w, h        -- renderer output size (pixels)
//   -- draw (valid inside on_render)
//   fstp.draw_text(x, y, text [, size [, r, g, b]])
//   -- actions (queued, run on the main thread)
//   fstp.seek(seconds [, player])   fstp.play([player])   fstp.pause([player])
//   fstp.set_speed(x [, player])    fstp.step(frames [, player])
//   fstp.shuttle(speed [, player])  -- wind with transport inertia (neg = reverse)
//   fstp.set_reverse(bool [, player])
//   fstp.screenshot()               -- current frame to clipboard
//   fstp.add_marker([name [, comment [, player]]])   fstp.export_csv(path)
//   fstp.log(msg)
//
// Built only when Lua is available (FSTP_HAVE_LUA, set by the makefile when
// pkg-config finds 'lua'). Without it every entry point is a safe no-op, so the
// extension registry still lists "lua" extensions — it just doesn't run them.
// ---------------------------------------------------------------------------

#include <SDL2/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

// True when the build has a Lua runtime (so type=lua extensions can run).
bool FSTPLua_IsAvailable(void);

// Load <dir>/<entry> as extension `id`: new VM, host API, run the script.
// Returns true on success. No-op returning false when Lua isn't built in.
bool FSTPLua_Load(const char* id, const char* dir, const char* entry);

// Unload a previously loaded extension (closes its VM).
void FSTPLua_Unload(const char* id);

// Per-frame: invoke every loaded extension's render callback so it can draw.
// Called from the shared render loop after the OSD/subtitles. win_w/win_h are
// the renderer output size in pixels.
void FSTPLua_Render(SDL_Renderer* renderer, int player_id, double time_seconds,
                    int win_w, int win_h);

// Drain actions scripts requested from the render thread (seek/play/screenshot/
// markers) and run them. Call once per iteration of the main/event loop so these
// run on the same thread as the keyboard/menu handlers. No-op without Lua.
void FSTPLua_ProcessPending(void);

#ifdef __cplusplus
}
#endif

#endif // FSTP_LUA_EXTENSION_H
