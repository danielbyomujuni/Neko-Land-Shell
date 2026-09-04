// RGB page, built to the current Settings.dc.html mockup:
//  - DEVICES: a row of selectable chips (status square + type + product
//    subtitle); clicking a chip swaps the detail card below
//  - detail card: darker header strip (device name · "Lighting" · on/off
//    switch) and form rows — Effect, Colour dots + custom, Brightness, Speed
//  - PROFILES: snapshots of the whole state (rgb-profiles.ini), unchanged
//
// Devices come from pluggable providers (rgb.h); all hardware in this
// machine is handled natively, the OpenRGB CLI fallback covers strangers.

#include "app.h"
#include "kraken_lcd.h"
#include "rgb.h"

#include <string.h>

static const RgbProvider *providers[] = {
    &rgb_ene_provider,
    &rgb_hue2_provider,
    &rgb_gpu_provider,
    &rgb_asrock_provider,
    &rgb_razer_provider,
    &rgb_logitech_provider,
    &rgb_openrgb_provider, // optional fallback, keep last
    NULL,
};

// rgb_device_new/free + rgb_claimed_locations live in rgb_common.c,
// shared with the headless nekoland-rgb-restore binary

// saturated palette: catppuccin pastels render near-white on LEDs, so the
// dots carry fully saturated hues that look on-hardware like they do here
static const struct {
    const char *css_class;
    GdkRGBA rgba;
} dots[] = {
    {"dot-red", {1.000, 0.000, 0.000, 1}},    // #FF0000
    {"dot-orange", {1.000, 0.478, 0.000, 1}}, // #FF7A00
    {"dot-yellow", {1.000, 0.831, 0.000, 1}}, // #FFD400
    {"dot-green", {0.000, 1.000, 0.266, 1}},  // #00FF44
    {"dot-cyan", {0.000, 0.898, 1.000, 1}},   // #00E5FF
    {"dot-blue", {0.000, 0.266, 1.000, 1}},   // #0044FF
    {"dot-magenta", {0.898, 0.000, 1.000, 1}},// #E600FF
};

// ---- page state ----

static GtkWidget *chips_box;  // device chip row
static GtkWidget *detail_box; // detail card, rebuilt on selection
static GtkWidget *status_label;
static GtkWidget *profiles_box;
static GPtrArray *devices; // RgbDevice*

// chips are device TYPES: one group per type, controls fan out to members
typedef struct {
    char *label;
    GPtrArray *members; // RgbDevice* (borrowed from `devices`)
} DevGroup;

static GPtrArray *groups; // DevGroup*
static DevGroup *selected;
static gboolean per_member;   // segmented "Per stick" mode
static guint member_idx;      // which member is targeted in per-member mode

static guint target_count(void) {
    if (!selected)
        return 0;
    return per_member ? 1 : selected->members->len;
}

static RgbDevice *target_at(guint i) {
    if (per_member)
        return g_ptr_array_index(selected->members,
                                 MIN(member_idx, selected->members->len - 1));
    return g_ptr_array_index(selected->members, i);
}

static void dev_group_free(gpointer p) {
    DevGroup *g = p;
    g_free(g->label);
    g_ptr_array_free(g->members, TRUE);
    g_free(g);
}

static RgbDevice *group_lead(DevGroup *g) {
    return g_ptr_array_index(g->members, 0);
}

static void rebuild_groups(void) {
    if (groups)
        g_ptr_array_free(groups, TRUE);
    groups = g_ptr_array_new_with_free_func(dev_group_free);
    for (guint i = 0; devices && i < devices->len; i++) {
        RgbDevice *d = g_ptr_array_index(devices, i);
        const char *label = d->type ? d->type : "Other";
        DevGroup *g = NULL;
        for (guint j = 0; j < groups->len; j++) {
            DevGroup *cand = g_ptr_array_index(groups, j);
            if (g_str_equal(cand->label, label))
                g = cand;
        }
        if (!g) {
            g = g_new0(DevGroup, 1);
            g->label = g_strdup(label);
            g->members = g_ptr_array_new();
            g_ptr_array_add(groups, g);
        }
        g_ptr_array_add(g->members, d);
    }
}
static gboolean rgb_updating;
static char *active_profile;

static void rebuild_chips(void);
static void rebuild_detail(void);
static void rebuild_profiles(void);

static void clear_box(GtkWidget *box) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(box)))
        gtk_box_remove(GTK_BOX(box), child);
}

static GtkWidget *row_sep(void) {
    GtkWidget *s = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_add_css_class(s, "nk-card-sep");
    return s;
}

// ---- applying state to hardware ----

static const char *device_mode_name(RgbDevice *d) {
    if (d->modes && d->cur_mode >= 0 && (guint)d->cur_mode < d->modes->len)
        return g_ptr_array_index(d->modes, d->cur_mode);
    return "Static";
}

static int find_mode(RgbDevice *d, const char *name) {
    if (!d->modes)
        return -1;
    for (guint m = 0; m < d->modes->len; m++)
        if (!g_ascii_strcasecmp(g_ptr_array_index(d->modes, m), name))
            return (int)m;
    return -1;
}

static gboolean mode_is_cycling(const char *mode) {
    return !g_ascii_strcasecmp(mode, "Rainbow") ||
           !g_ascii_strcasecmp(mode, "Spectrum Cycle") ||
           !g_ascii_strcasecmp(mode, "Super Rainbow") ||
           !g_ascii_strcasecmp(mode, "Color Cycle");
}

static gboolean mode_is_static(const char *mode) {
    return !g_ascii_strcasecmp(mode, "Static") ||
           !g_ascii_strcasecmp(mode, "Direct");
}

// ---- live state (rgb-state.ini) ----
//
// every change that reaches the hardware is also written (debounced) to
// ~/.config/nekoland/rgb-state.ini, one group per device id:
//   state = <mode>|<#hex>|<brightness>|<speed>|<enabled>
//   leds  = <#hex>;<#hex>;…            (per-LED-capable devices only)
// The settings app reads it back after a scan so the UI remembers the
// last setup, and nekoland-rgb-restore reapplies it at login.

static char *state_path(void) {
    return g_build_filename(g_get_user_config_dir(), "nekoland",
                            "rgb-state.ini", NULL);
}

static char *device_state_string(RgbDevice *d) {
    char hex[10];
    g_snprintf(hex, sizeof(hex), "#%02X%02X%02X",
               (int)(d->color.red * 255 + 0.5),
               (int)(d->color.green * 255 + 0.5),
               (int)(d->color.blue * 255 + 0.5));
    char bright[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_dtostr(bright, sizeof(bright), d->brightness);
    return g_strdup_printf("%s|%s|%s|%d|%d", device_mode_name(d), hex,
                           bright, d->speed, d->enabled ? 1 : 0);
}

static guint state_save_id;

static gboolean state_save_now(gpointer data) {
    (void)data;
    state_save_id = 0;
    GKeyFile *kf = g_key_file_new();
    if (active_profile)
        g_key_file_set_string(kf, "meta", "active-profile", active_profile);
    for (guint i = 0; devices && i < devices->len; i++) {
        RgbDevice *d = g_ptr_array_index(devices, i);
        char *val = device_state_string(d);
        g_key_file_set_string(kf, d->id, "state", val);
        // hidraw numbers shift across reboots: the name lets
        // nekoland-rgb-restore re-match devices whose id moved
        g_key_file_set_string(kf, d->id, "name", d->name);
        g_free(val);
        if (d->n_leds > 0) {
            GString *s = g_string_new(NULL);
            for (int l = 0; l < d->n_leds && l < RGB_MAX_LEDS; l++)
                g_string_append_printf(
                    s, "%s#%02X%02X%02X", l ? ";" : "",
                    (int)(d->led_colors[l].red * 255 + 0.5),
                    (int)(d->led_colors[l].green * 255 + 0.5),
                    (int)(d->led_colors[l].blue * 255 + 0.5));
            g_key_file_set_string(kf, d->id, "leds", s->str);
            g_string_free(s, TRUE);
        }
    }
    char *path = state_path();
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    g_key_file_save_to_file(kf, path, NULL);
    g_free(dir);
    g_free(path);
    g_key_file_free(kf);
    return G_SOURCE_REMOVE;
}

static void state_save_soon(void) {
    if (!state_save_id)
        state_save_id = g_timeout_add(400, state_save_now, NULL);
}

// after a scan: pull the remembered setup back into the device structs so
// the UI shows what was last configured (hardware is left untouched — it
// already carries this state, restored at login by nekoland-rgb-restore)
static void state_load_into_devices(void) {
    GKeyFile *kf = g_key_file_new();
    char *path = state_path();
    gboolean ok = g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL);
    g_free(path);
    if (!ok) {
        g_key_file_free(kf);
        return;
    }
    for (guint i = 0; devices && i < devices->len; i++) {
        RgbDevice *d = g_ptr_array_index(devices, i);
        char *val = g_key_file_get_string(kf, d->id, "state", NULL);
        if (val) {
            char **f = g_strsplit(val, "|", 5);
            if (g_strv_length(f) == 5) {
                int m = find_mode(d, f[0]);
                if (m >= 0)
                    d->cur_mode = m;
                gdk_rgba_parse(&d->color, f[1]);
                d->brightness = g_ascii_strtod(f[2], NULL);
                d->speed = atoi(f[3]);
                d->enabled = g_str_equal(f[4], "1");
            }
            g_strfreev(f);
            g_free(val);
        }
        char *leds = g_key_file_get_string(kf, d->id, "leds", NULL);
        if (leds && d->n_leds > 0) {
            char **c = g_strsplit(leds, ";", -1);
            for (int l = 0; c[l] && l < d->n_leds && l < RGB_MAX_LEDS; l++)
                gdk_rgba_parse(&d->led_colors[l], c[l]);
            g_strfreev(c);
        }
        g_free(leds);
    }
    char *prof = g_key_file_get_string(kf, "meta", "active-profile", NULL);
    if (prof) {
        g_free(active_profile);
        active_profile = prof;
    }
    g_key_file_free(kf);
}

