#ifdef __linux__

#include <gtk/gtk.h>
#include <portaudio.h>
#include <iostream>
#include <cmath>
#include <string>
#include <string>
#include "../FSTPSettings.h"
#include "../FSTPWindowManager.h"
#include "FSTPSettingsDialog.h"

// GTK initialization flag (shared across dialogs)
extern bool g_gtk_initialized;
extern bool g_dialog_open;

// RAII guard to ensure dialog flag is reset
class DialogGuard {
public:
    DialogGuard() {
        g_dialog_open = true;
    }
    ~DialogGuard() {
        g_dialog_open = false;
        std::cout << "✅ Settings dialog closed (flag reset)" << std::endl;
    }
};

// Forward declarations for MIDI functions
extern "C" int GetMIDIInputDeviceCount();
extern "C" int GetMIDIOutputDeviceCount();
extern "C" const char* GetMIDIInputDeviceName(int index);
extern "C" const char* GetMIDIOutputDeviceName(int index);
extern "C" void ApplyMIDISettings();

// Forward declarations for cache functions (implemented in other files)
extern "C" const char* FSTP_GetProxyCachePath();
extern "C" const char* FSTP_GetMemoryLocationsCachePath();
extern "C" int FSTP_GetProxyCacheSize();
extern "C" int FSTP_GetProxyFilesCount();
extern "C" void FSTP_ClearProxyCache(bool onlyInactive);
extern "C" int FSTP_GetMemoryLocationsFilesCount();
extern "C" const char* FSTP_GetMemoryLocationsFileName(int index);
extern "C" void FSTP_ClearAllMemoryLocations();
extern "C" bool FSTP_IsAnyPlayerActive();
extern "C" void ResetSettingsToDefault();

