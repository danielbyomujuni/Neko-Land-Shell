// toolbar.c — context toolbar sliding down from the top edge of the
// focused monitor, with actions for whatever app is active.
//
// An OPAQUE full-width slab in the shell's chrome colours (slab bg +
// bottom rim), sliding in/out via hyprland's layer animation (layerrule
// animation slide top on the nekobar-toolbar namespace) — we map/unmap.
//
// First provider: YouTube Music. While it's actively playing, the bar
// shows the track and prev/play/next controls; pausing keeps it around
// for a grace period, closing the app slides it away. Detection: an
// mpris player whose owning process is the youtube-music desktop app.

#include "nekobar.h"

#include <gtk-layer-shell/gtk-layer-shell.h>
#include <string.h>

#define TB_GRACE_S 8 // keep the bar this long after a pause
#define TB_H 32      // slab height; also carved out of the frame chrome

// the YouTube Music provider shows on EVERY monitor (its exception —
// follow-the-focused-monitor stays the default for future providers),
// so there's one toolbar window per bar
typedef struct {
    Bar *bar;
    GtkWidget *win;
    GtkWidget *title;
    GtkWidget *play; // play/pause label flips with status
} TbWin;

static GPtrArray *tb_wins; // TbWin*
static char tb_player[128];
static guint grace_id;

static gboolean tb_tick(gpointer data);

static void tb_cmd(const char *action) {
    if (!*tb_player)
        return;
    char *cmd = g_strdup_printf("playerctl -p %s %s", tb_player, action);
    spawn_cmd(cmd);
    g_free(cmd);
    g_timeout_add(350, tb_tick, NULL); // flip the icon promptly
}

static void on_tb_prev(GtkWidget *b, gpointer d) {
    (void)b;
    (void)d;
    tb_cmd("previous");
}
static void on_tb_play(GtkWidget *b, gpointer d) {
    (void)b;
    (void)d;
    tb_cmd("play-pause");
}
static void on_tb_next(GtkWidget *b, gpointer d) {
    (void)b;
    (void)d;
    tb_cmd("next");
}

static GtkWidget *tb_button(const char *glyph, GCallback cb) {
    GtkWidget *btn = gtk_button_new_with_label(glyph);
    gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);
    g_signal_connect(btn, "clicked", cb, NULL);
    return btn;
}

