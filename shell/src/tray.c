// StatusNotifierItem (SNI) system tray.
//
// nekobar tries to own org.kde.StatusNotifierWatcher. If another bar (e.g.
// waybar) already owns it, we stay queued for the name and meanwhile act as a
// plain host against the existing watcher; if that bar exits we inherit the
// watcher name and apps re-register with us.

#include "nekobar.h"

#include <libdbusmenu-gtk/menu.h>
#include <string.h>
#include <unistd.h>

#define SNI_IFACE "org.kde.StatusNotifierItem"
#define WATCHER_NAME "org.kde.StatusNotifierWatcher"
#define WATCHER_PATH "/StatusNotifierWatcher"
#define ICON_SIZE 15

typedef struct {
    char *bus_name;
    char *obj_path;
    char *menu_path; // may be NULL
    gboolean item_is_menu;
    GdkPixbuf *pixbuf;
    guint sig_sub;
    guint name_watch;
    GtkMenu *menu; // lazily created DbusmenuGtkMenu
} TrayItem;

static GDBusConnection *conn;
static GPtrArray *items;         // TrayItem*
static GPtrArray *reg_items;     // char* canonical "bus/path", watcher mode
static gboolean i_am_watcher;
static guint watcher_reg_id;

static void item_fetch_props(TrayItem *it);

// ---- helpers ----

static gboolean parse_spec(const char *spec, const char *sender, char **bus,
                           char **path) {
    if (spec[0] == '/') {
        if (!sender)
            return FALSE;
        *bus = g_strdup(sender);
        *path = g_strdup(spec);
        return TRUE;
    }
    const char *slash = strchr(spec, '/');
    if (slash) {
        *bus = g_strndup(spec, slash - spec);
        *path = g_strdup(slash);
    } else {
        *bus = g_strdup(spec);
        *path = g_strdup("/StatusNotifierItem");
    }
    return TRUE;
}

static TrayItem *item_find(const char *bus, const char *path) {
    for (guint i = 0; i < items->len; i++) {
        TrayItem *it = g_ptr_array_index(items, i);
        if (g_str_equal(it->bus_name, bus) && g_str_equal(it->obj_path, path))
            return it;
    }
    return NULL;
}

// ---- icon resolution ----

static void free_pixels(guchar *pixels, gpointer data) {
    (void)data;
    g_free(pixels);
}

static GdkPixbuf *pixbuf_from_pixmap_variant(GVariant *v) {
    // a(iiay): pick the smallest image that still covers ICON_SIZE
    GVariantIter iter;
    gint w, h, best_w = 0, best_h = 0;
    GVariant *bytes, *best = NULL;
    g_variant_iter_init(&iter, v);
    while (g_variant_iter_next(&iter, "(ii@ay)", &w, &h, &bytes)) {
        gboolean better =
            !best || (best_w < ICON_SIZE && w > best_w) ||
            (w >= ICON_SIZE && w < best_w);
        if (better && w > 0 && h > 0) {
            if (best)
                g_variant_unref(best);
            best = bytes;
            best_w = w;
            best_h = h;
        } else {
            g_variant_unref(bytes);
        }
    }
    if (!best)
        return NULL;

    gsize len;
    const guchar *argb = g_variant_get_fixed_array(best, &len, 1);
    if (len < (gsize)best_w * best_h * 4) {
        g_variant_unref(best);
        return NULL;
    }
    guchar *rgba = g_malloc((gsize)best_w * best_h * 4);
    for (gsize i = 0; i < (gsize)best_w * best_h; i++) {
        rgba[i * 4 + 0] = argb[i * 4 + 1];
        rgba[i * 4 + 1] = argb[i * 4 + 2];
        rgba[i * 4 + 2] = argb[i * 4 + 3];
        rgba[i * 4 + 3] = argb[i * 4 + 0];
    }
    GdkPixbuf *pb = gdk_pixbuf_new_from_data(
        rgba, GDK_COLORSPACE_RGB, TRUE, 8, best_w, best_h, best_w * 4,
        free_pixels, NULL);
    g_variant_unref(best);
    if (pb && best_w != ICON_SIZE) {
        GdkPixbuf *scaled = gdk_pixbuf_scale_simple(pb, ICON_SIZE, ICON_SIZE,
                                                    GDK_INTERP_BILINEAR);
        g_object_unref(pb);
        pb = scaled;
    }
    return pb;
}

static GdkPixbuf *pixbuf_from_name(const char *name, const char *theme_path) {
    GtkIconTheme *theme;
    if (theme_path && *theme_path) {
        theme = gtk_icon_theme_new();
        gtk_icon_theme_append_search_path(theme, theme_path);
    } else {
        theme = g_object_ref(gtk_icon_theme_get_default());
    }
    GdkPixbuf *pb = gtk_icon_theme_load_icon(
        theme, name, ICON_SIZE, GTK_ICON_LOOKUP_FORCE_SIZE, NULL);
    g_object_unref(theme);
    return pb;
}

