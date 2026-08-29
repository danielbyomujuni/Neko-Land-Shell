#pragma once

#include <gtk/gtk.h>

// One bar instance per monitor.
typedef struct {
    GtkWindow *window;
    GtkWidget *ws_box;       // workspace dot buttons
    GtkWidget *title_label;  // focused window title
    GtkWidget *mpris_event;  // mpris pill container (hidden when no player)
    GtkWidget *mpris_label;
    GtkWidget *tray_box;     // system tray icons
    GtkWidget *clock_label;
    GtkWidget *mem_label;
    GtkWidget *vol_label;
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

// tray.c — StatusNotifierItem system tray
void tray_init(void);

// modules.c — timers for clock / memory / volume / mpris
void modules_start(void);
void spawn_cmd(const char *shell_cmd);
