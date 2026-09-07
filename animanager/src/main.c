// Nekoland Animanager — manage anime stored across configured folders.
// GTK4 + libadwaita, same conventions as settings/: GKeyFile config under
// ~/.config/nekoland/, style.css loaded from beside the binary.

#include <adwaita.h>
#include <gtk/gtk.h>

static GtkWindow *main_window;
static GtkWidget *library_stack;   // "empty" page / "library" page
static GtkWidget *library_box;     // vertical box holding per-folder sections

// settings dialog state (NULL while the dialog is closed)
static AdwPreferencesGroup *folders_group;
static GPtrArray *folder_rows; // AdwActionRow* currently in folders_group

static GPtrArray *folders; // char* — configured anime directories

// ---------------------------------------------------------------- config --

static char *config_path(void) {
    return g_build_filename(g_get_user_config_dir(), "nekoland",
                            "animanager.ini", NULL);
}

static void config_load(void) {
    folders = g_ptr_array_new_with_free_func(g_free);
    char *path = config_path();
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        gsize n = 0;
        char **list =
            g_key_file_get_string_list(kf, "library", "folders", &n, NULL);
        for (gsize i = 0; list && i < n; i++)
            g_ptr_array_add(folders, g_strdup(list[i]));
        g_strfreev(list);
    }
    g_key_file_free(kf);
    g_free(path);
}

static void config_save(void) {
    char *path = config_path();
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);

    GKeyFile *kf = g_key_file_new();
    g_key_file_set_string_list(kf, "library", "folders",
                               (const char *const *)folders->pdata,
                               folders->len);
    g_key_file_save_to_file(kf, path, NULL);
    g_key_file_free(kf);
    g_free(path);
}

// --------------------------------------------------------------- library --

// One boxed list of the anime (subdirectories) inside a configured folder,
// under a heading with the folder's path.
static GtkWidget *build_folder_section(const char *folder) {
    GtkWidget *section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);

    GtkWidget *heading = gtk_label_new(folder);
    gtk_label_set_xalign(GTK_LABEL(heading), 0.0);
    gtk_widget_add_css_class(heading, "heading");
    gtk_box_append(GTK_BOX(section), heading);

    GtkWidget *list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(list, "boxed-list");
    gtk_box_append(GTK_BOX(section), list);

    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    GDir *dir = g_dir_open(folder, 0, NULL);
    if (dir) {
        const char *name;
        while ((name = g_dir_read_name(dir))) {
            if (name[0] == '.')
                continue;
            char *full = g_build_filename(folder, name, NULL);
            if (g_file_test(full, G_FILE_TEST_IS_DIR))
                g_ptr_array_add(names, g_strdup(name));
            g_free(full);
        }
        g_dir_close(dir);
    }
    g_ptr_array_sort_values(names, (GCompareFunc)g_strcmp0);

    if (!dir) {
        GtkWidget *row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                      "Folder not accessible");
        gtk_widget_add_css_class(row, "dim-label");
        gtk_list_box_append(GTK_LIST_BOX(list), row);
    } else if (names->len == 0) {
        GtkWidget *row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                      "No anime in this folder");
        gtk_widget_add_css_class(row, "dim-label");
        gtk_list_box_append(GTK_LIST_BOX(list), row);
    }
    for (guint i = 0; i < names->len; i++) {
        GtkWidget *row = adw_action_row_new();
        char *escaped = g_markup_escape_text(names->pdata[i], -1);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), escaped);
        g_free(escaped);
        gtk_list_box_append(GTK_LIST_BOX(list), row);
    }
    g_ptr_array_unref(names);
    return section;
}

// Rebuild the main window content from the configured folders.
static void library_refresh(void) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(library_box)))
        gtk_box_remove(GTK_BOX(library_box), child);

    if (folders->len == 0) {
        gtk_stack_set_visible_child_name(GTK_STACK(library_stack), "empty");
        return;
    }
    for (guint i = 0; i < folders->len; i++)
        gtk_box_append(GTK_BOX(library_box),
                       build_folder_section(folders->pdata[i]));
    gtk_stack_set_visible_child_name(GTK_STACK(library_stack), "library");
}

// -------------------------------------------------------------- settings --

static void settings_rows_refresh(void);

static void on_folder_remove(GtkButton *btn, gpointer data) {
    (void)btn;
    guint idx = GPOINTER_TO_UINT(data);
    g_ptr_array_remove_index(folders, idx);
    config_save();
    settings_rows_refresh();
    library_refresh();
}

static void settings_rows_refresh(void) {
    if (!folders_group)
        return;
    for (guint i = 0; i < folder_rows->len; i++)
        adw_preferences_group_remove(folders_group, folder_rows->pdata[i]);
    g_ptr_array_set_size(folder_rows, 0);

    if (folders->len == 0) {
        GtkWidget *row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                      "No folders configured");
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row),
                                    "Add a folder that contains your anime");
        adw_preferences_group_add(folders_group, row);
        g_ptr_array_add(folder_rows, row);
        return;
    }
    for (guint i = 0; i < folders->len; i++) {
        GtkWidget *row = adw_action_row_new();
        char *escaped = g_markup_escape_text(folders->pdata[i], -1);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), escaped);
        g_free(escaped);

        GtkWidget *remove = gtk_button_new_from_icon_name("edit-delete-symbolic");
        gtk_widget_set_valign(remove, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(remove, "flat");
        gtk_widget_set_tooltip_text(remove, "Remove folder");
        g_signal_connect(remove, "clicked", G_CALLBACK(on_folder_remove),
                         GUINT_TO_POINTER(i));
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), remove);

        adw_preferences_group_add(folders_group, row);
        g_ptr_array_add(folder_rows, row);
    }
}

