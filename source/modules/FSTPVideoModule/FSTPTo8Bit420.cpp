#include "FSTPTo8Bit420.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

extern "C" {
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

// Before this, the display converted such pictures itself, on the render thread, with a swscale
// context built and freed for every picture — three times per picture when the Betacam effect's
// neighbours came along. ProRes HQ 1280x720p50 on a Pentium Gold 7505: the render thread alone
// took 61% of a core.

bool FSTPIsDisplay420(int format) {
    return format == AV_PIX_FMT_YUV420P || format == AV_PIX_FMT_YUVJ420P || format == AV_PIX_FMT_NV12;
}

namespace {

// LSB-aligned samples of `depth` bits → 8 bits, rounded.
void RowDown(uint8_t* __restrict d, const uint16_t* __restrict s, int n, int depth) {
    const int sh = depth - 8;
    const int half = 1 << (sh - 1);
    for (int i = 0; i < n; ++i) d[i] = static_cast<uint8_t>(std::min(255, (s[i] + half) >> sh));
}

// The mean of two LSB-aligned rows → 8 bits (a 4:2:2 chroma row pair → one 4:2:0 row).
void RowPairDown(uint8_t* __restrict d, const uint16_t* __restrict a, const uint16_t* __restrict b, int n,
                 int depth) {
    const int sh = depth - 7;
    const int half = 1 << (sh - 1);
    for (int i = 0; i < n; ++i) d[i] = static_cast<uint8_t>(std::min(255, (a[i] + b[i] + half) >> sh));
}

void RowPair8(uint8_t* __restrict d, const uint8_t* __restrict a, const uint8_t* __restrict b, int n) {
    for (int i = 0; i < n; ++i) d[i] = static_cast<uint8_t>((a[i] + b[i] + 1) >> 1);
}

// MSB-aligned 16-bit containers (P010 / P016 and kin) → their top byte, rounded.
void RowTop(uint8_t* __restrict d, const uint16_t* __restrict s, int n) {
    for (int i = 0; i < n; ++i) d[i] = static_cast<uint8_t>(std::min(255, (s[i] + 128) >> 8));
}

void RowPairTop(uint8_t* __restrict d, const uint16_t* __restrict a, const uint16_t* __restrict b, int n) {
    for (int i = 0; i < n; ++i) d[i] = static_cast<uint8_t>(std::min(255, (a[i] + b[i] + 256) >> 9));
}

template <typename T>
const T* Row(const AVFrame* f, int plane, int y) {
    return reinterpret_cast<const T*>(f->data[plane] + static_cast<ptrdiff_t>(y) * f->linesize[plane]);
}

uint8_t* Row8(AVFrame* f, int plane, int y) {
    return f->data[plane] + static_cast<ptrdiff_t>(y) * f->linesize[plane];
}

// Planar Y/U/V, chroma halved horizontally (4:2:0 or 4:2:2), 8 bits or little-endian LSB-aligned.
bool IsDirectPlanar(const AVPixFmtDescriptor* d) {
    if (!d || (d->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_PAL |
                           AV_PIX_FMT_FLAG_BITSTREAM | AV_PIX_FMT_FLAG_BE)))
        return false;
    if (!(d->flags & AV_PIX_FMT_FLAG_PLANAR) || d->nb_components < 3) return false;
    if (d->log2_chroma_w != 1 || d->log2_chroma_h > 1) return false;
    const int depth = d->comp[0].depth;
    for (int c = 0; c < 3; ++c) {
        if (d->comp[c].plane != c || d->comp[c].shift != 0 || d->comp[c].depth != depth) return false;
        if (d->comp[c].step != (depth > 8 ? 2 : 1)) return false;
    }
    return depth >= 8 && depth <= 16;
}

// Y plane + interleaved UV plane in 16-bit MSB-aligned containers (P010, P012, P016; P210, P216).
bool IsDirectSemiPlanar16(const AVPixFmtDescriptor* d) {
    if (!d || (d->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_BE))) return false;
    if (d->nb_components != 3 || d->log2_chroma_w != 1 || d->log2_chroma_h > 1) return false;
    if (d->comp[0].plane != 0 || d->comp[1].plane != 1 || d->comp[2].plane != 1) return false;
    if (d->comp[0].step != 2 || d->comp[1].step != 4) return false;
    return d->comp[0].depth + d->comp[0].shift == 16;
}

}  // namespace

