/*
 * omarchy-soft-brightnessd -- per-monitor "virtual brightness" for Wayland
 * compositors that implement wlr-layer-shell (Hyprland/Omarchy included).
 *
 * Why this exists: on some machines, DDC/CI brightness control for an
 * external monitor fails -- for example because the HDMI port is actually
 * driven via the SoC's DisplayPort-AUX tunnel (bus named "dpu_dp_aux"), and
 * that tunnel does not proxy DDC/CI read/write cycles correctly. This tool
 * works around that the same way some GNOME
 * extensions did: instead of asking the monitor's own backlight to change,
 * it draws a black, click-through overlay on top of ONE chosen output and
 * varies its opacity -- same trick, reimplemented for a wlr-layer-shell
 * compositor since GNOME's own extension APIs do not exist here.
 *
 * Two modes in one binary:
 *   omarchy-soft-brightnessd daemon <output-name>
 *       Connects to Wayland, maps the overlay on the named output only,
 *       and serves control commands on a per-output Unix socket until
 *       told to quit or the output disappears.
 *   omarchy-soft-brightnessd send <output-name> <SET n|GET|QUIT>
 *       One-shot client: talks to an already-running daemon for that
 *       output over its socket. Exit code 2 means no daemon is listening
 *       (the caller should start one with "daemon" and retry).
 *
 * The daemon never touches real monitor hardware and needs no privileges
 * beyond the user's own Wayland session.
 */

#define _POSIX_C_SOURCE 200809L

/* Never let the overlay go fully (or near-fully) opaque: a monitor the user
 * cannot read at all is worse than one that is merely dim, and is easy to
 * end up at by mistake (see the "100 used to mean fully black" incident).
 */
#define MIN_BRIGHTNESS_LEVEL 10

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <wayland-client.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "single-pixel-buffer-v1-client-protocol.h"
#include "viewporter-client-protocol.h"

typedef struct {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct zwlr_layer_shell_v1 *layer_shell;
	struct wp_single_pixel_buffer_manager_v1 *buffer_manager;
	struct wp_viewporter *viewporter;

	const char *target_output_name;
	struct wl_output *target_output;
	int target_found;

	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *layer_surface;
	struct wp_viewport *viewport;

	uint32_t surf_width, surf_height;
	int configured;
	int level; /* 0 (darkest) .. 100 (full brightness, no dimming) */

	int listen_fd;
	char *sock_path;
	char *state_path;

	int should_exit;
} app_t;

static app_t app;

static void render_buffer(void);
static void create_overlay(void);

/* ---- small filesystem helpers ---- */

static void mkdir_p(const char *dir)
{
	char tmp[PATH_MAX];
	snprintf(tmp, sizeof tmp, "%s", dir);
	size_t len = strlen(tmp);
	if (len && tmp[len - 1] == '/')
		tmp[len - 1] = '\0';
	for (char *p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			mkdir(tmp, 0755);
			*p = '/';
		}
	}
	mkdir(tmp, 0755);
}

static void sanitize_name(const char *in, char *out, size_t outlen)
{
	size_t i = 0;
	for (; in[i] && i + 1 < outlen; i++) {
		unsigned char c = (unsigned char)in[i];
		out[i] = (isalnum(c) || c == '_' || c == '-' || c == '.') ? (char)c : '_';
	}
	out[i] = '\0';
}

static char *build_sock_path(const char *output_name)
{
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	if (!runtime || !*runtime)
		runtime = "/tmp";
	char safe[256];
	sanitize_name(output_name, safe, sizeof safe);
	char *path = malloc(PATH_MAX);
	snprintf(path, PATH_MAX, "%s/omarchy-soft-brightness-%s.sock", runtime, safe);
	return path;
}

static char *build_state_path(const char *output_name)
{
	char base[PATH_MAX];
	const char *state_home = getenv("XDG_STATE_HOME");
	if (state_home && *state_home) {
		snprintf(base, sizeof base, "%s/omarchy-soft-brightness", state_home);
	} else {
		const char *home = getenv("HOME");
		if (!home || !*home)
			home = "/tmp";
		snprintf(base, sizeof base, "%s/.local/state/omarchy-soft-brightness", home);
	}
	mkdir_p(base);

	char safe[256];
	sanitize_name(output_name, safe, sizeof safe);
	char *path = malloc(PATH_MAX);
	snprintf(path, PATH_MAX, "%s/%s.level", base, safe);
	return path;
}

