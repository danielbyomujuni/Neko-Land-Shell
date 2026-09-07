// Manual episode fetching for missing episodes.
//
// Two provider backends, both keyed off the same missing-episode list:
//   - SubsPlease: the public JSON search API
//     (https://subsplease.org/api/?f=search&s=<show>&p=<page>), which covers
//     the full release history and hands back per-resolution magnet links.
//   - Erai-raws: Nyaa's public RSS search for "[Erai-raws] <show> <ep>",
//     which mirrors the same releases without needing an erai-raws.info
//     login token, and additionally yields direct .torrent URLs, sizes and
//     seeder counts.
//
// Downloads: the .torrent (when the backend provides one) is saved into the
// episode's own folder, then the release is handed to a downloader — aria2c
// straight into that folder when installed, otherwise the desktop's torrent
// client via xdg-open. Install aria2c for fully automatic downloads.

#include "fetch.h"

#include <stdlib.h>
#include <string.h>

#include <gio/gio.h>
#include <json-glib/json-glib.h>

#include "anilist.h"

#define UA "nekoland-animanager/1.0"
#define MAX_SP_PAGES 6
#define MAX_TAIL_PER_GROUP 50

static const char *subsplease_base(void) {
    const char *env = g_getenv("NEKOLAND_SUBSPLEASE_URL"); // test override
    return env ? env : "https://subsplease.org";
}

static const char *nyaa_base(void) {
    const char *env = g_getenv("NEKOLAND_NYAA_URL"); // test override
    return env ? env : "https://nyaa.si";
}

// ------------------------------------------------------------------- prefs --

static char *prefs_path(void) {
    return g_build_filename(g_get_user_config_dir(), "nekoland",
                            "animanager.ini", NULL);
}

static void prefs_load(FetchProvider *provider, char **quality) {
    *provider = FETCH_SUBSPLEASE;
    *quality = g_strdup("1080");
    char *path = prefs_path();
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        char *p = g_key_file_get_string(kf, "fetch", "provider", NULL);
        if (p) {
            if (g_ascii_strcasecmp(p, "erai-raws") == 0 ||
                g_ascii_strcasecmp(p, "erai") == 0)
                *provider = FETCH_ERAI;
            g_free(p);
        }
        char *q = g_key_file_get_string(kf, "fetch", "quality", NULL);
        if (q) {
            if (strcmp(q, "480") == 0 || strcmp(q, "720") == 0 ||
                strcmp(q, "1080") == 0) {
                g_free(*quality);
                *quality = q;
            } else {
                g_free(q);
            }
        }
    }
    g_key_file_free(kf);
    g_free(path);
}

static void prefs_save(FetchProvider provider, const char *quality) {
    char *path = prefs_path();
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);
    GKeyFile *kf = g_key_file_new();
    // preserve the [library] section owned by main.c
    g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL);
    g_key_file_set_string(kf, "fetch", "provider",
                          provider == FETCH_ERAI ? "Erai-raws" : "SubsPlease");
    g_key_file_set_string(kf, "fetch", "quality", quality);
    g_key_file_save_to_file(kf, path, NULL);
    g_key_file_free(kf);
    g_free(path);
}

// ------------------------------------------------------------------ results --

void fetch_result_free(gpointer p) {
    FetchResult *r = p;
    g_free(r->title);
    g_free(r->magnet);
    g_free(r->torrent_url);
    g_free(r->size);
    g_free(r);
}

static FetchResult *result_copy(const FetchResult *src) {
    FetchResult *r = g_new0(FetchResult, 1);
    r->episode = src->episode;
    r->title = g_strdup(src->title);
    r->magnet = g_strdup(src->magnet);
    r->torrent_url = g_strdup(src->torrent_url);
    r->size = g_strdup(src->size);
    r->seeders = src->seeders;
    return r;
}

typedef struct {
    FetchCallback cb;
    gpointer data;
} EmptyCtx;

static gboolean empty_idle(gpointer data) {
    EmptyCtx *c = data;
    GPtrArray *arr = g_ptr_array_new_with_free_func(fetch_result_free);
    c->cb(arr, c->data);
    g_ptr_array_unref(arr);
    g_free(c);
    return G_SOURCE_REMOVE;
}

static void reply_empty(FetchCallback cb, gpointer data) {
    EmptyCtx *c = g_new0(EmptyCtx, 1);
    c->cb = cb;
    c->data = data;
    g_idle_add(empty_idle, c);
}

// ------------------------------------------------------------- subsplease --

typedef struct {
    FetchCallback cb;
    gpointer data;
    char *show;
    int episode;
    char *quality;
    int page;
} SpSearch;

static void sp_search_free(SpSearch *s) {
    g_free(s->show);
    g_free(s->quality);
    g_free(s);
}

// The API's episode field is a plain number for single episodes ("7",
// "1175"); batches look like "01-13" and must not match a single fetch.
static gboolean sp_episode_match(const char *epstr, int wanted) {
    if (!epstr || !*epstr)
        return FALSE;
    char *end = NULL;
    long v = strtol(epstr, &end, 10);
    return end != epstr && *end == '\0' && v == wanted;
}

// size display from the magnet's xl=<bytes> parameter
static char *magnet_size(const char *magnet) {
    const char *p = strstr(magnet, "xl=");
    if (!p)
        return NULL;
    char *end = NULL;
    gint64 bytes = g_ascii_strtoll(p + 3, &end, 10);
    if (bytes <= 0)
        return NULL;
    return g_format_size((guint64)bytes);
}

static void sp_fetch_page(SpSearch *s);

