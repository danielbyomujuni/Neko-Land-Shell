// Native NZXT provider — HID reports over /dev/hidraw*.
//
// Two protocols, selected per PID:
//  - PROTO_HUE2: legacy Hue 2 effect packets (0x28 0x03 …), pre-2023 units
//  - PROTO_CHUB: Control Hub (RGB & Fan Controller 2024, PID 0x2022):
//      fixed:  [0x26 0x04 chb 0x00] + GRB×24  (76 bytes, no padding!)
//      commit: [0x26 0x06 chb 0x00 0x01 0 0 0x18 0 0 0x80 0 0x32 0 0 0x01 0 0]
//      anim:   [0x2A 0x04 chb chb mode speedlo speedhi …] per liquidctl
//    (packet formats from liquidctl's control_hub.py, verified live on this
//     hardware: channels light, GRB order confirmed with pure red)
//
// The Kraken Plus V2 (PID 0x3014) drives its RGB radiator fans with the same
// Control Hub packets on a single channel; its LCD is a separate feature.

#include "rgb.h"

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#define NZXT_VID 0x1E71

typedef enum { PROTO_HUE2 = 0, PROTO_CHUB = 1 } NzxtProto;

static const struct {
    int pid;
    const char *name;
    const char *type; // chip group in the UI
    int channels;
    NzxtProto proto;
} nzxt_models[] = {
    {0x2022, "NZXT RGB & Fan Controller 2024", "Case fans", 5, PROTO_CHUB},
    // the Kraken Plus V2 hosts RGB radiator fans on one channel and accepts
    // the same Control Hub LED packets (verified live); its LCD is separate
    {0x3014, "NZXT Kraken Plus V2", "AIO cooler", 1, PROTO_CHUB},
    {0x2011, "NZXT RGB & Fan Controller", "Case fans", 3, PROTO_HUE2},
    {0x2019, "NZXT RGB & Fan Controller", "Case fans", 3, PROTO_HUE2},
    {0x2009, "NZXT RGB & Fan Controller", "Case fans", 3, PROTO_HUE2},
    {0x200E, "NZXT RGB & Fan Controller", "Case fans", 3, PROTO_HUE2},
    {0x2001, "NZXT Hue 2", "Case fans", 4, PROTO_HUE2},
    {0x2002, "NZXT Hue 2 Ambient", "Case fans", 2, PROTO_HUE2},
};

// id format: "/dev/hidrawN:channels:proto"
static void nzxt_id_params(const char *id, int *channels, NzxtProto *proto) {
    *channels = 0;
    *proto = PROTO_HUE2;
    char **parts = g_strsplit(id, ":", -1);
    if (parts[0] && parts[1]) {
        *channels = atoi(parts[1]);
        if (parts[2])
            *proto = (NzxtProto)atoi(parts[2]);
    }
    g_strfreev(parts);
}

static int nzxt_open(const char *id) {
    char *node = g_strndup(id, strchr(id, ':') - id);
    int fd = open(node, O_RDWR);
    g_free(node);
    return fd;
}

static void hid_write_pad(int fd, const guint8 *data, gsize len) {
    if (len >= 64) {
        (void)!write(fd, data, len);
        return;
    }
    guint8 buf[64] = {0};
    memcpy(buf, data, len);
    (void)!write(fd, buf, sizeof(buf));
}

// ---- legacy Hue 2 ----

static void hue2_send_effect(int fd, int channel, int mode, const GdkRGBA *c,
                             int speed) {
    guint8 buf[64] = {0};
    buf[0x00] = 0x28;
    buf[0x01] = 0x03;
    buf[0x02] = (guint8)(1 << channel);
    buf[0x04] = (guint8)mode;
    buf[0x05] = (guint8)CLAMP(speed / 25, 0, 4);
    buf[0x06] = 0x01;
    buf[0x07] = 0x00;
    if (c) {
        buf[0x08] = 0x01;
        buf[0x0A] = (guint8)(c->green * 255 + 0.5);
        buf[0x0B] = (guint8)(c->red * 255 + 0.5);
        buf[0x0C] = (guint8)(c->blue * 255 + 0.5);
    }
    if (write(fd, buf, sizeof(buf)) < 0)
        return;
    guint8 apply[64] = {0x22, 0xA0, (guint8)(1 << channel), 0};
    (void)!write(fd, apply, sizeof(apply));
}

// ---- Control Hub ----

