// animeschedule.net weekly timetable: async curl fetch of
// /api/v3/timetables/sub with the user's app token (free: account
// settings -> API tab -> create an application), parsed per the documented
// Timetable Anime object. Results cached in memory for 30 minutes.

#include "schedule.h"

#include <gio/gio.h>
#include <json-glib/json-glib.h>

#define SCHEDULE_URL "https://animeschedule.net/api/v3/timetables/sub"
#define CACHE_TTL 1800

static char *token;
static GPtrArray *cached; // ScheduleEntry*
static gint64 fetched_at;
static GSList *waiters; // Waiter*, non-NULL while a fetch is in flight
static gboolean in_flight;

typedef struct {
    ScheduleCallback cb;
    gpointer data;
} Waiter;

static const char *service_url(void) {
    const char *env = g_getenv("NEKOLAND_SCHEDULE_URL"); // test override
    return env ? env : SCHEDULE_URL;
}

static void entry_free(gpointer p) {
    ScheduleEntry *e = p;
    g_free(e->title);
    g_free(e->romaji);
    g_free(e->english);
    g_free(e->airing_status);
    g_clear_pointer(&e->date, g_date_time_unref);
    g_free(e);
}

// ----------------------------------------------------------------- config --

static char *config_path(void) {
    return g_build_filename(g_get_user_config_dir(), "nekoland",
                            "animanager.ini", NULL);
}

void schedule_init(void) {
    char *path = config_path();
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL))
        token = g_key_file_get_string(kf, "schedule", "token", NULL);
    g_key_file_free(kf);
    g_free(path);
}

const char *schedule_token(void) { return token; }

void schedule_set_token(const char *t) {
    g_free(token);
    token = t && *t ? g_strdup(t) : NULL;
    g_clear_pointer(&cached, g_ptr_array_unref);
    fetched_at = 0;

    char *path = config_path();
    GKeyFile *kf = g_key_file_new();
    g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL);
    if (token)
        g_key_file_set_string(kf, "schedule", "token", token);
    else
        g_key_file_remove_key(kf, "schedule", "token", NULL);
    g_key_file_save_to_file(kf, path, NULL);
    g_key_file_free(kf);
    g_free(path);
}

// ------------------------------------------------------------------ fetch --

static const char *str_member(JsonObject *o, const char *name) {
    return json_object_has_member(o, name) &&
                   !json_object_get_null_member(o, name)
               ? json_object_get_string_member(o, name)
               : NULL;
}

static void notify_all(void) {
    GSList *list = waiters;
    waiters = NULL;
    in_flight = FALSE;
    for (GSList *l = list; l; l = l->next) {
        Waiter *w = l->data;
        w->cb(cached, w->data);
        g_free(w);
    }
    g_slist_free(list);
}

static void on_curl_done(GObject *src, GAsyncResult *res, gpointer data) {
    (void)data;
    char *out = NULL;
    GPtrArray *entries = NULL;

    if (g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res, &out,
                                             NULL, NULL) &&
        out) {
        JsonParser *parser = json_parser_new();
        if (json_parser_load_from_data(parser, out, -1, NULL)) {
            JsonNode *root = json_parser_get_root(parser);
            if (root && JSON_NODE_HOLDS_ARRAY(root)) {
                JsonArray *arr = json_node_get_array(root);
                entries = g_ptr_array_new_with_free_func(entry_free);
                for (guint i = 0; i < json_array_get_length(arr); i++) {
                    JsonNode *n = json_array_get_element(arr, i);
                    if (!JSON_NODE_HOLDS_OBJECT(n))
                        continue;
                    JsonObject *o = json_node_get_object(n);
                    ScheduleEntry *e = g_new0(ScheduleEntry, 1);
                    e->title = g_strdup(str_member(o, "title"));
                    e->romaji = g_strdup(str_member(o, "romaji"));
                    e->english = g_strdup(str_member(o, "english"));
                    e->airing_status =
                        g_strdup(str_member(o, "airingStatus"));
                    const char *d = str_member(o, "episodeDate");
                    if (d)
                        e->date = g_date_time_new_from_iso8601(d, NULL);
                    e->episode =
                        json_object_has_member(o, "episodeNumber")
                            ? (int)json_object_get_int_member(
                                  o, "episodeNumber")
                            : -1;
                    if (e->title || e->romaji)
                        g_ptr_array_add(entries, e);
                    else
                        entry_free(e);
                }
            }
        }
        g_object_unref(parser);
    }
    g_free(out);

    if (entries) {
        g_clear_pointer(&cached, g_ptr_array_unref);
        cached = entries;
        fetched_at = g_get_real_time() / G_USEC_PER_SEC;
    }
    // on failure keep the previous cache (if any) rather than nothing
    notify_all();
}

typedef struct {
    ScheduleCallback cb;
    gpointer data;
} IdleReply;

static gboolean idle_reply(gpointer data) {
    IdleReply *r = data;
    r->cb(cached, r->data);
    g_free(r);
    return G_SOURCE_REMOVE;
}

void schedule_fetch(ScheduleCallback cb, gpointer data) {
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    if (!token || (cached && now - fetched_at < CACHE_TTL)) {
        IdleReply *r = g_new0(IdleReply, 1);
        r->cb = cb;
        r->data = data;
        g_idle_add(idle_reply, r);
        return;
    }

    Waiter *w = g_new0(Waiter, 1);
    w->cb = cb;
    w->data = data;
    waiters = g_slist_append(waiters, w);
    if (in_flight)
        return;
    in_flight = TRUE;

    char *auth = g_strdup_printf("Authorization: Bearer %s", token);
    GSubprocess *proc = g_subprocess_new(
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
        NULL, "curl", "-s", "--globoff", "-m", "20", "-H", auth,
        service_url(), NULL);
    g_free(auth);
    if (proc) {
        g_subprocess_communicate_utf8_async(proc, NULL, NULL, on_curl_done,
                                            NULL);
        g_object_unref(proc);
    } else {
        notify_all();
    }
}
