#ifndef FSTP_SETTINGS_H
#define FSTP_SETTINGS_H

#ifdef __cplusplus
extern "C" {
#endif

// TapeXPlayer settings structure (minimal version)
typedef struct {
    // Audio settings
    int audio_device_index;         // Audio device index
    float audio_master_volume;      // 0.0 - 1.0
    int audio_buffer_size;          // 512, 1024, 2048, 4096

    // A/V synchronization settings
    int frame_offset;               // Frame offset for monitor delay compensation (-10 to +10)

    // Multi-instance settings (protection from forgotten players)
    int auto_freeze_inactive;       // 1 = automatically freeze inactive players, 0 = disabled

    // MIDI settings
    int midi_enabled;               // 1 = MIDI controller enabled, 0 = disabled
    int midi_input_port;            // Input MIDI port index (-1 = not selected)
    int midi_output_port;           // Output MIDI port index (-1 = not selected)

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
int GetFrameOffset();          // Get frame offset
int GetAutoFreezeInactive();   // Get auto-freeze setting
int GetMIDIEnabled();          // Get MIDI state
int GetMIDIInputPort();        // Get input MIDI port
int GetMIDIOutputPort();       // Get output MIDI port

#ifdef __cplusplus
}
#endif

#endif // FSTP_SETTINGS_H