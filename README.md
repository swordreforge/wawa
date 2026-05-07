# wawa

A even simpler Wayland wallpaper setter utilizing `stb_image` that targets `wlr-layer-shell`
supported compositors. It takes in a path and zooms it to each screen.

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

It is reccomended you stick to [swaybg] for all intents and purposes. wawa aims
to be a simpler replacement for [swaybg] eventually.

[swaybg]: https://github.com/swaywm/swaybg
[wbg]: https://codeberg.org/dnkl/wbg
