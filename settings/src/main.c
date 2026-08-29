// Nekoland Settings — macOS System Settings–style shell, GTK4.
// Skeleton only for now: sidebar (search + category list) and a content stack.

#include <adwaita.h>
#include <gtk/gtk.h>

static GtkWidget *sidebar_list;
static GtkWidget *content_stack;

// Add a category: colored rounded-square icon + label in the sidebar, and a
// (currently empty) page in the content stack. Unused until settings exist.
G_GNUC_UNUSED
static void add_category(const char *id, const char *title,
                         const char *icon_name, const char *color_class,
                         GtkWidget *page) {
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(box, "category-row");

    GtkWidget *icon_bg = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(icon_bg, "category-icon");
    if (color_class)
        gtk_widget_add_css_class(icon_bg, color_class);
    GtkWidget *icon = gtk_image_new_from_icon_name(icon_name);
    gtk_widget_set_halign(icon, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(icon, TRUE);
    gtk_box_append(GTK_BOX(icon_bg), icon);

    gtk_box_append(GTK_BOX(box), icon_bg);
    gtk_box_append(GTK_BOX(box), gtk_label_new(title));
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
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
    gtk_window_present(GTK_WINDOW(win));
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
