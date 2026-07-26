#include "FSTPOSDSystem.h"
#include "FSTPSettings.h"
#include "fontdata.h"
#include "FSTPKeyboard.h"
#include "../FSTPPlayerModule/FSTPPlayerManager.h"
#include <cmath>
#include <algorithm>
#include <string>
#include <cstdio>
#include <iostream>
#include <unordered_map>
#include <map>
#include <set>
#include <chrono>

// Global OSD system variables
static SDL_Renderer* g_osd_renderer = nullptr;
static TTF_Font* g_large_font = nullptr;
static TTF_Font* g_normal_font = nullptr;
static TTF_Font* g_small_font = nullptr;


// Arrays for tracking loading state of each player
#define MAX_PLAYERS 5
static bool g_player_loading_states[MAX_PLAYERS] = {false};
static int g_player_loading_progress[MAX_PLAYERS] = {0};

// Structure for OSD data for each player
struct PlayerOSDData {
    double current_time = 0.0;
    double playback_rate = 1.0;        // Target speed
    double actual_playback_rate = 1.0; // Actual speed from audio module
    bool is_reverse = false;
    bool is_playing = false;
    bool jog_forward = false;
    bool jog_backward = false;
    bool seeking = false;
    std::string input_timecode = "";

    // Mode stabilization during animation
    std::string last_stable_mode = "still";
    int mode_stability_counter = 0;

    // Audio data with peak values
    float audio_left = 0.0f;
    float audio_right = 0.0f;
    float audio_left_peak = 0.0f;
    float audio_right_peak = 0.0f;

    // Full-resolution mode indicator (for "LOCK" display)
    bool is_full_res = false;

    // Position
    double total_duration = 100.0;

    // Display mode and loading
    OSDDisplayMode display_mode = OSD_MODE_NO_FILE;
    int loading_progress = 0;
    std::string loading_status = "threading";  // Stage text: threading, indexing, proxy
    bool is_audio_file = false;

    // TAPE THREADING badge: background proxy conversion progress (-1 = hidden).
    // Set from the conversion thread while the video is already playing.
    int proxy_threading_progress = -1;

    // Toggle between time and frame numbers
    bool show_frame_numbers = false;  // false = time (00:00:00:00), true = frames (0000000000)
    int current_frame_number = 0;     // Frame number from audio module

    // Real file FPS (for correct timecode display)
    double fps = 25.0;  // Default PAL, but updated when file is loaded

    // Timecode offset from file metadata (e.g. 00:59:30:00 = 3570s)
    double timecode_offset_seconds = 0.0;

    // Smoothing for VU meters
    float smooth_left = 0.0f;
    float smooth_right = 0.0f;
    float smooth_left_peak = 0.0f;
    float smooth_right_peak = 0.0f;

    // Full-res decoder status (for "lock" indicator)
    bool is_fullres_active = false;

    // Decoded frames visualization
    int total_frames = 0;
    std::vector<bool> decoded_frames_map;  // Map of which frames are decoded (low-res)

    // Optimization: last render time for throttling
    std::chrono::steady_clock::time_point last_render_time;
};

// Array of OSD data for each player
static PlayerOSDData g_player_osd_data[MAX_PLAYERS];

// Function to get player OSD data
static PlayerOSDData& GetPlayerOSDData(int player_id) {
    if (player_id < 0 || player_id >= MAX_PLAYERS) {
        return g_player_osd_data[0]; // Return player 0 data by default
    }
    return g_player_osd_data[player_id];
}

// Use embedded font data from fontdata.h
extern const unsigned char font_otf[];
extern const unsigned int font_otf_size;

// OSD system initialization
int InitOSDSystem(SDL_Renderer* renderer) {
    if (!renderer) {
        return -1;
    }

    g_osd_renderer = renderer;

    // Initialize SDL_TTF
    if (TTF_Init() == -1) {
        return -1;
    }

    // Create temporary file for embedded font
    SDL_RWops* font_rw = SDL_RWFromConstMem(font_otf, font_otf_size);
    if (!font_rw) {
        TTF_Quit();
        return -1;
    }

    // Load fonts of different sizes
    g_large_font = TTF_OpenFontRW(SDL_RWFromConstMem(font_otf, font_otf_size), 1, 32); // Large for timecode
    g_normal_font = TTF_OpenFontRW(SDL_RWFromConstMem(font_otf, font_otf_size), 1, 16); // Normal for status
    g_small_font = TTF_OpenFontRW(SDL_RWFromConstMem(font_otf, font_otf_size), 1, 12);  // Small for scale

    if (!g_large_font || !g_normal_font || !g_small_font) {
        TTF_Quit();
        return -1;
    }

    return 0;
}

// Font setup
void SetOSDFonts(TTF_Font* large_font, TTF_Font* normal_font, TTF_Font* small_font) {
    g_large_font = large_font;
    g_normal_font = normal_font;
    g_small_font = small_font;
}

// Forward declaration for caching functions
static void ClearAllCaches();

// OSD system shutdown
void ShutdownOSDSystem() {
    // Clear all caches before closing fonts
    ClearAllCaches();

    if (g_large_font) {
        TTF_CloseFont(g_large_font);
        g_large_font = nullptr;
    }
    if (g_normal_font) {
        TTF_CloseFont(g_normal_font);
        g_normal_font = nullptr;
    }
    if (g_small_font) {
        TTF_CloseFont(g_small_font);
        g_small_font = nullptr;
    }

    TTF_Quit();
    g_osd_renderer = nullptr;
}

// Generate timecode from time
std::string generateTimecode(double currentTime, int player_id = 0) {
    auto& data = GetPlayerOSDData(player_id);

    char timecode[16];

    if (data.show_frame_numbers) {
        // Frame number display mode from audio module: 0000000000
        snprintf(timecode, sizeof(timecode), "%010d", data.current_frame_number);
    } else {
        // Normal time mode
        int hours = (int)(currentTime / 3600);
        int minutes = (int)((currentTime - hours * 3600) / 60);
        int seconds = (int)(currentTime - hours * 3600 - minutes * 60);

        if (data.is_audio_file) {
            // For audio files: HH:MM:SS.CS (centiseconds)
            int centiseconds = (int)((currentTime - (int)currentTime) * 100) % 100;
            snprintf(timecode, sizeof(timecode), "%02d:%02d:%02d.%02d", hours, minutes, seconds, centiseconds);
        } else {
            // For video files: HH:MM:SS:FF (frames) - use REAL file FPS!
            double fps = (data.fps > 0.0) ? data.fps : 25.0; // Use real FPS or fallback to PAL
            int frames = (int)((currentTime - (int)currentTime) * fps);
            snprintf(timecode, sizeof(timecode), "%02d:%02d:%02d:%02d", hours, minutes, seconds, frames);
        }
    }

    return std::string(timecode);
}

// Get playback status for specific player
std::string getPlaybackStatus(int player_id) {
    PlayerOSDData& data = GetPlayerOSDData(player_id);
    if (data.jog_forward || data.jog_backward) {
        return "jog";
    } else if (std::abs(data.playback_rate) < 0.01) {
        return "still";
    } else if (std::abs(data.playback_rate) > 1.0) {
        return "shuttle";
    } else {
        return "play";
    }
}

// Get speed text for specific player
std::string getSpeedText(int player_id) {
    PlayerOSDData& data = GetPlayerOSDData(player_id);
    char speedBuffer[10];
    snprintf(speedBuffer, sizeof(speedBuffer), "%.1f", std::abs(data.playback_rate));
    return std::string(speedBuffer);
}

// Update OSD data for specific player
void UpdateOSDTimecode(int player_id, double currentTime) {
    GetPlayerOSDData(player_id).current_time = currentTime;
}

void UpdateOSDSpeed(int player_id, double playbackRate, bool isReverse) {
    GetPlayerOSDData(player_id).playback_rate = playbackRate;
    GetPlayerOSDData(player_id).is_reverse = isReverse;
}

void UpdateOSDActualSpeed(int player_id, double actualPlaybackRate) {
    GetPlayerOSDData(player_id).actual_playback_rate = actualPlaybackRate;
}

