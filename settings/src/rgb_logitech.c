// Native Logitech HID++ 2.0 provider (feature 0x8070 COLOR_LED_EFFECTS).
// Protocol from OpenRGB's LogitechProtocolCommon, verified live on the
// G502 HERO (wired): 20-byte long reports [0x11, dev_idx 0xFF, feat_idx,
// command, data[16]]. Feature lookup via root feature: [0x11, 0xFF, 0x00,
// 0x01, page_hi, page_lo] → index at reply[4]. SET_EFFECT = cmd 0x30, data:
// [zone, effect_slot, R, G, B, …speed/brightness per effect].
// Effect slots on the G502: 0 = off, 1 = fixed, 2 = breathing, 3 = cycle.

#include "rgb.h"

#include <fcntl.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

static const struct {
    int pid;
    const char *name;
    int zones;
} logi_models[] = {
    {0xC08B, "Logitech G502 HERO", 2},
};

static const struct {
    const char *name;
    int slot;
} logi_modes[] = {
    {"Off", 0},
    {"Static", 1},
    {"Breathing", 2},
    {"Spectrum Cycle", 3},
};
#define LOGI_N_MODES ((int)G_N_ELEMENTS(logi_modes))

static gboolean logi_xfer(int fd, guint8 *req20, guint8 *reply64) {
    if (write(fd, req20, 20) != 20)
        return FALSE;
    fd_set set;
    struct timeval tv = {0, 600000};
    FD_ZERO(&set);
    FD_SET(fd, &set);
    if (select(fd + 1, &set, NULL, NULL, &tv) <= 0)
        return FALSE;
    return read(fd, reply64, 64) > 0;
}

// id format: "/dev/hidrawN:featidx:zones"
static void logi_id_params(const char *id, int *feat, int *zones) {
    *feat = 0;
    *zones = 2;
    char **parts = g_strsplit(id, ":", -1);
    if (parts[0] && parts[1]) {
        *feat = atoi(parts[1]);
        if (parts[2])
            *zones = atoi(parts[2]);
    }
    g_strfreev(parts);
}

static int logi_open(const char *id) {
    char *node = g_strndup(id, strchr(id, ':') - id);
    int fd = open(node, O_RDWR | O_NONBLOCK);
    g_free(node);
    return fd;
}

static void logi_apply(RgbDevice *d, int slot, const GdkRGBA *c) {
    int feat, zones;
    logi_id_params(d->id, &feat, &zones);
    int fd = logi_open(d->id);
    if (fd < 0 || !feat)
        return;
    guint8 r = (guint8)(c->red * 255 + 0.5);
    guint8 g = (guint8)(c->green * 255 + 0.5);
    guint8 b = (guint8)(c->blue * 255 + 0.5);
    guint16 speed = (guint16)((100 - CLAMP(d->speed, 0, 100)) * 90 + 1000);
    for (int zone = 0; zone < zones; zone++) {
        guint8 req[20] = {0x11, 0xFF, (guint8)feat, 0x30};
        req[4] = (guint8)zone;
        req[5] = (guint8)slot;
        req[6] = r;
        req[7] = g;
        req[8] = b;
        if (slot == 2) { // breathing: speed at 5-6, brightness at 8... but
                         // 8 carries B; effect layout uses 9 for brightness
            req[9] = (guint8)(speed >> 8);
            req[10] = (guint8)(speed & 0xFF);
            req[12] = 0x64;
        } else if (slot == 3) { // cycle: speed at 11-12, brightness 13
            req[11] = (guint8)(speed >> 8);
            req[12] = (guint8)(speed & 0xFF);
            req[13] = 0x64;
        }
        guint8 reply[64];
        logi_xfer(fd, req, reply);
    }
    close(fd);
}

// ---- provider hooks ----

static GPtrArray *logi_list(void) {
    GPtrArray *devs = g_ptr_array_new_with_free_func(rgb_device_free);
    GHashTable *seen_pid = g_hash_table_new(g_direct_hash, g_direct_equal);

    GDir *dir = g_dir_open("/sys/class/hidraw", 0, NULL);
    if (!dir) {
        g_hash_table_destroy(seen_pid);
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

        int model = -1;
        for (gsize m = 0; m < G_N_ELEMENTS(logi_models); m++) {
            char *idstr = g_strdup_printf("HID_ID=0003:0000046D:%08X",
                                          logi_models[m].pid);
            if (strstr(uevent, idstr))
                model = (int)m;
            g_free(idstr);
            if (model >= 0)
                break;
        }
        g_free(uevent);
        if (model < 0)
            continue;

        char *node = g_strdup_printf("/dev/%s", entry);
        if (access(node, R_OK | W_OK) != 0) {
            g_free(node);
            continue;
        }
        // claim every interface so the CLI fallback dedupes regardless of
        // which node it opened
        g_ptr_array_add(rgb_claimed_locations, g_strdup(node));

        if (!g_hash_table_contains(seen_pid,
                                   GINT_TO_POINTER(logi_models[model].pid))) {
            // probe: only the HID++ interface answers the feature lookup
            int fd = open(node, O_RDWR | O_NONBLOCK);
            int feat = 0;
            if (fd >= 0) {
                guint8 req[20] = {0x11, 0xFF, 0x00, 0x01, 0x80, 0x70};
                guint8 reply[64] = {0};
                if (logi_xfer(fd, req, reply) && reply[0] == 0x11)
                    feat = reply[4];
                close(fd);
            }
            if (feat) {
                g_hash_table_add(seen_pid,
                                 GINT_TO_POINTER(logi_models[model].pid));
                char *id = g_strdup_printf("%s:%d:%d", node, feat,
                                           logi_models[model].zones);
                RgbDevice *d = rgb_device_new(&rgb_logitech_provider, id,
                                              logi_models[model].name,
                                              "Mouse");
                d->modes = g_ptr_array_new_with_free_func(g_free);
                d->cur_mode = -1;
                for (int m = 0; m < LOGI_N_MODES; m++)
                    g_ptr_array_add(d->modes, g_strdup(logi_modes[m].name));
                g_ptr_array_add(devs, d);
                g_free(id);
            }
        }
        g_free(node);
    }
    g_dir_close(dir);
    g_hash_table_destroy(seen_pid);
    return devs;
}

static void logi_set_color(RgbDevice *d, const GdkRGBA *c) {
    logi_apply(d, 1 /* fixed */, c);
}

static void logi_set_mode(RgbDevice *d, const char *mode) {
    for (int m = 0; m < LOGI_N_MODES; m++) {
        if (g_str_equal(logi_modes[m].name, mode)) {
            GdkRGBA eff = rgb_effective_color(d);
            logi_apply(d, logi_modes[m].slot, &eff);
            return;
        }
    }
}

const RgbProvider rgb_logitech_provider = {
    .name = "Logitech HID++",
    .list = logi_list,
    .set_color = logi_set_color,
    .set_mode = logi_set_mode,
};
