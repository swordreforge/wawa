# wawa

A simple, hackable, and distinctive Wayland wallpaper setter utilizing
[SAIL](https://github.com/HappySeaFox/sail) that targets `wlr-layer-shell` supported compositors, featuring
tiling, spreading across monitors, along with fill, fit and stretching the
wallpaper, with less SLOC than your average wallpaper setter.

This is a **fork** by [swordreforge](https://github.com/swordreforge) of the
original [wawa](https://github.com/sewn/wawa) by **sewn** (sewnie), with
significant enhancements (see [New Features](#new-features) below).

In wawa, the image is loaded only for monitors that need it, and is immediately
freed when all monitors have been configured; this keeps the memory usage of
wawa extremely low (7x less than wbg, almost 24x less than swaybg), which may
be a performance hit if youre constantly resizing the monitor. Please open an
issue if this concerns you, in which case a specific flag can be added to keep
the image in memory, unless it is a design flaw.

## New Features (fork)

This fork adds the following enhancements over the original wawa:

- **`--random <directory>`** — pick a random image from a directory on startup
  (and on each interval tick when combined with `--interval`)
- **`--interval <seconds>`** — automatically cycle wallpapers on a timer
- **`--smart[=<tolerance>]`** — aspect-ratio-aware random selection; skips
  images whose aspect ratio deviates from the screen's by more than the given
  tolerance (default 0.1). Use with `--random` for wallpaper slideshows that
  won't pick ultra-wide or portrait images on the wrong monitor.
- **Cross-fade animation** — smooth transitions between wallpapers when using
  `--interval` + `--random`
- **Dynamic codec loading** — SAIL codecs are built as standalone `.so`
  plugins loaded via `dlopen` at runtime; unused codecs never touch memory
- **LTO + `-march=native`** — link-time optimization and machine-specific
  tuning applied globally (wawa + SAIL)
- **Bundled SAIL** — SAIL is embedded as a subproject, stripped of tests,
  tools, bindings, and non-essential bloat
- **AUR package** — `PKGBUILD` for `wawa-git` with proper optdepends for
  optional codecs (HEIF, JPEG XL, OpenEXR, RAW, JPEG2000, SVG)
- **Bugfixes** — opaque alpha at SAIL pixel level, letterbox garbage fix,
  missing `wl_surface_damage` buffer, conditional SHM pre-allocation

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

## GNOME Support

wawa automatically detects GNOME/Mutter desktops and falls back to the
GSettings backend when `wlr-layer-shell` is not available. All CLI options
(`--random`, `--interval`, `--smart`, signals) work identically.

**Mode mapping** (wawa → GNOME `picture-options`):

| wawa mode | GNOME style |
|-----------|-------------|
| `fill`    | `zoom`      |
| `fit`     | `scaled`    |
| `stretch` | `stretched` |
| `tile`    | `wallpaper` |
| `spread`  | `spanned`   |

**Format compatibility:** Images in formats not natively supported by
GNOME's GdkPixbuf (e.g. PSD, QOI, HDR, EXR) are automatically converted
to PNG via SAIL before setting.

**Note:** Cross-fade animation is not available in GNOME mode — wallpaper
switches are instant (GNOME does not expose a cross-fade API for
programmatic wallpaper changes).

To disable the GNOME backend at build time:
```bash
cmake -B build -DWAWA_GNOME=OFF
```

## Building

```bash
# Dependencies (Arch Linux)
sudo pacman -S cmake wayland wayland-protocols glib2 \
  libpng libjpeg-turbo libwebp libtiff giflib libavif

# Build (Release)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Install
sudo cmake --install build

# Or build the AUR package
makepkg -si
```

## Authors

- **sewn** (sewnie) — original author
- **[swordreforge](https://github.com/swordreforge)** — contributor, fork
  maintainer (dynamic codec loading, `--random`, `--interval`, `--smart`,
  cross-fade, AUR packaging, bundled SAIL, optimizations)

## License

MIT — see [LICENSE](LICENSE).

[swaybg]: https://github.com/swaywm/swaybg
[wbg]: https://codeberg.org/dnkl/wbg
