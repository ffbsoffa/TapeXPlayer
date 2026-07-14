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
    #include <mmsystem.h>   // timeBeginPeriod (process-wide 1ms timer resolution)
    #include <commdlg.h>
    #include "WSGUI/windows/FSTPWindowsWS.h"
#elif defined(__linux__)
    #define PLATFORM_LINUX
    // GTK not needed for SDL2-based implementation
    // #include <gtk/gtk.h>
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
#include <cstdio>    // freopen / freopen_s / setvbuf (for --log)
#include <cstdlib>   // getenv (for --log)
#include "WSGUI/FSTPLog.h"   // always-on session logging
#include <cstring>   // strcmp
#include "main.h"
#include "../FSTPPlayerModule/FSTPPlayerManager.h"
#include "../FSTPVideoModule/FSTPHardwareDetection.h"

int main(int argc, char* argv[]) {
    #ifndef _WIN32
    // CRITICAL: Set malloc arena limit BEFORE any allocations
    // Fixes glibc malloc arena corruption in PipeWire/PortAudio cleanup
    // Must be set at program start, cannot be changed later
    setenv("MALLOC_ARENA_MAX", "2", 1);

    // CRITICAL FIX: Disable malloc_trim() globally to prevent crash during cleanup
    // Problem: FFmpeg av_frame_ref() corrupts heap during mass cleanup
    // When PipeWire/PortAudio later calls malloc_trim(), heap metadata is already corrupted
    // Solution: Disable trim by setting threshold to max value (memory leak on exit acceptable)
    setenv("MALLOC_TRIM_THRESHOLD_", "9999999999", 1);
    #endif

    #ifdef __linux__
    // CRITICAL FIX: Force native Wayland backend for correct scaling on GNOME
    // Without this, SDL may choose wrong backend and apply incorrect fractional scaling
    // This matches the working run_tapexplayer.sh script
    if (getenv("SDL_VIDEODRIVER") == nullptr) {
        setenv("SDL_VIDEODRIVER", "wayland", 1);
        setenv("GDK_BACKEND", "wayland", 1);
    }
    #endif

    #ifdef _WIN32
    // Request 1ms timer resolution for the WHOLE process, at the very start —
    // before the audio/render threads exist. Windows' default timer tick is
    // ~15.6ms, so std::this_thread::sleep_for(2ms) in the audio module's
    // smooth-speed / elastic-ease animation (FSTPAudioModule_wrapper.cpp) would
    // sleep ~15ms instead, stretching a 250ms ease into ~1.9s — exactly why the
    // tape-transport ramp animations looked far slower on Windows than macOS.
    // The event loop also calls timeBeginPeriod(1), but that happens later; doing
    // it here guarantees accuracy for the first play/pause too. Paired with
    // timeEndPeriod(1) at shutdown below.
    timeBeginPeriod(1);

    // On Windows the binary is built with -mwindows (no console by default).
    // Pass --debug on the command line to open a debug console window that
    // captures all stdout/stderr output (std::cout, std::cerr, printf).
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--debug") == 0) {
            // Try to attach to a parent console first (e.g. launched from MSYS2/cmd).
            // If there is no parent console, create a new one.
            if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
                AllocConsole();
                SetConsoleTitleA("TapeXPlayer - Debug Console");
            }
            // Redirect C-runtime stdout/stderr to the console
            FILE* f = nullptr;
            freopen_s(&f, "CONOUT$", "w", stdout);
            freopen_s(&f, "CONOUT$", "w", stderr);
            // Sync C++ streams with the new C handles
            std::cout.clear();
            std::cerr.clear();
            break;
        }
    }
    #endif

    // Session logging: always write a rolling log to a reliable per-user folder (see
    // FSTPLog) so a bug report already has a log — no flag, and no OneDrive-Desktop
    // hunt. `--log` only forces file capture even when attached to a terminal (dev use).
    bool force_log = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--log") == 0) { force_log = true; break; }
    }
    FSTPLog::Init(force_log);   // resolves the version itself, so the banner is complete

    std::cout << "=== TapeXPlayer 2026 - Initialization ===" << std::endl;

    // Check for file argument (skip --debug flag)
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--debug") == 0) continue;
        if (strcmp(argv[i], "--log") == 0) continue;
        std::cout << "📂 File argument detected: " << argv[i] << std::endl;
        SetInitialFileToLoad(argv[i]);
        break;
    }

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

        // Restate the machine's CPU/GPU next to the log banner. PrintDetectedHardware above
        // is verbose and scrolls away; a bug report needs the two lines that identify the
        // hardware to sit right at the top, next to the OS and build.
        const FSTPCPUInfo& cpu = g_hardware_detection->GetCPUInfo();
        const FSTPGPUInfo& gpu = g_hardware_detection->GetGPUInfo();

        std::string simd;
        if (cpu.has_avx512) simd += " AVX512";
        else if (cpu.has_avx2) simd += " AVX2";
        else if (cpu.has_avx) simd += " AVX";
        else if (cpu.has_sse4_2) simd += " SSE4.2";
        else if (cpu.has_sse2) simd += " SSE2";

        std::string accel;
        auto add = [&](bool on, const char* n) { if (on) { if (!accel.empty()) accel += ", "; accel += n; } };
        add(gpu.videotoolbox_available, "VideoToolbox");
        add(gpu.d3d11va_available,      "D3D11VA");
        add(gpu.dxva2_available,        "DXVA2");
        add(gpu.qsv_available,          "QuickSync");
        add(gpu.nvenc_available,        "NVENC");
        add(gpu.amf_available,          "AMF");
        add(gpu.vaapi_available,        "VA-API");
        add(gpu.vdpau_available,        "VDPAU");
        if (accel.empty()) accel = "none";

        std::string info =
            "CPU     : " + cpu.model_name + " (" + cpu.architecture + ", " +
                std::to_string(cpu.physical_cores) + "c/" + std::to_string(cpu.logical_cores) + "t," +
                (simd.empty() ? " no SIMD" : simd) + ")\n" +
            "GPU     : " + (gpu.model_name.empty() ? "(unknown)" : gpu.model_name) +
                (gpu.vendor.empty() ? "" : " [" + gpu.vendor + "]") +
                (gpu.memory_mb > 0 ? ", " + std::to_string(gpu.memory_mb) + " MB VRAM" : "") + "\n" +
            "HW accel: " + accel + "\n" +
            "Decoder : " + FSTPHardwareDetection::AccelTypeToString(best_decoder.accel_type);
        FSTPLog::LogSystemInfo(info);
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

    // Wait a bit before Pa_Terminate to let all streams finish cleanup
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    Pa_Terminate();
    CleanupHardwareDetection();

    #ifdef _WIN32
    timeEndPeriod(1);  // pair the process-wide timeBeginPeriod(1) from startup
    #endif

    return result;
}