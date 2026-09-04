// Sound page, styled after the Settings.dc.html mockup: macOS form rows
// (right-aligned label column), OUTPUT/INPUT device cards with type tags and
// accent checkmarks, balance control, and a live input level meter.
// Backends: pactl --format=json (devices, volume, balance) + parec (mic
// level) + cava (spectrum visualizer).

#include "app.h"

#include <json-glib/json-glib.h>
#include <math.h>
#include <string.h>

#define VIZ_BARS 48
#define METER_SEGS 7

static GtkWidget *out_list, *in_list;
static GtkWidget *out_scale, *out_pct;
static GtkWidget *bal_scale, *bal_val;
static GtkWidget *meter_seg[METER_SEGS];
static gboolean updating;
static char out_sig[1024], in_sig[1024];

// current output state (needed to preserve balance when setting volume)
static double cur_vol;     // 0..1
static double cur_balance; // -1..1

// devices hidden from Settings and the bar's quick settings
// (shared config: ~/.config/nekoland/hidden-audio.conf, one name per line)
static GHashTable *hidden;

static char *hidden_path(void) {
    return g_build_filename(g_get_user_config_dir(), "nekoland",
                            "hidden-audio.conf", NULL);
}

static void hidden_load(void) {
    if (!hidden)
        hidden = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_hash_table_remove_all(hidden);
    char *path = hidden_path();
    char *data = NULL;
    if (g_file_get_contents(path, &data, NULL, NULL)) {
        char **lines = g_strsplit(data, "\n", -1);
        for (char **l = lines; *l; l++) {
            g_strstrip(*l);
            if (**l)
                g_hash_table_add(hidden, g_strdup(*l));
        }
        g_strfreev(lines);
        g_free(data);
    }
    g_free(path);
}

static void hidden_save(void) {
    GString *s = g_string_new(NULL);
    GHashTableIter it;
    gpointer key;
    g_hash_table_iter_init(&it, hidden);
    while (g_hash_table_iter_next(&it, &key, NULL))
        g_string_append_printf(s, "%s\n", (char *)key);
    char *path = hidden_path();
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    g_file_set_contents(path, s->str, -1, NULL);
    g_free(dir);
    g_free(path);
    g_string_free(s, TRUE);
}

// ---- process helpers ----

static char *run_argv(char **argv) {
    char *out = NULL;
    if (!g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, &out,
                      NULL, NULL, NULL))
        return NULL;
    return out;
}

static void run_cmd(const char *shell_cmd) {
    char *argv[] = {"sh", "-c", (char *)shell_cmd, NULL};
    g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL,
                  NULL);
}

// ---- device listing ----

typedef struct {
    char *name;
    char *desc;
    gboolean is_default;
    gboolean available;
    double volume;  // 0..1, max channel (default device only)
    double balance; // -1..1
    gboolean muted;
    // set for "potential" outputs that need a card-profile switch first
    // (e.g. the Valve Index on the GPU's second HDMI/DP audio output)
    char *card;
    char *profile;
} Dev;

static void dev_free(gpointer p) {
    Dev *d = p;
    g_free(d->name);
    g_free(d->desc);
    g_free(d->card);
    g_free(d->profile);
    g_free(d);
}

