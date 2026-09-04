// launcher.c — built-in app launcher, phone-style:
//  - the main panel (morphing out of the sidebar) is a user-organised grid
//    of pinned apps: drag apps onto slots to place them, drop one app onto
//    another to fold them into a folder, drag folders to rearrange,
//    right-click a slot to unpin
//  - an "all apps" drawer slides up from the bottom with the search box and
//    the full application list; drag from it onto the grid to pin
//  - layout persists in ~/.config/nekoland/launcher-grid.conf
//
// The open/close morph itself lives in the shell chrome (main.c frame_draw
// follows Bar.launch_ext, animated here with a frame-clock tick).

#include "nekobar.h"

#include <gdk/gdkkeysyms.h>
#include <gio/gdesktopappinfo.h>
#include <stdlib.h>
#include <gtk-layer-shell/gtk-layer-shell.h>
#include <string.h>

#define GRID_COLS 4
#define GRID_ROWS 12
#define GRID_SLOTS (GRID_COLS * GRID_ROWS)
#define CELL_W 72
#define CELL_H 72
#define DRAWER_H NEKO_DRAWER_H

static const GtkTargetEntry dnd_target = {
    (char *)"application/x-nekoland-app", GTK_TARGET_SAME_APP, 0};

// slot -> desktop id, shared by every bar's grid
static GHashTable *grid_map;

// row -> label: a labeled divider line drawn above that grid row
static GHashTable *divider_map;

static gboolean ctx_menu_open;   // a context menu belongs to the launcher:
                                 // don't treat its grab as losing focus
static gboolean drag_active;     // a DnD grab also steals focus; ignore
                                 // focus-loss while dragging…
static gint64 drag_end_us;       // …and briefly after the drop settles
static gint64 last_autoclose_us; // guards the toggle button against
                                 // close-then-reopen on the same click

#define DRAG_GRACE_US 600000

static gboolean drag_settling(void) {
    return drag_active ||
           g_get_monotonic_time() - drag_end_us < DRAG_GRACE_US;
}

static gboolean drag_is_divider; // divider drags highlight a row edge,
                                 // not a landing cell

static void on_drag_begin(GtkWidget *w, GdkDragContext *ctx, gpointer data) {
    (void)ctx;
    (void)data;
    drag_active = TRUE;
    const char *p = g_object_get_data(G_OBJECT(w), "dnd-payload");
    drag_is_divider = p && g_str_has_prefix(p, "div:");
}

static void on_drag_end(GtkWidget *w, GdkDragContext *ctx, gpointer data) {
    (void)w;
    (void)ctx;
    (void)data;
    drag_active = FALSE;
    drag_is_divider = FALSE;
    drag_end_us = g_get_monotonic_time();
}

// ---- persistence ----

static char *grid_conf_path(void) {
    return g_build_filename(g_get_user_config_dir(), "nekoland",
                            "launcher-grid.conf", NULL);
}

static void grid_load(void) {
    if (grid_map)
        return;
    grid_map = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
                                     g_free);
    divider_map = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
                                        g_free);
    char *path = grid_conf_path();
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, path, 0, NULL)) {
        for (int i = 0; i < GRID_SLOTS; i++) {
            char key[16];
            g_snprintf(key, sizeof(key), "s%d", i);
            char *id = g_key_file_get_string(kf, "grid", key, NULL);
            if (id && *id)
                g_hash_table_insert(grid_map, GINT_TO_POINTER(i), id);
            else
                g_free(id);
        }
        for (int r = 0; r < GRID_ROWS; r++) {
            char key[16];
            g_snprintf(key, sizeof(key), "r%d", r);
            char *lbl = g_key_file_get_string(kf, "dividers", key, NULL);
            if (lbl)
                g_hash_table_insert(divider_map, GINT_TO_POINTER(r), lbl);
        }
    }
    g_key_file_free(kf);
    g_free(path);
}

static void grid_save(void) {
    GKeyFile *kf = g_key_file_new();
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, grid_map);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        char key[16];
        g_snprintf(key, sizeof(key), "s%d", GPOINTER_TO_INT(k));
        g_key_file_set_string(kf, "grid", key, v);
    }
    g_hash_table_iter_init(&it, divider_map);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        char key[16];
        g_snprintf(key, sizeof(key), "r%d", GPOINTER_TO_INT(k));
        g_key_file_set_string(kf, "dividers", key, v);
    }
    char *path = grid_conf_path();
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    g_key_file_save_to_file(kf, path, NULL);
    g_free(dir);
    g_free(path);
    g_key_file_free(kf);
}

// ---- folders ----
//
// a grid slot either holds a plain desktop id or a folder encoded as
//   "folder:<name>|<id>;<id>;…"
// ('|' and ';' are stripped from user-typed names, and don't occur in
// desktop ids in practice)

#define FOLDER_PREFIX "folder:"

static gboolean is_folder(const char *v) {
    return v && g_str_has_prefix(v, FOLDER_PREFIX);
}

// returns the id vector (g_strfreev; entries may be ""); name via out param
static char **folder_parse(const char *v, char **name_out) {
    const char *body = v + strlen(FOLDER_PREFIX);
    const char *sep = strchr(body, '|');
    if (name_out)
        *name_out = sep ? g_strndup(body, sep - body) : g_strdup("Folder");
    return g_strsplit(sep ? sep + 1 : "", ";", -1);
}

// append id to the folder at slot (no-op on duplicates)
static void folder_slot_add(int slot, const char *id) {
    const char *val = g_hash_table_lookup(grid_map, GINT_TO_POINTER(slot));
    if (!is_folder(val))
        return;
    char *name;
    char **ids = folder_parse(val, &name);
    GString *s = g_string_new(NULL);
    gboolean dup = FALSE;
    int n = 0;
    for (int i = 0; ids[i]; i++) {
        if (!*ids[i])
            continue;
        if (g_str_equal(ids[i], id))
            dup = TRUE;
        g_string_append_printf(s, "%s%s", n++ ? ";" : "", ids[i]);
    }
    if (!dup) {
        g_string_append_printf(s, "%s%s", n++ ? ";" : "", id);
        g_hash_table_insert(grid_map, GINT_TO_POINTER(slot),
                            g_strdup_printf(FOLDER_PREFIX "%s|%s", name,
                                            s->str));
    }
    g_string_free(s, TRUE);
    g_strfreev(ids);
    g_free(name);
}

// remove id from the folder at slot; a folder left with one app collapses
// back to a plain app cell, an empty one vanishes
static void folder_slot_remove(int slot, const char *id) {
    const char *val = g_hash_table_lookup(grid_map, GINT_TO_POINTER(slot));
    if (!is_folder(val))
        return;
    char *name;
    char **ids = folder_parse(val, &name);
    GString *s = g_string_new(NULL);
    const char *last = NULL;
    int n = 0;
    for (int i = 0; ids[i]; i++) {
        if (!*ids[i] || g_str_equal(ids[i], id))
            continue;
        g_string_append_printf(s, "%s%s", n++ ? ";" : "", ids[i]);
        last = ids[i];
    }
    if (n == 0)
        g_hash_table_remove(grid_map, GINT_TO_POINTER(slot));
    else if (n == 1)
        g_hash_table_insert(grid_map, GINT_TO_POINTER(slot),
                            g_strdup(last));
    else
        g_hash_table_insert(grid_map, GINT_TO_POINTER(slot),
                            g_strdup_printf(FOLDER_PREFIX "%s|%s", name,
                                            s->str));
    g_string_free(s, TRUE);
    g_strfreev(ids);
    g_free(name);
}

// drop an app onto a slot: empty → place, folder → add,
// another app → fold both into a new folder
static void slot_place_app(int slot, const char *id) {
    const char *cur = g_hash_table_lookup(grid_map, GINT_TO_POINTER(slot));
    if (!cur) {
        g_hash_table_insert(grid_map, GINT_TO_POINTER(slot), g_strdup(id));
    } else if (is_folder(cur)) {
        folder_slot_add(slot, id);
    } else if (!g_str_equal(cur, id)) {
        char *nv = g_strdup_printf(FOLDER_PREFIX "Folder|%s;%s", cur, id);
        g_hash_table_insert(grid_map, GINT_TO_POINTER(slot), nv);
    }
}

