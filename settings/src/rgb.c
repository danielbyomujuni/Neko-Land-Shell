// RGB page, built to the Settings.dc.html mockup's RGB pane:
//  - master "Lighting" card: on/off, effect, colour dots + hex, brightness,
//    speed, and an "Apply to All Devices" button (staged, applied on click)
//  - DEVICES card: expandable per-device rows (caret + colour square + name +
//    "type · mode" subtitle + on/off switch) revealing an inset panel with
//    the device's real effect list, colour dots, and brightness
//  - PROFILES: snapshots of the whole state, saved to
//    ~/.config/nekoland/rgb-profiles.ini; click a card to apply
//
// Devices come from pluggable providers (rgb.h); native SMBus/hidraw
// backends first, optional OpenRGB CLI fallback for the rest.

#include "app.h"
#include "rgb.h"

#include <string.h>

GPtrArray *rgb_claimed_locations;

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

RgbDevice *rgb_device_new(const RgbProvider *p, const char *id,
                          const char *name, const char *type) {
    RgbDevice *d = g_new0(RgbDevice, 1);
    d->provider = p;
    d->id = g_strdup(id);
    d->name = g_strdup(name);
    d->type = g_strdup(type);
    d->cur_mode = -1;
    d->color = (GdkRGBA){0.922, 0.627, 0.675, 1.0}; // catppuccin maroon
    d->brightness = 1.0;
    d->speed = 40;
    d->enabled = TRUE;
    return d;
}

void rgb_device_free(gpointer p) {
    RgbDevice *d = p;
    g_free(d->id);
    g_free(d->name);
    g_free(d->type);
    if (d->modes)
        g_ptr_array_free(d->modes, TRUE);
    g_free(d);
}

// mockup accent palette (master row shows six, like the design)
static const struct {
    const char *css_class;
    const char *hex;
    GdkRGBA rgba;
} dots[] = {
    {"dot-maroon", "#EBA0AC", {0.922, 0.627, 0.675, 1}},
    {"dot-red", "#F38BA8", {0.953, 0.545, 0.659, 1}},
    {"dot-peach", "#FAB387", {0.980, 0.702, 0.529, 1}},
    {"dot-yellow", "#F9E2AF", {0.976, 0.886, 0.686, 1}},
    {"dot-green", "#A6E3A1", {0.651, 0.890, 0.631, 1}},
    {"dot-blue", "#89B4FA", {0.537, 0.706, 0.980, 1}},
    {"dot-mv", "#CBA6F7", {0.796, 0.651, 0.969, 1}},
};

// ---- page state ----

typedef struct {
    RgbDevice *d;
    GtkWidget *caret;
    GtkWidget *swatch;   // small colour square (drawing area)
    GtkWidget *subtitle;
    GtkWidget *revealer;
    GtkWidget *toggle;   // on/off switch
} DevRow;

static GtkWidget *device_card;
static GtkWidget *status_label;
static GtkWidget *profiles_box; // row of profile cards
static GPtrArray *devices;      // RgbDevice*
static GPtrArray *dev_rows;     // DevRow*
static gboolean rgb_updating;

// master (staged until "Apply to All Devices")
static GdkRGBA master_color = {0.922, 0.627, 0.675, 1};
static double master_brightness = 0.7;
static int master_speed = 40;
static int master_mode = 1; // index into master_modes
static GtkWidget *master_hex;
static const char *master_modes[] = {"Off", "Static", "Breathing",
                                     "Spectrum Cycle", NULL};

static char *active_profile; // name or NULL

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

