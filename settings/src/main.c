// Nekoland Settings — macOS System Settings–style shell, GTK4.
// Skeleton only for now: sidebar (search + category list) and a content stack.

#include <adwaita.h>
#include <gtk/gtk.h>

#include "app.h"

static GtkWidget *sidebar_list;
static GtkWidget *content_stack;

static gboolean debug_dump_idle(gpointer data) {
    GtkWidget *lbl = data;
    GtkWidget *box = gtk_widget_get_parent(lbl);
    graphene_rect_t b;
    if (gtk_widget_compute_bounds(lbl, box, &b))
        g_printerr("DBG label in box: x=%.0f w=%.0f (box w=%d) halign=%d "
                   "hexpand=%d xalign=%.1f\n",
                   b.origin.x, b.size.width, gtk_widget_get_width(box),
                   gtk_widget_get_halign(lbl), gtk_widget_get_hexpand(lbl),
                   gtk_label_get_xalign(GTK_LABEL(lbl)));
    return FALSE;
}

static void debug_dump_geometry(GtkWidget *lbl, gpointer data) {
    (void)data;
    g_idle_add(debug_dump_idle, lbl);
}

// Add a category: colored rounded-square icon tile + label in the sidebar,
// and its page in the content stack. Glyphs are nerd-font characters (the
// installed icon theme lacks many symbolic icons).
static void add_category(const char *id, const char *title, const char *glyph,
                         const char *color_class, GtkWidget *page) {
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(box, "category-row");

    // the label IS the tile: CSS min-width/min-height form the colored
    // square and the label centers its glyph in it (no wrapper box, no
    // expand flags to leak into the row's space distribution)
    GtkWidget *icon = gtk_label_new(glyph);
    gtk_widget_add_css_class(icon, "category-icon");
    if (color_class)
        gtk_widget_add_css_class(icon, color_class);
    gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), icon);
    GtkWidget *title_lbl = gtk_label_new(title);
    // halign FILL + xalign 0: the label owns its whole slot and pins the
    // text left, immune to whatever repositions START-aligned children here
    gtk_label_set_xalign(GTK_LABEL(title_lbl), 0.0);
    gtk_widget_set_halign(title_lbl, GTK_ALIGN_FILL);
    gtk_widget_set_hexpand(title_lbl, TRUE);
    gtk_box_append(GTK_BOX(box), title_lbl);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    if (g_getenv("NEKOLAND_DEBUG_LAYOUT")) {
        g_signal_connect(title_lbl, "map", G_CALLBACK(debug_dump_geometry),
                         box);
    }
    g_object_set_data_full(G_OBJECT(row), "page-id", g_strdup(id), g_free);
    gtk_list_box_append(GTK_LIST_BOX(sidebar_list), row);

    gtk_stack_add_named(GTK_STACK(content_stack), page, id);
}

static void on_row_selected(GtkListBox *list, GtkListBoxRow *row,
                            gpointer data) {
    (void)list;
    (void)data;
    if (!row)
        return;
    const char *id = g_object_get_data(G_OBJECT(row), "page-id");
    if (id)
        gtk_stack_set_visible_child_name(GTK_STACK(content_stack), id);
}

static void load_css(void) {
    char *exe = g_file_read_link("/proc/self/exe", NULL);
    char *dir = g_path_get_dirname(exe ? exe : ".");
    char *css = g_build_filename(dir, "style.css", NULL);
    GtkCssProvider *prov = gtk_css_provider_new();
    gtk_css_provider_load_from_path(prov, css);
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(prov),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_free(css);
    g_free(dir);
    g_free(exe);
}

static void on_window_destroy(GtkWidget *w, gpointer data) {
    (void)w;
    (void)data;
    audio_shutdown();
}

static void activate(AdwApplication *app, gpointer data) {
    (void)data;
    load_css();

    GtkWidget *win = gtk_application_window_new(GTK_APPLICATION(app));
    gtk_window_set_title(GTK_WINDOW(win), "Settings");
    gtk_window_set_default_size(GTK_WINDOW(win), 780, 580);
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE); // no titlebar

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

    // ---- sidebar ----
    GtkWidget *sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class(sidebar, "sidebar");
    gtk_widget_set_size_request(sidebar, 215, -1);

    GtkWidget *search = gtk_search_entry_new();
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(search), "Search");
    gtk_widget_add_css_class(search, "sidebar-search");
    gtk_box_append(GTK_BOX(sidebar), search);

    sidebar_list = gtk_list_box_new();
    gtk_widget_add_css_class(sidebar_list, "sidebar-list");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(sidebar_list),
                                    GTK_SELECTION_SINGLE);
    g_signal_connect(sidebar_list, "row-selected",
                     G_CALLBACK(on_row_selected), NULL);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), sidebar_list);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(sidebar), scroll);

    gtk_box_append(GTK_BOX(root), sidebar);
    gtk_box_append(GTK_BOX(root),
                   gtk_separator_new(GTK_ORIENTATION_VERTICAL));

    // ---- content ----
    content_stack = gtk_stack_new();
    gtk_widget_add_css_class(content_stack, "content");
    gtk_widget_set_hexpand(content_stack, TRUE);
    gtk_stack_set_transition_type(GTK_STACK(content_stack),
                                  GTK_STACK_TRANSITION_TYPE_CROSSFADE);

    // empty state until real settings pages exist
    GtkWidget *empty = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_halign(empty, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(empty, GTK_ALIGN_CENTER);
    GtkWidget *empty_icon = gtk_label_new(""); // nerd-font gear
    gtk_widget_add_css_class(empty_icon, "empty-icon");
    GtkWidget *empty_label = gtk_label_new("Nothing here yet");
    gtk_widget_add_css_class(empty_label, "empty-label");
    gtk_box_append(GTK_BOX(empty), empty_icon);
    gtk_box_append(GTK_BOX(empty), empty_label);
    gtk_stack_add_named(GTK_STACK(content_stack), empty, "empty");

    // categories
    add_category("sound", "Sound", "", "icon-red", audio_page_new());
    gtk_list_box_select_row(
        GTK_LIST_BOX(sidebar_list),
        gtk_list_box_get_row_at_index(GTK_LIST_BOX(sidebar_list), 0));

    gtk_box_append(GTK_BOX(root), content_stack);

    // lone macOS-style close dot floating over the sidebar's top-left
    GtkWidget *overlay = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(overlay), root);
    GtkWidget *close_btn = gtk_button_new();
    gtk_widget_add_css_class(close_btn, "close-dot");
    gtk_widget_set_halign(close_btn, GTK_ALIGN_START);
    gtk_widget_set_valign(close_btn, GTK_ALIGN_START);
    gtk_widget_set_margin_start(close_btn, 12);
    gtk_widget_set_margin_top(close_btn, 12);
    g_signal_connect_swapped(close_btn, "clicked",
                             G_CALLBACK(gtk_window_close), win);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), close_btn);

    gtk_window_set_child(GTK_WINDOW(win), overlay);
    g_signal_connect(win, "destroy", G_CALLBACK(on_window_destroy), NULL);
    gtk_window_present(GTK_WINDOW(win));

    // dev hook: NEKOLAND_ADVANCED=1 opens the advanced modal on startup
    if (g_getenv("NEKOLAND_ADVANCED"))
        audio_open_advanced(GTK_WINDOW(win));
}

int main(int argc, char **argv) {
    // AdwApplication: with libadwaita-without-adwaita installed, this makes
    // the app follow the system GTK theme (catppuccin) instead of Adwaita
    AdwApplication *app = adw_application_new("org.nekoland.Settings",
                                              G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