static void on_sp_page(GObject *src, GAsyncResult *res, gpointer data) {
    SpSearch *s = data;
    char *out = NULL;
    FetchResult *found = NULL;

    if (g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res, &out,
                                             NULL, NULL) &&
        out) {
        JsonParser *parser = json_parser_new();
        if (json_parser_load_from_data(parser, out, -1, NULL)) {
            JsonNode *root = json_parser_get_root(parser);
            JsonObject *obj =
                root && JSON_NODE_HOLDS_OBJECT(root)
                    ? json_node_get_object(root)
                    : NULL;
            if (obj) {
                GList *members = json_object_get_members(obj);
                for (GList *l = members; l && !found; l = l->next) {
                    const char *key = l->data;
                    JsonObject *e =
                        json_object_get_object_member(obj, key);
                    const char *ep =
                        e && json_object_has_member(e, "episode")
                            ? json_object_get_string_member(e, "episode")
                            : NULL;
                    if (!sp_episode_match(ep, s->episode))
                        continue;
                    JsonArray *dls =
                        e && json_object_has_member(e, "downloads")
                            ? json_object_get_array_member(e, "downloads")
                            : NULL;
                    if (!dls || json_array_get_length(dls) == 0)
                        continue;
                    guint n = json_array_get_length(dls);
                    JsonObject *pick = NULL;
                    for (guint i = 0; i < n && !pick; i++) {
                        JsonObject *d = json_array_get_object_element(dls, i);
                        const char *r =
                            json_object_has_member(d, "res")
                                ? json_object_get_string_member(d, "res")
                                : "";
                        if (strcmp(r, s->quality) == 0)
                            pick = d;
                    }
                    for (guint i = 0; i < n && !pick; i++) {
                        JsonObject *d = json_array_get_object_element(dls, i);
                        const char *r =
                            json_object_has_member(d, "res")
                                ? json_object_get_string_member(d, "res")
                                : "";
                        if (strcmp(r, "1080") == 0)
                            pick = d;
                    }
                    if (!pick)
                        pick = json_array_get_object_element(dls, 0);
                    const char *mag =
                        json_object_has_member(pick, "magnet")
                            ? json_object_get_string_member(pick, "magnet")
                            : NULL;
                    if (!mag)
                        continue;
                    found = g_new0(FetchResult, 1);
                    found->episode = s->episode;
                    found->title = g_strdup(key);
                    found->magnet = g_strdup(mag);
                    found->size = magnet_size(mag);
                    found->seeders = -1;
                }
                g_list_free(members);
            }
        }
        g_object_unref(parser);
    }
    g_free(out);

    if (found || s->page + 1 >= MAX_SP_PAGES) {
        GPtrArray *arr = g_ptr_array_new_with_free_func(fetch_result_free);
        if (found)
            g_ptr_array_add(arr, found);
        s->cb(arr, s->data);
        g_ptr_array_unref(arr);
        sp_search_free(s);
        return;
    }
    s->page++;
    sp_fetch_page(s);
}

static void sp_fetch_page(SpSearch *s) {
    char *q = g_uri_escape_string(s->show, NULL, FALSE);
    char *url = g_strdup_printf("%s/api/?f=search&tz=UTC&s=%s&p=%d",
                                subsplease_base(), q, s->page);
    g_free(q);
    GSubprocess *proc = g_subprocess_new(
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
        NULL, "curl", "-s", "-m", "20", "--globoff", "-A", UA, url, NULL);
    g_free(url);
    if (proc) {
        g_subprocess_communicate_utf8_async(proc, NULL, NULL, on_sp_page, s);
        g_object_unref(proc);
    } else {
        reply_empty(s->cb, s->data);
        sp_search_free(s);
    }
}

// ------------------------------------------------------------------- nyaa --
// Erai-raws via Nyaa's public RSS: no login token needed, and every item
// carries a direct .torrent URL plus seeders.

typedef struct {
    FetchCallback cb;
    gpointer data;
    char *show;
    int episode;
    char *quality; // "480" / "720" / "1080"
    int attempt;   // 0 = zero-padded number, 1 = plain number
} NyaaSearch;

static void nyaa_search_free(NyaaSearch *s) {
    g_free(s->show);
    g_free(s->quality);
    g_free(s);
}

static char *xml_tag(const char *block, const char *tag) {
    char *open = g_strdup_printf("<%s>", tag);
    char *close = g_strdup_printf("</%s>", tag);
    const char *a = strstr(block, open);
    char *out = NULL;
    if (a) {
        a += strlen(open);
        const char *b = strstr(a, close);
        if (b)
            out = g_strndup(a, (gsize)(b - a));
    }
    g_free(open);
    g_free(close);
    return out;
}

static char *xml_unescape(const char *s) {
    GString *g = g_string_new(NULL);
    for (const char *p = s; *p;) {
        if (*p != '&') {
            g_string_append_c(g, *p);
            p++;
        } else if (g_str_has_prefix(p, "&amp;")) {
            g_string_append_c(g, '&');
            p += 5;
        } else if (g_str_has_prefix(p, "&lt;")) {
            g_string_append_c(g, '<');
            p += 4;
        } else if (g_str_has_prefix(p, "&gt;")) {
            g_string_append_c(g, '>');
            p += 4;
        } else if (g_str_has_prefix(p, "&quot;")) {
            g_string_append_c(g, '"');
            p += 6;
        } else if (g_str_has_prefix(p, "&#")) {
            const char *q = p + 2;
            int base = 10;
            if (*q == 'x' || *q == 'X') {
                base = 16;
                q++;
            }
            char *end = NULL;
            long v = strtol(q, &end, base);
            if (end != q && *end == ';' && v > 0 && v < 0x110000) {
                g_string_append_unichar(g, (gunichar)v);
                p = end + 1;
            } else {
                g_string_append_c(g, *p);
                p++;
            }
        } else {
            g_string_append_c(g, *p);
            p++;
        }
    }
    return g_string_free(g, FALSE);
}

