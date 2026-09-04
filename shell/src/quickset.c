// Quick settings popover: volume slider + audio output picker.
// Opens from a left-click on the volume module (or SIGUSR1 → focused monitor).

#include "nekobar.h"

#include <gtk-layer-shell/gtk-layer-shell.h>
#include <json-glib/json-glib.h>
#include <string.h>

static gboolean updating; // guard: we're setting the slider, not the user

// ---- backend ----

static void on_scale_changed(GtkRange *range, gpointer data) {
    (void)data;
    if (updating)
        return;
    char cmd[128];
    g_snprintf(cmd, sizeof(cmd),
               "wpctl set-volume -l 1.0 @DEFAULT_AUDIO_SINK@ %d%%",
               (int)gtk_range_get_value(range));
    spawn_cmd(cmd);
}

static gboolean refresh_later_cb(gpointer data) {
    (void)data;
    volume_refresh();
    return FALSE;
}

static void on_mute_clicked(GtkWidget *btn, gpointer data) {
    (void)btn;
    (void)data;
    spawn_cmd("wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle");
    g_timeout_add(250, refresh_later_cb, NULL);
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
    for (guint i = 0; i < bars->len; i++) {
        Bar *bar = g_ptr_array_index(bars, i);
        if (!bar->qs_scale)
            continue;
        gtk_range_set_value(GTK_RANGE(bar->qs_scale), cur_volume * 100.0);
        gtk_label_set_text(GTK_LABEL(bar->qs_mute_label),
                           cur_muted ? "\U000F075F" : "\U000F057E");
    }
    updating = FALSE;
}

// ---- panel window ----
// A dedicated layer-shell window: GTK popovers get clipped on layer surfaces,
// so the panel is its own surface anchored below the bar's right edge (the
// bar's exclusive zone pushes it down automatically).

static gboolean qs_click_off(Bar *bar); // defined with the morph code

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
    if (bar->qs_h != alloc->height) {
        bar->qs_h = alloc->height;
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

    // volume row: mute toggle + slider
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *mute = gtk_button_new_with_label("\U000F057E");
    bar->qs_mute_label = gtk_bin_get_child(GTK_BIN(mute));
    gtk_button_set_relief(GTK_BUTTON(mute), GTK_RELIEF_NONE);
    gtk_widget_set_name(mute, "qs-mute");
    g_signal_connect(mute, "clicked", G_CALLBACK(on_mute_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(row), mute, FALSE, FALSE, 0);

    bar->qs_scale =
        gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
    gtk_scale_set_draw_value(GTK_SCALE(bar->qs_scale), FALSE);
    gtk_widget_set_size_request(bar->qs_scale, 220, -1);
    gtk_widget_set_name(bar->qs_scale, "qs-scale");
    g_signal_connect(bar->qs_scale, "value-changed",
                     G_CALLBACK(on_scale_changed), NULL);
    gtk_box_pack_start(GTK_BOX(row), bar->qs_scale, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(v), row, FALSE, FALSE, 0);

    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(v), sep, FALSE, FALSE, 2);

    GtkWidget *hdr = gtk_label_new("Output");
    gtk_label_set_xalign(GTK_LABEL(hdr), 0.0);
    gtk_widget_set_name(hdr, "qs-header");
    gtk_box_pack_start(GTK_BOX(v), hdr, FALSE, FALSE, 0);

    bar->qs_sink_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_pack_start(GTK_BOX(v), bar->qs_sink_box, FALSE, FALSE, 0);
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
