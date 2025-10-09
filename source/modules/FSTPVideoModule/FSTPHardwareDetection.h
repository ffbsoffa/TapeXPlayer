#ifndef FSTPHARDWAREDETECTION_H
#define FSTPHARDWAREDETECTION_H

#include <string>
#include <vector>
#include <map>

// Platform detection
#ifdef _WIN32
    #define PLATFORM_WINDOWS
#elif defined(__linux__)
    #define PLATFORM_LINUX
#elif defined(__APPLE__)
    #define PLATFORM_MACOS
#endif

enum class FSTPHWAccelType {
    NONE = 0,
    VIDEOTOOLBOX,   // macOS
    QSV_INTEL,      // Intel Quick Sync Video
    NVENC_NVIDIA,   // NVIDIA NVENC
    AMF_AMD,        // AMD Advanced Media Framework
    VAAPI,          // Linux VA-API
    DXVA2,          // Windows DirectX Video Acceleration 2.0
    D3D11VA,        // Windows Direct3D 11
    OPENCL,         // OpenCL acceleration
    VULKAN          // Vulkan compute
};

enum class FSTPCodecSupport {
    NONE = 0,
    H264_DECODE = 1 << 0,
    H264_ENCODE = 1 << 1,
    H265_DECODE = 1 << 2,
    H265_ENCODE = 1 << 3,
    VP9_DECODE  = 1 << 4,
    VP9_ENCODE  = 1 << 5,
    AV1_DECODE  = 1 << 6,
    AV1_ENCODE  = 1 << 7,
    MPEG2_DECODE = 1 << 8,
    MPEG4_DECODE = 1 << 9
};

inline FSTPCodecSupport operator|(FSTPCodecSupport a, FSTPCodecSupport b) {
    return static_cast<FSTPCodecSupport>(static_cast<int>(a) | static_cast<int>(b));
}

inline FSTPCodecSupport operator&(FSTPCodecSupport a, FSTPCodecSupport b) {
    return static_cast<FSTPCodecSupport>(static_cast<int>(a) & static_cast<int>(b));
}

struct FSTPHardwareInfo {
    FSTPHWAccelType accel_type;
    std::string device_name;
    std::string driver_version;
    FSTPCodecSupport supported_codecs;
    int max_decode_width;
    int max_decode_height;
    int max_encode_width;
    int max_encode_height;
    bool supports_10bit;
    bool supports_444;
    int memory_mb;
    double performance_score;  // Relative performance score
};

class FSTPHardwareDetection {
public:
    FSTPHardwareDetection();
    ~FSTPHardwareDetection();

    // Main detection method - starts when program starts
    bool DetectAllHardware();

    // Getting results
    const std::vector<FSTPHardwareInfo>& GetDetectedHardware() const;
    FSTPHardwareInfo GetBestDecoder() const;
    FSTPHardwareInfo GetBestEncoder() const;

    // Check support for specific codec
    bool SupportsCodec(FSTPHWAccelType accel, FSTPCodecSupport codec) const;

    // Getting list of supported accelerations for specific codec
    std::vector<FSTPHWAccelType> GetAccelerationsForCodec(FSTPCodecSupport codec) const;

    // Utilities
    static std::string AccelTypeToString(FSTPHWAccelType type);
    static std::string CodecSupportToString(FSTPCodecSupport support);

    // Debug information
    void PrintDetectedHardware() const;

private:
    std::vector<FSTPHardwareInfo> detected_hardware_;

    // Platform-specific detection methods
#ifdef PLATFORM_MACOS
    bool DetectVideoToolbox();
    bool DetectMetalPerformanceShaders();
#endif

#ifdef PLATFORM_WINDOWS
    bool DetectDXVA2();
    bool DetectD3D11VA();
    bool DetectIntelQSV();
    bool DetectNVIDIANVENC();
    bool DetectAMDAdvancedMediaFramework();
#endif

#ifdef PLATFORM_LINUX
    bool DetectVAAPI();
    bool DetectVDPAU();
    bool DetectIntelQSVLinux();
#endif

    // Common methods
    bool DetectOpenCL();
    bool DetectVulkan();
    bool TestFFmpegHWAccel(FSTPHWAccelType type);

    // Utilities
    double CalculatePerformanceScore(const FSTPHardwareInfo& info) const;
    bool TestDecodeCapability(FSTPHWAccelType type, FSTPCodecSupport codec) const;
};

// Global variable for access to detection results
extern FSTPHardwareDetection* g_hardware_detection;

// Functions for initialization in main()
bool InitializeHardwareDetection();
void CleanupHardwareDetection();

#endif // FSTPHARDWAREDETECTION_H