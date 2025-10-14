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

// Inspector API - Player Instance Management
int GetActiveInstanceCount(void);
void GetActiveInstanceIDs(int* instance_ids, int* count);
int IsPlayerInstanceActive(int instance_id);
int IsVideoLoadedInInstance(int instance_id);

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
void OnMemoryLocationDialogClosedCallback(void);

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
