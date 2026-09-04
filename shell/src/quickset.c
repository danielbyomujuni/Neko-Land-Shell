// Quick settings popover: volume slider + audio output picker.
// Opens from a left-click on the volume module (or SIGUSR1 → focused monitor).

#include "nekobar.h"

#include <gtk-layer-shell/gtk-layer-shell.h>
#include <json-glib/json-glib.h>
#include <string.h>

static gboolean updating; // guard: we're setting the slider, not the user
static gboolean refresh_later_cb(gpointer data);

// ---- backend ----

#define VOLCAP_ICON_ZONE 44.0 // left region that toggles mute on click

static gboolean volcap_draw(GtkWidget *w, cairo_t *cr, gpointer data) {
    (void)data;
    double W = gtk_widget_get_allocated_width(w);
    double H = gtk_widget_get_allocated_height(w);
    double r = H / 2;
    double vol = CLAMP(cur_volume, 0.0, 1.0);

    // capsule track
    cairo_new_sub_path(cr);
    cairo_arc(cr, r, r, r, G_PI / 2, 3 * G_PI / 2);
    cairo_arc(cr, W - r, r, r, -G_PI / 2, G_PI / 2);
    cairo_close_path(cr);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.10);
    cairo_fill_preserve(cr);
    // fill: clip to the capsule, paint the filled fraction from the left
    cairo_clip(cr);
    if (!cur_muted && vol > 0.001) {
        cairo_rectangle(cr, 0, 0, W * vol, H);
        cairo_set_source_rgba(cr, 0xCD / 255.0, 0xD6 / 255.0, 0xF4 / 255.0,
                              0.95);
        cairo_fill(cr);
    }
    cairo_reset_clip(cr);

    // speaker glyph inside, iOS-style: dark over the fill, light over track
    const char *icon = cur_muted        ? "\U000F075F"
                       : vol < 0.34     ? "\U000F057F"
                       : vol < 0.67     ? "\U000F0580"
                                        : "\U000F057E";
    PangoLayout *pl = gtk_widget_create_pango_layout(w, icon);
    PangoFontDescription *fd =
        pango_font_description_from_string("JetBrainsMono Nerd Font 9");
    pango_layout_set_font_description(pl, fd);
    pango_font_description_free(fd);
    int iw, ih;
    pango_layout_get_pixel_size(pl, &iw, &ih);
    gboolean over_fill = !cur_muted && W * vol > 16 + iw;
    if (over_fill)
        cairo_set_source_rgba(cr, 0x11 / 255.0, 0x11 / 255.0, 0x1B / 255.0,
                              0.9);
    else
        cairo_set_source_rgba(cr, 0xCD / 255.0, 0xD6 / 255.0, 0xF4 / 255.0,
                              0.8);
    cairo_move_to(cr, 12, (H - ih) / 2);
    pango_cairo_show_layout(cr, pl);
    g_object_unref(pl);
    return TRUE;
}

static void volcap_redraw_all(void) {
    for (guint i = 0; i < bars->len; i++) {
        Bar *bar = g_ptr_array_index(bars, i);
        if (bar->qs_scale)
            gtk_widget_queue_draw(bar->qs_scale);
    }
}

static void volcap_set_from_x(GtkWidget *w, double x) {
    double W = gtk_widget_get_allocated_width(w);
    double vol = CLAMP(x / W, 0.0, 1.0);
    cur_volume = vol; // instant visual feedback
    char cmd[128];
    g_snprintf(cmd, sizeof(cmd),
               "wpctl set-volume -l 1.0 @DEFAULT_AUDIO_SINK@ %d%%",
               (int)(vol * 100 + 0.5));
    spawn_cmd(cmd);
    volcap_redraw_all();
}

static gboolean volcap_press(GtkWidget *w, GdkEventButton *ev,
                             gpointer data) {
    (void)data;
    if (ev->button != 1)
        return FALSE;
    if (ev->x < VOLCAP_ICON_ZONE) { // icon zone: toggle mute
        spawn_cmd("wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle");
        cur_muted = !cur_muted;
        volcap_redraw_all();
        g_timeout_add(250, refresh_later_cb, NULL);
        return TRUE;
    }
    g_object_set_data(G_OBJECT(w), "dragging", GINT_TO_POINTER(1));
    volcap_set_from_x(w, ev->x);
    return TRUE;
}

