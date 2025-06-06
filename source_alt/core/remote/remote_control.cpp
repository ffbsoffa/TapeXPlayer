#include "remote_control.h"
#include "common.h"  // Add explicit include of common.h
#include "../display/screenshot.h"  // Include screenshot functionality
#include <iostream>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

extern std::atomic<bool> quit;
extern std::atomic<bool> is_reverse;  // Add reference to is_reverse

// Speed control constants
const double MIN_SPEED = 0.01;   // Minimum speed (1%)
const double MAX_SPEED = 24.0;   // Maximum speed (1800%)
const double SPEED_EPSILON = 0.0001; // Epsilon for speed comparison
const double DEFAULT_SPEED = 4.0;   // 400% default speed
const double SPEED_STEP = 0.75;   // 50% speed change per step

// MIDI constants
const unsigned char MIDI_CC = 0xB0;
const unsigned char MIDI_NOTE_ON = 0x90;
const unsigned char MIDI_NOTE_OFF = 0x80;

// Transport Controls (X-Touch One MC mode)
const unsigned char PLAY_NOTE = 0x5E;  // Note number for Play button (94)
const unsigned char STOP_NOTE = 0x5D;  // Note number for Stop button (93)
const unsigned char JOG_CC = 0x3C;     // CC number for jog wheel (60)
const unsigned char ASSIGNMENT_CC = 0x0B;  // CC number for Assignment display

// LED Control (using Note On for LED feedback)
const unsigned char LED_PLAY = 0x5E;  // Same as PLAY_NOTE
const unsigned char LED_STOP = 0x5D;  // Same as STOP_NOTE

// Display Control (7-segment)
const unsigned char DISPLAY_TIME_1 = 0x20;    // First digit
const unsigned char DISPLAY_TIME_2 = 0x21;    // Second digit
const unsigned char DISPLAY_TIME_3 = 0x22;    // Third digit
const unsigned char DISPLAY_TIME_4 = 0x23;    // Fourth digit
const unsigned char DISPLAY_TIME_5 = 0x24;    // Fifth digit
const unsigned char DISPLAY_TIME_6 = 0x25;    // Sixth digit
const unsigned char DISPLAY_FRAMES_1 = 0x26;  // Frames first digit
const unsigned char DISPLAY_FRAMES_2 = 0x27;  // Frames second digit

// Display positions for timecode and speed
const unsigned char DISPLAY_SPEED_HUNDREDS = 0x4B;  // Leftmost position for speed
const unsigned char DISPLAY_SPEED_TENS = 0x4A;      // Second position for speed
const unsigned char DISPLAY_HOURS_HUNDREDS = 0x49;  // Start of timecode
const unsigned char DISPLAY_HOURS_TENS = 0x48;
const unsigned char DISPLAY_HOURS_ONES = 0x47;
const unsigned char DISPLAY_MINS_TENS = 0x46;
const unsigned char DISPLAY_MINS_ONES = 0x45;
const unsigned char DISPLAY_SECS_TENS = 0x44;
const unsigned char DISPLAY_SECS_ONES = 0x43;
const unsigned char DISPLAY_FRAMES_TENS = 0x42;
const unsigned char DISPLAY_FRAMES_ONES = 0x41;

// Add global state tracking
static bool is_playing = false;
static bool button_pressed = false;

RemoteControl::RemoteControl() 
    : initialized(false), 
      shared_cmd(nullptr), 
      quit(::quit),
      thread_running(false),
      hui_initialized(false) {
}

RemoteControl::~RemoteControl() {
    stop_processing_thread();
    cleanup_shared_memory();
    cleanup_hui();
}