static void apply_device(RgbDevice *d) {
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

// ---- device rows (expandable, mockup style) ----

static void update_row(DevRow *r) {
    char sub[128];
    g_snprintf(sub, sizeof(sub), "%s · %s", r->d->type ? r->d->type : "Device",
               r->d->enabled ? device_mode_name(r->d) : "Off");
    gtk_label_set_text(GTK_LABEL(r->subtitle), sub);
    gtk_widget_queue_draw(r->swatch);
}

static DevRow *row_for(RgbDevice *d) {
    for (guint i = 0; i < dev_rows->len; i++) {
        DevRow *r = g_ptr_array_index(dev_rows, i);
        if (r->d == d)
            return r;
    }
    return NULL;
}

static void swatch_draw(GtkDrawingArea *area, cairo_t *cr, int w, int h,
                        gpointer data) {
    (void)area;
    RgbDevice *d = data;
    GdkRGBA c = d->enabled ? d->color : (GdkRGBA){0.35, 0.36, 0.44, 1};
    gdk_cairo_set_source_rgba(cr, &c);
    double r = 3;
    cairo_new_sub_path(cr);
    cairo_arc(cr, w - r, r, r, -G_PI / 2, 0);
    cairo_arc(cr, w - r, h - r, r, 0, G_PI / 2);
    cairo_arc(cr, r, h - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, r, r, r, G_PI, 1.5 * G_PI);
    cairo_close_path(cr);
    cairo_fill(cr);
}

static void on_expand_clicked(GtkWidget *btn, gpointer data) {
    (void)btn;
    DevRow *r = data;
    gboolean open =
        !gtk_revealer_get_reveal_child(GTK_REVEALER(r->revealer));
    gtk_revealer_set_reveal_child(GTK_REVEALER(r->revealer), open);
    gtk_label_set_text(GTK_LABEL(r->caret), open ? "▼" : "▶");
}

static void on_enable_toggled(GObject *sw, GParamSpec *spec, gpointer data) {
    (void)spec;
    if (rgb_updating)
        return;
    RgbDevice *d = data;
    d->enabled = gtk_switch_get_active(GTK_SWITCH(sw));
    apply_device(d);
    DevRow *r = row_for(d);
    if (r)
        update_row(r);
}

static void on_dev_mode(GObject *dd, GParamSpec *spec, gpointer data) {
    (void)spec;
    if (rgb_updating)
        return;
    RgbDevice *d = data;
    d->cur_mode = (int)gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
    d->enabled = g_ascii_strcasecmp(device_mode_name(d), "Off") != 0;
    apply_device(d);
    DevRow *r = row_for(d);
    if (r) {
        rgb_updating = TRUE;
        gtk_switch_set_active(GTK_SWITCH(r->toggle), d->enabled);
        rgb_updating = FALSE;
        update_row(r);
    }
}

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
    // a colour pick implies a colour-showing mode
    if (!d->enabled || !g_ascii_strcasecmp(device_mode_name(d), "Off") ||
        !g_ascii_strcasecmp(device_mode_name(d), "Spectrum Cycle")) {
        int st = find_mode(d, "Static");
        if (st >= 0)
            d->cur_mode = st;
        d->enabled = TRUE;
    }
    apply_device(d);
    DevRow *r = row_for(d);
    if (r) {
        rgb_updating = TRUE;
        gtk_switch_set_active(GTK_SWITCH(r->toggle), TRUE);
        rgb_updating = FALSE;
        update_row(r);
    }
}

static void on_dev_dot(GtkWidget *dot, gpointer data) {
    DotClick *dc = data;
    mark_selected(dot);
    device_take_color(dc->dev, &dc->color);
}

static void on_dev_custom(GObject *btn, GParamSpec *spec, gpointer data) {
    (void)spec;
    if (rgb_updating)
        return;
    // "rgba" is a boxed property: g_object_get would hand back a pointer,
    // not fill a struct (reading it that way yields garbage ≈ black)
    const GdkRGBA *c =
        gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(btn));
    if (c)
        device_take_color(data, c);
}

static void on_dev_brightness(GtkRange *range, gpointer data) {
    if (rgb_updating)
        return;
    RgbDevice *d = data;
    d->brightness = gtk_range_get_value(range) / 100.0;
    apply_device(d);
}

static GtkWidget *inset_label(const char *text) {
    GtkWidget *l = gtk_label_new(text);
    gtk_widget_add_css_class(l, "inset-label");
    gtk_label_set_xalign(GTK_LABEL(l), 1.0);
    gtk_widget_set_size_request(l, 118, -1);
    return l;
}