// outputs that exist only under a different card profile (single-profile
// ALSA cards expose one HDMI/DP audio port at a time — the Valve Index
// lives on the GPU card's extra2 profile while a monitor holds stereo)
static void add_profile_outputs(GPtrArray *devs, gboolean include_hidden) {
    char *argv[] = {"pactl", "--format=json", "list", "cards", NULL};
    char *out = run_argv(argv);
    if (!out)
        return;
    JsonParser *p = json_parser_new();
    if (json_parser_load_from_data(p, out, -1, NULL)) {
        JsonArray *cards = json_node_get_array(json_parser_get_root(p));
        for (guint i = 0; i < json_array_get_length(cards); i++) {
            JsonObject *card = json_array_get_object_element(cards, i);
            const char *cname = json_object_get_string_member(card, "name");
            const char *active =
                json_object_has_member(card, "active_profile")
                    ? json_object_get_string_member(card, "active_profile")
                    : "";
            if (!g_str_has_prefix(cname, "alsa_card."))
                continue;
            JsonObject *ports =
                json_object_has_member(card, "ports")
                    ? json_object_get_object_member(card, "ports")
                    : NULL;
            if (!ports)
                continue;
            GList *pnames = json_object_get_members(ports);
            for (GList *l = pnames; l; l = l->next) {
                JsonObject *port =
                    json_object_get_object_member(ports, l->data);
                const char *avail =
                    json_object_has_member(port, "availability")
                        ? json_object_get_string_member(port,
                                                        "availability")
                        : "";
                if (g_str_equal(avail, "not available"))
                    continue;
                if (!json_object_has_member(port, "profiles"))
                    continue;
                JsonArray *profs =
                    json_object_get_array_member(port, "profiles");
                // preferred profile: first stereo output on this port
                const char *prof = NULL;
                for (guint k = 0; k < json_array_get_length(profs); k++) {
                    const char *pr =
                        json_array_get_string_element(profs, k);
                    if (g_str_has_prefix(pr, "output:") &&
                        !strstr(pr, "surround")) {
                        prof = pr;
                        break;
                    }
                }
                if (!prof || g_str_equal(prof, active))
                    continue; // no output profile / already active

                // predicted sink name once the profile is switched
                char *sink = g_strdup_printf(
                    "alsa_output.%s.%s", cname + strlen("alsa_card."),
                    prof + strlen("output:"));
                gboolean dup = FALSE;
                for (guint k = 0; k < devs->len && !dup; k++)
                    dup = g_str_equal(
                        ((Dev *)g_ptr_array_index(devs, k))->name, sink);
                if (dup || (!include_hidden && hidden &&
                            g_hash_table_contains(hidden, sink))) {
                    g_free(sink);
                    continue;
                }

                const char *desc = NULL;
                if (json_object_has_member(port, "properties")) {
                    JsonObject *props =
                        json_object_get_object_member(port, "properties");
                    if (json_object_has_member(props,
                                               "device.product.name"))
                        desc = json_object_get_string_member(
                            props, "device.product.name");
                }
                if (!desc)
                    desc = json_object_has_member(port, "description")
                               ? json_object_get_string_member(
                                     port, "description")
                               : sink;

                Dev *d = g_new0(Dev, 1);
                d->name = sink;
                d->desc = g_strdup(desc);
                d->available = TRUE;
                d->card = g_strdup(cname);
                d->profile = g_strdup(prof);
                g_ptr_array_add(devs, d);
            }
            g_list_free(pnames);
        }
    }
    g_object_unref(p);
    g_free(out);
}

static const char *transport_tag(const char *name) {
    if (strstr(name, "usb-"))
        return "USB";
    if (strstr(name, "hdmi"))
        return "HDMI";
    if (strstr(name, "bluez"))
        return "Bluetooth";
    if (strstr(name, "pci-"))
        return "Built-in";
    return "";
}

static double parse_volume(JsonObject *o) {
    if (!json_object_has_member(o, "volume"))
        return 0;
    JsonObject *vol = json_object_get_object_member(o, "volume");
    GList *members = json_object_get_members(vol);
    double best = 0;
    for (GList *l = members; l; l = l->next) {
        JsonObject *ch = json_object_get_object_member(vol, l->data);
        const char *pct = json_object_get_string_member(ch, "value_percent");
        double v = g_ascii_strtod(pct, NULL) / 100.0;
        best = MAX(best, v);
    }
    g_list_free(members);
    return best;
}