bool RemoteControl::initialize() {
    if (initialized) return true;
    
    try {
        if (!create_shared_memory()) {
            std::cerr << "Failed to create shared memory" << std::endl;
            return false;
        }
        
        if (shared_cmd) {
            shared_cmd->command_type = RemoteCommand::Type::NONE;
            shared_cmd->seek_time = 0.0;
            shared_cmd->status = 2;
            strncpy(shared_cmd->timecode, "00:00:00:00", 11);
            shared_cmd->timecode[11] = '\0';
            
            // Set initial speed to 75%
            handle_set_speed(DEFAULT_SPEED);
            
#ifndef _WIN32
            msync(shared_cmd, sizeof(RemoteCommand), MS_SYNC);
#endif
        }
        
        initialize_hui();
        initialized = true;
        start_processing_thread();
        return true;
    }
    catch (const std::exception& e) {
        std::cerr << "Error during initialization: " << e.what() << std::endl;
        return false;
    }
}

void RemoteControl::initialize_hui() {
    try {
        midi_in = std::make_unique<RtMidiIn>();
        midi_out = std::make_unique<RtMidiOut>();

        // Look for HUI ports
        bool found_in = false;
        bool found_out = false;
        
        std::cout << "Available MIDI Input ports:" << std::endl;
        for (unsigned int i = 0; i < midi_in->getPortCount(); i++) {
            std::string port_name = midi_in->getPortName(i);
            std::cout << "  " << i << ": " << port_name << std::endl;
            
            // Check for both HUI and X-Touch One
            if (port_name.find("HUI") != std::string::npos || 
                port_name.find("X-Touch") != std::string::npos ||
                port_name.find("X-TOUCH") != std::string::npos) {
                midi_in->openPort(i);
                found_in = true;
                std::cout << "Connected to MIDI input port: " << port_name << std::endl;
                break;
            }
        }

        std::cout << "Available MIDI Output ports:" << std::endl;
        for (unsigned int i = 0; i < midi_out->getPortCount(); i++) {
            std::string port_name = midi_out->getPortName(i);
            std::cout << "  " << i << ": " << port_name << std::endl;
            
            // Check for both HUI and X-Touch One
            if (port_name.find("HUI") != std::string::npos || 
                port_name.find("X-Touch") != std::string::npos ||
                port_name.find("X-TOUCH") != std::string::npos) {
                midi_out->openPort(i);
                found_out = true;
                std::cout << "Connected to MIDI output port: " << port_name << std::endl;
                break;
            }
        }

        if (!found_in || !found_out) {
            std::cout << "X-Touch One or HUI ports not found, creating virtual ports" << std::endl;
            midi_in->openVirtualPort("TapeXPlayer HUI In");
            midi_out->openVirtualPort("TapeXPlayer HUI Out");
        }

        // Initialize display after MIDI ports are open
        initialize_display();

        // Set callback
        midi_in->setCallback(&RemoteControl::hui_callback, this);
        midi_in->ignoreTypes(false, false, false);

        hui_initialized = true;
        std::cout << "HUI interface initialized successfully" << std::endl;
    }
    catch (RtMidiError &error) {
        std::cerr << "Error initializing HUI: " << error.getMessage() << std::endl;
        hui_initialized = false;
    }
}

void RemoteControl::cleanup_hui() {
    if (hui_initialized) {
        cleanup_display();
        if (midi_in) {
            midi_in->closePort();
            midi_in.reset();
        }
        if (midi_out) {
            midi_out->closePort();
            midi_out.reset();
        }
        hui_initialized = false;
    }
}

void RemoteControl::update_hui_timecode(const std::string& timecode) {
    if (!hui_initialized || !midi_out || timecode == last_timecode) {
        return;
    }

    try {
        // Parse timecode (format: HH:MM:SS:FF)
        int hours = std::stoi(timecode.substr(0, 2));
        int minutes = std::stoi(timecode.substr(3, 2));
        int seconds = std::stoi(timecode.substr(6, 2));
        int frames = std::stoi(timecode.substr(9, 2));

        // Display timecode using the new method
        displayTimecode(hours, minutes, seconds, frames);
        
        last_timecode = timecode;
    }
    catch (std::exception &e) {
        std::cerr << "Error parsing timecode: " << e.what() << std::endl;
    }
}

