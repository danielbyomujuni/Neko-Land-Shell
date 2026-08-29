// Native Gigabyte RGB Fusion 2 "Blackwell" GPU provider (RTX 50 series).
// Protocol from OpenRGB's GigabyteRGBFusion2BlackwellGPUController, verified
// live on the RTX 5090 Gaming OC (direct colour via reg 0x16 confirmed):
//   64-byte raw i2c packet to addr 0x75 on the card's NVIDIA i2c adapter:
//   [reg, 0x01, mode, speed(1-6), brightness(1-10), R, G, B, 0, zone, ncolors, …]
//   reg 0x16 = direct colour (mode 0x00), reg 0x12 = effects,
//   save = [0x13, 0x01, 0…]. Gaming layout has 4 zones.

#include "rgb.h"

#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define GPU_ADDR 0x75
// the Gaming layout expects a packet for hardware zones 0-5 (fewer visible
// zones, but effects only engage once every hardware zone is configured —
// verified live: 4-zone breathing stayed static, 6-zone breathes)
#define GPU_ZONES 6
#define REG_MODE 0x12
#define REG_COLOR 0x16

static const struct {
    const char *name;
    int value; // mode byte for REG_MODE; -1 = direct path, -2 = off
} gpu_modes[] = {
    {"Off", -2},          {"Static", -1},      {"Breathing", 0x02},
    {"Flashing", 0x03},   {"Color Cycle", 0x05}, {"Wave", 0x06},
    {"Gradient", 0x07},
};
#define GPU_N_MODES ((int)G_N_ELEMENTS(gpu_modes))

// id format: "/dev/i2c-N:0x75"
static int gpu_open(const char *id) {
    char *node = g_strndup(id, strchr(id, ':') - id);
    int fd = open(node, O_RDWR);
    g_free(node);
    if (fd >= 0 && ioctl(fd, I2C_SLAVE, GPU_ADDR) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// a uniform colour always travels in the header bytes with ncolors = 0;
// a non-zero ncolors means per-LED colours (starting at byte 12 on the
// Gaming layout) and makes the firmware ignore effect packets
static void gpu_pkt(int fd, guint8 reg, guint8 mode, guint8 speed,
                    guint8 bright, guint8 r, guint8 g, guint8 b,
                    guint8 zone) {
    guint8 p[64] = {0};
    p[0] = reg;
    p[1] = 0x01;
    p[2] = mode;
    p[3] = speed;
    p[4] = bright;
    p[5] = r;
    p[6] = g;
    p[7] = b;
    p[9] = zone;
    (void)!write(fd, p, sizeof(p));
}

static guint8 gpu_speed(int speed) { // 0-100 -> 1..6
    return (guint8)CLAMP(1 + speed * 5 / 100, 1, 6);
}

// ---- provider hooks ----

static GPtrArray *gpu_list(void) {
    GPtrArray *devs = g_ptr_array_new_with_free_func(rgb_device_free);
    GHashTable *seen_pci =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    GDir *dir = g_dir_open("/sys/bus/i2c/devices", 0, NULL);
    if (!dir) {
        g_hash_table_destroy(seen_pci);
        return devs;
    }
    const char *entry;
    while ((entry = g_dir_read_name(dir))) {
        if (!g_str_has_prefix(entry, "i2c-") || !g_ascii_isdigit(entry[4]))
            continue;
        char *name_path =
            g_strdup_printf("/sys/bus/i2c/devices/%s/name", entry);
        char *bus_name = NULL;
        g_file_get_contents(name_path, &bus_name, NULL, NULL);
        g_free(name_path);
        if (!bus_name || !strstr(bus_name, "NVIDIA i2c adapter")) {
            g_free(bus_name);
            continue;
        }
        // one controller per card: dedupe by the "at PCI" suffix
        const char *at = strstr(bus_name, " at ");
        char *pci = g_strdup(at ? g_strstrip((char *)at + 4) : bus_name);
        g_free(bus_name);
        if (g_hash_table_contains(seen_pci, pci)) {
            g_free(pci);
            continue;
        }

        char *node = g_strdup_printf("/dev/%s", entry);
        char *id = g_strdup_printf("%s:0x%02x", node, GPU_ADDR);
        int fd = gpu_open(id);
        gboolean present = FALSE;
        if (fd >= 0) {
            // SMBus quick-write ACK probe (the controller NACKs bare reads)
            struct i2c_smbus_ioctl_data args = {
                .read_write = I2C_SMBUS_WRITE,
                .command = 0,
                .size = I2C_SMBUS_QUICK,
                .data = NULL,
            };
            present = ioctl(fd, I2C_SMBUS, &args) >= 0;
            close(fd);
        }
        if (present) {
            g_hash_table_add(seen_pci, g_strdup(pci));
            RgbDevice *d = rgb_device_new(&rgb_gpu_provider, id,
                                          "Gigabyte GPU (RGB Fusion 2)",
                                          "GPU");
            d->modes = g_ptr_array_new_with_free_func(g_free);
            d->cur_mode = -1; // not readable
            for (int m = 0; m < GPU_N_MODES; m++)
                g_ptr_array_add(d->modes, g_strdup(gpu_modes[m].name));
            g_ptr_array_add(devs, d);
            g_ptr_array_add(rgb_claimed_locations, g_strdup(id));
        }
        g_free(id);
        g_free(node);
        g_free(pci);
    }
    g_dir_close(dir);
    g_hash_table_destroy(seen_pci);
    return devs;
}

static void gpu_set_color(RgbDevice *d, const GdkRGBA *c) {
    int fd = gpu_open(d->id);
    if (fd < 0)
        return;
    guint8 r = (guint8)(c->red * 255 + 0.5);
    guint8 g = (guint8)(c->green * 255 + 0.5);
    guint8 b = (guint8)(c->blue * 255 + 0.5);
    for (int z = 0; z < GPU_ZONES; z++)
        gpu_pkt(fd, REG_COLOR, 0x00 /* direct */, gpu_speed(d->speed), 0x0A,
                r, g, b, (guint8)z);
    close(fd);
}

static void gpu_set_mode(RgbDevice *d, const char *mode) {
    int value = -3;
    for (int m = 0; m < GPU_N_MODES; m++)
        if (g_str_equal(gpu_modes[m].name, mode))
            value = gpu_modes[m].value;
    if (value == -3)
        return;
    int fd = gpu_open(d->id);
    if (fd < 0)
        return;
    GdkRGBA eff = rgb_effective_color(d);
    guint8 r = (guint8)(eff.red * 255 + 0.5);
    guint8 g = (guint8)(eff.green * 255 + 0.5);
    guint8 b = (guint8)(eff.blue * 255 + 0.5);
    for (int z = 0; z < GPU_ZONES; z++) {
        if (value == -2) // off: direct black
            gpu_pkt(fd, REG_COLOR, 0x00, gpu_speed(d->speed), 0x0A, 0, 0, 0,
                    (guint8)z);
        else if (value == -1) // static: direct colour
            gpu_pkt(fd, REG_COLOR, 0x00, gpu_speed(d->speed), 0x0A, r, g, b,
                    (guint8)z);
        else // effects: colour in the header (ignored by cycling modes)
            gpu_pkt(fd, REG_MODE, (guint8)value, gpu_speed(d->speed), 0x0A,
                    r, g, b, (guint8)z);
    }
    close(fd);
}

const RgbProvider rgb_gpu_provider = {
    .name = "Gigabyte Fusion2 GPU",
    .list = gpu_list,
    .set_color = gpu_set_color,
    .set_mode = gpu_set_mode,
};