static void apply_device(RgbDevice *d) {
    state_save_soon(); // remember every change that reaches the hardware
    if (!d->enabled) {
        int off = find_mode(d, "Off");
        if (off >= 0) {
            d->provider->set_mode(d, g_ptr_array_index(d->modes, off));
        } else {
            GdkRGBA black = {0, 0, 0, 1};
            d->provider->set_color(d, &black);
        }
        return;
    }
    const char *mode = device_mode_name(d);
    if (!g_ascii_strcasecmp(mode, "Static") ||
        !g_ascii_strcasecmp(mode, "Direct")) {
        GdkRGBA eff = rgb_effective_color(d);
        d->provider->set_color(d, &eff);
    } else {
        d->provider->set_mode(d, mode);
    }
}

// ---- device chips ----

static void chip_swatch_draw(GtkDrawingArea *area, cairo_t *cr, int w, int h,
                             gpointer data) {
    (void)area;
    RgbDevice *d = data;
    // clip to the rounded square
    double r = 2;
    cairo_new_sub_path(cr);
    cairo_arc(cr, w - r, r, r, -G_PI / 2, 0);
    cairo_arc(cr, w - r, h - r, r, 0, G_PI / 2);
    cairo_arc(cr, r, h - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, r, r, r, G_PI, 1.5 * G_PI);
    cairo_close_path(cr);
    cairo_clip(cr);

    if (!d->enabled) {
        cairo_set_source_rgb(cr, 0.35, 0.36, 0.44);
        cairo_paint(cr);
        return;
    }
    if (mode_is_cycling(device_mode_name(d))) {
        cairo_pattern_t *pat = cairo_pattern_create_linear(0, 0, w, h);
        static const double hues[5][3] = {
            {1, 0, 0}, {1, 0.9, 0}, {0, 1, 0.2}, {0, 0.6, 1}, {0.9, 0, 1}};
        for (int i = 0; i < 5; i++)
            cairo_pattern_add_color_stop_rgb(pat, i / 4.0, hues[i][0],
                                             hues[i][1], hues[i][2]);
        cairo_set_source(cr, pat);
        cairo_paint(cr);
        cairo_pattern_destroy(pat);
        return;
    }
    gdk_cairo_set_source_rgba(cr, &d->color);
    cairo_paint(cr);
}

static void on_chip_clicked(GtkWidget *btn, gpointer data) {
    (void)btn;
    selected = data;
    per_member = FALSE;
    member_idx = 0;
    rebuild_chips();
    rebuild_detail();
}

static void clear_flowbox(GtkWidget *fb) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(fb)))
        gtk_flow_box_remove(GTK_FLOW_BOX(fb), child);
}

static void rebuild_chips(void) {
    clear_flowbox(chips_box);
    if (!devices || devices->len == 0) {
        GtkWidget *hint = gtk_label_new(devices ? "No RGB devices found"
                                                : "Detecting devices…");
        gtk_widget_add_css_class(hint, "hint-label");
        gtk_flow_box_append(GTK_FLOW_BOX(chips_box), hint);
        return;
    }
    for (guint i = 0; groups && i < groups->len; i++) {
        DevGroup *g = g_ptr_array_index(groups, i);
        RgbDevice *lead = group_lead(g);
        GtkWidget *chip = gtk_button_new();
        gtk_widget_add_css_class(chip, "dev-chip");
        if (g == selected)
            gtk_widget_add_css_class(chip, "sel");

        GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
        GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *sw = gtk_drawing_area_new();
        gtk_widget_set_size_request(sw, 8, 8);
        gtk_widget_set_valign(sw, GTK_ALIGN_CENTER);
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(sw), chip_swatch_draw,
                                       lead, NULL);
        gtk_box_append(GTK_BOX(top), sw);
        GtkWidget *title = gtk_label_new(g->label);
        gtk_widget_add_css_class(title, "chip-title");
        gtk_label_set_xalign(GTK_LABEL(title), 0.0);
        gtk_box_append(GTK_BOX(top), title);
        gtk_box_append(GTK_BOX(v), top);
        char subtext[160];
        if (g->members->len > 1)
            g_snprintf(subtext, sizeof(subtext), "%s · ×%u", lead->name,
                       g->members->len);
        else
            g_snprintf(subtext, sizeof(subtext), "%s", lead->name);
        GtkWidget *sub = gtk_label_new(subtext);
        gtk_widget_add_css_class(sub, "chip-sub");
        gtk_label_set_xalign(GTK_LABEL(sub), 0.0);
        gtk_label_set_ellipsize(GTK_LABEL(sub), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(sub), 18);
        gtk_box_append(GTK_BOX(v), sub);
        gtk_button_set_child(GTK_BUTTON(chip), v);

        g_signal_connect(chip, "clicked", G_CALLBACK(on_chip_clicked), g);
        gtk_flow_box_append(GTK_FLOW_BOX(chips_box), chip);
    }
}

// ---- detail card ----

typedef struct {
    RgbDevice *dev;
    GdkRGBA color;
} DotClick;

static void dot_click_free(gpointer data, GClosure *closure) {
    (void)closure;
    g_free(data);
}

static void mark_selected(GtkWidget *dot) {
    GtkWidget *parent = gtk_widget_get_parent(dot);
    for (GtkWidget *c = gtk_widget_get_first_child(parent); c;
         c = gtk_widget_get_next_sibling(c))
        gtk_widget_remove_css_class(c, "sel");
    gtk_widget_add_css_class(dot, "sel");
}

static void device_take_color(RgbDevice *d, const GdkRGBA *c) {
    d->color = *c;
    for (int i = 0; i < RGB_MAX_LEDS; i++)
        d->led_colors[i] = *c;
    if (!d->enabled || !g_ascii_strcasecmp(device_mode_name(d), "Off") ||
        !g_ascii_strcasecmp(device_mode_name(d), "Spectrum Cycle")) {
        int st = find_mode(d, "Static");
        if (st >= 0)
            d->cur_mode = st;
        d->enabled = TRUE;
    }
    apply_device(d);
}

static void group_take_color(DevGroup *g, const GdkRGBA *c) {
    (void)g;
    for (guint i = 0; i < target_count(); i++)
        device_take_color(target_at(i), c);
    rebuild_chips();
    rebuild_detail(); // refresh stick bars / LED cells
}

static void on_dev_dot(GtkWidget *dot, gpointer data) {
    DotClick *dc = data;
    mark_selected(dot);
    if (selected)
        group_take_color(selected, &dc->color);
    (void)dc->dev;
}

static void on_dev_custom(GObject *btn, GParamSpec *spec, gpointer data) {
    (void)spec;
    if (rgb_updating)
        return;
    const GdkRGBA *c =
        gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(btn));
    (void)data;
    if (c && selected)
        group_take_color(selected, c);
}

static gboolean detail_rebuild_idle(gpointer data) {
    (void)data;
    rebuild_detail();
    return FALSE;
}

static void on_dev_mode(GObject *dd, GParamSpec *spec, gpointer data) {
    (void)spec;
    (void)data;
    if (rgb_updating || !selected)
        return;
    RgbDevice *lead = group_lead(selected);
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
    if (!lead->modes || sel >= lead->modes->len)
        return;
    const char *mode_name = g_ptr_array_index(lead->modes, sel);
    for (guint i = 0; i < target_count(); i++) {
        RgbDevice *d = target_at(i);
        int m = find_mode(d, mode_name);
        if (m < 0)
            continue; // member lacks this effect
        d->cur_mode = m;
        d->enabled = g_ascii_strcasecmp(mode_name, "Off") != 0;
        apply_device(d);
    }
    rebuild_chips();
    g_idle_add(detail_rebuild_idle, NULL); // refresh disabled states
}

static void on_dev_enable(GObject *sw, GParamSpec *spec, gpointer data) {
    (void)spec;
    (void)data;
    if (rgb_updating || !selected)
        return;
    gboolean on = gtk_switch_get_active(GTK_SWITCH(sw));
    for (guint i = 0; i < target_count(); i++) {
        RgbDevice *d = target_at(i);
        d->enabled = on;
        apply_device(d);
    }
    rebuild_chips();
    g_idle_add(detail_rebuild_idle, NULL);
}

static void on_dev_brightness(GtkRange *range, gpointer data) {
    (void)data;
    if (rgb_updating || !selected)
        return;
    double v = gtk_range_get_value(range) / 100.0;
    for (guint i = 0; i < target_count(); i++) {
        RgbDevice *d = target_at(i);
        d->brightness = v;
        apply_device(d);
    }
}

static void on_dev_speed(GtkRange *range, gpointer data) {
    (void)data;
    if (rgb_updating || !selected)
        return;
    int v = (int)gtk_range_get_value(range);
    for (guint i = 0; i < target_count(); i++) {
        RgbDevice *d = target_at(i);
        d->speed = v;
        apply_device(d);
    }
}

// segmented "All together / Per stick" toggle
static void on_sync_all(GtkWidget *b, gpointer data) {
    (void)b; (void)data;
    per_member = FALSE;
    rebuild_detail();
}

static void on_sync_per(GtkWidget *b, gpointer data) {
    (void)b; (void)data;
    per_member = TRUE;
    rebuild_detail();
}

