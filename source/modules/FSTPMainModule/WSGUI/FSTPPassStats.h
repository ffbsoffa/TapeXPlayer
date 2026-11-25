#pragma once

// FSTP_RENDER_STATS=1: what each pass of the video texture update costs on the render thread
// (frame copy, Betacam effect, smear, edge fade, soft edges, film grain, upload), averaged and
// printed once a second. Off by default; when off a pass costs one branch.

#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace FSTPPassStats {

enum Pass { Copy, Betacam, Smear, EdgeFade, SoftEdges, Grain, Upload, kCount };

inline bool Enabled() {
    static const bool on = std::getenv("FSTP_RENDER_STATS") != nullptr;
    return on;
}

struct Stats {
    double ms[kCount] = {};
    int calls[kCount] = {};
    int updates = 0;
    double rows_uploaded = 0.0;   // sum over updates of the uploaded share of the picture (0..1)
    std::chrono::steady_clock::time_point window = std::chrono::steady_clock::now();
};
inline Stats& Get() { static Stats s; return s; }

inline std::chrono::steady_clock::time_point Start() {
    return Enabled() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
}

inline void Add(Pass pass, std::chrono::steady_clock::time_point t0) {
    if (!Enabled()) return;
    Stats& s = Get();
    s.ms[pass] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    ++s.calls[pass];
}

// Share of the picture's rows an update sent to the GPU (0 = skipped, 1 = whole picture).
inline void UploadedRows(int rows, int height) {
    if (!Enabled() || height <= 0) return;
    Get().rows_uploaded += static_cast<double>(rows) / height;
}

// Called once per texture update; prints and resets once a second.
inline void EndUpdate() {
    if (!Enabled()) return;
    Stats& s = Get();
    ++s.updates;
    auto now = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(now - s.window).count();
    if (secs < 1.0) return;
    static const char* names[kCount] = {"copy", "betacam", "smear", "edgefade", "softedges", "grain", "upload"};
    char line[512];
    int len = std::snprintf(line, sizeof(line), "🎨 [FX STATS] updates/s=%.1f", s.updates / secs);
    double total = 0.0;
    for (int i = 0; i < kCount; ++i) {
        if (s.calls[i] == 0) continue;
        len += std::snprintf(line + len, sizeof(line) - len, " %s=%.2fms", names[i], s.ms[i] / s.calls[i]);
        if (i != Smear && i != EdgeFade) total += s.ms[i];   // smear/edgefade may run inside betacam
    }
    len += std::snprintf(line + len, sizeof(line) - len, " rows_uploaded=%.0f%%",
                         s.updates ? 100.0 * s.rows_uploaded / s.updates : 0.0);
    std::snprintf(line + len, sizeof(line) - len, " | cpu/s=%.0fms\n", total / secs);
    std::fputs(line, stdout);
    std::fflush(stdout);
    s = Stats{};
}

}  // namespace FSTPPassStats
