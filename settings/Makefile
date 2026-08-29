PKGS = libadwaita-1 json-glib-1.0
CFLAGS ?= -O2 -Wall -Wextra
CFLAGS += $(shell pkg-config --cflags $(PKGS))
LDLIBS = $(shell pkg-config --libs $(PKGS)) -lm

SRC = src/main.c src/audio.c
OBJ = $(SRC:.c=.o)

nekoland-settings: $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDLIBS)

$(OBJ): src/app.h

clean:
	rm -f nekoland-settings $(OBJ)

.PHONY: clean