// is the app pinned anywhere, directly or inside a folder?
static gboolean grid_contains_app(const char *id) {
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, grid_map);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        if (is_folder(v)) {
            char *name;
            char **ids = folder_parse(v, &name);
            gboolean found = FALSE;
            for (int i = 0; ids[i] && !found; i++)
                if (g_str_equal(ids[i], id))
                    found = TRUE;
            g_strfreev(ids);
            g_free(name);
            if (found)
                return TRUE;
        } else if (g_str_equal(v, id)) {
            return TRUE;
        }
    }
    return FALSE;
}

// ---- morph animation (chrome opens/closes around the panel) ----

static gboolean launcher_visible(Bar *bar) {
    return bar->launch_target == 1;
}

static gboolean launch_tick(GtkWidget *w, GdkFrameClock *clock,
                            gpointer data) {
    (void)w;
    Bar *bar = data;
    gint64 now = gdk_frame_clock_get_frame_time(clock);
    double dt = CLAMP((now - bar->launch_last_us) / 1e6, 0.0, 0.05);
    bar->launch_last_us = now;

    double target = bar->launch_target;
    double diff = target - bar->launch_ext;
    bar->launch_ext += diff * MIN(1.0, 14.0 * dt);
    if (ABS(target - bar->launch_ext) < 0.004)
        bar->launch_ext = target;

    gtk_widget_queue_draw(bar->frame);
    if (bar->launcher)
        gtk_widget_set_opacity(bar->launcher,
                               bar->launch_ext * bar->launch_ext);

    if (bar->launch_ext == target) {
        if (target == 0 && bar->launcher)
            gtk_widget_hide(bar->launcher);
        bar->launch_tick = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void launch_animate(Bar *bar, int target) {
    bar->launch_target = target;
    if (!bar->launch_tick) {
        bar->launch_last_us = 0;
        GdkFrameClock *clock = gtk_widget_get_frame_clock(bar->frame);
        if (clock)
            bar->launch_last_us = gdk_frame_clock_get_frame_time(clock);
        bar->launch_tick =
            gtk_widget_add_tick_callback(bar->frame, launch_tick, bar, NULL);
    }
}

static void folder_card_close(Bar *bar);

static void launcher_hide(Bar *bar) {
    folder_card_close(bar);
    if (launcher_visible(bar))
        launch_animate(bar, 0);
}

// ---- app lookup ----

// caller owns the table; values are GAppInfo refs
static GHashTable *apps_by_id(void) {
    GHashTable *t = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                          g_object_unref);
    GList *apps = g_app_info_get_all();
    for (GList *l = apps; l; l = l->next) {
        GAppInfo *info = l->data;
        const char *id = g_app_info_get_id(info);
        if (id && g_app_info_should_show(info))
            g_hash_table_insert(t, g_strdup(id), info);
        else
            g_object_unref(info);
    }
    g_list_free(apps);
    return t;
}

// ---- shared cell content ----

static GtkWidget *app_cell_content(GAppInfo *info) {
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    // the cell label is ellipsized: hover reveals the full name
    gtk_widget_set_tooltip_text(v, g_app_info_get_display_name(info));
    GIcon *gicon = g_app_info_get_icon(info);
    GtkWidget *img =
        gicon ? gtk_image_new_from_gicon(gicon, GTK_ICON_SIZE_DND)
              : gtk_image_new_from_icon_name("application-x-executable",
                                             GTK_ICON_SIZE_DND);
    gtk_image_set_pixel_size(GTK_IMAGE(img), 34);
    gtk_box_pack_start(GTK_BOX(v), img, FALSE, FALSE, 0);
    GtkWidget *lbl = gtk_label_new(g_app_info_get_display_name(info));
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(lbl), 9);
    gtk_label_set_justify(GTK_LABEL(lbl), GTK_JUSTIFY_CENTER);
    gtk_widget_set_name(lbl, "app-label");
    gtk_box_pack_start(GTK_BOX(v), lbl, FALSE, FALSE, 0);
    return v;
}

// ---- launching ----

typedef struct {
    Bar *bar;
    GAppInfo *info;
} LaunchCtx;

static void launch_ctx_free(gpointer data, GClosure *closure) {
    (void)closure;
    LaunchCtx *c = data;
    g_object_unref(c->info);
    g_free(c);
}

static void on_app_clicked(GtkWidget *btn, gpointer data) {
    (void)btn;
    LaunchCtx *c = data;
    g_app_info_launch(c->info, NULL, NULL, NULL);
    launcher_hide(c->bar);
}

// ---- drag and drop ----

// payloads: "app:<desktop id>" (from the drawer), "slot:<n>" (from grid),
// "fold:<slot>|<id>" (an app dragged out of an open folder), or
// "div:<row>" (a row divider being moved)

static void on_drag_get(GtkWidget *w, GdkDragContext *ctx,
                        GtkSelectionData *sel, guint info, guint time,
                        gpointer data) {
    (void)ctx;
    (void)info;
    (void)time;
    (void)data;
    const char *payload = g_object_get_data(G_OBJECT(w), "dnd-payload");
    if (payload)
        gtk_selection_data_set(sel, gtk_selection_data_get_target(sel), 8,
                               (const guchar *)payload, strlen(payload));
}

static void grid_rebuild_all(void);

// a dragged divider lands above the hovered slot's whole row — mark the
// top edge of every cell in that row instead of glowing one cell
static void row_mark(GtkWidget *cell, gboolean on) {
    GtkWidget *grid = gtk_widget_get_parent(cell);
    int row = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(cell), "slot")) /
              GRID_COLS;
    GList *kids = gtk_container_get_children(GTK_CONTAINER(grid));
    for (GList *l = kids; l; l = l->next) {
        GtkWidget *w = l->data;
        if (!GTK_IS_BUTTON(w)) // skip divider event boxes
            continue;
        int s = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "slot"));
        if (s / GRID_COLS != row)
            continue;
        GtkStyleContext *sc = gtk_widget_get_style_context(w);
        if (on)
            gtk_style_context_add_class(sc, "drop-divider");
        else
            gtk_style_context_remove_class(sc, "drop-divider");
    }
    g_list_free(kids);
}

// while a drag hovers a slot, mark it so the CSS can glow the landing spot
static gboolean on_slot_motion(GtkWidget *w, GdkDragContext *ctx, gint x,
                               gint y, guint time, gpointer data) {
    (void)x;
    (void)y;
    (void)data;
    if (drag_is_divider)
        row_mark(w, TRUE);
    else
        gtk_style_context_add_class(gtk_widget_get_style_context(w),
                                    "drop-target");
    gdk_drag_status(ctx, gdk_drag_context_get_suggested_action(ctx), time);
    return TRUE;
}

static void on_slot_leave(GtkWidget *w, GdkDragContext *ctx, guint time,
                          gpointer data) {
    (void)ctx;
    (void)time;
    (void)data;
    row_mark(w, FALSE);
    gtk_style_context_remove_class(gtk_widget_get_style_context(w),
                                   "drop-target");
}

