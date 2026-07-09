#include "FSTPExtensions.h"
#include "FSTPSubtitles.h"      // the first built-in extension
#include "FSTPLuaExtension.h"   // executor for type=lua extensions

#include <SDL2/SDL.h>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <fstream>
#include <cstring>
#include <cstdlib>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

namespace {

struct Ext {
    std::string id, name, version, author, description, type;
    std::string dir, entry;   // for external extensions: directory + entry script
    bool builtin = false;
    bool enabled = true;
};

std::mutex g_mutex;
std::vector<Ext> g_exts;
bool g_inited = false;

std::string trim(const std::string& in) {
    size_t b = 0, e = in.size();
    while (b < e && (unsigned char)in[b] <= ' ') ++b;
    while (e > b && (unsigned char)in[e - 1] <= ' ') --e;
    return in.substr(b, e - b);
}

void copyField(char* dst, size_t cap, const std::string& src) {
    std::strncpy(dst, src.c_str(), cap - 1);
    dst[cap - 1] = '\0';
}

// Parse a manifest.txt (key=value) into an Ext. Returns false if it lacks an id.
bool parseManifest(const std::string& path, Ext& out) {
    std::ifstream f(path);
    if (!f.is_open()) return false;
    out = Ext{};
    out.enabled = true;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string t = trim(line);
        if (t.empty() || t[0] == '#' || t[0] == ';') continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(t.substr(0, eq));
        std::string v = trim(t.substr(eq + 1));
        if (k == "id") out.id = v;
        else if (k == "name") out.name = v;
        else if (k == "version") out.version = v;
        else if (k == "author") out.author = v;
        else if (k == "description") out.description = v;
        else if (k == "type") out.type = v;
        else if (k == "entry") out.entry = v;
        else if (k == "enabled") out.enabled = (v == "true" || v == "1" || v == "yes");
    }
    if (out.id.empty()) return false;
    if (out.type.empty()) out.type = "lua";
    if (out.name.empty()) out.name = out.id;
    return true;
}

// List immediate subdirectories of dir.
std::vector<std::string> subdirs(const std::string& dir) {
    std::vector<std::string> out;
#if defined(_WIN32)
    std::string pattern = dir + "\\*";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                std::strcmp(fd.cFileName, ".") && std::strcmp(fd.cFileName, "..")) {
                out.push_back(dir + "\\" + fd.cFileName);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    DIR* d = opendir(dir.c_str());
    if (d) {
        struct dirent* ent;
        while ((ent = readdir(d)) != nullptr) {
            if (!std::strcmp(ent->d_name, ".") || !std::strcmp(ent->d_name, "..")) continue;
            std::string full = dir + "/" + ent->d_name;
            struct stat st;
            if (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) out.push_back(full);
        }
        closedir(d);
    }
#endif
    return out;
}

std::string extensionsDir() {
    const char* env = std::getenv("FSTP_EXT_DIR");
    if (env && *env) return env;
    if (char* base = SDL_GetBasePath()) {
        std::string d = std::string(base) + "extensions";
        SDL_free(base);
        return d;
    }
    return "extensions";
}

void initLocked() {
    g_exts.clear();

    // --- Built-in extensions -------------------------------------------------
    Ext subs;
    subs.id = "subtitles";
    subs.name = "Subtitles";
    subs.version = "1.0.0";
    subs.author = "TapeXPlayer";
    subs.description = "Sidecar .srt subtitles, synced to the player position.";
    subs.type = "builtin";
    subs.builtin = true;
    subs.enabled = FSTPSubtitles_IsEnabled();
    g_exts.push_back(subs);

    // --- External extensions (extensions/<name>/manifest.txt) ----------------
    std::string dir = extensionsDir();
    char sep = '/';
#if defined(_WIN32)
    sep = '\\';
#endif
    for (const std::string& sub : subdirs(dir)) {
        Ext e;
        if (parseManifest(sub + sep + "manifest.txt", e)) {
            e.builtin = false;
            e.dir = sub;
            // Don't let an external manifest collide with a built-in id.
            bool dup = false;
            for (const auto& x : g_exts) if (x.id == e.id) { dup = true; break; }
            if (dup) continue;
            g_exts.push_back(e);
            // Start enabled lua extensions immediately.
            if (e.type == "lua" && e.enabled && !e.entry.empty()) {
                FSTPLua_Load(e.id.c_str(), e.dir.c_str(), e.entry.c_str());
            }
        }
    }

    g_inited = true;
}

} // namespace

void FSTPExt_Init(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    initLocked();
}

int FSTPExt_Count(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_inited) initLocked();
    return (int)g_exts.size();
}

bool FSTPExt_Get(int index, FSTP_ExtensionInfo* out) {
    if (!out) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_inited) initLocked();
    if (index < 0 || index >= (int)g_exts.size()) return false;
    const Ext& e = g_exts[index];
    copyField(out->id, sizeof(out->id), e.id);
    copyField(out->name, sizeof(out->name), e.name);
    copyField(out->version, sizeof(out->version), e.version);
    copyField(out->author, sizeof(out->author), e.author);
    copyField(out->description, sizeof(out->description), e.description);
    copyField(out->type, sizeof(out->type), e.type);
    out->builtin = e.builtin;
    out->enabled = e.enabled;
    return true;
}

bool FSTPExt_SetEnabled(const char* id, bool enabled) {
    if (!id) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_inited) initLocked();
    for (auto& e : g_exts) {
        if (e.id == id) {
            e.enabled = enabled;
            // Built-ins drive their feature directly; lua extensions are (un)loaded.
            if (e.id == "subtitles") {
                FSTPSubtitles_SetEnabled(enabled);
            } else if (e.type == "lua") {
                if (enabled && !e.entry.empty())
                    FSTPLua_Load(e.id.c_str(), e.dir.c_str(), e.entry.c_str());
                else
                    FSTPLua_Unload(e.id.c_str());
            }
            return true;
        }
    }
    return false;
}

bool FSTPExt_IsEnabled(const char* id) {
    if (!id) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_inited) initLocked();
    for (const auto& e : g_exts) if (e.id == id) return e.enabled;
    return false;
}
