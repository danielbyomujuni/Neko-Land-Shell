#include "nekobar.h"

#include <json-glib/json-glib.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static char *socket_path(const char *file) {
    const char *rt = g_getenv("XDG_RUNTIME_DIR");
    const char *sig = g_getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (!rt || !sig)
        return NULL;
    return g_strdup_printf("%s/hypr/%s/%s", rt, sig, file);
}

static int sock_connect(const char *file) {
    char *path = socket_path(file);
    if (!path)
        return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    g_strlcpy(addr.sun_path, path, sizeof(addr.sun_path));
    g_free(path);
    if (fd < 0)
        return -1;
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

char *hypr_request(const char *req) {
    int fd = sock_connect(".socket.sock");
    if (fd < 0)
        return NULL;
    if (write(fd, req, strlen(req)) < 0) {
        close(fd);
        return NULL;
    }
    GString *buf = g_string_new(NULL);
    char chunk[8192];
    ssize_t n;
    while ((n = read(fd, chunk, sizeof(chunk))) > 0)
        g_string_append_len(buf, chunk, n);
    close(fd);
    return g_string_free(buf, FALSE);
}

void hypr_dispatch(const char *cmd) {
    char *req = g_strdup_printf("dispatch %s", cmd);
    char *reply = hypr_request(req);
    g_free(reply);
    g_free(req);
}

gboolean hypr_monitor_name_at(int x, int y, char *out, gsize outlen) {
    gboolean found = FALSE;
    char *json = hypr_request("j/monitors");
    if (!json)
        return FALSE;
    JsonParser *p = json_parser_new();
    if (json_parser_load_from_data(p, json, -1, NULL)) {
        JsonArray *arr = json_node_get_array(json_parser_get_root(p));
        for (guint i = 0; i < json_array_get_length(arr); i++) {
            JsonObject *m = json_array_get_object_element(arr, i);
            if (json_object_get_int_member(m, "x") == x &&
                json_object_get_int_member(m, "y") == y) {
                g_strlcpy(out, json_object_get_string_member(m, "name"), outlen);
                found = TRUE;
                break;
            }
        }
    }
    g_object_unref(p);
    g_free(json);
    return found;
}

// ---- workspaces ----

static void ws_clicked(GtkWidget *btn, gpointer data) {
    (void)btn;
    char cmd[64];
    g_snprintf(cmd, sizeof(cmd), "workspace %d", GPOINTER_TO_INT(data));
    hypr_dispatch(cmd);
}

static void clear_children(GtkWidget *box) {
    GList *kids = gtk_container_get_children(GTK_CONTAINER(box));
    for (GList *l = kids; l; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(kids);
}

typedef struct {
    gint64 id;
    const char *monitor;
} WsInfo;

static gint ws_cmp(gconstpointer a, gconstpointer b) {
    const WsInfo *wa = a, *wb = b;
    return (wa->id > wb->id) - (wa->id < wb->id);
}

void hypr_refresh_workspaces(void) {
    char *ws_json = hypr_request("j/workspaces");
    char *mon_json = hypr_request("j/monitors");
    if (!ws_json || !mon_json) {
        g_free(ws_json);
        g_free(mon_json);
        return;
    }

    JsonParser *wp = json_parser_new(), *mp = json_parser_new();
    if (!json_parser_load_from_data(wp, ws_json, -1, NULL) ||
        !json_parser_load_from_data(mp, mon_json, -1, NULL))
        goto out;

    // monitor name -> active workspace id, and whether that monitor is focused
    GHashTable *active = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    char focused_mon[64] = "";
    JsonArray *mons = json_node_get_array(json_parser_get_root(mp));
    for (guint i = 0; i < json_array_get_length(mons); i++) {
        JsonObject *m = json_array_get_object_element(mons, i);
        const char *name = json_object_get_string_member(m, "name");
        JsonObject *aw = json_object_get_object_member(m, "activeWorkspace");
        gint64 aw_id = json_object_get_int_member(aw, "id");
        g_hash_table_insert(active, g_strdup(name), GINT_TO_POINTER((int)aw_id));
        if (json_object_get_boolean_member(m, "focused"))
            g_strlcpy(focused_mon, name, sizeof(focused_mon));
    }

    GArray *list = g_array_new(FALSE, FALSE, sizeof(WsInfo));
    JsonArray *wss = json_node_get_array(json_parser_get_root(wp));
    for (guint i = 0; i < json_array_get_length(wss); i++) {
        JsonObject *w = json_array_get_object_element(wss, i);
        WsInfo info = {
            .id = json_object_get_int_member(w, "id"),
            .monitor = json_object_get_string_member(w, "monitor"),
        };
        if (info.id > 0) // skip special workspaces
            g_array_append_val(list, info);
    }
    g_array_sort(list, ws_cmp);

    for (guint b = 0; b < bars->len; b++) {
        Bar *bar = g_ptr_array_index(bars, b);
        clear_children(bar->ws_box);
        int mon_active = GPOINTER_TO_INT(
            g_hash_table_lookup(active, bar->hypr_name));
        gboolean mon_focused = g_str_equal(bar->hypr_name, focused_mon);

        for (guint i = 0; i < list->len; i++) {
            WsInfo *w = &g_array_index(list, WsInfo, i);
            if (!w->monitor || !g_str_equal(w->monitor, bar->hypr_name))
                continue;
            gboolean is_active = w->id == mon_active;
            GtkWidget *btn = gtk_button_new_with_label(
                is_active ? "" : "");
            gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);
            GtkStyleContext *sc = gtk_widget_get_style_context(btn);
            if (is_active)
                gtk_style_context_add_class(sc, mon_focused ? "active" : "visible");
            g_signal_connect(btn, "clicked", G_CALLBACK(ws_clicked),
                             GINT_TO_POINTER((int)w->id));
            gtk_box_pack_start(GTK_BOX(bar->ws_box), btn, FALSE, FALSE, 0);
        }
        gtk_widget_show_all(bar->ws_box);
    }

    g_array_free(list, TRUE);
    g_hash_table_destroy(active);
out:
    g_object_unref(wp);
    g_object_unref(mp);
    g_free(ws_json);
    g_free(mon_json);
}

// ---- window title ----

typedef struct {
    GRegex *re;
    const char *prefix;
} Rewrite;

static GPtrArray *rewrites;

static void add_rewrite(const char *pattern, const char *prefix) {
    Rewrite *r = g_new0(Rewrite, 1);
    r->re = g_regex_new(pattern, 0, 0, NULL);
    r->prefix = prefix;
    g_ptr_array_add(rewrites, r);
}

static void rewrites_init(void) {
    rewrites = g_ptr_array_new();
    add_rewrite("^(.*) — Mozilla Firefox$", "\U000F0239  ");
    add_rewrite("^(.*) — Zen Browser$", "\U000F0239  Zen - ");
    add_rewrite("^(.*) - Google Chrome$", "  ");
    add_rewrite("^(.*) - Visual Studio Code$", "\U000F0A1E  ");
    add_rewrite("^(.*) - VSCodium$", "\U000F0A1E  ");
    add_rewrite("^(.*) - nvim$", "  ");
    add_rewrite("^(.*) - Obsidian.*$", "\U000F14E7  ");
    add_rewrite("^(.*) - fish$", "  ");
    add_rewrite("^yazi: (.*)$", "  ");
}

static char *rewrite_title(const char *t) {
    if (g_str_equal(t, "nwg-look"))
        return g_strdup("  GTK Settings");
    if (g_str_equal(t, "Qt6 Configuration Tool"))
        return g_strdup("  QT Settings");
    if (g_str_equal(t, "blueman-manager"))
        return g_strdup("Bluetooth Settings");
    for (guint i = 0; i < rewrites->len; i++) {
        Rewrite *r = g_ptr_array_index(rewrites, i);
        GMatchInfo *mi = NULL;
        if (g_regex_match(r->re, t, 0, &mi)) {
            char *cap = g_match_info_fetch(mi, 1);
            char *out = g_strconcat(r->prefix, cap, NULL);
            g_free(cap);
            g_match_info_free(mi);
            return out;
        }
        g_match_info_free(mi);
    }
    return g_strdup(t);
}

void hypr_refresh_title(void) {
    char *json = hypr_request("j/activewindow");
    char *title = NULL;
    if (json) {
        JsonParser *p = json_parser_new();
        if (json_parser_load_from_data(p, json, -1, NULL)) {
            JsonNode *root = json_parser_get_root(p);
            if (root && JSON_NODE_HOLDS_OBJECT(root)) {
                JsonObject *o = json_node_get_object(root);
                if (json_object_has_member(o, "title"))
                    title = g_strdup(json_object_get_string_member(o, "title"));
            }
        }
        g_object_unref(p);
        g_free(json);
    }
    char *shown = rewrite_title(title ? title : "");
    for (guint b = 0; b < bars->len; b++) {
        Bar *bar = g_ptr_array_index(bars, b);
        gtk_label_set_text(GTK_LABEL(bar->title_label), shown);
    }
    g_free(shown);
    g_free(title);
}

// ---- event socket ----

static gboolean on_event(GIOChannel *ch, GIOCondition cond, gpointer data) {
    (void)cond;
    (void)data;
    char *line = NULL;
    gsize len = 0;
    GIOStatus st;
    gboolean ws_dirty = FALSE, title_dirty = FALSE;
    while ((st = g_io_channel_read_line(ch, &line, &len, NULL, NULL)) ==
           G_IO_STATUS_NORMAL) {
        if (g_str_has_prefix(line, "workspace") ||
            g_str_has_prefix(line, "createworkspace") ||
            g_str_has_prefix(line, "destroyworkspace") ||
            g_str_has_prefix(line, "moveworkspace") ||
            g_str_has_prefix(line, "focusedmon") ||
            g_str_has_prefix(line, "monitor"))
            ws_dirty = TRUE;
        if (g_str_has_prefix(line, "activewindow") ||
            g_str_has_prefix(line, "focusedmon") ||
            g_str_has_prefix(line, "closewindow"))
            title_dirty = TRUE;
        g_free(line);
        line = NULL;
    }
    g_free(line);
    if (ws_dirty)
        hypr_refresh_workspaces();
    if (title_dirty)
        hypr_refresh_title();
    return st != G_IO_STATUS_EOF;
}

void hypr_init(void) {
    rewrites_init();
    int fd = sock_connect(".socket2.sock");
    if (fd < 0) {
        g_warning("could not connect to hyprland event socket");
        return;
    }
    GIOChannel *ch = g_io_channel_unix_new(fd);
    g_io_channel_set_encoding(ch, NULL, NULL);
    g_io_channel_set_flags(ch, G_IO_FLAG_NONBLOCK, NULL);
    g_io_add_watch(ch, G_IO_IN | G_IO_HUP, on_event, NULL);
}
