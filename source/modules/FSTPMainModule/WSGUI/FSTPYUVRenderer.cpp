#include "FSTPYUVRenderer.h"
#include "../../FSTPVideoModule/FSTPPerformanceProfiler.h"
#include "../../FSTPVideoModule/FSTPCallCounter.h"
#include <iostream>
#include <cstring>

extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/pixfmt.h>
}

FSTPYUVRenderer::FSTPYUVRenderer()
    : renderer_(nullptr)
    , texture_(nullptr)
    , mode_(YUVRendererMode::NATIVE_YUV)
    , texture_format_(SDL_PIXELFORMAT_UNKNOWN)
    , width_(0)
    , height_(0)
    , sws_ctx_(nullptr)
    , rgb_buffer_(nullptr)
    , rgb_buffer_size_(0)
{
}

FSTPYUVRenderer::~FSTPYUVRenderer() {
    Cleanup();
}

bool FSTPYUVRenderer::Initialize(SDL_Renderer* renderer, int width, int height, Uint32 sdl_format) {
    if (!renderer || width <= 0 || height <= 0) {
        return false;
    }

    renderer_ = renderer;
    width_ = width;
    height_ = height;

    // First try native YUV mode with specified format
    if (IsYUVSupported(renderer)) {
        if (InitializeNativeYUV(renderer, width, height, sdl_format)) {
            mode_ = YUVRendererMode::NATIVE_YUV;
            const char* format_name = (sdl_format == SDL_PIXELFORMAT_NV12) ? "NV12" : "IYUV";
            std::cout << "✅ [YUV RENDERER] Initialized in NATIVE YUV mode ("
                      << width << "x" << height << ", " << format_name << ")" << std::endl;
            return true;
        }
    }

    // Fallback to RGB conversion
    if (InitializeRGBFallback(renderer, width, height)) {
        mode_ = YUVRendererMode::RGB_FALLBACK;
        std::cout << "⚠️  [YUV RENDERER] Initialized in RGB FALLBACK mode ("
                  << width << "x" << height << ")" << std::endl;
        return true;
    }

    std::cerr << "❌ [YUV RENDERER] Failed to initialize" << std::endl;
    return false;
}

bool FSTPYUVRenderer::InitializeNativeYUV(SDL_Renderer* renderer, int width, int height, Uint32 sdl_format) {
    // CRITICAL: Use format that matches decoder
    // GPU (VideoToolbox): NV12 (format=23, Y + interleaved UV)
    // CPU (ffmpeg software): YUV420P/IYUV (format=0, Y + separate U + V)
    texture_format_ = sdl_format;

    const char* format_name = (sdl_format == SDL_PIXELFORMAT_NV12) ? "NV12" :
                              (sdl_format == SDL_PIXELFORMAT_IYUV) ? "IYUV" : "Unknown";

    // Create texture with specified format
    texture_ = SDL_CreateTexture(
        renderer,
        texture_format_,
        SDL_TEXTUREACCESS_STREAMING,
        width,
        height
    );

    if (!texture_) {
        std::cerr << "❌ [YUV RENDERER] Failed to create " << format_name << " texture: "
                  << SDL_GetError() << std::endl;
        return false;
    }

    // Enable bilinear filtering
    SDL_SetTextureScaleMode(texture_, SDL_ScaleModeLinear);

    std::cout << "✅ [YUV RENDERER] Created " << format_name << " texture ("
              << width << "x" << height << ")" << std::endl;
    return true;
}

bool FSTPYUVRenderer::InitializeRGBFallback(SDL_Renderer* renderer, int width, int height) {
    // Create RGB texture
    texture_ = SDL_CreateTexture(
        renderer,
        SDL_PIXELFORMAT_RGB24,
        SDL_TEXTUREACCESS_STREAMING,
        width,
        height
    );

    if (!texture_) {
        std::cerr << "❌ [YUV RENDERER] Failed to create RGB texture: "
                  << SDL_GetError() << std::endl;
        return false;
    }

    SDL_SetTextureScaleMode(texture_, SDL_ScaleModeLinear);

    // Create buffer for RGB data
    rgb_buffer_size_ = width * height * 3; // RGB24
    rgb_buffer_ = new uint8_t[rgb_buffer_size_];

    // Create SwsContext for YUV→RGB conversion
    sws_ctx_ = sws_getContext(
        width, height, AV_PIX_FMT_YUV420P,
        width, height, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );

    if (!sws_ctx_) {
        std::cerr << "❌ [YUV RENDERER] Failed to create SwsContext" << std::endl;
        delete[] rgb_buffer_;
        rgb_buffer_ = nullptr;
        return false;
    }

    return true;
}

