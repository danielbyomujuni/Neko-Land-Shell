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
#include <signal.h>
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
    GtkWidget *viz;  // spectrum drawing area behind the content
} TbWin;

static GPtrArray *tb_wins; // TbWin*
static char tb_player[128];
static guint grace_id;

// ---- audio visualizer (cava raw ascii → bars behind the content) ----

#define VIZ_BARS 192 // fine-grained: ~10px pitch across a 1920 monitor

static double viz_vals[VIZ_BARS]; // 0..1
static GPid viz_pid;
static guint viz_watch;
static GIOChannel *viz_ch;

static void viz_queue_draws(void) {
    for (guint i = 0; tb_wins && i < tb_wins->len; i++) {
        TbWin *tw = g_ptr_array_index(tb_wins, i);
        if (tw->viz)
            gtk_widget_queue_draw(tw->viz);
    }
}

static gboolean viz_io(GIOChannel *ch, GIOCondition cond, gpointer data) {
    (void)data;
    if (cond & (G_IO_HUP | G_IO_ERR)) {
        viz_watch = 0;
        return G_SOURCE_REMOVE;
    }
    char *line = NULL;
    gsize len = 0;
    // drain everything pending, keep only the newest frame
    while (g_io_channel_read_line(ch, &line, &len, NULL, NULL) ==
               G_IO_STATUS_NORMAL &&
           line) {
        char **v = g_strsplit(line, ";", -1);
        for (int i = 0; v[i] && i < VIZ_BARS; i++)
            viz_vals[i] = CLAMP(atoi(v[i]) / 100.0, 0.0, 1.0);
        g_strfreev(v);
        g_free(line);
        line = NULL;
        if (g_io_channel_get_buffer_condition(ch) != G_IO_IN)
            break;
    }
    viz_queue_draws();
    return G_SOURCE_CONTINUE;
}

static void viz_start(void) {
    if (viz_pid)
        return;
    char *cfg = g_build_filename(g_get_user_runtime_dir(),
                                 "nekobar-cava.conf", NULL);
    // capture the default sink's monitor — cava's auto input follows the
    // default SOURCE (a microphone), which visualises silence
    char *sink = NULL;
    g_spawn_command_line_sync("pactl get-default-sink", &sink, NULL, NULL,
                              NULL);
    if (sink)
        g_strchomp(sink);
    char *conf = g_strdup_printf(
        "[general]\nbars = %d\nframerate = 30\n"
        "[input]\nmethod = pulse\nsource = %s.monitor\n"
        "[output]\nmethod = raw\nraw_target = /dev/stdout\n"
        "data_format = ascii\nascii_max_range = 100\n",
        VIZ_BARS, sink && *sink ? sink : "auto");
    g_free(sink);
    g_file_set_contents(cfg, conf, -1, NULL);
    g_free(conf);
    char *argv[] = {"cava", "-p", cfg, NULL};
    int out_fd = -1;
    if (g_spawn_async_with_pipes(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                                 NULL, NULL, &viz_pid, NULL, &out_fd, NULL,
                                 NULL)) {
        viz_ch = g_io_channel_unix_new(out_fd);
        g_io_channel_set_flags(viz_ch, G_IO_FLAG_NONBLOCK, NULL);
        viz_watch = g_io_add_watch(viz_ch, G_IO_IN | G_IO_HUP | G_IO_ERR,
                                   viz_io, NULL);
    }
    g_free(cfg);
}

static void viz_stop(void) {
    if (viz_watch) {
        g_source_remove(viz_watch);
        viz_watch = 0;
    }
    if (viz_ch) {
        g_io_channel_shutdown(viz_ch, FALSE, NULL);
        g_io_channel_unref(viz_ch);
        viz_ch = NULL;
    }
    if (viz_pid) {
        kill(viz_pid, SIGTERM);
        g_spawn_close_pid(viz_pid);
        viz_pid = 0;
    }
    memset(viz_vals, 0, sizeof(viz_vals));
}

