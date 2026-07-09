#include "FSTPLuaExtension.h"

#ifdef FSTP_HAVE_LUA

#include "fontdata.h"
#include "FSTPMemoryLocations.h"                      // markers (add / export CSV)
#include "../FSTPPlayerModule/FSTPPlayerManager.h"    // state + control C API
#include <SDL2/SDL_ttf.h>
#include <map>
#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <iostream>
#include <cstdio>
#include <cmath>
#include <chrono>
#include <algorithm>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

// Declared in FSTPWindowManager.h / the platform WS files (both extern "C");
// forward-declared here to avoid pulling heavy headers into this translation unit.
extern "C" int GetActivePlayerID();
extern "C" void CopyScreenshotToClipboard();

namespace {

struct LuaExt {
    lua_State* L = nullptr;
    int render_ref = LUA_NOREF;   // on_render callback
    int load_ref = LUA_NOREF;     // on_load callback
};

std::mutex g_mutex;                       // guards g_exts + g_last_file (load/unload vs render)
std::map<std::string, LuaExt> g_exts;
LuaExt* g_loading = nullptr;
std::map<int, std::string> g_last_file;   // per player, for on_load detection

// Actions requested from the render thread, run later on the main thread.
std::mutex g_queue_mutex;
std::vector<std::function<void()>> g_pending;
void enqueue(std::function<void()> fn) {
    std::lock_guard<std::mutex> lk(g_queue_mutex);
    g_pending.push_back(std::move(fn));
}

// Tape-care shuttle: glide the actual shuttle speed toward the requested target
// with limited acceleration, so it winds up and eases off gently — a hard jump
// to/from a high speed would "tear the tape". g_shuttle_tgt is written by
// fstp.shuttle (render thread, under g_queue_mutex); the glide runs on the main
// thread in FSTPLua_ProcessPending.
std::map<int, double> g_shuttle_tgt;   // commanded signed speed (neg = reverse)
std::map<int, double> g_shuttle_cur;   // gliding signed speed (main thread only)
std::map<int, bool>   g_shuttle_on;    // glider managing this player
// easeOutQuint tween state (main thread only): each retarget starts a new tween
// from the current speed to the new target over a duration that scales with the
// size of the change (so the average wind rate stays bounded — tape care).
std::map<int, double> g_shuttle_start; // tween start speed
std::map<int, double> g_shuttle_active;// the target this tween is animating to
std::map<int, double> g_shuttle_dur;   // tween duration (s)
std::map<int, bool>   g_shuttle_decel; // this tween is slowing down (use fast curve)
std::map<int, std::chrono::steady_clock::time_point> g_shuttle_t0;
double g_shuttle_rate = 9.0;           // wind-UP units/sec (gentle, tape care)
double g_shuttle_rate_down = 55.0;     // wind-DOWN units/sec (~0→ from 16x in ~0.3s)

// easeOutQuint: fast pickup, gentle settle — for SPEEDING UP (kind to the tape).
inline double easeOutQuint(double t) {
    if (t < 0) t = 0; if (t > 1) t = 1;
    double u = 1.0 - t;
    return 1.0 - u * u * u * u * u;
}
// easeInQuint: holds the high speed, then drops fast at the end — for SLOWING
// DOWN, so the transport spends minimal time at low speeds where the Betacam
// stripes would otherwise show during the release.
inline double easeInQuint(double t) {
    if (t < 0) t = 0; if (t > 1) t = 1;
    return t * t * t * t * t;
}

// Drawing context, valid only while a render/on-load callback runs.
struct RenderCtx {
    SDL_Renderer* r = nullptr;
    int w = 0, h = 0;
    double time = 0.0;
    int player = 0;
    bool active = false;
};
RenderCtx g_ctx;

std::map<int, TTF_Font*> g_fonts;

TTF_Font* fontFor(int px) {
    if (px < 8) px = 8;
    if (px > 96) px = 96;
    auto it = g_fonts.find(px);
    if (it != g_fonts.end()) return it->second;
    if (TTF_WasInit() == 0) TTF_Init();
    TTF_Font* f = TTF_OpenFontRW(SDL_RWFromConstMem(font_otf, font_otf_size), 1, px);
    g_fonts[px] = f;
    return f;
}

void makeTimecode(double t, double fps, char* out, size_t n) {
    if (fps <= 0.0) fps = 25.0;
    if (t < 0) t = 0;
    int ifps = (int)(fps + 0.5);
    if (ifps < 1) ifps = 25;
    long total = (long)(t * fps);
    int ff = (int)(total % ifps);
    long secs = total / ifps;
    int ss = (int)(secs % 60), mm = (int)((secs / 60) % 60), hh = (int)(secs / 3600);
    std::snprintf(out, n, "%02d:%02d:%02d:%02d", hh, mm, ss, ff);
}

int argPlayer(lua_State* L, int idx) {
    if (lua_isnoneornil(L, idx)) {
        // Inside a render/on_load callback default to the player being rendered,
        // not the focus-dependent "active" player (GetActivePlayerID returns -1
        // when no window has focus — e.g. headless/background runs).
        if (g_ctx.active && g_ctx.player >= 0) return g_ctx.player;
        return GetActivePlayerID();
    }
    return (int)luaL_checkinteger(L, idx);
}

// ---- Host API: events ---------------------------------------------------

int l_on_render(lua_State* L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    if (g_loading) {
        lua_pushvalue(L, 1);
        if (g_loading->render_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, g_loading->render_ref);
        g_loading->render_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    return 0;
}

int l_on_load(lua_State* L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    if (g_loading) {
        lua_pushvalue(L, 1);
        if (g_loading->load_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, g_loading->load_ref);
        g_loading->load_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    return 0;
}

// ---- Host API: reads ----------------------------------------------------

int l_log(lua_State* L) {
    std::cout << "[lua] " << luaL_optstring(L, 1, "") << std::endl;
    return 0;
}

int l_active_player(lua_State* L) { lua_pushinteger(L, GetActivePlayerID()); return 1; }
int l_player_count(lua_State* L)  { lua_pushinteger(L, GetActiveInstanceCount()); return 1; }
int l_player_time(lua_State* L)   { lua_pushnumber(L, g_ctx.time); return 1; }
int l_window_size(lua_State* L)   { lua_pushinteger(L, g_ctx.w); lua_pushinteger(L, g_ctx.h); return 2; }

int l_state(lua_State* L) {
    int p = argPlayer(L, 1);
    lua_newtable(L);
    bool active = IsPlayerInstanceActive(p) != 0;
    lua_pushinteger(L, p);     lua_setfield(L, -2, "player");
    lua_pushboolean(L, active); lua_setfield(L, -2, "active");
    if (active) {
        const char* fp = GetInstanceFilePath(p);
        double t = GetInstancePosition(p);
        double dur = GetInstanceDuration(p);
        double fps = GetInstanceVideoFPS(p);
        char tc[24]; makeTimecode(t, fps, tc, sizeof tc);
        lua_pushstring(L, fp ? fp : "");           lua_setfield(L, -2, "file");
        lua_pushnumber(L, t);                      lua_setfield(L, -2, "time");
        lua_pushnumber(L, dur);                    lua_setfield(L, -2, "duration");
        lua_pushnumber(L, fps);                    lua_setfield(L, -2, "fps");
        lua_pushinteger(L, (fps > 0) ? (lua_Integer)(t * fps + 1e-4) : 0); lua_setfield(L, -2, "frame");
        lua_pushstring(L, tc);                     lua_setfield(L, -2, "timecode");
        lua_pushboolean(L, IsInstancePlaying(p) != 0); lua_setfield(L, -2, "playing");
        lua_pushnumber(L, GetInstanceActualSpeed(p));  lua_setfield(L, -2, "speed");
        lua_pushboolean(L, IsInstanceReverse(p) != 0); lua_setfield(L, -2, "reverse");
    }
    return 1;
}

// ---- Host API: draw (inside on_render) ----------------------------------

int l_draw_text(lua_State* L) {
    if (!g_ctx.active || !g_ctx.r) return 0;
    int x = (int)luaL_checknumber(L, 1);
    int y = (int)luaL_checknumber(L, 2);
    const char* text = luaL_checkstring(L, 3);
    int size = (int)luaL_optinteger(L, 4, 18);
    int r = (int)luaL_optinteger(L, 5, 235);
    int g = (int)luaL_optinteger(L, 6, 235);
    int b = (int)luaL_optinteger(L, 7, 235);
    if (!text || !*text) return 0;
    TTF_Font* f = fontFor(size);
    if (!f) return 0;
    SDL_Color col{ (Uint8)r, (Uint8)g, (Uint8)b, 255 };
    SDL_Surface* surf = TTF_RenderUTF8_Blended(f, text, col);
    if (!surf) return 0;
    SDL_Texture* t = SDL_CreateTextureFromSurface(g_ctx.r, surf);
    SDL_Rect d{ x, y, surf->w, surf->h };
    SDL_FreeSurface(surf);
    if (t) { SDL_RenderCopy(g_ctx.r, t, nullptr, &d); SDL_DestroyTexture(t); }
    return 0;
}

// ---- Host API: actions (queued → main thread) ---------------------------

int l_seek(lua_State* L) {
    double t = luaL_checknumber(L, 1);
    int p = argPlayer(L, 2);
    enqueue([p, t] { SeekInstance(p, t); });
    return 0;
}
int l_play(lua_State* L)  { int p = argPlayer(L, 1); enqueue([p] { PlayInstance(p); });  return 0; }
int l_pause(lua_State* L) { int p = argPlayer(L, 1); enqueue([p] { PauseInstance(p); }); return 0; }
int l_set_speed(lua_State* L) {
    double s = luaL_checknumber(L, 1);
    int p = argPlayer(L, 2);
    enqueue([p, s] { SetInstanceSpeed(p, s); });
    return 0;
}
int l_step(lua_State* L) {
    int n = (int)luaL_checkinteger(L, 1);
    int p = argPlayer(L, 2);
    enqueue([p, n] {
        double fps = GetInstanceVideoFPS(p);
        if (fps <= 0) fps = 25.0;
        SeekInstance(p, GetInstancePosition(p) + n / fps);
    });
    return 0;
}
// Shuttle: wind the transport at a signed speed (negative = reverse) using the
// ANIMATED speed/reverse path, so it ramps up with inertia and the direction
// sequencer like a real Betacam shuttle — not an instant jump. shuttle(0) stops.
int l_shuttle(lua_State* L) {
    double speed = luaL_checknumber(L, 1);
    int p = argPlayer(L, 2);
    // Just set the target; the tape-care glide (FSTPLua_ProcessPending) eases the
    // actual speed toward it on the main thread.
    std::lock_guard<std::mutex> lk(g_queue_mutex);
    g_shuttle_tgt[p] = speed;
    g_shuttle_on[p] = true;
    return 0;
}
int l_set_reverse(lua_State* L) {
    bool rev = lua_toboolean(L, 1) != 0;
    int p = argPlayer(L, 2);
    enqueue([p, rev] { SetInstanceReverse(p, rev); });
    return 0;
}
int l_screenshot(lua_State*) {
    enqueue([] { CopyScreenshotToClipboard(); });  // clipboard; path-save is a TODO
    return 0;
}
int l_add_marker(lua_State* L) {
    std::string name = luaL_optstring(L, 1, "");
    std::string comment = luaL_optstring(L, 2, "");
    int p = argPlayer(L, 3);
    enqueue([p, name, comment] { FSTP_AddMemoryLocationAtCurrentTime(p, name.c_str(), comment.c_str()); });
    return 0;
}
int l_export_csv(lua_State* L) {
    std::string path = luaL_checkstring(L, 1);
    enqueue([path] { FSTP_ExportMemoryLocationsToCSV(path.c_str()); });
    return 0;
}

void registerAPI(lua_State* L) {
    lua_newtable(L);
    auto set = [&](const char* name, lua_CFunction fn) { lua_pushcfunction(L, fn); lua_setfield(L, -2, name); };
    set("log", l_log);
    set("on_render", l_on_render);
    set("on_load", l_on_load);
    set("state", l_state);
    set("active_player", l_active_player);
    set("player_count", l_player_count);
    set("player_time", l_player_time);
    set("window_size", l_window_size);
    set("draw_text", l_draw_text);
    set("seek", l_seek);
    set("play", l_play);
    set("pause", l_pause);
    set("set_speed", l_set_speed);
    set("step", l_step);
    set("shuttle", l_shuttle);
    set("set_reverse", l_set_reverse);
    set("screenshot", l_screenshot);
    set("add_marker", l_add_marker);
    set("export_csv", l_export_csv);
    lua_setglobal(L, "fstp");
}

void unloadLocked(const std::string& id) {
    auto it = g_exts.find(id);
    if (it == g_exts.end()) return;
    if (it->second.L) {
        if (it->second.render_ref != LUA_NOREF) luaL_unref(it->second.L, LUA_REGISTRYINDEX, it->second.render_ref);
        if (it->second.load_ref != LUA_NOREF) luaL_unref(it->second.L, LUA_REGISTRYINDEX, it->second.load_ref);
        lua_close(it->second.L);
    }
    g_exts.erase(it);
}

// Invoke a stored callback (ref) with optional one string arg; disable it on error.
void callRef(const std::string& id, LuaExt& ext, int& ref, const char* strArg) {
    if (!ext.L || ref == LUA_NOREF) return;
    lua_rawgeti(ext.L, LUA_REGISTRYINDEX, ref);
    int nargs = 0;
    if (strArg) { lua_pushstring(ext.L, strArg); nargs = 1; }
    if (lua_pcall(ext.L, nargs, 0, 0) != LUA_OK) {
        const char* err = lua_tostring(ext.L, -1);
        std::cerr << "[lua] callback error in '" << id << "': " << (err ? err : "?")
                  << " (disabled)" << std::endl;
        lua_pop(ext.L, 1);
        luaL_unref(ext.L, LUA_REGISTRYINDEX, ref);
        ref = LUA_NOREF;
    }
}

} // namespace

// ---------------------------------------------------------------------------

bool FSTPLua_IsAvailable(void) { return true; }

bool FSTPLua_Load(const char* id, const char* dir, const char* entry) {
    if (!id || !dir || !entry) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    unloadLocked(id);

    lua_State* L = luaL_newstate();
    if (!L) return false;
    luaL_openlibs(L);   // NOTE: full stdlib for now; sandboxing (drop os/io) is a TODO
    registerAPI(L);

    LuaExt ext; ext.L = L;
    g_exts[id] = ext;
    g_loading = &g_exts[id];

    std::string path(dir);
    if (!path.empty() && path.back() != '/' && path.back() != '\\') path += '/';
    path += entry;

    int rc = luaL_dofile(L, path.c_str());
    g_loading = nullptr;

    if (rc != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        std::cerr << "[lua] error loading extension '" << id << "': " << (err ? err : "?") << std::endl;
        unloadLocked(id);
        return false;
    }
    std::cout << "[lua] extension loaded: " << id << " (" << path << ")" << std::endl;
    return true;
}

void FSTPLua_Unload(const char* id) {
    if (!id) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    unloadLocked(id);
}

void FSTPLua_Render(SDL_Renderer* renderer, int player_id, double time_seconds,
                    int win_w, int win_h) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_exts.empty() || !renderer) return;