// Erai-raws tags look like "[1080p CR WEB-DL ...]", SubsPlease-style like
// "(1080p)" — prefix-match inside either bracket style.
static gboolean title_has_res(const char *title, const char *quality) {
    char *a = g_strdup_printf("[%sp", quality);
    char *b = g_strdup_printf("(%sp", quality);
    gboolean ok = strstr(title, a) != NULL || strstr(title, b) != NULL;
    g_free(a);
    g_free(b);
    return ok;
}

static char *nyaa_magnet(const char *hash, const char *title) {
    char *dn = g_uri_escape_string(title, NULL, FALSE);
    char *m = g_strdup_printf(
        "magnet:?xt=urn:btih:%s&dn=%s"
        "&tr=http%%3A%%2F%%2Fnyaa.tracker.wf%%3A7777%%2Fannounce"
        "&tr=udp%%3A%%2F%%2Ftracker.opentrackr.org%%3A1337%%2Fannounce"
        "&tr=udp%%3A%%2F%%2Fopen.stealth.si%%3A80%%2Fannounce",
        hash, dn);
    g_free(dn);
    return m;
}

static int seed_cmp(gconstpointer a, gconstpointer b) {
    const FetchResult *ra = *(FetchResult *const *)a;
    const FetchResult *rb = *(FetchResult *const *)b;
    return rb->seeders - ra->seeders;
}

static void nyaa_fetch(NyaaSearch *s);

static void on_nyaa_done(GObject *src, GAsyncResult *res, gpointer data) {
    NyaaSearch *s = data;
    char *out = NULL;
    GPtrArray *arr = g_ptr_array_new_with_free_func(fetch_result_free);

    if (g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res, &out,
                                             NULL, NULL) &&
        out) {
        const char *p = out;
        while ((p = strstr(p, "<item>")) != NULL) {
            const char *end = strstr(p, "</item>");
            if (!end)
                break;
            char *block = g_strndup(p, (gsize)(end - p));
            char *t = xml_tag(block, "title");
            char *link = xml_tag(block, "link");
            char *size = xml_tag(block, "nyaa:size");
            char *seed = xml_tag(block, "nyaa:seeders");
            char *hash = xml_tag(block, "nyaa:infoHash");
            if (t) {
                char *title = xml_unescape(t);
                if (title_has_res(title, s->quality) &&
                    episode_number(title) == s->episode) {
                    FetchResult *r = g_new0(FetchResult, 1);
                    r->episode = s->episode;
                    r->title = title;
                    title = NULL;
                    r->torrent_url = link;
                    link = NULL;
                    r->size = size;
                    size = NULL;
                    r->seeders = seed ? atoi(seed) : 0;
                    if (hash)
                        r->magnet = nyaa_magnet(hash, r->title);
                    g_ptr_array_add(arr, r);
                }
                g_free(title);
            }
            g_free(t);
            g_free(link);
            g_free(size);
            g_free(seed);
            g_free(hash);
            g_free(block);
            p = end + 7;
        }
        g_ptr_array_sort(arr, seed_cmp);
    }
    g_free(out);

    // The feed pads single digits ("- 07"); if the padded query found
    // nothing, retry once with the plain number (and vice versa).
    if (arr->len == 0 && s->attempt == 0) {
        char a[16], b[16];
        g_snprintf(a, sizeof a, "%02d", s->episode);
        g_snprintf(b, sizeof b, "%d", s->episode);
        if (strcmp(a, b) != 0) {
            g_ptr_array_unref(arr);
            s->attempt = 1;
            nyaa_fetch(s);
            return;
        }
    }
    s->cb(arr, s->data);
    g_ptr_array_unref(arr);
    nyaa_search_free(s);
}

static void nyaa_fetch(NyaaSearch *s) {
    char epbuf[16];
    if (s->attempt == 0)
        g_snprintf(epbuf, sizeof epbuf, "%02d", s->episode);
    else
        g_snprintf(epbuf, sizeof epbuf, "%d", s->episode);
    char *raw = g_strdup_printf("[Erai-raws] %s %s", s->show, epbuf);
    char *q = g_uri_escape_string(raw, NULL, FALSE);
    g_free(raw);
    char *url =
        g_strdup_printf("%s/?page=rss&q=%s&c=1_2&f=0", nyaa_base(), q);
    g_free(q);
    GSubprocess *proc = g_subprocess_new(
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
        NULL, "curl", "-sL", "-m", "20", "--globoff", "-A", UA, url, NULL);
    g_free(url);
    if (proc) {
        g_subprocess_communicate_utf8_async(proc, NULL, NULL, on_nyaa_done,
                                            s);
        g_object_unref(proc);
    } else {
        reply_empty(s->cb, s->data);
        nyaa_search_free(s);
    }
}

void fetch_search(FetchProvider provider, const char *show, int episode,
                  const char *quality, FetchCallback cb, gpointer user_data) {
    if (!show || !*show || episode <= 0 || !quality) {
        reply_empty(cb, user_data);
        return;
    }
    if (provider == FETCH_ERAI) {
        NyaaSearch *s = g_new0(NyaaSearch, 1);
        s->cb = cb;
        s->data = user_data;
        s->show = g_strdup(show);
        s->episode = episode;
        s->quality = g_strdup(quality);
        s->attempt = 0;
        nyaa_fetch(s);
    } else {
        SpSearch *s = g_new0(SpSearch, 1);
        s->cb = cb;
        s->data = user_data;
        s->show = g_strdup(show);
        s->episode = episode;
        s->quality = g_strdup(quality);
        s->page = 0;
        sp_fetch_page(s);
    }
}

// ---------------------------------------------------------------- download --

