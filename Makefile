# SPDX-FileCopyrightText: 2026 frty2.ai and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
PKGS    := x11 xext xrandr xfixes cairo-xlib
CFLAGS  ?= -O2 -pipe -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation
CFLAGS  += -std=gnu11 $(shell pkg-config --cflags $(PKGS))
LDLIBS  := $(shell pkg-config --libs $(PKGS)) -lm
PREFIX  ?= $(HOME)/.local

bwv4d: src/*.c src/bwv4d.h
	$(CC) $(CFLAGS) -o $@ src/*.c $(LDFLAGS) $(LDLIBS)
	strip $@

install: bwv4d
	install -Dm755 bwv4d $(PREFIX)/bin/bwv4d

clean:
	rm -f bwv4d

.PHONY: install clean
