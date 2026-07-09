#ifndef FSTP_REMOTE_H
#define FSTP_REMOTE_H

#include <string>
#include <atomic>
#include <memory>
#include <chrono>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>

// RtMidi 4.0 MIDI library
#ifdef __APPLE__
#define __MACOSX_CORE__
#endif

#include <rtmidi/RtMidi.h>

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

    // Combine flags with current speed to save space
    union {
        struct {
            uint8_t is_playing : 1;    // Playback flag
            uint8_t is_reverse : 1;    // Reverse direction flag
            uint8_t reserved : 6;      // Reserved for future flags
            uint8_t padding[3];        // Alignment padding
        } flags;
        float current_rate;            // 4 bytes (overlaps with flags)
    };
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

class FSTPRemote {
public:
    FSTPRemote();
    ~FSTPRemote();

    bool Initialize();
    void Shutdown();
    void ProcessCommands();
    bool IsInitialized() const { return m_initialized; }

    // MIDI device management
    std::vector<std::string> GetInputDevices() const;
    std::vector<std::string> GetOutputDevices() const;
    bool SelectDevice(const std::string& device_name, bool is_input);
    std::string GetCurrentInputDevice() const;
    std::string GetCurrentOutputDevice() const;

private:
    // Shared memory for FSFrameDebugger
    bool CreateSharedMemory();
    void CleanupSharedMemory();

    // Command handlers - called from PlayerManager
    void HandleSeek(double time);
    void HandlePlay();
    void HandleStop();
    void HandleSetSpeed(double speed);
    void HandleAdjustSpeed(double delta);

    // Thread management
    void CommandProcessingThread();
    void StartProcessingThread();
    void StopProcessingThread();
    void EnqueueCommand(RemoteCommand::Type type, double value);

    // Timecode management
    void UpdateTimecode();
    std::string GetCurrentTimecode();

    // Mackie HUI / X-Touch One support
    void InitializeHUI();
    void CleanupHUI();
    void UpdateHUITimecode(const std::string& timecode);
    void HandleHUIMessage(double deltatime, std::vector<unsigned char>* message);
    static void HUICallback(double deltatime, std::vector<unsigned char>* message, void* userData);

    // Display management
    void DisplayTimecode(int hours, int minutes, int seconds, int frames);
    void InitializeDisplay();
    void CleanupDisplay();

    // LED management for speed indication
    void UpdateLEDStatus();

    bool m_initialized;
    RemoteCommand* m_shared_cmd;

#ifdef _WIN32
    HANDLE m_mapping_handle;
#else
    int m_shm_fd;
#endif

    std::atomic<bool> m_quit;

    // Thread management members
    std::thread m_processing_thread;
    std::mutex m_command_mutex;
    std::condition_variable m_command_cv;
    std::queue<CommandQueueItem> m_command_queue;
    std::atomic<bool> m_thread_running;

    // Mackie HUI MIDI members
    std::unique_ptr<RtMidiIn> m_midi_in;
    std::unique_ptr<RtMidiOut> m_midi_out;
    bool m_hui_initialized;
    std::string m_last_timecode;

    std::string m_current_input_device;
    std::string m_current_output_device;

    // LED blinking state for high speed indication
    std::chrono::steady_clock::time_point m_last_led_toggle;
    bool m_led_blink_state;
    static constexpr int BLINK_INTERVAL_MS = 100; // 100ms on/off - fast flashing for high speed

    // Button state tracking
    bool m_button_pressed;
    bool m_is_playing;
};

#endif // FSTP_REMOTE_H