gboolean fetch_have_aria2(void) {
    static int have = -1;
    if (have < 0) {
        char *p = g_find_program_in_path("aria2c");
        have = p ? 1 : 0;
        g_free(p);
    }
    return have == 1;
}

typedef struct {
    FetchDlCallback cb;
    gpointer data;
    FetchResult *res; // copy
    char *dest;
    char *torrent_file; // saved path, or NULL
} DlCtx;

static void dl_ctx_free(DlCtx *c) {
    fetch_result_free(c->res);
    g_free(c->dest);
    g_free(c->torrent_file);
    g_free(c);
}

static char *sanitize_filename(const char *title) {
    GString *g = g_string_new(NULL);
    for (const char *p = title; *p && g->len < 120; p++) {
        char c = *p;
        if (g_ascii_isalnum(c) || strchr(" ._-[]()", c))
            g_string_append_c(g, c);
        else
            g_string_append_c(g, '_');
    }
    if (g->len == 0)
        g_string_append(g, "episode");
    g_string_append(g, ".torrent");
    return g_string_free(g, FALSE);
}

static void dl_finish(DlCtx *c, gboolean ok, const char *msg) {
    c->cb(ok, msg, c->data);
    dl_ctx_free(c);
}

static void on_downloader_exit(GObject *src, GAsyncResult *res,
                               gpointer data) {
    DlCtx *c = data;
    if (fetch_have_aria2()) {
        gboolean ok =
            g_subprocess_wait_finish(G_SUBPROCESS(src), res, NULL);
        dl_finish(c, ok, ok ? "Video downloaded to folder" : "aria2c failed");
    } else {
        // xdg-open exits right after dispatching; the client owns it now
        dl_finish(c, TRUE, "Opened in torrent client");
    }
}

static void spawn_downloader(DlCtx *c) {
    const char *source = c->res->magnet ? c->res->magnet : c->torrent_file;
    if (!source) {
        dl_finish(c, FALSE, "No magnet or torrent");
        return;
    }
    GSubprocess *proc = NULL;
    if (fetch_have_aria2()) {
        char *dir = g_strdup_printf("--dir=%s", c->dest);
        proc = g_subprocess_new(
            G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                G_SUBPROCESS_FLAGS_STDERR_SILENCE,
            NULL, "aria2c", dir, "--seed-time=0", "--bt-enable-lpd=true",
            source, NULL);
        g_free(dir);
    } else {
        proc = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                                    G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                                NULL, "xdg-open", source, NULL);
    }
    if (!proc) {
        dl_finish(c, FALSE, "Could not launch downloader");
        return;
    }
    g_subprocess_wait_async(proc, NULL, on_downloader_exit, c);
    g_object_unref(proc);
}

static void on_torrent_saved(GObject *src, GAsyncResult *res,
                             gpointer data) {
    DlCtx *c = data;
    gboolean ok = g_subprocess_wait_finish(G_SUBPROCESS(src), res, NULL);
    if (!ok) {
        // keep going when a magnet is available; otherwise this is fatal
        g_clear_pointer(&c->torrent_file, g_free);
        if (!c->res->magnet) {
            dl_finish(c, FALSE, "Torrent download failed");
            return;
        }
    }
    spawn_downloader(c);
}

void fetch_download(const FetchResult *res, const char *dest_dir,
                    FetchDlCallback cb, gpointer user_data) {
    DlCtx *c = g_new0(DlCtx, 1);
    c->cb = cb;
    c->data = user_data;
    c->res = result_copy(res);
    c->dest = g_strdup(dest_dir);
    if (res->torrent_url && res->title) {
        char *fn = sanitize_filename(res->title);
        c->torrent_file = g_build_filename(dest_dir, fn, NULL);
        g_free(fn);
        GSubprocess *proc = g_subprocess_new(
            G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                G_SUBPROCESS_FLAGS_STDERR_SILENCE,
            NULL, "curl", "-sL", "-m", "60", "--globoff", "-A", UA, "-o",
            c->torrent_file, res->torrent_url, NULL);
        if (proc) {
            g_subprocess_wait_async(proc, NULL, on_torrent_saved, c);
            g_object_unref(proc);
            return;
        }
        g_clear_pointer(&c->torrent_file, g_free);
    }
    spawn_downloader(c);
}

// ------------------------------------------------------------------ dialog --

FetchGroup *fetch_group_new(const char *search, const char *label,
                            const char *dir) {
    FetchGroup *g = g_new0(FetchGroup, 1);
    g->search = g_strdup(search);
    g->label = g_strdup(label);
    g->dir = g_strdup(dir);
    return g;
}

void fetch_group_free(gpointer p) {
    FetchGroup *g = p;
    g_free(g->search);
    g_free(g->label);
    g_free(g->dir);
    g_free(g);
}

typedef struct FetchDlg FetchDlg;

typedef struct {
    FetchDlg *dlg;
    FetchGroup *group; // borrowed from dlg->groups
    int gidx;
    int episode;
    GtkWidget *row;     // borrowed: AdwActionRow in the list
    GtkWidget *spinner; // borrowed
    GtkWidget *dl_btn;  // borrowed
    GtkWidget *copy_btn; // borrowed
    FetchResult *result; // owned
    gboolean busy;      // searching or downloading
    gboolean downloaded;
} MissEp;

struct FetchDlg {
    char *series;
    GPtrArray *groups; // FetchGroup*
    FetchProvider provider;
    char *quality;
    AdwDialog *dlg; // borrowed while open
    GtkWidget *list;
    GtkWidget *hint;
    GtkWidget *summary;
    GtkWidget *find_btn;
    GtkWidget *dl_all_btn;
    GtkWidget *prov_drop;
    GtkWidget *qual_drop;
    GPtrArray *missing; // MissEp*
    int search_idx;
    gboolean searching;
    guint search_gen; // bumped when provider/quality changes
    int tails_pending;
    gboolean closed;
    int pending; // in-flight async ops; struct is freed when 0 + closed
    void (*done)(gpointer);
    gpointer done_data;
};