static gboolean volcap_release(GtkWidget *w, GdkEventButton *ev,
                               gpointer data) {
    (void)ev;
    (void)data;
    g_object_set_data(G_OBJECT(w), "dragging", GINT_TO_POINTER(0));
    return TRUE;
}

static gboolean volcap_motion(GtkWidget *w, GdkEventMotion *ev,
                              gpointer data) {
    (void)data;
    if (g_object_get_data(G_OBJECT(w), "dragging"))
        volcap_set_from_x(w, ev->x);
    return TRUE;
}

static void rebuild_sinks(Bar *bar);

// circle button beside the slider: expands the output-device picker
// (macOS control-center style)
static void on_out_btn(GtkWidget *btn, gpointer data) {
    (void)btn;
    Bar *bar = data;
    GtkRevealer *rev = g_object_get_data(G_OBJECT(bar->qs_popover),
                                         "out-revealer");
    gboolean open = !gtk_revealer_get_reveal_child(rev);
    if (open)
        rebuild_sinks(bar);
    gtk_revealer_set_reveal_child(rev, open);
}

// ---- input monitoring ----
//
// direct monitoring of the audio interface's inputs: a module-loopback
// per source routes it into the default output at low latency. State
// lives in pipewire itself (the loaded modules), so the switches always
// reflect reality even across bar restarts.

static GHashTable *load_hidden(void); // defined with the sink picker

// id of the loopback module feeding from a source, 0 when none runs
static guint loopback_module_for(const char *src) {
    char *out = NULL;
    guint id = 0;
    if (!g_spawn_command_line_sync("pactl list modules short", &out, NULL,
                                   NULL, NULL) ||
        !out)
        return 0;
    char *needle = g_strdup_printf("source=%s", src);
    gsize nl = strlen(needle);
    char **lines = g_strsplit(out, "\n", -1);
    for (int i = 0; lines[i] && !id; i++) {
        if (!strstr(lines[i], "module-loopback"))
            continue;
        char *pos = strstr(lines[i], needle);
        if (pos && (pos[nl] == ' ' || pos[nl] == '\t' || pos[nl] == '\0'))
            id = (guint)atoi(lines[i]);
    }
    g_strfreev(lines);
    g_free(needle);
    g_free(out);
    return id;
}

static gboolean on_mon_switch(GtkSwitch *sw, gboolean state, gpointer data) {
    (void)sw;
    const char *src = data;
    if (state) {
        if (!loopback_module_for(src)) {
            char *cmd = g_strdup_printf(
                "pactl load-module module-loopback source=%s latency_msec=5",
                src);
            g_spawn_command_line_sync(cmd, NULL, NULL, NULL, NULL);
            g_free(cmd);
        }
    } else {
        guint id = loopback_module_for(src);
        if (id) {
            char *cmd = g_strdup_printf("pactl unload-module %u", id);
            g_spawn_command_line_sync(cmd, NULL, NULL, NULL, NULL);
            g_free(cmd);
        }
    }
    return FALSE; // let the switch flip
}

static void mon_src_free(gpointer data, GClosure *closure) {
    (void)closure;
    g_free(data);
}

