#include "FSTPRemote.h"
#include "FSTPPlayerManager.h"
#include <iostream>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

// Forward declarations from FSTPWindowManager
extern "C" int GetActivePlayerID();

// Speed control constants
const double MIN_SPEED = 0.01;   // Minimum speed (1%)
const double MAX_SPEED = 24.0;   // Maximum speed (2400%)
const double SPEED_EPSILON = 0.0001; // Epsilon for speed comparison
const double DEFAULT_SPEED = 4.0;   // 400% default speed
const double SPEED_STEP = 0.75;   // 75% speed change per step

// MIDI constants
const unsigned char MIDI_CC = 0xB0;
const unsigned char MIDI_NOTE_ON = 0x90;
const unsigned char MIDI_NOTE_OFF = 0x80;

// Transport Controls (X-Touch One MC mode)
const unsigned char PLAY_NOTE = 0x5E;  // Note number for Play button (94)
const unsigned char STOP_NOTE = 0x5D;  // Note number for Stop button (93)
const unsigned char REW_NOTE = 0x5B;   // Note number for Rewind button (91)
const unsigned char FF_NOTE = 0x5C;    // Note number for Fast Forward button (92)
const unsigned char JOG_CC = 0x3C;     // CC number for jog wheel (60)

// LED Control
const unsigned char LED_PLAY = 0x5E;
const unsigned char LED_STOP = 0x5D;
const unsigned char LED_REW = 0x5B;
const unsigned char LED_FF = 0x5C;

// Display Control (7-segment)
const unsigned char DISPLAY_SPEED_HUNDREDS = 0x4B;
const unsigned char DISPLAY_SPEED_TENS = 0x4A;
const unsigned char DISPLAY_HOURS_HUNDREDS = 0x49;
const unsigned char DISPLAY_HOURS_TENS = 0x48;
const unsigned char DISPLAY_HOURS_ONES = 0x47;
const unsigned char DISPLAY_MINS_TENS = 0x46;
const unsigned char DISPLAY_MINS_ONES = 0x45;
const unsigned char DISPLAY_SECS_TENS = 0x44;
const unsigned char DISPLAY_SECS_ONES = 0x43;
const unsigned char DISPLAY_FRAMES_TENS = 0x42;
const unsigned char DISPLAY_FRAMES_ONES = 0x41;

FSTPRemote::FSTPRemote()
    : m_initialized(false),
      m_shared_cmd(nullptr),
      m_quit(false),
      m_thread_running(false),
      m_hui_initialized(false),
      m_last_led_toggle(std::chrono::steady_clock::now()),
      m_led_blink_state(false),
      m_button_pressed(false),
      m_is_playing(false) {
#ifdef _WIN32
    m_mapping_handle = NULL;
#else
    m_shm_fd = -1;
#endif
}

FSTPRemote::~FSTPRemote() {
    Shutdown();
}

bool FSTPRemote::Initialize() {
    if (m_initialized) return true;

    try {
        if (!CreateSharedMemory()) {
            std::cerr << "[FSTPRemote] Failed to create shared memory" << std::endl;
            return false;
        }

        if (m_shared_cmd) {
            m_shared_cmd->command_type = RemoteCommand::Type::NONE;
            m_shared_cmd->seek_time = 0.0;
            m_shared_cmd->status = 2;
            strncpy(m_shared_cmd->timecode, "00:00:00:00", 11);
            m_shared_cmd->timecode[11] = '\0';

            // Set initial speed
            m_shared_cmd->current_rate = static_cast<float>(DEFAULT_SPEED);

#ifndef _WIN32
            msync(m_shared_cmd, sizeof(RemoteCommand), MS_SYNC);
#endif
        }

        InitializeHUI();

        m_initialized = true;
        StartProcessingThread();
        std::cout << "[FSTPRemote] Initialized successfully" << std::endl;
        return true;
    }
    catch (const std::exception& e) {
        std::cerr << "[FSTPRemote] Error during initialization: " << e.what() << std::endl;
        return false;
    }
}

