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
#include <json-glib/json-glib.h>
#include <signal.h>
#include <string.h>

#define TB_GRACE_S 8 // keep the bar this long after a pause
#define TB_H 32 // slab height
// chrome carve: slab + a hair so the hole rim clears the slab and
// reads as the toolbar's bottom border (one line, not two)
#define TB_INSET (TB_H + 2)

// the YouTube Music provider shows on EVERY monitor (its exception —
// follow-the-focused-monitor stays the default for future providers),
// so there's one toolbar window per bar
typedef struct {
    Bar *bar;
    GtkWidget *win;
    GtkWidget *slab;    // full-width strip container
    GtkWidget *content; // icon/title/controls row
    GtkWidget *fix;  // GtkFixed: content placed at explicit coordinates
    GtkWidget *menubar; // VSCodium File/Edit/… strip (pill mode only)
    GtkWidget *icon;    // leading glyph: music note / monitoring mic
    GtkWidget *ctl[3];  // prev / play-pause / next (music only)
    GtkWidget *mon_off; // stop-monitoring button (monitoring only)
    GtkWidget *title;
    GtkWidget *play; // play/pause label flips with status
    int last_x, last_y; // last gtk_fixed_move, to skip redundant moves
    gboolean pill;   // compact right-side pill (app-focused mode)
    double pill_ext; // 0 = full-width bar … 1 = right-side pill
    guint pill_anim; // frame-clock tick id while morphing
    gint64 pill_last_us;
} TbWin;

static GPtrArray *tb_wins; // TbWin*
static char tb_player[128];
static guint grace_id;

static void tb_slab_sized(GtkWidget *w, GdkRectangle *alloc,
                          gpointer data); // defined with the pill morph
static void on_tb_monoff(GtkWidget *b, gpointer d); // with the providers

// ---- audio visualizer (cava raw ascii → bars behind the content) ----

#define VIZ_BARS 192 // fine-grained: ~10px pitch across a 1920 monitor

static double viz_vals[VIZ_BARS]; // 0..1
static GPid viz_pid;
static guint viz_watch;
static GIOChannel *viz_ch;

static void tw_update_margin(TbWin *tw);