static TbWin *tb_win_new(Bar *bar) {
    TbWin *tw = g_new0(TbWin, 1);
    tw->bar = bar;
    tw->win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_widget_set_name(tw->win, "toolbar");

    gtk_layer_init_for_window(GTK_WINDOW(tw->win));
    gtk_layer_set_layer(GTK_WINDOW(tw->win), GTK_LAYER_SHELL_LAYER_TOP);
    gtk_layer_set_namespace(GTK_WINDOW(tw->win), "nekobar-toolbar");
    gtk_layer_set_monitor(GTK_WINDOW(tw->win), bar->gdk_monitor);
    // full width: left edge lands past the sidebar (its exclusive zone
    // pushes us), right edge tucks inside the frame border
    gtk_layer_set_anchor(GTK_WINDOW(tw->win), GTK_LAYER_SHELL_EDGE_TOP,
                         TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(tw->win), GTK_LAYER_SHELL_EDGE_LEFT,
                         TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(tw->win), GTK_LAYER_SHELL_EDGE_RIGHT,
                         TRUE);
    gtk_layer_set_margin(GTK_WINDOW(tw->win), GTK_LAYER_SHELL_EDGE_TOP,
                         NEKO_FRAME_W);
    gtk_layer_set_margin(GTK_WINDOW(tw->win), GTK_LAYER_SHELL_EDGE_LEFT,
                         NEKO_FRAME_W);
    gtk_layer_set_margin(GTK_WINDOW(tw->win), GTK_LAYER_SHELL_EDGE_RIGHT,
                         NEKO_FRAME_W);
    gtk_layer_set_keyboard_mode(GTK_WINDOW(tw->win),
                                GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
    // reserve the strip: windows tile below the toolbar, exactly like
    // they tile beside the sidebar
    gtk_layer_auto_exclusive_zone_enable(GTK_WINDOW(tw->win));

    GdkScreen *screen = gtk_widget_get_screen(tw->win);
    GdkVisual *rgba = gdk_screen_get_rgba_visual(screen);
    if (rgba)
        gtk_widget_set_visual(tw->win, rgba);
    gtk_widget_set_app_paintable(tw->win, TRUE);

    // opaque slab spanning the width; content centred within it
    GtkWidget *slab = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_name(slab, "toolbar-box");
    gtk_widget_set_size_request(slab, -1, TB_H);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER); // dead-centred in the
    gtk_widget_set_hexpand(box, TRUE);            // fixed-height slab

    GtkWidget *icon = gtk_label_new("\U000F075A"); // music note
    gtk_widget_set_name(icon, "tb-icon");
    gtk_box_pack_start(GTK_BOX(box), icon, FALSE, FALSE, 0);

    tw->title = gtk_label_new("");
    gtk_widget_set_name(tw->title, "toolbar-title");
    gtk_label_set_ellipsize(GTK_LABEL(tw->title), PANGO_ELLIPSIZE_END);
    // ellipsized labels report a tiny natural width: pin a real one
    gtk_label_set_width_chars(GTK_LABEL(tw->title), 44);
    gtk_label_set_max_width_chars(GTK_LABEL(tw->title), 44);
    gtk_label_set_xalign(GTK_LABEL(tw->title), 0.0);
    gtk_box_pack_start(GTK_BOX(box), tw->title, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box),
                       tb_button("\U000F04AE", G_CALLBACK(on_tb_prev)),
                       FALSE, FALSE, 0);
    GtkWidget *play = tb_button("\U000F03E4", G_CALLBACK(on_tb_play));
    tw->play = gtk_bin_get_child(GTK_BIN(play));
    gtk_box_pack_start(GTK_BOX(box), play, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box),
                       tb_button("\U000F04AD", G_CALLBACK(on_tb_next)),
                       FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(slab), box, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(tw->win), slab);
    return tw;
}

// one window per current bar: drop stale ones (monitor unplugged), add
// missing ones (monitor plugged in)
static void tb_sync_windows(void) {
    if (!tb_wins)
        tb_wins = g_ptr_array_new();
    for (guint i = 0; i < tb_wins->len;) {
        TbWin *tw = g_ptr_array_index(tb_wins, i);
        gboolean alive = FALSE;
        for (guint j = 0; bars && j < bars->len; j++)
            if (g_ptr_array_index(bars, j) == tw->bar)
                alive = TRUE;
        if (!alive) {
            gtk_widget_destroy(tw->win);
            g_free(tw);
            g_ptr_array_remove_index(tb_wins, i);
        } else {
            i++;
        }
    }
    for (guint j = 0; bars && j < bars->len; j++) {
        Bar *bar = g_ptr_array_index(bars, j);
        gboolean have = FALSE;
        for (guint i = 0; i < tb_wins->len; i++)
            if (((TbWin *)g_ptr_array_index(tb_wins, i))->bar == bar)
                have = TRUE;
        if (!have)
            g_ptr_array_add(tb_wins, tb_win_new(bar));
    }
}

static gboolean tb_visible(void) {
    for (guint i = 0; tb_wins && i < tb_wins->len; i++)
        if (gtk_widget_get_visible(
                ((TbWin *)g_ptr_array_index(tb_wins, i))->win))
            return TRUE;
    return FALSE;
}

static void tb_hide(void) {
    if (grace_id) {
        g_source_remove(grace_id);
        grace_id = 0;
    }
    for (guint i = 0; tb_wins && i < tb_wins->len; i++) {
        TbWin *tw = g_ptr_array_index(tb_wins, i);
        if (gtk_widget_get_visible(tw->win))
            gtk_widget_hide(tw->win); // unmap → hyprland slides it up
        if (tw->bar->tb_inset) { // chrome hole grows back
            tw->bar->tb_inset = 0;
            gtk_widget_queue_draw(tw->bar->frame);
        }
    }
}