static void miss_free(gpointer p) {
    MissEp *m = p;
    if (m->result)
        fetch_result_free(m->result);
    g_free(m);
}

static void dlg_op_start(FetchDlg *dlg) {
    dlg->pending++;
}

static void dlg_op_done(FetchDlg *dlg) {
    if (--dlg->pending == 0 && dlg->closed) {
        g_free(dlg->series);
        g_ptr_array_unref(dlg->groups);
        g_free(dlg->quality);
        g_ptr_array_unref(dlg->missing);
        g_free(dlg);
    }
}

static const char *provider_name(FetchProvider p) {
    return p == FETCH_ERAI ? "Erai-raws" : "SubsPlease";
}

static void update_summary(FetchDlg *dlg) {
    if (dlg->closed)
        return;
    int found = 0, done = 0;
    for (guint i = 0; i < dlg->missing->len; i++) {
        MissEp *m = dlg->missing->pdata[i];
        if (m->result)
            found++;
        if (m->downloaded)
            done++;
    }
    char *txt;
    if (dlg->missing->len == 0) {
        txt = g_strdup("Nothing missing — the collection is complete.");
    } else if (dlg->tails_pending > 0) {
        txt = g_strdup_printf("%u missing · resolving aired counts…",
                              dlg->missing->len);
    } else if (!dlg->searching && found == 0 && dlg->search_idx == 0) {
        txt = g_strdup_printf(
            "%u missing · pick a provider and press Find",
            dlg->missing->len);
    } else {
        txt = g_strdup_printf("%u missing · %d found · %d downloaded",
                              dlg->missing->len, found, done);
    }
    gtk_label_set_text(GTK_LABEL(dlg->summary), txt);
    g_free(txt);

    gboolean any_ready = FALSE;
    for (guint i = 0; i < dlg->missing->len; i++) {
        MissEp *m = dlg->missing->pdata[i];
        if (m->result && !m->downloaded && !m->busy) {
            any_ready = TRUE;
            break;
        }
    }
    gtk_widget_set_sensitive(dlg->dl_all_btn, any_ready);
}

static void miss_set_status(MissEp *m, const char *txt, gboolean spinning) {
    if (m->dlg->closed)
        return;
    char *full;
    if (m->dlg->groups->len > 1)
        full = g_strdup_printf("%s · %s", m->group->label, txt);
    else
        full = g_strdup(txt);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(m->row), full);
    g_free(full);
    gtk_widget_set_visible(m->spinner, spinning);
    if (spinning)
        gtk_spinner_start(GTK_SPINNER(m->spinner));
    else
        gtk_spinner_stop(GTK_SPINNER(m->spinner));
}

// ------------------------------------------------------------- find chain --

typedef struct {
    FetchDlg *dlg;
    MissEp *miss;
    guint gen;
    int attempt; // 0 = group term, 1 = bare series name
    char *provider;
    char *quality;
} SearchCtx;

static void search_ctx_free(SearchCtx *c) {
    g_free(c->provider);
    g_free(c->quality);
    g_free(c);
}

static void fetch_dialog_search_next(FetchDlg *dlg);

static void on_found(GPtrArray *results, gpointer data) {
    SearchCtx *c = data;
    FetchDlg *dlg = c->dlg;
    MissEp *m = c->miss;
    gboolean stale =
        dlg->closed || c->gen != dlg->search_gen || m->result != NULL;

    if (!stale && results->len > 0) {
        m->result = result_copy(results->pdata[0]);
        m->busy = FALSE;
        if (!dlg->closed) {
            char *sub = m->result->size
                            ? g_strdup_printf("%s · %s", m->result->title,
                                              m->result->size)
                            : g_strdup(m->result->title);
            miss_set_status(m, sub, FALSE);
            g_free(sub);
            gtk_widget_set_visible(m->dl_btn, TRUE);
            gtk_widget_set_sensitive(m->dl_btn, TRUE);
            if (m->result->magnet || m->result->torrent_url)
                gtk_widget_set_visible(m->copy_btn, TRUE);
        }
    } else if (!stale && c->attempt == 0 &&
               g_strcmp0(m->group->search, dlg->series) != 0) {
        // season-qualified term found nothing ("Frieren Season 2" vs the
        // release's "Frieren S2") — retry once with the bare series name
        c->attempt = 1;
        g_free(c->provider); // kept in sync below; values can't change
        g_free(c->quality);  // mid-search (dropdowns are insensitive)
        c->provider = g_strdup(provider_name(dlg->provider));
        c->quality = g_strdup(dlg->quality);
        if (!dlg->closed) {
            char *txt = g_strdup_printf("Searching %s…", c->provider);
            miss_set_status(m, txt, TRUE);
            g_free(txt);
        }
        fetch_search(dlg->provider, dlg->series, m->episode, dlg->quality,
                     on_found, c);
        return; // op continues
    } else if (!stale) {
        m->busy = FALSE;
        miss_set_status(m, "Not found", FALSE);
    }

    gboolean advance = !stale;
    search_ctx_free(c);
    if (!dlg->closed && advance) {
        dlg->search_idx++;
        fetch_dialog_search_next(dlg);
    }
    dlg_op_done(dlg);
}