int FSTPConvertTo8Bit420(AVFrame* dst, const AVFrame* src, SwsContext** sws) {
    const auto fmt = static_cast<AVPixelFormat>(src->format);
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(fmt);
    const int w = src->width, h = src->height;
    const int cw = (w + 1) >> 1, ch = (h + 1) >> 1;

    const bool planar = IsDirectPlanar(desc);
    const bool semi16 = !planar && IsDirectSemiPlanar16(desc);
    const bool jpeg = fmt == AV_PIX_FMT_YUVJ422P || fmt == AV_PIX_FMT_YUVJ444P || fmt == AV_PIX_FMT_YUVJ440P ||
                      fmt == AV_PIX_FMT_YUVJ411P;

    dst->format = semi16 ? AV_PIX_FMT_NV12 : (jpeg ? AV_PIX_FMT_YUVJ420P : AV_PIX_FMT_YUV420P);
    dst->width = w;
    dst->height = h;
    int ret = av_frame_get_buffer(dst, 0);
    if (ret < 0) return ret;

    if (planar) {
        const int depth = desc->comp[0].depth;
        const bool is422 = desc->log2_chroma_h == 0;
        for (int y = 0; y < h; ++y) {
            if (depth == 8) std::memcpy(Row8(dst, 0, y), Row<uint8_t>(src, 0, y), w);
            else RowDown(Row8(dst, 0, y), Row<uint16_t>(src, 0, y), w, depth);
        }
        for (int p = 1; p <= 2; ++p) {
            for (int y = 0; y < ch; ++y) {
                // 4:2:2: the mean of rows 2y and 2y+1 (the last row alone if the height is odd).
                const int a = is422 ? 2 * y : y;
                const int b = is422 ? std::min(2 * y + 1, h - 1) : y;
                if (depth == 8) {
                    if (is422) RowPair8(Row8(dst, p, y), Row<uint8_t>(src, p, a), Row<uint8_t>(src, p, b), cw);
                    else std::memcpy(Row8(dst, p, y), Row<uint8_t>(src, p, a), cw);
                } else if (is422) {
                    RowPairDown(Row8(dst, p, y), Row<uint16_t>(src, p, a), Row<uint16_t>(src, p, b), cw, depth);
                } else {
                    RowDown(Row8(dst, p, y), Row<uint16_t>(src, p, a), cw, depth);
                }
            }
        }
    } else if (semi16) {
        const bool is422 = desc->log2_chroma_h == 0;
        for (int y = 0; y < h; ++y) RowTop(Row8(dst, 0, y), Row<uint16_t>(src, 0, y), w);
        for (int y = 0; y < ch; ++y) {
            const int a = is422 ? 2 * y : y;
            const int b = is422 ? std::min(2 * y + 1, h - 1) : y;
            if (is422) RowPairTop(Row8(dst, 1, y), Row<uint16_t>(src, 1, a), Row<uint16_t>(src, 1, b), 2 * cw);
            else RowTop(Row8(dst, 1, y), Row<uint16_t>(src, 1, a), 2 * cw);
        }
    } else {
        *sws = sws_getCachedContext(*sws, w, h, fmt, w, h, static_cast<AVPixelFormat>(dst->format),
                                    SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!*sws) {
            av_frame_unref(dst);
            return AVERROR(EINVAL);
        }
        sws_scale(*sws, src->data, src->linesize, 0, h, dst->data, dst->linesize);
    }

    ret = av_frame_copy_props(dst, src);
    if (ret < 0) {
        av_frame_unref(dst);
        return ret;
    }
    return 0;
}
