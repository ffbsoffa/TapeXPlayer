#ifdef __linux__

#include <gtk/gtk.h>
#include <iostream>
#include <string>
#include <cmath>
#include "../FSTPMemoryLocations.h"
#include "FSTPMemoryLocationsWindow.h"

// Forward declarations
extern "C" int GetActivePlayerID();
extern "C" double GetInstancePosition(int player_id);

// Global window state
static GtkWidget* g_memory_locations_window = nullptr;
static GtkWidget* g_treeview = nullptr;
static GtkListStore* g_liststore = nullptr;
static guint g_refresh_timer = 0;

// GTK TreeView columns
enum {
    COL_ID = 0,
    COL_TIMECODE,
    COL_NAME,
    NUM_COLS
};

// Refresh table data from backend
static void RefreshMemoryLocationsTable() {
    if (!g_liststore) return;

    // Clear existing rows
    gtk_list_store_clear(g_liststore);

    int count = FSTP_GetMemoryLocationsCount();

    for (int i = 0; i < count; i++) {
        FSTP_MemoryLocationData data;
        if (FSTP_GetMemoryLocationData(i, &data)) {
            GtkTreeIter iter;
            gtk_list_store_append(g_liststore, &iter);
            gtk_list_store_set(g_liststore, &iter,
                             COL_ID, data.id,
                             COL_TIMECODE, data.timecode_display,
                             COL_NAME, data.name,
                             -1);
        }
    }
}

// Timer callback for auto-refresh
static gboolean OnRefreshTimer(gpointer user_data) {
    RefreshMemoryLocationsTable();
    return G_SOURCE_CONTINUE;  // Continue timer
}

// Forward declarations for external functions
extern "C" {
    double GetInstanceVideoFPS(int player_id);
}

