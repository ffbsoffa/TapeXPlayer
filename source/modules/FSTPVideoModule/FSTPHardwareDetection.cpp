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
    detected_hardware_.clear();
}

bool FSTPHardwareDetection::DetectAllHardware() {
    std::cout << "FSTPHardwareDetection: Starting full hardware acceleration detection..." << std::endl;

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

    if (is_apple_silicon) {
        vt_info.device_name = "Apple VideoToolbox (Apple Silicon)";
        vt_info.supported_codecs = FSTPCodecSupport::H264_DECODE | FSTPCodecSupport::H264_ENCODE |
                                  FSTPCodecSupport::H265_DECODE | FSTPCodecSupport::H265_ENCODE |
                                  FSTPCodecSupport::VP9_DECODE | FSTPCodecSupport::AV1_DECODE |
                                  FSTPCodecSupport::MPEG2_DECODE | FSTPCodecSupport::MPEG4_DECODE;
        vt_info.supports_10bit = true;
        vt_info.performance_score = 95.0;
    } else {
        vt_info.device_name = "Apple VideoToolbox (Intel)";
        vt_info.supported_codecs = FSTPCodecSupport::H264_DECODE | FSTPCodecSupport::H264_ENCODE |
                                  FSTPCodecSupport::H265_DECODE | FSTPCodecSupport::H265_ENCODE |
                                  FSTPCodecSupport::MPEG2_DECODE | FSTPCodecSupport::MPEG4_DECODE;
        vt_info.supports_10bit = false;
        vt_info.performance_score = 85.0;
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