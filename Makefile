CC ?= cc
PKGS = cairo pangocairo wayland-client xkbcommon
CFLAGS ?= -march=native -O2 -g -std=c11 -Wall -Wextra -flto
CFLAGS += $(shell pkg-config --cflags $(PKGS))
LDLIBS = $(shell pkg-config --libs $(PKGS)) -lm -lpthread

PREFIX ?= /usr/local
BINDIR := $(PREFIX)/bin

WAYLAND_PROTOCOLS := $(shell pkg-config --variable=pkgdatadir wayland-protocols)
XDG_SHELL_XML := $(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml
XDG_DECO_XML := $(WAYLAND_PROTOCOLS)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml
XDG_ICON_XML := $(WAYLAND_PROTOCOLS)/staging/xdg-toplevel-icon/xdg-toplevel-icon-v1.xml
CURSOR_SHAPE_XML := $(WAYLAND_PROTOCOLS)/staging/cursor-shape/cursor-shape-v1.xml
TABLET_XML := $(WAYLAND_PROTOCOLS)/unstable/tablet/tablet-unstable-v2.xml

GEN = xdg-shell-client-protocol.h xdg-shell-protocol.c \
      xdg-decoration-client-protocol.h xdg-decoration-protocol.c \
      xdg-toplevel-icon-client-protocol.h xdg-toplevel-icon-protocol.c \
      cursor-shape-client-protocol.h cursor-shape-protocol.c \
      tablet-client-protocol.h tablet-protocol.c

all: jtupdate jtupdatectl

jtupdate: jtupdate.c $(GEN)
	$(CC) $(CFLAGS) -o $@ jtupdate.c \
		xdg-shell-protocol.c xdg-decoration-protocol.c \
		xdg-toplevel-icon-protocol.c cursor-shape-protocol.c \
		tablet-protocol.c \
		$(LDFLAGS) $(LDLIBS)

jtupdatectl: jtupdatectl.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

xdg-shell-client-protocol.h: $(XDG_SHELL_XML)
	wayland-scanner client-header $< $@

xdg-shell-protocol.c: $(XDG_SHELL_XML)
	wayland-scanner private-code $< $@

xdg-decoration-client-protocol.h: $(XDG_DECO_XML)
	wayland-scanner client-header $< $@

xdg-decoration-protocol.c: $(XDG_DECO_XML)
	wayland-scanner private-code $< $@

xdg-toplevel-icon-client-protocol.h: $(XDG_ICON_XML)
	wayland-scanner client-header $< $@

xdg-toplevel-icon-protocol.c: $(XDG_ICON_XML)
	wayland-scanner private-code $< $@

cursor-shape-client-protocol.h: $(CURSOR_SHAPE_XML)
	wayland-scanner client-header $< $@

cursor-shape-protocol.c: $(CURSOR_SHAPE_XML)
	wayland-scanner private-code $< $@

tablet-client-protocol.h: $(TABLET_XML)
	wayland-scanner client-header $< $@

tablet-protocol.c: $(TABLET_XML)
	wayland-scanner private-code $< $@

install: jtupdate jtupdatectl
	install -Dm755 jtupdate $(DESTDIR)$(BINDIR)/jtupdate
	install -Dm755 jtupdatectl $(DESTDIR)$(BINDIR)/jtupdatectl
	@echo "installed to $(DESTDIR)$(PREFIX)"

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/jtupdate
	rm -f $(DESTDIR)$(BINDIR)/jtupdatectl

preview: jtupdate
	./jtupdate --screenshot preview.png

clean:
	rm -f jtupdate jtupdatectl preview.png $(GEN)

.PHONY: all install uninstall preview clean