void RemoteControl::handle_hui_message(double deltatime, std::vector<unsigned char>* message) {
    if (!message || message->empty()) return;

    unsigned char status = message->at(0) & 0xF0;
    unsigned char channel = message->at(0) & 0x0F;

    if (status == MIDI_NOTE_ON && message->size() >= 3) {
        unsigned char note = message->at(1);
        unsigned char velocity = message->at(2);

        if (note == PLAY_NOTE) {
            if (velocity == 0x7F && !button_pressed) {  // Button pressed
                button_pressed = true;
                
                // Check speed before playback
                double current_speed = get_playback_rate().load();
                if (std::abs(current_speed) > 1.1) {
                    // At high speed, first reset speed
                    reset_to_normal_speed();
                } else if (!is_playing) {
                    // Start playback if stopped
                    is_playing = true;
                    handle_play();
                }
                
                // Always reset to normal forward playback at 1x speed
                is_reverse.store(false);
                handle_set_speed(1.0);
                
                // Send LED feedback
                if (midi_out) {
                    std::vector<unsigned char> led_msg = {
                        MIDI_NOTE_ON,
                        LED_PLAY,
                        0x7F  // Full brightness
                    };
                    midi_out->sendMessage(&led_msg);
                    // Turn off Stop LED
                    led_msg = {
                        MIDI_NOTE_ON,
                        LED_STOP,
                        0x00  // Off
                    };
                    midi_out->sendMessage(&led_msg);
                }
            }
            else if (velocity == 0x00) {  // Button released
                button_pressed = false;
            }
        }
        else if (note == STOP_NOTE) {
            if (velocity == 0x7F && !button_pressed) {  // Button pressed
                button_pressed = true;
                
                // Always stop playback
                is_playing = false;
                handle_stop();
                
                // Send LED feedback
                if (midi_out) {
                    std::vector<unsigned char> led_msg = {
                        MIDI_NOTE_ON,
                        LED_STOP,
                        0x7F  // Full brightness
                    };
                    midi_out->sendMessage(&led_msg);
                    // Turn off Play LED
                    led_msg = {
                        MIDI_NOTE_ON,
                        LED_PLAY,
                        0x00  // Off
                    };
                    midi_out->sendMessage(&led_msg);
                }
            }
            else if (velocity == 0x00) {  // Button released
                button_pressed = false;
            }
        }
    }
    else if (status == MIDI_CC && message->size() >= 3) {
        unsigned char controller = message->at(1);
        unsigned char value = message->at(2);
        
        if (controller == JOG_CC) {
            double speed_delta = 0.0;
            if (value == 0x01) {  // Clockwise
                speed_delta = SPEED_STEP;
            }
            else if (value == 0x41) {  // Counter-clockwise
                speed_delta = -SPEED_STEP;
            }
            
            if (speed_delta != 0.0) {
                // Invert delta if in reverse mode
                if (is_reverse.load()) {
                    speed_delta = -speed_delta;
                }
                handle_adjust_speed(speed_delta);
            }
        }
        else if (controller == ASSIGNMENT_CC) {
            // Handle Assignment display update
            std::string value_str(reinterpret_cast<char*>(&value), sizeof(value));
            handle_set_speed(std::stod(value_str));
        }
    }
}

void RemoteControl::hui_callback(double deltatime, std::vector<unsigned char>* message, void* userData) {
    RemoteControl* self = static_cast<RemoteControl*>(userData);
    if (self) {
        self->handle_hui_message(deltatime, message);
    }
}

