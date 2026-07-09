#ifndef FSTP_MEMORY_LOCATIONS_H
#define FSTP_MEMORY_LOCATIONS_H

#include <string>
#include <vector>
#include <map>
#include <cstdint>

// Cross-platform C++ backend for Memory Locations

namespace FSTP {

// Structure for one Memory Location
struct MemoryLocation {
    int id;                          // Unique ID (1-999)
    std::string name;                // Location name
    double timecode_seconds;         // Position in seconds
    std::string timecode_display;    // Display timecode (HH:MM:SS:FF)
    std::string comments;            // Comments
    bool is_active;                  // Whether location is active

    // Additional parameters
    int zoom_level;                  // Zoom level (optional) - deprecated, kept for compatibility
    bool recall_zoom;                // Remember zoom when transitioning

    // Zoom parameters (new version)
    float zoom_factor;               // Zoom factor (1.0 = no zoom, up to 8.0)
    float zoom_center_x;             // Zoom center X (0.0-1.0)
    float zoom_center_y;             // Zoom center Y (0.0-1.0)

    MemoryLocation()
        : id(0), timecode_seconds(0.0), is_active(true),
          zoom_level(100), recall_zoom(false),
          zoom_factor(1.0f), zoom_center_x(0.5f), zoom_center_y(0.5f) {}
};

// Memory Locations Manager (cross-platform C++ backend)
class MemoryLocationsManager {
public:
    static MemoryLocationsManager& GetInstance();

    // Main operations
    bool AddLocation(const MemoryLocation& location);
    bool UpdateLocation(int id, const MemoryLocation& location);
    bool DeleteLocation(int id);
    bool ClearAll();

    // Data retrieval
    MemoryLocation* GetLocation(int id);
    const std::vector<MemoryLocation>& GetAllLocations() const { return locations(); }
    int GetCount() const { return static_cast<int>(locations().size()); }

    // Multi-instance: select which player's marker set is active.
    // All operations (Add/Recall/Get/Delete/Save/Load…) act on the active set,
    // so markers follow the focused instance. Called by the C API before each op.
    void SetActivePlayer(int player_id);
    int GetActivePlayer() const { return active_player_; }

    // Navigation
    bool RecallLocation(int id, int player_id);  // Go to location
    MemoryLocation* GetNextLocation(double current_time);
    MemoryLocation* GetPreviousLocation(double current_time);

    // Import/Export
    bool SaveToFile(const std::string& filepath);
    bool LoadFromFile(const std::string& filepath);
    bool ExportToCSV(const std::string& filepath);  // Export to CSV for scientific analysis

    // Utilities
    static std::string SecondsToTimecode(double seconds, double fps = 25.0);
    static double TimecodeToSeconds(const std::string& timecode, double fps = 25.0);

private:
    MemoryLocationsManager() = default;
    ~MemoryLocationsManager() = default;
    MemoryLocationsManager(const MemoryLocationsManager&) = delete;
    MemoryLocationsManager& operator=(const MemoryLocationsManager&) = delete;

    // Per-player marker sets, keyed by player_id. Markers are isolated per
    // instance and follow the focused player (see SetActivePlayer).
    std::map<int, std::vector<MemoryLocation>> player_locations_;
    std::map<int, int> player_next_id_;
    int active_player_ = 0;

    // Accessors to the active player's set (used in place of the old locations_/next_id_).
    std::vector<MemoryLocation>& locations();
    const std::vector<MemoryLocation>& locations() const;
    int& next_id();

    int FindLocationIndex(int id) const;
};

} // namespace FSTP

// C API for integration with Swift/Objective-C and other code
#ifdef __cplusplus
extern "C" {
#endif

// System initialization
void FSTP_InitMemoryLocations();
void FSTP_ShutdownMemoryLocations();

// Add location at current player position
bool FSTP_AddMemoryLocationAtCurrentTime(int player_id, const char* name, const char* comments);

// Add location with specified ID
bool FSTP_AddMemoryLocationWithID(int player_id, int id, const char* name, const char* comments);

// Add location with specified ID and timecode
bool FSTP_AddMemoryLocationWithTimecode(int player_id, int id, const char* name, const char* comments, double timecode_seconds);

// Add location with specified ID, timecode and zoom parameters
bool FSTP_AddMemoryLocationWithZoom(int player_id, int id, const char* name, const char* comments,
                                     double timecode_seconds, bool recall_zoom,
                                     float zoom_factor, float zoom_center_x, float zoom_center_y);

// Go to location
bool FSTP_RecallMemoryLocation(int id, int player_id);

// Get number of locations
int FSTP_GetMemoryLocationsCount();

// Get location data (for passing to UI)
typedef struct {
    int id;
    char name[256];
    double timecode_seconds;
    char timecode_display[32];
    char comments[512];
    int zoom_level;
    bool recall_zoom;
    bool is_active;
} FSTP_MemoryLocationData;

bool FSTP_GetMemoryLocationData(int index, FSTP_MemoryLocationData* out_data);

// Location management
bool FSTP_DeleteMemoryLocation(int id);
bool FSTP_UpdateMemoryLocation(int id, const char* name, const char* comments);
bool FSTP_UpdateMemoryLocationFull(int id, const char* name, const char* comments,
                                    double timecode_seconds, bool recall_zoom,
                                    float zoom_factor, float zoom_center_x, float zoom_center_y);

// Import/Export
bool FSTP_SaveMemoryLocations(const char* filepath);
bool FSTP_LoadMemoryLocations(const char* filepath);
bool FSTP_ExportMemoryLocationsToCSV(const char* filepath);  // Export to CSV for scientific analysis

// Show UI window (platform-dependent implementation in Swift/Cocoa)
void FSTP_ShowMemoryLocationsWindow();
void FSTP_HideMemoryLocationsWindow();

// Cache Management API
const char* FSTP_GetProxyCachePath();           // Path to proxy files folder
const char* FSTP_GetMemoryLocationsCachePath(); // Path to memory locations folder
int FSTP_GetProxyCacheSize();                   // Proxy cache size in MB
int FSTP_GetProxyFilesCount();                  // Number of proxy files
bool FSTP_ClearProxyCache(bool keep_active);     // Clear proxy cache (keep_active=true keeps open video files)
bool FSTP_IsAnyPlayerActive();                   // Check if any files are loaded

// Memory Locations Data Management
int FSTP_GetMemoryLocationsFilesCount();         // Number of video files with Memory Locations
const char* FSTP_GetMemoryLocationsFileName(int index); // Video filename by index
bool FSTP_ClearAllMemoryLocations();              // Clear all Memory Locations

#ifdef __cplusplus
}
#endif

#endif // FSTP_MEMORY_LOCATIONS_H
