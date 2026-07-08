# See LICENSE file for copyright and license details.
#
# wawa now uses SAIL (Squirrel Abstract Imaging Library) via CMake.
# This Makefile is a convenience wrapper around the CMake build.
#
# Dependencies: cmake, pkg-config, wayland-client, wayland-protocols,
#               wayland-scanner, libwebp, libpng, libjpeg-turbo, etc.
#
# Install dependencies on Arch Linux:
#   sudo pacman -S cmake wayland wayland-protocols libpng libjpeg-turbo libwebp
#

BUILD_DIR = build
CONFIG  ?= Release
JOBS    ?= $(shell nproc 2>/dev/null || echo 4)

PREFIX = /usr/local

all: $(BUILD_DIR)/build.stamp

$(BUILD_DIR)/build.stamp: CMakeLists.txt sail/CMakeLists.txt wawa.c
	mkdir -p $(BUILD_DIR)
	cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(CONFIG) -DCMAKE_INSTALL_PREFIX=$(PREFIX)
	cmake --build $(BUILD_DIR) -j$(JOBS)
	touch $@

wawa: $(BUILD_DIR)/build.stamp
	cp $(BUILD_DIR)/wawa wawa

clean:
	rm -rf $(BUILD_DIR) wawa

distclean: clean
	rm -rf sail/build

install: wawa
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp -f wawa $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/wawa

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/wawa

.PHONY: all clean distclean install uninstall wawa
