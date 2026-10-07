#include "FSTPHwDownload.h"

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
}

#if defined(__linux__) && __has_include(<va/va.h>) && __has_include(<libavutil/hwcontext_vaapi.h>)
#define FSTP_VAAPI_FAST_DOWNLOAD 1
#include <dlfcn.h>
#include <cstdlib>
#include <va/va.h>
extern "C" {
#include <libavutil/hwcontext_vaapi.h>
}
#endif

#ifdef FSTP_VAAPI_FAST_DOWNLOAD
namespace {

// libva is already loaded when FFmpeg decodes on VA-API; take its functions from there instead of
// linking it, so the player keeps no build or load dependency of its own on libva.
struct VaApi {
    decltype(&vaSyncSurface) syncSurface = nullptr;
    decltype(&vaDeriveImage) deriveImage = nullptr;
    decltype(&vaMapBuffer) mapBuffer = nullptr;
    decltype(&vaUnmapBuffer) unmapBuffer = nullptr;
    decltype(&vaDestroyImage) destroyImage = nullptr;
    bool ok = false;
};

const VaApi& Va() {
    static const VaApi api = [] {
        VaApi a;
        if (std::getenv("FSTP_HW_DOWNLOAD_FFMPEG")) return a;   // A/B: FFmpeg's transfer only
        void* lib = dlopen("libva.so.2", RTLD_NOW | RTLD_NOLOAD);
        if (!lib) return a;
        a.syncSurface = reinterpret_cast<decltype(a.syncSurface)>(dlsym(lib, "vaSyncSurface"));
        a.deriveImage = reinterpret_cast<decltype(a.deriveImage)>(dlsym(lib, "vaDeriveImage"));
        a.mapBuffer = reinterpret_cast<decltype(a.mapBuffer)>(dlsym(lib, "vaMapBuffer"));
        a.unmapBuffer = reinterpret_cast<decltype(a.unmapBuffer)>(dlsym(lib, "vaUnmapBuffer"));
        a.destroyImage = reinterpret_cast<decltype(a.destroyImage)>(dlsym(lib, "vaDestroyImage"));
        a.ok = a.syncSurface && a.deriveImage && a.mapBuffer && a.unmapBuffer && a.destroyImage;
        return a;   // the handle stays open: the library is FFmpeg's and stays loaded anyway
    }();
    return api;
}

bool VaapiDownloadNV12(AVFrame* dst, const AVFrame* src) {
    if (src->format != AV_PIX_FMT_VAAPI || !src->hw_frames_ctx) return false;
    const auto* frames = reinterpret_cast<const AVHWFramesContext*>(src->hw_frames_ctx->data);
    if (frames->sw_format != AV_PIX_FMT_NV12) return false;
    const VaApi& va = Va();
    if (!va.ok) return false;

    const auto* device = reinterpret_cast<const AVVAAPIDeviceContext*>(frames->device_ctx->hwctx);
    VADisplay display = device->display;
    const VASurfaceID surface = static_cast<VASurfaceID>(reinterpret_cast<uintptr_t>(src->data[3]));
    if (va.syncSurface(display, surface) != VA_STATUS_SUCCESS) return false;

    VAImage image;
    if (va.deriveImage(display, surface, &image) != VA_STATUS_SUCCESS) return false;
    bool done = false;
    void* mapped = nullptr;
    if (image.format.fourcc == VA_FOURCC_NV12 && image.width >= src->width && image.height >= src->height &&
        va.mapBuffer(display, image.buf, &mapped) == VA_STATUS_SUCCESS) {
        dst->format = AV_PIX_FMT_NV12;
        dst->width = src->width;
        dst->height = src->height;
        if (av_frame_get_buffer(dst, 64) >= 0) {
            const uint8_t* planes[4] = {static_cast<const uint8_t*>(mapped) + image.offsets[0],
                                        static_cast<const uint8_t*>(mapped) + image.offsets[1], nullptr, nullptr};
            const ptrdiff_t src_pitch[4] = {static_cast<ptrdiff_t>(image.pitches[0]),
                                            static_cast<ptrdiff_t>(image.pitches[1]), 0, 0};
            const ptrdiff_t dst_pitch[4] = {dst->linesize[0], dst->linesize[1], 0, 0};
            av_image_copy_uc_from(dst->data, dst_pitch, planes, src_pitch, AV_PIX_FMT_NV12, src->width, src->height);
            done = true;
        } else {
            av_frame_unref(dst);
        }
        va.unmapBuffer(display, image.buf);
    }
    va.destroyImage(display, image.image_id);
    return done;
}

}  // namespace
#endif

int FSTPHwDownload(AVFrame* dst, const AVFrame* src) {
#ifdef FSTP_VAAPI_FAST_DOWNLOAD
    if (VaapiDownloadNV12(dst, src)) return 0;
#endif
    return av_hwframe_transfer_data(dst, src, 0);
}
