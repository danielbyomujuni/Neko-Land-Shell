#pragma once

#include <gtk/gtk.h>

// width of the launcher app-grid extension of the shell chrome
#define NEKO_LAUNCH_W 316
// height of the glassy all-apps drawer at the panel's bottom
#define NEKO_DRAWER_H 600
// thickness of the shell's screen-edge border
#define NEKO_FRAME_W 5
// corner radius of the shell's cutouts
#define NEKO_FRAME_R 14

// One bar instance per monitor.
typedef struct {
    GtkWindow *window;
    GtkWidget *frame; // screen-border frame surface (always visible)
    GdkMonitor *gdk_monitor;
    GtkWidget *ws_box;       // workspace dot buttons
    GtkWidget *title_label;  // focused window title
    GtkWidget *mpris_event;  // mpris pill container (hidden when no player)
    GtkWidget *mpris_icon;   // upright status glyph above the rotated text
    GtkWidget *mpris_label;
    GtkWidget *tray_box;     // system tray icons
    GtkWidget *clock_label;
    GtkWidget *clock_area;   // analog dial above the digits
    GtkWidget *clock_ampm;   // small am/pm tag under the digits
    GtkWidget *vr_btn;       // SteamVR status/launch button
    GtkWidget *mem_label;
    GtkWidget *vol_label;
    GtkWidget *launcher;        // app-grid panel (launcher.c)
    GtkWidget *launcher_search;
    GtkWidget *launcher_flow;   // drawer: all-apps list
    GtkWidget *launcher_grid;   // pinned grid
    GtkWidget *launcher_drawer; // bottom drawer revealer
    GtkWidget *launcher_overlay; // hosts grid + drawer + folder card
    GtkWidget *folder_card;     // open-folder overlay card (NULL when closed)
    int folder_slot;            // grid slot the open card belongs to
    double launch_ext;          // 0..1: how far the chrome has morphed open
    int launch_target;          // 0 = closed, 1 = open
    guint launch_tick;          // frame-clock tick callback id
    gint64 launch_last_us;
    double qs_ext;              // 0..1: quickset corner morph progress
    int qs_target;
    guint qs_tick;
    gint64 qs_last_us;
    int qs_h;                   // quickset panel height (for the chrome)
    int qs_w;                   // quickset panel width (for the chrome)
    int tb_inset;            // toolbar strip carved out of the top chrome
    GtkWidget *qs_popover;   // quick settings popover
    GtkWidget *qs_scale;
    GtkWidget *qs_mute_label;
    GtkWidget *qs_sink_box;
    char hypr_name[64];      // hyprland monitor name (e.g. "DP-3")
} Bar;

extern GPtrArray *bars; // Bar*

// hypr.c — Hyprland IPC
void hypr_init(void);
char *hypr_request(const char *req); // caller frees; NULL on error
void hypr_dispatch(const char *cmd);
void hypr_refresh_workspaces(void);
void hypr_refresh_title(void);
// resolve hyprland monitor name for a gdk monitor by matching layout coords
gboolean hypr_monitor_name_at(int x, int y, char *out, gsize outlen);
gboolean hypr_focused_monitor(char *out, gsize outlen);

// main.c — re-resolve hyprland names for all bars (layout changes)
void bars_refresh_names(void);

// tray.c — StatusNotifierItem system tray
void tray_init(void);
void tray_refresh(void); // repopulate tray boxes (e.g. after a bar is added)

// modules.c — timers for clock / memory / volume / mpris
void modules_start(void);
void volume_refresh(void); // re-poll volume now (also syncs quickset)
void spawn_cmd(const char *shell_cmd);
extern double cur_volume; // 0..1, last polled
extern gboolean cur_muted;

// launcher.c — built-in app grid sliding out of the sidebar
void launcher_attach(Bar *bar);
void launcher_toggle(Bar *bar);
void launcher_toggle_focused(void); // SIGUSR2 / keybind entry point
void launcher_autoclose(void); // close open launchers (focus moved away)

// osd.c — macOS-style volume popup on the focused monitor
void osd_volume_show(void);

// vr.c — SteamVR sidebar widget
GtkWidget *vr_widget_new(Bar *bar);
void vr_start(void); // status poll timer

// midiviz.c — piano-roll overlay for the toolbar visualizer
void midiviz_set_device(const char *dev); // NULL/empty stops listening
gboolean midiviz_active(void);
void midiviz_draw(cairo_t *cr, double x, double y, double w, double h);

// toolbar.c — context toolbar sliding from the top (YouTube Music etc.)
void toolbar_start(void);
void toolbar_refocus(void); // re-evaluate per-monitor mode (focused app)
void toolbar_monitor_poke(void); // input-monitoring loopbacks changed

// quickset.c — quick settings popover (volume + output picker)
void quickset_attach(Bar *bar, GtkWidget *anchor);
void quickset_toggle(Bar *bar);
void quickset_toggle_focused(void);
void quickset_sync(void); // update sliders/mute icons from cur_volume/cur_muted
void quickset_autoclose(void); // close open panels (focus moved away)
guint quickset_loopback_for(const char *src); // loopback module id, 0=none
