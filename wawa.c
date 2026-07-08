/* See LICENSE file for copyright and license details. */
#include <byteswap.h>
#include <dirent.h>
#include <fcntl.h>
#include <getopt.h>
#include <math.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client-protocol.h>
#include <wayland-client.h>
#ifdef __linux__
#include <linux/memfd.h>
#endif

#include <sail/sail.h>
#include <sail-manip/sail-manip.h>
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"

#include "wlr-layer-shell-unstable-v1-protocol.h"

#define MAX(A, B) ((A) > (B) ? (A) : (B))
#define MIN(A, B) ((A) < (B) ? (A) : (B))
#define FADE_STEPS 16

struct output {
	struct wl_output *wl;
	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *layer_surface;

	int32_t x, y;
	uint32_t width, height;
	uint32_t size, stride;

	bool configured;

	/* cross-fade animation state */
	struct wl_callback *frame_cb;
	unsigned char *anim_old, *anim_new;
	int anim_left;
	struct wl_buffer *anim_buf;  /* pre-allocated SHM buffer (avoid per-frame memfd) */
	void *anim_shm;              /* persistent mmap of anim_buf's backing memory */

	struct wl_list link;
};

struct rect {
	int width, height;
	int x, y;
};

static struct wl_display *display;
static struct wl_registry *registry;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct wl_list outputs;

struct {
	char *path;
	char *dir;       /* directory for re-scanning (--random mode) */
	struct sail_image *sail_img;
	int width, height;
	unsigned char *data;
} image;

static uint32_t color;

/* Smart resolution check: if non-zero, skip images whose aspect ratio
 * deviates from the screen's by more than this fraction. */
static double smart_tol;

static struct {
	struct sail_image *sail_img;
	int width, height;
	unsigned char *data;
} next_image;

static int interval_ms;   /* 0 = no interval switching */
static int animating;     /* non-zero = transition in progress */
static int anim_step;     /* current frame (0..FADE_STEPS-1) */
static uint64_t next_switch_ms; /* monotonic ms for next switch */

static void image_fill(unsigned char *dst, struct output *output);
static void (*image_modify)(unsigned char *, struct output *) = image_fill;

static void noop_geometry(void *data, struct wl_output *wl_output,
	int32_t x, int32_t y, int32_t physical_width, int32_t physical_height,
	int32_t subpixel, const char *make, const char *model, int32_t transform) {}
static void noop_mode(void *data, struct wl_output *wl_output,
	uint32_t flags, int32_t width, int32_t height, int32_t refresh) {}
static void noop_done(void *data, struct wl_output *wl_output) {}
static void noop_scale(void *data, struct wl_output *wl_output, int32_t factor) {}
static void noop_name(void *data, struct wl_output *wl_output, const char *name) {}
static void noop_description(void *data, struct wl_output *wl_output, const char *description) {}

static const char *supported_exts[] = {
	"png", "jpg", "jpeg", "webp", "bmp", "gif",
	"tiff", "tif", "pcx", "tga", "ico", "cur",
	"jp2", "j2k", "avif", "heif", "heic",
	"jxl", "psd", "qoi", "hdr", "exr",
	"svg", "xbm", "xpm", "xwd",
	"pnm", "pgm", "ppm", "pbm", "pam",
	"fli", "flc", "wal",
	NULL
};

static int
scan_filter(const struct dirent *entry)
{
	const char *dot;
	size_t i;

	if (entry->d_type != DT_REG && entry->d_type != DT_LNK)
		return 0;

	dot = strrchr(entry->d_name, '.');
	if (!dot || !dot[1])
		return 0;
	dot++;

	for (i = 0; supported_exts[i] != NULL; i++)
		if (!strcasecmp(dot, supported_exts[i]))
			return 1;
	return 0;
}

static void
die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);

	if (fmt[0] && fmt[strlen(fmt)-1] == ':') {
		fputc(' ', stderr);
		perror(NULL);
	} else {
		fputc('\n', stderr);
	}

	exit(EXIT_FAILURE);
}