static void miss_search(MissEp *m, int attempt) {
    FetchDlg *dlg = m->dlg;
    const char *term =
        attempt == 0 ? m->group->search : dlg->series;
    SearchCtx *c = g_new0(SearchCtx, 1);
    c->dlg = dlg;
    c->miss = m;
    c->gen = dlg->search_gen;
    c->attempt = attempt;
    c->provider = g_strdup(provider_name(dlg->provider));
    c->quality = g_strdup(dlg->quality);
    m->busy = TRUE;
    if (!dlg->closed) {
        char *txt = g_strdup_printf("Searching %s…", c->provider);
        miss_set_status(m, txt, TRUE);
        g_free(txt);
        gtk_widget_set_visible(m->dl_btn, FALSE);
        gtk_widget_set_visible(m->copy_btn, FALSE);
    }
    dlg_op_start(dlg);
    fetch_search(dlg->provider, term, m->episode, dlg->quality, on_found,
                 c);
}

static void fetch_dialog_search_next(FetchDlg *dlg) {
    if (dlg->closed)
        return;
    while (dlg->search_idx < (int)dlg->missing->len) {
        MissEp *m = dlg->missing->pdata[dlg->search_idx];
        if (!m->result && !m->busy && !m->downloaded)
            break;
        dlg->search_idx++;
    }
    if (dlg->search_idx >= (int)dlg->missing->len) {
        dlg->searching = FALSE;
        gtk_widget_set_sensitive(dlg->find_btn, TRUE);
        gtk_widget_set_sensitive(dlg->prov_drop, TRUE);
        gtk_widget_set_sensitive(dlg->qual_drop, TRUE);
        update_summary(dlg);
        return;
    }
    miss_search(dlg->missing->pdata[dlg->search_idx], 0);
}

static void on_find_clicked(GtkButton *btn, gpointer data) {
    (void)btn;
    FetchDlg *dlg = data;
    if (dlg->searching || dlg->missing->len == 0)
        return;
    dlg->searching = TRUE;
    dlg->search_idx = 0;
    gtk_widget_set_sensitive(dlg->find_btn, FALSE);
    gtk_widget_set_sensitive(dlg->prov_drop, FALSE);
    gtk_widget_set_sensitive(dlg->qual_drop, FALSE);
    update_summary(dlg);
    fetch_dialog_search_next(dlg);
}

// -------------------------------------------------------------- downloads --

static void on_dl_done(gboolean ok, const char *message, gpointer data) {
    MissEp *m = data;
    FetchDlg *dlg = m->dlg;
    m->busy = FALSE;
    if (ok)
        m->downloaded = TRUE;
    if (!dlg->closed) {
        if (ok) {
            miss_set_status(m, message, FALSE);
            gtk_widget_set_visible(m->dl_btn, FALSE);
        } else {
            char *txt = g_strdup_printf("Failed: %s", message);
            miss_set_status(m, txt, FALSE);
            g_free(txt);
            gtk_widget_set_sensitive(m->dl_btn, TRUE);
        }
        update_summary(dlg);
    }
    dlg_op_done(dlg);
}

static void start_download(MissEp *m) {
    FetchDlg *dlg = m->dlg;
    if (!m->result || m->busy || m->downloaded)
        return;
    m->busy = TRUE;
    if (!dlg->closed) {
        miss_set_status(m, "Downloading…", TRUE);
        gtk_widget_set_sensitive(m->dl_btn, FALSE);
        update_summary(dlg);
    }
    dlg_op_start(dlg);
    fetch_download(m->result, m->group->dir, on_dl_done, m);
}

static void on_dl_clicked(GtkButton *btn, gpointer data) {
    (void)btn;
    start_download(data);
}

static void on_dl_all_clicked(GtkButton *btn, gpointer data) {
    (void)btn;
    FetchDlg *dlg = data;
    for (guint i = 0; i < dlg->missing->len; i++)
        start_download(dlg->missing->pdata[i]);
}

static void on_copy_clicked(GtkButton *btn, gpointer data) {
    (void)btn;
    MissEp *m = data;
    if (!m->result)
        return;
    const char *txt = m->result->magnet ? m->result->magnet
                                        : m->result->torrent_url;
    if (txt)
        gdk_clipboard_set_text(gtk_widget_get_clipboard(m->row), txt);
}

// ------------------------------------------------------- provider switching --

static void results_reset(FetchDlg *dlg) {
    dlg->search_gen++;
    dlg->searching = FALSE;
    dlg->search_idx = 0;
    for (guint i = 0; i < dlg->missing->len; i++) {
        MissEp *m = dlg->missing->pdata[i];
        if (m->busy)
            continue; // in-flight rows keep spinning; their cb is stale
        g_clear_pointer(&m->result, fetch_result_free);
        m->downloaded = FALSE;
        if (!dlg->closed) {
            miss_set_status(m, "Not searched yet", FALSE);
            gtk_widget_set_visible(m->dl_btn, FALSE);
            gtk_widget_set_visible(m->copy_btn, FALSE);
        }
    }
    if (!dlg->closed) {
        gtk_widget_set_sensitive(dlg->find_btn, dlg->missing->len > 0);
        update_summary(dlg);
    }
}

static void on_provider_changed(GObject *obj, GParamSpec *pspec,
                                gpointer data) {
    (void)pspec;
    FetchDlg *dlg = data;
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(obj));
    dlg->provider = sel == 1 ? FETCH_ERAI : FETCH_SUBSPLEASE;
    prefs_save(dlg->provider, dlg->quality);
    results_reset(dlg);
}

static void on_quality_changed(GObject *obj, GParamSpec *pspec,
                               gpointer data) {
    (void)pspec;
    FetchDlg *dlg = data;
    static const char *quals[] = {"1080", "720", "480"};
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(obj));
    if (sel > 2)
        sel = 0;
    g_free(dlg->quality);
    dlg->quality = g_strdup(quals[sel]);
    prefs_save(dlg->provider, dlg->quality);
    results_reset(dlg);
}