void UpdateOSDFullResMode(int player_id, bool is_full_res) {
    GetPlayerOSDData(player_id).is_full_res = is_full_res;
}

void UpdateOSDProxyThreading(int player_id, int percent) {
    GetPlayerOSDData(player_id).proxy_threading_progress = percent;
}

void UpdateOSDPlayState(int player_id, bool isPlaying, bool jog_forward, bool jog_backward) {
    GetPlayerOSDData(player_id).is_playing = isPlaying;
    GetPlayerOSDData(player_id).jog_forward = jog_forward;
    GetPlayerOSDData(player_id).jog_backward = jog_backward;
}

void UpdateOSDAudioLevels(int player_id, float left, float right, float leftPeak, float rightPeak) {
    auto& data = GetPlayerOSDData(player_id);

    // Validate input data
    if (!std::isfinite(left) || !std::isfinite(right) || !std::isfinite(leftPeak) || !std::isfinite(rightPeak)) {
        printf("WARNING: Invalid audio levels detected for player %d - ignoring\n", player_id);
        return;
    }

    // Clamp values and filter extreme values
    left = std::max(0.0f, std::min(left, 1.0f));
    right = std::max(0.0f, std::min(right, 1.0f));
    leftPeak = std::max(0.0f, std::min(leftPeak, 1.0f));
    rightPeak = std::max(0.0f, std::min(rightPeak, 1.0f));

    // Additional filtering: ignore extremely small values (potential noise)
    if (left < 0.0001f) left = 0.0f;
    if (right < 0.0001f) right = 0.0f;
    if (leftPeak < 0.0001f) leftPeak = 0.0f;
    if (rightPeak < 0.0001f) rightPeak = 0.0f;

    data.audio_left = left;
    data.audio_right = right;
    data.audio_left_peak = leftPeak;
    data.audio_right_peak = rightPeak;

    // Temporary debug - output every 60 frames
    static int debug_counter = 0;
    if (++debug_counter % 60 == 0 && (left > 0.001f || right > 0.001f)) {
        //printf("OSD Audio Levels Player %d: L=%.3f R=%.3f LP=%.3f RP=%.3f\n",
        //       player_id, left, right, leftPeak, rightPeak);
    }
}

void UpdateOSDPosition(int player_id, double currentTime, double totalDuration) {
    auto& data = GetPlayerOSDData(player_id);
    data.current_time = currentTime;
    data.total_duration = totalDuration;
}

void UpdateOSDSeekMode(int player_id, bool seeking, const std::string& input_timecode) {
    auto& data = GetPlayerOSDData(player_id);
    data.seeking = seeking;
    data.input_timecode = input_timecode;
}

void UpdateOSDDisplayMode(int player_id, OSDDisplayMode mode) {
    GetPlayerOSDData(player_id).display_mode = mode;
}

void UpdateOSDLoadingProgress(int player_id, int percent) {
    GetPlayerOSDData(player_id).loading_progress = std::max(0, std::min(percent, 100));
}

void SetOSDFileType(int player_id, bool is_audio) {
    auto& data = GetPlayerOSDData(player_id);
    data.is_audio_file = is_audio;
    std::cout << "OSD Player " << player_id << " file type set to: "
              << (is_audio ? "audio" : "video") << std::endl;
}

void SetOSDTimecodeOffset(int player_id, double offset_seconds) {
    auto& data = GetPlayerOSDData(player_id);
    data.timecode_offset_seconds = offset_seconds;
    if (offset_seconds > 0.001) {
        std::cout << "[OSD] Timecode offset set: " << offset_seconds << "s for player " << player_id << std::endl;
    }
}

void UpdateOSDDecodedFrames(int player_id, const std::vector<bool>& decoded_map, int total_frames) {
    auto& data = GetPlayerOSDData(player_id);
    data.decoded_frames_map = decoded_map;
    data.total_frames = total_frames;
}

// Backward compatibility - functions without player_id (use player 0)
void UpdateOSDTimecode(double currentTime) {
    UpdateOSDTimecode(0, currentTime);
}

void UpdateOSDSpeed(double playbackRate, bool isReverse) {
    UpdateOSDSpeed(0, playbackRate, isReverse);
}

void UpdateOSDPlayState(bool isPlaying, bool jog_forward, bool jog_backward) {
    UpdateOSDPlayState(0, isPlaying, jog_forward, jog_backward);
}

void UpdateOSDAudioLevels(float left, float right, float leftPeak, float rightPeak) {
    UpdateOSDAudioLevels(0, left, right, leftPeak, rightPeak);
}

void UpdateOSDPosition(double currentTime, double totalDuration) {
    UpdateOSDPosition(0, currentTime, totalDuration);
}

void UpdateOSDSeekMode(bool seeking, const std::string& input_timecode) {
    UpdateOSDSeekMode(0, seeking, input_timecode);
}

void UpdateOSDDisplayMode(OSDDisplayMode mode) {
    UpdateOSDDisplayMode(0, mode);
}

void UpdateOSDLoadingProgress(int percent) {
    UpdateOSDLoadingProgress(0, percent);
}

// Static function declarations
static void RenderLoadingVUMeters();
static void RenderLoadingPositionIndicator();
static void ClearRendererCache(SDL_Renderer* renderer);
static void ClearAllCaches();

// === TEXTURE CACHING WITH MULTIPLE RENDERER SUPPORT ===

// Structure for cached glyph
struct CachedGlyph {
    SDL_Texture* texture = nullptr;
    SDL_Texture* outline_texture = nullptr;
    int width = 0;
    int height = 0;
};

// Structure for cached text
struct CachedText {
    SDL_Texture* texture = nullptr;
    SDL_Texture* outline_texture = nullptr;
    int width = 0;
    int height = 0;
};

// Cache for each renderer: renderer -> (char -> CachedGlyph)
// Separate caches for different colors (yellow, gray, white)
static std::map<SDL_Renderer*, std::unordered_map<char, CachedGlyph>> g_glyph_cache_yellow;
static std::map<SDL_Renderer*, std::unordered_map<char, CachedGlyph>> g_glyph_cache_gray;
static std::map<SDL_Renderer*, std::unordered_map<char, CachedGlyph>> g_glyph_cache_white;

// Cache for text strings: renderer -> (string -> CachedText)
static std::map<SDL_Renderer*, std::unordered_map<std::string, CachedText>> g_text_cache_normal_white;
static std::map<SDL_Renderer*, std::unordered_map<std::string, CachedText>> g_text_cache_normal_yellow;
static std::map<SDL_Renderer*, std::unordered_map<std::string, CachedText>> g_text_cache_large_white;
static std::map<SDL_Renderer*, std::unordered_map<std::string, CachedText>> g_text_cache_large_yellow;
static std::map<SDL_Renderer*, std::unordered_map<std::string, CachedText>> g_text_cache_small_white;
static std::map<SDL_Renderer*, std::unordered_map<std::string, CachedText>> g_text_cache_small_gray;

// Clear cache for specific renderer
static void ClearRendererCache(SDL_Renderer* renderer) {
    if (!renderer) return;

    // Clear glyphs
    auto clearGlyphMap = [](auto& cache_map, SDL_Renderer* r) {
        auto it = cache_map.find(r);
        if (it != cache_map.end()) {
            for (auto& pair : it->second) {
                if (pair.second.texture) SDL_DestroyTexture(pair.second.texture);
                if (pair.second.outline_texture) SDL_DestroyTexture(pair.second.outline_texture);
            }
            cache_map.erase(it);
        }
    };

    clearGlyphMap(g_glyph_cache_yellow, renderer);
    clearGlyphMap(g_glyph_cache_gray, renderer);
    clearGlyphMap(g_glyph_cache_white, renderer);

    // Clear texts
    auto clearTextMap = [](auto& cache_map, SDL_Renderer* r) {
        auto it = cache_map.find(r);
        if (it != cache_map.end()) {
            for (auto& pair : it->second) {
                if (pair.second.texture) SDL_DestroyTexture(pair.second.texture);
                if (pair.second.outline_texture) SDL_DestroyTexture(pair.second.outline_texture);
            }
            cache_map.erase(it);
        }
    };

    clearTextMap(g_text_cache_normal_white, renderer);
    clearTextMap(g_text_cache_normal_yellow, renderer);
    clearTextMap(g_text_cache_large_white, renderer);
    clearTextMap(g_text_cache_large_yellow, renderer);
    clearTextMap(g_text_cache_small_white, renderer);
    clearTextMap(g_text_cache_small_gray, renderer);
}

