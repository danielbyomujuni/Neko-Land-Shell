PKGS = libadwaita-1
CFLAGS ?= -O2 -Wall -Wextra
CFLAGS += $(shell pkg-config --cflags $(PKGS))
LDLIBS = $(shell pkg-config --libs $(PKGS))

SRC = src/main.c
OBJ = $(SRC:.c=.o)

nekoland-settings: $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDLIBS)

clean:
	rm -f nekoland-settings $(OBJ)

.PHONY: clean