static void rounded_rect(cairo_t *cr, double w, double h, double r) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, w - r, r, r, -G_PI / 2, 0);
    cairo_arc(cr, w - r, h - r, r, 0, G_PI / 2);
    cairo_arc(cr, r, h - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, r, r, r, G_PI, 1.5 * G_PI);
    cairo_close_path(cr);
}

// stick preview mirrors the hardware: gray when off, rainbow gradient for
// cycling effects, per-LED segments otherwise (top LED first)
static void bar_draw(GtkDrawingArea *area, cairo_t *cr, int w, int h,
                     gpointer data) {
    (void)area;
    RgbDevice *d = data;
    rounded_rect(cr, w, h, 2);
    cairo_clip(cr);

    if (!d->enabled) {
        cairo_set_source_rgb(cr, 0.35, 0.36, 0.44);
        cairo_paint(cr);
        return;
    }
    const char *mode = device_mode_name(d);
    if (!g_ascii_strcasecmp(mode, "Rainbow") ||
        !g_ascii_strcasecmp(mode, "Spectrum Cycle") ||
        !g_ascii_strcasecmp(mode, "Super Rainbow") ||
        !g_ascii_strcasecmp(mode, "Color Cycle")) {
        cairo_pattern_t *pat = cairo_pattern_create_linear(0, 0, 0, h);
        static const double hues[7][3] = {
            {1, 0, 0}, {1, 0.6, 0}, {1, 1, 0}, {0, 1, 0.2},
            {0, 0.9, 1}, {0.2, 0.2, 1}, {0.9, 0, 1}};
        for (int i = 0; i < 7; i++)
            cairo_pattern_add_color_stop_rgb(pat, i / 6.0, hues[i][0],
                                             hues[i][1], hues[i][2]);
        cairo_set_source(cr, pat);
        cairo_paint(cr);
        cairo_pattern_destroy(pat);
        return;
    }
    if (d->n_leds > 0) {
        double seg = (double)h / d->n_leds;
        for (int i = 0; i < d->n_leds; i++) {
            gdk_cairo_set_source_rgba(cr, &d->led_colors[i]);
            cairo_rectangle(cr, 0, i * seg, w, seg + 0.5);
            cairo_fill(cr);
        }
        return;
    }
    gdk_cairo_set_source_rgba(cr, &d->color);
    cairo_paint(cr);
}

static void on_bar_clicked(GtkWidget *b, gpointer data) {
    (void)b;
    per_member = TRUE;
    member_idx = GPOINTER_TO_UINT(data);
    rebuild_detail();
}

static GdkRGBA paint_color = {1.0, 0.0, 0.0, 1.0};
static GtkWidget *bars_box; // stick-bar strip, for cheap redraws

static void on_paint_color(GObject *btn, GParamSpec *spec, gpointer data) {
    (void)spec;
    (void)data;
    const GdkRGBA *c =
        gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(btn));
    if (c)
        paint_color = *c; // does not touch the hardware until a cell is hit
}

typedef struct {
    RgbDevice *dev;
    int led;
    GtkWidget *area;
} LedCell;

static void led_cell_draw(GtkDrawingArea *area, cairo_t *cr, int w, int h,
                          gpointer data) {
    (void)area;
    LedCell *lc = data;
    gdk_cairo_set_source_rgba(cr, &lc->dev->led_colors[lc->led]);
    double r = 3;
    cairo_new_sub_path(cr);
    cairo_arc(cr, w - r, r, r, -G_PI / 2, 0);
    cairo_arc(cr, w - r, h - r, r, 0, G_PI / 2);
    cairo_arc(cr, r, h - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, r, r, r, G_PI, 1.5 * G_PI);
    cairo_close_path(cr);
    cairo_fill(cr);
}

// paint the device's current colour onto one LED
static void on_led_clicked(GtkWidget *b, gpointer data) {
    (void)b;
    LedCell *lc = data;
    RgbDevice *d = lc->dev;
    if (!d->provider->set_led)
        return;
    d->led_colors[lc->led] = paint_color;
    GdkRGBA eff = paint_color;
    eff.red *= d->brightness;
    eff.green *= d->brightness;
    eff.blue *= d->brightness;
    d->provider->set_led(d, lc->led, &eff);
    d->enabled = TRUE;
    int st = find_mode(d, "Static");
    if (st >= 0)
        d->cur_mode = st;
    state_save_soon();
    gtk_widget_queue_draw(lc->area);
    rebuild_chips(); // chip swatches pick up the change
    if (bars_box)
        gtk_widget_queue_draw(bars_box); // stick previews too
}

static void led_cell_free(gpointer data, GClosure *closure) {
    (void)closure;
    g_free(data);
}

// ---- Kraken LCD section ----

static int lcd_brightness = -1, lcd_orientation = -1; // cached from device
static guint lcd_bright_timer;
static int lcd_bright_pending;

static void lcd_note(const char *msg) {
    gtk_label_set_text(GTK_LABEL(status_label), msg);
}

// persist the LCD choice so nekoland-lcdd --restore can reapply it at login
static void lcd_conf_save(const char *mode, const char *path) {
    char *dir = g_build_filename(g_get_user_config_dir(), "nekoland", NULL);
    g_mkdir_with_parents(dir, 0755);
    char *file = g_build_filename(dir, "lcd.conf", NULL);
    GKeyFile *kf = g_key_file_new();
    g_key_file_load_from_file(kf, file, 0, NULL); // keep other keys
    if (mode)
        g_key_file_set_string(kf, "lcd", "mode", mode);
    if (path)
        g_key_file_set_string(kf, "lcd", "path", path);
    if (lcd_brightness >= 0)
        g_key_file_set_integer(kf, "lcd", "brightness", lcd_brightness);
    if (lcd_orientation >= 0)
        g_key_file_set_integer(kf, "lcd", "rotation", lcd_orientation);
    g_key_file_save_to_file(kf, file, NULL);
    g_key_file_free(kf);
    g_free(file);
    g_free(dir);
}

// content is shown by the detached nekoland-lcdd daemon so it outlives the
// settings app; each spawn replaces the previous instance (pidfile)
static gboolean lcd_daemon_spawn(const char *arg) {
    char *self = g_file_read_link("/proc/self/exe", NULL);
    char *dir = self ? g_path_get_dirname(self) : g_strdup(".");
    char *exe = g_build_filename(dir, "nekoland-lcdd", NULL);
    const char *argv[] = {exe, arg, NULL};
    gboolean ok = g_spawn_async(NULL, (char **)argv, NULL, G_SPAWN_DEFAULT,
                                NULL, NULL, NULL, NULL);
    g_free(exe);
    g_free(dir);
    g_free(self);
    return ok;
}

static gboolean lcd_bright_commit(gpointer data) {
    (void)data;
    lcd_bright_timer = 0;
    kraken_lcd_set_brightness(lcd_bright_pending);
    lcd_brightness = lcd_bright_pending;
    lcd_conf_save(NULL, NULL);
    return G_SOURCE_REMOVE;
}

static void on_lcd_brightness(GtkRange *range, gpointer data) {
    (void)data;
    if (rgb_updating)
        return;
    lcd_bright_pending = (int)gtk_range_get_value(range);
    // debounce: each write is a HID round-trip
    if (lcd_bright_timer)
        g_source_remove(lcd_bright_timer);
    lcd_bright_timer = g_timeout_add(250, lcd_bright_commit, NULL);
}

static void on_lcd_orientation(GObject *dd, GParamSpec *spec, gpointer data) {
    (void)spec;
    (void)data;
    if (rgb_updating)
        return;
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
    lcd_orientation = (int)sel * 90;
    kraken_lcd_set_orientation(lcd_orientation);
    lcd_conf_save(NULL, NULL);
}

static void on_lcd_liquid(GtkWidget *btn, gpointer data) {
    (void)btn;
    (void)data;
    lcd_conf_save("liquid", NULL);
    lcd_note(lcd_daemon_spawn("--liquid") ? "LCD: liquid temperature"
                                          : "LCD: daemon launch failed");
}

static void on_lcd_file_done(GObject *src, GAsyncResult *res, gpointer data) {
    (void)data;
    GFile *f = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    if (!f)
        return;
    char *path = g_file_get_path(f);
    g_object_unref(f);
    if (!path)
        return;
    lcd_conf_save("file", path);
    // the daemon streams animations on its own so they outlive the app
    if (lcd_daemon_spawn(path)) {
        char *base = g_path_get_basename(path);
        char buf[160];
        g_snprintf(buf, sizeof(buf), "LCD: showing %s", base);
        lcd_note(buf);
        g_free(base);
    } else {
        lcd_note("LCD: daemon launch failed");
    }
    g_free(path);
}

static void on_lcd_image(GtkWidget *btn, gpointer data) {
    (void)data;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "Choose an image for the LCD");
    GtkFileFilter *filt = gtk_file_filter_new();
    gtk_file_filter_set_name(filt, "Images");
    gtk_file_filter_add_pixbuf_formats(filt);
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, filt);
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
    g_object_unref(filters);
    g_object_unref(filt);
    gtk_file_dialog_open(dlg,
                         GTK_WINDOW(gtk_widget_get_root(btn)),
                         NULL, on_lcd_file_done, NULL);
    g_object_unref(dlg);
}

static GtkWidget *form_label(const char *text) {
    GtkWidget *l = gtk_label_new(text);
    gtk_widget_add_css_class(l, "form-label");
    gtk_label_set_xalign(GTK_LABEL(l), 1.0);
    gtk_widget_set_size_request(l, 150, -1);
    return l;
}

