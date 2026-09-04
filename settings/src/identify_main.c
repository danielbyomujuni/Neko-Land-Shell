// nekoland-identify — flashes each monitor's name in an overlay for a
// moment, macOS-style. Spawned by the settings app's Displays page.
//
// GTK3 + gtk-layer-shell (the GTK4 layer-shell isn't packaged here; a
// separate tiny process keeps the GTK versions apart). Each argument is
// one monitor: "NAME;X;Y;SUBTITLE" — matched to a GdkMonitor by the x/y
// of its logical geometry, the same trick nekobar uses.

#include <gtk-layer-shell/gtk-layer-shell.h>
#include <gtk/gtk.h>
#include <stdlib.h>
#include <string.h>

#define SHOW_MS 1700

static const char *css =
    "window { background: transparent; }"
    ".ident-card { background: rgba(24, 24, 37, 0.92);"
    "  border: 2px solid rgba(137, 180, 250, 0.9); border-radius: 18px;"
    "  padding: 26px 44px; }"
    ".ident-name { color: #cdd6f4; font-family: 'JetBrainsMono Nerd Font',"
    "  monospace; font-size: 42px; font-weight: 800; }"
    ".ident-sub { color: #89b4fa; font-family: 'JetBrainsMono Nerd Font',"
    "  monospace; font-size: 16px; }";

static gboolean quit_cb(gpointer data) {
    (void)data;
    gtk_main_quit();
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv) {
    gtk_init(&argc, &argv);

    GtkCssProvider *prov = gtk_css_provider_new();
    gtk_css_provider_load_from_data(prov, css, -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(), GTK_STYLE_PROVIDER(prov),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    GdkDisplay *display = gdk_display_get_default();
    int n = gdk_display_get_n_monitors(display);
    for (int i = 0; i < n; i++) {
        GdkMonitor *mon = gdk_display_get_monitor(display, i);
        GdkRectangle geo;
        gdk_monitor_get_geometry(mon, &geo);

        // find the caller-supplied label for this monitor by position
        char *label = NULL, *sub = NULL;
        for (int a = 1; a < argc && !label; a++) {
            char **f = g_strsplit(argv[a], ";", 4);
            if (f[0] && f[1] && f[2] && atoi(f[1]) == geo.x &&
                atoi(f[2]) == geo.y) {
                label = g_strdup(f[0]);
                sub = g_strdup(f[3] ? f[3] : "");
            }
            g_strfreev(f);
        }
        const char *name =
            label ? label : gdk_monitor_get_model(mon);

        GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_layer_init_for_window(GTK_WINDOW(win));
        gtk_layer_set_layer(GTK_WINDOW(win), GTK_LAYER_SHELL_LAYER_OVERLAY);
        gtk_layer_set_namespace(GTK_WINDOW(win), "nekoland-identify");
        gtk_layer_set_monitor(GTK_WINDOW(win), mon);
        // no anchors: centered on the monitor

        GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_style_context_add_class(gtk_widget_get_style_context(card),
                                    "ident-card");
        GtkWidget *lbl = gtk_label_new(name ? name : "?");
        gtk_style_context_add_class(gtk_widget_get_style_context(lbl),
                                    "ident-name");
        gtk_box_pack_start(GTK_BOX(card), lbl, FALSE, FALSE, 0);
        if (sub && *sub) {
            GtkWidget *sl = gtk_label_new(sub);
            gtk_style_context_add_class(gtk_widget_get_style_context(sl),
                                        "ident-sub");
            gtk_box_pack_start(GTK_BOX(card), sl, FALSE, FALSE, 0);
        }
        g_free(sub);
        g_free(label);
        gtk_container_add(GTK_CONTAINER(win), card);
        gtk_widget_show_all(win);
    }

    g_timeout_add(SHOW_MS, quit_cb, NULL);
    gtk_main();
    return 0;
}