static GtkWidget *inset_row(const char *label, GtkWidget *control) {
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_widget_add_css_class(h, "inset-row");
    gtk_box_append(GTK_BOX(h), inset_label(label));
    gtk_widget_set_hexpand(control, TRUE);
    gtk_box_append(GTK_BOX(h), control);
    return h;
}

static GtkWidget *dev_dots_row(RgbDevice *d) {
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    for (gsize i = 0; i < G_N_ELEMENTS(dots); i++) {
        GtkWidget *dot = gtk_button_new();
        gtk_widget_add_css_class(dot, "color-dot");
        gtk_widget_add_css_class(dot, "small");
        gtk_widget_add_css_class(dot, dots[i].css_class);
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

static void build_device_rows(void) {
    clear_box(device_card);
    g_ptr_array_set_size(dev_rows, 0);

    if (!devices || devices->len == 0) {
        GtkWidget *empty = gtk_label_new(devices ? "No RGB devices found"
                                                 : "Detecting devices…");
        gtk_widget_add_css_class(empty, "hint-label");
        gtk_label_set_xalign(GTK_LABEL(empty), 0.0);
        GtkWidget *pad = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(pad, "form-row");
        gtk_box_append(GTK_BOX(pad), empty);
        gtk_box_append(GTK_BOX(device_card), pad);
        return;
    }

    for (guint i = 0; i < devices->len; i++) {
        RgbDevice *d = g_ptr_array_index(devices, i);
        DevRow *r = g_new0(DevRow, 1);
        r->d = d;
        g_ptr_array_add(dev_rows, r);

        if (i > 0)
            gtk_box_append(GTK_BOX(device_card), row_sep());

        GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_add_css_class(head, "dev-head");

        // clickable expander area: caret + swatch + name/subtitle
        GtkWidget *expand = gtk_button_new();
        gtk_widget_add_css_class(expand, "dev-expand");
        gtk_widget_set_hexpand(expand, TRUE);
        GtkWidget *eh = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        r->caret = gtk_label_new("▶");
        gtk_widget_add_css_class(r->caret, "caret");
        gtk_box_append(GTK_BOX(eh), r->caret);
        r->swatch = gtk_drawing_area_new();
        gtk_widget_set_size_request(r->swatch, 10, 10);
        gtk_widget_set_valign(r->swatch, GTK_ALIGN_CENTER);
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(r->swatch),
                                       swatch_draw, d, NULL);
        gtk_box_append(GTK_BOX(eh), r->swatch);
        GtkWidget *names = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
        GtkWidget *name = gtk_label_new(d->name);
        gtk_label_set_xalign(GTK_LABEL(name), 0.0);
        gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
        gtk_box_append(GTK_BOX(names), name);
        r->subtitle = gtk_label_new("");
        gtk_widget_add_css_class(r->subtitle, "dev-subtitle");
        gtk_label_set_xalign(GTK_LABEL(r->subtitle), 0.0);
        gtk_box_append(GTK_BOX(names), r->subtitle);
        gtk_widget_set_hexpand(names, TRUE);
        gtk_box_append(GTK_BOX(eh), names);
        gtk_button_set_child(GTK_BUTTON(expand), eh);
        g_signal_connect(expand, "clicked", G_CALLBACK(on_expand_clicked), r);
        gtk_box_append(GTK_BOX(head), expand);

        r->toggle = gtk_switch_new();
        gtk_widget_set_valign(r->toggle, GTK_ALIGN_CENTER);
        gtk_widget_set_margin_end(r->toggle, 12);
        rgb_updating = TRUE;
        gtk_switch_set_active(GTK_SWITCH(r->toggle), d->enabled);
        rgb_updating = FALSE;
        g_signal_connect(r->toggle, "notify::active",
                         G_CALLBACK(on_enable_toggled), d);
        gtk_box_append(GTK_BOX(head), r->toggle);
        gtk_box_append(GTK_BOX(device_card), head);

        // inset panel
        r->revealer = gtk_revealer_new();
        gtk_revealer_set_transition_type(
            GTK_REVEALER(r->revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
        GtkWidget *inset = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_add_css_class(inset, "rgb-inset");

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
                gtk_drop_down_set_selected(GTK_DROP_DOWN(dd),
                                           (guint)d->cur_mode);
            rgb_updating = FALSE;
            g_signal_connect(dd, "notify::selected", G_CALLBACK(on_dev_mode),
                             d);
            gtk_box_append(GTK_BOX(inset), inset_row("Effect", dd));
        }
        gtk_box_append(GTK_BOX(inset), inset_row("Colour", dev_dots_row(d)));

        GtkWidget *bh = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        GtkWidget *scale =
            gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
        gtk_widget_set_hexpand(scale, TRUE);
        rgb_updating = TRUE;
        gtk_range_set_value(GTK_RANGE(scale), d->brightness * 100);
        rgb_updating = FALSE;
        g_signal_connect(scale, "value-changed",
                         G_CALLBACK(on_dev_brightness), d);
        gtk_box_append(GTK_BOX(bh), scale);
        gtk_box_append(GTK_BOX(inset), inset_row("Brightness", bh));

        gtk_revealer_set_child(GTK_REVEALER(r->revealer), inset);
        gtk_box_append(GTK_BOX(device_card), r->revealer);
        update_row(r);
    }
}

// ---- master card ----

static void on_master_mode(GObject *dd, GParamSpec *spec, gpointer data) {
    (void)spec;
    (void)data;
    master_mode = (int)gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
}

static void on_master_dot(GtkWidget *dot, gpointer data) {
    DotClick *dc = data;
    mark_selected(dot);
    master_color = dc->color;
    char hex[10];
    g_snprintf(hex, sizeof(hex), "#%02X%02X%02X",
               (int)(master_color.red * 255 + 0.5),
               (int)(master_color.green * 255 + 0.5),
               (int)(master_color.blue * 255 + 0.5));
    gtk_label_set_text(GTK_LABEL(master_hex), hex);
}

static void on_master_brightness(GtkRange *r, gpointer data) {
    (void)data;
    master_brightness = gtk_range_get_value(r) / 100.0;
}

static void on_master_speed(GtkRange *r, gpointer data) {
    (void)data;
    master_speed = (int)gtk_range_get_value(r);
}

static void on_apply_all(GtkWidget *btn, gpointer data) {
    (void)btn;
    (void)data;
    if (!devices)
        return;
    const char *mode = master_modes[master_mode];
    for (guint i = 0; i < devices->len; i++) {
        RgbDevice *d = g_ptr_array_index(devices, i);
        d->color = master_color;
        d->brightness = master_brightness;
        d->speed = master_speed;
        d->enabled = g_ascii_strcasecmp(mode, "Off") != 0;
        int m = find_mode(d, mode);
        if (m >= 0)
            d->cur_mode = m;
        else if (d->enabled)
            d->cur_mode = MAX(find_mode(d, "Static"), 0);
        apply_device(d);
    }
    build_device_rows(); // refresh switches/subtitles/swatches
}

static void on_master_switch(GObject *sw, GParamSpec *spec, gpointer data) {
    (void)spec;
    (void)data;
    if (rgb_updating || !devices)
        return;
    gboolean on = gtk_switch_get_active(GTK_SWITCH(sw));
    for (guint i = 0; i < devices->len; i++) {
        RgbDevice *d = g_ptr_array_index(devices, i);
        d->enabled = on;
        apply_device(d);
    }
    build_device_rows();
}

static void slider_show_pct(GtkRange *range, gpointer data) {
    GtkWidget *lbl = data;
    char buf[8];
    g_snprintf(buf, sizeof(buf), "%d%%", (int)gtk_range_get_value(range));
    gtk_label_set_text(GTK_LABEL(lbl), buf);
}

static GtkWidget *pct_slider(double value, GCallback changed) {
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *scale =
        gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
    gtk_widget_set_hexpand(scale, TRUE);
    GtkWidget *pct = gtk_label_new("");
    gtk_widget_add_css_class(pct, "pct-label");
    gtk_label_set_width_chars(GTK_LABEL(pct), 4);
    gtk_label_set_xalign(GTK_LABEL(pct), 1.0);
    g_signal_connect(scale, "value-changed", G_CALLBACK(slider_show_pct), pct);
    g_signal_connect(scale, "value-changed", changed, NULL);
    gtk_range_set_value(GTK_RANGE(scale), value);
    gtk_box_append(GTK_BOX(h), scale);
    gtk_box_append(GTK_BOX(h), pct);
    return h;
}

static GtkWidget *form_label(const char *text) {
    GtkWidget *l = gtk_label_new(text);
    gtk_widget_add_css_class(l, "form-label");
    gtk_label_set_xalign(GTK_LABEL(l), 1.0);
    gtk_widget_set_size_request(l, 150, -1);
    return l;
}

static GtkWidget *master_row(const char *label, GtkWidget *control) {
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_widget_add_css_class(h, "form-row");
    gtk_box_append(GTK_BOX(h), form_label(label));
    gtk_widget_set_hexpand(control, TRUE);
    gtk_box_append(GTK_BOX(h), control);
    return h;
}

static GtkWidget *build_master_card(void) {
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(card, "nk-card");

    // Lighting: device count + master switch
    GtkWidget *lh = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    status_label = gtk_label_new("scanning…");
    gtk_widget_add_css_class(status_label, "dev-subtitle");
    gtk_label_set_xalign(GTK_LABEL(status_label), 0.0);
    gtk_widget_set_hexpand(status_label, TRUE);
    gtk_box_append(GTK_BOX(lh), status_label);
    GtkWidget *msw = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(msw), TRUE);
    gtk_widget_set_valign(msw, GTK_ALIGN_CENTER);
    g_signal_connect(msw, "notify::active", G_CALLBACK(on_master_switch),
                     NULL);
    gtk_box_append(GTK_BOX(lh), msw);
    gtk_box_append(GTK_BOX(card), master_row("Lighting", lh));
    gtk_box_append(GTK_BOX(card), row_sep());

    // Effect
    GtkWidget *dd = gtk_drop_down_new_from_strings(master_modes);
    gtk_widget_add_css_class(dd, "rgb-mode");
    gtk_widget_set_halign(dd, GTK_ALIGN_START);
    gtk_widget_set_valign(dd, GTK_ALIGN_CENTER);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(dd), (guint)master_mode);
    g_signal_connect(dd, "notify::selected", G_CALLBACK(on_master_mode), NULL);
    gtk_box_append(GTK_BOX(card), master_row("Effect", dd));
    gtk_box_append(GTK_BOX(card), row_sep());

    // Colour dots + hex
    GtkWidget *ch = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 7);
    for (gsize i = 0; i < G_N_ELEMENTS(dots); i++) {
        GtkWidget *dot = gtk_button_new();
        gtk_widget_add_css_class(dot, "color-dot");
        gtk_widget_add_css_class(dot, dots[i].css_class);
        if (i == 0)
            gtk_widget_add_css_class(dot, "sel");
        gtk_widget_set_valign(dot, GTK_ALIGN_CENTER);
        DotClick *dc = g_new0(DotClick, 1);
        dc->color = dots[i].rgba;
        g_signal_connect_data(dot, "clicked", G_CALLBACK(on_master_dot), dc,
                              dot_click_free, 0);
        gtk_box_append(GTK_BOX(ch), dot);
    }
    master_hex = gtk_label_new("#EBA0AC");
    gtk_widget_add_css_class(master_hex, "pct-label");
    gtk_widget_set_margin_start(master_hex, 4);
    gtk_box_append(GTK_BOX(ch), master_hex);
    gtk_box_append(GTK_BOX(card), master_row("Colour", ch));
    gtk_box_append(GTK_BOX(card), row_sep());

    gtk_box_append(GTK_BOX(card),
                   master_row("Brightness",
                              pct_slider(master_brightness * 100,
                                         G_CALLBACK(on_master_brightness))));
    gtk_box_append(GTK_BOX(card), row_sep());
    gtk_box_append(
        GTK_BOX(card),
        master_row("Speed",
                   pct_slider(master_speed, G_CALLBACK(on_master_speed))));
    gtk_box_append(GTK_BOX(card), row_sep());

    // Apply row
    GtkWidget *ah = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *apply = gtk_button_new_with_label("Apply to All Devices");
    gtk_widget_add_css_class(apply, "accent-btn");
    g_signal_connect(apply, "clicked", G_CALLBACK(on_apply_all), NULL);
    gtk_box_append(GTK_BOX(ah), apply);
    GtkWidget *hint = gtk_label_new("Overrides per-device settings below");
    gtk_widget_add_css_class(hint, "dev-subtitle");
    gtk_box_append(GTK_BOX(ah), hint);
    gtk_box_append(GTK_BOX(card), master_row("", ah));

    return card;
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

