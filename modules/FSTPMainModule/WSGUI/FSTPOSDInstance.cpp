#include "FSTPOSDInstance.h"
#include "FSTPOSDSystem.h"
#include "fontdata.h"
#include "../FSTPPlayerModule/FSTPPlayerManager.h"
#include <iostream>
#include <cmath>
#include <sstream>
#include <iomanip>

FSTPOSDInstance::FSTPOSDInstance()
    : m_renderer(nullptr), m_large_font(nullptr), m_normal_font(nullptr), m_small_font(nullptr),
      m_initialized(false), m_player_id(-1), m_current_timecode("00:00:00:00"),
      m_playback_rate(1.0), m_is_reverse(false), m_is_playing(false),
      m_jog_forward(false), m_jog_backward(false), m_audio_left(0.0f), m_audio_right(0.0f),
      m_audio_left_peak(0.0f), m_audio_right_peak(0.0f), m_current_time(0.0),
      m_total_duration(0.0), m_seek_mode(false), m_input_timecode(""),
      m_display_mode(OSD_MODE_NO_FILE), m_loading_progress(0), m_is_audio_file(false) {
}

FSTPOSDInstance::~FSTPOSDInstance() {
    if (m_initialized) {
        Shutdown();
    }
}

int FSTPOSDInstance::Initialize(SDL_Renderer* renderer, int player_id) {
    if (m_initialized) {
        std::cout << "OSD instance already initialized for player " << player_id << std::endl;
        return 0;
    }

    if (!renderer) {
        std::cerr << "Invalid renderer for OSD instance" << std::endl;
        return -1;
    }

    std::cout << "Initializing OSD instance for player " << player_id << std::endl;

    m_renderer = renderer;
    m_player_id = player_id;

    // Initialize TTF if not already initialized globally
    if (TTF_WasInit() == 0) {
        if (TTF_Init() < 0) {
            std::cerr << "TTF_Init failed: " << TTF_GetError() << std::endl;
            return -2;
        }
    }

    // Load fonts from embedded data - EXACTLY as in the original
    m_large_font = TTF_OpenFontRW(SDL_RWFromConstMem(font_otf, font_otf_size), 1, 32); // Large for timecode
    m_normal_font = TTF_OpenFontRW(SDL_RWFromConstMem(font_otf, font_otf_size), 1, 16); // Normal for status
    m_small_font = TTF_OpenFontRW(SDL_RWFromConstMem(font_otf, font_otf_size), 1, 12);  // Small for scale

    if (!m_large_font || !m_normal_font || !m_small_font) {
        std::cerr << "Failed to load fonts for OSD instance: " << TTF_GetError() << std::endl;
        return -4;
    }

    m_initialized = true;
    std::cout << "OSD instance initialized successfully for player " << player_id << std::endl;

    return 0;
}

void FSTPOSDInstance::Shutdown() {
    if (!m_initialized) {
        return;
    }

    std::cout << "Shutting down OSD instance for player " << m_player_id << std::endl;

    // Release fonts
    if (m_large_font) {
        TTF_CloseFont(m_large_font);
        m_large_font = nullptr;
    }

    if (m_normal_font) {
        TTF_CloseFont(m_normal_font);
        m_normal_font = nullptr;
    }

    if (m_small_font) {
        TTF_CloseFont(m_small_font);
        m_small_font = nullptr;
    }

    m_renderer = nullptr;
    m_initialized = false;
    m_player_id = -1;

    std::cout << "OSD instance shutdown complete" << std::endl;
}

void FSTPOSDInstance::SetFonts(TTF_Font* large_font, TTF_Font* normal_font, TTF_Font* small_font) {
    m_large_font = large_font;
    m_normal_font = normal_font;
    m_small_font = small_font;
}