static void on_slot_drop(GtkWidget *w, GdkDragContext *ctx, gint x, gint y,
                         GtkSelectionData *sel, guint info, guint time,
                         gpointer data) {
    (void)x;
    (void)y;
    (void)info;
    (void)data;
    int slot = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "slot"));
    const guchar *raw = gtk_selection_data_get_data(sel);
    int len = gtk_selection_data_get_length(sel);
    gboolean ok = FALSE;
    if (raw && len > 4) {
        char *payload = g_strndup((const char *)raw, len);
        if (g_str_has_prefix(payload, "app:")) {
            slot_place_app(slot, payload + 4);
            ok = TRUE;
        } else if (g_str_has_prefix(payload, "fold:")) {
            char *sep = strchr(payload + 5, '|');
            int from = atoi(payload + 5);
            if (sep && from != slot) {
                folder_slot_remove(from, sep + 1);
                slot_place_app(slot, sep + 1);
                ok = TRUE;
            }
        } else if (g_str_has_prefix(payload, "div:")) {
            int from = atoi(payload + 4);
            int to = slot / GRID_COLS; // divider lands above this row
            if (to != from &&
                g_hash_table_contains(divider_map, GINT_TO_POINTER(from)) &&
                !g_hash_table_contains(divider_map, GINT_TO_POINTER(to))) {
                char *lbl = g_strdup(g_hash_table_lookup(
                    divider_map, GINT_TO_POINTER(from)));
                g_hash_table_remove(divider_map, GINT_TO_POINTER(from));
                g_hash_table_insert(divider_map, GINT_TO_POINTER(to), lbl);
                ok = TRUE;
            }
        } else if (g_str_has_prefix(payload, "slot:")) {
            int from = atoi(payload + 5);
            const char *a =
                g_hash_table_lookup(grid_map, GINT_TO_POINTER(from));
            const char *b =
                g_hash_table_lookup(grid_map, GINT_TO_POINTER(slot));
            if (from != slot && a) {
                if (b && !is_folder(a)) {
                    // an app dropped on an occupied cell folds into it
                    char *id = g_strdup(a);
                    g_hash_table_remove(grid_map, GINT_TO_POINTER(from));
                    slot_place_app(slot, id);
                    g_free(id);
                } else {
                    // folders (and drops on empty cells) move/swap
                    char *av = g_strdup(a);
                    char *bv = b ? g_strdup(b) : NULL;
                    if (bv)
                        g_hash_table_insert(grid_map, GINT_TO_POINTER(from),
                                            bv);
                    else
                        g_hash_table_remove(grid_map, GINT_TO_POINTER(from));
                    g_hash_table_insert(grid_map, GINT_TO_POINTER(slot), av);
                }
                ok = TRUE;
            }
        }
        g_free(payload);
    }
    row_mark(w, FALSE);
    gtk_style_context_remove_class(gtk_widget_get_style_context(w),
                                   "drop-target");
    gtk_drag_finish(ctx, ok, FALSE, time);
    // the rebuild below may destroy the drag source before its drag-end
    // fires, which would leave drag_active stuck — settle it here
    on_drag_end(NULL, NULL, NULL);
    if (ok) {
        grid_save();
        grid_rebuild_all();
    }
}

// ---- context menus ----

static void str_free_notify(gpointer data, GClosure *closure) {
    (void)closure;
    g_free(data);
}

static gboolean menu_destroy_idle(gpointer menu) {
    gtk_widget_destroy(menu);
    return G_SOURCE_REMOVE;
}

static void on_menu_deactivate(GtkWidget *menu, gpointer data) {
    (void)data; // destroy after the activate handler has run
    ctx_menu_open = FALSE;
    g_idle_add(menu_destroy_idle, menu);
}

static void menu_launch_cb(GtkMenuItem *item, gpointer data) {
    (void)item;
    LaunchCtx *c = data;
    g_app_info_launch(c->info, NULL, NULL, NULL);
    launcher_hide(c->bar);
}

static void menu_action_cb(GtkMenuItem *item, gpointer data) {
    LaunchCtx *c = data;
    const char *action = g_object_get_data(G_OBJECT(item), "action");
    if (action && G_IS_DESKTOP_APP_INFO(c->info))
        g_desktop_app_info_launch_action(G_DESKTOP_APP_INFO(c->info),
                                         action, NULL);
    launcher_hide(c->bar);
}

static void menu_unpin_cb(GtkMenuItem *item, gpointer data) {
    (void)item;
    int slot = GPOINTER_TO_INT(data);
    if (g_hash_table_remove(grid_map, GINT_TO_POINTER(slot))) {
        grid_save();
        grid_rebuild_all();
    }
}

static void menu_pin_cb(GtkMenuItem *item, gpointer data) {
    (void)item;
    const char *id = data;
    for (int i = 0; i < GRID_SLOTS; i++) { // first free slot
        if (!g_hash_table_lookup(grid_map, GINT_TO_POINTER(i))) {
            g_hash_table_insert(grid_map, GINT_TO_POINTER(i),
                                g_strdup(id));
            grid_save();
            grid_rebuild_all();
            break;
        }
    }
}

// ---- row dividers (segment the grid: games / IDEs / …) ----

static void menu_divider_add_cb(GtkMenuItem *item, gpointer data) {
    (void)item;
    g_hash_table_insert(divider_map, data, g_strdup(""));
    grid_save();
    grid_rebuild_all();
}

static void menu_divider_remove_cb(GtkMenuItem *item, gpointer data) {
    (void)item;
    if (g_hash_table_remove(divider_map, data)) {
        grid_save();
        grid_rebuild_all();
    }
}

// "Add/Remove divider above" for the row a slot belongs to
static void menu_append_divider_items(GtkWidget *menu, int slot) {
    int row = slot / GRID_COLS;
    GtkWidget *item;
    if (g_hash_table_contains(divider_map, GINT_TO_POINTER(row))) {
        item = gtk_menu_item_new_with_label("Remove divider above");
        g_signal_connect(item, "activate",
                         G_CALLBACK(menu_divider_remove_cb),
                         GINT_TO_POINTER(row));
    } else {
        item = gtk_menu_item_new_with_label("Add divider above");
        g_signal_connect(item, "activate",
                         G_CALLBACK(menu_divider_add_cb),
                         GINT_TO_POINTER(row));
    }
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
}

static void divider_commit(GtkWidget *entry) {
    int row = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(entry), "div-row"));
    if (!g_hash_table_contains(divider_map, GINT_TO_POINTER(row)))
        return;
    const char *cur = g_hash_table_lookup(divider_map, GINT_TO_POINTER(row));
    const char *txt = gtk_entry_get_text(GTK_ENTRY(entry));
    if (!g_str_equal(cur ? cur : "", txt)) {
        g_hash_table_insert(divider_map, GINT_TO_POINTER(row),
                            g_strdup(txt));
        grid_save();
    }
}

static void on_divider_activate(GtkEntry *entry, gpointer data) {
    (void)data;
    divider_commit(GTK_WIDGET(entry));
}

static gboolean on_divider_focus_out(GtkWidget *entry, GdkEventFocus *ev,
                                     gpointer data) {
    (void)ev;
    (void)data;
    divider_commit(entry);
    return FALSE;
}

static gboolean on_divider_press(GtkWidget *w, GdkEventButton *ev,
                                 gpointer data) {
    (void)data;
    if (ev->button != 3)
        return FALSE;
    int row = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "div-row"));
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *item = gtk_menu_item_new_with_label("Remove divider");
    g_signal_connect(item, "activate", G_CALLBACK(menu_divider_remove_cb),
                     GINT_TO_POINTER(row));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    g_signal_connect(menu, "deactivate", G_CALLBACK(on_menu_deactivate),
                     NULL);
    ctx_menu_open = TRUE;
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
    return TRUE;
}

// line — editable label — line, spanning the grid's width; the line part
// drags (payload "div:<row>") to move the divider to another row
static GtkWidget *divider_new(int row) {
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_name(h, "grid-divider");
    GtkWidget *left = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_valign(left, GTK_ALIGN_CENTER);
    GtkWidget *entry = gtk_entry_new();
    gtk_widget_set_name(entry, "divider-label");
    const char *lbl = g_hash_table_lookup(divider_map, GINT_TO_POINTER(row));
    gtk_entry_set_text(GTK_ENTRY(entry), lbl ? lbl : "");
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "label");
    gtk_entry_set_has_frame(GTK_ENTRY(entry), FALSE);
    gtk_entry_set_alignment(GTK_ENTRY(entry), 0.5);
    gtk_entry_set_width_chars(GTK_ENTRY(entry), 8);
    g_object_set_data(G_OBJECT(entry), "div-row", GINT_TO_POINTER(row));
    g_signal_connect(entry, "activate", G_CALLBACK(on_divider_activate),
                     NULL);
    g_signal_connect(entry, "focus-out-event",
                     G_CALLBACK(on_divider_focus_out), NULL);
    g_signal_connect(entry, "button-press-event",
                     G_CALLBACK(on_divider_press), NULL);
    GtkWidget *right = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_valign(right, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(h), left, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(h), entry, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(h), right, TRUE, TRUE, 0);

    // event box catches presses the entry doesn't (i.e. on the lines):
    // left-drag moves the divider, right-click offers removal
    GtkWidget *eb = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(eb), FALSE);
    gtk_container_add(GTK_CONTAINER(eb), h);
    g_object_set_data(G_OBJECT(eb), "div-row", GINT_TO_POINTER(row));
    char *payload = g_strdup_printf("div:%d", row);
    g_object_set_data_full(G_OBJECT(eb), "dnd-payload", payload, g_free);
    gtk_drag_source_set(eb, GDK_BUTTON1_MASK, &dnd_target, 1,
                        GDK_ACTION_MOVE);
    g_signal_connect(eb, "drag-data-get", G_CALLBACK(on_drag_get), NULL);
    g_signal_connect(eb, "drag-begin", G_CALLBACK(on_drag_begin), NULL);
    g_signal_connect(eb, "drag-end", G_CALLBACK(on_drag_end), NULL);
    g_signal_connect(eb, "button-press-event", G_CALLBACK(on_divider_press),
                     NULL);
    return eb;
}