static bool
parse_color(const char *src)
{
	int len;

	if (src[0] == '#')
		src++;
	len = strlen(src);
	if (len != 6 && len != 8) return false;

	color = strtoul(src, NULL, 16);
	if (len == 6)
		color = (color << 8) | 0xFF;

	/* for colorspace conversion in output_image_load */
	color = bswap_32(color);
	return true;
}

static void
image_color(unsigned char *dst, struct output *output)
{
	for (size_t i = 0; i < output->size; i += 4)
		memcpy(dst + i, &color, 4);
}

static void
image_stretch(unsigned char *dst, struct output *output)
{
	stbir_resize_uint8_linear(
	  image.data, image.width, image.height, image.width * 4,
		dst, output->width, output->height, output->stride, 4);
}

static void
image_fit(unsigned char *dst, struct output *output)
{
	struct rect crop;
	double factor;

	factor = fmin((double)output->width/image.width, (double)output->height/image.height);
	crop.width = image.width * factor;
	crop.height = image.height * factor;
	crop.x = (output->width - crop.width) / 2;
	crop.y = (output->height - crop.height) / 2;
	stbir_resize_uint8_linear(
	  image.data, image.width, image.height, image.width * 4,
		dst + crop.y * output->stride + crop.x * 4,
		crop.width, crop.height, output->stride, 4);
}

static void
image_fill(unsigned char *dst, struct output *output)
{
	struct rect crop;
	double factor;

	factor = fmin((double)image.width/output->width, (double)image.height/output->height);
	crop.width = output->width * factor;
	crop.height = output->height * factor;
	crop.x = (image.width - crop.width) / 2;
	crop.y = (image.height - crop.height) / 2;
	stbir_resize_uint8_linear(
	  image.data + crop.y * image.width * 4 + crop.x * 4,
	  crop.width, crop.height, image.width * 4,
		dst, output->width, output->height, output->stride, 4);
}

static void
image_tile(unsigned char *dst, struct output *o)
{
	unsigned char *to, *src;
	uint16_t off_x, off_y, w, h;

	/* implementation shamelessly stolen from xwallpaper, MIT:
	 * 2025 Tobias Stoeckmann <tobias@stoeckmann.org> */
	for (off_y = 0; off_y < o->height; off_y += image.height) {
		h = (off_y + image.height > o->height) ? o->height - off_y : image.height;
		for (off_x = 0; off_x < o->width; off_x += image.width) {
			w = (off_x + image.width > o->width) ? o->width - off_x : image.width;
			for (int y = 0; y < h; y++) {
				to = dst + ((off_y + y) * o->stride);
				src = image.data + (y * image.width * 4);
				memcpy(to + (off_x * 4), src, w * 4);
			}
		}
	}
}

static void
image_spread(unsigned char *dst, struct output *output)
{
	/* i can only claim that i designated the exact ways to perform
	 * the calculations. */
	struct output *o;
	struct rect crop;
	int min_x, min_y, max_x, max_y;
	double scale;

	/* set initial value to account for incoming negative outputs */
	o = wl_container_of(outputs.next, o, link);
	max_x = (min_x = o->x) + (o->width);
	max_y = (min_y = o->y) + (o->height);

	wl_list_for_each(o, &outputs, link) {
		/* avoids some obscure compiler optimization ?? */
		int32_t ow = o->width, oh = o->height;
		min_x = MIN(min_x, o->x);
		min_y = MIN(min_y, o->y);
		max_x = MAX(o->x + ow, max_x);
		max_y = MAX(o->y + oh, max_y);
	}

	crop.width = max_x - min_x;
	crop.height = max_y - min_y;
	scale = 1.0 / fmax((double)crop.width / image.width, (double)crop.height / image.height);
	/* offset total bounding to center */
	crop.x = (output->x - min_x) * scale + (image.width - crop.width * scale) / 2;
	crop.y = (output->y - min_y) * scale + (image.height - crop.height * scale) / 2;
	crop.width = MIN(ceil(output->width * scale), image.width - crop.x);
	crop.height = MIN(ceil(output->height * scale), image.height - crop.y);

	stbir_resize_uint8_linear(
		image.data + (crop.y * image.width + crop.x) * 4,
		crop.width, crop.height, image.width * 4,
		dst, output->width, output->height, output->stride, 4);
}

