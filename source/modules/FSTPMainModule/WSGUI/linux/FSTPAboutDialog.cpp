#ifdef __linux__

#include <gtk/gtk.h>
#include <iostream>
#include <ctime>
#include "FSTPAboutDialog.h"
#include "BuildInfo.h"  // Build version information

// External GTK initialization flag
extern bool g_gtk_initialized;

// Build information accessors (using BuildInfo.h)
static const char* GetBuildDate() {
    return GetTapeXPlayerBuildDate();
}

static const char* GetBuildNumber() {
    return GetTapeXPlayerBuildNumber();
}

static const char* GetVersion() {
    return GetTapeXPlayerVersion();
}

static const char* GetCodeName() {
    return GetTapeXPlayerCodeName();
}

// Helper function to create a library info section
static GtkWidget* CreateLibraryInfoSection(
    const char* name,
    const char* description,
    const char* license,
    const char* url)
{
    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_top(vbox, 8);
    gtk_widget_set_margin_bottom(vbox, 8);

    // Library name (bold)
    GtkWidget* name_label = gtk_label_new(nullptr);
    char* name_markup = g_markup_printf_escaped("<b>%s</b>", name);
    gtk_label_set_markup(GTK_LABEL(name_label), name_markup);
    g_free(name_markup);
    gtk_label_set_xalign(GTK_LABEL(name_label), 0.0);
    gtk_box_pack_start(GTK_BOX(vbox), name_label, FALSE, FALSE, 0);

    // Description
    GtkWidget* desc_label = gtk_label_new(description);
    gtk_label_set_xalign(GTK_LABEL(desc_label), 0.0);
    gtk_widget_set_opacity(desc_label, 0.7);
    gtk_box_pack_start(GTK_BOX(vbox), desc_label, FALSE, FALSE, 0);

    // License
    GtkWidget* license_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget* license_title = gtk_label_new("License:");
    gtk_widget_set_opacity(license_title, 0.7);
    GtkWidget* license_value = gtk_label_new(license);
    gtk_widget_set_opacity(license_value, 0.7);
    gtk_box_pack_start(GTK_BOX(license_box), license_title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(license_box), license_value, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), license_box, FALSE, FALSE, 0);

    // URL (clickable link)
    GtkWidget* url_label = gtk_link_button_new_with_label(url, url);
    gtk_button_set_relief(GTK_BUTTON(url_label), GTK_RELIEF_NONE);
    gtk_widget_set_halign(url_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), url_label, FALSE, FALSE, 0);

    return vbox;
}

// Helper function to create a separator
static GtkWidget* CreateSeparator() {
    GtkWidget* separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top(separator, 4);
    gtk_widget_set_margin_bottom(separator, 4);
    return separator;
}

