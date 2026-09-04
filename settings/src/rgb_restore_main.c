// nekoland-rgb-restore — headless one-shot that reapplies the RGB setup
// remembered in ~/.config/nekoland/rgb-state.ini (written continuously by
// the settings app) to the hardware. Run once at login:
//
//   exec-once = nekoland-rgb-restore
//
// Scans the same native providers as the settings app, matches devices by
// id, and replays mode / colour / brightness / speed / per-LED paint.

#include "rgb.h"

#include <stdio.h>
#include <stdlib.h>
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

static int find_mode(RgbDevice *d, const char *name) {
    if (!d->modes)
        return -1;
    for (guint m = 0; m < d->modes->len; m++)
        if (!g_ascii_strcasecmp(g_ptr_array_index(d->modes, m), name))
            return (int)m;
    return -1;
}

static const char *mode_name(RgbDevice *d) {
    if (d->modes && d->cur_mode >= 0 && (guint)d->cur_mode < d->modes->len)
        return g_ptr_array_index(d->modes, d->cur_mode);
    return "Static";
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
    const char *mode = mode_name(d);
    if (!g_ascii_strcasecmp(mode, "Static") ||
        !g_ascii_strcasecmp(mode, "Direct")) {
        GdkRGBA eff = rgb_effective_color(d);
        d->provider->set_color(d, &eff);
    } else {
        d->provider->set_mode(d, mode);
    }
}

// one remembered device from rgb-state.ini
typedef struct {
    char *id;
    char *name;  // fallback match key: hidraw numbers shift across boots
    char *state;
    char *leds;
    gboolean used;
} SavedDev;

static void saved_free(gpointer p) {
    SavedDev *s = p;
    g_free(s->id);
    g_free(s->name);
    g_free(s->state);
    g_free(s->leds);
    g_free(s);
}

static SavedDev *saved_match(GPtrArray *saved, RgbDevice *d) {
    for (guint i = 0; i < saved->len; i++) { // exact id first
        SavedDev *s = g_ptr_array_index(saved, i);
        if (!s->used && g_str_equal(s->id, d->id))
            return s;
    }
    for (guint i = 0; i < saved->len; i++) { // then by device name
        SavedDev *s = g_ptr_array_index(saved, i);
        if (!s->used && s->name && g_str_equal(s->name, d->name))
            return s;
    }
    return NULL;
}

int main(void) {
    char *path = g_build_filename(g_get_user_config_dir(), "nekoland",
                                  "rgb-state.ini", NULL);
    GKeyFile *kf = g_key_file_new();
    if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        fprintf(stderr, "rgb-restore: no %s, nothing to do\n", path);
        g_free(path);
        return 0;
    }
    g_free(path);

    GPtrArray *saved = g_ptr_array_new_with_free_func(saved_free);
    gsize ngroups = 0;
    char **grp = g_key_file_get_groups(kf, &ngroups);
    for (gsize i = 0; i < ngroups; i++) {
        if (g_str_equal(grp[i], "meta"))
            continue;
        SavedDev *s = g_new0(SavedDev, 1);
        s->id = g_strdup(grp[i]);
        s->name = g_key_file_get_string(kf, grp[i], "name", NULL);
        s->state = g_key_file_get_string(kf, grp[i], "state", NULL);
        s->leds = g_key_file_get_string(kf, grp[i], "leds", NULL);
        if (s->state)
            g_ptr_array_add(saved, s);
        else
            saved_free(s);
    }
    g_strfreev(grp);
    g_key_file_free(kf);

    rgb_claimed_locations = g_ptr_array_new_with_free_func(g_free);
    int restored = 0;
    for (const RgbProvider **p = providers; *p; p++) {
        GPtrArray *found = (*p)->list();
        for (guint i = 0; i < found->len; i++) {
            RgbDevice *d = g_ptr_array_index(found, i);
            SavedDev *sv = saved_match(saved, d);
            if (!sv) {
                fprintf(stderr, "rgb-restore: no saved state for %s (%s)\n",
                        d->id, d->name);
                continue;
            }
            sv->used = TRUE;
            char **f = g_strsplit(sv->state, "|", 5);
            if (g_strv_length(f) == 5) {
                int m = find_mode(d, f[0]);
                if (m >= 0)
                    d->cur_mode = m;
                gdk_rgba_parse(&d->color, f[1]);
                d->brightness = g_ascii_strtod(f[2], NULL);
                d->speed = atoi(f[3]);
                d->enabled = g_str_equal(f[4], "1");
                apply_device(d);
                // replay per-LED paint on top of a static base
                if (sv->leds && d->n_leds > 0 && d->provider->set_led &&
                    d->enabled &&
                    (!g_ascii_strcasecmp(mode_name(d), "Static") ||
                     !g_ascii_strcasecmp(mode_name(d), "Direct"))) {
                    char **c = g_strsplit(sv->leds, ";", -1);
                    for (int l = 0;
                         c[l] && l < d->n_leds && l < RGB_MAX_LEDS; l++) {
                        GdkRGBA lc;
                        if (!gdk_rgba_parse(&lc, c[l]))
                            continue;
                        d->led_colors[l] = lc;
                        lc.red *= d->brightness;
                        lc.green *= d->brightness;
                        lc.blue *= d->brightness;
                        d->provider->set_led(d, l, &lc);
                    }
                    g_strfreev(c);
                }
                fprintf(stderr, "rgb-restore: %s <- %s\n", d->name,
                        sv->state);
                restored++;
            }
            g_strfreev(f);
        }
        g_ptr_array_free(found, TRUE);
    }
    g_ptr_array_free(saved, TRUE);
    fprintf(stderr, "rgb-restore: %d device(s) restored\n", restored);
    return 0;
}
