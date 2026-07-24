#include "FSTPHardwareDetection.h"
#include <iostream>
#include <algorithm>
#include <memory>
#include <thread>   // std::thread::hardware_concurrency — cross-platform core count

// Platform detection
#ifdef _WIN32
    #define PLATFORM_WINDOWS
#elif defined(__linux__)
    #define PLATFORM_LINUX
#elif defined(__APPLE__)
    #define PLATFORM_MACOS
#endif

// Platform-specific includes
#ifdef PLATFORM_MACOS
    // Use only basic C API without Objective-C
    #include <sys/sysctl.h>
    #include <sys/utsname.h>
#endif

#ifdef PLATFORM_WINDOWS
    #include <windows.h>
    #include <dxgi.h>
    // NOTE: We deliberately do NOT statically link d3d11 / dxgi / mfplat.
    //   * d3d11.dll and mfplat.dll functions are never actually called here (only DXGI is used),
    //     so linking them only added a load-time import that breaks startup on a bare Windows 7
    //     SP1 without the DirectX Platform Update ("d3d11.dll is missing").
    //   * CreateDXGIFactory (the one DLL function we do use) is now resolved at RUNTIME via
    //     LoadLibrary/GetProcAddress (see DetectGPU_Windows), so if dxgi.dll is absent we simply
    //     fall back to "CPU decode only" instead of failing to launch.
    // dxgi.h is still included for the IDXGIFactory / IDXGIAdapter interface + struct definitions.
#endif

#ifdef PLATFORM_LINUX
    #include <va/va.h>
    #include <va/va_drm.h>
    #include <fcntl.h>
    #include <unistd.h>
    #include <fstream>
    #include <sstream>
    #include <cstring>
    #include <sys/utsname.h>
#endif

// FFmpeg includes for testing
extern "C" {
    #include <libavcodec/avcodec.h>
    #include <libavutil/hwcontext.h>
    #include <libavutil/pixfmt.h>
}

// Global variable
FSTPHardwareDetection* g_hardware_detection = nullptr;


// ============================================================
// Correctly parse the Mac release year from the model identifier.
// Fixes a bug in the original code: it took the digit AFTER the comma
// ("MacBookPro13,3" → "3"), when the digit BEFORE the comma ("13") is needed.
// ============================================================
static int GetMacYearFromModelString(const std::string& mac_model) {
    size_t comma = mac_model.find(',');
    if (comma == std::string::npos || comma == 0) return 0;

    // Find the start of the number before the comma
    size_t num_end = comma;
    size_t num_start = num_end;
    while (num_start > 0 && std::isdigit(mac_model[num_start - 1])) num_start--;
    if (num_start >= num_end) return 0;

    int major = std::stoi(mac_model.substr(num_start, num_end - num_start));
    std::string family = mac_model.substr(0, num_start);

    if (family == "MacBookPro") {
        // MacBookPro: major versions → release years
        // Source: Apple Hardware Identifiers
        // major 8  = 2011 (Sandy Bridge) ← minimum Mac per user's choice
        // major 12 = 2015 (Broadwell)    ← minimum for reliable H.264 HW decode
        // major 13 = 2016 (Skylake)      ← HYBRID reference device (user tested)
        // major 17 = 2020 (M1)           ← Apple Silicon starts here
        static const int tbl[] = {
            0,    // 0  invalid
            2006, // 1  Core Duo
            2006, // 2  Core 2 Duo
            2007, // 3  Santa Rosa
            2008, // 4  Penryn
            2009, // 5  Nehalem/Penryn
            2010, // 6  Arrandale
            2010, // 7  Arrandale
            2011, // 8  Sandy Bridge  (2nd gen)
            2012, // 9  Ivy Bridge    (3rd gen)
            2012, // 10 Ivy Bridge Retina
            2013, // 11 Haswell       (4th gen, 2013–2014)
            2015, // 12 Broadwell     (5th gen) — min. reliable H.264 HW
            2016, // 13 Skylake       (6th gen) — HYBRID reference (tested)
            2017, // 14 Kaby Lake     (7th gen)
            2018, // 15 Coffee Lake   (8th gen)
            2019, // 16 Coffee Lake   (9th gen)
            2020, // 17 M1 Apple Silicon
            2021, // 18 M1 Pro/Max
            2023, // 19 M2 Pro/Max
        };
        int n = (int)(sizeof(tbl) / sizeof(tbl[0]));
        if (major >= 0 && major < n) return tbl[major];
        if (major >= n) return 2024;  // future Apple Silicon
    }
    else if (family == "MacBookAir") {
        static const int tbl[] = {0, 2008, 2009, 2010, 2011, 2012, 2013, 2015,
                                   2017, 2018, 2019, 2020, 2021, 2022, 2024};
        int n = (int)(sizeof(tbl) / sizeof(tbl[0]));
        if (major >= 0 && major < n) return tbl[major];
    }
    else if (family == "Macmini") {
        static const int tbl[] = {0, 2006, 2007, 2009, 2010, 2011, 2012, 2014,
                                   2018, 2020, 2023};
        int n = (int)(sizeof(tbl) / sizeof(tbl[0]));
        if (major >= 0 && major < n) return tbl[major];
    }
    else if (family == "iMac") {
        // iMac7=2007, iMac8=2008 ... iMac20=2020, iMac21=2021 M1
        if (major >= 7 && major <= 21) return 2000 + major;
    }
    // MacBook, MacPro, iMacPro, etc. — approximately by number
    if (major >= 15) return 2020;
    if (major >= 12) return 2017;
    if (major >= 9)  return 2014;
    if (major >= 6)  return 2011;
    return 2008;
}

// ============================================================
// Determine the decoder profile from CPU/GPU characteristics.
//
// Three user reference devices:
//   FULL_HARDWARE  → Apple M1 Mac
//   HYBRID_VT_CPU  → MacBook Pro 2016 Intel (Skylake i7-6xxx)
//   MINIMUM        → Intel Celeron Gold 7505 (Tiger Lake 2-core, 15 W)
// ============================================================
FSTPDecoderProfile FSTPHardwareDetection::DetermineDecoderProfile(const FSTPCPUInfo& cpu) const {
    // With the division of labour settled — the GPU decodes the original, the CPU serves the proxy
    // — exactly one question is left worth asking here: is there a hardware decoder to put the
    // original on? Everything this function used to weigh has been removed, because none of it
    // survived contact with the evidence:
    //
    //   * The Intel-generation ladder never ran. ExtractIntelGeneration looked for the literal
    //     "Core i", and no real brand string contains it — CPUID reports "Intel(R) Core(TM) i9-…".
    //     Every Intel machine scored 0 and fell straight through, so those branches were dead from
    //     the day they were written.
    //   * The Celeron/Atom and core-count rules sorted CPUs into profiles that nothing consumed.
    //   * FULL_HARDWARE ("VideoToolbox for everything") is not true even on Apple Silicon: the
    //     proxy decodes on the CPU there too, and has for a long time.
    //
    // Worse, the ladder printed its verdict into every session log, where it reads as a decision.
    // It never was one — nothing in the tree consumes this value; the real choices are made at the
    // decode sites. A machine with a D3D11VA-capable RTX 4080 was being told, in its own log, that
    // it had "no suitable HW acceleration", and that line cost real time during the #10 hunt.
    //
    // This is the interim, honest version: one question, answered from what was actually detected.
    // The proper rework — decide by measuring both paths rather than by recognising hardware — is
    // issue #12.
    (void)cpu;

    if (gpu_info_.HasAnyHWAccel() && gpu_info_.hw_decode_score >= 50.0) {
        std::cout << "  🎯 Decode plan: GPU for the original, CPU for the proxy"
                  << "  [" << (gpu_info_.model_name.empty() ? std::string("unnamed GPU")
                                                            : gpu_info_.model_name)
                  << ", hw_decode_score " << gpu_info_.hw_decode_score << "]" << std::endl;
        return FSTPDecoderProfile::HYBRID_VT_CPU;
    }

    std::cout << "  🎯 Decode plan: CPU for both — no usable hardware decoder";
    if (gpu_info_.HasAnyHWAccel()) {
        std::cout << " (one was detected but scored only " << gpu_info_.hw_decode_score
                  << ", below the 50 needed)";
    }
    std::cout << std::endl;
    return FSTPDecoderProfile::SOFTWARE_ONLY;
}