void ShowGTKAboutDialog() {
    std::cout << "🔵 ShowGTKAboutDialog called" << std::endl;

    // Initialize GTK if needed
    if (!g_gtk_initialized) {
        gtk_init(nullptr, nullptr);
        g_gtk_initialized = true;
    }

    // Create main window
    GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "About TapeXPlayer");
    gtk_window_set_default_size(GTK_WINDOW(window), 700, 550);
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
    gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);
    gtk_container_set_border_width(GTK_CONTAINER(window), 0);

    // Main horizontal box (left panel + right panel)
    GtkWidget* main_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_container_add(GTK_CONTAINER(window), main_hbox);

    // === LEFT PANEL (2/5 width) ===
    GtkWidget* left_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_size_request(left_panel, 280, -1);  // 2/5 of 700 = 280px
    gtk_widget_set_margin_start(left_panel, 20);
    gtk_widget_set_margin_end(left_panel, 20);
    gtk_widget_set_margin_top(left_panel, 40);
    gtk_widget_set_margin_bottom(left_panel, 20);

    // Spacer at top
    GtkWidget* top_spacer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(top_spacer, TRUE);
    gtk_box_pack_start(GTK_BOX(left_panel), top_spacer, TRUE, TRUE, 0);

    // Application icon (use GTK default application icon if no custom icon available)
    GtkWidget* icon_image = gtk_image_new_from_icon_name("video-x-generic", GTK_ICON_SIZE_DIALOG);
    gtk_image_set_pixel_size(GTK_IMAGE(icon_image), 128);
    gtk_box_pack_start(GTK_BOX(left_panel), icon_image, FALSE, FALSE, 0);

    // Application name (large bold text)
    GtkWidget* app_name = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(app_name), "<span size='xx-large' weight='bold'>TapeXPlayer</span>");
    gtk_box_pack_start(GTK_BOX(left_panel), app_name, FALSE, FALSE, 0);

    // Build information
    char build_info[256];
    if (GetCodeName()) {
        snprintf(build_info, sizeof(build_info),
                 "Build %s \"%s\" (%s)", GetBuildNumber(), GetCodeName(), GetBuildDate());
    } else {
        snprintf(build_info, sizeof(build_info),
                 "Build %s (%s)", GetBuildNumber(), GetBuildDate());
    }
    GtkWidget* build_label = gtk_label_new(build_info);
    gtk_widget_set_opacity(build_label, 0.7);
    gtk_box_pack_start(GTK_BOX(left_panel), build_label, FALSE, FALSE, 4);

    // Spacer in middle
    GtkWidget* mid_spacer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(mid_spacer, TRUE);
    gtk_box_pack_start(GTK_BOX(left_panel), mid_spacer, TRUE, TRUE, 0);

    // Copyright and links section
    GtkWidget* copyright_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);

    // Copyright text
    GtkWidget* copyright_label = gtk_label_new(
        "© 2026 Maksim Maloletkin (FFB_soffa).\n"
        "Licensed under GPL."
    );
    gtk_label_set_justify(GTK_LABEL(copyright_label), GTK_JUSTIFY_CENTER);
    gtk_widget_set_opacity(copyright_label, 0.7);
    gtk_box_pack_start(GTK_BOX(copyright_vbox), copyright_label, FALSE, FALSE, 0);

    // GitHub link
    GtkWidget* github_link = gtk_link_button_new_with_label(
        "https://github.com/ffbsoffa/TapeXPlayer",
        "github.com/ffbsoffa/TapeXPlayer"
    );
    gtk_button_set_relief(GTK_BUTTON(github_link), GTK_RELIEF_NONE);
    gtk_box_pack_start(GTK_BOX(copyright_vbox), github_link, FALSE, FALSE, 0);

    // Website link
    GtkWidget* website_link = gtk_link_button_new_with_label(
        "https://ffbsoffa.org",
        "ffbsoffa.org"
    );
    gtk_button_set_relief(GTK_BUTTON(website_link), GTK_RELIEF_NONE);
    gtk_box_pack_start(GTK_BOX(copyright_vbox), website_link, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(left_panel), copyright_vbox, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(main_hbox), left_panel, FALSE, FALSE, 0);

    // === RIGHT PANEL (3/5 width) with ScrolledWindow ===
    GtkWidget* right_panel_frame = gtk_frame_new(nullptr);
    gtk_frame_set_shadow_type(GTK_FRAME(right_panel_frame), GTK_SHADOW_IN);
    gtk_widget_set_size_request(right_panel_frame, 420, -1);  // 3/5 of 700 = 420px

    // ScrolledWindow for libraries list
    GtkWidget* scrolled_window = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window),
                                   GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(right_panel_frame), scrolled_window);

    // Content vbox inside scrolled window
    GtkWidget* content_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_start(content_vbox, 20);
    gtk_widget_set_margin_end(content_vbox, 20);
    gtk_widget_set_margin_top(content_vbox, 20);
    gtk_widget_set_margin_bottom(content_vbox, 20);
    gtk_container_add(GTK_CONTAINER(scrolled_window), content_vbox);

    // Header
    GtkWidget* header = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(header), "<span size='large' weight='bold'>Third-Party Libraries</span>");
    gtk_label_set_xalign(GTK_LABEL(header), 0.0);
    gtk_widget_set_margin_bottom(header, 16);
    gtk_box_pack_start(GTK_BOX(content_vbox), header, FALSE, FALSE, 0);

    // FFmpeg
    gtk_box_pack_start(GTK_BOX(content_vbox),
        CreateLibraryInfoSection(
            "FFmpeg",
            "Video and audio codec library",
            "LGPL v2.1+ / GPL v2+",
            "https://ffmpeg.org"
        ), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content_vbox), CreateSeparator(), FALSE, FALSE, 0);

    // SDL2
    gtk_box_pack_start(GTK_BOX(content_vbox),
        CreateLibraryInfoSection(
            "SDL2",
            "Cross-platform multimedia library",
            "zlib License",
            "https://www.libsdl.org"
        ), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content_vbox), CreateSeparator(), FALSE, FALSE, 0);

    // SDL2_ttf
    gtk_box_pack_start(GTK_BOX(content_vbox),
        CreateLibraryInfoSection(
            "SDL2_ttf",
            "TrueType font rendering library",
            "zlib License",
            "https://github.com/libsdl-org/SDL_ttf"
        ), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content_vbox), CreateSeparator(), FALSE, FALSE, 0);

    // PortAudio
    gtk_box_pack_start(GTK_BOX(content_vbox),
        CreateLibraryInfoSection(
            "PortAudio",
            "Cross-platform audio I/O library",
            "MIT-like License",
            "http://www.portaudio.com"
        ), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content_vbox), CreateSeparator(), FALSE, FALSE, 0);

    // RtMidi
    gtk_box_pack_start(GTK_BOX(content_vbox),
        CreateLibraryInfoSection(
            "RtMidi",
            "Cross-platform MIDI I/O library",
            "MIT-like License",
            "https://github.com/thestk/rtmidi"
        ), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content_vbox), CreateSeparator(), FALSE, FALSE, 0);

    // OpenSSL
    gtk_box_pack_start(GTK_BOX(content_vbox),
        CreateLibraryInfoSection(
            "OpenSSL",
            "Cryptography and SSL/TLS toolkit",
            "Apache License 2.0",
            "https://www.openssl.org"
        ), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content_vbox), CreateSeparator(), FALSE, FALSE, 0);

    // === GTK Section (instead of Apple Frameworks) ===
    GtkWidget* gtk_header = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(gtk_header), "<span size='large' weight='bold'>GTK Frameworks (Linux)</span>");
    gtk_label_set_xalign(GTK_LABEL(gtk_header), 0.0);
    gtk_widget_set_margin_top(gtk_header, 16);
    gtk_widget_set_margin_bottom(gtk_header, 12);
    gtk_box_pack_start(GTK_BOX(content_vbox), gtk_header, FALSE, FALSE, 0);

    // GTK frameworks (simplified list)
    const char* gtk_frameworks[] = {
        "GTK3 - Cross-platform GUI toolkit",
        "GDK - Low-level graphics and windowing system",
        "GLib - Core application building blocks",
        "Cairo - 2D graphics library",
        "Pango - Text layout and rendering",
        "X11 / Wayland - Display server protocols",
        nullptr
    };

    for (int i = 0; gtk_frameworks[i] != nullptr; i++) {
        GtkWidget* framework_label = gtk_label_new(gtk_frameworks[i]);
        gtk_label_set_xalign(GTK_LABEL(framework_label), 0.0);
        gtk_widget_set_opacity(framework_label, 0.7);
        gtk_widget_set_margin_start(framework_label, 12);
        gtk_widget_set_margin_top(framework_label, 4);
        gtk_widget_set_margin_bottom(framework_label, 4);
        gtk_box_pack_start(GTK_BOX(content_vbox), framework_label, FALSE, FALSE, 0);
    }

    GtkWidget* gtk_notice = gtk_label_new(
        "GTK and related libraries are © The GTK Team and subject to LGPL license."
    );
    gtk_label_set_xalign(GTK_LABEL(gtk_notice), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(gtk_notice), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(gtk_notice), 50);
    gtk_widget_set_opacity(gtk_notice, 0.6);
    gtk_widget_set_margin_top(gtk_notice, 12);
    gtk_box_pack_start(GTK_BOX(content_vbox), gtk_notice, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(content_vbox), CreateSeparator(), FALSE, FALSE, 16);

    // === Disclaimer ===
    GtkWidget* disclaimer_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);

    const char* disclaimer_texts[] = {
        "This software uses libraries from the FFmpeg project under the LGPLv2.1.",
        "FFmpeg is a trademark of Fabrice Bellard.",
        "All trademarks are property of their respective owners.",
        nullptr
    };

    for (int i = 0; disclaimer_texts[i] != nullptr; i++) {
        GtkWidget* disclaimer_label = gtk_label_new(disclaimer_texts[i]);
        gtk_label_set_xalign(GTK_LABEL(disclaimer_label), 0.0);
        gtk_label_set_line_wrap(GTK_LABEL(disclaimer_label), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(disclaimer_label), 50);
        gtk_widget_set_opacity(disclaimer_label, 0.6);
        gtk_box_pack_start(GTK_BOX(disclaimer_vbox), disclaimer_label, FALSE, FALSE, 0);
    }

    gtk_box_pack_start(GTK_BOX(content_vbox), disclaimer_vbox, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(main_hbox), right_panel_frame, TRUE, TRUE, 0);

    // Connect close signal
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_widget_destroyed), &window);

    // Show all widgets
    gtk_widget_show_all(window);

    std::cout << "✅ About dialog displayed" << std::endl;
}

#endif // __linux__
