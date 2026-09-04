// displays.c — the Displays page.
//
// Live monitor state and changes go through hyprctl; the persistent source
// of truth is Lua: ~/.config/nekoland/monitors.lua (hand-editable, executed
// with an embedded Lua 5.4). Applying changes
//   1. rewrites monitors.lua (only while its "nekoland:managed" marker is
//      present — remove the line and the app reads but never rewrites it),
//   2. regenerates ~/.config/nekoland/monitors-gen.conf (sourced from
//      hyprland.conf after the user's own monitor conf, so it wins),
//   3. applies each monitor live via `hyprctl keyword monitor …`.

#include "app.h"

#include <json-glib/json-glib.h>
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <math.h>
#include <string.h>

#define LUA_MARKER "nekoland:managed"

typedef struct {
    char *name;
    char *desc;
    GPtrArray *modes; // char* "1920x1080@74.97Hz" (from hyprctl)
    // staged configuration (lua overlaid on live state)
    int w, h;
    double refresh;
    int x, y;
    double scale;
    int transform; // 0..3 = ×90° counter-clockwise (hyprland convention)
    gboolean vrr;
    gboolean enabled;
} Mon;

static GPtrArray *mons; // Mon*
static guint sel;
static GtkWidget *dsp_chips;   // monitor chip row
static GtkWidget *dsp_detail;  // detail form
static GtkWidget *dsp_arrange; // arrangement drawing area
static GtkWidget *dsp_status;
static gboolean dsp_updating;

static void rebuild_display_ui(void);
static GtkWidget *tearing_section_new(void);

static void mon_free(gpointer p) {
    Mon *m = p;
    g_free(m->name);
    g_free(m->desc);
    if (m->modes)
        g_ptr_array_free(m->modes, TRUE);
    g_free(m);
}

static Mon *mon_sel(void) {
    return mons && sel < mons->len ? g_ptr_array_index(mons, sel) : NULL;
}

static char *lua_conf_path(void) {
    return g_build_filename(g_get_user_config_dir(), "nekoland",
                            "monitors.lua", NULL);
}

static char *gen_conf_path(void) {
    return g_build_filename(g_get_user_config_dir(), "nekoland",
                            "monitors-gen.conf", NULL);
}

// ---- state loading ----

static void load_live(void) {
    if (mons)
        g_ptr_array_free(mons, TRUE);
    mons = g_ptr_array_new_with_free_func(mon_free);

    char *out = NULL;
    if (!g_spawn_command_line_sync("hyprctl monitors all -j", &out, NULL,
                                   NULL, NULL) ||
        !out)
        return;
    JsonParser *p = json_parser_new();
    if (json_parser_load_from_data(p, out, -1, NULL)) {
        JsonArray *arr = json_node_get_array(json_parser_get_root(p));
        for (guint i = 0; i < json_array_get_length(arr); i++) {
            JsonObject *o = json_array_get_object_element(arr, i);
            Mon *m = g_new0(Mon, 1);
            m->name = g_strdup(json_object_get_string_member(o, "name"));
            m->desc =
                g_strdup(json_object_get_string_member(o, "description"));
            m->w = (int)json_object_get_int_member(o, "width");
            m->h = (int)json_object_get_int_member(o, "height");
            m->refresh = json_object_get_double_member(o, "refreshRate");
            m->x = (int)json_object_get_int_member(o, "x");
            m->y = (int)json_object_get_int_member(o, "y");
            m->scale = json_object_get_double_member(o, "scale");
            m->transform =
                (int)json_object_get_int_member(o, "transform") & 0x3;
            m->vrr = json_object_get_boolean_member(o, "vrr");
            m->enabled = !json_object_get_boolean_member(o, "disabled");
            m->modes = g_ptr_array_new_with_free_func(g_free);
            JsonArray *am = json_object_get_array_member(o, "availableModes");
            for (guint j = 0; am && j < json_array_get_length(am); j++)
                g_ptr_array_add(m->modes,
                                g_strdup(json_array_get_string_element(
                                    am, j)));
            g_ptr_array_add(mons, m);
        }
    }
    g_object_unref(p);
    g_free(out);
}

static Mon *mon_by_name(const char *name) {
    for (guint i = 0; mons && i < mons->len; i++) {
        Mon *m = g_ptr_array_index(mons, i);
        if (g_str_equal(m->name, name))
            return m;
    }
    return NULL;
}

// overlay monitors.lua on the live state; the file either sets a global
// `monitors` table or returns one
static void load_lua(void) {
    char *path = lua_conf_path();
    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
        g_free(path);
        return;
    }
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    if (luaL_dofile(L, path) != LUA_OK) {
        const char *e = lua_tostring(L, -1);
        char buf[256];
        g_snprintf(buf, sizeof(buf), "lua error: %s", e ? e : "?");
        if (dsp_status)
            gtk_label_set_text(GTK_LABEL(dsp_status), buf);
        lua_close(L);
        g_free(path);
        return;
    }
    if (!lua_istable(L, -1)) { // no return value: use the global
        lua_getglobal(L, "monitors");
    }
    if (lua_istable(L, -1)) {
        for (int i = 1;; i++) {
            lua_rawgeti(L, -1, i);
            if (!lua_istable(L, -1)) {
                lua_pop(L, 1);
                break;
            }
            lua_getfield(L, -1, "name");
            Mon *m = lua_isstring(L, -1) ? mon_by_name(lua_tostring(L, -1))
                                         : NULL;
            lua_pop(L, 1);
            if (m) {
                lua_getfield(L, -1, "mode");
                if (lua_isstring(L, -1)) {
                    int w, h;
                    double r;
                    if (sscanf(lua_tostring(L, -1), "%dx%d@%lf", &w, &h,
                               &r) == 3) {
                        m->w = w;
                        m->h = h;
                        m->refresh = r;
                    }
                }
                lua_pop(L, 1);
                lua_getfield(L, -1, "pos");
                if (lua_isstring(L, -1))
                    sscanf(lua_tostring(L, -1), "%dx%d", &m->x, &m->y);
                lua_pop(L, 1);
                lua_getfield(L, -1, "scale");
                if (lua_isnumber(L, -1))
                    m->scale = lua_tonumber(L, -1);
                lua_pop(L, 1);
                lua_getfield(L, -1, "transform");
                if (lua_isnumber(L, -1))
                    m->transform = (int)lua_tointeger(L, -1) & 0x3;
                lua_pop(L, 1);
                lua_getfield(L, -1, "vrr");
                if (lua_isboolean(L, -1))
                    m->vrr = lua_toboolean(L, -1);
                lua_pop(L, 1);
                lua_getfield(L, -1, "enabled");
                if (lua_isboolean(L, -1))
                    m->enabled = lua_toboolean(L, -1);
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
        }
    }
    lua_close(L);
    g_free(path);
}

