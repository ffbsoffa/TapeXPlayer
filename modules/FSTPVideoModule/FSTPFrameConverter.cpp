#include "FSTPFrameConverter.h"
#include <iostream>

FSTPFrameConverter::FSTPFrameConverter()
    : m_sws_context(nullptr)
    , m_renderer(nullptr)
    , m_last_src_width(0)
    , m_last_src_height(0)
    , m_last_src_format(AV_PIX_FMT_NONE)
    , m_rgb_buffer(nullptr)
    , m_rgb_buffer_size(0) {
}

FSTPFrameConverter::~FSTPFrameConverter() {
    Shutdown();
}

bool FSTPFrameConverter::Initialize(SDL_Renderer* renderer) {
    if (!renderer) {
        std::cerr << "🖼️ [FRAME CONVERTER] Invalid renderer provided" << std::endl;
        return false;
    }

    m_renderer = renderer;
    std::cout << "🖼️ [FRAME CONVERTER] Initialized with SDL renderer" << std::endl;
    return true;
}

void FSTPFrameConverter::Shutdown() {
    if (m_sws_context) {
        sws_freeContext(m_sws_context);
        m_sws_context = nullptr;
    }

    FreeRGBBuffer();
    m_renderer = nullptr;

    // Reset state
    m_last_src_width = 0;
    m_last_src_height = 0;
    m_last_src_format = AV_PIX_FMT_NONE;

    std::cout << "🖼️ [FRAME CONVERTER] Shutdown complete" << std::endl;
}

SDL_Texture* FSTPFrameConverter::ConvertFrameToTexture(AVFrame* frame) {
    if (!m_renderer || !frame) {
        std::cerr << "🖼️ [FRAME CONVERTER] Invalid renderer or frame" << std::endl;
        return nullptr;
    }

    // Check if frame is valid
    if (frame->width <= 0 || frame->height <= 0 || !frame->data[0]) {
        std::cerr << "🖼️ [FRAME CONVERTER] Invalid frame dimensions or data" << std::endl;
        return nullptr;
    }

    int src_width = frame->width;
    int src_height = frame->height;
    AVPixelFormat src_format = static_cast<AVPixelFormat>(frame->format);

    // Update SwsContext if needed
    if (!UpdateSwsContext(src_width, src_height, src_format)) {
        std::cerr << "🖼️ [FRAME CONVERTER] Failed to update SwsContext" << std::endl;
        return nullptr;
    }

    // Allocate buffer for RGB data if needed
    if (!AllocateRGBBuffer(src_width, src_height)) {
        std::cerr << "🖼️ [FRAME CONVERTER] Failed to allocate RGB buffer" << std::endl;
        return nullptr;
    }

    // Convert frame to RGB
    uint8_t* dst_data[4] = { m_rgb_buffer, nullptr, nullptr, nullptr };
    int dst_linesize[4] = { src_width * 3, 0, 0, 0 }; // RGB24 = 3 bytes per pixel

    int result = sws_scale(m_sws_context,
                          frame->data, frame->linesize,
                          0, src_height,
                          dst_data, dst_linesize);

    if (result <= 0) {
        std::cerr << "🖼️ [FRAME CONVERTER] Failed to scale frame" << std::endl;
        return nullptr;
    }

    // Create SDL_Texture from RGB data (STATIC to avoid Metal problems)
    SDL_Texture* texture = SDL_CreateTexture(m_renderer,
                                           SDL_PIXELFORMAT_RGB24,
                                           SDL_TEXTUREACCESS_STATIC,
                                           src_width, src_height);

    if (!texture) {
        std::cerr << "🖼️ [FRAME CONVERTER] Failed to create SDL texture: " << SDL_GetError() << std::endl;
        return nullptr;
    }

    // Use SDL_UpdateTexture instead of Lock/Unlock for safety
    int expected_pitch = src_width * 3; // RGB24 = 3 bytes per pixel
    
    // Check if data is correct
    if (!m_rgb_buffer || m_rgb_buffer_size < expected_pitch * src_height) {
        std::cerr << "🖼️ [FRAME CONVERTER] Invalid RGB buffer size" << std::endl;
        SDL_DestroyTexture(texture);
        return nullptr;
    }
    
    int update_result = SDL_UpdateTexture(texture, nullptr, m_rgb_buffer, expected_pitch);
    if (update_result < 0) {
        std::cerr << "🖼️ [FRAME CONVERTER] Failed to update texture: " << SDL_GetError() << std::endl;
        SDL_DestroyTexture(texture);
        return nullptr;
    }

    // Set scaling mode for better quality
    SDL_SetTextureScaleMode(texture, SDL_ScaleModeBest);

    // Reduced logging to avoid spam
    static int success_count = 0;
    success_count++;
    if (success_count <= 5 || success_count % 100 == 0) {
        std::cout << "🖼️ [FRAME CONVERTER] Successfully converted " << src_width << "x" << src_height
                  << " frame to SDL texture (#" << success_count << ")" << std::endl;
    }

    return texture;
}

