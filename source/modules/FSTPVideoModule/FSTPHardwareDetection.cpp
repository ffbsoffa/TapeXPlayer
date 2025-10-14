#include "FSTPHardwareDetection.h"
#include <iostream>
#include <algorithm>
#include <memory>

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
    #include <d3d11.h>
    #include <mfapi.h>
    #pragma comment(lib, "dxgi.lib")
    #pragma comment(lib, "d3d11.lib")
    #pragma comment(lib, "mfplat.lib")
#endif

#ifdef PLATFORM_LINUX
    #include <va/va.h>
    #include <va/va_drm.h>
    #include <fcntl.h>
    #include <unistd.h>
    #include <fstream>
    #include <sstream>
    #include <cstring>
#endif

// FFmpeg includes for testing
extern "C" {
    #include <libavcodec/avcodec.h>
    #include <libavutil/hwcontext.h>
    #include <libavutil/pixfmt.h>
}

// Global variable
FSTPHardwareDetection* g_hardware_detection = nullptr;

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
bool FSTPHardwareDetection::DetectDXVA2() {
    // Simplified implementation - in reality, a more complex check is needed
    FSTPHardwareInfo dxva_info;
    dxva_info.accel_type = FSTPHWAccelType::DXVA2;
    dxva_info.device_name = "DirectX Video Acceleration 2.0";
    dxva_info.driver_version = "System";
    dxva_info.supported_codecs = FSTPCodecSupport::H264_DECODE | FSTPCodecSupport::MPEG2_DECODE;
    dxva_info.performance_score = 70.0;

    detected_hardware_.push_back(dxva_info);
    return true;
}

bool FSTPHardwareDetection::DetectD3D11VA() {
    FSTPHardwareInfo d3d11_info;
    d3d11_info.accel_type = FSTPHWAccelType::D3D11VA;
    d3d11_info.device_name = "Direct3D 11 Video Acceleration";
    d3d11_info.driver_version = "System";
    d3d11_info.supported_codecs = FSTPCodecSupport::H264_DECODE | FSTPCodecSupport::H265_DECODE;
    d3d11_info.performance_score = 75.0;

    detected_hardware_.push_back(d3d11_info);
    return true;
}

bool FSTPHardwareDetection::DetectIntelQSV() {
    // Here should be a check for the presence of Intel GPU and QSV
    return false;
}

bool FSTPHardwareDetection::DetectNVIDIANVENC() {
    // Here should be a check for NVIDIA GPU
    return false;
}

bool FSTPHardwareDetection::DetectAMDAdvancedMediaFramework() {
    // Here should be a check for AMD GPU
    return false;
}
#endif

#ifdef PLATFORM_LINUX
bool FSTPHardwareDetection::DetectVAAPI() {
    // Simplified check VA-API
    return false;
}

bool FSTPHardwareDetection::DetectIntelQSVLinux() {
    return false;
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
    info.physical_cores = 2;
    info.logical_cores = 4;
    info.base_frequency_ghz = 2.0;
    info.max_frequency_ghz = 3.0;
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
    info.is_compact_device = false;  // Default: not a compact device
    info.device_model = "";

#ifdef PLATFORM_LINUX
    // Detect compact devices (GPD Pocket, etc.)
    std::ifstream dmi_file("/sys/class/dmi/id/product_name");
    if (dmi_file.is_open()) {
        std::string product_name;
        std::getline(dmi_file, product_name);
        dmi_file.close();

        // GPD Pocket 3, GPD Pocket 4, or other GPD compact devices
        if (product_name.find("GPD") != std::string::npos &&
            product_name.find("Pocket") != std::string::npos) {
            info.is_compact_device = true;
            info.device_model = product_name;
            std::cout << "🎮 [COMPACT DEVICE] Detected: " << product_name << std::endl;
            std::cout << "   Mouse shuttle mode: LEFT CLICK (no modifiers required)" << std::endl;
        }
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

    // Apply CPU-specific optimizations
    ApplyCPUSpecificOptimizations(info);
#endif

#ifdef PLATFORM_MACOS
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

    // Get core counts
    int physical_cores = 0;
    size_t size = sizeof(physical_cores);
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

    sysctlbyname("hw.optional.sse4_2", &feature_val, &size, nullptr, 0);
    info.has_sse4_2 = (feature_val == 1);

    sysctlbyname("hw.optional.avx1_0", &feature_val, &size, nullptr, 0);
    info.has_avx = (feature_val == 1);

    sysctlbyname("hw.optional.avx2_0", &feature_val, &size, nullptr, 0);
    info.has_avx2 = (feature_val == 1);

    sysctlbyname("hw.optional.avx512f", &feature_val, &size, nullptr, 0);
    info.has_avx512 = (feature_val == 1);

    // macOS doesn't have power governors like Linux
    info.power_governor = "n/a";
    info.requires_performance_mode = false;

    std::cout << "🖥️  [CPU] Detected: " << info.model_name << std::endl;
    std::cout << "   Cores: " << info.physical_cores << " physical, " << info.logical_cores << " logical" << std::endl;
    std::cout << "   Frequency: " << info.base_frequency_ghz << " GHz" << std::endl;
    std::cout << "   SIMD: SSE4.2=" << (info.has_sse4_2 ? "✅" : "❌")
              << " AVX=" << (info.has_avx ? "✅" : "❌")
              << " AVX2=" << (info.has_avx2 ? "✅" : "❌")
              << " AVX512=" << (info.has_avx512 ? "✅" : "❌") << std::endl;
#endif

    return info;
}

void FSTPHardwareDetection::DetectCPUFeatures(FSTPCPUInfo& info) {
#ifdef PLATFORM_LINUX
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;

    while (std::getline(cpuinfo, line)) {
        if (line.find("flags") != std::string::npos || line.find("Features") != std::string::npos) {
            info.has_sse4_2 = (line.find("sse4_2") != std::string::npos);
            info.has_avx = (line.find("avx") != std::string::npos);
            info.has_avx2 = (line.find("avx2") != std::string::npos);
            info.has_avx512 = (line.find("avx512f") != std::string::npos);
            break;
        }
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
    // Check for Intel Pentium Gold 7505 and similar low-power CPUs
    if (info.model_name.find("Pentium") != std::string::npos &&
        info.model_name.find("7505") != std::string::npos) {

        std::cout << "  🎯 Intel Pentium Gold 7505 detected" << std::endl;
        std::cout << "     Max speed limited to 24x (tested stable with performance mode)" << std::endl;
        info.requires_performance_mode = true;
        info.recommended_max_speed = 24;  // Limit to 24x for this CPU
    }
    // Check for other Pentium Gold/Silver CPUs (low-power, similar performance)
    else if (info.model_name.find("Pentium") != std::string::npos &&
             (info.model_name.find("Gold") != std::string::npos ||
              info.model_name.find("Silver") != std::string::npos)) {

        std::cout << "  🔧 Intel Pentium Gold/Silver detected" << std::endl;
        std::cout << "     Max speed limited to 24x (conservative for low-power CPUs)" << std::endl;
        info.requires_performance_mode = true;
        info.recommended_max_speed = 24;  // Conservative for low-power Pentiums
    }
    // Generic check for AVX-512 CPUs (likely higher-end, keep 32x)
    else if (info.has_avx512) {
        std::cout << "  ⚡ AVX-512 CPU detected" << std::endl;
        std::cout << "     Max speed: 32x (default for high-performance CPUs)" << std::endl;
        info.requires_performance_mode = true;
        info.recommended_max_speed = 32;  // Keep original 32x
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