// ---- persistence + live apply ----

static void mon_command(Mon *m, char *buf, gsize len) {
    if (!m->enabled) {
        g_snprintf(buf, len, "%s,disable", m->name);
        return;
    }
    g_snprintf(buf, len, "%s,%dx%d@%.2f,%dx%d,%g,transform,%d,vrr,%d",
               m->name, m->w, m->h, m->refresh, m->x, m->y, m->scale,
               m->transform, m->vrr ? 1 : 0);
}

static gboolean lua_is_managed(const char *path) {
    char *content = NULL;
    gboolean managed = TRUE; // absent file: we may create it
    if (g_file_get_contents(path, &content, NULL, NULL)) {
        managed = strstr(content, LUA_MARKER) != NULL;
        g_free(content);
    }
    return managed;
}

static void write_lua(void) {
    char *path = lua_conf_path();
    if (!lua_is_managed(path)) {
        g_free(path);
        return; // hand-managed: read-only for us
    }
    GString *s = g_string_new(NULL);
    g_string_append(
        s,
        "-- " LUA_MARKER " — rewritten by nekoland-settings on Apply.\n"
        "-- Remove the marker line above to hand-manage this file; the app\n"
        "-- will still run it and apply the result, but never overwrite "
        "it.\n"
        "-- Fields: name, mode \"WxH@Hz\", pos \"XxY\", scale,\n"
        "--         transform (0-3 = ×90°), vrr, enabled.\n"
        "-- This is real Lua — feel free to compute positions.\n\n"
        "monitors = {\n");
    for (guint i = 0; mons && i < mons->len; i++) {
        Mon *m = g_ptr_array_index(mons, i);
        g_string_append_printf(
            s,
            "  { name = \"%s\", mode = \"%dx%d@%.2f\", pos = \"%dx%d\", "
            "scale = %g, transform = %d, vrr = %s, enabled = %s },\n",
            m->name, m->w, m->h, m->refresh, m->x, m->y, m->scale,
            m->transform, m->vrr ? "true" : "false",
            m->enabled ? "true" : "false");
    }
    g_string_append(s, "}\n");
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);
    g_file_set_contents(path, s->str, -1, NULL);
    g_string_free(s, TRUE);
    g_free(path);
}

static void write_gen_conf(void) {
    GString *s = g_string_new(
        "# generated by nekoland-settings from monitors.lua — do not edit\n");
    for (guint i = 0; mons && i < mons->len; i++) {
        Mon *m = g_ptr_array_index(mons, i);
        char cmd[256];
        mon_command(m, cmd, sizeof(cmd));
        g_string_append_printf(s, "monitor=%s\n", cmd);
    }
    char *path = gen_conf_path();
    g_file_set_contents(path, s->str, -1, NULL);
    g_string_free(s, TRUE);
    g_free(path);
}

static void apply_live(void) {
    for (guint i = 0; mons && i < mons->len; i++) {
        Mon *m = g_ptr_array_index(mons, i);
        char cmd[256];
        mon_command(m, cmd, sizeof(cmd));
        char *line = g_strdup_printf("hyprctl keyword monitor \"%s\"", cmd);
        g_spawn_command_line_sync(line, NULL, NULL, NULL, NULL);
        g_free(line);
    }
}

// flash each monitor's name on the monitor itself (spawns the GTK3
// layer-shell helper; args are "NAME;X;Y;SUBTITLE" matched by position)
static void on_identify(GtkWidget *b, gpointer data) {
    (void)b;
    (void)data;
    char *self = g_file_read_link("/proc/self/exe", NULL);
    char *dir = self ? g_path_get_dirname(self) : g_strdup(".");
    char *exe = g_build_filename(dir, "nekoland-identify", NULL);
    GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(argv, g_strdup(exe));
    for (guint i = 0; mons && i < mons->len; i++) {
        Mon *m = g_ptr_array_index(mons, i);
        if (!m->enabled)
            continue;
        g_ptr_array_add(argv,
                        g_strdup_printf("%s;%d;%d;%dx%d @ %.0f", m->name,
                                        m->x, m->y, m->w, m->h,
                                        m->refresh));
    }
    g_ptr_array_add(argv, NULL);
    g_spawn_async(NULL, (char **)argv->pdata, NULL, G_SPAWN_DEFAULT, NULL,
                  NULL, NULL, NULL);
    g_ptr_array_free(argv, TRUE);
    g_free(exe);
    g_free(dir);
    g_free(self);
}

static void on_apply(GtkWidget *b, gpointer data) {
    (void)b;
    (void)data;
    write_lua();
    write_gen_conf();
    apply_live();
    char *path = lua_conf_path();
    char buf[256];
    g_snprintf(buf, sizeof(buf), "applied · %s%s", path,
               lua_is_managed(path) ? "" : " (hand-managed, not rewritten)");
    gtk_label_set_text(GTK_LABEL(dsp_status), buf);
    g_free(path);
}

static void on_reload(GtkWidget *b, gpointer data) {
    (void)b;
    (void)data;
    load_live();
    load_lua();
    if (sel >= mons->len)
        sel = 0;
    rebuild_display_ui();
    gtk_label_set_text(GTK_LABEL(dsp_status), "reloaded (lua + live state)");
}

// ---- arrangement (layout units, drag to move, snaps to edges) ----

static struct {
    double px_scale; // layout px -> widget px
    double off_x, off_y;
    gboolean dragging;
    double grab_dx, grab_dy; // pointer offset inside the grabbed rect
} arr;

static void mon_layout_size(Mon *m, int *w, int *h) {
    gboolean rot = m->transform & 1;
    *w = (int)((rot ? m->h : m->w) / m->scale);
    *h = (int)((rot ? m->w : m->h) / m->scale);
}