void RemoteControl::start_processing_thread() {
    if (thread_running) return;
    
    thread_running = true;
    processing_thread = std::thread(&RemoteControl::command_processing_thread, this);
    
#ifdef _WIN32
    SetThreadPriority(processing_thread.native_handle(), THREAD_PRIORITY_HIGHEST);
#else
    struct sched_param param;
    param.sched_priority = sched_get_priority_max(SCHED_FIFO);
    pthread_setschedparam(processing_thread.native_handle(), SCHED_FIFO, &param);
#endif
}

void RemoteControl::stop_processing_thread() {
    if (!thread_running) return;
    
    {
        std::lock_guard<std::mutex> lock(command_mutex);
        thread_running = false;
        // Clear command queue
        std::queue<CommandQueueItem>().swap(command_queue);
    }
    command_cv.notify_one();
    
    if (processing_thread.joinable()) {
        processing_thread.join();
    }
}

void RemoteControl::command_processing_thread() {
    while (thread_running) {
        process_commands();
        
        // Update timecode even without commands to keep FSFrameDebugger current
        update_timecode();
        
        // Optimize update interval to reduce CPU load
        std::this_thread::sleep_for(std::chrono::milliseconds(30)); // ~30 fps
    }
}

void RemoteControl::update_timecode() {
    if (!initialized || !shared_cmd) return;
    
    try {
        std::string current_tc = get_current_timecode();
        
        // Get current playback state
        bool is_playing_state = true;  // Should be logical value from player state
        bool is_reverse_state = is_reverse.load();
        float current_rate_value = static_cast<float>(get_playback_rate().load());
        double current_fps = original_fps.load();
        
        // Update all fields in shared memory
        shared_cmd->flags.is_playing = is_playing_state ? 1 : 0;
        shared_cmd->flags.is_reverse = is_reverse_state ? 1 : 0;
        shared_cmd->current_rate = current_rate_value;
        shared_cmd->total_duration = total_duration.load();
        shared_cmd->current_fps = current_fps;  // Add FPS to shared memory
        
        // Update timecode only if changed
        if (strncmp(shared_cmd->timecode, current_tc.c_str(), 11) != 0) {
            strncpy(shared_cmd->timecode, current_tc.c_str(), 11);
            shared_cmd->timecode[11] = '\0';
            
#ifndef _WIN32
            // Sync entire structure
            msync(shared_cmd, sizeof(RemoteCommand), MS_ASYNC);
#endif
            
            // Also update timecode on external display
            if (hui_initialized) {
                update_hui_timecode(current_tc);
            }
        }
    }
    catch (const std::exception& e) {
        std::cerr << "Error updating timecode: " << e.what() << std::endl;
    }
}

void RemoteControl::enqueue_command(RemoteCommand::Type type, double value) {
    std::lock_guard<std::mutex> lock(command_mutex);
    command_queue.emplace(type, value);
    command_cv.notify_one();
}