// empty slots only get a menu for divider management
static gboolean on_empty_press(GtkWidget *w, GdkEventButton *ev,
                               gpointer data) {
    (void)data;
    if (ev->button != 3)
        return FALSE;
    int slot = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "slot"));
    GtkWidget *menu = gtk_menu_new();
    menu_append_divider_items(menu, slot);
    g_signal_connect(menu, "deactivate", G_CALLBACK(on_menu_deactivate),
                     NULL);
    ctx_menu_open = TRUE;
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
    return TRUE;
}

static GtkWidget *menu_item_ctx(GtkWidget *menu, const char *label,
                                GCallback cb, Bar *bar, GAppInfo *info) {
    GtkWidget *item = gtk_menu_item_new_with_label(label);
    LaunchCtx *c = g_new0(LaunchCtx, 1);
    c->bar = bar;
    c->info = g_object_ref(info);
    g_signal_connect_data(item, "activate", cb, c, launch_ctx_free, 0);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    return item;
}

// slot >= 0: pinned cell (offers Unpin); slot < 0: drawer cell (offers Pin)
static void app_menu_popup(Bar *bar, GAppInfo *info, int slot,
                           GdkEventButton *ev) {
    GtkWidget *menu = gtk_menu_new();

    menu_item_ctx(menu, "Launch", G_CALLBACK(menu_launch_cb), bar, info);

    // .desktop actions (e.g. "New Private Window")
    if (G_IS_DESKTOP_APP_INFO(info)) {
        const char *const *actions =
            g_desktop_app_info_list_actions(G_DESKTOP_APP_INFO(info));
        if (actions && actions[0])
            gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                                  gtk_separator_menu_item_new());
        for (int i = 0; actions && actions[i]; i++) {
            char *name = g_desktop_app_info_get_action_name(
                G_DESKTOP_APP_INFO(info), actions[i]);
            GtkWidget *item = menu_item_ctx(menu, name ? name : actions[i],
                                            G_CALLBACK(menu_action_cb), bar,
                                            info);
            g_object_set_data_full(G_OBJECT(item), "action",
                                   g_strdup(actions[i]), g_free);
            g_free(name);
        }
    }

    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                          gtk_separator_menu_item_new());
    if (slot >= 0) {
        GtkWidget *item = gtk_menu_item_new_with_label("Unpin");
        g_signal_connect(item, "activate", G_CALLBACK(menu_unpin_cb),
                         GINT_TO_POINTER(slot));
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
        menu_append_divider_items(menu, slot);
    } else {
        const char *id = g_app_info_get_id(info);
        gboolean pinned = id && grid_contains_app(id), full = TRUE;
        for (int i = 0; i < GRID_SLOTS && full; i++)
            if (!g_hash_table_lookup(grid_map, GINT_TO_POINTER(i)))
                full = FALSE;
        GtkWidget *item = gtk_menu_item_new_with_label("Pin to grid");
        gtk_widget_set_sensitive(item, id && !pinned && !full);
        g_object_set_data_full(G_OBJECT(item), "pin-id", g_strdup(id),
                               g_free);
        g_signal_connect_data(item, "activate", G_CALLBACK(menu_pin_cb),
                              g_strdup(id), str_free_notify, 0);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }

    g_signal_connect(menu, "deactivate", G_CALLBACK(on_menu_deactivate),
                     NULL);
    ctx_menu_open = TRUE;
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
}

static gboolean on_slot_press(GtkWidget *w, GdkEventButton *ev,
                              gpointer data) {
    LaunchCtx *c = data; // the cell's launch context (bar + app)
    if (ev->button == 3) {
        int slot = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "slot"));
        app_menu_popup(c->bar, c->info, slot, ev);
        return TRUE;
    }
    return FALSE; // let clicks/drags through
}

static gboolean on_drawer_press(GtkWidget *w, GdkEventButton *ev,
                                gpointer data) {
    (void)w;
    LaunchCtx *c = data;
    if (ev->button == 3) {
        app_menu_popup(c->bar, c->info, -1, ev);
        return TRUE;
    }
    return FALSE;
}

// ---- folder cells & popup ----

static gboolean rebuild_idle(gpointer data) {
    (void)data;
    grid_rebuild_all();
    return G_SOURCE_REMOVE;
}

// mini 2x2 icon preview + folder name
static GtkWidget *folder_cell_content(const char *val, GHashTable *apps) {
    char *name;
    char **ids = folder_parse(val, &name);
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_set_tooltip_text(v, name); // full folder name on hover
    GtkWidget *mini = gtk_grid_new();
    gtk_widget_set_name(mini, "folder-preview");
    gtk_grid_set_row_spacing(GTK_GRID(mini), 2);
    gtk_grid_set_column_spacing(GTK_GRID(mini), 2);
    gtk_widget_set_halign(mini, GTK_ALIGN_CENTER);
    int n = 0;
    for (int i = 0; ids[i] && n < 4; i++) {
        if (!*ids[i])
            continue;
        GAppInfo *info = g_hash_table_lookup(apps, ids[i]);
        GIcon *gicon = info ? g_app_info_get_icon(info) : NULL;
        GtkWidget *img =
            gicon ? gtk_image_new_from_gicon(gicon, GTK_ICON_SIZE_MENU)
                  : gtk_image_new_from_icon_name("application-x-executable",
                                                 GTK_ICON_SIZE_MENU);
        gtk_image_set_pixel_size(GTK_IMAGE(img), 13);
        gtk_grid_attach(GTK_GRID(mini), img, n % 2, n / 2, 1, 1);
        n++;
    }
    gtk_box_pack_start(GTK_BOX(v), mini, FALSE, FALSE, 0);
    GtkWidget *lbl = gtk_label_new(name);
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(lbl), 9);
    gtk_label_set_justify(GTK_LABEL(lbl), GTK_JUSTIFY_CENTER);
    gtk_widget_set_name(lbl, "app-label");
    gtk_box_pack_start(GTK_BOX(v), lbl, FALSE, FALSE, 0);
    g_strfreev(ids);
    g_free(name);
    return v;
}

// context for an app row inside an open folder card
typedef struct {
    Bar *bar;
    int slot;
    char *id;
} FItemCtx;

static FItemCtx *fitem_new(Bar *bar, int slot, const char *id) {
    FItemCtx *c = g_new0(FItemCtx, 1);
    c->bar = bar;
    c->slot = slot;
    c->id = g_strdup(id);
    return c;
}

static void fitem_free(gpointer data, GClosure *closure) {
    (void)closure;
    FItemCtx *c = data;
    g_free(c->id);
    g_free(c);
}

static void on_fitem_clicked(GtkWidget *btn, gpointer data) {
    (void)btn;
    FItemCtx *c = data;
    GDesktopAppInfo *info = g_desktop_app_info_new(c->id);
    if (info) {
        g_app_info_launch(G_APP_INFO(info), NULL, NULL, NULL);
        g_object_unref(info);
    }
    launcher_hide(c->bar);
}

static void fitem_remove_cb(GtkMenuItem *item, gpointer data) {
    (void)item;
    FItemCtx *c = data;
    folder_slot_remove(c->slot, c->id);
    grid_save();
    folder_card_close(c->bar);
    g_idle_add(rebuild_idle, NULL);
}