// one row per hardware capture source (alsa_input.*)
static void rebuild_monitors(Bar *bar) {
    GtkWidget *box =
        g_object_get_data(G_OBJECT(bar->qs_popover), "mon-box");
    if (!box)
        return;
    GList *kids = gtk_container_get_children(GTK_CONTAINER(box));
    for (GList *l = kids; l; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(kids);

    char *out = NULL;
    if (!g_spawn_command_line_sync("pactl --format=json list sources", &out,
                                   NULL, NULL, NULL) ||
        !out)
        return;
    GHashTable *hidden = load_hidden(); // Sound → Advanced hides sources too
    JsonParser *p = json_parser_new();
    if (json_parser_load_from_data(p, out, -1, NULL)) {
        JsonArray *arr = json_node_get_array(json_parser_get_root(p));
        for (guint i = 0; i < json_array_get_length(arr); i++) {
            JsonObject *o = json_array_get_object_element(arr, i);
            const char *name = json_object_get_string_member(o, "name");
            const char *desc =
                json_object_get_string_member(o, "description");
            if (!name || !g_str_has_prefix(name, "alsa_input.") ||
                g_hash_table_contains(hidden, name))
                continue;
            GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
            GtkWidget *lbl = gtk_label_new(desc ? desc : name);
            gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
            gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
            gtk_label_set_max_width_chars(GTK_LABEL(lbl), 30);
            // NO hexpand here: the property propagates up to the panel's
            // column and re-centers the whole card mid-surface; box pack
            // expansion below does the same job without propagating
            gtk_widget_set_name(lbl, "qs-mon-label");
            gtk_box_pack_start(GTK_BOX(row), lbl, TRUE, TRUE, 0);
            GtkWidget *sw = gtk_switch_new();
            gtk_widget_set_valign(sw, GTK_ALIGN_CENTER);
            gtk_switch_set_active(GTK_SWITCH(sw),
                                  loopback_module_for(name) != 0);
            g_signal_connect_data(sw, "state-set",
                                  G_CALLBACK(on_mon_switch),
                                  g_strdup(name), mon_src_free, 0);
            gtk_box_pack_end(GTK_BOX(row), sw, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
        }
    }
    g_hash_table_destroy(hidden);
    g_object_unref(p);
    g_free(out);
    gtk_widget_show_all(box);
}

// circle mic button: expands the input-monitoring section
static void on_mon_btn(GtkWidget *btn, gpointer data) {
    (void)btn;
    Bar *bar = data;
    GtkRevealer *rev = g_object_get_data(G_OBJECT(bar->qs_popover),
                                         "mon-revealer");
    gboolean open = !gtk_revealer_get_reveal_child(rev);
    if (open)
        rebuild_monitors(bar);
    gtk_revealer_set_reveal_child(rev, open);
}

static gboolean volcap_scroll(GtkWidget *w, GdkEventScroll *ev,
                              gpointer data) {
    (void)w;
    (void)data;
    if (ev->direction == GDK_SCROLL_UP)
        cur_volume = CLAMP(cur_volume + 0.05, 0.0, 1.0);
    else if (ev->direction == GDK_SCROLL_DOWN)
        cur_volume = CLAMP(cur_volume - 0.05, 0.0, 1.0);
    else
        return FALSE;
    char cmd[128];
    g_snprintf(cmd, sizeof(cmd),
               "wpctl set-volume -l 1.0 @DEFAULT_AUDIO_SINK@ %d%%",
               (int)(cur_volume * 100 + 0.5));
    spawn_cmd(cmd);
    volcap_redraw_all();
    return TRUE;
}

static gboolean refresh_later_cb(gpointer data) {
    (void)data;
    volume_refresh();
    return FALSE;
}

typedef struct {
    char *name;
    char *desc;
    gboolean is_default;
    gboolean available; // active port not reported as unplugged
} Sink;

static void sink_free(gpointer p) {
    Sink *s = p;
    g_free(s->name);
    g_free(s->desc);
    g_free(s);
}

// sinks hidden via nekoland-settings (Sound → Advanced)
static GHashTable *load_hidden(void) {
    GHashTable *set =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    char *path = g_build_filename(g_get_user_config_dir(), "nekoland",
                                  "hidden-audio.conf", NULL);
    char *data = NULL;
    if (g_file_get_contents(path, &data, NULL, NULL)) {
        char **lines = g_strsplit(data, "\n", -1);
        for (char **l = lines; *l; l++) {
            g_strstrip(*l);
            if (**l)
                g_hash_table_add(set, g_strdup(*l));
        }
        g_strfreev(lines);
        g_free(data);
    }
    g_free(path);
    return set;
}

static GPtrArray *list_sinks(void) {
    GPtrArray *sinks = g_ptr_array_new_with_free_func(sink_free);
    GHashTable *hidden = load_hidden();

    char *def = NULL;
    char *argv_def[] = {"pactl", "get-default-sink", NULL};
    g_spawn_sync(NULL, argv_def, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, &def,
                 NULL, NULL, NULL);
    if (def)
        g_strchomp(def);

    char *out = NULL;
    char *argv[] = {"pactl", "--format=json", "list", "sinks", NULL};
    if (g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, &out,
                     NULL, NULL, NULL) &&
        out) {
        JsonParser *p = json_parser_new();
        if (json_parser_load_from_data(p, out, -1, NULL)) {
            JsonArray *arr = json_node_get_array(json_parser_get_root(p));
            for (guint i = 0; i < json_array_get_length(arr); i++) {
                JsonObject *o = json_array_get_object_element(arr, i);
                const char *name = json_object_get_string_member(o, "name");
                if (g_hash_table_contains(hidden, name))
                    continue;
                Sink *s = g_new0(Sink, 1);
                s->name = g_strdup(name);
                s->desc =
                    g_strdup(json_object_get_string_member(o, "description"));
                s->is_default = def && g_str_equal(s->name, def);

                // WirePlumber won't switch to a sink whose active port is
                // unplugged; reflect that in the UI
                s->available = TRUE;
                const char *ap = NULL;
                if (json_object_has_member(o, "active_port"))
                    ap = json_object_get_string_member(o, "active_port");
                JsonArray *ports =
                    json_object_has_member(o, "ports")
                        ? json_object_get_array_member(o, "ports")
                        : NULL;
                for (guint j = 0; ap && ports && j < json_array_get_length(ports);
                     j++) {
                    JsonObject *port = json_array_get_object_element(ports, j);
                    if (g_str_equal(json_object_get_string_member(port, "name"),
                                    ap)) {
                        const char *avail = json_object_get_string_member(
                            port, "availability");
                        s->available = !g_str_equal(avail, "not available");
                        break;
                    }
                }
                g_ptr_array_add(sinks, s);
            }
        }
        g_object_unref(p);
    }
    g_hash_table_destroy(hidden);
    g_free(out);
    g_free(def);
    return sinks;
}

// ---- sink rows ----

static void rebuild_sinks(Bar *bar);

typedef struct {
    Bar *bar;
    char *name;
} SinkClick;

static gboolean sinks_rebuild_cb(gpointer data) {
    rebuild_sinks(data);
    volume_refresh();
    return FALSE;
}

static void on_sink_clicked(GtkWidget *btn, gpointer data) {
    (void)btn;
    SinkClick *sc = data;
    char *cmd = g_strdup_printf("pactl set-default-sink %s", sc->name);
    spawn_cmd(cmd);
    g_free(cmd);
    g_timeout_add(300, sinks_rebuild_cb, sc->bar);
}

static void sink_click_free(gpointer data, GClosure *closure) {
    (void)closure;
    SinkClick *sc = data;
    g_free(sc->name);
    g_free(sc);
}

static void rebuild_sinks(Bar *bar) {
    GList *kids = gtk_container_get_children(GTK_CONTAINER(bar->qs_sink_box));
    for (GList *l = kids; l; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(kids);

    GPtrArray *sinks = list_sinks();
    for (guint i = 0; i < sinks->len; i++) {
        Sink *s = g_ptr_array_index(sinks, i);
        char *text = g_strdup_printf("%s %s", s->is_default ? "󰄬" : " ",
                                     s->desc);
        GtkWidget *btn = gtk_button_new_with_label(text);
        g_free(text);
        GtkWidget *lbl = gtk_bin_get_child(GTK_BIN(btn));
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
        gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(lbl), 38);
        gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);
        GtkStyleContext *sc = gtk_widget_get_style_context(btn);
        gtk_style_context_add_class(sc, "sink-row");
        if (s->is_default)
            gtk_style_context_add_class(sc, "active-sink");
        if (!s->available) {
            gtk_style_context_add_class(sc, "sink-unavailable");
            gtk_widget_set_sensitive(btn, FALSE);
        }

        SinkClick *click = g_new0(SinkClick, 1);
        click->bar = bar;
        click->name = g_strdup(s->name);
        g_signal_connect_data(btn, "clicked", G_CALLBACK(on_sink_clicked),
                              click, sink_click_free, 0);
        gtk_box_pack_start(GTK_BOX(bar->qs_sink_box), btn, FALSE, FALSE, 0);
    }
    g_ptr_array_free(sinks, TRUE);
    gtk_widget_show_all(bar->qs_sink_box);
}