FSTPHardwareDetection::FSTPHardwareDetection() {
    std::cout << "FSTPHardwareDetection: Initialization of hardware acceleration detection system..." << std::endl;
}

FSTPHardwareDetection::~FSTPHardwareDetection() {
    // Restore original power mode on exit
    RestorePowerMode();
    detected_hardware_.clear();
}

bool FSTPHardwareDetection::DetectAllHardware() {
    std::cout << "FSTPHardwareDetection: Starting full hardware acceleration detection..." << std::endl;

    // Detect CPU and set performance mode if needed
    cpu_info_ = DetectCPU();

    if (cpu_info_.requires_performance_mode) {
        SetPerformanceMode();
    }

    // Detect GPU (requires CPU info to be already detected)
    gpu_info_ = DetectGPU();

    // Determine the decoder profile (based on CPU + GPU together)
    cpu_info_.decoder_profile = DetermineDecoderProfile(cpu_info_);
    static const char* profile_names[] = {"UNKNOWN", "FULL_HARDWARE", "HYBRID_VT_CPU",
                                           "MINIMUM", "SOFTWARE_ONLY"};
    int pidx = static_cast<int>(cpu_info_.decoder_profile);
    if (pidx >= 0 && pidx <= 4)
        std::cout << "  → Active profile: " << profile_names[pidx] << std::endl;

    detected_hardware_.clear();
    bool found_any = false;

#ifdef PLATFORM_MACOS
    if (DetectVideoToolbox()) {
        found_any = true;
        std::cout << "✓ VideoToolbox detected" << std::endl;
    }
    if (DetectMetalPerformanceShaders()) {
        found_any = true;
        std::cout << "✓ Metal Performance Shaders detected" << std::endl;
    }
#endif

#ifdef PLATFORM_WINDOWS
    if (DetectDXVA2()) {
        found_any = true;
        std::cout << "✓ DXVA2 detected" << std::endl;
    }
    if (DetectD3D11VA()) {
        found_any = true;
        std::cout << "✓ D3D11VA detected" << std::endl;
    }
    if (DetectIntelQSV()) {
        found_any = true;
        std::cout << "✓ Intel QSV detected" << std::endl;
    }
    if (DetectNVIDIANVENC()) {
        found_any = true;
        std::cout << "✓ NVIDIA NVENC detected" << std::endl;
    }
    if (DetectAMDAdvancedMediaFramework()) {
        found_any = true;
        std::cout << "✓ AMD AMF detected" << std::endl;
    }
#endif

#ifdef PLATFORM_LINUX
    if (DetectVAAPI()) {
        found_any = true;
        std::cout << "✓ VA-API detected" << std::endl;
    }
    if (DetectIntelQSVLinux()) {
        found_any = true;
        std::cout << "✓ Intel QSV (Linux) detected" << std::endl;
    }
#endif

    // Common methods for all platforms
    if (DetectOpenCL()) {
        found_any = true;
        std::cout << "✓ OpenCL detected" << std::endl;
    }

    if (DetectVulkan()) {
        found_any = true;
        std::cout << "✓ Vulkan detected" << std::endl;
    }

    // Sort by performance
    std::sort(detected_hardware_.begin(), detected_hardware_.end(),
              [](const FSTPHardwareInfo& a, const FSTPHardwareInfo& b) {
                  return a.performance_score > b.performance_score;
              });

    std::cout << "FSTPHardwareDetection: Detected " << detected_hardware_.size()
              << " hardware accelerations" << std::endl;

    return found_any;
}

const std::vector<FSTPHardwareInfo>& FSTPHardwareDetection::GetDetectedHardware() const {
    return detected_hardware_;
}

FSTPHardwareInfo FSTPHardwareDetection::GetBestDecoder() const {
    for (const auto& hw : detected_hardware_) {
        if (static_cast<int>(hw.supported_codecs) &
            (static_cast<int>(FSTPCodecSupport::H264_DECODE) |
             static_cast<int>(FSTPCodecSupport::H265_DECODE))) {
            return hw;
        }
    }
    return FSTPHardwareInfo{FSTPHWAccelType::NONE, "Software", "N/A", FSTPCodecSupport::NONE, 0, 0, 0, 0, false, false, 0, 0.0};
}

FSTPHardwareInfo FSTPHardwareDetection::GetBestEncoder() const {
    for (const auto& hw : detected_hardware_) {
        if (static_cast<int>(hw.supported_codecs) &
            (static_cast<int>(FSTPCodecSupport::H264_ENCODE) |
             static_cast<int>(FSTPCodecSupport::H265_ENCODE))) {
            return hw;
        }
    }
    return FSTPHardwareInfo{FSTPHWAccelType::NONE, "Software", "N/A", FSTPCodecSupport::NONE, 0, 0, 0, 0, false, false, 0, 0.0};
}

#ifdef PLATFORM_MACOS
bool FSTPHardwareDetection::DetectVideoToolbox() {
    FSTPHardwareInfo vt_info;
    vt_info.accel_type = FSTPHWAccelType::VIDEOTOOLBOX;

    // Determine architecture via sysctl
    size_t size = 0;
    sysctlbyname("hw.optional.arm64", nullptr, &size, nullptr, 0);
    bool is_apple_silicon = (size > 0);

    // Get Mac model and year to detect old hardware with poor VideoToolbox performance
    char model[256];
    size_t model_size = sizeof(model);
    sysctlbyname("hw.model", model, &model_size, nullptr, 0);
    std::string mac_model(model);

    // Get CPU brand string for Intel Macs
    char cpu_brand[256];
    size_t cpu_brand_size = sizeof(cpu_brand);
    sysctlbyname("machdep.cpu.brand_string", cpu_brand, &cpu_brand_size, nullptr, 0);
    std::string cpu_name(cpu_brand);

    if (is_apple_silicon) {
        vt_info.device_name = "Apple VideoToolbox (Apple Silicon)";
        vt_info.supported_codecs = FSTPCodecSupport::H264_DECODE | FSTPCodecSupport::H264_ENCODE |
                                  FSTPCodecSupport::H265_DECODE | FSTPCodecSupport::H265_ENCODE |
                                  FSTPCodecSupport::VP9_DECODE | FSTPCodecSupport::AV1_DECODE |
                                  FSTPCodecSupport::MPEG2_DECODE | FSTPCodecSupport::MPEG4_DECODE;
        vt_info.supports_10bit = true;
        vt_info.performance_score = 95.0;

        std::cout << "  ⚡ Apple Silicon detected: " << mac_model << std::endl;
        std::cout << "     VideoToolbox hardware acceleration: EXCELLENT" << std::endl;
    } else {
        // Intel Mac - check for old models with poor VideoToolbox performance
        bool is_old_intel = false;

        // MacBook Pro 2016-2017 with Intel HD Graphics 530/630 have poor VideoToolbox performance
        // Better to use CPU decoding with SIMD optimizations
        if (mac_model.find("MacBookPro13") != std::string::npos ||  // 2016 models
            mac_model.find("MacBookPro14") != std::string::npos ||  // 2017 models
            cpu_name.find("i5-6") != std::string::npos ||           // Skylake 6th gen
            cpu_name.find("i7-6") != std::string::npos ||           // Skylake 6th gen
            cpu_name.find("i5-7") != std::string::npos ||           // Kaby Lake 7th gen
            cpu_name.find("i7-7") != std::string::npos) {           // Kaby Lake 7th gen
            is_old_intel = true;
        }

        if (is_old_intel) {
            // On old Intel Macs (2016-2017), VideoToolbox is SLOWER than CPU decoding
            // Reduce performance score significantly to prefer CPU decoding
            vt_info.device_name = "Apple VideoToolbox (Intel - OLD, NOT RECOMMENDED)";
            vt_info.supported_codecs = FSTPCodecSupport::H264_DECODE | FSTPCodecSupport::H264_ENCODE |
                                      FSTPCodecSupport::H265_DECODE | FSTPCodecSupport::H265_ENCODE |
                                      FSTPCodecSupport::MPEG2_DECODE | FSTPCodecSupport::MPEG4_DECODE;
            vt_info.supports_10bit = false;
            vt_info.performance_score = 30.0;  // LOW score - prefer CPU decoding

            std::cout << "  ⚠️  Old Intel Mac detected: " << mac_model << std::endl;
            std::cout << "     CPU: " << cpu_name << std::endl;
            std::cout << "     VideoToolbox hardware acceleration: POOR (use CPU decoding instead)" << std::endl;
            std::cout << "     Recommendation: CPU with AVX2 SIMD is FASTER than VideoToolbox on this hardware" << std::endl;
        } else {
            // Newer Intel Mac (2018+) - VideoToolbox is acceptable
            vt_info.device_name = "Apple VideoToolbox (Intel)";
            vt_info.supported_codecs = FSTPCodecSupport::H264_DECODE | FSTPCodecSupport::H264_ENCODE |
                                      FSTPCodecSupport::H265_DECODE | FSTPCodecSupport::H265_ENCODE |
                                      FSTPCodecSupport::MPEG2_DECODE | FSTPCodecSupport::MPEG4_DECODE;
            vt_info.supports_10bit = false;
            vt_info.performance_score = 85.0;

            std::cout << "  ✅ Intel Mac detected: " << mac_model << std::endl;
            std::cout << "     VideoToolbox hardware acceleration: GOOD" << std::endl;
        }
    }

    vt_info.driver_version = "System";
    vt_info.max_decode_width = 8192;
    vt_info.max_decode_height = 8192;
    vt_info.max_encode_width = 8192;
    vt_info.max_encode_height = 8192;
    vt_info.supports_444 = true;
    vt_info.memory_mb = 0; // Uses system memory

    detected_hardware_.push_back(vt_info);
    return true;
}