// channel byte is a BITMASK (ch1=0x01 … ch5=0x10) — verified live on this
// hub; liquidctl's {0x02,0x04,0x06,0x08,0x10} map never addresses channel 1
// (its 0x06 is really "ch2|ch3")
static const guint8 chub_channel_byte[] = {0x01, 0x02, 0x04, 0x08, 0x10};

static void chub_speed_bytes(int speed, gboolean fading, guint8 *lo,
                             guint8 *hi) {
    static const guint8 std[5][2] = {
        {0x5E, 0x01}, {0x2C, 0x01}, {0xFA, 0x00}, {0x96, 0x00}, {0x50, 0x00}};
    static const guint8 fad[5][2] = {
        {0x50, 0x00}, {0x3C, 0x00}, {0x28, 0x00}, {0x14, 0x00}, {0x0A, 0x00}};
    int i = CLAMP(speed / 20, 0, 4);
    *lo = fading ? fad[i][0] : std[i][0];
    *hi = fading ? fad[i][1] : std[i][1];
}

static void chub_fixed(int fd, int channel, const GdkRGBA *c) {
    guint8 chb = chub_channel_byte[channel];
    guint8 pkt[76] = {0x26, 0x04, chb, 0x00};
    guint8 g = (guint8)(c->green * 255 + 0.5);
    guint8 r = (guint8)(c->red * 255 + 0.5);
    guint8 b = (guint8)(c->blue * 255 + 0.5);
    for (int led = 0; led < 24; led++) {
        pkt[4 + led * 3 + 0] = g;
        pkt[4 + led * 3 + 1] = r;
        pkt[4 + led * 3 + 2] = b;
    }
    (void)!write(fd, pkt, sizeof(pkt)); // 76 bytes, deliberately unpadded
    static const guint8 commit[18] = {0x26, 0x06, 0,    0x00, 0x01, 0x00,
                                      0x00, 0x18, 0x00, 0x00, 0x80, 0x00,
                                      0x32, 0x00, 0x00, 0x01, 0x00, 0x00};
    guint8 ap[18];
    memcpy(ap, commit, sizeof(ap));
    ap[2] = chb;
    hid_write_pad(fd, ap, sizeof(ap));
}

// animated packet per liquidctl's _build_mode_packet
static void chub_animated(int fd, int channel, int mode, const GdkRGBA *c,
                          int speed) {
    guint8 chb = chub_channel_byte[channel];
    guint8 pkt[64] = {0x2A, 0x04, chb, chb, (guint8)mode};
    gsize n = 5;
    guint8 lo, hi;
    chub_speed_bytes(speed, mode == 0x01, &lo, &hi);
    pkt[n++] = lo;
    pkt[n++] = hi;
    (void)c;
    if (mode == 0x0C) { // super-rainbow: header padding
        pkt[n++] = 0x00;
        pkt[n++] = 0x00;
    }
    while (n < 56)
        pkt[n++] = 0x00;
    if (mode == 0x02 || mode == 0x0C) // direction byte
        pkt[n++] = 0x00;              // forward
    static const guint8 foot_wave[] = {0x00, 0x00, 0x12, 0x03, 0x00, 0x00};
    static const guint8 foot_rainbow[] = {0x00, 0x18, 0x03, 0x00, 0x00};
    const guint8 *foot = mode == 0x02 ? foot_wave : foot_rainbow;
    gsize foot_len = mode == 0x02 ? sizeof(foot_wave) : sizeof(foot_rainbow);
    for (gsize i = 0; i < foot_len && n < sizeof(pkt); i++)
        pkt[n++] = foot[i];
    (void)!write(fd, pkt, sizeof(pkt));
}

// fading uses liquidctl's Nzxt2023RgbController packet shape, not the
// control_hub one (0x18 at byte 58 kills the effect on this firmware):
// [0x2A 0x04 chb chb 0x01 speedlo speedhi GRB…] with direction@55,
// colour count@56, then 0x08 0x08 0x03. Verified live: the hub fades a
// single colour on its own; the Kraken shows a single colour as solid and
// needs an explicit second colour (black) to fade to.
static void chub_fading(int fd, int channel, const GdkRGBA *c, int speed,
                        gboolean add_black) {
    guint8 chb = chub_channel_byte[channel];
    guint8 pkt[64] = {0x2A, 0x04, chb, chb, 0x01};
    guint8 lo, hi;
    chub_speed_bytes(speed, TRUE, &lo, &hi);
    pkt[5] = lo;
    pkt[6] = hi;
    pkt[7] = (guint8)(c->green * 255 + 0.5); // GRB
    pkt[8] = (guint8)(c->red * 255 + 0.5);
    pkt[9] = (guint8)(c->blue * 255 + 0.5);
    pkt[55] = 0x00; // direction: forward
    pkt[56] = add_black ? 0x02 : 0x01;
    pkt[57] = 0x08;
    pkt[58] = 0x08;
    pkt[59] = 0x03;
    (void)!write(fd, pkt, sizeof(pkt));
}

