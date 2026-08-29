// Native ASRock Polychrome USB provider (motherboard lighting).
// Protocol from OpenRGB's PolychromeUSBController, verified live on the
// X870E Taichi: 65-byte HID reports (leading 0x00 report id), command at [1]:
//   SET_ZONE 0x10: [3]=zone 0-7, [4]=mode, [5..7]=RGB, [8]=speed
//   (0xFF slowest .. 0x00 fastest, 0xE0 default), [9]=0xFF.
//   READ_HEADER 0x14 with [3]=0x02 returns per-zone LED counts at [4..11].
// Zones: RGB1, RGB2, ARGB1, ARGB2, PCH, IO cover, PCB, Audio.

#include "rgb.h"

#include <fcntl.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#define ASROCK_ZONES 8

static const struct {
    const char *name;
    int value;
} asrock_modes[] = {
    {"Off", 0x00},      {"Static", 0x01},        {"Breathing", 0x02},
    {"Strobe", 0x03},   {"Spectrum Cycle", 0x04}, {"Wave", 0x07},
    {"Rainbow", 0x0E},
};
#define ASROCK_N_MODES ((int)G_N_ELEMENTS(asrock_modes))

static void drain(int fd) {
    fd_set set;
    struct timeval tv = {0, 300000};
    FD_ZERO(&set);
    FD_SET(fd, &set);
    if (select(fd + 1, &set, NULL, NULL, &tv) > 0) {
        guint8 buf[64];
        (void)!read(fd, buf, sizeof(buf));
    }
}

static void set_zone(int fd, int zone, guint8 mode, guint8 r, guint8 g,
                     guint8 b, guint8 speed) {
    guint8 buf[65] = {0};
    buf[1] = 0x10; // SET_ZONE
    buf[3] = (guint8)zone;
    buf[4] = mode;
    buf[5] = r;
    buf[6] = g;
    buf[7] = b;
    buf[8] = speed;
    buf[9] = 0xFF;
    if (write(fd, buf, sizeof(buf)) > 0)
        drain(fd); // device answers every command
}

static guint8 asrock_speed(int speed) { // 0-100 -> 0xFF slow .. 0x00 fast
    return (guint8)(0xFF - CLAMP(speed, 0, 100) * 0xFF / 100);
}

// ---- provider hooks ----

static GPtrArray *asrock_list(void) {
    GPtrArray *devs = g_ptr_array_new_with_free_func(rgb_device_free);
    GDir *dir = g_dir_open("/sys/class/hidraw", 0, NULL);
    if (!dir)
        return devs;
    const char *entry;
    while ((entry = g_dir_read_name(dir))) {
        char *uevent_path =
            g_strdup_printf("/sys/class/hidraw/%s/device/uevent", entry);
        char *uevent = NULL;
        g_file_get_contents(uevent_path, &uevent, NULL, NULL);
        g_free(uevent_path);
        if (!uevent)
            continue;
        // match by VID and the exact controller name (the vendor id also
        // appears on unrelated ASRock USB audio HID endpoints)
        gboolean match = strstr(uevent, "000026CE") &&
                         strstr(uevent, "HID_NAME=ASRock LED Controller");
        g_free(uevent);
        if (!match)
            continue;

        char *node = g_strdup_printf("/dev/%s", entry);
        if (access(node, R_OK | W_OK) != 0) {
            g_free(node);
            continue;
        }
        RgbDevice *d = rgb_device_new(&rgb_asrock_provider, node,
                                      "ASRock Polychrome (Motherboard)",
                                      "Motherboard");
        d->modes = g_ptr_array_new_with_free_func(g_free);
        d->cur_mode = -1; // per-zone on the wire; not summarised
        for (int m = 0; m < ASROCK_N_MODES; m++)
            g_ptr_array_add(d->modes, g_strdup(asrock_modes[m].name));
        g_ptr_array_add(devs, d);
        g_ptr_array_add(rgb_claimed_locations, g_strdup(node));
        g_free(node);
    }
    g_dir_close(dir);
    return devs;
}

static void asrock_apply(RgbDevice *d, int mode_value, const GdkRGBA *c) {
    int fd = open(d->id, O_RDWR | O_NONBLOCK);
    if (fd < 0)
        return;
    guint8 r = (guint8)(c->red * 255 + 0.5);
    guint8 g = (guint8)(c->green * 255 + 0.5);
    guint8 b = (guint8)(c->blue * 255 + 0.5);
    for (int z = 0; z < ASROCK_ZONES; z++)
        set_zone(fd, z, (guint8)mode_value, r, g, b, asrock_speed(d->speed));
    close(fd);
}

static void asrock_set_color(RgbDevice *d, const GdkRGBA *c) {
    asrock_apply(d, 0x01 /* static */, c);
}

static void asrock_set_mode(RgbDevice *d, const char *mode) {
    int value = -1;
    for (int m = 0; m < ASROCK_N_MODES; m++)
        if (g_str_equal(asrock_modes[m].name, mode))
            value = asrock_modes[m].value;
    if (value < 0)
        return;
    GdkRGBA eff = rgb_effective_color(d);
    asrock_apply(d, value, &eff);
}

const RgbProvider rgb_asrock_provider = {
    .name = "ASRock Polychrome",
    .list = asrock_list,
    .set_color = asrock_set_color,
    .set_mode = asrock_set_mode,
};
