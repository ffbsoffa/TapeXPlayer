#ifndef FSTP_EXTENSIONS_H
#define FSTP_EXTENSIONS_H

// ---------------------------------------------------------------------------
// FSTPExtensions — the extension registry.
//
// Goal (Milestone 6): a clear, flexible extension system that a human OR an AI
// can author against. An extension is described by a tiny text manifest, so no
// recompilation is needed to add one and the format is trivial to generate.
//
// Two kinds of extension share one registry:
//   * BUILT-IN — implemented in C++ and registered in code (the first one is
//     "subtitles", see FSTPSubtitles). These ship with the player.
//   * EXTERNAL — discovered at runtime in an extensions/ folder. Each lives in
//     its own subdirectory with a "manifest.txt":
//
//         id          = my-overlay
//         name        = My Overlay
//         version     = 1.0.0
//         author      = Someone
//         description = Draws a custom overlay
//         type        = lua          ; how the entry is executed
//         entry       = main.lua     ; script / payload, relative to the dir
//         enabled     = true
//
// The registry itself only tracks metadata + on/off state and is what a UI
// (Settings ▸ Extensions) lists. EXECUTION of a given type is the executor's
// job: "builtin" is wired in code today; a "lua" executor is the planned next
// step (a small embedded Lua VM calling a documented host API). Until that
// executor exists, external "lua" extensions are listed but not run.
// ---------------------------------------------------------------------------

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char id[64];
    char name[128];
    char version[32];
    char author[128];
    char description[256];
    char type[32];        // "builtin", "lua", ...
    bool builtin;         // true = shipped, implemented in C++
    bool enabled;
} FSTP_ExtensionInfo;

// Build the registry: register the built-in extensions and scan the extensions
// directory (FSTP_EXT_DIR, else <exe-dir>/extensions). Safe to call more than
// once; subsequent calls re-scan. Called lazily by the query functions too.
void FSTPExt_Init(void);

// How many extensions are registered.
int FSTPExt_Count(void);

// Copy the i-th extension's info. Returns false for an out-of-range index.
bool FSTPExt_Get(int index, FSTP_ExtensionInfo* out);

// Toggle an extension by id. For built-ins this also drives the feature (e.g.
// "subtitles" → FSTPSubtitles_SetEnabled). Returns false for an unknown id.
bool FSTPExt_SetEnabled(const char* id, bool enabled);
bool FSTPExt_IsEnabled(const char* id);

#ifdef __cplusplus
}
#endif

#endif // FSTP_EXTENSIONS_H