void FSTPRemote::Shutdown() {
    if (!m_initialized) return;

    StopProcessingThread();
    CleanupHUI();
    CleanupSharedMemory();

    m_initialized = false;
    std::cout << "[FSTPRemote] Shutdown complete" << std::endl;
}

// === Shared Memory ===

bool FSTPRemote::CreateSharedMemory() {
    std::cout << "[FSTPRemote] Creating shared memory..." << std::endl;

#ifdef _WIN32
    m_mapping_handle = CreateFileMapping(
        INVALID_HANDLE_VALUE,
        NULL,
        PAGE_READWRITE,
        0,
        sizeof(RemoteCommand),
        "Local\\TapeXPlayerControl"
    );

    if (m_mapping_handle == NULL) {
        std::cerr << "[FSTPRemote] Failed to create file mapping" << std::endl;
        return false;
    }

    m_shared_cmd = static_cast<RemoteCommand*>(
        MapViewOfFile(m_mapping_handle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(RemoteCommand))
    );

    if (m_shared_cmd == nullptr) {
        std::cerr << "[FSTPRemote] Failed to map view of file" << std::endl;
        CloseHandle(m_mapping_handle);
        return false;
    }
#else
    const char* SHM_NAME = "/tmp/tapexplayer_control";

    // Remove old shared memory file
    unlink(SHM_NAME);

    // Create new shared memory file
    m_shm_fd = open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (m_shm_fd == -1) {
        std::cerr << "[FSTPRemote] Failed to open shared memory: " << strerror(errno) << std::endl;
        return false;
    }

    // Set size
    if (ftruncate(m_shm_fd, sizeof(RemoteCommand)) == -1) {
        std::cerr << "[FSTPRemote] Failed to set shared memory size: " << strerror(errno) << std::endl;
        close(m_shm_fd);
        unlink(SHM_NAME);
        return false;
    }

    // Map into memory
    m_shared_cmd = static_cast<RemoteCommand*>(
        mmap(nullptr, sizeof(RemoteCommand), PROT_READ | PROT_WRITE, MAP_SHARED, m_shm_fd, 0)
    );

    if (m_shared_cmd == MAP_FAILED) {
        std::cerr << "[FSTPRemote] Failed to map shared memory: " << strerror(errno) << std::endl;
        close(m_shm_fd);
        unlink(SHM_NAME);
        return false;
    }

    // Initialize
    std::memset(m_shared_cmd, 0, sizeof(RemoteCommand));
    m_shared_cmd->command_type = RemoteCommand::Type::NONE;
    m_shared_cmd->seek_time = 0.0;
    m_shared_cmd->speed_value = 0.0;
    m_shared_cmd->status = 2;
    strncpy(m_shared_cmd->timecode, "00:00:00:00", 11);
    m_shared_cmd->timecode[11] = '\0';
    m_shared_cmd->flags.is_playing = 0;
    m_shared_cmd->flags.is_reverse = 0;
    m_shared_cmd->current_rate = 1.0f;

    msync(m_shared_cmd, sizeof(RemoteCommand), MS_SYNC);
#endif

    std::cout << "[FSTPRemote] Shared memory initialized" << std::endl;
    return true;
}

void FSTPRemote::CleanupSharedMemory() {
    if (m_shared_cmd) {
        m_shared_cmd->command_type = RemoteCommand::Type::NONE;
        m_shared_cmd->status = 2;

#ifndef _WIN32
        msync(m_shared_cmd, sizeof(RemoteCommand), MS_SYNC);
#endif
    }

#ifdef _WIN32
    if (m_shared_cmd) {
        UnmapViewOfFile(m_shared_cmd);
        m_shared_cmd = nullptr;
    }
    if (m_mapping_handle) {
        CloseHandle(m_mapping_handle);
        m_mapping_handle = NULL;
    }
#else
    if (m_shared_cmd != MAP_FAILED && m_shared_cmd != nullptr) {
        munmap(m_shared_cmd, sizeof(RemoteCommand));
        m_shared_cmd = nullptr;
    }
    if (m_shm_fd >= 0) {
        close(m_shm_fd);
        if (m_quit) {
            unlink("/tmp/tapexplayer_control");
        }
        m_shm_fd = -1;
    }
#endif
}

