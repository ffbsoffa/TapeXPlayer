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

// ============================================================
// Decoder Profile — три профиля, откалиброванных на реальных устройствах:
//
//   FULL_HARDWARE  → Apple M1 Mac (2020+)
//                    VideoToolbox отлично работает для всех разрешений.
//                    Измерение: hw.optional.arm64 != 0 (sysctl macOS)
//
//   HYBRID_VT_CPU  → MacBook Pro 2016 Intel (Skylake i7-6xxx) — тест пользователя
//                    VideoToolbox для полного разрешения + CPU для прокси ≤640p.
//                    Измерение: Intel Mac год 2012+, или Intel gen ≥ 5 (Broadwell+)
//                    Граница снизу: 2011 MacBook Pro (Sandy Bridge, gen 2) = граница поддержки
//                    Минимальный надёжный H.264 HW: 2015 Mac (Broadwell, gen 5)
//
//   MINIMUM        → Intel Celeron Gold 7505 (Tiger Lake 2-core, 15W) — тест пользователя
//                    QSV-decode + прокси 360p + ограничение 24× скорости.
//                    Измерение (первичное):  бренд содержит "Celeron"/"Pentium"/"Atom"
//                    Измерение (вторичное):  ≤2 физических ядра на x86
//                    Экстраполяция: более ранние 2-ядерные Core i3/i5 имеют аналогичную
//                    пропускную способность декодера (i3-6006U 2016, i5-5300U 2015 и т.п.)
// ============================================================
enum class FSTPDecoderProfile {
    UNKNOWN       = 0,
    FULL_HARDWARE = 1,   // Apple Silicon: VideoToolbox/Metal для всего
    HYBRID_VT_CPU = 2,   // Intel Mac 2012+ или способный x86: HW для full-res, CPU для прокси
    MINIMUM       = 3,   // Celeron/Pentium/Atom или ≤2-ядерный x86: QSV, 360p, лимит 24×
    SOFTWARE_ONLY = 4    // Нет жизнеспособного HW-ускорения: чистый CPU decode
};

// CPU capabilities and performance profile
struct FSTPCPUInfo {
    // === Basic Information ===
    std::string model_name;          // "Intel(R) Core(TM) i7-7820HQ CPU @ 2.90GHz"
    std::string vendor;              // "GenuineIntel", "AuthenticAMD", "Apple"
    std::string architecture;        // "x86_64", "arm64", "aarch64"
    int physical_cores;
    int logical_cores;
    double base_frequency_ghz;
    double max_frequency_ghz;

    // === SIMD Capabilities ===
    bool has_sse2;
    bool has_sse4_2;
    bool has_avx;
    bool has_avx2;
    bool has_avx512;                 // FORCE DISABLED (heap corruption risk)

    // === Power Management ===
    std::string power_governor;      // "powersave", "performance", "schedutil" (Linux)
    bool requires_performance_mode;  // true for Pentium 7505, etc.

    // === Performance Profile ===
    int recommended_max_speed;       // 24 for Pentium 7505, 32 for others
    int optimal_segment_size;        // 500 frames (default)
    int optimal_preload_count;       // 8 segments
    double decode_throughput_fps;    // Estimated throughput (800-4000 fps)

    // === Proxy Optimization ===
    bool requires_half_fps_for_50_60;  // true for Pentium 7505 with QSV (50/60fps → 25/30fps)
    bool is_pentium_gold_7505;         // true for Intel Pentium Gold 7505 specifically

    // === Device Type ===
    bool is_compact_device;          // true for GPD Pocket, etc.
    std::string device_model;        // "GPD Pocket 3", "" if not compact

    // === macOS Specific ===
    bool is_apple_silicon;           // true for M1/M2/M3/M4
    std::string mac_model;           // "MacBookPro14,3" (via hw.model)
    int mac_year;                    // 2016, 2020, etc. (корректно распарсен из модели)

    // === Intel CPU Generation (кросс-платформенно) ===
    // Извлекается из строки бренда: "Core i5-6267U" → 6, "Core i7-8750H" → 8
    // Используется для определения поддержки H.264 HW decode:
    //   gen 5+ (Broadwell, 2015) = минимум для надёжного VideoToolbox/QSV на H.264
    //   gen 3-4 (Ivy Bridge/Haswell, 2012-2014) = ограниченный HW decode
    //   gen 1-2 (Sandy Bridge, 2011) = очень ограниченный, граница поддержки
    int intel_generation;

