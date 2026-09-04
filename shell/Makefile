PKGS = gtk+-3.0 gtk-layer-shell-0 json-glib-1.0 dbusmenu-gtk3-0.4 gio-unix-2.0
CFLAGS ?= -O2 -Wall -Wextra
CFLAGS += $(shell pkg-config --cflags $(PKGS))
LDLIBS = $(shell pkg-config --libs $(PKGS)) -lm

SRC = src/main.c src/hypr.c src/modules.c src/tray.c src/quickset.c \
      src/launcher.c src/osd.c src/vr.c src/toolbar.c
OBJ = $(SRC:.c=.o)

nekobar: $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDLIBS)

$(OBJ): src/nekobar.h

clean:
	rm -f nekobar $(OBJ)

.PHONY: clean