bool FSTPYUVRenderer::UpdateYUVTexture(const YUVPlanes& planes) {
    if (!texture_) {
        return false;
    }

    if (mode_ == YUVRendererMode::NATIVE_YUV) {
        return UpdateYUVNative(planes);
    } else {
        return UpdateYUVFallback(planes);
    }
}

bool FSTPYUVRenderer::UpdateYUVNative(const YUVPlanes& planes) {
    FSTP_COUNT_CALL("UpdateYUVNative");
    FSTP_PROFILE_TEXTURE_BEGIN("update_yuv_native");

    // CRITICAL: Switch SDL YUV conversion mode depending on color_range
    // Proxy: full range (0-255) → SDL_YUV_CONVERSION_JPEG
    // Full-res: limited range (16-235) → SDL_YUV_CONVERSION_BT709
    if (planes.is_full_range) {
        SDL_SetYUVConversionMode(SDL_YUV_CONVERSION_JPEG);
    } else {
        SDL_SetYUVConversionMode(SDL_YUV_CONVERSION_BT709);
    }

    int result;
    if (texture_format_ == SDL_PIXELFORMAT_NV12) {
        // NV12 format: Y plane + interleaved UV plane
        result = SDL_UpdateNVTexture(
            texture_,
            nullptr,  // Update full texture
            planes.y_plane, planes.y_pitch,
            planes.u_plane, planes.u_pitch  // UV interleaved plane
        );
    } else {
        // IYUV/YV12 format: Y + U + V separate planes
        result = SDL_UpdateYUVTexture(
            texture_,
            nullptr,  // Update full texture
            planes.y_plane, planes.y_pitch,
            planes.u_plane, planes.u_pitch,
            planes.v_plane, planes.v_pitch
        );
    }

    FSTP_PROFILE_TEXTURE_END("update_yuv_native");

    if (result != 0) {
        std::cerr << "❌ [YUV RENDERER] SDL_UpdateNVTexture/YUVTexture failed: "
                  << SDL_GetError() << std::endl;
        return false;
    }

    return true;
}

bool FSTPYUVRenderer::UpdateYUVFallback(const YUVPlanes& planes) {
    FSTP_PROFILE_TEXTURE_BEGIN("update_yuv_fallback");

    if (!sws_ctx_ || !rgb_buffer_) {
        return false;
    }

    // Prepare input data for swscale
    const uint8_t* src_data[4] = {
        planes.y_plane,
        planes.u_plane,
        planes.v_plane,
        nullptr
    };

    int src_linesize[4] = {
        planes.y_pitch,
        planes.u_pitch,
        planes.v_pitch,
        0
    };

    // Output data
    uint8_t* dst_data[4] = { rgb_buffer_, nullptr, nullptr, nullptr };
    int dst_linesize[4] = { width_ * 3, 0, 0, 0 };

    // Conversion YUV→RGB
    FSTP_PROFILE_SWSCALE_BEGIN(width_, height_);
    sws_scale(sws_ctx_, src_data, src_linesize, 0, height_, dst_data, dst_linesize);
    FSTP_PROFILE_SWSCALE_END();

    // Update texture
    int result = SDL_UpdateTexture(texture_, nullptr, rgb_buffer_, width_ * 3);

    FSTP_PROFILE_TEXTURE_END("update_yuv_fallback");

    if (result != 0) {
        std::cerr << "❌ [YUV RENDERER] SDL_UpdateTexture failed: "
                  << SDL_GetError() << std::endl;
        return false;
    }

    return true;
}