static int read_state(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return 100; /* no state saved yet: start at full brightness, no dimming */
	int v = 100;
	if (fscanf(f, "%d", &v) != 1)
		v = 100;
	fclose(f);
	if (v < MIN_BRIGHTNESS_LEVEL)
		v = MIN_BRIGHTNESS_LEVEL;
	if (v > 100)
		v = 100;
	return v;
}

static void write_state(const char *path, int v)
{
	FILE *f = fopen(path, "w");
	if (!f)
		return;
	fprintf(f, "%d\n", v);
	fclose(f);
}

/* ---- Wayland: buffer lifecycle ---- */

static void buffer_release(void *data, struct wl_buffer *buffer)
{
	(void)data;
	wl_buffer_destroy(buffer);
}

static const struct wl_buffer_listener buffer_listener = {
	.release = buffer_release,
};

static void render_buffer(void)
{
	if (!app.configured || !app.surface)
		return;

	uint32_t w = app.surf_width ? app.surf_width : 1;
	uint32_t h = app.surf_height ? app.surf_height : 1;

	if (!app.viewport)
		app.viewport = wp_viewporter_get_viewport(app.viewporter, app.surface);
	wp_viewport_set_destination(app.viewport, (int32_t)w, (int32_t)h);

	/* app.level is desired brightness (100 = full brightness, no overlay;
	 * 0 = darkest), so the overlay's own opacity is the complement. */
	double frac = (100 - app.level) / 100.0;
	if (frac < 0.0)
		frac = 0.0;
	if (frac > 1.0)
		frac = 1.0;
	uint32_t a = (uint32_t)(frac * (double)UINT32_MAX + 0.5);

	/* Plain black (r=g=b=0); only alpha carries the dimming amount. */
	struct wl_buffer *buf =
		wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer(app.buffer_manager, 0, 0, 0, a);
	wl_buffer_add_listener(buf, &buffer_listener, NULL);

	wl_surface_attach(app.surface, buf, 0, 0);
	wl_surface_damage(app.surface, 0, 0, INT32_MAX, INT32_MAX);
	wl_surface_commit(app.surface);
}

/* ---- Wayland: layer surface ---- */

static void layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *ls, uint32_t serial,
				     uint32_t width, uint32_t height)
{
	(void)data;
	zwlr_layer_surface_v1_ack_configure(ls, serial);
	app.surf_width = width;
	app.surf_height = height;
	app.configured = 1;
	render_buffer();
}

static void layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *ls)
{
	(void)data;
	(void)ls;
	/* Monitor unplugged or compositor tore us down: nothing to recover,
	 * the caller script will start a fresh daemon next time it is needed. */
	app.should_exit = 1;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = layer_surface_configure,
	.closed = layer_surface_closed,
};

