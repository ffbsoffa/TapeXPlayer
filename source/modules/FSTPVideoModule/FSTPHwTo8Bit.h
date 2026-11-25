#pragma once

// 10-bit hardware-decoded pictures (HEVC Main 10, VP9 profile 2: P010 surfaces) turned into 8-bit
// NV12 on the GPU before they come back to memory — the display takes 8 bits anyway. Half the
// bytes cross over and the CPU converts nothing (FSTPConvertTo8Bit420 is what it replaces).
// Linux / VA-API (scale_vaapi) for now; elsewhere Applies() is false and nothing changes.
//
// Pentium Gold 7505, HEVC Main 10 1080p25 through FFmpeg's own download: 8.0 -> 4.8 ms of CPU per
// picture; against FFmpeg's CPU conversion the pixels are at most 1 level apart, 75% identical.

extern "C" {
#include <libavutil/frame.h>
}

struct AVFilterGraph;
struct AVFilterContext;

class FSTPHwTo8Bit {
public:
    FSTPHwTo8Bit() = default;
    ~FSTPHwTo8Bit();
    FSTPHwTo8Bit(const FSTPHwTo8Bit&) = delete;
    FSTPHwTo8Bit& operator=(const FSTPHwTo8Bit&) = delete;

    // A hardware picture this can take (and hasn't failed on before).
    bool Applies(const AVFrame* hw) const;

    // `dst` receives a new hardware picture in NV12. False if the GPU path is unavailable — the
    // caller then downloads `src` as it is; after a failure Applies() stays false.
    bool Convert(const AVFrame* src, AVFrame* dst);

private:
    bool Build(const AVFrame* src);
    void Reset();

    AVFilterGraph* graph_ = nullptr;
    AVFilterContext* in_ = nullptr;
    AVFilterContext* out_ = nullptr;
    const void* frames_ = nullptr;   // the hw_frames_ctx the graph was built for
    int width_ = 0, height_ = 0;
    bool failed_ = false;
};