// ------------------------------------------------------------- missing list --

static int miss_sort(GtkListBoxRow *a, GtkListBoxRow *b, gpointer data) {
    (void)data;
    MissEp *ma = g_object_get_data(G_OBJECT(a), "miss");
    MissEp *mb = g_object_get_data(G_OBJECT(b), "miss");
    if (ma->gidx != mb->gidx)
        return ma->gidx - mb->gidx;
    return ma->episode - mb->episode;
}

static MissEp *miss_add_row(FetchDlg *dlg, FetchGroup *g, int gidx, int ep) {
    MissEp *m = g_new0(MissEp, 1);
    m->dlg = dlg;
    m->group = g;
    m->gidx = gidx;
    m->episode = ep;

    GtkWidget *row = adw_action_row_new();
    char *title = (dlg->groups->len > 1)
                      ? g_strdup_printf("Episode %d · %s", ep, g->label)
                      : g_strdup_printf("Episode %d", ep);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    g_free(title);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), "Not searched yet");

    m->spinner = gtk_spinner_new();
    gtk_widget_set_valign(m->spinner, GTK_ALIGN_CENTER);
    gtk_widget_set_visible(m->spinner, FALSE);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), m->spinner);

    m->copy_btn = gtk_button_new_from_icon_name("edit-copy-symbolic");
    gtk_widget_add_css_class(m->copy_btn, "flat");
    gtk_widget_set_valign(m->copy_btn, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(m->copy_btn, "Copy magnet link");
    gtk_widget_set_visible(m->copy_btn, FALSE);
    g_signal_connect(m->copy_btn, "clicked", G_CALLBACK(on_copy_clicked),
                     m);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), m->copy_btn);

    m->dl_btn = gtk_button_new_with_label("Download");
    gtk_widget_add_css_class(m->dl_btn, "suggested-action");
    gtk_widget_set_valign(m->dl_btn, GTK_ALIGN_CENTER);
    gtk_widget_set_visible(m->dl_btn, FALSE);
    g_signal_connect(m->dl_btn, "clicked", G_CALLBACK(on_dl_clicked), m);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), m->dl_btn);

    m->row = row;
    g_object_set_data(G_OBJECT(row), "miss", m);
    g_ptr_array_add(dlg->missing, m);
    if (!dlg->closed) {
        gtk_list_box_append(GTK_LIST_BOX(dlg->list), row);
        gtk_list_box_invalidate_sort(GTK_LIST_BOX(dlg->list));
    }
    return m;
}

typedef struct {
    FetchDlg *dlg;
    int gidx;
    int local_max;
} TailCtx;

// Episodes aired past the local max (AniList tail) become fetchable rows.
static void on_tail_info(const AniInfo *info, gpointer data) {
    TailCtx *t = data;
    FetchDlg *dlg = t->dlg;
    if (!dlg->closed && info && info->ok) {
        int expected =
            (info->airing && info->aired > 0) ? info->aired : info->episodes;
        if (expected > t->local_max) {
            FetchGroup *g = dlg->groups->pdata[t->gidx];
            int cap = MIN(expected, t->local_max + MAX_TAIL_PER_GROUP);
            for (int ep = t->local_max + 1; ep <= cap; ep++)
                miss_add_row(dlg, g, t->gidx, ep);
            if (expected > cap && !dlg->closed) {
                char *txt = g_strdup_printf(
                    "%s: showing first %d of %d aired-but-missing",
                    g->label, cap - t->local_max,
                    expected - t->local_max);
                GtkWidget *note = gtk_label_new(txt);
                g_free(txt);
                gtk_widget_add_css_class(note, "dim-label");
                gtk_label_set_xalign(GTK_LABEL(note), 0.0);
                gtk_list_box_append(GTK_LIST_BOX(dlg->list), note);
            }
        }
    }
    if (--dlg->tails_pending == 0 && !dlg->closed) {
        // Tails may have landed behind an already-finished Find pass;
        // restart the chain at the top (completed rows are skipped).
        if (dlg->searching) {
            dlg->search_idx = 0;
            fetch_dialog_search_next(dlg);
        }
        update_summary(dlg);
    }
    g_free(t);
    dlg_op_done(dlg);
}

// Gaps inside the local runs are known synchronously; tails resolve async.
static void build_missing(FetchDlg *dlg) {
    for (guint gi = 0; gi < dlg->groups->len; gi++) {
        FetchGroup *g = dlg->groups->pdata[gi];
        GArray *nums = g_array_new(FALSE, FALSE, sizeof(int));
        GDir *dir = g_dir_open(g->dir, 0, NULL);
        if (dir) {
            const char *name;
            while ((name = g_dir_read_name(dir))) {
                if (!is_video(name))
                    continue;
                int ep = episode_number(name);
                if (ep >= 0)
                    g_array_append_val(nums, ep);
            }
            g_dir_close(dir);
        }
        int local_max = -1;
        for (guint i = 0; i < nums->len; i++)
            local_max = MAX(local_max, g_array_index(nums, int, i));
        GArray *gaps = find_gaps(nums);
        for (guint i = 0; i < gaps->len; i++)
            miss_add_row(dlg, g, (int)gi, g_array_index(gaps, int, i));
        g_array_unref(gaps);
        g_array_unref(nums);

        TailCtx *t = g_new0(TailCtx, 1);
        t->dlg = dlg;
        t->gidx = (int)gi;
        t->local_max = local_max;
        dlg->tails_pending++;
        dlg_op_start(dlg);
        anilist_lookup(g->search, on_tail_info, t);
    }
}

// ------------------------------------------------------------------ dialog --