static GPtrArray *list_devices(gboolean input, gboolean include_hidden) {
    GPtrArray *devs = g_ptr_array_new_with_free_func(dev_free);

    char *argv_def[] = {"pactl",
                        input ? "get-default-source" : "get-default-sink",
                        NULL};
    char *def = run_argv(argv_def);
    if (def)
        g_strchomp(def);

    char *argv[] = {"pactl", "--format=json", "list",
                    input ? "sources" : "sinks", NULL};
    char *out = run_argv(argv);
    if (!out) {
        g_free(def);
        return devs;
    }

    JsonParser *p = json_parser_new();
    if (json_parser_load_from_data(p, out, -1, NULL)) {
        JsonArray *arr = json_node_get_array(json_parser_get_root(p));
        for (guint i = 0; i < json_array_get_length(arr); i++) {
            JsonObject *o = json_array_get_object_element(arr, i);
            const char *name = json_object_get_string_member(o, "name");

            // skip sink monitors in the input list
            if (input && (strstr(name, ".monitor") ||
                          (json_object_has_member(o, "monitor_of_sink") &&
                           !json_object_get_null_member(o, "monitor_of_sink"))))
                continue;

            if (!include_hidden && hidden && g_hash_table_contains(hidden, name))
                continue;

            Dev *d = g_new0(Dev, 1);
            d->name = g_strdup(name);
            d->desc =
                g_strdup(json_object_get_string_member(o, "description"));
            d->is_default = def && g_str_equal(d->name, def);
            d->volume = parse_volume(o);
            d->balance = json_object_has_member(o, "balance")
                             ? json_object_get_double_member(o, "balance")
                             : 0;
            d->muted = json_object_has_member(o, "mute") &&
                       json_object_get_boolean_member(o, "mute");

            d->available = TRUE;
            const char *ap = NULL;
            if (json_object_has_member(o, "active_port"))
                ap = json_object_get_string_member(o, "active_port");
            JsonArray *ports = json_object_has_member(o, "ports")
                                   ? json_object_get_array_member(o, "ports")
                                   : NULL;
            for (guint j = 0;
                 ap && ports && j < json_array_get_length(ports); j++) {
                JsonObject *port = json_array_get_object_element(ports, j);
                if (g_str_equal(json_object_get_string_member(port, "name"),
                                ap)) {
                    d->available = !g_str_equal(
                        json_object_get_string_member(port, "availability"),
                        "not available");
                    break;
                }
            }
            g_ptr_array_add(devs, d);
        }
    }
    g_object_unref(p);
    g_free(out);
    g_free(def);
    if (!input)
        add_profile_outputs(devs, include_hidden);
    return devs;
}

// ---- device rows ----

typedef struct {
    char *name;
    gboolean input;
    char *card;    // when set: switch this card's profile first
    char *profile;
} DevClick;

static gboolean poke_refresh(gpointer data) {
    (void)data;
    audio_refresh();
    return FALSE;
}

static gboolean set_default_later(gpointer data) {
    char *name = data;
    char *cmd = g_strdup_printf("pactl set-default-sink %s", name);
    run_cmd(cmd);
    g_free(cmd);
    g_free(name);
    g_timeout_add(300, poke_refresh, NULL);
    return G_SOURCE_REMOVE;
}

static void on_dev_clicked(GtkWidget *btn, gpointer data) {
    (void)btn;
    DevClick *dc = data;
    if (dc->card) {
        // potential output: activate its card profile, then make the
        // resulting sink the default once it has appeared
        char *cmd = g_strdup_printf("pactl set-card-profile %s %s",
                                    dc->card, dc->profile);
        run_cmd(cmd);
        g_free(cmd);
        g_timeout_add(400, set_default_later, g_strdup(dc->name));
        return;
    }
    char *cmd = g_strdup_printf("pactl set-default-%s %s",
                                dc->input ? "source" : "sink", dc->name);
    run_cmd(cmd);
    g_free(cmd);
    g_timeout_add(300, poke_refresh, NULL);
}

static void dev_click_free(gpointer data, GClosure *closure) {
    (void)closure;
    DevClick *dc = data;
    g_free(dc->name);
    g_free(dc->card);
    g_free(dc->profile);
    g_free(dc);
}

static void clear_box(GtkWidget *box) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(box)))
        gtk_box_remove(GTK_BOX(box), child);
}

static GtkWidget *row_sep(void) {
    GtkWidget *s = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_add_css_class(s, "card-sep");
    return s;
}

