// Nekoland Animanager — manage anime stored across configured folders.
// GTK4 + libadwaita, same conventions as settings/: GKeyFile config under
// ~/.config/nekoland/, style.css loaded from beside the binary.

#define _GNU_SOURCE // strverscmp
#include <string.h>
#include <sys/stat.h>

#include <glib/gstdio.h>

#include <adwaita.h>
#include <gtk/gtk.h>

#include "anilist.h"

static GtkWindow *main_window;
static GtkWidget *nav_view;        // library page + pushed series pages
static GtkWidget *library_stack;   // "empty" page / "library" page
static GtkWidget *library_box;     // vertical box holding season sections
static GtkWidget *errors_box;      // folder-error notes above the sections
static guint library_gen;          // bumped on refresh; stale async work
                                   // checks it before touching the UI

// settings dialog state (NULL while the dialog is closed)
static AdwPreferencesGroup *folders_group;
static GPtrArray *folder_rows; // AdwActionRow* currently in folders_group

static GPtrArray *folders; // char* — configured anime directories

static void library_refresh(void);

// ---------------------------------------------------------------- config --

static char *config_path(void) {
    return g_build_filename(g_get_user_config_dir(), "nekoland",
                            "animanager.ini", NULL);
}

static void config_load(void) {
    folders = g_ptr_array_new_with_free_func(g_free);
    char *path = config_path();
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        gsize n = 0;
        char **list =
            g_key_file_get_string_list(kf, "library", "folders", &n, NULL);
        for (gsize i = 0; list && i < n; i++)
            g_ptr_array_add(folders, g_strdup(list[i]));
        g_strfreev(list);
    }
    g_key_file_free(kf);
    g_free(path);
}

static void config_save(void) {
    char *path = config_path();
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);

    GKeyFile *kf = g_key_file_new();
    g_key_file_set_string_list(kf, "library", "folders",
                               (const char *const *)folders->pdata,
                               folders->len);
    g_key_file_save_to_file(kf, path, NULL);
    g_key_file_free(kf);
    g_free(path);
}

// --------------------------------------------------------------- library --

#define POSTER_W 185
#define POSTER_H 264 // ~2:3 like the jellyfin-style folder.jpg covers

// Per-card background load: cover art + episode/season counts, off the main
// thread because the library usually lives on a network mount.
typedef struct {
    char *path;          // anime directory
    GtkWidget *card;     // ref'd (sunk); placed into a season section
    GtkWidget *picture;  // ref'd
    GtkWidget *subtitle; // ref'd
    GtkWidget *badge;    // ref'd; missing-episode warning chip
    GdkPixbuf *pixbuf;   // result: scaled cover, or NULL
    gint64 min_mtime;    // oldest episode file: season fallback
    int episodes;
    int seasons;      // video-bearing subdirectories (Season 1, ...)
    int missing_eps;  // holes in the episode runs
    gboolean missing_season; // hole in the season numbering
    char *missing_tip; // human-readable detail for the badge tooltip
    GPtrArray *groups; // EpGroup* — one per episode-bearing directory
} CardLoad;

// one episode group (top level or a Season dir) for AniList comparison
typedef struct {
    char *search; // AniList search term
    char *label;  // "Episodes" / "Season 2"
    int count;    // local videos
} EpGroup;

static void ep_group_free(gpointer p) {
    EpGroup *g = p;
    g_free(g->search);
    g_free(g->label);
    g_free(g);
}

static void card_load_free(gpointer data) {
    CardLoad *cl = data;
    g_free(cl->path);
    g_object_unref(cl->card);
    g_object_unref(cl->picture);
    g_object_unref(cl->subtitle);
    g_object_unref(cl->badge);
    g_clear_object(&cl->pixbuf);
    g_free(cl->missing_tip);
    g_clear_pointer(&cl->groups, g_ptr_array_unref);
    g_free(cl);
}

// compiled once before any card threads start (GRegex matching is
// thread-safe, creation is not)
static GRegex *re_ep_dash;   // " - 01", " - 01v2", " - 01.5"
static GRegex *re_ep_word;   // "E01", "Ep 01", "Episode 01"
static GRegex *re_ep_bare;   // fallback: standalone 1-4 digit number
static GRegex *re_season;    // "Season 2", "S2"

static void regexes_init(void) {
    re_ep_dash = g_regex_new("\\s-\\s*([0-9]{1,4})(?:v[0-9]+|\\.[0-9]+)?\\b",
                             0, 0, NULL);
    re_ep_word = g_regex_new("\\b(?:e|ep|episode)\\.?\\s*([0-9]{1,4})\\b",
                             G_REGEX_CASELESS, 0, NULL);
    re_ep_bare = g_regex_new("\\b([0-9]{1,4})\\b", 0, 0, NULL);
    re_season = g_regex_new("\\b(?:season|s)\\s*([0-9]{1,3})\\b",
                            G_REGEX_CASELESS, 0, NULL);
}

// strip extension, "[...]" release tags and "(...)" quality tags so bare
// numbers in them (crc32, 1080p) don't read as episode numbers
static char *episode_stem(const char *fname) {
    GString *s = g_string_new(NULL);
    int depth = 0;
    for (const char *p = fname; *p; p++) {
        if (*p == '[' || *p == '(')
            depth++;
        else if (*p == ']' || *p == ')') {
            if (depth > 0)
                depth--;
        } else if (depth == 0)
            g_string_append_c(s, *p);
    }
    char *dot = strrchr(s->str, '.');
    if (dot)
        g_string_truncate(s, dot - s->str);
    return g_string_free(s, FALSE);
}

static int match_int(GRegex *re, const char *str) {
    GMatchInfo *mi = NULL;
    int out = -1;
    if (g_regex_match(re, str, 0, &mi)) {
        char *num = g_match_info_fetch(mi, 1);
        out = atoi(num);
        g_free(num);
    }
    g_match_info_free(mi);
    return out;
}

static int episode_number(const char *fname) {
    char *stem = episode_stem(fname);
    int n = match_int(re_ep_dash, stem);
    if (n < 0)
        n = match_int(re_ep_word, stem);
    if (n < 0)
        n = match_int(re_ep_bare, stem);
    g_free(stem);
    return n;
}

static int season_number(const char *dirname) {
    return match_int(re_season, dirname);
}

// numbers absent between the smallest and largest present — holes in the
// run, so a collection starting at a later cour isn't flagged
static int int_cmp(gconstpointer a, gconstpointer b) {
    return *(const int *)a - *(const int *)b;
}