static void fitem_togrid_cb(GtkMenuItem *item, gpointer data) {
    (void)item;
    FItemCtx *c = data;
    int free_slot = -1;
    for (int s = 0; s < GRID_SLOTS && free_slot < 0; s++)
        if (!g_hash_table_lookup(grid_map, GINT_TO_POINTER(s)))
            free_slot = s;
    if (free_slot < 0)
        return;
    folder_slot_remove(c->slot, c->id);
    g_hash_table_insert(grid_map, GINT_TO_POINTER(free_slot),
                        g_strdup(c->id));
    grid_save();
    folder_card_close(c->bar);
    g_idle_add(rebuild_idle, NULL);
}

static gboolean on_fitem_press(GtkWidget *w, GdkEventButton *ev,
                               gpointer data) {
    (void)w;
    FItemCtx *c = data;
    if (ev->button != 3)
        return FALSE;
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *item = gtk_menu_item_new_with_label("Move to grid");
    g_signal_connect_data(item, "activate", G_CALLBACK(fitem_togrid_cb),
                          fitem_new(c->bar, c->slot, c->id), fitem_free, 0);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    item = gtk_menu_item_new_with_label("Remove from folder");
    g_signal_connect_data(item, "activate", G_CALLBACK(fitem_remove_cb),
                          fitem_new(c->bar, c->slot, c->id), fitem_free, 0);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    g_signal_connect(menu, "deactivate", G_CALLBACK(on_menu_deactivate),
                     NULL);
    ctx_menu_open = TRUE;
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
    return TRUE;
}

// rename applied when the card closes (Enter also closes it)
static gboolean folder_name_commit(Bar *bar) {
    int slot = bar->folder_slot;
    GtkWidget *entry =
        g_object_get_data(G_OBJECT(bar->folder_card), "name-entry");
    const char *val = g_hash_table_lookup(grid_map, GINT_TO_POINTER(slot));
    if (!entry || !is_folder(val))
        return FALSE;
    char *name;
    char **ids = folder_parse(val, &name);
    char *clean = g_strdup(gtk_entry_get_text(GTK_ENTRY(entry)));
    g_strdelimit(clean, "|;", ' ');
    g_strstrip(clean);
    gboolean changed = *clean && !g_str_equal(clean, name);
    if (changed) {
        char *joined = g_strjoinv(";", ids);
        g_hash_table_insert(grid_map, GINT_TO_POINTER(slot),
                            g_strdup_printf(FOLDER_PREFIX "%s|%s", clean,
                                            joined));
        g_free(joined);
        grid_save();
    }
    g_free(clean);
    g_strfreev(ids);
    g_free(name);
    return changed;
}

// destroy the open folder card, committing a pending rename
static void folder_card_close(Bar *bar) {
    if (!bar->folder_card)
        return;
    gboolean changed = folder_name_commit(bar);
    GtkWidget *card = bar->folder_card;
    bar->folder_card = NULL;
    gtk_widget_destroy(card);
    if (changed)
        g_idle_add(rebuild_idle, NULL);
}

static void on_folder_name_activate(GtkEntry *entry, gpointer data) {
    (void)entry;
    folder_card_close(data);
}

// open the folder as a card floating over the pinned grid — same surface
// as the panel, so no popup focus churn and no compositor round-trips
static void folder_card_open(Bar *bar, int slot) {
    const char *val = g_hash_table_lookup(grid_map, GINT_TO_POINTER(slot));
    if (!is_folder(val) || !bar->launcher_overlay)
        return;
    folder_card_close(bar);
    char *name;
    char **ids = folder_parse(val, &name);
    GHashTable *apps = apps_by_id();

    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_name(v, "folder-card");
    gtk_container_set_border_width(GTK_CONTAINER(v), 10);
    gtk_widget_set_halign(v, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(v, GTK_ALIGN_CENTER);
    bar->folder_card = v;
    bar->folder_slot = slot;

    GtkWidget *entry = gtk_entry_new();
    gtk_widget_set_name(entry, "folder-name");
    gtk_entry_set_text(GTK_ENTRY(entry), name);
    gtk_entry_set_alignment(GTK_ENTRY(entry), 0.5);
    gtk_entry_set_width_chars(GTK_ENTRY(entry), 10);
    g_signal_connect(entry, "activate",
                     G_CALLBACK(on_folder_name_activate), bar);
    gtk_box_pack_start(GTK_BOX(v), entry, FALSE, FALSE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 4);
    int n = 0;
    for (int i = 0; ids[i]; i++) {
        if (!*ids[i])
            continue;
        GAppInfo *info = g_hash_table_lookup(apps, ids[i]);
        GtkWidget *btn = gtk_button_new();
        gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);
        gtk_widget_set_name(btn, "app-cell");
        gtk_widget_set_size_request(btn, CELL_W, CELL_H);
        if (info) {
            gtk_container_add(GTK_CONTAINER(btn), app_cell_content(info));
        } else { // app no longer installed — still removable
            GtkWidget *fb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
            GtkWidget *img = gtk_image_new_from_icon_name(
                "application-x-executable", GTK_ICON_SIZE_DND);
            gtk_image_set_pixel_size(GTK_IMAGE(img), 34);
            gtk_box_pack_start(GTK_BOX(fb), img, FALSE, FALSE, 0);
            gtk_widget_set_tooltip_text(fb, ids[i]); // full id on hover
            GtkWidget *lbl = gtk_label_new(ids[i]);
            gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
            gtk_label_set_max_width_chars(GTK_LABEL(lbl), 9);
            gtk_widget_set_name(lbl, "app-label");
            gtk_box_pack_start(GTK_BOX(fb), lbl, FALSE, FALSE, 0);
            gtk_container_add(GTK_CONTAINER(btn), fb);
        }
        g_signal_connect_data(btn, "clicked", G_CALLBACK(on_fitem_clicked),
                              fitem_new(bar, slot, ids[i]), fitem_free, 0);
        g_signal_connect_data(btn, "button-press-event",
                              G_CALLBACK(on_fitem_press),
                              fitem_new(bar, slot, ids[i]), fitem_free, 0);
        // drag out of the folder back onto the grid (or the drawer handle)
        char *payload = g_strdup_printf("fold:%d|%s", slot, ids[i]);
        g_object_set_data_full(G_OBJECT(btn), "dnd-payload", payload,
                               g_free);
        gtk_drag_source_set(btn, GDK_BUTTON1_MASK, &dnd_target, 1,
                            GDK_ACTION_MOVE);
        GIcon *gicon = info ? g_app_info_get_icon(info) : NULL;
        if (gicon)
            gtk_drag_source_set_icon_gicon(btn, gicon);
        g_signal_connect(btn, "drag-data-get", G_CALLBACK(on_drag_get),
                         NULL);
        g_signal_connect(btn, "drag-begin", G_CALLBACK(on_drag_begin),
                         NULL);
        g_signal_connect(btn, "drag-end", G_CALLBACK(on_drag_end), NULL);
        gtk_grid_attach(GTK_GRID(grid), btn, n % 3, n / 3, 1, 1);
        n++;
    }
    gtk_box_pack_start(GTK_BOX(v), grid, FALSE, FALSE, 0);

    g_object_set_data(G_OBJECT(v), "name-entry", entry);
    gtk_overlay_add_overlay(GTK_OVERLAY(bar->launcher_overlay), v);
    gtk_widget_show_all(v);

    g_hash_table_destroy(apps);
    g_strfreev(ids);
    g_free(name);
}

static void on_folder_clicked(GtkWidget *btn, gpointer data) {
    Bar *bar = data;
    int slot = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "slot"));
    folder_card_open(bar, slot);
}

typedef struct {
    Bar *bar;
    int slot;
} FolderMenuCtx;

static void folder_menu_open_cb(GtkMenuItem *item, gpointer data) {
    (void)item;
    FolderMenuCtx *c = data;
    folder_card_open(c->bar, c->slot);
}

// spill the folder's apps into free slots, then drop the folder itself
static void folder_menu_disband_cb(GtkMenuItem *item, gpointer data) {
    (void)item;
    FolderMenuCtx *c = data;
    const char *val =
        g_hash_table_lookup(grid_map, GINT_TO_POINTER(c->slot));
    if (!is_folder(val))
        return;
    char *name;
    char **ids = folder_parse(val, &name);
    g_hash_table_remove(grid_map, GINT_TO_POINTER(c->slot));
    for (int i = 0; ids[i]; i++) {
        if (!*ids[i])
            continue;
        for (int s = 0; s < GRID_SLOTS; s++) {
            if (!g_hash_table_lookup(grid_map, GINT_TO_POINTER(s))) {
                g_hash_table_insert(grid_map, GINT_TO_POINTER(s),
                                    g_strdup(ids[i]));
                break;
            }
        }
    }
    g_strfreev(ids);
    g_free(name);
    grid_save();
    grid_rebuild_all();
}

