// Native ENE (Aura) DRAM provider — SMBus register protocol over /dev/i2c-*.
// Register map and access sequence from OpenRGB's ENESMBusController:
//   pointer:  i2c_smbus_write_word_data(0x00, byteswap16(reg))
//   read:     i2c_smbus_read_byte_data(0x81)
//   write:    i2c_smbus_write_byte_data(0x01, val)
// Colours are stored R,B,G. Requires rw access to the i2c device node
// (granted by OpenRGB's udev rules / the i2c group).

#include "rgb.h"

#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define ENE_REG_DEVICE_NAME 0x1000
#define ENE_REG_CONFIG_TABLE 0x1C00
#define ENE_CONFIG_LED_COUNT 0x02
#define ENE_REG_COLORS_EFFECT 0x8010    // first-gen chips (E8K4, LED-0116…)
#define ENE_REG_COLORS_EFFECT_V2 0x8160 // second-gen (E6K5: G.Skill DDR5 etc.)
#define ENE_REG_DIRECT 0x8020
#define ENE_REG_MODE 0x8021
#define ENE_REG_APPLY 0x80A0
#define ENE_APPLY_VAL 0x01

// probe only addresses ENE actually uses for DRAM; never 0x70/0x72/0x74/0x76
// (i2c mux territory) to avoid poking unrelated hardware
static const int ene_addresses[] = {0x71, 0x73, 0x67};

// value of ENE_REG_MODE for each entry of ene_modes[]
static const struct {
    const char *name;
    int value;
} ene_modes[] = {
    {"Off", 0},         {"Static", 1},   {"Breathing", 2},
    {"Flashing", 3},    {"Spectrum Cycle", 4}, {"Rainbow", 5},
};
#define ENE_N_MODES ((int)G_N_ELEMENTS(ene_modes))

// ---- raw smbus ----

static int smbus_access(int fd, char rw, guint8 cmd, int size,
                        union i2c_smbus_data *data) {
    struct i2c_smbus_ioctl_data args = {
        .read_write = rw, .command = cmd, .size = size, .data = data};
    return ioctl(fd, I2C_SMBUS, &args);
}

static int ene_reg_ptr(int fd, guint16 reg) {
    union i2c_smbus_data data = {.word = (guint16)(((reg << 8) & 0xFF00) |
                                                   ((reg >> 8) & 0x00FF))};
    return smbus_access(fd, I2C_SMBUS_WRITE, 0x00, I2C_SMBUS_WORD_DATA, &data);
}

static int ene_reg_read(int fd, guint16 reg) {
    if (ene_reg_ptr(fd, reg) < 0)
        return -1;
    union i2c_smbus_data data;
    if (smbus_access(fd, I2C_SMBUS_READ, 0x81, I2C_SMBUS_BYTE_DATA, &data) < 0)
        return -1;
    return data.byte;
}

static int ene_reg_write(int fd, guint16 reg, guint8 val) {
    if (ene_reg_ptr(fd, reg) < 0)
        return -1;
    union i2c_smbus_data data = {.byte = val};
    return smbus_access(fd, I2C_SMBUS_WRITE, 0x01, I2C_SMBUS_BYTE_DATA, &data);
}

// id format: "/dev/i2c-N:0xADDR:EFFECTBASEHEX:LEDCOUNT"
static int ene_open(const char *id) {
    char **parts = g_strsplit(id, ":", -1);
    int fd = -1;
    if (parts[0] && parts[1]) {
        int addr = (int)g_ascii_strtoll(parts[1], NULL, 16);
        fd = open(parts[0], O_RDWR);
        if (fd >= 0 && ioctl(fd, I2C_SLAVE, addr) < 0) {
            close(fd);
            fd = -1;
        }
    }
    g_strfreev(parts);
    return fd;
}

