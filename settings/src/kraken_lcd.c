// Kraken Plus V2 LCD driver. Protocol verified live on firmware 1.0.0:
//   info:        HID [0x30 0x01] -> reply 0x31 0x01, brightness@0x18,
//                orientation@0x1A (quarter turns)
//   set b/o:     HID [0x30 0x02 0x01 bright 0 0 0x01 orient]
//   liquid mode: HID [0x38 0x01 0x02 0x00] -> 0x39 0x01
//   frame:       HID [0x36 0x01 0x00 0x01 0x06] -> 0x37 0x01,
//                bulk 12-byte magic + [0x06 0 0 0] + size(LE32), RGB565
//                pixels, HID [0x36 0x02] -> 0x37 0x02
// The device continuously streams 0x75 status reports on the HID pipe, so
// every read must skim until the wanted prefix (liquidctl's fixed 12-read
// budget is what makes it fail on this unit).

#include "kraken_lcd.h"

#include <fcntl.h>
#include <libusb-1.0/libusb.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>

#define LCD_VID 0x1E71
#define LCD_PID 0x3014
#define LCD_W 240
#define LCD_H 240
#define LCD_EP_OUT 0x02
#define LCD_IFACE 0

// serialises all device access (UI calls vs the animation thread)
static GMutex lcd_lock;

// ---- HID side ----

static char *lcd_find_hidraw(void) {
    char *found = NULL;
    GDir *dir = g_dir_open("/sys/class/hidraw", 0, NULL);
    if (!dir)
        return NULL;
    const char *entry;
    while (!found && (entry = g_dir_read_name(dir))) {
        char *path =
            g_strdup_printf("/sys/class/hidraw/%s/device/uevent", entry);
        char *uevent = NULL;
        g_file_get_contents(path, &uevent, NULL, NULL);
        g_free(path);
        if (uevent && strstr(uevent, "HID_ID=0003:00001E71:00003014"))
            found = g_strdup_printf("/dev/%s", entry);
        g_free(uevent);
    }
    g_dir_close(dir);
    return found;
}

static int lcd_hid_open(void) {
    char *node = lcd_find_hidraw();
    if (!node)
        return -1;
    int fd = open(node, O_RDWR | O_NONBLOCK);
    g_free(node);
    return fd;
}

static void lcd_hid_write(int fd, const guint8 *data, gsize len) {
    guint8 buf[64] = {0};
    memcpy(buf, data, MIN(len, sizeof(buf)));
    (void)!write(fd, buf, sizeof(buf));
}

static void lcd_hid_drain(int fd) {
    guint8 buf[64];
    while (read(fd, buf, sizeof(buf)) > 0)
        ;
}

// skim the status stream until a reply starting with p0 p1 arrives
static gboolean lcd_hid_read_until(int fd, guint8 p0, guint8 p1, guint8 *out) {
    for (int i = 0; i < 80; i++) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        if (poll(&pfd, 1, 250) <= 0)
            continue;
        guint8 buf[64];
        if (read(fd, buf, sizeof(buf)) < 2)
            continue;
        if (buf[0] == p0 && buf[1] == p1) {
            if (out)
                memcpy(out, buf, sizeof(buf));
            return TRUE;
        }
    }
    return FALSE;
}

// ---- bulk side ----

static libusb_context *usb_ctx;

static libusb_device_handle *lcd_bulk_open(void) {
    if (!usb_ctx && libusb_init(&usb_ctx) < 0)
        return NULL;
    libusb_device_handle *h =
        libusb_open_device_with_vid_pid(usb_ctx, LCD_VID, LCD_PID);
    if (!h)
        return NULL;
    libusb_set_auto_detach_kernel_driver(h, 1);
    if (libusb_claim_interface(h, LCD_IFACE) < 0) {
        libusb_close(h);
        return NULL;
    }
    return h;
}

static void lcd_bulk_close(libusb_device_handle *h) {
    libusb_release_interface(h, LCD_IFACE);
    libusb_close(h);
}

static gboolean lcd_bulk_write(libusb_device_handle *h, const guint8 *data,
                               int len) {
    int done = 0;
    while (done < len) {
        int n = 0;
        if (libusb_bulk_transfer(h, LCD_EP_OUT, (guint8 *)data + done,
                                 len - done, &n, 10000) < 0)
            return FALSE;
        done += n;
    }
    return TRUE;
}