static GArray *find_gaps(GArray *present) {
    GArray *gaps = g_array_new(FALSE, FALSE, sizeof(int));
    if (present->len < 2)
        return gaps;
    g_array_sort(present, int_cmp);
    int lo = g_array_index(present, int, 0);
    int hi = g_array_index(present, int, present->len - 1);
    guint idx = 0;
    for (int n = lo; n <= hi; n++) {
        while (idx < present->len && g_array_index(present, int, idx) < n)
            idx++;
        if (idx >= present->len || g_array_index(present, int, idx) != n)
            g_array_append_val(gaps, n);
    }
    return gaps;
}

// "6, 8–10"
static char *format_ranges(GArray *nums) {
    GString *s = g_string_new(NULL);
    for (guint i = 0; i < nums->len;) {
        guint j = i;
        while (j + 1 < nums->len &&
               g_array_index(nums, int, j + 1) ==
                   g_array_index(nums, int, j) + 1)
            j++;
        if (s->len)
            g_string_append(s, ", ");
        if (j > i)
            g_string_append_printf(s, "%d–%d", g_array_index(nums, int, i),
                                   g_array_index(nums, int, j));
        else
            g_string_append_printf(s, "%d", g_array_index(nums, int, i));
        i = j + 1;
    }
    return g_string_free(s, FALSE);
}

static gboolean is_video(const char *name) {
    static const char *exts[] = {".mkv", ".mp4",  ".avi", ".webm",
                                 ".mov", ".m2ts", ".ts",  NULL};
    for (int i = 0; exts[i]; i++)
        if (g_str_has_suffix(name, exts[i]))
            return TRUE;
    return FALSE;
}

// count the videos directly in path, collecting their parsed episode
// numbers into nums and the oldest video mtime into min_mtime (when given)
static int count_videos(const char *path, GArray *nums, gint64 *min_mtime) {
    int n = 0;
    GDir *dir = g_dir_open(path, 0, NULL);
    if (!dir)
        return 0;
    const char *name;
    while ((name = g_dir_read_name(dir))) {
        if (!is_video(name))
            continue;
        n++;
        if (nums) {
            int ep = episode_number(name);
            if (ep >= 0)
                g_array_append_val(nums, ep);
        }
        if (min_mtime) {
            char *full = g_build_filename(path, name, NULL);
            GStatBuf st;
            if (g_stat(full, &st) == 0 &&
                (*min_mtime == 0 || st.st_mtime < *min_mtime))
                *min_mtime = st.st_mtime;
            g_free(full);
        }
    }
    g_dir_close(dir);
    return n;
}

// gap-check one group of episode numbers; appends "Label missing: 6, 8-9"
// to tip and returns how many are missing
static int report_gaps(GArray *nums, const char *label, GString *tip) {
    GArray *gaps = find_gaps(nums);
    int n = gaps->len;
    if (n > 0) {
        char *ranges = format_ranges(gaps);
        if (tip->len)
            g_string_append_c(tip, '\n');
        g_string_append_printf(tip, "%s missing: %s", label, ranges);
        g_free(ranges);
    }
    g_array_unref(gaps);
    return n;
}

static void card_load_thread(GTask *task, gpointer src, gpointer data,
                             GCancellable *cancel) {
    (void)src;
    (void)cancel;
    CardLoad *cl = data;

    static const char *covers[] = {"folder.jpg", "folder.png", "cover.jpg",
                                   "cover.png",  "poster.jpg", NULL};
    for (int i = 0; covers[i] && !cl->pixbuf; i++) {
        char *p = g_build_filename(cl->path, covers[i], NULL);
        if (g_file_test(p, G_FILE_TEST_EXISTS))
            cl->pixbuf = gdk_pixbuf_new_from_file_at_scale(p, POSTER_W * 2,
                                                           -1, TRUE, NULL);
        g_free(p);
    }

    GString *tip = g_string_new(NULL);
    GArray *top_nums = g_array_new(FALSE, FALSE, sizeof(int));
    GArray *season_nums = g_array_new(FALSE, FALSE, sizeof(int));
    char *series = g_path_get_basename(cl->path);
    cl->groups = g_ptr_array_new_with_free_func(ep_group_free);

    cl->episodes = count_videos(cl->path, top_nums, &cl->min_mtime);
    cl->missing_eps += report_gaps(top_nums, "Episodes", tip);
    if (cl->episodes > 0) {
        EpGroup *g = g_new0(EpGroup, 1);
        g->search = g_strdup(series);
        g->label = g_strdup("Episodes");
        g->count = cl->episodes;
        g_ptr_array_add(cl->groups, g);
    }
    g_array_unref(top_nums);

    GDir *dir = g_dir_open(cl->path, 0, NULL);
    if (dir) {
        const char *name;
        while ((name = g_dir_read_name(dir))) {
            if (name[0] == '.')
                continue;
            char *sub = g_build_filename(cl->path, name, NULL);
            if (g_file_test(sub, G_FILE_TEST_IS_DIR)) {
                GArray *nums = g_array_new(FALSE, FALSE, sizeof(int));
                int n = count_videos(sub, nums, &cl->min_mtime);
                if (n > 0) {
                    cl->seasons++;
                    cl->episodes += n;
                    cl->missing_eps += report_gaps(nums, name, tip);
                    int sn = season_number(name);
                    if (sn >= 0)
                        g_array_append_val(season_nums, sn);
                    EpGroup *g = g_new0(EpGroup, 1);
                    g->search = g_strdup_printf("%s %s", series, name);
                    g->label = g_strdup(name);
                    g->count = n;
                    g_ptr_array_add(cl->groups, g);
                }
                g_array_unref(nums);
            }
            g_free(sub);
        }
        g_dir_close(dir);
    }
    if (report_gaps(season_nums, "Seasons", tip) > 0)
        cl->missing_season = TRUE;
    g_array_unref(season_nums);
    g_free(series);

    if (tip->len)
        cl->missing_tip = g_string_free(tip, FALSE);
    else
        g_string_free(tip, TRUE);
    g_task_return_boolean(task, TRUE);
}

// ------------------------------------------------------- season sections --

static const char *SEASON_NAMES[] = {"Winter", "Spring", "Summer", "Fall"};

typedef struct {
    GtkWidget *box;  // heading + grid, child of library_box
    GtkWidget *grid; // flowbox of cards
    int score;       // year*4 + season index; -1 = unknown; sorted desc
} SeasonSection;

static GPtrArray *sections; // SeasonSection*, kept sorted by score desc

static int flow_name_cmp(GtkFlowBoxChild *a, GtkFlowBoxChild *b,
                         gpointer data) {
    (void)data;
    const char *na = g_object_get_data(
        G_OBJECT(gtk_flow_box_child_get_child(a)), "series-name");
    const char *nb = g_object_get_data(
        G_OBJECT(gtk_flow_box_child_get_child(b)), "series-name");
    return g_utf8_collate(na ? na : "", nb ? nb : "");
}

