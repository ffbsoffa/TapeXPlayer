#include "screenshot.h"
#include "../audio/mainau.h"
#include <iostream>
#include <chrono>
#include <ctime>

#import <AppKit/AppKit.h> // For NSPasteboard and NSImage

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/hwcontext.h>
#include <libavutil/error.h>
#include <libswscale/swscale.h>
}

// External zoom variables
extern std::atomic<bool> zoom_enabled;
extern std::atomic<float> zoom_factor;
extern std::atomic<float> zoom_center_x;
extern std::atomic<float> zoom_center_y;
extern std::atomic<bool> show_zoom_thumbnail;

// Simple bitmap font data for Tamsyn 8x16 style (basic ASCII digits and colon)
// Each character is 8x16 pixels, stored as 16 bytes (1 byte per row)
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

// Get bitmap data for a character (returns nullptr if not supported)
static const uint8_t* getBitmapChar(char c) {
    if (c >= '0' && c <= '9') {
        return bitmap_font_8x16[c - '0'];
    } else if (c == ':') {
        return bitmap_font_8x16[10]; // colon is at index 10
    }
    return nullptr;
}

// Render a single bitmap character onto frame at position (x, y)
bool renderBitmapChar(AVFrame* frame, char c, int x, int y, uint8_t brightness = 255) {
    const uint8_t* charData = getBitmapChar(c);
    if (!charData) {
        return false; // Character not supported
    }
    
    // Render 8x16 character
    for (int row = 0; row < 16; row++) {
        if (y + row >= frame->height) break;
        
        uint8_t rowData = charData[row];
        for (int col = 0; col < 8; col++) {
            if (x + col >= frame->width) break;
            
            // Check if pixel is set (bit 7-col)
            if (rowData & (0x80 >> col)) {
                int frame_y = y + row;
                int frame_x = x + col;
                
                // Set pixel on Y plane (luminance)
                if (frame_x < frame->linesize[0] && frame_y < frame->height) {
                    frame->data[0][frame_y * frame->linesize[0] + frame_x] = brightness;
                }
            }
        }
    }
    
    return true;
}

// Render bitmap text string onto frame
bool renderBitmapText(AVFrame* frame, const std::string& text, int x, int y, uint8_t brightness = 255) {
    int currentX = x;
    
    for (char c : text) {
        if (renderBitmapChar(frame, c, currentX, y, brightness)) {
            currentX += 8; // Move to next character position (8 pixels wide)
        } else {
            // Skip unsupported characters but still advance position
            currentX += 8;
        }
    }
    
    return true;
}

bool renderTimecodeOnFrame(AVFrame* frame, const std::string& timecode, int x, int y) {
    if (!frame || !frame->data[0]) {
        std::cerr << "[Screenshot] renderTimecodeOnFrame: Invalid frame or no data" << std::endl;
        return false;
    }
    
    // Support YUV420P and NV12 formats
    if (frame->format != AV_PIX_FMT_YUV420P && frame->format != AV_PIX_FMT_NV12) {
        std::cout << "[Screenshot] renderTimecodeOnFrame: Unsupported frame format for timecode overlay: " << frame->format << std::endl;
        return true; // Return true to allow screenshot without overlay
    }
    
    // Calculate text dimensions (8 pixels per character, 16 pixels height)
    int text_width = timecode.length() * 8;
    int text_height = 16;
    
    // Ensure coordinates are within frame bounds
    x = std::max(0, std::min(x, frame->width - text_width));
    y = std::max(0, std::min(y, frame->height - text_height));
    
    std::cout << "[Screenshot] renderTimecodeOnFrame: Rendering bitmap text '" << timecode 
              << "' at (" << x << "," << y << "), size: " << text_width << "x" << text_height 
              << ", frame size: " << frame->width << "x" << frame->height << std::endl;
    
    // Render timecode using built-in bitmap font (white on black background)
    // First, create a dark background rectangle
    for (int bg_y = y - 2; bg_y < y + text_height + 2; bg_y++) {
        for (int bg_x = x - 2; bg_x < x + text_width + 2; bg_x++) {
            if (bg_x >= 0 && bg_x < frame->width && bg_y >= 0 && bg_y < frame->height && 
                bg_x < frame->linesize[0]) {
                frame->data[0][bg_y * frame->linesize[0] + bg_x] = 32; // Dark gray background
            }
        }
    }
    
    // Render the text
    bool success = renderBitmapText(frame, timecode, x, y, 255); // White text
    
    if (success) {
        std::cout << "[Screenshot] renderTimecodeOnFrame: Bitmap text overlay completed successfully" << std::endl;
    } else {
        std::cout << "[Screenshot] renderTimecodeOnFrame: Failed to render bitmap text" << std::endl;
    }
    
    return success;
}

// Function to copy RGB image data to macOS clipboard
bool copyImageToClipboard(uint8_t* rgbData, int width, int height) {
    @autoreleasepool {
        // Create NSBitmapImageRep from RGB data
        NSBitmapImageRep* bitmapRep = [[NSBitmapImageRep alloc]
            initWithBitmapDataPlanes:&rgbData
                          pixelsWide:width
                          pixelsHigh:height
                       bitsPerSample:8
                     samplesPerPixel:3
                            hasAlpha:NO
                            isPlanar:NO
                      colorSpaceName:NSCalibratedRGBColorSpace
                         bytesPerRow:width * 3
                        bitsPerPixel:24];
        
        if (!bitmapRep) {
            std::cerr << "[Screenshot] Failed to create NSBitmapImageRep" << std::endl;
            return false;
        }
        
        // Create NSImage from bitmap representation
        NSImage* image = [[NSImage alloc] init];
        [image addRepresentation:bitmapRep];
        
        if (!image) {
            std::cerr << "[Screenshot] Failed to create NSImage" << std::endl;
            return false;
        }
        
        // Get the general pasteboard
        NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
        
        // Clear the pasteboard and set the image
        [pasteboard clearContents];
        BOOL success = [pasteboard writeObjects:@[image]];
        
        if (success) {
            std::cout << "[Screenshot] Image copied to clipboard successfully" << std::endl;
        } else {
            std::cerr << "[Screenshot] Failed to copy image to clipboard" << std::endl;
        }
        
        return success;
    }
}