static gboolean grace_expired(gpointer data) {
    (void)data;
    grace_id = 0;
    tb_hide();
    return G_SOURCE_REMOVE;
}

// YouTube Music's exception: the bar appears on EVERY monitor
static void tb_show(void) {
    tb_sync_windows();
    for (guint i = 0; tb_wins && i < tb_wins->len; i++) {
        TbWin *tw = g_ptr_array_index(tb_wins, i);
        if (!gtk_widget_get_visible(tw->win))
            gtk_widget_show_all(tw->win); // map → hyprland slides down
        if (tw->bar->tb_inset != TB_H) { // chrome carves out the strip
            tw->bar->tb_inset = TB_H;
            gtk_widget_queue_draw(tw->bar->frame);
        }
    }
}

// mpris player owned by the youtube-music desktop app, or NULL
static char *yt_player(void) {
    char *out = NULL;
    if (!g_spawn_command_line_sync("playerctl -l", &out, NULL, NULL, NULL) ||
        !out)
        return NULL;
    char *found = NULL;
    char **lines = g_strsplit(out, "\n", -1);
    for (int i = 0; lines[i] && !found; i++) {
        char *line = g_strstrip(lines[i]);
        if (!*line)
            continue;
        const char *e = line + strlen(line); // trailing digits = pid
        const char *d = e;
        while (d > line && g_ascii_isdigit(*(d - 1)))
            d--;
        if (d == e)
            continue;
        char *cp = g_strdup_printf("/proc/%d/comm", atoi(d));
        char *comm = NULL;
        g_file_get_contents(cp, &comm, NULL, NULL);
        g_free(cp);
        if (comm && g_str_has_prefix(comm, "youtube-music"))
            found = g_strdup(line);
        g_free(comm);
    }
    g_strfreev(lines);
    g_free(out);
    return found;
}

static gboolean tb_tick(gpointer data) {
    (void)data;
    char *player = yt_player();
    if (!player) {
        tb_player[0] = '\0';
        tb_hide();
        return G_SOURCE_REMOVE; // re-armed by the repeating timer only
    }
    g_strlcpy(tb_player, player, sizeof(tb_player));
    g_free(player);

    char *out = NULL;
    char *argv[] = {"playerctl", "-p", tb_player, "metadata", "--format",
                    "{{status}}\x1f{{title}}\x1f{{artist}}", NULL};
    g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, &out,
                 NULL, NULL, NULL);
    if (!out) {
        tb_hide();
        return G_SOURCE_REMOVE;
    }
    g_strchomp(out);
    char **f = g_strsplit(out, "\x1f", 3);
    if (g_strv_length(f) == 3) {
        gboolean playing = g_str_equal(f[0], "Playing");
        if (tb_wins || playing) { // don't build windows just to idle
            char *lbl = (*f[2])
                            ? g_strdup_printf("%s — %s", f[1], f[2])
                            : g_strdup(f[1]);
            if (playing) {
                if (grace_id) {
                    g_source_remove(grace_id);
                    grace_id = 0;
                }
                tb_show();
            } else if (tb_visible() && !grace_id) {
                grace_id = g_timeout_add_seconds(TB_GRACE_S, grace_expired,
                                                 NULL);
            }
            for (guint i = 0; tb_wins && i < tb_wins->len; i++) {
                TbWin *tw = g_ptr_array_index(tb_wins, i);
                gtk_label_set_text(GTK_LABEL(tw->title), lbl);
                gtk_label_set_text(GTK_LABEL(tw->play),
                                   playing ? "\U000F03E4" : "\U000F040A");
            }
            g_free(lbl);
        }
    }
    g_strfreev(f);
    g_free(out);
    return G_SOURCE_REMOVE;
}

static gboolean tb_poll(gpointer data) {
    (void)data;
    tb_tick(NULL);
    return TRUE;
}

void toolbar_start(void) {
    tb_tick(NULL);
    g_timeout_add_seconds(2, tb_poll, NULL);
}