// Unified Add/Edit Memory Location Dialog
// location_id == -1 means "Add mode", otherwise "Edit mode"
static void ShowAddEditMemoryLocationDialog(int player_id, int location_id, double current_time) {
    bool is_add_mode = (location_id == -1);

    std::cout << (is_add_mode ? "➕ Opening add dialog" : "✏️ Opening edit dialog")
              << " for Memory Location"
              << (is_add_mode ? "" : " #" + std::to_string(location_id)) << std::endl;

    // In edit mode, fetch existing location data
    FSTP_MemoryLocationData data;
    if (!is_add_mode) {
        bool found = false;
        int count = FSTP_GetMemoryLocationsCount();
        for (int i = 0; i < count; i++) {
            FSTP_MemoryLocationData temp;
            if (FSTP_GetMemoryLocationData(i, &temp) && temp.id == location_id) {
                data = temp;
                found = true;
                break;
            }
        }

        if (!found) {
            std::cerr << "❌ Memory Location #" << location_id << " not found" << std::endl;
            return;
        }
    } else {
        // Add mode: initialize with defaults
        data.id = FSTP_GetMemoryLocationsCount() + 1;
        data.timecode_seconds = current_time;
        data.recall_zoom = false;
        snprintf(data.name, sizeof(data.name), "%s", "");
        snprintf(data.comments, sizeof(data.comments), "%s", "");

        // Format timecode
        double fps = GetInstanceVideoFPS(player_id);
        if (fps <= 0) fps = 25.0;

        int hours = static_cast<int>(current_time) / 3600;
        int minutes = (static_cast<int>(current_time) % 3600) / 60;
        int seconds = static_cast<int>(current_time) % 60;
        int frames = static_cast<int>((current_time - floor(current_time)) * fps);

        snprintf(data.timecode_display, sizeof(data.timecode_display),
                 "%02d:%02d:%02d:%02d", hours, minutes, seconds, frames);
    }

    // Create dialog
    GtkWidget* dialog = gtk_dialog_new_with_buttons(
        is_add_mode ? "New Memory Location" : "Edit Memory Location",
        g_memory_locations_window ? GTK_WINDOW(g_memory_locations_window) : nullptr,
        GTK_DIALOG_MODAL,
        "Cancel", GTK_RESPONSE_CANCEL,
        is_add_mode ? "Add" : "Save", GTK_RESPONSE_OK,
        nullptr
    );

    gtk_window_set_default_size(GTK_WINDOW(dialog), 500, 400);

    GtkWidget* content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(content_area), 10);

    // Create grid for form
    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_container_add(GTK_CONTAINER(content_area), grid);

    int row = 0;

    // Number/ID field
    GtkWidget* id_label = gtk_label_new(is_add_mode ? "Number:" : "ID:");
    gtk_widget_set_halign(id_label, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), id_label, 0, row, 1, 1);

    GtkWidget* id_widget;
    if (is_add_mode) {
        // Editable number entry
        id_widget = gtk_entry_new();
        gchar* id_text = g_strdup_printf("%d", data.id);
        gtk_entry_set_text(GTK_ENTRY(id_widget), id_text);
        g_free(id_text);
        gtk_entry_set_width_chars(GTK_ENTRY(id_widget), 10);
    } else {
        // Read-only label
        gchar* id_text = g_strdup_printf("%d", data.id);
        id_widget = gtk_label_new(id_text);
        g_free(id_text);
        gtk_widget_set_halign(id_widget, GTK_ALIGN_START);
    }
    gtk_grid_attach(GTK_GRID(grid), id_widget, 1, row, 1, 1);
    row++;

    // Timecode field
    GtkWidget* timecode_label = gtk_label_new("Timecode:");
    gtk_widget_set_halign(timecode_label, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), timecode_label, 0, row, 1, 1);

    GtkWidget* timecode_widget;
    if (is_add_mode) {
        // Editable timecode entry
        timecode_widget = gtk_entry_new();
        gtk_entry_set_text(GTK_ENTRY(timecode_widget), data.timecode_display);
        gtk_entry_set_width_chars(GTK_ENTRY(timecode_widget), 15);
    } else {
        // Read-only label
        timecode_widget = gtk_label_new(data.timecode_display);
        gtk_widget_set_halign(timecode_widget, GTK_ALIGN_START);
    }
    gtk_grid_attach(GTK_GRID(grid), timecode_widget, 1, row, 1, 1);
    row++;

    // Name (editable)
    GtkWidget* name_label = gtk_label_new("Name:");
    gtk_widget_set_halign(name_label, GTK_ALIGN_END);
    gtk_widget_set_valign(name_label, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), name_label, 0, row, 1, 1);

    GtkWidget* name_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(name_entry), data.name);
    gtk_widget_set_hexpand(name_entry, TRUE);
    gtk_grid_attach(GTK_GRID(grid), name_entry, 1, row, 1, 1);
    row++;

    // Comments (editable, multiline)
    GtkWidget* comments_label = gtk_label_new("Comments:");
    gtk_widget_set_halign(comments_label, GTK_ALIGN_END);
    gtk_widget_set_valign(comments_label, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), comments_label, 0, row, 1, 1);

    // Create scrolled window for text view
    GtkWidget* scrolled = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_hexpand(scrolled, TRUE);
    gtk_widget_set_vexpand(scrolled, TRUE);

    GtkWidget* text_view = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text_view), GTK_WRAP_WORD);
    gtk_container_add(GTK_CONTAINER(scrolled), text_view);

    GtkTextBuffer* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_view));
    gtk_text_buffer_set_text(buffer, data.comments, -1);

    gtk_grid_attach(GTK_GRID(grid), scrolled, 1, row, 1, 1);
    row++;

    // Recall zoom checkbox (only in add mode)
    GtkWidget* recall_zoom_check = nullptr;
    if (is_add_mode) {
        recall_zoom_check = gtk_check_button_new_with_label("Recall zoom settings");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(recall_zoom_check), data.recall_zoom);
        gtk_grid_attach(GTK_GRID(grid), recall_zoom_check, 0, row, 2, 1);
    }

    gtk_widget_show_all(dialog);

    // Focus name field
    gtk_widget_grab_focus(name_entry);

    // Run dialog
    gint response = gtk_dialog_run(GTK_DIALOG(dialog));

    if (response == GTK_RESPONSE_OK) {
        // Get updated values
        const gchar* new_name = gtk_entry_get_text(GTK_ENTRY(name_entry));

        GtkTextIter start, end;
        gtk_text_buffer_get_bounds(buffer, &start, &end);
        gchar* new_comments = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);

        if (is_add_mode) {
            // Add mode: create new location
            const gchar* number_text = gtk_entry_get_text(GTK_ENTRY(id_widget));
            const gchar* timecode_text = gtk_entry_get_text(GTK_ENTRY(timecode_widget));
            gboolean recall_zoom = recall_zoom_check ?
                gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(recall_zoom_check)) : FALSE;

            // Parse location number
            int new_id = atoi(number_text);
            if (new_id <= 0) new_id = data.id;

            // Parse timecode back to seconds
            double fps = GetInstanceVideoFPS(player_id);
            if (fps <= 0) fps = 25.0;

            int tc_hours = 0, tc_minutes = 0, tc_seconds = 0, tc_frames = 0;
            if (sscanf(timecode_text, "%d:%d:%d:%d", &tc_hours, &tc_minutes, &tc_seconds, &tc_frames) == 4) {
                double timecode_seconds = tc_hours * 3600.0 + tc_minutes * 60.0 + tc_seconds;
                timecode_seconds += tc_frames / fps;

                // Get zoom state if requested
                float zoom_factor = 1.0f;
                float zoom_center_x = 0.5f;
                float zoom_center_y = 0.5f;

                if (recall_zoom) {
                    // TODO: Get actual zoom state from player
                }

                // Add location
                if (FSTP_AddMemoryLocationWithZoom(player_id, new_id, new_name, new_comments,
                                                    timecode_seconds, recall_zoom,
                                                    zoom_factor, zoom_center_x, zoom_center_y)) {
                    std::cout << "✅ Memory Location added: " << new_name << " at " << timecode_text << std::endl;
                    RefreshMemoryLocationsTable();
                } else {
                    std::cerr << "❌ Failed to add Memory Location" << std::endl;
                }
            }
        } else {
            // Edit mode: update existing location
            if (FSTP_UpdateMemoryLocation(location_id, new_name, new_comments)) {
                std::cout << "✅ Memory Location #" << location_id << " updated successfully" << std::endl;
                RefreshMemoryLocationsTable();
            } else {
                std::cerr << "❌ Failed to update Memory Location #" << location_id << std::endl;
            }
        }

        g_free(new_comments);
    }

    gtk_widget_destroy(dialog);
}