static void rebuild_list(GtkWidget *box, gboolean input, char *sig,
                         gsize siglen) {
    GPtrArray *devs = list_devices(input, FALSE);

    GString *s = g_string_new(NULL);
    for (guint i = 0; i < devs->len; i++) {
        Dev *d = g_ptr_array_index(devs, i);
        g_string_append_printf(s, "%s|%d|%d;", d->name, d->is_default,
                               d->available);
    }
    if (g_str_equal(s->str, sig)) {
        g_string_free(s, TRUE);
        g_ptr_array_free(devs, TRUE);
        return;
    }
    g_strlcpy(sig, s->str, siglen);
    g_string_free(s, TRUE);

    clear_box(box);
    for (guint i = 0; i < devs->len; i++) {
        Dev *d = g_ptr_array_index(devs, i);

        if (i > 0)
            gtk_box_append(GTK_BOX(box), row_sep());

        GtkWidget *row = gtk_button_new();
        gtk_widget_add_css_class(row, "device-row");

        GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        GtkWidget *lbl = gtk_label_new(d->desc);
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
        gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
        gtk_widget_set_hexpand(lbl, TRUE);
        gtk_box_append(GTK_BOX(h), lbl);

        GtkWidget *tag = gtk_label_new(transport_tag(d->name));
        gtk_widget_add_css_class(tag, "dev-tag");
        gtk_box_append(GTK_BOX(h), tag);

        GtkWidget *check = gtk_label_new("✓");
        gtk_widget_add_css_class(check, "device-check");
        gtk_widget_set_opacity(check, d->is_default ? 1.0 : 0.0);
        gtk_box_append(GTK_BOX(h), check);
        gtk_button_set_child(GTK_BUTTON(row), h);

        if (!d->available) {
            gtk_widget_add_css_class(row, "unavailable");
            gtk_widget_set_sensitive(row, FALSE);
        }

        DevClick *dc = g_new0(DevClick, 1);
        dc->name = g_strdup(d->name);
        dc->input = input;
        dc->card = g_strdup(d->card);
        dc->profile = g_strdup(d->profile);
        g_signal_connect_data(row, "clicked", G_CALLBACK(on_dev_clicked), dc,
                              dev_click_free, 0);
        gtk_box_append(GTK_BOX(box), row);
    }
    g_ptr_array_free(devs, TRUE);
}

// ---- output volume + balance ----

static void apply_output_channels(void) {
    // preserve balance when setting volume (and vice versa):
    // L = V * min(1, 1-b), R = V * min(1, 1+b)
    double l = cur_vol * MIN(1.0, 1.0 - cur_balance);
    double r = cur_vol * MIN(1.0, 1.0 + cur_balance);
    char cmd[160];
    g_snprintf(cmd, sizeof(cmd),
               "pactl set-sink-volume @DEFAULT_SINK@ %d%% %d%%",
               (int)(l * 100 + 0.5), (int)(r * 100 + 0.5));
    run_cmd(cmd);
}

static void on_volume_changed(GtkRange *range, gpointer data) {
    (void)data;
    if (updating)
        return;
    cur_vol = gtk_range_get_value(range) / 100.0;
    char buf[16];
    g_snprintf(buf, sizeof(buf), "%d%%", (int)(cur_vol * 100 + 0.5));
    gtk_label_set_text(GTK_LABEL(out_pct), buf); // live, don't wait for tick
    apply_output_channels();
}

static void on_balance_changed(GtkRange *range, gpointer data) {
    (void)data;
    if (updating)
        return;
    cur_balance = gtk_range_get_value(range) / 50.0;
    char buf[16];
    g_snprintf(buf, sizeof(buf), "%d", (int)(cur_balance * 50 + 0.5));
    gtk_label_set_text(GTK_LABEL(bal_val), buf);
    apply_output_channels();
}

static void sync_output_state(void) {
    GPtrArray *devs = list_devices(FALSE, TRUE);
    for (guint i = 0; i < devs->len; i++) {
        Dev *d = g_ptr_array_index(devs, i);
        if (!d->is_default)
            continue;
        cur_vol = d->volume;
        cur_balance = d->balance;
        updating = TRUE;
        gtk_range_set_value(GTK_RANGE(out_scale), cur_vol * 100.0);
        gtk_range_set_value(GTK_RANGE(bal_scale), cur_balance * 50.0);
        updating = FALSE;
        char buf[16];
        g_snprintf(buf, sizeof(buf), "%d%%", (int)(cur_vol * 100 + 0.5));
        gtk_label_set_text(GTK_LABEL(out_pct), buf);
        g_snprintf(buf, sizeof(buf), "%d", (int)(cur_balance * 50 + 0.5));
        gtk_label_set_text(GTK_LABEL(bal_val), buf);
        break;
    }
    g_ptr_array_free(devs, TRUE);
}