// === Thread Management ===

void FSTPRemote::StartProcessingThread() {
    if (m_thread_running) return;

    m_thread_running = true;
    m_processing_thread = std::thread(&FSTPRemote::CommandProcessingThread, this);

#if defined(_WIN32) && !defined(__MINGW32__)
    SetThreadPriority(m_processing_thread.native_handle(), THREAD_PRIORITY_HIGHEST);
#else
    struct sched_param param;
    param.sched_priority = sched_get_priority_max(SCHED_FIFO);
    pthread_setschedparam(m_processing_thread.native_handle(), SCHED_FIFO, &param);
#endif

    std::cout << "[FSTPRemote] Processing thread started" << std::endl;
}

void FSTPRemote::StopProcessingThread() {
    if (!m_thread_running) return;

    {
        std::lock_guard<std::mutex> lock(m_command_mutex);
        m_thread_running = false;
        std::queue<CommandQueueItem>().swap(m_command_queue);
    }
    m_command_cv.notify_one();

    if (m_processing_thread.joinable()) {
        m_processing_thread.join();
    }

    std::cout << "[FSTPRemote] Processing thread stopped" << std::endl;
}

void FSTPRemote::CommandProcessingThread() {
    while (m_thread_running) {
        ProcessCommands();
        UpdateTimecode();
        std::this_thread::sleep_for(std::chrono::milliseconds(30)); // ~30 fps
    }
}

// === Command Processing ===

void FSTPRemote::ProcessCommands() {
    if (!m_initialized || !m_shared_cmd) {
        return;
    }

    RemoteCommand cmd;
    {
#ifndef _WIN32
        msync(m_shared_cmd, sizeof(RemoteCommand), MS_SYNC | MS_INVALIDATE);
#endif
        std::memcpy(&cmd, m_shared_cmd, sizeof(RemoteCommand));
    }

    if (cmd.status != 0) {
        return;
    }

    RemoteCommand::Type original_type = cmd.command_type;

    try {
        m_shared_cmd->status = 1;
#ifndef _WIN32
        msync(m_shared_cmd, sizeof(RemoteCommand), MS_SYNC);
#endif

        switch (original_type) {
            case RemoteCommand::Type::SEEK:
                HandleSeek(cmd.seek_time);
                break;
            case RemoteCommand::Type::PLAY:
                HandlePlay();
                break;
            case RemoteCommand::Type::STOP:
                HandleStop();
                break;
            case RemoteCommand::Type::SET_SPEED:
                HandleSetSpeed(cmd.speed_value);
                break;
            case RemoteCommand::Type::ADJUST_SPEED:
                HandleAdjustSpeed(cmd.speed_value);
                break;
            case RemoteCommand::Type::SEEK_TIMECODE:
                {
                    std::string tc_str(cmd.seek_timecode, static_cast<size_t>(8));
                    int hours = std::stoi(tc_str.substr(0,2));
                    int minutes = std::stoi(tc_str.substr(2,2));
                    int seconds = std::stoi(tc_str.substr(4,2));
                    int frames = std::stoi(tc_str.substr(6,2));

                    int active_player = GetActivePlayerID();
                    double fps = GetInstanceVideoFPS(active_player);
                    if (fps <= 0) fps = 30.0;

                    double target_time = hours * 3600.0 +
                                       minutes * 60.0 +
                                       seconds +
                                       frames / fps;

                    HandleSeek(target_time);
                }
                break;
            case RemoteCommand::Type::NONE:
                break;
        }

        UpdateTimecode();

        m_shared_cmd->command_type = RemoteCommand::Type::NONE;
        m_shared_cmd->status = 2;
#ifndef _WIN32
        msync(m_shared_cmd, sizeof(RemoteCommand), MS_SYNC);
#endif
    }
    catch (const std::exception& e) {
        std::cerr << "[FSTPRemote] Error executing command: " << e.what() << std::endl;
        m_shared_cmd->command_type = RemoteCommand::Type::NONE;
        m_shared_cmd->status = 2;
#ifndef _WIN32
        msync(m_shared_cmd, sizeof(RemoteCommand), MS_SYNC);
#endif
    }
}

