// Native Razer provider — 91-byte feature reports (extended matrix devices).
// Protocol from OpenRGB's RazerController, verified live on the BlackWidow
// V4 X: report = [id 0x00, status, transaction 0x1F, remaining(2), proto,
// data_size, class, cmd, args[80], crc, 0], crc = XOR of bytes 3..88.
// Extended matrix effects: class 0x0F cmd 0x02, args
// {varstore 0x01, led 0x00, effect, ...}: off 0x00, static 0x01 (+0x01 count
// + RGB at 6..8), breathing 0x02, spectrum 0x03.
// Older (non-extended-matrix) Razer devices are not handled here.

#include "rgb.h"

#include <fcntl.h>
#include <linux/hidraw.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static const struct {
    const char *name;
    int effect; // extended matrix effect byte
} razer_modes[] = {
    {"Off", 0x00},
    {"Static", 0x01},
    {"Breathing", 0x02},
    {"Spectrum Cycle", 0x03},
};
#define RAZER_N_MODES ((int)G_N_ELEMENTS(razer_modes))

static void razer_send(int fd, guint8 cls, guint8 cmd, const guint8 *args,
                       gsize n_args) {
    guint8 buf[91] = {0};
    buf[0] = 0x00; // report id
    buf[2] = 0x1F; // transaction id
    buf[6] = (guint8)n_args;
    buf[7] = cls;
    buf[8] = cmd;
    memcpy(&buf[9], args, n_args);
    guint8 crc = 0;
    for (int i = 3; i <= 88; i++)
        crc ^= buf[i];
    buf[89] = crc;
    ioctl(fd, HIDIOCSFEATURE(sizeof(buf)), buf);
}

// d->id is a comma-separated list of the device's interface nodes; only one
// of them reaches the RGB firmware and it is not predictable, so the report
// goes to all of them (extra interfaces accept and ignore it)
static void razer_apply(RgbDevice *d, int effect, const GdkRGBA *c) {
    guint8 r = (guint8)(c->red * 255 + 0.5);
    guint8 g = (guint8)(c->green * 255 + 0.5);
    guint8 b = (guint8)(c->blue * 255 + 0.5);

    char **nodes = g_strsplit(d->id, ",", -1);
    for (char **n = nodes; *n; n++) {
        int fd = open(*n, O_RDWR);
        if (fd < 0)
            continue;
        switch (effect) {
        case 0x00: { // off
            guint8 args[6] = {0x01, 0x00, 0x00};
            razer_send(fd, 0x0F, 0x02, args, sizeof(args));
            break;
        }
        case 0x01: { // static
            guint8 args[9] = {0x01, 0x00, 0x01, 0x00, 0x00, 0x01, r, g, b};
            razer_send(fd, 0x0F, 0x02, args, sizeof(args));
            break;
        }
        case 0x02: { // breathing, one colour
            guint8 args[9] = {0x01, 0x00, 0x02, 0x01, 0x00, 0x01, r, g, b};
            razer_send(fd, 0x0F, 0x02, args, sizeof(args));
            break;
        }
        case 0x03: { // spectrum cycle
            guint8 args[6] = {0x01, 0x00, 0x03};
            razer_send(fd, 0x0F, 0x02, args, sizeof(args));
            break;
        }
        }
        close(fd);
    }
    g_strfreev(nodes);
}

// ---- provider hooks ----

typedef struct {
    GString *nodes; // comma-separated interface nodes
    char label[128];
} RazerFound;

static void razer_found_free(gpointer p) {
    RazerFound *f = p;
    g_string_free(f->nodes, TRUE);
    g_free(f);
}

static GPtrArray *razer_list(void) {
    GPtrArray *devs = g_ptr_array_new_with_free_func(rgb_device_free);
    // pid -> RazerFound (collect every interface node of each device)
    GHashTable *found = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                              razer_found_free);

    GDir *dir = g_dir_open("/sys/class/hidraw", 0, NULL);
    if (!dir) {
        g_hash_table_destroy(found);
        return devs;
    }
    const char *entry;
    while ((entry = g_dir_read_name(dir))) {
        char *uevent_path =
            g_strdup_printf("/sys/class/hidraw/%s/device/uevent", entry);
        char *uevent = NULL;
        g_file_get_contents(uevent_path, &uevent, NULL, NULL);
        g_free(uevent_path);
        if (!uevent)
            continue;
        char *idline = strstr(uevent, "HID_ID=0003:00001532:");
        if (!idline) {
            g_free(uevent);
            continue;
        }
        char pid[9] = {0};
        g_strlcpy(pid, idline + 21, sizeof(pid));

        char *node = g_strdup_printf("/dev/%s", entry);
        if (access(node, R_OK | W_OK) != 0) {
            g_free(node);
            g_free(uevent);
            continue;
        }
        // every interface node of the device must be claimed so the CLI
        // fallback (which may have opened a different one) gets deduped
        g_ptr_array_add(rgb_claimed_locations, g_strdup(node));

        RazerFound *f = g_hash_table_lookup(found, pid);
        if (!f) {
            f = g_new0(RazerFound, 1);
            f->nodes = g_string_new(NULL);
            g_strlcpy(f->label, "Razer Device", sizeof(f->label));
            char *name = strstr(uevent, "HID_NAME=");
            if (name) {
                name += 9;
                char *nl = strchr(name, '\n');
                gsize len = nl ? (gsize)(nl - name) + 1 : sizeof(f->label);
                g_strlcpy(f->label, name, MIN(sizeof(f->label), len));
                // uevent names double the vendor: "Razer Razer BlackWidow…"
                if (g_str_has_prefix(f->label, "Razer Razer "))
                    memmove(f->label, f->label + 6, strlen(f->label + 6) + 1);
            }
            g_hash_table_insert(found, g_strdup(pid), f);
        }
        if (f->nodes->len)
            g_string_append_c(f->nodes, ',');
        g_string_append(f->nodes, node);
        g_free(node);
        g_free(uevent);
    }
    g_dir_close(dir);

    GHashTableIter it;
    gpointer key, val;
    g_hash_table_iter_init(&it, found);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        RazerFound *f = val;
        RgbDevice *d = rgb_device_new(&rgb_razer_provider, f->nodes->str,
                                      f->label, "Keyboard");
        d->has_speed = FALSE; // extended matrix has no speed parameter
        d->modes = g_ptr_array_new_with_free_func(g_free);
        d->cur_mode = -1;
        for (int m = 0; m < RAZER_N_MODES; m++)
            g_ptr_array_add(d->modes, g_strdup(razer_modes[m].name));
        g_ptr_array_add(devs, d);
    }
    g_hash_table_destroy(found);
    return devs;
}

static void razer_set_color(RgbDevice *d, const GdkRGBA *c) {
    razer_apply(d, 0x01, c);
}

static void razer_set_mode(RgbDevice *d, const char *mode) {
    for (int m = 0; m < RAZER_N_MODES; m++) {
        if (g_str_equal(razer_modes[m].name, mode)) {
            GdkRGBA eff = rgb_effective_color(d);
            razer_apply(d, razer_modes[m].effect, &eff);
            return;
        }
    }
}

const RgbProvider rgb_razer_provider = {
    .name = "Razer",
    .list = razer_list,
    .set_color = razer_set_color,
    .set_mode = razer_set_mode,
};