// ---- input level meter (parec on the default source) ----

static GSubprocess *parec_proc;
static GInputStream *parec_stream;
static char meter_src[256];
static guchar meter_buf[1024];
static double meter_peak;

static void meter_update_segments(void) {
    int lit = (int)(pow(meter_peak, 0.5) * METER_SEGS + 0.5);
    for (int i = 0; i < METER_SEGS; i++) {
        if (i < lit)
            gtk_widget_add_css_class(meter_seg[i], "on");
        else
            gtk_widget_remove_css_class(meter_seg[i], "on");
    }
}

static void meter_read_cb(GObject *src, GAsyncResult *res, gpointer data);

static void meter_read_next(void) {
    g_input_stream_read_async(parec_stream, meter_buf, sizeof(meter_buf),
                              G_PRIORITY_DEFAULT, NULL, meter_read_cb, NULL);
}

static void meter_read_cb(GObject *src, GAsyncResult *res, gpointer data) {
    (void)data;
    gssize n = g_input_stream_read_finish(G_INPUT_STREAM(src), res, NULL);
    if (n <= 0)
        return; // parec gone; restarted on next refresh if needed
    const gint16 *samples = (const gint16 *)meter_buf;
    double peak = 0;
    for (gssize i = 0; i < n / 2; i++)
        peak = MAX(peak, ABS(samples[i]) / 32768.0);
    // fast attack, slow decay
    meter_peak = peak > meter_peak ? peak : meter_peak * 0.8;
    meter_update_segments();
    meter_read_next();
}

static void meter_start(const char *source) {
    if (parec_proc) {
        g_subprocess_force_exit(parec_proc);
        g_clear_object(&parec_proc);
        parec_stream = NULL;
    }
    g_strlcpy(meter_src, source, sizeof(meter_src));
    // low latency: without --latency-msec the record stream buffers ~2s
    parec_proc = g_subprocess_new(
        G_SUBPROCESS_FLAGS_STDOUT_PIPE, NULL, "parec", "--raw",
        "--format=s16le", "--rate=8000", "--channels=1",
        "--latency-msec=30", "-d", source, NULL);
    if (!parec_proc)
        return;
    parec_stream = g_subprocess_get_stdout_pipe(parec_proc);
    meter_read_next();
}

static void meter_follow_default(void) {
    char *argv[] = {"pactl", "get-default-source", NULL};
    char *def = run_argv(argv);
    if (!def)
        return;
    g_strchomp(def);
    if (*def && !g_str_equal(def, meter_src))
        meter_start(def);
    g_free(def);
}

// ---- visualizer (cava in raw ascii mode -> GtkDrawingArea) ----

static GtkWidget *viz_area;
static double viz_vals[VIZ_BARS];
static GSubprocess *cava_proc;
static GDataInputStream *cava_out;

static void viz_draw(GtkDrawingArea *area, cairo_t *cr, int w, int h,
                     gpointer data) {
    (void)data;
    GdkRGBA color;
    gtk_widget_get_color(GTK_WIDGET(area), &color);
    gdk_cairo_set_source_rgba(cr, &color);

    double gap = 3.0;
    double bw = (w - gap * (VIZ_BARS - 1)) / VIZ_BARS;
    for (int i = 0; i < VIZ_BARS; i++) {
        double bh = MAX(2.0, viz_vals[i] / 100.0 * h);
        double x = i * (bw + gap);
        double y = h - bh;
        double r = MIN(bw / 2, 2.5);
        cairo_new_sub_path(cr);
        cairo_arc(cr, x + r, y + r, r, G_PI, 1.5 * G_PI);
        cairo_arc(cr, x + bw - r, y + r, r, 1.5 * G_PI, 2 * G_PI);
        cairo_line_to(cr, x + bw, h);
        cairo_line_to(cr, x, h);
        cairo_close_path(cr);
        cairo_fill(cr);
    }
}