// GTK3 Settings Dialog Implementation
void ShowGTKSettingsDialog() {
    std::cout << "🔵 ShowGTKSettingsDialog called" << std::endl;

    // Prevent multiple dialog invocations
    if (g_dialog_open) {
        std::cout << "⚠️ Dialog already open, skipping call" << std::endl;
        return;
    }

    DialogGuard guard;

    // Initialize GTK if needed
    if (!g_gtk_initialized) {
        gtk_init(nullptr, nullptr);
        g_gtk_initialized = true;
    }

    // Get current settings
    const FSTPSettings* settings = GetSettings();
    if (!settings) {
        std::cerr << "❌ Failed to get settings" << std::endl;
        return;
    }

    // Create dialog
    GtkWidget* dialog = gtk_dialog_new_with_buttons(
        "Settings",
        nullptr,
        GTK_DIALOG_MODAL,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_OK", GTK_RESPONSE_ACCEPT,
        nullptr
    );

    gtk_window_set_default_size(GTK_WINDOW(dialog), 800, 550);
    gtk_window_set_resizable(GTK_WINDOW(dialog), TRUE);

    // Content area
    GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(content), 0);

    // Create PANED layout (like NSSplitView on macOS)
    GtkWidget* paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);

    // Make paned expand to fill all available space
    gtk_widget_set_hexpand(paned, TRUE);
    gtk_widget_set_vexpand(paned, TRUE);

    gtk_box_pack_start(GTK_BOX(content), paned, TRUE, TRUE, 0);

    // Add separator between content and buttons
    GtkWidget* separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(content), separator, FALSE, FALSE, 0);

    // LEFT: Sidebar with category list
    GtkWidget* sidebar = gtk_list_box_new();
    gtk_widget_set_size_request(sidebar, 180, -1);
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(sidebar), GTK_SELECTION_SINGLE);

    // Sidebar items
    GtkWidget* audio_row = gtk_label_new("Audio");
    gtk_widget_set_halign(audio_row, GTK_ALIGN_START);
    gtk_widget_set_margin_start(audio_row, 16);
    gtk_widget_set_margin_end(audio_row, 16);
    gtk_widget_set_margin_top(audio_row, 10);
    gtk_widget_set_margin_bottom(audio_row, 10);
    gtk_list_box_insert(GTK_LIST_BOX(sidebar), audio_row, -1);

    GtkWidget* video_row = gtk_label_new("Video & Sync");
    gtk_widget_set_halign(video_row, GTK_ALIGN_START);
    gtk_widget_set_margin_start(video_row, 16);
    gtk_widget_set_margin_end(video_row, 16);
    gtk_widget_set_margin_top(video_row, 10);
    gtk_widget_set_margin_bottom(video_row, 10);
    gtk_list_box_insert(GTK_LIST_BOX(sidebar), video_row, -1);

    GtkWidget* midi_row = gtk_label_new("MIDI");
    gtk_widget_set_halign(midi_row, GTK_ALIGN_START);
    gtk_widget_set_margin_start(midi_row, 16);
    gtk_widget_set_margin_end(midi_row, 16);
    gtk_widget_set_margin_top(midi_row, 10);
    gtk_widget_set_margin_bottom(midi_row, 10);
    gtk_list_box_insert(GTK_LIST_BOX(sidebar), midi_row, -1);

    GtkWidget* cache_row = gtk_label_new("Cache & Data");
    gtk_widget_set_halign(cache_row, GTK_ALIGN_START);
    gtk_widget_set_margin_start(cache_row, 16);
    gtk_widget_set_margin_end(cache_row, 16);
    gtk_widget_set_margin_top(cache_row, 10);
    gtk_widget_set_margin_bottom(cache_row, 10);
    gtk_list_box_insert(GTK_LIST_BOX(sidebar), cache_row, -1);

    GtkWidget* extensions_row = gtk_label_new("Extensions");
    gtk_widget_set_halign(extensions_row, GTK_ALIGN_START);
    gtk_widget_set_margin_start(extensions_row, 16);
    gtk_widget_set_margin_end(extensions_row, 16);
    gtk_widget_set_margin_top(extensions_row, 10);
    gtk_widget_set_margin_bottom(extensions_row, 10);
    gtk_list_box_insert(GTK_LIST_BOX(sidebar), extensions_row, -1);

    // Select first item
    gtk_list_box_select_row(GTK_LIST_BOX(sidebar), gtk_list_box_get_row_at_index(GTK_LIST_BOX(sidebar), 0));

    // Add sidebar to left pane (will not resize)
    gtk_paned_pack1(GTK_PANED(paned), sidebar, FALSE, FALSE);

    // RIGHT: Stack for content pages
    GtkWidget* stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(GTK_STACK(stack), 150);

    // Add stack to right pane (will resize and take remaining space)
    gtk_paned_pack2(GTK_PANED(paned), stack, TRUE, TRUE);

    // Set initial sidebar position (180px from left)
    gtk_paned_set_position(GTK_PANED(paned), 180);

    // Connect sidebar selection to stack
    auto sidebar_callback = +[](GtkListBox*, GtkListBoxRow* row, gpointer data) -> void {
        if (!row) return;
        GtkStack* stack = GTK_STACK(data);
        int index = gtk_list_box_row_get_index(row);

        const char* page_names[] = {"audio", "video", "midi", "cache", "extensions"};
        if (index >= 0 && index < 5) {
            gtk_stack_set_visible_child_name(stack, page_names[index]);
        }
    };
    g_signal_connect(sidebar, "row-selected", G_CALLBACK(sidebar_callback), stack);

    // ===== AUDIO PAGE =====
    GtkWidget* audio_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(audio_page), 20);

    // Audio device selection
    GtkWidget* audio_device_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget* audio_device_label = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(audio_device_label), "<b>Audio Device</b>");
    gtk_widget_set_halign(audio_device_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(audio_device_box), audio_device_label, FALSE, FALSE, 0);

    GtkWidget* device_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(device_combo), "Default Audio Device");

    int device_count = Pa_GetDeviceCount();
    for (int i = 0; i < device_count; i++) {
        const PaDeviceInfo* dev_info = Pa_GetDeviceInfo(i);
        if (dev_info && dev_info->maxOutputChannels > 0) {
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(device_combo), dev_info->name);
        }
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(device_combo), settings->audio_device_index + 1);
    gtk_box_pack_start(GTK_BOX(audio_device_box), device_combo, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(audio_page), audio_device_box, FALSE, FALSE, 0);

    // Master volume
    GtkWidget* volume_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget* volume_label = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(volume_label), "<b>Volume</b>");
    gtk_widget_set_halign(volume_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(volume_box), volume_label, FALSE, FALSE, 0);

    GtkWidget* volume_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.0, 1.0, 0.01);
    gtk_range_set_value(GTK_RANGE(volume_scale), settings->audio_master_volume);
    gtk_scale_set_value_pos(GTK_SCALE(volume_scale), GTK_POS_RIGHT);
    gtk_box_pack_start(GTK_BOX(volume_box), volume_scale, FALSE, FALSE, 0);

    // Volume ducking (ear protection) checkbox
    GtkWidget* ducking_check = gtk_check_button_new_with_label("Auto-Reduce Volume at High Speeds");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ducking_check), settings->audio_volume_ducking_enabled);
    gtk_box_pack_start(GTK_BOX(volume_box), ducking_check, FALSE, FALSE, 0);

    GtkWidget* ducking_note = gtk_label_new("Protects your ears during shuttle (6x: fade starts, 12x: -24dB, 32x: -40dB)");
    gtk_widget_set_halign(ducking_note, GTK_ALIGN_START);
    PangoAttrList* ducking_attrs = pango_attr_list_new();
    pango_attr_list_insert(ducking_attrs, pango_attr_scale_new(PANGO_SCALE_SMALL));
    gtk_label_set_attributes(GTK_LABEL(ducking_note), ducking_attrs);
    pango_attr_list_unref(ducking_attrs);
    gtk_box_pack_start(GTK_BOX(volume_box), ducking_note, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(audio_page), volume_box, FALSE, FALSE, 0);

    // Buffer size
    GtkWidget* buffer_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget* buffer_label = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(buffer_label), "<b>Performance</b>");
    gtk_widget_set_halign(buffer_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(buffer_box), buffer_label, FALSE, FALSE, 0);

    GtkWidget* buffer_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(buffer_combo), "512 samples");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(buffer_combo), "1024 samples");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(buffer_combo), "2048 samples");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(buffer_combo), "4096 samples");

    int buffer_index = 1; // default 1024
    if (settings->audio_buffer_size == 512) buffer_index = 0;
    else if (settings->audio_buffer_size == 1024) buffer_index = 1;
    else if (settings->audio_buffer_size == 2048) buffer_index = 2;
    else if (settings->audio_buffer_size == 4096) buffer_index = 3;
    gtk_combo_box_set_active(GTK_COMBO_BOX(buffer_combo), buffer_index);

    gtk_box_pack_start(GTK_BOX(buffer_box), buffer_combo, FALSE, FALSE, 0);

    GtkWidget* buffer_note = gtk_label_new("Note: Buffer size changes require application restart");
    gtk_widget_set_halign(buffer_note, GTK_ALIGN_START);
    PangoAttrList* attrs = pango_attr_list_new();
    pango_attr_list_insert(attrs, pango_attr_scale_new(PANGO_SCALE_SMALL));
    gtk_label_set_attributes(GTK_LABEL(buffer_note), attrs);
    pango_attr_list_unref(attrs);
    gtk_box_pack_start(GTK_BOX(buffer_box), buffer_note, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(audio_page), buffer_box, FALSE, FALSE, 0);

    gtk_stack_add_named(GTK_STACK(stack), audio_page, "audio");

    // ===== VIDEO & SYNC PAGE =====
    GtkWidget* video_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(video_page), 20);

    // Frame offset
    GtkWidget* offset_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget* offset_label = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(offset_label), "<b>Display Synchronization</b>");
    gtk_widget_set_halign(offset_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(offset_box), offset_label, FALSE, FALSE, 0);

    GtkWidget* offset_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(offset_hbox), gtk_label_new("Frame Offset:"), FALSE, FALSE, 0);

    GtkWidget* offset_spin = gtk_spin_button_new_with_range(-10, 10, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(offset_spin), settings->frame_offset);
    gtk_box_pack_start(GTK_BOX(offset_hbox), offset_spin, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(offset_hbox), gtk_label_new("frames"), FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(offset_box), offset_hbox, FALSE, FALSE, 0);

    GtkWidget* offset_note = gtk_label_new("Compensate for display lag (-10 to +10 frames)");
    gtk_widget_set_halign(offset_note, GTK_ALIGN_START);
    attrs = pango_attr_list_new();
    pango_attr_list_insert(attrs, pango_attr_scale_new(PANGO_SCALE_SMALL));
    gtk_label_set_attributes(GTK_LABEL(offset_note), attrs);
    pango_attr_list_unref(attrs);
    gtk_box_pack_start(GTK_BOX(offset_box), offset_note, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(video_page), offset_box, FALSE, FALSE, 0);

    // Auto-freeze inactive
    GtkWidget* freeze_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget* freeze_title = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(freeze_title), "<b>Multi-Instance Performance</b>");
    gtk_widget_set_halign(freeze_title, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(freeze_box), freeze_title, FALSE, FALSE, 0);

    GtkWidget* freeze_check = gtk_check_button_new_with_label("Auto-freeze inactive players");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(freeze_check), settings->auto_freeze_inactive);
    gtk_box_pack_start(GTK_BOX(freeze_box), freeze_check, FALSE, FALSE, 0);

    GtkWidget* freeze_note = gtk_label_new("Prevents forgotten players from consuming resources");
    gtk_widget_set_halign(freeze_note, GTK_ALIGN_START);
    attrs = pango_attr_list_new();
    pango_attr_list_insert(attrs, pango_attr_scale_new(PANGO_SCALE_SMALL));
    gtk_label_set_attributes(GTK_LABEL(freeze_note), attrs);
    pango_attr_list_unref(attrs);
    gtk_box_pack_start(GTK_BOX(freeze_box), freeze_note, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(video_page), freeze_box, FALSE, FALSE, 0);

    GtkWidget* betacam_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget* betacam_title = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(betacam_title), "<b>Visual Effects</b>");
    gtk_widget_set_halign(betacam_title, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(betacam_box), betacam_title, FALSE, FALSE, 0);

    GtkWidget* betacam_check = gtk_check_button_new_with_label("Enable Betacam tape artefact emulation");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(betacam_check), settings->betacam_effect_enabled);
    gtk_box_pack_start(GTK_BOX(betacam_box), betacam_check, FALSE, FALSE, 0);

    GtkWidget* betacam_note = gtk_label_new("Adds rewind/fast-forward tape jitter. May impact performance on slower GPUs.");
    gtk_widget_set_halign(betacam_note, GTK_ALIGN_START);
    attrs = pango_attr_list_new();
    pango_attr_list_insert(attrs, pango_attr_scale_new(PANGO_SCALE_SMALL));
    gtk_label_set_attributes(GTK_LABEL(betacam_note), attrs);
    pango_attr_list_unref(attrs);
    gtk_box_pack_start(GTK_BOX(betacam_box), betacam_note, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(video_page), betacam_box, FALSE, FALSE, 0);

    // Developer/Debug settings
    GtkWidget* debug_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget* debug_title = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(debug_title), "<b>Developer/Debug</b>");
    gtk_widget_set_halign(debug_title, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(debug_box), debug_title, FALSE, FALSE, 0);

    GtkWidget* decoder_status_check = gtk_check_button_new_with_label("Show Decoder Status");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(decoder_status_check), settings->show_decoder_status);
    gtk_box_pack_start(GTK_BOX(debug_box), decoder_status_check, FALSE, FALSE, 0);

    GtkWidget* decoder_note = gtk_label_new("Displays decoded frames indicator at the top of the screen. Useful for debugging decoder performance.");
    gtk_widget_set_halign(decoder_note, GTK_ALIGN_START);
    gtk_label_set_line_wrap(GTK_LABEL(decoder_note), TRUE);
    gtk_label_set_xalign(GTK_LABEL(decoder_note), 0.0f);
    attrs = pango_attr_list_new();
    pango_attr_list_insert(attrs, pango_attr_scale_new(PANGO_SCALE_SMALL));
    gtk_label_set_attributes(GTK_LABEL(decoder_note), attrs);
    pango_attr_list_unref(attrs);
    gtk_box_pack_start(GTK_BOX(debug_box), decoder_note, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(video_page), debug_box, FALSE, FALSE, 0);

    gtk_stack_add_named(GTK_STACK(stack), video_page, "video");

    // ===== MIDI PAGE =====
    GtkWidget* midi_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(midi_page), 20);

    // MIDI enable
    GtkWidget* midi_enable_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget* midi_enable_label = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(midi_enable_label), "<b>MIDI Controller</b>");
    gtk_widget_set_halign(midi_enable_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(midi_enable_box), midi_enable_label, FALSE, FALSE, 0);

    GtkWidget* midi_enable_check = gtk_check_button_new_with_label("Enable MIDI Controller");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(midi_enable_check), settings->midi_enabled);
    gtk_box_pack_start(GTK_BOX(midi_enable_box), midi_enable_check, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(midi_page), midi_enable_box, FALSE, FALSE, 0);

    // MIDI ports
    GtkWidget* midi_ports_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget* midi_ports_label = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(midi_ports_label), "<b>MIDI Ports</b>");
    gtk_widget_set_halign(midi_ports_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(midi_ports_box), midi_ports_label, FALSE, FALSE, 0);

    // Input port
    GtkWidget* input_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(input_hbox), gtk_label_new("Input Port:"), FALSE, FALSE, 0);
    GtkWidget* input_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(input_combo), "(None)");
    int midi_input_count = GetMIDIInputDeviceCount();
    for (int i = 0; i < midi_input_count; i++) {
        const char* name = GetMIDIInputDeviceName(i);
        if (name) {
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(input_combo), name);
        }
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(input_combo), settings->midi_input_port + 1);
    gtk_widget_set_sensitive(input_combo, settings->midi_enabled);
    gtk_box_pack_start(GTK_BOX(input_hbox), input_combo, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(midi_ports_box), input_hbox, FALSE, FALSE, 0);

    // Output port
    GtkWidget* output_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(output_hbox), gtk_label_new("Output Port:"), FALSE, FALSE, 0);
    GtkWidget* output_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(output_combo), "(None)");
    int midi_output_count = GetMIDIOutputDeviceCount();
    for (int i = 0; i < midi_output_count; i++) {
        const char* name = GetMIDIOutputDeviceName(i);
        if (name) {
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(output_combo), name);
        }
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(output_combo), settings->midi_output_port + 1);
    gtk_widget_set_sensitive(output_combo, settings->midi_enabled);
    gtk_box_pack_start(GTK_BOX(output_hbox), output_combo, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(midi_ports_box), output_hbox, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(midi_page), midi_ports_box, FALSE, FALSE, 0);

    // MIDI info
    GtkWidget* midi_info_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget* midi_info_label = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(midi_info_label), "<b>Protocol Information</b>");
    gtk_widget_set_halign(midi_info_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(midi_info_box), midi_info_label, FALSE, FALSE, 0);

    GtkWidget* midi_proto = gtk_label_new("Mackie Control Protocol");
    gtk_widget_set_halign(midi_proto, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(midi_info_box), midi_proto, FALSE, FALSE, 0);

    GtkWidget* midi_warning = gtk_label_new("⚠️ Controller support is in development");
    gtk_widget_set_halign(midi_warning, GTK_ALIGN_START);
    attrs = pango_attr_list_new();
    pango_attr_list_insert(attrs, pango_attr_scale_new(PANGO_SCALE_SMALL));
    pango_attr_list_insert(attrs, pango_attr_foreground_new(0xFFFF, 0xA500, 0));
    gtk_label_set_attributes(GTK_LABEL(midi_warning), attrs);
    pango_attr_list_unref(attrs);
    gtk_box_pack_start(GTK_BOX(midi_info_box), midi_warning, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(midi_page), midi_info_box, FALSE, FALSE, 0);

    // Connect MIDI enable to port sensitivity
    struct MIDIWidgets { GtkWidget* input; GtkWidget* output; };
    MIDIWidgets* midi_widgets = new MIDIWidgets{input_combo, output_combo};

    g_signal_connect(midi_enable_check, "toggled", G_CALLBACK(+[](GtkToggleButton* button, gpointer data) {
        struct MIDIWidgets { GtkWidget* input; GtkWidget* output; };
        MIDIWidgets* w = static_cast<MIDIWidgets*>(data);
        gboolean active = gtk_toggle_button_get_active(button);
        gtk_widget_set_sensitive(w->input, active);
        gtk_widget_set_sensitive(w->output, active);
    }), midi_widgets);

    gtk_stack_add_named(GTK_STACK(stack), midi_page, "midi");

    // ===== CACHE & DATA PAGE =====
    GtkWidget* cache_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(cache_page), 20);

    // Proxy cache info
    GtkWidget* proxy_cache_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget* proxy_cache_label = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(proxy_cache_label), "<b>Proxy Video Cache</b>");
    gtk_widget_set_halign(proxy_cache_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(proxy_cache_box), proxy_cache_label, FALSE, FALSE, 0);

    const char* proxy_path = FSTP_GetProxyCachePath();
    int proxy_size = FSTP_GetProxyCacheSize();
    int proxy_count = FSTP_GetProxyFilesCount();

    char proxy_info[512];
    snprintf(proxy_info, sizeof(proxy_info), "Location: %s\nCache Size: %d MB (%d files)",
             proxy_path ? proxy_path : "Unknown", proxy_size, proxy_count);
    GtkWidget* proxy_info_label = gtk_label_new(proxy_info);
    gtk_widget_set_halign(proxy_info_label, GTK_ALIGN_START);
    gtk_label_set_line_wrap(GTK_LABEL(proxy_info_label), TRUE);
    attrs = pango_attr_list_new();
    pango_attr_list_insert(attrs, pango_attr_scale_new(PANGO_SCALE_SMALL));
    pango_attr_list_insert(attrs, pango_attr_family_new("monospace"));
    gtk_label_set_attributes(GTK_LABEL(proxy_info_label), attrs);
    pango_attr_list_unref(attrs);
    gtk_box_pack_start(GTK_BOX(proxy_cache_box), proxy_info_label, FALSE, FALSE, 0);

    GtkWidget* proxy_buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    if (FSTP_IsAnyPlayerActive()) {
        GtkWidget* clear_inactive_btn = gtk_button_new_with_label("Clear Inactive");
        g_signal_connect(clear_inactive_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer) {
            FSTP_ClearProxyCache(true);  // Keep active files
            std::cout << "✅ Inactive proxy cache cleared" << std::endl;
        }), nullptr);
        gtk_box_pack_end(GTK_BOX(proxy_buttons), clear_inactive_btn, FALSE, FALSE, 0);
    }
    GtkWidget* clear_all_proxy_btn = gtk_button_new_with_label("Clear All");
    gtk_widget_set_sensitive(clear_all_proxy_btn, proxy_count > 0);
    g_signal_connect(clear_all_proxy_btn, "clicked", G_CALLBACK(+[](GtkButton* button, gpointer user_data) {
        GtkWidget* dialog_widget = GTK_WIDGET(user_data);

        GtkWidget* confirm_dialog = gtk_message_dialog_new(
            GTK_WINDOW(gtk_widget_get_toplevel(dialog_widget)),
            GTK_DIALOG_MODAL,
            GTK_MESSAGE_WARNING,
            GTK_BUTTONS_YES_NO,
            "Clear All Proxy Cache?"
        );

        int proxy_count = FSTP_GetProxyFilesCount();
        int proxy_size = FSTP_GetProxyCacheSize();
        char message[256];
        snprintf(message, sizeof(message),
                 "This will delete all proxy video files (%d files, %d MB). This cannot be undone.",
                 proxy_count, proxy_size);
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(confirm_dialog), "%s", message);

        gint response = gtk_dialog_run(GTK_DIALOG(confirm_dialog));
        gtk_widget_destroy(confirm_dialog);

        if (response == GTK_RESPONSE_YES) {
            FSTP_ClearProxyCache(false);  // Clear all
            std::cout << "✅ All proxy cache cleared" << std::endl;
        }
    }), dialog);
    gtk_box_pack_end(GTK_BOX(proxy_buttons), clear_all_proxy_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(proxy_cache_box), proxy_buttons, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(cache_page), proxy_cache_box, FALSE, FALSE, 0);

    // Memory locations info
    GtkWidget* memory_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget* memory_label = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(memory_label), "<b>Memory Locations</b>");
    gtk_widget_set_halign(memory_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(memory_box), memory_label, FALSE, FALSE, 0);

    const char* memory_path = FSTP_GetMemoryLocationsCachePath();
    int memory_count = FSTP_GetMemoryLocationsFilesCount();

    char memory_info[512];
    snprintf(memory_info, sizeof(memory_info), "Location: %s/memory_locations\nSaved Data: %d video files",
             memory_path ? memory_path : "Unknown", memory_count);
    GtkWidget* memory_info_label = gtk_label_new(memory_info);
    gtk_widget_set_halign(memory_info_label, GTK_ALIGN_START);
    gtk_label_set_line_wrap(GTK_LABEL(memory_info_label), TRUE);
    attrs = pango_attr_list_new();
    pango_attr_list_insert(attrs, pango_attr_scale_new(PANGO_SCALE_SMALL));
    pango_attr_list_insert(attrs, pango_attr_family_new("monospace"));
    gtk_label_set_attributes(GTK_LABEL(memory_info_label), attrs);
    pango_attr_list_unref(attrs);
    gtk_box_pack_start(GTK_BOX(memory_box), memory_info_label, FALSE, FALSE, 0);

    // Show list of files with Memory Locations if any exist
    if (memory_count > 0) {
        GtkWidget* separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
        gtk_box_pack_start(GTK_BOX(memory_box), separator, FALSE, FALSE, 4);

        GtkWidget* files_label = gtk_label_new("Files with Memory Locations:");
        gtk_widget_set_halign(files_label, GTK_ALIGN_START);
        attrs = pango_attr_list_new();
        pango_attr_list_insert(attrs, pango_attr_scale_new(PANGO_SCALE_SMALL));
        gtk_label_set_attributes(GTK_LABEL(files_label), attrs);
        pango_attr_list_unref(attrs);
        gtk_box_pack_start(GTK_BOX(memory_box), files_label, FALSE, FALSE, 0);

        // Scrolled window for file list
        GtkWidget* scrolled = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                       GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
        gtk_widget_set_size_request(scrolled, -1, 120);

        GtkWidget* files_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        for (int i = 0; i < memory_count; i++) {
            const char* filename = FSTP_GetMemoryLocationsFileName(i);
            if (filename) {
                char bullet_text[512];
                snprintf(bullet_text, sizeof(bullet_text), "• %s", filename);
                GtkWidget* file_label = gtk_label_new(bullet_text);
                gtk_widget_set_halign(file_label, GTK_ALIGN_START);
                attrs = pango_attr_list_new();
                pango_attr_list_insert(attrs, pango_attr_scale_new(PANGO_SCALE_SMALL));
                pango_attr_list_insert(attrs, pango_attr_family_new("monospace"));
                gtk_label_set_attributes(GTK_LABEL(file_label), attrs);
                pango_attr_list_unref(attrs);
                gtk_box_pack_start(GTK_BOX(files_vbox), file_label, FALSE, FALSE, 0);
            }
        }

        gtk_container_add(GTK_CONTAINER(scrolled), files_vbox);
        gtk_box_pack_start(GTK_BOX(memory_box), scrolled, FALSE, FALSE, 0);
    }

    GtkWidget* memory_note = gtk_label_new("Memory Locations are saved per-video like browser cookies");
    gtk_widget_set_halign(memory_note, GTK_ALIGN_START);
    attrs = pango_attr_list_new();
    pango_attr_list_insert(attrs, pango_attr_scale_new(PANGO_SCALE_SMALL));
    gtk_label_set_attributes(GTK_LABEL(memory_note), attrs);
    pango_attr_list_unref(attrs);
    gtk_box_pack_start(GTK_BOX(memory_box), memory_note, FALSE, FALSE, 0);

    GtkWidget* memory_buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget* clear_memory_btn = gtk_button_new_with_label("Clear All");
    gtk_widget_set_sensitive(clear_memory_btn, memory_count > 0);
    g_signal_connect(clear_memory_btn, "clicked", G_CALLBACK(+[](GtkButton* button, gpointer user_data) {
        GtkWidget* dialog_widget = GTK_WIDGET(user_data);

        int memory_count = FSTP_GetMemoryLocationsFilesCount();

        GtkWidget* confirm_dialog = gtk_message_dialog_new(
            GTK_WINDOW(gtk_widget_get_toplevel(dialog_widget)),
            GTK_DIALOG_MODAL,
            GTK_MESSAGE_WARNING,
            GTK_BUTTONS_YES_NO,
            "Clear All Memory Locations?"
        );

        char message[256];
        snprintf(message, sizeof(message),
                 "This will delete all saved Memory Locations for %d video files. This cannot be undone.",
                 memory_count);
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(confirm_dialog), "%s", message);

        gint response = gtk_dialog_run(GTK_DIALOG(confirm_dialog));
        gtk_widget_destroy(confirm_dialog);

        if (response == GTK_RESPONSE_YES) {
            FSTP_ClearAllMemoryLocations();
            std::cout << "✅ All Memory Locations cleared" << std::endl;
        }
    }), dialog);
    gtk_box_pack_end(GTK_BOX(memory_buttons), clear_memory_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(memory_box), memory_buttons, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(cache_page), memory_box, FALSE, FALSE, 0);

    gtk_stack_add_named(GTK_STACK(stack), cache_page, "cache");

    // ===== EXTENSIONS PAGE =====
    GtkWidget* extensions_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(extensions_page), 20);

    GtkWidget* extensions_title = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(extensions_title), "<b>Extensions</b>");
    gtk_widget_set_halign(extensions_title, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(extensions_page), extensions_title, FALSE, FALSE, 0);

    std::string extension_language = GetExtensionLanguage();
    GtkWidget* yt_dlp_check = nullptr;
    bool yt_dlp_available = FSTP_YTDLP_IsAvailable();

    if (yt_dlp_available) {
        yt_dlp_check = gtk_check_button_new_with_label("Enable yt-dlp network downloader");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(yt_dlp_check), settings->yt_dlp_extension_enabled);
        gtk_box_pack_start(GTK_BOX(extensions_page), yt_dlp_check, FALSE, FALSE, 0);
    } else {
        GtkWidget* yt_dlp_info = gtk_label_new("yt-dlp binary not found. Install it (e.g. via package manager) to enable network downloads.");
        gtk_widget_set_halign(yt_dlp_info, GTK_ALIGN_START);
        GtkWidget* info_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        attrs = pango_attr_list_new();
        pango_attr_list_insert(attrs, pango_attr_scale_new(PANGO_SCALE_SMALL));
        gtk_label_set_attributes(GTK_LABEL(yt_dlp_info), attrs);
        pango_attr_list_unref(attrs);
        gtk_box_pack_start(GTK_BOX(info_box), yt_dlp_info, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(extensions_page), info_box, FALSE, FALSE, 0);
    }

    std::string extensions_info_text =
        "TapeXPlayer extensions are scripted using " + extension_language +
        " (.lua) files. Drop .lua bundles into your extensions directory to augment playback workflows.\n\n"
        "Extension loading is being prepared; this section will expand with management tools as the system evolves.";

    GtkWidget* extensions_info = gtk_label_new(extensions_info_text.c_str());
    gtk_widget_set_halign(extensions_info, GTK_ALIGN_START);
    gtk_label_set_line_wrap(GTK_LABEL(extensions_info), TRUE);
    gtk_label_set_xalign(GTK_LABEL(extensions_info), 0.0f);
    attrs = pango_attr_list_new();
    pango_attr_list_insert(attrs, pango_attr_scale_new(PANGO_SCALE_SMALL));
    gtk_label_set_attributes(GTK_LABEL(extensions_info), attrs);
    pango_attr_list_unref(attrs);
    gtk_box_pack_start(GTK_BOX(extensions_page), extensions_info, FALSE, FALSE, 0);

    gtk_stack_add_named(GTK_STACK(stack), extensions_page, "extensions");

    // Set initial visible page
    gtk_stack_set_visible_child_name(GTK_STACK(stack), "audio");

    // Add padding to button area (action area)
    GtkWidget* action_area = gtk_dialog_get_action_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(action_area), 12);

    // Add "Reset to Defaults" button on the left side
    GtkWidget* reset_btn = gtk_button_new_with_label("Reset to Defaults");
    gtk_widget_set_halign(reset_btn, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(action_area), reset_btn, FALSE, FALSE, 0);
    gtk_box_reorder_child(GTK_BOX(action_area), reset_btn, 0);  // Move to first position

    // Connect reset button
    g_signal_connect(reset_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer) {
        ResetSettingsToDefault();
        // TODO: Reload dialog with new values
    }), nullptr);

    // Show all widgets
    gtk_widget_show_all(dialog);

    // Run dialog
    gint result = gtk_dialog_run(GTK_DIALOG(dialog));

    if (result == GTK_RESPONSE_ACCEPT) {
        // Get mutable settings
        FSTPSettings* writable_settings = const_cast<FSTPSettings*>(settings);

        // Audio device: save the NAME too so the choice survives PortAudio index shifts.
        int new_device_index = gtk_combo_box_get_active(GTK_COMBO_BOX(device_combo)) - 1;
        if (new_device_index < 0) {
            SetAudioDevice(-1, "");
        } else {
            gchar* dev_name = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(device_combo));
            SetAudioDevice(new_device_index, dev_name ? dev_name : "");
            if (dev_name) g_free(dev_name);
        }

        // Volume
        writable_settings->audio_master_volume = gtk_range_get_value(GTK_RANGE(volume_scale));

        // Volume ducking (ear protection)
        writable_settings->audio_volume_ducking_enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ducking_check));

        // Buffer size
        int buffer_sizes[] = {512, 1024, 2048, 4096};
        int new_buffer_index = gtk_combo_box_get_active(GTK_COMBO_BOX(buffer_combo));
        writable_settings->audio_buffer_size = buffer_sizes[new_buffer_index];

        // Frame offset
        writable_settings->frame_offset = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(offset_spin));

        // Auto-freeze
        writable_settings->auto_freeze_inactive = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(freeze_check));

        // Betacam effect
        writable_settings->betacam_effect_enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(betacam_check));
        SetBetacamEffectEnabled(writable_settings->betacam_effect_enabled);

        // Decoder status display
        writable_settings->show_decoder_status = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(decoder_status_check));

        // yt-dlp extension
        if (yt_dlp_available && yt_dlp_check) {
            writable_settings->yt_dlp_extension_enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(yt_dlp_check));
        } else {
            writable_settings->yt_dlp_extension_enabled = 0;
        }
        SetYTDLPExtensionEnabled(writable_settings->yt_dlp_extension_enabled);

        // MIDI
        writable_settings->midi_enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(midi_enable_check));
        writable_settings->midi_input_port = gtk_combo_box_get_active(GTK_COMBO_BOX(input_combo)) - 1;
        writable_settings->midi_output_port = gtk_combo_box_get_active(GTK_COMBO_BOX(output_combo)) - 1;

        // Save settings
        SaveSettings();
        ApplyAudioSettings();
        ApplyMIDISettings();

        std::cout << "✅ Settings saved" << std::endl;
    }

    // Cleanup
    delete midi_widgets;

    gtk_widget_destroy(dialog);

    // Process events
    while (gtk_events_pending()) {
        gtk_main_iteration();
    }

    std::cout << "🗑️  Settings dialog closed" << std::endl;
}

#endif // __linux__
