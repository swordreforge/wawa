/* See LICENSE file for copyright and license details. */
#include <fcntl.h>
#include <stdbool.h>
#include <sys/mman.h>
#include <math.h>
#include <unistd.h>
#include <malloc.h>
#include <wayland-client-protocol.h>
#include <wayland-client.h>

#ifdef __linux__
#include <linux/memfd.h>
#endif

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "wlr-layer-shell-unstable-v1-protocol.h"
#include "viewporter-protocol.h"

struct output {
	uint32_t registry_name;

	struct wl_output *wl;
	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *layer_surface;
	struct wp_viewport *viewport;	

	bool configured;

	struct wl_list link;
};

static struct wl_display *display;
static struct wl_registry *registry;
static struct wl_compositor *compositor;
static struct wp_viewporter *viewporter;
static struct wl_shm *shm;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct wl_list outputs;

/*
 * Rather than create, resize, and maintain a image buffer for each
 * output available, use wp_viewport to scale and crop the image to the
 * output contents by having a full representation of the image.
 * This is created every time a set of monitors need to be drawn.
 */
struct image {
	struct wl_buffer *buffer;
	const char *filename;
	int32_t width, height, size;
	uint32_t *data;
} image;

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
image_cleanup_callback(void *data, struct wl_callback *callback, uint32_t id)
{
	struct output *output;
	wl_callback_destroy(callback);

	/* Already destroyed */
	if (!image.buffer) return;

	wl_list_for_each(output, &outputs, link)
		if (!output->configured) return;

	munmap(image.data, image.size);
	wl_buffer_destroy(image.buffer);
	image = (struct image){0};
}

static struct wl_callback_listener image_cleanup_callback_listener = {
	.done = image_cleanup_callback,
};

static void
load_image()
{
	int fd;
	struct wl_shm_pool *shm_pool;
	unsigned char *data;
	int32_t size;

	if (!(data = stbi_load(image.filename,
	                       &image.width, &image.height, NULL, 4)))
		die("failed to load image: %s", stbi_failure_reason());

	size = image.width * image.height * 4;
	
#if defined(__linux__) || \
	((defined(__FreeBSD__) && (__FreeBSD_version >= 1300048)))
	fd = memfd_create("wawa-shm-buffer",
		MFD_CLOEXEC | MFD_ALLOW_SEALING |
#if defined(MFD_NOEXEC_SEAL)
		MFD_NOEXEC_SEAL
#else
		0
#endif
	);
#else
	char template[] = "/tmp/wawa-XXXXXX";
#if defined(__OpenBSD__)
	fd = shm_mkstemp(template);
#else
	fd = mkostemp(template, O_CLOEXEC);
#endif
	if (fd < 0) die("mktemp:")
#if defined(__OpenBSD__)
	shm_unlink(template);
#else
	unlink(template);
#endif
#endif

	if ((ftruncate(fd, size)) < 0) {
		close(fd);
		die("ftruncate:");
	}

	image.data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (image.data == MAP_FAILED) {
		close(fd);
		die("mmap:");
	}

#if defined(__linux__) || \
	((defined(__FreeBSD__) && (__FreeBSD_version >= 1300048)))
	fcntl(fd, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL);
#endif

	shm_pool = wl_shm_create_pool(shm, fd, size);
	image.buffer = wl_shm_pool_create_buffer(shm_pool, 0,
		image.width, image.height, image.width * 4, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(shm_pool);
	close(fd);
	
  /* [R, G, B, A] -> ARGB8888 */    
	for (int i = 0; i < image.width * image.height; i++)
			image.data[i] =
				data[4 * i + 3] << 24 |
			  data[4 * i + 0] << 16 |
			  data[4 * i + 1] << 8 |
			  data[4 * i + 2]; 

	stbi_image_free(data);
}

static void
layer_surface_handle_configure(void *data, struct zwlr_layer_surface_v1 *layer_surface,
		uint32_t serial, uint32_t width, uint32_t height)
{
	struct wl_callback *callback;
	struct output *output = data;
	zwlr_layer_surface_v1_ack_configure(layer_surface, serial);

	/* Load the image only when necessary, and free when all monitors have
	 * been configured. */
	if (!image.buffer) load_image();

	/* Fit to screen */
	double factor = fmin((double)(image.width)/width, (double)(image.height)/height);
	wp_viewport_set_source(output->viewport,
	    wl_fixed_from_double((image.width - (width * factor)) / 2),
	    wl_fixed_from_double((image.height - (height * factor)) / 2),
	    wl_fixed_from_double(width * factor), wl_fixed_from_double(height * factor));

	wl_surface_attach(output->surface, image.buffer, 0, 0);
	wl_surface_damage_buffer(output->surface, 0, 0, INT32_MAX, INT32_MAX);
	wp_viewport_set_destination(output->viewport, width, height);
	wl_surface_commit(output->surface);

	output->configured = true;

	/* Niri doesn't actually tell the buffer that it is released or not.
	 * Use a callback.. */
	callback = wl_display_sync(display);
	wl_callback_add_listener(callback, &image_cleanup_callback_listener, NULL);
}

static void
layer_surface_handle_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
	struct output *output = data;
	zwlr_layer_surface_v1_destroy(output->layer_surface);
	wp_viewport_destroy(output->viewport);
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
	output->viewport = wp_viewporter_get_viewport(viewporter, output->surface);
	
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
	else if (!strcmp(interface, wp_viewporter_interface.name))
		viewporter = wl_registry_bind(registry, name, &wp_viewporter_interface, 1);
	else if (!strcmp(interface, zwlr_layer_shell_v1_interface.name))
		layer_shell = wl_registry_bind(registry, name,
			&zwlr_layer_shell_v1_interface, 1);
	else if (!strcmp(interface, wl_output_interface.name)) {
		struct output *output = calloc(1, sizeof(struct output));
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

	image.filename = argv[1];

	if (!(display = wl_display_connect(NULL)))
		die("failed to connect to wayland");

	wl_list_init(&outputs);

	registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
		
	if (!compositor || !layer_shell || !viewporter || !shm)
		die("bad compositor available");

	while (wl_display_dispatch(display))
		;

	return EXIT_SUCCESS;
}