    // === Decoder Profile (определяется после полного обнаружения оборудования) ===
    FSTPDecoderProfile decoder_profile;

    // === Helper Methods ===
    bool IsLowPowerCPU() const {
        return model_name.find("Pentium") != std::string::npos ||
               model_name.find("Celeron") != std::string::npos ||
               model_name.find("Atom") != std::string::npos;
    }

    // Возвращает true для MINIMUM-класса CPU:
    //   Intel — первичный критерий:  бренд Celeron / Pentium / Atom
    //   AMD   — первичный критерий:  "Athlon Silver" / "Athlon Gold"
    //             (AMD-эквивалент Intel Pentium/Celeron: 2–4 ядра, 6–15 W, APU)
    //   Вторичный критерий:  ≤2 физических ядра x86
    //             (i3-6006U 2016, i5-5300U 2015, Athlon Silver 3050e — аналог Celeron 7505)
    bool IsMinimumClass() const {
        // Intel low-end
        if (model_name.find("Celeron") != std::string::npos ||
            model_name.find("Pentium") != std::string::npos ||
            model_name.find("Atom")    != std::string::npos) return true;
        // AMD low-end: Athlon Silver/Gold = AMD-эквивалент Intel Pentium/Celeron
        // "Athlon Silver 3050e" (2C, 6W), "Athlon Gold 3150U" (2C, 15W),
        // "Athlon Gold Pro 3150GE" (4C, 35W) — все MINIMUM по пропускной способности
        if (model_name.find("Athlon Silver") != std::string::npos ||
            model_name.find("Athlon Gold")   != std::string::npos) return true;
        // Вторичный: ≤2 физических ядра x86 (не Apple Silicon)
        if (!is_apple_silicon && physical_cores <= 2 &&
            (architecture == "x86_64" || architecture.find("i686") != std::string::npos))
            return true;
        return false;
    }

    // Возвращает true для любого Intel Mac (2011–2021, до Apple Silicon)
    bool IsIntelMac() const {
        return !is_apple_silicon && !mac_model.empty();
    }

    bool IsOldIntelMac() const {
        // Обратная совместимость: 2016-2017 MacBook Pro с плохим VideoToolbox на прокси
        return mac_model.find("MacBookPro13") != std::string::npos ||
               mac_model.find("MacBookPro14") != std::string::npos ||
               (mac_year >= 2016 && mac_year <= 2017);
    }
};

inline FSTPCodecSupport operator|(FSTPCodecSupport a, FSTPCodecSupport b) {
    return static_cast<FSTPCodecSupport>(static_cast<int>(a) | static_cast<int>(b));
}

inline FSTPCodecSupport operator&(FSTPCodecSupport a, FSTPCodecSupport b) {
    return static_cast<FSTPCodecSupport>(static_cast<int>(a) & static_cast<int>(b));
}

// GPU capabilities and hardware acceleration info
struct FSTPGPUInfo {
    // === Basic Information ===
    std::string model_name;          // "Intel HD Graphics 630", "Apple M1 GPU"
    std::string vendor;              // "Intel", "AMD", "NVIDIA", "Apple"
    int memory_mb;                   // VRAM in MB

    // === Hardware Acceleration Support ===
    bool videotoolbox_available;     // macOS only
    bool vaapi_available;            // Linux only
    bool vdpau_available;            // Linux only (legacy)
    bool dxva2_available;            // Windows only
    bool d3d11va_available;          // Windows only
    bool qsv_available;              // Intel Quick Sync Video
    bool nvenc_available;            // NVIDIA NVENC
    bool amf_available;              // AMD Advanced Media Framework

    // === VA-API devices (Linux) ===
    std::vector<std::string> vaapi_devices;  // ["/dev/dri/renderD128", ...]

    // === Performance ===
    double hw_decode_score;          // 0-100 (VideoToolbox on M1 = 95, old Intel = 30)
    bool prefer_cpu_for_lowres;      // true for Intel Mac ≤480p

    // === Supported Codecs ===
    FSTPCodecSupport supported_codecs;