static void rebuild_profiles(void);

static void profile_apply(const char *name) {
    GKeyFile *kf = profiles_load();
    for (guint i = 0; devices && i < devices->len; i++) {
        RgbDevice *d = g_ptr_array_index(devices, i);
        char *key = g_strdup_printf("dev %s", d->id);
        char *val = g_key_file_get_string(kf, name, key, NULL);
        g_free(key);
        if (!val)
            continue;
        // "<mode>|#RRGGBB|<brightness>|<speed>|<enabled>"
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
    build_device_rows();
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
    rebuild_profiles();
}

// profile thumbnail: gradient from the profile's stored colours
static void thumb_draw(GtkDrawingArea *area, cairo_t *cr, int w, int h,
                       gpointer data) {
    (void)area;
    GPtrArray *colors = data; // GdkRGBA*
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
        // collect up to 4 colours for the thumbnail
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
    build_device_rows();
    char buf[64];
    g_snprintf(buf, sizeof(buf), "%u device%s connected", devices->len,
               devices->len == 1 ? "" : "s");
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
        GPtrArray *found = (*p)->list();
        for (guint i = 0; i < found->len; i++)
            g_ptr_array_add(all, g_ptr_array_index(found, i));
        g_ptr_array_set_free_func(found, NULL); // ownership moved to `all`
        g_ptr_array_free(found, TRUE);
    }
    g_task_return_pointer(task, all, NULL);
}