void FSTPYUVRenderer::Cleanup() {
    // CRITICAL: DO NOT delete texture_ here!
    // Ownership of texture belongs to WindowManager (g_windows[i].texture_buffer)
    // WindowManager will delete texture when it gets a new one (line 601 WindowManager.cpp)
    // If we delete here, WindowManager will try to delete already deleted memory → CRASH
    texture_ = nullptr;

    if (sws_ctx_) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }

    if (rgb_buffer_) {
        delete[] rgb_buffer_;
        rgb_buffer_ = nullptr;
        rgb_buffer_size_ = 0;
    }

    renderer_ = nullptr;
    width_ = 0;
    height_ = 0;
}

bool FSTPYUVRenderer::IsYUVSupported(SDL_Renderer* renderer) {
    if (!renderer) {
        return false;
    }

    // Check renderer information
    SDL_RendererInfo info;
    if (SDL_GetRendererInfo(renderer, &info) != 0) {
        return false;
    }

   // std::cout << "🎮 [YUV CAPS] Renderer: " << info.name << std::endl;
    // std::cout << "  Hardware accelerated: " << (info.flags & SDL_RENDERER_ACCELERATED ? "YES" : "NO") << std::endl;
    // std::cout << "  Supported formats (" << info.num_texture_formats << "):" << std::endl;

    // Check support for YUV formats
    bool yuv_supported = false;
    for (Uint32 i = 0; i < info.num_texture_formats; i++) {
        Uint32 format = info.texture_formats[i];

        // Check YUV formats
        bool is_yuv = (format == SDL_PIXELFORMAT_IYUV ||
                       format == SDL_PIXELFORMAT_YV12 ||
                       format == SDL_PIXELFORMAT_NV12 ||
                       format == SDL_PIXELFORMAT_NV21);

        // Debug output removed - only enable if debugging YUV support issues
        // const char* format_name = SDL_GetPixelFormatName(format);
        // std::cout << "    [" << i << "] " << format_name
        //           << (is_yuv ? " ✅ YUV" : "") << std::endl;

        if (is_yuv) {
            yuv_supported = true;
        }
    }

    return yuv_supported;

    return false;
}

Uint32 GetOptimalYUVFormat() {
    // IYUV (YUV420P) - most universal format
    // Supported on:
    // - macOS (Metal)
    // - Linux (OpenGL, Vulkan)
    // - Windows (Direct3D)

    // Priority order:
    // 1. IYUV - YUV 4:2:0 planar (Y, U, V separately)
    // 2. YV12 - YUV 4:2:0 planar (Y, V, U order)
    // 3. NV12 - YUV 4:2:0 semi-planar (Y separately, UV interleaved)

    return SDL_PIXELFORMAT_IYUV;
}

YUVCapabilities QueryYUVCapabilities(SDL_Renderer* renderer) {
    YUVCapabilities caps = {};

    if (!renderer) {
        return caps;
    }

    SDL_RendererInfo info;
    if (SDL_GetRendererInfo(renderer, &info) != 0) {
        return caps;
    }

    // Check hardware acceleration
    caps.hardware_acceleration = (info.flags & SDL_RENDERER_ACCELERATED) != 0;

    // Check supported formats
    for (Uint32 i = 0; i < info.num_texture_formats; i++) {
        Uint32 format = info.texture_formats[i];

        switch (format) {
            case SDL_PIXELFORMAT_IYUV:
                caps.supports_iyuv = true;
                break;
            case SDL_PIXELFORMAT_YV12:
                caps.supports_yv12 = true;
                break;
            case SDL_PIXELFORMAT_NV12:
                caps.supports_nv12 = true;
                break;
            case SDL_PIXELFORMAT_NV21:
                caps.supports_nv21 = true;
                break;
        }
    }

    // Log capabilities
    static bool logged = false;
    if (!logged) {
        std::cout << "🎮 [YUV CAPS] Renderer: " << info.name << std::endl;
        std::cout << "  Hardware accelerated: " << (caps.hardware_acceleration ? "YES" : "NO") << std::endl;
        std::cout << "  IYUV (YUV420P): " << (caps.supports_iyuv ? "✅" : "❌") << std::endl;
        std::cout << "  YV12: " << (caps.supports_yv12 ? "✅" : "❌") << std::endl;
        std::cout << "  NV12: " << (caps.supports_nv12 ? "✅" : "❌") << std::endl;
        std::cout << "  NV21: " << (caps.supports_nv21 ? "✅" : "❌") << std::endl;
        logged = true;
    }

    return caps;
}