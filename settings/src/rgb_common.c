// Shared RgbDevice helpers, linked by both the settings app and the
// headless nekoland-rgb-restore binary.

#include "rgb.h"

GPtrArray *rgb_claimed_locations;

RgbDevice *rgb_device_new(const RgbProvider *p, const char *id,
                          const char *name, const char *type) {
    RgbDevice *d = g_new0(RgbDevice, 1);
    d->provider = p;
    d->id = g_strdup(id);
    d->name = g_strdup(name);
    d->type = g_strdup(type);
    d->cur_mode = -1;
    d->color = (GdkRGBA){1.0, 0.0, 0.0, 1.0}; // saturated red
    d->brightness = 1.0;
    d->speed = 40;
    d->has_speed = TRUE;
    d->enabled = TRUE;
    for (int i = 0; i < RGB_MAX_LEDS; i++)
        d->led_colors[i] = d->color;
    return d;
}

void rgb_device_free(gpointer p) {
    RgbDevice *d = p;
    g_free(d->id);
    g_free(d->name);
    g_free(d->type);
    if (d->modes)
        g_ptr_array_free(d->modes, TRUE);
    g_free(d);
}
