PKGS = libadwaita-1 json-glib-1.0
CFLAGS ?= -O2 -Wall -Wextra
CFLAGS += $(shell pkg-config --cflags $(PKGS))
LDLIBS = $(shell pkg-config --libs $(PKGS)) -lm

SRC = src/main.c src/audio.c src/rgb.c src/rgb_ene.c src/rgb_hue2.c \
      src/rgb_gpu.c src/rgb_asrock.c src/rgb_razer.c src/rgb_logitech.c \
      src/rgb_openrgb.c
OBJ = $(SRC:.c=.o)

nekoland-settings: $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDLIBS)

$(OBJ): src/app.h src/rgb.h

clean:
	rm -f nekoland-settings $(OBJ)

.PHONY: clean
