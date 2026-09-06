PKGS = libadwaita-1 json-glib-1.0 libusb-1.0 lua5.4 alsa
CFLAGS ?= -O2 -Wall -Wextra
CFLAGS += $(shell pkg-config --cflags $(PKGS))
LDLIBS = $(shell pkg-config --libs $(PKGS)) -lm

SRC = src/main.c src/audio.c src/rgb.c src/rgb_common.c src/rgb_ene.c \
      src/rgb_hue2.c src/rgb_gpu.c src/rgb_asrock.c src/rgb_razer.c \
      src/rgb_logitech.c src/rgb_openrgb.c src/kraken_lcd.c src/displays.c \
      src/wallpaper.c src/midi.c
OBJ = $(SRC:.c=.o)

# headless login-time RGB restore: providers + shared helpers, no UI
RESTORE_OBJ = src/rgb_restore_main.o src/rgb_common.o src/rgb_ene.o \
      src/rgb_hue2.o src/rgb_gpu.o src/rgb_asrock.o src/rgb_razer.o \
      src/rgb_logitech.o src/rgb_openrgb.o

# windowless LCD daemon: only needs gdk-pixbuf + libusb
LCDD_LIBS = $(shell pkg-config --libs gdk-pixbuf-2.0 gthread-2.0 libusb-1.0)

# monitor-identify overlay helper: GTK3 + gtk-layer-shell (kept in its own
# process so the GTK versions never mix)
IDENT_PKGS = gtk+-3.0 gtk-layer-shell-0

all: nekoland-settings nekoland-lcdd nekoland-identify nekoland-rgb-restore \
     nekoland-midid

nekoland-settings: $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDLIBS)

nekoland-lcdd: src/lcdd_main.o src/kraken_lcd.o
	$(CC) -o $@ src/lcdd_main.o src/kraken_lcd.o $(LCDD_LIBS)

nekoland-rgb-restore: $(RESTORE_OBJ)
	$(CC) -o $@ $(RESTORE_OBJ) $(LDLIBS)

# MIDI→input daemon: just glib + alsa
nekoland-midid: src/midid_main.o
	$(CC) -o $@ src/midid_main.o \
	  $(shell pkg-config --libs glib-2.0 alsa)

src/identify_main.o: src/identify_main.c
	$(CC) -O2 -Wall -Wextra \
	  $(shell pkg-config --cflags $(IDENT_PKGS)) -c -o $@ $<

nekoland-identify: src/identify_main.o
	$(CC) -o $@ src/identify_main.o \
	  $(shell pkg-config --libs $(IDENT_PKGS))

$(OBJ) $(RESTORE_OBJ) src/lcdd_main.o: src/app.h src/rgb.h src/kraken_lcd.h

clean:
	rm -f nekoland-settings nekoland-lcdd nekoland-identify \
	  nekoland-rgb-restore $(OBJ) $(RESTORE_OBJ) \
	  src/lcdd_main.o src/identify_main.o

.PHONY: all clean
