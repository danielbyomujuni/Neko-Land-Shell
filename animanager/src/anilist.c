// AniList GraphQL lookups: async via curl subprocess, answers cached in
// ~/.cache/nekoland/animanager-anilist.ini, requests queued at one per 2s
// (their public rate limit is 30/min). No auth needed for public queries.

#include "anilist.h"

#include <gio/gio.h>
#include <json-glib/json-glib.h>

#define ANILIST_URL "https://graphql.anilist.co"
#define REQUEST_INTERVAL_MS 2000
#define TTL_FINISHED (7 * 24 * 3600) // finished shows don't change
#define TTL_AIRING (6 * 3600)        // airing counts move weekly
#define TTL_MISS (6 * 3600)          // not found / API down: retry later

typedef struct {
    AniInfo info;
    gint64 fetched; // unix time
} CacheEntry;

typedef struct {
    AniCallback cb;
    gpointer data;
} Waiter;

static GHashTable *cache;   // search key -> CacheEntry*
static GHashTable *waiters; // search key -> GSList of Waiter* (in flight)
static GQueue queue;        // search keys waiting for a request slot
static guint timer;

static const char *service_url(void) {
    const char *env = g_getenv("NEKOLAND_ANILIST_URL"); // test override
    return env ? env : ANILIST_URL;
}

// ----------------------------------------------------------------- cache --

static char *cache_path(void) {
    return g_build_filename(g_get_user_cache_dir(), "nekoland",
                            "animanager-anilist.ini", NULL);
}

static void cache_entry_free(gpointer p) {
    CacheEntry *e = p;
    g_free(e->info.title);
    g_free(e->info.season);
    g_free(e);
}

static void cache_load(void) {
    char *path = cache_path();
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        gsize n = 0;
        char **groups = g_key_file_get_groups(kf, &n);
        for (gsize i = 0; i < n; i++) {
            CacheEntry *e = g_new0(CacheEntry, 1);
            e->info.ok = g_key_file_get_boolean(kf, groups[i], "ok", NULL);
            e->info.episodes =
                g_key_file_get_integer(kf, groups[i], "episodes", NULL);
            e->info.aired =
                g_key_file_get_integer(kf, groups[i], "aired", NULL);
            e->info.airing =
                g_key_file_get_boolean(kf, groups[i], "airing", NULL);
            e->info.title =
                g_key_file_get_string(kf, groups[i], "title", NULL);
            e->info.season =
                g_key_file_get_string(kf, groups[i], "season", NULL);
            e->info.season_year = g_key_file_has_key(kf, groups[i],
                                                     "season-year", NULL)
                                      ? g_key_file_get_integer(
                                            kf, groups[i], "season-year",
                                            NULL)
                                      : -1;
            e->fetched =
                g_key_file_get_int64(kf, groups[i], "fetched", NULL);
            g_hash_table_replace(cache, g_strdup(groups[i]), e);
        }
        g_strfreev(groups);
    }
    g_key_file_free(kf);
    g_free(path);
}

static void cache_save(void) {
    char *path = cache_path();
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);

    GKeyFile *kf = g_key_file_new();
    GHashTableIter it;
    gpointer key, val;
    g_hash_table_iter_init(&it, cache);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        CacheEntry *e = val;
        g_key_file_set_boolean(kf, key, "ok", e->info.ok);
        g_key_file_set_integer(kf, key, "episodes", e->info.episodes);
        g_key_file_set_integer(kf, key, "aired", e->info.aired);
        g_key_file_set_boolean(kf, key, "airing", e->info.airing);
        if (e->info.title)
            g_key_file_set_string(kf, key, "title", e->info.title);
        if (e->info.season)
            g_key_file_set_string(kf, key, "season", e->info.season);
        g_key_file_set_integer(kf, key, "season-year", e->info.season_year);
        g_key_file_set_int64(kf, key, "fetched", e->fetched);
    }
    g_key_file_save_to_file(kf, path, NULL);
    g_key_file_free(kf);
    g_free(path);
}

static gboolean entry_fresh(CacheEntry *e) {
    gint64 age = g_get_real_time() / G_USEC_PER_SEC - e->fetched;
    if (!e->info.ok)
        return age < TTL_MISS;
    return age < (e->info.airing ? TTL_AIRING : TTL_FINISHED);
}

// ---------------------------------------------------------------- fetch --

static void notify_waiters(const char *key) {
    CacheEntry *e = g_hash_table_lookup(cache, key);
    GSList *list = g_hash_table_lookup(waiters, key);
    for (GSList *l = list; l; l = l->next) {
        Waiter *w = l->data;
        w->cb(e ? &e->info : NULL, w->data);
        g_free(w);
    }
    g_slist_free(list);
    g_hash_table_remove(waiters, key);
}

static CacheEntry *store_miss(const char *key) {
    CacheEntry *e = g_new0(CacheEntry, 1);
    e->info.episodes = -1;
    e->info.aired = -1;
    e->info.season_year = -1;
    e->fetched = g_get_real_time() / G_USEC_PER_SEC;
    g_hash_table_replace(cache, g_strdup(key), e);
    return e;
}

