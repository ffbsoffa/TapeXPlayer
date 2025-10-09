// TapeXPlayer (2026 version).
//
// Experimental video player representing a software implementation of professional videotape recorder approach
// in digital format. The project arose from interest in how the logic of Betacam format videotape recorders can
// be embodied in software code.
//
// © 2026 Maksim Maloletkin (FFB_soffa) under GPL license.


// Preprocessor directives (check if we're running on macOS, Linux, Haiku, Windows) //
#ifdef _WIN32
    #define PLATFORM_WINDOWS
    #include <windows.h>
    #include <commdlg.h>
#elif defined(__linux__)
    #define PLATFORM_LINUX
    #include <gtk/gtk.h>
    #include <unistd.h>
#elif defined(__APPLE__)
    #define PLATFORM_MACOS
    // #include <Cocoa/Cocoa.h>
    // #include <CoreFoundation/CoreFoundation.h>
    #include "WSGUI/darwin/sdl/FSTPDarwinWS.h"
#else
    #error "Unsupported platform"
#endif


// Native headers for platform (temporarily commented out)
// #ifdef PLATFORM_MACOS
//     #include "WSGUI/FSTPNativeMacOS.h"
// #else
    #include <SDL2/SDL.h>  // Fallback for other platforms
// #endif

#include <portaudio.h>
#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include "main.h"
#include "../FSTPPlayerModule/FSTPPlayerManager.h"
#include "../FSTPVideoModule/FSTPHardwareDetection.h"

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[]) {
    std::cout << "=== TapeXPlayer 2026 - Initialization ===" << std::endl;

    // 1. Hardware acceleration detection (must be first)
    std::cout << "Stage 1: Graphics capabilities detection..." << std::endl;
    if (!InitializeHardwareDetection()) {
        std::cout << "Warning: Hardware acceleration not detected, using software decoding" << std::endl;
    }

    // Show detection results
    if (g_hardware_detection) {
        g_hardware_detection->PrintDetectedHardware();
        auto best_decoder = g_hardware_detection->GetBestDecoder();
        std::cout << "Best decoder: " << FSTPHardwareDetection::AccelTypeToString(best_decoder.accel_type) << std::endl;
    }

    // 2. Early PortAudio initialization for smooth animation
    std::cout << "Stage 2: Audio system initialization..." << std::endl;
    PaError err = Pa_Initialize();
    if (err != paNoError) {
        std::cerr << "Failed to initialize PortAudio: " << Pa_GetErrorText(err) << std::endl;
        CleanupHardwareDetection();
        return -1;
    }

    // 3. Pre-initialization of player manager
    std::cout << "Stage 3: Player initialization..." << std::endl;
    InitPlayerManager();

    // 4. Start main interface loop
    std::cout << "Stage 4: Starting user interface..." << std::endl;
    int result = RunMainUILoop();

    // Cleanup on exit
    std::cout << "TapeXPlayer shutdown..." << std::endl;
    Pa_Terminate();
    CleanupHardwareDetection();

    return result;
}