// ---- sync from modules.c volume poller ----

void quickset_sync(void) {
    updating = TRUE;
    volcap_redraw_all();
    updating = FALSE;
}

// ---- panel window ----
// A dedicated layer-shell window: GTK popovers get clipped on layer surfaces,
// so the panel is its own surface anchored below the bar's right edge (the
// bar's exclusive zone pushes it down automatically).

static gboolean qs_click_off(Bar *bar); // defined with the morph code

// gear button: open the full settings app and retract the panel
static void on_settings_btn(GtkWidget *btn, gpointer data) {
    (void)btn;
    (void)data;
    spawn_cmd("gtk-launch nekoland-settings");
    quickset_autoclose();
}

// concave glass fillets where the panel's rims tee into the sidebar and
// the bottom border — drawn on THIS surface so the frost matches the
// panel glass exactly (blur is per-surface)
static gboolean qs_draw_bg(GtkWidget *w, cairo_t *cr, gpointer data) {
    (void)data;
    GtkWidget *box = g_object_get_data(G_OBJECT(w), "qs-box");
    int fx = 0, fy = 0;
    if (!box ||
        !gtk_widget_translate_coordinates(box, w, 0, 0, &fx, &fy))
        return FALSE;
    double r = NEKO_FRAME_R;
    double px = fx + NEKO_LAUNCH_W; // panel's right edge
    double bottom = fy + gtk_widget_get_allocated_height(box);

    cairo_set_source_rgba(cr, 0x11 / 255.0, 0x11 / 255.0, 0x1B / 255.0,
                          0.45);
    // top-left tee: wedge between the sidebar wall and the top rim
    cairo_move_to(cr, fx, fy - r);
    cairo_arc_negative(cr, fx + r, fy - r, r, G_PI, G_PI / 2);
    cairo_line_to(cr, fx, fy);
    cairo_close_path(cr);
    cairo_fill(cr);
    // bottom-right tee: wedge between the right rim and the bottom border
    cairo_move_to(cr, px, bottom - r);
    cairo_arc_negative(cr, px + r, bottom - r, r, G_PI, G_PI / 2);
    cairo_line_to(cr, px, bottom);
    cairo_close_path(cr);
    cairo_fill(cr);

    // rim strokes along the fillet arcs
    cairo_set_line_width(cr, 2);
    cairo_set_source_rgb(cr, 0x1E / 255.0, 0x1E / 255.0, 0x2E / 255.0);
    cairo_new_path(cr);
    cairo_arc_negative(cr, fx + r, fy - r, r, G_PI, G_PI / 2);
    cairo_stroke(cr);
    cairo_new_path(cr);
    cairo_arc_negative(cr, px + r, bottom - r, r, G_PI, G_PI / 2);
    cairo_stroke(cr);
    return FALSE;
}