// Public function to clear cache for specific renderer
// IMPORTANT: Call BEFORE SDL_DestroyRenderer!
void ClearOSDCacheForRenderer(SDL_Renderer* renderer) {
    ClearRendererCache(renderer);
}

// Clear all caches (on shutdown)
static void ClearAllCaches() {
    // Get list of all renderers from caches
    std::set<SDL_Renderer*> renderers;

    for (auto& pair : g_glyph_cache_yellow) renderers.insert(pair.first);
    for (auto& pair : g_text_cache_normal_white) renderers.insert(pair.first);

    // Clear each renderer
    for (SDL_Renderer* renderer : renderers) {
        ClearRendererCache(renderer);
    }
}

// Get or create cached glyph for specific renderer
static CachedGlyph* GetCachedGlyph(SDL_Renderer* renderer, char c, SDL_Color color, SDL_Color outline_color, TTF_Font* font) {
    if (!font || !renderer) return nullptr;

    // Select correct cache by color
    std::map<SDL_Renderer*, std::unordered_map<char, CachedGlyph>>* cache = nullptr;
    if (color.r == 255 && color.g == 255 && color.b == 0) {
        cache = &g_glyph_cache_yellow;
    } else if (color.r == 100 && color.g == 100 && color.b == 100) {
        cache = &g_glyph_cache_gray;
    } else {
        cache = &g_glyph_cache_white;
    }

    // Get cache for specific renderer
    auto& renderer_cache = (*cache)[renderer];

    auto it = renderer_cache.find(c);
    if (it != renderer_cache.end()) {
        return &it->second; // Already in cache
    }

    // Create new glyph
    char text[2] = {c, '\0'};
    CachedGlyph glyph;

    // Outline
    SDL_Surface* outline_surface = TTF_RenderText_Blended(font, text, outline_color);
    if (outline_surface) {
        glyph.outline_texture = SDL_CreateTextureFromSurface(renderer, outline_surface);
        glyph.width = outline_surface->w;
        glyph.height = outline_surface->h;
        SDL_FreeSurface(outline_surface);
    }

    // Main text
    SDL_Surface* surface = TTF_RenderText_Blended(font, text, color);
    if (surface) {
        glyph.texture = SDL_CreateTextureFromSurface(renderer, surface);
        SDL_FreeSurface(surface);
    }

    renderer_cache[c] = glyph;
    return &renderer_cache[c];
}

// Get or create cached text for specific renderer
static CachedText* GetCachedText(SDL_Renderer* renderer, const std::string& text, SDL_Color color, SDL_Color outline_color, TTF_Font* font) {
    if (!font || !renderer || text.empty()) return nullptr;

    // Select cache by color and font
    std::map<SDL_Renderer*, std::unordered_map<std::string, CachedText>>* cache = nullptr;
    bool is_yellow = (color.r == 255 && color.g == 255 && color.b == 0);
    bool is_gray = (color.r == 100 && color.g == 100 && color.b == 100);

    if (font == g_large_font) {
        cache = is_yellow ? &g_text_cache_large_yellow : &g_text_cache_large_white;
    } else if (font == g_small_font) {
        cache = is_gray ? &g_text_cache_small_gray : &g_text_cache_small_white;
    } else {
        cache = is_yellow ? &g_text_cache_normal_yellow : &g_text_cache_normal_white;
    }

    // Get cache for specific renderer
    auto& renderer_cache = (*cache)[renderer];

    // Key by text AND colour. The caches above only split white/yellow/gray, so any OTHER
    // colour (e.g. the {150,150,150} threading-grey speed on the normal font) fell into the
    // "white" map under the bare text key and poisoned it: the glyph was baked grey, so when
    // the same text later rendered white it got the stale grey texture back — the "speed stays
    // grey / stuck after threading" bug. Folding the colour into the key keeps each
    // (text,colour) a distinct entry.
    std::string key = text;
    key.push_back('\x1f');
    key.push_back(static_cast<char>(color.r));
    key.push_back(static_cast<char>(color.g));
    key.push_back(static_cast<char>(color.b));
    key.push_back(static_cast<char>(color.a));

    auto it = renderer_cache.find(key);
    if (it != renderer_cache.end()) {
        return &it->second; // Already in cache
    }

    // Create new texture for text
    CachedText cached;

    // Outline
    SDL_Surface* outline_surface = TTF_RenderText_Blended(font, text.c_str(), outline_color);
    if (outline_surface) {
        cached.outline_texture = SDL_CreateTextureFromSurface(renderer, outline_surface);
        cached.width = outline_surface->w;
        cached.height = outline_surface->h;
        SDL_FreeSurface(outline_surface);
    }

    // Main text
    SDL_Surface* surface = TTF_RenderText_Blended(font, text.c_str(), color);
    if (surface) {
        cached.texture = SDL_CreateTextureFromSurface(renderer, surface);
        SDL_FreeSurface(surface);
    }

    renderer_cache[key] = cached;
    return &renderer_cache[key];
}

// Render cached text
static void RenderCachedText(SDL_Renderer* renderer, const std::string& text, int x, int y, SDL_Color color, SDL_Color outline_color, TTF_Font* font) {
    CachedText* cached = GetCachedText(renderer, text, color, outline_color, font);
    if (!cached || !cached->texture) return;

    // Draw outline in 4 directions
    if (cached->outline_texture) {
        static const int offsets[4][2] = {{0, -1}, {0, 1}, {-1, 0}, {1, 0}};
        for (int i = 0; i < 4; i++) {
            SDL_Rect outline_rect = {x + offsets[i][0], y + offsets[i][1], cached->width, cached->height};
            SDL_RenderCopy(renderer, cached->outline_texture, nullptr, &outline_rect);
        }
    }

    // Draw main text
    SDL_Rect dst_rect = {x, y, cached->width, cached->height};
    SDL_RenderCopy(renderer, cached->texture, nullptr, &dst_rect);
}

// Render cached glyph (returns width for positioning)
static int RenderCachedGlyph(SDL_Renderer* renderer, char c, int x, int y, SDL_Color color, SDL_Color outline_color, TTF_Font* font) {
    CachedGlyph* glyph = GetCachedGlyph(renderer, c, color, outline_color, font);
    if (!glyph || !glyph->texture) return 0;

    // Draw outline in 4 directions
    if (glyph->outline_texture) {
        static const int offsets[4][2] = {{0, -1}, {0, 1}, {-1, 0}, {1, 0}};
        for (int i = 0; i < 4; i++) {
            SDL_Rect outline_rect = {x + offsets[i][0], y + offsets[i][1], glyph->width, glyph->height};
            SDL_RenderCopy(renderer, glyph->outline_texture, nullptr, &outline_rect);
        }
    }

    // Draw main text
    SDL_Rect dst_rect = {x, y, glyph->width, glyph->height};
    SDL_RenderCopy(renderer, glyph->texture, nullptr, &dst_rect);
    return glyph->width;
}

// === OSD CONTROL FUNCTIONS ===

// DEPRECATED: This function is no longer used.
// All rendering code is now embedded in RenderOSDForPlayer() for better performance.

// DEPRECATED: This function is no longer used.
// All rendering code is now embedded in RenderOSDForPlayer() for better performance.

