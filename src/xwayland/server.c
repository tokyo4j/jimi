#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "config.h"
#include "core.h"
#include "xwayland-shell-v1-protocol.h"

#define MAX_X11_DISPLAY 32
#define X11_SOCKET_DIR "/tmp/.X11-unix"
#define X11_SOCKET_FORMAT X11_SOCKET_DIR "/X%d"
#define X11_LOCK_FORMAT "/tmp/.X%d-lock"

// Binds a listening socket. An abstract address starts with a NUL byte.
static int
open_socket(struct sockaddr_un *addr, size_t path_length)
{
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	assert(fd >= 0);
	if (addr->sun_path[0]) {
		unlink(addr->sun_path);
	}
	socklen_t size = offsetof(struct sockaddr_un, sun_path) + path_length + 1;
	if (bind(fd, (struct sockaddr *)addr, size) < 0 || listen(fd, 1) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

// Takes the lock of the display unless a running process holds it.
static bool
lock_display(int display)
{
	char path[64];
	snprintf(path, sizeof(path), X11_LOCK_FORMAT, display);
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0444);
	if (fd < 0) {
		// Remove the lock of a process that is gone, and retry.
		char pid[12] = {0};
		fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd < 0) {
			return false;
		}
		ssize_t length = read(fd, pid, sizeof(pid) - 1);
		close(fd);
		if (length <= 0 || kill((pid_t)atoi(pid), 0) == 0 || errno != ESRCH
				|| unlink(path) != 0) {
			return false;
		}
		return lock_display(display);
	}
	// The lock contains the PID padded to 10 characters and a newline.
	char pid[12];
	snprintf(pid, sizeof(pid), "%10d\n", (int)getpid());
	bool written = write(fd, pid, 11) == 11;
	close(fd);
	return written;
}

static void
unlink_display(int display)
{
	char path[64];
	snprintf(path, sizeof(path), X11_SOCKET_FORMAT, display);
	unlink(path);
	snprintf(path, sizeof(path), X11_LOCK_FORMAT, display);
	unlink(path);
}

// Listens on the abstract and the file system sockets of the first free
// display.
static int
open_display(int fds[2])
{
	if (mkdir(X11_SOCKET_DIR, 01777) != 0 && errno != EEXIST) {
		return -1;
	}
	for (int display = 0; display <= MAX_X11_DISPLAY; display++) {
		if (!lock_display(display)) {
			continue;
		}
		struct sockaddr_un addr = {.sun_family = AF_UNIX};
		size_t length = (size_t)snprintf(addr.sun_path + 1,
			sizeof(addr.sun_path) - 1, X11_SOCKET_FORMAT, display);
		fds[0] = open_socket(&addr, length);
		length = (size_t)snprintf(addr.sun_path, sizeof(addr.sun_path),
			X11_SOCKET_FORMAT, display);
		fds[1] = open_socket(&addr, length);
		if (fds[0] >= 0 && fds[1] >= 0) {
			return display;
		}
		if (fds[0] >= 0) {
			close(fds[0]);
		}
		if (fds[1] >= 0) {
			close(fds[1]);
		}
		unlink_display(display);
	}
	return -1;
}

static void
inherit_fd(int fd, char string[static 16])
{
	assert(fcntl(fd, F_SETFD, 0) == 0);
	snprintf(string, 16, "%d", fd);
}

static void
exec_xwayland(int display, const int x_fds[2], int wl_fd, int wm_fd, int ready_fd)
{
	char display_name[16], listen_fds[2][16], wayland_socket[16];
	char wm[16], ready[16];
	snprintf(display_name, sizeof(display_name), ":%d", display);
	inherit_fd(x_fds[0], listen_fds[0]);
	inherit_fd(x_fds[1], listen_fds[1]);
	inherit_fd(wl_fd, wayland_socket);
	inherit_fd(wm_fd, wm);
	inherit_fd(ready_fd, ready);
	setenv("WAYLAND_SOCKET", wayland_socket, 1);
	prepare_exec();
	char *args[] = {"Xwayland", display_name, "-rootless", "-core",
		"-listenfd", listen_fds[0], "-listenfd", listen_fds[1],
		"-displayfd", ready, "-wm", wm,
		// Without persistence, Xwayland exits 10 seconds after the
		// last X11 client disconnects. With it, the arguments end here.
		config.backend.xwayland_persistence ? NULL : "-terminate", "10",
		NULL};
	execvp(args[0], args);
	perror("execvp Xwayland");
	_exit(127);
}

// Only Xwayland may associate its surfaces with X11 windows.
static bool
filter_global(const struct wl_client *client, const struct wl_global *global,
	void *data)
{
	return global != core.xwayland.shell || client == core.xwayland.client;
}