static SeasonSection *section_get(int score, const char *label) {
    for (guint i = 0; i < sections->len; i++) {
        SeasonSection *s = sections->pdata[i];
        if (s->score == score)
            return s;
    }
    SeasonSection *s = g_new0(SeasonSection, 1);
    s->score = score;
    s->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    GtkWidget *heading = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(heading), 0.0);
    gtk_widget_add_css_class(heading, "season-heading");
    gtk_box_append(GTK_BOX(s->box), heading);
    s->grid = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(s->grid),
                                    GTK_SELECTION_NONE);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(s->grid), TRUE);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(s->grid), 16);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(s->grid), 16);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(s->grid), 2);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(s->grid), 30);
    gtk_flow_box_set_sort_func(GTK_FLOW_BOX(s->grid), flow_name_cmp, NULL,
                               NULL);
    gtk_widget_set_halign(s->grid, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(s->box), s->grid);

    // insert sorted: newest season first, unknown last
    guint pos = sections->len;
    for (guint i = 0; i < sections->len; i++)
        if (((SeasonSection *)sections->pdata[i])->score < score) {
            pos = i;
            break;
        }
    g_ptr_array_insert(sections, pos, s);
    gtk_box_append(GTK_BOX(library_box), s->box);
    gtk_box_reorder_child_after(
        GTK_BOX(library_box), s->box,
        pos == 0 ? errors_box
                 : ((SeasonSection *)sections->pdata[pos - 1])->box);
    return s;
}

// move card into the section for score, creating/pruning sections as needed
static void card_place(GtkWidget *card, int score, const char *label) {
    SeasonSection *target = section_get(score, label);
    GtkWidget *flow_child = gtk_widget_get_parent(card);
    if (flow_child) {
        GtkWidget *old_grid = gtk_widget_get_parent(flow_child);
        if (old_grid == target->grid)
            return;
        g_object_ref(card);
        gtk_flow_box_remove(GTK_FLOW_BOX(old_grid), flow_child);
        gtk_flow_box_insert(GTK_FLOW_BOX(target->grid), card, -1);
        g_object_unref(card);
        for (guint i = 0; i < sections->len; i++) {
            SeasonSection *s = sections->pdata[i];
            if (s->grid == old_grid && !gtk_widget_get_first_child(old_grid)) {
                gtk_box_remove(GTK_BOX(library_box), s->box);
                g_ptr_array_remove_index(sections, i);
                break;
            }
        }
    } else {
        gtk_flow_box_insert(GTK_FLOW_BOX(target->grid), card, -1);
    }
}

static gboolean card_is_current(GtkWidget *card) {
    return GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(card), "gen")) ==
           library_gen;
}

// AniList knows the broadcast season; move the card there if the local
// file-date guess was off
static void card_season_done(const AniInfo *info, gpointer data) {
    GtkWidget *card = data;
    if (card_is_current(card) && info && info->ok && info->season &&
        info->season_year > 0) {
        int idx = 0;
        if (!g_ascii_strcasecmp(info->season, "SPRING"))
            idx = 1;
        else if (!g_ascii_strcasecmp(info->season, "SUMMER"))
            idx = 2;
        else if (!g_ascii_strcasecmp(info->season, "FALL"))
            idx = 3;
        char *label =
            g_strdup_printf("%s %d", SEASON_NAMES[idx], info->season_year);
        card_place(card, info->season_year * 4 + idx, label);
        g_free(label);
    }
    g_object_unref(card);
}

// Per-card AniList aggregation: one lookup per episode group, badge
// refreshed once every response is in.
typedef struct {
    GtkWidget *badge; // ref'd
    int local_missing;
    gboolean season_gap;
    GString *tip;
    int shortfall;
    int pending;
} CardAni;

typedef struct {
    CardAni *agg;
    char *label;
    int local;
} CardAniReq;

static void card_ani_done(const AniInfo *info, gpointer data) {
    CardAniReq *req = data;
    CardAni *agg = req->agg;

    if (info && info->ok) {
        int expected = info->airing ? info->aired : info->episodes;
        if (expected > 0 && req->local < expected) {
            agg->shortfall += expected - req->local;
            if (agg->tip->len)
                g_string_append_c(agg->tip, '\n');
            g_string_append_printf(agg->tip, "%s: have %d of %d%s", req->label,
                                   req->local, expected,
                                   info->airing ? " aired" : "");
        }
    }

    if (--agg->pending == 0) {
        int total = agg->local_missing + agg->shortfall;
        if (total > 0 || agg->season_gap) {
            char *txt = total > 0 ? g_strdup_printf("%d missing", total)
                                  : g_strdup("season gap");
            gtk_label_set_text(GTK_LABEL(agg->badge), txt);
            g_free(txt);
            if (agg->tip->len)
                gtk_widget_set_tooltip_text(agg->badge, agg->tip->str);
            gtk_widget_set_visible(agg->badge, TRUE);
        }
        g_object_unref(agg->badge);
        g_string_free(agg->tip, TRUE);
        g_free(agg);
    }
    g_free(req->label);
    g_free(req);
}