// Render loading screen (flashing timecode)
static void RenderLoadingScreen(int player_id) {
    if (!g_large_font || !g_normal_font || !g_osd_renderer) return;

    int windowWidth, windowHeight;
    SDL_GetRendererOutputSize(g_osd_renderer, &windowWidth, &windowHeight);

    // OPTIMIZATION: Do NOT call SDL_RenderClear here - already done in RenderAllWindows()
    // Double screen clear increases CPU by 10-15%!

    // Colors (exactly as in the original)
    SDL_Color textColor = {255, 255, 255, 255};
    SDL_Color shadowColor = {0, 0, 0, 180};

    // Flashing timecode
    static Uint32 lastBlinkTime = 0;
    static bool showDashes = true;

    Uint32 currentTicks = SDL_GetTicks();
    if (currentTicks - lastBlinkTime > 500) { // Flash every 500ms
        showDashes = !showDashes;
        lastBlinkTime = currentTicks;
    }

    std::string timecode = showDashes ? "--:--:--:--" : "  :  :  :  ";

    // Calculate size for centering
    int text_width, text_height;
    TTF_SizeText(g_large_font, timecode.c_str(), &text_width, &text_height);

    int timecodeX = (windowWidth - text_width) / 2;
    int timecodeY = windowHeight - 86; // Exact position as in the original

    RenderCachedText(g_osd_renderer, timecode, timecodeX, timecodeY, textColor, shadowColor, g_large_font);

    // Loading status (threading/indexing/proxy) left below timecode
    const auto& player_data = g_player_osd_data[player_id];
    std::string statusText = player_data.loading_status;
    int statusY = timecodeY + text_height;

    RenderCachedText(g_osd_renderer, statusText, timecodeX, statusY, textColor, shadowColor, g_normal_font);

    // Progress (000-100) right below timecode
    char progressBuffer[4];
    snprintf(progressBuffer, sizeof(progressBuffer), "%03d", player_data.loading_progress);

    int progress_width, progress_height;
    TTF_SizeText(g_normal_font, progressBuffer, &progress_width, &progress_height);
    int progressX = timecodeX + text_width - progress_width;

    RenderCachedText(g_osd_renderer, std::string(progressBuffer), progressX, statusY, textColor, shadowColor, g_normal_font);

    // VU meters (empty) and position indicator
    RenderLoadingVUMeters();
    RenderLoadingPositionIndicator();
}

// Render screen without file
static void RenderNoFileScreen(int player_id) {
    (void)player_id;
    if (!g_large_font || !g_normal_font || !g_osd_renderer) return;

    int windowWidth, windowHeight;
    SDL_GetRendererOutputSize(g_osd_renderer, &windowWidth, &windowHeight);

    SDL_Color grayColor = {128, 128, 128, 255};
    SDL_Color shadowColor = {0, 0, 0, 180};

    // Timecode
    std::string timecode = "--:--:--:--";
    int text_width, text_height;
    TTF_SizeText(g_large_font, timecode.c_str(), &text_width, &text_height);

    int timecodeX = (windowWidth - text_width) / 2;
    int timecodeY = windowHeight - 86;

    RenderCachedText(g_osd_renderer, timecode, timecodeX, timecodeY, grayColor, shadowColor, g_large_font);

    // Status
    std::string statusText = "stop";
    int statusY = timecodeY + text_height;
    RenderCachedText(g_osd_renderer, statusText, timecodeX, statusY, grayColor, shadowColor, g_normal_font);

    // Speed
    std::string speedText = "--";
    int speed_width, speed_height;
    TTF_SizeText(g_normal_font, speedText.c_str(), &speed_width, &speed_height);
    int speedX = timecodeX + text_width - speed_width;
    RenderCachedText(g_osd_renderer, speedText, speedX, statusY, grayColor, shadowColor, g_normal_font);

    // VU meters and position indicator
    RenderLoadingVUMeters();
    RenderLoadingPositionIndicator();

    // Message in center
    std::string message = "Press Ctrl+O to open a file";
    int msg_width, msg_height;
    TTF_SizeText(g_normal_font, message.c_str(), &msg_width, &msg_height);
    int msgX = windowWidth / 2 - msg_width / 2;
    int msgY = windowHeight / 2 - msg_height / 2;
    RenderCachedText(g_osd_renderer, message, msgX, msgY, grayColor, shadowColor, g_normal_font);
}

// Render empty VU meters for loading screens
static void RenderLoadingVUMeters() {
    if (!g_normal_font || !g_small_font || !g_osd_renderer) return;

    int windowWidth, windowHeight;
    SDL_GetRendererOutputSize(g_osd_renderer, &windowWidth, &windowHeight);

    // Exactly the same sizes as in normal mode
    const int METER_WIDTH = 150;
    const int METER_HEIGHT = 12;
    const int METER_MARGIN = 30;
    const int METER_SPACING = 5;

    int meterX = METER_MARGIN;
    int leftMeterY = windowHeight - 74;
    int rightMeterY = leftMeterY + METER_HEIGHT + METER_SPACING;

    // Colors
    SDL_Color whiteColor = {255, 255, 255, 255};
    SDL_Color bgColor = {40, 40, 40, 255};
    SDL_Color grayColor = {180, 180, 180, 255};

    // Function to draw empty meter
    auto drawEmptyMeter = [&](int y, const std::string& label) {
        // Meter background
        SDL_SetRenderDrawColor(g_osd_renderer, bgColor.r, bgColor.g, bgColor.b, bgColor.a);
        SDL_Rect bgRect = {meterX, y, METER_WIDTH, METER_HEIGHT};
        SDL_RenderFillRect(g_osd_renderer, &bgRect);

        // Frame
        SDL_SetRenderDrawColor(g_osd_renderer, 100, 100, 100, 255);
        SDL_RenderDrawRect(g_osd_renderer, &bgRect);

        // Channel label
        RenderCachedText(g_osd_renderer, label, meterX - 20, y + (METER_HEIGHT - 16) / 2, whiteColor, {0, 0, 0, 180}, g_normal_font);
    };

    // Render empty meters
    drawEmptyMeter(leftMeterY, "1");
    drawEmptyMeter(rightMeterY, "2");

    // dB scale (exactly as in the original)
    const int SCALE_Y = rightMeterY + METER_HEIGHT + 8;
    float dbMarkings[] = {-60, -40, -20, -12, -6, 0};
    const char* dbLabels[] = {"-60", "-40", "-20", "-12", "-6", "0"};
    int numMarkings = 6;

    const float MIN_DB = -60.0f;
    const float MAX_DB = 0.0f;

    for (int i = 0; i < numMarkings; i++) {
        float db = dbMarkings[i];
        float position = ((db - MIN_DB) / (MAX_DB - MIN_DB));
        int markX = meterX + static_cast<int>(position * METER_WIDTH);

        int label_width, label_height;
        TTF_SizeText(g_small_font, dbLabels[i], &label_width, &label_height);
        int labelX = markX - label_width / 2;

        RenderCachedText(g_osd_renderer, std::string(dbLabels[i]), labelX, SCALE_Y, grayColor, {0, 0, 0, 180}, g_small_font);

        // Tick
        SDL_SetRenderDrawColor(g_osd_renderer, grayColor.r, grayColor.g, grayColor.b, grayColor.a);
        SDL_Rect tickRect = {markX - 1, SCALE_Y - 7, 2, 5};
        SDL_RenderFillRect(g_osd_renderer, &tickRect);
    }
}

// Render empty position indicator for loading screens
static void RenderLoadingPositionIndicator() {
    if (!g_osd_renderer) return;

    int windowWidth, windowHeight;
    SDL_GetRendererOutputSize(g_osd_renderer, &windowWidth, &windowHeight);

    // Exactly the same sizes
    const int INDICATOR_WIDTH = 82;
    const int INDICATOR_HEIGHT = 46;
    const int MARGIN_RIGHT = 30;

    int indicatorX = windowWidth - INDICATOR_WIDTH - MARGIN_RIGHT;
    int indicatorY = windowHeight - 64 - (INDICATOR_HEIGHT / 2) + 10;

    SDL_Color whiteColor = {255, 255, 255, 255};

    // Black outline around indicator (like text with outline)
    SDL_Rect outlineRect = {indicatorX - 1, indicatorY - 1, INDICATOR_WIDTH + 2, INDICATOR_HEIGHT + 2};
    SDL_SetRenderDrawColor(g_osd_renderer, 0, 0, 0, 255);
    SDL_RenderDrawRect(g_osd_renderer, &outlineRect);

    // Main white frame
    SDL_Rect bgRect = {indicatorX, indicatorY, INDICATOR_WIDTH, INDICATOR_HEIGHT};
    SDL_SetRenderDrawColor(g_osd_renderer, whiteColor.r, whiteColor.g, whiteColor.b, whiteColor.a);
    SDL_RenderDrawRect(g_osd_renderer, &bgRect);
}