static void start_scan(void) {
    if (devices) {
        g_ptr_array_free(devices, TRUE);
        devices = NULL;
    }
    build_device_rows();
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
    dev_rows = g_ptr_array_new_with_free_func(g_free);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(header, "pane-header");
    GtkWidget *chev = gtk_label_new("‹");
    gtk_widget_add_css_class(chev, "pane-chevron");
    gtk_box_append(GTK_BOX(header), chev);
    GtkWidget *ptitle = gtk_label_new("RGB Devices");
    gtk_widget_add_css_class(ptitle, "pane-title");
    gtk_box_append(GTK_BOX(header), ptitle);
    gtk_box_append(GTK_BOX(root), header);

    GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 20);
    gtk_widget_add_css_class(col, "content-col");
    gtk_widget_set_size_request(col, 560, -1);
    gtk_widget_set_halign(col, GTK_ALIGN_CENTER);

    gtk_box_append(GTK_BOX(col), build_master_card());

    GtkWidget *sec = gtk_label_new("DEVICES");
    gtk_label_set_xalign(GTK_LABEL(sec), 0.0);
    gtk_widget_add_css_class(sec, "section-label");
    gtk_box_append(GTK_BOX(col), sec);
    device_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(device_card, "nk-card");
    gtk_box_append(GTK_BOX(col), device_card);

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
    GtkWidget *newp = gtk_button_new_with_label("Save as Profile");
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

    GtkWidget *btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *rescan = gtk_button_new_with_label("Rescan");
    gtk_widget_add_css_class(rescan, "advanced-btn");
    gtk_widget_set_halign(rescan, GTK_ALIGN_END);
    gtk_widget_set_hexpand(rescan, TRUE);
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