static void
set_image_modify(const char *mode)
{
	if      (!strcmp(mode, "stretch")) image_modify = image_stretch;
	else if (!strcmp(mode, "fit"))     image_modify = image_fit;
	else if (!strcmp(mode, "fill"))    image_modify = image_fill;
	else if (!strcmp(mode, "tile"))    image_modify = image_tile;
	else if (!strcmp(mode, "spread"))  image_modify = image_spread;
	else die("unknown mode: %s (use fill|fit|spread|stretch|tile)", mode);
}

/* Force every pixel in a BGRA 32bpp image to fully opaque.
 * Wallpaper must never carry alpha — any pixel with A < 255 causes
 * the compositor (which uses premultiplied alpha internally) to
 * semi-transparently blend with stale BACKGROUND-layer content. */
static void
sail_force_opaque(struct sail_image *img)
{
	unsigned char *pixels = img->pixels;
	size_t n = (size_t)img->width * img->height * 4;
	for (size_t i = 3; i < n; i += 4)
		pixels[i] = 255;
}

/* Apply current image_modify mode using a source other than the global image */
static void
resize_to(unsigned char *dst, struct output *output,
          unsigned char *src, int sw, int sh)
{
	int old_w = image.width, old_h = image.height;
	unsigned char *old_data = image.data;
	image.width = sw; image.height = sh; image.data = src;
	image_modify(dst, output);
	image.width = old_w; image.height = old_h; image.data = old_data;
}