static void arrange_metrics(int wpx, int hpx) {
    int minx = G_MAXINT, miny = G_MAXINT, maxx = G_MININT, maxy = G_MININT;
    for (guint i = 0; mons && i < mons->len; i++) {
        Mon *m = g_ptr_array_index(mons, i);
        if (!m->enabled)
            continue;
        int w, h;
        mon_layout_size(m, &w, &h);
        minx = MIN(minx, m->x);
        miny = MIN(miny, m->y);
        maxx = MAX(maxx, m->x + w);
        maxy = MAX(maxy, m->y + h);
    }
    if (minx > maxx)
        return;
    double sx = (wpx - 24) / (double)MAX(maxx - minx, 1);
    double sy = (hpx - 24) / (double)MAX(maxy - miny, 1);
    arr.px_scale = MIN(MIN(sx, sy), 0.2);
    arr.off_x = 12 + ((wpx - 24) - (maxx - minx) * arr.px_scale) / 2 -
                minx * arr.px_scale;
    arr.off_y = 12 + ((hpx - 24) - (maxy - miny) * arr.px_scale) / 2 -
                miny * arr.px_scale;
}

static void arrange_draw(GtkDrawingArea *area, cairo_t *cr, int w, int h,
                         gpointer data) {
    (void)area;
    (void)data;
    arrange_metrics(w, h);
    for (guint i = 0; mons && i < mons->len; i++) {
        Mon *m = g_ptr_array_index(mons, i);
        if (!m->enabled)
            continue;
        int mw, mh;
        mon_layout_size(m, &mw, &mh);
        double x = arr.off_x + m->x * arr.px_scale;
        double y = arr.off_y + m->y * arr.px_scale;
        double rw = mw * arr.px_scale, rh = mh * arr.px_scale;
        if (i == sel)
            cairo_set_source_rgba(cr, 0.54, 0.71, 0.98, m->enabled ? 0.95
                                                                   : 0.4);
        else
            cairo_set_source_rgba(cr, 0.5, 0.5, 0.58, m->enabled ? 0.75
                                                                 : 0.3);
        cairo_rectangle(cr, x + 1, y + 1, rw - 2, rh - 2);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, 0.08, 0.08, 0.12, 0.9);
        cairo_select_font_face(cr, "JetBrainsMono Nerd Font",
                               CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 11);
        cairo_text_extents_t te;
        cairo_text_extents(cr, m->name, &te);
        cairo_move_to(cr, x + (rw - te.width) / 2, y + (rh + te.height) / 2);
        cairo_show_text(cr, m->name);
    }
}

static int arrange_hit(double px, double py) {
    for (guint i = 0; mons && i < mons->len; i++) {
        Mon *m = g_ptr_array_index(mons, i);
        if (!m->enabled)
            continue;
        int mw, mh;
        mon_layout_size(m, &mw, &mh);
        double x = arr.off_x + m->x * arr.px_scale;
        double y = arr.off_y + m->y * arr.px_scale;
        if (px >= x && px <= x + mw * arr.px_scale && py >= y &&
            py <= y + mh * arr.px_scale)
            return (int)i;
    }
    return -1;
}

static void snap_axis(int *v, int size, int lo, int hi) {
    // snap an edge pair (v, v+size) to the reference pair (lo, hi)
    const int SNAP = 32;
    if (ABS(*v - hi) < SNAP)
        *v = hi; // left edge to their right edge
    if (ABS(*v + size - lo) < SNAP)
        *v = lo - size; // right edge to their left
    if (ABS(*v - lo) < SNAP)
        *v = lo; // aligned edges
    if (ABS(*v + size - hi) < SNAP)
        *v = hi - size;
}

static void on_drag_begin(GtkGestureDrag *g, double x, double y,
                          gpointer data) {
    (void)g;
    (void)data;
    int hit = arrange_hit(x, y);
    arr.dragging = hit >= 0;
    if (hit >= 0 && (guint)hit != sel) {
        sel = (guint)hit;
        rebuild_display_ui();
    }
    if (arr.dragging) {
        Mon *m = mon_sel();
        arr.grab_dx = x - (arr.off_x + m->x * arr.px_scale);
        arr.grab_dy = y - (arr.off_y + m->y * arr.px_scale);
    }
}

static void on_drag_update(GtkGestureDrag *g, double dx, double dy,
                           gpointer data) {
    (void)data;
    if (!arr.dragging)
        return;
    double sx, sy;
    gtk_gesture_drag_get_start_point(g, &sx, &sy);
    Mon *m = mon_sel();
    int mw, mh;
    mon_layout_size(m, &mw, &mh);
    m->x = (int)((sx + dx - arr.grab_dx - arr.off_x) / arr.px_scale);
    m->y = (int)((sy + dy - arr.grab_dy - arr.off_y) / arr.px_scale);
    for (guint i = 0; i < mons->len; i++) {
        if (i == sel)
            continue;
        Mon *o = g_ptr_array_index(mons, i);
        int ow, oh;
        mon_layout_size(o, &ow, &oh);
        snap_axis(&m->x, mw, o->x, o->x + ow);
        snap_axis(&m->y, mh, o->y, o->y + oh);
    }
    gtk_widget_queue_draw(dsp_arrange);
}

static void on_drag_end(GtkGestureDrag *g, double dx, double dy,
                        gpointer data) {
    (void)g;
    (void)dx;
    (void)dy;
    (void)data;
    arr.dragging = FALSE;
    gtk_widget_queue_draw(dsp_arrange);
}

// ---- detail form ----

static GtkWidget *dsp_row(const char *label, GtkWidget *control) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(row, "form-row");
    GtkWidget *lbl = gtk_label_new(label);
    gtk_widget_add_css_class(lbl, "form-label");
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
    gtk_widget_set_size_request(lbl, 150, -1);
    gtk_box_append(GTK_BOX(row), lbl);
    gtk_widget_set_hexpand(control, TRUE);
    gtk_widget_set_halign(control, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(row), control);
    return row;
}

static GtkWidget *dsp_sep(void) {
    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_add_css_class(sep, "nk-card-sep");
    return sep;
}

// distinct resolutions (keep mode-list order: best first)
static GPtrArray *mon_resolutions(Mon *m) {
    GPtrArray *res = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; m->modes && i < m->modes->len; i++) {
        int w, h;
        if (sscanf(g_ptr_array_index(m->modes, i), "%dx%d", &w, &h) != 2)
            continue;
        char *s = g_strdup_printf("%dx%d", w, h);
        gboolean dup = FALSE;
        for (guint j = 0; j < res->len && !dup; j++)
            dup = g_str_equal(g_ptr_array_index(res, j), s);
        if (dup)
            g_free(s);
        else
            g_ptr_array_add(res, s);
    }
    return res;
}

