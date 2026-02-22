#include "FSTPScreenshot.h"
#include <iostream>
#include <cstring>
#include <algorithm>
#include <vector>
#ifndef _WIN32
#include <sys/mman.h>
#include <unistd.h>
#endif

extern "C" {
#include <libswscale/swscale.h>
}

// Platform-specific clipboard function (implemented separately for each platform)
extern "C" bool CopyImageToClipboard(const uint8_t* rgb_data, int width, int height);

// Bitmap font data for Tamsyn 8x16 style (digits and colon only)
static const uint8_t bitmap_font_8x16[][16] = {
    // '0' (ASCII 48)
    {0x3C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C},
    // '1' (ASCII 49)
    {0x18, 0x18, 0x38, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x7E},
    // '2' (ASCII 50)
    {0x3C, 0x66, 0x66, 0x06, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x66, 0x7E},
    // '3' (ASCII 51)
    {0x3C, 0x66, 0x66, 0x06, 0x06, 0x06, 0x1C, 0x06, 0x06, 0x06, 0x06, 0x06, 0x66, 0x66, 0x66, 0x3C},
    // '4' (ASCII 52)
    {0x0C, 0x1C, 0x3C, 0x6C, 0x6C, 0x6C, 0x6C, 0x6C, 0x6C, 0x7E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C},
    // '5' (ASCII 53)
    {0x7E, 0x60, 0x60, 0x60, 0x60, 0x60, 0x7C, 0x06, 0x06, 0x06, 0x06, 0x06, 0x66, 0x66, 0x66, 0x3C},
    // '6' (ASCII 54)
    {0x3C, 0x66, 0x66, 0x60, 0x60, 0x60, 0x7C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C},
    // '7' (ASCII 55)
    {0x7E, 0x66, 0x06, 0x06, 0x0C, 0x0C, 0x18, 0x18, 0x18, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30},
    // '8' (ASCII 56)
    {0x3C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C},
    // '9' (ASCII 57)
    {0x3C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3E, 0x06, 0x06, 0x66, 0x66, 0x66, 0x3C},
    // ':' (ASCII 58)
    {0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00}
};

// Get bitmap data for a character
static const uint8_t* GetBitmapChar(char c) {
    if (c >= '0' && c <= '9') {
        return bitmap_font_8x16[c - '0'];
    } else if (c == ':') {
        return bitmap_font_8x16[10];
    }
    return nullptr;
}

// Render a single bitmap character onto YUV420P buffer
static bool RenderBitmapChar(uint8_t* y_plane, int stride, int width, int height,
                            char c, int x, int y, uint8_t brightness = 255) {
    const uint8_t* charData = GetBitmapChar(c);
    if (!charData) {
        return false;
    }

    // Render 8x16 character
    for (int row = 0; row < 16; row++) {
        if (y + row >= height) break;

        uint8_t rowData = charData[row];
        for (int col = 0; col < 8; col++) {
            if (x + col >= width) break;

            // Check if pixel is set
            if (rowData & (0x80 >> col)) {
                int frame_y = y + row;
                int frame_x = x + col;

                // Set pixel on Y plane (luminance)
                if (frame_x < stride && frame_y < height) {
                    y_plane[frame_y * stride + frame_x] = brightness;
                }
            }
        }
    }

    return true;
}

// Render bitmap text string onto YUV buffer
static bool RenderBitmapText(uint8_t* y_plane, int stride, int width, int height,
                            const std::string& text, int x, int y, uint8_t brightness = 255) {
    int currentX = x;

    for (char c : text) {
        if (RenderBitmapChar(y_plane, stride, width, height, c, currentX, y, brightness)) {
            currentX += 8;
        } else {
            currentX += 8; // Skip unsupported characters
        }
    }

    return true;
}

