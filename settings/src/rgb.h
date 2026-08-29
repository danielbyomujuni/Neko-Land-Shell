// Shared types for RGB lighting providers.
//
// A provider implements list()/set_color()/set_mode() for one family of
// devices and is registered in providers[] (rgb.c). list() runs in a worker
// thread and may block; set_* run on the main thread and must not block for
// long. Native providers should append the device node they claim to
// rgb_claimed_locations so fallback providers can skip those devices.

#pragma once

#include <gtk/gtk.h>

typedef struct RgbProvider RgbProvider;

#define RGB_MAX_LEDS 32

typedef struct {
    const RgbProvider *provider;
    char *id;   // provider-specific handle (e.g. "/dev/i2c-14:0x71")
    char *name;
    char *type;       // chip group: Memory / GPU / Case fans / ...
    GPtrArray *modes; // char*
    int cur_mode;     // index into modes, -1 if unknown
    GdkRGBA color;    // chosen colour (unscaled; brightness applied on send)
    double brightness; // 0..1, multiplied into the colour on send
    int speed;         // 0..100, used by providers with a speed control
    gboolean enabled;  // row switch; off = lighting off for this device
    gboolean has_speed; // FALSE when the protocol has no speed control
    gboolean has_lcd;   // device carries an LCD (Kraken Plus V2)
    int n_leds;        // >0 when the provider supports per-LED painting
    GdkRGBA led_colors[RGB_MAX_LEDS]; // UI-side per-LED state
} RgbDevice;

struct RgbProvider {
    const char *name;
    GPtrArray *(*list)(void); // RgbDevice*; called off the main thread
    void (*set_color)(RgbDevice *d, const GdkRGBA *c);
    void (*set_mode)(RgbDevice *d, const char *mode);
    // optional: apply one colour to every device in a single operation
    void (*set_color_all)(const GdkRGBA *c);
    // optional: set a single LED (for devices with n_leds > 0); the colour
    // arrives pre-scaled by brightness; may be NULL
    void (*set_led)(RgbDevice *d, int led, const GdkRGBA *c);
};

RgbDevice *rgb_device_new(const RgbProvider *p, const char *id,
                          const char *name, const char *type);
void rgb_device_free(gpointer p);

// the colour a provider should actually emit (brightness folded in)
static inline GdkRGBA rgb_effective_color(const RgbDevice *d) {
    GdkRGBA c = d->color;
    c.red *= d->brightness;
    c.green *= d->brightness;
    c.blue *= d->brightness;
    return c;
}

// device nodes claimed by native providers during the current scan
extern GPtrArray *rgb_claimed_locations; // char*

extern const RgbProvider rgb_ene_provider;     // ENE DRAM over SMBus
extern const RgbProvider rgb_hue2_provider;    // NZXT hubs over hidraw
extern const RgbProvider rgb_gpu_provider;     // Gigabyte Fusion2 GPU (i2c)
extern const RgbProvider rgb_asrock_provider;  // ASRock Polychrome USB
extern const RgbProvider rgb_razer_provider;   // Razer extended matrix
extern const RgbProvider rgb_logitech_provider; // Logitech HID++ 0x8070
extern const RgbProvider rgb_openrgb_provider; // optional CLI fallback
