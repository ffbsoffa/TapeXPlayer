#include "FSTPSubtitles.h"
#include "fontdata.h"   // embedded OTF (font_otf / font_otf_size)
#include "../FSTPPlayerModule/FSTPPlayerManager.h"  // GetInstanceFilePath

#include <SDL2/SDL_ttf.h>
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <cstdio>

// ---------------------------------------------------------------------------
// Model
// ---------------------------------------------------------------------------
namespace {

struct Cue {
    double start = 0.0;   // seconds
    double end = 0.0;     // seconds
    std::string text;     // may contain '\n' for multi-line
};

struct Track {
    std::string source_video;       // the video path this track was auto-loaded for
    std::string srt_path;           // the .srt actually loaded ("" if none)
    std::vector<Cue> cues;          // sorted by start
    int last_idx = -1;              // search hint
};

std::mutex g_mutex;
std::map<int, Track> g_tracks;      // per player_id
std::atomic<bool> g_enabled{true};

TTF_Font* g_font = nullptr;
int g_font_px = 0;

// ---- SRT parsing --------------------------------------------------------

// "HH:MM:SS,mmm" (or '.') → seconds. Returns -1 on failure.
double parseTimestamp(const std::string& s) {
    int h = 0, m = 0, sec = 0, ms = 0;
    // Accept both ',' (standard SRT) and '.' as the millisecond separator.
    if (std::sscanf(s.c_str(), "%d:%d:%d,%d", &h, &m, &sec, &ms) == 4 ||
        std::sscanf(s.c_str(), "%d:%d:%d.%d", &h, &m, &sec, &ms) == 4) {
        return h * 3600.0 + m * 60.0 + sec + ms / 1000.0;
    }
    return -1.0;
}

std::string stripCR(std::string s) {
    if (!s.empty() && s.back() == '\r') s.pop_back();
    return s;
}

// Remove the most common SRT/HTML inline tags (<i>, <b>, <font ...>) so they
// don't show as literal text. Keeps it simple — not a full HTML parser.
std::string stripTags(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    bool in_tag = false;
    for (char c : in) {
        if (c == '<') { in_tag = true; continue; }
        if (c == '>') { in_tag = false; continue; }
        if (!in_tag) out += c;
    }
    return out;
}

std::vector<Cue> parseSRT(const std::string& path) {
    std::vector<Cue> cues;
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return cues;

    std::string line;
    bool first = true;
    Cue cur;
    bool have_time = false;
    std::string text_accum;

    auto flush = [&]() {
        if (have_time && !text_accum.empty()) {
            cur.text = text_accum;
            cues.push_back(cur);
        }
        cur = Cue{};
        have_time = false;
        text_accum.clear();
    };

    while (std::getline(f, line)) {
        line = stripCR(line);
        if (first) {
            // Strip UTF-8 BOM.
            if (line.size() >= 3 && (unsigned char)line[0] == 0xEF &&
                (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF)
                line = line.substr(3);
            first = false;
        }

        if (line.find("-->") != std::string::npos) {
            // Timing line: "start --> end"
            size_t arrow = line.find("-->");
            std::string a = line.substr(0, arrow);
            std::string b = line.substr(arrow + 3);
            // trim spaces
            auto trim = [](std::string& x){
                size_t i = x.find_first_not_of(" \t");
                size_t j = x.find_last_not_of(" \t");
                x = (i == std::string::npos) ? "" : x.substr(i, j - i + 1);
            };
            trim(a); trim(b);
            // 'b' may carry positioning info after the timestamp — cut at first space.
            size_t sp = b.find(' ');
            if (sp != std::string::npos) b = b.substr(0, sp);
            double s = parseTimestamp(a), e = parseTimestamp(b);
            if (s >= 0 && e >= 0) { cur.start = s; cur.end = e; have_time = true; text_accum.clear(); }
            continue;
        }

        if (line.empty()) {
            flush();
            continue;
        }

        // A bare integer line right before a timing block is the cue index; skip
        // it only when we don't yet have a time (avoids eating numeric subtitles).
        if (!have_time) {
            bool all_digits = !line.empty();
            for (char c : line) if (c < '0' || c > '9') { all_digits = false; break; }
            if (all_digits) continue;
        }

        if (have_time) {
            std::string t = stripTags(line);
            if (!text_accum.empty()) text_accum += '\n';
            text_accum += t;
        }
    }
    flush();

    std::sort(cues.begin(), cues.end(), [](const Cue& x, const Cue& y){ return x.start < y.start; });
    return cues;
}

// Replace a path's extension with ".srt".
std::string sidecarPath(const std::string& video) {
    size_t slash = video.find_last_of("/\\");
    size_t dot = video.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return video + ".srt";
    return video.substr(0, dot) + ".srt";
}

bool fileExists(const std::string& p) {
    std::ifstream f(p);
    return f.good();
}

// Find the active cue index for time t in a sorted, mostly non-overlapping list.
int findCue(const std::vector<Cue>& cues, double t, int hint) {
    if (cues.empty()) return -1;
    // Fast path: the hint cue or its neighbour still matches (monotonic playback).
    if (hint >= 0 && hint < (int)cues.size()) {
        if (t >= cues[hint].start && t < cues[hint].end) return hint;
        if (hint + 1 < (int)cues.size() &&
            t >= cues[hint + 1].start && t < cues[hint + 1].end) return hint + 1;
    }
    // Binary search: last cue with start <= t.
    int lo = 0, hi = (int)cues.size() - 1, idx = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (cues[mid].start <= t) { idx = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    if (idx >= 0 && t < cues[idx].end) return idx;
    return -1;
}

void ensureFont(int px) {
    if (g_font && g_font_px == px) return;
    if (g_font) { TTF_CloseFont(g_font); g_font = nullptr; }
    if (TTF_WasInit() == 0) TTF_Init();
    g_font = TTF_OpenFontRW(SDL_RWFromConstMem(font_otf, font_otf_size), 1, px);
    g_font_px = px;
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void FSTPSubtitles_SetEnabled(bool enabled) { g_enabled.store(enabled); }
bool FSTPSubtitles_IsEnabled(void) { return g_enabled.load(); }
void FSTPSubtitles_Toggle(void) { g_enabled.store(!g_enabled.load()); }

bool FSTPSubtitles_LoadForPlayer(int player_id, const char* srt_path) {
    if (!srt_path) return false;
    std::vector<Cue> cues = parseSRT(srt_path);
    std::lock_guard<std::mutex> lock(g_mutex);
    Track& tr = g_tracks[player_id];
    tr.srt_path = srt_path;
    tr.cues = std::move(cues);
    tr.last_idx = -1;
    return !tr.cues.empty();
}

void FSTPSubtitles_ClearForPlayer(int player_id) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_tracks.find(player_id);
    if (it != g_tracks.end()) { it->second.cues.clear(); it->second.srt_path.clear(); it->second.last_idx = -1; }
}

bool FSTPSubtitles_HasTrack(int player_id) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_tracks.find(player_id);
    return it != g_tracks.end() && !it->second.cues.empty();
}

void FSTPSubtitles_Render(SDL_Renderer* renderer, int player_id,
                          double time_seconds, int win_w, int win_h) {
    if (!g_enabled.load() || !renderer || player_id < 0 || win_w <= 0 || win_h <= 0) return;

    std::string text;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        Track& tr = g_tracks[player_id];

        // Lazy auto-load: if the player's file changed, look for a sidecar .srt.
        const char* fp = GetInstanceFilePath(player_id);
        std::string cur_video = fp ? fp : "";
        if (cur_video != tr.source_video) {
            tr.source_video = cur_video;
            tr.cues.clear();
            tr.srt_path.clear();
            tr.last_idx = -1;
            if (!cur_video.empty()) {
                std::string side = sidecarPath(cur_video);
                if (fileExists(side)) {
                    tr.cues = parseSRT(side);
                    tr.srt_path = side;
                }
            }
        }

        if (tr.cues.empty()) return;
        int idx = findCue(tr.cues, time_seconds, tr.last_idx);
        tr.last_idx = idx;
        if (idx < 0) return;
        text = tr.cues[idx].text;
    }
    if (text.empty()) return;

    // Size the font to the window height (clamped); reopen only on change.
    // Smaller than before so it sits unobtrusively above the OSD timecode.
    int px = (int)(win_h * 0.028);
    if (px < 12) px = 12;
    if (px > 40) px = 40;
    ensureFont(px);
    if (!g_font) return;

    // Split into lines.
    std::vector<std::string> lines;
    {
        std::stringstream ss(text);
        std::string ln;
        while (std::getline(ss, ln, '\n')) lines.push_back(ln);
    }
    if (lines.empty()) return;

    SDL_Color fg{ 255, 255, 255, 255 };
    int line_h = TTF_FontLineSkip(g_font);
    int total_h = line_h * (int)lines.size();
    // Sit ABOVE the OSD timecode (which the OSD draws at a fixed win_h - 80) so
    // subtitles never collide with the timecode / playback-state readout.
    const int osd_reserve = 92;
    int start_y = win_h - osd_reserve - total_h;
    if (start_y < 8) start_y = 8;

    // Backing box behind the whole block, sized to the widest line.
    int max_w = 0;
    std::vector<SDL_Texture*> texs;
    std::vector<SDL_Rect> rects;
    texs.reserve(lines.size());
    for (const auto& ln : lines) {
        SDL_Texture* t = nullptr;
        int tw = 0, th = line_h;
        if (!ln.empty()) {
            SDL_Surface* surf = TTF_RenderUTF8_Blended(g_font, ln.c_str(), fg);
            if (surf) {
                tw = surf->w; th = surf->h;
                t = SDL_CreateTextureFromSurface(renderer, surf);
                SDL_FreeSurface(surf);
            }
        }
        texs.push_back(t);
        rects.push_back(SDL_Rect{ 0, 0, tw, th });
        if (tw > max_w) max_w = tw;
    }

    SDL_BlendMode prev;
    SDL_GetRenderDrawBlendMode(renderer, &prev);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    int pad_x = px / 2, pad_y = px / 4;
    SDL_Rect box{ (win_w - max_w) / 2 - pad_x, start_y - pad_y,
                  max_w + 2 * pad_x, total_h + 2 * pad_y };
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 140);
    SDL_RenderFillRect(renderer, &box);

    int y = start_y;
    for (size_t i = 0; i < texs.size(); ++i) {
        if (texs[i]) {
            SDL_Rect d{ (win_w - rects[i].w) / 2, y, rects[i].w, rects[i].h };
            SDL_RenderCopy(renderer, texs[i], nullptr, &d);
            SDL_DestroyTexture(texs[i]);
        }
        y += line_h;
    }

    SDL_SetRenderDrawBlendMode(renderer, prev);
}
