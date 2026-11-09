# SuperV — build:  make        install:  ./install.sh
# Needs: build-essential libgtk-3-dev   (sudo apt install build-essential libgtk-3-dev)
CC      ?= gcc
CFLAGS  ?= -Os -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
PKGS     = gtk+-3.0 x11

superv: superv.c
	$(CC) -std=gnu11 $(CFLAGS) -o $@ $< $$(pkg-config --cflags --libs $(PKGS)) -ldl -Wl,--as-needed -s

clean:
	rm -f superv

.PHONY: clean