// Export to CSV callback
static void OnExportToCSV(GtkWidget* button, gpointer user_data) {
    std::cout << "📊 Exporting Memory Locations to CSV..." << std::endl;

    GtkWidget* dialog = gtk_file_chooser_dialog_new(
        "Export Memory Locations to CSV",
        GTK_WINDOW(g_memory_locations_window),
        GTK_FILE_CHOOSER_ACTION_SAVE,
        "Cancel", GTK_RESPONSE_CANCEL,
        "Export", GTK_RESPONSE_ACCEPT,
        nullptr
    );

    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), "memory_locations.csv");
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);

    // Add CSV filter
    GtkFileFilter* filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "CSV files (*.csv)");
    gtk_file_filter_add_pattern(filter, "*.csv");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        gchar* filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));

        if (FSTP_ExportMemoryLocationsToCSV(filename)) {
            std::cout << "✅ Memory Locations exported to: " << filename << std::endl;

            // Show success message
            GtkWidget* success_dialog = gtk_message_dialog_new(
                GTK_WINDOW(g_memory_locations_window),
                GTK_DIALOG_MODAL,
                GTK_MESSAGE_INFO,
                GTK_BUTTONS_OK,
                "Export Successful"
            );
            gtk_message_dialog_format_secondary_text(
                GTK_MESSAGE_DIALOG(success_dialog),
                "Memory Locations exported to:\n%s", filename
            );
            gtk_dialog_run(GTK_DIALOG(success_dialog));
            gtk_widget_destroy(success_dialog);
        } else {
            std::cerr << "❌ Failed to export Memory Locations to CSV" << std::endl;

            // Show error message
            GtkWidget* error_dialog = gtk_message_dialog_new(
                GTK_WINDOW(g_memory_locations_window),
                GTK_DIALOG_MODAL,
                GTK_MESSAGE_ERROR,
                GTK_BUTTONS_OK,
                "Export Failed"
            );
            gtk_message_dialog_format_secondary_text(
                GTK_MESSAGE_DIALOG(error_dialog),
                "Failed to export Memory Locations to CSV"
            );
            gtk_dialog_run(GTK_DIALOG(error_dialog));
            gtk_widget_destroy(error_dialog);
        }

        g_free(filename);
    }

    gtk_widget_destroy(dialog);
}

