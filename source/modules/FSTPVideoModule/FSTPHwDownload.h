#pragma once

// Bring a hardware-decoded frame into system memory. Same contract as
// av_hwframe_transfer_data(dst, src, 0): dst is a fresh frame, src a hardware frame; returns 0 or
// a negative AVERROR. Only the pixels are copied — callers copy the properties.
//
// Linux, VA-API, NV12 or P010: the surface is mapped through vaDeriveImage and read with streaming
// loads (av_image_copy_uc_from). FFmpeg's own transfer maps the surface too but reads it with a
// plain copy, and mapped VA surfaces are write-combined memory, which plain loads read very
// slowly: on a Pentium Gold 7505, a 1280x720 frame 4.1-4.8 ms -> 1.4 ms of CPU, 720x540 1.6 ->
// 0.6-0.7 ms, the same bytes. Anything else (other platforms, formats, drivers, any failure)
// goes through av_hwframe_transfer_data.

extern "C" {
#include <libavutil/frame.h>
}

int FSTPHwDownload(AVFrame* dst, const AVFrame* src);
