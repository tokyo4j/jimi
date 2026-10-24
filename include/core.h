#ifndef JIMI_CORE_H
#define JIMI_CORE_H

#include <time.h>

#include "backend.h"
#include "protocols.h"
#include "render.h"
#include "scene.h"
#include "seat.h"
#include "window.h"

// 32 hex digits and the terminator
#define ACTIVATION_TOKEN_SIZE 33

struct gbm_device;
struct icon_theme;
struct xwm;

struct core {
	// Cleared by SIGINT, SIGTERM and the quit binding to exit the main loop.
	bool running;
	// Set by SIGHUP and the reload binding to apply the config file again
	// in the main loop.
	bool reload;
	struct wl_display *display;
	struct wl_event_loop *wl_loop;
	struct session session;
	struct kms kms;
	struct input input;
	struct gbm_device *gbm;
	struct vk vk;
	// Wakes up the main loop when the next frame of an output is due.
	struct {
		int fd;
		struct wl_event_source *source;
		int64_t wakeup; // CLOCK_MONOTONIC ns, or 0 while disarmed
		// Predicted from the last wakeups, how late the outputs are
		// repainted after the timer expires, in ns.
		int64_t delay;
	} repaint_timer;
	// Warms the colors of the outputs at night.
	struct {
		int temperature; // Shown now, in K
		// Of the local time. Expires through the transitions, and else
		// at the next start or end of the night.
		int timer_fd;
		struct wl_event_source *timer_source;
	} night_light;
	// Magnifies the area around the pointer on the output under it, with
	// the cursor drawn into the images of the swapchain.
	struct {
		double zoom; // 1 while not magnifying
		// The lens of the image is copied into it, and drawn back
		// magnified. NULL until the first magnified frame.
		struct texture *texture;
	} magnifier;
	// The connected outputs, enabled or not.
	struct wl_list outputs; // output.link
	struct scene scene;
	struct wl_list surfaces; // surface.link
	// Waiting for their buffers or for earlier ones.
	struct wl_list transactions; // transaction.link, in commit order
	struct wl_list windows; // window.link, bottom-to-top
	struct {
		// Shown on all outputs, an index of config.workspaces.names
		int current;
		struct wl_list managers; // workspace_manager.link
	} workspaces;
	struct seat seat;
	// Hides the surfaces other than the lock surfaces, and keeps the input
	// from them.
	struct {
		bool locked;
		// The ext_session_lock_v1 that locked the session, or NULL. The
		// session stays locked if it is destroyed without unlocking,
		// until another client locks it.
		struct wl_resource *resource;
		// The resource has been told that the session is locked.
		bool locked_sent;
	} session_lock;
	struct {
		struct wl_list notifications; // idle_notification.link
		// zwp_idle_inhibitor_v1 resource links, with the inhibiting
		// surfaces or NULL once they are destroyed.
		struct wl_list inhibitors;
		// Some inhibiting surface is shown, which keeps the
		// notifications that honor the inhibitors from idling.
		bool inhibited;
	} idle;
	// Shows where a moved window snaps to when released, below the window.
	// Enabled after the pointer stays at snapping edges for a while.
	struct scene_rect snap_preview;
	struct {
		// The window raised above the others while switching windows,
		// or NULL if windows are not being switched.
		struct window *selected;
		// Around the frame of the selected window, below the grid
		struct scene_rect outline;
		// Enabled while switching windows. Shows the windows in a grid
		// in the middle of the output.
		struct scene_tree tree;
		struct scene_rect background;
		// window_switcher_item.link, top-to-bottom as when switching
		// started. Windows mapped since then are not listed.
		struct wl_list items;
		struct output *output; // Under the pointer when switching started
		// Shown on the close buttons of the items, for the scale of the
		// output.
		struct texture *close_icon;
		struct window_switcher_target hover; // Under the pointer
		// Where the left or middle button was pressed. Releasing it on
		// the same target clicks it.
		struct window_switcher_target press;
	} window_switcher;
	struct {
		struct wl_list windows; // window.foreign.link, in mapping order
		struct wl_list ext_lists; // ext_foreign_toplevel_list_v1 resource links
		struct wl_list wlr_managers; // zwlr_foreign_toplevel_manager_v1 resource links
		uint64_t next_identifier;
	} foreign_toplevel;
	struct wl_list server_decorations; // server_decoration.link
	// The xdg-activation token granted last, which a window can use once to
	// take the keyboard focus.
	struct {
		char name[ACTIVATION_TOKEN_SIZE]; // Empty if none
		time_t time; // CLOCK_MONOTONIC s, when granted
	} activation_token;
	struct {
		struct wl_list managers; // zwlr_output_manager_v1 resource links
		// Of the current state of the outputs. Configurations created
		// for an older one are cancelled.
		uint32_t serial;
	} output_management;
	struct {
		int display; // The X11 display number, or -1.
		// The listening sockets of the display, which Xwayland inherits
		// each time it starts.
		int x_fds[2];
		// Without persistence, they wait for an X11 client to start
		// Xwayland.
		struct wl_event_source *x_sources[2];
		struct wl_client *client; // NULL while Xwayland is not running.
		struct wl_listener client_destroy;
		struct wl_global *shell;
		// Until Xwayland is ready, which hands wm_fd over to the xwm
		struct wl_event_source *ready_source;
		int ready_fd;
		int wm_fd;
		struct xwm *xwm; // NULL until Xwayland is ready.
		struct wl_list surfaces; // xwayland_surface.link
	} xwayland;
	struct {
		// The directories to find icon themes in, and icons outside
		// themes in, by precedence.
		char **base_dirs;
		// The configured theme, and the fallback theme unless it is
		// configured.
		struct icon_theme *themes;
		int nr_themes;
	} icon_themes;
	struct dmabuf_feedback dmabuf_feedback;
	const char *socket;
	uint32_t serial;
};

extern struct core core;

#endif
