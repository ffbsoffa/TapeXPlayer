#include "FSTPLang.h"

#include <map>
#include <string>
#include <mutex>
#include <fstream>
#include <cstdlib>
#include <cctype>

#ifdef _WIN32
#include <windows.h>
#endif

// ---------------------------------------------------------------------------
// State. Two string tables: the built-in English defaults (registered in code)
// and the active language (populated from a pack). FSTP_Tr prefers the active
// table, then defaults, then the key itself. A mutex guards the rare writes
// (startup registration + pack load) against the per-frame reads from the
// render thread.
// ---------------------------------------------------------------------------
namespace {

std::mutex& mutex() {
    static std::mutex m;
    return m;
}

std::map<std::string, std::string>& defaults() {
    static std::map<std::string, std::string> m;
    return m;
}

std::map<std::string, std::string>& active() {
    static std::map<std::string, std::string> m;
    return m;
}

std::string& currentLocale() {
    static std::string s = "en";
    return s;
}

// Trim ASCII whitespace from both ends (UTF-8 safe: only touches < 0x80 bytes).
std::string trim(const std::string& in) {
    size_t b = 0, e = in.size();
    while (b < e && (unsigned char)in[b] <= ' ') ++b;
    while (e > b && (unsigned char)in[e - 1] <= ' ') --e;
    return in.substr(b, e - b);
}

// Interpret C-style escapes in a pack value so a single-line entry can carry
// line breaks: "\n" → newline, "\t" → tab, "\\" → backslash.
std::string unescape(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '\\' && i + 1 < in.size()) {
            char n = in[i + 1];
            if (n == 'n') { out += '\n'; ++i; continue; }
            if (n == 't') { out += '\t'; ++i; continue; }
            if (n == '\\') { out += '\\'; ++i; continue; }
        }
        out += in[i];
    }
    return out;
}

// Read the system locale's 2-letter prefix from the environment. Returns ""
// when nothing usable is found (typical for macOS GUI apps — those call
// FSTP_LangSetLocale explicitly instead).
std::string detectLocale() {
#ifdef _WIN32
    wchar_t buf[16] = {0};
    if (GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SISO639LANGNAME, buf, 16) > 0) {
        std::string out;
        for (int i = 0; i < 16 && buf[i]; ++i) out += (char)std::tolower((int)buf[i]);
        return out;
    }
    return "";
#else
    const char* vars[] = {"LANGUAGE", "LC_ALL", "LC_MESSAGES", "LANG"};
    for (const char* v : vars) {
        const char* val = std::getenv(v);
        if (val && *val) {
            std::string s(val);
            // Take the part before '_' / '.' / ':' → the language code.
            size_t cut = s.find_first_of("_.:");
            if (cut != std::string::npos) s = s.substr(0, cut);
            for (auto& c : s) c = (char)std::tolower((unsigned char)c);
            if (s.size() >= 2 && s != "c" && s != "posix") return s;
        }
    }
    return "";
#endif
}

bool g_locale_explicit = false;

} // namespace

// ---------------------------------------------------------------------------

const char* FSTP_Tr(const char* key) {
    if (!key) return "";
    std::lock_guard<std::mutex> lock(mutex());
    auto& act = active();
    auto it = act.find(key);
    if (it != act.end()) return it->second.c_str();
    auto& def = defaults();
    auto dit = def.find(key);
    if (dit != def.end()) return dit->second.c_str();
    return key; // visible fallback
}

void FSTP_LangRegisterDefault(const char* key, const char* english) {
    if (!key || !english) return;
    std::lock_guard<std::mutex> lock(mutex());
    defaults()[key] = english;
}

bool FSTP_LangLoadPack(const char* path) {
    if (!path) return false;
    std::ifstream f(path);
    if (!f.is_open()) return false;

    std::lock_guard<std::mutex> lock(mutex());
    std::string line;
    while (std::getline(f, line)) {
        // Strip UTF-8 BOM on the first line if present.
        if (line.size() >= 3 && (unsigned char)line[0] == 0xEF &&
            (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF) {
            line = line.substr(3);
        }
        std::string t = trim(line);
        if (t.empty() || t[0] == '#' || t[0] == ';') continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(t.substr(0, eq));
        std::string val = trim(t.substr(eq + 1));
        if (key.empty()) continue;
        if (key == "locale") { currentLocale() = val; continue; }
        if (key == "language") continue; // human-readable name, ignored at runtime
        active()[key] = unescape(val);
    }
    return true;
}

void FSTP_LangSetLocale(const char* locale_prefix) {
    if (!locale_prefix || !*locale_prefix) return;
    std::lock_guard<std::mutex> lock(mutex());
    std::string s(locale_prefix);
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    if (s.size() > 2) s = s.substr(0, 2);
    currentLocale() = s;
    g_locale_explicit = true;
}

void FSTP_LangAutoLoad(const char* lang_dir) {
    std::string loc;
    {
        std::lock_guard<std::mutex> lock(mutex());
        loc = g_locale_explicit ? currentLocale() : std::string();
    }
    if (loc.empty()) loc = detectLocale();
    if (loc.empty() || loc == "en") {
        // English is the built-in default — nothing to load.
        std::lock_guard<std::mutex> lock(mutex());
        currentLocale() = "en";
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex());
        currentLocale() = loc;
    }
    if (!lang_dir || !*lang_dir) return;
    std::string path = std::string(lang_dir);
    if (!path.empty() && path.back() != '/' && path.back() != '\\') path += '/';
    path += loc + ".lang";
    FSTP_LangLoadPack(path.c_str());
}

const char* FSTP_LangCurrent() {
    std::lock_guard<std::mutex> lock(mutex());
    return currentLocale().c_str();
}