// subtle sapphire capsules rising from the strip's bottom edge — same
// rounded-pill language as the workspace dots
static gboolean viz_draw(GtkWidget *w, cairo_t *cr, gpointer data) {
    (void)data;
    double W = gtk_widget_get_allocated_width(w);
    double H = gtk_widget_get_allocated_height(w);
    double pitch = W / VIZ_BARS;
    double barw = pitch * 0.62;
    cairo_set_source_rgba(cr, 0x74 / 255.0, 0xc7 / 255.0, 0xec / 255.0,
                          0.30);
    for (int i = 0; i < VIZ_BARS; i++) {
        double h = MAX(viz_vals[i] * (H - 2), 0.0);
        if (h < 1.5)
            continue;
        double x = i * pitch + (pitch - barw) / 2;
        double r = MIN(barw / 2, h / 2);
        double y = H - h;
        cairo_new_sub_path(cr);
        cairo_arc(cr, x + barw - r, y + r, r, -G_PI / 2, 0);
        cairo_line_to(cr, x + barw, H);
        cairo_line_to(cr, x, H);
        cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
        cairo_close_path(cr);
    }
    cairo_fill(cr);
    return TRUE;
}

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

    // spectrum behind the content, spanning the whole slab
    GtkWidget *over = gtk_overlay_new();
    tw->viz = gtk_drawing_area_new();
    g_signal_connect(tw->viz, "draw", G_CALLBACK(viz_draw), NULL);
    gtk_container_add(GTK_CONTAINER(over), tw->viz);
    gtk_overlay_add_overlay(GTK_OVERLAY(over), box);
    gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(over), box, FALSE);

    gtk_box_pack_start(GTK_BOX(slab), over, TRUE, TRUE, 0);
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
    viz_stop();
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
    viz_start();
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

// ---- event-driven wakeup: playerctl --follow pushes a line the moment
// any player's status/track changes, so the bar slides in instantly
// (the poll above stays as a slow fallback) ----

static guint ev_debounce;
static void ev_spawn(void);

static gboolean ev_poke(gpointer data) {
    (void)data;
    ev_debounce = 0;
    tb_tick(NULL);
    return G_SOURCE_REMOVE;
}

static gboolean ev_respawn(gpointer data) {
    (void)data;
    ev_spawn();
    return G_SOURCE_REMOVE;
}

static gboolean ev_io(GIOChannel *ch, GIOCondition cond, gpointer data) {
    (void)data;
    if (cond & (G_IO_HUP | G_IO_ERR)) { // playerctld went away: retry
        g_timeout_add_seconds(5, ev_respawn, NULL);
        return G_SOURCE_REMOVE;
    }
    char *line = NULL;
    gsize len = 0;
    while (g_io_channel_read_line(ch, &line, &len, NULL, NULL) ==
               G_IO_STATUS_NORMAL &&
           line) {
        g_free(line);
        line = NULL;
        if (g_io_channel_get_buffer_condition(ch) != G_IO_IN)
            break;
    }
    if (!ev_debounce)
        ev_debounce = g_timeout_add(120, ev_poke, NULL);
    return G_SOURCE_CONTINUE;
}

static void ev_spawn(void) {
    char *argv[] = {"playerctl", "--all-players", "--follow", "status",
                    NULL};
    int out_fd = -1;
    if (!g_spawn_async_with_pipes(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                                  NULL, NULL, NULL, NULL, &out_fd, NULL,
                                  NULL))
        return;
    GIOChannel *ch = g_io_channel_unix_new(out_fd);
    g_io_channel_set_flags(ch, G_IO_FLAG_NONBLOCK, NULL);
    g_io_channel_set_close_on_unref(ch, TRUE);
    g_io_add_watch(ch, G_IO_IN | G_IO_HUP | G_IO_ERR, ev_io, NULL);
    g_io_channel_unref(ch);
}

void toolbar_start(void) {
    tb_tick(NULL);
    ev_spawn();
    g_timeout_add_seconds(5, tb_poll, NULL); // slow fallback only
}
