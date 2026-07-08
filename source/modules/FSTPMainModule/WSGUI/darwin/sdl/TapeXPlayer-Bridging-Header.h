//
// TapeXPlayer-Bridging-Header.h
// Bridge between C and Swift
//
// IMPORTANT: Swift can only import pure C code, not C++
// Make sure all headers use extern "C" guards

#ifndef TapeXPlayer_Bridging_Header_h
#define TapeXPlayer_Bridging_Header_h

// Import FSTPSettings C API
#include "FSTPSettings.h"

// Import PortAudio for audio device enumeration
#include <portaudio.h>

// Forward declarations for MIDI functions (avoiding C++ in FSTPPlayerManager.h)
#ifdef __cplusplus
extern "C" {
#endif

int GetMIDIInputDeviceCount(void);
int GetMIDIOutputDeviceCount(void);
const char* GetMIDIInputDeviceName(int index);
const char* GetMIDIOutputDeviceName(int index);
void ApplyMIDISettings(void);
void SetBetacamEffectEnabled(int enabled);
const char* GetExtensionLanguage(void);
void InitToolsMenu(void);
int FSTP_YTDLP_IsAvailable(void);
int FSTP_YTDLP_Download(const char* url, char* out_path, int out_path_size, char* error_buf, int error_buf_size);
const char* FSTP_YTDLP_GetDownloadsDir(void);
void SetYTDLPExtensionEnabled(int enabled);

// Inspector API - Player Instance Management
int GetActiveInstanceCount(void);
void GetActiveInstanceIDs(int* instance_ids, int* count);
int IsPlayerInstanceActive(int instance_id);
int IsVideoLoadedInInstance(int instance_id);

// Presentation mode bridge (Settings UI)
int FSTP_GetPresentationDisplayCount(void);
const char* FSTP_GetPresentationDisplayName(int idx);
void FSTP_ReapplyPresentationIfActive(void);

// Inspector API - File Properties
const char* GetInstanceFilePath(int instance_id);
const char* GetInstanceFileName(int instance_id);
double GetInstanceDuration(int instance_id);
double GetInstanceVideoFPS(int instance_id);
int GetInstanceVideoWidth(int instance_id);
int GetInstanceVideoHeight(int instance_id);
int GetInstanceTotalFrames(int instance_id);
double GetInstancePosition(int instance_id);
int GetInstanceAudioSampleRate(int instance_id);
int GetInstanceAudioChannels(int instance_id);
const char* GetInstanceAudioCodecName(int instance_id);
const char* GetInstanceVideoCodecName(int instance_id);

// Memory Locations API
int FSTP_GetMemoryLocationsCount(void);
_Bool FSTP_AddMemoryLocationWithTimecode(int player_id, int id, const char* name, const char* comments, double timecode_seconds);
_Bool FSTP_AddMemoryLocationWithZoom(int player_id, int id, const char* name, const char* comments,
                                      double timecode_seconds, _Bool recall_zoom,
                                      float zoom_factor, float zoom_center_x, float zoom_center_y);
_Bool FSTP_UpdateMemoryLocationFull(int id, const char* name, const char* comments,
                                     double timecode_seconds, _Bool recall_zoom,
                                     float zoom_factor, float zoom_center_x, float zoom_center_y);

typedef struct {
    int id;
    char name[256];
    double timecode_seconds;
    char timecode_display[32];
    char comments[512];
    int zoom_level;
    _Bool recall_zoom;
    _Bool is_active;
} FSTP_MemoryLocationData;

_Bool FSTP_GetMemoryLocationData(int index, FSTP_MemoryLocationData* out_data);
void OnMemoryLocationDialogClosedCallback(void);
void ShowSwiftUIMemoryLocationDialogEdit(int player_id, double current_time, int location_id);

// Memory Locations window (SwiftUI) — actions used by the list view
int GetActivePlayerID(void);
_Bool FSTP_RecallMemoryLocation(int id, int player_id);
_Bool FSTP_DeleteMemoryLocation(int id);
_Bool FSTP_ExportMemoryLocationsToCSV(const char* filepath);
_Bool FSTP_LoadMemoryLocations(const char* filepath);
void CreateMemoryLocationAtCurrentTime(void);

// Keyboard bindings (Settings ▸ Keyboard)
#define FSTP_MOD_SHIFT 1
#define FSTP_MOD_CTRL  2
#define FSTP_MOD_ALT   4
int  FSTP_KB_GetActionCount(void);
int  FSTP_KB_GetActionIdByIndex(int index);
const char* FSTP_KB_GetActionName(int action_id);
const char* FSTP_KB_GetActionGroup(int action_id);
int  FSTP_KB_IsActionEditable(int action_id);
int  FSTP_KB_GetKeycode(int action_id);
int  FSTP_KB_GetMods(int action_id);
int  FSTP_KB_GetDefaultKeycode(int action_id);
int  FSTP_KB_GetDefaultMods(int action_id);
const char* FSTP_KB_GetKeyName(int keycode);
int  FSTP_KB_SetBinding(int action_id, int keycode, int mods, int* out_conflict_action);
void FSTP_KB_ResetToDefaults(void);
int  FSTP_KB_MacKeyToSDL(int mac_keycode, int unicode_char);
void FSTP_KB_Save(void);
void FSTP_KB_Load(void);

// Zoom API
typedef struct {
    _Bool enabled;
    float factor;
    float center_x;
    float center_y;
    _Bool show_thumbnail;
} FSTPZoomState;

FSTPZoomState* GetZoomState(int window_index);

// Cache Management API
const char* FSTP_GetProxyCachePath(void);
const char* FSTP_GetMemoryLocationsCachePath(void);
int FSTP_GetProxyCacheSize(void);
int FSTP_GetProxyFilesCount(void);
_Bool FSTP_ClearProxyCache(_Bool keep_active);
_Bool FSTP_IsAnyPlayerActive(void);

// Memory Locations Data Management
int FSTP_GetMemoryLocationsFilesCount(void);
const char* FSTP_GetMemoryLocationsFileName(int index);
_Bool FSTP_ClearAllMemoryLocations(void);

#ifdef __cplusplus
}
#endif

#endif /* TapeXPlayer_Bridging_Header_h */
