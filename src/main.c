#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "config.h"
#include "core.h"
#include "output.h"
#include "surface.h"

struct core core = {
	.running = true,
	.serial = 1,
	.xwayland = {.display = -1},
	.magnifier = {.zoom = 1.0},
};

// The signals are read in the event loop, which they wake up whichever thread
// they are sent to.
static int
handle_signal(int signo, void *data)
{
	if (signo == SIGHUP) {
		core.reload = true;
	} else {
		core.running = false;
	}
	return 0;
}

// Clients draw the same cursors as the compositor, which replaces the
// variables of the environment it was started with.
static void
set_cursor_environment(void)
{
	if (config.cursor.theme) {
		setenv("XCURSOR_THEME", config.cursor.theme, 1);
	} else {
		unsetenv("XCURSOR_THEME");
	}
	char cursor_size[16];
	snprintf(cursor_size, sizeof(cursor_size), "%d", config.cursor.size);
	setenv("XCURSOR_SIZE", cursor_size, 1);
}

// Sets the environment inherited by the spawned clients. WAYLAND_DISPLAY and
// DISPLAY are set when their sockets are created.
static void
environment_init(void)
{
	setenv("XDG_SESSION_TYPE", "wayland", 1);
	// For jimi -r
	char pid[16];
	snprintf(pid, sizeof(pid), "%d", (int)getpid());
	setenv("JIMI_PID", pid, 1);
	// The user may override these defaults.
	// xdg-desktop-portal selects its backends by XDG_CURRENT_DESKTOP.
	setenv("XDG_CURRENT_DESKTOP", "jimi", 0);
	// Java applications assume a reparenting window manager by default and
	// draw blank windows. See https://github.com/swaywm/sway/issues/595
	setenv("_JAVA_AWT_WM_NONREPARENTING", "1", 0);
	set_cursor_environment();
}

// Services activated by D-Bus, such as xdg-desktop-portal, are started by
// systemd or dbus-daemon instead of us, and take the variables from them.
static void
activation_environment_init(void)
{
	// Unset variables, such as DISPLAY without Xwayland, are skipped.
	if (system("dbus-update-activation-environment --systemd "
			"WAYLAND_DISPLAY DISPLAY XDG_CURRENT_DESKTOP") != 0) {
		fprintf(stderr, "Failed to update the activation environment\n");
	}
}

// Clears the variables, which would otherwise point the services activated
// after we exit to our closed sockets. dbus-daemon can only set them empty,
// but systemd has to unset them: wlroots compositors started by systemd take
// even an empty WAYLAND_DISPLAY as the parent compositor to nest in.
static void
activation_environment_fini(void)
{
	int dbus = system("dbus-update-activation-environment "
		"WAYLAND_DISPLAY= DISPLAY= XDG_CURRENT_DESKTOP=");
	int systemd = system("systemctl --user unset-environment "
		"WAYLAND_DISPLAY DISPLAY XDG_CURRENT_DESKTOP");
	if (dbus != 0 || systemd != 0) {
		fprintf(stderr, "Failed to update the activation environment\n");
	}
}

// Applies the config file again, unless it is invalid. Clients started before
// keep the environment, and the autostarted ones are not started again.
static void
reload(void)
{
	// The grid is laid out by the config.
	if (core.window_switcher.selected) {
		window_switcher_cancel();
	}
	// The input devices point to their configs until they are removed, and
	// are configured again when they are added back. Their pressed keys
	// are released meanwhile.
	input_suspend();
	size_t nr_workspaces = config.workspaces.nr_names;
	bool reloaded = config_reload();
	input_resume();
	if (!reloaded) {
		return;
	}
	set_cursor_environment();
	keyboard_reload();
	icon_themes_fini();
	icon_themes_init();
	// Draws the decorations again, with the icons of the theme.
	outputs_reload();
	night_light_update();
	workspaces_reload(nr_workspaces);
	fprintf(stderr, "Reloaded %s\n", config.path);
}

