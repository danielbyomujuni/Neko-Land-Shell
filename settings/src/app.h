#pragma once

#include <gtk/gtk.h>

// audio.c — the Sound page
GtkWidget *audio_page_new(void);
void audio_refresh(void);
void audio_open_advanced(GtkWindow *parent);
void audio_shutdown(void); // kill the cava child
