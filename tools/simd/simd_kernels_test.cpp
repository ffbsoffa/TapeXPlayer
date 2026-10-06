// Bit-exactness test for the CPU-specific kernels in FSTPSIMDOptimizations.cpp: compiles the
// production file into this test and compares every kernel with the plain scalar loop it
// replaces (the Betacam smear and soft-edge row, the audio sample conversions), then times the
// 720p smear. Every path must match exactly, so the picture and the sound never depend on the CPU.
//
// Built by run_simd_tests.sh (see tools/README.md) in up to three flavours:
//   native        — SSE2 on x86-64 (Intel and AMD), real NEON on arm64
//   -DPATH_SCALAR — the scalar fallback (x86-64 only)
//   -DPATH_NEON   — the NEON code on x86-64 through neon_shim/arm_neon.h
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstddef>
#include <vector>
#include <random>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <map>
#include <memory>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavutil/opt.h>
}
#if defined(PATH_SCALAR)
#include <immintrin.h>   // pre-include: its target pragmas would re-define __SSE2__
#endif
#if defined(PATH_NEON) || defined(PATH_SCALAR)
#undef __SSE2__
#endif
#if defined(PATH_NEON)
#undef __x86_64__
#define __aarch64__ 1
#define __ARM_NEON 1
#endif
#include "FSTPSIMDOptimizations.cpp"
FSTPHardwareDetection* g_hardware_detection = nullptr;   // referenced by DetectCPUFeatures