bool saveFrameAsPNGWithTimecode(AVFrame* frame, const std::string& timecode, const std::string& outputPath) {
    if (!frame) {
        std::cerr << "[Screenshot] Frame is null" << std::endl;
        return false;
    }

    // Use the exact timecode string passed from display.mm
    // This ensures we use the same calculation method for both display and screenshots
    std::cout << "[Screenshot] Using display-synchronized timecode: " << timecode << std::endl;
    
    // Log frame information for debugging
    std::cout << "[Screenshot] Frame info: format=" << frame->format 
              << ", width=" << frame->width 
              << ", height=" << frame->height 
              << ", hw_frames_ctx=" << (frame->hw_frames_ctx ? "yes" : "no") << std::endl;
    
    AVFrame* workingFrame = nullptr;
    bool needToFreeWorkingFrame = false;
    
    // Check if this is a hardware frame that needs to be transferred to system memory
    if (frame->hw_frames_ctx) {
        std::cout << "[Screenshot] Hardware frame detected, transferring to system memory..." << std::endl;
        
        workingFrame = av_frame_alloc();
        if (!workingFrame) {
            std::cerr << "[Screenshot] Failed to allocate working frame" << std::endl;
            return false;
        }
        
        // Transfer hardware frame to system memory
        int ret = av_hwframe_transfer_data(workingFrame, frame, 0);
        if (ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, sizeof(errbuf));
            std::cerr << "[Screenshot] Failed to transfer hardware frame to system memory: " << errbuf << std::endl;
            av_frame_free(&workingFrame);
            return false;
        }
        
        needToFreeWorkingFrame = true;
        std::cout << "[Screenshot] Hardware frame transferred successfully, new format: " << workingFrame->format << std::endl;
    } else {
        // Use the original frame directly
        workingFrame = frame;
    }
    
    // Check if the working frame has valid data
    if (!workingFrame->data[0]) {
        std::cerr << "[Screenshot] Working frame has no data" << std::endl;
        if (needToFreeWorkingFrame) av_frame_free(&workingFrame);
        return false;
    }
    
    // Scale frame with preserved aspect ratio for better timecode visibility
    // Calculate target dimensions based on original aspect ratio
    float originalAspect = (float)workingFrame->width / (float)workingFrame->height;
    
    int targetWidth, targetHeight;
    
    // For better timecode visibility, aim for 480p resolution
    if (originalAspect >= 1.0f) {
        // Landscape or square: limit height to 480p
        targetHeight = 480;
        targetWidth = (int)(targetHeight * originalAspect);
    } else {
        // Portrait: limit width to 480p  
        targetWidth = 480;
        targetHeight = (int)(targetWidth / originalAspect);
    }
    
    // Ensure dimensions are even (required for many video formats)
    targetWidth = (targetWidth + 1) & ~1;
    targetHeight = (targetHeight + 1) & ~1;
    
    std::cout << "[Screenshot] Scaling frame from " << workingFrame->width << "x" << workingFrame->height 
              << " (aspect " << originalAspect << ") to " << targetWidth << "x" << targetHeight << " (480p)" << std::endl;
    
    // Create scaled frame
    AVFrame* scaledFrame = av_frame_alloc();
    if (!scaledFrame) {
        std::cerr << "[Screenshot] Failed to allocate scaled frame" << std::endl;
        if (needToFreeWorkingFrame) av_frame_free(&workingFrame);
        return false;
    }
    
    scaledFrame->format = workingFrame->format;
    scaledFrame->width = targetWidth;
    scaledFrame->height = targetHeight;
    
    if (av_frame_get_buffer(scaledFrame, 32) < 0) {
        std::cerr << "[Screenshot] Failed to allocate scaled frame buffer" << std::endl;
        av_frame_free(&scaledFrame);
        if (needToFreeWorkingFrame) av_frame_free(&workingFrame);
        return false;
    }
    
    // Scale the frame
    SwsContext* scaleContext = sws_getContext(
        workingFrame->width, workingFrame->height, static_cast<AVPixelFormat>(workingFrame->format),
        targetWidth, targetHeight, static_cast<AVPixelFormat>(workingFrame->format),
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );
    
    if (!scaleContext) {
        std::cerr << "[Screenshot] Failed to create scaling context" << std::endl;
        av_frame_free(&scaledFrame);
        if (needToFreeWorkingFrame) av_frame_free(&workingFrame);
        return false;
    }
    
    int scaleResult = sws_scale(scaleContext, workingFrame->data, workingFrame->linesize, 0, workingFrame->height,
                               scaledFrame->data, scaledFrame->linesize);
    
    sws_freeContext(scaleContext);
    
    if (scaleResult <= 0) {
        std::cerr << "[Screenshot] Failed to scale frame" << std::endl;
        av_frame_free(&scaledFrame);
        if (needToFreeWorkingFrame) av_frame_free(&workingFrame);
        return false;
    }
    
    std::cout << "[Screenshot] Frame scaled successfully to " << targetWidth << "x" << targetHeight << std::endl;
    
    // Free the working frame if it was allocated, we now use scaled frame
    if (needToFreeWorkingFrame) {
        av_frame_free(&workingFrame);
        needToFreeWorkingFrame = false;
    }
    
    // Create a copy of the scaled frame for timecode overlay
    AVFrame* frameCopy = av_frame_alloc();
    if (!frameCopy) {
        std::cerr << "[Screenshot] Failed to allocate frame copy" << std::endl;
        av_frame_free(&scaledFrame);
        return false;
    }
    
    if (av_frame_ref(frameCopy, scaledFrame) < 0) {
        std::cerr << "[Screenshot] Failed to reference scaled frame" << std::endl;
        av_frame_free(&frameCopy);
        av_frame_free(&scaledFrame);
        return false;
    }
    
    // Free the scaled frame as we now have a reference copy
    av_frame_free(&scaledFrame);
    
    // Add timecode overlay to the top-left corner with some padding
    int overlay_x = 20;
    int overlay_y = 20;
    
    // Only try to render timecode if the frame format supports it
    if (frameCopy->format == AV_PIX_FMT_YUV420P || frameCopy->format == AV_PIX_FMT_NV12) {
        if (!renderTimecodeOnFrame(frameCopy, timecode, overlay_x, overlay_y)) {
            std::cout << "[Screenshot] Note: Failed to render timecode on frame, continuing without overlay" << std::endl;
        }
    } else {
        std::cout << "[Screenshot] Note: Frame format " << frameCopy->format << " not supported for timecode overlay, continuing without overlay" << std::endl;
    }
    
    // Convert frame to RGB for saving
    SwsContext* swsContext = sws_getContext(
        frameCopy->width, frameCopy->height, static_cast<AVPixelFormat>(frameCopy->format),
        frameCopy->width, frameCopy->height, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );
    
    if (!swsContext) {
        std::cerr << "[Screenshot] Failed to create SWS context for format " << frameCopy->format << std::endl;
        av_frame_free(&frameCopy);
        return false;
    }
    
    // Allocate RGB buffer
    uint8_t* rgbData = static_cast<uint8_t*>(av_malloc(frameCopy->width * frameCopy->height * 3));
    if (!rgbData) {
        std::cerr << "[Screenshot] Failed to allocate RGB buffer" << std::endl;
        sws_freeContext(swsContext);
        av_frame_free(&frameCopy);
        return false;
    }
    
    uint8_t* rgbDataArray[1] = { rgbData };
    int rgbLinesize[1] = { frameCopy->width * 3 };
    
    // Convert to RGB
    int scaledHeight = sws_scale(swsContext, frameCopy->data, frameCopy->linesize, 0, frameCopy->height,
                                rgbDataArray, rgbLinesize);
    
    if (scaledHeight <= 0) {
        std::cerr << "[Screenshot] Failed to scale frame to RGB" << std::endl;
        av_free(rgbData);
        sws_freeContext(swsContext);
        av_frame_free(&frameCopy);
        return false;
    }
    
    // Copy to clipboard instead of saving to file
    bool success = copyImageToClipboard(rgbData, frameCopy->width, frameCopy->height);
    
    if (success) {
        std::cout << "[Screenshot] Screenshot copied to clipboard successfully!" << std::endl;
        std::cout << "[Screenshot] Timecode: " << timecode << std::endl;
        std::cout << "[Screenshot] Frame dimensions: " << frameCopy->width << "x" << frameCopy->height << std::endl;
    }
    
    // Cleanup
    av_free(rgbData);
    sws_freeContext(swsContext);
    av_frame_free(&frameCopy);
    
    return success;
}