// Row activated callback (double-click)
static void OnRowActivated(GtkTreeView* tree_view, GtkTreePath* path,
                          GtkTreeViewColumn* column, gpointer user_data) {
    GtkTreeModel* model = gtk_tree_view_get_model(tree_view);
    GtkTreeIter iter;

    if (gtk_tree_model_get_iter(model, &iter, path)) {
        gint id;
        gtk_tree_model_get(model, &iter, COL_ID, &id, -1);

        // Get active player
        int active_player = GetActivePlayerID();

        if (active_player >= 0) {
            FSTP_RecallMemoryLocation(id, active_player);
            std::cout << "✅ Recalled Memory Location #" << id << std::endl;
        } else {
            std::cout << "⚠️ No active player for recalling memory location" << std::endl;
        }
    }
}

// Window close callback
static gboolean OnWindowClose(GtkWidget* widget, GdkEvent* event, gpointer data) {
    HideGTKMemoryLocationsWindow();
    return TRUE;  // Prevent actual destruction
}

// Create the Memory Locations window
static void CreateMemoryLocationsWindow() {
    if (g_memory_locations_window) return;

    std::cout << "📍 Creating Memory Locations window..." << std::endl;

    // Create window
    g_memory_locations_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(g_memory_locations_window), "📍 Memory Locations");
    gtk_window_set_default_size(GTK_WINDOW(g_memory_locations_window), 700, 400);
    gtk_window_set_type_hint(GTK_WINDOW(g_memory_locations_window), GDK_WINDOW_TYPE_HINT_DIALOG);

    // Connect close signal
    g_signal_connect(g_memory_locations_window, "delete-event", G_CALLBACK(OnWindowClose), nullptr);

    // Create main container
    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(g_memory_locations_window), vbox);

    // Create scrolled window for table
    GtkWidget* scrolled = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(vbox), scrolled, TRUE, TRUE, 0);

    // Create list store (data model)
    g_liststore = gtk_list_store_new(NUM_COLS,
                                     G_TYPE_INT,      // ID
                                     G_TYPE_STRING,   // Timecode
                                     G_TYPE_STRING);  // Name

    // Create tree view (table widget)
    g_treeview = gtk_tree_view_new_with_model(GTK_TREE_MODEL(g_liststore));
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(g_treeview), FALSE);
    gtk_container_add(GTK_CONTAINER(scrolled), g_treeview);

    // Connect row activated signal (double-click)
    g_signal_connect(g_treeview, "row-activated", G_CALLBACK(OnRowActivated), nullptr);

    // Connect button-press-event for modifier key handling
    g_signal_connect(g_treeview, "button-press-event", G_CALLBACK(+[](GtkWidget* widget, GdkEventButton* event, gpointer user_data) -> gboolean {
        GtkTreeView* tree_view = GTK_TREE_VIEW(widget);
        GtkTreePath* path = nullptr;

        // Get clicked row
        if (gtk_tree_view_get_path_at_pos(tree_view, (gint)event->x, (gint)event->y,
                                          &path, nullptr, nullptr, nullptr)) {
            GtkTreeModel* model = gtk_tree_view_get_model(tree_view);
            GtkTreeIter iter;

            if (gtk_tree_model_get_iter(model, &iter, path)) {
                gint id;
                gtk_tree_model_get(model, &iter, COL_ID, &id, -1);

                // Ctrl+Click - Edit (like Command+Click on macOS)
                if (event->state & GDK_CONTROL_MASK) {
                    std::cout << "✏️ Ctrl+Click - Editing Memory Location #" << id << std::endl;
                    ShowAddEditMemoryLocationDialog(0, id, 0.0);  // player_id and current_time unused in edit mode
                    gtk_tree_path_free(path);
                    return TRUE;  // Event handled
                }

                // Alt+Click - Delete
                if (event->state & GDK_MOD1_MASK) {
                    std::cout << "🗑️ Alt+Click - Deleting Memory Location #" << id << std::endl;

                    if (FSTP_DeleteMemoryLocation(id)) {
                        RefreshMemoryLocationsTable();
                        std::cout << "✅ Memory Location #" << id << " deleted" << std::endl;
                    } else {
                        std::cout << "❌ Failed to delete Memory Location #" << id << std::endl;
                    }
                    gtk_tree_path_free(path);
                    return TRUE;  // Event handled
                }
            }

            gtk_tree_path_free(path);
        }

        return FALSE;  // Event not handled
    }), nullptr);

    // Create columns
    GtkCellRenderer* renderer;
    GtkTreeViewColumn* column;

    // ID column
    renderer = gtk_cell_renderer_text_new();
    column = gtk_tree_view_column_new_with_attributes("#", renderer,
                                                      "text", COL_ID,
                                                      nullptr);
    gtk_tree_view_column_set_sizing(column, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width(column, 50);
    gtk_tree_view_append_column(GTK_TREE_VIEW(g_treeview), column);

    // Timecode column
    renderer = gtk_cell_renderer_text_new();
    g_object_set(renderer, "family", "monospace", nullptr);
    column = gtk_tree_view_column_new_with_attributes("Timecode", renderer,
                                                      "text", COL_TIMECODE,
                                                      nullptr);
    gtk_tree_view_column_set_sizing(column, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width(column, 120);
    gtk_tree_view_append_column(GTK_TREE_VIEW(g_treeview), column);

    // Name column (expands to fill remaining space)
    renderer = gtk_cell_renderer_text_new();
    column = gtk_tree_view_column_new_with_attributes("Name", renderer,
                                                      "text", COL_NAME,
                                                      nullptr);
    gtk_tree_view_column_set_resizable(column, TRUE);
    gtk_tree_view_column_set_expand(column, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(g_treeview), column);

    // Bottom button bar
    GtkWidget* button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(button_box), 5);
    gtk_box_pack_start(GTK_BOX(vbox), button_box, FALSE, FALSE, 0);

    // Export to CSV button
    GtkWidget* export_button = gtk_button_new_with_label("📊 Export to CSV");
    g_signal_connect(export_button, "clicked", G_CALLBACK(OnExportToCSV), nullptr);
    gtk_box_pack_end(GTK_BOX(button_box), export_button, FALSE, FALSE, 0);

    // Info label
    GtkWidget* info_label = gtk_label_new("Double-click: Recall | Ctrl+Click: Edit | Alt+Click: Delete");
    gtk_widget_set_halign(info_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(button_box), info_label, FALSE, FALSE, 0);

    // Show all widgets
    gtk_widget_show_all(g_memory_locations_window);

    // Start auto-refresh timer (100ms interval, like macOS)
    g_refresh_timer = g_timeout_add(100, OnRefreshTimer, nullptr);

    std::cout << "✅ Memory Locations window created" << std::endl;
}