static void viz_read_cb(GObject *src, GAsyncResult *res, gpointer data);

static void viz_read_next(void) {
    g_data_input_stream_read_line_async(cava_out, G_PRIORITY_DEFAULT, NULL,
                                        viz_read_cb, NULL);
}

static void viz_read_cb(GObject *src, GAsyncResult *res, gpointer data) {
    (void)data;
    gsize len;
    char *line = g_data_input_stream_read_line_finish(
        G_DATA_INPUT_STREAM(src), res, &len, NULL);
    if (!line)
        return;
    char **parts = g_strsplit(line, ";", -1);
    for (int i = 0; i < VIZ_BARS && parts[i] && *parts[i]; i++)
        viz_vals[i] = g_ascii_strtod(parts[i], NULL);
    g_strfreev(parts);
    g_free(line);
    if (viz_area)
        gtk_widget_queue_draw(viz_area);
    viz_read_next();
}

static void viz_start(void) {
    char *conf = g_build_filename(g_get_user_runtime_dir(),
                                  "nekoland-cava.conf", NULL);
    char *cfg = g_strdup_printf(
        "[general]\nbars = %d\nframerate = 30\n"
        "[input]\nmethod = pulse\nsource = auto\n"
        "[output]\nmethod = raw\nraw_target = /dev/stdout\n"
        "data_format = ascii\nascii_max_range = 100\n",
        VIZ_BARS);
    g_file_set_contents(conf, cfg, -1, NULL);
    g_free(cfg);

    cava_proc = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE, NULL, "cava",
                                 "-p", conf, NULL);
    g_free(conf);
    if (!cava_proc)
        return;
    cava_out = g_data_input_stream_new(
        g_subprocess_get_stdout_pipe(cava_proc));
    viz_read_next();
}

void audio_shutdown(void) {
    if (cava_proc)
        g_subprocess_force_exit(cava_proc);
    if (parec_proc)
        g_subprocess_force_exit(parec_proc);
}

// ---- advanced modal: hide devices from Settings + quick settings ----

static void free_closure_data(gpointer data, GClosure *closure) {
    (void)closure;
    g_free(data);
}

static void on_visible_toggled(GObject *sw, GParamSpec *spec, gpointer data) {
    (void)spec;
    const char *name = data;
    gboolean visible = gtk_switch_get_active(GTK_SWITCH(sw));
    if (visible)
        g_hash_table_remove(hidden, name);
    else
        g_hash_table_add(hidden, g_strdup(name));
    hidden_save();
    out_sig[0] = in_sig[0] = '\0';
    audio_refresh();
}

static void modal_add_devices(GtkWidget *box, gboolean input) {
    GtkWidget *c = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(c, "nk-card");
    GPtrArray *devs = list_devices(input, TRUE);
    for (guint i = 0; i < devs->len; i++) {
        Dev *d = g_ptr_array_index(devs, i);
        if (i > 0)
            gtk_box_append(GTK_BOX(c), row_sep());
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_add_css_class(row, "adv-row");
        GtkWidget *lbl = gtk_label_new(d->desc);
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
        gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
        gtk_widget_set_hexpand(lbl, TRUE);
        gtk_box_append(GTK_BOX(row), lbl);

        GtkWidget *sw = gtk_switch_new();
        gtk_switch_set_active(GTK_SWITCH(sw),
                              !g_hash_table_contains(hidden, d->name));
        gtk_widget_set_valign(sw, GTK_ALIGN_CENTER);
        g_signal_connect_data(sw, "notify::active",
                              G_CALLBACK(on_visible_toggled),
                              g_strdup(d->name), free_closure_data, 0);
        gtk_box_append(GTK_BOX(row), sw);
        gtk_box_append(GTK_BOX(c), row);
    }
    g_ptr_array_free(devs, TRUE);
    gtk_box_append(GTK_BOX(box), c);
}