// Advanced screenshot function that takes into account zoom and thumbnails
bool takeAdvancedScreenshotWithTimecode(AVFrame* frame, const std::string& timecode, 
                                       int windowWidth, int windowHeight, 
                                       bool isZoomEnabled, float zoomFactor, 
                                       float zoomCenterX, float zoomCenterY, 
                                       bool showThumbnail) {
    if (!frame) {
        std::cerr << "[Screenshot] No frame available for screenshot" << std::endl;
        return false;
    }

    // Use the exact timecode from display.mm which now uses generateTXTimecode
    std::cout << "[Screenshot] Using display-synchronized timecode: " << timecode << std::endl;
    
    // Log frame information for debugging
    std::cout << "[Screenshot] Frame info: format=" << frame->format 
              << ", width=" << frame->width 
              << ", height=" << frame->height 
              << ", hw_frames_ctx=" << (frame->hw_frames_ctx ? "yes" : "no") << std::endl;
    
    AVFrame* workingFrame = frame;
    bool needToFreeWorkingFrame = false;
    
    if (frame->hw_frames_ctx) {
        workingFrame = av_frame_alloc();
        if (!workingFrame) {
            std::cerr << "[Screenshot] Failed to allocate working frame" << std::endl;
            return false;
        }
        
        int ret = av_hwframe_transfer_data(workingFrame, frame, 0);
        if (ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, sizeof(errbuf));
            std::cerr << "[Screenshot] Failed to transfer hardware frame: " << errbuf << std::endl;
            av_frame_free(&workingFrame);
            return false;
        }
        
        needToFreeWorkingFrame = true;
    }
    
    // Calculate video display dimensions (maintaining aspect ratio)
    float aspectRatio = (float)workingFrame->width / (float)workingFrame->height;
    int videoDisplayWidth, videoDisplayHeight;
    int videoOffsetX, videoOffsetY;
    
    std::cout << "[Screenshot] Working frame dimensions: " << workingFrame->width << "x" << workingFrame->height 
              << ", window: " << windowWidth << "x" << windowHeight << std::endl;
    
    if (windowWidth / aspectRatio <= windowHeight) {
        videoDisplayWidth = windowWidth;
        videoDisplayHeight = (int)(windowWidth / aspectRatio);
    } else {
        videoDisplayHeight = windowHeight;
        videoDisplayWidth = (int)(windowHeight * aspectRatio);
    }
    
    videoOffsetX = (windowWidth - videoDisplayWidth) / 2;
    videoOffsetY = (windowHeight - videoDisplayHeight) / 2;
    
    // Create a target frame for our final composition
    AVFrame* compositeFrame = av_frame_alloc();
    if (!compositeFrame) {
        std::cerr << "[Screenshot] Failed to allocate composite frame" << std::endl;
        if (needToFreeWorkingFrame) av_frame_free(&workingFrame);
        return false;
    }
    
    // Scale down to 480p but maintain aspect ratio of the VIDEO, not window
    // Use the original video aspect ratio for the final output
    int finalWidth, finalHeight;
    
    if (aspectRatio >= 1.0f) {
        finalHeight = 480;
        finalWidth = (int)(finalHeight * aspectRatio);
    } else {
        finalWidth = 480;
        finalHeight = (int)(finalWidth / aspectRatio);
    }
    
    // Ensure dimensions are even
    finalWidth = (finalWidth + 1) & ~1;
    finalHeight = (finalHeight + 1) & ~1;
    
    std::cout << "[Screenshot] Video aspect ratio: " << aspectRatio 
              << ", Final screenshot dimensions: " << finalWidth << "x" << finalHeight 
              << " (maintaining video aspect ratio)" << std::endl;
    
    compositeFrame->format = AV_PIX_FMT_YUV420P;
    compositeFrame->width = finalWidth;
    compositeFrame->height = finalHeight;
    
    if (av_frame_get_buffer(compositeFrame, 32) < 0) {
        std::cerr << "[Screenshot] Failed to allocate composite frame buffer" << std::endl;
        av_frame_free(&compositeFrame);
        if (needToFreeWorkingFrame) av_frame_free(&workingFrame);
        return false;
    }
    
    // Make frame writable to ensure we can modify it safely
    if (av_frame_make_writable(compositeFrame) < 0) {
        std::cerr << "[Screenshot] Failed to make composite frame writable" << std::endl;
        av_frame_free(&compositeFrame);
        if (needToFreeWorkingFrame) av_frame_free(&workingFrame);
        return false;
    }
    
    // Fill with black background first - completely zero out all buffers
    // Y plane - fill with black (16) - fill entire linesize to avoid artifacts
    for (int y = 0; y < compositeFrame->height; y++) {
        memset(compositeFrame->data[0] + y * compositeFrame->linesize[0], 16, compositeFrame->linesize[0]);
    }
    
    // U and V planes - fill with neutral color (128) for YUV420P
    // In YUV420P, U and V planes are half the size in both dimensions
    int uvHeight = compositeFrame->height / 2;
    
    for (int y = 0; y < uvHeight; y++) {
        memset(compositeFrame->data[1] + y * compositeFrame->linesize[1], 128, compositeFrame->linesize[1]); // U plane - fill entire linesize
        memset(compositeFrame->data[2] + y * compositeFrame->linesize[2], 128, compositeFrame->linesize[2]); // V plane - fill entire linesize
    }
    
    // For screenshots, we want the video to fill the entire final frame
    // because the final frame should only contain video content with proper aspect ratio
    int scaledVideoDisplayWidth = finalWidth;
    int scaledVideoDisplayHeight = finalHeight;
    int scaledVideoOffsetX = 0;
    int scaledVideoOffsetY = 0;
    
    std::cout << "[Screenshot] Composite layout: video area " << scaledVideoDisplayWidth << "x" << scaledVideoDisplayHeight 
              << " at offset (" << scaledVideoOffsetX << "," << scaledVideoOffsetY 
              << ") in " << finalWidth << "x" << finalHeight << " composite" << std::endl;
    
    // Create the main video content
    AVFrame* mainVideoFrame = av_frame_alloc();
    if (!mainVideoFrame) {
        std::cerr << "[Screenshot] Failed to allocate main video frame" << std::endl;
        av_frame_free(&compositeFrame);
        if (needToFreeWorkingFrame) av_frame_free(&workingFrame);
        return false;
    }
    
    mainVideoFrame->format = AV_PIX_FMT_YUV420P;
    mainVideoFrame->width = scaledVideoDisplayWidth;
    mainVideoFrame->height = scaledVideoDisplayHeight;
    
    if (av_frame_get_buffer(mainVideoFrame, 32) < 0) {
        std::cerr << "[Screenshot] Failed to allocate main video frame buffer" << std::endl;
        av_frame_free(&mainVideoFrame);
        av_frame_free(&compositeFrame);
        if (needToFreeWorkingFrame) av_frame_free(&workingFrame);
        return false;
    }
    
    // Setup scaling for the main video
    SwsContext* mainScaleContext;
    
    if (isZoomEnabled && zoomFactor > 1.0f) {
        // Calculate zoomed source area with even dimensions for YUV420P
        int srcWidth = ((int)(workingFrame->width / zoomFactor) + 1) & ~1;
        int srcHeight = ((int)(workingFrame->height / zoomFactor) + 1) & ~1;
        int srcX = (int)(zoomCenterX * workingFrame->width - srcWidth / 2);
        int srcY = (int)(zoomCenterY * workingFrame->height - srcHeight / 2);
        
        // Ensure even coordinates for chroma subsampling
        srcX = srcX & ~1;
        srcY = srcY & ~1;
        
        // Constrain to frame boundaries
        if (srcX < 0) srcX = 0;
        if (srcY < 0) srcY = 0;
        if (srcX + srcWidth > workingFrame->width) srcX = workingFrame->width - srcWidth;
        if (srcY + srcHeight > workingFrame->height) srcY = workingFrame->height - srcHeight;
        
        std::cout << "[Screenshot] Zoom applied: source area " << srcX << "," << srcY 
                  << " " << srcWidth << "x" << srcHeight << " from " 
                  << workingFrame->width << "x" << workingFrame->height 
                  << ", format=" << workingFrame->format << std::endl;
        
        // Create cropped frame from zoom area
        AVFrame* croppedFrame = av_frame_alloc();
        if (!croppedFrame) {
            std::cerr << "[Screenshot] Failed to allocate cropped frame" << std::endl;
            av_frame_free(&mainVideoFrame);
            av_frame_free(&compositeFrame);
            if (needToFreeWorkingFrame) av_frame_free(&workingFrame);
            return false;
        }
        
        croppedFrame->format = workingFrame->format;
        // Ensure dimensions are even for YUV420P compatibility
        croppedFrame->width = (srcWidth + 1) & ~1;
        croppedFrame->height = (srcHeight + 1) & ~1;
        
        if (av_frame_get_buffer(croppedFrame, 32) < 0) {
            std::cerr << "[Screenshot] Failed to allocate cropped frame buffer" << std::endl;
            av_frame_free(&croppedFrame);
            av_frame_free(&mainVideoFrame);
            av_frame_free(&compositeFrame);
            if (needToFreeWorkingFrame) av_frame_free(&workingFrame);
            return false;
        }
        
        std::cout << "[Screenshot] Created cropped frame: " << croppedFrame->width << "x" << croppedFrame->height << std::endl;
        
        // Safe approach: Convert to RGB, crop, then convert back
        // This avoids all YUV subsampling complexities
        
        // Step 1: Convert source frame to RGB
        AVFrame* rgbSourceFrame = av_frame_alloc();
        if (!rgbSourceFrame) {
            std::cerr << "[Screenshot] Failed to allocate RGB source frame" << std::endl;
            av_frame_free(&croppedFrame);
            av_frame_free(&mainVideoFrame);
            av_frame_free(&compositeFrame);
            if (needToFreeWorkingFrame) av_frame_free(&workingFrame);
            return false;
        }
        
        rgbSourceFrame->format = AV_PIX_FMT_RGB24;
        rgbSourceFrame->width = workingFrame->width;
        rgbSourceFrame->height = workingFrame->height;
        
        if (av_frame_get_buffer(rgbSourceFrame, 32) < 0) {
            std::cerr << "[Screenshot] Failed to allocate RGB source frame buffer" << std::endl;
            av_frame_free(&rgbSourceFrame);
            av_frame_free(&croppedFrame);
            av_frame_free(&mainVideoFrame);
            av_frame_free(&compositeFrame);
            if (needToFreeWorkingFrame) av_frame_free(&workingFrame);
            return false;
        }
        
        // Convert YUV to RGB
        SwsContext* yuvToRgbContext = sws_getContext(
            workingFrame->width, workingFrame->height, static_cast<AVPixelFormat>(workingFrame->format),
            workingFrame->width, workingFrame->height, AV_PIX_FMT_RGB24,
            SWS_BILINEAR, nullptr, nullptr, nullptr
        );
        
        if (!yuvToRgbContext) {
            std::cerr << "[Screenshot] Failed to create YUV to RGB context" << std::endl;
            av_frame_free(&rgbSourceFrame);
            av_frame_free(&croppedFrame);
            av_frame_free(&mainVideoFrame);
            av_frame_free(&compositeFrame);
            if (needToFreeWorkingFrame) av_frame_free(&workingFrame);
            return false;
        }
        
        int rgbResult = sws_scale(yuvToRgbContext, workingFrame->data, workingFrame->linesize, 0, workingFrame->height,
                                 rgbSourceFrame->data, rgbSourceFrame->linesize);
        sws_freeContext(yuvToRgbContext);
        
        if (rgbResult <= 0) {
            std::cerr << "[Screenshot] Failed to convert YUV to RGB" << std::endl;
            av_frame_free(&rgbSourceFrame);
            av_frame_free(&croppedFrame);
            av_frame_free(&mainVideoFrame);
            av_frame_free(&compositeFrame);
            if (needToFreeWorkingFrame) av_frame_free(&workingFrame);
            return false;
        }
        
        // Step 2: Create RGB cropped frame
        AVFrame* rgbCroppedFrame = av_frame_alloc();
        if (!rgbCroppedFrame) {
            std::cerr << "[Screenshot] Failed to allocate RGB cropped frame" << std::endl;
            av_frame_free(&rgbSourceFrame);
            av_frame_free(&croppedFrame);
            av_frame_free(&mainVideoFrame);
            av_frame_free(&compositeFrame);
            if (needToFreeWorkingFrame) av_frame_free(&workingFrame);
            return false;
        }
        
        rgbCroppedFrame->format = AV_PIX_FMT_RGB24;
        rgbCroppedFrame->width = srcWidth;
        rgbCroppedFrame->height = srcHeight;
        
        if (av_frame_get_buffer(rgbCroppedFrame, 32) < 0) {
            std::cerr << "[Screenshot] Failed to allocate RGB cropped frame buffer" << std::endl;
            av_frame_free(&rgbCroppedFrame);
            av_frame_free(&rgbSourceFrame);
            av_frame_free(&croppedFrame);
            av_frame_free(&mainVideoFrame);
            av_frame_free(&compositeFrame);
            if (needToFreeWorkingFrame) av_frame_free(&workingFrame);
            return false;
        }
        
        // Step 3: Crop in RGB space (simple pixel copying)
        for (int y = 0; y < srcHeight; y++) {
            if (srcY + y < rgbSourceFrame->height && y < rgbCroppedFrame->height) {
                int bytesPerPixel = 3; // RGB24
                int srcRowOffset = (srcY + y) * rgbSourceFrame->linesize[0] + srcX * bytesPerPixel;
                int dstRowOffset = y * rgbCroppedFrame->linesize[0];
                int copyBytes = srcWidth * bytesPerPixel;
                
                if (srcX + srcWidth <= rgbSourceFrame->width && copyBytes <= rgbCroppedFrame->linesize[0]) {
                    memcpy(rgbCroppedFrame->data[0] + dstRowOffset,
                           rgbSourceFrame->data[0] + srcRowOffset,
                           copyBytes);
                }
            }
        }
        
        av_frame_free(&rgbSourceFrame);
        
        // Step 4: Convert cropped RGB back to YUV
        SwsContext* rgbToYuvContext = sws_getContext(
            srcWidth, srcHeight, AV_PIX_FMT_RGB24,
            croppedFrame->width, croppedFrame->height, static_cast<AVPixelFormat>(croppedFrame->format),
            SWS_BILINEAR, nullptr, nullptr, nullptr
        );
        
        if (!rgbToYuvContext) {
            std::cerr << "[Screenshot] Failed to create RGB to YUV context" << std::endl;
            av_frame_free(&rgbCroppedFrame);
            av_frame_free(&croppedFrame);
            av_frame_free(&mainVideoFrame);
            av_frame_free(&compositeFrame);
            if (needToFreeWorkingFrame) av_frame_free(&workingFrame);
            return false;
        }
        
        int yuvResult = sws_scale(rgbToYuvContext, rgbCroppedFrame->data, rgbCroppedFrame->linesize, 0, srcHeight,
                                 croppedFrame->data, croppedFrame->linesize);
        sws_freeContext(rgbToYuvContext);
        av_frame_free(&rgbCroppedFrame);
        
        if (yuvResult <= 0) {
            std::cerr << "[Screenshot] Failed to convert RGB back to YUV" << std::endl;
            av_frame_free(&croppedFrame);
            av_frame_free(&mainVideoFrame);
            av_frame_free(&compositeFrame);
            if (needToFreeWorkingFrame) av_frame_free(&workingFrame);
            return false;
        }
        
        std::cout << "[Screenshot] Successfully cropped frame via RGB conversion" << std::endl;
        
        // Scale cropped frame to main video size
        mainScaleContext = sws_getContext(
            croppedFrame->width, croppedFrame->height, static_cast<AVPixelFormat>(croppedFrame->format),
            mainVideoFrame->width, mainVideoFrame->height, static_cast<AVPixelFormat>(mainVideoFrame->format),
            SWS_BILINEAR, nullptr, nullptr, nullptr
        );
        
        if (mainScaleContext) {
            sws_scale(mainScaleContext, croppedFrame->data, croppedFrame->linesize, 0, croppedFrame->height,
                     mainVideoFrame->data, mainVideoFrame->linesize);
            sws_freeContext(mainScaleContext);
        }
        
        av_frame_free(&croppedFrame);
    } else {
        // No zoom - scale entire frame
        mainScaleContext = sws_getContext(
            workingFrame->width, workingFrame->height, static_cast<AVPixelFormat>(workingFrame->format),
            mainVideoFrame->width, mainVideoFrame->height, static_cast<AVPixelFormat>(mainVideoFrame->format),
            SWS_BILINEAR, nullptr, nullptr, nullptr
        );
        
        if (mainScaleContext) {
            sws_scale(mainScaleContext, workingFrame->data, workingFrame->linesize, 0, workingFrame->height,
                     mainVideoFrame->data, mainVideoFrame->linesize);
            sws_freeContext(mainScaleContext);
        }
    }
    
    // Copy main video into composite frame
    for (int plane = 0; plane < 3; plane++) {
        // Make sure the plane exists in both frames
        if (!compositeFrame->data[plane] || !mainVideoFrame->data[plane]) {
            continue;
        }
        
        int planeOffsetX = (plane == 0) ? scaledVideoOffsetX : scaledVideoOffsetX / 2;
        int planeOffsetY = (plane == 0) ? scaledVideoOffsetY : scaledVideoOffsetY / 2;
        int planeWidth = (plane == 0) ? scaledVideoDisplayWidth : scaledVideoDisplayWidth / 2;
        int planeHeight = (plane == 0) ? scaledVideoDisplayHeight : scaledVideoDisplayHeight / 2;
        
        // Additional bounds checking for U and V planes
        int maxPlaneHeight = (plane == 0) ? compositeFrame->height : compositeFrame->height / 2;
        int maxPlaneWidth = (plane == 0) ? compositeFrame->width : compositeFrame->width / 2;
        
        for (int y = 0; y < planeHeight && (planeOffsetY + y) < maxPlaneHeight; y++) {
            if (planeOffsetY + y >= 0 && planeOffsetX >= 0 && planeOffsetX < maxPlaneWidth) {
                int copyWidth = std::min(planeWidth, maxPlaneWidth - planeOffsetX);
                if (copyWidth > 0 && copyWidth <= mainVideoFrame->linesize[plane]) {
                    memcpy(compositeFrame->data[plane] + (planeOffsetY + y) * compositeFrame->linesize[plane] + planeOffsetX,
                           mainVideoFrame->data[plane] + y * mainVideoFrame->linesize[plane],
                           copyWidth);
                }
            }
        }
    }
    
    av_frame_free(&mainVideoFrame);
    
    // Add thumbnail if zoom is enabled and thumbnail is shown
    if (isZoomEnabled && showThumbnail && zoomFactor > 1.0f) {
        // Calculate thumbnail dimensions and position (scale to final composite size)
        int thumbnailWidth = std::min(150, (int)(finalWidth * 0.2f)); // 150px max, 20% of width
        int thumbnailHeight = (int)(thumbnailWidth / aspectRatio);
        int thumbnailX = finalWidth - thumbnailWidth - 10; // 10px padding from right
        int thumbnailY = 10; // 10px padding from top
        
        std::cout << "[Screenshot] Adding thumbnail at " << thumbnailX << "," << thumbnailY 
                  << " size " << thumbnailWidth << "x" << thumbnailHeight << std::endl;
        
        // Create thumbnail frame
        AVFrame* thumbnailFrame = av_frame_alloc();
        if (thumbnailFrame) {
            thumbnailFrame->format = AV_PIX_FMT_YUV420P;
            thumbnailFrame->width = thumbnailWidth;
            thumbnailFrame->height = thumbnailHeight;
            
            if (av_frame_get_buffer(thumbnailFrame, 32) == 0) {
                // Scale full frame to thumbnail size
                SwsContext* thumbScaleContext = sws_getContext(
                    workingFrame->width, workingFrame->height, static_cast<AVPixelFormat>(workingFrame->format),
                    thumbnailFrame->width, thumbnailFrame->height, static_cast<AVPixelFormat>(thumbnailFrame->format),
                    SWS_BILINEAR, nullptr, nullptr, nullptr
                );
                
                if (thumbScaleContext) {
                    sws_scale(thumbScaleContext, workingFrame->data, workingFrame->linesize, 0, workingFrame->height,
                             thumbnailFrame->data, thumbnailFrame->linesize);
                    sws_freeContext(thumbScaleContext);
                    
                    // Copy thumbnail into composite
                    for (int plane = 0; plane < 3; plane++) {
                        // Make sure the plane exists in both frames
                        if (!compositeFrame->data[plane] || !thumbnailFrame->data[plane]) {
                            continue;
                        }
                        
                        int planeThumbX = (plane == 0) ? thumbnailX : thumbnailX / 2;
                        int planeThumbY = (plane == 0) ? thumbnailY : thumbnailY / 2;
                        int planeThumbWidth = (plane == 0) ? thumbnailWidth : thumbnailWidth / 2;
                        int planeThumbHeight = (plane == 0) ? thumbnailHeight : thumbnailHeight / 2;
                        
                        // Additional bounds checking for U and V planes in thumbnail
                        int maxPlaneHeight = (plane == 0) ? compositeFrame->height : compositeFrame->height / 2;
                        int maxPlaneWidth = (plane == 0) ? compositeFrame->width : compositeFrame->width / 2;
                        
                        for (int y = 0; y < planeThumbHeight && (planeThumbY + y) < maxPlaneHeight; y++) {
                            if (planeThumbY + y >= 0 && planeThumbX >= 0 && planeThumbX < maxPlaneWidth) {
                                int copyWidth = std::min(planeThumbWidth, maxPlaneWidth - planeThumbX);
                                if (copyWidth > 0 && copyWidth <= thumbnailFrame->linesize[plane]) {
                                    memcpy(compositeFrame->data[plane] + (planeThumbY + y) * compositeFrame->linesize[plane] + planeThumbX,
                                           thumbnailFrame->data[plane] + y * thumbnailFrame->linesize[plane],
                                           copyWidth);
                                }
                            }
                        }
                    }
                    
                    // Draw border around thumbnail (simple white border)
                    int borderThickness = 2;
                    // Top and bottom borders
                    for (int x = 0; x < thumbnailWidth && (thumbnailX + x) < compositeFrame->width; x++) {
                        for (int t = 0; t < borderThickness; t++) {
                            if (thumbnailY - t >= 0) {
                                compositeFrame->data[0][(thumbnailY - t) * compositeFrame->linesize[0] + thumbnailX + x] = 235; // White in Y
                            }
                            if (thumbnailY + thumbnailHeight + t < compositeFrame->height) {
                                compositeFrame->data[0][(thumbnailY + thumbnailHeight + t) * compositeFrame->linesize[0] + thumbnailX + x] = 235;
                            }
                        }
                    }
                    // Left and right borders  
                    for (int y = 0; y < thumbnailHeight && (thumbnailY + y) < compositeFrame->height; y++) {
                        for (int t = 0; t < borderThickness; t++) {
                            if (thumbnailX - t >= 0) {
                                compositeFrame->data[0][(thumbnailY + y) * compositeFrame->linesize[0] + thumbnailX - t] = 235;
                            }
                            if (thumbnailX + thumbnailWidth + t < compositeFrame->width) {
                                compositeFrame->data[0][(thumbnailY + y) * compositeFrame->linesize[0] + thumbnailX + thumbnailWidth + t] = 235;
                            }
                        }
                    }
                    
                    // Draw red rectangle showing zoomed area in thumbnail
                    int zoomRectWidth = (int)(thumbnailWidth / zoomFactor);
                    int zoomRectHeight = (int)(thumbnailHeight / zoomFactor);
                    int zoomRectX = thumbnailX + (int)(zoomCenterX * thumbnailWidth - zoomRectWidth / 2);
                    int zoomRectY = thumbnailY + (int)(zoomCenterY * thumbnailHeight - zoomRectHeight / 2);
                    
                    // Constrain zoom rectangle to thumbnail boundaries
                    if (zoomRectX < thumbnailX) zoomRectX = thumbnailX;
                    if (zoomRectY < thumbnailY) zoomRectY = thumbnailY;
                    if (zoomRectX + zoomRectWidth > thumbnailX + thumbnailWidth) zoomRectX = thumbnailX + thumbnailWidth - zoomRectWidth;
                    if (zoomRectY + zoomRectHeight > thumbnailY + thumbnailHeight) zoomRectY = thumbnailY + thumbnailHeight - zoomRectHeight;
                    
                    std::cout << "[Screenshot] Drawing zoom rectangle at " << zoomRectX << "," << zoomRectY 
                              << " size " << zoomRectWidth << "x" << zoomRectHeight << " in thumbnail" << std::endl;
                    
                    // Draw red rectangle outline (just the edges)
                    int redThickness = 1;
                    // Top and bottom edges
                    for (int x = 0; x < zoomRectWidth && (zoomRectX + x) < compositeFrame->width; x++) {
                        for (int t = 0; t < redThickness; t++) {
                            // Top edge
                            if (zoomRectY + t >= 0 && zoomRectY + t < compositeFrame->height) {
                                compositeFrame->data[0][(zoomRectY + t) * compositeFrame->linesize[0] + zoomRectX + x] = 76;  // Red in Y (darker)
                                // Set U and V for red color (approximately)
                                if ((zoomRectY + t) / 2 < compositeFrame->height / 2 && (zoomRectX + x) / 2 < compositeFrame->width / 2) {
                                    compositeFrame->data[1][((zoomRectY + t) / 2) * compositeFrame->linesize[1] + (zoomRectX + x) / 2] = 84;  // U for red
                                    compositeFrame->data[2][((zoomRectY + t) / 2) * compositeFrame->linesize[2] + (zoomRectX + x) / 2] = 255; // V for red
                                }
                            }
                            // Bottom edge
                            if (zoomRectY + zoomRectHeight - 1 - t >= 0 && zoomRectY + zoomRectHeight - 1 - t < compositeFrame->height) {
                                compositeFrame->data[0][(zoomRectY + zoomRectHeight - 1 - t) * compositeFrame->linesize[0] + zoomRectX + x] = 76;
                                if ((zoomRectY + zoomRectHeight - 1 - t) / 2 < compositeFrame->height / 2 && (zoomRectX + x) / 2 < compositeFrame->width / 2) {
                                    compositeFrame->data[1][((zoomRectY + zoomRectHeight - 1 - t) / 2) * compositeFrame->linesize[1] + (zoomRectX + x) / 2] = 84;
                                    compositeFrame->data[2][((zoomRectY + zoomRectHeight - 1 - t) / 2) * compositeFrame->linesize[2] + (zoomRectX + x) / 2] = 255;
                                }
                            }
                        }
                    }
                    // Left and right edges
                    for (int y = 0; y < zoomRectHeight && (zoomRectY + y) < compositeFrame->height; y++) {
                        for (int t = 0; t < redThickness; t++) {
                            // Left edge
                            if (zoomRectX + t >= 0 && zoomRectX + t < compositeFrame->width) {
                                compositeFrame->data[0][(zoomRectY + y) * compositeFrame->linesize[0] + zoomRectX + t] = 76;
                                if ((zoomRectY + y) / 2 < compositeFrame->height / 2 && (zoomRectX + t) / 2 < compositeFrame->width / 2) {
                                    compositeFrame->data[1][((zoomRectY + y) / 2) * compositeFrame->linesize[1] + (zoomRectX + t) / 2] = 84;
                                    compositeFrame->data[2][((zoomRectY + y) / 2) * compositeFrame->linesize[2] + (zoomRectX + t) / 2] = 255;
                                }
                            }
                            // Right edge
                            if (zoomRectX + zoomRectWidth - 1 - t >= 0 && zoomRectX + zoomRectWidth - 1 - t < compositeFrame->width) {
                                compositeFrame->data[0][(zoomRectY + y) * compositeFrame->linesize[0] + zoomRectX + zoomRectWidth - 1 - t] = 76;
                                if ((zoomRectY + y) / 2 < compositeFrame->height / 2 && (zoomRectX + zoomRectWidth - 1 - t) / 2 < compositeFrame->width / 2) {
                                    compositeFrame->data[1][((zoomRectY + y) / 2) * compositeFrame->linesize[1] + (zoomRectX + zoomRectWidth - 1 - t) / 2] = 84;
                                    compositeFrame->data[2][((zoomRectY + y) / 2) * compositeFrame->linesize[2] + (zoomRectX + zoomRectWidth - 1 - t) / 2] = 255;
                                }
                            }
                        }
                    }
                }
            }
            av_frame_free(&thumbnailFrame);
        }
    }
    
    // Add timecode overlay
    int overlay_x = 20;
    int overlay_y = 20;
    
    if (!renderTimecodeOnFrame(compositeFrame, timecode, overlay_x, overlay_y)) {
        std::cout << "[Screenshot] Note: Failed to render timecode on frame, continuing without overlay" << std::endl;
    }
    
    // Convert to RGB and copy to clipboard
    SwsContext* swsContext = sws_getContext(
        compositeFrame->width, compositeFrame->height, static_cast<AVPixelFormat>(compositeFrame->format),
        compositeFrame->width, compositeFrame->height, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );
    
    if (!swsContext) {
        std::cerr << "[Screenshot] Failed to create RGB conversion context" << std::endl;
        av_frame_free(&compositeFrame);
        return false;
    }
    
    uint8_t* rgbData = static_cast<uint8_t*>(av_malloc(compositeFrame->width * compositeFrame->height * 3));
    if (!rgbData) {
        std::cerr << "[Screenshot] Failed to allocate RGB buffer" << std::endl;
        sws_freeContext(swsContext);
        av_frame_free(&compositeFrame);
        return false;
    }
    
    uint8_t* rgbDataArray[1] = { rgbData };
    int rgbLinesize[1] = { compositeFrame->width * 3 };
    
    int scaledHeight = sws_scale(swsContext, compositeFrame->data, compositeFrame->linesize, 0, compositeFrame->height,
                                rgbDataArray, rgbLinesize);
    
    bool success = false;
    if (scaledHeight > 0) {
        success = copyImageToClipboard(rgbData, compositeFrame->width, compositeFrame->height);
        
        if (success) {
            std::cout << "[Screenshot] Advanced screenshot copied to clipboard successfully!" << std::endl;
            std::cout << "[Screenshot] Timecode: " << timecode << std::endl;
            std::cout << "[Screenshot] Final dimensions: " << compositeFrame->width << "x" << compositeFrame->height << std::endl;
            std::cout << "[Screenshot] Zoom: " << (isZoomEnabled ? "enabled" : "disabled") 
                      << ", Thumbnail: " << (showThumbnail && isZoomEnabled ? "shown" : "hidden") << std::endl;
        }
    }
    
    av_free(rgbData);
    sws_freeContext(swsContext);
    av_frame_free(&compositeFrame);
    
    // Clean up working frame if we allocated it
    if (needToFreeWorkingFrame) {
        av_frame_free(&workingFrame);
    }
    
    return success;
}

bool takeScreenshotWithTimecode(void* renderer, AVFrame* frame, const std::string& timecode, 
                               const std::string& outputPath, int windowWidth, int windowHeight) {
    if (!frame) {
        std::cerr << "[Screenshot] No frame available for screenshot" << std::endl;
        return false;
    }
    
    // Pass the exact timecode from display.mm to ensure synchronization
    return saveFrameAsPNGWithTimecode(frame, timecode, outputPath);
} 