// Show the Memory Locations window
void ShowGTKMemoryLocationsWindow() {
    std::cout << "🔵 ShowGTKMemoryLocationsWindow called" << std::endl;

    // Initialize Memory Locations system
    FSTP_InitMemoryLocations();

    // Create window if it doesn't exist
    if (!g_memory_locations_window) {
        CreateMemoryLocationsWindow();
    }

    // Show window
    gtk_widget_show_all(g_memory_locations_window);
    gtk_window_present(GTK_WINDOW(g_memory_locations_window));

    // Refresh data
    RefreshMemoryLocationsTable();
}

// Hide the Memory Locations window
void HideGTKMemoryLocationsWindow() {
    if (g_memory_locations_window) {
        gtk_widget_hide(g_memory_locations_window);
    }
}

// Toggle the Memory Locations window
void ToggleGTKMemoryLocationsWindow() {
    if (g_memory_locations_window && gtk_widget_get_visible(g_memory_locations_window)) {
        HideGTKMemoryLocationsWindow();
    } else {
        ShowGTKMemoryLocationsWindow();
    }
}

// C API: Show unified Add/Edit dialog for Memory Locations
// Use location_id = -1 for "Add mode", otherwise "Edit mode"
extern "C" void ShowGTKMemoryLocationDialog(int player_id, int location_id, double current_time) {
    ShowAddEditMemoryLocationDialog(player_id, location_id, current_time);
}

#endif // __linux__