// ---- widgets ----

static gboolean tray_clicked(GtkWidget *w, GdkEventButton *ev, gpointer data) {
    (void)w;
    TrayItem *it = data;
    gboolean want_menu = ev->button == 3 || (ev->button == 1 && it->item_is_menu);
    if (want_menu) {
        if (it->menu_path) {
            if (!it->menu) {
                it->menu = GTK_MENU(
                    dbusmenu_gtkmenu_new(it->bus_name, it->menu_path));
                g_object_ref_sink(it->menu);
            }
            gtk_menu_popup_at_pointer(it->menu, (GdkEvent *)ev);
        } else {
            g_dbus_connection_call(conn, it->bus_name, it->obj_path, SNI_IFACE,
                                   "ContextMenu", g_variant_new("(ii)", 0, 0),
                                   NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL,
                                   NULL);
        }
    } else if (ev->button == 1) {
        g_dbus_connection_call(conn, it->bus_name, it->obj_path, SNI_IFACE,
                               "Activate", g_variant_new("(ii)", 0, 0), NULL,
                               G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    }
    return TRUE;
}

static void rebuild_trays(void) {
    for (guint b = 0; b < bars->len; b++) {
        Bar *bar = g_ptr_array_index(bars, b);
        GList *kids = gtk_container_get_children(GTK_CONTAINER(bar->tray_box));
        for (GList *l = kids; l; l = l->next)
            gtk_widget_destroy(GTK_WIDGET(l->data));
        g_list_free(kids);

        for (guint i = 0; i < items->len; i++) {
            TrayItem *it = g_ptr_array_index(items, i);
            if (!it->pixbuf)
                continue;
            GtkWidget *btn = gtk_button_new();
            gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);
            gtk_container_add(GTK_CONTAINER(btn),
                              gtk_image_new_from_pixbuf(it->pixbuf));
            g_signal_connect(btn, "button-press-event",
                             G_CALLBACK(tray_clicked), it);
            gtk_box_pack_start(GTK_BOX(bar->tray_box), btn, FALSE, FALSE, 0);
        }
        gtk_widget_show_all(bar->tray_box);
        gtk_widget_set_visible(bar->tray_box, items->len > 0);
    }
}

// ---- item lifecycle ----

static void item_free(TrayItem *it) {
    if (it->sig_sub)
        g_dbus_connection_signal_unsubscribe(conn, it->sig_sub);
    if (it->name_watch)
        g_bus_unwatch_name(it->name_watch);
    if (it->menu)
        g_object_unref(it->menu);
    if (it->pixbuf)
        g_object_unref(it->pixbuf);
    g_free(it->bus_name);
    g_free(it->obj_path);
    g_free(it->menu_path);
    g_free(it);
}

static void item_remove(const char *bus, const char *path) {
    for (guint i = 0; i < items->len; i++) {
        TrayItem *it = g_ptr_array_index(items, i);
        if (g_str_equal(it->bus_name, bus) &&
            (!path || g_str_equal(it->obj_path, path))) {
            g_ptr_array_remove_index(items, i);
            rebuild_trays();
            item_free(it);
            return;
        }
    }
}

static void on_props_ready(GObject *src, GAsyncResult *res, gpointer data) {
    char *key = data; // "bus\npath"
    GVariant *reply =
        g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
    char **kv = g_strsplit(key, "\n", 2);
    TrayItem *it = item_find(kv[0], kv[1]);
    g_strfreev(kv);
    g_free(key);
    if (!reply)
        return;
    if (!it) {
        g_variant_unref(reply);
        return;
    }

    GVariant *dict;
    g_variant_get(reply, "(@a{sv})", &dict);

    const char *icon_name = NULL, *theme_path = NULL, *menu = NULL;
    gboolean is_menu = FALSE;
    g_variant_lookup(dict, "IconName", "&s", &icon_name);
    g_variant_lookup(dict, "IconThemePath", "&s", &theme_path);
    g_variant_lookup(dict, "Menu", "&o", &menu);
    g_variant_lookup(dict, "ItemIsMenu", "b", &is_menu);

    g_clear_object(&it->pixbuf);
    if (icon_name && *icon_name)
        it->pixbuf = pixbuf_from_name(icon_name, theme_path);
    if (!it->pixbuf) {
        GVariant *pm =
            g_variant_lookup_value(dict, "IconPixmap", G_VARIANT_TYPE("a(iiay)"));
        if (pm) {
            it->pixbuf = pixbuf_from_pixmap_variant(pm);
            g_variant_unref(pm);
        }
    }
    if (!it->pixbuf)
        it->pixbuf = pixbuf_from_name("image-missing", NULL);

    g_free(it->menu_path);
    it->menu_path = menu && *menu ? g_strdup(menu) : NULL;
    it->item_is_menu = is_menu;
    if (it->menu) {
        g_object_unref(it->menu);
        it->menu = NULL;
    }

    g_variant_unref(dict);
    g_variant_unref(reply);
    rebuild_trays();
}

