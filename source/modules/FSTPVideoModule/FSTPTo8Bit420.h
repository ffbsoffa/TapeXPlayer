#pragma once

// The display (and the Betacam effect) take 8-bit 4:2:0 pictures only: YUV420P, YUVJ420P or NV12.
// Originals in anything else — 10-bit ProRes / XAVC / HEVC, P010 from a GPU decoder, 8-bit 4:2:2 —
// are brought to that once, on the decoder's own thread, by FSTPConvertTo8Bit420.

extern "C" {
#include <libavutil/frame.h>
}

struct SwsContext;

// True if the display takes pictures in this pixel format as they are.
bool FSTPIsDisplay420(int format);

// Converts `src` (a software picture, any pixel format) into `dst`: NV12 from P010, YUVJ420P from
// YUVJ4xxP, YUV420P otherwise. The common decoder outputs (10-bit 4:2:0 / 4:2:2 planar, P010, 8-bit
// 4:2:2) take a direct path that rounds to 8 bits and averages chroma row pairs; anything else goes
// through swscale with the context cached in *sws (free it with sws_freeContext). Copies the frame
// properties (timestamps, colour) too. Returns 0 or a negative AVERROR.
int FSTPConvertTo8Bit420(AVFrame* dst, const AVFrame* src, SwsContext** sws);
