#ifndef FSTP_KEY_BINDINGS_H
#define FSTP_KEY_BINDINGS_H

// Rebindable keyboard shortcuts (key + optional modifiers).
//
// Design: the main keyboard handler keeps its existing switch on the DEFAULT
// keys. This registry only translates an incoming (key, mods) to the canonical
// default (key, mods) of whatever action it is bound to (and neutralises a
// default whose action was remapped away). So transport/shuttle logic is never
// touched — only key identity is remapped.
//
// Keycodes are SDL_Keycode values; modifiers use the FSTP_MOD_* bits below.
// (Command/⌘ is intentionally NOT bindable — it is reserved for menu shortcuts.)

#define FSTP_MOD_SHIFT 1
#define FSTP_MOD_CTRL  2
#define FSTP_MOD_ALT   4

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KA_NONE = 0,
    KA_PLAY_PAUSE,
    KA_STOP,
    KA_REVERSE,
    KA_GO_START,
    KA_GO_END,
    KA_SPEED_1X,
    KA_SPEED_3X,
    KA_SPEED_UP,
    KA_SPEED_DOWN,
    KA_MUTE,
    KA_TIMEFRAME,
    KA_FULLSCREEN,
    KA_MARKER_NEW,      // fixed (Enter is shared with modal confirm)
    KA_MARKER_PREV,
    KA_MARKER_NEXT,
    KA_MARKER_RECALL,   // numpad '.' trigger
    KA_TIMECODE_INPUT,  // numpad '*' trigger
    KA_COUNT
} KeyActionId;

// --- Metadata (settings UI) -------------------------------------------------
int         FSTP_KB_GetActionCount(void);
int         FSTP_KB_GetActionIdByIndex(int index);
const char* FSTP_KB_GetActionName(int action_id);
const char* FSTP_KB_GetActionGroup(int action_id);
int         FSTP_KB_IsActionEditable(int action_id);

// --- Bindings ---------------------------------------------------------------
int         FSTP_KB_GetKeycode(int action_id);        // current SDL keycode
int         FSTP_KB_GetMods(int action_id);           // current FSTP_MOD_* mask
int         FSTP_KB_GetDefaultKeycode(int action_id);
int         FSTP_KB_GetDefaultMods(int action_id);
const char* FSTP_KB_GetKeyName(int keycode);          // SDL key label (no mods)
// Assign a combo. Returns 0 on success; -1 on conflict (out_conflict_action set
// to the action already using it); -2 if the action is not editable.
int         FSTP_KB_SetBinding(int action_id, int keycode, int mods, int* out_conflict_action);
void        FSTP_KB_ResetToDefaults(void);

// Map a macOS hardware key (NSEvent.keyCode + unshifted unicode char) to an
// SDL keycode. Lives here so the SDLK_* table stays in C++ (Swift calls it).
int         FSTP_KB_MacKeyToSDL(int mac_keycode, int unicode_char);

// --- Persistence ------------------------------------------------------------
void        FSTP_KB_Save(void);
void        FSTP_KB_Load(void);

// --- Used by the keyboard handler -------------------------------------------
// In/out: SDL keycode and SDL KMOD mask. Rewrites them to the bound action's
// canonical default, or sets *sym = 0 to swallow a remapped default.
void        FSTP_KB_TranslateEvent(int* sym, int* sdl_mods);

#ifdef __cplusplus
}
#endif

#endif // FSTP_KEY_BINDINGS_H