static uint64_t
now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static struct wl_buffer *
output_load_image(struct output *output)
{
	unsigned char *data;

	if (output->anim_left > 0 && output->anim_buf) {
		/* Fast path — use pre-allocated SHM buffer, zero syscalls */
		data = output->anim_shm;
		float a = 1.0f - (float)output->anim_left / FADE_STEPS;
		for (int i = 0; i < output->size; i += 4) {
			data[i+0] = output->anim_old[i+0] * (1.0f - a) +
			            output->anim_new[i+0] * a;
			data[i+1] = output->anim_old[i+1] * (1.0f - a) +
			            output->anim_new[i+1] * a;
			data[i+2] = output->anim_old[i+2] * (1.0f - a) +
			            output->anim_new[i+2] * a;
			data[i+3] = 255;
		}
		return output->anim_buf;
	}

	int fd = -1;
	struct wl_shm_pool *shm_pool;
	struct wl_buffer *buffer;

	fd = memfd_create("drwbuf-shm-buffer-pool",
		MFD_CLOEXEC | MFD_ALLOW_SEALING 
	#ifdef __linux__
		| MFD_NOEXEC_SEAL
	#endif
	);
	if (fd < 0) die("memfd_create:");

	if ((ftruncate(fd, output->size)) < 0) die("ftruncate:");

	data = mmap(NULL, output->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) die("mmap:");

	fcntl(fd, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL);

	shm_pool = wl_shm_create_pool(shm, fd, output->size);
	buffer = wl_shm_pool_create_buffer(shm_pool, 0,
		output->width, output->height, output->stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(shm_pool);
	close(fd);

	if (output->anim_left > 0) {
		/* cross-fade blend (fallback, no pre-allocated buffer) */
		float a = 1.0f - (float)output->anim_left / FADE_STEPS;
		for (int i = 0; i < output->size; i += 4) {
			data[i+0] = output->anim_old[i+0] * (1.0f - a) +
			            output->anim_new[i+0] * a;
			data[i+1] = output->anim_old[i+1] * (1.0f - a) +
			            output->anim_new[i+1] * a;
			data[i+2] = output->anim_old[i+2] * (1.0f - a) +
			            output->anim_new[i+2] * a;
			data[i+3] = 255;
		}
	} else {
		image_modify(data, output);
		/* Force full opacity — many image formats carry RGBA
		 * data with A < 255 which would composite semi‑transparent
		 * over whatever the compositor keeps behind the BACKGROUND
		 * layer (e.g. a stale framebuffer or scenefx cache). */
		for (int i = 3; i < output->size; i += 4)
			data[i] = 255;
	}

	munmap(data, output->size);
	return buffer;
}

static void
layer_surface_handle_configure(void *data, struct zwlr_layer_surface_v1 *layer_surface,
		uint32_t serial, uint32_t width, uint32_t height)
{
	struct wl_buffer *buffer;
	struct output *output = data;

	zwlr_layer_surface_v1_ack_configure(layer_surface, serial);

	if (output->configured && width == output->width && height == output->height)
		return;

	if (!image.data) {
		if (image.path) {
			struct sail_image *sail_img = NULL;
			sail_status_t status;

			status = sail_load_from_file(image.path, &sail_img);
			if (status != SAIL_OK) die("failed to load image");

			if (sail_img->pixel_format != SAIL_PIXEL_FORMAT_BPP32_BGRA) {
				struct sail_image *converted = NULL;
				status = sail_convert_image(sail_img, SAIL_PIXEL_FORMAT_BPP32_BGRA, &converted);
				if (status != SAIL_OK) {
					sail_destroy_image(sail_img);
					die("failed to convert image to RGBA");
				}
				sail_destroy_image(sail_img);
				sail_img = converted;
			}

			sail_force_opaque(sail_img);

			image.sail_img = sail_img;
			image.data = sail_img->pixels;
			image.width = sail_img->width;
			image.height = sail_img->height;
		}
	}

	output->width = width;
	output->height = height;
	output->stride = width * 4;
	output->size = width * height * 4;

	buffer = output_load_image(output);
	wl_surface_attach(output->surface, buffer, 0, 0);
	wl_surface_commit(output->surface);
	wl_buffer_destroy(buffer);

	output->configured = true;

	if (!image.path) return;
	wl_list_for_each(output, &outputs, link)
		if (!output->configured) return;

	if (interval_ms == 0) {
		sail_destroy_image(image.sail_img);
		image.data = NULL;
		image.sail_img = NULL;
	}

	if (interval_ms > 0 && !animating)
		next_switch_ms = now_ms() + interval_ms;
}

static void
layer_surface_handle_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
	struct output *output = data;

	if (output->anim_shm) {
		munmap(output->anim_shm, output->size);
	}
	if (output->anim_buf) {
		wl_buffer_destroy(output->anim_buf);
	}
	free(output->anim_old);
	free(output->anim_new);
	if (output->frame_cb) {
		wl_callback_destroy(output->frame_cb);
	}

	zwlr_layer_surface_v1_destroy(output->layer_surface);
	wl_surface_destroy(output->surface);
	wl_output_destroy(output->wl);
	wl_list_remove(&output->link);
	free(output);
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = layer_surface_handle_configure,
	.closed = layer_surface_handle_closed,
};

static void
frame_handle_done(void *data, struct wl_callback *callback, uint32_t time)
{
	struct output *output = data;
	wl_callback_destroy(callback);
	output->frame_cb = NULL;
}

static const struct wl_callback_listener frame_listener = {
	.done = frame_handle_done,
};

/* Probe image metadata and reject images whose aspect ratio deviates too
 * far from the screen's.  Uses sail_probe_file() which is lightweight
 * (no pixel decoding).  When screen dimensions aren't known yet (0),
 * passes everything — the first pick happens before outputs are
 * configured; subsequent interval picks do full validation. */
static int
image_probe_suitable(const char *path, int screen_w, int screen_h)
{
	if (smart_tol == 0.0 || screen_w == 0 || screen_h == 0)
		return 1;

	struct sail_image *info = NULL;
	sail_status_t st = sail_probe_file(path, &info, NULL);
	if (st != SAIL_OK)
		return 0;

	double img_ratio = (double)info->width / info->height;
	double scr_ratio = (double)screen_w / screen_h;

	sail_destroy_image(info);

	return fabs(img_ratio - scr_ratio) / scr_ratio <= smart_tol;
}