static void folder_menu_popup(Bar *bar, int slot,
                              GdkEventButton *ev) {
    GtkWidget *menu = gtk_menu_new();
    FolderMenuCtx base = {bar, slot};

    GtkWidget *item = gtk_menu_item_new_with_label("Open");
    g_signal_connect_data(item, "activate", G_CALLBACK(folder_menu_open_cb),
                          g_memdup2(&base, sizeof base), str_free_notify, 0);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                          gtk_separator_menu_item_new());

    item = gtk_menu_item_new_with_label("Disband folder");
    g_signal_connect_data(item, "activate",
                          G_CALLBACK(folder_menu_disband_cb),
                          g_memdup2(&base, sizeof base), str_free_notify, 0);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    item = gtk_menu_item_new_with_label("Unpin");
    g_signal_connect(item, "activate", G_CALLBACK(menu_unpin_cb),
                     GINT_TO_POINTER(slot));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    menu_append_divider_items(menu, slot);

    g_signal_connect(menu, "deactivate", G_CALLBACK(on_menu_deactivate),
                     NULL);
    ctx_menu_open = TRUE;
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
}

static gboolean on_folder_press(GtkWidget *w, GdkEventButton *ev,
                                gpointer data) {
    Bar *bar = data;
    if (ev->button != 3)
        return FALSE;
    int slot = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "slot"));
    folder_menu_popup(bar, slot, ev);
    return TRUE;
}

// ---- pinned grid ----

static void grid_rebuild(Bar *bar) {
    if (!bar->launcher_grid)
        return;
    GList *kids = gtk_container_get_children(GTK_CONTAINER(bar->launcher_grid));
    for (GList *l = kids; l; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(kids);

    GHashTable *apps = apps_by_id();
    int extra = 0; // grid rows taken up by dividers so far
    for (int slot = 0; slot < GRID_SLOTS; slot++) {
        int row = slot / GRID_COLS, col = slot % GRID_COLS;
        if (col == 0 &&
            g_hash_table_contains(divider_map, GINT_TO_POINTER(row))) {
            gtk_grid_attach(GTK_GRID(bar->launcher_grid), divider_new(row),
                            0, row + extra, GRID_COLS, 1);
            extra++;
        }
        const char *id = g_hash_table_lookup(grid_map, GINT_TO_POINTER(slot));
        GAppInfo *info =
            (id && !is_folder(id)) ? g_hash_table_lookup(apps, id) : NULL;

        GtkWidget *btn = gtk_button_new();
        gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);
        gtk_widget_set_size_request(btn, CELL_W, CELL_H);
        g_object_set_data(G_OBJECT(btn), "slot", GINT_TO_POINTER(slot));

        if (id && is_folder(id)) {
            gtk_widget_set_name(btn, "app-cell");
            gtk_container_add(GTK_CONTAINER(btn),
                              folder_cell_content(id, apps));
            g_signal_connect(btn, "clicked", G_CALLBACK(on_folder_clicked),
                             bar);
            char *payload = g_strdup_printf("slot:%d", slot);
            g_object_set_data_full(G_OBJECT(btn), "dnd-payload", payload,
                                   g_free);
            gtk_drag_source_set(btn, GDK_BUTTON1_MASK, &dnd_target, 1,
                                GDK_ACTION_MOVE);
            gtk_drag_source_set_icon_name(btn, "folder");
            g_signal_connect(btn, "drag-data-get", G_CALLBACK(on_drag_get),
                             NULL);
            g_signal_connect(btn, "drag-begin", G_CALLBACK(on_drag_begin),
                             NULL);
            g_signal_connect(btn, "drag-end", G_CALLBACK(on_drag_end),
                             NULL);
            g_signal_connect(btn, "button-press-event",
                             G_CALLBACK(on_folder_press), bar);
        } else if (info) {
            gtk_widget_set_name(btn, "app-cell");
            gtk_container_add(GTK_CONTAINER(btn), app_cell_content(info));
            LaunchCtx *c = g_new0(LaunchCtx, 1);
            c->bar = bar;
            c->info = g_object_ref(info);
            g_signal_connect_data(btn, "clicked",
                                  G_CALLBACK(on_app_clicked), c,
                                  launch_ctx_free, 0);
            // drag to rearrange
            char *payload = g_strdup_printf("slot:%d", slot);
            g_object_set_data_full(G_OBJECT(btn), "dnd-payload", payload,
                                   g_free);
            gtk_drag_source_set(btn, GDK_BUTTON1_MASK, &dnd_target, 1,
                                GDK_ACTION_MOVE);
            GIcon *gicon = g_app_info_get_icon(info);
            if (gicon)
                gtk_drag_source_set_icon_gicon(btn, gicon);
            g_signal_connect(btn, "drag-data-get", G_CALLBACK(on_drag_get),
                             NULL);
            g_signal_connect(btn, "drag-begin", G_CALLBACK(on_drag_begin),
                             NULL);
            g_signal_connect(btn, "drag-end", G_CALLBACK(on_drag_end),
                             NULL);
            g_signal_connect(btn, "button-press-event",
                             G_CALLBACK(on_slot_press), c);
        } else {
            gtk_widget_set_name(btn, "slot-empty");
            g_signal_connect(btn, "button-press-event",
                             G_CALLBACK(on_empty_press), NULL);
        }
        // every slot accepts drops; highlight is ours, not GTK's box
        gtk_drag_dest_set(btn, GTK_DEST_DEFAULT_DROP, &dnd_target, 1,
                          GDK_ACTION_COPY | GDK_ACTION_MOVE);
        g_signal_connect(btn, "drag-motion", G_CALLBACK(on_slot_motion),
                         NULL);
        g_signal_connect(btn, "drag-leave", G_CALLBACK(on_slot_leave),
                         NULL);
        g_signal_connect(btn, "drag-data-received",
                         G_CALLBACK(on_slot_drop), NULL);

        gtk_grid_attach(GTK_GRID(bar->launcher_grid), btn, col, row + extra,
                        1, 1);
    }
    g_hash_table_destroy(apps);
    gtk_widget_show_all(bar->launcher_grid);
}

static void grid_rebuild_all(void) {
    for (guint i = 0; i < bars->len; i++)
        grid_rebuild(g_ptr_array_index(bars, i));
}

// ---- drawer (all apps + search) ----

static void on_search_changed(GtkSearchEntry *entry, gpointer data) {
    Bar *bar = data;
    (void)entry;
    gtk_flow_box_invalidate_filter(GTK_FLOW_BOX(bar->launcher_flow));
}

static gboolean flow_filter(GtkFlowBoxChild *child, gpointer data) {
    Bar *bar = data;
    const char *needle = gtk_entry_get_text(GTK_ENTRY(bar->launcher_search));
    if (!needle || !*needle)
        return TRUE;
    const char *hay = g_object_get_data(G_OBJECT(child), "haystack");
    char *lc = g_utf8_strdown(needle, -1);
    gboolean hit = hay && strstr(hay, lc) != NULL;
    g_free(lc);
    return hit;
}

static void on_search_activate(GtkEntry *entry, gpointer data) {
    (void)entry;
    Bar *bar = data;
    GList *kids =
        gtk_container_get_children(GTK_CONTAINER(bar->launcher_flow));
    for (GList *l = kids; l; l = l->next) {
        GtkFlowBoxChild *child = l->data;
        if (flow_filter(child, bar)) {
            GtkWidget *btn = gtk_bin_get_child(GTK_BIN(child));
            gtk_button_clicked(GTK_BUTTON(btn));
            break;
        }
    }
    g_list_free(kids);
}

static void drawer_set_open(Bar *bar, gboolean open) {
    gtk_revealer_set_reveal_child(GTK_REVEALER(bar->launcher_drawer), open);
    if (open) {
        gtk_entry_set_text(GTK_ENTRY(bar->launcher_search), "");
        gtk_widget_grab_focus(bar->launcher_search);
    }
}