void audio_open_advanced(GtkWindow *parent) {
    GtkWidget *win = gtk_window_new();
    gtk_widget_set_name(win, "adv-modal");
    gtk_window_set_transient_for(GTK_WINDOW(win), parent);
    gtk_window_set_modal(GTK_WINDOW(win), TRUE);
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(win), 440, 520);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    // slim header, same style as the pane header (close dot overlays left)
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(header, "pane-header");
    GtkWidget *title = gtk_label_new("Advanced");
    gtk_widget_add_css_class(title, "pane-title");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0);
    gtk_widget_set_margin_start(title, 24); // clear the close dot
    gtk_box_append(GTK_BOX(header), title);
    gtk_box_append(GTK_BOX(root), header);

    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class(v, "page");

    GtkWidget *hint = gtk_label_new(
        "Switched-off devices are hidden from Settings\n"
        "and the bar's quick settings.");
    gtk_label_set_xalign(GTK_LABEL(hint), 0.0);
    gtk_widget_add_css_class(hint, "hint-label");
    gtk_box_append(GTK_BOX(v), hint);

    GtkWidget *out_lbl = gtk_label_new("OUTPUTS");
    gtk_label_set_xalign(GTK_LABEL(out_lbl), 0.0);
    gtk_widget_add_css_class(out_lbl, "section-label");
    gtk_box_append(GTK_BOX(v), out_lbl);
    modal_add_devices(v, FALSE);

    GtkWidget *in_lbl = gtk_label_new("INPUTS");
    gtk_label_set_xalign(GTK_LABEL(in_lbl), 0.0);
    gtk_widget_add_css_class(in_lbl, "section-label");
    gtk_box_append(GTK_BOX(v), in_lbl);
    modal_add_devices(v, TRUE);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), v);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(root), scroll);

    GtkWidget *overlay = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(overlay), root);
    GtkWidget *close_btn = gtk_button_new();
    gtk_widget_add_css_class(close_btn, "close-dot");
    gtk_widget_set_halign(close_btn, GTK_ALIGN_START);
    gtk_widget_set_valign(close_btn, GTK_ALIGN_START);
    gtk_widget_set_margin_start(close_btn, 12);
    gtk_widget_set_margin_top(close_btn, 16); // centered in the 44px header
    g_signal_connect_swapped(close_btn, "clicked",
                             G_CALLBACK(gtk_window_close), win);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), close_btn);

    gtk_window_set_child(GTK_WINDOW(win), overlay);
    gtk_window_present(GTK_WINDOW(win));
}

static void on_advanced_clicked(GtkWidget *btn, gpointer data) {
    (void)data;
    GtkRoot *root = gtk_widget_get_root(btn);
    audio_open_advanced(GTK_WINDOW(root));
}

// ---- page assembly ----

void audio_refresh(void) {
    rebuild_list(out_list, FALSE, out_sig, sizeof(out_sig));
    rebuild_list(in_list, TRUE, in_sig, sizeof(in_sig));
    sync_output_state();
    meter_follow_default();
}

static gboolean tick(gpointer data) {
    (void)data;
    audio_refresh();
    return TRUE;
}

static GtkWidget *section_label(const char *text) {
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0);
    gtk_widget_add_css_class(l, "section-label");
    return l;
}

static GtkWidget *card(void) {
    GtkWidget *c = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(c, "nk-card");
    return c;
}

// macOS-style form row: right-aligned 150px label + control
static GtkWidget *form_row(const char *label, GtkWidget *control) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_widget_add_css_class(row, "form-row");
    GtkWidget *l = gtk_label_new(label);
    gtk_widget_add_css_class(l, "form-label");
    gtk_label_set_xalign(GTK_LABEL(l), 0.0);
    gtk_widget_set_size_request(l, 150, -1);
    gtk_box_append(GTK_BOX(row), l);
    gtk_widget_set_hexpand(control, TRUE);
    gtk_box_append(GTK_BOX(row), control);
    return row;
}