static void create_overlay(void)
{
	app.surface = wl_compositor_create_surface(app.compositor);

	/* Click-through: an empty input region means pointer/touch/keyboard
	 * events fall through to whatever is below us. */
	struct wl_region *empty_region = wl_compositor_create_region(app.compositor);
	wl_surface_set_input_region(app.surface, empty_region);
	wl_region_destroy(empty_region);

	app.layer_surface = zwlr_layer_shell_v1_get_layer_surface(
		app.layer_shell, app.surface, app.target_output, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
		"omarchy-soft-brightness");

	zwlr_layer_surface_v1_set_anchor(app.layer_surface, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
								     ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
								     ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
								     ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	zwlr_layer_surface_v1_set_exclusive_zone(app.layer_surface, -1);
	zwlr_layer_surface_v1_set_size(app.layer_surface, 0, 0);
	zwlr_layer_surface_v1_set_keyboard_interactivity(app.layer_surface,
							  ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);

	zwlr_layer_surface_v1_add_listener(app.layer_surface, &layer_surface_listener, NULL);

	/* Required initial commit with no buffer attached; the compositor
	 * answers with the first configure event, which is what triggers
	 * render_buffer(). */
	wl_surface_commit(app.surface);
}

/* ---- Wayland: output discovery ---- */

static void output_name(void *data, struct wl_output *wl_output, const char *name)
{
	(void)data;
	if (app.target_found)
		return;
	if (strcmp(name, app.target_output_name) == 0) {
		app.target_found = 1;
		app.target_output = wl_output;
		create_overlay();
	}
}

static void output_geometry(void *data, struct wl_output *wl_output, int32_t x, int32_t y,
			     int32_t physical_width, int32_t physical_height, int32_t subpixel,
			     const char *make, const char *model, int32_t transform)
{
	(void)data;
	(void)wl_output;
	(void)x;
	(void)y;
	(void)physical_width;
	(void)physical_height;
	(void)subpixel;
	(void)make;
	(void)model;
	(void)transform;
}

static void output_mode(void *data, struct wl_output *wl_output, uint32_t flags, int32_t width,
			 int32_t height, int32_t refresh)
{
	(void)data;
	(void)wl_output;
	(void)flags;
	(void)width;
	(void)height;
	(void)refresh;
}

static void output_done(void *data, struct wl_output *wl_output)
{
	(void)data;
	(void)wl_output;
}

static void output_scale(void *data, struct wl_output *wl_output, int32_t factor)
{
	(void)data;
	(void)wl_output;
	(void)factor;
}

static void output_description(void *data, struct wl_output *wl_output, const char *description)
{
	(void)data;
	(void)wl_output;
	(void)description;
}

static const struct wl_output_listener output_listener = {
	.geometry = output_geometry,
	.mode = output_mode,
	.done = output_done,
	.scale = output_scale,
	.name = output_name,
	.description = output_description,
};

/* ---- Wayland: registry ---- */

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
			     const char *interface, uint32_t version)
{
	(void)data;
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		app.compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 1);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		app.layer_shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 1);
	} else if (strcmp(interface, wp_single_pixel_buffer_manager_v1_interface.name) == 0) {
		app.buffer_manager =
			wl_registry_bind(registry, name, &wp_single_pixel_buffer_manager_v1_interface, 1);
	} else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
		app.viewporter = wl_registry_bind(registry, name, &wp_viewporter_interface, 1);
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		uint32_t v = version < 4 ? version : 4; /* need v4 for the "name" event */
		struct wl_output *output = wl_registry_bind(registry, name, &wl_output_interface, v);
		wl_output_add_listener(output, &output_listener, NULL);
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
	(void)data;
	(void)registry;
	(void)name;
	/* If our target output is the one removed, the layer_surface's
	 * "closed" event fires separately and we exit from there. */
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

/* ---- control socket ---- */

/* Two "omarchy-soft-brightness" invocations can race to start a daemon for
 * the same output (e.g. a manual command and the panel's own refresh timer
 * landing a few milliseconds apart). Unconditionally unlinking the socket
 * path here would let a second daemon steal it out from under a first one
 * that is already serving it, leaving two live Wayland clients mapping two
 * overlapping overlays. Probe first: if something is listening, back off. */
static int socket_has_listener(const char *path)
{
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return 0;

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof addr);
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

	int ok = connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0;
	close(fd);
	return ok;
}

/* Serializes the check-then-bind sequence below across racing daemons for
 * the same output. Held for the process's whole lifetime; released
 * automatically when it exits, however it exits. */
static void acquire_startup_lock(const char *sock_path)
{
	char lock_path[PATH_MAX];
	snprintf(lock_path, sizeof lock_path, "%s.lock", sock_path);

	int fd = open(lock_path, O_CREAT | O_RDWR, 0600);
	if (fd < 0)
		return; /* can't lock; fall back to the connect()-based check alone */

	if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
		fprintf(stderr, "omarchy-soft-brightnessd: another daemon for this output is starting up\n");
		exit(2);
	}
}

static int setup_socket(const char *path)
{
	acquire_startup_lock(path);

	if (socket_has_listener(path)) {
		fprintf(stderr, "omarchy-soft-brightnessd: a daemon for this output is already running\n");
		exit(2);
	}

	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("socket");
		exit(1);
	}

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof addr);
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

	unlink(path); /* drop a stale socket left by a previous crashed run */

	if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
		perror("bind");
		exit(1);
	}
	if (listen(fd, 8) < 0) {
		perror("listen");
		exit(1);
	}
	return fd;
}

static void handle_client(void)
{
	int cfd = accept(app.listen_fd, NULL, NULL);
	if (cfd < 0)
		return;

	char buf[256];
	ssize_t n = read(cfd, buf, sizeof(buf) - 1);
	if (n <= 0) {
		close(cfd);
		return;
	}
	buf[n] = '\0';
	for (ssize_t i = n - 1; i >= 0 && (buf[i] == '\n' || buf[i] == '\r'); i--)
		buf[i] = '\0';

	char reply[64];
	if (strncmp(buf, "SET ", 4) == 0) {
		int v = atoi(buf + 4);
		if (v < MIN_BRIGHTNESS_LEVEL)
			v = MIN_BRIGHTNESS_LEVEL;
		if (v > 100)
			v = 100;
		app.level = v;
		render_buffer();
		write_state(app.state_path, v);
		snprintf(reply, sizeof reply, "OK %d\n", v);
	} else if (strcmp(buf, "GET") == 0) {
		snprintf(reply, sizeof reply, "%d\n", app.level);
	} else if (strcmp(buf, "QUIT") == 0) {
		snprintf(reply, sizeof reply, "OK\n");
		app.should_exit = 1;
	} else {
		snprintf(reply, sizeof reply, "ERR\n");
	}

	size_t len = strlen(reply);
	size_t off = 0;
	while (off < len) {
		ssize_t w = write(cfd, reply + off, len - off);
		if (w <= 0)
			break;
		off += (size_t)w;
	}
	close(cfd);
}

