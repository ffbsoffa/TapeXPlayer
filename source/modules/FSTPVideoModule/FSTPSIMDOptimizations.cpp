#include "FSTPSIMDOptimizations.h"
#include "FSTPHardwareDetection.h"  // NEW: Use unified hardware detection
#include <iostream>
#include <algorithm>

extern "C" {
#include <libavutil/opt.h>
}

namespace FSTP {

// Static member initialization
bool SIMDOptimizations::sse2_available = false;
bool SIMDOptimizations::sse41_available = false;
bool SIMDOptimizations::avx_available = false;
bool SIMDOptimizations::avx2_available = false;
bool SIMDOptimizations::avx512_available = false;
bool SIMDOptimizations::features_detected = false;

void SIMDOptimizations::DetectCPUFeatures() {
    if (features_detected) return;

    // ========================================
    // UNIFIED HARDWARE DETECTION (NEW!)
    // ========================================
    // Use centralized FSTPHardwareDetection instead of FFmpeg av_get_cpu_flags()

    if (g_hardware_detection) {
        const FSTPCPUInfo& cpu_info = g_hardware_detection->GetCPUInfo();

        // Map CPU info to SIMD flags
        sse2_available = cpu_info.has_sse2;
        sse41_available = cpu_info.has_sse4_2;  // Note: SSE4.2 includes SSE4.1
        avx_available = cpu_info.has_avx;
        avx2_available = cpu_info.has_avx2;

        // FORCE DISABLE AVX-512 due to heap corruption on Intel CPUs
        // AVX-512 requires 64-byte alignment which causes issues with AVFrame allocation
        avx512_available = false;

        features_detected = true;

        // Log detected features for debugging
        if (cpu_info.is_apple_silicon) {
            // Apple Silicon - uses NEON
            std::cout << "🔧 [SIMD] CPU Features detected (Apple Silicon ARM64):" << std::endl;
            std::cout << "   ARM NEON: ✅ (always available)" << std::endl;
            std::cout << "   Optimal SIMD Level: NEON" << std::endl;
        } else if (cpu_info.architecture == "x86_64") {
            // x86_64 Intel/AMD
            std::cout << "🔧 [SIMD] CPU Features detected (x86_64):" << std::endl;
            std::cout << "   SSE2: " << (sse2_available ? "✅" : "❌") << std::endl;
            std::cout << "   SSE4.1: " << (sse41_available ? "✅" : "❌") << std::endl;
            std::cout << "   AVX: " << (avx_available ? "✅" : "❌") << std::endl;
            std::cout << "   AVX2: " << (avx2_available ? "✅" : "❌") << std::endl;
            std::cout << "   AVX-512: ❌ (DISABLED - stability)" << std::endl;
            std::cout << "   Optimal SIMD Level: " << GetOptimalSIMDLevel() << std::endl;
        } else {
            // Other architectures
            std::cout << "🔧 [SIMD] CPU Features detected (" << cpu_info.architecture << "):" << std::endl;
            std::cout << "   Optimal SIMD Level: " << GetOptimalSIMDLevel() << std::endl;
        }
    } else {
        // Fallback: g_hardware_detection not initialized
        std::cerr << "⚠️  [SIMD] g_hardware_detection not initialized - using defaults" << std::endl;

        // Defaults: assume basic SIMD support on x86_64
#ifdef FSTP_SIMD_X86
        sse2_available = true;   // SSE2 is baseline for x86_64
        sse41_available = false;
        avx_available = false;
        avx2_available = false;
        avx512_available = false;
#else
        sse2_available = false;
        sse41_available = false;
        avx_available = false;
        avx2_available = false;
        avx512_available = false;
#endif
        features_detected = true;
    }
}

void SIMDOptimizations::OptimizeFrameAlignment(AVFrame* frame) {
    if (!frame || !frame->data[0]) return;
    
    // Intel Celeron + PowerSaver optimization: ensure proper alignment for SIMD
    const int SIMD_ALIGNMENT = 16; // SSE2 requires 16-byte alignment
    
    // Check if frame data is properly aligned
    bool needs_alignment = false;
    
    for (int i = 0; i < 3; ++i) {
        if (frame->data[i] && (reinterpret_cast<uintptr_t>(frame->data[i]) % SIMD_ALIGNMENT != 0)) {
            needs_alignment = true;
            break;
        }
    }
    
    if (needs_alignment && HasSSE2()) {
        // Reallocate frame with proper alignment
        int ret = av_frame_get_buffer(frame, SIMD_ALIGNMENT);
        if (ret >= 0) {
            std::cout << "🔧 [SIMD] Frame realigned for optimal SIMD performance" << std::endl;
        }
    }
}

void SIMDOptimizations::OptimizeLinesizeAlignment(uint8_t* data, int width, int height, int linesize, int alignment) {
    if (!data || width <= 0 || height <= 0 || linesize <= 0) return;
    
    // Ensure linesize is aligned for SIMD operations
    if (linesize % alignment != 0 && HasSSE2()) {
        // Pad the end of each line with zeros for SIMD safety
        int padding = alignment - (width % alignment);
        if (padding != alignment) {
            for (int y = 0; y < height; ++y) {
                uint8_t* row = data + y * linesize;
                FastMemset(row + width, 0, padding);
            }
        }
    }
}

void SIMDOptimizations::ProcessYUV420P_SSE2(uint8_t* y_plane, uint8_t* u_plane, uint8_t* v_plane,
                                           int width, int height, int y_stride, int uv_stride) {
    if (!HasSSE2() || !y_plane || !u_plane || !v_plane) return;
    
    // Process Y plane with SSE2
    ProcessPlane_SSE2(y_plane, width, height, y_stride);
    
    // Process U and V planes (half resolution)
    ProcessPlane_SSE2(u_plane, width / 2, height / 2, uv_stride);
    ProcessPlane_SSE2(v_plane, width / 2, height / 2, uv_stride);
}

void SIMDOptimizations::ProcessYUV420P_AVX2(uint8_t* y_plane, uint8_t* u_plane, uint8_t* v_plane,
                                           int width, int height, int y_stride, int uv_stride) {
    if (!HasAVX2() || !y_plane || !u_plane || !v_plane) return;
    
    // Process Y plane with AVX2 (2x faster than SSE2)
    ProcessPlane_AVX2(y_plane, width, height, y_stride);
    
    // Process U and V planes (half resolution)
    ProcessPlane_AVX2(u_plane, width / 2, height / 2, uv_stride);
    ProcessPlane_AVX2(v_plane, width / 2, height / 2, uv_stride);
}

void SIMDOptimizations::ProcessYUV420P_AVX512(uint8_t* y_plane, uint8_t* u_plane, uint8_t* v_plane,
                                             int width, int height, int y_stride, int uv_stride) {
    // AVX-512 DISABLED due to heap corruption issues on Intel CPUs
    (void)y_plane; (void)u_plane; (void)v_plane;
    (void)width; (void)height; (void)y_stride; (void)uv_stride;
    return;
}

void SIMDOptimizations::ProcessNV12_SSE2(uint8_t* y_plane, uint8_t* uv_plane,
                                        int width, int height, int y_stride, int uv_stride) {
    if (!HasSSE2() || !y_plane || !uv_plane) return;
    
    // Process Y plane with SSE2
    ProcessPlane_SSE2(y_plane, width, height, y_stride);
    
    // Process UV plane (interleaved, half resolution)
    ProcessPlane_SSE2(uv_plane, width, height / 2, uv_stride);
}

void SIMDOptimizations::ProcessNV12_AVX2(uint8_t* y_plane, uint8_t* uv_plane,
                                        int width, int height, int y_stride, int uv_stride) {
    if (!HasAVX2() || !y_plane || !uv_plane) return;
    
    // Process Y plane with AVX2 (2x faster than SSE2)
    ProcessPlane_AVX2(y_plane, width, height, y_stride);
    
    // Process UV plane (interleaved, half resolution)
    ProcessPlane_AVX2(uv_plane, width, height / 2, uv_stride);
}

void SIMDOptimizations::ProcessNV12_AVX512(uint8_t* y_plane, uint8_t* uv_plane,
                                          int width, int height, int y_stride, int uv_stride) {
    // AVX-512 DISABLED due to heap corruption issues on Intel CPUs
    (void)y_plane; (void)uv_plane;
    (void)width; (void)height; (void)y_stride; (void)uv_stride;
    return;
}

void SIMDOptimizations::YUV420P_to_RGB24_SSE2(const uint8_t* y, const uint8_t* u, const uint8_t* v,
                                             uint8_t* rgb, int width, int height,
                                             int y_stride, int uv_stride, int rgb_stride) {
    // Suppress unused parameter warnings
    (void)y; (void)u; (void)v; (void)rgb; (void)width; (void)height;
    (void)y_stride; (void)uv_stride; (void)rgb_stride;

#ifdef FSTP_SIMD_X86
    if (!HasSSE2() || !y || !u || !v || !rgb) return;

    // Suppress unused parameter warnings (u, v, uv_stride not used in simplified implementation)
    (void)u; (void)v; (void)uv_stride;

    // Simplified YUV to RGB conversion using SSE2
    // This is a basic implementation - full conversion would be more complex

    for (int row = 0; row < height; ++row) {
        for (int col = 0; col < width - 15; col += 16) {
            // Load Y values
            __m128i y_data = _mm_loadu_si128(reinterpret_cast<const __m128i*>(y + row * y_stride + col));

            // Convert to RGB (simplified - just copy Y values as grayscale)
            __m128i rgb_data = _mm_unpacklo_epi8(y_data, y_data);

            // Store RGB values
            _mm_storeu_si128(reinterpret_cast<__m128i*>(rgb + row * rgb_stride + col * 3), rgb_data);
        }
    }
#else
    // Not implemented for non-x86 platforms yet
    return;
#endif
}

void SIMDOptimizations::SanitizeFrameData(AVFrame* frame) {
    if (!frame || !frame->data[0]) return;
    
    // Sanitize frame data to prevent artifacts
    if (HasSSE2()) {
        // Process each plane
        for (int plane = 0; plane < 3; ++plane) {
            if (frame->data[plane] && frame->linesize[plane] > 0) {
                int width = (plane == 0) ? frame->width : frame->width / 2;
                int height = (plane == 0) ? frame->height : frame->height / 2;
                
                ProcessPlane_SSE2(frame->data[plane], width, height, frame->linesize[plane]);
            }
        }
    }
}

void SIMDOptimizations::AlignMemory_SSE2(void* ptr, size_t size, int alignment) {
    // Suppress unused parameter warnings
    (void)ptr; (void)size; (void)alignment;

#ifdef FSTP_SIMD_X86
    if (!HasSSE2()) return;

    // Ensure memory is aligned for SIMD operations
    uint8_t* ptr_u8 = static_cast<uint8_t*>(ptr);
    size_t aligned_size = (size / alignment) * alignment;

    // Process aligned portion with SSE2
    for (size_t i = 0; i < aligned_size; i += 16) {
        __m128i data = _mm_loadu_si128(reinterpret_cast<__m128i*>(ptr_u8 + i));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(ptr_u8 + i), data);
    }
#else
    // Not implemented for non-x86 platforms
    return;
#endif
}