void FSTPOSDInstance::UpdateTimecode(double currentTime) {
    int hours = (int)(currentTime / 3600);
    int minutes = (int)((currentTime - hours * 3600) / 60);
    int seconds = (int)(currentTime - hours * 3600 - minutes * 60);

    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(2) << hours << ":"
        << std::setw(2) << minutes << ":"
        << std::setw(2) << seconds;

    if (m_is_audio_file) {
        // For audio files: HH:MM:SS.CS (centiseconds)
        int centiseconds = (int)((currentTime - (int)currentTime) * 100) % 100;
        oss << "." << std::setfill('0') << std::setw(2) << centiseconds;
    } else {
        // For video files: HH:MM:SS:FF (frames)
        double video_fps = GetInstanceVideoFPS(m_player_id);
        if (video_fps <= 0) video_fps = 25.0; // fallback
        int frames = (int)((currentTime - (int)currentTime) * video_fps);
        oss << ":" << std::setfill('0') << std::setw(2) << frames;
    }

    m_current_timecode = oss.str();
    m_current_time = currentTime;
}

void FSTPOSDInstance::UpdateSpeed(double playbackRate, bool isReverse) {
    m_playback_rate = playbackRate;
    m_is_reverse = isReverse;
}

void FSTPOSDInstance::UpdatePlayState(bool isPlaying, bool jog_forward, bool jog_backward) {
    m_is_playing = isPlaying;
    m_jog_forward = jog_forward;
    m_jog_backward = jog_backward;
}

void FSTPOSDInstance::UpdateAudioLevels(float left, float right, float leftPeak, float rightPeak) {
    m_audio_left = left;
    m_audio_right = right;
    m_audio_left_peak = leftPeak;
    m_audio_right_peak = rightPeak;
}

void FSTPOSDInstance::UpdatePosition(double currentTime, double totalDuration) {
    m_current_time = currentTime;
    m_total_duration = totalDuration;
}

void FSTPOSDInstance::UpdateSeekMode(bool seeking, const std::string& input_timecode) {
    m_seek_mode = seeking;
    m_input_timecode = input_timecode;
}

void FSTPOSDInstance::UpdateDisplayMode(OSDDisplayMode mode) {
    m_display_mode = mode;
}

void FSTPOSDInstance::UpdateLoadingProgress(int percent) {
    m_loading_progress = percent;
}

void FSTPOSDInstance::SetLoadingState(bool is_loading) {
    // This method can be used to set the loading state
    // For now, just update the display mode
    if (is_loading) {
        m_display_mode = OSD_MODE_LOADING;
    } else {
        m_display_mode = OSD_MODE_NORMAL;
    }
}

void FSTPOSDInstance::SetFileType(bool is_audio) {
    m_is_audio_file = is_audio;
    // Update initial timecode with correct format
    if (is_audio) {
        m_current_timecode = "00:00:00.00"; // Audio format (centiseconds)
    } else {
        m_current_timecode = "00:00:00:00";  // Video format (frames)
    }
}

void FSTPOSDInstance::Render() {
    if (!m_initialized || !m_renderer) {
        return;
    }

    switch (m_display_mode) {
        case OSD_MODE_NORMAL:
            RenderTimecode();
            RenderTransportControls();
            RenderVUMeters();
            RenderPositionBar();
            if (m_seek_mode) {
                RenderSeekMode();
            }
            break;

        case OSD_MODE_LOADING:
            RenderLoadingScreen();
            break;

        case OSD_MODE_NO_FILE:
            RenderNoFileScreen();
            break;
    }
}

// Helper function for rendering text with outline - EXACTLY as in the original
void FSTPOSDInstance::RenderTextWithOutline(const char* text, int x, int y, SDL_Color color, SDL_Color outline_color, TTF_Font* font) {
    if (!font || !m_renderer) return;

    // Render outline (8 directions)
    SDL_Surface* outline_surface = TTF_RenderText_Blended(font, text, outline_color);
    if (outline_surface) {
        SDL_Texture* outline_texture = SDL_CreateTextureFromSurface(m_renderer, outline_surface);
        if (outline_texture) {
            for (int dx = -1; dx <= 1; dx++) {
                for (int dy = -1; dy <= 1; dy++) {
                    if (dx != 0 || dy != 0) {
                        SDL_Rect outline_rect = {x + dx, y + dy, outline_surface->w, outline_surface->h};
                        SDL_RenderCopy(m_renderer, outline_texture, nullptr, &outline_rect);
                    }
                }
            }
            // Force clear Metal command buffer before release
            SDL_RenderFlush(m_renderer);
            SDL_DestroyTexture(outline_texture);
        }
        SDL_FreeSurface(outline_surface);
    }

    // Render main text
    SDL_Surface* surface = TTF_RenderText_Blended(font, text, color);
    if (!surface) return;

    SDL_Texture* texture = SDL_CreateTextureFromSurface(m_renderer, surface);
    if (!texture) {
        SDL_FreeSurface(surface);
        return;
    }

    SDL_Rect dst_rect = { x, y, surface->w, surface->h };
    SDL_RenderCopy(m_renderer, texture, nullptr, &dst_rect);

    // Force clear Metal command buffer before releasing main texture
    SDL_RenderFlush(m_renderer);
    SDL_DestroyTexture(texture);
    SDL_FreeSurface(surface);
}

