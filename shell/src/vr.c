// vr.c — SteamVR sidebar widget.
//
// A headset glyph that tracks SteamVR: dim when it isn't running, green
// when it is. Left-click launches SteamVR (or, once running, routes audio
// to the Index HMD — the GPU card-profile switch the settings app does);
// right-click quits SteamVR. Status polls every 5s.

#include "nekobar.h"

#include <string.h>

// GPU audio card + the profile exposing the Index HMD port (see the
// settings app's Sound page / rgb memory notes)
#define VR_CARD "alsa_card.pci-0000_01_00.1"
#define VR_PROFILE "output:hdmi-stereo-extra2"
#define VR_SINK "alsa_output.pci-0000_01_00.1.hdmi-stereo-extra2"

static gboolean vr_running;

static void round_rect(cairo_t *cr, double x, double y, double w, double h,
                       double r) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
}

// hand-drawn HMD silhouette: rounded body, strap bump on top, ear pieces
// at the sides, nose notch cut from the bottom
static gboolean vr_draw(GtkWidget *w, cairo_t *cr, gpointer data) {
    (void)data;
    double W = gtk_widget_get_allocated_width(w);
    double H = gtk_widget_get_allocated_height(w);
    double cw = 18, ch = 14; // content box, centred in the widget
    double ox = (W - cw) / 2, oy = (H - ch) / 2;

    cairo_push_group(cr);
    if (vr_running)
        cairo_set_source_rgb(cr, 0xa6 / 255.0, 0xe3 / 255.0, 0xa1 / 255.0);
    else
        cairo_set_source_rgb(cr, 0x58 / 255.0, 0x5b / 255.0, 0x70 / 255.0);

    double bx = ox + 2, by = oy + 3.4, bw = 14, bh = 10.2;
    // strap bump
    cairo_move_to(cr, ox + 6.0, by + 0.5);
    cairo_line_to(cr, ox + 7.2, oy + 0.6);
    cairo_line_to(cr, ox + 10.8, oy + 0.6);
    cairo_line_to(cr, ox + 12.0, by + 0.5);
    cairo_close_path(cr);
    cairo_fill(cr);
    // ear pieces
    round_rect(cr, bx - 1.8, by + 2.6, 2.6, 5.0, 1.2);
    cairo_fill(cr);
    round_rect(cr, bx + bw - 0.8, by + 2.6, 2.6, 5.0, 1.2);
    cairo_fill(cr);
    // body
    round_rect(cr, bx, by, bw, bh, 3.2);
    cairo_fill(cr);
    // nose notch (cleared, so the bar shows through)
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_arc(cr, ox + cw / 2, by + bh + 0.6, 3.0, 0, 2 * G_PI);
    cairo_fill(cr);

    cairo_pop_group_to_source(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_paint(cr);
    return TRUE;
}

static void vr_update_ui(void) {
    const char *tip =
        vr_running
            ? "SteamVR running\nclick: route audio to Index\nright-click: quit SteamVR"
            : "SteamVR — click to launch";
    for (guint i = 0; bars && i < bars->len; i++) {
        Bar *bar = g_ptr_array_index(bars, i);
        if (!bar->vr_btn)
            continue;
        GtkWidget *area = gtk_bin_get_child(GTK_BIN(bar->vr_btn));
        if (area)
            gtk_widget_queue_draw(area);
        gtk_widget_set_tooltip_text(bar->vr_btn, tip);
    }
}

static gboolean vr_tick(gpointer data) {
    (void)data;
    char *out = NULL;
    g_spawn_command_line_sync("pgrep -x vrserver", &out, NULL, NULL, NULL);
    gboolean run = out && *out;
    g_free(out);
    if (run != vr_running) {
        vr_running = run;
        vr_update_ui();
    }
    return TRUE;
}

static gboolean vr_pressed(GtkWidget *w, GdkEventButton *ev, gpointer data) {
    (void)w;
    (void)data;
    if (ev->button == 1) {
        if (!vr_running) {
            spawn_cmd("steam steam://rungameid/250820"); // SteamVR
        } else {
            // route audio to the headset: activate the HDMI profile that
            // carries the Index port, then default to its sink
            spawn_cmd("pactl set-card-profile " VR_CARD " " VR_PROFILE
                      "; sleep 0.5; pactl set-default-sink " VR_SINK);
        }
        return TRUE;
    }
    if (ev->button == 3 && vr_running) {
        spawn_cmd("pkill -x vrmonitor; pkill -x vrserver");
        return TRUE;
    }
    return FALSE;
}

// build the sidebar button for one bar (packed by the caller)
GtkWidget *vr_widget_new(Bar *bar) {
    GtkWidget *btn = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);
    gtk_widget_set_name(btn, "vr-btn");
    GtkWidget *area = gtk_drawing_area_new();
    gtk_widget_set_size_request(area, 20, 16);
    gtk_widget_set_halign(area, GTK_ALIGN_CENTER);
    g_signal_connect(area, "draw", G_CALLBACK(vr_draw), NULL);
    gtk_container_add(GTK_CONTAINER(btn), area);
    g_signal_connect(btn, "button-press-event", G_CALLBACK(vr_pressed),
                     NULL);
    bar->vr_btn = btn;
    vr_update_ui();
    return btn;
}

void vr_start(void) {
    vr_tick(NULL);
    g_timeout_add_seconds(5, vr_tick, NULL);
}
