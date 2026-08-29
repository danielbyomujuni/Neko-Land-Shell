PKGS = gtk+-3.0 gtk-layer-shell-0 json-glib-1.0 dbusmenu-gtk3-0.4
CFLAGS ?= -O2 -Wall -Wextra
CFLAGS += $(shell pkg-config --cflags $(PKGS))
LDLIBS = $(shell pkg-config --libs $(PKGS))

SRC = src/main.c src/hypr.c src/modules.c src/tray.c src/quickset.c
OBJ = $(SRC:.c=.o)

nekobar: $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDLIBS)

$(OBJ): src/nekobar.h

clean:
	rm -f nekobar $(OBJ)

.PHONY: clean