// ---- frame upload ----

// fd/h already open; sends one RGB565 frame through the framing handshake
static gboolean lcd_send_frame(int fd, libusb_device_handle *h,
                               const guint8 *fb, gsize fb_len) {
    static const guint8 start[] = {0x36, 0x01, 0x00, 0x01, 0x06};
    static const guint8 end[] = {0x36, 0x02};
    guint8 header[20] = {0x12, 0xFA, 0x01, 0xE8, 0xAB, 0xCD, 0xEF,
                         0x98, 0x76, 0x54, 0x32, 0x10, 0x06, 0x00,
                         0x00, 0x00};
    header[16] = fb_len & 0xFF;
    header[17] = (fb_len >> 8) & 0xFF;
    header[18] = (fb_len >> 16) & 0xFF;
    header[19] = (fb_len >> 24) & 0xFF;

    lcd_hid_drain(fd);
    lcd_hid_write(fd, start, sizeof(start));
    if (!lcd_hid_read_until(fd, 0x37, 0x01, NULL))
        return FALSE;
    if (!lcd_bulk_write(h, header, sizeof(header)) ||
        !lcd_bulk_write(h, fb, (int)fb_len))
        return FALSE;
    lcd_hid_drain(fd);
    lcd_hid_write(fd, end, sizeof(end));
    return lcd_hid_read_until(fd, 0x37, 0x02, NULL);
}

// ---- pixbuf -> RGB565 ----

static int lcd_read_orientation(int fd) {
    guint8 info[64];
    lcd_hid_drain(fd);
    lcd_hid_write(fd, (const guint8[]){0x30, 0x01}, 2);
    if (!lcd_hid_read_until(fd, 0x31, 0x01, info))
        return 0;
    return info[0x1A] & 0x03;
}

// gdk_pixbuf_scale_simple/rotate_simple come from gdk-pixbuf, not GTK, so
// this file also links into the windowless nekoland-lcdd daemon
// scales to 240×240, pre-rotates for the mounted orientation and packs
// RGB565 big-endianish the way the panel wants it
static guint8 *lcd_pack_pixbuf(GdkPixbuf *src, int orientation,
                               gsize *out_len) {
    GdkPixbuf *scaled = gdk_pixbuf_scale_simple(src, LCD_W, LCD_H,
                                                GDK_INTERP_BILINEAR);
    if (!scaled)
        return NULL;
    if (orientation) {
        // panel is mounted rotated: counter-rotate the image
        // (gdk rotates counterclockwise; the panel needs clockwise)
        GdkPixbufRotation rot = (GdkPixbufRotation)((360 - 90 * orientation) %
                                                    360);
        GdkPixbuf *r = gdk_pixbuf_rotate_simple(scaled, rot);
        g_object_unref(scaled);
        scaled = r;
        if (!scaled)
            return NULL;
    }
    int channels = gdk_pixbuf_get_n_channels(scaled);
    int stride = gdk_pixbuf_get_rowstride(scaled);
    const guint8 *px = gdk_pixbuf_read_pixels(scaled);
    gsize len = (gsize)LCD_W * LCD_H * 2;
    guint8 *fb = g_malloc(len);
    gsize o = 0;
    for (int y = 0; y < LCD_H; y++) {
        const guint8 *row = px + (gsize)y * stride;
        for (int x = 0; x < LCD_W; x++) {
            const guint8 *p = row + (gsize)x * channels;
            guint8 r5 = p[0] >> 3, g6 = p[1] >> 2, b5 = p[2] >> 3;
            fb[o++] = (guint8)((r5 << 3) | (g6 >> 3));
            fb[o++] = (guint8)(((g6 & 0x7) << 5) | b5);
        }
    }
    g_object_unref(scaled);
    *out_len = len;
    return fb;
}

// ---- animation worker ----

typedef struct {
    GPtrArray *frames; // guint8* RGB565 buffers
    GArray *delays;    // int, ms per frame
} LcdAnim;

static GThread *anim_thread;
static volatile gboolean anim_stop_flag;

static void lcd_anim_free(LcdAnim *a) {
    g_ptr_array_free(a->frames, TRUE);
    g_array_free(a->delays, TRUE);
    g_free(a);
}