/* ---- daemon main loop ---- */

static int run_daemon(void)
{
	if (!app.target_found) {
		fprintf(stderr, "omarchy-soft-brightnessd: output '%s' not found\n",
			app.target_output_name);
		return 1;
	}

	app.sock_path = build_sock_path(app.target_output_name);
	app.state_path = build_state_path(app.target_output_name);
	app.level = read_state(app.state_path);

	app.listen_fd = setup_socket(app.sock_path);

	int wl_fd = wl_display_get_fd(app.display);

	while (!app.should_exit) {
		while (wl_display_prepare_read(app.display) != 0)
			wl_display_dispatch_pending(app.display);
		wl_display_flush(app.display);

		struct pollfd pfds[2] = {
			{.fd = wl_fd, .events = POLLIN, .revents = 0},
			{.fd = app.listen_fd, .events = POLLIN, .revents = 0},
		};

		int ret = poll(pfds, 2, -1);
		if (ret < 0) {
			wl_display_cancel_read(app.display);
			if (errno == EINTR)
				continue;
			break;
		}

		if (pfds[0].revents & POLLIN)
			wl_display_read_events(app.display);
		else
			wl_display_cancel_read(app.display);

		wl_display_dispatch_pending(app.display);

		if (pfds[1].revents & POLLIN)
			handle_client();
	}

	unlink(app.sock_path);
	if (app.layer_surface)
		zwlr_layer_surface_v1_destroy(app.layer_surface);
	if (app.surface)
		wl_surface_destroy(app.surface);
	wl_display_disconnect(app.display);
	return 0;
}

/* ---- one-shot client ("send") mode ---- */

static int run_send(const char *output_name, const char *command)
{
	char *path = build_sock_path(output_name);

	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		free(path);
		return 1;
	}

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof addr);
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
	free(path);

	if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
		close(fd);
		return 2; /* no daemon listening for this output */
	}

	char msg[128];
	snprintf(msg, sizeof msg, "%s\n", command);
	if (write(fd, msg, strlen(msg)) < 0) {
		close(fd);
		return 1;
	}

	char buf[256];
	ssize_t n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n > 0) {
		buf[n] = '\0';
		fputs(buf, stdout);
	}
	return 0;
}

int main(int argc, char **argv)
{
	signal(SIGPIPE, SIG_IGN);

	if (argc >= 3 && strcmp(argv[1], "daemon") == 0) {
		memset(&app, 0, sizeof app);
		app.target_output_name = argv[2];

		app.display = wl_display_connect(NULL);
		if (!app.display) {
			fprintf(stderr, "omarchy-soft-brightnessd: cannot connect to Wayland display\n");
			return 1;
		}

		app.registry = wl_display_get_registry(app.display);
		wl_registry_add_listener(app.registry, &registry_listener, NULL);
		wl_display_roundtrip(app.display); /* receive globals, incl. wl_output objects */
		wl_display_roundtrip(app.display); /* flush per-output events (name, etc.) */

		if (!app.compositor || !app.layer_shell || !app.buffer_manager || !app.viewporter) {
			fprintf(stderr,
				"omarchy-soft-brightnessd: compositor is missing a required protocol "
				"(wlr-layer-shell / single-pixel-buffer / viewporter)\n");
			return 1;
		}

		return run_daemon();
	}

	if (argc >= 4 && strcmp(argv[1], "send") == 0) {
		char command[128] = {0};
		for (int i = 3; i < argc; i++) {
			if (i > 3)
				strncat(command, " ", sizeof(command) - strlen(command) - 1);
			strncat(command, argv[i], sizeof(command) - strlen(command) - 1);
		}
		return run_send(argv[2], command);
	}

	fprintf(stderr,
		"usage:\n"
		"  %s daemon <output-name>\n"
		"  %s send <output-name> <SET n|GET|QUIT>\n",
		argv[0], argv[0]);
	return 1;
}
