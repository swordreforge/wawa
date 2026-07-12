/* gnome-backend.c — GNOME/GSettings wallpaper backend for wawa
 *
 * Used when wlr-layer-shell is not available (e.g. on GNOME/Mutter).
 * Sets wallpaper via GSettings C API; reuses SAIL for image loading
 * and file selection logic from the main wawa codebase.
 */
#include "wawa.h"

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <gio/gio.h>
#include <sail/sail.h>
#include <sail-manip/sail-manip.h>

/* ---- GNOME format compatibility ----
 * GdkPixbuf natively supports these via dynamically-loaded loaders.
 * Formats NOT in this list will be converted to PNG before setting. */
static const char *gnome_native_fmts[] = {
	"png", "jpg", "jpeg", "bmp", "gif", "ico", "svg",
	"webp", "avif", "heif", "heic", "jxl",
	NULL
};

static int
gnome_supports_format(const char *path)
{
	const char *dot = strrchr(path, '.');
	if (!dot || !dot[1])
		return 0;
	dot++;
	for (int i = 0; gnome_native_fmts[i]; i++)
		if (!strcasecmp(dot, gnome_native_fmts[i]))
			return 1;
	return 0;
}

/* Convert an image to PNG via SAIL, write to /tmp, return the temp path.
 * Caller must free() the returned path. */
static char *
convert_to_png(const char *path)
{
	struct sail_image *img = NULL;
	sail_status_t st;

	st = sail_load_from_file(path, &img);
	if (st != SAIL_OK)
		die("gnome: failed to load image: %s", path);

	/* Ensure BGRA for consistent output */
	if (img->pixel_format != SAIL_PIXEL_FORMAT_BPP32_BGRA) {
		struct sail_image *conv = NULL;
		st = sail_convert_image(img, SAIL_PIXEL_FORMAT_BPP32_BGRA, &conv);
		if (st != SAIL_OK) {
			sail_destroy_image(img);
			die("gnome: failed to convert image to BGRA");
		}
		sail_destroy_image(img);
		img = conv;
	}

	sail_force_opaque(img);

	char *tmp = NULL;
	if (asprintf(&tmp, "/tmp/wawa-gnome-%d.png", getpid()) < 0)
		die("gnome: asprintf:");

	st = sail_save_into_file(tmp, img);
	sail_destroy_image(img);
	if (st != SAIL_OK)
		die("gnome: failed to save PNG to %s", tmp);

	return tmp;
}

/* ---- GSettings helpers ---- */

/* Map wawa rendering mode to GNOME picture-options string */
static const char *
mode_to_gnome(const char *mode)
{
	if (!strcmp(mode, "fill"))    return "zoom";
	if (!strcmp(mode, "fit"))     return "scaled";
	if (!strcmp(mode, "stretch")) return "stretched";
	if (!strcmp(mode, "tile"))    return "wallpaper";
	if (!strcmp(mode, "spread"))  return "spanned";
	return "zoom";
}

static void
gnome_set_wallpaper(const char *path)
{
	char *uri = g_filename_to_uri(path, NULL, NULL);
	if (!uri) die("gnome: g_filename_to_uri failed");

	GSettings *s = g_settings_new("org.gnome.desktop.background");
	g_settings_set_string(s, "picture-uri", uri);
	g_settings_set_string(s, "picture-uri-dark", uri);
	g_object_unref(s);
	g_free(uri);
}

static void
gnome_set_picture_options(const char *mode)
{
	GSettings *s = g_settings_new("org.gnome.desktop.background");
	g_settings_set_string(s, "picture-options", mode_to_gnome(mode));
	g_object_unref(s);
}

/* Set solid background color (for `wawa RRGGBBAA` mode) */
static void
gnome_set_color(uint32_t color)
{
	/* color is in BGRA byte order from parse_color(); convert to #RRGGBB */
	char hex[8];
	snprintf(hex, sizeof(hex), "#%02x%02x%02x",
		(color >> 8) & 0xFF,
		(color >> 16) & 0xFF,
		(color >> 24) & 0xFF);

	GSettings *s = g_settings_new("org.gnome.desktop.background");
	g_settings_set_string(s, "picture-options", "none");
	g_settings_set_string(s, "primary-color", hex);
	g_settings_set_string(s, "color-shading-type", "solid");
	g_object_unref(s);
}

