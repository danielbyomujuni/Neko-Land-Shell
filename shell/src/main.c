#include "nekobar.h"

#include <glib-unix.h>
#include <gtk-layer-shell/gtk-layer-shell.h>

GPtrArray *bars;

// ---- clickable icon "buttons" (labels in event boxes, waybar-style) ----

typedef struct {
    const char *left;
    const char *right;
    const char *middle;
} ClickCmds;

static gboolean icon_pressed(GtkWidget *w, GdkEventButton *ev, gpointer data) {
    (void)w;
    ClickCmds *c = data;
    const char *cmd = NULL;
    if (ev->button == 1)
        cmd = c->left;
    else if (ev->button == 3)
        cmd = c->right;
    else if (ev->button == 2)
        cmd = c->middle;
    if (cmd)
        spawn_cmd(cmd);
    return TRUE;
}

static GtkWidget *icon_button(const char *glyph, const char *css_name,
                              const char *left, const char *right,
                              const char *middle) {
    GtkWidget *btn = gtk_button_new_with_label(glyph);
    gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);
    gtk_widget_set_name(btn, css_name);
    ClickCmds *c = g_new0(ClickCmds, 1);
    c->left = left;
    c->right = right;
    c->middle = middle;
    g_signal_connect(btn, "button-press-event", G_CALLBACK(icon_pressed), c);
    return btn;
}

// ---- mpris pill clicks ----

static gboolean mpris_pressed(GtkWidget *w, GdkEventButton *ev, gpointer data) {
    (void)w;
    (void)data;
    if (ev->button == 1)
        spawn_cmd("playerctl play-pause");
    else if (ev->button == 2)
        spawn_cmd("playerctl previous");
    else if (ev->button == 3)
        spawn_cmd("playerctl next");
    return TRUE;
}

// ---- volume scroll ----

static gboolean vol_scrolled(GtkWidget *w, GdkEventScroll *ev, gpointer data) {
    (void)w;
    (void)data;
    if (ev->direction == GDK_SCROLL_UP)
        spawn_cmd("wpctl set-volume -l 1.0 @DEFAULT_AUDIO_SINK@ 5%+");
    else if (ev->direction == GDK_SCROLL_DOWN)
        spawn_cmd("wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%-");
    return TRUE;
}

static gboolean vol_pressed(GtkWidget *w, GdkEventButton *ev, gpointer data) {
    (void)w;
    if (ev->button == 1)
        quickset_toggle(data);
    else if (ev->button == 3)
        spawn_cmd("pavucontrol");
    return TRUE;
}