static GPtrArray *mon_rates(Mon *m, int w, int h) {
    GPtrArray *rates = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; m->modes && i < m->modes->len; i++) {
        int mw, mh;
        double r;
        if (sscanf(g_ptr_array_index(m->modes, i), "%dx%d@%lf", &mw, &mh,
                   &r) == 3 &&
            mw == w && mh == h)
            g_ptr_array_add(rates, g_strdup_printf("%.2f Hz", r));
    }
    return rates;
}

static void on_resolution(GObject *dd, GParamSpec *spec, gpointer data) {
    (void)spec;
    (void)data;
    if (dsp_updating)
        return;
    Mon *m = mon_sel();
    if (!m)
        return;
    guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
    GPtrArray *res = mon_resolutions(m);
    if (i < res->len) {
        sscanf(g_ptr_array_index(res, i), "%dx%d", &m->w, &m->h);
        // pick that resolution's best refresh rate
        GPtrArray *rates = mon_rates(m, m->w, m->h);
        if (rates->len)
            sscanf(g_ptr_array_index(rates, 0), "%lf", &m->refresh);
        g_ptr_array_free(rates, TRUE);
    }
    g_ptr_array_free(res, TRUE);
    rebuild_display_ui();
}

static void on_rate(GObject *dd, GParamSpec *spec, gpointer data) {
    (void)spec;
    (void)data;
    if (dsp_updating)
        return;
    Mon *m = mon_sel();
    if (!m)
        return;
    guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
    GPtrArray *rates = mon_rates(m, m->w, m->h);
    if (i < rates->len)
        sscanf(g_ptr_array_index(rates, i), "%lf", &m->refresh);
    g_ptr_array_free(rates, TRUE);
}

static const double scale_vals[] = {1.0, 1.25, 1.5, 1.75, 2.0};

static void on_scale(GObject *dd, GParamSpec *spec, gpointer data) {
    (void)spec;
    (void)data;
    if (dsp_updating)
        return;
    Mon *m = mon_sel();
    guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
    if (m && i < G_N_ELEMENTS(scale_vals)) {
        m->scale = scale_vals[i];
        gtk_widget_queue_draw(dsp_arrange);
    }
}

static void on_transform(GObject *dd, GParamSpec *spec, gpointer data) {
    (void)spec;
    (void)data;
    if (dsp_updating)
        return;
    Mon *m = mon_sel();
    if (m) {
        m->transform =
            (int)gtk_drop_down_get_selected(GTK_DROP_DOWN(dd)) & 0x3;
        gtk_widget_queue_draw(dsp_arrange);
    }
}

static void on_vrr(GObject *sw, GParamSpec *spec, gpointer data) {
    (void)spec;
    (void)data;
    if (dsp_updating)
        return;
    Mon *m = mon_sel();
    if (m)
        m->vrr = gtk_switch_get_active(GTK_SWITCH(sw));
}

static void on_mon_enable(GObject *sw, GParamSpec *spec, gpointer data) {
    (void)spec;
    (void)data;
    if (dsp_updating)
        return;
    Mon *m = mon_sel();
    if (m) {
        m->enabled = gtk_switch_get_active(GTK_SWITCH(sw));
        gtk_widget_queue_draw(dsp_arrange);
    }
}

static void on_mon_chip(GtkWidget *btn, gpointer data) {
    (void)btn;
    sel = GPOINTER_TO_UINT(data);
    rebuild_display_ui();
}

static void clear_children(GtkWidget *box) {
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(box)))
        gtk_box_remove(GTK_BOX(box), c);
}