static GtkWidget *detail_row(const char *label, GtkWidget *control) {
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_widget_add_css_class(h, "form-row");
    gtk_box_append(GTK_BOX(h), form_label(label));
    gtk_widget_set_hexpand(control, TRUE);
    gtk_box_append(GTK_BOX(h), control);
    return h;
}

static void slider_show_pct(GtkRange *range, gpointer data) {
    char buf[8];
    g_snprintf(buf, sizeof(buf), "%d%%", (int)gtk_range_get_value(range));
    gtk_label_set_text(GTK_LABEL(data), buf);
}

static GtkWidget *pct_slider(double value, GCallback changed, gpointer arg) {
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *scale =
        gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
    gtk_widget_set_hexpand(scale, TRUE);
    GtkWidget *pct = gtk_label_new("");
    gtk_widget_add_css_class(pct, "pct-label");
    gtk_label_set_width_chars(GTK_LABEL(pct), 4);
    gtk_label_set_xalign(GTK_LABEL(pct), 1.0);
    g_signal_connect(scale, "value-changed", G_CALLBACK(slider_show_pct), pct);
    g_signal_connect(scale, "value-changed", changed, arg);
    rgb_updating = TRUE;
    gtk_range_set_value(GTK_RANGE(scale), value);
    rgb_updating = FALSE;
    slider_show_pct(GTK_RANGE(scale), pct);
    gtk_box_append(GTK_BOX(h), scale);
    gtk_box_append(GTK_BOX(h), pct);
    return h;
}

static GtkWidget *dots_row(RgbDevice *d) {
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 7);
    for (gsize i = 0; i < G_N_ELEMENTS(dots); i++) {
        GtkWidget *dot = gtk_button_new();
        gtk_widget_add_css_class(dot, "color-dot");
        gtk_widget_add_css_class(dot, dots[i].css_class);
        if (gdk_rgba_equal(&dots[i].rgba, &d->color))
            gtk_widget_add_css_class(dot, "sel");
        gtk_widget_set_valign(dot, GTK_ALIGN_CENTER);
        DotClick *dc = g_new0(DotClick, 1);
        dc->dev = d;
        dc->color = dots[i].rgba;
        g_signal_connect_data(dot, "clicked", G_CALLBACK(on_dev_dot), dc,
                              dot_click_free, 0);
        gtk_box_append(GTK_BOX(h), dot);
    }
    GtkWidget *custom = gtk_color_dialog_button_new(gtk_color_dialog_new());
    gtk_widget_add_css_class(custom, "rgb-color-btn");
    gtk_widget_set_valign(custom, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_start(custom, 4);
    rgb_updating = TRUE;
    gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(custom),
                                     &d->color);
    rgb_updating = FALSE;
    g_signal_connect(custom, "notify::rgba", G_CALLBACK(on_dev_custom), d);
    gtk_box_append(GTK_BOX(h), custom);
    return h;
}

static void rebuild_detail(void) {
    bars_box = NULL; // about to be destroyed with the old panel
    clear_box(detail_box);
    if (!selected) {
        GtkWidget *hint = gtk_label_new("Select a device above");
        gtk_widget_add_css_class(hint, "hint-label");
        gtk_label_set_xalign(GTK_LABEL(hint), 0.0);
        GtkWidget *pad = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(pad, "form-row");
        gtk_box_append(GTK_BOX(pad), hint);
        gtk_box_append(GTK_BOX(detail_box), pad);
        return;
    }
    RgbDevice *d = group_lead(selected);

    // header strip: name · "Lighting" · switch
    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(head, "detail-header");
    char titlebuf[192];
    if (selected->members->len > 1)
        g_snprintf(titlebuf, sizeof(titlebuf), "%s · ×%u", d->name,
                   selected->members->len);
    else
        g_snprintf(titlebuf, sizeof(titlebuf), "%s", d->name);
    GtkWidget *name = gtk_label_new(titlebuf);
    gtk_widget_add_css_class(name, "detail-title");
    gtk_label_set_xalign(GTK_LABEL(name), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(name, TRUE);
    gtk_box_append(GTK_BOX(head), name);
    GtkWidget *ltag = gtk_label_new("Lighting");
    gtk_widget_add_css_class(ltag, "dev-subtitle");
    gtk_box_append(GTK_BOX(head), ltag);
    GtkWidget *sw = gtk_switch_new();
    gtk_widget_set_valign(sw, GTK_ALIGN_CENTER);
    rgb_updating = TRUE;
    gtk_switch_set_active(GTK_SWITCH(sw), d->enabled);
    rgb_updating = FALSE;
    g_signal_connect(sw, "notify::active", G_CALLBACK(on_dev_enable), d);
    gtk_box_append(GTK_BOX(head), sw);
    gtk_box_append(GTK_BOX(detail_box), head);

    const char *cur_mode = device_mode_name(d);
    gboolean off = !d->enabled;
    gboolean color_ok = !off && !mode_is_cycling(cur_mode);
    gboolean speed_ok = !off && !mode_is_static(cur_mode) && d->has_speed;

    // effect
    if (d->modes && d->modes->len > 0) {
        const char **strv = g_new0(const char *, d->modes->len + 1);
        for (guint m = 0; m < d->modes->len; m++)
            strv[m] = g_ptr_array_index(d->modes, m);
        GtkWidget *dd = gtk_drop_down_new_from_strings(strv);
        g_free(strv);
        gtk_widget_add_css_class(dd, "rgb-mode");
        gtk_widget_set_halign(dd, GTK_ALIGN_START);
        gtk_widget_set_valign(dd, GTK_ALIGN_CENTER);
        rgb_updating = TRUE;
        if (d->cur_mode >= 0)
            gtk_drop_down_set_selected(GTK_DROP_DOWN(dd), (guint)d->cur_mode);
        rgb_updating = FALSE;
        g_signal_connect(dd, "notify::selected", G_CALLBACK(on_dev_mode), d);
        GtkWidget *effect_row = detail_row("Effect", dd);
        gtk_widget_set_sensitive(effect_row, !off);
        gtk_box_append(GTK_BOX(detail_box), effect_row);
        gtk_box_append(GTK_BOX(detail_box), row_sep());
    }

    GtkWidget *colour_row = detail_row("Colour", dots_row(d));
    gtk_widget_set_sensitive(colour_row, color_ok);
    gtk_box_append(GTK_BOX(detail_box), colour_row);
    gtk_box_append(GTK_BOX(detail_box), row_sep());
    GtkWidget *bright_row =
        detail_row("Brightness", pct_slider(d->brightness * 100,
                                            G_CALLBACK(on_dev_brightness), d));
    gtk_widget_set_sensitive(bright_row, color_ok);
    gtk_box_append(GTK_BOX(detail_box), bright_row);
    gtk_box_append(GTK_BOX(detail_box), row_sep());
    GtkWidget *speed_row = detail_row(
        "Speed", pct_slider(d->speed, G_CALLBACK(on_dev_speed), d));
    gtk_widget_set_sensitive(speed_row, speed_ok);
    gtk_box_append(GTK_BOX(detail_box), speed_row);

    gboolean is_memory = g_str_equal(selected->label, "Memory");

    // multi-member groups: sync toggle + member bars
    if (selected->members->len > 1) {
        gtk_box_append(GTK_BOX(detail_box), row_sep());

        GtkWidget *seg = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
        gtk_widget_add_css_class(seg, "seg-box");
        GtkWidget *all_btn = gtk_button_new_with_label("All together");
        gtk_widget_add_css_class(all_btn, "seg-btn");
        if (!per_member)
            gtk_widget_add_css_class(all_btn, "sel");
        g_signal_connect(all_btn, "clicked", G_CALLBACK(on_sync_all), NULL);
        gtk_widget_set_hexpand(all_btn, TRUE);
        gtk_box_append(GTK_BOX(seg), all_btn);
        GtkWidget *per_btn = gtk_button_new_with_label(
            is_memory ? "Per stick" : "Per device");
        gtk_widget_add_css_class(per_btn, "seg-btn");
        if (per_member)
            gtk_widget_add_css_class(per_btn, "sel");
        g_signal_connect(per_btn, "clicked", G_CALLBACK(on_sync_per), NULL);
        gtk_widget_set_hexpand(per_btn, TRUE);
        gtk_box_append(GTK_BOX(seg), per_btn);
        gtk_box_append(GTK_BOX(detail_box),
                       detail_row(is_memory ? "Sync sticks" : "Sync", seg));

        GtkWidget *bars = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        bars_box = bars;
        for (guint i = 0; i < selected->members->len; i++) {
            RgbDevice *m = g_ptr_array_index(selected->members, i);
            GtkWidget *btn = gtk_button_new();
            gtk_widget_add_css_class(btn, "stick-bar");
            if (per_member && i == member_idx)
                gtk_widget_add_css_class(btn, "sel");
            GtkWidget *bv = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
            GtkWidget *bar = gtk_drawing_area_new();
            gtk_widget_set_size_request(bar, 10, 26);
            gtk_widget_set_halign(bar, GTK_ALIGN_CENTER);
            gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(bar), bar_draw, m,
                                           NULL);
            gtk_box_append(GTK_BOX(bv), bar);
            char lbl[16];
            g_snprintf(lbl, sizeof(lbl), is_memory ? "DIMM %u" : "#%u",
                       i + 1);
            GtkWidget *bl = gtk_label_new(lbl);
            gtk_widget_add_css_class(bl, "chip-sub");
            gtk_box_append(GTK_BOX(bv), bl);
            gtk_button_set_child(GTK_BUTTON(btn), bv);
            g_signal_connect(btn, "clicked", G_CALLBACK(on_bar_clicked),
                             GUINT_TO_POINTER(i));
            gtk_box_append(GTK_BOX(bars), btn);
        }
        gtk_box_append(GTK_BOX(detail_box), row_sep());
        gtk_box_append(GTK_BOX(detail_box),
                       detail_row(is_memory ? "Sticks" : "Devices", bars));
    }

    // per-LED painting when a single per-LED-capable device is targeted
    RgbDevice *paint = per_member ? target_at(0)
                       : selected->members->len == 1 ? d
                                                     : NULL;
    if (paint && paint->n_leds > 0 && paint->provider->set_led) {
        GtkWidget *cells = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
        for (int i = 0; i < MIN(paint->n_leds, RGB_MAX_LEDS); i++) {
            GtkWidget *btn = gtk_button_new();
            gtk_widget_add_css_class(btn, "led-cell");
            LedCell *lc = g_new0(LedCell, 1);
            lc->dev = paint;
            lc->led = i;
            GtkWidget *cell = gtk_drawing_area_new();
            gtk_widget_set_size_request(cell, 16, 16);
            gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(cell),
                                           led_cell_draw, lc, NULL);
            lc->area = cell;
            gtk_button_set_child(GTK_BUTTON(btn), cell);
            g_signal_connect_data(btn, "clicked", G_CALLBACK(on_led_clicked),
                                  lc, led_cell_free, 0);
            gtk_box_append(GTK_BOX(cells), btn);
        }
        GtkWidget *pbtn =
            gtk_color_dialog_button_new(gtk_color_dialog_new());
        gtk_widget_add_css_class(pbtn, "rgb-color-btn");
        gtk_widget_set_valign(pbtn, GTK_ALIGN_CENTER);
        gtk_widget_set_margin_start(pbtn, 6);
        rgb_updating = TRUE;
        gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(pbtn),
                                         &paint_color);
        rgb_updating = FALSE;
        g_signal_connect(pbtn, "notify::rgba", G_CALLBACK(on_paint_color),
                         NULL);
        gtk_box_append(GTK_BOX(cells), pbtn);
        GtkWidget *hint = gtk_label_new("click a cell to paint");
        gtk_widget_add_css_class(hint, "chip-sub");
        gtk_widget_set_margin_start(hint, 4);
        gtk_box_append(GTK_BOX(cells), hint);
        gtk_box_append(GTK_BOX(detail_box), row_sep());
        gtk_box_append(GTK_BOX(detail_box), detail_row("LEDs", cells));
    }

    // Kraken pump-cap LCD controls
    gboolean group_has_lcd = FALSE;
    for (guint i = 0; i < selected->members->len; i++)
        if (((RgbDevice *)g_ptr_array_index(selected->members, i))->has_lcd)
            group_has_lcd = TRUE;
    if (group_has_lcd && kraken_lcd_present()) {
        if (lcd_brightness < 0) // first visit: read current state once
            kraken_lcd_get_info(&lcd_brightness, &lcd_orientation);

        gtk_box_append(GTK_BOX(detail_box), row_sep());

        GtkWidget *seg = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
        gtk_widget_add_css_class(seg, "seg-box");
        GtkWidget *liq = gtk_button_new_with_label("Liquid temp");
        gtk_widget_add_css_class(liq, "seg-btn");
        gtk_widget_set_hexpand(liq, TRUE);
        g_signal_connect(liq, "clicked", G_CALLBACK(on_lcd_liquid), NULL);
        gtk_box_append(GTK_BOX(seg), liq);
        GtkWidget *img = gtk_button_new_with_label("Image / GIF…");
        gtk_widget_add_css_class(img, "seg-btn");
        gtk_widget_set_hexpand(img, TRUE);
        g_signal_connect(img, "clicked", G_CALLBACK(on_lcd_image), NULL);
        gtk_box_append(GTK_BOX(seg), img);
        gtk_box_append(GTK_BOX(detail_box), detail_row("LCD screen", seg));
        gtk_box_append(GTK_BOX(detail_box), row_sep());

        gtk_box_append(GTK_BOX(detail_box),
                       detail_row("LCD brightness",
                                  pct_slider(lcd_brightness >= 0
                                                 ? lcd_brightness
                                                 : 80,
                                             G_CALLBACK(on_lcd_brightness),
                                             NULL)));
        gtk_box_append(GTK_BOX(detail_box), row_sep());

        static const char *degs[] = {"0°", "90°", "180°", "270°", NULL};
        GtkWidget *dd = gtk_drop_down_new_from_strings(degs);
        gtk_widget_add_css_class(dd, "rgb-mode");
        gtk_widget_set_halign(dd, GTK_ALIGN_START);
        gtk_widget_set_valign(dd, GTK_ALIGN_CENTER);
        rgb_updating = TRUE;
        if (lcd_orientation >= 0)
            gtk_drop_down_set_selected(GTK_DROP_DOWN(dd),
                                       (guint)(lcd_orientation / 90));
        rgb_updating = FALSE;
        g_signal_connect(dd, "notify::selected",
                         G_CALLBACK(on_lcd_orientation), NULL);
        gtk_box_append(GTK_BOX(detail_box), detail_row("LCD rotation", dd));
    }
}

