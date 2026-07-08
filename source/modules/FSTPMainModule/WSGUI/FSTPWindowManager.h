#ifndef FSTP_WINDOW_MANAGER_H
#define FSTP_WINDOW_MANAGER_H

#include <SDL2/SDL.h>
#include <memory>

// Forward declaration for C++
#ifdef __cplusplus
class FSTPOSDInstance;
class FSTPPixelBufferManager;
extern "C" {
#endif

// Maximum number of windows
#define MAX_WINDOWS 5

// Hard binding of windows to players
// Window 0 is always bound to player 0, window 1 to player 1, etc.
#define WINDOW_PLAYER_BINDING(window_index) (window_index)

// Structure for individual window
typedef struct {
    SDL_Window* window;
    SDL_Renderer* renderer;
    Uint32 window_id;
    bool is_active;
    bool has_focus;
    bool is_minimized;
    bool is_closing;  // Set to true when close is requested, prevents OSD updates
    int player_instance_id;  // ID of bound player instance (-1 if not bound)
    char window_title[256];

    // DOUBLE TEXTURE BUFFERING to eliminate dropouts
    SDL_Texture* texture_buffer[2];        // Two texture buffers
    int texture_buffer_width[2];           // Sizes for each buffer
    int texture_buffer_height[2];
    double texture_buffer_timestamp[2];    // Timestamps
    bool texture_buffer_valid[2];          // Validity of each buffer
    int current_buffer_index;              // Current active buffer (0 or 1)
    int write_buffer_index;                // Buffer for writing (0 or 1)
    int last_rendered_frame;               // Last frame number rendered (for effects)
    int betacam_hold_frames;
    double last_effect_speed;
    bool last_frame_aligned;         // Previous IsFrameAligned() state (for transition detection)
    uint64_t last_settled_render_ms; // Timestamp of last render in settled pause (for throttling)
    
    // DEPRECATED fields - kept for compatibility but use new buffers
    SDL_Texture* video_texture;            // -> texture_buffer[current_buffer_index]
    SDL_Texture* previous_video_texture;   // For deferred deallocation
    int video_width;                       // -> texture_buffer_width[current_buffer_index]
    int video_height;                      // -> texture_buffer_height[current_buffer_index]
    double video_timestamp;                // -> texture_buffer_timestamp[current_buffer_index]
    bool video_texture_valid;              // -> texture_buffer_valid[current_buffer_index]

#ifdef __cplusplus
    FSTPOSDInstance* osd_instance;  // OSD instance for this window
#else
    void* osd_instance;  // For C
#endif
} FSTPWindow;

// Window manager initialization
int InitWindowManager();

// Window manager shutdown
void ShutdownWindowManager();

// Create new window
int CreateNewWindow(const char* title, int width, int height);

// Close window by ID
void FSTPCloseWindow(int window_index);

// Get player ID for window (hard binding)
int GetPlayerIDForWindow(int window_index);

// Check if window-player binding is active
int IsWindowPlayerBindingActive(int window_index);

// Get window by SDL window ID
FSTPWindow* GetWindowBySDLID(Uint32 sdl_window_id);

// Get window by index
FSTPWindow* GetWindowByIndex(int index);

// Handle events for all windows
void HandleWindowEvents(SDL_Event* event);

// Render all active windows
void RenderAllWindows();

// Get number of active windows
int GetActiveWindowCount();

// Get main window (index 0)
FSTPWindow* GetMainWindow();

// Get active (focused) window
FSTPWindow* FSTPGetActiveWindow();

// Get active window's player ID
int GetActivePlayerID();

// Restore focus to main SDL window
void RestoreFocusToMainWindow();

// === Texture interface for video ===
// Get SDL renderer for window (for texture interface initialization)
SDL_Renderer* GetWindowRenderer(int window_index);

// New safe functions for pixel data
// Forward declaration for AVFrame (zero-copy architecture)
struct AVFrame;

// Legacy method with data copying (deprecated)
void SubmitPixelData(int player_id, const uint8_t* pixel_data, int width, int height,
                    unsigned int format, double timestamp, int frame_number);

FSTPPixelBufferManager* GetPixelBufferManager();

// Runtime control over Betacam effect
void SetBetacamEffectEnabled(int enabled);

// REMOVED: RenderVideoTextures() - now only RenderAllWindows() is used

// === OSD management for windows ===
// Update OSD for specific window
void UpdateWindowOSD(int window_index, double current_time, double total_duration, bool is_playing, double speed, bool is_reverse);
void UpdateWindowOSDLoading(int window_index, bool is_loading, int progress);

// Update loading progress for window
void UpdateWindowLoadingProgress(int window_index, int progress);

// Update window title with instance number and filename
void UpdateWindowTitle(int window_index, int instance_id, const char* filename);

// Presentation display enumeration for the Settings UI. Uses the SAME SDL indices as the
// presentation window code, so a choice made in Settings maps 1:1 to the output target.
int FSTP_GetPresentationDisplayCount(void);
const char* FSTP_GetPresentationDisplayName(int idx);
// Re-apply the presentation output target if presentation is currently active — called after a
// Settings change so display/output-mode edits take effect live.
void FSTP_ReapplyPresentationIfActive(void);

#ifdef __cplusplus
}  // end extern "C"

// Presentation mode: output clean video (no OSD / indicators / cursor — ever) to an external
// display or, as a fallback / by choice, a separate movable window.
//   output_mode: 0 = external display (falls back to windowed if none), 1 = separate window
bool CreatePresentationWindow(int player_id, int output_mode = 0, int display_index = -1);
void ClosePresentationWindow();
bool IsPresentationWindowActive();
void SetPresentationModeEnabled(bool enabled);
bool IsPresentationModeEnabled();
// Focus/pin: follow_focus = output follows the focused player; otherwise pinned to pinned_player.
void SetPresentationFollow(bool follow_focus, int pinned_player);

// ZERO-COPY method - pass shared_ptr<AVFrame> directly (RECOMMENDED)
void SubmitAVFrame(int player_id, std::shared_ptr<AVFrame> av_frame,
                  double timestamp, int frame_number,
                  std::shared_ptr<AVFrame> prev_frame = nullptr,
                  std::shared_ptr<AVFrame> next_frame = nullptr);
#endif

#endif // FSTP_WINDOW_MANAGER_H
