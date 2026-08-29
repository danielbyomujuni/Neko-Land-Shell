// NZXT Kraken Plus V2 (PID 0x3014) LCD control — native, no liquidctl.
//
// The 240×240 screen is driven over two pipes of the same USB device:
//  - HID (hidraw): framing/control packets, 64-byte reports
//  - a vendor bulk interface (iface 0, EP 0x02 OUT): pixel data
//
// This firmware generation is bucket-less (liquidctl's "fw2" path): raw
// RGB565 framebuffers with transfer type 0x06. The GIF transfer type of
// older Krakens is NOT decoded (renders as noise), so animations are done
// client-side by streaming frames.

#pragma once

#include <gtk/gtk.h>

gboolean kraken_lcd_present(void);

// reads brightness (0-100) and orientation (degrees) from the device
gboolean kraken_lcd_get_info(int *brightness, int *orientation_deg);

gboolean kraken_lcd_set_brightness(int pct);
gboolean kraken_lcd_set_orientation(int deg); // 0 / 90 / 180 / 270

// built-in liquid-temperature screen
gboolean kraken_lcd_set_liquid(void);

// show a still image (any GdkPixbuf-loadable format, scaled to 240×240)
gboolean kraken_lcd_set_image(const char *path, GError **err);

// animated images: pre-renders the frames, then a worker thread streams
// them to the screen in a loop until stopped (or the app exits)
gboolean kraken_lcd_anim_start(const char *path, GError **err);
void kraken_lcd_anim_stop(void);
gboolean kraken_lcd_anim_active(void);