bool FSTPFrameConverter::UpdateSwsContext(int src_width, int src_height, AVPixelFormat src_format) {
    // Check if we need to update context
    if (m_sws_context &&
        m_last_src_width == src_width &&
        m_last_src_height == src_height &&
        m_last_src_format == src_format) {
        return true; // Context is already ready
    }

    // Free old context
    if (m_sws_context) {
        sws_freeContext(m_sws_context);
        m_sws_context = nullptr;
    }

    // Create new context for conversion to RGB24
    m_sws_context = sws_getContext(
        src_width, src_height, src_format,      // Source parameters
        src_width, src_height, AV_PIX_FMT_RGB24, // Target parameters
        SWS_BILINEAR,                           // Scaling algorithm
        nullptr, nullptr, nullptr               // Additional parameters
    );

    if (!m_sws_context) {
        std::cerr << "🖼️ [FRAME CONVERTER] Failed to create SwsContext for format "
                  << av_get_pix_fmt_name(src_format) << std::endl;
        return false;
    }

        // CRITICAL: Set correct color space handling
    // By default we use BT.709 (HD) and FULL range (0-255) for modern content
    const int* srcCoef = (src_width >= 1280 || src_height >= 720)
        ? sws_getCoefficients(SWS_CS_ITU709)   // HD: BT.709
        : sws_getCoefficients(SWS_CS_ITU601);  // SD: BT.601
    const int* dstCoef = sws_getCoefficients(SWS_CS_DEFAULT); // RGB

    int srcRange = 1;  // FULL range (0-255) - modern cameras and recorders
    int dstRange = 1;  // RGB always FULL (0-255)

    sws_setColorspaceDetails(
        m_sws_context,
        srcCoef, srcRange,
        dstCoef, dstRange,
        0, (1<<16), (1<<16)
    );

    // Save parameters
    m_last_src_width = src_width;
    m_last_src_height = src_height;
    m_last_src_format = src_format;

    std::cout << "🖼️ [FRAME CONVERTER] Created SwsContext: "
              << src_width << "x" << src_height << " "
              << av_get_pix_fmt_name(src_format) << " -> RGB24"
              << " (matrix=" << ((src_width >= 1280 || src_height >= 720) ? "BT709" : "BT601")
              << ", range=FULL)" << std::endl;

    return true;
}

bool FSTPFrameConverter::AllocateRGBBuffer(int width, int height) {
    int required_size = width * height * 3; // RGB24 = 3 bytes per pixel

    // Check if we need to reallocate buffer
    if (m_rgb_buffer && m_rgb_buffer_size >= required_size) {
        return true; // Buffer is already large enough
    }

    // Free old buffer
    FreeRGBBuffer();

    // Allocate new buffer
    m_rgb_buffer = static_cast<uint8_t*>(av_malloc(required_size));
    if (!m_rgb_buffer) {
        std::cerr << "🖼️ [FRAME CONVERTER] Failed to allocate RGB buffer of size " << required_size << std::endl;
        m_rgb_buffer_size = 0;
        return false;
    }

    m_rgb_buffer_size = required_size;
    std::cout << "🖼️ [FRAME CONVERTER] Allocated RGB buffer: " << required_size << " bytes" << std::endl;

    return true;
}

void FSTPFrameConverter::FreeRGBBuffer() {
    if (m_rgb_buffer) {
        av_free(m_rgb_buffer);
        m_rgb_buffer = nullptr;
        m_rgb_buffer_size = 0;
    }
}

void FSTPFrameConverter::GetLastFrameInfo(int& width, int& height) const {
    width = m_last_src_width;
    height = m_last_src_height;
}