void RemoteControl::process_commands() {
    if (!initialized || !shared_cmd) {
        return;
    }
    
    RemoteCommand cmd;
    {
#ifndef _WIN32
        msync(shared_cmd, sizeof(RemoteCommand), MS_SYNC | MS_INVALIDATE);
#endif
        std::memcpy(&cmd, shared_cmd, sizeof(RemoteCommand));
    }
    
    if (cmd.status != 0) {
        return;
    }
    
    RemoteCommand::Type original_type = cmd.command_type;
    
    try {
        shared_cmd->status = 1;
#ifndef _WIN32
        msync(shared_cmd, sizeof(RemoteCommand), MS_SYNC);
#endif
        
        switch (original_type) {
            case RemoteCommand::Type::SEEK:
                handle_seek(cmd.seek_time);
                break;
            case RemoteCommand::Type::PLAY:
                handle_play();
                break;
            case RemoteCommand::Type::STOP:
                handle_stop();
                break;
            case RemoteCommand::Type::SET_SPEED:
                handle_set_speed(cmd.speed_value);
                break;
            case RemoteCommand::Type::ADJUST_SPEED:
                handle_adjust_speed(cmd.speed_value);
                break;
            case RemoteCommand::Type::SEEK_TIMECODE:
                {
                    std::string tc_str(cmd.seek_timecode, static_cast<size_t>(8));
                    handle_seek_timecode(tc_str);
                }
                break;
            case RemoteCommand::Type::SCREENSHOT:
                trigger_screenshot();
                break;
            case RemoteCommand::Type::SET_REVERSE:
                is_reverse.store(cmd.speed_value > 0);
                break;
            case RemoteCommand::Type::SEEK_AND_SCREENSHOT:
                handle_seek(cmd.seek_time);
                // Wait a bit for seek to complete
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                trigger_screenshot();
                break;
            case RemoteCommand::Type::NONE:
                break;
        }
        
        update_timecode();
        
        shared_cmd->command_type = RemoteCommand::Type::NONE;
        shared_cmd->status = 2;
#ifndef _WIN32
        msync(shared_cmd, sizeof(RemoteCommand), MS_SYNC);
#endif
    }
    catch (const std::exception& e) {
        std::cerr << "Error executing command: " << e.what() << std::endl;
        shared_cmd->command_type = RemoteCommand::Type::NONE;
        shared_cmd->status = 2;
#ifndef _WIN32
        msync(shared_cmd, sizeof(RemoteCommand), MS_SYNC);
#endif
    }
}

bool RemoteControl::create_shared_memory() {
    std::cout << "RemoteControl: Creating shared memory..." << std::endl;
    
#ifdef _WIN32
    mapping_handle = CreateFileMapping(
        INVALID_HANDLE_VALUE,
        NULL,
        PAGE_READWRITE,
        0,
        sizeof(RemoteCommand),
        L"Local\\TapeXPlayerControl"
    );
    
    if (mapping_handle == NULL) {
        std::cerr << "Failed to create file mapping" << std::endl;
        return false;
    }
    
    shared_cmd = static_cast<RemoteCommand*>(
        MapViewOfFile(mapping_handle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(RemoteCommand))
    );
    
    if (shared_cmd == nullptr) {
        std::cerr << "Failed to map view of file" << std::endl;
        CloseHandle(mapping_handle);
        return false;
    }
#else
    const char* SHM_NAME = "/tmp/tapexplayer_control";
    
    // Remove old shared memory file
    unlink(SHM_NAME);
    
    // Create new shared memory file
    shm_fd = open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) {
        std::cerr << "Failed to open shared memory: " << strerror(errno) << std::endl;
        return false;
    }
    
    // Set size
    if (ftruncate(shm_fd, sizeof(RemoteCommand)) == -1) {
        std::cerr << "Failed to set shared memory size: " << strerror(errno) << std::endl;
        close(shm_fd);
        unlink(SHM_NAME);
        return false;
    }
    
    // Map into memory
    shared_cmd = static_cast<RemoteCommand*>(
        mmap(nullptr, sizeof(RemoteCommand), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0)
    );
    
    if (shared_cmd == MAP_FAILED) {
        std::cerr << "Failed to map shared memory: " << strerror(errno) << std::endl;
        close(shm_fd);
        unlink(SHM_NAME);
        return false;
    }
    
    // Fully clear structure
    std::memset(shared_cmd, 0, sizeof(RemoteCommand));
    
    // Initialize with correct values
    shared_cmd->command_type = RemoteCommand::Type::NONE;
    shared_cmd->seek_time = 0.0;
    shared_cmd->speed_value = 0.0;
    shared_cmd->status = 2;  // completed
    strncpy(shared_cmd->timecode, "00:00:00:00", 11);
    shared_cmd->timecode[11] = '\0';
    
    // Initialize new fields
    shared_cmd->flags.is_playing = 0;
    shared_cmd->flags.is_reverse = 0;
    shared_cmd->current_rate = 1.0f;
    shared_cmd->total_duration = 0.0;
    shared_cmd->current_fps = 25.0;  // Default FPS
    
    // Force synchronization
    msync(shared_cmd, sizeof(RemoteCommand), MS_SYNC);
    
    // Check if data has been written
    RemoteCommand verify;
    std::memcpy(&verify, shared_cmd, sizeof(RemoteCommand));
    if (verify.status != 2 || verify.command_type != RemoteCommand::Type::NONE) {
        std::cerr << "Failed to initialize shared memory with correct values" << std::endl;
        cleanup_shared_memory();
        return false;
    }
