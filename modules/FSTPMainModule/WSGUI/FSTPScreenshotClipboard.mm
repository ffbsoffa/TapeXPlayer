#include <Cocoa/Cocoa.h>
#include <iostream>

// Copy RGB24 image data to macOS clipboard
extern "C" bool CopyImageToClipboard(const uint8_t* rgb_data, int width, int height) {
    @autoreleasepool {
        if (!rgb_data || width <= 0 || height <= 0) {
            NSLog(@"❌ [CLIPBOARD] Invalid image data: width=%d, height=%d, data=%p", width, height, rgb_data);
            return false;
        }

        NSLog(@"📋 [CLIPBOARD] Creating image: %dx%d, %d bytes", width, height, width * height * 3);

        // Create CGImage from RGB24 data using data provider
        CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
        if (!colorSpace) {
            NSLog(@"❌ [CLIPBOARD] Failed to create color space");
            return false;
        }

        // Copy data to NSData for safety
        NSData* imageData = [NSData dataWithBytes:rgb_data length:width * height * 3];
        CGDataProviderRef provider = CGDataProviderCreateWithCFData((__bridge CFDataRef)imageData);

        if (!provider) {
            NSLog(@"❌ [CLIPBOARD] Failed to create data provider");
            CGColorSpaceRelease(colorSpace);
            return false;
        }

        CGImageRef cgImage = CGImageCreate(
            width,
            height,
            8,                          // bits per component
            24,                         // bits per pixel (RGB = 8*3)
            width * 3,                  // bytes per row
            colorSpace,
            kCGBitmapByteOrderDefault,  // bitmap info
            provider,
            NULL,                       // decode array
            false,                      // should interpolate
            kCGRenderingIntentDefault   // rendering intent
        );

        CGDataProviderRelease(provider);
        CGColorSpaceRelease(colorSpace);

        if (!cgImage) {
            NSLog(@"❌ [CLIPBOARD] Failed to create CGImage");
            return false;
        }

        // Convert to NSImage
        NSSize imageSize = NSMakeSize(width, height);
        NSImage* nsImage = [[NSImage alloc] initWithCGImage:cgImage size:imageSize];
        CGImageRelease(cgImage);

        if (!nsImage) {
            NSLog(@"❌ [CLIPBOARD] Failed to create NSImage");
            return false;
        }

        // Get TIFF representation for clipboard
        NSData* tiffData = [nsImage TIFFRepresentation];
        if (!tiffData) {
            NSLog(@"❌ [CLIPBOARD] Failed to get TIFF representation");
            return false;
        }

        // Copy to clipboard
        NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
        [pasteboard clearContents];
        BOOL success = [pasteboard setData:tiffData forType:NSPasteboardTypeTIFF];

        if (success) {
            NSLog(@"✅ [CLIPBOARD] Image copied successfully (%dx%d)", width, height);
        } else {
            NSLog(@"❌ [CLIPBOARD] Failed to write to pasteboard");
        }

        return success ? true : false;
    }
}
