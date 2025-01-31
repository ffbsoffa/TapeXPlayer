#pragma once

#include <string>
#include <atomic>
#include <memory>
#include <chrono>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include "RtMidi.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#endif

// Forward declarations
void seek_to_time(double time);
void toggle_pause();
std::atomic<double>& get_playback_rate();
std::atomic<double>& get_target_playback_rate();
std::string get_current_timecode();
double parse_timecode(const std::string& timecode);

// Ensure consistent memory layout
#pragma pack(push, 1)
struct RemoteCommand {
    enum Type : int32_t {
        NONE = 0,
        SEEK = 1,
        PLAY = 2,
        STOP = 3,
        SET_SPEED = 4,
        ADJUST_SPEED = 5,
        SEEK_TIMECODE = 6
    };
    
    Type command_type;      // 4 bytes
    union {
        double seek_time;   // 8 bytes
        double speed_value; // 8 bytes
        char seek_timecode[8]; // 8 bytes
    };
    int32_t status;        // 4 bytes
    char timecode[12];     // 12 bytes
    char padding[4];       // 4 bytes for alignment
};
#pragma pack(pop)

// Ensure the size matches between Python and C++
static_assert(sizeof(RemoteCommand) == 32, "RemoteCommand size must be 32 bytes");

// Structure for internal command queue
struct CommandQueueItem {
    RemoteCommand::Type type;
    double value;
    
    CommandQueueItem(RemoteCommand::Type t, double v) : type(t), value(v) {}
};

// External variables
extern std::atomic<bool> quit;
extern std::atomic<double> original_fps;

class RemoteControl {
public:
    RemoteControl();
    ~RemoteControl();

    bool initialize();
    void process_commands();
    bool is_initialized() const { return initialized; }

private:
    bool create_shared_memory();
    void cleanup_shared_memory();
    
    // Command handlers
    void handle_seek(double time);
    void handle_play();
    void handle_stop();
    void handle_set_speed(double speed);
    void handle_adjust_speed(double delta);
    
    // Thread management
    void command_processing_thread();
    void start_processing_thread();
    void stop_processing_thread();
    void enqueue_command(RemoteCommand::Type type, double value);
    
    // Timecode management
    void update_timecode();
    
    // Mackie HUI support
    void initialize_hui();
    void cleanup_hui();
    void update_hui_timecode(const std::string& timecode);
    void handle_hui_message(double deltatime, std::vector<unsigned char>* message);
    static void hui_callback(double deltatime, std::vector<unsigned char>* message, void* userData);
    
    // Display management
    void displayTimecode(int hours, int minutes, int seconds, int frames);
    void initialize_display();
    void cleanup_display();
    
    bool initialized;
    RemoteCommand* shared_cmd;
    
#ifdef _WIN32
    HANDLE mapping_handle;
#else
    int shm_fd;
#endif

    std::atomic<bool>& quit;
    
    // Thread management members
    std::thread processing_thread;
    std::mutex command_mutex;
    std::condition_variable command_cv;
    std::queue<CommandQueueItem> command_queue;
    std::atomic<bool> thread_running;

    // Mackie HUI MIDI members
    std::unique_ptr<RtMidiIn> midi_in;
    std::unique_ptr<RtMidiOut> midi_out;
    bool hui_initialized;
    std::string last_timecode;
}; 