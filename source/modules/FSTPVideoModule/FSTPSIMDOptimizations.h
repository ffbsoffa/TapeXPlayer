#pragma once

// Platform-specific SIMD headers
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    // x86/x64 architecture - use Intel intrinsics
    #define FSTP_SIMD_X86
    #include <immintrin.h>
#elif defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
    // ARM64 architecture - use NEON intrinsics
    #define FSTP_SIMD_ARM_NEON
    #include <arm_neon.h>
#else
    // No SIMD support
    #define FSTP_SIMD_NONE
#endif

#include <cstring>
#include <cstdint>
#include <algorithm>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace FSTP {

// SIMD Optimization utilities for Intel Celeron + PowerSaver
// Provides efficient vectorized operations for video frame processing

class SIMDOptimizations {
public:
    // CPU feature detection
    static bool HasSSE2();
    static bool HasSSE41();
    static bool HasAVX();
    static bool HasAVX2();
    static bool HasAVX512();
    
    // Get optimal SIMD level for current CPU
    static int GetOptimalSIMDLevel();
    
    // Memory operations
    static void FastMemcpy(void* dest, const void* src, size_t size);
    static void FastMemset(void* ptr, int value, size_t size);
    
    // Frame processing optimizations
    static void OptimizeFrameAlignment(AVFrame* frame);
    static void OptimizeLinesizeAlignment(uint8_t* data, int width, int height, int linesize, int alignment);
    
    // YUV processing optimizations
    static void ProcessYUV420P_SSE2(uint8_t* y_plane, uint8_t* u_plane, uint8_t* v_plane,
                                   int width, int height, int y_stride, int uv_stride);
    static void ProcessYUV420P_AVX2(uint8_t* y_plane, uint8_t* u_plane, uint8_t* v_plane,
                                   int width, int height, int y_stride, int uv_stride);
    static void ProcessYUV420P_AVX512(uint8_t* y_plane, uint8_t* u_plane, uint8_t* v_plane,
                                     int width, int height, int y_stride, int uv_stride);
    static void ProcessNV12_SSE2(uint8_t* y_plane, uint8_t* uv_plane,
                                int width, int height, int y_stride, int uv_stride);
    static void ProcessNV12_AVX2(uint8_t* y_plane, uint8_t* uv_plane,
                                int width, int height, int y_stride, int uv_stride);
    static void ProcessNV12_AVX512(uint8_t* y_plane, uint8_t* uv_plane,
                                 int width, int height, int y_stride, int uv_stride);
    
    // Color space conversions
    static void YUV420P_to_RGB24_SSE2(const uint8_t* y, const uint8_t* u, const uint8_t* v,
                                     uint8_t* rgb, int width, int height,
                                     int y_stride, int uv_stride, int rgb_stride);
    
    // Frame validation and safety checks
    static bool ValidateFrameData(const AVFrame* frame);
    static void SanitizeFrameData(AVFrame* frame);
    
private:
    // Internal helper functions
    static void AlignMemory_SSE2(void* ptr, size_t size, int alignment);
    static void ProcessPlane_SSE2(uint8_t* plane, int width, int height, int stride);
    static void ProcessPlane_AVX2(uint8_t* plane, int width, int height, int stride);
    static void ProcessPlane_AVX512(uint8_t* plane, int width, int height, int stride);
    
    // CPU feature flags (cached)
    static bool sse2_available;
    static bool sse41_available;
    static bool avx_available;
    static bool avx2_available;
    static bool avx512_available;
    static bool features_detected;
    
    static void DetectCPUFeatures();
};

// Inline implementations for performance-critical functions
inline bool SIMDOptimizations::HasSSE2() {
    if (!features_detected) DetectCPUFeatures();
    return sse2_available;
}

inline bool SIMDOptimizations::HasSSE41() {
    if (!features_detected) DetectCPUFeatures();
    return sse41_available;
}

inline bool SIMDOptimizations::HasAVX() {
    if (!features_detected) DetectCPUFeatures();
    return avx_available;
}

inline bool SIMDOptimizations::HasAVX2() {
    if (!features_detected) DetectCPUFeatures();
    return avx2_available;
}

inline bool SIMDOptimizations::HasAVX512() {
    if (!features_detected) DetectCPUFeatures();
    return avx512_available;
}