// Tells the jimi running the client to reload its config.
static int
send_reload(void)
{
	const char *pid = getenv("JIMI_PID");
	if (!pid || atoi(pid) <= 0) {
		fprintf(stderr, "JIMI_PID is not set to the pid of jimi\n");
		return 1;
	}
	if (kill(atoi(pid), SIGHUP) != 0) {
		perror("kill");
		return 1;
	}
	return 0;
}

static int
usage(const char *name)
{
	fprintf(stderr, "Usage: %s [-C config] [-c command] | -r\n", name);
	return 1;
}

int
main(int argc, char **argv)
{
	const char *config_path = NULL;
	char *client_cmd = NULL;
	int opt;
	while ((opt = getopt(argc, argv, "C:c:r")) != -1) {
		switch (opt) {
		case 'C':
			config_path = optarg;
			break;
		case 'c':
			client_cmd = optarg;
			break;
		case 'r':
			return send_reload();
		default:
			return usage(argv[0]);
		}
	}
	if (optind < argc) {
		return usage(argv[0]);
	}

	config_load(config_path);
	environment_init();
	icon_themes_init();

	// Writing to the pipes of clients that closed them fails instead.
	signal(SIGPIPE, SIG_IGN);
	raise_nofile_limit();

	// The backends add their fds to the event loop of the display.
	core.display = wl_display_create();
	assert(core.display);
	// The 4 KiB default fills up with the events of fast pointer motion
	// while a client is stalled for a moment, which disconnects it.
	wl_display_set_default_max_buffer_size(core.display, 1024 * 1024);
	core.wl_loop = wl_display_get_event_loop(core.display);
	// Blocked for the signalfds before the renderer starts its threads,
	// which inherit the blocked signals.
	const int signals[] = {SIGINT, SIGTERM, SIGHUP};
	struct wl_event_source *signal_sources[ARRAY_LEN(signals)];
	for (size_t i = 0; i < ARRAY_LEN(signals); i++) {
		signal_sources[i] = wl_event_loop_add_signal(core.wl_loop, signals[i],
			handle_signal, NULL);
		assert(signal_sources[i]);
	}
	session_init();
	kms_init();
	// The outputs get their gamma LUTs for the temperature once they are
	// configured.
	night_light_init();
	renderer_init();
	keyboard_init();
	wayland_init();
	// Needs the renderer to draw the images for the scales, and the lists of
	// wayland_init() to lay out the surfaces.
	outputs_init();
	// Starts at the center of the laid out outputs.
	pointer_init();
	input_init();
	// After the sockets are created, and before the autostarted clients,
	// which may activate the services.
	activation_environment_init();
	for (size_t i = 0; i < config.nr_autostart; i++) {
		spawn(config.autostart[i]);
	}
	if (client_cmd) {
		spawn(client_cmd);
	}

	renderer_repaint_outputs();
	while (core.running) {
		wl_display_flush_clients(core.display);
		// Stopping and continuing the process interrupts the wait.
		int r = wl_event_loop_dispatch(core.wl_loop, -1);
		assert(r >= 0 || errno == EINTR);
		// The devices are suspended while the session is inactive.
		if (core.reload && core.session.active) {
			core.reload = false;
			reload();
		}
		windows_flush_configures();
		// Before the outputs, which the cursor of the focused client enters.
		pointer_update_focus(0, false);
		surfaces_update_outputs();
		idle_update_inhibited();
		// After the updates above, which the frames show. The repaint
		// timer wakes up the loop when a frame is due.
		renderer_repaint_outputs();
		session_lock_send_locked();
	}

	activation_environment_fini();
	renderer_wait_idle();
	kms_restore();
	wayland_fini();
	renderer_fini();
	night_light_fini();
	kms_fini();
	input_fini();
	tablets_fini();
	keyboard_fini();
	pointer_fini();
	session_fini();
	for (size_t i = 0; i < ARRAY_LEN(signals); i++) {
		wl_event_source_remove(signal_sources[i]);
	}
	wl_display_destroy(core.display);
	icon_themes_fini();
	config_fini();
	return 0;
}