static Bar *bar_new(GdkMonitor *gdk_mon) {
    Bar *bar = g_new0(Bar, 1);

    GdkRectangle geo;
    gdk_monitor_get_geometry(gdk_mon, &geo);
    if (!hypr_monitor_name_at(geo.x, geo.y, bar->hypr_name,
                              sizeof(bar->hypr_name)))
        g_strlcpy(bar->hypr_name, "?", sizeof(bar->hypr_name));

    GtkWindow *win = GTK_WINDOW(gtk_window_new(GTK_WINDOW_TOPLEVEL));
    bar->window = win;
    bar->gdk_monitor = gdk_mon;
    gtk_widget_set_name(GTK_WIDGET(win), "nekobar");

    gtk_layer_init_for_window(win);
    gtk_layer_set_layer(win, GTK_LAYER_SHELL_LAYER_TOP);
    gtk_layer_set_namespace(win, "nekobar");
    gtk_layer_set_monitor(win, gdk_mon);
    gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
    gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
    // waybar config: margin-left/right 4
    gtk_layer_set_margin(win, GTK_LAYER_SHELL_EDGE_LEFT, 4);
    gtk_layer_set_margin(win, GTK_LAYER_SHELL_EDGE_RIGHT, 4);
    gtk_layer_auto_exclusive_zone_enable(win);

    // transparent window background
    GdkScreen *screen = gtk_widget_get_screen(GTK_WIDGET(win));
    GdkVisual *rgba = gdk_screen_get_rgba_visual(screen);
    if (rgba)
        gtk_widget_set_visual(GTK_WIDGET(win), rgba);
    gtk_widget_set_app_paintable(GTK_WIDGET(win), TRUE);

    // rounded inner box, like waybar's `#waybar.top > box.horizontal`
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_name(box, "bar-box");
    gtk_container_add(GTK_CONTAINER(win), box);

    // ---- left ----
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(box), left, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(left),
                       icon_button("\U000F08C7", "rofi",
                                   "~/.config/rofi/launchers/type-7/launcher.sh",
                                   NULL, "pkill -9 rofi"),
                       FALSE, FALSE, 0);

    bar->ws_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_name(bar->ws_box, "workspaces");
    gtk_box_pack_start(GTK_BOX(left), bar->ws_box, FALSE, FALSE, 0);

    bar->mpris_event = gtk_button_new_with_label("");
    gtk_button_set_relief(GTK_BUTTON(bar->mpris_event), GTK_RELIEF_NONE);
    bar->mpris_label = gtk_bin_get_child(GTK_BIN(bar->mpris_event));
    gtk_widget_set_name(bar->mpris_event, "mpris");
    g_signal_connect(bar->mpris_event, "button-press-event",
                     G_CALLBACK(mpris_pressed), NULL);
    gtk_box_pack_start(GTK_BOX(left), bar->mpris_event, FALSE, FALSE, 0);

    bar->title_label = gtk_label_new("");
    gtk_widget_set_name(bar->title_label, "window-title");
    gtk_label_set_ellipsize(GTK_LABEL(bar->title_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(bar->title_label), 60);
    gtk_box_pack_start(GTK_BOX(left), bar->title_label, FALSE, FALSE, 0);

    // ---- center ----
    bar->clock_label = gtk_label_new("");
    gtk_widget_set_name(bar->clock_label, "clock");
    gtk_box_set_center_widget(GTK_BOX(box), bar->clock_label);

    // ---- right (packed end: reverse order) ----
    gtk_box_pack_end(GTK_BOX(box),
                     icon_button("", "power", "~/.config/rofi/powermenu.sh",
                                 NULL, NULL),
                     FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(box),
                     icon_button("\U000F0E09", "wallpaper", "waypaper",
                                 "waypaper --random", NULL),
                     FALSE, FALSE, 0);
    gtk_box_pack_end(
        GTK_BOX(box),
        icon_button("", "screenshot",
                    "~/.config/waybar/scripts/screenshot_full.sh",
                    "~/.config/waybar/scripts/screenshot_area.sh", NULL),
        FALSE, FALSE, 0);
    gtk_box_pack_end(
        GTK_BOX(box),
        icon_button("", "color-picker",
                    "hyprpicker -an && notify-send 'Colour copied to clipboard'",
                    NULL, NULL),
        FALSE, FALSE, 0);

    GtkWidget *vol_ev = gtk_button_new_with_label("");
    gtk_button_set_relief(GTK_BUTTON(vol_ev), GTK_RELIEF_NONE);
    bar->vol_label = gtk_bin_get_child(GTK_BIN(vol_ev));
    gtk_widget_set_name(vol_ev, "pulseaudio");
    gtk_widget_add_events(vol_ev, GDK_SCROLL_MASK);
    g_signal_connect(vol_ev, "button-press-event", G_CALLBACK(vol_pressed), bar);
    g_signal_connect(vol_ev, "scroll-event", G_CALLBACK(vol_scrolled), NULL);
    gtk_box_pack_end(GTK_BOX(box), vol_ev, FALSE, FALSE, 0);
    quickset_attach(bar, vol_ev);

    bar->tray_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_name(bar->tray_box, "tray");
    gtk_box_pack_end(GTK_BOX(box), bar->tray_box, FALSE, FALSE, 0);

    bar->mem_label = gtk_label_new("");
    gtk_widget_set_name(bar->mem_label, "memory");
    gtk_box_pack_end(GTK_BOX(box), bar->mem_label, FALSE, FALSE, 0);

    gtk_widget_show_all(GTK_WIDGET(win));
    gtk_widget_set_visible(bar->mpris_event, FALSE);
    gtk_widget_set_visible(bar->tray_box, FALSE);
    return bar;
}