    g_ctx.r = renderer; g_ctx.w = win_w; g_ctx.h = win_h;
    g_ctx.time = time_seconds; g_ctx.player = player_id; g_ctx.active = true;

    // on_load: fire when the player's file changes (safe read — see the
    // DestroyPlayerInstance ordering fix).
    if (player_id >= 0 && IsPlayerInstanceActive(player_id)) {
        const char* fp = GetInstanceFilePath(player_id);
        std::string cur = fp ? fp : "";
        std::string& last = g_last_file[player_id];
        if (cur != last) {
            last = cur;
            if (!cur.empty())
                for (auto& [id, ext] : g_exts) callRef(id, ext, ext.load_ref, cur.c_str());
        }
    }

    for (auto& [id, ext] : g_exts) callRef(id, ext, ext.render_ref, nullptr);

    g_ctx.active = false;
    g_ctx.r = nullptr;
}

void FSTPLua_ProcessPending(void) {
    std::vector<std::function<void()>> todo;
    {
        std::lock_guard<std::mutex> lk(g_queue_mutex);
        todo.swap(g_pending);
    }
    for (auto& fn : todo) fn();

    // --- Tape-care shuttle glide (easeOutQuint) ----------------------------
    std::lock_guard<std::mutex> lk(g_queue_mutex);
    auto now = std::chrono::steady_clock::now();

    for (auto& kv : g_shuttle_on) {
        int p = kv.first;
        if (!kv.second) continue;
        double newTgt = g_shuttle_tgt.count(p) ? g_shuttle_tgt[p] : 0.0;

        // Retarget: when the requested speed moves, start a fresh tween from the
        // current speed. Duration scales with the size of the change so the
        // average wind rate stays bounded (tape care); shape is easeOutQuint.
        if (!g_shuttle_active.count(p) || std::fabs(g_shuttle_active[p] - newTgt) > 0.01) {
            double start;
            if (g_shuttle_cur.count(p)) {
                start = g_shuttle_cur[p];
            } else {
                double m = GetInstanceActualSpeed(p);
                start = IsInstanceReverse(p) ? -m : m;
            }
            // Speeding up = gentle (easeOutQuint, slow rate); slowing down = fast
            // (easeInQuint, high rate) so the release skips past the stripe-prone
            // low-speed band quickly.
            bool decel = std::fabs(newTgt) < std::fabs(start) - 0.01;
            g_shuttle_decel[p]  = decel;
            g_shuttle_start[p]  = start;
            g_shuttle_active[p] = newTgt;
            g_shuttle_t0[p]     = now;
            double rate = decel ? g_shuttle_rate_down : g_shuttle_rate;
            g_shuttle_dur[p]    = std::max(0.03, std::fabs(newTgt - start) / rate);
        }

        double t = std::chrono::duration<double>(now - g_shuttle_t0[p]).count() / g_shuttle_dur[p];
        double e = g_shuttle_decel[p] ? easeInQuint(t) : easeOutQuint(t);
        double cur = g_shuttle_start[p] + (g_shuttle_active[p] - g_shuttle_start[p]) * e;
        g_shuttle_cur[p] = cur;

        double mag = std::fabs(cur);
        bool rev = cur < 0.0;
        if (std::fabs(newTgt) < 0.01) {
            // Easing down to a stop — never (re)start the transport here.
            if (t >= 1.0 || mag < 0.05) {
                if (IsInstancePlaying(p)) PauseInstance(p);   // gentle final stop
                g_shuttle_cur[p] = 0.0; kv.second = false;
            } else if (IsInstancePlaying(p)) {
                SetInstanceSpeed(p, std::max(mag, 0.05)); SetInstanceReverse(p, rev);
            } else {
                g_shuttle_cur[p] = 0.0; kv.second = false;    // stopped elsewhere
            }
        } else {
            if (!IsInstancePlaying(p)) PlayInstance(p);   // Play resets speed → set it after
            SetInstanceSpeed(p, std::max(mag, 0.05));
            SetInstanceReverse(p, rev);
        }
    }
}

#else  // !FSTP_HAVE_LUA — safe no-ops so the registry still lists lua extensions.

bool FSTPLua_IsAvailable(void) { return false; }
bool FSTPLua_Load(const char*, const char*, const char*) { return false; }
void FSTPLua_Unload(const char*) {}
void FSTPLua_Render(SDL_Renderer*, int, double, int, int) {}
void FSTPLua_ProcessPending(void) {}

#endif // FSTP_HAVE_LUA