/* Pick a random image file from directory (re-scans each call).
 * When smart_tol is set, probes candidates and skips mismatched
 * aspect ratios, trying each file once before falling back. */
static char *
pick_random_file(const char *dir, int screen_w, int screen_h)
{
	struct dirent **namelist;
	int n = scandir(dir, &namelist, scan_filter, alphasort);
	if (n < 0) die("scandir %s:", dir);
	if (n == 0) die("no supported images in %s", dir);

	size_t dirlen = strlen(dir);
	while (dirlen > 1 && dir[dirlen - 1] == '/')
		dirlen--;

	int start = rand() % n;
	char *path = NULL;
	for (int i = 0; i < n; i++) {
		int idx = (start + i) % n;

		free(path);
		if (asprintf(&path, "%.*s/%s", (int)dirlen, dir, namelist[idx]->d_name) < 0)
			die("asprintf:");

		if (image_probe_suitable(path, screen_w, screen_h))
			break;
	}

	for (int i = 0; i < n; i++)
		free(namelist[i]);
	free(namelist);
	return path;
}

/* Load an image file into next_image (RGBA 32bpp) */
static void
load_next_image(const char *path)
{
	struct sail_image *img = NULL;
	sail_status_t status;

	status = sail_load_from_file(path, &img);
	if (status != SAIL_OK)
		die("failed to load image: %s", path);

	if (img->pixel_format != SAIL_PIXEL_FORMAT_BPP32_BGRA) {
		struct sail_image *conv = NULL;
		status = sail_convert_image(img, SAIL_PIXEL_FORMAT_BPP32_BGRA, &conv);
		if (status != SAIL_OK) {
			sail_destroy_image(img);
			die("failed to convert image to BGRA");
		}
		sail_destroy_image(img);
		img = conv;
	}

	sail_force_opaque(img);

	next_image.sail_img = img;
	next_image.data = img->pixels;
	next_image.width = img->width;
	next_image.height = img->height;
}

/* Render the current image (image.data) onto all outputs */
static void
render_static_frame(void)
{
	struct output *output;
	wl_list_for_each(output, &outputs, link) {
		if (!output->configured)
			continue;
		struct wl_buffer *buf = output_load_image(output);
		wl_surface_attach(output->surface, buf, 0, 0);
		wl_surface_damage(output->surface,
			0, 0, output->width, output->height);
		wl_surface_commit(output->surface);
		wl_buffer_destroy(buf);
	}
	wl_display_flush(display);
}

/* End transition: swap next_image → image, free old resources,
 * then commit a clean 100% new frame (the last animation frame
 * still has ~6% old blended in). */
static void
end_transition(void)
{
	struct output *output;

	/* free old image */
	sail_destroy_image(image.sail_img);

	/* swap next into current */
	image.sail_img = next_image.sail_img;
	image.data = next_image.data;
	image.width = next_image.width;
	image.height = next_image.height;
	memset(&next_image, 0, sizeof(next_image));

	/* free per-output animation buffers and reset */
	wl_list_for_each(output, &outputs, link) {
		if (output->anim_shm) {
			munmap(output->anim_shm, output->size);
			output->anim_shm = NULL;
		}
		if (output->anim_buf) {
			wl_buffer_destroy(output->anim_buf);
			output->anim_buf = NULL;
		}
		free(output->anim_old);
		free(output->anim_new);
		output->anim_old = NULL;
		output->anim_new = NULL;
		output->anim_left = 0;
		if (output->frame_cb) {
			wl_callback_destroy(output->frame_cb);
			output->frame_cb = NULL;
		}
	}

	animating = 0;
	anim_step = 0;

	/* flush final 100% new frame to erase any blend residue */
	render_static_frame();

	/* Free current image data — next transition will reload from image.path.
	 * This keeps steady-state memory near zero in --interval mode. */
	if (image.dir) {
		sail_destroy_image(image.sail_img);
		image.data = NULL;
		image.sail_img = NULL;
	}
}