// ---- provider hooks ----

static GPtrArray *nzxt_list(void) {
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

        int model = -1;
        for (gsize m = 0; m < G_N_ELEMENTS(nzxt_models); m++) {
            char *idstr = g_strdup_printf("HID_ID=0003:%08X:%08X", NZXT_VID,
                                          nzxt_models[m].pid);
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
        char *id = g_strdup_printf("%s:%d:%d", node,
                                   nzxt_models[model].channels,
                                   (int)nzxt_models[model].proto);
        RgbDevice *d = rgb_device_new(&rgb_hue2_provider, id,
                                      nzxt_models[model].name,
                                      nzxt_models[model].type);
        d->modes = g_ptr_array_new_with_free_func(g_free);
        d->cur_mode = -1; // not readable
        g_ptr_array_add(d->modes, g_strdup("Off"));
        g_ptr_array_add(d->modes, g_strdup("Static"));
        g_ptr_array_add(d->modes, g_strdup("Fading"));
        g_ptr_array_add(d->modes, g_strdup("Spectrum Cycle"));
        if (nzxt_models[model].proto == PROTO_CHUB)
            g_ptr_array_add(d->modes, g_strdup("Super Rainbow"));
        else
            g_ptr_array_add(d->modes, g_strdup("Breathing"));
        g_ptr_array_add(devs, d);
        g_ptr_array_add(rgb_claimed_locations, g_strdup(node));
        g_free(id);
        g_free(node);
    }
    g_dir_close(dir);
    return devs;
}

static void nzxt_apply(RgbDevice *d, const char *mode, const GdkRGBA *color) {
    int channels;
    NzxtProto proto;
    nzxt_id_params(d->id, &channels, &proto);
    int fd = nzxt_open(d->id);
    if (fd < 0)
        return;
    GdkRGBA black = {0, 0, 0, 1};

    for (int ch = 0; ch < channels; ch++) {
        if (proto == PROTO_CHUB) {
            if (!g_ascii_strcasecmp(mode, "Off"))
                chub_fixed(fd, ch, &black);
            else if (!g_ascii_strcasecmp(mode, "Static"))
                chub_fixed(fd, ch, color);
            else if (!g_ascii_strcasecmp(mode, "Fading"))
                // single-channel CHUB = the Kraken, which needs the
                // explicit fade-to-black second colour
                chub_fading(fd, ch, color, d->speed, channels == 1);
            else if (!g_ascii_strcasecmp(mode, "Spectrum Cycle"))
                chub_animated(fd, ch, 0x02, NULL, d->speed);
            else if (!g_ascii_strcasecmp(mode, "Super Rainbow"))
                chub_animated(fd, ch, 0x0C, NULL, d->speed);
        } else {
            if (!g_ascii_strcasecmp(mode, "Off"))
                hue2_send_effect(fd, ch, 0x00, &black, d->speed);
            else if (!g_ascii_strcasecmp(mode, "Static"))
                hue2_send_effect(fd, ch, 0x00, color, d->speed);
            else if (!g_ascii_strcasecmp(mode, "Fading"))
                hue2_send_effect(fd, ch, 0x01, color, d->speed);
            else if (!g_ascii_strcasecmp(mode, "Spectrum Cycle"))
                hue2_send_effect(fd, ch, 0x02, NULL, d->speed);
            else if (!g_ascii_strcasecmp(mode, "Breathing"))
                hue2_send_effect(fd, ch, 0x07, color, d->speed);
        }
    }
    close(fd);
}

static void nzxt_set_color(RgbDevice *d, const GdkRGBA *c) {
    nzxt_apply(d, "Static", c);
}

static void nzxt_set_mode(RgbDevice *d, const char *mode) {
    GdkRGBA eff = rgb_effective_color(d);
    nzxt_apply(d, mode, &eff);
}

const RgbProvider rgb_hue2_provider = {
    .name = "NZXT",
    .list = nzxt_list,
    .set_color = nzxt_set_color,
    .set_mode = nzxt_set_mode,
};
