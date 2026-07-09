#include "FSTPKeyBindings.h"
#include "FSTPMemoryLocations.h"   // FSTP_GetMemoryLocationsCachePath()

#include <SDL2/SDL_keycode.h>
#include <SDL2/SDL_keyboard.h>

#include <string>
#include <fstream>
#include <sstream>
#include <mutex>
#include <filesystem>

namespace fs = std::filesystem;

namespace {

struct ActionInfo {
    KeyActionId id;
    const char* name;
    const char* group;
    int         defaultKey;
    int         defaultMods;   // FSTP_MOD_* (all 0 today)
    bool        editable;
};

// Order here defines the order shown in the settings list.
const ActionInfo kActions[] = {
    { KA_PLAY_PAUSE,    "Play / Pause",            "Transport", SDLK_SPACE,        0, true  },
    { KA_STOP,          "Stop",                    "Transport", SDLK_s,            0, true  },
    { KA_REVERSE,       "Reverse",                 "Transport", SDLK_r,            0, true  },
    { KA_GO_START,      "Go to Start",             "Transport", SDLK_HOME,         0, true  },
    { KA_GO_END,        "Go to End",               "Transport", SDLK_END,          0, true  },
    { KA_SPEED_1X,      "Speed 1x",                "Speed",     SDLK_1,            0, true  },
    { KA_SPEED_3X,      "Speed 3x",                "Speed",     SDLK_2,            0, true  },
    { KA_SPEED_UP,      "Speed Up",                "Speed",     SDLK_EQUALS,       0, true  },
    { KA_SPEED_DOWN,    "Speed Down",              "Speed",     SDLK_MINUS,        0, true  },
    { KA_MUTE,          "Mute",                    "Volume",    SDLK_m,            0, true  },
    { KA_TIMEFRAME,     "Timecode / Frames",       "View",      SDLK_t,            0, true  },
    { KA_FULLSCREEN,    "Fullscreen",              "View",      SDLK_f,            0, true  },
    { KA_MARKER_NEW,    "New Marker (dialog)",     "Markers",   SDLK_RETURN,       0, false },
    { KA_MARKER_PREV,   "Previous Marker",         "Markers",   SDLK_LEFTBRACKET,  0, true  },
    { KA_MARKER_NEXT,   "Next Marker",             "Markers",   SDLK_RIGHTBRACKET, 0, true  },
    { KA_MARKER_RECALL, "Recall Marker (numpad)",  "Markers",   SDLK_KP_PERIOD,    0, true  },
    { KA_TIMECODE_INPUT,"Timecode Entry (numpad)", "Transport", SDLK_KP_MULTIPLY,  0, true  },
};
constexpr int kActionCount = (int)(sizeof(kActions) / sizeof(kActions[0]));

std::mutex g_mutex;
int  g_curKey[kActionCount];
int  g_curMod[kActionCount];
bool g_loaded = false;

int IndexOfAction(int action_id) {
    for (int i = 0; i < kActionCount; ++i)
        if (kActions[i].id == action_id) return i;
    return -1;
}

// SDL KMOD mask -> normalized FSTP_MOD_* bits (collapse L/R, ignore GUI/Caps/Num).
int NormalizeSDLMods(int kmod) {
    int m = 0;
    if (kmod & KMOD_SHIFT) m |= FSTP_MOD_SHIFT;
    if (kmod & KMOD_CTRL)  m |= FSTP_MOD_CTRL;
    if (kmod & KMOD_ALT)   m |= FSTP_MOD_ALT;
    return m;
}

// FSTP_MOD_* bits -> an SDL KMOD mask the handler's checks understand.
int ToSDLMods(int m) {
    int k = 0;
    if (m & FSTP_MOD_SHIFT) k |= KMOD_SHIFT;
    if (m & FSTP_MOD_CTRL)  k |= KMOD_CTRL;
    if (m & FSTP_MOD_ALT)   k |= KMOD_ALT;
    return k;
}

std::string ConfigPath() {
    const char* base = FSTP_GetMemoryLocationsCachePath();
    fs::path p = (base && *base) ? fs::path(base) : fs::temp_directory_path();
    return (p / "keybindings.conf").string();
}

void EnsureDefaults() {
    for (int i = 0; i < kActionCount; ++i) {
        g_curKey[i] = kActions[i].defaultKey;
        g_curMod[i] = kActions[i].defaultMods;
    }
}

void LoadLocked() {
    EnsureDefaults();
    std::ifstream f(ConfigPath());
    if (f.is_open()) {
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream ss(line);
            int aid = 0, key = 0, mod = 0;
            if (ss >> aid >> key >> mod) {
                int idx = IndexOfAction(aid);
                if (idx >= 0 && kActions[idx].editable) {
                    g_curKey[idx] = key;
                    g_curMod[idx] = mod;
                }
            }
        }
    }
    g_loaded = true;
}