/* Render one frame of the cross-fade transition for all outputs */
static void
render_transition_frame(void)
{
	struct output *output;

	wl_list_for_each(output, &outputs, link) {
		if (output->anim_left <= 0)
			continue;

		struct wl_buffer *buf = output_load_image(output);
		wl_surface_attach(output->surface, buf, 0, 0);

		/* When reusing the pre‑allocated SHM buffer we must tell the
		 * compositor the pixel contents changed; otherwise scenefx
		 * (and potentially other rendering pipelines) may cache the
		 * old content and the cross‑fade appears frozen.
		 *
		 * Use wl_surface_damage (v1, surface-local coords) rather
		 * than damage_buffer (v4+) for maximum compositor compat;
		 * wawa renders 1:1 so the coordinates are identical. */
		wl_surface_damage(output->surface,
			0, 0, output->width, output->height);

		if (output->frame_cb)
			wl_callback_destroy(output->frame_cb);
		output->frame_cb = wl_surface_frame(output->surface);
		wl_callback_add_listener(output->frame_cb, &frame_listener, output);

		wl_surface_commit(output->surface);
		/* Pre-allocated animation buffer is reused across frames;
		 * only destroy fallback buffers created on the fly. */
		if (buf != output->anim_buf)
			wl_buffer_destroy(buf);

		output->anim_left--;
	}
	anim_step++;
	wl_display_flush(display);
}

/* Begin a cross-fade transition from current image to a random one */
static void
start_transition(void)
{
	char *path = NULL;
	struct output *output;

	/* ensure we have an image; interval without path is invalid */
	if (!image.path)
		return;

	if (next_image.sail_img)
		end_transition(); /* finish lingering transition */

	/* pick and load new image, update image.path */
	if (image.dir) {
		/* Save old path for reloading the 'old' image if it was freed
		 * after the previous transition (steady-state memory saving). */
		char *old_path = image.path;

		/* Use the first output's dimensions for smart probing. */
		{	struct output *o = wl_container_of(outputs.next, o, link);
			image.path = pick_random_file(image.dir,
				o->configured ? (int)o->width : 0,
				o->configured ? (int)o->height : 0);
		}
		load_next_image(image.path);

		/* If image.data was freed (end_transition freed it), reload the
		 * old image so we can pre-render anim_old for cross-fade. */
		if (!image.data && old_path) {
			struct sail_image *img = NULL;
			sail_status_t st;
			st = sail_load_from_file(old_path, &img);
			if (st == SAIL_OK) {
				if (img->pixel_format != SAIL_PIXEL_FORMAT_BPP32_BGRA) {
					struct sail_image *conv = NULL;
					st = sail_convert_image(img, SAIL_PIXEL_FORMAT_BPP32_BGRA, &conv);
					if (st == SAIL_OK) { sail_destroy_image(img); img = conv; }
				}
				sail_force_opaque(img);
				image.sail_img = img;
				image.data = img->pixels;
				image.width = img->width;
				image.height = img->height;
			}
		}
		free(old_path);
	}

	/* pre-resize old and new into each output's animation buffers */
	wl_list_for_each(output, &outputs, link) {
		if (!output->configured)
			continue;
		output->anim_old = calloc(1, output->size);
		output->anim_new = calloc(1, output->size);
		if (!output->anim_old || !output->anim_new)
			die("calloc:");
		/* Letterbox areas (fit mode) must be opaque black, not
		 * transparent black.  calloc zeros everything (A=0), so
		 * force alpha to 255 before resize_to fills the center. */
		for (size_t i = 3; i < output->size; i += 4) {
			output->anim_old[i] = 255;
			output->anim_new[i] = 255;
		}
		resize_to(output->anim_old, output,
			image.data, image.width, image.height);
		resize_to(output->anim_new, output,
			next_image.data, next_image.width, next_image.height);
		output->anim_left = FADE_STEPS;
	}

	/* Pre-allocate SHM buffers for the animation — eliminates
	 * memfd_create/ftruncate/mmap/shm_pool per frame (8+ syscalls).
	 * Only for modes that fill the entire output (no letterbox).
	 * fit mode creates per-frame buffers because letterbox areas
	 * trigger compositor caching issues with reused buffers. */
	if (image_modify != image_fit) {
		wl_list_for_each(output, &outputs, link) {
			if (!output->configured)
				continue;
			int fd = memfd_create("wawa-anim",
				MFD_CLOEXEC | MFD_ALLOW_SEALING
			#ifdef __linux__
				| MFD_NOEXEC_SEAL
			#endif
			);
			if (fd < 0) die("memfd_create:");
			if (ftruncate(fd, output->size) < 0) die("ftruncate:");
			output->anim_shm = mmap(NULL, output->size,
				PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
			if (output->anim_shm == MAP_FAILED) die("mmap:");
			fcntl(fd, F_ADD_SEALS,
				F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL);

			struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, output->size);
			output->anim_buf = wl_shm_pool_create_buffer(pool, 0,
				output->width, output->height,
				output->stride, WL_SHM_FORMAT_ARGB8888);
			wl_shm_pool_destroy(pool);
			close(fd);
		}
	}

	animating = 1;
	anim_step = 0;

	render_transition_frame();
}

