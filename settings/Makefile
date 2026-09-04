PKGS = libadwaita-1 json-glib-1.0 libusb-1.0 lua5.4
CFLAGS ?= -O2 -Wall -Wextra
CFLAGS += $(shell pkg-config --cflags $(PKGS))
LDLIBS = $(shell pkg-config --libs $(PKGS)) -lm

SRC = src/main.c src/audio.c src/rgb.c src/rgb_ene.c src/rgb_hue2.c \
      src/rgb_gpu.c src/rgb_asrock.c src/rgb_razer.c src/rgb_logitech.c \
      src/rgb_openrgb.c src/kraken_lcd.c src/displays.c
OBJ = $(SRC:.c=.o)

# windowless LCD daemon: only needs gdk-pixbuf + libusb
LCDD_LIBS = $(shell pkg-config --libs gdk-pixbuf-2.0 gthread-2.0 libusb-1.0)

# monitor-identify overlay helper: GTK3 + gtk-layer-shell (kept in its own
# process so the GTK versions never mix)
IDENT_PKGS = gtk+-3.0 gtk-layer-shell-0

all: nekoland-settings nekoland-lcdd nekoland-identify

nekoland-settings: $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDLIBS)

nekoland-lcdd: src/lcdd_main.o src/kraken_lcd.o
	$(CC) -o $@ src/lcdd_main.o src/kraken_lcd.o $(LCDD_LIBS)

src/identify_main.o: src/identify_main.c
	$(CC) -O2 -Wall -Wextra \
	  $(shell pkg-config --cflags $(IDENT_PKGS)) -c -o $@ $<

nekoland-identify: src/identify_main.o
	$(CC) -o $@ src/identify_main.o \
	  $(shell pkg-config --libs $(IDENT_PKGS))

$(OBJ) src/lcdd_main.o: src/app.h src/rgb.h src/kraken_lcd.h

clean:
	rm -f nekoland-settings nekoland-lcdd nekoland-identify $(OBJ) \
	  src/lcdd_main.o src/identify_main.o

.PHONY: all clean
