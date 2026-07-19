#include "FSTPMemoryLocations.h"
#include "../FSTPPlayerModule/FSTPPlayerManager.h"
#include "FSTPWindowManager.h"
#include "FSTPZoom.h"
#include "../FSTPVideoModule/FSTPLowResDecoder.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <filesystem>
#include <set>

namespace fs = std::filesystem;

namespace FSTP {

// Singleton instance
MemoryLocationsManager& MemoryLocationsManager::GetInstance() {
    static MemoryLocationsManager instance;
    return instance;
}

// ── Per-player active-set accessors ──────────────────────────────────────────
// The manager keeps one marker set per player_id; these return the set for the
// currently active player so all existing logic stays unchanged.
std::vector<MemoryLocation>& MemoryLocationsManager::locations() {
    return player_locations_[active_player_];  // default-constructs an empty set if absent
}

const std::vector<MemoryLocation>& MemoryLocationsManager::locations() const {
    static const std::vector<MemoryLocation> empty;
    auto it = player_locations_.find(active_player_);
    return it != player_locations_.end() ? it->second : empty;
}

int& MemoryLocationsManager::next_id() {
    auto it = player_next_id_.find(active_player_);
    if (it == player_next_id_.end()) {
        it = player_next_id_.emplace(active_player_, 1).first;
    }
    return it->second;
}

// Switch which player's marker set subsequent operations act on.
void MemoryLocationsManager::SetActivePlayer(int player_id) {
    if (player_id < 0) return;  // keep current set for invalid id
    active_player_ = player_id;
}

// Add new location
bool MemoryLocationsManager::AddLocation(const MemoryLocation& location) {
    MemoryLocation new_loc = location;

    // Automatically assign ID if not specified
    if (new_loc.id == 0) {
        new_loc.id = next_id()++;
    } else {
        // Check ID uniqueness
        if (FindLocationIndex(new_loc.id) >= 0) {
            std::cerr << "Memory Location ID " << new_loc.id << " already exists" << std::endl;
            return false;
        }
        // Update next id if needed
        if (new_loc.id >= next_id()) {
            next_id() = new_loc.id + 1;
        }
    }

    auto& locs = locations();
    locs.push_back(new_loc);

    // Sort by time
    std::sort(locs.begin(), locs.end(),
        [](const MemoryLocation& a, const MemoryLocation& b) {
            return a.timecode_seconds < b.timecode_seconds;
        });

    std::cout << "📍 Added Memory Location #" << new_loc.id
              << " at " << new_loc.timecode_display << std::endl;
    return true;
}

// Update existing location
bool MemoryLocationsManager::UpdateLocation(int id, const MemoryLocation& location) {
    int index = FindLocationIndex(id);
    if (index < 0) {
        std::cerr << "Memory Location ID " << id << " not found" << std::endl;
        return false;
    }

    auto& locs = locations();
    locs[index] = location;
    locs[index].id = id; // Preserve original ID

    // Re-sort if time changed
    std::sort(locs.begin(), locs.end(),
        [](const MemoryLocation& a, const MemoryLocation& b) {
            return a.timecode_seconds < b.timecode_seconds;
        });

    std::cout << "📍 Updated Memory Location #" << id << std::endl;
    return true;
}

// Delete location
bool MemoryLocationsManager::DeleteLocation(int id) {
    int index = FindLocationIndex(id);
    if (index < 0) {
        return false;
    }

    locations().erase(locations().begin() + index);
    std::cout << "📍 Deleted Memory Location #" << id << std::endl;
    return true;
}

// Clear all locations
bool MemoryLocationsManager::ClearAll() {
    locations().clear();
    next_id() = 1;
    std::cout << "📍 Cleared all Memory Locations" << std::endl;
    return true;
}

// Get location by ID
MemoryLocation* MemoryLocationsManager::GetLocation(int id) {
    int index = FindLocationIndex(id);
    if (index < 0) {
        return nullptr;
    }
    return &locations()[index];
}

// Go to location
bool MemoryLocationsManager::RecallLocation(int id, int player_id) {
    MemoryLocation* loc = GetLocation(id);
    if (!loc || !loc->is_active) {
        return false;
    }

    std::cout << "📍 Recalling Memory Location #" << id
              << " \"" << loc->name << "\" at " << loc->timecode_display;

    // Restore zoom if required
    int window_index = player_id;  // Hard binding of window to player
    if (loc->recall_zoom && loc->zoom_factor > 1.0f) {
        std::cout << " with zoom " << loc->zoom_factor << "x";

        // Get current zoom state
        FSTPZoomState* zoom_state = GetZoomState(window_index);
        if (zoom_state) {
            // Set zoom parameters
            zoom_state->enabled = true;
            zoom_state->factor = loc->zoom_factor;
            zoom_state->center_x = loc->zoom_center_x;
            zoom_state->center_y = loc->zoom_center_y;
            zoom_state->show_thumbnail = true;
        }
    } else {
        // Reset zoom if not saved
        ResetZoom(window_index);
    }
    std::cout << std::endl;

    // Use existing API for seeking
    SeekInstance(player_id, loc->timecode_seconds);

    // Force update OSD after seeking
    // (important for cases when player is paused or frozen)
    double current_time = GetInstancePosition(player_id);
    double total_duration = GetInstanceDuration(player_id);
    bool is_playing = IsInstancePlaying(player_id);
    double speed = GetInstanceSpeed(player_id);
    bool is_reverse = IsInstanceReverse(player_id);

    UpdateWindowOSD(window_index, current_time, total_duration, is_playing, speed, is_reverse);

    return true;
}

// Next location
MemoryLocation* MemoryLocationsManager::GetNextLocation(double current_time) {
    for (auto& loc : locations()) {
        if (loc.is_active && loc.timecode_seconds > current_time) {
            return &loc;
        }
    }
    return nullptr;
}

// Previous location
MemoryLocation* MemoryLocationsManager::GetPreviousLocation(double current_time) {
    auto& locs = locations();
    for (auto it = locs.rbegin(); it != locs.rend(); ++it) {
        if (it->is_active && it->timecode_seconds < current_time) {
            return &(*it);
        }
    }
    return nullptr;
}

// Save to file
bool MemoryLocationsManager::SaveToFile(const std::string& filepath) {
    std::ofstream file{fs::path(filepath)};
    if (!file.is_open()) {
        std::cerr << "Failed to save Memory Locations to " << filepath << std::endl;
        return false;
    }

    file << "# TapeXPlayer Memory Locations\n";
    file << "# Format: ID|Name|Timecode|Comments|ZoomLevel|RecallZoom|Active\n\n";

    for (const auto& loc : locations()) {
        file << loc.id << "|"
             << loc.name << "|"
             << loc.timecode_display << "|"
             << loc.comments << "|"
             << loc.zoom_level << "|"
             << (loc.recall_zoom ? "1" : "0") << "|"
             << (loc.is_active ? "1" : "0") << "\n";
    }

    file.close();
    std::cout << "📍 Saved " << locations().size() << " Memory Locations to " << filepath << std::endl;
    return true;
}

// Load from file
bool MemoryLocationsManager::LoadFromFile(const std::string& filepath) {
    std::ifstream file{fs::path(filepath)};
    if (!file.is_open()) {
        std::cerr << "Failed to load Memory Locations from " << filepath << std::endl;
        return false;
    }

    ClearAll();

    std::string line;
    int loaded_count = 0;

    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;

        std::stringstream ss(line);
        MemoryLocation loc;
        std::string token;

        // Parse: ID|Name|Timecode|Comments|ZoomLevel|RecallZoom|Active
        if (std::getline(ss, token, '|')) loc.id = std::stoi(token);
        std::getline(ss, loc.name, '|');
        if (std::getline(ss, loc.timecode_display, '|')) {
            loc.timecode_seconds = TimecodeToSeconds(loc.timecode_display);
        }
        std::getline(ss, loc.comments, '|');
        if (std::getline(ss, token, '|')) loc.zoom_level = std::stoi(token);
        if (std::getline(ss, token, '|')) loc.recall_zoom = (token == "1");
        if (std::getline(ss, token, '|')) loc.is_active = (token == "1");

        if (AddLocation(loc)) {
            loaded_count++;
        }
    }