void EnsureLoaded() { if (!g_loaded) LoadLocked(); }

} // namespace

// ============================== C API =======================================

int FSTP_KB_GetActionCount(void) { return kActionCount; }

int FSTP_KB_GetActionIdByIndex(int index) {
    if (index < 0 || index >= kActionCount) return KA_NONE;
    return kActions[index].id;
}

const char* FSTP_KB_GetActionName(int action_id) {
    int i = IndexOfAction(action_id);
    return i >= 0 ? kActions[i].name : "";
}

const char* FSTP_KB_GetActionGroup(int action_id) {
    int i = IndexOfAction(action_id);
    return i >= 0 ? kActions[i].group : "";
}

int FSTP_KB_IsActionEditable(int action_id) {
    int i = IndexOfAction(action_id);
    return (i >= 0 && kActions[i].editable) ? 1 : 0;
}

int FSTP_KB_GetKeycode(int action_id) {
    std::lock_guard<std::mutex> lk(g_mutex);
    EnsureLoaded();
    int i = IndexOfAction(action_id);
    return i >= 0 ? g_curKey[i] : 0;
}

int FSTP_KB_GetMods(int action_id) {
    std::lock_guard<std::mutex> lk(g_mutex);
    EnsureLoaded();
    int i = IndexOfAction(action_id);
    return i >= 0 ? g_curMod[i] : 0;
}

int FSTP_KB_GetDefaultKeycode(int action_id) {
    int i = IndexOfAction(action_id);
    return i >= 0 ? kActions[i].defaultKey : 0;
}

int FSTP_KB_GetDefaultMods(int action_id) {
    int i = IndexOfAction(action_id);
    return i >= 0 ? kActions[i].defaultMods : 0;
}

const char* FSTP_KB_GetKeyName(int keycode) {
    return SDL_GetKeyName((SDL_Keycode)keycode);
}

int FSTP_KB_SetBinding(int action_id, int keycode, int mods, int* out_conflict_action) {
    std::lock_guard<std::mutex> lk(g_mutex);
    EnsureLoaded();
    int idx = IndexOfAction(action_id);
    if (idx < 0 || !kActions[idx].editable) return -2;
    mods &= (FSTP_MOD_SHIFT | FSTP_MOD_CTRL | FSTP_MOD_ALT);
    for (int i = 0; i < kActionCount; ++i) {
        if (i != idx && g_curKey[i] == keycode && g_curMod[i] == mods) {
            if (out_conflict_action) *out_conflict_action = kActions[i].id;
            return -1;
        }
    }
    g_curKey[idx] = keycode;
    g_curMod[idx] = mods;
    return 0;
}

void FSTP_KB_ResetToDefaults(void) {
    std::lock_guard<std::mutex> lk(g_mutex);
    EnsureDefaults();
    g_loaded = true;
}