static void card_load_done(GObject *src, GAsyncResult *res, gpointer data) {
    (void)src;
    (void)data;
    CardLoad *cl = g_task_get_task_data(G_TASK(res));
    if (!card_is_current(cl->card))
        return; // library was rebuilt while we were scanning

    // place by oldest-file date (downloads track airing), then let the
    // AniList answer correct it
    int score = -1;
    char *label = g_strdup("Unknown season");
    if (cl->min_mtime > 0) {
        GDateTime *dt = g_date_time_new_from_unix_local(cl->min_mtime);
        int idx = (g_date_time_get_month(dt) - 1) / 3;
        int year = g_date_time_get_year(dt);
        score = year * 4 + idx;
        g_free(label);
        label = g_strdup_printf("%s %d", SEASON_NAMES[idx], year);
        g_date_time_unref(dt);
    }
    card_place(cl->card, score, label);
    g_free(label);
    anilist_lookup(g_object_get_data(G_OBJECT(cl->card), "series-name"),
                   card_season_done, g_object_ref(cl->card));
    if (cl->pixbuf) {
        // gdk_texture_new_for_pixbuf is deprecated; wrap the pixels directly
        GdkPixbuf *pb = cl->pixbuf;
        int h = gdk_pixbuf_get_height(pb);
        gsize stride = gdk_pixbuf_get_rowstride(pb);
        gsize size = stride * (h - 1) +
                     (gsize)gdk_pixbuf_get_width(pb) *
                         gdk_pixbuf_get_n_channels(pb);
        GBytes *bytes = g_bytes_new(gdk_pixbuf_read_pixels(pb), size);
        GdkTexture *tex = gdk_memory_texture_new(
            gdk_pixbuf_get_width(pb), h,
            gdk_pixbuf_get_has_alpha(pb) ? GDK_MEMORY_R8G8B8A8
                                         : GDK_MEMORY_R8G8B8,
            bytes, stride);
        g_bytes_unref(bytes);
        gtk_picture_set_paintable(GTK_PICTURE(cl->picture),
                                  GDK_PAINTABLE(tex));
        g_object_unref(tex);
    }
    char *sub;
    if (cl->seasons > 1)
        sub = g_strdup_printf("%d seasons · %d episodes", cl->seasons,
                              cl->episodes);
    else
        sub = g_strdup_printf("%d episode%s", cl->episodes,
                              cl->episodes == 1 ? "" : "s");
    gtk_label_set_text(GTK_LABEL(cl->subtitle), sub);
    g_free(sub);

    if (cl->missing_eps > 0 || cl->missing_season) {
        char *txt;
        if (cl->missing_eps > 0)
            txt = g_strdup_printf("%d missing", cl->missing_eps);
        else
            txt = g_strdup("season gap");
        gtk_label_set_text(GTK_LABEL(cl->badge), txt);
        g_free(txt);
        if (cl->missing_tip)
            gtk_widget_set_tooltip_text(cl->badge, cl->missing_tip);
        gtk_widget_set_visible(cl->badge, TRUE);
    }

    // compare each group against AniList; the badge upgrades as answers
    // arrive (cached answers land immediately)
    if (cl->groups && cl->groups->len > 0) {
        CardAni *agg = g_new0(CardAni, 1);
        agg->badge = g_object_ref(cl->badge);
        agg->local_missing = cl->missing_eps;
        agg->season_gap = cl->missing_season;
        agg->tip = g_string_new(cl->missing_tip ? cl->missing_tip : "");
        agg->pending = cl->groups->len;
        for (guint i = 0; i < cl->groups->len; i++) {
            EpGroup *g = cl->groups->pdata[i];
            CardAniReq *req = g_new0(CardAniReq, 1);
            req->agg = agg;
            req->label = g_strdup(g->label);
            req->local = g->count;
            anilist_lookup(g->search, card_ani_done, req);
        }
    }
}

// ---------------------------------------------------------- series page --

static int verscmp(gconstpointer a, gconstpointer b) {
    return strverscmp(*(char *const *)a, *(char *const *)b);
}

// "[Erai-raws] Akane-banashi - 01 [1080p ...][...].mkv" -> "Episode 01",
// falling back to the filename minus extension and release-group prefix
static char *episode_title(const char *fname) {
    char *base = g_strdup(fname);
    char *dot = strrchr(base, '.');
    if (dot)
        *dot = '\0';

    static GRegex *re;
    if (!re)
        re = g_regex_new("\\s-\\s*([0-9]+(?:\\.[0-9]+)?(?:v[0-9]+)?)\\b", 0,
                         0, NULL);
    GMatchInfo *mi = NULL;
    char *out = NULL;
    if (g_regex_match(re, base, 0, &mi)) {
        char *num = g_match_info_fetch(mi, 1);
        out = g_strdup_printf("Episode %s", num);
        g_free(num);
    }
    g_match_info_free(mi);
    if (!out) {
        char *p = base;
        if (*p == '[') {
            char *end = strstr(p, "] ");
            if (end)
                p = end + 2;
        }
        out = g_strdup(p);
    }
    g_free(base);
    return out;
}

static void on_episode_activated(AdwActionRow *row, gpointer data) {
    (void)data;
    const char *path = g_object_get_data(G_OBJECT(row), "episode-path");
    GFile *file = g_file_new_for_path(path);
    GtkFileLauncher *launcher = gtk_file_launcher_new(file);
    gtk_file_launcher_launch(launcher, main_window, NULL, NULL, NULL);
    g_object_unref(launcher);
    g_object_unref(file);
}

// one episode group registered on a series page, for the manual refresh
typedef struct {
    char *search;
    GtkWidget *label; // ref'd
    int local;
} PageGroup;

static void page_group_free(gpointer p) {
    PageGroup *g = p;
    g_free(g->search);
    g_object_unref(g->label);
    g_free(g);
}

// pending manual-refresh state; re-enables the button when done
typedef struct {
    GtkWidget *button; // ref'd
    int pending;
} RefreshCtx;

typedef struct {
    GtkWidget *label; // ref'd
    int local;
    RefreshCtx *ctx; // NULL for the automatic page-load lookup
} PageAniReq;

static void page_ani_done(const AniInfo *info, gpointer data) {
    PageAniReq *req = data;
    gtk_widget_remove_css_class(req->label, "missing-label");
    gtk_widget_remove_css_class(req->label, "dim-label");
    if (info && !info->ok && req->ctx) {
        gtk_label_set_text(GTK_LABEL(req->label),
                           "AniList lookup failed (API down or no match)");
        gtk_widget_add_css_class(req->label, "dim-label");
        gtk_widget_set_visible(req->label, TRUE);
    }
    if (info && info->ok) {
        int expected = info->airing ? info->aired : info->episodes;
        if (expected > 0) {
            char *txt;
            if (req->local < expected) {
                txt = g_strdup_printf("Have %d of %d%s — AniList: %s",
                                      req->local, expected,
                                      info->airing ? " aired" : "",
                                      info->title ? info->title : "?");
                gtk_widget_add_css_class(req->label, "missing-label");
            } else {
                txt = g_strdup_printf("Complete — %d episode%s on AniList",
                                      expected, expected == 1 ? "" : "s");
                gtk_widget_add_css_class(req->label, "dim-label");
            }
            gtk_label_set_text(GTK_LABEL(req->label), txt);
            g_free(txt);
            gtk_widget_set_visible(req->label, TRUE);
        }
    }
    if (req->ctx && --req->ctx->pending == 0) {
        gtk_widget_set_sensitive(req->ctx->button, TRUE);
        g_object_unref(req->ctx->button);
        g_free(req->ctx);
        library_refresh(); // badges and season groups use the same cache
    }
    g_object_unref(req->label);
    g_free(req);
}