/* Ensure the image path is compatible with GNOME; convert if needed.
 * Returns the path to use (may be a temp file; caller should free). */
static char *
gnome_ensure_compatible(const char *path)
{
	if (gnome_supports_format(path))
		return strdup(path);
	return convert_to_png(path);
}

/* ---- Signal handling (same as main wawa) ---- */
static volatile sig_atomic_t gnome_sig_skip;
static volatile sig_atomic_t gnome_sig_toggle_pause;

static void
gnome_sig_handler(int sig)
{
	switch (sig) {
	case SIGUSR1: gnome_sig_skip         = 1; break;
	case SIGUSR2: gnome_sig_toggle_pause  = 1; break;
	case SIGHUP:  sig_rescan              = 1; break; /* global from wawa.c */
	}
}

static void
install_signal_handlers(void)
{
	struct sigaction sa = { .sa_handler = gnome_sig_handler, .sa_flags = SA_RESTART };
	sigemptyset(&sa.sa_mask);
	sigaction(SIGUSR1, &sa, NULL);
	sigaction(SIGUSR2, &sa, NULL);
	sigaction(SIGHUP,  &sa, NULL);
}

/* ---- Main entry point for GNOME backend ---- */
void
gnome_backend_run(int argc, char *argv[], const char *image_path,
                  const char *image_dir, int random_flag,
                  int interval_ms, int paused_flag)
{
	(void)argc; (void)argv;

	/* Verify GNOME background schema is available */
	{
		GSettingsSchemaSource *src = g_settings_schema_source_get_default();
		if (!src)
			die("gnome: GSettings schema source not available");
		GSettingsSchema *schema = g_settings_schema_source_lookup(
			src, "org.gnome.desktop.background", TRUE);
		if (!schema)
			die("gnome: org.gnome.desktop.background schema not found "
			    "(not a GNOME desktop?)");
		g_settings_schema_unref(schema);
	}

	install_signal_handlers();

	/* Set initial wallpaper */
	if (image_path) {
		char *compatible = gnome_ensure_compatible(image_path);
		gnome_set_wallpaper(compatible);
		free(compatible);
	}

	/* Set picture mode — pass "fill" as default mode.
	 * The actual mode is parsed by the caller and stored in
	 * the global; we need to pass it. For now use "fill". */
	gnome_set_picture_options("zoom");

	/* No interval: set once and wait for signals */
	if (interval_ms <= 0) {
		fprintf(stderr, "gnome: wallpaper set, waiting for signals...\n");
		while (1)
			pause();
		return;
	}

	/* Interval mode: timer loop */
	uint64_t next_switch_ms = (uint64_t)time(NULL) * 1000 + interval_ms;

	while (1) {
		/* Sleep 1 second at a time to remain responsive to signals */
		for (int i = 0; i < interval_ms / 1000 && !gnome_sig_skip; i++)
			sleep(1);

		/* Handle signals */
		if (gnome_sig_skip) {
			gnome_sig_skip = 0;
			if (!paused_flag && random_flag && image_dir) {
				char *next = pick_random_file(image_dir, 0, 0);
				char *compat = gnome_ensure_compatible(next);
				gnome_set_wallpaper(compat);
				free(compat);
				free(next);
			}
			next_switch_ms = (uint64_t)time(NULL) * 1000 + interval_ms;
			continue;
		}

		if (gnome_sig_toggle_pause) {
			gnome_sig_toggle_pause = 0;
			paused_flag = !paused_flag;
			if (!paused_flag)
				next_switch_ms = (uint64_t)time(NULL) * 1000 + interval_ms;
			fprintf(stderr, "gnome: %s\n",
				paused_flag ? "paused" : "resumed");
			continue;
		}

		/* sig_rescan (global) is set by SIGHUP handler;
		 * pick_random_file() checks it internally and re-scans */

		/* Timer expired — switch wallpaper */
		if (!paused_flag && random_flag && image_dir) {
			char *next = pick_random_file(image_dir, 0, 0);
			char *compat = gnome_ensure_compatible(next);
			gnome_set_wallpaper(compat);
			free(compat);
			free(next);
			next_switch_ms = (uint64_t)time(NULL) * 1000 + interval_ms;
		}
	}
}