static void
output_handle_geometry(void *data, struct wl_output *wl_output,
	int32_t x, int32_t y, int32_t physical_width, int32_t physical_height,
	int32_t subpixel, const char *make, const char *model, int32_t transform)
{
	struct output *output = data;
	output->x = x;
	output->y = y;

	/* A output's geometry has changed, mark all outputs as misconfigured */
	wl_list_for_each(output, &outputs, link)
		output->configured = false;
}

static const struct wl_output_listener output_listener = {
	.geometry = output_handle_geometry,
	.mode = noop_mode,
	.done = noop_done,
	.scale = noop_scale,
	.name = noop_name,
	.description = noop_description,
};

static void
output_setup_callback(void *data, struct wl_callback *callback,
		uint32_t time)
{
	struct output *output = data;

	output->surface = wl_compositor_create_surface(compositor);
	
	output->layer_surface = zwlr_layer_shell_v1_get_layer_surface(layer_shell,
				output->surface, output->wl, ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, "wallpaper");
	zwlr_layer_surface_v1_set_anchor(output->layer_surface, 
		ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	zwlr_layer_surface_v1_set_exclusive_zone(output->layer_surface, -1);
	zwlr_layer_surface_v1_set_keyboard_interactivity(output->layer_surface, 0);
	zwlr_layer_surface_v1_add_listener(output->layer_surface, &layer_surface_listener, output);
			
	wl_surface_commit(output->surface);
	wl_callback_destroy(callback);
}

static const struct wl_callback_listener output_setup_listener = {
	.done = output_setup_callback,
};

static void
registry_handle_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version)
{
	struct wl_callback *callback;

	if (!strcmp(interface, wl_compositor_interface.name))
		compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 1);
 	else if (!strcmp(interface, wl_shm_interface.name))
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	else if (!strcmp(interface, zwlr_layer_shell_v1_interface.name))
		layer_shell = wl_registry_bind(registry, name,
			&zwlr_layer_shell_v1_interface, 1);
	else if (!strcmp(interface, wl_output_interface.name)) {
		struct output *output = calloc(1, sizeof(struct output));
		if (!output) die("calloc:");
		output->wl = wl_registry_bind(registry, name,
			&wl_output_interface, 1);
		wl_output_add_listener(output->wl, &output_listener, output);
		wl_list_insert(&outputs, &output->link);
		/*
		 * There is no gurantee of the registry order, ensure a callback is used
		 * to setup the output, which is going to be called in the next event
		 * iteration, after the checks for registry objects is completed.
		 */
		callback = wl_display_sync(display);
		wl_callback_add_listener(callback, &output_setup_listener, output);
	}
}

static void
registry_handle_remove(void *data, struct wl_registry *registry, uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_handle_global,
	.global_remove = registry_handle_remove,
};