// Main function to render OSD elements
void RenderOSD() {
    RenderOSDForPlayer(g_osd_renderer, 0);
}

// Rendering OSD for specific player - SINGLE RENDERING LOOP
void RenderOSDForPlayer(SDL_Renderer* renderer, int player_id) {
    if (!renderer) return;

    // Save current renderer
    SDL_Renderer* original_renderer = g_osd_renderer;

    // Temporarily use passed renderer
    // IMPORTANT: DO NOT clear cache when switching renderer!
    // Cache textures are bound to renderer and will be automatically recreated as needed
    g_osd_renderer = renderer;

    // Get player data
    auto& data = GetPlayerOSDData(player_id);

    // ============================================================
    // SINGLE RENDERING LOOP FOR ALL OSD ELEMENTS
    // ============================================================

    // Display mode selection
    if (data.display_mode == OSD_MODE_LOADING) {
        RenderLoadingScreen(player_id);
    } else if (data.display_mode == OSD_MODE_NO_FILE) {
        RenderNoFileScreen(player_id);
    } else {
        // OSD_MODE_NORMAL - render all elements sequentially

        if (!g_large_font || !g_normal_font || !g_small_font) {
            g_osd_renderer = original_renderer;
            return;
        }

        int windowWidth, windowHeight;
        SDL_GetRendererOutputSize(renderer, &windowWidth, &windowHeight);

        // auto timecode_start = std::chrono::high_resolution_clock::now();

        // Colors
        SDL_Color textColor = {255, 255, 255, 255};
        SDL_Color shadowColor = {0, 0, 0, 180};
        SDL_Color seekColor = {255, 255, 0, 255};
        SDL_Color whiteColor = {255, 255, 255, 255};
        SDL_Color redColor = {255, 0, 0, 255};
        SDL_Color bgColor = {40, 40, 40, 255};
        SDL_Color grayColor = {180, 180, 180, 255};
        // VU/PPM meter colour zones (like Logic/FCP/Pro Tools): green safe → amber → red near 0 dBFS.
        SDL_Color greenColor = {0, 200, 60, 255};
        SDL_Color amberColor = {235, 190, 0, 255};

        // ========== 1. CENTRAL TIMECODE AND STATUS ==========
        std::string timecode;
        int user_input_length = 0;  // Number of digits entered by user

        if (data.seeking) {
            user_input_length = static_cast<int>(data.input_timecode.length());
            std::string padded = data.input_timecode;
            while (padded.length() < 8) padded = "0" + padded;
            timecode = padded.substr(0, 2) + ":" +
                       padded.substr(2, 2) + ":" +
                       padded.substr(4, 2) + ":" +
                       padded.substr(6, 2);
        } else {
            timecode = generateTimecode(data.current_time, player_id);
        }

        int text_width, text_height;
        TTF_SizeText(g_large_font, timecode.c_str(), &text_width, &text_height);

        int timecodeX = (windowWidth - text_width) / 2;
        int timecodeY = windowHeight - 86;

        // In seek mode, render each symbol with the correct color
        if (data.seeking) {
            SDL_Color dimColor = {100, 100, 100, 255};  // Gray for unfilled
            int currentX = timecodeX;
            int charIndex = 0;  // Index of digit (without separators)

            for (size_t i = 0; i < timecode.length(); i++) {
                char c = timecode[i];
                char buf[2] = {c, '\0'};

                SDL_Color charColor;

                // Digits: check if this position is entered
                // Count from the end: if 3 digits are entered, the last 3 are bright
                int digitsFromEnd = 8 - charIndex;
                bool isEntered = (digitsFromEnd <= user_input_length);

                if (c == ':') {
                    // Separator: bright if digits to the right of it are entered
                    // Check the next digit after the separator
                    charColor = isEntered ? seekColor : dimColor;
                } else {
                    // Digit
                    charColor = isEntered ? seekColor : dimColor;
                    charIndex++;
                }

                int charWidth = RenderCachedGlyph(renderer, buf[0], currentX, timecodeY, charColor, shadowColor, g_large_font);
                currentX += charWidth;
            }
        } else {
            // Normal mode - render the whole string
            RenderCachedText(renderer, timecode, timecodeX, timecodeY, textColor, shadowColor, g_large_font);
        }

        // auto timecode_end = std::chrono::high_resolution_clock::now();
        // total_timecode_us += std::chrono::duration_cast<std::chrono::microseconds>(timecode_end - timecode_start).count();

        // auto status_start = std::chrono::high_resolution_clock::now();

        // Status left under timecode
        std::string statusText;
        if (data.seeking) {
            statusText = "seek";
        } else {
            double actual_speed = std::abs(data.actual_playback_rate);
            std::string potential_mode;

            if (data.jog_forward || data.jog_backward) {
                potential_mode = "jog";
            } else if (actual_speed < 0.01) {
                potential_mode = "still";
            } else if (actual_speed > 1.4) {
                potential_mode = "shuttle";
            } else if (actual_speed < 0.6) {
                potential_mode = "still";
            } else {
                potential_mode = "play";
            }

            const int STABILITY_THRESHOLD = 10;

            if (potential_mode == data.last_stable_mode) {
                data.mode_stability_counter = 0;
                statusText = data.last_stable_mode;
            } else {
                bool is_major_change = (data.last_stable_mode == "still" && potential_mode != "still") ||
                                       (data.last_stable_mode != "still" && potential_mode == "still");

                if (is_major_change) {
                    data.mode_stability_counter += 2;
                } else {
                    data.mode_stability_counter++;
                }

                if (data.mode_stability_counter >= STABILITY_THRESHOLD) {
                    data.last_stable_mode = potential_mode;
                    data.mode_stability_counter = 0;
                    statusText = potential_mode;
                } else {
                    statusText = data.last_stable_mode;
                }
            }
        }

        SDL_Color statusColor = data.seeking ? seekColor : textColor;
        int statusY = timecodeY + text_height;
        RenderCachedText(renderer, statusText, timecodeX, statusY, statusColor, shadowColor, g_normal_font);

        // Speed right under timecode - show "LOCK" for 1.0× full-res (Betacam SP tribute)
        std::string speedText;
        double abs_speed = std::abs(data.actual_playback_rate);

        // TAPE THREADING: while the proxy builds in the background, transport is
        // limited (shuttle/reverse locked). Minimal indication: the speed itself
        // in blinking GREY text — no separate badge.
        bool tape_threading = (data.proxy_threading_progress >= 0);

        if (tape_threading) {
            char speedBuffer[10];
            snprintf(speedBuffer, sizeof(speedBuffer), "%.1f", abs_speed);
            speedText = speedBuffer;  // during threading always a number, not "lock"
        } else if (data.is_full_res && abs_speed >= 0.95 && abs_speed <= 1.05) {
            speedText = "lock";  // Locked 1:1 speed with full-res decoder
        } else {
            char speedBuffer[10];
            snprintf(speedBuffer, sizeof(speedBuffer), "%.1f", abs_speed);
            speedText = speedBuffer;
        }

        int speed_width, speed_height;
        TTF_SizeText(g_normal_font, speedText.c_str(), &speed_width, &speed_height);
        int speedX = timecodeX + text_width - speed_width;
        int speedY = timecodeY + text_height;
        if (tape_threading) {
            // Blink ~1 Hz (600 ms visible / 400 ms hidden), grey = limited mode.
            if (SDL_GetTicks() % 1000 < 600) {
                SDL_Color threadingGray = {150, 150, 150, 255};
                RenderCachedText(renderer, speedText, speedX, speedY, threadingGray, shadowColor, g_normal_font);
            }
        } else {
            RenderCachedText(renderer, speedText, speedX, speedY, textColor, shadowColor, g_normal_font);
        }

        // auto status_end = std::chrono::high_resolution_clock::now();
        // total_status_us += std::chrono::duration_cast<std::chrono::microseconds>(status_end - status_start).count();

        // auto other_start = std::chrono::high_resolution_clock::now();

        // ========== 2. VU METERS ==========
        bool is_player_active = IsPlayerInstanceActive(player_id);
        bool is_playing = is_player_active && data.is_playing;

        // PPM (Peak Program Meter) ballistics - matching professional broadcast standards
        // Both bar and line jump to peaks INSTANTLY, but fall at different rates
        // At 60 FPS: 1.0 = instant, 0.4 = ~5 frames (~80ms), 0.03 = ~33 frames (~550ms)
        const float LEVEL_ATTACK = 1.0f;          // Quasi-peak: INSTANT attack (same as true peak)
        const float LEVEL_DECAY = 0.4f;           // Quasi-peak: moderate decay for readability
        const float PEAK_ATTACK = 1.0f;           // True peak: instant capture
        const float PEAK_DECAY = 0.03f;           // True peak: slow decay (~2s hold)
        const float STOP_DECAY = 0.05f;

        if (is_playing) {
            // Peak meter (FCP/Pro Tools style): the solid bar follows TRUE PEAK with fast attack and
            // a moderate fall-back, and the cap holds the peak — so the cap sits just above the bar
            // instead of floating far from a low RMS bar (the big gap the RMS bar showed). RMS is
            // still computed in the audio callback (audio_left/right) and available for a future
            // inner loudness sub-bar.
            if (data.audio_left_peak > data.smooth_left) {
                data.smooth_left += (data.audio_left_peak - data.smooth_left) * LEVEL_ATTACK;
            } else {
                data.smooth_left += (data.audio_left_peak - data.smooth_left) * LEVEL_DECAY;
            }

            if (data.audio_right_peak > data.smooth_right) {
                data.smooth_right += (data.audio_right_peak - data.smooth_right) * LEVEL_ATTACK;
            } else {
                data.smooth_right += (data.audio_right_peak - data.smooth_right) * LEVEL_DECAY;
            }

            // True peak line: instant attack, slow decay (hold behavior)
            if (data.audio_left_peak > data.smooth_left_peak) {
                data.smooth_left_peak += (data.audio_left_peak - data.smooth_left_peak) * PEAK_ATTACK;
            } else {
                data.smooth_left_peak += (data.audio_left_peak - data.smooth_left_peak) * PEAK_DECAY;
            }

            if (data.audio_right_peak > data.smooth_right_peak) {
                data.smooth_right_peak += (data.audio_right_peak - data.smooth_right_peak) * PEAK_ATTACK;
            } else {
                data.smooth_right_peak += (data.audio_right_peak - data.smooth_right_peak) * PEAK_DECAY;
            }
        } else {
            data.smooth_left *= (1.0f - STOP_DECAY);
            data.smooth_right *= (1.0f - STOP_DECAY);
            data.smooth_left_peak *= (1.0f - STOP_DECAY);
            data.smooth_right_peak *= (1.0f - STOP_DECAY);

            if (data.smooth_left < 0.001f) data.smooth_left = 0.0f;
            if (data.smooth_right < 0.001f) data.smooth_right = 0.0f;
            if (data.smooth_left_peak < 0.001f) data.smooth_left_peak = 0.0f;
            if (data.smooth_right_peak < 0.001f) data.smooth_right_peak = 0.0f;
        }

        if (!std::isfinite(data.smooth_left)) data.smooth_left = 0.0f;
        if (!std::isfinite(data.smooth_right)) data.smooth_right = 0.0f;
        if (!std::isfinite(data.smooth_left_peak)) data.smooth_left_peak = 0.0f;
        if (!std::isfinite(data.smooth_right_peak)) data.smooth_right_peak = 0.0f;

        const int METER_WIDTH = 150;
        const int METER_HEIGHT = 12;
        const int METER_MARGIN = 30;
        const int METER_SPACING = 5;

        int meterX = METER_MARGIN;
        int leftMeterY = windowHeight - 74;
        int rightMeterY = leftMeterY + METER_HEIGHT + METER_SPACING;

        auto drawHorizontalMeter = [&](int y, float level, float peak, const std::string& label) {
            SDL_SetRenderDrawColor(renderer, bgColor.r, bgColor.g, bgColor.b, bgColor.a);
            SDL_Rect bgRect = {meterX, y, METER_WIDTH, METER_HEIGHT};
            SDL_RenderFillRect(renderer, &bgRect);

            SDL_SetRenderDrawColor(renderer, 100, 100, 100, 255);
            SDL_RenderDrawRect(renderer, &bgRect);

            float levelDB = (level > 0.0001f) ? 20.0f * log10f(level) : -80.0f;
            float peakDB = (peak > 0.0001f) ? 20.0f * log10f(peak) : -80.0f;

            const float MIN_DB = -60.0f;
            const float MAX_DB = 0.0f;

            float levelWidth = ((levelDB - MIN_DB) / (MAX_DB - MIN_DB)) * METER_WIDTH;
            float peakWidth = ((peakDB - MIN_DB) / (MAX_DB - MIN_DB)) * METER_WIDTH;

            levelWidth = std::max(0.0f, std::min(static_cast<float>(METER_WIDTH), levelWidth));
            peakWidth = std::max(0.0f, std::min(static_cast<float>(METER_WIDTH), peakWidth));

            if (levelWidth > 0) {
                // Colour-zoned fill (green ≤ -18, amber -18..-6, red -6..0 dBFS), each segment drawn
                // up to the current level — the classic DAW peak/PPM look.
                auto dbToW = [&](float db) {
                    float w = ((db - MIN_DB) / (MAX_DB - MIN_DB)) * METER_WIDTH;
                    return std::max(0.0f, std::min(static_cast<float>(METER_WIDTH), w));
                };
                const float amberAt = dbToW(-18.0f);
                const float redAt   = dbToW(-6.0f);
                const int barTop = y + 1;
                const int barH   = METER_HEIGHT - 2;
                auto fillSeg = [&](float fromW, float toW, SDL_Color c) {
                    float a = std::max(fromW, 0.0f);
                    float b = std::min(toW, levelWidth);
                    if (b - a < 1.0f) return;
                    SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, c.a);
                    SDL_Rect r = {meterX + 1 + static_cast<int>(a), barTop,
                                  static_cast<int>(b - a), barH};
                    SDL_RenderFillRect(renderer, &r);
                };
                fillSeg(0.0f,    amberAt,    greenColor);
                fillSeg(amberAt, redAt,      amberColor);
                fillSeg(redAt,   levelWidth, redColor);
            }

            if (peakWidth > 2) {
                int peakX = meterX + static_cast<int>(peakWidth);
                SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
                SDL_Rect peakRect = {peakX - 1, y + 1, 2, METER_HEIGHT - 2};
                SDL_RenderFillRect(renderer, &peakRect);
            }

            RenderCachedText(renderer, label, meterX - 20, y + (METER_HEIGHT - 16) / 2, whiteColor, {0, 0, 0, 180}, g_normal_font);
        };

        drawHorizontalMeter(leftMeterY, data.smooth_left, data.smooth_left_peak, "1");
        drawHorizontalMeter(rightMeterY, data.smooth_right, data.smooth_right_peak, "2");

        // dB scale under meters
        const int SCALE_Y = rightMeterY + METER_HEIGHT + 8;
        float dbMarkings[] = {-60, -40, -20, -12, -6, 0};
        const char* dbLabels[] = {"-60", "-40", "-20", "-12", "-6", "0"};
        int numMarkings = 6;

        const float MIN_DB = -60.0f;
        const float MAX_DB = 0.0f;

        for (int i = 0; i < numMarkings; i++) {
            float db = dbMarkings[i];
            float position = ((db - MIN_DB) / (MAX_DB - MIN_DB));
            int markX = meterX + static_cast<int>(position * METER_WIDTH);

            int label_width, label_height;
            TTF_SizeText(g_small_font, dbLabels[i], &label_width, &label_height);
            int labelX = markX - label_width / 2;

            RenderCachedText(renderer, std::string(dbLabels[i]), labelX, SCALE_Y, grayColor, {0, 0, 0, 180}, g_small_font);

            SDL_SetRenderDrawColor(renderer, grayColor.r, grayColor.g, grayColor.b, grayColor.a);
            SDL_Rect tickRect = {markX - 1, SCALE_Y - 7, 2, 5};
            SDL_RenderFillRect(renderer, &tickRect);
        }

        // ========== 3. POSITION INDICATOR ==========
        const int INDICATOR_WIDTH = 82;
        const int INDICATOR_HEIGHT = 46;
        const int MARGIN_RIGHT = 30;

        int indicatorX = windowWidth - INDICATOR_WIDTH - MARGIN_RIGHT;
        int indicatorY = windowHeight - 64 - (INDICATOR_HEIGHT / 2) + 10;

        // Black outline around indicator (like text with outline)
        SDL_Rect outlineRect = {indicatorX - 1, indicatorY - 1, INDICATOR_WIDTH + 2, INDICATOR_HEIGHT + 2};
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderDrawRect(renderer, &outlineRect);

        // Main white frame
        SDL_Rect bgRect = {indicatorX, indicatorY, INDICATOR_WIDTH, INDICATOR_HEIGHT};
        SDL_SetRenderDrawColor(renderer, whiteColor.r, whiteColor.g, whiteColor.b, whiteColor.a);
        SDL_RenderDrawRect(renderer, &bgRect);

        // PROXY THREADING PROGRESS: this box maps to the WHOLE clip (position 0..1), so
        // proxy conversion (now a background job after load) reads naturally as the box
        // filling left→right — it literally shows how much of the clip has been proxied.
        // Drawn UNDER the playhead bar below so the head stays visible on top.
        if (data.proxy_threading_progress >= 0) {
            int pct = std::max(0, std::min(100, data.proxy_threading_progress));
            int innerW = INDICATOR_WIDTH - 4;              // inside the 2px frame padding
            int fillW = (innerW * pct) / 100;
            if (fillW > 0) {
                SDL_BlendMode prevBlend;
                SDL_GetRenderDrawBlendMode(renderer, &prevBlend);
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

                // Translucent green wash = "building proxy".
                SDL_SetRenderDrawColor(renderer, greenColor.r, greenColor.g, greenColor.b, 90);
                SDL_Rect fillRect = {indicatorX + 2, indicatorY + 2, fillW, INDICATOR_HEIGHT - 4};
                SDL_RenderFillRect(renderer, &fillRect);

                // Brighter leading edge so the growth stays legible frame-to-frame.
                SDL_SetRenderDrawColor(renderer, greenColor.r, greenColor.g, greenColor.b, 220);
                SDL_Rect edgeRect = {indicatorX + 2 + fillW - 1, indicatorY + 2, 1, INDICATOR_HEIGHT - 4};
                SDL_RenderFillRect(renderer, &edgeRect);

                SDL_SetRenderDrawBlendMode(renderer, prevBlend);
            }
        }

        double position = 0.0;
        if (data.total_duration > 0.0) {
            // Subtract timecode offset for correct position indicator
            double raw_time = data.current_time - data.timecode_offset_seconds;
            position = raw_time / data.total_duration;
            position = std::max(0.0, std::min(1.0, position));
        }

        if (position > 0.0) {
            int barX = indicatorX + 2 + static_cast<int>((INDICATOR_WIDTH - 4) * position);

            // Black background/outline for position bar (makes it readable on white background)
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_Rect barOutline = {barX - 2, indicatorY + 1, 4, INDICATOR_HEIGHT - 2};
            SDL_RenderFillRect(renderer, &barOutline);

            // Main white position bar
            SDL_SetRenderDrawColor(renderer, whiteColor.r, whiteColor.g, whiteColor.b, whiteColor.a);
            SDL_Rect positionBar = {barX - 1, indicatorY + 2, 2, INDICATOR_HEIGHT - 4};
            SDL_RenderFillRect(renderer, &positionBar);
        }

        // auto other_end = std::chrono::high_resolution_clock::now();
        // total_other_us += std::chrono::duration_cast<std::chrono::microseconds>(other_end - other_start).count();

        // ========== DECODED FRAMES INDICATOR (TOP OF SCREEN) ==========
        // Only show if enabled in settings (developer/debug feature)
        if (GetShowDecoderStatus() && !data.decoded_frames_map.empty() && data.total_frames > 0) {
            const int BAR_HEIGHT = 8;
            const int BAR_MARGIN = 10;
            const int BAR_Y = BAR_MARGIN;
            const int BAR_WIDTH = windowWidth - (BAR_MARGIN * 2);

            // Background (dark gray)
            SDL_SetRenderDrawColor(renderer, 40, 40, 40, 200);
            SDL_Rect bgRect = {BAR_MARGIN, BAR_Y, BAR_WIDTH, BAR_HEIGHT};
            SDL_RenderFillRect(renderer, &bgRect);

            // Decoded segments (green)
            SDL_SetRenderDrawColor(renderer, 0, 255, 0, 255);

            // Sample decoded frames map to fit screen width
            int samplesPerPixel = std::max(1, data.total_frames / BAR_WIDTH);

            for (int x = 0; x < BAR_WIDTH; ++x) {
                int frameStart = (x * data.total_frames) / BAR_WIDTH;
                int frameEnd = std::min(frameStart + samplesPerPixel, data.total_frames);

                // Check if any frame in this pixel range is decoded
                bool hasDecoded = false;
                for (int f = frameStart; f < frameEnd; ++f) {
                    if (f < static_cast<int>(data.decoded_frames_map.size()) && data.decoded_frames_map[f]) {
                        hasDecoded = true;
                        break;
                    }
                }

                if (hasDecoded) {
                    SDL_RenderDrawLine(renderer, BAR_MARGIN + x, BAR_Y, BAR_MARGIN + x, BAR_Y + BAR_HEIGHT - 1);
                }
            }

            // Current position indicator (white vertical line)
            if (data.total_duration > 0.0 && data.current_time >= 0.0) {
                double raw_time = data.current_time - data.timecode_offset_seconds;
                double position = raw_time / data.total_duration;
                int posX = BAR_MARGIN + static_cast<int>(BAR_WIDTH * position);
                SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
                SDL_RenderDrawLine(renderer, posX, BAR_Y, posX, BAR_Y + BAR_HEIGHT - 1);
            }

            // Border
            SDL_SetRenderDrawColor(renderer, 100, 100, 100, 255);
            SDL_RenderDrawRect(renderer, &bgRect);
        }
    }

    // Output profiling OSD once per second (DISABLED)
    // osd_profile_samples++;
    // if (osd_profile_samples >= 60) {
    //     uint64_t avg_timecode = total_timecode_us / osd_profile_samples;
    //     uint64_t avg_status = total_status_us / osd_profile_samples;
    //     uint64_t avg_other = total_other_us / osd_profile_samples;

    //     std::cout << "📊 [OSD DETAIL] Timecode: " << avg_timecode << "μs"
    //               << ", Status+Speed: " << avg_status << "μs"
    //               << ", VU+Indicators: " << avg_other << "μs" << std::endl;

    //     total_timecode_us = 0;
    //     total_status_us = 0;
    //     total_other_us = 0;
    //     osd_profile_samples = 0;
    // }

    // Restore original renderer
    g_osd_renderer = original_renderer;
}