void SIMDOptimizations::ProcessPlane_SSE2(uint8_t* plane, int width, int height, int stride) {
    if (!HasSSE2() || !plane || width <= 0 || height <= 0 || stride <= 0) return;

    // DISABLED: This function was modifying frame data, causing visual artifacts
    // These functions should ONLY be used for specific processing tasks (noise reduction, etc.)
    // NOT for regular frame decoding/playback
    //
    // Keeping function signature for API compatibility but doing nothing
    (void)plane; (void)width; (void)height; (void)stride;
    return;

    /* ORIGINAL CODE - DISABLED
    // Process plane with SSE2 optimizations
    for (int y = 0; y < height; ++y) {
        uint8_t* row = plane + y * stride;

        // Process 16 bytes at a time
        for (int x = 0; x < width - 15; x += 16) {
            __m128i data = _mm_loadu_si128(reinterpret_cast<__m128i*>(row + x));

            // Apply some basic processing (e.g., noise reduction)
            // This is a placeholder - real processing would depend on requirements

            _mm_storeu_si128(reinterpret_cast<__m128i*>(row + x), data);
        }

        // Process remaining bytes
        for (int x = (width / 16) * 16; x < width; ++x) {
            // Basic processing for remaining bytes
            row[x] = row[x]; // Placeholder
        }
    }
    */
}