// the chrome's corner morph needs the panel's real height
static void qs_frame_sized(GtkWidget *w, GdkRectangle *alloc,
                           gpointer data) {
    (void)w;
    Bar *bar = data;
    if (bar->qs_h != alloc->height || bar->qs_w != alloc->width) {
        bar->qs_h = alloc->height;
        bar->qs_w = alloc->width;
        gtk_widget_queue_draw(bar->frame);
    }
}

void quickset_attach(Bar *bar, GtkWidget *anchor) {
    (void)anchor;
    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    bar->qs_popover = win;
    gtk_widget_set_name(win, "quickset");

    gtk_layer_init_for_window(GTK_WINDOW(win));
    gtk_layer_set_layer(GTK_WINDOW(win), GTK_LAYER_SHELL_LAYER_TOP);
    gtk_layer_set_namespace(GTK_WINDOW(win), "nekobar-quickset");
    gtk_layer_set_monitor(GTK_WINDOW(win), bar->gdk_monitor);
    // bottom-left corner card, flush against the sidebar and bottom
    // border — same glass language as the app launcher
    // full-monitor surface: the panel hugs the bottom-left corner and
    // the rest is a transparent click-catcher (click off = close)
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
    gtk_layer_set_margin(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_LEFT,
                         NEKO_FRAME_W);
    gtk_layer_set_margin(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_BOTTOM,
                         NEKO_FRAME_W);

    GdkScreen *screen = gtk_widget_get_screen(win);
    GdkVisual *rgba = gdk_screen_get_rgba_visual(screen);
    if (rgba)
        gtk_widget_set_visual(win, rgba);
    gtk_widget_set_app_paintable(win, TRUE);

    GtkWidget *frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_name(frame, "quickset-box");
    gtk_widget_set_size_request(frame, NEKO_LAUNCH_W, -1);
    g_signal_connect(frame, "size-allocate", G_CALLBACK(qs_frame_sized),
                     bar);

    // spacers extend the surface over the bevel wedge areas
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *top_sp = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(top_sp, -1, NEKO_FRAME_R);
    gtk_box_pack_start(GTK_BOX(outer), top_sp, FALSE, FALSE, 0);
    GtkWidget *bevel_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *right_sp = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(right_sp, NEKO_FRAME_R, -1);
    gtk_box_pack_start(GTK_BOX(bevel_row), frame, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bevel_row), right_sp, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), bevel_row, TRUE, TRUE, 0);
    g_signal_connect(win, "draw", G_CALLBACK(qs_draw_bg), bar);
    g_object_set_data(G_OBJECT(win), "qs-box", frame);

    GtkWidget *rootbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *leftcol = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *topcatch = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(topcatch), FALSE);
    gtk_widget_set_vexpand(topcatch, TRUE);
    g_signal_connect_swapped(topcatch, "button-press-event",
                             G_CALLBACK(qs_click_off), bar);
    gtk_box_pack_start(GTK_BOX(leftcol), topcatch, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(leftcol), outer, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(rootbox), leftcol, FALSE, FALSE, 0);
    GtkWidget *rightcatch = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(rightcatch), FALSE);
    gtk_widget_set_hexpand(rightcatch, TRUE);
    g_signal_connect_swapped(rightcatch, "button-press-event",
                             G_CALLBACK(qs_click_off), bar);
    gtk_box_pack_start(GTK_BOX(rootbox), rightcatch, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(win), rootbox);

    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(v), 12);
    gtk_box_pack_start(GTK_BOX(frame), v, FALSE, FALSE, 0);

    // volume: iOS-style capsule — the fill is the control, the speaker
    // icon inside toggles mute, drag anywhere to set
    bar->qs_scale = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(bar->qs_scale), FALSE);
    // the drawing area has its own GdkWindow that would eat the clicks;
    // the event box's input window must sit above it
    gtk_event_box_set_above_child(GTK_EVENT_BOX(bar->qs_scale), TRUE);
    GtkWidget *volarea = gtk_drawing_area_new();
    gtk_widget_set_size_request(volarea, 220, 26);
    g_signal_connect(volarea, "draw", G_CALLBACK(volcap_draw), NULL);
    gtk_container_add(GTK_CONTAINER(bar->qs_scale), volarea);
    gtk_widget_add_events(bar->qs_scale, GDK_BUTTON_PRESS_MASK |
                                             GDK_BUTTON_RELEASE_MASK |
                                             GDK_POINTER_MOTION_MASK |
                                             GDK_SCROLL_MASK);
    g_signal_connect(bar->qs_scale, "scroll-event",
                     G_CALLBACK(volcap_scroll), NULL);
    g_signal_connect(bar->qs_scale, "button-press-event",
                     G_CALLBACK(volcap_press), NULL);
    g_signal_connect(bar->qs_scale, "button-release-event",
                     G_CALLBACK(volcap_release), NULL);
    g_signal_connect(bar->qs_scale, "motion-notify-event",
                     G_CALLBACK(volcap_motion), NULL);
    bar->qs_mute_label = NULL; // capsule shows mute state itself

    // slider row: capsule + round output-picker button
    GtkWidget *volrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(volrow), bar->qs_scale, TRUE, TRUE, 0);
    GtkWidget *outbtn = gtk_button_new_with_label("\U000F02CB");
    gtk_button_set_relief(GTK_BUTTON(outbtn), GTK_RELIEF_NONE);
    gtk_widget_set_name(outbtn, "qs-out-btn");
    gtk_widget_set_size_request(outbtn, 30, 30);
    gtk_widget_set_valign(outbtn, GTK_ALIGN_CENTER);
    g_signal_connect(outbtn, "clicked", G_CALLBACK(on_out_btn), bar);
    gtk_box_pack_start(GTK_BOX(volrow), outbtn, FALSE, FALSE, 0);
    GtkWidget *monbtn = gtk_button_new_with_label("\U000F036C");
    gtk_button_set_relief(GTK_BUTTON(monbtn), GTK_RELIEF_NONE);
    gtk_widget_set_name(monbtn, "qs-out-btn"); // same round-button style
    gtk_widget_set_size_request(monbtn, 30, 30);
    gtk_widget_set_valign(monbtn, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(monbtn, "Input monitoring");
    g_signal_connect(monbtn, "clicked", G_CALLBACK(on_mon_btn), bar);
    gtk_box_pack_start(GTK_BOX(volrow), monbtn, FALSE, FALSE, 0);
    GtkWidget *setbtn = gtk_button_new_with_label("\U000F0493");
    gtk_button_set_relief(GTK_BUTTON(setbtn), GTK_RELIEF_NONE);
    gtk_widget_set_name(setbtn, "qs-out-btn"); // same round-button style
    gtk_widget_set_size_request(setbtn, 30, 30);
    gtk_widget_set_valign(setbtn, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(setbtn, "All settings");
    g_signal_connect(setbtn, "clicked", G_CALLBACK(on_settings_btn), bar);
    gtk_box_pack_start(GTK_BOX(volrow), setbtn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), volrow, FALSE, FALSE, 0);

    // output picker, collapsed until the circle button opens it
    GtkWidget *rev = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(rev),
                                     GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    gtk_revealer_set_transition_duration(GTK_REVEALER(rev), 200);
    g_object_set_data(G_OBJECT(win), "out-revealer", rev);
    GtkWidget *outv = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);

    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(outv), sep, FALSE, FALSE, 2);

    GtkWidget *hdr = gtk_label_new("Output");
    gtk_label_set_xalign(GTK_LABEL(hdr), 0.0);
    gtk_widget_set_name(hdr, "qs-header");
    gtk_box_pack_start(GTK_BOX(outv), hdr, FALSE, FALSE, 0);

    bar->qs_sink_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_pack_start(GTK_BOX(outv), bar->qs_sink_box, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(rev), outv);
    gtk_box_pack_start(GTK_BOX(v), rev, FALSE, FALSE, 0);

    // input monitoring, collapsed until the mic button opens it
    GtkWidget *mrev = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(mrev),
                                     GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    gtk_revealer_set_transition_duration(GTK_REVEALER(mrev), 200);
    g_object_set_data(G_OBJECT(win), "mon-revealer", mrev);
    GtkWidget *mv = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);

    GtkWidget *msep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(mv), msep, FALSE, FALSE, 2);

    GtkWidget *mhdr = gtk_label_new("Input monitoring");
    gtk_label_set_xalign(GTK_LABEL(mhdr), 0.0);
    gtk_widget_set_name(mhdr, "qs-header");
    gtk_box_pack_start(GTK_BOX(mv), mhdr, FALSE, FALSE, 0);

    GtkWidget *mbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    g_object_set_data(G_OBJECT(win), "mon-box", mbox);
    gtk_box_pack_start(GTK_BOX(mv), mbox, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(mrev), mv);
    gtk_box_pack_start(GTK_BOX(v), mrev, FALSE, FALSE, 0);
}

