#ifndef FSTP_SCREENSHOT_H
#define FSTP_SCREENSHOT_H

#include <string>
#include <cstdint>

// Take screenshot from YUV420P pixel buffer with timecode overlay
// Supports zoom and thumbnail features
// Automatically copies to clipboard (macOS/Windows/Linux)
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
    bool show_thumbnail
);

#endif // FSTP_SCREENSHOT_H
