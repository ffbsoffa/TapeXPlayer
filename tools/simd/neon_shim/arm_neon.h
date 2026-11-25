// Test-only model of the AArch64 NEON intrinsics used by FSTPSIMDOptimizations, in portable C++
// (little-endian lane order, semantics as in the Arm Architecture Reference Manual). It lets the
// NEON code paths run — and be checked for bit-exactness — on an x86 machine with no ARM compiler
// or emulator. On a real arm64 machine the test uses the system <arm_neon.h> instead.
// Add any intrinsic a new NEON kernel needs here, with its exact ARM semantics.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
struct int16x8_t { int16_t v[8]; };
struct uint16x8_t { uint16_t v[8]; };
struct int32x4_t { int32_t v[4]; };
struct int64x2_t { int64_t v[2]; };
struct uint8x8_t { uint8_t v[8]; };
struct uint8x8x2_t { uint8x8_t val[2]; };
template <class D, class S> static inline D re(const S& s) { static_assert(sizeof(D) == sizeof(S)); D d; std::memcpy(&d, &s, sizeof d); return d; }
static inline int16x8_t vreinterpretq_s16_u16(uint16x8_t a) { return re<int16x8_t>(a); }
static inline int32x4_t vreinterpretq_s32_s16(int16x8_t a) { return re<int32x4_t>(a); }
static inline int64x2_t vreinterpretq_s64_s32(int32x4_t a) { return re<int64x2_t>(a); }
static inline int16x8_t vreinterpretq_s16_s64(int64x2_t a) { return re<int16x8_t>(a); }
static inline int16x8_t vdupq_n_s16(int16_t x) { int16x8_t r; for (auto& e : r.v) e = x; return r; }
static inline int16x8_t vsubq_s16(int16x8_t a, int16x8_t b) { for (int i = 0; i < 8; ++i) a.v[i] = (int16_t)(a.v[i] - b.v[i]); return a; }
static inline int16x8_t vaddq_s16(int16x8_t a, int16x8_t b) { for (int i = 0; i < 8; ++i) a.v[i] = (int16_t)(a.v[i] + b.v[i]); return a; }
static inline int16x8_t vshlq_n_s16(int16x8_t a, int n) { for (auto& e : a.v) e = (int16_t)((uint16_t)e << n); return a; }
// SQDMULH: saturate((2*a*b) >> 16), arithmetic shift
static inline int16x8_t vqdmulhq_s16(int16x8_t a, int16x8_t b) {
    for (int i = 0; i < 8; ++i) { int64_t p = 2LL * a.v[i] * b.v[i]; int64_t h = p >> 16; if (h > 32767) h = 32767; if (h < -32768) h = -32768; a.v[i] = (int16_t)h; }
    return a;
}
static inline uint8x8_t vld1_u8(const uint8_t* p) { uint8x8_t r; std::memcpy(r.v, p, 8); return r; }
static inline void vst1_u8(uint8_t* p, uint8x8_t a) { std::memcpy(p, a.v, 8); }
static inline uint16x8_t vmovl_u8(uint8x8_t a) { uint16x8_t r; for (int i = 0; i < 8; ++i) r.v[i] = a.v[i]; return r; }
static inline uint8x8_t vqmovun_s16(int16x8_t a) { uint8x8_t r; for (int i = 0; i < 8; ++i) r.v[i] = (uint8_t)(a.v[i] < 0 ? 0 : (a.v[i] > 255 ? 255 : a.v[i])); return r; }
static inline uint8x8x2_t vld2_u8(const uint8_t* p) { uint8x8x2_t r; for (int i = 0; i < 8; ++i) { r.val[0].v[i] = p[2*i]; r.val[1].v[i] = p[2*i+1]; } return r; }
static inline void vst2_u8(uint8_t* p, uint8x8x2_t a) { for (int i = 0; i < 8; ++i) { p[2*i] = a.val[0].v[i]; p[2*i+1] = a.val[1].v[i]; } }
static inline int16x8_t vld1q_s16(const int16_t* p) { int16x8_t r; std::memcpy(r.v, p, 16); return r; }
static inline void vst1q_s16(int16_t* p, int16x8_t a) { std::memcpy(p, a.v, 16); }
// TRN1/TRN2: even / odd lanes of a and b interleaved
#define FSTP_TRN(T, N, name1, name2) \
    static inline T name1(T a, T b) { T r; for (int i = 0; i < N / 2; ++i) { r.v[2*i] = a.v[2*i];   r.v[2*i+1] = b.v[2*i]; }   return r; } \
    static inline T name2(T a, T b) { T r; for (int i = 0; i < N / 2; ++i) { r.v[2*i] = a.v[2*i+1]; r.v[2*i+1] = b.v[2*i+1]; } return r; }