static gint64 qs_last_autoclose_us;

static gboolean qs_tick_cb(GtkWidget *w, GdkFrameClock *clock,
                           gpointer data) {
    (void)w;
    Bar *bar = data;
    gint64 now = gdk_frame_clock_get_frame_time(clock);
    double dt = CLAMP((now - bar->qs_last_us) / 1e6, 0.0, 0.05);
    bar->qs_last_us = now;
    double target = bar->qs_target;
    bar->qs_ext += (target - bar->qs_ext) * MIN(1.0, 14.0 * dt);
    if (ABS(target - bar->qs_ext) < 0.004)
        bar->qs_ext = target;
    gtk_widget_queue_draw(bar->frame);
    if (bar->qs_popover)
        gtk_widget_set_opacity(bar->qs_popover,
                               bar->qs_ext * bar->qs_ext);
    if (bar->qs_ext == target) {
        if (target == 0 && bar->qs_popover)
            gtk_widget_hide(bar->qs_popover);
        bar->qs_tick = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void qs_animate(Bar *bar, int target) {
    bar->qs_target = target;
    if (!bar->qs_tick) {
        GdkFrameClock *clock = gtk_widget_get_frame_clock(bar->frame);
        bar->qs_last_us = clock ? gdk_frame_clock_get_frame_time(clock) : 0;
        bar->qs_tick =
            gtk_widget_add_tick_callback(bar->frame, qs_tick_cb, bar, NULL);
    }
}

static gboolean qs_click_off(Bar *bar) {
    qs_last_autoclose_us = g_get_monotonic_time();
    qs_animate(bar, 0);
    return TRUE;
}

// backstop driven from hypr.c: a window took focus / mouse left monitor
void quickset_autoclose(void) {
    for (guint i = 0; i < bars->len; i++) {
        Bar *bar = g_ptr_array_index(bars, i);
        if (bar->qs_target == 1) {
            qs_last_autoclose_us = g_get_monotonic_time();
            qs_animate(bar, 0);
        }
    }
}

void quickset_toggle(Bar *bar) {
    if (!bar->qs_popover)
        return;
    if (bar->qs_target == 1) {
        qs_animate(bar, 0); // chrome retracts, panel fades
    } else {
        // the click that just auto-closed it shouldn't reopen it
        if (g_get_monotonic_time() - qs_last_autoclose_us < 400000)
            return;
        rebuild_sinks(bar);
        quickset_sync();
        gtk_widget_set_opacity(bar->qs_popover, 0.0);
        gtk_widget_show_all(bar->qs_popover);
        GtkRevealer *rev = g_object_get_data(G_OBJECT(bar->qs_popover),
                                             "out-revealer");
        if (rev)
            gtk_revealer_set_reveal_child(rev, FALSE);
        GtkRevealer *mrev = g_object_get_data(G_OBJECT(bar->qs_popover),
                                              "mon-revealer");
        if (mrev) { // NEKOBAR_QS_TEST opens the monitoring list for debug
            gboolean test = g_getenv("NEKOBAR_QS_TEST") != NULL;
            if (test)
                rebuild_monitors(bar);
            gtk_revealer_set_reveal_child(mrev, test);
        }
        qs_animate(bar, 1);
    }
}

void quickset_toggle_focused(void) {
    char name[64] = "";
    Bar *target = bars->len ? g_ptr_array_index(bars, 0) : NULL;
    if (hypr_focused_monitor(name, sizeof(name))) {
        for (guint i = 0; i < bars->len; i++) {
            Bar *bar = g_ptr_array_index(bars, i);
            if (g_str_equal(bar->hypr_name, name)) {
                target = bar;
                break;
            }
        }
    }
    if (target)
        quickset_toggle(target);
}