static void rebuild_display_ui(void) {
    // chips
    {
        GtkWidget *c;
        while ((c = gtk_widget_get_first_child(dsp_chips)))
            gtk_flow_box_remove(GTK_FLOW_BOX(dsp_chips), c);
    }
    for (guint i = 0; mons && i < mons->len; i++) {
        Mon *m = g_ptr_array_index(mons, i);
        GtkWidget *btn = gtk_button_new();
        gtk_widget_add_css_class(btn, "dev-chip");
        if (i == sel)
            gtk_widget_add_css_class(btn, "sel");
        GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        GtkWidget *name = gtk_label_new(m->name);
        gtk_widget_add_css_class(name, "chip-title");
        gtk_box_append(GTK_BOX(v), name);
        char sub[64];
        g_snprintf(sub, sizeof(sub), "%dx%d @ %.0f", m->w, m->h, m->refresh);
        GtkWidget *subl = gtk_label_new(m->enabled ? sub : "off");
        gtk_widget_add_css_class(subl, "chip-sub");
        gtk_box_append(GTK_BOX(v), subl);
        gtk_button_set_child(GTK_BUTTON(btn), v);
        g_signal_connect(btn, "clicked", G_CALLBACK(on_mon_chip),
                         GUINT_TO_POINTER(i));
        gtk_flow_box_append(GTK_FLOW_BOX(dsp_chips), btn);
    }

    // detail
    clear_children(dsp_detail);
    Mon *m = mon_sel();
    if (!m)
        return;

    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(head, "detail-header");
    GtkWidget *title = gtk_label_new(m->desc && *m->desc ? m->desc
                                                         : m->name);
    gtk_widget_add_css_class(title, "detail-title");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(title, TRUE);
    gtk_box_append(GTK_BOX(head), title);
    GtkWidget *etag = gtk_label_new("Enabled");
    gtk_widget_add_css_class(etag, "dev-subtitle");
    gtk_box_append(GTK_BOX(head), etag);
    GtkWidget *esw = gtk_switch_new();
    gtk_widget_set_valign(esw, GTK_ALIGN_CENTER);
    dsp_updating = TRUE;
    gtk_switch_set_active(GTK_SWITCH(esw), m->enabled);
    dsp_updating = FALSE;
    g_signal_connect(esw, "notify::active", G_CALLBACK(on_mon_enable), NULL);
    gtk_box_append(GTK_BOX(head), esw);
    gtk_box_append(GTK_BOX(dsp_detail), head);

    // resolution
    GPtrArray *res = mon_resolutions(m);
    const char **strv = g_new0(const char *, res->len + 1);
    guint cur_res = 0;
    char want[32];
    g_snprintf(want, sizeof(want), "%dx%d", m->w, m->h);
    for (guint i = 0; i < res->len; i++) {
        strv[i] = g_ptr_array_index(res, i);
        if (g_str_equal(strv[i], want))
            cur_res = i;
    }
    GtkWidget *rdd = gtk_drop_down_new_from_strings(strv);
    g_free(strv);
    gtk_widget_add_css_class(rdd, "rgb-mode");
    gtk_widget_set_valign(rdd, GTK_ALIGN_CENTER);
    dsp_updating = TRUE;
    gtk_drop_down_set_selected(GTK_DROP_DOWN(rdd), cur_res);
    dsp_updating = FALSE;
    g_signal_connect(rdd, "notify::selected", G_CALLBACK(on_resolution),
                     NULL);
    GtkWidget *row = dsp_row("Resolution", rdd);
    gtk_widget_set_sensitive(row, m->enabled);
    gtk_box_append(GTK_BOX(dsp_detail), row);
    gtk_box_append(GTK_BOX(dsp_detail), dsp_sep());
    g_ptr_array_free(res, TRUE);

    // refresh rate
    GPtrArray *rates = mon_rates(m, m->w, m->h);
    strv = g_new0(const char *, rates->len + 1);
    guint cur_rate = 0;
    for (guint i = 0; i < rates->len; i++) {
        strv[i] = g_ptr_array_index(rates, i);
        double r;
        if (sscanf(strv[i], "%lf", &r) == 1 && fabs(r - m->refresh) < 0.5 &&
            cur_rate == 0)
            cur_rate = i;
    }
    GtkWidget *hdd = gtk_drop_down_new_from_strings(strv);
    g_free(strv);
    gtk_widget_add_css_class(hdd, "rgb-mode");
    gtk_widget_set_valign(hdd, GTK_ALIGN_CENTER);
    dsp_updating = TRUE;
    gtk_drop_down_set_selected(GTK_DROP_DOWN(hdd), cur_rate);
    dsp_updating = FALSE;
    g_signal_connect(hdd, "notify::selected", G_CALLBACK(on_rate), NULL);
    row = dsp_row("Refresh rate", hdd);
    gtk_widget_set_sensitive(row, m->enabled);
    gtk_box_append(GTK_BOX(dsp_detail), row);
    gtk_box_append(GTK_BOX(dsp_detail), dsp_sep());
    g_ptr_array_free(rates, TRUE);

    // scale
    static const char *scales[] = {"100%", "125%", "150%", "175%", "200%",
                                   NULL};
    GtkWidget *sdd = gtk_drop_down_new_from_strings(scales);
    gtk_widget_add_css_class(sdd, "rgb-mode");
    gtk_widget_set_valign(sdd, GTK_ALIGN_CENTER);
    guint cur_scale = 0;
    for (guint i = 0; i < G_N_ELEMENTS(scale_vals); i++)
        if (fabs(scale_vals[i] - m->scale) < 0.01)
            cur_scale = i;
    dsp_updating = TRUE;
    gtk_drop_down_set_selected(GTK_DROP_DOWN(sdd), cur_scale);
    dsp_updating = FALSE;
    g_signal_connect(sdd, "notify::selected", G_CALLBACK(on_scale), NULL);
    row = dsp_row("Scale", sdd);
    gtk_widget_set_sensitive(row, m->enabled);
    gtk_box_append(GTK_BOX(dsp_detail), row);
    gtk_box_append(GTK_BOX(dsp_detail), dsp_sep());

    // rotation
    static const char *rots[] = {"Normal", "90°", "180°", "270°", NULL};
    GtkWidget *tdd = gtk_drop_down_new_from_strings(rots);
    gtk_widget_add_css_class(tdd, "rgb-mode");
    gtk_widget_set_valign(tdd, GTK_ALIGN_CENTER);
    dsp_updating = TRUE;
    gtk_drop_down_set_selected(GTK_DROP_DOWN(tdd), (guint)m->transform);
    dsp_updating = FALSE;
    g_signal_connect(tdd, "notify::selected", G_CALLBACK(on_transform),
                     NULL);
    row = dsp_row("Rotation", tdd);
    gtk_widget_set_sensitive(row, m->enabled);
    gtk_box_append(GTK_BOX(dsp_detail), row);
    gtk_box_append(GTK_BOX(dsp_detail), dsp_sep());

    // VRR
    GtkWidget *vsw = gtk_switch_new();
    gtk_widget_set_valign(vsw, GTK_ALIGN_CENTER);
    dsp_updating = TRUE;
    gtk_switch_set_active(GTK_SWITCH(vsw), m->vrr);
    dsp_updating = FALSE;
    g_signal_connect(vsw, "notify::active", G_CALLBACK(on_vrr), NULL);
    row = dsp_row("Variable refresh (VRR)", vsw);
    gtk_widget_set_sensitive(row, m->enabled);
    gtk_box_append(GTK_BOX(dsp_detail), row);

    gtk_widget_queue_draw(dsp_arrange);
}

// ---- page ----