static gboolean on_handle_motion(GtkWidget *w, GdkDragContext *ctx,
                                 gint x, gint y, guint time,
                                 gpointer data) {
    (void)x;
    (void)y;
    (void)data;
    gtk_style_context_add_class(gtk_widget_get_style_context(w),
                                "drop-remove");
    gdk_drag_status(ctx, GDK_ACTION_MOVE, time);
    return TRUE;
}

static void on_handle_leave(GtkWidget *w, GdkDragContext *ctx, guint time,
                            gpointer data) {
    (void)ctx;
    (void)time;
    (void)data;
    gtk_style_context_remove_class(gtk_widget_get_style_context(w),
                                   "drop-remove");
}

static void on_handle_drop(GtkWidget *w, GdkDragContext *ctx, gint x,
                           gint y, GtkSelectionData *sel, guint info,
                           guint time, gpointer data) {
    (void)x;
    (void)y;
    (void)info;
    (void)data;
    gtk_style_context_remove_class(gtk_widget_get_style_context(w),
                                   "drop-remove");
    const guchar *raw = gtk_selection_data_get_data(sel);
    int len = gtk_selection_data_get_length(sel);
    gboolean ok = FALSE;
    if (raw && len > 4) {
        char *payload = g_strndup((const char *)raw, len);
        if (g_str_has_prefix(payload, "slot:")) { // unpin
            ok = g_hash_table_remove(grid_map,
                                     GINT_TO_POINTER(atoi(payload + 5)));
        } else if (g_str_has_prefix(payload, "fold:")) {
            char *sep = strchr(payload + 5, '|'); // out of its folder
            if (sep) {
                folder_slot_remove(atoi(payload + 5), sep + 1);
                ok = TRUE;
            }
        } else if (g_str_has_prefix(payload, "div:")) { // delete divider
            ok = g_hash_table_remove(divider_map,
                                     GINT_TO_POINTER(atoi(payload + 4)));
        }
        g_free(payload);
    }
    gtk_drag_finish(ctx, ok, FALSE, time);
    on_drag_end(NULL, NULL, NULL);
    if (ok) {
        grid_save();
        grid_rebuild_all();
    }
}

static void on_handle_clicked(GtkWidget *btn, gpointer data) {
    (void)btn;
    Bar *bar = data;
    drawer_set_open(bar, !gtk_revealer_get_reveal_child(
                             GTK_REVEALER(bar->launcher_drawer)));
}

// clicking the empty area beside the panel closes it
static gboolean launcher_click_off(Bar *bar) {
    if (drag_settling())
        return TRUE;
    if (bar->folder_card) { // first click off an open folder just closes it
        folder_card_close(bar);
        return TRUE;
    }
    last_autoclose_us = g_get_monotonic_time();
    launcher_hide(bar);
    return TRUE;
}

// close when the surface loses keyboard focus (click on a window, the
// wallpaper, another monitor…) — unless one of our own menus took it
static gboolean on_focus_out(GtkWidget *w, GdkEventFocus *ev,
                             gpointer data) {
    (void)w;
    (void)ev;
    Bar *bar = data;
    if (!ctx_menu_open && !drag_settling() && launcher_visible(bar)) {
        last_autoclose_us = g_get_monotonic_time();
        launcher_hide(bar);
    }
    return FALSE;
}

// backstop for keyboard focus changes (alt-tab etc.), driven from hypr.c
void launcher_autoclose(void) {
    if (ctx_menu_open || drag_settling())
        return;
    for (guint i = 0; i < bars->len; i++) {
        Bar *bar = g_ptr_array_index(bars, i);
        if (launcher_visible(bar)) {
            last_autoclose_us = g_get_monotonic_time();
            launcher_hide(bar);
        }
    }
}

static gboolean on_key(GtkWidget *w, GdkEventKey *ev, gpointer data) {
    (void)w;
    Bar *bar = data;
    if (ev->keyval == GDK_KEY_Escape) {
        if (bar->folder_card)
            folder_card_close(bar);
        else if (gtk_revealer_get_reveal_child(
                     GTK_REVEALER(bar->launcher_drawer)))
            drawer_set_open(bar, FALSE);
        else
            launcher_hide(bar);
        return TRUE;
    }
    return FALSE;
}

typedef struct {
    GAppInfo *info;
    char *haystack;
} AppEntry;

static void app_entry_free(gpointer p) {
    AppEntry *e = p;
    g_object_unref(e->info);
    g_free(e->haystack);
    g_free(e);
}

static gint app_entry_cmp(gconstpointer a, gconstpointer b) {
    const AppEntry *ea = *(AppEntry *const *)a;
    const AppEntry *eb = *(AppEntry *const *)b;
    return g_utf8_collate(g_app_info_get_display_name(ea->info),
                          g_app_info_get_display_name(eb->info));
}

static void drawer_populate(Bar *bar) {
    GList *kids =
        gtk_container_get_children(GTK_CONTAINER(bar->launcher_flow));
    for (GList *l = kids; l; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(kids);

    GList *apps = g_app_info_get_all();
    GPtrArray *entries = g_ptr_array_new_with_free_func(app_entry_free);
    for (GList *l = apps; l; l = l->next) {
        GAppInfo *info = l->data;
        if (!g_app_info_should_show(info)) {
            g_object_unref(info);
            continue;
        }
        AppEntry *e = g_new0(AppEntry, 1);
        e->info = info;
        const char *name = g_app_info_get_display_name(info);
        const char *desc = g_app_info_get_description(info);
        char *mix =
            g_strdup_printf("%s %s", name ? name : "", desc ? desc : "");
        e->haystack = g_utf8_strdown(mix, -1);
        g_free(mix);
        g_ptr_array_add(entries, e);
    }
    g_list_free(apps);
    g_ptr_array_sort(entries, app_entry_cmp);

    for (guint i = 0; i < entries->len; i++) {
        AppEntry *e = g_ptr_array_index(entries, i);
        GtkWidget *btn = gtk_button_new();
        gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);
        gtk_widget_set_name(btn, "app-cell");
        gtk_container_add(GTK_CONTAINER(btn), app_cell_content(e->info));

        LaunchCtx *c = g_new0(LaunchCtx, 1);
        c->bar = bar;
        c->info = g_object_ref(e->info);
        g_signal_connect_data(btn, "clicked", G_CALLBACK(on_app_clicked), c,
                              launch_ctx_free, 0);
        g_signal_connect(btn, "button-press-event",
                         G_CALLBACK(on_drawer_press), c);

        // drag out of the drawer to pin onto the grid
        const char *id = g_app_info_get_id(e->info);
        if (id) {
            char *payload = g_strdup_printf("app:%s", id);
            g_object_set_data_full(G_OBJECT(btn), "dnd-payload", payload,
                                   g_free);
            gtk_drag_source_set(btn, GDK_BUTTON1_MASK, &dnd_target, 1,
                                GDK_ACTION_COPY);
            GIcon *gicon = g_app_info_get_icon(e->info);
            if (gicon)
                gtk_drag_source_set_icon_gicon(btn, gicon);
            g_signal_connect(btn, "drag-data-get", G_CALLBACK(on_drag_get),
                             NULL);
            g_signal_connect(btn, "drag-begin", G_CALLBACK(on_drag_begin),
                             NULL);
            g_signal_connect(btn, "drag-end", G_CALLBACK(on_drag_end),
                             NULL);
        }

        gtk_flow_box_insert(GTK_FLOW_BOX(bar->launcher_flow), btn, -1);
        GtkWidget *child = gtk_widget_get_parent(btn);
        g_object_set_data_full(G_OBJECT(child), "haystack",
                               g_strdup(e->haystack), g_free);
    }
    g_ptr_array_free(entries, TRUE);
    gtk_widget_show_all(bar->launcher_flow);
}

// ---- panel ----