int
main(int argc, char *argv[])
{
	int random_flag = 0, opt;

	static const struct option long_opts[] = {
		{"random",   no_argument,       NULL, 'r'},
		{"interval", required_argument, NULL, 'i'},
		{"smart",    optional_argument, NULL, 's'},
		{0, 0, 0, 0}
	};

	while ((opt = getopt_long(argc, argv, "ri:", long_opts, NULL)) != -1) {
		switch (opt) {
		case 'r':
			random_flag = 1;
			break;
		case 'i': {
			long sec = strtol(optarg, NULL, 10);
			if (sec <= 0) die("invalid interval: %s", optarg);
			interval_ms = sec * 1000;
			break;
		}
		case 's':
			smart_tol = optarg ? strtod(optarg, NULL) : 0.1;
			if (smart_tol <= 0 || smart_tol > 1)
				die("invalid tolerance: %s", optarg ? optarg : "0.1");
			break;
		default:
			goto usage;
		}
	}

	if (random_flag) {
		if (optind + 2 != argc)
			die("usage: %s [--interval N] [--smart[=tol]] --random "
			    "fill|fit|spread|stretch|tile <directory>", argv[0]);

		set_image_modify(argv[optind]);

		srand(time(NULL) ^ (getpid() << 16));
		image.path = pick_random_file(argv[optind + 1], 0, 0);
		image.dir = argv[optind + 1];
	} else if (optind + 1 == argc) {
		if (!parse_color(argv[optind])) goto usage;
		image_modify = image_color;
	} else if (optind + 2 == argc) {
		set_image_modify(argv[optind]);
		image.path = argv[optind + 1];
	} else {
		goto usage;
	}

	/* Initialize SAIL image library */
	sail_init();

	if (!(display = wl_display_connect(NULL)))
		die("failed to connect to wayland");

	wl_list_init(&outputs);

	registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	wl_display_roundtrip(display); /* output handlers */


	/* Flush the initial surface commits queued in output_setup_callback.
	 * Without this, the compositor never receives the layer surface
	 * creation and never sends a configure event, leaving the first
	 * image stuck in the queue indefinitely. */
	wl_display_flush(display);

	if (!compositor || !layer_shell || !shm)
		die("bad compositor available");

	if (interval_ms > 0)
		next_switch_ms = now_ms() + interval_ms;

	/* poll-based event loop: dispatches wayland events, fires timers,
	 * and drives cross-fade animation */
	while (1) {
		int timeout = -1;
		uint64_t now = now_ms();

		if (animating) {
			/* during transition, check if all outputs are ready */
			bool all_ready = true;
			struct output *o;
			wl_list_for_each(o, &outputs, link) {
				if (o->frame_cb != NULL) {
					all_ready = false;
					break;
				}
			}
			if (all_ready) {
				if (anim_step >= FADE_STEPS) {
					end_transition();
					if (interval_ms > 0)
						next_switch_ms = now_ms() + interval_ms;
				} else {
					render_transition_frame();
				}
				continue;
			}
			timeout = 16; /* ~60 fps poll while waiting for frame cb */
		} else if (interval_ms > 0 && next_switch_ms <= now) {
			start_transition();
			continue;
		} else if (interval_ms > 0) {
			int remaining = (int)(next_switch_ms - now);
			timeout = (remaining > 0) ? remaining : 0;
		}

		struct pollfd pfd = {
			.fd = wl_display_get_fd(display),
			.events = POLLIN
		};
		int ret = poll(&pfd, 1, timeout);

		if (ret > 0 && (pfd.revents & POLLIN)) {
			if (wl_display_dispatch(display) < 0)
				break;
			/* Flush outgoing requests queued by event handlers
			 * (e.g. wl_surface_commit in configure handler).
			 * Without this, commits pile up until the next
			 * dispatch call, which may be seconds away. */
			wl_display_flush(display);
		} else if (ret < 0) {
			break;
		}
	}

	return EXIT_SUCCESS;

usage:
	fprintf(stderr, "usage: %s [--interval <sec>] [--random] "
	                "[--smart[=<tol>]] "
	                "fill|fit|spread|stretch|tile <file|directory>\n"
	                "       %s RRGGBBAA\n",
	        argv[0], argv[0]);
	return EXIT_FAILURE;
}