GtkWidget *displays_page_new(void) {
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(header, "pane-header");
    GtkWidget *chev = gtk_label_new("‹");
    gtk_widget_add_css_class(chev, "pane-chevron");
    gtk_box_append(GTK_BOX(header), chev);
    GtkWidget *ptitle = gtk_label_new("Displays");
    gtk_widget_add_css_class(ptitle, "pane-title");
    gtk_box_append(GTK_BOX(header), ptitle);
    dsp_status = gtk_label_new("");
    gtk_widget_add_css_class(dsp_status, "dev-tag");
    gtk_widget_set_hexpand(dsp_status, TRUE);
    gtk_label_set_xalign(GTK_LABEL(dsp_status), 1.0);
    gtk_label_set_ellipsize(GTK_LABEL(dsp_status), PANGO_ELLIPSIZE_START);
    gtk_box_append(GTK_BOX(header), dsp_status);
    gtk_box_append(GTK_BOX(root), header);

    GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 20);
    gtk_widget_add_css_class(col, "content-col");
    gtk_widget_set_size_request(col, 560, -1);
    gtk_widget_set_halign(col, GTK_ALIGN_CENTER);

    // arrangement
    GtkWidget *asec = gtk_box_new(GTK_ORIENTATION_VERTICAL, 9);
    GtkWidget *alab = gtk_label_new("ARRANGEMENT");
    gtk_label_set_xalign(GTK_LABEL(alab), 0.0);
    gtk_widget_add_css_class(alab, "section-label");
    gtk_box_append(GTK_BOX(asec), alab);
    GtkWidget *acard = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(acard, "nk-card");
    dsp_arrange = gtk_drawing_area_new();
    gtk_widget_set_size_request(dsp_arrange, -1, 160);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(dsp_arrange),
                                   arrange_draw, NULL, NULL);
    GtkGesture *drag = gtk_gesture_drag_new();
    g_signal_connect(drag, "drag-begin", G_CALLBACK(on_drag_begin), NULL);
    g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), NULL);
    g_signal_connect(drag, "drag-end", G_CALLBACK(on_drag_end), NULL);
    gtk_widget_add_controller(dsp_arrange, GTK_EVENT_CONTROLLER(drag));
    gtk_box_append(GTK_BOX(acard), dsp_arrange);
    gtk_box_append(GTK_BOX(asec), acard);
    gtk_box_append(GTK_BOX(col), asec);

    // monitors: chip row + detail card
    GtkWidget *msec = gtk_box_new(GTK_ORIENTATION_VERTICAL, 9);
    GtkWidget *mlab = gtk_label_new("MONITORS");
    gtk_label_set_xalign(GTK_LABEL(mlab), 0.0);
    gtk_widget_add_css_class(mlab, "section-label");
    gtk_box_append(GTK_BOX(msec), mlab);
    dsp_chips = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(dsp_chips),
                                    GTK_SELECTION_NONE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(dsp_chips), 4);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(dsp_chips), 8);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(dsp_chips), 8);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(dsp_chips), TRUE);
    gtk_box_append(GTK_BOX(msec), dsp_chips);
    dsp_detail = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(dsp_detail, "nk-card");
    gtk_box_append(GTK_BOX(msec), dsp_detail);
    gtk_box_append(GTK_BOX(col), msec);

    gtk_box_append(GTK_BOX(col), tearing_section_new());

    // footer: apply / reload + lua path hint
    GtkWidget *foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *apply = gtk_button_new_with_label("Apply");
    gtk_widget_add_css_class(apply, "seg-btn");
    gtk_widget_add_css_class(apply, "sel");
    g_signal_connect(apply, "clicked", G_CALLBACK(on_apply), NULL);
    gtk_box_append(GTK_BOX(foot), apply);
    GtkWidget *reload = gtk_button_new_with_label("Reload");
    gtk_widget_add_css_class(reload, "seg-btn");
    g_signal_connect(reload, "clicked", G_CALLBACK(on_reload), NULL);
    gtk_box_append(GTK_BOX(foot), reload);
    GtkWidget *ident = gtk_button_new_with_label("Identify");
    gtk_widget_add_css_class(ident, "seg-btn");
    g_signal_connect(ident, "clicked", G_CALLBACK(on_identify), NULL);
    gtk_box_append(GTK_BOX(foot), ident);
    GtkWidget *hint =
        gtk_label_new("config: ~/.config/nekoland/monitors.lua");
    gtk_widget_add_css_class(hint, "chip-sub");
    gtk_widget_set_hexpand(hint, TRUE);
    gtk_label_set_xalign(GTK_LABEL(hint), 1.0);
    gtk_box_append(GTK_BOX(foot), hint);
    gtk_box_append(GTK_BOX(col), foot);

    load_live();
    load_lua();
    rebuild_display_ui();

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), col);
    gtk_widget_set_hexpand(scroll, TRUE);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(root), scroll);
    return root;
}

// ---- allow tearing (immediate windowrules) ----
// Managed rules live in ~/.config/nekoland/tearing.conf and are rendered
// into tearing-gen.conf (sourced from hyprland.conf): per rule an
// `immediate on` + `border_color` windowrule, plus the master
// general:allow_tearing toggle. Live-applied with `hyprctl reload`.

typedef struct {
    char *field;   // "class" or "title"
    char *pattern; // regex
    char *note;
} TearRule;

static GPtrArray *tear_rules; // TearRule*
static gboolean tear_enabled = TRUE;
static char tear_border[16] = "#74c7ec";
static GtkWidget *tear_list_box;  // rebuilt on changes
static GtkWidget *tear_draft_entry;
static GtkWidget *tear_draft_field;
static GtkWidget *tear_draft_hint;
static GtkWidget *tear_draft_add;

static void tear_save_apply(void);

static void tear_rule_free(gpointer p) {
    TearRule *r = p;
    g_free(r->field);
    g_free(r->pattern);
    g_free(r->note);
    g_free(r);
}

static char *tear_conf_path(void) {
    return g_build_filename(g_get_user_config_dir(), "nekoland",
                            "tearing.conf", NULL);
}

static void tear_load(void) {
    if (tear_rules)
        return;
    tear_rules = g_ptr_array_new_with_free_func(tear_rule_free);
    char *path = tear_conf_path();
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, path, 0, NULL)) {
        tear_enabled =
            g_key_file_get_boolean(kf, "tearing", "enabled", NULL);
        char *b = g_key_file_get_string(kf, "tearing", "border", NULL);
        if (b && b[0] == '#')
            g_strlcpy(tear_border, b, sizeof(tear_border));
        g_free(b);
        for (int i = 0;; i++) {
            char grp[16];
            g_snprintf(grp, sizeof(grp), "rule%d", i);
            if (!g_key_file_has_group(kf, grp))
                break;
            TearRule *r = g_new0(TearRule, 1);
            r->field = g_key_file_get_string(kf, grp, "field", NULL);
            r->pattern = g_key_file_get_string(kf, grp, "pattern", NULL);
            r->note = g_key_file_get_string(kf, grp, "note", NULL);
            if (r->field && r->pattern)
                g_ptr_array_add(tear_rules, r);
            else
                tear_rule_free(r);
        }
    }
    g_key_file_free(kf);
    g_free(path);
    // self-heal: hyprland sources tearing-gen.conf, so it must exist even
    // before the first edit in this app
    char *gen = g_build_filename(g_get_user_config_dir(), "nekoland",
                                 "tearing-gen.conf", NULL);
    gboolean missing = !g_file_test(gen, G_FILE_TEST_EXISTS);
    g_free(gen);
    if (missing)
        tear_save_apply();
}