// Xwayland writes the display number once it accepts X11 clients, and closes
// the pipe without it if it fails to start.
static int
handle_ready(int fd, uint32_t mask, void *data)
{
	char buf[16];
	if (read(fd, buf, sizeof(buf)) <= 0) {
		wl_client_destroy(core.xwayland.client);
		return 0;
	}
	wl_event_source_remove(core.xwayland.ready_source);
	core.xwayland.ready_source = NULL;
	close(fd);
	xwm_init(core.xwayland.wm_fd);
	return 0;
}

static void serve_display(void);

// Stops serving the display, so that X11 clients fail to connect.
static void
close_display(void)
{
	for (int i = 0; i < 2; i++) {
		if (core.xwayland.x_sources[i]) {
			wl_event_source_remove(core.xwayland.x_sources[i]);
			core.xwayland.x_sources[i] = NULL;
		}
		close(core.xwayland.x_fds[i]);
	}
	unlink_display(core.xwayland.display);
	core.xwayland.display = -1;
}

// Xwayland exits once its Wayland client is destroyed, if it has not
// already. It is restarted unless it failed to start, as it would again.
static void
handle_client_destroy(struct wl_listener *listener, void *data)
{
	core.xwayland.client = NULL;
	if (core.xwayland.xwm) {
		xwm_fini();
		if (core.running) {
			serve_display();
		}
		return;
	}
	wl_event_source_remove(core.xwayland.ready_source);
	core.xwayland.ready_source = NULL;
	close(core.xwayland.ready_fd);
	close(core.xwayland.wm_fd);
	if (core.running) {
		fprintf(stderr, "Xwayland failed to start\n");
		close_display();
	}
}

// X11 clients may connect right away, and are served once Xwayland is ready.
static void
start(void)
{
	int wl_fds[2], wm_fds[2], ready_fds[2];
	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, wl_fds) == 0);
	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, wm_fds) == 0);
	assert(pipe(ready_fds) == 0);
	assert(fcntl(ready_fds[0], F_SETFD, FD_CLOEXEC) == 0);
	assert(fcntl(ready_fds[1], F_SETFD, FD_CLOEXEC) == 0);

	core.xwayland.client = wl_client_create(core.display, wl_fds[0]);
	assert(core.xwayland.client);
	core.xwayland.client_destroy.notify = handle_client_destroy;
	wl_client_add_destroy_listener(core.xwayland.client,
		&core.xwayland.client_destroy);
	core.xwayland.wm_fd = wm_fds[0];
	core.xwayland.ready_fd = ready_fds[0];
	core.xwayland.ready_source = wl_event_loop_add_fd(core.wl_loop,
		ready_fds[0], WL_EVENT_READABLE, handle_ready, NULL);
	assert(core.xwayland.ready_source);

	// Fork twice, so that Xwayland is not left as a zombie.
	pid_t pid = fork();
	assert(pid >= 0);
	if (pid == 0) {
		pid = fork();
		if (pid == 0) {
			exec_xwayland(core.xwayland.display, core.xwayland.x_fds,
				wl_fds[1], wm_fds[1], ready_fds[1]);
		}
		_exit(pid < 0);
	}
	assert(waitpid(pid, NULL, 0) == pid);
	close(wl_fds[1]);
	close(wm_fds[1]);
	close(ready_fds[1]);
}

static int
handle_connection(int fd, uint32_t mask, void *data)
{
	for (int i = 0; i < 2; i++) {
		wl_event_source_remove(core.xwayland.x_sources[i]);
		core.xwayland.x_sources[i] = NULL;
	}
	start();
	return 0;
}

// Starts Xwayland now with persistence, and otherwise once an X11 client
// connects.
static void
serve_display(void)
{
	if (config.backend.xwayland_persistence) {
		start();
		return;
	}
	for (int i = 0; i < 2; i++) {
		core.xwayland.x_sources[i] = wl_event_loop_add_fd(core.wl_loop,
			core.xwayland.x_fds[i], WL_EVENT_READABLE, handle_connection, NULL);
		assert(core.xwayland.x_sources[i]);
	}
}

void
xwayland_init(void)
{
	wl_list_init(&core.xwayland.surfaces);
	core.xwayland.display = open_display(core.xwayland.x_fds);
	if (core.xwayland.display < 0) {
		fprintf(stderr, "No free X11 display for Xwayland\n");
		return;
	}
	core.xwayland.shell = wl_global_create(core.display,
		&xwayland_shell_v1_interface, 1, NULL, wayland_bind_xwayland_shell);
	assert(core.xwayland.shell);
	wl_display_set_global_filter(core.display, filter_global, NULL);
	serve_display();

	char display_name[16];
	snprintf(display_name, sizeof(display_name), ":%d", core.xwayland.display);
	setenv("DISPLAY", display_name, 1);
	printf("DISPLAY=%s\n", display_name);
	fflush(stdout);
}

// The client of Xwayland is destroyed with the others before.
void
xwayland_fini(void)
{
	if (core.xwayland.display >= 0) {
		close_display();
	}
}