// ---- profiles ----

static char *profiles_path(void) {
    return g_build_filename(g_get_user_config_dir(), "nekoland",
                            "rgb-profiles.ini", NULL);
}

static GKeyFile *profiles_load(void) {
    GKeyFile *kf = g_key_file_new();
    char *path = profiles_path();
    g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL);
    g_free(path);
    return kf;
}

static void profiles_store(GKeyFile *kf) {
    char *path = profiles_path();
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    g_key_file_save_to_file(kf, path, NULL);
    g_free(dir);
    g_free(path);
}

static void profile_apply(const char *name) {
    GKeyFile *kf = profiles_load();
    for (guint i = 0; devices && i < devices->len; i++) {
        RgbDevice *d = g_ptr_array_index(devices, i);
        char *key = g_strdup_printf("dev %s", d->id);
        char *val = g_key_file_get_string(kf, name, key, NULL);
        g_free(key);
        if (!val)
            continue;
        char **f = g_strsplit(val, "|", 5);
        if (g_strv_length(f) == 5) {
            int m = find_mode(d, f[0]);
            if (m >= 0)
                d->cur_mode = m;
            gdk_rgba_parse(&d->color, f[1]);
            d->brightness = g_ascii_strtod(f[2], NULL);
            d->speed = atoi(f[3]);
            d->enabled = g_str_equal(f[4], "1");
            apply_device(d);
        }
        g_strfreev(f);
        g_free(val);
    }
    g_free(active_profile);
    active_profile = g_strdup(name);
    g_key_file_free(kf);
    rebuild_chips();
    rebuild_detail();
    rebuild_profiles();
}

static void profile_save_current(void) {
    GKeyFile *kf = profiles_load();
    char name[32];
    for (int n = 1;; n++) {
        g_snprintf(name, sizeof(name), "Profile %d", n);
        if (!g_key_file_has_group(kf, name))
            break;
    }
    for (guint i = 0; devices && i < devices->len; i++) {
        RgbDevice *d = g_ptr_array_index(devices, i);
        char *key = g_strdup_printf("dev %s", d->id);
        char hex[10];
        g_snprintf(hex, sizeof(hex), "#%02X%02X%02X",
                   (int)(d->color.red * 255 + 0.5),
                   (int)(d->color.green * 255 + 0.5),
                   (int)(d->color.blue * 255 + 0.5));
        char bright[G_ASCII_DTOSTR_BUF_SIZE];
        g_ascii_dtostr(bright, sizeof(bright), d->brightness);
        char *val = g_strdup_printf("%s|%s|%s|%d|%d", device_mode_name(d), hex,
                                    bright, d->speed, d->enabled ? 1 : 0);
        g_key_file_set_string(kf, name, key, val);
        g_free(key);
        g_free(val);
    }
    profiles_store(kf);
    g_key_file_free(kf);
    g_free(active_profile);
    active_profile = g_strdup(name);
    state_save_soon(); // remember which profile is active
    rebuild_profiles();
}

static void profile_delete_active(void) {
    if (!active_profile)
        return;
    GKeyFile *kf = profiles_load();
    g_key_file_remove_group(kf, active_profile, NULL);
    profiles_store(kf);
    g_key_file_free(kf);
    g_clear_pointer(&active_profile, g_free);
    state_save_soon();
    rebuild_profiles();
}

static void thumb_draw(GtkDrawingArea *area, cairo_t *cr, int w, int h,
                       gpointer data) {
    (void)area;
    GPtrArray *colors = data;
    cairo_pattern_t *pat = cairo_pattern_create_linear(0, 0, w, h);
    if (colors->len == 0) {
        cairo_pattern_add_color_stop_rgb(pat, 0, 0.18, 0.18, 0.25);
        cairo_pattern_add_color_stop_rgb(pat, 1, 0.10, 0.10, 0.15);
    }
    for (guint i = 0; i < colors->len; i++) {
        GdkRGBA *c = g_ptr_array_index(colors, i);
        double pos = colors->len == 1 ? 0.5 : (double)i / (colors->len - 1);
        cairo_pattern_add_color_stop_rgb(pat, pos, c->red, c->green, c->blue);
    }
    cairo_set_source(cr, pat);
    double r = 6;
    cairo_new_sub_path(cr);
    cairo_arc(cr, w - r, r, r, -G_PI / 2, 0);
    cairo_arc(cr, w - r, h - r, r, 0, G_PI / 2);
    cairo_arc(cr, r, h - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, r, r, r, G_PI, 1.5 * G_PI);
    cairo_close_path(cr);
    cairo_fill(cr);
    cairo_pattern_destroy(pat);
}

static void colors_free(gpointer data) {
    g_ptr_array_free(data, TRUE);
}