static void tear_save_apply(void) {
    // 1) our config
    GString *s = g_string_new("[tearing]\n");
    g_string_append_printf(s, "enabled=%s\nborder=%s\n\n",
                           tear_enabled ? "true" : "false", tear_border);
    for (guint i = 0; i < tear_rules->len; i++) {
        TearRule *r = g_ptr_array_index(tear_rules, i);
        g_string_append_printf(s,
                               "[rule%u]\nfield=%s\npattern=%s\nnote=%s\n\n",
                               i, r->field, r->pattern,
                               r->note ? r->note : "");
    }
    char *path = tear_conf_path();
    g_file_set_contents(path, s->str, -1, NULL);
    g_free(path);
    g_string_free(s, TRUE);

    // 2) the generated hyprland include
    GString *g = g_string_new(
        "# generated by nekoland-settings from tearing.conf — do not edit\n");
    g_string_append_printf(g, "general {\n    allow_tearing = %s\n}\n",
                           tear_enabled ? "true" : "false");
    for (guint i = 0; i < tear_rules->len; i++) {
        TearRule *r = g_ptr_array_index(tear_rules, i);
        g_string_append_printf(
            g, "windowrule = immediate on, match:%s %s\n", r->field,
            r->pattern);
        g_string_append_printf(
            g, "windowrule = border_color rgb(%s), match:%s %s\n",
            tear_border + 1, r->field, r->pattern);
    }
    char *gen = g_build_filename(g_get_user_config_dir(), "nekoland",
                                 "tearing-gen.conf", NULL);
    g_file_set_contents(gen, g->str, -1, NULL);
    g_free(gen);
    g_string_free(g, TRUE);

    // 3) live
    g_spawn_command_line_sync("hyprctl reload", NULL, NULL, NULL, NULL);
}

// count live windows matching a rule (the MATCHES column)
static int tear_count_matches(TearRule *r) {
    char *out = NULL;
    if (!g_spawn_command_line_sync("hyprctl clients -j", &out, NULL, NULL,
                                   NULL) ||
        !out)
        return -1;
    int n = 0;
    GRegex *re = g_regex_new(r->pattern, 0, 0, NULL);
    if (re) {
        JsonParser *p = json_parser_new();
        if (json_parser_load_from_data(p, out, -1, NULL)) {
            JsonArray *arr = json_node_get_array(json_parser_get_root(p));
            for (guint i = 0; i < json_array_get_length(arr); i++) {
                JsonObject *c = json_array_get_object_element(arr, i);
                const char *v = json_object_get_string_member(
                    c, g_str_equal(r->field, "class") ? "class" : "title");
                if (v && g_regex_match(re, v, 0, NULL))
                    n++;
            }
        }
        g_object_unref(p);
        g_regex_unref(re);
    }
    g_free(out);
    return n;
}

static void tear_rebuild_list(void);

static void on_tear_remove(GtkWidget *btn, gpointer data) {
    (void)btn;
    guint idx = GPOINTER_TO_UINT(data);
    if (idx < tear_rules->len) {
        g_ptr_array_remove_index(tear_rules, idx);
        tear_save_apply();
        tear_rebuild_list();
    }
}

static void tear_rebuild_list(void) {
    if (!tear_list_box)
        return;
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(tear_list_box)))
        gtk_box_remove(GTK_BOX(tear_list_box), c);

    for (guint i = 0; i < tear_rules->len; i++) {
        TearRule *r = g_ptr_array_index(tear_rules, i);
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_add_css_class(row, "form-row");

        GtkWidget *field = gtk_label_new(r->field);
        gtk_widget_add_css_class(field, "tear-field");
        gtk_widget_set_size_request(field, 44, -1);
        gtk_label_set_xalign(GTK_LABEL(field), 0.0);
        gtk_box_append(GTK_BOX(row), field);

        GtkWidget *pv = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
        GtkWidget *pat = gtk_label_new(r->pattern);
        gtk_widget_add_css_class(pat, "tear-pattern");
        gtk_label_set_xalign(GTK_LABEL(pat), 0.0);
        gtk_label_set_ellipsize(GTK_LABEL(pat), PANGO_ELLIPSIZE_END);
        gtk_box_append(GTK_BOX(pv), pat);
        if (r->note && *r->note) {
            GtkWidget *note = gtk_label_new(r->note);
            gtk_widget_add_css_class(note, "chip-sub");
            gtk_label_set_xalign(GTK_LABEL(note), 0.0);
            gtk_box_append(GTK_BOX(pv), note);
        }
        gtk_widget_set_hexpand(pv, TRUE);
        gtk_box_append(GTK_BOX(row), pv);

        int n = tear_count_matches(r);
        char mbuf[32];
        if (n < 0)
            g_strlcpy(mbuf, "?", sizeof(mbuf));
        else if (n == 0)
            g_strlcpy(mbuf, "none", sizeof(mbuf));
        else
            g_snprintf(mbuf, sizeof(mbuf), "%d window%s", n,
                       n == 1 ? "" : "s");
        GtkWidget *matches = gtk_label_new(mbuf);
        gtk_widget_add_css_class(matches, n > 0 ? "tear-matches-live"
                                                : "chip-sub");
        gtk_box_append(GTK_BOX(row), matches);

        GtkWidget *rm = gtk_button_new_with_label("⊖");
        gtk_widget_add_css_class(rm, "tear-remove");
        g_signal_connect(rm, "clicked", G_CALLBACK(on_tear_remove),
                         GUINT_TO_POINTER(i));
        gtk_box_append(GTK_BOX(row), rm);

        gtk_box_append(GTK_BOX(tear_list_box), row);
        gtk_box_append(GTK_BOX(tear_list_box), dsp_sep());
    }
}

static void on_tear_toggle(GObject *sw, GParamSpec *spec, gpointer data) {
    (void)spec;
    (void)data;
    tear_enabled = gtk_switch_get_active(GTK_SWITCH(sw));
    tear_save_apply();
}

static void on_tear_draft_changed(GtkEditable *e, gpointer data) {
    (void)data;
    const char *txt = gtk_editable_get_text(e);
    gboolean valid = FALSE;
    if (txt && *txt) {
        GError *err = NULL;
        GRegex *re = g_regex_new(txt, 0, 0, &err);
        valid = re != NULL;
        if (re)
            g_regex_unref(re);
        g_clear_error(&err);
        gtk_label_set_text(GTK_LABEL(tear_draft_hint),
                           valid ? "valid" : "invalid regex");
        gtk_widget_remove_css_class(tear_draft_hint,
                                    valid ? "test-fail" : "test-pass");
        gtk_widget_add_css_class(tear_draft_hint,
                                 valid ? "test-pass" : "test-fail");
    } else {
        gtk_label_set_text(GTK_LABEL(tear_draft_hint), "");
    }
    gtk_widget_set_sensitive(tear_draft_add, valid);
}

