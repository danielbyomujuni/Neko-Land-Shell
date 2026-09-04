// osd.c — macOS-style volume HUD: a small glass pill that pops up near
// the bottom of the FOCUSED monitor whenever the volume changes (keyboard
// roller, media keys, apps), then fades away. Suppressed while a quickset
// panel is open — the user is already looking at a slider there.

#include "nekobar.h"

#include <gtk-layer-shell/gtk-layer-shell.h>
#include <string.h>

#define OSD_W 240
#define OSD_H 46
#define OSD_HOLD_MS 1200

static GtkWidget *osd_win;
static GtkWidget *osd_icon;
static GtkWidget *osd_area;
static guint hide_id;
static guint fade_id;
static double osd_opacity;
static char osd_mon[64];

// thin trough + fill capsule, mirroring the quickset volume capsule
static gboolean osd_draw(GtkWidget *w, cairo_t *cr, gpointer data) {
    (void)data;
    double W = gtk_widget_get_allocated_width(w);
    double H = gtk_widget_get_allocated_height(w);
    double bh = 6, y = (H - bh) / 2, r = bh / 2;
    // trough
    cairo_new_sub_path(cr);
    cairo_arc(cr, r, y + r, r, G_PI / 2, 3 * G_PI / 2);
    cairo_arc(cr, W - r, y + r, r, 3 * G_PI / 2, G_PI / 2);
    cairo_close_path(cr);
    cairo_set_source_rgb(cr, 0x31 / 255.0, 0x32 / 255.0, 0x44 / 255.0);
    cairo_fill(cr);
    // fill
    double fw = MAX(bh, W * CLAMP(cur_volume, 0.0, 1.0));
    cairo_new_sub_path(cr);
    cairo_arc(cr, r, y + r, r, G_PI / 2, 3 * G_PI / 2);
    cairo_arc(cr, fw - r, y + r, r, 3 * G_PI / 2, G_PI / 2);
    cairo_close_path(cr);
    if (cur_muted)
        cairo_set_source_rgb(cr, 0x58 / 255.0, 0x5b / 255.0, 0x70 / 255.0);
    else
        cairo_set_source_rgb(cr, 0x74 / 255.0, 0xc7 / 255.0, 0xec / 255.0);
    cairo_fill(cr);
    return TRUE;
}

static const char *osd_glyph(void) {
    if (cur_muted)
        return "\U000F075F"; // muted
    if (cur_volume < 0.34)
        return "\U000F057F"; // low
    if (cur_volume < 0.67)
        return "\U000F0580"; // medium
    return "\U000F057E";     // high
}

static void osd_build(void) {
    osd_win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_widget_set_name(osd_win, "osd");

    gtk_layer_init_for_window(GTK_WINDOW(osd_win));
    gtk_layer_set_layer(GTK_WINDOW(osd_win), GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_namespace(GTK_WINDOW(osd_win), "nekobar-osd");
    gtk_layer_set_anchor(GTK_WINDOW(osd_win), GTK_LAYER_SHELL_EDGE_BOTTOM,
                         TRUE);
    gtk_layer_set_margin(GTK_WINDOW(osd_win), GTK_LAYER_SHELL_EDGE_BOTTOM,
                         90);
    gtk_layer_set_keyboard_mode(GTK_WINDOW(osd_win),
                                GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);

    GdkScreen *screen = gtk_widget_get_screen(osd_win);
    GdkVisual *rgba = gdk_screen_get_rgba_visual(screen);
    if (rgba)
        gtk_widget_set_visual(osd_win, rgba);
    gtk_widget_set_app_paintable(osd_win, TRUE);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_name(box, "osd-box");
    gtk_widget_set_size_request(box, OSD_W, OSD_H);

    osd_icon = gtk_label_new("");
    gtk_widget_set_name(osd_icon, "osd-icon");
    gtk_box_pack_start(GTK_BOX(box), osd_icon, FALSE, FALSE, 0);

    osd_area = gtk_drawing_area_new();
    g_signal_connect(osd_area, "draw", G_CALLBACK(osd_draw), NULL);
    gtk_box_pack_start(GTK_BOX(box), osd_area, TRUE, TRUE, 0);

    gtk_container_add(GTK_CONTAINER(osd_win), box);
}

static gboolean fade_step(gpointer data) {
    (void)data;
    osd_opacity -= 0.09;
    if (osd_opacity <= 0.0) {
        gtk_widget_hide(osd_win);
        fade_id = 0;
        return G_SOURCE_REMOVE;
    }
    gtk_widget_set_opacity(osd_win, osd_opacity);
    return G_SOURCE_CONTINUE;
}

static gboolean hold_expired(gpointer data) {
    (void)data;
    hide_id = 0;
    if (!fade_id)
        fade_id = g_timeout_add(16, fade_step, NULL);
    return G_SOURCE_REMOVE;
}

void osd_volume_show(void) {
    // quickset open anywhere → its capsule already shows the change
    for (guint i = 0; bars && i < bars->len; i++) {
        Bar *bar = g_ptr_array_index(bars, i);
        if (bar->qs_target == 1)
            return;
    }
    if (!osd_win)
        osd_build();

    // follow the focused monitor
    char name[64];
    if (hypr_focused_monitor(name, sizeof(name)) &&
        !g_str_equal(name, osd_mon)) {
        for (guint i = 0; bars && i < bars->len; i++) {
            Bar *bar = g_ptr_array_index(bars, i);
            if (g_str_equal(bar->hypr_name, name)) {
                gtk_widget_hide(osd_win); // remap on the new output
                gtk_layer_set_monitor(GTK_WINDOW(osd_win),
                                      bar->gdk_monitor);
                g_strlcpy(osd_mon, name, sizeof(osd_mon));
                break;
            }
        }
    }

    gtk_label_set_text(GTK_LABEL(osd_icon), osd_glyph());
    gtk_widget_queue_draw(osd_area);

    if (fade_id) { // interrupt a fade-out: snap back to solid
        g_source_remove(fade_id);
        fade_id = 0;
    }
    osd_opacity = 1.0;
    gtk_widget_set_opacity(osd_win, 1.0);
    gtk_widget_show_all(osd_win);

    if (hide_id)
        g_source_remove(hide_id);
    hide_id = g_timeout_add(OSD_HOLD_MS, hold_expired, NULL);
}