    // === Helper Methods ===
    bool HasAnyHWAccel() const {
        return videotoolbox_available || vaapi_available ||
               dxva2_available || d3d11va_available ||
               qsv_available || nvenc_available || amf_available;
    }

    FSTPHWAccelType GetBestAccelType() const {
        if (videotoolbox_available) return FSTPHWAccelType::VIDEOTOOLBOX;
        if (vaapi_available) return FSTPHWAccelType::VAAPI;
        if (d3d11va_available) return FSTPHWAccelType::D3D11VA;
        if (dxva2_available) return FSTPHWAccelType::DXVA2;
        if (qsv_available) return FSTPHWAccelType::QSV_INTEL;
        return FSTPHWAccelType::NONE;
    }
};

// Decoder strategy for specific video resolution/codec
struct FSTPDecoderStrategy {
    // === Hardware Acceleration ===
    bool use_hw_accel;               // true/false
    FSTPHWAccelType hw_accel_type;   // VIDEOTOOLBOX, VAAPI, DXVA2, etc.
    std::string hw_device_path;      // "/dev/dri/renderD128" (VA-API only)

    // === Threading ===
    int thread_count;                // 1-4 (FFmpeg threads)
    int thread_type;                 // FF_THREAD_FRAME | FF_THREAD_SLICE

    // === Memory Management ===
    bool use_frame_pool;             // true (always for CPU decode)
    int pool_initial_size;           // 50 frames (default)

    // === SIMD ===
    int simd_level;                  // 0=None, 1=SSE2, 2=SSE4.1, 3=AVX, 4=AVX2, 5=NEON

    // === Codec Flags ===
    int codec_flags;                 // AV_CODEC_FLAG_LOW_DELAY, etc.
    int codec_flags2;                // AV_CODEC_FLAG2_FAST, etc.

    // === Performance Tuning ===
    int skip_frame;                  // AVDISCARD_DEFAULT (don't skip)
    int skip_idct;                   // AVDISCARD_DEFAULT
    int skip_loop_filter;            // AVDISCARD_DEFAULT

    // === Reasoning (for debugging) ===
    std::string strategy_reason;     // "Intel Mac ≤480p → CPU faster than VideoToolbox"
};

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

    // CPU detection
    FSTPCPUInfo DetectCPU();
    const FSTPCPUInfo& GetCPUInfo() const { return cpu_info_; }

    // GPU detection (NEW!)
    FSTPGPUInfo DetectGPU();
    const FSTPGPUInfo& GetGPUInfo() const { return gpu_info_; }

    // Decoder strategy (NEW!)
    FSTPDecoderStrategy GetDecoderStrategy(
        int width,
        int height,
        int codec_id,  // AVCodecID
        double playback_speed = 1.0
    ) const;

    // CPU power mode control
    bool SetPerformanceMode();  // Switch to performance mode
    bool RestorePowerMode();    // Restore original mode on exit

    // CPU-specific speed limit
    double GetMaxRecommendedSpeed() const { return cpu_info_.recommended_max_speed; }

    // Профиль декодера для текущего оборудования
    FSTPDecoderProfile GetDecoderProfile() const { return cpu_info_.decoder_profile; }

    // Device type detection
    bool IsCompactDevice() const { return cpu_info_.is_compact_device; }
    std::string GetDeviceModel() const { return cpu_info_.device_model; }

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
    FSTPCPUInfo cpu_info_;
    FSTPGPUInfo gpu_info_;  // NEW!
    std::string original_governor_;  // Store original power governor

    // Platform-specific GPU detection methods (NEW!)
    void DetectGPU_macOS(FSTPGPUInfo& info);
    void DetectGPU_Linux(FSTPGPUInfo& info);
    void DetectGPU_Windows(FSTPGPUInfo& info);

    // Platform-specific detection methods (OLD - will be refactored)
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

    // CPU detection helpers
    void DetectCPUFeatures(FSTPCPUInfo& info);
    void DetectCPUPowerMode(FSTPCPUInfo& info);
    void ApplyCPUSpecificOptimizations(FSTPCPUInfo& info);

    // Определение профиля декодера по характеристикам CPU
    // (вызывается из DetectAllHardware после заполнения cpu_info_ и gpu_info_)
    FSTPDecoderProfile DetermineDecoderProfile(const FSTPCPUInfo& cpu) const;

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