// === Command Handlers ===

void FSTPRemote::HandleSeek(double time) {
    int active_player = GetActivePlayerID();
    if (active_player >= 0) {
        SeekInstance(active_player, time);
        std::cout << "[FSTPRemote] Seek to " << time << "s on player " << active_player << std::endl;
    }
}

void FSTPRemote::HandlePlay() {
    int active_player = GetActivePlayerID();

    if (active_player >= 0) {
        double current_speed = GetInstanceSpeed(active_player);

        if (std::abs(current_speed) > 1.1) {
            // Reset to normal speed
            SetInstanceSpeed(active_player, 1.0);
        } else {
            // Toggle play/pause
            bool is_playing = IsInstancePlaying(active_player);
            if (is_playing) {
                PauseInstance(active_player);
                m_is_playing = false;
            } else {
                PlayInstance(active_player);
                m_is_playing = true;
            }
        }
    }
}

void FSTPRemote::HandleStop() {
    int active_player = GetActivePlayerID();
    if (active_player >= 0) {
        PauseInstance(active_player);
        m_is_playing = false;
        std::cout << "[FSTPRemote] Stop player " << active_player << std::endl;
    }
}

void FSTPRemote::HandleSetSpeed(double speed) {
    speed = std::clamp(speed, MIN_SPEED, MAX_SPEED);
    int active_player = GetActivePlayerID();
    if (active_player >= 0) {
        SetInstanceSpeed(active_player, speed);
        std::cout << "[FSTPRemote] Set speed to " << speed << "x on player " << active_player << std::endl;
    }
}

void FSTPRemote::HandleAdjustSpeed(double delta) {
    int active_player = GetActivePlayerID();
    if (active_player < 0) return;

    double current_speed = GetInstanceSpeed(active_player);
    bool is_reverse = IsInstanceReverse(active_player);
    double new_speed = current_speed + delta;

    // Check for zero crossing
    if ((current_speed > 0 && new_speed < 0) || (current_speed < 0 && new_speed > 0)) {
        SetInstanceReverse(active_player, !is_reverse);
        new_speed = std::abs(new_speed);
    }

    new_speed = std::clamp(new_speed, MIN_SPEED, MAX_SPEED);

    if (std::abs(new_speed - current_speed) > SPEED_EPSILON) {
        SetInstanceSpeed(active_player, new_speed);
    }
}

// === Timecode Management ===

void FSTPRemote::UpdateTimecode() {
    if (!m_initialized || !m_shared_cmd) return;

    try {
        std::string current_tc = GetCurrentTimecode();
        int active_player = GetActivePlayerID();

        // OPTIMIZATION: Cache previous values, update only on change
        static bool last_is_playing = false;
        static bool last_is_reverse = false;
        static float last_rate = 0.0f;
        bool status_changed = false;

        if (active_player >= 0) {
            bool is_playing = IsInstancePlaying(active_player);
            bool is_reverse = IsInstanceReverse(active_player);
            float current_rate = static_cast<float>(GetInstanceActualSpeed(active_player));

            // Update shared memory only if status changed
            if (is_playing != last_is_playing ||
                is_reverse != last_is_reverse ||
                std::abs(current_rate - last_rate) > 0.01f) {

                m_shared_cmd->flags.is_playing = is_playing ? 1 : 0;
                m_shared_cmd->flags.is_reverse = is_reverse ? 1 : 0;
                m_shared_cmd->current_rate = current_rate;

                last_is_playing = is_playing;
                last_is_reverse = is_reverse;
                last_rate = current_rate;
                status_changed = true;
            }
        }

        bool timecode_changed = false;
        if (strncmp(m_shared_cmd->timecode, current_tc.c_str(), 11) != 0) {
            strncpy(m_shared_cmd->timecode, current_tc.c_str(), 11);
            m_shared_cmd->timecode[11] = '\0';
            timecode_changed = true;

            if (m_hui_initialized) {
                UpdateHUITimecode(current_tc);
            }
        }

        // msync only if something changed
        if (timecode_changed || status_changed) {
#ifndef _WIN32
            msync(m_shared_cmd, sizeof(RemoteCommand), MS_ASYNC);
#endif
        }

        if (m_hui_initialized) {
            UpdateLEDStatus();
        }
    }
    catch (const std::exception& e) {
        std::cerr << "[FSTPRemote] Error updating timecode: " << e.what() << std::endl;
    }
}

