#include "FSTPHwTo8Bit.h"

#include <iostream>

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

FSTPHwTo8Bit::~FSTPHwTo8Bit() { Reset(); }

void FSTPHwTo8Bit::Reset() {
    avfilter_graph_free(&graph_);   // frees the filters too
    in_ = out_ = nullptr;
    frames_ = nullptr;
}

bool FSTPHwTo8Bit::Applies(const AVFrame* hw) const {
#if defined(__linux__)
    if (failed_ || !hw || hw->format != AV_PIX_FMT_VAAPI || !hw->hw_frames_ctx) return false;
    const auto* frames = reinterpret_cast<const AVHWFramesContext*>(hw->hw_frames_ctx->data);
    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(frames->sw_format);
    return d && d->comp[0].depth > 8;
#else
    (void)hw;
    return false;
#endif
}

bool FSTPHwTo8Bit::Build(const AVFrame* src) {
    Reset();
    graph_ = avfilter_graph_alloc();
    const AVFilter* buffer = avfilter_get_by_name("buffer");
    const AVFilter* scale = avfilter_get_by_name("scale_vaapi");
    const AVFilter* sink = avfilter_get_by_name("buffersink");
    if (!graph_ || !buffer || !scale || !sink) return false;

    in_ = avfilter_graph_alloc_filter(graph_, buffer, "in");
    AVBufferSrcParameters* par = av_buffersrc_parameters_alloc();
    if (!in_ || !par) {
        av_free(par);
        return false;
    }
    par->format = src->format;
    par->width = src->width;
    par->height = src->height;
    par->time_base = AVRational{1, 1000};   // timestamps don't matter to the scaler
    par->sample_aspect_ratio = src->sample_aspect_ratio;
    par->hw_frames_ctx = src->hw_frames_ctx;   // borrowed: the call takes its own reference
    int ret = av_buffersrc_parameters_set(in_, par);
    av_free(par);
    if (ret < 0 || avfilter_init_str(in_, nullptr) < 0) return false;

    AVFilterContext* conv = nullptr;
    if (avfilter_graph_create_filter(&conv, scale, "to8bit", "format=nv12", nullptr, graph_) < 0) return false;
    if (avfilter_graph_create_filter(&out_, sink, "out", nullptr, nullptr, graph_) < 0) return false;
    if (avfilter_link(in_, 0, conv, 0) < 0 || avfilter_link(conv, 0, out_, 0) < 0) return false;
    if (avfilter_graph_config(graph_, nullptr) < 0) return false;

    frames_ = src->hw_frames_ctx->data;
    width_ = src->width;
    height_ = src->height;
    return true;
}

bool FSTPHwTo8Bit::Convert(const AVFrame* src, AVFrame* dst) {
    if (!Applies(src)) return false;
    if ((frames_ != src->hw_frames_ctx->data || src->width != width_ || src->height != height_) && !Build(src)) {
        std::cerr << "⚠️  [HW 10→8] scale_vaapi unavailable — 10-bit pictures come back as they are" << std::endl;
        Reset();
        failed_ = true;
        return false;
    }
    // The graph reads a reference of `src`; `src` itself stays the caller's.
    AVFrame* ref = av_frame_clone(src);
    if (!ref) return false;
    int ret = av_buffersrc_add_frame_flags(in_, ref, 0);
    av_frame_free(&ref);
    if (ret >= 0) ret = av_buffersink_get_frame(out_, dst);
    if (ret < 0) {
        av_frame_unref(dst);
        Reset();
        failed_ = true;
        std::cerr << "⚠️  [HW 10→8] conversion failed — 10-bit pictures come back as they are" << std::endl;
        return false;
    }
    return true;
}