static void on_curl_done(GObject *src, GAsyncResult *res, gpointer data) {
    char *key = data;
    char *out = NULL;
    CacheEntry *e = NULL;

    if (g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res, &out,
                                             NULL, NULL) &&
        out) {
        JsonParser *parser = json_parser_new();
        if (json_parser_load_from_data(parser, out, -1, NULL)) {
            JsonObject *root =
                json_node_get_object(json_parser_get_root(parser));
            JsonObject *d =
                json_object_has_member(root, "data") &&
                        !json_object_get_null_member(root, "data")
                    ? json_object_get_object_member(root, "data")
                    : NULL;
            JsonObject *media =
                d && json_object_has_member(d, "Media") &&
                        !json_object_get_null_member(d, "Media")
                    ? json_object_get_object_member(d, "Media")
                    : NULL;
            if (media) {
                e = store_miss(key);
                e->info.ok = TRUE;
                if (json_object_has_member(media, "episodes") &&
                    !json_object_get_null_member(media, "episodes"))
                    e->info.episodes =
                        json_object_get_int_member(media, "episodes");
                const char *status =
                    json_object_has_member(media, "status")
                        ? json_object_get_string_member(media, "status")
                        : "";
                e->info.airing = g_strcmp0(status, "RELEASING") == 0;
                if (json_object_has_member(media, "nextAiringEpisode") &&
                    !json_object_get_null_member(media,
                                                 "nextAiringEpisode")) {
                    JsonObject *next = json_object_get_object_member(
                        media, "nextAiringEpisode");
                    e->info.aired =
                        json_object_get_int_member(next, "episode") - 1;
                }
                JsonObject *title =
                    json_object_get_object_member(media, "title");
                if (title && json_object_has_member(title, "romaji"))
                    e->info.title = g_strdup(
                        json_object_get_string_member(title, "romaji"));
                if (json_object_has_member(media, "season") &&
                    !json_object_get_null_member(media, "season"))
                    e->info.season = g_strdup(
                        json_object_get_string_member(media, "season"));
                if (json_object_has_member(media, "seasonYear") &&
                    !json_object_get_null_member(media, "seasonYear"))
                    e->info.season_year =
                        json_object_get_int_member(media, "seasonYear");
            }
        }
        g_object_unref(parser);
    }
    g_free(out);

    if (!e)
        store_miss(key); // API down, no match, or parse failure
    cache_save();
    notify_waiters(key);
    g_free(key);
}

static gboolean process_queue(gpointer data) {
    (void)data;
    char *key = g_queue_pop_head(&queue);
    if (!key) {
        timer = 0;
        return G_SOURCE_REMOVE;
    }

    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "query");
    json_builder_add_string_value(
        b, "query($s:String){Media(search:$s,type:ANIME){episodes status "
           "season seasonYear title{romaji} nextAiringEpisode{episode}}}");
    json_builder_set_member_name(b, "variables");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "s");
    json_builder_add_string_value(b, key);
    json_builder_end_object(b);
    json_builder_end_object(b);
    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, json_builder_get_root(b));
    char *body = json_generator_to_data(gen, NULL);
    g_object_unref(gen);
    g_object_unref(b);

    GSubprocess *proc = g_subprocess_new(
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
        NULL, "curl", "-s", "-m", "15", "-X", "POST", service_url(), "-H",
        "Content-Type: application/json", "-d", body, NULL);
    g_free(body);
    if (proc) {
        g_subprocess_communicate_utf8_async(proc, NULL, NULL, on_curl_done,
                                            key);
        g_object_unref(proc);
    } else {
        store_miss(key);
        notify_waiters(key);
        g_free(key);
    }
    return G_SOURCE_CONTINUE;
}

// ------------------------------------------------------------------ api --

void anilist_init(void) {
    cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                  cache_entry_free);
    waiters = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_queue_init(&queue);
    cache_load();
}

typedef struct {
    char *key;
    AniCallback cb;
    gpointer data;
} IdleReply;

static gboolean idle_reply(gpointer data) {
    IdleReply *r = data;
    CacheEntry *e = g_hash_table_lookup(cache, r->key);
    r->cb(e ? &e->info : NULL, r->data);
    g_free(r->key);
    g_free(r);
    return G_SOURCE_REMOVE;
}

void anilist_lookup(const char *search, AniCallback cb, gpointer data) {
    CacheEntry *e = g_hash_table_lookup(cache, search);
    if (e && entry_fresh(e)) {
        IdleReply *r = g_new0(IdleReply, 1);
        r->key = g_strdup(search);
        r->cb = cb;
        r->data = data;
        g_idle_add(idle_reply, r);
        return;
    }

    Waiter *w = g_new0(Waiter, 1);
    w->cb = cb;
    w->data = data;
    GSList *list = g_hash_table_lookup(waiters, search);
    gboolean in_flight = list != NULL;
    list = g_slist_append(list, w);
    g_hash_table_replace(waiters, g_strdup(search), list);
    if (in_flight)
        return; // request already queued or running

    g_queue_push_tail(&queue, g_strdup(search));
    if (!timer)
        timer = g_timeout_add(REQUEST_INTERVAL_MS, process_queue, NULL);
}