static gpointer lcd_anim_worker(gpointer data) {
    LcdAnim *a = data;
    gsize fb_len = (gsize)LCD_W * LCD_H * 2;

    // hold both pipes open for the whole session: per-frame claim/release
    // churn makes the device stop taking frames after the first one
    g_mutex_lock(&lcd_lock);
    int fd = lcd_hid_open();
    libusb_device_handle *h = fd >= 0 ? lcd_bulk_open() : NULL;
    g_mutex_unlock(&lcd_lock);
    if (!h) {
        if (fd >= 0)
            close(fd);
        g_printerr("lcd anim: cannot open device\n");
        lcd_anim_free(a);
        return NULL;
    }

    int failures = 0;
    while (!anim_stop_flag && failures < 5) {
        for (guint i = 0; i < a->frames->len && !anim_stop_flag; i++) {
            gint64 t0 = g_get_monotonic_time();
            g_mutex_lock(&lcd_lock);
            gboolean ok = lcd_send_frame(fd, h,
                                         g_ptr_array_index(a->frames, i),
                                         fb_len);
            g_mutex_unlock(&lcd_lock);
            if (!ok) {
                failures++;
                g_printerr("lcd anim: frame %u failed\n", i);
            } else {
                failures = 0;
            }
            int delay = g_array_index(a->delays, int, i);
            gint64 spent = (g_get_monotonic_time() - t0) / 1000;
            if (delay > spent)
                g_usleep((gulong)(delay - spent) * 1000);
        }
    }
    lcd_bulk_close(h);
    close(fd);
    lcd_anim_free(a);
    return NULL;
}

void kraken_lcd_anim_stop(void) {
    if (!anim_thread)
        return;
    anim_stop_flag = TRUE;
    g_thread_join(anim_thread);
    anim_thread = NULL;
}

gboolean kraken_lcd_anim_active(void) {
    return anim_thread != NULL && !anim_stop_flag;
}

// ---- public API ----

gboolean kraken_lcd_present(void) {
    char *node = lcd_find_hidraw();
    gboolean ok = node != NULL;
    g_free(node);
    return ok;
}

gboolean kraken_lcd_get_info(int *brightness, int *orientation_deg) {
    g_mutex_lock(&lcd_lock);
    int fd = lcd_hid_open();
    gboolean ok = FALSE;
    if (fd >= 0) {
        guint8 info[64];
        lcd_hid_drain(fd);
        lcd_hid_write(fd, (const guint8[]){0x30, 0x01}, 2);
        ok = lcd_hid_read_until(fd, 0x31, 0x01, info);
        if (ok) {
            if (brightness)
                *brightness = info[0x18];
            if (orientation_deg)
                *orientation_deg = (info[0x1A] & 0x03) * 90;
        }
        close(fd);
    }
    g_mutex_unlock(&lcd_lock);
    return ok;
}

static gboolean lcd_set_bo(int brightness, int orientation) {
    g_mutex_lock(&lcd_lock);
    int fd = lcd_hid_open();
    gboolean ok = FALSE;
    if (fd >= 0) {
        guint8 info[64];
        lcd_hid_drain(fd);
        lcd_hid_write(fd, (const guint8[]){0x30, 0x01}, 2);
        if (lcd_hid_read_until(fd, 0x31, 0x01, info)) {
            guint8 b = brightness >= 0 ? (guint8)brightness : info[0x18];
            guint8 o = orientation >= 0 ? (guint8)orientation
                                        : (guint8)(info[0x1A] & 0x03);
            guint8 pkt[] = {0x30, 0x02, 0x01, b, 0x00, 0x00, 0x01, o};
            lcd_hid_write(fd, pkt, sizeof(pkt));
            ok = TRUE;
        }
        close(fd);
    }
    g_mutex_unlock(&lcd_lock);
    return ok;
}

gboolean kraken_lcd_set_brightness(int pct) {
    return lcd_set_bo(CLAMP(pct, 0, 100), -1);
}

gboolean kraken_lcd_set_orientation(int deg) {
    return lcd_set_bo(-1, CLAMP(deg, 0, 270) / 90);
}

gboolean kraken_lcd_set_liquid(void) {
    kraken_lcd_anim_stop();
    g_mutex_lock(&lcd_lock);
    int fd = lcd_hid_open();
    gboolean ok = FALSE;
    if (fd >= 0) {
        lcd_hid_drain(fd);
        lcd_hid_write(fd, (const guint8[]){0x38, 0x01, 0x02, 0x00}, 4);
        ok = lcd_hid_read_until(fd, 0x39, 0x01, NULL);
        close(fd);
    }
    g_mutex_unlock(&lcd_lock);
    return ok;
}

