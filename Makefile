# See LICENSE file for copyright and license details.
.POSIX:

VERSION = 1.0

PKG_CONFIG = pkg-config

PREFIX = /usr/local

PKGS = wayland-client
INCS != $(PKG_CONFIG) --cflags $(PKGS)
LIBS != $(PKG_CONFIG) --libs $(PKGS)

CPPFLAGS = -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -DVERSION=\"$(VERSION)\"
CFLAGS   = -pedantic -Wall $(INCS)
LDLIBS   = $(LIBS)

all: wawa

PROTO = viewporter-protocol.h wlr-layer-shell-unstable-v1-protocol.h xdg-shell-protocol.h
OBJ = wawa.o $(PROTO:.h=.o)

wawa.c: $(PROTO)
wawa: $(OBJ)

WAYLAND_PROTOCOLS != $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols
WAYLAND_SCANNER   != $(PKG_CONFIG) --variable=wayland_scanner wayland-scanner

# I absolutely despise this. Cannot avoid it without using BSD or GNU make.
xdg-shell-protocol.c xdg-shell-protocol.h: $(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml
xdg-shell-protocol.h:
	$(WAYLAND_SCANNER) client-header $< $@
xdg-shell-protocol.c:
	$(WAYLAND_SCANNER) private-code $< $@
viewporter-protocol.c viewporter-protocol.h: $(WAYLAND_PROTOCOLS)/stable/viewporter/viewporter.xml
viewporter-protocol.h:
	$(WAYLAND_SCANNER) client-header $< $@
viewporter-protocol.c:
	$(WAYLAND_SCANNER) private-code $< $@
wlr-layer-shell-unstable-v1-protocol.c wlr-layer-shell-unstable-v1-protocol.h: wlr-layer-shell-unstable-v1.xml
wlr-layer-shell-unstable-v1-protocol.h:
	$(WAYLAND_SCANNER) client-header $< $@
wlr-layer-shell-unstable-v1-protocol.c:
	$(WAYLAND_SCANNER) private-code $< $@
	
clean:
	rm -f wawa $(OBJ) $(PROTO) $(PROTO:.h=.c)

install: all
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp -f wawa $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/wawa

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/wawa

.PHONY: all clean install uninstall