bool FSTPHardwareDetection::DetectMetalPerformanceShaders() {
    // Simple check - Metal is available on all modern Macs
    FSTPHardwareInfo metal_info;
    metal_info.accel_type = FSTPHWAccelType::OPENCL; // Use as OpenCL equivalent
    metal_info.device_name = "Metal Performance Shaders";
    metal_info.driver_version = "System";
    metal_info.supported_codecs = FSTPCodecSupport::NONE; // For calculations, not decoding
    metal_info.performance_score = 80.0;
    metal_info.max_decode_width = 0;
    metal_info.max_decode_height = 0;
    metal_info.max_encode_width = 0;
    metal_info.max_encode_height = 0;
    metal_info.supports_10bit = false;
    metal_info.supports_444 = false;
    metal_info.memory_mb = 0;

    detected_hardware_.push_back(metal_info);
    return true;
}
#endif

#ifdef PLATFORM_WINDOWS
// Real detection happens in DetectGPU_Windows (DXGI adapter enumeration) and is
// already written to gpu_info_ by the time these methods run. Here we only copy
// the result into detected_hardware_ — previously these methods unconditionally
// returned true and the list showed DXVA2/D3D11VA even on a machine without a
// single hardware GPU.
bool FSTPHardwareDetection::DetectDXVA2() {
    if (!gpu_info_.dxva2_available) return false;
    FSTPHardwareInfo dxva_info{};
    dxva_info.accel_type = FSTPHWAccelType::DXVA2;
    dxva_info.device_name = gpu_info_.model_name + " (DXVA2)";
    dxva_info.driver_version = "System";
    dxva_info.supported_codecs = FSTPCodecSupport::H264_DECODE | FSTPCodecSupport::MPEG2_DECODE;
    dxva_info.performance_score = gpu_info_.hw_decode_score - 5.0;  // legacy path, just below D3D11VA
    dxva_info.memory_mb = gpu_info_.memory_mb;

    detected_hardware_.push_back(dxva_info);
    return true;
}

bool FSTPHardwareDetection::DetectD3D11VA() {
    if (!gpu_info_.d3d11va_available) return false;
    FSTPHardwareInfo d3d11_info{};
    d3d11_info.accel_type = FSTPHWAccelType::D3D11VA;
    d3d11_info.device_name = gpu_info_.model_name + " (D3D11VA)";
    d3d11_info.driver_version = "System";
    d3d11_info.supported_codecs = gpu_info_.supported_codecs;
    d3d11_info.performance_score = gpu_info_.hw_decode_score;
    d3d11_info.memory_mb = gpu_info_.memory_mb;

    detected_hardware_.push_back(d3d11_info);
    return true;
}

bool FSTPHardwareDetection::DetectIntelQSV() {
    return gpu_info_.qsv_available;   // set by VendorId in DetectGPU_Windows
}

bool FSTPHardwareDetection::DetectNVIDIANVENC() {
    return gpu_info_.nvenc_available; // set by VendorId in DetectGPU_Windows
}

bool FSTPHardwareDetection::DetectAMDAdvancedMediaFramework() {
    return gpu_info_.amf_available;   // set by VendorId in DetectGPU_Windows
}
#endif

#ifdef PLATFORM_LINUX
bool FSTPHardwareDetection::DetectVAAPI() {
    // Real check is in DetectGPU_Linux (av_hwdevice_ctx_create probe over render
    // nodes); here the result is copied into detected_hardware_. Previously this
    // method always returned false, so DetectAllHardware printed "Hardware
    // acceleration not detected" even with a working VA-API.
    if (!gpu_info_.vaapi_available || gpu_info_.vaapi_devices.empty()) return false;
    FSTPHardwareInfo va_info{};
    va_info.accel_type = FSTPHWAccelType::VAAPI;
    va_info.device_name = gpu_info_.model_name + " (" + gpu_info_.vaapi_devices[0] + ")";
    va_info.driver_version = "libva";
    va_info.supported_codecs = gpu_info_.supported_codecs;
    va_info.performance_score = gpu_info_.hw_decode_score;

    detected_hardware_.push_back(va_info);
    return true;
}

bool FSTPHardwareDetection::DetectIntelQSVLinux() {
    return gpu_info_.qsv_available;   // Intel GPU with working VA-API
}
#endif

bool FSTPHardwareDetection::DetectOpenCL() {
    // Base check OpenCL will be implemented later
    return false;
}

bool FSTPHardwareDetection::DetectVulkan() {
    // Base check Vulkan will be implemented later
    return false;
}

std::string FSTPHardwareDetection::AccelTypeToString(FSTPHWAccelType type) {
    switch (type) {
        case FSTPHWAccelType::NONE: return "None";
        case FSTPHWAccelType::VIDEOTOOLBOX: return "VideoToolbox";
        case FSTPHWAccelType::QSV_INTEL: return "Intel QSV";
        case FSTPHWAccelType::NVENC_NVIDIA: return "NVIDIA NVENC";
        case FSTPHWAccelType::AMF_AMD: return "AMD AMF";
        case FSTPHWAccelType::VAAPI: return "VA-API";
        case FSTPHWAccelType::DXVA2: return "DXVA2";
        case FSTPHWAccelType::D3D11VA: return "D3D11VA";
        case FSTPHWAccelType::OPENCL: return "OpenCL";
        case FSTPHWAccelType::VULKAN: return "Vulkan";
        default: return "Unknown";
    }
}

void FSTPHardwareDetection::PrintDetectedHardware() const {
    std::cout << "\n=== Detected hardware accelerations ===" << std::endl;
    for (size_t i = 0; i < detected_hardware_.size(); ++i) {
        const auto& hw = detected_hardware_[i];
        std::cout << "[" << i << "] " << AccelTypeToString(hw.accel_type) << std::endl;
        std::cout << "    Device: " << hw.device_name << std::endl;
        std::cout << "    Performance: " << hw.performance_score << std::endl;
        std::cout << "    Supports 10-bit: " << (hw.supports_10bit ? "Yes" : "No") << std::endl;
        std::cout << std::endl;
    }
}

// Global functions
bool InitializeHardwareDetection() {
    if (g_hardware_detection == nullptr) {
        g_hardware_detection = new FSTPHardwareDetection();
        return g_hardware_detection->DetectAllHardware();
    }
    return true;
}

void CleanupHardwareDetection() {
    delete g_hardware_detection;
    g_hardware_detection = nullptr;
}