static void on_tear_add(GtkWidget *btn, gpointer data) {
    (void)btn;
    (void)data;
    const char *txt =
        gtk_editable_get_text(GTK_EDITABLE(tear_draft_entry));
    if (!txt || !*txt)
        return;
    TearRule *r = g_new0(TearRule, 1);
    guint f = gtk_drop_down_get_selected(GTK_DROP_DOWN(tear_draft_field));
    r->field = g_strdup(f == 0 ? "class" : "title");
    r->pattern = g_strdup(txt);
    r->note = g_strdup("Custom rule");
    g_ptr_array_add(tear_rules, r);
    gtk_editable_set_text(GTK_EDITABLE(tear_draft_entry), "");
    tear_save_apply();
    tear_rebuild_list();
}

static const struct {
    const char *name;
    const char *hex;
} tear_colors[] = {
    {"Sapphire", "#74c7ec"}, {"Mauve", "#cba6f7"}, {"Maroon", "#eba0ac"},
    {"Red", "#f38ba8"},      {"Green", "#a6e3a1"}, {"Lavender", "#b4befe"},
};

static void on_tear_color(GtkWidget *btn, gpointer data) {
    (void)btn;
    g_strlcpy(tear_border, tear_colors[GPOINTER_TO_INT(data)].hex,
              sizeof(tear_border));
    tear_save_apply();
    // refresh selection rings
    GtkWidget *box = gtk_widget_get_parent(btn);
    GtkWidget *c = gtk_widget_get_first_child(box);
    int i = 0;
    while (c) {
        if (g_str_equal(tear_colors[i].hex, tear_border))
            gtk_widget_add_css_class(c, "sel");
        else
            gtk_widget_remove_css_class(c, "sel");
        c = gtk_widget_get_next_sibling(c);
        i++;
    }
}

static GtkWidget *tearing_section_new(void) {
    tear_load();

    GtkWidget *sec = gtk_box_new(GTK_ORIENTATION_VERTICAL, 9);
    GtkWidget *lab = gtk_label_new("ALLOW TEARING");
    gtk_label_set_xalign(GTK_LABEL(lab), 0.0);
    gtk_widget_add_css_class(lab, "section-label");
    gtk_box_append(GTK_BOX(sec), lab);

    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(card, "nk-card");

    // master toggle
    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(head, "form-row");
    GtkWidget *tv = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    GtkWidget *title = gtk_label_new("Tearing");
    gtk_widget_add_css_class(title, "detail-title");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0);
    gtk_box_append(GTK_BOX(tv), title);
    GtkWidget *sub =
        gtk_label_new("Unredirects matching windows to skip vsync");
    gtk_widget_add_css_class(sub, "chip-sub");
    gtk_label_set_xalign(GTK_LABEL(sub), 0.0);
    gtk_box_append(GTK_BOX(tv), sub);
    gtk_widget_set_hexpand(tv, TRUE);
    gtk_box_append(GTK_BOX(head), tv);
    GtkWidget *sw = gtk_switch_new();
    gtk_widget_set_valign(sw, GTK_ALIGN_CENTER);
    gtk_switch_set_active(GTK_SWITCH(sw), tear_enabled);
    g_signal_connect(sw, "notify::active", G_CALLBACK(on_tear_toggle),
                     NULL);
    gtk_box_append(GTK_BOX(head), sw);
    gtk_box_append(GTK_BOX(card), head);
    gtk_box_append(GTK_BOX(card), dsp_sep());

    // rules
    tear_list_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(card), tear_list_box);
    tear_rebuild_list();

    // draft row
    GtkWidget *draft = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(draft, "form-row");
    static const char *fields[] = {"class", "title", NULL};
    tear_draft_field = gtk_drop_down_new_from_strings(fields);
    gtk_widget_add_css_class(tear_draft_field, "rgb-mode");
    gtk_box_append(GTK_BOX(draft), tear_draft_field);
    tear_draft_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(tear_draft_entry),
                                   "regex pattern");
    gtk_widget_add_css_class(tear_draft_entry, "tear-entry");
    gtk_widget_set_hexpand(tear_draft_entry, TRUE);
    g_signal_connect(tear_draft_entry, "changed",
                     G_CALLBACK(on_tear_draft_changed), NULL);
    gtk_box_append(GTK_BOX(draft), tear_draft_entry);
    tear_draft_hint = gtk_label_new("");
    gtk_widget_add_css_class(tear_draft_hint, "chip-sub");
    gtk_box_append(GTK_BOX(draft), tear_draft_hint);
    tear_draft_add = gtk_button_new_with_label("⊕");
    gtk_widget_add_css_class(tear_draft_add, "tear-add");
    gtk_widget_set_sensitive(tear_draft_add, FALSE);
    g_signal_connect(tear_draft_add, "clicked", G_CALLBACK(on_tear_add),
                     NULL);
    g_signal_connect(tear_draft_entry, "activate",
                     G_CALLBACK(on_tear_add), NULL);
    gtk_box_append(GTK_BOX(draft), tear_draft_add);
    gtk_box_append(GTK_BOX(card), draft);
    gtk_box_append(GTK_BOX(card), dsp_sep());

    // tearing border colour
    GtkWidget *crow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(crow, "form-row");
    GtkWidget *clab = gtk_label_new("Tearing border");
    gtk_widget_add_css_class(clab, "form-label");
    gtk_label_set_xalign(GTK_LABEL(clab), 0.0);
    gtk_widget_set_size_request(clab, 150, -1);
    gtk_box_append(GTK_BOX(crow), clab);
    GtkWidget *dots = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    for (guint i = 0; i < G_N_ELEMENTS(tear_colors); i++) {
        GtkWidget *dot = gtk_button_new();
        char cls[32];
        g_snprintf(cls, sizeof(cls), "tear-dot-%u", i);
        gtk_widget_add_css_class(dot, "tear-dot");
        gtk_widget_add_css_class(dot, cls);
        if (g_str_equal(tear_colors[i].hex, tear_border))
            gtk_widget_add_css_class(dot, "sel");
        gtk_widget_set_tooltip_text(dot, tear_colors[i].name);
        g_signal_connect(dot, "clicked", G_CALLBACK(on_tear_color),
                         GINT_TO_POINTER((int)i));
        gtk_box_append(GTK_BOX(dots), dot);
    }
    gtk_box_append(GTK_BOX(crow), dots);
    gtk_box_append(GTK_BOX(card), crow);

    gtk_box_append(GTK_BOX(sec), card);
    return sec;
}
