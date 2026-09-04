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

// ---- shell chrome ----
// A thin border slab per monitor, continuous with the sidebar strip: the
// hole is cut FRAME_W from the screen edges (and at the sidebar's right
// edge), leaving the rest of hyprland's gaps_out as visible margin between
// the shell and the windows. The sidebar draws its content transparently
// on top of this slab, so the two are one connected piece.

#define FRAME_W ((double)NEKO_FRAME_W)
#define FRAME_R ((double)NEKO_FRAME_R)

// separate left/right corner radii: the hole's left corners straighten
// while the launcher glass is open so the seam has no bevel wedges
static void rounded_path_lr(cairo_t *cr, double x, double y, double w,
                            double h, double rl, double rr) {
    rl = MAX(rl, 0.5);
    rr = MAX(rr, 0.5);
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - rr, y + rr, rr, -G_PI / 2, 0);
    cairo_arc(cr, x + w - rr, y + h - rr, rr, 0, G_PI / 2);
    cairo_arc(cr, x + rl, y + h - rl, rl, G_PI / 2, G_PI);
    cairo_arc(cr, x + rl, y + rl, rl, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
}


static gboolean frame_draw_cb(GtkWidget *w, cairo_t *cr, gpointer data) {
    Bar *bar = data;
    double width = gtk_widget_get_allocated_width(w);
    double height = gtk_widget_get_allocated_height(w);
    // the hole starts a FRAME_W lip after the sidebar's right edge, so
    // the visible margin to the windows matches the other borders;
    // elsewhere the border is a thin FRAME_W strip along the screen edge
    double bar_w = 44;
    if (bar->window && gtk_widget_get_realized(GTK_WIDGET(bar->window)))
        bar_w = gtk_widget_get_allocated_width(GTK_WIDGET(bar->window));
    double lip = bar_w + FRAME_W;
    // the launcher morphs the chrome open: the hole's left edge slides
    // right as the shell grows out of the sidebar
    double hx = lip + bar->launch_ext * NEKO_LAUNCH_W;
    double hy = FRAME_W;
    double hw = width - hx - FRAME_W;
    double hh = height - 2 * FRAME_W;

    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    // the hole keeps its rounded corners on all sides — the launcher's
    // right border shares the same bevel language as the rest of the shell
    double hole_rl = FRAME_R;
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_rectangle(cr, 0, 0, width, height);
    rounded_path_lr(cr, hx, hy, hw, hh, hole_rl, FRAME_R);
    cairo_set_source_rgb(cr, 0x11 / 255.0, 0x11 / 255.0, 0x1B / 255.0);
    cairo_fill(cr);
    // glassy launcher panel: the slab opens up behind the whole panel so
    // the compositor blur shows the desktop through the grid, with a
    // short horizontal gradient melting the opaque sidebar into glass
    if (bar->launch_ext > 0.001) {
        double gx = lip;
        double gw = bar->launch_ext * NEKO_LAUNCH_W;
        cairo_save(cr);
        cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
        cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
        // extend past the border into the hole's corner radius so the
        // bevel wedges are glass too, not solid chrome (the rim and
        // shadow are stroked back on top afterwards)
        cairo_rectangle(cr, gx, FRAME_W, gw + FRAME_R,
                        height - 2 * FRAME_W);
        cairo_fill(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
        cairo_pattern_t *grad =
            cairo_pattern_create_linear(gx, 0, gx + 70, 0);
        cairo_pattern_add_color_stop_rgba(grad, 0, 0x11 / 255.0,
                                          0x11 / 255.0, 0x1B / 255.0, 1.0);
        cairo_pattern_add_color_stop_rgba(grad, 1, 0x11 / 255.0,
                                          0x11 / 255.0, 0x1B / 255.0, 0.0);
        cairo_set_source(cr, grad);
        cairo_rectangle(cr, gx, FRAME_W, MIN(70, gw),
                        height - 2 * FRAME_W);
        cairo_fill(cr);
        cairo_pattern_destroy(grad);
        // tight rim where the glass meets the top/bottom borders
        cairo_set_line_width(cr, 2);
        cairo_set_source_rgb(cr, 0x1E / 255.0, 0x1E / 255.0, 0x2E / 255.0);
        cairo_move_to(cr, gx, FRAME_W);
        cairo_line_to(cr, gx + gw + FRAME_R, FRAME_W);
        cairo_stroke(cr);
        cairo_move_to(cr, gx, height - FRAME_W);
        cairo_line_to(cr, gx + gw + FRAME_R, height - FRAME_W);
        cairo_stroke(cr);
        cairo_restore(cr);
    }
    // rim line around the hole
    rounded_path_lr(cr, hx, hy, hw, hh, hole_rl, FRAME_R);
    cairo_set_line_width(cr, 2);
    cairo_set_source_rgb(cr, 0x1E / 255.0, 0x1E / 255.0, 0x2E / 255.0);
    cairo_stroke(cr);
    // soft inner shadow just inside the hole: the inset depth cue
    for (int i = 1; i <= 4; i++) {
        rounded_path_lr(cr, hx + i, hy + i, hw - 2 * i, hh - 2 * i,
                        MAX(hole_rl - i, 0.5), MAX(FRAME_R - i, 1));
        cairo_set_line_width(cr, 1.2);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.16 - 0.035 * i);
        cairo_stroke(cr);
    }
    return TRUE;
}