FSTP_TRN(int16x8_t, 8, vtrn1q_s16, vtrn2q_s16)
FSTP_TRN(int32x4_t, 4, vtrn1q_s32, vtrn2q_s32)
FSTP_TRN(int64x2_t, 2, vtrn1q_s64, vtrn2q_s64)
// --- FSTPSIMDOptimizations.h inline helpers, SoftEdgeRow ---
struct uint8x16_t { uint8_t v[16]; };
struct uint64x2_t { uint64_t v[2]; };
static inline uint8x16_t vld1q_u8(const uint8_t* p) { uint8x16_t r; std::memcpy(r.v, p, 16); return r; }
static inline void vst1q_u8(uint8_t* p, uint8x16_t a) { std::memcpy(p, a.v, 16); }
static inline uint8x16_t vdupq_n_u8(uint8_t x) { uint8x16_t r; for (auto& e : r.v) e = x; return r; }
static inline uint8x16_t vceqq_u8(uint8x16_t a, uint8x16_t b) { for (int i = 0; i < 16; ++i) a.v[i] = a.v[i] == b.v[i] ? 0xFF : 0; return a; }
static inline uint64x2_t vreinterpretq_u64_u8(uint8x16_t a) { return re<uint64x2_t>(a); }
#define vgetq_lane_u64(a, n) ((a).v[n])
static inline uint8x8_t vget_low_u8(uint8x16_t a) { uint8x8_t r; std::memcpy(r.v, a.v, 8); return r; }
static inline uint8x8_t vget_high_u8(uint8x16_t a) { uint8x8_t r; std::memcpy(r.v, a.v + 8, 8); return r; }
static inline uint8x16_t vcombine_u8(uint8x8_t lo, uint8x8_t hi) { uint8x16_t r; std::memcpy(r.v, lo.v, 8); std::memcpy(r.v + 8, hi.v, 8); return r; }
static inline int16x8_t vshrq_n_s16(int16x8_t a, int n) { for (auto& e : a.v) e = (int16_t)(e >> n); return a; }
static inline int16x8_t vmulq_s16(int16x8_t a, int16x8_t b) { for (int i = 0; i < 8; ++i) a.v[i] = (int16_t)(uint16_t)((uint32_t)(uint16_t)a.v[i] * (uint32_t)(uint16_t)b.v[i]); return a; }
// --- audio sample conversion ---
struct float32x4_t { float v[4]; };
struct int16x4_t { int16_t v[4]; };
struct int16x8x2_t { int16x8_t val[2]; };
static inline float32x4_t vld1q_f32(const float* p) { float32x4_t r; std::memcpy(r.v, p, 16); return r; }
static inline float32x4_t vdupq_n_f32(float x) { float32x4_t r; for (auto& e : r.v) e = x; return r; }
static inline float32x4_t vmulq_f32(float32x4_t a, float32x4_t b) { for (int i = 0; i < 4; ++i) a.v[i] = a.v[i] * b.v[i]; return a; }
// FMINNM/FMAXNM: if exactly one operand is a (quiet) NaN, return the other
static inline float32x4_t vminnmq_f32(float32x4_t a, float32x4_t b) { for (int i = 0; i < 4; ++i) a.v[i] = std::isnan(a.v[i]) ? b.v[i] : std::isnan(b.v[i]) ? a.v[i] : std::fmin(a.v[i], b.v[i]); return a; }
static inline float32x4_t vmaxnmq_f32(float32x4_t a, float32x4_t b) { for (int i = 0; i < 4; ++i) a.v[i] = std::isnan(a.v[i]) ? b.v[i] : std::isnan(b.v[i]) ? a.v[i] : std::fmax(a.v[i], b.v[i]); return a; }
// FCVTZS: toward zero, saturating, NaN → 0
static inline int32x4_t vcvtq_s32_f32(float32x4_t a) { int32x4_t r; for (int i = 0; i < 4; ++i) { float f = a.v[i]; r.v[i] = std::isnan(f) ? 0 : f >= 2147483647.0f ? 2147483647 : f <= -2147483648.0f ? (int32_t)-2147483648LL : (int32_t)f; } return r; }
static inline int16x4_t vqmovn_s32(int32x4_t a) { int16x4_t r; for (int i = 0; i < 4; ++i) r.v[i] = (int16_t)std::max(-32768, std::min(32767, a.v[i])); return r; }
static inline int16x4_t vmovn_s32(int32x4_t a) { int16x4_t r; for (int i = 0; i < 4; ++i) r.v[i] = (int16_t)a.v[i]; return r; }
static inline int16x8_t vcombine_s16(int16x4_t lo, int16x4_t hi) { int16x8_t r; std::memcpy(r.v, lo.v, 8); std::memcpy(r.v + 4, hi.v, 8); return r; }
static inline int32x4_t vld1q_s32(const int32_t* p) { int32x4_t r; std::memcpy(r.v, p, 16); return r; }
static inline int32x4_t vshrq_n_s32(int32x4_t a, int n) { for (auto& e : a.v) e >>= n; return a; }
static inline void vst2q_s16(int16_t* p, int16x8x2_t a) { for (int i = 0; i < 8; ++i) { p[2*i] = a.val[0].v[i]; p[2*i+1] = a.val[1].v[i]; } }