static void load_css(void) {
    char *exe = g_file_read_link("/proc/self/exe", NULL);
    char *dir = g_path_get_dirname(exe ? exe : ".");
    char *css = g_build_filename(dir, "style.css", NULL);
    GtkCssProvider *prov = gtk_css_provider_new();
    GError *err = NULL;
    if (!gtk_css_provider_load_from_path(prov, css, &err)) {
        g_warning("css load failed (%s): %s", css, err ? err->message : "?");
        g_clear_error(&err);
    }
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(), GTK_STYLE_PROVIDER(prov),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_free(css);
    g_free(dir);
    g_free(exe);
}

static gboolean on_sigusr1(gpointer data) {
    (void)data;
    quickset_toggle_focused();
    return TRUE;
}

// ---- monitor hotplug ----
// Bars are per-monitor layer surfaces: when a monitor is destroyed its bar
// must go with it, and a (re)connected monitor needs a fresh bar.

static void bar_free(Bar *bar) {
    if (bar->qs_popover)
        gtk_widget_destroy(bar->qs_popover);
    gtk_widget_destroy(GTK_WIDGET(bar->window));
    g_free(bar);
}

// re-resolve each bar's hyprland monitor name (layout may have changed)
void bars_refresh_names(void) {
    for (guint i = 0; i < bars->len; i++) {
        Bar *bar = g_ptr_array_index(bars, i);
        GdkRectangle geo;
        gdk_monitor_get_geometry(bar->gdk_monitor, &geo);
        hypr_monitor_name_at(geo.x, geo.y, bar->hypr_name,
                             sizeof(bar->hypr_name));
    }
}

static gboolean add_bar_delayed(gpointer data) {
    GdkMonitor *mon = data;
    // the monitor may have vanished again during the delay
    GdkDisplay *display = gdk_display_get_default();
    gboolean still_here = FALSE;
    for (int i = 0; i < gdk_display_get_n_monitors(display); i++)
        still_here |= gdk_display_get_monitor(display, i) == mon;
    gboolean have_bar = FALSE;
    for (guint i = 0; i < bars->len; i++)
        have_bar |=
            ((Bar *)g_ptr_array_index(bars, i))->gdk_monitor == mon;
    if (still_here && !have_bar) {
        g_ptr_array_add(bars, bar_new(mon));
        bars_refresh_names();
        hypr_refresh_workspaces();
        hypr_refresh_title();
        tray_refresh();
        volume_refresh();
    }
    g_object_unref(mon);
    return G_SOURCE_REMOVE;
}

static void on_monitor_added(GdkDisplay *display, GdkMonitor *mon,
                             gpointer data) {
    (void)display;
    (void)data;
    // give hyprland a moment to register the monitor (name resolution and
    // the split-workspaces plugin's mapping both need it settled)
    g_timeout_add(700, add_bar_delayed, g_object_ref(mon));
}

static void on_monitor_removed(GdkDisplay *display, GdkMonitor *mon,
                               gpointer data) {
    (void)display;
    (void)data;
    for (guint i = 0; i < bars->len; i++) {
        Bar *bar = g_ptr_array_index(bars, i);
        if (bar->gdk_monitor == mon) {
            g_ptr_array_remove_index(bars, i);
            bar_free(bar);
            break;
        }
    }
    hypr_refresh_workspaces();
}

int main(int argc, char **argv) {
    gtk_init(&argc, &argv);
    load_css();

    bars = g_ptr_array_new();
    GdkDisplay *display = gdk_display_get_default();
    int n = gdk_display_get_n_monitors(display);
    for (int i = 0; i < n; i++) {
        GdkMonitor *mon = gdk_display_get_monitor(display, i);
        g_ptr_array_add(bars, bar_new(mon));
    }

    g_signal_connect(display, "monitor-added",
                     G_CALLBACK(on_monitor_added), NULL);
    g_signal_connect(display, "monitor-removed",
                     G_CALLBACK(on_monitor_removed), NULL);

    hypr_init();
    hypr_refresh_workspaces();
    hypr_refresh_title();
    tray_init();
    modules_start();

    // e.g. `pkill -USR1 nekobar` from a Hyprland keybind
    g_unix_signal_add(SIGUSR1, on_sigusr1, NULL);

    gtk_main();
    return 0;
}
