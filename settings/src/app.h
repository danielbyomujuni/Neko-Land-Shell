#pragma once

#include <gtk/gtk.h>

// audio.c — the Sound page
GtkWidget *audio_page_new(void);
void audio_refresh(void);
void audio_open_advanced(GtkWindow *parent);
void audio_shutdown(void); // kill the cava/parec children

// rgb.c — the RGB lighting page
GtkWidget *rgb_page_new(void);

// displays.c — monitors via hyprctl + Lua config
GtkWidget *displays_page_new(void);

// wallpaper.c — hyprpaper-backed wallpaper picker
GtkWidget *wallpaper_page_new(void);

// midi.c — Sound page section: MIDI control → audio input bindings
GtkWidget *midi_section_new(void);