static void PlainSmear(uint8_t* p, int pitch, int rows, int n, int step, int phase, int k) {
    for (int yy = 0; yy < rows; ++yy) { uint8_t* row = p + (size_t)yy * pitch + phase; int prev = row[0];
        for (int x = step; x < n * step; x += step) { int cur = (row[x] * (256 - k) + prev * k) >> 8; row[x] = (uint8_t)cur; prev = cur; } }
}
int main() {
#if defined(FSTP_KERNEL_SSE2)
    const char* path = "SSE2";
#elif defined(FSTP_KERNEL_NEON) && defined(PATH_NEON)
    const char* path = "NEON (shim)";
#elif defined(FSTP_KERNEL_NEON)
    const char* path = "NEON";
#else
    const char* path = "scalar";
#endif
    std::mt19937 rng(11);
    int bad = 0, cases = 0;
    for (int t = 0; t < 4000; ++t) {
        int w = 1 + rng() % 100, h = 1 + rng() % 41, k = rng() % 256;
        bool nv = rng() % 2;
        int pitch = (nv ? 2 * w : w) + rng() % 40;
        std::vector<uint8_t> a((size_t)pitch * h);
        int mode = rng() % 3;
        for (auto& v : a) v = mode == 0 ? rng() : (mode == 1 ? (rng() % 2 ? 0 : 255) : 100 + rng() % 30);
        std::vector<uint8_t> b = a;
        if (nv) { PlainSmear(a.data(), pitch, h, w, 2, 0, k); PlainSmear(a.data(), pitch, h, w, 2, 1, k); }
        else      PlainSmear(a.data(), pitch, h, w, 1, 0, k);
        FSTP::SIMDOptimizations::AnalogSmear(b.data(), pitch, h, w, nv, k);
        ++cases;
        if (a != b && ++bad <= 5) printf("  SMEAR MISMATCH w=%d h=%d k=%d nv=%d\n", w, h, k, nv);
    }
    int sbad = 0;
    for (int t = 0; t < 4000; ++t) {
        int w = 1 + rng() % 200, mix = rng() % 257;
        std::vector<uint8_t> ab(w), c(w), be(w), d1(w), d2(w);
        for (int i = 0; i < w; ++i) { ab[i] = rng(); c[i] = rng(); be[i] = rng(); }
        for (int x = 0; x < w; ++x) { int bl = (ab[x] + 2 * c[x] + be[x]) >> 2; d1[x] = (uint8_t)(c[x] + (((bl - c[x]) * mix) >> 8)); }
        FSTP::SIMDOptimizations::SoftEdgeRow(d2.data(), ab.data(), c.data(), be.data(), w, mix);
        if (d1 != d2 && ++sbad <= 5) printf("  SOFTEDGE MISMATCH w=%d mix=%d\n", w, mix);
    }
    printf("%-12s smear: %d/%d differ   soft-edge rows: %d/4000 differ\n", path, bad, cases, sbad);
    // audio: reference = the old scalar loops of FSTPAudioModule_wrapper.cpp
    auto refF = [](float x) { float s = std::max(-1.0f, std::min(1.0f, x)); return static_cast<int16_t>(s * 32767.0f); };
    std::uniform_real_distribution<float> wide(-3.0f, 3.0f), unit(-1.0f, 1.0f);
    const float specials[] = {0.0f, -0.0f, 1.0f, -1.0f, 0.99999f, -0.99999f, 1e-8f, -1e-8f,
                              INFINITY, -INFINITY, NAN, 32767.0f, -1e30f, 1.0000001f, 0.5f / 32767.0f};
    int abad = 0, acases = 0;
    for (int t = 0; t < 3000; ++t) {
        size_t n = rng() % 70; int ch = 1 + rng() % 6;
        std::vector<float> f(n * ch);
        for (auto& v : f) { int m = rng() % 4; v = m == 0 ? wide(rng) : m == 1 ? unit(rng) : m == 2 ? specials[rng() % 15] : unit(rng) * 1e-3f; }
        std::vector<int16_t> a(n * ch), b(n * ch);
        for (size_t i = 0; i < n * ch; ++i) a[i] = refF(f[i]);
        FSTP::SIMDOptimizations::AudioFloatToS16(f.data(), b.data(), n * ch);
        ++acases; if (a != b && ++abad <= 3) printf("  FLT MISMATCH n=%zu\n", n * ch);
        std::vector<std::vector<float>> pl(ch, std::vector<float>(n));
        std::vector<const float*> pp(ch);
        for (int c = 0; c < ch; ++c) { for (size_t i = 0; i < n; ++i) pl[c][i] = f[i * ch + c]; pp[c] = pl[c].data(); }
        std::fill(b.begin(), b.end(), 0x5555);
        FSTP::SIMDOptimizations::AudioFloatPlanarToS16(pp.data(), ch, n, b.data());
        ++acases; if (a != b && ++abad <= 3) printf("  FLTP MISMATCH n=%zu ch=%d\n", n, ch);
        std::vector<int32_t> s32(n * ch);
        for (auto& v : s32) v = static_cast<int32_t>(rng());
        std::vector<int16_t> c1(n * ch), c2(n * ch);
        for (size_t i = 0; i < n * ch; ++i) c1[i] = static_cast<int16_t>(s32[i] >> 16);
        FSTP::SIMDOptimizations::AudioS32ToS16(s32.data(), c2.data(), n * ch);
        ++acases; if (c1 != c2 && ++abad <= 3) printf("  S32 MISMATCH n=%zu\n", n * ch);
    }
    printf("%-12s audio conversions: %d/%d differ\n", path, abad, acases);
    bad += abad;
    const int W = 1280, H = 720;
    std::vector<uint8_t> y((size_t)W * H), uv((size_t)W * H / 2);
    for (auto& v : y) v = rng();
    for (auto& v : uv) v = rng();
    double best = 1e9;
    for (int it = 0; it < 200; ++it) {
        auto t0 = std::chrono::steady_clock::now();
        FSTP::SIMDOptimizations::AnalogSmear(y.data(), W, H, W, false, 71);
        FSTP::SIMDOptimizations::AnalogSmear(uv.data(), W, H / 2, W / 2, true, 125);
        best = std::min(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    printf("%-12s 720p NV12 smear best %.3f ms\n", path, best);
    return (bad || sbad) ? 1 : 0;
}