void FSTPOSDInstance::RenderTimecode() {
    if (!m_large_font || !m_normal_font || !m_renderer) return;

    int windowWidth, windowHeight;
    SDL_GetRendererOutputSize(m_renderer, &windowWidth, &windowHeight);

    // Colors - EXACTLY as in the original
    SDL_Color textColor = {255, 255, 255, 255};
    SDL_Color shadowColor = {0, 0, 0, 180};
    SDL_Color seekColor = {255, 255, 0, 255}; // Yellow for seek mode

    // Central timecode - use large font
    SDL_Color timecodeColor = m_seek_mode ? seekColor : textColor;

    // Calculate timecode size for centering
    int text_width, text_height;
    TTF_SizeText(m_large_font, m_current_timecode.c_str(), &text_width, &text_height);

    int timecodeX = (windowWidth - text_width) / 2;
    int timecodeY = windowHeight - 80; // At the bottom of the screen - AS IN THE ORIGINAL

    RenderTextWithOutline(m_current_timecode.c_str(), timecodeX, timecodeY, timecodeColor, shadowColor, m_large_font);
}

void FSTPOSDInstance::RenderTransportControls() {
    if (!m_normal_font) return;

    std::string status = "STOP";
    SDL_Color color = {255, 0, 0, 255}; // Red for STOP

    if (m_is_playing) {
        status = "PLAY";
        color = {0, 255, 0, 255}; // Green for PLAY
    } else if (m_jog_forward || m_jog_backward) {
        status = m_jog_forward ? "JOG FWD" : "JOG BWD";
        color = {255, 255, 0, 255}; // Yellow for JOG
    }

    // Add speed information
    if (m_playback_rate != 1.0) {
        std::ostringstream oss;
        oss << status << " " << std::fixed << std::setprecision(1) << m_playback_rate << "x";
        if (m_is_reverse) oss << " REV";
        status = oss.str();
    }

    SDL_Surface* surface = TTF_RenderText_Blended(m_normal_font, status.c_str(), color);
    if (!surface) return;

    SDL_Texture* texture = SDL_CreateTextureFromSurface(m_renderer, surface);
    if (!texture) {
        SDL_FreeSurface(surface);
        return;
    }

    int w, h;
    SDL_QueryTexture(texture, nullptr, nullptr, &w, &h);

    // Position status below timecode
    SDL_Rect dst_rect = {20, 100, w, h};
    SDL_RenderCopy(m_renderer, texture, nullptr, &dst_rect);

    SDL_DestroyTexture(texture);
    SDL_FreeSurface(surface);
}

void FSTPOSDInstance::RenderVUMeters() {
    // Render VU meters on the right side of the screen
    int renderer_w, renderer_h;
    SDL_GetRendererOutputSize(m_renderer, &renderer_w, &renderer_h);

    const int meter_width = 20;
    const int meter_height = 200;
    const int meter_x = renderer_w - 60;
    const int left_meter_y = 50;
    const int right_meter_y = 50;

    // Left channel
    SDL_Rect left_bg = {meter_x - 30, left_meter_y, meter_width, meter_height};
    SDL_SetRenderDrawColor(m_renderer, 40, 40, 40, 255);
    SDL_RenderFillRect(m_renderer, &left_bg);

    // Right channel
    SDL_Rect right_bg = {meter_x, right_meter_y, meter_width, meter_height};
    SDL_RenderFillRect(m_renderer, &right_bg);

    // Audio levels (simulation)
    int left_level = (int)(m_audio_left * meter_height);
    int right_level = (int)(m_audio_right * meter_height);

    if (left_level > 0) {
        SDL_Rect left_fill = {meter_x - 30, left_meter_y + meter_height - left_level,
                             meter_width, left_level};
        SDL_SetRenderDrawColor(m_renderer, 0, 255, 0, 255);
        SDL_RenderFillRect(m_renderer, &left_fill);
    }

    if (right_level > 0) {
        SDL_Rect right_fill = {meter_x, right_meter_y + meter_height - right_level,
                              meter_width, right_level};
        SDL_SetRenderDrawColor(m_renderer, 0, 255, 0, 255);
        SDL_RenderFillRect(m_renderer, &right_fill);
    }
}

