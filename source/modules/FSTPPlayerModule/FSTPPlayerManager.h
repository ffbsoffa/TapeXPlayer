#ifndef FSTP_PLAYER_MANAGER_H
#define FSTP_PLAYER_MANAGER_H

#ifdef __cplusplus
extern "C" {
#endif

// Maximum number of player instances
// Limited to 3 for full operation of full-res decoder
#define MAX_PLAYER_INSTANCES 3

// Player manager initialization
// Returns 0 on success, negative value on error
int InitPlayerManager();

// Player manager shutdown
void ShutdownPlayerManager();

// Create new player instance with file
// Returns instance ID (0-4) or negative value on error
int CreatePlayerInstance(const char* filepath, int player_id = -1);

// Load file into existing player instance
// Returns instance ID (0-4) or negative value on error
int LoadFileIntoPlayerInstance(const char* filepath, int player_id);

// Destroy player instance by ID
void DestroyPlayerInstance(int instance_id);

// Check if player instance is active
int IsPlayerInstanceActive(int instance_id);

// Check if file is loaded in instance
int IsVideoLoadedInInstance(int instance_id);

// Get instance file path (or NULL if not loaded)
const char* GetInstanceFilePath(int instance_id);

// Get number of active instances
int GetActiveInstanceCount();

// Get list of active instance IDs (array must be size MAX_PLAYER_INSTANCES)
void GetActiveInstanceIDs(int* instance_ids, int* count);

// Find instance by file path
int FindInstanceByFilePath(const char* filepath);

// === Playback Control for Specific Instance ===
int PlayInstance(int instance_id);
int PauseInstance(int instance_id);
int StopInstance(int instance_id);
// Resource courtesy: free (backgrounded=1) / restore (0) an unfocused instance's full-res decoder.
void SetInstanceBackgrounded(int instance_id, int backgrounded);
int SeekInstance(int instance_id, double position);

// === Speed and Direction Control ===
int SetInstanceSpeed(int instance_id, double speed);
int SetInstanceSpeedInstant(int instance_id, double speed);    // For Mouse Shuttle - without animation
int SetInstanceReverse(int instance_id, bool reverse);         // With direction-change sequencer
int SetInstanceReverseInstant(int instance_id, bool reverse);  // For Mouse Shuttle - instant

// === Instance State Retrieval ===
double GetInstancePosition(int instance_id);
double GetInstanceDuration(int instance_id);
double GetInstanceTimecodeOffset(int instance_id);
int IsInstancePlaying(int instance_id);
double GetInstanceSpeed(int instance_id);           // Target speed
double GetInstanceActualSpeed(int instance_id);     // Actual animated speed
int IsInstanceReverse(int instance_id);

// === Audio Signal Level Retrieval ===
float GetInstanceAudioLevelLeft(int instance_id);
float GetInstanceAudioLevelRight(int instance_id);
float GetInstanceAudioPeakLeft(int instance_id);
float GetInstanceAudioPeakRight(int instance_id);

bool GetInstanceFrameAligned(int instance_id);

// Get audio module and video FPS for frame number calculation
class FSTPAudioModuleWrapper* GetInstanceAudioModule(int instance_id);
double GetInstanceVideoFPS(int instance_id);

// === Active Player Management ===
// Note: GetActivePlayerID() declared in FSTPWindowManager.h
void SetActivePlayerID(int player_id);

// === Audio Settings Application ===
// Restarts audio streams of all active players with new settings
void RestartAllAudioStreams();

// === Video Update (60 FPS) ===
// Updates video frames of all active players (slave mode)
void UpdateAllVideoFrames();

// Updates video frame for specific player (to eliminate serialization bottleneck)
void UpdateVideoFrameForPlayer(int instance_id);

// === MIDI Remote Control API ===
// Get list of MIDI devices (returns device count)
int GetMIDIInputDeviceCount();
int GetMIDIOutputDeviceCount();
const char* GetMIDIInputDeviceName(int index);  // Returns device name by index
const char* GetMIDIOutputDeviceName(int index);
void ApplyMIDISettings();  // Apply MIDI settings from FSTPSettings

// === Inspector API - File Properties ===
// Get video properties for inspector
int GetInstanceVideoWidth(int instance_id);      // Video width in pixels
int GetInstanceVideoHeight(int instance_id);     // Video height in pixels
int GetInstanceTotalFrames(int instance_id);     // Total number of frames
const char* GetInstanceFileName(int instance_id); // Only filename without path

// Get audio properties for inspector
int GetInstanceAudioSampleRate(int instance_id);  // Sample rate (Hz)
int GetInstanceAudioChannels(int instance_id);    // Number of channels
const char* GetInstanceAudioCodecName(int instance_id); // Audio codec name
const char* GetInstanceVideoCodecName(int instance_id); // Video codec name

#ifdef __cplusplus
}
#endif

#endif // FSTP_PLAYER_MANAGER_H