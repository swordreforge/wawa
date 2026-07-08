# wawa

A simple, hackable, and distinctive Wayland wallpaper setter utilizing
[SAIL](https://github.com/HappySeaFox/sail) that targets `wlr-layer-shell` supported compositors, featuring
tiling, spreading across monitors, along with fill, fit and stretching the
wallpaper, with less SLOC than your average wallpaper setter.

In wawa, the image is loaded only for monitors that need it, and is immediately
freed when all monitors have been configured; this keeps the memory usage of
wawa extremely low (7x less than wbg, almost 24x less than swaybg), which may
be a performance hit if youre constantly resizing the monitor. Please open an
issue if this concerns you, in which case a specific flag can be added to keep
the image in memory, unless it is a design flaw.

## Comparison

[swaybg] uses cairo, and has a significant amount of customization, with
a slightly larger codebase. [wbg] is more sophisticated, but is simpler than
swaybg, and relies on speed using minimal third-party libraries for loading
images, much like wbg. It also has support for defining multiple images for each
monitor specified on the command line.

Unfortunately, wbg is so simple, while supporting modern libraries, it has only
two modes, fit (with `-s` flag) and fill (default), which makes it undesireable
for those who want to tile or etc.

No other wallpaper setter supports tiling out of the box.

## Building

```bash
# Dependencies (Arch Linux)
sudo pacman -S cmake wayland wayland-protocols \
  libpng libjpeg-turbo libwebp libtiff giflib libavif

# Build (Release)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Install
sudo cmake --install build

# Or build the AUR package
makepkg -si
```

[swaybg]: https://github.com/swaywm/swaybg
[wbg]: https://codeberg.org/dnkl/wbg
