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

static void vr_update_ui(void) {
    const char *tip =
        vr_running
            ? "SteamVR running\nclick: route audio to Index\nright-click: quit SteamVR"
            : "SteamVR — click to launch";
    for (guint i = 0; bars && i < bars->len; i++) {
        Bar *bar = g_ptr_array_index(bars, i);
        if (!bar->vr_btn)
            continue;
        GtkStyleContext *sc = gtk_widget_get_style_context(bar->vr_btn);
        if (vr_running)
            gtk_style_context_add_class(sc, "vr-on");
        else
            gtk_style_context_remove_class(sc, "vr-on");
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
    GtkWidget *btn = gtk_button_new_with_label("\U000F0894");
    gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);
    gtk_widget_set_name(btn, "vr-btn");
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
