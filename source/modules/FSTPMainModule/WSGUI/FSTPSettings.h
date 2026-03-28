#ifndef FSTP_SETTINGS_H
#define FSTP_SETTINGS_H

#ifdef __cplusplus
extern "C" {
#endif

#define FSTP_EXTENSION_SCRIPT_LANGUAGE "Lua"

// TapeXPlayer settings structure (minimal version)
typedef struct {
    // Audio settings
    int audio_device_index;         // Audio device index
    float audio_master_volume;      // 0.0 - 1.0
    int audio_buffer_size;          // 512, 1024, 2048, 4096
    int audio_volume_ducking_enabled; // 1 = Auto-reduce volume at high shuttle speeds (ear protection), 0 = disabled

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

// yt-dlp integration helpers
int FSTP_YTDLP_IsAvailable(void);
int FSTP_YTDLP_Download(const char* url, char* out_path, int out_path_size, char* error_buf, int error_buf_size);
const char* FSTP_YTDLP_GetDownloadsDir(void);

#ifdef __cplusplus
}
#endif

#endif // FSTP_SETTINGS_H