// Render timecode with dark background
static void RenderTimecode(uint8_t* y_plane, int stride, int width, int height,
                          const std::string& timecode, int x, int y) {
    // Calculate text dimensions
    int text_width = timecode.length() * 8;
    int text_height = 16;

    // Clamp to frame bounds
    x = std::max(0, std::min(x, width - text_width));
    y = std::max(0, std::min(y, height - text_height));

    std::cout << "📝 [SCREENSHOT] Rendering timecode '" << timecode << "' at (" << x << "," << y << ")" << std::endl;

    // Create dark background
    for (int bg_y = y - 2; bg_y < y + text_height + 2; bg_y++) {
        for (int bg_x = x - 2; bg_x < x + text_width + 2; bg_x++) {
            if (bg_x >= 0 && bg_x < width && bg_y >= 0 && bg_y < height && bg_x < stride) {
                y_plane[bg_y * stride + bg_x] = 32; // Dark background
            }
        }
    }

    // Render white text
    RenderBitmapText(y_plane, stride, width, height, timecode, x, y, 255);
}

bool TakeScreenshotFromPixelBuffer(
    const uint8_t* yuv_data,
    int width,
    int height,
    const std::string& timecode,
    int window_width,
    int window_height,
    bool is_zoom_enabled,
    float zoom_factor,
    float zoom_center_x,
    float zoom_center_y,
    bool show_thumbnail)
{
    if (!yuv_data || width <= 0 || height <= 0) {
        std::cerr << "❌ [SCREENSHOT] Invalid input data" << std::endl;
        return false;
    }

    std::cout << "📸 [SCREENSHOT] Taking screenshot with timecode: " << timecode << std::endl;
    std::cout << "📸 [SCREENSHOT] Source: " << width << "x" << height << " (YUV420P)" << std::endl;

    // Debug: Check if source data already has black stripe
    const uint8_t* check_y = yuv_data;
    bool has_black_stripe = true;
    for (int y = 0; y < std::min(height, 10); y++) {
        // Check rightmost 10 pixels
        for (int x = width - 10; x < width; x++) {
            if (check_y[y * width + x] > 20) {  // Not black
                has_black_stripe = false;
                break;
            }
        }
        if (!has_black_stripe) break;
    }
    if (has_black_stripe) {
        std::cout << "⚠️ [SCREENSHOT] Source data already has black stripe on right edge!" << std::endl;
    }

    // Calculate aspect ratio and final dimensions (480p)
    float aspect_ratio = (float)width / (float)height;
    int final_width, final_height;

    if (aspect_ratio >= 1.0f) {
        final_height = 480;
        final_width = (int)(final_height * aspect_ratio);
    } else {
        final_width = 480;
        final_height = (int)(final_width / aspect_ratio);
    }

    // Ensure even dimensions AND that width/2 and height/2 are also even (divisible by 4)
    // This is critical for YUV420P where U/V planes are half resolution
    // Round DOWN to avoid adding black padding
    final_width = final_width & ~3;  // Round DOWN to multiple of 4
    final_height = final_height & ~3;

    std::cout << "📸 [SCREENSHOT] Output: " << final_width << "x" << final_height << std::endl;

    // Calculate YUV420P buffer sizes
    int y_size = width * height;
    int uv_size = (width / 2) * (height / 2);

    // Setup source YUV420P planes
    const uint8_t* src_y = yuv_data;
    const uint8_t* src_u = yuv_data + y_size;
    const uint8_t* src_v = yuv_data + y_size + uv_size;

    // Create RGB output buffer
    size_t rgb_buffer_size = final_width * final_height * 3;
#ifdef _WIN32
    // On Windows there is no GTK, so malloc is safe
    void* rgb_mmap = malloc(rgb_buffer_size);
    if (rgb_mmap == nullptr) {
#else
    void* rgb_mmap = mmap(nullptr, rgb_buffer_size,
                          PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS,
                          -1, 0);
    if (rgb_mmap == MAP_FAILED) {
#endif
        std::cerr << "❌ [SCREENSHOT] Failed to allocate RGB buffer" << std::endl;
        return false;
    }

    uint8_t* rgb_data = (uint8_t*)rgb_mmap;
    std::cout << "📸 [SCREENSHOT] Allocated " << rgb_buffer_size << " bytes via mmap for RGB data" << std::endl;

    // Direct YUV -> RGB conversion in one pass
    SwsContext* sws_ctx = nullptr;
    const uint8_t* src_data[4] = {nullptr};
    int src_linesize[4] = {0};

    if (is_zoom_enabled && zoom_factor > 1.0f) {
        // Calculate zoomed source rectangle
        int src_w = (int)(width / zoom_factor);
        int src_h = (int)(height / zoom_factor);

        // Ensure even dimensions for YUV420P
        // Round DOWN to avoid black padding
        src_w = src_w & ~1;
        src_h = src_h & ~1;

        int src_x = (int)(width * zoom_center_x) - src_w / 2;
        int src_y_offset = (int)(height * zoom_center_y) - src_h / 2;

        // Ensure even offsets for YUV420P alignment
        src_x = src_x & ~1;
        src_y_offset = src_y_offset & ~1;

        // Clamp to bounds
        if (src_x < 0) src_x = 0;
        if (src_y_offset < 0) src_y_offset = 0;
        if (src_x + src_w > width) src_x = (width - src_w) & ~1;
        if (src_y_offset + src_h > height) src_y_offset = (height - src_h) & ~1;

        std::cout << "🔍 [SCREENSHOT] Zoom " << zoom_factor << "x at ("
                  << zoom_center_x << "," << zoom_center_y << "), cropping to "
                  << src_x << "," << src_y_offset << " " << src_w << "x" << src_h << std::endl;

        // Adjust source pointers for crop
        src_data[0] = src_y + src_y_offset * width + src_x;
        src_data[1] = src_u + (src_y_offset/2) * (width/2) + (src_x/2);
        src_data[2] = src_v + (src_y_offset/2) * (width/2) + (src_x/2);
        src_linesize[0] = width;
        src_linesize[1] = width / 2;
        src_linesize[2] = width / 2;

        sws_ctx = sws_getContext(
            src_w, src_h, AV_PIX_FMT_YUV420P,
            final_width, final_height, AV_PIX_FMT_RGB24,
            SWS_BILINEAR, nullptr, nullptr, nullptr
        );

        if (sws_ctx) {
            uint8_t* rgb_dst[4] = {rgb_data, nullptr, nullptr, nullptr};
            int rgb_linesize[4] = {final_width * 3, 0, 0, 0};
            sws_scale(sws_ctx, src_data, src_linesize, 0, src_h, rgb_dst, rgb_linesize);
        }
    } else {
        // No zoom - simple scale
        src_data[0] = src_y;
        src_data[1] = src_u;
        src_data[2] = src_v;
        src_linesize[0] = width;
        src_linesize[1] = width / 2;
        src_linesize[2] = width / 2;

        sws_ctx = sws_getContext(
            width, height, AV_PIX_FMT_YUV420P,
            final_width, final_height, AV_PIX_FMT_RGB24,
            SWS_BILINEAR, nullptr, nullptr, nullptr
        );

        if (sws_ctx) {
            uint8_t* rgb_dst[4] = {rgb_data, nullptr, nullptr, nullptr};
            int rgb_linesize[4] = {final_width * 3, 0, 0, 0};
            sws_scale(sws_ctx, src_data, src_linesize, 0, height, rgb_dst, rgb_linesize);
        }
    }

    if (sws_ctx) {
        sws_freeContext(sws_ctx);
    }

    std::cout << "✅ [SCREENSHOT] Direct YUV->RGB conversion complete: " << final_width << "x" << final_height << std::endl;

    // Render timecode overlay in RGB (top-left corner)
    int timecode_x = 10;
    int timecode_y = 10;

    // Dark background for timecode
    int text_width = timecode.length() * 8;
    int text_height = 16;
    for (int y = timecode_y - 2; y < timecode_y + text_height + 2; y++) {
        for (int x = timecode_x - 2; x < timecode_x + text_width + 2; x++) {
            if (x >= 0 && x < final_width && y >= 0 && y < final_height) {
                uint8_t* pixel = rgb_data + (y * final_width + x) * 3;
                pixel[0] = pixel[1] = pixel[2] = 32; // Dark gray
            }
        }
    }

    // White text
    for (size_t i = 0; i < timecode.length(); i++) {
        const uint8_t* charData = GetBitmapChar(timecode[i]);
        if (!charData) continue;

        for (int row = 0; row < 16; row++) {
            if (timecode_y + row >= final_height) break;

            uint8_t rowData = charData[row];
            for (int col = 0; col < 8; col++) {
                int px = timecode_x + i * 8 + col;
                int py = timecode_y + row;

                if (px >= final_width) break;
                if (rowData & (0x80 >> col)) {
                    uint8_t* pixel = rgb_data + (py * final_width + px) * 3;
                    pixel[0] = pixel[1] = pixel[2] = 255; // White
                }
            }
        }
    }

    std::cout << "📝 [SCREENSHOT] Rendered timecode '" << timecode << "' at (" << timecode_x << "," << timecode_y << ")" << std::endl;

    // Render thumbnail if zoom is enabled
    if (is_zoom_enabled && show_thumbnail && zoom_factor > 1.0f) {
        float aspect_ratio = (float)width / (float)height;
        int thumb_width = std::min(180, static_cast<int>(final_width * 0.15f));
        int thumb_height = (int)(thumb_width / aspect_ratio);
        // Round DOWN to avoid black padding
        thumb_width = thumb_width & ~1;
        thumb_height = thumb_height & ~1;

        // Create thumbnail RGB buffer
        std::vector<uint8_t> thumb_rgb(thumb_width * thumb_height * 3);

        // Scale full source to thumbnail using direct YUV->RGB
        const uint8_t* thumb_src[4] = {src_y, src_u, src_v, nullptr};
        int thumb_src_linesize[4] = {width, width/2, width/2, 0};

        SwsContext* thumb_ctx = sws_getContext(
            width, height, AV_PIX_FMT_YUV420P,
            thumb_width, thumb_height, AV_PIX_FMT_RGB24,
            SWS_BILINEAR, nullptr, nullptr, nullptr
        );

        if (thumb_ctx) {
            uint8_t* thumb_dst[4] = {thumb_rgb.data(), nullptr, nullptr, nullptr};
            int thumb_linesize[4] = {thumb_width * 3, 0, 0, 0};
            sws_scale(thumb_ctx, thumb_src, thumb_src_linesize, 0, height, thumb_dst, thumb_linesize);
            sws_freeContext(thumb_ctx);

            // Draw white border (2px)
            uint8_t* thumb_data = thumb_rgb.data();
            for (int y = 0; y < thumb_height; y++) {
                for (int x = 0; x < thumb_width; x++) {
                    if (y < 2 || y >= thumb_height - 2 || x < 2 || x >= thumb_width - 2) {
                        uint8_t* pixel = thumb_data + (y * thumb_width + x) * 3;
                        pixel[0] = pixel[1] = pixel[2] = 255; // White
                    }
                }
            }

            // Draw red zoom rectangle
            int rect_w = (int)(thumb_width / zoom_factor);
            int rect_h = (int)(thumb_height / zoom_factor);
            int rect_x = (int)(thumb_width * zoom_center_x) - rect_w / 2;
            int rect_y = (int)(thumb_height * zoom_center_y) - rect_h / 2;

            // Clamp rectangle
            if (rect_x < 0) rect_x = 0;
            if (rect_y < 0) rect_y = 0;
            if (rect_x + rect_w > thumb_width) rect_x = thumb_width - rect_w;
            if (rect_y + rect_h > thumb_height) rect_y = thumb_height - rect_h;

            // Draw red rectangle outline (1px)
            for (int x = rect_x; x < rect_x + rect_w; x++) {
                if (x >= 0 && x < thumb_width) {
                    // Top edge
                    if (rect_y >= 0 && rect_y < thumb_height) {
                        uint8_t* pixel = thumb_data + (rect_y * thumb_width + x) * 3;
                        pixel[0] = 255; pixel[1] = 0; pixel[2] = 0; // Red
                    }
                    // Bottom edge
                    if (rect_y + rect_h - 1 >= 0 && rect_y + rect_h - 1 < thumb_height) {
                        uint8_t* pixel = thumb_data + ((rect_y + rect_h - 1) * thumb_width + x) * 3;
                        pixel[0] = 255; pixel[1] = 0; pixel[2] = 0; // Red
                    }
                }
            }
            for (int y = rect_y; y < rect_y + rect_h; y++) {
                if (y >= 0 && y < thumb_height) {
                    // Left edge
                    if (rect_x >= 0 && rect_x < thumb_width) {
                        uint8_t* pixel = thumb_data + (y * thumb_width + rect_x) * 3;
                        pixel[0] = 255; pixel[1] = 0; pixel[2] = 0; // Red
                    }
                    // Right edge
                    if (rect_x + rect_w - 1 >= 0 && rect_x + rect_w - 1 < thumb_width) {
                        uint8_t* pixel = thumb_data + (y * thumb_width + rect_x + rect_w - 1) * 3;
                        pixel[0] = 255; pixel[1] = 0; pixel[2] = 0; // Red
                    }
                }
            }

            // Composite thumbnail onto main image (top-right corner)
            int thumb_dst_x = final_width - thumb_width - 10;
            int thumb_dst_y = 10;

            for (int y = 0; y < thumb_height; y++) {
                for (int x = 0; x < thumb_width; x++) {
                    int dst_x = thumb_dst_x + x;
                    int dst_y = thumb_dst_y + y;

                    if (dst_x >= 0 && dst_x < final_width && dst_y >= 0 && dst_y < final_height) {
                        uint8_t* src_pixel = thumb_data + (y * thumb_width + x) * 3;
                        uint8_t* dst_pixel = rgb_data + (dst_y * final_width + dst_x) * 3;
                        dst_pixel[0] = src_pixel[0];
                        dst_pixel[1] = src_pixel[1];
                        dst_pixel[2] = src_pixel[2];
                    }
                }
            }

            std::cout << "🖼️ [SCREENSHOT] Thumbnail added at (" << thumb_dst_x << "," << thumb_dst_y
                      << ") " << thumb_width << "x" << thumb_height << std::endl;
        }
    }

    // Copy to clipboard
    bool success = CopyImageToClipboard(rgb_data, final_width, final_height);

    if (success) {
        std::cout << "✅ [SCREENSHOT] Copied to clipboard: " << final_width << "x" << final_height << std::endl;
    } else {
        std::cerr << "❌ [SCREENSHOT] Failed to copy to clipboard" << std::endl;
    }

    // Free RGB buffer
#ifdef _WIN32
    free(rgb_mmap);
#else
    munmap(rgb_mmap, rgb_buffer_size);
#endif
    std::cout << "📋 [SCREENSHOT] Freed RGB buffer (" << rgb_buffer_size << " bytes)" << std::endl;

    return success;
}

/*
    // OLD YUV OVERLAY CODE - DISABLED FOR NOW
    if (false && is_zoom_enabled && show_thumbnail && zoom_factor > 1.0f) {
        // Match on-screen thumbnail size (15% of width, max 180px)
        int thumb_width = std::min(180, static_cast<int>(final_width * 0.15f));
        int thumb_height = (int)(thumb_width / aspect_ratio);
        thumb_width = (thumb_width + 1) & ~1;
        thumb_height = (thumb_height + 1) & ~1;

        // Allocate thumbnail YUV buffer
        int thumb_y_size = thumb_width * thumb_height;
        int thumb_uv_size = (thumb_width / 2) * (thumb_height / 2);
        std::vector<uint8_t> thumb_yuv(thumb_y_size + thumb_uv_size * 2);

        uint8_t* thumb_y = thumb_yuv.data();
        uint8_t* thumb_u = thumb_yuv.data() + thumb_y_size;
        uint8_t* thumb_v = thumb_yuv.data() + thumb_y_size + thumb_uv_size;

        // Scale full frame to thumbnail
        const uint8_t* thumb_src[4] = {src_y, src_u, src_v, nullptr};
        int thumb_src_linesize[4] = {width, width/2, width/2, 0};
        uint8_t* thumb_dst[4] = {thumb_y, thumb_u, thumb_v, nullptr};
        int thumb_dst_linesize[4] = {thumb_width, thumb_width/2, thumb_width/2, 0};

        SwsContext* thumb_ctx = sws_getContext(
            width, height, AV_PIX_FMT_YUV420P,
            thumb_width, thumb_height, AV_PIX_FMT_YUV420P,
            SWS_BILINEAR, nullptr, nullptr, nullptr
        );

        if (thumb_ctx) {
            sws_scale(thumb_ctx, thumb_src, thumb_src_linesize, 0, height, thumb_dst, thumb_dst_linesize);
            sws_freeContext(thumb_ctx);

            // Draw white border around thumbnail (2px thick)
            int borderThickness = 2;
            for (int x = 0; x < thumb_width; x++) {
                for (int t = 0; t < borderThickness; t++) {
                    if (t < thumb_height) thumb_y[t * thumb_width + x] = 235; // Top
                    if (thumb_height - 1 - t >= 0) thumb_y[(thumb_height - 1 - t) * thumb_width + x] = 235; // Bottom
                }
            }
            for (int y = 0; y < thumb_height; y++) {
                for (int t = 0; t < borderThickness; t++) {
                    if (t < thumb_width) thumb_y[y * thumb_width + t] = 235; // Left
                    if (thumb_width - 1 - t >= 0) thumb_y[y * thumb_width + thumb_width - 1 - t] = 235; // Right
                }
            }

            // Draw red zoom rectangle on thumbnail
            int rect_w = (int)(thumb_width / zoom_factor);
            int rect_h = (int)(thumb_height / zoom_factor);
            int rect_x = (int)(thumb_width * zoom_center_x) - rect_w / 2;
            int rect_y = (int)(thumb_height * zoom_center_y) - rect_h / 2;

            if (rect_x < 0) rect_x = 0;
            if (rect_y < 0) rect_y = 0;
            if (rect_x + rect_w > thumb_width) rect_x = thumb_width - rect_w;
            if (rect_y + rect_h > thumb_height) rect_y = thumb_height - rect_h;

            // Draw red rectangle (Y=76, U=84, V=255 for red in YUV)
            int redThickness = 1;
            for (int x = 0; x < rect_w && (rect_x + x) < thumb_width; x++) {
                for (int t = 0; t < redThickness; t++) {
                    // Top edge
                    if (rect_y + t >= 0 && rect_y + t < thumb_height) {
                        thumb_y[(rect_y + t) * thumb_width + rect_x + x] = 76;
                        if ((rect_y + t) / 2 < thumb_height / 2 && (rect_x + x) / 2 < thumb_width / 2) {
                            thumb_u[((rect_y + t) / 2) * (thumb_width / 2) + (rect_x + x) / 2] = 84;
                            thumb_v[((rect_y + t) / 2) * (thumb_width / 2) + (rect_x + x) / 2] = 255;
                        }
                    }
                    // Bottom edge
                    if (rect_y + rect_h - 1 - t >= 0 && rect_y + rect_h - 1 - t < thumb_height) {
                        thumb_y[(rect_y + rect_h - 1 - t) * thumb_width + rect_x + x] = 76;
                        if ((rect_y + rect_h - 1 - t) / 2 < thumb_height / 2 && (rect_x + x) / 2 < thumb_width / 2) {
                            thumb_u[((rect_y + rect_h - 1 - t) / 2) * (thumb_width / 2) + (rect_x + x) / 2] = 84;
                            thumb_v[((rect_y + rect_h - 1 - t) / 2) * (thumb_width / 2) + (rect_x + x) / 2] = 255;
                        }
                    }
                }
            }
            for (int y = 0; y < rect_h && (rect_y + y) < thumb_height; y++) {
                for (int t = 0; t < redThickness; t++) {
                    // Left edge
                    if (rect_x + t >= 0 && rect_x + t < thumb_width) {
                        thumb_y[(rect_y + y) * thumb_width + rect_x + t] = 76;
                        if ((rect_y + y) / 2 < thumb_height / 2 && (rect_x + t) / 2 < thumb_width / 2) {
                            thumb_u[((rect_y + y) / 2) * (thumb_width / 2) + (rect_x + t) / 2] = 84;
                            thumb_v[((rect_y + y) / 2) * (thumb_width / 2) + (rect_x + t) / 2] = 255;
                        }
                    }
                    // Right edge
                    if (rect_x + rect_w - 1 - t >= 0 && rect_x + rect_w - 1 - t < thumb_width) {
                        thumb_y[(rect_y + y) * thumb_width + rect_x + rect_w - 1 - t] = 76;
                        if ((rect_y + y) / 2 < thumb_height / 2 && (rect_x + rect_w - 1 - t) / 2 < thumb_width / 2) {
                            thumb_u[((rect_y + y) / 2) * (thumb_width / 2) + (rect_x + rect_w - 1 - t) / 2] = 84;
                            thumb_v[((rect_y + y) / 2) * (thumb_width / 2) + (rect_x + rect_w - 1 - t) / 2] = 255;
                        }
                    }
                }
            }

            // Composite thumbnail onto top-right corner (symmetric to timecode on top-left)
            int thumb_dst_x = final_width - thumb_width - 10;
            int thumb_dst_y = 10;

            // Copy Y plane
            for (int y = 0; y < thumb_height; y++) {
                if (thumb_dst_y + y >= 0 && thumb_dst_y + y < final_height) {
                    for (int x = 0; x < thumb_width; x++) {
                        if (thumb_dst_x + x >= 0 && thumb_dst_x + x < final_width) {
                            dst_y[(thumb_dst_y + y) * dst_y_linesize + (thumb_dst_x + x)] = thumb_y[y * thumb_width + x];
                        }
                    }
                }
            }

            // Copy U and V planes
            for (int y = 0; y < thumb_height / 2; y++) {
                if ((thumb_dst_y / 2) + y >= 0 && (thumb_dst_y / 2) + y < final_height / 2) {
                    for (int x = 0; x < thumb_width / 2; x++) {
                        if ((thumb_dst_x / 2) + x >= 0 && (thumb_dst_x / 2) + x < final_width / 2) {
                            int dst_idx = ((thumb_dst_y / 2) + y) * dst_u_linesize + ((thumb_dst_x / 2) + x);
                            int src_idx = y * (thumb_width / 2) + x;
                            dst_u[dst_idx] = thumb_u[src_idx];
                            dst_v[dst_idx] = thumb_v[src_idx];
                        }
                    }
                }
            }

            std::cout << "🖼️ [SCREENSHOT] Thumbnail added at (" << thumb_dst_x << "," << thumb_dst_y
                      << ") " << thumb_width << "x" << thumb_height << std::endl;
        }
    }

    // Create a clean YUV frame with guaranteed no padding
    AVFrame* clean_yuv = av_frame_alloc();
    if (!clean_yuv) {
        std::cerr << "❌ [SCREENSHOT] Failed to allocate clean YUV frame" << std::endl;
        av_frame_free(&scaled_frame);
        return false;
    }

    clean_yuv->format = AV_PIX_FMT_YUV420P;
    clean_yuv->width = final_width;
    clean_yuv->height = final_height;

    if (av_frame_get_buffer(clean_yuv, 1) < 0) {
        std::cerr << "❌ [SCREENSHOT] Failed to allocate clean YUV buffer" << std::endl;
        av_frame_free(&clean_yuv);
        av_frame_free(&scaled_frame);
        return false;
    }

    // Manually copy only the actual image data (no padding)
    for (int plane = 0; plane < 3; plane++) {
        int h = (plane == 0) ? final_height : final_height / 2;
        int w = (plane == 0) ? final_width : final_width / 2;

        for (int y = 0; y < h; y++) {
            memcpy(clean_yuv->data[plane] + y * w,
                   scaled_frame->data[plane] + y * scaled_frame->linesize[plane],
                   w);
        }
    }

    // Debug: Check if scaled_frame has black stripe
    bool scaled_has_stripe = true;
    uint8_t* check_scaled_y = scaled_frame->data[0];
    for (int y = 0; y < std::min(final_height, 10); y++) {
        for (int x = final_width - 10; x < final_width; x++) {
            if (x < scaled_frame->linesize[0] && check_scaled_y[y * scaled_frame->linesize[0] + x] > 20) {
                scaled_has_stripe = false;
                break;
            }
        }
        if (!scaled_has_stripe) break;
    }
    if (scaled_has_stripe) {
        std::cout << "⚠️ [SCREENSHOT] Scaled frame has black stripe on right edge!" << std::endl;
    }

    // Debug: Check if clean_yuv has black stripe
    bool clean_has_stripe = true;
    uint8_t* check_clean_y = clean_yuv->data[0];
    for (int y = 0; y < std::min(final_height, 10); y++) {
        for (int x = final_width - 10; x < final_width; x++) {
            if (check_clean_y[y * final_width + x] > 20) {
                clean_has_stripe = false;
                break;
            }
        }
        if (!clean_has_stripe) break;
    }
    if (clean_has_stripe) {
        std::cout << "⚠️ [SCREENSHOT] Clean YUV frame has black stripe on right edge!" << std::endl;
    }

    // Now convert this clean frame to RGB
    std::vector<uint8_t> rgb_data(final_width * final_height * 3);

    SwsContext* rgb_ctx = sws_getContext(
        final_width, final_height, AV_PIX_FMT_YUV420P,
        final_width, final_height, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );

    if (rgb_ctx) {
        uint8_t* rgb_dst[4] = {rgb_data.data(), nullptr, nullptr, nullptr};
        int rgb_linesize[4] = {final_width * 3, 0, 0, 0};

        // Use clean YUV with linesize = width
        uint8_t* clean_data[3] = {clean_yuv->data[0], clean_yuv->data[1], clean_yuv->data[2]};
        int clean_linesize[3] = {final_width, final_width/2, final_width/2};

        sws_scale(rgb_ctx, clean_data, clean_linesize, 0, final_height, rgb_dst, rgb_linesize);
        sws_freeContext(rgb_ctx);
    } else {
        std::cerr << "❌ [SCREENSHOT] Failed to create RGB converter" << std::endl;
        av_frame_free(&clean_yuv);
        av_frame_free(&scaled_frame);
        return false;
    }

    av_frame_free(&clean_yuv);

    // Debug: Check RGB data for black stripe
    bool rgb_has_stripe = true;
    for (int y = 0; y < std::min(final_height, 10); y++) {
        for (int x = final_width - 10; x < final_width; x++) {
            uint8_t* pixel = rgb_data.data() + (y * final_width + x) * 3;
            if (pixel[0] > 20 || pixel[1] > 20 || pixel[2] > 20) {
                rgb_has_stripe = false;
                break;
            }
        }
        if (!rgb_has_stripe) break;
    }
    if (rgb_has_stripe) {
        std::cout << "⚠️ [SCREENSHOT] RGB data has black stripe on right edge!" << std::endl;

        // Sample some RGB values from right edge
        std::cout << "📊 [SCREENSHOT] Right edge RGB samples:" << std::endl;
        for (int y = 0; y < 3; y++) {
            int x = final_width - 5;
            uint8_t* pixel = rgb_data.data() + (y * final_width + x) * 3;
            std::cout << "   Y=" << y << " X=" << x << ": R=" << (int)pixel[0]
                      << " G=" << (int)pixel[1] << " B=" << (int)pixel[2] << std::endl;
        }
    }

*/
