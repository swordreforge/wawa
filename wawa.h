/* wawa.h — shared declarations for wlr-layer-shell and GNOME backends */
#ifndef WAWA_H
#define WAWA_H

#include <dirent.h>
#include <signal.h>
#include <stdbool.h>
#include <sail/sail.h>

/* ---- Global image state ---- */
struct wawa_image {
	char *path;
	char *dir;
	struct sail_image *sail_img;
	int width, height;
	unsigned char *data;
};

extern struct wawa_image image;

extern double smart_tol;
extern int interval_ms;
extern int paused;

/* Signal-driven IPC flags */
extern volatile sig_atomic_t sig_skip_next;
extern volatile sig_atomic_t sig_toggle_pause;
extern volatile sig_atomic_t sig_rescan;

/* ---- Shared functions ---- */
void die(const char *fmt, ...);
bool parse_color(const char *src);
int scan_filter(const struct dirent *entry);
int image_probe_suitable(const char *path, int screen_w, int screen_h);
char *pick_random_file(const char *dir, int screen_w, int screen_h);
void sail_force_opaque(struct sail_image *img);
void sig_handler(int sig);

#endif /* WAWA_H */
