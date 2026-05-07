# wawa

A even simpler Wayland wallpaper setter utilizing `stb_image` that targets `wlr-layer-shell`
supported compositors, with stretch, fit, fill, tile and a color, with less SLOC than your
average wallpaper setter.

## Comparison

[swaybg] uses cairo, and has a significant amount of customization, with
a slightly larger codebase. [wbg] is more sophisticated, but is simpler than
swaybg, and relies on speed using minimal third-party libraries for loading
images.

Unfortunately, wbg is not simple enough. It forces the average user if they
want more than just fitting the wallpaper to the screen - which is completely
undesirable for 4:3 wallpapers or other monitors, which wbg doesn't even have
support for setting for each monitor, wawa zooms the image by default, and is
significantly simpler.

In wawa, the image is loaded only for monitors that need it, and is immediately
freed when all monitors have been configured; this keeps the memory usage of
wawa extremely low (7x less than wbg, almost 24x less than swaybg), which may
be a performance hit if youre constantly resizing the monitor. Please open an
issue if this concerns you, in which case a specific flag can be added to keep
the image in memory, unless it is a design flaw.

[swaybg]: https://github.com/swaywm/swaybg
[wbg]: https://codeberg.org/dnkl/wbg
