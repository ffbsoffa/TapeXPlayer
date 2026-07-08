#ifndef FSTPOSD_SYSTEM_H
#define FSTPOSD_SYSTEM_H

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <string>
#include <vector>

#ifdef __cplusplus
extern "C" {
#endif

// OSD playback states
typedef enum {
    OSD_STOP = 0,
    OSD_PLAY = 1,
    OSD_PAUSE = 2,
    OSD_REWIND = 3,
    OSD_FAST_FORWARD = 4
} OSDPlayState;

// OSD screen modes
typedef enum {
    OSD_MODE_NORMAL = 0,    // Normal playback
    OSD_MODE_LOADING = 1,   // File loading
    OSD_MODE_NO_FILE = 2    // No file
} OSDDisplayMode;

// OSD system initialization
int InitOSDSystem(SDL_Renderer* renderer);

// Font setup (large and normal)
void SetOSDFonts(TTF_Font* large_font, TTF_Font* normal_font, TTF_Font* small_font);

// Clear OSD cache for specific renderer (call before SDL_DestroyRenderer!)
void ClearOSDCacheForRenderer(SDL_Renderer* renderer);

// OSD system shutdown
void ShutdownOSDSystem();

// Update OSD data for specific player
void UpdateOSDTimecode(int player_id, double currentTime);
void UpdateOSDSpeed(int player_id, double playbackRate, bool isReverse);
void UpdateOSDActualSpeed(int player_id, double actualPlaybackRate);
void UpdateOSDFullResMode(int player_id, bool is_full_res);
void UpdateOSDPlayState(int player_id, bool isPlaying, bool jog_forward, bool jog_backward);
void UpdateOSDAudioLevels(int player_id, float left, float right, float leftPeak, float rightPeak);
void UpdateOSDPosition(int player_id, double currentTime, double totalDuration);
void UpdateOSDSeekMode(int player_id, bool seeking, const std::string& input_timecode);
void UpdateOSDDisplayMode(int player_id, OSDDisplayMode mode);
void UpdateOSDLoadingProgress(int player_id, int percent);
// TAPE THREADING badge: background proxy conversion progress shown while the video
// already plays (transport limited to ≤1× forward). percent 0-100 shows "thread nn%",
// -1 hides the badge. Safe to call from any thread (single int store, like other OSD setters).
void UpdateOSDProxyThreading(int player_id, int percent);
void SetOSDFileType(int player_id, bool is_audio);
void SetOSDTimecodeOffset(int player_id, double offset_seconds);
void UpdateOSDDecodedFrames(int player_id, const std::vector<bool>& decoded_map, int total_frames);

// Backward compatibility - functions without player_id (use player 0)
// Declarations are in .cpp file

// OSD element rendering
void RenderOSD();

// OSD rendering with specified renderer (for multiple windows)
void RenderOSDWithRenderer(SDL_Renderer* renderer);

// OSD rendering for specific player with specified renderer
void RenderOSDForPlayer(SDL_Renderer* renderer, int player_id);

// Check: should OSD be rendered for player (considers throttling for still mode)
bool ShouldRenderOSDForPlayer(int player_id);

// Set loading state for specific player
void SetPlayerLoadingState(int player_id, bool is_loading);

// Check player loading state
bool IsPlayerLoading(int player_id);

// Set loading progress for specific player
void SetPlayerLoadingProgress(int player_id, int progress);

// Get player loading progress
int GetPlayerLoadingProgress(int player_id);

// Set loading stage text (threading, indexing, proxy)
void SetPlayerLoadingStatus(int player_id, const char* status);

// Toggle between time and frame number display
void SetOSDFrameNumberMode(int player_id, bool show_frame_numbers);
bool GetOSDFrameNumberMode(int player_id);

// Update frame number from audio module
void UpdateOSDFrameNumber(int player_id, int frame_number);

// Set real file FPS for correct timecode display
void SetOSDFrameRate(int player_id, double fps);

// Get current player mode for rendering optimization
// Returns: "still", "play", "jog", "shuttle", "seek"
const char* GetPlayerMode(int player_id);

// Check: player in still mode (doesn't require frequent rendering)
bool IsPlayerInStillMode(int player_id);

// Check: should rendering FPS be limited (for no file/loading screens)
// Returns true if can render at reduced frequency (10 FPS instead of 60)
bool ShouldThrottleRendering(int player_id);

// Menu bar rendering (Linux)
void RenderMenuBar(SDL_Renderer* renderer, int window_width, int window_height);

// Check if mouse click is on menu bar
// Returns: -1 = no menu, 0 = File, 1 = Edit, 2 = View
int CheckMenuBarClick(int mouse_x, int mouse_y, int window_width);

#ifdef __cplusplus
}
#endif

#endif // FSTPOSD_SYSTEM_H