static void on_page_refresh(GtkButton *btn, gpointer data) {
    GPtrArray *groups = data;
    if (groups->len == 0)
        return;
    RefreshCtx *ctx = g_new0(RefreshCtx, 1);
    ctx->button = g_object_ref(GTK_WIDGET(btn));
    ctx->pending = groups->len;
    gtk_widget_set_sensitive(GTK_WIDGET(btn), FALSE);
    for (guint i = 0; i < groups->len; i++) {
        PageGroup *g = groups->pdata[i];
        gtk_widget_remove_css_class(g->label, "missing-label");
        gtk_widget_add_css_class(g->label, "dim-label");
        gtk_label_set_text(GTK_LABEL(g->label), "Checking AniList…");
        gtk_widget_set_visible(g->label, TRUE);
        PageAniReq *req = g_new0(PageAniReq, 1);
        req->label = g_object_ref(g->label);
        req->local = g->local;
        req->ctx = ctx;
        anilist_refresh(g->search, page_ani_done, req);
    }
}

// boxed list of the videos in one directory, under a heading; returns the
// episode count (0 = nothing appended). search is the AniList term for
// this group (NULL to skip the lookup); the group is also registered in
// page_groups for the manual refresh button.
static int append_episode_group(GtkWidget *box, const char *title,
                                const char *dir_path, const char *search,
                                GPtrArray *page_groups) {
    GPtrArray *files = g_ptr_array_new_with_free_func(g_free);
    GDir *dir = g_dir_open(dir_path, 0, NULL);
    if (dir) {
        const char *name;
        while ((name = g_dir_read_name(dir)))
            if (is_video(name))
                g_ptr_array_add(files, g_strdup(name));
        g_dir_close(dir);
    }
    g_ptr_array_sort(files, verscmp);

    int n = files->len;
    if (n > 0) {
        GtkWidget *heading = gtk_label_new(title);
        gtk_label_set_xalign(GTK_LABEL(heading), 0.0);
        gtk_widget_add_css_class(heading, "heading");
        gtk_box_append(GTK_BOX(box), heading);

        GArray *nums = g_array_new(FALSE, FALSE, sizeof(int));
        for (guint i = 0; i < files->len; i++) {
            int ep = episode_number(files->pdata[i]);
            if (ep >= 0)
                g_array_append_val(nums, ep);
        }
        GArray *gaps = find_gaps(nums);
        if (gaps->len > 0) {
            char *ranges = format_ranges(gaps);
            char *txt = g_strdup_printf("Missing: %s", ranges);
            GtkWidget *warn = gtk_label_new(txt);
            g_free(txt);
            g_free(ranges);
            gtk_widget_add_css_class(warn, "missing-label");
            gtk_label_set_xalign(GTK_LABEL(warn), 0.0);
            gtk_label_set_wrap(GTK_LABEL(warn), TRUE);
            gtk_box_append(GTK_BOX(box), warn);
        }
        g_array_unref(gaps);
        g_array_unref(nums);

        if (search) {
            GtkWidget *ani = gtk_label_new("");
            gtk_label_set_xalign(GTK_LABEL(ani), 0.0);
            gtk_label_set_wrap(GTK_LABEL(ani), TRUE);
            gtk_widget_add_css_class(ani, "anilist-label");
            gtk_widget_set_visible(ani, FALSE);
            gtk_box_append(GTK_BOX(box), ani);
            PageAniReq *req = g_new0(PageAniReq, 1);
            req->label = g_object_ref(ani);
            req->local = n;
            anilist_lookup(search, page_ani_done, req);
            if (page_groups) {
                PageGroup *g = g_new0(PageGroup, 1);
                g->search = g_strdup(search);
                g->label = g_object_ref(ani);
                g->local = n;
                g_ptr_array_add(page_groups, g);
            }
        }

        GtkWidget *list = gtk_list_box_new();
        gtk_list_box_set_selection_mode(GTK_LIST_BOX(list),
                                        GTK_SELECTION_NONE);
        gtk_widget_add_css_class(list, "boxed-list");
        for (guint i = 0; i < files->len; i++) {
            GtkWidget *row = adw_action_row_new();
            char *title_txt = episode_title(files->pdata[i]);
            char *escaped = g_markup_escape_text(title_txt, -1);
            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), escaped);
            g_free(escaped);
            g_free(title_txt);
            GtkWidget *play = gtk_image_new_from_icon_name(
                "media-playback-start-symbolic");
            adw_action_row_add_suffix(ADW_ACTION_ROW(row), play);
            gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
            adw_action_row_set_activatable_widget(ADW_ACTION_ROW(row), NULL);
            g_object_set_data_full(
                G_OBJECT(row), "episode-path",
                g_build_filename(dir_path, files->pdata[i], NULL), g_free);
            g_signal_connect(row, "activated",
                             G_CALLBACK(on_episode_activated), NULL);
            gtk_list_box_append(GTK_LIST_BOX(list), row);
        }
        gtk_box_append(GTK_BOX(box), list);
    }
    g_ptr_array_unref(files);
    return n;
}