    file.close();
    std::cout << "📍 Loaded " << loaded_count << " Memory Locations from " << filepath << std::endl;
    return loaded_count > 0;
}

// Export to CSV for scientific analysis
bool MemoryLocationsManager::ExportToCSV(const std::string& filepath) {
    std::ofstream file{fs::path(filepath)};
    if (!file.is_open()) {
        std::cerr << "Failed to export Memory Locations to CSV: " << filepath << std::endl;
        return false;
    }

    // CSV Header
    file << "ID,Name,Timecode,Seconds,Comments,Active\n";

    // Export each location
    for (const auto& loc : locations()) {
        // Escape quotes in name and comments for CSV
        std::string name = loc.name;
        std::string comments = loc.comments;

        // Replace quotes with double quotes (CSV standard)
        size_t pos = 0;
        while ((pos = name.find('"', pos)) != std::string::npos) {
            name.replace(pos, 1, "\"\"");
            pos += 2;
        }

        pos = 0;
        while ((pos = comments.find('"', pos)) != std::string::npos) {
            comments.replace(pos, 1, "\"\"");
            pos += 2;
        }

        // Write CSV line
        file << loc.id << ","
             << "\"" << name << "\","
             << "\"" << loc.timecode_display << "\","
             << std::fixed << std::setprecision(3) << loc.timecode_seconds << ","
             << "\"" << comments << "\","
             << (loc.is_active ? "Yes" : "No") << "\n";
    }

    file.close();
    std::cout << "📊 Exported " << locations().size() << " Memory Locations to CSV: " << filepath << std::endl;
    return true;
}