std::string FSTPRemote::GetCurrentTimecode() {
    int active_player = GetActivePlayerID();
    if (active_player < 0) {
        return "00:00:00:00";
    }

    double position = GetInstancePosition(active_player);
    double fps = GetInstanceVideoFPS(active_player);
    if (fps <= 0) fps = 30.0;

    // SYNCHRONIZATION WITH OSD: Use same formula as in FSTPOSDSystem.cpp
    // This guarantees that timecode on controller matches OSD
    int hours = static_cast<int>(position / 3600);
    int minutes = static_cast<int>((position - hours * 3600) / 60);
    int seconds = static_cast<int>(position - hours * 3600 - minutes * 60);
    int frames = static_cast<int>((position - static_cast<int>(position)) * fps);

    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(2) << hours << ":"
        << std::setw(2) << minutes << ":"
        << std::setw(2) << seconds << ":"
        << std::setw(2) << frames;

    return oss.str();
}

// === HUI / MIDI Functions ===

void FSTPRemote::InitializeHUI() {
    try {
        std::cout << "[FSTPRemote] Initializing MIDI interfaces..." << std::endl;

        try {
            m_midi_in = std::make_unique<RtMidiIn>();
            std::cout << "[FSTPRemote] MIDI Input interface created" << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "[FSTPRemote] Failed to create MIDI Input: " << e.what() << std::endl;
            return;
        }

        try {
            m_midi_out = std::make_unique<RtMidiOut>();
            std::cout << "[FSTPRemote] MIDI Output interface created" << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "[FSTPRemote] Failed to create MIDI Output: " << e.what() << std::endl;
            m_midi_in.reset();
            return;
        }

        bool found_in = false;
        bool found_out = false;

        std::cout << "[FSTPRemote] Available MIDI Input ports:" << std::endl;
        for (unsigned int i = 0; i < m_midi_in->getPortCount(); i++) {
            std::string port_name = m_midi_in->getPortName(i);
            std::cout << "  " << i << ": " << port_name << std::endl;

            if (port_name.find("HUI") != std::string::npos ||
                port_name.find("X-Touch") != std::string::npos ||
                port_name.find("X-TOUCH") != std::string::npos) {
                m_midi_in->openPort(i);
                found_in = true;
                m_current_input_device = port_name;
                std::cout << "[FSTPRemote] Connected to MIDI input: " << port_name << std::endl;
                break;
            }
        }

        std::cout << "[FSTPRemote] Available MIDI Output ports:" << std::endl;
        for (unsigned int i = 0; i < m_midi_out->getPortCount(); i++) {
            std::string port_name = m_midi_out->getPortName(i);
            std::cout << "  " << i << ": " << port_name << std::endl;

            if (port_name.find("HUI") != std::string::npos ||
                port_name.find("X-Touch") != std::string::npos ||
                port_name.find("X-TOUCH") != std::string::npos) {
                m_midi_out->openPort(i);
                found_out = true;
                m_current_output_device = port_name;
                std::cout << "[FSTPRemote] Connected to MIDI output: " << port_name << std::endl;
                break;
            }
        }

        if (!found_in || !found_out) {
            std::cout << "[FSTPRemote] HUI/X-Touch not found, creating virtual ports" << std::endl;
            if (!found_in) {
                m_midi_in->openVirtualPort("TapeXPlayer HUI In");
                m_current_input_device = "TapeXPlayer HUI In (Virtual)";
            }
            if (!found_out) {
                m_midi_out->openVirtualPort("TapeXPlayer HUI Out");
                m_current_output_device = "TapeXPlayer HUI Out (Virtual)";
            }
        }

        InitializeDisplay();

        m_midi_in->setCallback(&FSTPRemote::HUICallback, this);
        m_midi_in->ignoreTypes(false, false, false);

        m_hui_initialized = true;
        std::cout << "[FSTPRemote] HUI interface initialized" << std::endl;
    }
    catch (RtMidiError &error) {
        std::cerr << "[FSTPRemote] Error initializing HUI: " << error.getMessage() << std::endl;
        m_hui_initialized = false;
    }
}