static void open_series(const char *path, const char *name,
                        GdkPaintable *cover) {
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_set_margin_top(content, 24);
    gtk_widget_set_margin_bottom(content, 24);
    gtk_widget_set_margin_start(content, 18);
    gtk_widget_set_margin_end(content, 18);

    // header: poster + title + counts
    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 20);
    // fixed-size bin so the picture can never balloon to the texture's
    // natural size (a size request is only a minimum)
    GtkWidget *poster_bin = gtk_overlay_new();
    gtk_widget_set_size_request(poster_bin, POSTER_W, POSTER_H);
    gtk_widget_set_overflow(poster_bin, GTK_OVERFLOW_HIDDEN);
    gtk_widget_add_css_class(poster_bin, "detail-poster");
    gtk_widget_set_halign(poster_bin, GTK_ALIGN_START);
    gtk_widget_set_valign(poster_bin, GTK_ALIGN_START);
    // overlay children aren't measured, so the picture's natural (texture)
    // size can't inflate the box beyond the fixed request
    gtk_overlay_set_child(GTK_OVERLAY(poster_bin),
                          gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
    GtkWidget *poster = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(poster), GTK_CONTENT_FIT_COVER);
    if (cover)
        gtk_picture_set_paintable(GTK_PICTURE(poster), cover);
    gtk_overlay_add_overlay(GTK_OVERLAY(poster_bin), poster);
    gtk_box_append(GTK_BOX(head), poster_bin);

    GtkWidget *meta = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_valign(meta, GTK_ALIGN_END);
    GtkWidget *title = gtk_label_new(name);
    gtk_widget_add_css_class(title, "detail-title");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0);
    gtk_label_set_wrap(GTK_LABEL(title), TRUE);
    gtk_box_append(GTK_BOX(meta), title);
    GtkWidget *counts = gtk_label_new("");
    gtk_widget_add_css_class(counts, "dim-label");
    gtk_label_set_xalign(GTK_LABEL(counts), 0.0);
    gtk_box_append(GTK_BOX(meta), counts);
    GtkWidget *where = gtk_label_new(path);
    gtk_widget_add_css_class(where, "dim-label");
    gtk_widget_add_css_class(where, "caption-label");
    gtk_label_set_xalign(GTK_LABEL(where), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(where), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_append(GTK_BOX(meta), where);
    gtk_box_append(GTK_BOX(head), meta);
    gtk_box_append(GTK_BOX(content), head);

    // episodes: top-level files, then each Season-style subdirectory
    int episodes = 0, seasons = 0;
    GPtrArray *subdirs = g_ptr_array_new_with_free_func(g_free);
    GDir *dir = g_dir_open(path, 0, NULL);
    if (dir) {
        const char *entry;
        while ((entry = g_dir_read_name(dir))) {
            if (entry[0] == '.')
                continue;
            char *full = g_build_filename(path, entry, NULL);
            if (g_file_test(full, G_FILE_TEST_IS_DIR))
                g_ptr_array_add(subdirs, g_strdup(entry));
            g_free(full);
        }
        g_dir_close(dir);
    }
    g_ptr_array_sort(subdirs, verscmp);

    GPtrArray *page_groups =
        g_ptr_array_new_with_free_func(page_group_free);
    GArray *season_nums = g_array_new(FALSE, FALSE, sizeof(int));
    episodes += append_episode_group(content, "Episodes", path, name,
                                     page_groups);
    for (guint i = 0; i < subdirs->len; i++) {
        char *sub = g_build_filename(path, subdirs->pdata[i], NULL);
        char *search = g_strdup_printf("%s %s", name,
                                       (char *)subdirs->pdata[i]);
        int n = append_episode_group(content, subdirs->pdata[i], sub, search,
                                     page_groups);
        g_free(search);
        if (n > 0) {
            seasons++;
            episodes += n;
            int sn = season_number(subdirs->pdata[i]);
            if (sn >= 0)
                g_array_append_val(season_nums, sn);
        }
        g_free(sub);
    }
    g_ptr_array_unref(subdirs);

    GArray *season_gaps = find_gaps(season_nums);
    if (season_gaps->len > 0) {
        char *ranges = format_ranges(season_gaps);
        char *txt = g_strdup_printf("Missing season%s: %s",
                                    season_gaps->len == 1 ? "" : "s", ranges);
        GtkWidget *warn = gtk_label_new(txt);
        g_free(txt);
        g_free(ranges);
        gtk_widget_add_css_class(warn, "missing-label");
        gtk_label_set_xalign(GTK_LABEL(warn), 0.0);
        gtk_box_append(GTK_BOX(meta), warn);
    }
    g_array_unref(season_gaps);
    g_array_unref(season_nums);

    char *sub;
    if (seasons > 1)
        sub = g_strdup_printf("%d seasons · %d episodes", seasons, episodes);
    else
        sub = g_strdup_printf("%d episode%s", episodes,
                              episodes == 1 ? "" : "s");
    gtk_label_set_text(GTK_LABEL(counts), sub);
    g_free(sub);

    if (episodes == 0) {
        GtkWidget *msg = gtk_label_new("No episodes found");
        gtk_widget_add_css_class(msg, "dim-label");
        gtk_label_set_xalign(GTK_LABEL(msg), 0.0);
        gtk_box_append(GTK_BOX(content), msg);
    }

    GtkWidget *clamp = adw_clamp_new();
    adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 860);
    adw_clamp_set_child(ADW_CLAMP(clamp), content);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), clamp);

    GtkWidget *view = adw_toolbar_view_new();
    GtkWidget *header = adw_header_bar_new();
    GtkWidget *refresh =
        gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_set_tooltip_text(refresh,
                                "Refresh episode counts from AniList");
    g_signal_connect(refresh, "clicked", G_CALLBACK(on_page_refresh),
                     page_groups);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), refresh);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), scroll);

    AdwNavigationPage *page = adw_navigation_page_new(view, name);
    // the groups live exactly as long as the page
    g_object_set_data_full(G_OBJECT(page), "page-groups", page_groups,
                           (GDestroyNotify)g_ptr_array_unref);
    adw_navigation_view_push(ADW_NAVIGATION_VIEW(nav_view), page);
}

static void on_card_clicked(GtkGestureClick *gesture, int n_press, double x,
                            double y, gpointer data) {
    (void)n_press;
    (void)x;
    (void)y;
    GtkWidget *card = data;
    GtkWidget *pic = g_object_get_data(G_OBJECT(card), "cover-picture");
    open_series(g_object_get_data(G_OBJECT(card), "series-path"),
                g_object_get_data(G_OBJECT(card), "series-name"),
                pic ? gtk_picture_get_paintable(GTK_PICTURE(pic)) : NULL);
    gtk_gesture_set_state(GTK_GESTURE(gesture),
                          GTK_EVENT_SEQUENCE_CLAIMED);
}

