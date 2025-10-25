#ifdef __linux__

#include <gtk/gtk.h>
#include <iostream>
#include "../FSTPMemoryLocations.h"
#include "FSTPMemoryLocationsWindow.h"

// Forward declarations
extern "C" int GetActivePlayerID();

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

// Row activated callback (double-click)
static void OnRowActivated(GtkTreeView* tree_view, GtkTreePath* path,
                          GtkTreeViewColumn* column, gpointer user_data) {
    GtkTreeModel* model = gtk_tree_view_get_model(tree_view);
    GtkTreeIter iter;

    if (gtk_tree_model_get_iter(model, &iter, path)) {
        gint id;
        gtk_tree_model_get(model, &iter, COL_ID, &id, -1);

        // Get active player
        extern int GetActivePlayerID();
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
    gtk_window_set_default_size(GTK_WINDOW(g_memory_locations_window), 500, 400);
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

    // Connect button-press-event for Alt+Click handling (deletion)
    g_signal_connect(g_treeview, "button-press-event", G_CALLBACK(+[](GtkWidget* widget, GdkEventButton* event, gpointer user_data) -> gboolean {
        // Check Alt/Option key
        if (event->state & GDK_MOD1_MASK) {  // MOD1 = Alt key
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

                    std::cout << "🗑️ Alt+Click - Deleting Memory Location #" << id << std::endl;

                    if (FSTP_DeleteMemoryLocation(id)) {
                        // Reload table
                        RefreshMemoryLocationsTable();
                        std::cout << "✅ Memory Location #" << id << " deleted" << std::endl;
                    } else {
                        std::cout << "❌ Failed to delete Memory Location #" << id << std::endl;
                    }
                }

                gtk_tree_path_free(path);
                return TRUE;  // Event handled, stop propagation
            }
        }

        return FALSE;  // Event not handled, continue propagation
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

#endif // __linux__