void FSTPRemote::CleanupHUI() {
    if (m_hui_initialized) {
        CleanupDisplay();
        if (m_midi_in) {
            m_midi_in->closePort();
            m_midi_in.reset();
        }
        if (m_midi_out) {
            m_midi_out->closePort();
            m_midi_out.reset();
        }
        m_hui_initialized = false;
        std::cout << "[FSTPRemote] HUI interface cleaned up" << std::endl;
    }
}

void FSTPRemote::UpdateHUITimecode(const std::string& timecode) {
    if (!m_hui_initialized || !m_midi_out || timecode == m_last_timecode) {
        return;
    }

    try {
        int hours = std::stoi(timecode.substr(0, 2));
        int minutes = std::stoi(timecode.substr(3, 2));
        int seconds = std::stoi(timecode.substr(6, 2));
        int frames = std::stoi(timecode.substr(9, 2));

        DisplayTimecode(hours, minutes, seconds, frames);
        m_last_timecode = timecode;
    }
    catch (std::exception &e) {
        std::cerr << "[FSTPRemote] Error parsing timecode: " << e.what() << std::endl;
    }
}

void FSTPRemote::HandleHUIMessage(double deltatime, std::vector<unsigned char>* message) {
    if (!message || message->empty()) {
        return;
    }

    unsigned char status = message->at(0) & 0xF0;

    if (status == MIDI_NOTE_ON && message->size() >= 3) {
        unsigned char note = message->at(1);
        unsigned char velocity = message->at(2);

        if (note == PLAY_NOTE) {
            if (velocity == 0x7F && !m_button_pressed) {
                m_button_pressed = true;
                int active_player = GetActivePlayerID();
                if (active_player >= 0) {
                    double current_speed = GetInstanceSpeed(active_player);
                    if (std::abs(current_speed) > 1.1) {
                        SetInstanceSpeed(active_player, 1.0);
                    } else if (!m_is_playing) {
                        PlayInstance(active_player);
                        m_is_playing = true;
                    }
                    SetInstanceReverse(active_player, false);
                }

                if (m_midi_out) {
                    std::vector<unsigned char> led_msg = {MIDI_NOTE_ON, LED_PLAY, 0x7F};
                    m_midi_out->sendMessage(&led_msg);
                    led_msg = {MIDI_NOTE_ON, LED_STOP, 0x00};
                    m_midi_out->sendMessage(&led_msg);
                }
            }
            else if (velocity == 0x00) {
                m_button_pressed = false;
            }
        }
        else if (note == STOP_NOTE) {
            if (velocity == 0x7F && !m_button_pressed) {
                m_button_pressed = true;
                int active_player = GetActivePlayerID();
                if (active_player >= 0) {
                    PauseInstance(active_player);
                    m_is_playing = false;
                }

                if (m_midi_out) {
                    std::vector<unsigned char> led_msg = {MIDI_NOTE_ON, LED_STOP, 0x7F};
                    m_midi_out->sendMessage(&led_msg);
                    led_msg = {MIDI_NOTE_ON, LED_PLAY, 0x00};
                    m_midi_out->sendMessage(&led_msg);
                }
            }
            else if (velocity == 0x00) {
                m_button_pressed = false;
            }
        }
    }
    else if (status == MIDI_CC && message->size() >= 3) {
        unsigned char controller = message->at(1);
        unsigned char value = message->at(2);

        if (controller == JOG_CC) {
            double speed_delta = 0.0;
            if (value == 0x01) {
                speed_delta = SPEED_STEP;
            }
            else if (value == 0x41) {
                speed_delta = -SPEED_STEP;
            }

            if (speed_delta != 0.0) {
                int active_player = GetActivePlayerID();
                if (active_player >= 0 && IsInstanceReverse(active_player)) {
                    speed_delta = -speed_delta;
                }
                HandleAdjustSpeed(speed_delta);
            }
        }
    }
}

