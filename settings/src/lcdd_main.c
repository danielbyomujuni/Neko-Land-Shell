// nekoland-lcdd — tiny detached daemon that drives the Kraken LCD so the
// content survives the settings app closing.
//
//   nekoland-lcdd <image-or-gif>   show a file (stills exit immediately,
//                                  animations keep streaming until replaced)
//   nekoland-lcdd --liquid         switch to the liquid-temperature screen
//   nekoland-lcdd --restore        reapply ~/.config/nekoland/lcd.conf
//                                  (for login autostart)
//
// A pidfile in $XDG_RUNTIME_DIR makes each new invocation replace the
// previous one, so the settings app just spawns it and forgets.

#include "kraken_lcd.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char *pidfile_path(void) {
    return g_build_filename(g_get_user_runtime_dir(), "nekoland-lcdd.pid",
                            NULL);
}

// kill any previous instance, then claim the pidfile
static void singleton_replace(void) {
    char *pf = pidfile_path();
    char *content = NULL;
    if (g_file_get_contents(pf, &content, NULL, NULL)) {
        int old = atoi(content);
        g_free(content);
        if (old > 0 && old != getpid()) {
            char *comm_path = g_strdup_printf("/proc/%d/comm", old);
            char *comm = NULL;
            g_file_get_contents(comm_path, &comm, NULL, NULL);
            g_free(comm_path);
            if (comm && g_str_has_prefix(comm, "nekoland-lcdd")) {
                kill(old, SIGTERM);
                for (int i = 0; i < 20 && kill(old, 0) == 0; i++)
                    g_usleep(50 * 1000);
            }
            g_free(comm);
        }
    }
    char pid[16];
    g_snprintf(pid, sizeof(pid), "%d\n", getpid());
    g_file_set_contents(pf, pid, -1, NULL);
    g_free(pf);
}

static char *conf_path(void) {
    return g_build_filename(g_get_user_config_dir(), "nekoland", "lcd.conf",
                            NULL);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: nekoland-lcdd <file>|--liquid|--restore\n");
        return 2;
    }
    setsid(); // detach from the launcher's session
    singleton_replace();

    const char *mode = "file";
    char *path = NULL;

    if (g_str_equal(argv[1], "--restore")) {
        char *cf = conf_path();
        GKeyFile *kf = g_key_file_new();
        if (g_key_file_load_from_file(kf, cf, 0, NULL)) {
            int b = g_key_file_get_integer(kf, "lcd", "brightness", NULL);
            if (b > 0)
                kraken_lcd_set_brightness(b);
            GError *e = NULL;
            int r = g_key_file_get_integer(kf, "lcd", "rotation", &e);
            if (!e)
                kraken_lcd_set_orientation(r);
            g_clear_error(&e);
            char *m = g_key_file_get_string(kf, "lcd", "mode", NULL);
            if (m && g_str_equal(m, "liquid"))
                mode = "liquid";
            else
                path = g_key_file_get_string(kf, "lcd", "path", NULL);
            g_free(m);
        }
        g_key_file_free(kf);
        g_free(cf);
        if (g_str_equal(mode, "file") && !path)
            return 0; // nothing saved yet
    } else if (g_str_equal(argv[1], "--liquid")) {
        mode = "liquid";
    } else {
        path = g_strdup(argv[1]);
    }

    if (g_str_equal(mode, "liquid"))
        return kraken_lcd_set_liquid() ? 0 : 1;

    GError *err = NULL;
    if (!kraken_lcd_anim_start(path, &err)) { // stills upload and return
        fprintf(stderr, "nekoland-lcdd: %s\n",
                err ? err->message : "upload failed");
        g_clear_error(&err);
        g_free(path);
        return 1;
    }
    g_free(path);
    if (!kraken_lcd_anim_active())
        return 0; // still image: done

    while (kraken_lcd_anim_active()) // stream until killed/replaced
        g_usleep(G_USEC_PER_SEC);
    return 0;
}