static void ene_id_params(const char *id, guint16 *effect_base,
                          int *led_count) {
    *effect_base = ENE_REG_COLORS_EFFECT;
    *led_count = 8;
    char **parts = g_strsplit(id, ":", -1);
    if (parts[0] && parts[1] && parts[2]) {
        *effect_base = (guint16)g_ascii_strtoll(parts[2], NULL, 16);
        if (parts[3])
            *led_count = CLAMP(atoi(parts[3]), 1, 32);
    }
    g_strfreev(parts);
    // HARD BOUND: the V1 colour bank ends at 0x801F — 0x8020 onward is the
    // control block (direct/mode/speed/…). Writing colours past it bricks
    // the stick's lighting until a power cycle (learned the hard way).
    if (*effect_base == ENE_REG_COLORS_EFFECT &&
        *led_count > (ENE_REG_DIRECT - ENE_REG_COLORS_EFFECT) / 3)
        *led_count = (ENE_REG_DIRECT - ENE_REG_COLORS_EFFECT) / 3;
}

// ---- provider hooks ----

static GPtrArray *ene_list(void) {
    GPtrArray *devs = g_ptr_array_new_with_free_func(rgb_device_free);

    GDir *dir = g_dir_open("/sys/bus/i2c/devices", 0, NULL);
    if (!dir)
        return devs;
    const char *entry;
    while ((entry = g_dir_read_name(dir))) {
        if (!g_str_has_prefix(entry, "i2c-") ||
            !g_ascii_isdigit(entry[4]))
            continue;
        char *name_path = g_strdup_printf("/sys/bus/i2c/devices/%s/name", entry);
        char *bus_name = NULL;
        g_file_get_contents(name_path, &bus_name, NULL, NULL);
        g_free(name_path);
        gboolean smbus = bus_name && strstr(bus_name, "SMBus");
        g_free(bus_name);
        if (!smbus)
            continue;

        char *node = g_strdup_printf("/dev/%s", entry);
        for (gsize a = 0; a < G_N_ELEMENTS(ene_addresses); a++) {
            char *probe_id =
                g_strdup_printf("%s:0x%02x", node, ene_addresses[a]);
            int fd = ene_open(probe_id);
            g_free(probe_id);
            if (fd < 0)
                continue;
            // validate: device name register must hold printable ASCII
            char devname[17] = {0};
            gboolean ok = TRUE;
            for (int i = 0; i < 16 && ok; i++) {
                int v = ene_reg_read(fd, ENE_REG_DEVICE_NAME + i);
                if (v < 0 || (i < 4 && !g_ascii_isprint(v)))
                    ok = FALSE;
                devname[i] = g_ascii_isprint(v) ? (char)v : '\0';
            }
            int mode = ok ? ene_reg_read(fd, ENE_REG_MODE) : -1;
            // second-gen chips (E6K5 family) keep colours in the V2 bank
            guint16 base = strstr(devname, "E6K5")
                               ? ENE_REG_COLORS_EFFECT_V2
                               : ENE_REG_COLORS_EFFECT;
            int leds = ok ? ene_reg_read(
                                fd, ENE_REG_CONFIG_TABLE + ENE_CONFIG_LED_COUNT)
                          : -1;
            if (leds < 1 || leds > 32)
                leds = 8;
            close(fd);
            if (!ok || devname[0] == '\0')
                continue;

            char *id = g_strdup_printf("%s:0x%02x:%04x:%d", node,
                                       ene_addresses[a], base, leds);
            char *label = g_strdup_printf("ENE DRAM (%s)", devname);
            RgbDevice *d = rgb_device_new(&rgb_ene_provider, id, label, "Memory");
            g_free(label);
            d->n_leds = leds; // per-LED painting supported
            d->modes = g_ptr_array_new_with_free_func(g_free);
            d->cur_mode = -1;
            for (int m = 0; m < ENE_N_MODES; m++) {
                g_ptr_array_add(d->modes, g_strdup(ene_modes[m].name));
                if (mode == ene_modes[m].value)
                    d->cur_mode = m;
            }
            g_ptr_array_add(devs, d);
            // claim as "node:addr" so the CLI fallback's location match works
            g_ptr_array_add(rgb_claimed_locations,
                            g_strdup_printf("%s:0x%02x", node,
                                            ene_addresses[a]));
            g_free(id);
        }
        g_free(node);
    }
    g_dir_close(dir);
    return devs;
}