void SIMDOptimizations::ProcessPlane_AVX2(uint8_t* plane, int width, int height, int stride) {
    if (!HasAVX2() || !plane || width <= 0 || height <= 0 || stride <= 0) return;

    // DISABLED: This function was modifying frame data, causing visual artifacts
    // These functions should ONLY be used for specific processing tasks (noise reduction, etc.)
    // NOT for regular frame decoding/playback
    //
    // Keeping function signature for API compatibility but doing nothing
    (void)plane; (void)width; (void)height; (void)stride;
    return;

    /* ORIGINAL CODE - DISABLED
    // Process plane with AVX2 optimizations (2x faster than SSE2)
    for (int y = 0; y < height; ++y) {
        uint8_t* row = plane + y * stride;

        // Process 32 bytes at a time using AVX2
        for (int x = 0; x < width - 31; x += 32) {
            __m256i data = _mm256_loadu_si256(reinterpret_cast<__m256i*>(row + x));

            // Apply some basic processing (e.g., noise reduction)
            // This is a placeholder - real processing would depend on requirements

            _mm256_storeu_si256(reinterpret_cast<__m256i*>(row + x), data);
        }

        // Process remaining bytes with SSE2 if needed
        int remaining = width - ((width / 32) * 32);
        if (remaining >= 16 && HasSSE2()) {
            int x = (width / 32) * 32;
            __m128i data = _mm_loadu_si128(reinterpret_cast<__m128i*>(row + x));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(row + x), data);
            remaining -= 16;
            x += 16;
        }

        // Process final remaining bytes
        for (int x = width - remaining; x < width; ++x) {
            // Basic processing for remaining bytes
            row[x] = row[x]; // Placeholder
        }
    }
    */
}

void SIMDOptimizations::ProcessPlane_AVX512(uint8_t* plane, int width, int height, int stride) {
    // AVX-512 DISABLED due to heap corruption issues on Intel CPUs
    // Function kept for API compatibility but does nothing
    (void)plane; (void)width; (void)height; (void)stride;
    return;
}

} // namespace FSTP