gboolean kraken_lcd_set_image(const char *path, GError **err) {
    kraken_lcd_anim_stop();
    GdkPixbuf *pb = gdk_pixbuf_new_from_file(path, err);
    if (!pb)
        return FALSE;
    g_mutex_lock(&lcd_lock);
    gboolean ok = FALSE;
    int fd = lcd_hid_open();
    libusb_device_handle *h = fd >= 0 ? lcd_bulk_open() : NULL;
    if (h) {
        gsize fb_len = 0;
        guint8 *fb = lcd_pack_pixbuf(pb, lcd_read_orientation(fd), &fb_len);
        if (fb) {
            // firmware wants the first upload twice (framebuffer swap)
            ok = lcd_send_frame(fd, h, fb, fb_len) &&
                 lcd_send_frame(fd, h, fb, fb_len);
            g_free(fb);
        }
        lcd_bulk_close(h);
    }
    if (fd >= 0)
        close(fd);
    g_mutex_unlock(&lcd_lock);
    g_object_unref(pb);
    if (!ok && err && !*err)
        g_set_error(err, g_quark_from_static_string("kraken-lcd"), 1,
                    "LCD transfer failed");
    return ok;
}

gboolean kraken_lcd_anim_start(const char *path, GError **err) {
    kraken_lcd_anim_stop();
    GdkPixbufAnimation *anim = gdk_pixbuf_animation_new_from_file(path, err);
    if (!anim)
        return FALSE;
    if (gdk_pixbuf_animation_is_static_image(anim)) {
        g_object_unref(anim);
        return kraken_lcd_set_image(path, err);
    }

    // pre-render every frame so the worker only streams
    g_mutex_lock(&lcd_lock);
    int fd = lcd_hid_open();
    int orientation = fd >= 0 ? lcd_read_orientation(fd) : 0;
    if (fd >= 0)
        close(fd);
    g_mutex_unlock(&lcd_lock);

    LcdAnim *a = g_new0(LcdAnim, 1);
    a->frames = g_ptr_array_new_with_free_func(g_free);
    a->delays = g_array_new(FALSE, FALSE, sizeof(int));

    G_GNUC_BEGIN_IGNORE_DEPRECATIONS // the iter API still wants GTimeVal
    GTimeVal tv = {0, 0};
    GdkPixbufAnimationIter *it = gdk_pixbuf_animation_get_iter(anim, &tv);
    for (int i = 0; i < 240; i++) { // frame cap
        GdkPixbuf *pb = gdk_pixbuf_animation_iter_get_pixbuf(it);
        gsize fb_len = 0;
        guint8 *fb = lcd_pack_pixbuf(pb, orientation, &fb_len);
        if (fb) {
            // the iterator loops forever; a frame identical to the first
            // one means the sequence wrapped
            if (i > 0 &&
                memcmp(fb, g_ptr_array_index(a->frames, 0), fb_len) == 0) {
                g_free(fb);
                break;
            }
            g_ptr_array_add(a->frames, fb);
        }
        int delay = gdk_pixbuf_animation_iter_get_delay_time(it);
        if (delay < 40)
            delay = 40; // ~25 fps cap
        g_array_append_val(a->delays, delay);
        gint64 us = (gint64)tv.tv_sec * 1000000 + tv.tv_usec +
                    (gint64)delay * 1000;
        tv.tv_sec = us / 1000000;
        tv.tv_usec = us % 1000000;
        gdk_pixbuf_animation_iter_advance(it, &tv);
    }
    g_object_unref(it);
    G_GNUC_END_IGNORE_DEPRECATIONS
    g_object_unref(anim);

    if (a->frames->len == 0) {
        lcd_anim_free(a);
        g_set_error(err, g_quark_from_static_string("kraken-lcd"), 2,
                    "no frames decoded");
        return FALSE;
    }
    g_printerr("lcd anim: %u frames\n", a->frames->len);
    anim_stop_flag = FALSE;
    anim_thread = g_thread_new("kraken-lcd-anim", lcd_anim_worker, a);
    return TRUE;
}