int FSTP_KB_MacKeyToSDL(int mac, int uni) {
    switch (mac) {
        case 0x24: return SDLK_RETURN;
        case 0x30: return SDLK_TAB;
        case 0x31: return SDLK_SPACE;
        case 0x33: return SDLK_BACKSPACE;
        case 0x35: return SDLK_ESCAPE;
        case 0x75: return SDLK_DELETE;
        case 0x73: return SDLK_HOME;
        case 0x77: return SDLK_END;
        case 0x74: return SDLK_PAGEUP;
        case 0x79: return SDLK_PAGEDOWN;
        case 0x7B: return SDLK_LEFT;
        case 0x7C: return SDLK_RIGHT;
        case 0x7D: return SDLK_DOWN;
        case 0x7E: return SDLK_UP;
        case 0x7A: return SDLK_F1;
        case 0x78: return SDLK_F2;
        case 0x63: return SDLK_F3;
        case 0x76: return SDLK_F4;
        case 0x60: return SDLK_F5;
        case 0x61: return SDLK_F6;
        case 0x62: return SDLK_F7;
        case 0x64: return SDLK_F8;
        case 0x65: return SDLK_F9;
        case 0x6D: return SDLK_F10;
        case 0x67: return SDLK_F11;
        case 0x6F: return SDLK_F12;
        case 0x52: return SDLK_KP_0;
        case 0x53: return SDLK_KP_1;
        case 0x54: return SDLK_KP_2;
        case 0x55: return SDLK_KP_3;
        case 0x56: return SDLK_KP_4;
        case 0x57: return SDLK_KP_5;
        case 0x58: return SDLK_KP_6;
        case 0x59: return SDLK_KP_7;
        case 0x5B: return SDLK_KP_8;
        case 0x5C: return SDLK_KP_9;
        case 0x41: return SDLK_KP_PERIOD;
        case 0x43: return SDLK_KP_MULTIPLY;
        case 0x45: return SDLK_KP_PLUS;
        case 0x4E: return SDLK_KP_MINUS;
        case 0x4B: return SDLK_KP_DIVIDE;
        case 0x4C: return SDLK_KP_ENTER;
        case 0x51: return SDLK_KP_EQUALS;
        default: break;
    }
    // Printable key: SDL keycodes for ASCII == the ASCII value (letters lowercase).
    if (uni >= 32 && uni < 127) {
        if (uni >= 'A' && uni <= 'Z') uni += 32;
        return uni;
    }
    return 0;
}

void FSTP_KB_Save(void) {
    std::lock_guard<std::mutex> lk(g_mutex);
    EnsureLoaded();
    std::ofstream f(ConfigPath(), std::ios::trunc);
    if (!f.is_open()) return;
    f << "# TapeXPlayer key bindings (action_id keycode mods)\n";
    for (int i = 0; i < kActionCount; ++i) {
        if (kActions[i].editable)
            f << kActions[i].id << " " << g_curKey[i] << " " << g_curMod[i] << "\n";
    }
}

void FSTP_KB_Load(void) {
    std::lock_guard<std::mutex> lk(g_mutex);
    LoadLocked();
}

void FSTP_KB_TranslateEvent(int* sym, int* sdl_mods) {
    if (!sym || !sdl_mods) return;
    int key = *sym;
    if (key == 0) return;
    int nmod = NormalizeSDLMods(*sdl_mods);

    std::lock_guard<std::mutex> lk(g_mutex);
    EnsureLoaded();

    // 1. This combo is the current binding of some action -> emit its default.
    for (int i = 0; i < kActionCount; ++i) {
        if (g_curKey[i] == key && g_curMod[i] == nmod) {
            *sym = kActions[i].defaultKey;
            *sdl_mods = ToSDLMods(kActions[i].defaultMods);
            return;
        }
    }
    // 2. This combo is the default of an editable action that was remapped away
    //    -> neutralise so the hardcoded switch case doesn't fire.
    for (int i = 0; i < kActionCount; ++i) {
        if (kActions[i].editable &&
            kActions[i].defaultKey == key && kActions[i].defaultMods == nmod &&
            (g_curKey[i] != key || g_curMod[i] != nmod)) {
            *sym = 0;  // SDLK_UNKNOWN
            return;
        }
    }
    // 3. Pass through unchanged (keeps fixed modifier combos working).
}