// CPU Detection Implementation
FSTPCPUInfo FSTPHardwareDetection::DetectCPU() {
    FSTPCPUInfo info;

    // Initialize defaults
    info.model_name = "Unknown CPU";
    info.vendor = "Unknown";
    info.architecture = "unknown";  // NEW: will be set per-platform
    info.physical_cores = 2;
    info.logical_cores = 4;
    info.base_frequency_ghz = 2.0;
    info.max_frequency_ghz = 3.0;
    info.has_sse2 = false;  // NEW: will be detected
    info.has_avx = false;
    info.has_avx2 = false;
    info.has_avx512 = false;
    info.has_sse4_2 = false;
    info.power_governor = "unknown";
    info.recommended_max_speed = 32;  // Default: keep original 32x
    info.optimal_segment_size = 500;
    info.optimal_preload_count = 8;
    info.decode_throughput_fps = 800.0;
    info.requires_performance_mode = false;
    info.requires_half_fps_for_50_60 = false;  // Default: full FPS proxy
    info.is_pentium_gold_7505 = false;
    info.is_compact_device = false;
    info.device_model = "";
    info.decoder_profile = FSTPDecoderProfile::UNKNOWN;

    // macOS-specific fields
    info.is_apple_silicon = false;
    info.mac_model = "";
    info.mac_year = 0;

    // Real core count for EVERY platform, set BEFORE the capability classification runs. Windows and
    // Linux never populated it, so cpu_info_ kept a hardcoded default of 2 — which mislabelled every
    // machine as a weak 2-core CPU (the MINIMUM screen) and starved the decode thread-count sizing, on
    // ANY processor including recent high-core parts. hardware_concurrency() is logical cores:
    // brand-free, correct for any part present or future (no lookup table to go stale), and enough for
    // the coarse tiers. macOS refines it via sysctl below. (The HW-vs-software decode CHOICE is made
    // from the GPU in DetermineDecoderProfile — this feeds honest reporting, thread sizing and tiers.)
    if (unsigned hc = std::thread::hardware_concurrency()) {
        info.logical_cores  = static_cast<int>(hc);
        info.physical_cores = static_cast<int>(hc);
    }

#ifdef PLATFORM_LINUX
    // Detect architecture
    struct utsname uts;
    if (uname(&uts) == 0) {
        info.architecture = std::string(uts.machine);  // "x86_64", "aarch64", etc.
    }

    // Detect compact devices (GPD Pocket, etc.)
    std::ifstream vendor_file("/sys/class/dmi/id/sys_vendor");
    std::ifstream product_file("/sys/class/dmi/id/product_name");

    std::string vendor_name, product_name;

    if (vendor_file.is_open()) {
        std::getline(vendor_file, vendor_name);
        vendor_file.close();
    }

    if (product_file.is_open()) {
        std::getline(product_file, product_name);
        product_file.close();
    }

    // GPD Pocket 3 (G1621-02), GPD Pocket 4, or other GPD compact devices
    if (vendor_name == "GPD" ||
        (product_name.find("GPD") != std::string::npos && product_name.find("Pocket") != std::string::npos)) {
        info.is_compact_device = true;

        // Determine specific model
        if (product_name == "G1621-02") {
            info.device_model = "GPD Pocket 3";
        } else if (product_name.find("Pocket") != std::string::npos) {
            info.device_model = product_name;
        } else {
            info.device_model = "GPD " + product_name;
        }

        std::cout << "🎮 [COMPACT DEVICE] Detected: " << info.device_model
                  << " (" << product_name << ")" << std::endl;
        std::cout << "   Mouse shuttle mode: LEFT CLICK (no modifiers required)" << std::endl;
    }
    // Read CPU model name from /proc/cpuinfo
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    while (std::getline(cpuinfo, line)) {
        if (line.find("model name") != std::string::npos) {
            size_t pos = line.find(':');
            if (pos != std::string::npos) {
                info.model_name = line.substr(pos + 2);
                break;
            }
        }
    }

    // Detect CPU features
    DetectCPUFeatures(info);

    // Detect power mode
    DetectCPUPowerMode(info);

    // Classify the CPU by capability (core count) — the brand-string generation parser that used to
    // sit here was dead (it matched the literal "Core i", never present in a real CPUID string) and
    // is gone; the decode plan no longer depends on it (see DetermineDecoderProfile).
    ApplyCPUSpecificOptimizations(info);
#endif

#ifdef PLATFORM_MACOS
    // NEW: Detect Apple Silicon via arm64 support
    size_t size = 0;
    sysctlbyname("hw.optional.arm64", nullptr, &size, nullptr, 0);
    info.is_apple_silicon = (size > 0);

    // NEW: Get Mac model (e.g., "MacBookPro14,3")
    char model[256];
    size_t model_size = sizeof(model);
    sysctlbyname("hw.model", model, &model_size, nullptr, 0);
    info.mac_model = std::string(model);

    // Parse the release year from the model identifier ("MacBookPro13,3" → 2016).
    // Fixed: the original code took the digit AFTER the comma ("3"), we need BEFORE ("13").
    info.mac_year = GetMacYearFromModelString(info.mac_model);

    // NEW: Set architecture
    info.architecture = info.is_apple_silicon ? "arm64" : "x86_64";

    // Get CPU brand string
    char cpu_brand[256];
    size_t cpu_brand_size = sizeof(cpu_brand);
    sysctlbyname("machdep.cpu.brand_string", cpu_brand, &cpu_brand_size, nullptr, 0);
    info.model_name = std::string(cpu_brand);

    // Get CPU vendor
    char cpu_vendor[256];
    size_t cpu_vendor_size = sizeof(cpu_vendor);
    sysctlbyname("machdep.cpu.vendor", cpu_vendor, &cpu_vendor_size, nullptr, 0);
    info.vendor = std::string(cpu_vendor);

    // For Apple Silicon, override vendor
    if (info.is_apple_silicon) {
        info.vendor = "Apple";
    }

    // Get core counts
    int physical_cores = 0;
    size = sizeof(physical_cores);
    sysctlbyname("hw.physicalcpu", &physical_cores, &size, nullptr, 0);
    info.physical_cores = physical_cores;

    int logical_cores = 0;
    size = sizeof(logical_cores);
    sysctlbyname("hw.logicalcpu", &logical_cores, &size, nullptr, 0);
    info.logical_cores = logical_cores;

    // Get CPU frequency (in Hz, convert to GHz)
    int64_t freq = 0;
    size = sizeof(freq);
    sysctlbyname("hw.cpufrequency", &freq, &size, nullptr, 0);
    info.base_frequency_ghz = freq / 1000000000.0;
    info.max_frequency_ghz = info.base_frequency_ghz * 1.5; // Estimate

    // Detect SIMD features via sysctl
    int feature_val = 0;
    size = sizeof(feature_val);

    // NEW: SSE2 detection (only for x86_64)
    if (!info.is_apple_silicon) {
        sysctlbyname("hw.optional.sse2", &feature_val, &size, nullptr, 0);
        info.has_sse2 = (feature_val == 1);

        sysctlbyname("hw.optional.sse4_2", &feature_val, &size, nullptr, 0);
        info.has_sse4_2 = (feature_val == 1);

        sysctlbyname("hw.optional.avx1_0", &feature_val, &size, nullptr, 0);
        info.has_avx = (feature_val == 1);

        sysctlbyname("hw.optional.avx2_0", &feature_val, &size, nullptr, 0);
        info.has_avx2 = (feature_val == 1);

        sysctlbyname("hw.optional.avx512f", &feature_val, &size, nullptr, 0);
        info.has_avx512 = (feature_val == 1);
    } else {
        // Apple Silicon uses NEON, not SSE/AVX
        info.has_sse2 = false;
        info.has_sse4_2 = false;
        info.has_avx = false;
        info.has_avx2 = false;
        info.has_avx512 = false;
    }

    // macOS doesn't have power governors like Linux
    info.power_governor = "n/a";
    info.requires_performance_mode = false;

    std::cout << "🖥️  [CPU] Detected: " << info.model_name << std::endl;
    std::cout << "   Architecture: " << info.architecture << std::endl;
    std::cout << "   Mac Model: " << info.mac_model;
    if (info.mac_year > 0) {
        std::cout << " (≈" << info.mac_year << ")";
    }
    std::cout << std::endl;
    std::cout << "   Cores: " << info.physical_cores << " physical, " << info.logical_cores << " logical" << std::endl;
    std::cout << "   Frequency: " << info.base_frequency_ghz << " GHz" << std::endl;

    if (info.is_apple_silicon) {
        std::cout << "   SIMD: NEON (Apple Silicon)" << std::endl;
    } else {
        std::cout << "   SIMD: SSE2=" << (info.has_sse2 ? "✅" : "❌")
                  << " SSE4.2=" << (info.has_sse4_2 ? "✅" : "❌")
                  << " AVX=" << (info.has_avx ? "✅" : "❌")
                  << " AVX2=" << (info.has_avx2 ? "✅" : "❌")
                  << " AVX512=" << (info.has_avx512 ? "✅" : "❌") << std::endl;
    }
#endif

#ifdef PLATFORM_WINDOWS
    // Windows CPU probe. The GPU side is detected above; fill the CPU side too so cpu_info_ carries
    // the real processor instead of the "Unknown CPU" default — the honest report and the
    // "---- system info ----" line then name it correctly. Same registry source as the session-log
    // header: it carries the brand string verbatim on x86 and ARM, sidestepping the MSVC-vs-MinGW
    // cpuid-name split.
    {
        HKEY hk;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                0, KEY_READ, &hk) == ERROR_SUCCESS) {
            wchar_t nameW[256]; DWORD nsz = sizeof(nameW), type = 0;
            if (RegQueryValueExW(hk, L"ProcessorNameString", nullptr, &type,
                    reinterpret_cast<LPBYTE>(nameW), &nsz) == ERROR_SUCCESS && type == REG_SZ) {
                int wlen = (int)(nsz / sizeof(wchar_t));
                while (wlen > 0 && nameW[wlen - 1] == L'\0') --wlen;   // drop trailing NULs
                if (wlen > 0) {
                    int len = WideCharToMultiByte(CP_UTF8, 0, nameW, wlen, nullptr, 0, nullptr, nullptr);
                    std::string name(len, '\0');
                    WideCharToMultiByte(CP_UTF8, 0, nameW, wlen, &name[0], len, nullptr, nullptr);
                    while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
                    if (!name.empty()) info.model_name = name;
                }
            }
            RegCloseKey(hk);
        }

        // Vendor from the brand string (the registry name always leads with the maker).
        if (info.model_name.find("Intel") != std::string::npos)      info.vendor = "Intel";
        else if (info.model_name.find("AMD") != std::string::npos)   info.vendor = "AMD";

        // Architecture of the running machine.
        SYSTEM_INFO si{}; GetNativeSystemInfo(&si);
        info.architecture = (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64) ? "arm64"
                          : (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL) ? "x86"
                          : "x86_64";

        // SIMD is REPORTED ONLY (it never gates the decode plan — that reads the GPU). The GCC/MinGW
        // builtins read CPUID without executing any wide instruction, so probing can't fault; this is
        // just honest reporting, not the AVX-512 tier test that was deliberately removed.
