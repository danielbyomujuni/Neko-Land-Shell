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

#define POSTER_W 185
#define POSTER_H 264 // ~2:3 like the jellyfin-style folder.jpg covers

// Per-card background load: cover art + episode/season counts, off the main
// thread because the library usually lives on a network mount.
typedef struct {
    char *path;          // anime directory
    GtkWidget *picture;  // ref'd
    GtkWidget *subtitle; // ref'd
    GdkPixbuf *pixbuf;   // result: scaled cover, or NULL
    int episodes;
    int seasons; // video-bearing subdirectories (Season 1, ...)
} CardLoad;

static void card_load_free(gpointer data) {
    CardLoad *cl = data;
    g_free(cl->path);
    g_object_unref(cl->picture);
    g_object_unref(cl->subtitle);
    g_clear_object(&cl->pixbuf);
    g_free(cl);
}

static gboolean is_video(const char *name) {
    static const char *exts[] = {".mkv", ".mp4",  ".avi", ".webm",
                                 ".mov", ".m2ts", ".ts",  NULL};
    for (int i = 0; exts[i]; i++)
        if (g_str_has_suffix(name, exts[i]))
            return TRUE;
    return FALSE;
}

static int count_videos(const char *path) {
    int n = 0;
    GDir *dir = g_dir_open(path, 0, NULL);
    if (!dir)
        return 0;
    const char *name;
    while ((name = g_dir_read_name(dir)))
        if (is_video(name))
            n++;
    g_dir_close(dir);
    return n;
}

static void card_load_thread(GTask *task, gpointer src, gpointer data,
                             GCancellable *cancel) {
    (void)src;
    (void)cancel;
    CardLoad *cl = data;

    static const char *covers[] = {"folder.jpg", "folder.png", "cover.jpg",
                                   "cover.png",  "poster.jpg", NULL};
    for (int i = 0; covers[i] && !cl->pixbuf; i++) {
        char *p = g_build_filename(cl->path, covers[i], NULL);
        if (g_file_test(p, G_FILE_TEST_EXISTS))
            cl->pixbuf = gdk_pixbuf_new_from_file_at_scale(p, POSTER_W * 2,
                                                           -1, TRUE, NULL);
        g_free(p);
    }

    cl->episodes = count_videos(cl->path);
    GDir *dir = g_dir_open(cl->path, 0, NULL);
    if (dir) {
        const char *name;
        while ((name = g_dir_read_name(dir))) {
            if (name[0] == '.')
                continue;
            char *sub = g_build_filename(cl->path, name, NULL);
            if (g_file_test(sub, G_FILE_TEST_IS_DIR)) {
                int n = count_videos(sub);
                if (n > 0) {
                    cl->seasons++;
                    cl->episodes += n;
                }
            }
            g_free(sub);
        }
        g_dir_close(dir);
    }
    g_task_return_boolean(task, TRUE);
}

static void card_load_done(GObject *src, GAsyncResult *res, gpointer data) {
    (void)src;
    (void)data;
    CardLoad *cl = g_task_get_task_data(G_TASK(res));
    if (cl->pixbuf) {
        // gdk_texture_new_for_pixbuf is deprecated; wrap the pixels directly
        GdkPixbuf *pb = cl->pixbuf;
        int h = gdk_pixbuf_get_height(pb);
        gsize stride = gdk_pixbuf_get_rowstride(pb);
        gsize size = stride * (h - 1) +
                     (gsize)gdk_pixbuf_get_width(pb) *
                         gdk_pixbuf_get_n_channels(pb);
        GBytes *bytes = g_bytes_new(gdk_pixbuf_read_pixels(pb), size);
        GdkTexture *tex = gdk_memory_texture_new(
            gdk_pixbuf_get_width(pb), h,
            gdk_pixbuf_get_has_alpha(pb) ? GDK_MEMORY_R8G8B8A8
                                         : GDK_MEMORY_R8G8B8,
            bytes, stride);
        g_bytes_unref(bytes);
        gtk_picture_set_paintable(GTK_PICTURE(cl->picture),
                                  GDK_PAINTABLE(tex));
        g_object_unref(tex);
    }
    char *sub;
    if (cl->seasons > 1)
        sub = g_strdup_printf("%d seasons · %d episodes", cl->seasons,
                              cl->episodes);
    else
        sub = g_strdup_printf("%d episode%s", cl->episodes,
                              cl->episodes == 1 ? "" : "s");
    gtk_label_set_text(GTK_LABEL(cl->subtitle), sub);
    g_free(sub);
}

