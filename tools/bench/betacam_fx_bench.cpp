// Betacam effect on its own: how long ApplyPixelFX takes per picture at a given shuttle speed,
// outside the player (no decoder, no display, no thread preemption in the numbers).
//   tools/bench/run_betacam_fx_bench.sh [width height]
// Runs each speed with the physical shuttle (default) and with FSTP_BETACAM_PHYS=0 is done by
// the script, as the switch is read once per process.
#include "FSTPBetacamEffect.h"
#include "FSTPHardwareDetection.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

FSTPHardwareDetection* g_hardware_detection = nullptr;   // referenced by the SIMD kernels' CPU check

int main(int argc, char** argv) {
    const int W = argc > 1 ? std::atoi(argv[1]) : 720;
    const int H = argc > 2 ? std::atoi(argv[2]) : 540;
    const double speeds[] = {9, 14, 24, 32, -24};
    std::vector<uint8_t> src_y(W * H), src_u(W * H / 4), src_v(W * H / 4);
    std::mt19937 rng(1);
    for (int y = 0; y < H; ++y)                       // a picture with texture, not a flat field
        for (int x = 0; x < W; ++x) src_y[y * W + x] = static_cast<uint8_t>(16 + (x * 7 + y * 3 + rng() % 40) % 219);
    for (auto& c : src_u) c = static_cast<uint8_t>(100 + rng() % 56);
    for (auto& c : src_v) c = static_cast<uint8_t>(100 + rng() % 56);
    std::vector<uint8_t> y(src_y.size()), u(src_u.size()), v(src_v.size());

    for (double s : speeds) {
        FSTPBetacamEffect fx;
        fx.SetEnabled(true);
        FSTPBetacamEffect::PlaybackMetrics m;
        m.playback_rate = s;
        m.is_reverse = s < 0;
        m.position_seconds = 1500.0;                         // mid-clip: room both ways for 2000 pictures
        m.duration_seconds = 3000.0;
        m.frame_rate = 25.0;
        const int frames = 2000;
        double total = 0.0;
        for (int i = 0; i < frames; ++i) {
            m.position_seconds += s / 60.0;
            fx.UpdatePlaybackMetrics(0, m);
            std::memcpy(y.data(), src_y.data(), y.size());   // the manager hands a fresh copy each time
            std::memcpy(u.data(), src_u.data(), u.size());
            std::memcpy(v.data(), src_v.data(), v.size());
            FSTPBetacamEffect::FrameContext fc;
            fc.pixel_format = SDL_PIXELFORMAT_IYUV;
            fc.width = W;
            fc.height = H;
            fc.frame_number = i / 2 * 10;                    // a new picture every other call, as at 24× on 60 Hz
            fc.new_frame = (i % 2 == 0);
            fc.planes[0] = y.data(); fc.linesize[0] = W;
            fc.planes[1] = u.data(); fc.linesize[1] = W / 2;
            fc.planes[2] = v.data(); fc.linesize[2] = W / 2;
            auto t0 = std::chrono::steady_clock::now();
            fx.ApplyPixelFX(0, fc);
            total += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        }
        const char* phys = std::getenv("FSTP_BETACAM_PHYS");
        std::printf("%s %4.0fx  %dx%d  %.3f ms per picture\n", (phys && phys[0] == '0') ? "old " : "phys", s, W, H,
                    total / frames);
    }
    return 0;
}