// Rendering OSD with specified renderer (for multiple windows)
void RenderOSDWithRenderer(SDL_Renderer* renderer) {
    if (!renderer) return;

    // Save current renderer
    SDL_Renderer* original_renderer = g_osd_renderer;

    // Temporarily use passed renderer
    g_osd_renderer = renderer;

    // Call regular rendering function
    RenderOSD();

    g_osd_renderer = original_renderer;
}


// === Functions for managing player loading states ===

// Set loading state for specific player
void SetPlayerLoadingState(int player_id, bool is_loading) {
    // FIXED: Use 0-based indexing to match GetPlayerOSDData and other OSD functions
    if (player_id >= 0 && player_id < MAX_PLAYERS) {
        g_player_loading_states[player_id] = is_loading;
        if (!is_loading) {
            g_player_loading_progress[player_id] = 0;  // Reset progress when loading is finished
        }
    }
}

    // Check player loading state
bool IsPlayerLoading(int player_id) {
    // FIXED: Use 0-based indexing to match GetPlayerOSDData and other OSD functions
    if (player_id >= 0 && player_id < MAX_PLAYERS) {
        return g_player_loading_states[player_id];
    }
    return false;
}

// Set loading progress for specific player
void SetPlayerLoadingProgress(int player_id, int progress) {
    // FIXED: Use 0-based indexing to match GetPlayerOSDData and other OSD functions
    if (player_id >= 0 && player_id < MAX_PLAYERS) {
        g_player_loading_progress[player_id] = std::max(0, std::min(100, progress));
    }
}