inline int SIMDOptimizations::GetOptimalSIMDLevel() {
    if (!features_detected) DetectCPUFeatures();
    
    if (avx512_available) return 5; // AVX-512
    if (avx2_available) return 4;  // AVX2
    if (avx_available) return 3;   // AVX
    if (sse41_available) return 2; // SSE4.1
    if (sse2_available) return 1;  // SSE2
    return 0; // No SIMD
}

    // Fast memory operations using SIMD with AVX2/NEON optimization
    // AVX512 removed due to heap corruption issues on Intel CPUs
#ifdef FSTP_SIMD_X86
__attribute__((target("avx2")))
#endif
inline void SIMDOptimizations::FastMemcpy(void* dest, const void* src, size_t size) {
    if (size == 0) return;

    const uint8_t* src_ptr = static_cast<const uint8_t*>(src);
    uint8_t* dest_ptr = static_cast<uint8_t*>(dest);

#ifdef FSTP_SIMD_X86
    // Use AVX2 for optimal performance (AVX512 disabled for stability)
    if (size >= 128 && HasAVX2()) {
        // 32-byte aligned copy using AVX2 (2x faster than SSE2)
        size_t avx2_size = (size / 32) * 32;
        for (size_t i = 0; i < avx2_size; i += 32) {
            __m256i data = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src_ptr + i));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(dest_ptr + i), data);
        }

        // Copy remaining bytes
        for (size_t i = avx2_size; i < size; ++i) {
            dest_ptr[i] = src_ptr[i];
        }
    } else if (size >= 64 && HasSSE2()) {
        // 16-byte aligned copy using SSE2
        size_t sse2_size = (size / 16) * 16;
        for (size_t i = 0; i < sse2_size; i += 16) {
            __m128i data = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src_ptr + i));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dest_ptr + i), data);
        }

        // Copy remaining bytes
        for (size_t i = sse2_size; i < size; ++i) {
            dest_ptr[i] = src_ptr[i];
        }
    } else {
        // Fallback to standard memcpy for small blocks
        std::memcpy(dest, src, size);
    }
#elif defined(FSTP_SIMD_ARM_NEON)
    // Use ARM NEON for 16-byte aligned operations
    if (size >= 64) {
        size_t neon_size = (size / 16) * 16;
        for (size_t i = 0; i < neon_size; i += 16) {
            uint8x16_t data = vld1q_u8(src_ptr + i);
            vst1q_u8(dest_ptr + i, data);
        }

        // Copy remaining bytes
        for (size_t i = neon_size; i < size; ++i) {
            dest_ptr[i] = src_ptr[i];
        }
    } else {
        // Fallback to standard memcpy for small blocks
        std::memcpy(dest, src, size);
    }
#else
    // No SIMD - use standard memcpy
    std::memcpy(dest, src, size);
#endif
}

#ifdef FSTP_SIMD_X86
__attribute__((target("avx2")))
#endif
inline void SIMDOptimizations::FastMemset(void* ptr, int value, size_t size) {
    if (size == 0) return;

    uint8_t* ptr_u8 = static_cast<uint8_t*>(ptr);
    uint8_t val_u8 = static_cast<uint8_t>(value);

#ifdef FSTP_SIMD_X86
    // Use AVX2 for optimal performance (AVX512 disabled for stability)
    if (size >= 128 && HasAVX2()) {
        // Create 32-byte pattern using AVX2 (2x faster than SSE2)
        __m256i pattern = _mm256_set1_epi8(val_u8);

        // Set 32 bytes at a time
        size_t avx2_size = (size / 32) * 32;
        for (size_t i = 0; i < avx2_size; i += 32) {
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(ptr_u8 + i), pattern);
        }

        // Set remaining bytes
        for (size_t i = avx2_size; i < size; ++i) {
            ptr_u8[i] = val_u8;
        }
    } else if (size >= 64 && HasSSE2()) {
        // Create 16-byte pattern using SSE2
        __m128i pattern = _mm_set1_epi8(val_u8);

        // Set 16 bytes at a time
        size_t sse2_size = (size / 16) * 16;
        for (size_t i = 0; i < sse2_size; i += 16) {
            _mm_storeu_si128(reinterpret_cast<__m128i*>(ptr_u8 + i), pattern);
        }

        // Set remaining bytes
        for (size_t i = sse2_size; i < size; ++i) {
            ptr_u8[i] = val_u8;
        }
    } else {
        // Fallback to standard memset for small blocks
        std::memset(ptr, value, size);
    }