void FSTPOSDInstance::RenderPositionBar() {
    if (m_total_duration <= 0) return;

    int renderer_w, renderer_h;
    SDL_GetRendererOutputSize(m_renderer, &renderer_w, &renderer_h);

    const int bar_width = renderer_w - 40;
    const int bar_height = 4;
    const int bar_x = 20;
    const int bar_y = renderer_h - 60;

    // Position bar background
    SDL_Rect bg_rect = {bar_x, bar_y, bar_width, bar_height};
    SDL_SetRenderDrawColor(m_renderer, 60, 60, 60, 255);
    SDL_RenderFillRect(m_renderer, &bg_rect);

    // Current position
    double progress = m_current_time / m_total_duration;
    int pos_width = (int)(progress * bar_width);

    SDL_Rect pos_rect = {bar_x, bar_y, pos_width, bar_height};
    SDL_SetRenderDrawColor(m_renderer, 255, 255, 255, 255);
    SDL_RenderFillRect(m_renderer, &pos_rect);
}

void FSTPOSDInstance::RenderSeekMode() {
    // Render seek mode with input timecode
    // TODO: Implement display of input timecode
}

void FSTPOSDInstance::RenderLoadingScreen() {
    RenderTimecode(); // Flashing timecode

    if (!m_normal_font) return;

    std::ostringstream oss;
    oss << "Loading Player " << m_player_id << "... " << m_loading_progress << "%";
    std::string loading_text = oss.str();

    SDL_Color color = {255, 255, 0, 255}; // Yellow
    SDL_Surface* surface = TTF_RenderText_Blended(m_normal_font, loading_text.c_str(), color);
    if (!surface) return;

    SDL_Texture* texture = SDL_CreateTextureFromSurface(m_renderer, surface);
    if (!texture) {
        SDL_FreeSurface(surface);
        return;
    }

    int w, h;
    SDL_QueryTexture(texture, nullptr, nullptr, &w, &h);

    int renderer_w, renderer_h;
    SDL_GetRendererOutputSize(m_renderer, &renderer_w, &renderer_h);

    // Center loading message
    SDL_Rect dst_rect = {(renderer_w - w) / 2, (renderer_h - h) / 2, w, h};
    SDL_RenderCopy(m_renderer, texture, nullptr, &dst_rect);

    SDL_DestroyTexture(texture);
    SDL_FreeSurface(surface);
}

void FSTPOSDInstance::RenderNoFileScreen() {
    if (!m_normal_font) return;

    std::ostringstream oss;
    oss << "Player " << m_player_id << " - No File Loaded";
    std::string no_file_text = oss.str();

    SDL_Color color = {128, 128, 128, 255}; // Gray
    SDL_Surface* surface = TTF_RenderText_Blended(m_normal_font, no_file_text.c_str(), color);
    if (!surface) return;

    SDL_Texture* texture = SDL_CreateTextureFromSurface(m_renderer, surface);
    if (!texture) {
        SDL_FreeSurface(surface);
        return;
    }

    int w, h;
    SDL_QueryTexture(texture, nullptr, nullptr, &w, &h);

    int renderer_w, renderer_h;
    SDL_GetRendererOutputSize(m_renderer, &renderer_w, &renderer_h);

    // Center message
    SDL_Rect dst_rect = {(renderer_w - w) / 2, (renderer_h - h) / 2, w, h};
    SDL_RenderCopy(m_renderer, texture, nullptr, &dst_rect);

    SDL_DestroyTexture(texture);
    SDL_FreeSurface(surface);
}