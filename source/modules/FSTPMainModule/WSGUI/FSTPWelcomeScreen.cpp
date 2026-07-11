#include "FSTPWelcomeScreen.h"
#include "FSTPLang.h"
#include "fontdata.h"   // embedded OTF used by the OSD (font_otf / font_otf_size)

#include <SDL2/SDL_ttf.h>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdlib>

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#endif

// The documentation link the "Open documentation" button opens.
static const char* kDocsURL = "https://docs.ffbsoffa.org";

// Platform modifier label used in the controls list.
#if defined(__APPLE__)
static const char* kMod = "Cmd";
#else
static const char* kMod = "Ctrl";
#endif

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
namespace {

std::atomic<bool> g_active{false};
std::once_flag    g_init_once;

TTF_Font* g_title_font = nullptr;  // heading
TTF_Font* g_body_font  = nullptr;  // paragraph / controls
TTF_Font* g_btn_font   = nullptr;  // button labels

int g_page = 0;            // 0 = intro, 1 = quick controls
const int kPageCount = 2;

// Footer buttons. Which are shown depends on the page.
enum Btn { BTN_NONE = 0, BTN_DOCS, BTN_BACK, BTN_NEXT, BTN_START };
int g_hover = BTN_NONE;

struct WLayout {
    SDL_Rect panel;
    SDL_Rect btn_docs;
    SDL_Rect btn_back;
    SDL_Rect btn_next;   // page 0 right button
    SDL_Rect btn_start;  // page 1 right button
};

// One row in the quick-controls list: a key label and a translated action.
struct CtrlRow { std::string key; const char* action_key; };

std::vector<CtrlRow> controlRows() {
    std::string mod = kMod;
    return {
        { mod + " O",              "welcome.act_open" },
        { mod + "+Shift+O",        "welcome.act_open_new" },
        { "Space",                 "welcome.act_playpause" },
        { "Left / Right",          "welcome.act_direction" },
        { "Shift + Left / Right",  "welcome.act_seek" },
        { "Up / Down",             "welcome.act_speed" },
        { "Shift+Ctrl+Click",      "welcome.act_shuttle" },
        { "F",                     "welcome.act_fullscreen" },
        { "C",                     "welcome.act_subs" },
        { mod + " Q / Esc",        "welcome.act_quit" },
    };
}

WLayout computeLayout(int W, int H) {
    WLayout L{};
    int pw = std::min(640, W - 80);
    int ph = std::min(500, H - 60);
    if (pw < 320) pw = std::max(W - 24, 260);
    if (ph < 300) ph = std::max(H - 24, 220);
    L.panel = SDL_Rect{ (W - pw) / 2, (H - ph) / 2, pw, ph };

    const int margin = 28;
    const int gap = 14;
    int bh = 42;
    int bw = std::min(190, (pw - 2 * margin - gap) / 2);
    if (bw < 110) bw = 110;
    int by = L.panel.y + L.panel.h - 24 - bh;

    L.btn_docs  = SDL_Rect{ L.panel.x + margin, by, bw, bh };
    int rightx  = L.panel.x + L.panel.w - margin - bw;
    L.btn_next  = SDL_Rect{ rightx, by, bw, bh };
    L.btn_start = SDL_Rect{ rightx, by, bw, bh };
    L.btn_back  = SDL_Rect{ rightx - gap - bw, by, bw, bh };
    return L;
}

bool pointIn(const SDL_Rect& r, int x, int y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

void initOnce() {
    // Built-in English defaults (the shipped language). A pack overrides these.
    FSTP_LangRegisterDefault("welcome.title", "Welcome to TapeXPlayer");
    FSTP_LangRegisterDefault("welcome.body",
        "TapeXPlayer plays video like a professional Betacam SP deck — built for "
        "careful, frame-by-frame study of footage, and as a tribute to tape.\n\n"
        "The tape look — noise, dropouts, horizontal-sync tearing — is modelled on "
        "purpose, not a bug. Turn the Betacam effect on for texture, or off for a "
        "clean signal.");
    FSTP_LangRegisterDefault("welcome.docs_hint", "Full guides & keyboard shortcuts:");
    FSTP_LangRegisterDefault("welcome.open_docs", "Open documentation");
    FSTP_LangRegisterDefault("welcome.get_started", "Get Started");
    FSTP_LangRegisterDefault("welcome.next", "Next");
    FSTP_LangRegisterDefault("welcome.back", "Back");

    FSTP_LangRegisterDefault("welcome.controls_title", "Quick controls");
    FSTP_LangRegisterDefault("welcome.act_open", "Open / load a file");
    FSTP_LangRegisterDefault("welcome.act_open_new", "Open in a new instance");
    FSTP_LangRegisterDefault("welcome.act_playpause", "Play / Pause");
    FSTP_LangRegisterDefault("welcome.act_direction", "Play backward / forward");
    FSTP_LangRegisterDefault("welcome.act_seek", "Jump back / forward 60 s");
    FSTP_LangRegisterDefault("welcome.act_speed", "Slow down / speed up");
    FSTP_LangRegisterDefault("welcome.act_shuttle", "Mouse shuttle — drag to scrub");
    FSTP_LangRegisterDefault("welcome.act_fullscreen", "Fullscreen");
    FSTP_LangRegisterDefault("welcome.act_subs", "Subtitles on / off");
    FSTP_LangRegisterDefault("welcome.act_quit", "Quit");
    FSTP_LangRegisterDefault("welcome.unload_note",
        "Mouse shuttle: the click point is the centre; speed depends on drag distance, "
        "or just hold the left button on compact devices. "
        "To unload, open another file (it replaces the current one) or close the window.");

    // Language packs are looked for in a lang/ folder. Prefer the explicit
    // FSTP_LANG_DIR override; otherwise the folder next to the executable
    // (SDL_GetBasePath is the app bundle Resources dir on macOS, the exe dir on
    // Windows/Linux); fall back to a CWD-relative "lang".
    const char* env_dir = std::getenv("FSTP_LANG_DIR");
    if (env_dir && *env_dir) {
        FSTP_LangAutoLoad(env_dir);
    } else if (char* base = SDL_GetBasePath()) {
        std::string dir = std::string(base) + "lang";
        SDL_free(base);
        FSTP_LangAutoLoad(dir.c_str());
    } else {
        FSTP_LangAutoLoad("lang");
    }

    // Fonts are opened from the same embedded OTF the OSD uses, at our sizes.
    if (TTF_WasInit() == 0) TTF_Init();
    g_title_font = TTF_OpenFontRW(SDL_RWFromConstMem(font_otf, font_otf_size), 1, 26);
    g_body_font  = TTF_OpenFontRW(SDL_RWFromConstMem(font_otf, font_otf_size), 1, 15);
    g_btn_font   = TTF_OpenFontRW(SDL_RWFromConstMem(font_otf, font_otf_size), 1, 15);
}

// Render UTF-8 text to a temporary texture and copy it at (x,y). When wrap > 0
// the text is word-wrapped to that pixel width. Textures are created fresh each
// frame so this is renderer-agnostic.
void drawText(SDL_Renderer* r, TTF_Font* font, const char* text, SDL_Color col,
              int x, int y, int wrap, int* out_w, int* out_h) {
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    if (!font || !text || !*text) return;
    SDL_Surface* s = wrap > 0 ? TTF_RenderUTF8_Blended_Wrapped(font, text, col, (Uint32)wrap)
                              : TTF_RenderUTF8_Blended(font, text, col);
    if (!s) return;
    SDL_Texture* t = SDL_CreateTextureFromSurface(r, s);
    int w = s->w, h = s->h;
    SDL_FreeSurface(s);
    if (!t) return;
    SDL_Rect d{ x, y, w, h };
    SDL_RenderCopy(r, t, nullptr, &d);
    SDL_DestroyTexture(t);
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
}

void drawButton(SDL_Renderer* r, const SDL_Rect& rect, const char* label,
                bool filled, bool hover) {
    SDL_Color bg = filled
        ? SDL_Color{ (Uint8)(hover ? 78 : 58), (Uint8)(hover ? 132 : 104), (Uint8)(hover ? 220 : 184), 255 }
        : SDL_Color{ (Uint8)(hover ? 62 : 46), (Uint8)(hover ? 64 : 48), (Uint8)(hover ? 72 : 54), 255 };
    SDL_SetRenderDrawColor(r, bg.r, bg.g, bg.b, 255);
    SDL_RenderFillRect(r, &rect);
    SDL_SetRenderDrawColor(r, 116, 116, 124, 255);
    SDL_RenderDrawRect(r, &rect);

    SDL_Color fg{ 236, 236, 240, 255 };
    int tw = 0, th = 0;
    if (g_btn_font && TTF_SizeUTF8(g_btn_font, label, &tw, &th) == 0) {
        drawText(r, g_btn_font, label, fg,
                 rect.x + (rect.w - tw) / 2, rect.y + (rect.h - th) / 2, 0, nullptr, nullptr);
    }
}

// Map a mouse event's window coordinates (points) to renderer pixels, so hit
// testing matches the pixel-space layout used for drawing (HiDPI safe).
bool eventToPixels(const SDL_Event* e, int* px, int* py, int* out_w, int* out_h) {
    Uint32 wid = 0;
    int mx = 0, my = 0;
    if (e->type == SDL_MOUSEBUTTONDOWN || e->type == SDL_MOUSEBUTTONUP) {
        wid = e->button.windowID; mx = e->button.x; my = e->button.y;
    } else if (e->type == SDL_MOUSEMOTION) {
        wid = e->motion.windowID; mx = e->motion.x; my = e->motion.y;
    } else {
        return false;
    }
    SDL_Window* win = SDL_GetWindowFromID(wid);
    if (!win) return false;
    SDL_Renderer* r = SDL_GetRenderer(win);
    int pw = 0, ph = 0, ow = 0, oh = 0;
    SDL_GetWindowSize(win, &pw, &ph);
    if (r) SDL_GetRendererOutputSize(r, &ow, &oh);
    if (ow == 0 || oh == 0) { ow = pw; oh = ph; }
    float sx = pw > 0 ? (float)ow / pw : 1.0f;
    float sy = ph > 0 ? (float)oh / ph : 1.0f;
    *px = (int)(mx * sx);
    *py = (int)(my * sy);
    *out_w = ow;
    *out_h = oh;
    return true;
}

void openDocs() {
#if defined(_WIN32)
    ShellExecuteA(NULL, "open", kDocsURL, NULL, NULL, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    std::string cmd = std::string("open \"") + kDocsURL + "\" >/dev/null 2>&1 &";
    std::system(cmd.c_str());
#else
    std::string cmd = std::string("xdg-open \"") + kDocsURL + "\" >/dev/null 2>&1 &";
    std::system(cmd.c_str());
#endif
}

// Hit-test the footer buttons that are visible on the current page.
int hitButton(const WLayout& L, int px, int py) {
    if (pointIn(L.btn_docs, px, py)) return BTN_DOCS;
    if (g_page == 0) {
        if (pointIn(L.btn_next, px, py)) return BTN_NEXT;
    } else {
        if (pointIn(L.btn_back, px, py)) return BTN_BACK;
        if (pointIn(L.btn_start, px, py)) return BTN_START;
    }
    return BTN_NONE;
}

void activate(int btn) {
    switch (btn) {
        case BTN_DOCS:  openDocs(); break;
        case BTN_NEXT:  g_page = 1; g_hover = BTN_NONE; break;
        case BTN_BACK:  g_page = 0; g_hover = BTN_NONE; break;
        case BTN_START: FSTPWelcome_Hide(); break;
        default: break;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void FSTPWelcome_Show() {
    std::call_once(g_init_once, initOnce);
    g_page = 0;
    g_hover = BTN_NONE;
    g_active.store(true);
}

void FSTPWelcome_Hide() {
    g_active.store(false);
}

bool FSTPWelcome_IsActive() {
    return g_active.load();
}

void FSTPWelcome_Render(SDL_Renderer* renderer, int W, int H) {
    if (!g_active.load() || !renderer || W <= 0 || H <= 0) return;
    std::call_once(g_init_once, initOnce);

    WLayout L = computeLayout(W, H);

    SDL_BlendMode prev;
    SDL_GetRenderDrawBlendMode(renderer, &prev);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    // Dim the whole window behind the panel.
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 175);
    SDL_Rect full{ 0, 0, W, H };
    SDL_RenderFillRect(renderer, &full);

    // Panel.
    SDL_SetRenderDrawColor(renderer, 26, 26, 29, 250);
    SDL_RenderFillRect(renderer, &L.panel);
    SDL_SetRenderDrawColor(renderer, 92, 92, 98, 255);
    SDL_RenderDrawRect(renderer, &L.panel);
    SDL_SetRenderDrawColor(renderer, 58, 104, 184, 255);
    SDL_Rect accent{ L.panel.x, L.panel.y, L.panel.w, 4 };
    SDL_RenderFillRect(renderer, &accent);

    const int padX = 32;
    int contentX = L.panel.x + padX;
    int contentW = L.panel.w - 2 * padX;

    SDL_Color white{ 240, 240, 244, 255 };
    SDL_Color grey{ 188, 188, 196, 255 };
    SDL_Color dim{ 150, 150, 158, 255 };
    SDL_Color accentText{ 122, 170, 230, 255 };

    int line = TTF_FontLineSkip(g_body_font);
    int dots_y = L.btn_docs.y - 18;       // page dots sit just above the buttons
    int y = L.panel.y + 26;
    int tw = 0, th = 0;

    if (g_page == 0) {
        drawText(renderer, g_title_font, FSTP_Tr("welcome.title"), white, contentX, y, 0, &tw, &th);
        y += th + 18;
        drawText(renderer, g_body_font, FSTP_Tr("welcome.body"), grey, contentX, y, contentW, nullptr, nullptr);
        // Docs hint on its own line just above the page dots.
        drawText(renderer, g_body_font, FSTP_Tr("welcome.docs_hint"), dim,
                 contentX, dots_y - 8 - line, 0, nullptr, nullptr);
    } else {
        drawText(renderer, g_title_font, FSTP_Tr("welcome.controls_title"), white, contentX, y, 0, &tw, &th);
        y += th + 10;

        // Reserve space for the (wrapped) bottom note above the dots, then fit the
        // control rows in whatever remains — so nothing overlaps at any window size.
        // The note now carries the mouse-shuttle details, so it wraps to ~3 lines.
        int note_reserve = 3 * line + 6;
        int note_top = dots_y - 8 - note_reserve;
        int rows_top = y;
        std::vector<CtrlRow> rows = controlRows();
        int n = (int)rows.size();
        int row_h = (n > 0) ? (note_top - 8 - rows_top) / n : line;
        if (row_h > line + 4) row_h = line + 4;
        if (row_h < 13) row_h = 13;
        // Size the key column to the widest key label so long combos like
        // "Shift + Left / Right" never overlap the action text. Cap it at half
        // the content width so the action column always keeps room.
        int key_col_w = 0;
        for (const auto& row : rows) {
            int kw = 0, kh = 0;
            if (g_body_font && TTF_SizeUTF8(g_body_font, row.key.c_str(), &kw, &kh) == 0)
                key_col_w = std::max(key_col_w, kw);
        }
        key_col_w += 28;                       // gap between key and action columns
        if (key_col_w > contentW / 2) key_col_w = contentW / 2;
        int ry = rows_top;
        for (const auto& row : rows) {
            drawText(renderer, g_body_font, row.key.c_str(), accentText, contentX, ry, 0, nullptr, nullptr);
            drawText(renderer, g_body_font, FSTP_Tr(row.action_key), grey, contentX + key_col_w, ry, 0, nullptr, nullptr);
            ry += row_h;
        }
        drawText(renderer, g_body_font, FSTP_Tr("welcome.unload_note"), dim,
                 contentX, note_top, contentW, nullptr, nullptr);
    }

    // Page dots.
    int dotsY = dots_y;
    int dotR = 4, dotGap = 14;
    int dotsW = kPageCount * dotR * 2 + (kPageCount - 1) * (dotGap - dotR * 2);
    int dotX = L.panel.x + (L.panel.w - dotsW) / 2;
    for (int i = 0; i < kPageCount; ++i) {
        if (i == g_page) SDL_SetRenderDrawColor(renderer, 122, 170, 230, 255);
        else SDL_SetRenderDrawColor(renderer, 90, 90, 96, 255);
        SDL_Rect dot{ dotX, dotsY, dotR * 2, dotR * 2 };
        SDL_RenderFillRect(renderer, &dot);
        dotX += dotGap;
    }

    // Footer buttons.
    drawButton(renderer, L.btn_docs, FSTP_Tr("welcome.open_docs"), false, g_hover == BTN_DOCS);
    if (g_page == 0) {
        drawButton(renderer, L.btn_next, FSTP_Tr("welcome.next"), true, g_hover == BTN_NEXT);
    } else {
        drawButton(renderer, L.btn_back, FSTP_Tr("welcome.back"), false, g_hover == BTN_BACK);
        drawButton(renderer, L.btn_start, FSTP_Tr("welcome.get_started"), true, g_hover == BTN_START);
    }

    SDL_SetRenderDrawBlendMode(renderer, prev);
}

bool FSTPWelcome_HandleEvent(const SDL_Event* event) {
    if (!g_active.load() || !event) return false;

    switch (event->type) {
        case SDL_MOUSEMOTION: {
            int px, py, w, h;
            if (eventToPixels(event, &px, &py, &w, &h)) {
                WLayout L = computeLayout(w, h);
                g_hover = hitButton(L, px, py);
            }
            return true;
        }
        case SDL_MOUSEBUTTONDOWN: {
            if (event->button.button != SDL_BUTTON_LEFT) return true;
            int px, py, w, h;
            if (eventToPixels(event, &px, &py, &w, &h)) {
                WLayout L = computeLayout(w, h);
                activate(hitButton(L, px, py));
            }
            return true;
        }
        case SDL_MOUSEBUTTONUP:
            return true;
        case SDL_KEYDOWN: {
            // Let the global quit chords through (Cmd+Q / Ctrl+Q).
            if (event->key.keysym.mod & (KMOD_GUI | KMOD_CTRL)) return false;
            SDL_Keycode k = event->key.keysym.sym;
            if (k == SDLK_ESCAPE) { FSTPWelcome_Hide(); return true; }
            if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE || k == SDLK_RIGHT) {
                if (g_page < kPageCount - 1) g_page++;
                else FSTPWelcome_Hide();
                return true;
            }
            if (k == SDLK_LEFT) { if (g_page > 0) g_page--; return true; }
            // Modal: swallow everything else so playback controls don't fire.
            return true;
        }
        case SDL_KEYUP:
            return true;
        default:
            return false;
    }
}
