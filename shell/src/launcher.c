// launcher.c — built-in app launcher: a thin app grid on its own
// layer-shell surface that slides out from the left sidebar (the slide is
// hyprland's `animation slide left` layerrule on the namespace).

#include "nekobar.h"

#include <gdk/gdkkeysyms.h>
#include <gtk-layer-shell/gtk-layer-shell.h>
#include <string.h>

#define GRID_COLS 4
#define PANEL_W NEKO_LAUNCH_W

typedef struct {
    GAppInfo *info;
    char *haystack; // lowercase name + keywords for filtering
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

static gboolean launcher_visible(Bar *bar) {
    return bar->launch_target == 1;
}

// chrome morph: the frame's cutout slides as launch_ext approaches the
// target; the panel's contents fade in on top of the growing slab
static gboolean launch_tick(GtkWidget *w, GdkFrameClock *clock,
                            gpointer data) {
    (void)w;
    Bar *bar = data;
    gint64 now = gdk_frame_clock_get_frame_time(clock);
    double dt = CLAMP((now - bar->launch_last_us) / 1e6, 0.0, 0.05);
    bar->launch_last_us = now;

    double target = bar->launch_target;
    double diff = target - bar->launch_ext;
    bar->launch_ext += diff * MIN(1.0, 14.0 * dt); // smooth exponential
    if (ABS(target - bar->launch_ext) < 0.004)
        bar->launch_ext = target;

    gtk_widget_queue_draw(bar->frame);
    if (bar->launcher) // contents trail the slab slightly
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

static void launcher_hide(Bar *bar) {
    if (launcher_visible(bar))
        launch_animate(bar, 0);
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

// ---- filtering ----

static void on_search_changed(GtkSearchEntry *entry, gpointer data) {
    Bar *bar = data;
    (void)entry;
    gtk_flow_box_invalidate_filter(GTK_FLOW_BOX(bar->launcher_flow));
}

static gboolean flow_filter(GtkFlowBoxChild *child, gpointer data) {
    Bar *bar = data;
    const char *needle =
        gtk_entry_get_text(GTK_ENTRY(bar->launcher_search));
    if (!needle || !*needle)
        return TRUE;
    const char *hay = g_object_get_data(G_OBJECT(child), "haystack");
    char *lc = g_utf8_strdown(needle, -1);
    gboolean hit = hay && strstr(hay, lc) != NULL;
    g_free(lc);
    return hit;
}

// Enter in the search box: launch the first visible app
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

static gboolean on_key(GtkWidget *w, GdkEventKey *ev, gpointer data) {
    (void)w;
    Bar *bar = data;
    if (ev->keyval == GDK_KEY_Escape) {
        launcher_hide(bar);
        return TRUE;
    }
    return FALSE;
}

// ---- population ----

static void launcher_populate(Bar *bar) {
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
        char *mix = g_strdup_printf("%s %s", name ? name : "",
                                    desc ? desc : "");
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

        GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
        GIcon *gicon = g_app_info_get_icon(e->info);
        GtkWidget *img =
            gicon ? gtk_image_new_from_gicon(gicon, GTK_ICON_SIZE_DND)
                  : gtk_image_new_from_icon_name(
                        "application-x-executable", GTK_ICON_SIZE_DND);
        gtk_image_set_pixel_size(GTK_IMAGE(img), 34);
        gtk_box_pack_start(GTK_BOX(v), img, FALSE, FALSE, 0);
        GtkWidget *lbl =
            gtk_label_new(g_app_info_get_display_name(e->info));
        gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(lbl), 9);
        gtk_label_set_justify(GTK_LABEL(lbl), GTK_JUSTIFY_CENTER);
        gtk_widget_set_name(lbl, "app-label");
        gtk_box_pack_start(GTK_BOX(v), lbl, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(btn), v);

        LaunchCtx *c = g_new0(LaunchCtx, 1);
        c->bar = bar;
        c->info = g_object_ref(e->info);
        g_signal_connect_data(btn, "clicked", G_CALLBACK(on_app_clicked), c,
                              launch_ctx_free, 0);

        gtk_flow_box_insert(GTK_FLOW_BOX(bar->launcher_flow), btn, -1);
        GtkWidget *child = gtk_widget_get_parent(btn);
        g_object_set_data_full(G_OBJECT(child), "haystack",
                               g_strdup(e->haystack), g_free);
    }
    g_ptr_array_free(entries, TRUE);
    gtk_widget_show_all(bar->launcher_flow);
}

// ---- panel ----

void launcher_attach(Bar *bar) {
    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    bar->launcher = win;
    gtk_widget_set_name(win, "launcher");

    gtk_layer_init_for_window(GTK_WINDOW(win));
    gtk_layer_set_layer(GTK_WINDOW(win), GTK_LAYER_SHELL_LAYER_TOP);
    gtk_layer_set_namespace(GTK_WINDOW(win), "nekobar-launcher");
    gtk_layer_set_monitor(GTK_WINDOW(win), bar->gdk_monitor);
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_keyboard_mode(GTK_WINDOW(win),
                                GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);

    GdkScreen *screen = gtk_widget_get_screen(win);
    GdkVisual *rgba = gdk_screen_get_rgba_visual(screen);
    if (rgba)
        gtk_widget_set_visual(win, rgba);
    gtk_widget_set_app_paintable(win, TRUE);

    GtkWidget *frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_name(frame, "launcher-box");
    gtk_widget_set_size_request(frame, PANEL_W, -1);
    gtk_container_add(GTK_CONTAINER(win), frame);

    bar->launcher_search = gtk_search_entry_new();
    gtk_widget_set_name(bar->launcher_search, "launcher-search");
    g_signal_connect(bar->launcher_search, "search-changed",
                     G_CALLBACK(on_search_changed), bar);
    g_signal_connect(bar->launcher_search, "activate",
                     G_CALLBACK(on_search_activate), bar);
    gtk_box_pack_start(GTK_BOX(frame), bar->launcher_search, FALSE, FALSE,
                       0);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);

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
    gtk_container_add(GTK_CONTAINER(scroll), bar->launcher_flow);
    gtk_box_pack_start(GTK_BOX(frame), scroll, TRUE, TRUE, 0);

    g_signal_connect(win, "key-press-event", G_CALLBACK(on_key), bar);
}

void launcher_toggle(Bar *bar) {
    if (!bar->launcher)
        return;
    if (launcher_visible(bar)) {
        launcher_hide(bar);
        return;
    }
    // no left margin needed: the sidebar's exclusive zone already offsets
    // left-anchored surfaces, so margin 0 lands flush against the bar
    gtk_layer_set_margin(GTK_WINDOW(bar->launcher),
                         GTK_LAYER_SHELL_EDGE_LEFT, 0);
    launcher_populate(bar);
    gtk_entry_set_text(GTK_ENTRY(bar->launcher_search), "");
    gtk_widget_set_opacity(bar->launcher, 0.0); // fades in with the morph
    gtk_widget_show_all(bar->launcher);
    gtk_widget_grab_focus(bar->launcher_search);
    launch_animate(bar, 1);
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