static void viz_queue_draws(void) {
    for (guint i = 0; tb_wins && i < tb_wins->len; i++) {
        TbWin *tw = g_ptr_array_index(tb_wins, i);
        if (tw->slab)
            gtk_widget_queue_draw(tw->slab);
        if (!tw->pill_anim) // keep position synced (dedup'd, so cheap)
            tw_update_margin(tw);
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
// rounded-pill language as the workspace dots. As the bar morphs into
// the app-focused pill, the spectrum compresses INTO the pill: the bars
// re-span the shrinking region and are clipped to its rounded card.
// Drawn by the SLAB's own draw handler (before its children) — no
// overlay, no child GdkWindows (those broke repositioning on remap).
static gboolean viz_draw(GtkWidget *w, cairo_t *cr, gpointer data) {
    TbWin *tw = data;
    double W = gtk_widget_get_allocated_width(w);
    double H = gtk_widget_get_allocated_height(w);
    // full-mode bars stop short of the strip's ends, clear of the
    // chrome's bevel curves at the corners
    double inset = NEKO_FRAME_R + 6;
    double rx = inset, rw = W - 2 * inset, ry = 0, rh = H;
    double e = tw ? tw->pill_ext : 0.0;
    // slab background (cairo, not CSS — CSS paints over this handler):
    // fades out as the strip empties into pill mode
    cairo_set_source_rgba(cr, 0x11 / 255.0, 0x11 / 255.0, 0x1B / 255.0,
                          1.0 - e);
    cairo_paint(cr);
    if (e > 0.001 && tw->content &&
        gtk_widget_get_mapped(tw->content)) {
        int px = 0, py = 0;
        if (gtk_widget_translate_coordinates(tw->content, w, 0, 0, &px,
                                             &py)) {
            double pw = gtk_widget_get_allocated_width(tw->content);
            double ph = gtk_widget_get_allocated_height(tw->content);
            rx = inset + (px - inset) * e;
            ry = py * e;
            rw = (W - 2 * inset) + (pw - (W - 2 * inset)) * e;
            rh = H + (ph - H) * e;
            double cr_r = 13.0 * e;
            cairo_new_sub_path(cr); // rounded clip = the pill card
            cairo_arc(cr, rx + rw - cr_r, ry + cr_r, cr_r, -G_PI / 2, 0);
            cairo_arc(cr, rx + rw - cr_r, ry + rh - cr_r, cr_r, 0,
                      G_PI / 2);
            cairo_arc(cr, rx + cr_r, ry + rh - cr_r, cr_r, G_PI / 2,
                      G_PI);
            cairo_arc(cr, rx + cr_r, ry + cr_r, cr_r, G_PI,
                      3 * G_PI / 2);
            cairo_close_path(cr);
            cairo_clip(cr);
        }
    }
    double pitch = rw / VIZ_BARS;
    double barw = pitch * 0.62;
    // fade out as the bars converge on the pill — the pill's own card
    // bars (tb_content_draw) fade in to take over
    cairo_set_source_rgba(cr, 0x74 / 255.0, 0xc7 / 255.0, 0xec / 255.0,
                          0.30 * (1.0 - e));
    double bottom = ry + rh;
    for (int i = 0; i < VIZ_BARS; i++) {
        double h = MAX(viz_vals[i] * (rh - 2), 0.0);
        if (h < 1.5)
            continue;
        double x = rx + i * pitch + (pitch - barw) / 2;
        double r = MIN(barw / 2, h / 2);
        double y = bottom - h;
        cairo_new_sub_path(cr);
        cairo_arc(cr, x + barw - r, y + r, r, -G_PI / 2, 0);
        cairo_line_to(cr, x + barw, bottom);
        cairo_line_to(cr, x, bottom);
        cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
        cairo_close_path(cr);
    }
    cairo_fill(cr);
    return FALSE; // children (the content card) draw on top
}

static gboolean tb_tick(gpointer data);

// ---- VSCodium menubar ----
//
// While VSCodium holds the strip (pill mode), the empty space carries a
// File/Edit/… menubar. Electron exports no global menu on wayland, so
// the entries drive VSCodium through its default keybindings, delivered
// with hyprland's sendshortcut dispatcher. Chords join with '+'.

typedef struct {
    const char *label;
    const char *keys; // "MODS,key"; chord steps joined with '+'
} VsItem;

typedef struct {
    const char *title;
    VsItem items[12];
} VsMenu;

static const VsMenu vs_menus[] = {
    {"File",
     {{"New File", "CTRL,n"},
      {"New Window", "CTRL SHIFT,n"},
      {"Open File…", "CTRL,o"},
      {"Open Folder…", "CTRL,k+CTRL,o"},
      {"Save", "CTRL,s"},
      {"Save As…", "CTRL SHIFT,s"},
      {"Close Editor", "CTRL,w"},
      {"Close Window", "CTRL SHIFT,w"},
      {NULL, NULL}}},
    {"Edit",
     {{"Undo", "CTRL,z"},
      {"Redo", "CTRL,y"},
      {"Cut", "CTRL,x"},
      {"Copy", "CTRL,c"},
      {"Paste", "CTRL,v"},
      {"Find", "CTRL,f"},
      {"Replace", "CTRL,h"},
      {"Find in Files", "CTRL SHIFT,f"},
      {NULL, NULL}}},
    {"Selection",
     {{"Select All", "CTRL,a"},
      {"Expand Selection", "SHIFT ALT,right"},
      {"Shrink Selection", "SHIFT ALT,left"},
      {"Copy Line Up", "SHIFT ALT,up"},
      {"Copy Line Down", "SHIFT ALT,down"},
      {"Add Cursor Above", "CTRL ALT,up"},
      {"Add Cursor Below", "CTRL ALT,down"},
      {NULL, NULL}}},
    {"View",
     {{"Command Palette…", "CTRL SHIFT,p"},
      {"Explorer", "CTRL SHIFT,e"},
      {"Source Control", "CTRL SHIFT,g"},
      {"Extensions", "CTRL SHIFT,x"},
      {"Toggle Sidebar", "CTRL,b"},
      {"Toggle Panel", "CTRL,j"},
      {NULL, NULL}}},
    {"Go",
     {{"Go to File…", "CTRL,p"},
      {"Go to Symbol…", "CTRL SHIFT,o"},
      {"Go to Line…", "CTRL,g"},
      {"Go to Definition", ",F12"},
      {"Back", "ALT,left"},
      {"Forward", "ALT,right"},
      {NULL, NULL}}},
    {"Run",
     {{"Start Debugging", ",F5"},
      {"Run Without Debugging", "CTRL,F5"},
      {"Stop Debugging", "SHIFT,F5"},
      {"Restart Debugging", "CTRL SHIFT,F5"},
      {"Toggle Breakpoint", ",F9"},
      {NULL, NULL}}},
    {"Terminal",
     {{"New Terminal", "CTRL SHIFT,grave"},
      {"Toggle Terminal", "CTRL,grave"},
      {NULL, NULL}}},
};

// while one of our menus is open, hyprland's focus wanders to the popup;
// the mode logic must not read that as "VSCodium lost focus"
static gboolean tb_menu_open;

static gboolean vs_menu_destroy(gpointer menu) {
    gtk_widget_destroy(menu);
    return G_SOURCE_REMOVE;
}

static void vs_menu_closed(GtkWidget *menu, gpointer data) {
    (void)data;
    tb_menu_open = FALSE;
    g_idle_add(vs_menu_destroy, menu);
}

static void vs_item_cb(GtkMenuItem *item, gpointer data) {
    (void)item;
    const char *keys = data;
    GString *cmd = g_string_new(NULL);
    char **steps = g_strsplit(keys, "+", -1);
    for (int i = 0; steps[i]; i++)
        g_string_append_printf(
            cmd, "%shyprctl dispatch sendshortcut '%s,class:codium'",
            i ? "; sleep 0.08; " : "", steps[i]);
    g_strfreev(steps);
    spawn_cmd(cmd->str);
    g_string_free(cmd, TRUE);
}

static void vs_menu_btn(GtkWidget *btn, gpointer data) {
    const VsMenu *m = data;
    GtkWidget *menu = gtk_menu_new();
    for (int i = 0; m->items[i].label; i++) {
        GtkWidget *it = gtk_menu_item_new_with_label(m->items[i].label);
        g_signal_connect(it, "activate", G_CALLBACK(vs_item_cb),
                         (gpointer)m->items[i].keys);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
    }
    g_signal_connect(menu, "deactivate", G_CALLBACK(vs_menu_closed), NULL);
    tb_menu_open = TRUE;
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_widget(GTK_MENU(menu), btn, GDK_GRAVITY_SOUTH_WEST,
                             GDK_GRAVITY_NORTH_WEST, NULL);
}

// spectrum inside the pill: painted over the card's CSS background but
// beneath its children (runs before child draw, returns FALSE)
static gboolean tb_content_draw(GtkWidget *w, cairo_t *cr, gpointer data) {
    TbWin *tw = data;
    double e = tw->pill_ext;
    if (e < 0.02)
        return FALSE;
    double W = gtk_widget_get_allocated_width(w);
    double H = gtk_widget_get_allocated_height(w);
    double r = 13;
    cairo_save(cr);
    cairo_new_sub_path(cr);
    cairo_arc(cr, W - r, r, r, -G_PI / 2, 0);
    cairo_arc(cr, W - r, H - r, r, 0, G_PI / 2);
    cairo_arc(cr, r, H - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, r, r, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
    cairo_clip(cr);
    // the card background is painted HERE (not CSS): default draw runs
    // after this handler and would cover the bars otherwise
    cairo_set_source_rgba(cr, 0x18 / 255.0, 0x18 / 255.0, 0x25 / 255.0,
                          e);
    cairo_paint(cr);
    int step = VIZ_BARS / 48; // coarser sampling for the small card
    double pitch = W / 48.0;
    double barw = pitch * 0.62;
    cairo_set_source_rgba(cr, 0x74 / 255.0, 0xc7 / 255.0, 0xec / 255.0,
                          0.45 * e); // punchier over the dark card
    for (int i = 0; i < 48; i++) {
        double h = MAX(viz_vals[i * step] * (H - 2), 0.0);
        if (h < 1.5)
            continue;
        double x = i * pitch + (pitch - barw) / 2;
        double rr = MIN(barw / 2, h / 2);
        double y = H - h;
        cairo_new_sub_path(cr);
        cairo_arc(cr, x + barw - rr, y + rr, rr, -G_PI / 2, 0);
        cairo_line_to(cr, x + barw, H);
        cairo_line_to(cr, x, H);
        cairo_arc(cr, x + rr, y + rr, rr, G_PI, 3 * G_PI / 2);
        cairo_close_path(cr);
    }
    cairo_fill(cr);
    cairo_restore(cr);
    return FALSE; // children (title, controls) draw on top
}

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
    // they tile beside the sidebar. The compositor adds the top margin
    // to the zone, so subtract it — otherwise the top window gap ends
    // up 5px wider than the other edges
    gtk_layer_set_exclusive_zone(GTK_WINDOW(tw->win),
                                 TB_H - NEKO_FRAME_W);

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
    // positioned by gtk_fixed_move in tw_update_margin — no alignment
    gtk_style_context_add_class(gtk_widget_get_style_context(box),
                                "tb-content"); // colour-fade base
    g_signal_connect(box, "draw", G_CALLBACK(tb_content_draw), tw);

    tw->icon = gtk_label_new("\U000F075A"); // music note (or mic)
    gtk_widget_set_name(tw->icon, "tb-icon");
    gtk_box_pack_start(GTK_BOX(box), tw->icon, FALSE, FALSE, 0);

    tw->title = gtk_label_new("");
    gtk_widget_set_name(tw->title, "toolbar-title");
    gtk_label_set_ellipsize(GTK_LABEL(tw->title), PANGO_ELLIPSIZE_END);
    // ellipsized labels report a tiny natural width: pin a real one
    gtk_label_set_width_chars(GTK_LABEL(tw->title), 44);
    gtk_label_set_max_width_chars(GTK_LABEL(tw->title), 44);
    gtk_label_set_xalign(GTK_LABEL(tw->title), 0.0);
    gtk_box_pack_start(GTK_BOX(box), tw->title, FALSE, FALSE, 0);

    tw->ctl[0] = tb_button("\U000F04AE", G_CALLBACK(on_tb_prev));
    gtk_box_pack_start(GTK_BOX(box), tw->ctl[0], FALSE, FALSE, 0);
    tw->ctl[1] = tb_button("\U000F03E4", G_CALLBACK(on_tb_play));
    tw->play = gtk_bin_get_child(GTK_BIN(tw->ctl[1]));
    gtk_box_pack_start(GTK_BOX(box), tw->ctl[1], FALSE, FALSE, 0);
    tw->ctl[2] = tb_button("\U000F04AD", G_CALLBACK(on_tb_next));
    gtk_box_pack_start(GTK_BOX(box), tw->ctl[2], FALSE, FALSE, 0);
    // monitoring-only: mic-off stops the shown input's loopback
    tw->mon_off = tb_button("\U000F036D", G_CALLBACK(on_tb_monoff));
    gtk_widget_set_tooltip_text(tw->mon_off, "Stop monitoring this input");
    gtk_box_pack_start(GTK_BOX(box), tw->mon_off, FALSE, FALSE, 0);

    tw->slab = slab;
    tw->content = box;
    g_signal_connect(slab, "size-allocate", G_CALLBACK(tb_slab_sized),
                     tw);

    // the spectrum is painted by the slab's draw handler (behind its
    // children); the content packs straight into the slab — windowless
    // widgets whose margins are always honoured
    g_signal_connect(slab, "draw", G_CALLBACK(viz_draw), tw);
    // VSCodium menubar at the strip's left, pill mode only
    GtkWidget *mb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_set_name(mb, "vs-menubar");
    for (guint mi = 0; mi < G_N_ELEMENTS(vs_menus); mi++) {
        GtkWidget *b = gtk_button_new_with_label(vs_menus[mi].title);
        gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
        g_signal_connect(b, "clicked", G_CALLBACK(vs_menu_btn),
                         (gpointer)&vs_menus[mi]);
        gtk_box_pack_start(GTK_BOX(mb), b, FALSE, FALSE, 0);
    }
    gtk_widget_set_no_show_all(mb, TRUE); // mode-controlled visibility
    tw->menubar = mb;
    gtk_box_pack_start(GTK_BOX(slab), mb, FALSE, FALSE, 0);
    tw->fix = gtk_fixed_new();
    gtk_widget_set_hexpand(tw->fix, TRUE);
    gtk_fixed_put(GTK_FIXED(tw->fix), box, 0, 0);
    g_signal_connect(tw->fix, "size-allocate", G_CALLBACK(tb_slab_sized),
                     tw);
    gtk_box_pack_start(GTK_BOX(slab), tw->fix, TRUE, TRUE, 0);
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
            if (g_getenv("NEKOBAR_TB_DEBUG"))
                g_printerr("sync: destroy stale tw=%p\n", (void *)tw);
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
        if (!have) {
            TbWin *nw = tb_win_new(bar);
            if (g_getenv("NEKOBAR_TB_DEBUG"))
                g_printerr("sync: create tw=%p mon=%s\n", (void *)nw,
                           bar->hypr_name);
            g_ptr_array_add(tb_wins, nw);
        }
    }
}

// ---- per-monitor mode: when a matching app (VSCodium for now) is
// focused on a monitor, that monitor's strip empties out and the music
// controls shrink into a pill on the right ----

// (timeout-driven morph: the old frame-clock driver froze across remaps)
// place the content at explicit pixels inside the GtkFixed: ext=0 centres
// it, ext=1 tucks it 10px from the right — no halign/margin machinery,
// which proved unreliable across layer-surface remaps
static void tw_update_margin(TbWin *tw) {
    if (!tw->fix)
        return;
    GtkAllocation fa;
    gtk_widget_get_allocation(tw->fix, &fa);
    GtkRequisition nat;
    gtk_widget_get_preferred_size(tw->content, NULL, &nat);
    if (fa.width <= nat.width) // pre-allocation
        return;
    double cx = (fa.width - nat.width) / 2.0;
    double rx = fa.width - nat.width - 10.0;
    int x = (int)(cx + (rx - cx) * tw->pill_ext + 0.5);
    int y = MAX(0, (fa.height - nat.height) / 2);
    if (x != tw->last_x || y != tw->last_y) {
        tw->last_x = x;
        tw->last_y = y;
        gtk_fixed_move(GTK_FIXED(tw->fix), tw->content, x, y);
    }
    if (g_getenv("NEKOBAR_TB_DEBUG"))
        g_printerr("upd_pos mon=%s ext=%.2f x=%d y=%d fw=%d cw=%d\n",
                   tw->bar->hypr_name, tw->pill_ext, x, y, fa.width,
                   nat.width);
}

// plain timeout, NOT a frame-clock tick: tick callbacks proved
// unreliable on layer surfaces that were unmapped and remapped
static gboolean pill_tick(gpointer data) {
    TbWin *tw = data;
    gint64 now = g_get_monotonic_time();
    double dt = CLAMP((now - tw->pill_last_us) / 1e6, 0.0, 0.05);
    tw->pill_last_us = now;
    double target = tw->pill ? 1.0 : 0.0;
    tw->pill_ext += (target - tw->pill_ext) * MIN(1.0, 12.0 * dt);
    if (ABS(target - tw->pill_ext) < 0.004)
        tw->pill_ext = target;
    tw_update_margin(tw);
    gtk_widget_queue_draw(tw->slab);
    if (g_getenv("NEKOBAR_TB_DEBUG"))
        g_printerr("pill_tick ext=%.3f\n", tw->pill_ext);
    if (tw->pill_ext == target) {
        tw->pill_anim = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

// make sure the morph is running (or snapped) whenever ext disagrees
// with the mode — safe to call any time, e.g. right after mapping
static void tw_kick_anim(TbWin *tw) {
    double target = tw->pill ? 1.0 : 0.0;
    if (tw->pill_ext == target || tw->pill_anim)
        return;
    if (gtk_widget_get_visible(tw->win)) {
        tw->pill_last_us = g_get_monotonic_time();
        tw->pill_anim = g_timeout_add(16, pill_tick, tw);
    } else { // not on screen: snap
        tw->pill_ext = target;
        tw_update_margin(tw);
        gtk_widget_queue_draw(tw->slab);
    }
}

static void tw_apply_mode(TbWin *tw, gboolean pill) {
    if (tw->pill == pill)
        return;
    tw->pill = pill;
    // card/strip colours cross-fade via the CSS transitions; the slide
    // and the visualizer compression follow pill_ext below
    GtkStyleContext *ss = gtk_widget_get_style_context(tw->slab);
    GtkStyleContext *cs = gtk_widget_get_style_context(tw->content);
    if (pill) {
        gtk_style_context_add_class(ss, "empty");
        gtk_style_context_add_class(cs, "tb-pill");
        // (show_all early-outs on a no-show-all widget: lift it briefly)
        gtk_widget_set_no_show_all(tw->menubar, FALSE);
        gtk_widget_show_all(tw->menubar); // the app's menus take over
        gtk_widget_set_no_show_all(tw->menubar, TRUE);
    } else {
        gtk_style_context_remove_class(ss, "empty");
        gtk_style_context_remove_class(cs, "tb-pill");
        gtk_widget_hide(tw->menubar);
    }
    tw_kick_anim(tw);
}

static void tb_slab_sized(GtkWidget *w, GdkRectangle *alloc,
                          gpointer data) {
    (void)w;
    (void)alloc;
    tw_update_margin(data); // keep the centring exact on any resize
}

// pill only while a matching app IS the focused window, and only on the
// monitor holding it — focus anything else and every bar re-centres
static void tb_apply_visibility(void); // defined with show/hide below

static void tb_refocus_now(void) {
    if (tb_menu_open) // our own menu popup holds the focus right now
        return;
    tb_sync_windows(); // codium alone can summon the bar: need windows
    char *win = NULL;
    g_spawn_command_line_sync("hyprctl activewindow -j", &win, NULL, NULL,
                              NULL);
    char pill_mon[64] = "";
    if (win) {
        JsonParser *p = json_parser_new();
        if (json_parser_load_from_data(p, win, -1, NULL) &&
            json_parser_get_root(p) &&
            JSON_NODE_HOLDS_OBJECT(json_parser_get_root(p))) {
            JsonObject *o = json_node_get_object(json_parser_get_root(p));
            const char *cls =
                json_object_get_string_member_with_default(o, "class", "");
            if (cls && strstr(cls, "codium")) {
                gint64 mon_id =
                    json_object_get_int_member_with_default(o, "monitor",
                                                            -1);
                // resolve the monitor id to its name
                char *mons = NULL;
                g_spawn_command_line_sync("hyprctl monitors -j", &mons,
                                          NULL, NULL, NULL);
                if (mons) {
                    JsonParser *pm = json_parser_new();
                    if (json_parser_load_from_data(pm, mons, -1, NULL)) {
                        JsonArray *arr =
                            json_node_get_array(json_parser_get_root(pm));
                        for (guint i = 0; i < json_array_get_length(arr);
                             i++) {
                            JsonObject *m =
                                json_array_get_object_element(arr, i);
                            if (json_object_get_int_member(m, "id") ==
                                mon_id)
                                g_strlcpy(pill_mon,
                                          json_object_get_string_member(
                                              m, "name"),
                                          sizeof(pill_mon));
                        }
                    }
                    g_object_unref(pm);
                    g_free(mons);
                }
            }
        }
        g_object_unref(p);
        g_free(win);
    }
    for (guint i = 0; i < tb_wins->len; i++) {
        TbWin *tw = g_ptr_array_index(tb_wins, i);
        tw_apply_mode(tw, *pill_mon &&
                              g_str_equal(tw->bar->hypr_name, pill_mon));
    }
    tb_apply_visibility(); // a focused VSCodium can summon/dismiss it
}

static guint refocus_id;

static gboolean refocus_cb(gpointer data) {
    (void)data;
    refocus_id = 0;
    tb_refocus_now();
    return G_SOURCE_REMOVE;
}

// hypr.c calls this on focus/workspace events — debounced, they're chatty
void toolbar_refocus(void) {
    if (!refocus_id)
        refocus_id = g_timeout_add(120, refocus_cb, NULL);
}

static gboolean music_on; // yt music playing (or within the pause grace)
static gboolean mon_on;   // a direct-monitoring loopback is running
static char mon_label[160];
static char mon_src[256]; // source of the loopback the bar is showing

void toolbar_monitor_poke(void);

// mic-off button: unload the shown input's loopback (if more inputs are
// monitored, the bar moves on to the next one)
static void on_tb_monoff(GtkWidget *b, gpointer d) {
    (void)b;
    (void)d;
    if (!*mon_src)
        return;
    guint id = quickset_loopback_for(mon_src);
    if (id) {
        char *cmd = g_strdup_printf("pactl unload-module %u", id);
        g_spawn_command_line_sync(cmd, NULL, NULL, NULL, NULL);
        g_free(cmd);
    }
    toolbar_monitor_poke();
}

// present the content row for the current audio source: music keeps its
// transport controls; input monitoring is a mic + the input's name
static void tw_refresh_content(TbWin *tw) {
    if (music_on) {
        gtk_label_set_text(GTK_LABEL(tw->icon), "\U000F075A");
        for (int i = 0; i < 3; i++)
            gtk_widget_set_visible(tw->ctl[i], TRUE);
        gtk_widget_set_visible(tw->mon_off, FALSE);
    } else if (mon_on) {
        gtk_label_set_text(GTK_LABEL(tw->icon), "\U000F036C"); // mic
        gtk_label_set_text(GTK_LABEL(tw->title), mon_label);
        for (int i = 0; i < 3; i++)
            gtk_widget_set_visible(tw->ctl[i], FALSE);
        gtk_widget_set_visible(tw->mon_off, TRUE);
    }
}

// one window on/off, with its chrome inset and morph housekeeping
static void tw_set_shown(TbWin *tw, gboolean on) {
    if (on) {
        if (!gtk_widget_get_visible(tw->win)) {
            gtk_widget_show_all(tw->win); // map → hyprland slides down
            // menubar visibility is mode-managed, not show_all's call
            if (tw->pill) {
                gtk_widget_set_no_show_all(tw->menubar, FALSE);
                gtk_widget_show_all(tw->menubar);
                gtk_widget_set_no_show_all(tw->menubar, TRUE);
            } else {
                gtk_widget_hide(tw->menubar);
            }
        }
        // the content row exists whenever some audio drives the bar
        gtk_widget_set_visible(tw->content, music_on || mon_on);
        tw_refresh_content(tw);
        if (tw->bar->tb_inset != TB_INSET) {
            tw->bar->tb_inset = TB_INSET;
            gtk_widget_queue_draw(tw->bar->frame);
        }
        tw_kick_anim(tw);
    } else {
        if (gtk_widget_get_visible(tw->win))
            gtk_widget_hide(tw->win); // unmap → hyprland slides it up
        if (tw->bar->tb_inset) { // chrome hole grows back
            tw->bar->tb_inset = 0;
            gtk_widget_queue_draw(tw->bar->frame);
        }
        if (tw->pill_anim) { // finish any morph instantly while hidden
            g_source_remove(tw->pill_anim);
            tw->pill_anim = 0;
        }
        tw->pill_ext = tw->pill ? 1.0 : 0.0;
        tw_update_margin(tw);
    }
}

// the visibility rule: music shows the bar on EVERY monitor (that
// provider's exception); a focused VSCodium shows it on ITS monitor
// (menubar only, unless music is also up)
static void tb_apply_visibility(void) {
    tb_sync_windows();
    gboolean any = FALSE;
    for (guint i = 0; tb_wins && i < tb_wins->len; i++) {
        TbWin *tw = g_ptr_array_index(tb_wins, i);
        gboolean want = music_on || mon_on || tw->pill;
        tw_set_shown(tw, want);
        any = any || want;
    }
    if (music_on || mon_on)
        viz_start();
    else
        viz_stop();
    (void)any;
}

// ---- direct-monitoring provider: any module-loopback (quickset's input
// monitoring) puts the bar up with the visualizer — the loopback feeds
// the default sink, which is exactly what cava is listening to ----

static void tb_check_monitoring(void) {
    gboolean was = mon_on;
    char first_src[256] = "";
    int count = 0;
    char *out = NULL;
    g_spawn_command_line_sync("pactl list modules short", &out, NULL, NULL,
                              NULL);
    if (out) {
        char **lines = g_strsplit(out, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            if (!strstr(lines[i], "module-loopback"))
                continue;
            const char *s = strstr(lines[i], "source=");
            if (!s)
                continue;
            s += 7;
            const char *e = s;
            while (*e && *e != ' ' && *e != '\t')
                e++;
            if (!count)
                g_strlcpy(first_src, s,
                          MIN((gsize)(e - s + 1), sizeof(first_src)));
            count++;
        }
        g_strfreev(lines);
        g_free(out);
    }
    mon_on = count > 0;
    g_strlcpy(mon_src, mon_on ? first_src : "", sizeof(mon_src));
    if (mon_on) {
        // human name for the monitored input
        char *desc = NULL;
        char *js = NULL;
        g_spawn_command_line_sync("pactl --format=json list sources", &js,
                                  NULL, NULL, NULL);
        if (js) {
            JsonParser *p = json_parser_new();
            if (json_parser_load_from_data(p, js, -1, NULL)) {
                JsonArray *arr =
                    json_node_get_array(json_parser_get_root(p));
                for (guint i = 0; i < json_array_get_length(arr); i++) {
                    JsonObject *o = json_array_get_object_element(arr, i);
                    const char *n =
                        json_object_get_string_member(o, "name");
                    if (n && g_str_equal(n, first_src))
                        desc = g_strdup(json_object_get_string_member(
                            o, "description"));
                }
            }
            g_object_unref(p);
            g_free(js);
        }
        if (count > 1)
            g_snprintf(mon_label, sizeof(mon_label),
                       "Monitoring · %s  +%d", desc ? desc : first_src,
                       count - 1);
        else
            g_snprintf(mon_label, sizeof(mon_label), "Monitoring · %s",
                       desc ? desc : first_src);
        g_free(desc);
    }
    if (was != mon_on) {
        tb_apply_visibility();
    } else if (mon_on) { // label may have changed (different input)
        for (guint i = 0; tb_wins && i < tb_wins->len; i++)
            tw_refresh_content(g_ptr_array_index(tb_wins, i));
    }
}

// quickset flips a monitoring switch → reflect it right away
void toolbar_monitor_poke(void) {
    tb_check_monitoring();
}

static void tb_hide(void) { // music went away
    if (grace_id) {
        g_source_remove(grace_id);
        grace_id = 0;
    }
    music_on = FALSE;
    tb_apply_visibility();
}

static gboolean grace_expired(gpointer data) {
    (void)data;
    grace_id = 0;
    tb_hide();
    return G_SOURCE_REMOVE;
}

static void tb_show(void) { // music is playing
    music_on = TRUE;
    tb_apply_visibility();
    tb_refocus_now(); // pick the right mode per monitor immediately
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
            } else if (music_on && !grace_id) {
                grace_id = g_timeout_add_seconds(TB_GRACE_S, grace_expired,
                                                 NULL);
            }
            for (guint i = 0; music_on && tb_wins && i < tb_wins->len;
                 i++) { // don't clobber the monitoring label
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
    tb_check_monitoring(); // catches loopbacks toggled outside quickset
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
    tb_check_monitoring();
    ev_spawn();
    g_timeout_add_seconds(5, tb_poll, NULL); // slow fallback only
}
