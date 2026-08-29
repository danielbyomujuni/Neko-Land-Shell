// Optional OpenRGB CLI fallback provider.
//
// Only used when the `openrgb` binary is installed, and only for devices no
// native provider has claimed (it skips devices whose Location matches an
// entry in rgb_claimed_locations). Covers hardware we have no native
// implementation for yet (GPU, mice, keyboards, motherboards, ...). The app
// works without it — native devices simply remain the only ones listed.

#include "rgb.h"

#include <string.h>

// concurrent openrgb processes deadlock each other on the hardware, so
// every invocation is serialized through flock and bounded by timeout
#define CLI_WRAP "flock", "-w", "60", "/tmp/nekoland-openrgb.lock", \
                 "timeout", "30"

static void cli_run(char **argv) {
    g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL,
                  NULL);
}

static void cli_set_color(RgbDevice *d, const GdkRGBA *c) {
    char hex[8];
    g_snprintf(hex, sizeof(hex), "%02X%02X%02X", (int)(c->red * 255 + 0.5),
               (int)(c->green * 255 + 0.5), (int)(c->blue * 255 + 0.5));
    char *argv[] = {CLI_WRAP, "openrgb", "-d", d->id, "-m", "static",
                    "-c", hex, NULL};
    cli_run(argv);
}

static void cli_set_mode(RgbDevice *d, const char *mode) {
    char *argv[] = {CLI_WRAP, "openrgb", "-d", d->id, "-m", (char *)mode,
                    NULL};
    cli_run(argv);
}

static void parse_modes(const char *s, RgbDevice *d) {
    d->modes = g_ptr_array_new_with_free_func(g_free);
    d->cur_mode = -1;
    while (*s) {
        while (*s == ' ')
            s++;
        if (!*s)
            break;
        gboolean current = FALSE;
        if (*s == '[') {
            current = TRUE;
            s++;
        }
        char *tok;
        if (*s == '\'') {
            const char *end = strchr(s + 1, '\'');
            if (!end)
                break;
            tok = g_strndup(s + 1, end - s - 1);
            s = end + 1;
        } else {
            const char *end = s;
            while (*end && *end != ' ')
                end++;
            tok = g_strndup(s, end - s);
            s = end;
        }
        if (current && tok[strlen(tok) - 1] == ']')
            tok[strlen(tok) - 1] = '\0';
        if (*s == ']')
            s++;
        if (*tok) {
            if (current)
                d->cur_mode = d->modes->len;
            g_ptr_array_add(d->modes, tok);
        } else {
            g_free(tok);
        }
    }
}

static gboolean location_claimed(const char *location) {
    if (!location)
        return FALSE;
    for (guint i = 0; i < rgb_claimed_locations->len; i++) {
        const char *claim = g_ptr_array_index(rgb_claimed_locations, i);
        // claims look like "/dev/i2c-14:0x71" or "/dev/hidraw18"; CLI
        // locations like "I2C: /dev/i2c-14, address 0x71" or "HID: /dev/..."
        char **parts = g_strsplit(claim, ":", 2);
        gboolean node_match = strstr(location, parts[0]) != NULL;
        gboolean addr_match = !parts[1] || strstr(location, parts[1]) != NULL;
        g_strfreev(parts);
        if (node_match && addr_match)
            return TRUE;
    }
    return FALSE;
}

static GPtrArray *cli_list(void) {
    GPtrArray *devs = g_ptr_array_new_with_free_func(rgb_device_free);

    char *bin = g_find_program_in_path("openrgb");
    if (!bin)
        return devs;
    g_free(bin);

    char *out = NULL;
    char *argv[] = {CLI_WRAP, "openrgb", "--list-devices", NULL};
    if (!g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, &out,
                      NULL, NULL, NULL) ||
        !out)
        return devs;

    RgbDevice *d = NULL;
    char **lines = g_strsplit(out, "\n", -1);
    for (char **l = lines; *l; l++) {
        char *line = *l;
        if (g_ascii_isdigit(line[0]) && strstr(line, ": ")) {
            char *colon = strstr(line, ": ");
            char *idx = g_strndup(line, colon - line);
            d = rgb_device_new(&rgb_openrgb_provider, idx, colon + 2, NULL);
            g_free(idx);
            g_ptr_array_add(devs, d);
        } else if (d && g_str_has_prefix(line, "  Type:")) {
            d->type = g_strdup(g_strstrip(line + 7));
        } else if (d && g_str_has_prefix(line, "  Location:")) {
            if (location_claimed(line + 11)) {
                g_ptr_array_remove(devs, d); // natively handled
                d = NULL;
            }
        } else if (d && g_str_has_prefix(line, "  Modes: ")) {
            parse_modes(line + 9, d);
        }
    }
    g_strfreev(lines);
    g_free(out);
    return devs;
}

const RgbProvider rgb_openrgb_provider = {
    .name = "OpenRGB CLI",
    .list = cli_list,
    .set_color = cli_set_color,
    .set_mode = cli_set_mode,
};