// the bevel wedges beyond the panel's right border are tinted HERE, on the
// same surface as the panel glass, so hyprland's blur treats them
// identically and the frost matches exactly
static gboolean launcher_draw_bg(GtkWidget *w, cairo_t *cr, gpointer data) {
    (void)data;
    double h = gtk_widget_get_allocated_height(w);
    double x = NEKO_LAUNCH_W;
    double r = NEKO_FRAME_R;
    cairo_set_source_rgba(cr, 0x11 / 255.0, 0x11 / 255.0, 0x1B / 255.0,
                          0.45);
    cairo_move_to(cr, x, 0);
    cairo_arc(cr, x + r, r, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
    cairo_fill(cr);
    cairo_move_to(cr, x, h);
    cairo_arc_negative(cr, x + r, h - r, r, G_PI, G_PI / 2);
    cairo_close_path(cr);
    cairo_fill(cr);
    return FALSE; // children (panel, catcher) draw as usual
}

void launcher_attach(Bar *bar) {
    grid_load();

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    bar->launcher = win;
    gtk_widget_set_name(win, "launcher");

    gtk_layer_init_for_window(GTK_WINDOW(win));
    gtk_layer_set_layer(GTK_WINDOW(win), GTK_LAYER_SHELL_LAYER_TOP);
    gtk_layer_set_namespace(GTK_WINDOW(win), "nekobar-launcher");
    gtk_layer_set_monitor(GTK_WINDOW(win), bar->gdk_monitor);
    // span the whole monitor: the panel sits left, the rest is a
    // transparent click-catcher so clicking anywhere off closes it
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    // tuck the glass inside the shell's top/bottom borders
    gtk_layer_set_margin(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_TOP,
                         NEKO_FRAME_W);
    gtk_layer_set_margin(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_BOTTOM,
                         NEKO_FRAME_W);
    gtk_layer_set_keyboard_mode(GTK_WINDOW(win),
                                GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);

    GdkScreen *screen = gtk_widget_get_screen(win);
    GdkVisual *rgba = gdk_screen_get_rgba_visual(screen);
    if (rgba)
        gtk_widget_set_visual(win, rgba);
    gtk_widget_set_app_paintable(win, TRUE);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_container_add(GTK_CONTAINER(win), root);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_name(box, "launcher-box");
    gtk_widget_set_size_request(box, NEKO_LAUNCH_W, -1);
    gtk_box_pack_start(GTK_BOX(root), box, FALSE, FALSE, 0);

    // transparent input-only area covering the rest of the screen
    GtkWidget *catcher = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(catcher), FALSE);
    gtk_widget_set_hexpand(catcher, TRUE);
    g_signal_connect_swapped(catcher, "button-press-event",
                             G_CALLBACK(launcher_click_off), bar);
    gtk_box_pack_start(GTK_BOX(root), catcher, TRUE, TRUE, 0);

    GtkWidget *overlay = gtk_overlay_new();
    gtk_widget_set_vexpand(overlay, TRUE);
    gtk_box_pack_start(GTK_BOX(box), overlay, TRUE, TRUE, 0);
    bar->launcher_overlay = overlay;

    // base: pinned grid + drawer handle at the bottom
    GtkWidget *base = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *gscroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(gscroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(gscroll, TRUE);
    bar->launcher_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(bar->launcher_grid), 2);
    gtk_grid_set_column_spacing(GTK_GRID(bar->launcher_grid), 2);
    gtk_widget_set_halign(bar->launcher_grid, GTK_ALIGN_CENTER);
    gtk_container_add(GTK_CONTAINER(gscroll), bar->launcher_grid);
    gtk_box_pack_start(GTK_BOX(base), gscroll, TRUE, TRUE, 0);

    GtkWidget *handle = gtk_button_new_with_label("\U000F003B  All apps");
    gtk_button_set_relief(GTK_BUTTON(handle), GTK_RELIEF_NONE);
    gtk_widget_set_name(handle, "drawer-handle");
    g_signal_connect(handle, "clicked", G_CALLBACK(on_handle_clicked), bar);
    // dropping a pinned app on the handle unpins it
    gtk_drag_dest_set(handle, GTK_DEST_DEFAULT_DROP, &dnd_target, 1,
                      GDK_ACTION_MOVE);
    g_signal_connect(handle, "drag-motion", G_CALLBACK(on_handle_motion),
                     NULL);
    g_signal_connect(handle, "drag-leave", G_CALLBACK(on_handle_leave),
                     NULL);
    g_signal_connect(handle, "drag-data-received",
                     G_CALLBACK(on_handle_drop), NULL);
    gtk_box_pack_end(GTK_BOX(base), handle, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(overlay), base);

    // drawer: slides up from the bottom over the grid
    bar->launcher_drawer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(bar->launcher_drawer),
                                     GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
    gtk_revealer_set_transition_duration(GTK_REVEALER(bar->launcher_drawer),
                                         240);
    gtk_widget_set_halign(bar->launcher_drawer, GTK_ALIGN_FILL);
    gtk_widget_set_valign(bar->launcher_drawer, GTK_ALIGN_END);
    GtkWidget *drawer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_name(drawer, "drawer-box");
    gtk_widget_set_size_request(drawer, -1, DRAWER_H);
    gtk_widget_set_margin_start(drawer, 4);
    gtk_widget_set_margin_end(drawer, 4);
    gtk_widget_set_margin_bottom(drawer, 8);

    bar->launcher_search = gtk_search_entry_new();
    gtk_widget_set_name(bar->launcher_search, "launcher-search");
    g_signal_connect(bar->launcher_search, "search-changed",
                     G_CALLBACK(on_search_changed), bar);
    g_signal_connect(bar->launcher_search, "activate",
                     G_CALLBACK(on_search_activate), bar);
    gtk_box_pack_start(GTK_BOX(drawer), bar->launcher_search, FALSE, FALSE,
                       0);

    GtkWidget *fscroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(fscroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(fscroll, TRUE);
    bar->launcher_flow = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(bar->launcher_flow),
                                    GTK_SELECTION_NONE);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(bar->launcher_flow),
                                           GRID_COLS);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(bar->launcher_flow),
                                           GRID_COLS);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(bar->launcher_flow), TRUE);
    gtk_flow_box_set_filter_func(GTK_FLOW_BOX(bar->launcher_flow),
                                 flow_filter, bar, NULL);
    gtk_container_add(GTK_CONTAINER(fscroll), bar->launcher_flow);
    gtk_box_pack_start(GTK_BOX(drawer), fscroll, TRUE, TRUE, 0);

    gtk_container_add(GTK_CONTAINER(bar->launcher_drawer), drawer);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), bar->launcher_drawer);

    g_signal_connect(win, "key-press-event", G_CALLBACK(on_key), bar);
    g_signal_connect(win, "focus-out-event", G_CALLBACK(on_focus_out), bar);
    g_signal_connect(win, "draw", G_CALLBACK(launcher_draw_bg), NULL);
}

static gboolean drawer_test_open(gpointer data) {
    drawer_set_open(data, TRUE);
    return G_SOURCE_REMOVE;
}

void launcher_toggle(Bar *bar) {
    if (!bar->launcher)
        return;
    if (launcher_visible(bar)) {
        launcher_hide(bar);
        return;
    }
    // the click that just auto-closed it shouldn't immediately reopen it
    if (g_get_monotonic_time() - last_autoclose_us < 400000)
        return;
    // exclusive zone offsets past the sidebar; add the shell's lip so
    // the glass tint spans exactly to its right border
    gtk_layer_set_margin(GTK_WINDOW(bar->launcher),
                         GTK_LAYER_SHELL_EDGE_LEFT, NEKO_FRAME_W);
    grid_rebuild(bar);
    drawer_populate(bar);
    gtk_widget_set_opacity(bar->launcher, 0.0);
    gtk_widget_show_all(bar->launcher);
    drawer_set_open(bar, FALSE);
    launch_animate(bar, 1);
    if (g_getenv("NEKOBAR_DRAWER_TEST")) // headless testing hook
        g_timeout_add(900, (GSourceFunc)drawer_test_open, bar);
}

// toggle on the focused monitor (e.g. `pkill -USR2 nekobar` from a keybind)
void launcher_toggle_focused(void) {
    char name[64] = "";
    Bar *target = bars->len ? g_ptr_array_index(bars, 0) : NULL;
    if (hypr_focused_monitor(name, sizeof(name))) {
        for (guint i = 0; i < bars->len; i++) {
            Bar *bar = g_ptr_array_index(bars, i);
            if (g_str_equal(bar->hypr_name, name)) {
                target = bar;
                break;
            }
        }
    }
    if (target)
        launcher_toggle(target);
}