static GtkWidget *build_anime_card(const char *folder, const char *name) {
    // the whole card is the poster; title + counts sit on a gradient
    // scrim over the artwork's bottom edge
    GtkWidget *card = gtk_overlay_new();
    gtk_widget_add_css_class(card, "anime-card");
    gtk_widget_set_overflow(card, GTK_OVERFLOW_HIDDEN);
    gtk_widget_set_size_request(card, POSTER_W, POSTER_H);

    GtkWidget *ph = gtk_image_new_from_icon_name("folder-videos-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(ph), 48);
    gtk_widget_add_css_class(ph, "poster-placeholder");
    gtk_overlay_set_child(GTK_OVERLAY(card), ph);

    GtkWidget *pic = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(pic), GTK_CONTENT_FIT_COVER);
    gtk_overlay_add_overlay(GTK_OVERLAY(card), pic);

    GtkWidget *caption = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_add_css_class(caption, "caption");
    gtk_widget_set_valign(caption, GTK_ALIGN_END);

    GtkWidget *title = gtk_label_new(name);
    gtk_widget_add_css_class(title, "anime-title");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0);
    gtk_label_set_wrap(GTK_LABEL(title), TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
    gtk_label_set_lines(GTK_LABEL(title), 2);
    // natural width ~0 so the flowbox child stays poster-width
    gtk_label_set_max_width_chars(GTK_LABEL(title), 1);
    gtk_box_append(GTK_BOX(caption), title);

    GtkWidget *subtitle = gtk_label_new("…");
    gtk_widget_add_css_class(subtitle, "anime-sub");
    gtk_label_set_xalign(GTK_LABEL(subtitle), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(subtitle), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(subtitle), 1);
    gtk_box_append(GTK_BOX(caption), subtitle);

    gtk_overlay_add_overlay(GTK_OVERLAY(card), caption);

    // warning chip in the poster's top-right corner, shown by the loader
    // when the collection has holes
    GtkWidget *badge = gtk_label_new("");
    gtk_widget_add_css_class(badge, "missing-badge");
    gtk_widget_set_halign(badge, GTK_ALIGN_END);
    gtk_widget_set_valign(badge, GTK_ALIGN_START);
    gtk_widget_set_margin_top(badge, 8);
    gtk_widget_set_margin_end(badge, 8);
    gtk_widget_set_visible(badge, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(card), badge);

    g_object_set_data_full(G_OBJECT(card), "series-path",
                           g_build_filename(folder, name, NULL), g_free);
    g_object_set_data_full(G_OBJECT(card), "series-name", g_strdup(name),
                           g_free);
    g_object_set_data(G_OBJECT(card), "cover-picture", pic);
    g_object_set_data(G_OBJECT(card), "gen", GUINT_TO_POINTER(library_gen));
    gtk_widget_set_cursor_from_name(card, "pointer");
    GtkGesture *click = gtk_gesture_click_new();
    g_signal_connect(click, "released", G_CALLBACK(on_card_clicked), card);
    gtk_widget_add_controller(card, GTK_EVENT_CONTROLLER(click));

    CardLoad *cl = g_new0(CardLoad, 1);
    cl->path = g_build_filename(folder, name, NULL);
    cl->card = g_object_ref_sink(card); // parented later by card_place
    cl->picture = g_object_ref(pic);
    cl->subtitle = g_object_ref(subtitle);
    cl->badge = g_object_ref(badge);
    GTask *task = g_task_new(NULL, NULL, card_load_done, NULL);
    g_task_set_task_data(task, cl, card_load_free);
    g_task_run_in_thread(task, card_load_thread);
    g_object_unref(task);

    return card;
}

static int name_collate(gconstpointer a, gconstpointer b) {
    return g_utf8_collate(*(char *const *)a, *(char *const *)b);
}

// Rebuild the main window content: cards from every configured folder,
// grouped into season sections as their background scans resolve.
static void library_refresh(void) {
    library_gen++;
    g_clear_pointer(&sections, g_ptr_array_unref);
    sections = g_ptr_array_new_with_free_func(g_free);
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(library_box)))
        gtk_box_remove(GTK_BOX(library_box), child);
    errors_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_append(GTK_BOX(library_box), errors_box);

    if (folders->len == 0) {
        gtk_stack_set_visible_child_name(GTK_STACK(library_stack), "empty");
        return;
    }
    for (guint i = 0; i < folders->len; i++) {
        const char *folder = folders->pdata[i];
        GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
        GDir *dir = g_dir_open(folder, 0, NULL);
        if (dir) {
            const char *name;
            while ((name = g_dir_read_name(dir))) {
                if (name[0] == '.')
                    continue;
                char *full = g_build_filename(folder, name, NULL);
                if (g_file_test(full, G_FILE_TEST_IS_DIR))
                    g_ptr_array_add(names, g_strdup(name));
                g_free(full);
            }
            g_dir_close(dir);
        } else {
            char *txt = g_strdup_printf("Folder not accessible: %s", folder);
            GtkWidget *msg = gtk_label_new(txt);
            g_free(txt);
            gtk_label_set_xalign(GTK_LABEL(msg), 0.0);
            gtk_widget_add_css_class(msg, "dim-label");
            gtk_box_append(GTK_BOX(errors_box), msg);
        }
        g_ptr_array_sort(names, name_collate);
        for (guint j = 0; j < names->len; j++)
            build_anime_card(folder, names->pdata[j]);
        g_ptr_array_unref(names);
    }
    gtk_stack_set_visible_child_name(GTK_STACK(library_stack), "library");
}

// -------------------------------------------------------------- settings --

static void settings_rows_refresh(void);

static void on_folder_remove(GtkButton *btn, gpointer data) {
    (void)btn;
    guint idx = GPOINTER_TO_UINT(data);
    g_ptr_array_remove_index(folders, idx);
    config_save();
    settings_rows_refresh();
    library_refresh();
}

static void settings_rows_refresh(void) {
    if (!folders_group)
        return;
    for (guint i = 0; i < folder_rows->len; i++)
        adw_preferences_group_remove(folders_group, folder_rows->pdata[i]);
    g_ptr_array_set_size(folder_rows, 0);

    if (folders->len == 0) {
        GtkWidget *row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                      "No folders configured");
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row),
                                    "Add a folder that contains your anime");
        adw_preferences_group_add(folders_group, row);
        g_ptr_array_add(folder_rows, row);
        return;
    }
    for (guint i = 0; i < folders->len; i++) {
        GtkWidget *row = adw_action_row_new();
        char *escaped = g_markup_escape_text(folders->pdata[i], -1);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), escaped);
        g_free(escaped);

        GtkWidget *remove = gtk_button_new_from_icon_name("edit-delete-symbolic");
        gtk_widget_set_valign(remove, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(remove, "flat");
        gtk_widget_set_tooltip_text(remove, "Remove folder");
        g_signal_connect(remove, "clicked", G_CALLBACK(on_folder_remove),
                         GUINT_TO_POINTER(i));
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), remove);

        adw_preferences_group_add(folders_group, row);
        g_ptr_array_add(folder_rows, row);
    }
}

static void on_folder_chosen(GObject *src, GAsyncResult *res, gpointer data) {
    (void)data;
    GFile *file =
        gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, NULL);
    if (!file)
        return; // dismissed
    char *path = g_file_get_path(file);
    g_object_unref(file);
    if (!path)
        return;

    gboolean dup = FALSE;
    for (guint i = 0; i < folders->len; i++)
        if (g_strcmp0(folders->pdata[i], path) == 0)
            dup = TRUE;
    if (dup) {
        g_free(path);
        return;
    }
    g_ptr_array_add(folders, path);
    config_save();
    settings_rows_refresh();
    library_refresh();
}

static void on_add_folder(GtkButton *btn, gpointer data) {
    (void)btn;
    (void)data;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "Select anime folder");
    gtk_file_dialog_select_folder(dlg, main_window, NULL, on_folder_chosen,
                                  NULL);
    g_object_unref(dlg);
}

static void on_settings_closed(AdwDialog *dlg, gpointer data) {
    (void)dlg;
    (void)data;
    folders_group = NULL;
    g_clear_pointer(&folder_rows, g_ptr_array_unref);
}