#endif

    std::cout << "RemoteControl: Shared memory initialized successfully" << std::endl;
    return true;
}

void RemoteControl::cleanup_shared_memory() {
    if (!initialized) return;
    
    if (shared_cmd) {
        // Reset state before closing
        shared_cmd->command_type = RemoteCommand::Type::NONE;
        shared_cmd->seek_time = 0.0;
        shared_cmd->status = 2;
        
#ifndef _WIN32
        // Sync changes before closing
        msync(shared_cmd, sizeof(RemoteCommand), MS_SYNC);
#endif
    }
    
#ifdef _WIN32
    if (shared_cmd) {
        UnmapViewOfFile(shared_cmd);
        shared_cmd = nullptr;
    }
    if (mapping_handle) {
        CloseHandle(mapping_handle);
        mapping_handle = NULL;
    }
#else
    if (shared_cmd != MAP_FAILED && shared_cmd != nullptr) {
        munmap(shared_cmd, sizeof(RemoteCommand));
        shared_cmd = nullptr;
    }
    if (shm_fd >= 0) {
        close(shm_fd);
        // Remove shared memory file only when program is fully finished
        if (quit) {
            unlink("/tmp/tapexplayer_control");
        }
        shm_fd = -1;
    }
#endif

    initialized = false;
}

void RemoteControl::handle_seek(double time) {
    seek_to_time(time);
}

void RemoteControl::handle_play() {
    // Check current speed
    double current_speed = get_playback_rate().load();
    
    if (std::abs(current_speed) > 1.1) {
        // If speed is high, reset it
        reset_to_normal_speed();
    } else {
        // O3therwise normal pause toggle
        toggle_pause();
    }
}

void RemoteControl::handle_stop() {
    toggle_pause();
}

void RemoteControl::handle_set_speed(double speed) {
    speed = std::clamp(speed, MIN_SPEED, MAX_SPEED);
    auto& target_rate = get_target_playback_rate();
    target_rate.store(speed);
}

void RemoteControl::handle_adjust_speed(double delta) {
    auto& current_rate = get_playback_rate();
    auto& target_rate = get_target_playback_rate();
    
    double current_speed = current_rate.load();
    double new_speed = current_speed + delta;

    // Check for zero crossing
    if ((current_speed > 0 && new_speed < 0) || (current_speed < 0 && new_speed > 0)) {
        is_reverse.store(!is_reverse.load());
        new_speed = std::abs(new_speed);
    }

    // Limit speed range
    new_speed = std::clamp(new_speed, MIN_SPEED, MAX_SPEED);
    
    if (std::abs(new_speed - current_speed) > SPEED_EPSILON) {
        target_rate.store(new_speed);
    }
}