static void on_folder_chosen(GObject *src, GAsyncResult *res, gpointer data) {
    (void)data;
    GFile *file =
        gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, NULL);
    if (!file)
        return; // dismissed
    char *path = g_file_get_path(file);
    g_object_unref(file);
    if (!path)
        return;

    gboolean dup = FALSE;
    for (guint i = 0; i < folders->len; i++)
        if (g_strcmp0(folders->pdata[i], path) == 0)
            dup = TRUE;
    if (dup) {
        g_free(path);
        return;
    }
    g_ptr_array_add(folders, path);
    config_save();
    settings_rows_refresh();
    library_refresh();
}

static void on_add_folder(GtkButton *btn, gpointer data) {
    (void)btn;
    (void)data;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "Select anime folder");
    gtk_file_dialog_select_folder(dlg, main_window, NULL, on_folder_chosen,
                                  NULL);
    g_object_unref(dlg);
}

static void on_settings_closed(AdwDialog *dlg, gpointer data) {
    (void)dlg;
    (void)data;
    folders_group = NULL;
    g_clear_pointer(&folder_rows, g_ptr_array_unref);
}

static void open_settings(GtkButton *btn, gpointer data) {
    (void)btn;
    (void)data;
    AdwDialog *dlg = adw_preferences_dialog_new();
    adw_dialog_set_title(dlg, "Settings");
    adw_dialog_set_content_width(dlg, 560);

    AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    folders_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(folders_group, "Anime folders");
    adw_preferences_group_set_description(
        folders_group, "Each subdirectory of these folders is treated as one "
                       "anime");

    GtkWidget *add = gtk_button_new_from_icon_name("list-add-symbolic");
    gtk_widget_add_css_class(add, "flat");
    gtk_widget_set_tooltip_text(add, "Add folder");
    g_signal_connect(add, "clicked", G_CALLBACK(on_add_folder), NULL);
    adw_preferences_group_set_header_suffix(folders_group, add);

    folder_rows = g_ptr_array_new();
    settings_rows_refresh();

    adw_preferences_page_add(page, folders_group);
    adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(dlg), page);
    g_signal_connect(dlg, "closed", G_CALLBACK(on_settings_closed), NULL);
    adw_dialog_present(dlg, GTK_WIDGET(main_window));
}

// ------------------------------------------------------------------- app --

static void load_css(void) {
    char *exe = g_file_read_link("/proc/self/exe", NULL);
    char *dir = g_path_get_dirname(exe ? exe : ".");
    char *css = g_build_filename(dir, "style.css", NULL);
    if (g_file_test(css, G_FILE_TEST_EXISTS)) {
        GtkCssProvider *prov = gtk_css_provider_new();
        gtk_css_provider_load_from_path(prov, css);
        gtk_style_context_add_provider_for_display(
            gdk_display_get_default(), GTK_STYLE_PROVIDER(prov),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(prov);
    }
    g_free(css);
    g_free(dir);
    g_free(exe);
}

static void activate(AdwApplication *app, gpointer data) {
    (void)data;
    load_css();
    config_load();

    GtkWidget *win = adw_application_window_new(GTK_APPLICATION(app));
    main_window = GTK_WINDOW(win);
    gtk_window_set_title(main_window, "Animanager");
    gtk_window_set_default_size(main_window, 900, 640);

    GtkWidget *view = adw_toolbar_view_new();
    GtkWidget *header = adw_header_bar_new();
    GtkWidget *settings_btn =
        gtk_button_new_from_icon_name("emblem-system-symbolic");
    gtk_widget_set_tooltip_text(settings_btn, "Settings");
    g_signal_connect(settings_btn, "clicked", G_CALLBACK(open_settings), NULL);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), settings_btn);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);

    library_stack = gtk_stack_new();

    // empty state: no folders configured yet
    GtkWidget *status = adw_status_page_new();
    adw_status_page_set_title(ADW_STATUS_PAGE(status), "No anime folders");
    adw_status_page_set_description(
        ADW_STATUS_PAGE(status),
        "Add the folders that hold your anime to build the library");
    adw_status_page_set_icon_name(ADW_STATUS_PAGE(status),
                                  "folder-videos-symbolic");
    GtkWidget *open_btn = gtk_button_new_with_label("Open Settings");
    gtk_widget_add_css_class(open_btn, "suggested-action");
    gtk_widget_add_css_class(open_btn, "pill");
    gtk_widget_set_halign(open_btn, GTK_ALIGN_CENTER);
    g_signal_connect(open_btn, "clicked", G_CALLBACK(open_settings), NULL);
    adw_status_page_set_child(ADW_STATUS_PAGE(status), open_btn);
    gtk_stack_add_named(GTK_STACK(library_stack), status, "empty");

    // library: per-folder sections in a clamped scrolling column
    library_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 24);
    gtk_widget_set_margin_top(library_box, 24);
    gtk_widget_set_margin_bottom(library_box, 24);
    GtkWidget *clamp = adw_clamp_new();
    adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 760);
    adw_clamp_set_child(ADW_CLAMP(clamp), library_box);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), clamp);
    gtk_stack_add_named(GTK_STACK(library_stack), scroll, "library");

    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), library_stack);
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(win), view);

    library_refresh();
    gtk_window_present(main_window);
}

int main(int argc, char **argv) {
    AdwApplication *app = adw_application_new("org.nekoland.Animanager",
                                              G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
