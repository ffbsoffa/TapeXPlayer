#ifndef FSTP_YUV_RENDERER_H
#define FSTP_YUV_RENDERER_H

#include <SDL2/SDL.h>
#include <cstdint>

/**
 * Cross-platform YUV texture renderer
 * Supports:
 * - macOS (VideoToolbox → YUV420P → SDL YUV texture)
 * - Linux (VA-API → YUV420P → SDL YUV texture)
 * - RGB fallback for older systems
 */

enum class YUVRendererMode {
    NATIVE_YUV,     // Direct YUV texture (recommended)
    RGB_FALLBACK    // RGB conversion (for compatibility)
};

struct YUVPlanes {
    const uint8_t* y_plane;
    const uint8_t* u_plane;
    const uint8_t* v_plane;
    int y_pitch;
    int u_pitch;
    int v_pitch;
    int width;
    int height;
    bool is_full_range;  // true = 0-255 (JPEG/proxy), false = 16-235 (TV/BT.709)
    int format;  // AVPixelFormat (AV_PIX_FMT_YUV420P, AV_PIX_FMT_NV12, etc.)
};

class FSTPYUVRenderer {
public:
    FSTPYUVRenderer();
    ~FSTPYUVRenderer();

    // Initialize renderer with auto-detection of best mode
    bool Initialize(SDL_Renderer* renderer, int width, int height, Uint32 sdl_format = SDL_PIXELFORMAT_NV12);

    // Update YUV texture
    bool UpdateYUVTexture(const YUVPlanes& planes);

    // Get texture for rendering
    SDL_Texture* GetTexture() const { return texture_; }

    // Get current rendering mode
    YUVRendererMode GetMode() const { return mode_; }

    // Get current texture size
    void GetSize(int& width, int& height) const {
        width = width_;
        height = height_;
    }

    // Get texture format
    Uint32 GetTextureFormat() const { return texture_format_; }

    // Check native YUV support
    static bool IsYUVSupported(SDL_Renderer* renderer);

    // Release resources
    void Cleanup();

private:
    bool InitializeNativeYUV(SDL_Renderer* renderer, int width, int height, Uint32 sdl_format);
    bool InitializeRGBFallback(SDL_Renderer* renderer, int width, int height);

    bool UpdateYUVNative(const YUVPlanes& planes);
    bool UpdateYUVFallback(const YUVPlanes& planes);

    SDL_Renderer* renderer_;
    SDL_Texture* texture_;
    YUVRendererMode mode_;
    Uint32 texture_format_;  // SDL_PIXELFORMAT_IYUV or SDL_PIXELFORMAT_NV12

    int width_;
    int height_;

    // For RGB fallback
    struct SwsContext* sws_ctx_;
    uint8_t* rgb_buffer_;
    size_t rgb_buffer_size_;
    int current_sws_format_;  // Track current AVPixelFormat for SwsContext
};

// Global function for determining optimal YUV format for platform
Uint32 GetOptimalYUVFormat();

// Check capabilities of renderer
struct YUVCapabilities {
    bool supports_iyuv;      // SDL_PIXELFORMAT_IYUV
    bool supports_yv12;      // SDL_PIXELFORMAT_YV12
    bool supports_nv12;      // SDL_PIXELFORMAT_NV12 (Windows/Xbox)
    bool supports_nv21;      // SDL_PIXELFORMAT_NV21 (Android)
    bool hardware_acceleration;
};

YUVCapabilities QueryYUVCapabilities(SDL_Renderer* renderer);

#endif // FSTP_YUV_RENDERER_H