static void open_settings(GtkButton *btn, gpointer data) {
    (void)btn;
    (void)data;
    AdwDialog *dlg = adw_preferences_dialog_new();
    adw_dialog_set_title(dlg, "Settings");
    adw_dialog_set_content_width(dlg, 560);

    AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    folders_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(folders_group, "Anime folders");
    adw_preferences_group_set_description(
        folders_group, "Each subdirectory of these folders is treated as one "
                       "anime");

    GtkWidget *add = gtk_button_new_from_icon_name("list-add-symbolic");
    gtk_widget_add_css_class(add, "flat");
    gtk_widget_set_tooltip_text(add, "Add folder");
    g_signal_connect(add, "clicked", G_CALLBACK(on_add_folder), NULL);
    adw_preferences_group_set_header_suffix(folders_group, add);

    folder_rows = g_ptr_array_new();
    settings_rows_refresh();

    adw_preferences_page_add(page, folders_group);
    adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(dlg), page);
    g_signal_connect(dlg, "closed", G_CALLBACK(on_settings_closed), NULL);
    adw_dialog_present(dlg, GTK_WIDGET(main_window));
}

// ------------------------------------------------------------------- app --

// dev hook: NEKOLAND_DEBUG_SCROLL=1 logs every scroll event reaching the
// window plus the resulting adjustment moves, to diagnose dropped wheel
// input
static gboolean dbg_scroll(GtkEventControllerScroll *c, double dx, double dy,
                           gpointer data) {
    (void)data;
    g_printerr("[scroll] dx=%+.3f dy=%+.3f unit=%s t=%u\n", dx, dy,
               gtk_event_controller_scroll_get_unit(c) ==
                       GDK_SCROLL_UNIT_WHEEL
                   ? "wheel"
                   : "surface",
               gdk_event_get_time(gtk_event_controller_get_current_event(
                   GTK_EVENT_CONTROLLER(c))));
    return FALSE; // observe only
}

static void dbg_scroll_edge(GtkEventControllerScroll *c, gpointer data) {
    g_printerr("[scroll] %s\n", (const char *)data);
    (void)c;
}

static void dbg_adj_changed(GtkAdjustment *adj, gpointer data) {
    (void)data;
    g_printerr("[adj] value=%.1f\n", gtk_adjustment_get_value(adj));
}

static void dbg_scroll_attach(GtkWidget *win, GtkWidget *scroll) {
    if (!g_getenv("NEKOLAND_DEBUG_SCROLL"))
        return;
    GtkEventController *c =
        gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    gtk_event_controller_set_propagation_phase(c, GTK_PHASE_CAPTURE);
    g_signal_connect(c, "scroll", G_CALLBACK(dbg_scroll), NULL);
    g_signal_connect(c, "scroll-begin", G_CALLBACK(dbg_scroll_edge), "begin");
    g_signal_connect(c, "scroll-end", G_CALLBACK(dbg_scroll_edge), "end");
    gtk_widget_add_controller(win, c);
    g_signal_connect(gtk_scrolled_window_get_vadjustment(
                         GTK_SCROLLED_WINDOW(scroll)),
                     "value-changed", G_CALLBACK(dbg_adj_changed), NULL);
}

static void load_css(void) {
    char *exe = g_file_read_link("/proc/self/exe", NULL);
    char *dir = g_path_get_dirname(exe ? exe : ".");
    char *css = g_build_filename(dir, "style.css", NULL);
    if (g_file_test(css, G_FILE_TEST_EXISTS)) {
        GtkCssProvider *prov = gtk_css_provider_new();
        gtk_css_provider_load_from_path(prov, css);
        gtk_style_context_add_provider_for_display(
            gdk_display_get_default(), GTK_STYLE_PROVIDER(prov),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(prov);
    }
    g_free(css);
    g_free(dir);
    g_free(exe);
}

static void activate(AdwApplication *app, gpointer data) {
    (void)data;
    load_css();
    regexes_init();
    anilist_init();
    config_load();

    GtkWidget *win = adw_application_window_new(GTK_APPLICATION(app));
    main_window = GTK_WINDOW(win);
    gtk_window_set_title(main_window, "Animanager");
    gtk_window_set_default_size(main_window, 900, 640);

    GtkWidget *view = adw_toolbar_view_new();
    GtkWidget *header = adw_header_bar_new();
    GtkWidget *settings_btn =
        gtk_button_new_from_icon_name("emblem-system-symbolic");
    gtk_widget_set_tooltip_text(settings_btn, "Settings");
    g_signal_connect(settings_btn, "clicked", G_CALLBACK(open_settings), NULL);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), settings_btn);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);

    library_stack = gtk_stack_new();

    // empty state: no folders configured yet
    GtkWidget *status = adw_status_page_new();
    adw_status_page_set_title(ADW_STATUS_PAGE(status), "No anime folders");
    adw_status_page_set_description(
        ADW_STATUS_PAGE(status),
        "Add the folders that hold your anime to build the library");
    adw_status_page_set_icon_name(ADW_STATUS_PAGE(status),
                                  "folder-videos-symbolic");
    GtkWidget *open_btn = gtk_button_new_with_label("Open Settings");
    gtk_widget_add_css_class(open_btn, "suggested-action");
    gtk_widget_add_css_class(open_btn, "pill");
    gtk_widget_set_halign(open_btn, GTK_ALIGN_CENTER);
    g_signal_connect(open_btn, "clicked", G_CALLBACK(open_settings), NULL);
    adw_status_page_set_child(ADW_STATUS_PAGE(status), open_btn);
    gtk_stack_add_named(GTK_STACK(library_stack), status, "empty");

    // library: per-folder sections in a clamped scrolling column
    library_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 24);
    gtk_widget_set_margin_top(library_box, 24);
    gtk_widget_set_margin_bottom(library_box, 24);
    gtk_widget_set_margin_start(library_box, 18);
    gtk_widget_set_margin_end(library_box, 18);
    GtkWidget *clamp = adw_clamp_new();
    adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 1400);
    adw_clamp_set_child(ADW_CLAMP(clamp), library_box);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), clamp);
    gtk_stack_add_named(GTK_STACK(library_stack), scroll, "library");
    dbg_scroll_attach(win, scroll);

    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), library_stack);

    nav_view = adw_navigation_view_new();
    adw_navigation_view_add(ADW_NAVIGATION_VIEW(nav_view),
                            adw_navigation_page_new(view, "Animanager"));
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(win), nav_view);

    library_refresh();
    gtk_window_present(main_window);
}

int main(int argc, char **argv) {
    // GTK defaults to the Vulkan renderer, which stutters when scrolling
    // on NVIDIA + Wayland; prefer GL (FALSE keeps any explicit override)
    g_setenv("GSK_RENDERER", "gl", FALSE);
    AdwApplication *app = adw_application_new("org.nekoland.Animanager",
                                              G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
