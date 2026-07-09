#ifndef FSTP_SETTINGS_H
#define FSTP_SETTINGS_H

#ifdef __cplusplus
extern "C" {
#endif

#define FSTP_EXTENSION_SCRIPT_LANGUAGE "Lua"

// Current welcome/onboarding revision. Bump this when the welcome screen changes
// meaningfully — users who saw an older revision will be shown the screen again
// (same versioning idea as the proxy cache manifest schema).
//   1 — SwiftUI 5-card deck (macOS only)
//   2 — simplified cross-platform SDL overlay (FSTPWelcomeScreen)
#define FSTP_WELCOME_VERSION 2

// TapeXPlayer settings structure (minimal version)
typedef struct {
    // Audio settings
    int audio_device_index;         // Audio device index
    float audio_master_volume;      // 0.0 - 1.0
    int audio_buffer_size;          // 512, 1024, 2048, 4096
    int audio_volume_ducking_enabled; // 1 = Auto-reduce volume at high shuttle speeds (ear protection), 0 = disabled
    char audio_device_name[256];      // Preferred output device NAME (stable across PortAudio index shifts); "" = none
    int audio_follow_default;         // 1 = follow the OS default output device, 0 = pin to audio_device_name

    // A/V synchronization settings
    int frame_offset;               // Frame offset for monitor delay compensation (-10 to +10)

    // Multi-instance settings (protection from forgotten players)
    int auto_freeze_inactive;       // 1 = automatically freeze inactive players, 0 = disabled
    int betacam_effect_enabled;     // 1 = Betacam effect enabled, 0 = disabled
    int yt_dlp_extension_enabled;   // 1 = yt-dlp extension enabled, 0 = disabled

    // MIDI settings
    int midi_enabled;               // 1 = MIDI controller enabled, 0 = disabled
    int midi_input_port;            // Input MIDI port index (-1 = not selected)
    int midi_output_port;           // Output MIDI port index (-1 = not selected)

    // Developer/Debug settings
    int show_decoder_status;        // 1 = Show decoder status indicator (OSD), 0 = hidden (default)

    // Presentation mode settings
    int presentation_display_index; // Preferred display for presentation window (-1 = auto/first external)
    int presentation_output_mode;   // 0 = external display, 1 = separate window
    int presentation_follow_focus;  // 1 = follow focused player (default), 0 = pinned to one player
    int presentation_pinned_player; // Player id used when follow_focus == 0

    // Onboarding / first-run
    int welcome_version;            // Highest welcome-screen revision the user has seen (0 = never)

} FSTPSettings;

// Settings management functions
int InitSettings();
void ShutdownSettings();
FSTPSettings* GetSettings();
int SaveSettings();
int LoadSettings();
void ResetSettingsToDefault();

// Show settings dialog (platform-dependent)
void ShowSettingsDialog();

// Apply settings to audio system
void ApplyAudioSettings();

// Get individual settings (convenience functions)
int GetAudioDeviceIndex();
float GetMasterVolume();
int GetAudioBufferSize();
const char* GetAudioDeviceName();
int GetAudioFollowDefault();
void SetAudioDevice(int index, const char* name);   // pin to a specific device (sets follow_default=0)
void SetAudioFollowDefault(int follow);             // 1 = follow system default, 0 = pin
int GetAudioVolumeDuckingEnabled(); // Get volume ducking (ear protection) state
int GetFrameOffset();          // Get frame offset
int GetAutoFreezeInactive();   // Get auto-freeze setting
int GetBetacamEffectEnabled(); // Get Betacam effect state
int GetYTDLPExtensionEnabled(); // Get yt-dlp extension state
void SetYTDLPExtensionEnabled(int enabled);
int GetMIDIEnabled();          // Get MIDI state
int GetMIDIInputPort();        // Get input MIDI port
int GetMIDIOutputPort();       // Get output MIDI port
const char* GetExtensionLanguage(); // Get script language for extensions
int GetShowDecoderStatus();    // Get decoder status display setting
int GetPresentationDisplayIndex(); // Get preferred display index for presentation mode
void SetPresentationDisplayIndex(int idx); // Save preferred display index
int GetPresentationOutputMode();   // 0 = external display, 1 = separate window
void SetPresentationOutputMode(int mode);
int GetPresentationFollowFocus();  // 1 = follow focused player, 0 = pinned
void SetPresentationFollowFocus(int follow);
int GetPresentationPinnedPlayer(); // Player id used when follow_focus == 0
void SetPresentationPinnedPlayer(int player_id);

// Onboarding / first-run
int GetWelcomeVersion();            // Highest welcome revision the user has seen (0 = first run)
void SetWelcomeVersion(int version); // Persist the welcome revision the user has seen

// yt-dlp integration helpers
int FSTP_YTDLP_IsAvailable(void);
int FSTP_YTDLP_Download(const char* url, char* out_path, int out_path_size, char* error_buf, int error_buf_size);
const char* FSTP_YTDLP_GetDownloadsDir(void);

// Resume positions — remember last playback position per file
void SaveResumePosition(const char* filepath, double position_seconds);
double LoadResumePosition(const char* filepath);  // returns -1.0 if not found
void ClearResumePosition(const char* filepath);

// Recent files — MRU list for the "Open Recent" menu.
// AddRecentFile moves the path to the front (dedup) and persists; the list is
// capped internally. GetRecentFile returns NULL for an out-of-range index; the
// returned pointer is valid until the list is next modified.
void AddRecentFile(const char* filepath);
int GetRecentFileCount();
const char* GetRecentFile(int index);
void ClearRecentFiles();

#ifdef __cplusplus
}
#endif

#endif // FSTP_SETTINGS_H