// Convert seconds to timecode
std::string MemoryLocationsManager::SecondsToTimecode(double seconds, double fps) {
    int hours = static_cast<int>(seconds / 3600);
    int minutes = static_cast<int>((seconds - hours * 3600) / 60);
    int secs = static_cast<int>(seconds - hours * 3600 - minutes * 60);
    int frames = static_cast<int>(round((seconds - static_cast<int>(seconds)) * fps)) % static_cast<int>(fps);

    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d:%02d", hours, minutes, secs, frames);
    return std::string(buffer);
}

// Convert timecode to seconds
double MemoryLocationsManager::TimecodeToSeconds(const std::string& timecode, double fps) {
    int hours = 0, minutes = 0, seconds = 0, frames = 0;
    sscanf(timecode.c_str(), "%d:%d:%d:%d", &hours, &minutes, &seconds, &frames);

    return hours * 3600.0 + minutes * 60.0 + seconds + frames / fps;
}

// Find location index by ID
int MemoryLocationsManager::FindLocationIndex(int id) const {
    const auto& locs = locations();
    for (size_t i = 0; i < locs.size(); ++i) {
        if (locs[i].id == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

} // namespace FSTP

// ============================================================================
// C API Implementation
// ============================================================================

void FSTP_InitMemoryLocations() {
    std::cout << "📍 Initializing Memory Locations system..." << std::endl;
    // Singleton already created on first access
}

void FSTP_ShutdownMemoryLocations() {
    std::cout << "📍 Shutting down Memory Locations system..." << std::endl;
    FSTP::MemoryLocationsManager::GetInstance().ClearAll();
}

bool FSTP_AddMemoryLocationAtCurrentTime(int player_id, const char* name, const char* comments) {
    FSTP::MemoryLocationsManager::GetInstance().SetActivePlayer(player_id);
    double current_time = GetInstancePosition(player_id);
    if (current_time < 0) {
        return false;
    }

    // Get real FPS from player
    double fps = GetInstanceVideoFPS(player_id);
    if (fps <= 0) fps = 25.0;  // fallback

    FSTP::MemoryLocation loc;
    loc.name = name ? name : "";
    loc.comments = comments ? comments : "";
    loc.timecode_seconds = current_time;
    loc.timecode_display = FSTP::MemoryLocationsManager::SecondsToTimecode(current_time, fps);

    return FSTP::MemoryLocationsManager::GetInstance().AddLocation(loc);
}

bool FSTP_AddMemoryLocationWithID(int player_id, int id, const char* name, const char* comments) {
    FSTP::MemoryLocationsManager::GetInstance().SetActivePlayer(player_id);
    double current_time = GetInstancePosition(player_id);
    if (current_time < 0) {
        return false;
    }

    // Get real FPS from player
    double fps = GetInstanceVideoFPS(player_id);
    if (fps <= 0) fps = 25.0;  // fallback

    FSTP::MemoryLocation loc;
    loc.id = id;  // Set user-specified ID
    loc.name = name ? name : "";
    loc.comments = comments ? comments : "";
    loc.timecode_seconds = current_time;
    loc.timecode_display = FSTP::MemoryLocationsManager::SecondsToTimecode(current_time, fps);

    return FSTP::MemoryLocationsManager::GetInstance().AddLocation(loc);
}

bool FSTP_AddMemoryLocationWithTimecode(int player_id, int id, const char* name, const char* comments, double timecode_seconds) {
    FSTP::MemoryLocationsManager::GetInstance().SetActivePlayer(player_id);
    // Get real FPS from player
    double fps = GetInstanceVideoFPS(player_id);
    if (fps <= 0) fps = 25.0;  // fallback

    FSTP::MemoryLocation loc;
    loc.id = id;  // Set user-specified ID
    loc.name = name ? name : "";
    loc.comments = comments ? comments : "";
    loc.timecode_seconds = timecode_seconds;
    loc.timecode_display = FSTP::MemoryLocationsManager::SecondsToTimecode(timecode_seconds, fps);

    return FSTP::MemoryLocationsManager::GetInstance().AddLocation(loc);
}

bool FSTP_AddMemoryLocationWithZoom(int player_id, int id, const char* name, const char* comments,
                                     double timecode_seconds, bool recall_zoom,
                                     float zoom_factor, float zoom_center_x, float zoom_center_y) {
    FSTP::MemoryLocationsManager::GetInstance().SetActivePlayer(player_id);
    // Get real FPS from player
    double fps = GetInstanceVideoFPS(player_id);
    if (fps <= 0) fps = 25.0;  // fallback

    FSTP::MemoryLocation loc;
    loc.id = id;
    loc.name = name ? name : "";
    loc.comments = comments ? comments : "";
    loc.timecode_seconds = timecode_seconds;
    loc.timecode_display = FSTP::MemoryLocationsManager::SecondsToTimecode(timecode_seconds, fps);
    loc.recall_zoom = recall_zoom;
    loc.zoom_factor = zoom_factor;
    loc.zoom_center_x = zoom_center_x;
    loc.zoom_center_y = zoom_center_y;

    std::cout << "📍 Adding Memory Location #" << id << " \"" << loc.name << "\""
              << " at " << loc.timecode_display;
    if (recall_zoom) {
        std::cout << " with zoom " << zoom_factor << "x at (" << zoom_center_x << ", " << zoom_center_y << ")";
    }
    std::cout << std::endl;

    return FSTP::MemoryLocationsManager::GetInstance().AddLocation(loc);
}

bool FSTP_RecallMemoryLocation(int id, int player_id) {
    FSTP::MemoryLocationsManager::GetInstance().SetActivePlayer(player_id);
    return FSTP::MemoryLocationsManager::GetInstance().RecallLocation(id, player_id);
}

int FSTP_GetMemoryLocationsCount() {
    FSTP::MemoryLocationsManager::GetInstance().SetActivePlayer(GetActivePlayerID());
    return FSTP::MemoryLocationsManager::GetInstance().GetCount();
}

bool FSTP_GetMemoryLocationData(int index, FSTP_MemoryLocationData* out_data) {
    if (!out_data) return false;

    FSTP::MemoryLocationsManager::GetInstance().SetActivePlayer(GetActivePlayerID());
    const auto& locations = FSTP::MemoryLocationsManager::GetInstance().GetAllLocations();
    if (index < 0 || index >= static_cast<int>(locations.size())) {
        return false;
    }

    const auto& loc = locations[index];
    out_data->id = loc.id;
    out_data->timecode_seconds = loc.timecode_seconds;
    out_data->zoom_level = loc.zoom_level;
    out_data->recall_zoom = loc.recall_zoom;
    out_data->is_active = loc.is_active;

    strncpy(out_data->name, loc.name.c_str(), sizeof(out_data->name) - 1);
    strncpy(out_data->timecode_display, loc.timecode_display.c_str(), sizeof(out_data->timecode_display) - 1);
    strncpy(out_data->comments, loc.comments.c_str(), sizeof(out_data->comments) - 1);

    return true;
}

bool FSTP_DeleteMemoryLocation(int id) {
    FSTP::MemoryLocationsManager::GetInstance().SetActivePlayer(GetActivePlayerID());
    return FSTP::MemoryLocationsManager::GetInstance().DeleteLocation(id);
}

bool FSTP_UpdateMemoryLocation(int id, const char* name, const char* comments) {
    FSTP::MemoryLocationsManager::GetInstance().SetActivePlayer(GetActivePlayerID());
    auto* loc = FSTP::MemoryLocationsManager::GetInstance().GetLocation(id);
    if (!loc) return false;

    if (name) loc->name = name;
    if (comments) loc->comments = comments;

    return true;
}

bool FSTP_UpdateMemoryLocationFull(int id, const char* name, const char* comments,
                                    double timecode_seconds, bool recall_zoom,
                                    float zoom_factor, float zoom_center_x, float zoom_center_y) {
    FSTP::MemoryLocationsManager::GetInstance().SetActivePlayer(GetActivePlayerID());
    auto* loc = FSTP::MemoryLocationsManager::GetInstance().GetLocation(id);
    if (!loc) return false;

    if (name) loc->name = name;
    if (comments) loc->comments = comments;

    // Update timecode
    loc->timecode_seconds = timecode_seconds;
    loc->timecode_display = FSTP::MemoryLocationsManager::SecondsToTimecode(timecode_seconds, 25.0);

    // Update zoom parameters
    loc->recall_zoom = recall_zoom;
    loc->zoom_factor = zoom_factor;
    loc->zoom_center_x = zoom_center_x;
    loc->zoom_center_y = zoom_center_y;

    std::cout << "📝 Updated Memory Location #" << id << ": \"" << loc->name
              << "\" @ " << loc->timecode_display << std::endl;

    return true;
}

bool FSTP_SaveMemoryLocations(const char* filepath) {
    FSTP::MemoryLocationsManager::GetInstance().SetActivePlayer(GetActivePlayerID());
    return FSTP::MemoryLocationsManager::GetInstance().SaveToFile(filepath);
}

bool FSTP_LoadMemoryLocations(const char* filepath) {
    FSTP::MemoryLocationsManager::GetInstance().SetActivePlayer(GetActivePlayerID());
    return FSTP::MemoryLocationsManager::GetInstance().LoadFromFile(filepath);
}

bool FSTP_ExportMemoryLocationsToCSV(const char* filepath) {
    FSTP::MemoryLocationsManager::GetInstance().SetActivePlayer(GetActivePlayerID());
    return FSTP::MemoryLocationsManager::GetInstance().ExportToCSV(filepath);
}

// ── Platform-specific app data base path ─────────────────────────────────────
// Returns the root folder for TapeXPlayer user data:
//   Windows  → %APPDATA%\TapeXPlayer   (C:\Users\<user>\AppData\Roaming\TapeXPlayer)
//   macOS    → $HOME/.fstp
//   Linux    → $HOME/.fstp
static fs::path getFstpBasePath() {
#ifdef _WIN32
    const char* appData = std::getenv("APPDATA");
    if (appData) return fs::path(appData) / "TapeXPlayer";
    const char* localAppData = std::getenv("LOCALAPPDATA");
    if (localAppData) return fs::path(localAppData) / "TapeXPlayer";
    return fs::path("C:/TapeXPlayer");
#else
    const char* homeDir = std::getenv("HOME");
    return fs::path(homeDir ? homeDir : "/tmp") / ".fstp";
#endif
}

// Cache Management API
const char* FSTP_GetProxyCachePath() {
    static std::string proxy_path;
    proxy_path = FSTP::LowResDecoder::getCachePath() + "/proxy";
    return proxy_path.c_str();
}

const char* FSTP_GetMemoryLocationsCachePath() {
    static std::string locations_path;
    locations_path = getFstpBasePath().string();
    return locations_path.c_str();
}

int FSTP_GetProxyCacheSize() {
    try {
        fs::path proxy_dir(FSTP_GetProxyCachePath());
        if (!fs::exists(proxy_dir)) return 0;

        uintmax_t total_size = 0;
        for (const auto& entry : fs::recursive_directory_iterator(proxy_dir)) {
            if (fs::is_regular_file(entry)) {
                total_size += fs::file_size(entry);
            }
        }
        return static_cast<int>(total_size / (1024 * 1024)); // MB
    } catch (...) {
        return 0;
    }
}

int FSTP_GetProxyFilesCount() {
    try {
        fs::path proxy_dir(FSTP_GetProxyCachePath());
        if (!fs::exists(proxy_dir)) return 0;

        int count = 0;
        for (const auto& entry : fs::directory_iterator(proxy_dir)) {
            if (fs::is_regular_file(entry)) {
                count++;
            }
        }
        return count;
    } catch (...) {
        return 0;
    }
}

bool FSTP_ClearProxyCache(bool keep_active) {
    try {
        fs::path proxy_dir(FSTP_GetProxyCachePath());
        if (!fs::exists(proxy_dir)) return true;

        // Get list of active files if we need to keep them
        std::set<std::string> active_files;
        if (keep_active) {
            for (int i = 0; i < 3; i++) {  // MAX 3 players
                const char* loaded_file = GetInstanceFilePath(i);
                if (loaded_file && strlen(loaded_file) > 0) {
                    // Generate file ID and proxy path
                    std::string file_id = FSTP::LowResDecoder::generateFileId(loaded_file);
                    std::string proxy_path = (proxy_dir / (file_id + "_lowres.mp4")).string();
                    active_files.insert(proxy_path);
                    std::cout << "🔒 [CACHE] Keeping active proxy: " << proxy_path << std::endl;
                }
            }
        }

        int deleted = 0;
        for (const auto& entry : fs::directory_iterator(proxy_dir)) {
            if (fs::is_regular_file(entry)) {
                std::string file_path = entry.path().string();

                // Skip active files
                if (keep_active && active_files.count(file_path) > 0) {
                    continue;
                }

                fs::remove(entry);
                deleted++;
            }
        }

        std::cout << "🗑️  [CACHE] Cleared " << deleted << " proxy files";
        if (keep_active) {
            std::cout << " (kept " << active_files.size() << " active)";
        }
        std::cout << std::endl;

        return true;
    } catch (const std::exception& e) {
        std::cerr << "❌ [CACHE] Error clearing proxy cache: " << e.what() << std::endl;
        return false;
    }
}

bool FSTP_IsAnyPlayerActive() {
    for (int i = 0; i < 3; i++) {  // MAX 3 players
        const char* loaded_file = GetInstanceFilePath(i);
        if (loaded_file && strlen(loaded_file) > 0) {
            return true;
        }
    }
    return false;
}

// Memory Locations Data Management
static std::vector<std::string> g_memory_locations_files;

int FSTP_GetMemoryLocationsFilesCount() {
    g_memory_locations_files.clear();

    fs::path locations_dir = getFstpBasePath() / "memory_locations";

    if (!fs::exists(locations_dir) || !fs::is_directory(locations_dir)) {
        return 0;
    }

    // Scan all .txt files in directory
    try {
        for (const auto& entry : fs::directory_iterator(locations_dir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".txt") {
                // Read first line which contains original video file path
                std::ifstream file(entry.path());
                if (file.is_open()) {
                    std::string first_line;
                    if (std::getline(file, first_line)) {
                        // Remove "# VideoFile: " prefix
                        if (first_line.find("# VideoFile: ") == 0) {
                            std::string video_file = first_line.substr(13);
                            g_memory_locations_files.push_back(video_file);
                        }
                    }
                    file.close();
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "⚠️ Error scanning memory locations: " << e.what() << std::endl;
        return 0;
    }

    return static_cast<int>(g_memory_locations_files.size());
}

const char* FSTP_GetMemoryLocationsFileName(int index) {
    if (index < 0 || index >= static_cast<int>(g_memory_locations_files.size())) {
        return nullptr;
    }

    // Return just the filename, not full path
    fs::path full_path(g_memory_locations_files[index]);
    static std::string filename;
    filename = full_path.filename().string();
    return filename.c_str();
}

bool FSTP_ClearAllMemoryLocations() {
    fs::path locations_dir = getFstpBasePath() / "memory_locations";

    if (!fs::exists(locations_dir)) {
        return true; // Nothing to clear
    }

    try {
        int deleted_count = 0;
        for (const auto& entry : fs::directory_iterator(locations_dir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".txt") {
                fs::remove(entry.path());
                deleted_count++;
            }
        }
        std::cout << "🗑️ Cleared " << deleted_count << " Memory Locations files" << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "⚠️ Error clearing memory locations: " << e.what() << std::endl;
        return false;
    }
}
