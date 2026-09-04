#include "nekobar.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

void spawn_cmd(const char *shell_cmd) {
    char *argv[] = {"sh", "-c", (char *)shell_cmd, NULL};
    g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
}

// ---- clock ----

static gboolean clock_tick(gpointer data) {
    (void)data;
    char buf[128];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(buf, sizeof(buf), "%H\n%M", &tm);
    char tip[64];
    strftime(tip, sizeof(tip), "%A %e %B", &tm);
    for (guint i = 0; i < bars->len; i++) {
        Bar *bar = g_ptr_array_index(bars, i);
        gtk_label_set_text(GTK_LABEL(bar->clock_label), buf);
        gtk_widget_set_tooltip_text(bar->clock_label, tip);
    }
    return TRUE;
}

// ---- memory ----

static gboolean mem_tick(gpointer data) {
    (void)data;
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f)
        return TRUE;
    double total = 0, avail = 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        sscanf(line, "MemTotal: %lf kB", &total);
        sscanf(line, "MemAvailable: %lf kB", &avail);
    }
    fclose(f);
    char buf[64];
    g_snprintf(buf, sizeof(buf), "%.0fG", (total - avail) / 1048576.0);
    for (guint i = 0; i < bars->len; i++) {
        Bar *bar = g_ptr_array_index(bars, i);
        gtk_label_set_text(GTK_LABEL(bar->mem_label), buf);
    }
    return TRUE;
}

// ---- volume (wireplumber) ----

double cur_volume;
gboolean cur_muted;

static gboolean vol_tick(gpointer data) {
    (void)data;
    char *out = NULL;
    char *argv[] = {"wpctl", "get-volume", "@DEFAULT_AUDIO_SINK@", NULL};
    if (!g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, &out,
                      NULL, NULL, NULL) ||
        !out)
        return TRUE;
    double vol = 0;
    sscanf(out, "Volume: %lf", &vol);
    gboolean muted = strstr(out, "MUTED") != NULL;
    g_free(out);
    cur_volume = vol;
    cur_muted = muted;

    // the button shows a fixed quick-settings glyph; volume state lives
    // in the tooltip and the quickset panel
    char tip[48];
    g_snprintf(tip, sizeof(tip), muted ? "muted" : "volume %d%%",
               (int)(vol * 100 + 0.5));
    for (guint i = 0; i < bars->len; i++) {
        Bar *bar = g_ptr_array_index(bars, i);
        gtk_widget_set_tooltip_text(bar->vol_label, tip);
    }
    quickset_sync();
    return TRUE;
}

void volume_refresh(void) {
    vol_tick(NULL);
}

// ---- mpris (via playerctl) ----

static gboolean mpris_tick(gpointer data) {
    (void)data;
    char *out = NULL;
    int status = 0;
    char *argv[] = {"playerctl", "metadata", "--format",
                    "{{playerName}}\x1f{{artist}}\x1f{{title}}\x1f{{status}}",
                    NULL};
    gboolean ok = g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL,
                               NULL, &out, NULL, &status, NULL);
    gboolean have = ok && status == 0 && out && strchr(out, 0x1f);

    char label[256] = "";
    if (have) {
        g_strchomp(out);
        char **parts = g_strsplit(out, "\x1f", 4);
        if (g_strv_length(parts) == 4) {
            const char *player = parts[0], *artist = parts[1], *status_s = parts[3];
            gboolean playing = g_str_equal(status_s, "Playing");
            const char *icon;
            if (!playing)
                icon = "";
            else if (strstr(player, "spotify"))
                icon = "\U000F04C7";
            else if (strstr(player, "mpv"))
                icon = "";
            else
                icon = "\U000F040A"; // NF play glyph: the U+25B6
                // fallback font has taller metrics, which widens
                // the rotated pill by a full extra line
            // truncate title to 20 chars like waybar's title-len
            char *title = g_utf8_substring(parts[2], 0,
                                           MIN(20, g_utf8_strlen(parts[2], -1)));
            g_snprintf(label, sizeof(label), "%s - %s", artist, title);
            g_free(title);
            for (guint i = 0; i < bars->len; i++) {
                Bar *bar = g_ptr_array_index(bars, i);
                gtk_label_set_text(GTK_LABEL(bar->mpris_icon), icon);
            }
        } else {
            have = FALSE;
        }
        g_strfreev(parts);
    }
    g_free(out);

    for (guint i = 0; i < bars->len; i++) {
        Bar *bar = g_ptr_array_index(bars, i);
        gtk_widget_set_visible(bar->mpris_event, have);
        if (have)
            gtk_label_set_text(GTK_LABEL(bar->mpris_label), label);
    }
    return TRUE;
}

// ---- event-driven volume: pactl subscribe pushes sink changes ----
// (the poll below stays only as a slow fallback; without this, roller /
// keybind changes took up to 2s to show in the capsule)

static void pactl_subscribe_start(void);

static gboolean pactl_retry(gpointer data) {
    (void)data;
    pactl_subscribe_start();
    return G_SOURCE_REMOVE;
}

static guint vol_refresh_pending;

static gboolean vol_refresh_now(gpointer data) {
    (void)data;
    vol_refresh_pending = 0;
    volume_refresh();
    return G_SOURCE_REMOVE;
}

static gboolean pactl_event(GIOChannel *ch, GIOCondition cond,
                            gpointer data) {
    (void)cond;
    (void)data;
    char *line = NULL;
    gsize len = 0;
    GIOStatus st;
    gboolean dirty = FALSE;
    while ((st = g_io_channel_read_line(ch, &line, &len, NULL, NULL)) ==
           G_IO_STATUS_NORMAL) {
        if (strstr(line, "sink") || strstr(line, "server"))
            dirty = TRUE;
        g_free(line);
        line = NULL;
    }
    g_free(line);
    if (dirty && !vol_refresh_pending) // debounce event bursts
        vol_refresh_pending = g_timeout_add(50, vol_refresh_now, NULL);
    if (st == G_IO_STATUS_EOF) { // pipewire restarted: reconnect
        g_timeout_add_seconds(5, pactl_retry, NULL);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void pactl_subscribe_start(void) {
    char *argv[] = {"pactl", "subscribe", NULL};
    gint out_fd = -1;
    if (!g_spawn_async_with_pipes(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                                  NULL, NULL, NULL, NULL, &out_fd, NULL,
                                  NULL)) {
        g_timeout_add_seconds(5, pactl_retry, NULL);
        return;
    }
    GIOChannel *ch = g_io_channel_unix_new(out_fd);
    g_io_channel_set_flags(ch, G_IO_FLAG_NONBLOCK, NULL);
    g_io_add_watch(ch, G_IO_IN | G_IO_HUP, pactl_event, NULL);
}

void modules_start(void) {
    clock_tick(NULL);
    mem_tick(NULL);
    vol_tick(NULL);
    mpris_tick(NULL);
    g_timeout_add_seconds(5, clock_tick, NULL);
    g_timeout_add_seconds(30, mem_tick, NULL);
    g_timeout_add_seconds(15, vol_tick, NULL); // slow fallback only
    pactl_subscribe_start();
    g_timeout_add_seconds(2, mpris_tick, NULL);
}