static void item_fetch_props(TrayItem *it) {
    char *key = g_strdup_printf("%s\n%s", it->bus_name, it->obj_path);
    g_dbus_connection_call(conn, it->bus_name, it->obj_path,
                           "org.freedesktop.DBus.Properties", "GetAll",
                           g_variant_new("(s)", SNI_IFACE),
                           G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE,
                           -1, NULL, on_props_ready, key);
}

static void on_item_signal(GDBusConnection *c, const gchar *sender,
                           const gchar *path, const gchar *iface,
                           const gchar *signal, GVariant *params,
                           gpointer data) {
    (void)c;
    (void)sender;
    (void)path;
    (void)iface;
    (void)signal;
    (void)params;
    item_fetch_props(data);
}

static void on_item_vanished(GDBusConnection *c, const gchar *name,
                             gpointer data) {
    (void)c;
    (void)data;
    item_remove(name, NULL);
}

static void item_add(const char *bus, const char *path) {
    if (item_find(bus, path))
        return;
    TrayItem *it = g_new0(TrayItem, 1);
    it->bus_name = g_strdup(bus);
    it->obj_path = g_strdup(path);
    it->sig_sub = g_dbus_connection_signal_subscribe(
        conn, bus, SNI_IFACE, NULL, path, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
        on_item_signal, it, NULL);
    it->name_watch = g_bus_watch_name_on_connection(
        conn, bus, G_BUS_NAME_WATCHER_FLAGS_NONE, NULL, on_item_vanished, NULL,
        NULL);
    g_ptr_array_add(items, it);
    item_fetch_props(it);
}

static void item_add_spec(const char *spec, const char *sender) {
    char *bus = NULL, *path = NULL;
    if (parse_spec(spec, sender, &bus, &path)) {
        item_add(bus, path);
        g_free(bus);
        g_free(path);
    }
}

// ---- watcher service (when we own the name) ----

static const char watcher_xml[] =
    "<node>"
    " <interface name='org.kde.StatusNotifierWatcher'>"
    "  <method name='RegisterStatusNotifierItem'>"
    "   <arg type='s' direction='in'/>"
    "  </method>"
    "  <method name='RegisterStatusNotifierHost'>"
    "   <arg type='s' direction='in'/>"
    "  </method>"
    "  <property name='RegisteredStatusNotifierItems' type='as' access='read'/>"
    "  <property name='IsStatusNotifierHostRegistered' type='b' access='read'/>"
    "  <property name='ProtocolVersion' type='i' access='read'/>"
    "  <signal name='StatusNotifierItemRegistered'><arg type='s'/></signal>"
    "  <signal name='StatusNotifierItemUnregistered'><arg type='s'/></signal>"
    "  <signal name='StatusNotifierHostRegistered'/>"
    " </interface>"
    "</node>";

static void watcher_method(GDBusConnection *c, const gchar *sender,
                           const gchar *path, const gchar *iface,
                           const gchar *method, GVariant *params,
                           GDBusMethodInvocation *inv, gpointer data) {
    (void)path;
    (void)iface;
    (void)data;
    const char *arg;
    g_variant_get(params, "(&s)", &arg);

    if (g_str_equal(method, "RegisterStatusNotifierItem")) {
        char *bus = NULL, *opath = NULL;
        if (parse_spec(arg, sender, &bus, &opath)) {
            char *canonical = g_strconcat(bus, opath, NULL);
            g_ptr_array_add(reg_items, canonical);
            item_add(bus, opath);
            g_dbus_connection_emit_signal(
                c, NULL, WATCHER_PATH, WATCHER_NAME,
                "StatusNotifierItemRegistered", g_variant_new("(s)", canonical),
                NULL);
            g_free(bus);
            g_free(opath);
        }
    } else if (g_str_equal(method, "RegisterStatusNotifierHost")) {
        g_dbus_connection_emit_signal(c, NULL, WATCHER_PATH, WATCHER_NAME,
                                      "StatusNotifierHostRegistered", NULL,
                                      NULL);
    }
    g_dbus_method_invocation_return_value(inv, NULL);
}

