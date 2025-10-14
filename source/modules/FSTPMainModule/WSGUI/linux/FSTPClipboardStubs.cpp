// Linux clipboard implementation using GTK3
#ifdef __linux__

#include <gtk/gtk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <iostream>
#include <cstring>
#include <cstdlib>
#include <unistd.h>

// External GTK initialization flag
extern bool g_gtk_initialized;

#ifdef __cplusplus
extern "C" {
#endif

// Copy RGB24 image data to Linux clipboard using wl-copy or xclip
bool CopyImageToClipboard(const uint8_t* rgb_data, int width, int height) {
    if (!rgb_data || width <= 0 || height <= 0) {
        std::cerr << "❌ [CLIPBOARD] Invalid image data: width=" << width
                  << ", height=" << height << ", data=" << (void*)rgb_data << std::endl;
        return false;
    }

    std::cout << "📋 [CLIPBOARD] Creating image: " << width << "x" << height
              << ", " << (width * height * 3) << " bytes" << std::endl;

    // Initialize GTK if needed
    if (!g_gtk_initialized) {
        gtk_init(nullptr, nullptr);
        g_gtk_initialized = true;
    }

    // Create pixbuf by copying data
    GdkPixbuf* pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, width, height);

    if (!pixbuf) {
        std::cerr << "❌ [CLIPBOARD] Failed to create GdkPixbuf" << std::endl;
        return false;
    }

    // Copy RGB data into pixbuf
    guchar* pixels = gdk_pixbuf_get_pixels(pixbuf);
    int rowstride = gdk_pixbuf_get_rowstride(pixbuf);

    for (int y = 0; y < height; y++) {
        memcpy(pixels + y * rowstride,
               rgb_data + y * width * 3,
               width * 3);
    }

    // Save to temporary PNG file
    char temp_filename[] = "/tmp/tapexplayer_screenshot_XXXXXX.png";
    int fd = mkstemps(temp_filename, 4); // 4 = length of ".png"
    if (fd == -1) {
        std::cerr << "❌ [CLIPBOARD] Failed to create temp file" << std::endl;
        g_object_unref(pixbuf);
        return false;
    }
    close(fd); // Close fd, we'll use the filename

    GError* error = nullptr;
    gboolean saved = gdk_pixbuf_save(pixbuf, temp_filename, "png", &error, NULL);
    g_object_unref(pixbuf);

    if (!saved) {
        std::cerr << "❌ [CLIPBOARD] Failed to save PNG: " << (error ? error->message : "unknown error") << std::endl;
        if (error) g_error_free(error);
        unlink(temp_filename);
        return false;
    }

    std::cout << "📋 [CLIPBOARD] Saved to temp file: " << temp_filename << std::endl;

    // Try wl-copy first (Wayland), then xclip (X11)
    bool success = false;

    // Check XDG_SESSION_TYPE to determine display server
    const char* session_type = getenv("XDG_SESSION_TYPE");
    std::cout << "📋 [CLIPBOARD] Session type: " << (session_type ? session_type : "unknown") << std::endl;

    if (session_type && strcmp(session_type, "wayland") == 0) {
        // Wayland - use wl-copy
        std::string cmd = "wl-copy --type image/png < ";
        cmd += temp_filename;
        std::cout << "📋 [CLIPBOARD] Using wl-copy (Wayland)" << std::endl;
        int ret = system(cmd.c_str());
        success = (ret == 0);
        if (!success) {
            std::cerr << "❌ [CLIPBOARD] wl-copy failed with exit code " << ret << std::endl;
            std::cerr << "💡 [CLIPBOARD] Install wl-clipboard package: sudo apt install wl-clipboard" << std::endl;
        }
    } else {
        // X11 - use xclip
        std::string cmd = "xclip -selection clipboard -t image/png -i ";
        cmd += temp_filename;
        std::cout << "📋 [CLIPBOARD] Using xclip (X11)" << std::endl;
        int ret = system(cmd.c_str());
        success = (ret == 0);
        if (!success) {
            std::cerr << "❌ [CLIPBOARD] xclip failed with exit code " << ret << std::endl;
            std::cerr << "💡 [CLIPBOARD] Install xclip package: sudo apt install xclip" << std::endl;
        }
    }

    // Clean up temp file
    unlink(temp_filename);

    if (success) {
        std::cout << "✅ [CLIPBOARD] Image copied successfully (" << width << "x" << height << ")" << std::endl;
    } else {
        std::cerr << "❌ [CLIPBOARD] Failed to copy to clipboard" << std::endl;
    }

    return success;
}

#ifdef __cplusplus
}
#endif

#endif // __linux__