void RemoteControl::displayTimecode(int hours, int minutes, int seconds, int frames) {
    if (!midi_out) return;

    // Format timecode digits (skip hundreds of hours)
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

    // Get current speed and FPS for display
    double current_speed = get_playback_rate().load();
    double current_fps = original_fps.load();
    
    // Format speed as integer percentage (1-24 -> 100-2400)
    int speed_display = static_cast<int>(std::abs(current_speed * 100));
    speed_display = std::min(speed_display, 2400); // Cap at 2400%
    
    // Add speed digits (100-2400)
    digits.insert(digits.begin(), {
        {DISPLAY_SPEED_HUNDREDS, (speed_display / 100) % 10},
        {DISPLAY_SPEED_TENS, (speed_display / 10) % 10}
    });

    // Show FPS in the HOURS_HUNDREDS position
    std::vector<unsigned char> fps_msg = {
        0xB0,  // CC on channel 1
        DISPLAY_HOURS_HUNDREDS,
        static_cast<unsigned char>(static_cast<int>(current_fps) % 100)  // Show FPS value
    };
    midi_out->sendMessage(&fps_msg);

    // Send each digit using CC messages
    for (const auto& [position, digit] : digits) {
        std::vector<unsigned char> message = {
            0xB0,  // CC on channel 1
            position,
            static_cast<unsigned char>(0x30 + digit)  // Convert digit to ASCII-like value
        };
        midi_out->sendMessage(&message);
    }
}

void RemoteControl::handle_seek_timecode(const std::string& timecode) {
    try {
        // Use the same parse_timecode function as GUI (command-g)
        // This ensures consistent behavior across all input methods
        double target_time = parse_timecode(timecode);
        
        // Perform seek using the parsed time
        handle_seek(target_time);
    }
    catch (const std::exception& e) {
        std::cerr << "Error parsing timecode: " << e.what() << std::endl;
    }
}

void RemoteControl::initialize_display() {
    if (!midi_out) return;

    // Clear all display positions
    for (unsigned char pos = 0x41; pos <= 0x4B; pos++) {
        std::vector<unsigned char> clear_msg = {
            0xB0,  // CC on channel 1
            pos,   // Position
            0x00   // Clear value
        };
        midi_out->sendMessage(&clear_msg);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void RemoteControl::cleanup_display() {
    if (!midi_out) return;
    
    // Clear all display positions before shutdown
    for (unsigned char pos = 0x41; pos <= 0x4B; pos++) {
        std::vector<unsigned char> clear_msg = {
            0xB0,
            pos,
            0x00
        };
        midi_out->sendMessage(&clear_msg);
    }
}

std::vector<std::string> RemoteControl::get_input_devices() const {
    std::vector<std::string> devices;
    if (!midi_in) return devices;
    
    for (unsigned int i = 0; i < midi_in->getPortCount(); i++) {
        devices.push_back(midi_in->getPortName(i));
    }
    return devices;
}

std::vector<std::string> RemoteControl::get_output_devices() const {
    std::vector<std::string> devices;
    if (!midi_out) return devices;
    
    for (unsigned int i = 0; i < midi_out->getPortCount(); i++) {
        devices.push_back(midi_out->getPortName(i));
    }
    return devices;
}

bool RemoteControl::select_device(const std::string& device_name, bool is_input) {
    try {
        if (is_input && midi_in) {
            // Close current port if open
            midi_in->closePort();
            
            // Find and open new port
            for (unsigned int i = 0; i < midi_in->getPortCount(); i++) {
                if (midi_in->getPortName(i) == device_name) {
                    midi_in->openPort(i);
                    midi_in->setCallback(&RemoteControl::hui_callback, this);
                    midi_in->ignoreTypes(false, false, false);
                    current_input_device = device_name;
                    return true;
                }
            }
        }
        else if (!is_input && midi_out) {
            // Close current port if open
            midi_out->closePort();
            
            // Find and open new port
            for (unsigned int i = 0; i < midi_out->getPortCount(); i++) {
                if (midi_out->getPortName(i) == device_name) {
                    midi_out->openPort(i);
                    current_output_device = device_name;
                    initialize_display();
                    return true;
                }
            }
        }
    }
    catch (RtMidiError &error) {
        std::cerr << "Error selecting MIDI device: " << error.getMessage() << std::endl;
    }
    return false;
}

std::string RemoteControl::get_current_input_device() const {
    return current_input_device;
}

std::string RemoteControl::get_current_output_device() const {
    return current_output_device;
} 