static GVariant *watcher_get_prop(GDBusConnection *c, const gchar *sender,
                                  const gchar *path, const gchar *iface,
                                  const gchar *prop, GError **err,
                                  gpointer data) {
    (void)c;
    (void)sender;
    (void)path;
    (void)iface;
    (void)err;
    (void)data;
    if (g_str_equal(prop, "RegisteredStatusNotifierItems")) {
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("as"));
        for (guint i = 0; i < reg_items->len; i++)
            g_variant_builder_add(&b, "s",
                                  (char *)g_ptr_array_index(reg_items, i));
        return g_variant_builder_end(&b);
    }
    if (g_str_equal(prop, "IsStatusNotifierHostRegistered"))
        return g_variant_new_boolean(TRUE);
    if (g_str_equal(prop, "ProtocolVersion"))
        return g_variant_new_int32(0);
    return NULL;
}

static const GDBusInterfaceVTable watcher_vtable = {
    .method_call = watcher_method,
    .get_property = watcher_get_prop,
};

static void on_bus_acquired(GDBusConnection *c, const gchar *name,
                            gpointer data) {
    (void)name;
    (void)data;
    GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(watcher_xml, NULL);
    watcher_reg_id = g_dbus_connection_register_object(
        c, WATCHER_PATH, node->interfaces[0], &watcher_vtable, NULL, NULL,
        NULL);
    g_dbus_node_info_unref(node);
}

static void on_watcher_acquired(GDBusConnection *c, const gchar *name,
                                gpointer data) {
    (void)c;
    (void)name;
    (void)data;
    i_am_watcher = TRUE;
}

static void on_watcher_lost(GDBusConnection *c, const gchar *name,
                            gpointer data) {
    (void)c;
    (void)name;
    (void)data;
    i_am_watcher = FALSE; // someone else is watcher; we stay queued as host
}

// ---- external watcher (waybar etc.) ----

static void on_watcher_signal(GDBusConnection *c, const gchar *sender,
                              const gchar *path, const gchar *iface,
                              const gchar *signal, GVariant *params,
                              gpointer data) {
    (void)c;
    (void)path;
    (void)iface;
    (void)data;
    if (i_am_watcher)
        return; // our own emissions are handled directly
    const char *spec;
    if (g_str_equal(signal, "StatusNotifierItemRegistered")) {
        g_variant_get(params, "(&s)", &spec);
        item_add_spec(spec, sender);
    } else if (g_str_equal(signal, "StatusNotifierItemUnregistered")) {
        g_variant_get(params, "(&s)", &spec);
        char *bus = NULL, *opath = NULL;
        if (parse_spec(spec, sender, &bus, &opath)) {
            item_remove(bus, opath);
            g_free(bus);
            g_free(opath);
        }
    }
}

static void query_existing_items(void) {
    GVariant *reply = g_dbus_connection_call_sync(
        conn, WATCHER_NAME, WATCHER_PATH, "org.freedesktop.DBus.Properties",
        "Get",
        g_variant_new("(ss)", WATCHER_NAME, "RegisteredStatusNotifierItems"),
        G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 500, NULL, NULL);
    if (!reply)
        return;
    GVariant *v;
    g_variant_get(reply, "(v)", &v);
    GVariantIter iter;
    const char *spec;
    g_variant_iter_init(&iter, v);
    while (g_variant_iter_next(&iter, "&s", &spec))
        item_add_spec(spec, NULL);
    g_variant_unref(v);
    g_variant_unref(reply);
}

static void on_host_acquired(GDBusConnection *c, const gchar *name,
                             gpointer data) {
    (void)data;
    // announce ourselves to whatever watcher exists
    g_dbus_connection_call(c, WATCHER_NAME, WATCHER_PATH, WATCHER_NAME,
                           "RegisterStatusNotifierHost",
                           g_variant_new("(s)", name), NULL,
                           G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

void tray_init(void) {
    conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if (!conn) {
        g_warning("tray: no session bus");
        return;
    }
    items = g_ptr_array_new();
    reg_items = g_ptr_array_new_with_free_func(g_free);

    // react to items (un)registering, whoever the watcher is
    g_dbus_connection_signal_subscribe(conn, NULL, WATCHER_NAME, NULL,
                                       WATCHER_PATH, NULL,
                                       G_DBUS_SIGNAL_FLAGS_NONE,
                                       on_watcher_signal, NULL, NULL);

    // pick up items already registered with an existing watcher
    query_existing_items();

    // queue for the watcher name (inherited if the current owner exits)
    g_bus_own_name(G_BUS_TYPE_SESSION, WATCHER_NAME,
                   G_BUS_NAME_OWNER_FLAGS_NONE, on_bus_acquired,
                   on_watcher_acquired, on_watcher_lost, NULL, NULL);

    char *host = g_strdup_printf("org.kde.StatusNotifierHost-%d-nekobar",
                                 getpid());
    g_bus_own_name(G_BUS_TYPE_SESSION, host, G_BUS_NAME_OWNER_FLAGS_NONE, NULL,
                   on_host_acquired, NULL, NULL, NULL);
    g_free(host);
}

// repopulate every bar's tray box (bars can come and go with monitors)
void tray_refresh(void) { rebuild_trays(); }