#elif defined(FSTP_SIMD_ARM_NEON)
    // Use ARM NEON for 16-byte aligned operations
    if (size >= 64) {
        // Create 16-byte pattern using NEON
        uint8x16_t pattern = vdupq_n_u8(val_u8);

        // Set 16 bytes at a time
        size_t neon_size = (size / 16) * 16;
        for (size_t i = 0; i < neon_size; i += 16) {
            vst1q_u8(ptr_u8 + i, pattern);
        }

        // Set remaining bytes
        for (size_t i = neon_size; i < size; ++i) {
            ptr_u8[i] = val_u8;
        }
    } else {
        // Fallback to standard memset for small blocks
        std::memset(ptr, value, size);
    }
#else
    // No SIMD - use standard memset
    std::memset(ptr, value, size);
#endif
}

// Frame validation with SIMD acceleration
#ifdef FSTP_SIMD_X86
__attribute__((target("sse2")))
#endif
inline bool SIMDOptimizations::ValidateFrameData(const AVFrame* frame) {
    if (!frame || !frame->data[0]) return false;

    // Check basic properties
    if (frame->width <= 0 || frame->height <= 0) return false;
    if (frame->linesize[0] <= 0) return false;

#ifdef FSTP_SIMD_X86
    // Use SSE2 to quickly check for valid data patterns
    if (HasSSE2() && frame->width >= 16) {
        const uint8_t* y_plane = frame->data[0];
        int width = frame->width;
        int height = frame->height;
        int stride = frame->linesize[0];

        // Check first few rows for valid Y values (0-255)
        for (int y = 0; y < (4 < height ? 4 : height); ++y) {
            const uint8_t* row = y_plane + y * stride;

            // Process 16 bytes at a time
            for (int x = 0; x < width - 15; x += 16) {
                __m128i data = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row + x));

                // Check if all values are valid (0-255) - this is always true for uint8_t
                // But we can check for completely zero or completely 255 patterns (suspicious)
                __m128i zero = _mm_setzero_si128();
                __m128i ones = _mm_set1_epi8(0xFF);

                __m128i is_zero = _mm_cmpeq_epi8(data, zero);
                __m128i is_ones = _mm_cmpeq_epi8(data, ones);

                // If entire block is zero or 255, it might be uninitialized
                int zero_count = _mm_movemask_epi8(is_zero);
                int ones_count = _mm_movemask_epi8(is_ones);

                if (zero_count == 0xFFFF || ones_count == 0xFFFF) {
                    // Suspicious pattern - might be uninitialized data
                    return false;
                }
            }
        }
    }
#elif defined(FSTP_SIMD_ARM_NEON)
    // Use NEON to quickly check for valid data patterns
    if (frame->width >= 16) {
        const uint8_t* y_plane = frame->data[0];
        int width = frame->width;
        int height = frame->height;
        int stride = frame->linesize[0];

        // Check first few rows for valid Y values (0-255)
        for (int y = 0; y < (4 < height ? 4 : height); ++y) {
            const uint8_t* row = y_plane + y * stride;

            // Process 16 bytes at a time
            for (int x = 0; x < width - 15; x += 16) {
                uint8x16_t data = vld1q_u8(row + x);

                // Check for completely zero or completely 255 patterns (suspicious)
                uint8x16_t zero = vdupq_n_u8(0);
                uint8x16_t ones = vdupq_n_u8(0xFF);

                uint8x16_t is_zero = vceqq_u8(data, zero);
                uint8x16_t is_ones = vceqq_u8(data, ones);

                // Check if all lanes match (all true)
                uint64x2_t is_zero_64 = vreinterpretq_u64_u8(is_zero);
                uint64x2_t is_ones_64 = vreinterpretq_u64_u8(is_ones);

                if ((vgetq_lane_u64(is_zero_64, 0) == ~0ULL && vgetq_lane_u64(is_zero_64, 1) == ~0ULL) ||
                    (vgetq_lane_u64(is_ones_64, 0) == ~0ULL && vgetq_lane_u64(is_ones_64, 1) == ~0ULL)) {
                    // Suspicious pattern - might be uninitialized data
                    return false;
                }
            }
        }
    }
#endif

    return true;
}

} // namespace FSTP