static void ene_set_color(RgbDevice *d, const GdkRGBA *c) {
    int fd = ene_open(d->id);
    if (fd < 0)
        return;
    guint16 effect_base;
    int leds;
    ene_id_params(d->id, &effect_base, &leds);
    guint8 r = (guint8)(c->red * 255 + 0.5);
    guint8 g = (guint8)(c->green * 255 + 0.5);
    guint8 b = (guint8)(c->blue * 255 + 0.5);
    for (int led = 0; led < leds; led++) {
        guint16 base = effect_base + led * 3;
        ene_reg_write(fd, base + 0, r); // R,B,G order
        ene_reg_write(fd, base + 1, b);
        ene_reg_write(fd, base + 2, g);
    }
    ene_reg_write(fd, ENE_REG_DIRECT, 0x00);
    ene_reg_write(fd, ENE_REG_MODE, 1 /* static */);
    ene_reg_write(fd, ENE_REG_APPLY, ENE_APPLY_VAL);
    close(fd);
}

static void ene_set_mode(RgbDevice *d, const char *mode) {
    int value = -1;
    for (int m = 0; m < ENE_N_MODES; m++)
        if (g_str_equal(ene_modes[m].name, mode))
            value = ene_modes[m].value;
    if (value < 0)
        return;
    int fd = ene_open(d->id);
    if (fd < 0)
        return;
    if (value == 1 || value == 2 || value == 3) {
        // static/breathing/flashing show a colour
        guint16 effect_base;
        int leds;
        ene_id_params(d->id, &effect_base, &leds);
        GdkRGBA eff = rgb_effective_color(d);
        guint8 r = (guint8)(eff.red * 255 + 0.5);
        guint8 g = (guint8)(eff.green * 255 + 0.5);
        guint8 b = (guint8)(eff.blue * 255 + 0.5);
        for (int led = 0; led < leds; led++) {
            guint16 base = effect_base + led * 3;
            ene_reg_write(fd, base + 0, r);
            ene_reg_write(fd, base + 1, b);
            ene_reg_write(fd, base + 2, g);
        }
    }
    ene_reg_write(fd, ENE_REG_DIRECT, 0x00);
    ene_reg_write(fd, ENE_REG_MODE, (guint8)value);
    ene_reg_write(fd, ENE_REG_APPLY, ENE_APPLY_VAL);
    close(fd);
}

// paint one LED: touches only that LED's three colour registers, then makes
// sure the effect engine shows the bank (static + apply)
static void ene_set_led(RgbDevice *d, int led, const GdkRGBA *c) {
    guint16 effect_base;
    int leds;
    ene_id_params(d->id, &effect_base, &leds);
    if (led < 0 || led >= leds)
        return;
    int fd = ene_open(d->id);
    if (fd < 0)
        return;
    guint16 base = effect_base + led * 3;
    ene_reg_write(fd, base + 0, (guint8)(c->red * 255 + 0.5));
    ene_reg_write(fd, base + 1, (guint8)(c->blue * 255 + 0.5)); // R,B,G
    ene_reg_write(fd, base + 2, (guint8)(c->green * 255 + 0.5));
    ene_reg_write(fd, ENE_REG_DIRECT, 0x00);
    ene_reg_write(fd, ENE_REG_MODE, 1 /* static */);
    ene_reg_write(fd, ENE_REG_APPLY, ENE_APPLY_VAL);
    close(fd);
}

const RgbProvider rgb_ene_provider = {
    .name = "ENE SMBus",
    .list = ene_list,
    .set_color = ene_set_color,
    .set_mode = ene_set_mode,
    .set_led = ene_set_led,
};