#if defined(__i386__) || defined(__x86_64__)
        __builtin_cpu_init();
        info.has_sse2   = __builtin_cpu_supports("sse2");
        info.has_sse4_2 = __builtin_cpu_supports("sse4.2");
        info.has_avx    = __builtin_cpu_supports("avx");
        info.has_avx2   = __builtin_cpu_supports("avx2");
        info.has_avx512 = __builtin_cpu_supports("avx512f");
#endif

        int c = info.physical_cores;
        std::cout << "🖥️  [CPU] " << info.model_name << " — " << c << " logical cores, "
                  << (c >= 6 ? "high-performance" : c <= 2 ? "minimum-class" : "standard")
                  << " (decode plan is GPU-driven; the CPU serves the light proxy)." << std::endl;
    }
#endif

    return info;
}

void FSTPHardwareDetection::DetectCPUFeatures(FSTPCPUInfo& info) {
#ifdef PLATFORM_LINUX
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    bool got_flags = false;
    bool got_vendor = false;

    while (std::getline(cpuinfo, line)) {
        // Read vendor_id: "GenuineIntel" or "AuthenticAMD"
        if (!got_vendor && line.find("vendor_id") != std::string::npos) {
            size_t pos = line.find(':');
            if (pos != std::string::npos) {
                info.vendor = line.substr(pos + 2);
                // Strip any trailing whitespace
                while (!info.vendor.empty() && (info.vendor.back() == '\n' ||
                                                info.vendor.back() == '\r' ||
                                                info.vendor.back() == ' '))
                    info.vendor.pop_back();
                got_vendor = true;
            }
        }
        // Read SIMD flags
        if (!got_flags &&
            (line.find("flags") != std::string::npos || line.find("Features") != std::string::npos)) {
            info.has_sse2   = (line.find("sse2")    != std::string::npos);
            info.has_sse4_2 = (line.find("sse4_2")  != std::string::npos);
            info.has_avx    = (line.find("avx")     != std::string::npos);  // includes avx2
            info.has_avx2   = (line.find("avx2")    != std::string::npos);
            info.has_avx512 = (line.find("avx512f") != std::string::npos);
            got_flags = true;
        }
        if (got_flags && got_vendor) break;
    }
#endif
}