// Get loading progress for specific player
int GetPlayerLoadingProgress(int player_id) {
    // FIXED: Use 0-based indexing to match GetPlayerOSDData and other OSD functions
    if (player_id >= 0 && player_id < MAX_PLAYERS) {
        return g_player_loading_progress[player_id];
    }
    return 0;
}

// Set loading status for specific player
void SetPlayerLoadingStatus(int player_id, const char* status) {
    if (!status) return;
    GetPlayerOSDData(player_id).loading_status = status;
}

// Toggle between time and frame numbers
void SetOSDFrameNumberMode(int player_id, bool show_frame_numbers) {
    GetPlayerOSDData(player_id).show_frame_numbers = show_frame_numbers;
}

bool GetOSDFrameNumberMode(int player_id) {
    return GetPlayerOSDData(player_id).show_frame_numbers;
}

// Update frame number from audio module
void UpdateOSDFrameNumber(int player_id, int frame_number) {
    GetPlayerOSDData(player_id).current_frame_number = frame_number;
}

// Set real file FPS for correct timecode display
void SetOSDFrameRate(int player_id, double fps) {
    auto& data = GetPlayerOSDData(player_id);
    data.fps = fps;
    std::cout << "📹 [OSD] Player " << player_id << " FPS set to " << fps << std::endl;
}

