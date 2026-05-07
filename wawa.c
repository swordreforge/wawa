/* See LICENSE file for copyright and license details. */
#include <fcntl.h>
#include <stdbool.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client-protocol.h>
#include <wayland-client.h>
#include <linux/memfd.h>

#include "stbi_alloc.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"

#include "wlr-layer-shell-unstable-v1-protocol.h"

struct output {
	uint32_t registry_name;

	struct wl_output *wl;
	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *layer_surface;

	uint32_t width, height;
	uint32_t size, stride;

	bool configured;

	struct wl_list link;
};

/*
 * Must be valid when a single monitor is misconfigured
 * or during startup, after which the image is freed.
 */
struct {
	int width, height;
	unsigned char *data;
} image;

static struct wl_display *display;
static struct wl_registry *registry;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct wl_list outputs;

static const char *filename;

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

static void
image_crop(unsigned char *dst, struct output *output)
{
	struct {
		int width, height;
		int x, y;
	} crop;
	double factor;

	factor = fmin((double)image.width/output->width, (double)image.height/output->height);
	crop.x = (image.width - (output->width * factor)) / 2;
	crop.y = (image.height - (output->height * factor)) / 2;
	crop.width = output->width * factor;
	crop.height = output->height * factor;
	stbir_resize_uint8_linear(
	  image.data + (crop.y * image.width + crop.x) * 4, crop.width, crop.height, image.width * 4,
		dst, output->width, output->height, output->stride,
		4);
}

static struct wl_buffer *
output_load_image(struct output *output)
{
	int fd = -1;
	struct wl_shm_pool *shm_pool;
	struct wl_buffer *buffer;
	unsigned char *data;
	
	fd = memfd_create("drwbuf-shm-buffer-pool",
		MFD_CLOEXEC | MFD_ALLOW_SEALING | MFD_NOEXEC_SEAL);
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

	image_crop(data, output);

	/* RGBA->BGRA */
	for (int i = 0; i < output->size; i += 4) {
		data[i] ^= data[i+2];
		data[i+2] ^= data[i];
		data[i] ^= data[i+2];
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

	output->configured = width == output->width && height == output->height;
	if (output->configured) return;

	if (!image.data) {
		fputs("loaded image", stderr);
		if (!(image.data = stbi_load(filename,
		                       &image.width, &image.height, NULL, 4)))
			die("failed to load image: %s", stbi_failure_reason());
	}

	output->width = width;
	output->height = height;
	output->stride = width * 4;
	output->size = width * height * 4;

	buffer = output_load_image(output);
	wl_surface_attach(output->surface, buffer, 0, 0);
	wl_surface_damage_buffer(output->surface, 0, 0, INT32_MAX, INT32_MAX);
	wl_surface_commit(output->surface);
	wl_buffer_destroy(buffer);

	output->configured = true;
}

static void
layer_surface_handle_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
	struct output *output = data;
	zwlr_layer_surface_v1_destroy(output->layer_surface);
	wl_surface_destroy(output->surface);
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = layer_surface_handle_configure,
	.closed = layer_surface_handle_closed,
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
		compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 6);
 	else if (!strcmp(interface, wl_shm_interface.name))
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	else if (!strcmp(interface, zwlr_layer_shell_v1_interface.name))
		layer_shell = wl_registry_bind(registry, name,
			&zwlr_layer_shell_v1_interface, 1);
	else if (!strcmp(interface, wl_output_interface.name)) {
		struct output *output = calloc(1, sizeof(struct output));
		if (!output) die("calloc:");
		output->registry_name = name;
		output->wl = wl_registry_bind(registry, name,
			&wl_output_interface, 4);
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
	struct output *output, *tmp;
	
	wl_list_for_each_safe(output, tmp, &outputs, link) {
		if (output->registry_name != name)
			continue;	
		wl_output_destroy(output->wl);
		wl_list_remove(&output->link);
		free(output);
		break;
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_handle_global,
	.global_remove = registry_handle_remove,
};

int
main(int argc, char *argv[])
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s filename\n", argv[0]);
		return EXIT_FAILURE;
	}

	filename = argv[1];

	if (!(display = wl_display_connect(NULL)))
		die("failed to connect to wayland");

	wl_list_init(&outputs);

	registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
		
	if (!compositor || !layer_shell || !shm)
		die("bad compositor available");

	while (wl_display_dispatch(display))
		;

	return EXIT_SUCCESS;
}
