#pragma once

#include <SDL2/SDL.h>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

#include <memory>

// AVFrame to SDL_Texture converter for displaying decoded frames
class FSTPFrameConverter {
private:
    SwsContext* m_sws_context;
    SDL_Renderer* m_renderer;

    // Last known parameters for context reuse
    int m_last_src_width;
    int m_last_src_height;
    AVPixelFormat m_last_src_format;

    // Buffer for converted data
    uint8_t* m_rgb_buffer;
    int m_rgb_buffer_size;

public:
    FSTPFrameConverter();
    ~FSTPFrameConverter();

    // Initialize with renderer
    bool Initialize(SDL_Renderer* renderer);
    void Shutdown();

    // Convert AVFrame to SDL_Texture
    SDL_Texture* ConvertFrameToTexture(AVFrame* frame);

    // Check readiness
    bool IsReady() const { return m_renderer != nullptr; }

    // Get last frame information
    void GetLastFrameInfo(int& width, int& height) const;

private:
    // Internal methods
    bool UpdateSwsContext(int src_width, int src_height, AVPixelFormat src_format);
    bool AllocateRGBBuffer(int width, int height);
    void FreeRGBBuffer();
};

// Structure for passing texture information
struct FrameTextureInfo {
    SDL_Texture* texture;
    int width;
    int height;
    Uint32 format;
    double timestamp;
    int frame_number;
    bool is_valid;
    bool is_iframe;  // Flag indicating this is an I-frame

    FrameTextureInfo()
        : texture(nullptr), width(0), height(0), format(0)
        , timestamp(0.0), frame_number(-1), is_valid(false), is_iframe(false) {}
};