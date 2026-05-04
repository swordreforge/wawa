# wawa

A even simpler Wayland wallpaper setter that targets `wlr-layer-shell` supported
compositors. It takes in a path and zooms it to each screen.

## Comparison

[swaybg] uses cairo, and has a significant amount of customization, with
a slightly larger codebase. [wbg] is more sophisticated, but is simpler,
and relies on speed using minimal libraries. Both seem to create a buffer
for each screen, and resize the image from its side before pushing to the
compositor. wawa retains a single buffer until all monitors have been set
up, and uses `wp_viewporter` to scale or crop the image to the monitor.

It is reccomended you stick to [swaybg].

[swaybg]: https://github.com/swaywm/swaybg
[wbg]: https://codeberg.org/dnkl/wbg