void FSTPHardwareDetection::DetectCPUPowerMode(FSTPCPUInfo& info) {
#ifdef PLATFORM_LINUX
    std::ifstream governor("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
    if (governor.is_open()) {
        std::getline(governor, info.power_governor);
    }
#endif
}

void FSTPHardwareDetection::ApplyCPUSpecificOptimizations(FSTPCPUInfo& info) {
    // ── MINIMUM class: Celeron / Pentium / Atom ───────────────────────────────
    // Reference: Intel Celeron Gold 7505 (Tiger Lake, 2-core, 15 W, user tested)
    // Extrapolation: all Celeron, Pentium, Atom share similar limitations

    bool is_celeron = info.model_name.find("Celeron") != std::string::npos;
    bool is_pentium = info.model_name.find("Pentium") != std::string::npos;
    bool is_atom    = info.model_name.find("Atom")    != std::string::npos;

    if (is_celeron || is_pentium || is_atom) {
        // Exact match with the user's reference device: Pentium/Celeron 7505
        bool is_7505 = info.model_name.find("7505") != std::string::npos;

        if (is_7505) {
            std::cout << "  🎯 Intel Celeron/Pentium Gold 7505 (MINIMUM reference device)" << std::endl;
            std::cout << "     User test: 24× max, 360p proxy, QSV" << std::endl;
            info.decode_throughput_fps = 1450.0;  // QSV: 1400-1500 fps
        } else if (is_atom) {
            std::cout << "  🔧 Intel Atom detected: " << info.model_name << std::endl;
            std::cout << "     Max speed: 16× (very low-power CPU)" << std::endl;
            info.recommended_max_speed = 16;  // Atom slower than Celeron
        } else {
            std::cout << "  🔧 Intel Celeron/Pentium detected: " << info.model_name << std::endl;
            std::cout << "     Extrapolated from Celeron 7505: 24× max, 360p proxy" << std::endl;
        }

        info.requires_performance_mode = true;
        if (info.recommended_max_speed == 32) info.recommended_max_speed = 24;  // limit
        info.is_pentium_gold_7505 = true;  // flag for MINIMUM-profile optimizations
        return;
    }

    // ── Secondary MINIMUM criterion: ≤2 physical x86 cores ──────────────────
    // Rationale: 2-core Core i3/i5 of previous generations have similar
    // decoder throughput (i3-6006U 2016, i5-5300U 2015, etc.)
    if (!info.is_apple_silicon && info.physical_cores <= 2 &&
        (info.architecture == "x86_64" || info.architecture.find("i686") != std::string::npos)) {
        std::cout << "  🔧 2-core x86 CPU: " << info.model_name << std::endl;
        std::cout << "     Throughput similar to Celeron 7505 → 24× limit" << std::endl;
        info.requires_performance_mode = true;
        if (info.recommended_max_speed == 32) info.recommended_max_speed = 24;
        info.is_pentium_gold_7505 = true;
        return;
    }

    // ── AMD Athlon Silver/Gold (MINIMUM class, like Celeron/Pentium) ─────────
    bool is_athlon_low = (info.model_name.find("Athlon Silver") != std::string::npos ||
                          info.model_name.find("Athlon Gold")   != std::string::npos);
    if (is_athlon_low) {
        std::cout << "  🔧 AMD Athlon Silver/Gold detected: " << info.model_name << std::endl;
        std::cout << "     AMD equivalent of Celeron/Pentium → 24× limit, 360p proxy" << std::endl;
        info.requires_performance_mode = true;
        if (info.recommended_max_speed == 32) info.recommended_max_speed = 24;
        info.is_pentium_gold_7505 = true;  // use the same MINIMUM profile
        return;
    }

    // ── AMD Ryzen (HYBRID class) ──────────────────────────────────────────────
    if (info.model_name.find("Ryzen") != std::string::npos) {
        // All Ryzen: VA-API (Linux) / D3D11VA + AMF (Windows)
        std::cout << "  ✅ AMD Ryzen detected: " << info.model_name << std::endl;
        std::cout << "     VA-API (Linux) / D3D11VA + AMF (Windows)" << std::endl;
        // Keep the recommended speed (32×) — Ryzen handles it
        return;
    }

    // ── High-performance CPUs — recognised by CORE COUNT, nothing else ───────────
    // Deliberately NOT AVX-512: (a) the old gate was AVX-512-only, and modern Intel from Alder Lake
    // on (Arrow Lake / Core Ultra and later) DROPPED it, so no recent high-end Intel was ever
    // recognised; (b) AVX-512 on Intel is a crash minefield — fused off on Pentium/Celeron Tiger Lake
    // (the 7505 MINIMUM device, where it crashed the app) and unsafe under P/E-core migration — which
    // is exactly why has_avx512 and the AVX-512 SIMD paths are FORCE-DISABLED across this codebase
    // (heap-corruption risk). Physical core count is a robust, brand-free capability signal that
    // covers future parts without a lookup table. Anything reaching here already cleared the MINIMUM
    // screens above, so a healthy multi-core part is a capable software-proxy decoder — full 32×.
    if (info.physical_cores >= 6) {
        std::cout << "  ⚡ High-performance CPU: " << info.model_name
                  << " (" << info.physical_cores << " cores) → full 32× shuttle" << std::endl;
        info.recommended_max_speed = 32;
    } else {
        // Mid-range (e.g. a 4-core i5/i7): keep whatever the default/MINIMUM screens left. No brand
        // guessing — the GPU decodes the original and the CPU only serves the light 540p proxy.
        std::cout << "  ℹ️  CPU: " << info.model_name << " (" << info.physical_cores
                  << " cores) — default decode plan, GPU handles the original" << std::endl;
    }
}

bool FSTPHardwareDetection::SetPerformanceMode() {
#ifdef PLATFORM_LINUX
    // Save original governor
    std::ifstream gov_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
    if (gov_file.is_open()) {
        std::getline(gov_file, original_governor_);
        gov_file.close();
    }

    std::cout << "  ⚡ Switching CPU to performance mode..." << std::endl;
    std::cout << "     Original mode: " << original_governor_ << std::endl;

    // Try to set performance mode for all CPUs
    int result = system("for cpu in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo performance | sudo tee $cpu > /dev/null 2>&1; done");

    if (result == 0) {
        std::cout << "  ✅ CPU switched to performance mode" << std::endl;
        std::cout << "     Will be restored to '" << original_governor_ << "' on exit" << std::endl;
        return true;
    } else {
        std::cout << "  ⚠️  Could not switch to performance mode (need sudo)" << std::endl;
        std::cout << "     Run with: sudo or grant permissions:" << std::endl;
        std::cout << "     echo 'performance' | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor" << std::endl;
        return false;
    }
#endif
    return false;
}

bool FSTPHardwareDetection::RestorePowerMode() {
#ifdef PLATFORM_LINUX
    if (original_governor_.empty() || original_governor_ == "performance") {
        return true; // Nothing to restore
    }

    std::cout << "  🔄 Restoring CPU power mode to: " << original_governor_ << std::endl;

    std::string cmd = "for cpu in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo " +
                      original_governor_ + " | sudo tee $cpu > /dev/null 2>&1; done";
    int result = system(cmd.c_str());

    if (result == 0) {
        std::cout << "  ✅ CPU power mode restored" << std::endl;
        return true;
    } else {
        std::cout << "  ⚠️  Could not restore power mode" << std::endl;
        return false;
    }
#endif
    return false;
}

// ========================================
// GPU Detection Implementation (NEW!)
// ========================================

FSTPGPUInfo FSTPHardwareDetection::DetectGPU() {
    FSTPGPUInfo info;

    // Initialize defaults
    info.model_name = "Unknown GPU";
    info.vendor = "Unknown";
    info.memory_mb = 0;
    info.videotoolbox_available = false;
    info.vaapi_available = false;
    info.vdpau_available = false;
    info.dxva2_available = false;
    info.d3d11va_available = false;
    info.qsv_available = false;
    info.nvenc_available = false;
    info.amf_available = false;
    info.hw_decode_score = 0.0;
    info.prefer_cpu_for_lowres = false;
    info.supported_codecs = FSTPCodecSupport::NONE;

#ifdef PLATFORM_MACOS
    DetectGPU_macOS(info);
#elif defined(PLATFORM_LINUX)
    DetectGPU_Linux(info);
#elif defined(PLATFORM_WINDOWS)
    DetectGPU_Windows(info);
#endif

    gpu_info_ = info;  // Store in member variable
    return info;
}

#ifdef PLATFORM_MACOS
void FSTPHardwareDetection::DetectGPU_macOS(FSTPGPUInfo& info) {
    std::cout << "🎨 [GPU] Detecting macOS GPU and hardware acceleration..." << std::endl;

    // Detect Apple Silicon vs Intel
    if (cpu_info_.is_apple_silicon) {
        // Apple Silicon (M1/M2/M3/M4)
        info.model_name = "Apple GPU (Apple Silicon)";
        info.vendor = "Apple";
        info.videotoolbox_available = true;
        info.hw_decode_score = 95.0;  // Excellent performance
        info.prefer_cpu_for_lowres = false;  // VideoToolbox is always good on Apple Silicon

        info.supported_codecs = FSTPCodecSupport::H264_DECODE | FSTPCodecSupport::H264_ENCODE |
                               FSTPCodecSupport::H265_DECODE | FSTPCodecSupport::H265_ENCODE |
                               FSTPCodecSupport::VP9_DECODE | FSTPCodecSupport::AV1_DECODE |
                               FSTPCodecSupport::MPEG2_DECODE | FSTPCodecSupport::MPEG4_DECODE;

        std::cout << "   ✅ Apple Silicon GPU detected" << std::endl;
        std::cout << "   VideoToolbox: EXCELLENT (score: 95)" << std::endl;
        std::cout << "   Supported codecs: H.264, H.265, VP9, AV1, MPEG-2, MPEG-4" << std::endl;

    } else {
        // Intel Mac — any (2011–2021, before Apple Silicon)
        // Reference: MacBook Pro 2016 (Skylake, user tested) works best in
        // HYBRID_VT_CPU mode: VideoToolbox for full-res + CPU for proxy ≤640p.
        // Extrapolation: this behavior applies to ALL Intel Macs, because the
        // VideoToolbox GPU-pipeline overhead at small resolutions is constant
        // and CPU decode for the proxy is always competitive.
        info.vendor = "Intel";
        info.videotoolbox_available = true;
        info.prefer_cpu_for_lowres = true;  // HYBRID: CPU for proxy, VT for full-res

        info.supported_codecs = FSTPCodecSupport::H264_DECODE | FSTPCodecSupport::H264_ENCODE |
                               FSTPCodecSupport::H265_DECODE | FSTPCodecSupport::H265_ENCODE |
                               FSTPCodecSupport::MPEG2_DECODE | FSTPCodecSupport::MPEG4_DECODE;

        // The VideoToolbox score depends on CPU generation (measured by Mac year).
        // The newer the Mac, the better VideoToolbox for full-res, but proxy stays CPU.
        int year = cpu_info_.mac_year;
        if (year >= 2018) {
            // Coffee Lake (8th gen) and newer — VideoToolbox good for full-res
            info.model_name = "Intel GPU (Mac 2018+, Coffee Lake+)";
            info.hw_decode_score = 70.0;
            std::cout << "   ✅ Intel Mac GPU (2018+): VideoToolbox full-res"
                      << " + CPU proxy (score: 70)" << std::endl;
        } else if (year >= 2015) {
            // Broadwell–Kaby Lake (5–7 gen) — user's test range (2016 MBP)
            info.model_name = "Intel GPU (Mac 2015–2017, Broadwell/Skylake/KabyLake)";
            info.hw_decode_score = 55.0;
            std::cout << "   ⚙️  Intel Mac GPU (" << year << "): HYBRID — VideoToolbox full-res"
                      << " + CPU proxy (score: 55)" << std::endl;
            std::cout << "      Profile confirmed by user test (MacBook Pro 2016)" << std::endl;
        } else if (year >= 2012) {
            // Ivy Bridge / Haswell (3–4 gen, 2012–2014)
            info.model_name = "Intel GPU (Mac 2012–2014, Ivy Bridge/Haswell)";
            info.hw_decode_score = 45.0;
            std::cout << "   ⚙️  Intel Mac GPU (" << year << "): HYBRID conservative"
                      << " (score: 45)" << std::endl;
        } else {
            // Sandy Bridge (2011) — minimal VideoToolbox support
            info.model_name = "Intel GPU (Mac 2011, Sandy Bridge)";
            info.hw_decode_score = 25.0;
            std::cout << "   ⚠️  Intel Mac GPU (" << year << "): Sandy Bridge,"
                      << " minimal VideoToolbox support (score: 25)" << std::endl;
        }
    }

    std::cout << "   Hardware Acceleration: VideoToolbox available" << std::endl;
}
#endif

#ifdef PLATFORM_LINUX
void FSTPHardwareDetection::DetectGPU_Linux(FSTPGPUInfo& info) {
    std::cout << "🎨 [GPU] Detecting Linux GPU and hardware acceleration..." << std::endl;

    // Walk the render nodes and actually TRY to create a VA-API device via FFmpeg.
    // The presence of /dev/dri/renderD* does not mean VA-API works: a proprietary
    // NVIDIA driver, a missing libva-driver package, or an unprivileged container
    // give a node where av_hwdevice_ctx_create() will fail later in the decoder.
    // One probe at startup guarantees that only working nodes land in vaapi_devices,
    // so decoders don't waste time on a path that is known to be dead.
    int present_nodes = 0;
    for (int i = 128; i < 140; ++i) {
        std::string device_path = "/dev/dri/renderD" + std::to_string(i);
        int fd = open(device_path.c_str(), O_RDWR);
        if (fd < 0) continue;
        close(fd);
        ++present_nodes;

        AVBufferRef* test_ctx = nullptr;
        int ret = av_hwdevice_ctx_create(&test_ctx, AV_HWDEVICE_TYPE_VAAPI,
                                         device_path.c_str(), nullptr, 0);
        if (ret >= 0) {
            av_buffer_unref(&test_ctx);
            info.vaapi_devices.push_back(device_path);
            std::cout << "   ✅ VA-API works on " << device_path << std::endl;
        } else {
            char errbuf[128] = {0};
            av_strerror(ret, errbuf, sizeof(errbuf));
            std::cout << "   ⚠️  " << device_path << ": VA-API init failed ("
                      << errbuf << ")" << std::endl;
        }
    }

    if (!info.vaapi_devices.empty()) {
        info.vaapi_available = true;
        info.prefer_cpu_for_lowres = false;

        info.supported_codecs = FSTPCodecSupport::H264_DECODE | FSTPCodecSupport::H265_DECODE |
                               FSTPCodecSupport::VP9_DECODE | FSTPCodecSupport::MPEG2_DECODE;

        // Vendor comes from sysfs of the SELECTED node, not card0: on hybrid
        // graphics (Intel+NVIDIA laptop) card0 may not be the GPU whose render
        // node we actually use.
        std::string node = info.vaapi_devices[0];
        std::string node_name = node.substr(node.find_last_of('/') + 1);
        std::string vendor_id;
        std::ifstream vendor_file("/sys/class/drm/" + node_name + "/device/vendor");
        if (vendor_file.is_open()) vendor_file >> vendor_id;

        if (vendor_id == "0x8086") {
            info.vendor = "Intel";
            info.model_name = "Intel GPU (VA-API)";
            info.qsv_available = true;  // Intel GPUs support QSV
            info.hw_decode_score = 80.0;
        } else if (vendor_id == "0x1002") {
            info.vendor = "AMD";
            info.model_name = "AMD GPU (VA-API)";
            info.hw_decode_score = 75.0;  // Mesa VA-API on AMD is stable
        } else if (vendor_id == "0x10de") {
            info.vendor = "NVIDIA";
            info.model_name = "NVIDIA GPU (VA-API)";
            info.hw_decode_score = 60.0;  // nouveau/vaapi-nvidia: works, but conservative
        } else {
            info.vendor = "Unknown";
            info.model_name = "GPU (VA-API)";
            info.hw_decode_score = 60.0;  // probe passed → VA-API actually works
        }

        std::cout << "   ✅ VA-API devices working: " << info.vaapi_devices.size() << std::endl;
        for (const auto& device : info.vaapi_devices) {
            std::cout << "      • " << device << std::endl;
        }
        std::cout << "   Hardware Acceleration: VA-API available (score: "
                  << static_cast<int>(info.hw_decode_score) << ")" << std::endl;

    } else {
        if (present_nodes == 0) {
            std::cout << "   ⚠️  No /dev/dri/renderD* nodes found" << std::endl;
        } else {
            std::cout << "   ⚠️  Render nodes present, but VA-API failed to initialize"
                      << " (no libva driver? NVIDIA proprietary?)" << std::endl;
        }
        std::cout << "   Hardware Acceleration: NOT AVAILABLE (CPU decode only)" << std::endl;
        info.model_name = "No GPU acceleration";
        info.vendor = "Unknown";
        info.hw_decode_score = 0.0;
    }
}
#endif

#ifdef PLATFORM_WINDOWS
void FSTPHardwareDetection::DetectGPU_Windows(FSTPGPUInfo& info) {
    std::cout << "🎨 [GPU] Detecting Windows GPU and hardware acceleration..." << std::endl;

    // Enumerate GPUs via DXGI and pick the most capable adapter. Any real (non-WARP)
    // GPU from the last decade can do D3D11VA decode for H.264/H.265, so the mere
    // presence of an adapter is enough to enable the existing D3D11VA branch in
    // FSTPFullResDecoderV2. If av_hwdevice_ctx_create() fails at runtime anyway,
    // there is a software fallback there.
    // Resolve CreateDXGIFactory at runtime so a missing dxgi.dll (bare Win7 SP1 without the
    // DirectX Platform Update) degrades to "CPU decode only" instead of preventing the exe from
    // loading. IID_IDXGIFactory is defined locally to avoid any link-time GUID dependency.
    typedef HRESULT (WINAPI *PFN_CreateDXGIFactory)(REFIID, void**);
    static const GUID kIID_IDXGIFactory =
        { 0x7b7166ec, 0x21c7, 0x44ae, { 0xb2, 0x1a, 0xc9, 0xae, 0x32, 0x1a, 0xe3, 0x69 } };

    HMODULE dxgi_dll = LoadLibraryW(L"dxgi.dll");
    if (!dxgi_dll) {
        std::cout << "   ⚠️  dxgi.dll not available (Win7 without DX Platform Update?) — CPU decode only" << std::endl;
        info.model_name = "No GPU acceleration";
        info.vendor = "Unknown";
        info.hw_decode_score = 0.0;
        return;
    }
    auto pCreateDXGIFactory =
        reinterpret_cast<PFN_CreateDXGIFactory>(GetProcAddress(dxgi_dll, "CreateDXGIFactory"));
    if (!pCreateDXGIFactory) {
        std::cout << "   ⚠️  CreateDXGIFactory not found in dxgi.dll — CPU decode only" << std::endl;
        info.model_name = "No GPU acceleration";
        info.vendor = "Unknown";
        info.hw_decode_score = 0.0;
        FreeLibrary(dxgi_dll);
        return;
    }

    IDXGIFactory* factory = nullptr;
    HRESULT hr = pCreateDXGIFactory(kIID_IDXGIFactory, reinterpret_cast<void**>(&factory));
    if (FAILED(hr) || !factory) {
        std::cout << "   ⚠️  CreateDXGIFactory failed — CPU decode only" << std::endl;
        info.model_name = "No GPU acceleration";
        info.vendor = "Unknown";
        info.hw_decode_score = 0.0;
        FreeLibrary(dxgi_dll);
        return;
    }

    IDXGIAdapter* adapter = nullptr;
    IDXGIAdapter* best_adapter = nullptr;
    DXGI_ADAPTER_DESC best_desc = {};
    SIZE_T best_vram = 0;

    for (UINT i = 0; factory->EnumAdapters(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC desc = {};
        if (SUCCEEDED(adapter->GetDesc(&desc))) {
            // Skip Microsoft Basic Render Driver (WARP, pure software): vendor 0x1414
            bool is_software = (desc.VendorId == 0x1414);
            if (!is_software && desc.DedicatedVideoMemory >= best_vram) {
                if (best_adapter) best_adapter->Release();
                best_adapter = adapter;
                best_desc = desc;
                best_vram = desc.DedicatedVideoMemory;
                adapter = nullptr;  // ownership moved to best_adapter
            }
        }
        if (adapter) { adapter->Release(); adapter = nullptr; }
    }

    if (!best_adapter) {
        std::cout << "   ⚠️  No hardware GPU found (software adapter only) — CPU decode" << std::endl;
        info.model_name = "No GPU acceleration";
        info.vendor = "Unknown";
        info.hw_decode_score = 0.0;
        factory->Release();
        FreeLibrary(dxgi_dll);  // release the DLL only after the COM interface is released
        return;
    }

    // Adapter description WCHAR → UTF-8
    char name_buf[256] = {0};
    WideCharToMultiByte(CP_UTF8, 0, best_desc.Description, -1,
                        name_buf, sizeof(name_buf) - 1, nullptr, nullptr);
    info.model_name = name_buf;
    info.memory_mb = static_cast<int>(best_desc.DedicatedVideoMemory / (1024 * 1024));

    // All real Windows GPUs of recent years support D3D11VA decode.
    info.d3d11va_available = true;
    info.dxva2_available = true;  // legacy path as a fallback
    info.supported_codecs = FSTPCodecSupport::H264_DECODE | FSTPCodecSupport::H265_DECODE |
                            FSTPCodecSupport::MPEG2_DECODE;

    switch (best_desc.VendorId) {
        case 0x10DE:  // NVIDIA
            info.vendor = "NVIDIA";
            info.nvenc_available = true;
            info.hw_decode_score = 85.0;
            break;
        case 0x1002:  // AMD / ATI
            info.vendor = "AMD";
            info.amf_available = true;
            info.hw_decode_score = 80.0;
            break;
        case 0x8086:  // Intel
            info.vendor = "Intel";
            info.qsv_available = true;
            info.hw_decode_score = 75.0;
            break;
        default:
            info.vendor = "Unknown";
            info.hw_decode_score = 60.0;  // unknown but real GPU → assume D3D11VA is usable
            break;
    }
    info.prefer_cpu_for_lowres = false;

    std::cout << "   ✅ GPU detected: " << info.model_name
              << " (" << info.vendor << ", " << info.memory_mb << " MB VRAM)" << std::endl;
    std::cout << "   Hardware Acceleration: D3D11VA available (score: "
              << static_cast<int>(info.hw_decode_score) << ")" << std::endl;

    best_adapter->Release();
    factory->Release();
    FreeLibrary(dxgi_dll);  // release the DLL only after all COM interfaces are released
}
#endif

// ========================================
// Decoder Strategy Selection (NEW!)
// ========================================

FSTPDecoderStrategy FSTPHardwareDetection::GetDecoderStrategy(
    int width,
    int height,
    int codec_id,
    double playback_speed) const {

    FSTPDecoderStrategy strategy;

    // === 1. DETERMINE IF WE SHOULD USE HARDWARE ACCELERATION ===

    bool is_lowres = (width <= 640 && height <= 480);
    bool is_highspeed = (playback_speed > 8.0);

    // OLD INTEL MAC RULE: CPU decode is FASTER for ≤480p!
    if (gpu_info_.prefer_cpu_for_lowres && is_lowres) {
        strategy.use_hw_accel = false;
        strategy.hw_accel_type = FSTPHWAccelType::NONE;
        strategy.strategy_reason = "Intel Mac ≤480p → CPU faster than VideoToolbox";
    }
    // High-speed playback (shuttle mode) - prefer CPU for stability
    else if (is_highspeed && is_lowres) {
        strategy.use_hw_accel = false;
        strategy.hw_accel_type = FSTPHWAccelType::NONE;
        strategy.strategy_reason = "High-speed shuttle mode (>8×) → CPU decode for stability";
    }
    // GPU available and beneficial
    else if (gpu_info_.HasAnyHWAccel() && gpu_info_.hw_decode_score >= 50.0) {
        strategy.use_hw_accel = true;
        strategy.hw_accel_type = gpu_info_.GetBestAccelType();
        strategy.strategy_reason = "GPU decode (score: " + std::to_string(static_cast<int>(gpu_info_.hw_decode_score)) + ")";

        // Set device path for VA-API (Linux)
        if (strategy.hw_accel_type == FSTPHWAccelType::VAAPI && !gpu_info_.vaapi_devices.empty()) {
            strategy.hw_device_path = gpu_info_.vaapi_devices[0];  // Use first device
        }
    }
    // Fallback to CPU
    else {
        strategy.use_hw_accel = false;
        strategy.hw_accel_type = FSTPHWAccelType::NONE;
        strategy.strategy_reason = "No suitable HW acceleration → CPU decode";
    }

    // === 2. THREADING CONFIGURATION ===

    if (is_highspeed) {
        // High-speed shuttle mode - reduce threads for stability
        strategy.thread_count = 1;
        strategy.thread_type = 1;  // FF_THREAD_FRAME only (simplified)
    } else {
        // Normal playback - use more threads
        strategy.thread_count = std::min(cpu_info_.physical_cores, 4);
        strategy.thread_type = 3;  // FF_THREAD_FRAME | FF_THREAD_SLICE
    }

    // === 3. SIMD LEVEL ===

    if (cpu_info_.is_apple_silicon) {
        strategy.simd_level = 5;  // NEON (ARM)
    } else if (cpu_info_.has_avx2) {
        strategy.simd_level = 4;  // AVX2
    } else if (cpu_info_.has_avx) {
        strategy.simd_level = 3;  // AVX
    } else if (cpu_info_.has_sse4_2) {
        strategy.simd_level = 2;  // SSE4.1
    } else if (cpu_info_.has_sse2) {
        strategy.simd_level = 1;  // SSE2
    } else {
        strategy.simd_level = 0;  // No SIMD
    }

    // === 4. MEMORY MANAGEMENT ===

    // ALWAYS use frame pool for efficient memory reuse
    strategy.use_frame_pool = true;
    strategy.pool_initial_size = 50;  // Default pool size

    // === 5. CODEC FLAGS ===

    // AV_CODEC_FLAG_LOW_DELAY = 0x0080 (minimize latency)
    // AV_CODEC_FLAG2_FAST = 0x0001 (fast decoding)
    strategy.codec_flags = 0x0080;   // LOW_DELAY
    strategy.codec_flags2 = 0x0001;  // FAST

    // === 6. PERFORMANCE TUNING ===

    // Don't skip frames/IDCTs/loop filter (full quality)
    strategy.skip_frame = 0;         // AVDISCARD_DEFAULT
    strategy.skip_idct = 0;          // AVDISCARD_DEFAULT
    strategy.skip_loop_filter = 0;   // AVDISCARD_DEFAULT

    return strategy;
}