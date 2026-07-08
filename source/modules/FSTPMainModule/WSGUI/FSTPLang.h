#ifndef FSTP_LANG_H
#define FSTP_LANG_H

// ---------------------------------------------------------------------------
// FSTPLang — minimal cross-platform localization core.
//
// Design (Milestone 6, language packs):
//   * The OFFICIAL build ships ONLY built-in English defaults, registered in
//     code via FSTP_LangRegisterDefault(). There are no bundled translations.
//   * A user adds a language by dropping a plain-text pack into the lang/
//     directory (next to the executable / in resources). Packs are matched
//     against the system locale and override the English defaults.
//   * Anything still missing falls back to English, and if even that is absent
//     the key itself is returned — so a missing string is visible, never blank.
//
// Pack file format (UTF-8, "<dir>/<locale>.lang", e.g. lang/fr.lang):
//     # comment line
//     language = Francais           ; optional human-readable name
//     locale   = fr                 ; locale prefix this pack serves
//     welcome.title = Bienvenue dans TapeXPlayer
//     welcome.body  = ...
//
// Keys use a dotted namespace ("welcome.title", "settings.keyboard", ...).
// The format is intentionally trivial so a human OR an AI can author a pack.
// ---------------------------------------------------------------------------

#ifdef __cplusplus
extern "C" {
#endif

// Translate a key. Returns the active-language value when a pack defines it,
// otherwise the built-in English default, otherwise the key string itself.
// The returned pointer stays valid until the next FSTP_LangLoadPack().
const char* FSTP_Tr(const char* key);

// Register the built-in English default for a key. Call once at startup for
// every key the UI uses. Safe to call repeatedly (last write wins).
void FSTP_LangRegisterDefault(const char* key, const char* english);

// Load a language pack file, overriding defaults for the keys it contains.
// Returns true if the file was opened and parsed.
bool FSTP_LangLoadPack(const char* path);

// Override the detected locale (2-letter prefix, e.g. "ru"). Platforms whose
// locale can't be read from the environment (macOS GUI apps) call this before
// FSTP_LangAutoLoad with the value from the system API.
void FSTP_LangSetLocale(const char* locale_prefix);

// Detect the system locale (unless already set) and load a matching pack
// "<lang_dir>/<locale>.lang". No-op when no pack matches — the UI stays
// English. Safe to call with a directory that does not exist.
void FSTP_LangAutoLoad(const char* lang_dir);

// The 2-letter locale prefix the UI settled on ("en", "ru", ...).
const char* FSTP_LangCurrent();

#ifdef __cplusplus
}
#endif

#endif // FSTP_LANG_H