typedef struct {
    char *name;
} ProfClick;

static void prof_click_free(gpointer data, GClosure *closure) {
    (void)closure;
    ProfClick *pc = data;
    g_free(pc->name);
    g_free(pc);
}

static void on_profile_clicked(GtkWidget *btn, gpointer data) {
    (void)btn;
    ProfClick *pc = data;
    profile_apply(pc->name);
}

static void on_profile_new(GtkWidget *btn, gpointer data) {
    (void)btn;
    (void)data;
    profile_save_current();
}

static void on_profile_delete(GtkWidget *btn, gpointer data) {
    (void)btn;
    (void)data;
    profile_delete_active();
}

static void rebuild_profiles(void) {
    clear_box(profiles_box);
    GKeyFile *kf = profiles_load();
    gsize n_groups = 0;
    char **groups = g_key_file_get_groups(kf, &n_groups);

    if (n_groups == 0) {
        GtkWidget *hint = gtk_label_new(
            "No profiles yet — set things up, then save one.");
        gtk_widget_add_css_class(hint, "dev-subtitle");
        gtk_label_set_xalign(GTK_LABEL(hint), 0.0);
        gtk_box_append(GTK_BOX(profiles_box), hint);
    }

    for (gsize g = 0; g < n_groups; g++) {
        GPtrArray *colors = g_ptr_array_new_with_free_func(g_free);
        gsize n_keys = 0;
        char **keys = g_key_file_get_keys(kf, groups[g], &n_keys, NULL);
        for (gsize k = 0; k < n_keys && colors->len < 4; k++) {
            char *val = g_key_file_get_string(kf, groups[g], keys[k], NULL);
            char **f = val ? g_strsplit(val, "|", 5) : NULL;
            if (f && g_strv_length(f) == 5 && g_str_equal(f[4], "1")) {
                GdkRGBA *c = g_new0(GdkRGBA, 1);
                if (gdk_rgba_parse(c, f[1]))
                    g_ptr_array_add(colors, c);
                else
                    g_free(c);
            }
            if (f)
                g_strfreev(f);
            g_free(val);
        }
        g_strfreev(keys);

        gboolean is_active =
            active_profile && g_str_equal(groups[g], active_profile);

        GtkWidget *cardbtn = gtk_button_new();
        gtk_widget_add_css_class(cardbtn, "profile-card");
        if (is_active)
            gtk_widget_add_css_class(cardbtn, "active");
        GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        GtkWidget *thumb = gtk_drawing_area_new();
        gtk_widget_set_size_request(thumb, 108, 56);
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(thumb), thumb_draw,
                                       colors, colors_free);
        gtk_box_append(GTK_BOX(v), thumb);
        GtkWidget *nh = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
        GtkWidget *nm = gtk_label_new(groups[g]);
        gtk_widget_add_css_class(nm, "profile-name");
        gtk_label_set_xalign(GTK_LABEL(nm), 0.0);
        gtk_label_set_ellipsize(GTK_LABEL(nm), PANGO_ELLIPSIZE_END);
        gtk_box_append(GTK_BOX(nh), nm);
        if (is_active) {
            GtkWidget *tick = gtk_label_new("✓");
            gtk_widget_add_css_class(tick, "device-check");
            gtk_box_append(GTK_BOX(nh), tick);
        }
        gtk_box_append(GTK_BOX(v), nh);
        gtk_button_set_child(GTK_BUTTON(cardbtn), v);

        ProfClick *pc = g_new0(ProfClick, 1);
        pc->name = g_strdup(groups[g]);
        g_signal_connect_data(cardbtn, "clicked",
                              G_CALLBACK(on_profile_clicked), pc,
                              prof_click_free, 0);
        gtk_box_append(GTK_BOX(profiles_box), cardbtn);
    }
    g_strfreev(groups);
    g_key_file_free(kf);
}

// ---- hardware test wizard ----
// Exhaustive: every effect of every device, plus brightness, slow/fast
// speed, per-member isolation, and per-LED painting branches. Applies each
// config for real, asks Yes/No, writes a report, restores state after.

typedef enum { T_MODE, T_BRIGHT, T_SPEED, T_ISOLATE, T_LED } TestKind;

typedef struct {
    RgbDevice *dev;
    TestKind kind;
    const char *mode; // T_MODE / T_SPEED (points into dev->modes)
    int speed;        // T_SPEED
    char label[96];   // report line
    char expect[192]; // what the user should see
    int answer;       // -1 pending/skip, 1 pass, 0 fail
} TestStep;

typedef struct {
    int cur_mode;
    GdkRGBA color;
    double brightness;
    int speed;
    gboolean enabled;
} DevSnapshot;

static GArray *test_steps; // TestStep
static GArray *test_snaps; // DevSnapshot, parallel to devices
static guint test_idx;
static GtkWidget *test_win;
static GtkWidget *test_body;

static gboolean mode_has_color(const char *mode) {
    return !mode_is_cycling(mode) && g_ascii_strcasecmp(mode, "Off") != 0;
}

static void test_set(RgbDevice *d, const char *mode, const GdkRGBA *c,
                     double bright, int speed) {
    int m = find_mode(d, mode);
    if (m >= 0)
        d->cur_mode = m;
    d->enabled = g_ascii_strcasecmp(mode, "Off") != 0;
    d->color = *c;
    d->brightness = bright;
    d->speed = speed;
    apply_device(d);
}

static void test_apply_step(TestStep *st) {
    RgbDevice *d = st->dev;
    GdkRGBA red = {1, 0, 0, 1};
    GdkRGBA green = {0, 1, 0, 1};

    switch (st->kind) {
    case T_MODE:
        test_set(d, st->mode, &red, 1.0, 50);
        break;
    case T_BRIGHT:
        test_set(d, "Static", &red, 0.25, 50);
        break;
    case T_SPEED:
        test_set(d, st->mode, &red, 1.0, st->speed);
        break;
    case T_ISOLATE:
        // everything of this type red, this one device green
        for (guint i = 0; devices && i < devices->len; i++) {
            RgbDevice *o = g_ptr_array_index(devices, i);
            if (o != d && o->type && d->type && g_str_equal(o->type, d->type))
                test_set(o, "Static", &red, 1.0, 50);
        }
        test_set(d, "Static", &green, 1.0, 50);
        break;
    case T_LED: {
        GdkRGBA blue = {0, 0.266, 1, 1};
        int st_m = find_mode(d, "Static");
        if (st_m >= 0)
            d->cur_mode = st_m;
        d->enabled = TRUE;
        d->brightness = 1.0;
        for (int i = 0; i < d->n_leds; i++)
            d->provider->set_led(d, i, (i % 2) ? &blue : &red);
        break;
    }
    }
}

static void test_restore_all(void) {
    if (!test_snaps || !devices)
        return;
    for (guint i = 0; i < devices->len && i < test_snaps->len; i++) {
        RgbDevice *d = g_ptr_array_index(devices, i);
        DevSnapshot *sn = &g_array_index(test_snaps, DevSnapshot, i);
        d->cur_mode = sn->cur_mode;
        d->color = sn->color;
        d->brightness = sn->brightness;
        d->speed = sn->speed;
        d->enabled = sn->enabled;
        apply_device(d);
    }
    rebuild_chips();
    rebuild_detail();
}

static void test_write_report(void) {
    GString *rep = g_string_new("nekoland RGB hardware test — full sweep\n\n");
    guint pass = 0, fail = 0;
    const char *last_dev = NULL;
    for (guint i = 0; i < test_steps->len; i++) {
        TestStep *st = &g_array_index(test_steps, TestStep, i);
        if (!last_dev || !g_str_equal(last_dev, st->dev->name)) {
            g_string_append_printf(rep, "\n[%s — %s]\n",
                                   st->dev->type ? st->dev->type : "?",
                                   st->dev->name);
            last_dev = st->dev->name;
        }
        const char *verdict = st->answer == 1   ? "PASS"
                              : st->answer == 0 ? "FAIL"
                                                : "SKIP";
        if (st->answer == 1)
            pass++;
        else if (st->answer == 0)
            fail++;
        g_string_append_printf(rep, "  %s  %s\n", verdict, st->label);
    }
    g_string_append_printf(rep, "\n%u passed, %u failed, %u skipped\n", pass,
                           fail, test_steps->len - pass - fail);
    char *path = g_build_filename(g_get_user_config_dir(), "nekoland",
                                  "rgb-test-report.txt", NULL);
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    g_file_set_contents(path, rep->str, -1, NULL);
    g_free(dir);
    g_free(path);
    g_string_free(rep, TRUE);
}

static void test_show_step(void);