// Get current player mode for rendering optimization
const char* GetPlayerMode(int player_id) {
    auto& data = GetPlayerOSDData(player_id);

    if (data.seeking) {
        return "seek";
    }

    return data.last_stable_mode.c_str();
}

// Check: player in still mode (does not require frequent rendering)
bool IsPlayerInStillMode(int player_id) {
    auto& data = GetPlayerOSDData(player_id);

    // Still mode ONLY if:
    // 1. NOT in seek mode
    // 2. Actual speed < 0.1 (almost full stop)
    // 3. NOT in jog mode
    if (data.seeking) {
        return false;
    }

    if (data.jog_forward || data.jog_backward) {
        return false;  // Jog requires frequent rendering
    }

    double actual_speed = std::abs(data.actual_playback_rate);

    // Still mode only if speed is very small (< 0.1)
    // This works for pause and very slow playback
    return actual_speed < 0.1;
}

// Check: need to throttle rendering (for no file/loading screens and still mode)
bool ShouldThrottleRendering(int player_id) {
    auto& data = GetPlayerOSDData(player_id);

    // Throttle rendering for static screens:
    // - NO_FILE: static screen with gray timecode
    // - LOADING: blinking timecode (2 Hz = enough 10 FPS)
    // - STILL: player on pause (speed < 0.1) - enough 10 FPS
    // For normal mode (NORMAL) with playback - always render with full frequency
    if (data.display_mode == OSD_MODE_NO_FILE || data.display_mode == OSD_MODE_LOADING) {
        return true;
    }

    // Also throttle if player in still mode (pause)
    return IsPlayerInStillMode(player_id);
}

// Check: need to render OSD for player (with throttling)
bool ShouldRenderOSDForPlayer(int player_id) {
    auto& data = GetPlayerOSDData(player_id);

    // EXCEPTION: If zoom panning is active - render at 60 FPS without throttling
    if (IsZoomPanningActive()) {
        return true;
    }

    // If throttling is not needed - always render
    if (!ShouldThrottleRendering(player_id)) {
        return true;
    }

    // Throttling needed - check time
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - data.last_render_time).count();

    // 100ms = 10 FPS (enough for static screens and pause)
    if (elapsed < 100) {
        return false;  // Too early to render
    }

    // Update last render time
    data.last_render_time = now;
    return true;  // Can render
}


// ========== MENU BAR RENDERING (LINUX) ==========

// Render menu bar at top of window
extern "C" void RenderMenuBar(SDL_Renderer* renderer, int window_width, int window_height) {
    (void)window_height;  // Unused
    
    if (!renderer || !g_normal_font) return;

    const int MENU_BAR_HEIGHT = 24;
    const int MENU_ITEM_PADDING = 20;
    
    // Background for menu bar
    SDL_SetRenderDrawColor(renderer, 240, 240, 240, 255);
    SDL_Rect menu_bg = {0, 0, window_width, MENU_BAR_HEIGHT};
    SDL_RenderFillRect(renderer, &menu_bg);
    
    // Bottom border
    SDL_SetRenderDrawColor(renderer, 180, 180, 180, 255);
    SDL_RenderDrawLine(renderer, 0, MENU_BAR_HEIGHT - 1, window_width, MENU_BAR_HEIGHT - 1);
    
    // Menu items
    SDL_Color text_color = {0, 0, 0, 255};
    SDL_Color no_outline = {0, 0, 0, 0};  // No outline for menu text
    
    int x = 10;
    
    // File
    RenderCachedText(renderer, "File", x, 4, text_color, no_outline, g_normal_font);
    int file_width, file_height;
    TTF_SizeText(g_normal_font, "File", &file_width, &file_height);
    x += file_width + MENU_ITEM_PADDING;
    
    // Edit
    RenderCachedText(renderer, "Edit", x, 4, text_color, no_outline, g_normal_font);
    int edit_width, edit_height;
    TTF_SizeText(g_normal_font, "Edit", &edit_width, &edit_height);
    x += edit_width + MENU_ITEM_PADDING;
    
    // View
    RenderCachedText(renderer, "View", x, 4, text_color, no_outline, g_normal_font);
}

// Check if mouse click is on menu bar and which item
// Returns: -1 = no menu, 0 = File, 1 = Edit, 2 = View
extern "C" int CheckMenuBarClick(int mouse_x, int mouse_y, int window_width) {
    (void)window_width;  // Unused
    
    if (!g_normal_font) return -1;
    
    const int MENU_BAR_HEIGHT = 24;
    const int MENU_ITEM_PADDING = 20;
    
    // Check if click is in menu bar area
    if (mouse_y < 0 || mouse_y >= MENU_BAR_HEIGHT) {
        return -1;  // Not in menu bar
    }
    
    int x = 10;
    
    // File
    int file_width, file_height;
    TTF_SizeText(g_normal_font, "File", &file_width, &file_height);
    if (mouse_x >= x && mouse_x < x + file_width) {
        return 0;  // File menu
    }
    x += file_width + MENU_ITEM_PADDING;
    
    // Edit
    int edit_width, edit_height;
    TTF_SizeText(g_normal_font, "Edit", &edit_width, &edit_height);
    if (mouse_x >= x && mouse_x < x + edit_width) {
        return 1;  // Edit menu
    }
    x += edit_width + MENU_ITEM_PADDING;
    
    // View
    int view_width, view_height;
    TTF_SizeText(g_normal_font, "View", &view_width, &view_height);
    if (mouse_x >= x && mouse_x < x + view_width) {
        return 2;  // View menu
    }
    
    return -1;  // Clicked in menu bar but not on any item
}
