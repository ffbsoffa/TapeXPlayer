#include "../FSTPScreenshotClipboard.h"
#include <windows.h>

bool FSTPCopyScreenshotToClipboard(const uint8_t* rgbaData, int width, int height) {
    if (!rgbaData || width <= 0 || height <= 0) {
        return false;
    }

    // Create DIB section
    BITMAPINFOHEADER bi = {0};
    bi.biSize = sizeof(BITMAPINFOHEADER);
    bi.biWidth = width;
    bi.biHeight = -height;  // Negative height for top-down bitmap
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP hBitmap = CreateDIBSection(NULL, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!hBitmap) {
        return false;
    }

    // Copy pixel data
    memcpy(bits, rgbaData, width * height * 4);

    // Copy to clipboard
    if (OpenClipboard(NULL)) {
        EmptyClipboard();
        SetClipboardData(CF_BITMAP, hBitmap);
        CloseClipboard();
        return true;
    }

    DeleteObject(hBitmap);
    return false;
}

bool FSTPGetScreenshotFromClipboard(uint8_t** rgbaData, int* width, int* height) {
    if (!rgbaData || !width || !height) {
        return false;
    }

    if (!OpenClipboard(NULL)) {
        return false;
    }

    HBITMAP hBitmap = (HBITMAP)GetClipboardData(CF_BITMAP);
    if (!hBitmap) {
        CloseClipboard();
        return false;
    }

    BITMAP bm;
    GetObject(hBitmap, sizeof(BITMAP), &bm);

    *width = bm.bmWidth;
    *height = bm.bmHeight;
    *rgbaData = new uint8_t[bm.bmWidth * bm.bmHeight * 4];

    HDC hdc = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdc);
    HBITMAP hOldBitmap = (HBITMAP)SelectObject(hdcMem, hBitmap);

    BITMAPINFOHEADER bi = {0};
    bi.biSize = sizeof(BITMAPINFOHEADER);
    bi.biWidth = bm.bmWidth;
    bi.biHeight = -bm.bmHeight;
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;

    GetDIBits(hdcMem, hBitmap, 0, bm.bmHeight, *rgbaData, (BITMAPINFO*)&bi, DIB_RGB_COLORS);

    SelectObject(hdcMem, hOldBitmap);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdc);

    CloseClipboard();
    return true;
}