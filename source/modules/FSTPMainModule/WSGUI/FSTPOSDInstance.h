#ifndef FSTP_OSD_INSTANCE_H
#define FSTP_OSD_INSTANCE_H

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <string>

// Include enum definitions from FSTPOSDSystem.h
// But only if they're not already defined
#ifndef FSTPOSD_SYSTEM_H
#include "FSTPOSDSystem.h"
#endif

// Class for individual OSD instance for specific window
class FSTPOSDInstance {
private:
    SDL_Renderer* m_renderer;
    TTF_Font* m_large_font;
    TTF_Font* m_normal_font;
    TTF_Font* m_small_font;

    bool m_initialized;
    int m_player_id;  // ID of bound player

    // OSD data for this instance
    std::string m_current_timecode;
    double m_playback_rate;
    bool m_is_reverse;
    bool m_is_playing;
    bool m_jog_forward;
    bool m_jog_backward;
    float m_audio_left;
    float m_audio_right;
    float m_audio_left_peak;
    float m_audio_right_peak;
    double m_current_time;
    double m_total_duration;
    bool m_seek_mode;
    std::string m_input_timecode;
    OSDDisplayMode m_display_mode;
    int m_loading_progress;
    bool m_is_audio_file;  // Flag for determining file type

    // Helper rendering methods
    void RenderTextWithOutline(const char* text, int x, int y, SDL_Color color, SDL_Color outline_color, TTF_Font* font);
    void RenderTimecode();
    void RenderTransportControls();
    void RenderVUMeters();
    void RenderPositionBar();
    void RenderSeekMode();
    void RenderLoadingScreen();
    void RenderNoFileScreen();

public:
    FSTPOSDInstance();
    ~FSTPOSDInstance();

    // Initialization with binding to renderer and player
    int Initialize(SDL_Renderer* renderer, int player_id);

    // Shutdown
    void Shutdown();

    // Font setup
    void SetFonts(TTF_Font* large_font, TTF_Font* normal_font, TTF_Font* small_font);

    // OSD data updates
    void UpdateTimecode(double currentTime);
    void UpdateSpeed(double playbackRate, bool isReverse);
    void UpdatePlayState(bool isPlaying, bool jog_forward, bool jog_backward);
    void UpdateAudioLevels(float left, float right, float leftPeak, float rightPeak);
    void UpdatePosition(double currentTime, double totalDuration);
    void UpdateSeekMode(bool seeking, const std::string& input_timecode);
    void UpdateDisplayMode(OSDDisplayMode mode);
    void UpdateLoadingProgress(int percent);
    void SetLoadingState(bool is_loading);
    void SetFileType(bool is_audio);  // Set file type

    // OSD element rendering
    void Render();

    // State checks
    bool IsInitialized() const { return m_initialized; }
    int GetPlayerID() const { return m_player_id; }
    OSDDisplayMode GetDisplayMode() const { return m_display_mode; }
};

#endif // FSTP_OSD_INSTANCE_H