static void test_finish(void) {
    test_write_report();
    test_restore_all();

    clear_box(test_body);
    GtkWidget *title = gtk_label_new("Results");
    gtk_widget_add_css_class(title, "detail-title");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0);
    gtk_widget_set_margin_bottom(title, 6);
    gtk_box_append(GTK_BOX(test_body), title);

    guint pass = 0, fail = 0, skip = 0;
    const char *last_dev = NULL;
    for (guint i = 0; i < test_steps->len; i++) {
        TestStep *st = &g_array_index(test_steps, TestStep, i);
        if (st->answer == 1) {
            pass++;
            continue; // results page lists only failures/skips, report has all
        }
        if (st->answer == 0)
            fail++;
        else
            skip++;
        if (!last_dev || !g_str_equal(last_dev, st->dev->name)) {
            GtkWidget *h = gtk_label_new(st->dev->name);
            gtk_widget_add_css_class(h, "dev-subtitle");
            gtk_label_set_xalign(GTK_LABEL(h), 0.0);
            gtk_widget_set_margin_top(h, 4);
            gtk_box_append(GTK_BOX(test_body), h);
            last_dev = st->dev->name;
        }
        char line[192];
        g_snprintf(line, sizeof(line), "%s   %s",
                   st->answer == 0 ? "✗" : "–", st->label);
        GtkWidget *l = gtk_label_new(line);
        gtk_label_set_xalign(GTK_LABEL(l), 0.0);
        gtk_widget_add_css_class(l, st->answer == 0 ? "test-fail"
                                                    : "dev-subtitle");
        gtk_box_append(GTK_BOX(test_body), l);
    }
    if (fail == 0 && skip == 0) {
        GtkWidget *ok = gtk_label_new("Everything passed 🎉");
        gtk_widget_add_css_class(ok, "test-pass");
        gtk_label_set_xalign(GTK_LABEL(ok), 0.0);
        gtk_box_append(GTK_BOX(test_body), ok);
    }
    char sum[160];
    g_snprintf(sum, sizeof(sum),
               "%u passed, %u failed, %u skipped — full report: "
               "~/.config/nekoland/rgb-test-report.txt",
               pass, fail, skip);
    GtkWidget *s2 = gtk_label_new(sum);
    gtk_widget_add_css_class(s2, "dev-subtitle");
    gtk_label_set_xalign(GTK_LABEL(s2), 0.0);
    gtk_label_set_wrap(GTK_LABEL(s2), TRUE);
    gtk_widget_set_margin_top(s2, 8);
    gtk_box_append(GTK_BOX(test_body), s2);

    GtkWidget *done = gtk_button_new_with_label("Done");
    gtk_widget_add_css_class(done, "accent-btn");
    gtk_widget_set_halign(done, GTK_ALIGN_END);
    gtk_widget_set_margin_top(done, 10);
    g_signal_connect_swapped(done, "clicked", G_CALLBACK(gtk_window_close),
                             test_win);
    gtk_box_append(GTK_BOX(test_body), done);
}

static void test_answer(int answer) {
    if (test_idx < test_steps->len)
        g_array_index(test_steps, TestStep, test_idx).answer = answer;
    test_idx++;
    if (test_idx >= test_steps->len)
        test_finish();
    else
        test_show_step();
}

static void on_test_yes(GtkWidget *b, gpointer u) { (void)b; (void)u; test_answer(1); }
static void on_test_no(GtkWidget *b, gpointer u) { (void)b; (void)u; test_answer(0); }
static void on_test_skip(GtkWidget *b, gpointer u) { (void)b; (void)u; test_answer(-1); }

static void on_test_skip_device(GtkWidget *b, gpointer u) {
    (void)b;
    (void)u;
    RgbDevice *cur = g_array_index(test_steps, TestStep, test_idx).dev;
    while (test_idx < test_steps->len &&
           g_array_index(test_steps, TestStep, test_idx).dev == cur) {
        g_array_index(test_steps, TestStep, test_idx).answer = -1;
        test_idx++;
    }
    if (test_idx >= test_steps->len)
        test_finish();
    else
        test_show_step();
}

static void test_show_step(void) {
    TestStep *st = &g_array_index(test_steps, TestStep, test_idx);
    test_apply_step(st);

    clear_box(test_body);
    char prog[64];
    g_snprintf(prog, sizeof(prog), "Step %u of %u", test_idx + 1,
               test_steps->len);
    GtkWidget *p = gtk_label_new(prog);
    gtk_widget_add_css_class(p, "dev-subtitle");
    gtk_label_set_xalign(GTK_LABEL(p), 0.0);
    gtk_box_append(GTK_BOX(test_body), p);

    char what[192];
    g_snprintf(what, sizeof(what), "%s — %s",
               st->dev->type ? st->dev->type : "?", st->dev->name);
    GtkWidget *t = gtk_label_new(what);
    gtk_widget_add_css_class(t, "detail-title");
    gtk_label_set_xalign(GTK_LABEL(t), 0.0);
    gtk_label_set_wrap(GTK_LABEL(t), TRUE);
    gtk_box_append(GTK_BOX(test_body), t);

    GtkWidget *e = gtk_label_new(st->expect);
    gtk_label_set_xalign(GTK_LABEL(e), 0.0);
    gtk_label_set_wrap(GTK_LABEL(e), TRUE);
    gtk_widget_set_margin_top(e, 4);
    gtk_box_append(GTK_BOX(test_body), e);

    GtkWidget *q = gtk_label_new("Does it look right?");
    gtk_widget_add_css_class(q, "dev-subtitle");
    gtk_label_set_xalign(GTK_LABEL(q), 0.0);
    gtk_widget_set_margin_top(q, 10);
    gtk_box_append(GTK_BOX(test_body), q);

    GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(btns, 8);
    GtkWidget *yes = gtk_button_new_with_label("Yes ✓");
    gtk_widget_add_css_class(yes, "accent-btn");
    g_signal_connect(yes, "clicked", G_CALLBACK(on_test_yes), NULL);
    gtk_box_append(GTK_BOX(btns), yes);
    GtkWidget *no = gtk_button_new_with_label("No ✗");
    gtk_widget_add_css_class(no, "advanced-btn");
    gtk_widget_add_css_class(no, "danger");
    g_signal_connect(no, "clicked", G_CALLBACK(on_test_no), NULL);
    gtk_box_append(GTK_BOX(btns), no);
    GtkWidget *skip = gtk_button_new_with_label("Skip");
    gtk_widget_add_css_class(skip, "advanced-btn");
    g_signal_connect(skip, "clicked", G_CALLBACK(on_test_skip), NULL);
    gtk_box_append(GTK_BOX(btns), skip);
    GtkWidget *skipdev = gtk_button_new_with_label("Skip device");
    gtk_widget_add_css_class(skipdev, "advanced-btn");
    g_signal_connect(skipdev, "clicked", G_CALLBACK(on_test_skip_device),
                     NULL);
    gtk_box_append(GTK_BOX(btns), skipdev);
    gtk_box_append(GTK_BOX(test_body), btns);
}

static void on_test_closed(GtkWidget *w, gpointer u) {
    (void)w;
    (void)u;
    if (test_idx < test_steps->len)
        test_restore_all();
    test_win = NULL;
}

static void test_add_step(RgbDevice *d, TestKind kind, const char *mode,
                          int speed, const char *label, const char *expect) {
    TestStep st = {d, kind, mode, speed, "", "", -1};
    g_strlcpy(st.label, label, sizeof(st.label));
    g_strlcpy(st.expect, expect, sizeof(st.expect));
    g_array_append_val(test_steps, st);
}

static void test_build_steps(void) {
    if (test_steps)
        g_array_free(test_steps, TRUE);
    test_steps = g_array_new(FALSE, TRUE, sizeof(TestStep));

    for (guint i = 0; i < devices->len; i++) {
        RgbDevice *d = g_ptr_array_index(devices, i);
        const char *type = d->type ? d->type : "device";
        char buf[192];

        // every effect branch
        for (guint m = 0; d->modes && m < d->modes->len; m++) {
            const char *mode = g_ptr_array_index(d->modes, m);
            char label[96];
            g_snprintf(label, sizeof(label), "effect: %s", mode);
            if (!g_ascii_strcasecmp(mode, "Off"))
                g_snprintf(buf, sizeof(buf),
                           "The %s should now be completely OFF.", type);
            else if (mode_is_cycling(mode))
                g_snprintf(buf, sizeof(buf),
                           "The %s should be running the \"%s\" effect "
                           "(cycling colours).", type, mode);
            else if (mode_has_color(mode))
                g_snprintf(buf, sizeof(buf),
                           "The %s should show the \"%s\" effect in RED.",
                           type, mode);
            else
                g_snprintf(buf, sizeof(buf),
                           "The %s should be running \"%s\".", type, mode);
            test_add_step(d, T_MODE, mode, 50, label, buf);
        }

        // brightness branch
        if (find_mode(d, "Static") >= 0 || find_mode(d, "Direct") >= 0) {
            g_snprintf(buf, sizeof(buf),
                       "The %s should be DIM red (25%% brightness).", type);
            test_add_step(d, T_BRIGHT, NULL, 50, "brightness: 25%% red", buf);
        }

        // speed branches on the first animated effect
        static const char *animated[] = {"Breathing", "Fading", "Flashing",
                                         "Strobe", "Rainbow",
                                         "Spectrum Cycle", "Super Rainbow",
                                         "Color Cycle", "Wave", NULL};
        const char *anim = NULL;
        for (const char **a = animated; *a && !anim; a++)
            if (find_mode(d, *a) >= 0)
                anim = g_ptr_array_index(d->modes, find_mode(d, *a));
        if (anim && d->has_speed) {
            char label[96];
            g_snprintf(label, sizeof(label), "speed: slow (%s)", anim);
            g_snprintf(buf, sizeof(buf),
                       "\"%s\" on the %s should animate SLOWLY.", anim,
                       type);
            test_add_step(d, T_SPEED, anim, 5, label, buf);
            g_snprintf(label, sizeof(label), "speed: fast (%s)", anim);
            g_snprintf(buf, sizeof(buf),
                       "\"%s\" on the %s should animate FAST.", anim, type);
            test_add_step(d, T_SPEED, anim, 95, label, buf);
        }

        // per-member isolation (only for types with siblings)
        guint siblings = 0;
        for (guint j = 0; j < devices->len; j++) {
            RgbDevice *o = g_ptr_array_index(devices, j);
            if (o->type && d->type && g_str_equal(o->type, d->type))
                siblings++;
        }
        if (siblings > 1) {
            g_snprintf(buf, sizeof(buf),
                       "ONLY this unit should be GREEN — its siblings stay "
                       "RED. (Separate addressing check.)");
            test_add_step(d, T_ISOLATE, NULL, 50, "isolation: green vs red",
                          buf);
        }

        // per-LED branch
        if (d->n_leds > 0 && d->provider->set_led) {
            g_snprintf(buf, sizeof(buf),
                       "The %s's LEDs should ALTERNATE red/blue "
                       "(per-LED addressing).", type);
            test_add_step(d, T_LED, NULL, 50, "per-LED: alternating red/blue",
                          buf);
        }
    }
}

