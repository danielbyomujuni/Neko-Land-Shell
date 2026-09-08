PKGS = libadwaita-1 json-glib-1.0
CFLAGS ?= -O2 -Wall -Wextra
CFLAGS += $(shell pkg-config --cflags $(PKGS))
LDLIBS = $(shell pkg-config --libs $(PKGS))

SRC = src/main.c src/anilist.c src/fetch.c src/schedule.c
OBJ = $(SRC:.c=.o)

all: nekoland-animanager

nekoland-animanager: $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDLIBS)

clean:
	rm -f $(OBJ) nekoland-animanager

.PHONY: all clean