void FSTPRemote::HUICallback(double deltatime, std::vector<unsigned char>* message, void* userData) {
    FSTPRemote* self = static_cast<FSTPRemote*>(userData);
    if (self) {
        self->HandleHUIMessage(deltatime, message);
    }
}

// === Display Functions ===

void FSTPRemote::DisplayTimecode(int hours, int minutes, int seconds, int frames) {
    if (!m_midi_out) return;

    std::vector<std::pair<unsigned char, int>> digits = {
        {DISPLAY_HOURS_TENS, (hours % 100) / 10},
        {DISPLAY_HOURS_ONES, hours % 10},
        {DISPLAY_MINS_TENS, minutes / 10},
        {DISPLAY_MINS_ONES, minutes % 10},
        {DISPLAY_SECS_TENS, seconds / 10},
        {DISPLAY_SECS_ONES, seconds % 10},
        {DISPLAY_FRAMES_TENS, frames / 10},
        {DISPLAY_FRAMES_ONES, frames % 10}
    };

    int active_player = GetActivePlayerID();
    int speed_display = 1;
    if (active_player >= 0) {
        double current_speed = GetInstanceActualSpeed(active_player);
        speed_display = static_cast<int>(std::abs(current_speed));
        speed_display = std::clamp(speed_display, 1, 99);
    }

    digits.insert(digits.begin(), {
        {DISPLAY_SPEED_HUNDREDS, speed_display / 10},
        {DISPLAY_SPEED_TENS, speed_display % 10}
    });

    std::vector<unsigned char> clear_msg = {0xB0, DISPLAY_HOURS_HUNDREDS, 0x00};
    m_midi_out->sendMessage(&clear_msg);

    for (const auto& [position, digit] : digits) {
        std::vector<unsigned char> message = {
            0xB0,
            position,
            static_cast<unsigned char>(0x30 + digit)
        };
        m_midi_out->sendMessage(&message);
    }
}