static GtkWidget *slider_control(GtkWidget **scale_out, GtkWidget **val_out,
                                 double min, double max,
                                 GCallback on_changed) {
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *scale =
        gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, min, max, 1);
    gtk_widget_set_hexpand(scale, TRUE);
    g_signal_connect(scale, "value-changed", on_changed, NULL);
    *scale_out = scale;
    gtk_box_append(GTK_BOX(h), scale);
    GtkWidget *val = gtk_label_new("");
    gtk_widget_add_css_class(val, "pct-label");
    gtk_label_set_width_chars(GTK_LABEL(val), 4);
    gtk_label_set_xalign(GTK_LABEL(val), 1.0);
    *val_out = val;
    gtk_box_append(GTK_BOX(h), val);
    return h;
}

GtkWidget *audio_page_new(void) {
    hidden_load();

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    // slim pane header, like the mockup
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(header, "pane-header");
    GtkWidget *chev = gtk_label_new("‹");
    gtk_widget_add_css_class(chev, "pane-chevron");
    gtk_box_append(GTK_BOX(header), chev);
    GtkWidget *ptitle = gtk_label_new("Sound");
    gtk_widget_add_css_class(ptitle, "pane-title");
    gtk_label_set_xalign(GTK_LABEL(ptitle), 0.0);
    gtk_box_append(GTK_BOX(header), ptitle);
    gtk_box_append(GTK_BOX(root), header);

    // 560px centered column
    GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 20);
    gtk_widget_add_css_class(col, "content-col");
    gtk_widget_set_size_request(col, 560, -1);
    gtk_widget_set_halign(col, GTK_ALIGN_CENTER);

    // visualizer card
    GtkWidget *viz_card = card();
    gtk_widget_add_css_class(viz_card, "viz-card");
    viz_area = gtk_drawing_area_new();
    gtk_widget_add_css_class(viz_area, "viz");
    gtk_widget_set_size_request(viz_area, -1, 64);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(viz_area), viz_draw, NULL,
                                   NULL);
    gtk_box_append(GTK_BOX(viz_card), viz_area);
    gtk_box_append(GTK_BOX(col), viz_card);
    viz_start();

    // volume / balance form card
    GtkWidget *form = card();
    gtk_box_append(GTK_BOX(form),
                   form_row("Output volume",
                            slider_control(&out_scale, &out_pct, 0, 100,
                                           G_CALLBACK(on_volume_changed))));
    gtk_box_append(GTK_BOX(form), row_sep());
    gtk_box_append(GTK_BOX(form),
                   form_row("Balance",
                            slider_control(&bal_scale, &bal_val, -50, 50,
                                           G_CALLBACK(on_balance_changed))));
    gtk_box_append(GTK_BOX(col), form);

    // output devices
    gtk_box_append(GTK_BOX(col), section_label("OUTPUT"));
    out_list = card();
    gtk_box_append(GTK_BOX(col), out_list);

    // input devices + level meter
    gtk_box_append(GTK_BOX(col), section_label("INPUT"));
    in_list = card();
    gtk_box_append(GTK_BOX(col), in_list);

    GtkWidget *meter_card = card();
    GtkWidget *meter = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
    gtk_widget_set_valign(meter, GTK_ALIGN_CENTER);
    for (int i = 0; i < METER_SEGS; i++) {
        meter_seg[i] = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(meter_seg[i], "meter-seg");
        gtk_widget_set_hexpand(meter_seg[i], TRUE);
        gtk_box_append(GTK_BOX(meter), meter_seg[i]);
    }
    gtk_box_append(GTK_BOX(meter_card), form_row("Input level", meter));
    gtk_box_append(GTK_BOX(col), meter_card);

    // advanced button, bottom right
    GtkWidget *adv_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *adv = gtk_button_new_with_label("Advanced…");
    gtk_widget_add_css_class(adv, "advanced-btn");
    gtk_widget_set_halign(adv, GTK_ALIGN_END);
    gtk_widget_set_hexpand(adv, TRUE);
    g_signal_connect(adv, "clicked", G_CALLBACK(on_advanced_clicked), NULL);
    gtk_box_append(GTK_BOX(adv_row), adv);
    gtk_box_append(GTK_BOX(col), adv_row);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), col);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(root), scroll);

    audio_refresh();
    g_timeout_add(2000, tick, NULL);
    return root;
}