static void on_test_clicked(GtkWidget *btn, gpointer data) {
    (void)data;
    if (!devices || devices->len == 0 || test_win)
        return;

    if (test_snaps)
        g_array_free(test_snaps, TRUE);
    test_snaps = g_array_new(FALSE, FALSE, sizeof(DevSnapshot));
    for (guint i = 0; i < devices->len; i++) {
        RgbDevice *d = g_ptr_array_index(devices, i);
        DevSnapshot sn = {d->cur_mode, d->color, d->brightness, d->speed,
                          d->enabled};
        g_array_append_val(test_snaps, sn);
    }

    test_build_steps();
    test_idx = 0;

    test_win = gtk_window_new();
    gtk_widget_set_name(test_win, "adv-modal");
    gtk_window_set_transient_for(GTK_WINDOW(test_win),
                                 GTK_WINDOW(gtk_widget_get_root(btn)));
    gtk_window_set_modal(GTK_WINDOW(test_win), TRUE);
    gtk_window_set_decorated(GTK_WINDOW(test_win), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(test_win), 440, 340);
    g_signal_connect(test_win, "destroy", G_CALLBACK(on_test_closed), NULL);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(header, "pane-header");
    GtkWidget *title = gtk_label_new("Hardware Test");
    gtk_widget_add_css_class(title, "pane-title");
    gtk_widget_set_margin_start(title, 24);
    gtk_box_append(GTK_BOX(header), title);
    gtk_box_append(GTK_BOX(root), header);

    test_body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(test_body, "page");
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), test_body);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(root), scroll);

    GtkWidget *overlay = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(overlay), root);
    GtkWidget *close_btn = gtk_button_new();
    gtk_widget_add_css_class(close_btn, "close-dot");
    gtk_widget_set_halign(close_btn, GTK_ALIGN_START);
    gtk_widget_set_valign(close_btn, GTK_ALIGN_START);
    gtk_widget_set_margin_start(close_btn, 12);
    gtk_widget_set_margin_top(close_btn, 16);
    g_signal_connect_swapped(close_btn, "clicked",
                             G_CALLBACK(gtk_window_close), test_win);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), close_btn);

    gtk_window_set_child(GTK_WINDOW(test_win), overlay);
    gtk_window_present(GTK_WINDOW(test_win));
    test_show_step();
}

// ---- async scan ----

static void scan_done(GObject *src, GAsyncResult *res, gpointer data) {
    (void)src;
    (void)data;
    GPtrArray *found = g_task_propagate_pointer(G_TASK(res), NULL);
    if (devices)
        g_ptr_array_free(devices, TRUE);
    devices = found ? found : g_ptr_array_new_with_free_func(rgb_device_free);
    for (guint i = 0; i < devices->len; i++) {
        RgbDevice *d = g_ptr_array_index(devices, i);
        d->enabled = g_ascii_strcasecmp(device_mode_name(d), "Off") != 0;
    }
    state_load_into_devices(); // remembered setup wins over the heuristic
    rebuild_groups();
    selected = groups->len ? g_ptr_array_index(groups, 0) : NULL;
    rebuild_chips();
    rebuild_detail();
    char buf[64];
    g_snprintf(buf, sizeof(buf), "%u devices", devices->len);
    gtk_label_set_text(GTK_LABEL(status_label), buf);
}

static void scan_thread(GTask *task, gpointer src, gpointer data,
                        GCancellable *cancel) {
    (void)src;
    (void)data;
    (void)cancel;
    g_ptr_array_set_size(rgb_claimed_locations, 0);
    GPtrArray *all = g_ptr_array_new_with_free_func(rgb_device_free);
    for (const RgbProvider **p = providers; *p; p++) {
        g_printerr("scan: %s...\n", (*p)->name);
        GPtrArray *found = (*p)->list();
        g_printerr("scan: %s -> %u\n", (*p)->name, found->len);
        for (guint i = 0; i < found->len; i++)
            g_ptr_array_add(all, g_ptr_array_index(found, i));
        g_ptr_array_set_free_func(found, NULL); // ownership moved to `all`
        g_ptr_array_free(found, TRUE);
    }
    g_printerr("scan: done, %u total\n", all->len);
    g_task_return_pointer(task, all, NULL);
}

static void start_scan(void) {
    if (devices) {
        g_ptr_array_free(devices, TRUE);
        devices = NULL;
        selected = NULL;
        if (groups) {
            g_ptr_array_free(groups, TRUE);
            groups = NULL;
        }
    }
    rebuild_chips();
    rebuild_detail();
    gtk_label_set_text(GTK_LABEL(status_label), "scanning…");
    GTask *task = g_task_new(NULL, NULL, scan_done, NULL);
    g_task_run_in_thread(task, scan_thread);
    g_object_unref(task);
}

static void on_rescan_clicked(GtkWidget *btn, gpointer data) {
    (void)btn;
    (void)data;
    start_scan();
}

// ---- page ----

GtkWidget *rgb_page_new(void) {
    rgb_claimed_locations = g_ptr_array_new_with_free_func(g_free);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(header, "pane-header");
    GtkWidget *chev = gtk_label_new("‹");
    gtk_widget_add_css_class(chev, "pane-chevron");
    gtk_box_append(GTK_BOX(header), chev);
    GtkWidget *ptitle = gtk_label_new("RGB Devices");
    gtk_widget_add_css_class(ptitle, "pane-title");
    gtk_box_append(GTK_BOX(header), ptitle);
    status_label = gtk_label_new("");
    gtk_widget_add_css_class(status_label, "dev-tag");
    gtk_widget_set_hexpand(status_label, TRUE);
    gtk_label_set_xalign(GTK_LABEL(status_label), 1.0);
    gtk_box_append(GTK_BOX(header), status_label);
    gtk_box_append(GTK_BOX(root), header);

    GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 20);
    gtk_widget_add_css_class(col, "content-col");
    gtk_widget_set_size_request(col, 560, -1);
    gtk_widget_set_halign(col, GTK_ALIGN_CENTER);

    // devices: chip row + detail card
    GtkWidget *dev_section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 9);
    GtkWidget *sec = gtk_label_new("DEVICES");
    gtk_label_set_xalign(GTK_LABEL(sec), 0.0);
    gtk_widget_add_css_class(sec, "section-label");
    gtk_box_append(GTK_BOX(dev_section), sec);

    chips_box = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(chips_box),
                                    GTK_SELECTION_NONE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(chips_box), 4);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(chips_box), 8);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(chips_box), 8);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(chips_box), TRUE);
    gtk_box_append(GTK_BOX(dev_section), chips_box);

    detail_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(detail_box, "nk-card");
    gtk_box_append(GTK_BOX(dev_section), detail_box);
    gtk_box_append(GTK_BOX(col), dev_section);

    // profiles
    GtkWidget *psec = gtk_label_new("PROFILES");
    gtk_label_set_xalign(GTK_LABEL(psec), 0.0);
    gtk_widget_add_css_class(psec, "section-label");
    gtk_box_append(GTK_BOX(col), psec);
    GtkWidget *pcard = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(pcard, "nk-card");
    profiles_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(profiles_box, "profiles-row");
    gtk_box_append(GTK_BOX(pcard), profiles_box);
    gtk_box_append(GTK_BOX(pcard), row_sep());
    GtkWidget *pb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(pb, "form-row");
    GtkWidget *newp = gtk_button_new_with_label("New Profile");
    gtk_widget_add_css_class(newp, "accent-btn");
    g_signal_connect(newp, "clicked", G_CALLBACK(on_profile_new), NULL);
    gtk_box_append(GTK_BOX(pb), newp);
    GtkWidget *filler = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(filler, TRUE);
    gtk_box_append(GTK_BOX(pb), filler);
    GtkWidget *del = gtk_button_new_with_label("Delete");
    gtk_widget_add_css_class(del, "advanced-btn");
    gtk_widget_add_css_class(del, "danger");
    g_signal_connect(del, "clicked", G_CALLBACK(on_profile_delete), NULL);
    gtk_box_append(GTK_BOX(pb), del);
    gtk_box_append(GTK_BOX(pcard), pb);
    gtk_box_append(GTK_BOX(col), pcard);

    GtkWidget *btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *testbtn = gtk_button_new_with_label("Test Hardware");
    gtk_widget_add_css_class(testbtn, "advanced-btn");
    g_signal_connect(testbtn, "clicked", G_CALLBACK(on_test_clicked), NULL);
    GtkWidget *tfill = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(tfill, TRUE);
    gtk_box_append(GTK_BOX(btn_row), tfill);
    gtk_box_append(GTK_BOX(btn_row), testbtn);
    GtkWidget *rescan = gtk_button_new_with_label("Rescan");
    gtk_widget_add_css_class(rescan, "advanced-btn");
    gtk_widget_set_halign(rescan, GTK_ALIGN_END);
    g_signal_connect(rescan, "clicked", G_CALLBACK(on_rescan_clicked), NULL);
    gtk_box_append(GTK_BOX(btn_row), rescan);
    gtk_box_append(GTK_BOX(col), btn_row);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), col);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(root), scroll);

    rebuild_profiles();
    start_scan();
    return root;
}