void FSTPRemote::InitializeDisplay() {
    if (!m_midi_out) return;

    for (unsigned char pos = 0x41; pos <= 0x4B; pos++) {
        std::vector<unsigned char> clear_msg = {0xB0, pos, 0x00};
        m_midi_out->sendMessage(&clear_msg);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void FSTPRemote::CleanupDisplay() {
    if (!m_midi_out) return;

    for (unsigned char pos = 0x41; pos <= 0x4B; pos++) {
        std::vector<unsigned char> clear_msg = {0xB0, pos, 0x00};
        m_midi_out->sendMessage(&clear_msg);
    }
}

// === LED Functions ===

void FSTPRemote::UpdateLEDStatus() {
    if (!m_midi_out || !m_hui_initialized) return;

    int active_player = GetActivePlayerID();
    if (active_player < 0) return;

    double current_speed = GetInstanceActualSpeed(active_player);
    double abs_speed = std::abs(current_speed);
    bool is_reverse = IsInstanceReverse(active_player);

    auto now = std::chrono::steady_clock::now();
    auto time_since_toggle = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_last_led_toggle).count();

    static double last_speed_state = -999.0;
    bool speed_changed = (std::abs(current_speed - last_speed_state) > 0.1);
    bool should_blink = (abs_speed > 1.1);
    bool blink_changed = false;

    if (should_blink && time_since_toggle >= BLINK_INTERVAL_MS) {
        m_led_blink_state = !m_led_blink_state;
        m_last_led_toggle = now;
        blink_changed = true;
    }

    if (!speed_changed && !blink_changed) {
        return;
    }

    last_speed_state = current_speed;

    unsigned char play_value = 0x00;
    unsigned char ff_value = 0x00;
    unsigned char rew_value = 0x00;
    unsigned char stop_value = 0x00;

    if (current_speed > 0) {
        if (!is_reverse) {
            if (abs_speed > 1.1) {
                play_value = m_led_blink_state ? 0x7F : 0x00;
                ff_value = 0x7F;
            } else {
                play_value = 0x7F;
            }
        } else {
            if (abs_speed > 1.1) {
                play_value = m_led_blink_state ? 0x7F : 0x00;
                rew_value = 0x7F;
            } else {
                play_value = 0x7F;
            }
        }
    } else {
        stop_value = 0x7F;
    }

    try {
        if (should_blink || speed_changed) {
            std::vector<unsigned char> play_msg = {MIDI_NOTE_ON, LED_PLAY, play_value};
            m_midi_out->sendMessage(&play_msg);
        }

        if (speed_changed) {
            std::vector<unsigned char> ff_msg = {MIDI_NOTE_ON, LED_FF, ff_value};
            m_midi_out->sendMessage(&ff_msg);

            std::vector<unsigned char> rew_msg = {MIDI_NOTE_ON, LED_REW, rew_value};
            m_midi_out->sendMessage(&rew_msg);

            std::vector<unsigned char> stop_msg = {MIDI_NOTE_ON, LED_STOP, stop_value};
            m_midi_out->sendMessage(&stop_msg);
        }
    } catch (const std::exception& e) {
        std::cerr << "[FSTPRemote] Error sending LED MIDI: " << e.what() << std::endl;
    }
}

// === Device Management ===

std::vector<std::string> FSTPRemote::GetInputDevices() const {
    std::vector<std::string> devices;
    if (!m_midi_in) return devices;

    for (unsigned int i = 0; i < m_midi_in->getPortCount(); i++) {
        devices.push_back(m_midi_in->getPortName(i));
    }
    return devices;
}

std::vector<std::string> FSTPRemote::GetOutputDevices() const {
    std::vector<std::string> devices;
    if (!m_midi_out) return devices;

    for (unsigned int i = 0; i < m_midi_out->getPortCount(); i++) {
        devices.push_back(m_midi_out->getPortName(i));
    }
    return devices;
}

bool FSTPRemote::SelectDevice(const std::string& device_name, bool is_input) {
    try {
        if (is_input && m_midi_in) {
            m_midi_in->closePort();

            for (unsigned int i = 0; i < m_midi_in->getPortCount(); i++) {
                if (m_midi_in->getPortName(i) == device_name) {
                    m_midi_in->openPort(i);
                    m_midi_in->setCallback(&FSTPRemote::HUICallback, this);
                    m_midi_in->ignoreTypes(false, false, false);
                    m_current_input_device = device_name;
                    std::cout << "[FSTPRemote] Selected input: " << device_name << std::endl;
                    return true;
                }
            }
        }
        else if (!is_input && m_midi_out) {
            m_midi_out->closePort();

            for (unsigned int i = 0; i < m_midi_out->getPortCount(); i++) {
                if (m_midi_out->getPortName(i) == device_name) {
                    m_midi_out->openPort(i);
                    m_current_output_device = device_name;
                    InitializeDisplay();
                    std::cout << "[FSTPRemote] Selected output: " << device_name << std::endl;
                    return true;
                }
            }
        }
    }
    catch (RtMidiError &error) {
        std::cerr << "[FSTPRemote] Error selecting device: " << error.getMessage() << std::endl;
    }
    return false;
}

std::string FSTPRemote::GetCurrentInputDevice() const {
    return m_current_input_device;
}

std::string FSTPRemote::GetCurrentOutputDevice() const {
    return m_current_output_device;
}