static void frame_mapped(GtkWidget *w, gpointer data) {
    (void)data;
    // click-through: empty input region
    cairo_region_t *empty = cairo_region_create();
    gtk_widget_input_shape_combine_region(w, empty);
    cairo_region_destroy(empty);
}

static GtkWidget *frame_new(GdkMonitor *gdk_mon, Bar *bar) {
    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_layer_init_for_window(GTK_WINDOW(win));
    gtk_layer_set_layer(GTK_WINDOW(win), GTK_LAYER_SHELL_LAYER_TOP);
    gtk_layer_set_namespace(GTK_WINDOW(win), "nekobar-frame");
    gtk_layer_set_monitor(GTK_WINDOW(win), gdk_mon);
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_exclusive_zone(GTK_WINDOW(win), -1); // hug the true edges
    GdkScreen *screen = gtk_widget_get_screen(win);
    GdkVisual *rgba = gdk_screen_get_rgba_visual(screen);
    if (rgba)
        gtk_widget_set_visual(win, rgba);
    gtk_widget_set_app_paintable(win, TRUE);
    g_signal_connect(win, "draw", G_CALLBACK(frame_draw_cb), bar);
    g_signal_connect(win, "map", G_CALLBACK(frame_mapped), NULL);
    gtk_widget_show_all(win);
    return win;
}

static gboolean launcher_btn_pressed(GtkWidget *w, GdkEventButton *ev,
                                     gpointer data) {
    (void)w;
    if (ev->button == 1)
        launcher_toggle(data);
    return TRUE;
}