static GtkWidget *build_anime_card(const char *folder, const char *name) {
    // the whole card is the poster; title + counts sit on a gradient
    // scrim over the artwork's bottom edge
    GtkWidget *card = gtk_overlay_new();
    gtk_widget_add_css_class(card, "anime-card");
    gtk_widget_set_overflow(card, GTK_OVERFLOW_HIDDEN);
    gtk_widget_set_size_request(card, POSTER_W, POSTER_H);

    GtkWidget *ph = gtk_image_new_from_icon_name("folder-videos-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(ph), 48);
    gtk_widget_add_css_class(ph, "poster-placeholder");
    gtk_overlay_set_child(GTK_OVERLAY(card), ph);

    GtkWidget *pic = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(pic), GTK_CONTENT_FIT_COVER);
    gtk_overlay_add_overlay(GTK_OVERLAY(card), pic);

    GtkWidget *caption = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_add_css_class(caption, "caption");
    gtk_widget_set_valign(caption, GTK_ALIGN_END);

    GtkWidget *title = gtk_label_new(name);
    gtk_widget_add_css_class(title, "anime-title");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0);
    gtk_label_set_wrap(GTK_LABEL(title), TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
    gtk_label_set_lines(GTK_LABEL(title), 2);
    // natural width ~0 so the flowbox child stays poster-width
    gtk_label_set_max_width_chars(GTK_LABEL(title), 1);
    gtk_box_append(GTK_BOX(caption), title);

    GtkWidget *subtitle = gtk_label_new("…");
    gtk_widget_add_css_class(subtitle, "anime-sub");
    gtk_label_set_xalign(GTK_LABEL(subtitle), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(subtitle), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(subtitle), 1);
    gtk_box_append(GTK_BOX(caption), subtitle);

    gtk_overlay_add_overlay(GTK_OVERLAY(card), caption);

    CardLoad *cl = g_new0(CardLoad, 1);
    cl->path = g_build_filename(folder, name, NULL);
    cl->picture = g_object_ref(pic);
    cl->subtitle = g_object_ref(subtitle);
    GTask *task = g_task_new(NULL, NULL, card_load_done, NULL);
    g_task_set_task_data(task, cl, card_load_free);
    g_task_run_in_thread(task, card_load_thread);
    g_object_unref(task);

    return card;
}

static int name_collate(gconstpointer a, gconstpointer b) {
    return g_utf8_collate(*(char *const *)a, *(char *const *)b);
}

// Poster grid for one configured folder; the folder-path heading is only
// shown when several folders are configured.
static GtkWidget *build_folder_section(const char *folder,
                                       gboolean show_heading) {
    GtkWidget *section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);

    if (show_heading) {
        GtkWidget *heading = gtk_label_new(folder);
        gtk_label_set_xalign(GTK_LABEL(heading), 0.0);
        gtk_widget_add_css_class(heading, "heading");
        gtk_box_append(GTK_BOX(section), heading);
    }

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
    g_ptr_array_sort(names, name_collate);

    if (!dir || names->len == 0) {
        GtkWidget *msg = gtk_label_new(!dir ? "Folder not accessible"
                                            : "No anime in this folder");
        gtk_label_set_xalign(GTK_LABEL(msg), 0.0);
        gtk_widget_add_css_class(msg, "dim-label");
        gtk_box_append(GTK_BOX(section), msg);
        g_ptr_array_unref(names);
        return section;
    }

    GtkWidget *grid = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(grid), GTK_SELECTION_NONE);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(grid), TRUE);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(grid), 16);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(grid), 16);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(grid), 2);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(grid), 30);
    gtk_widget_set_halign(grid, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(section), grid);

    for (guint i = 0; i < names->len; i++)
        gtk_flow_box_insert(GTK_FLOW_BOX(grid),
                            build_anime_card(folder, names->pdata[i]), -1);
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
                       build_folder_section(folders->pdata[i],
                                            folders->len > 1));
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

// dev hook: NEKOLAND_DEBUG_SCROLL=1 logs every scroll event reaching the
// window plus the resulting adjustment moves, to diagnose dropped wheel
// input
static gboolean dbg_scroll(GtkEventControllerScroll *c, double dx, double dy,
                           gpointer data) {
    (void)data;
    g_printerr("[scroll] dx=%+.3f dy=%+.3f unit=%s t=%u\n", dx, dy,
               gtk_event_controller_scroll_get_unit(c) ==
                       GDK_SCROLL_UNIT_WHEEL
                   ? "wheel"
                   : "surface",
               gdk_event_get_time(gtk_event_controller_get_current_event(
                   GTK_EVENT_CONTROLLER(c))));
    return FALSE; // observe only
}

static void dbg_scroll_edge(GtkEventControllerScroll *c, gpointer data) {
    g_printerr("[scroll] %s\n", (const char *)data);
    (void)c;
}

static void dbg_adj_changed(GtkAdjustment *adj, gpointer data) {
    (void)data;
    g_printerr("[adj] value=%.1f\n", gtk_adjustment_get_value(adj));
}

static void dbg_scroll_attach(GtkWidget *win, GtkWidget *scroll) {
    if (!g_getenv("NEKOLAND_DEBUG_SCROLL"))
        return;
    GtkEventController *c =
        gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    gtk_event_controller_set_propagation_phase(c, GTK_PHASE_CAPTURE);
    g_signal_connect(c, "scroll", G_CALLBACK(dbg_scroll), NULL);
    g_signal_connect(c, "scroll-begin", G_CALLBACK(dbg_scroll_edge), "begin");
    g_signal_connect(c, "scroll-end", G_CALLBACK(dbg_scroll_edge), "end");
    gtk_widget_add_controller(win, c);
    g_signal_connect(gtk_scrolled_window_get_vadjustment(
                         GTK_SCROLLED_WINDOW(scroll)),
                     "value-changed", G_CALLBACK(dbg_adj_changed), NULL);
}

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
    gtk_widget_set_margin_start(library_box, 18);
    gtk_widget_set_margin_end(library_box, 18);
    GtkWidget *clamp = adw_clamp_new();
    adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 1400);
    adw_clamp_set_child(ADW_CLAMP(clamp), library_box);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), clamp);
    gtk_stack_add_named(GTK_STACK(library_stack), scroll, "library");
    dbg_scroll_attach(win, scroll);

    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), library_stack);
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(win), view);

    library_refresh();
    gtk_window_present(main_window);
}

int main(int argc, char **argv) {
    // GTK defaults to the Vulkan renderer, which stutters when scrolling
    // on NVIDIA + Wayland; prefer GL (FALSE keeps any explicit override)
    g_setenv("GSK_RENDERER", "gl", FALSE);
    AdwApplication *app = adw_application_new("org.nekoland.Animanager",
                                              G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