static void on_dialog_closed(AdwDialog *dialog, gpointer data) {
    (void)dialog;
    FetchDlg *dlg = data;
    dlg->closed = TRUE;
    if (dlg->done)
        dlg->done(dlg->done_data);
    dlg_op_done(dlg); // frees when no async op is in flight
}

AdwDialog *fetch_dialog_show(GtkWindow *parent, const char *series,
                             GPtrArray *groups, void (*done)(gpointer),
                             gpointer done_data) {
    FetchDlg *dlg = g_new0(FetchDlg, 1);
    dlg->series = g_strdup(series);
    dlg->groups = g_ptr_array_new_with_free_func(fetch_group_free);
    for (guint i = 0; i < groups->len; i++) {
        FetchGroup *g = groups->pdata[i];
        g_ptr_array_add(dlg->groups,
                        fetch_group_new(g->search, g->label, g->dir));
    }
    prefs_load(&dlg->provider, &dlg->quality);
    dlg->missing = g_ptr_array_new_with_free_func(miss_free);
    dlg->done = done;
    dlg->done_data = done_data;
    dlg_op_start(dlg); // the open dialog itself

    AdwDialog *ddlg = adw_dialog_new();
    dlg->dlg = ddlg;
    char *title = g_strdup_printf("Fetch missing — %s", series);
    adw_dialog_set_title(ddlg, title);
    g_free(title);
    adw_dialog_set_content_width(ddlg, 640);
    adw_dialog_set_content_height(ddlg, 560);
    g_signal_connect(ddlg, "closed", G_CALLBACK(on_dialog_closed), dlg);

    GtkWidget *view = adw_toolbar_view_new();
    GtkWidget *header = adw_header_bar_new();
    adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(header),
                                              TRUE);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);

    GtkWidget *content =
        gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(content, 16);
    gtk_widget_set_margin_bottom(content, 16);
    gtk_widget_set_margin_start(content, 16);
    gtk_widget_set_margin_end(content, 16);

    GtkWidget *controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    dlg->prov_drop = gtk_drop_down_new_from_strings(
        (const char *[]){"SubsPlease", "Erai-raws", NULL});
    gtk_drop_down_set_selected(GTK_DROP_DOWN(dlg->prov_drop),
                               dlg->provider == FETCH_ERAI ? 1 : 0);
    gtk_widget_set_tooltip_text(dlg->prov_drop, "Release group");
    gtk_box_append(GTK_BOX(controls), dlg->prov_drop);
    g_signal_connect(dlg->prov_drop, "notify::selected",
                     G_CALLBACK(on_provider_changed), dlg);

    const char *qs[] = {"1080p", "720p", "480p", NULL};
    dlg->qual_drop = gtk_drop_down_new_from_strings(qs);
    guint qsel = 0;
    if (strcmp(dlg->quality, "720") == 0)
        qsel = 1;
    else if (strcmp(dlg->quality, "480") == 0)
        qsel = 2;
    gtk_drop_down_set_selected(GTK_DROP_DOWN(dlg->qual_drop), qsel);
    gtk_widget_set_tooltip_text(dlg->qual_drop, "Resolution");
    gtk_box_append(GTK_BOX(controls), dlg->qual_drop);
    g_signal_connect(dlg->qual_drop, "notify::selected",
                     G_CALLBACK(on_quality_changed), dlg);

    dlg->find_btn = gtk_button_new_with_label("Find");
    gtk_widget_add_css_class(dlg->find_btn, "suggested-action");
    gtk_widget_set_tooltip_text(dlg->find_btn,
                                "Search the provider for every missing "
                                "episode");
    g_signal_connect(dlg->find_btn, "clicked",
                     G_CALLBACK(on_find_clicked), dlg);
    gtk_box_append(GTK_BOX(controls), dlg->find_btn);

    dlg->dl_all_btn = gtk_button_new_with_label("Download all");
    gtk_widget_set_tooltip_text(dlg->dl_all_btn,
                                "Download every found episode");
    gtk_widget_set_sensitive(dlg->dl_all_btn, FALSE);
    g_signal_connect(dlg->dl_all_btn, "clicked",
                     G_CALLBACK(on_dl_all_clicked), dlg);
    gtk_box_append(GTK_BOX(controls), dlg->dl_all_btn);
    gtk_box_append(GTK_BOX(content), controls);

    dlg->hint = gtk_label_new(
        fetch_have_aria2()
            ? "Videos download straight into the episode folder (aria2c)."
            : "aria2c is not installed: releases open in your torrent "
              "client instead. Install aria2c for fully automatic "
              "downloads into the episode folder.");
    gtk_label_set_wrap(GTK_LABEL(dlg->hint), TRUE);
    gtk_label_set_xalign(GTK_LABEL(dlg->hint), 0.0);
    gtk_widget_add_css_class(dlg->hint, "dim-label");
    gtk_widget_add_css_class(dlg->hint, "caption-label");
    gtk_box_append(GTK_BOX(content), dlg->hint);

    dlg->summary = gtk_label_new("Scanning local episodes…");
    gtk_label_set_xalign(GTK_LABEL(dlg->summary), 0.0);
    gtk_widget_add_css_class(dlg->summary, "heading");
    gtk_box_append(GTK_BOX(content), dlg->summary);

    dlg->list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(dlg->list),
                                    GTK_SELECTION_NONE);
    gtk_widget_add_css_class(dlg->list, "boxed-list");
    gtk_list_box_set_sort_func(GTK_LIST_BOX(dlg->list), miss_sort, NULL,
                               NULL);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), dlg->list);
    gtk_box_append(GTK_BOX(content), scroll);

    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), content);
    adw_dialog_set_child(ddlg, view);
    adw_dialog_present(ddlg, GTK_WIDGET(parent));

    build_missing(dlg);
    update_summary(dlg);
    return ddlg;
}