static Bar *bar_new(GdkMonitor *gdk_mon) {
    Bar *bar = g_new0(Bar, 1);

    GdkRectangle geo;
    gdk_monitor_get_geometry(gdk_mon, &geo);
    if (!hypr_monitor_name_at(geo.x, geo.y, bar->hypr_name,
                              sizeof(bar->hypr_name)))
        g_strlcpy(bar->hypr_name, "?", sizeof(bar->hypr_name));

    bar->frame = frame_new(gdk_mon, bar);

    GtkWindow *win = GTK_WINDOW(gtk_window_new(GTK_WINDOW_TOPLEVEL));
    bar->window = win;
    bar->gdk_monitor = gdk_mon;
    gtk_widget_set_name(GTK_WIDGET(win), "nekobar");

    gtk_layer_init_for_window(win);
    gtk_layer_set_layer(win, GTK_LAYER_SHELL_LAYER_TOP);
    gtk_layer_set_namespace(win, "nekobar");
    gtk_layer_set_monitor(win, gdk_mon);
    // vertical sidebar hugging the left edge (caelestia-style layout)
    gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
    gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_auto_exclusive_zone_enable(win);

    // transparent window background
    GdkScreen *screen = gtk_widget_get_screen(GTK_WIDGET(win));
    GdkVisual *rgba = gdk_screen_get_rgba_visual(screen);
    if (rgba)
        gtk_widget_set_visual(GTK_WIDGET(win), rgba);
    gtk_widget_set_app_paintable(GTK_WIDGET(win), TRUE);

    // vertical rounded pill, caelestia-style sidebar layout
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_name(box, "bar-box");
    gtk_container_add(GTK_CONTAINER(win), box);

    // ---- top: launcher + workspaces + media ----
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(box), left, FALSE, FALSE, 0);

    // built-in launcher (launcher.c) instead of rofi
    GtkWidget *launch_btn = gtk_button_new_with_label("\U000F08C7");
    gtk_button_set_relief(GTK_BUTTON(launch_btn), GTK_RELIEF_NONE);
    gtk_widget_set_name(launch_btn, "rofi"); // keep the existing styling
    g_signal_connect(launch_btn, "button-press-event",
                     G_CALLBACK(launcher_btn_pressed), bar);
    gtk_box_pack_start(GTK_BOX(left), launch_btn, FALSE, FALSE, 0);

    bar->ws_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_name(bar->ws_box, "workspaces");
    gtk_box_pack_start(GTK_BOX(left), bar->ws_box, FALSE, FALSE, 0);

    bar->mpris_event = gtk_button_new_with_label("");
    gtk_button_set_relief(GTK_BUTTON(bar->mpris_event), GTK_RELIEF_NONE);
    bar->mpris_label = gtk_bin_get_child(GTK_BIN(bar->mpris_event));
    gtk_label_set_angle(GTK_LABEL(bar->mpris_label), 270);
    gtk_label_set_ellipsize(GTK_LABEL(bar->mpris_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(bar->mpris_label), 28);
    gtk_widget_set_name(bar->mpris_event, "mpris");
    g_signal_connect(bar->mpris_event, "button-press-event",
                     G_CALLBACK(mpris_pressed), NULL);
    gtk_box_pack_start(GTK_BOX(left), bar->mpris_event, FALSE, FALSE, 0);

    // focused window title reads top-to-bottom (caelestia ActiveWindow)
    bar->title_label = gtk_label_new("");
    gtk_widget_set_name(bar->title_label, "window-title");
    gtk_label_set_angle(GTK_LABEL(bar->title_label), 270);
    gtk_label_set_ellipsize(GTK_LABEL(bar->title_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(bar->title_label), 32);
    gtk_box_set_center_widget(GTK_BOX(box), bar->title_label);

    bar->clock_label = gtk_label_new("");
    gtk_widget_set_name(bar->clock_label, "clock");
    gtk_label_set_justify(GTK_LABEL(bar->clock_label), GTK_JUSTIFY_CENTER);

    // ---- bottom (packed end: first call sits at the very bottom) ----
    gtk_box_pack_end(GTK_BOX(box),
                     icon_button("", "power", "~/.config/rofi/powermenu.sh",
                                 NULL, NULL),
                     FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(box), bar->clock_label, FALSE, FALSE, 0);
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
    launcher_attach(bar);

    bar->tray_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_name(bar->tray_box, "tray");
    gtk_box_pack_end(GTK_BOX(box), bar->tray_box, FALSE, FALSE, 0);

    bar->mem_label = gtk_label_new("");
    gtk_widget_set_name(bar->mem_label, "memory");
    gtk_box_pack_end(GTK_BOX(box), bar->mem_label, FALSE, FALSE, 0);

    g_signal_connect_swapped(GTK_WIDGET(win), "size-allocate",
                             G_CALLBACK(gtk_widget_queue_draw), bar->frame);
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

static gboolean on_sigusr2(gpointer data) {
    (void)data;
    launcher_toggle_focused();
    return TRUE;
}

// ---- monitor hotplug ----
// Bars are per-monitor layer surfaces: when a monitor is destroyed its bar
// must go with it, and a (re)connected monitor needs a fresh bar.

static void bar_free(Bar *bar) {
    if (bar->qs_popover)
        gtk_widget_destroy(bar->qs_popover);
    if (bar->launcher)
        gtk_widget_destroy(bar->launcher);
    if (bar->frame)
        gtk_widget_destroy(bar->frame);
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
    g_unix_signal_add(SIGUSR2, on_sigusr2, NULL);

    gtk_main();
    return 0;
}
