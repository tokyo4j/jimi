#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <unistd.h>

#include "core.h"
#include "output.h"
#include "ext-foreign-toplevel-list-v1-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-protocol.h"
#include "xfce-foreign-toplevel-management-private-v1-protocol.h"

// Mapped windows have a handle for each list and manager resource. Handles
// have the window as user data until it unmaps, after which they are inert.
// Handles of the ext protocol have no requests to act on the window, but
// image capture sources are created from them.
//
// The xfce protocol extends wlr handles with an xfce handle each, which has
// the wlr handle as user data until either becomes inert. Its events are
// followed by the done event of the wlr handle. Of its states, only sticky
// is supported.

static void
handle_close(struct wl_resource *resource)
{
	wl_resource_set_user_data(resource, NULL);
	wl_list_remove(wl_resource_get_link(resource));
	wl_list_init(wl_resource_get_link(resource));
}

static const struct ext_foreign_toplevel_handle_v1_interface ext_handle_impl = {
	.destroy = resource_destroy_req,
};

static void
ext_handle_create(struct window *window, struct wl_resource *list)
{
	struct wl_resource *resource = wl_resource_create(
		wl_resource_get_client(list), &ext_foreign_toplevel_handle_v1_interface,
		wl_resource_get_version(list), 0);
	assert(resource);
	wl_resource_set_implementation(resource, &ext_handle_impl, window,
		resource_unlink);
	list_append(&window->foreign.ext_handles, wl_resource_get_link(resource));
	ext_foreign_toplevel_list_v1_send_toplevel(list, resource);

	char identifier[17];
	snprintf(identifier, sizeof(identifier), "%016" PRIx64,
		window->foreign.identifier);
	ext_foreign_toplevel_handle_v1_send_identifier(resource, identifier);
	if (window->title) {
		ext_foreign_toplevel_handle_v1_send_title(resource, window->title);
	}
	if (window->app_id) {
		ext_foreign_toplevel_handle_v1_send_app_id(resource, window->app_id);
	}
	ext_foreign_toplevel_handle_v1_send_done(resource);
}

static void
wlr_handle_set_maximized_req(struct wl_client *client, struct wl_resource *resource)
{
	struct window *window = wl_resource_get_user_data(resource);
	if (window) {
		window_set_maximized(window, true);
	}
}

static void
wlr_handle_unset_maximized_req(struct wl_client *client,
	struct wl_resource *resource)
{
	struct window *window = wl_resource_get_user_data(resource);
	if (window) {
		window_set_maximized(window, false);
	}
}

static void
wlr_handle_set_minimized_req(struct wl_client *client, struct wl_resource *resource)
{
	struct window *window = wl_resource_get_user_data(resource);
	if (window) {
		window_set_minimized(window, true);
	}
}

static void
wlr_handle_unset_minimized_req(struct wl_client *client,
	struct wl_resource *resource)
{
	struct window *window = wl_resource_get_user_data(resource);
	if (window) {
		window_set_minimized(window, false);
	}
}

static void
wlr_handle_activate_req(struct wl_client *client,
	struct wl_resource *resource, struct wl_resource *seat)
{
	struct window *window = wl_resource_get_user_data(resource);
	if (window) {
		window_focus(window);
	}
}

static void
wlr_handle_close_req(struct wl_client *client, struct wl_resource *resource)
{
	struct window *window = wl_resource_get_user_data(resource);
	if (window) {
		window_close(window);
	}
}

static void
wlr_handle_set_rectangle_req(struct wl_client *client,
	struct wl_resource *resource, struct wl_resource *surface,
	int32_t x, int32_t y, int32_t width, int32_t height)
{
	if (width < 0 || height < 0) {
		wl_resource_post_error(resource,
			ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_ERROR_INVALID_RECTANGLE,
			"rectangle size must not be negative");
	}
	// The rectangle is only a target for minimize animations.
}

static void
wlr_handle_set_fullscreen_req(struct wl_client *client,
	struct wl_resource *resource, struct wl_resource *output)
{
	struct window *window = wl_resource_get_user_data(resource);
	if (window) {
		window_set_fullscreen(window, true);
	}
}

static void
wlr_handle_unset_fullscreen_req(struct wl_client *client,
	struct wl_resource *resource)
{
	struct window *window = wl_resource_get_user_data(resource);
	if (window) {
		window_set_fullscreen(window, false);
	}
}

static const struct zwlr_foreign_toplevel_handle_v1_interface wlr_handle_impl = {
	.set_maximized = wlr_handle_set_maximized_req,
	.unset_maximized = wlr_handle_unset_maximized_req,
	.set_minimized = wlr_handle_set_minimized_req,
	.unset_minimized = wlr_handle_unset_minimized_req,
	.activate = wlr_handle_activate_req,
	.close = wlr_handle_close_req,
	.set_rectangle = wlr_handle_set_rectangle_req,
	.destroy = resource_destroy_req,
	.set_fullscreen = wlr_handle_set_fullscreen_req,
	.unset_fullscreen = wlr_handle_unset_fullscreen_req,
};

// Maximization and fullscreen follow client commits. Activation and
// minimization apply immediately.
static void
wlr_handle_send_state(struct wl_resource *resource, struct window *window)
{
	uint32_t data[4];
	size_t nr_states = 0;
	if (window->current.maximized) {
		data[nr_states++] = ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MAXIMIZED;
	}
	if (window->minimized) {
		data[nr_states++] = ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED;
	}
	if (window->activated) {
		data[nr_states++] = ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED;
	}
	if (window->current.fullscreen && wl_resource_get_version(resource)
			>= ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN_SINCE_VERSION) {
		data[nr_states++] = ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN;
	}
	struct wl_array states = {
		.size = nr_states * sizeof(data[0]),
		.data = data,
	};
	zwlr_foreign_toplevel_handle_v1_send_state(resource, &states);
}

// The parent is referred to by its handle of the same client.
static void
wlr_handle_send_parent(struct wl_resource *resource, struct window *window)
{
	if (wl_resource_get_version(resource)
			< ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_PARENT_SINCE_VERSION) {
		return;
	}
	struct wl_client *client = wl_resource_get_client(resource);
	struct wl_resource *parent = NULL;
	if (window->parent) {
		struct wl_resource *handle;
		wl_resource_for_each(handle, &window->parent->foreign.wlr_handles) {
			if (wl_resource_get_client(handle) == client) {
				parent = handle;
				break;
			}
		}
	}
	zwlr_foreign_toplevel_handle_v1_send_parent(resource, parent);
}

// Windows have no output while no output is enabled.
static void
wlr_handle_send_output(struct wl_resource *resource, struct output *output,
	bool enter)
{
	if (!output) {
		return;
	}
	struct wl_client *client = wl_resource_get_client(resource);
	struct wl_resource *output_resource;
	wl_resource_for_each(output_resource, &output->resources) {
		if (wl_resource_get_client(output_resource) != client) {
			continue;
		}
		if (enter) {
			zwlr_foreign_toplevel_handle_v1_send_output_enter(resource,
				output_resource);
		} else {
			zwlr_foreign_toplevel_handle_v1_send_output_leave(resource,
				output_resource);
		}
	}
}

static struct wl_resource *
xfce_handle_from_wlr_handle(struct window *window, struct wl_resource *wlr_handle)
{
	struct wl_resource *resource;
	wl_resource_for_each(resource, &window->foreign.xfce_handles) {
		if (wl_resource_get_user_data(resource) == wlr_handle) {
			return resource;
		}
	}
	return NULL;
}

static void
wlr_handle_resource_destroy(struct wl_resource *resource)
{
	struct window *window = wl_resource_get_user_data(resource);
	if (window) {
		struct wl_resource *xfce_handle =
			xfce_handle_from_wlr_handle(window, resource);
		if (xfce_handle) {
			handle_close(xfce_handle);
		}
	}
	resource_unlink(resource);
}

static struct wl_resource *
wlr_handle_create(struct window *window, struct wl_resource *manager)
{
	struct wl_resource *resource = wl_resource_create(
		wl_resource_get_client(manager), &zwlr_foreign_toplevel_handle_v1_interface,
		wl_resource_get_version(manager), 0);
	assert(resource);
	wl_resource_set_implementation(resource, &wlr_handle_impl, window,
		wlr_handle_resource_destroy);
	list_append(&window->foreign.wlr_handles, wl_resource_get_link(resource));
	zwlr_foreign_toplevel_manager_v1_send_toplevel(manager, resource);
	return resource;
}

// The parent's handle has to exist before it is sent.
static void
wlr_handle_send_details(struct wl_resource *resource, struct window *window)
{
	if (window->title) {
		zwlr_foreign_toplevel_handle_v1_send_title(resource, window->title);
	}
	if (window->app_id) {
		zwlr_foreign_toplevel_handle_v1_send_app_id(resource, window->app_id);
	}
	wlr_handle_send_output(resource, window->output, true);
	wlr_handle_send_state(resource, window);
	wlr_handle_send_parent(resource, window);
	zwlr_foreign_toplevel_handle_v1_send_done(resource);
}

// NULL if inert
static struct window *
xfce_handle_window(struct wl_resource *resource)
{
	struct wl_resource *wlr_handle = wl_resource_get_user_data(resource);
	return wlr_handle ? wl_resource_get_user_data(wlr_handle) : NULL;
}

static void
xfce_handle_send_state(struct wl_resource *resource, struct window *window)
{
	uint32_t state = XFCE_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_STICKY;
	struct wl_array states = {
		.size = window->workspace == WORKSPACE_STICKY ? sizeof(state) : 0,
		.data = &state,
	};
	xfce_foreign_toplevel_handle_v1_send_state(resource, &states);
}

// Sticky windows are on no workspace. Clients without workspace handles get
// the workspace once they bind ext-workspace-v1.
static void
xfce_handle_send_workspace(struct wl_resource *resource, int workspace,
	bool enter)
{
	if (workspace == WORKSPACE_STICKY) {
		return;
	}
	struct wl_resource *workspace_resource =
		workspace_handle(wl_resource_get_client(resource), workspace);
	if (!workspace_resource) {
		return;
	}
	if (enter) {
		xfce_foreign_toplevel_handle_v1_send_workspace_enter(resource,
			workspace_resource);
	} else {
		xfce_foreign_toplevel_handle_v1_send_workspace_leave(resource,
			workspace_resource);
	}
}

// Only the largest image of the icon is kept, at scale 1.
static void
xfce_handle_send_icon(struct wl_resource *resource, struct window *window)
{
	struct toplevel_icon *icon = window->icon;
	if (!icon) {
		xfce_foreign_toplevel_handle_v1_send_no_icon(resource);
		return;
	}
	if (icon->name) {
		xfce_foreign_toplevel_handle_v1_send_icon_name(resource, icon->name);
	}
	if (icon->pixels) {
		xfce_foreign_toplevel_handle_v1_send_icon_size(resource,
			(uint32_t)icon->width, 1);
	}
}

static void
xfce_handle_set_sticky_req(struct wl_client *client, struct wl_resource *resource)
{
	struct window *window = xfce_handle_window(resource);
	if (window) {
		window_set_sticky(window, true);
	}
}

static void
xfce_handle_unset_sticky_req(struct wl_client *client,
	struct wl_resource *resource)
{
	struct window *window = xfce_handle_window(resource);
	if (window) {
		window_set_sticky(window, false);
	}
}

// Shading, stacking layers, highlighting, and moving or resizing without a
// button press are not supported.
static void
xfce_handle_noop_req(struct wl_client *client, struct wl_resource *resource)
{
}

static void
xfce_handle_seat_noop_req(struct wl_client *client, struct wl_resource *resource,
	struct wl_resource *seat)
{
}

// Moving a window to another output would need it placed there.
static void
xfce_handle_move_to_output_req(struct wl_client *client,
	struct wl_resource *resource, struct wl_resource *output)
{
}

// Sticky windows are on all workspaces, and stay there.
static void
xfce_handle_move_to_workspace_req(struct wl_client *client,
	struct wl_resource *resource, struct wl_resource *workspace_resource)
{
	struct window *window = xfce_handle_window(resource);
	int workspace = workspace_from_handle(workspace_resource);
	if (window && workspace >= 0 && window->workspace != WORKSPACE_STICKY) {
		window_set_workspace(window, workspace);
	}
}

static const struct xfce_foreign_toplevel_icon_pixels_v1_interface
icon_pixels_impl = {
	.destroy = resource_destroy_req,
};

// The pixels are premultiplied ARGB8888 without padding, as they are kept.
static void
xfce_handle_get_icon_pixels_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id, uint32_t size, uint32_t scale)
{
	struct wl_resource *pixels_resource = wl_resource_create(client,
		&xfce_foreign_toplevel_icon_pixels_v1_interface,
		wl_resource_get_version(resource), id);
	assert(pixels_resource);
	wl_resource_set_implementation(pixels_resource, &icon_pixels_impl, NULL, NULL);
	struct window *window = xfce_handle_window(resource);
	if (!window) {
		xfce_foreign_toplevel_icon_pixels_v1_send_failed(pixels_resource,
			XFCE_FOREIGN_TOPLEVEL_ICON_PIXELS_V1_FAILURE_REASON_UNKNOWN);
		return;
	}
	struct toplevel_icon *icon = window->icon;
	if (!icon || !icon->pixels) {
		xfce_foreign_toplevel_icon_pixels_v1_send_failed(pixels_resource,
			XFCE_FOREIGN_TOPLEVEL_ICON_PIXELS_V1_FAILURE_REASON_NO_PIXEL_DATA);
		return;
	}
	if (size != (uint32_t)icon->width || scale != 1) {
		xfce_foreign_toplevel_icon_pixels_v1_send_failed(pixels_resource,
			XFCE_FOREIGN_TOPLEVEL_ICON_PIXELS_V1_FAILURE_REASON_INVALID_ARGS);
		return;
	}
	uint32_t stride = (uint32_t)icon->width * 4;
	int fd = create_sealed_file("jimi-toplevel-icon", icon->pixels,
		(size_t)stride * (size_t)icon->height);
	xfce_foreign_toplevel_icon_pixels_v1_send_pixels(pixels_resource, fd,
		(uint32_t)icon->width, (uint32_t)icon->height, stride);
	close(fd);
}

static const struct xfce_foreign_toplevel_handle_v1_interface xfce_handle_impl = {
	.set_shaded = xfce_handle_noop_req,
	.unset_shaded = xfce_handle_noop_req,
	.set_sticky = xfce_handle_set_sticky_req,
	.unset_sticky = xfce_handle_unset_sticky_req,
	.set_above = xfce_handle_noop_req,
	.unset_above = xfce_handle_noop_req,
	.set_below = xfce_handle_noop_req,
	.unset_below = xfce_handle_noop_req,
	.highlight = xfce_handle_seat_noop_req,
	.unhighlight = xfce_handle_noop_req,
	.move = xfce_handle_seat_noop_req,
	.resize = xfce_handle_seat_noop_req,
	.move_to_output = xfce_handle_move_to_output_req,
	.move_to_workspace = xfce_handle_move_to_workspace_req,
	.get_icon_pixels = xfce_handle_get_icon_pixels_req,
	.destroy = resource_destroy_req,
};

void
foreign_toplevel_init(void)
{
	wl_list_init(&core.foreign_toplevel.windows);
	wl_list_init(&core.foreign_toplevel.ext_lists);
	wl_list_init(&core.foreign_toplevel.wlr_managers);
}

void
foreign_toplevel_map(struct window *window)
{
	// Each mapping is a new toplevel for the clients.
	window->foreign.identifier = ++core.foreign_toplevel.next_identifier;
	list_append(&core.foreign_toplevel.windows, &window->foreign.link);
	struct wl_resource *list;
	wl_resource_for_each(list, &core.foreign_toplevel.ext_lists) {
		ext_handle_create(window, list);
	}
	struct wl_resource *manager;
	wl_resource_for_each(manager, &core.foreign_toplevel.wlr_managers) {
		struct wl_resource *resource = wlr_handle_create(window, manager);
		wlr_handle_send_details(resource, window);
	}
}

void
foreign_toplevel_unmap(struct window *window)
{
	wl_list_remove(&window->foreign.link);
	wl_list_init(&window->foreign.link);
	struct wl_resource *resource, *tmp;
	wl_resource_for_each_safe(resource, tmp, &window->foreign.ext_handles) {
		ext_foreign_toplevel_handle_v1_send_closed(resource);
		handle_close(resource);
	}
	wl_resource_for_each_safe(resource, tmp, &window->foreign.wlr_handles) {
		zwlr_foreign_toplevel_handle_v1_send_closed(resource);
		handle_close(resource);
	}
	wl_resource_for_each_safe(resource, tmp, &window->foreign.xfce_handles) {
		handle_close(resource);
	}
}

void
foreign_toplevel_update_title(struct window *window)
{
	struct wl_resource *resource;
	wl_resource_for_each(resource, &window->foreign.ext_handles) {
		ext_foreign_toplevel_handle_v1_send_title(resource, window->title);
		ext_foreign_toplevel_handle_v1_send_done(resource);
	}
	wl_resource_for_each(resource, &window->foreign.wlr_handles) {
		zwlr_foreign_toplevel_handle_v1_send_title(resource, window->title);
		zwlr_foreign_toplevel_handle_v1_send_done(resource);
	}
}

void
foreign_toplevel_update_app_id(struct window *window)
{
	struct wl_resource *resource;
	wl_resource_for_each(resource, &window->foreign.ext_handles) {
		ext_foreign_toplevel_handle_v1_send_app_id(resource, window->app_id);
		ext_foreign_toplevel_handle_v1_send_done(resource);
	}
	wl_resource_for_each(resource, &window->foreign.wlr_handles) {
		zwlr_foreign_toplevel_handle_v1_send_app_id(resource, window->app_id);
		zwlr_foreign_toplevel_handle_v1_send_done(resource);
	}
}

void
foreign_toplevel_update_state(struct window *window)
{
	struct wl_resource *resource;
	wl_resource_for_each(resource, &window->foreign.wlr_handles) {
		wlr_handle_send_state(resource, window);
		zwlr_foreign_toplevel_handle_v1_send_done(resource);
	}
}

void
foreign_toplevel_update_parent(struct window *window)
{
	struct wl_resource *resource;
	wl_resource_for_each(resource, &window->foreign.wlr_handles) {
		wlr_handle_send_parent(resource, window);
		zwlr_foreign_toplevel_handle_v1_send_done(resource);
	}
}

// Sticky windows leave their workspace before they are told to be sticky,
// and enter one after they are told not to be.
void
foreign_toplevel_update_workspace(struct window *window, int old_workspace)
{
	struct wl_resource *resource;
	wl_resource_for_each(resource, &window->foreign.xfce_handles) {
		xfce_handle_send_workspace(resource, old_workspace, false);
		xfce_handle_send_state(resource, window);
		xfce_handle_send_workspace(resource, window->workspace, true);
		zwlr_foreign_toplevel_handle_v1_send_done(
			wl_resource_get_user_data(resource));
	}
}

void
foreign_toplevel_update_icon(struct window *window)
{
	struct wl_resource *resource;
	wl_resource_for_each(resource, &window->foreign.xfce_handles) {
		xfce_handle_send_icon(resource, window);
		zwlr_foreign_toplevel_handle_v1_send_done(
			wl_resource_get_user_data(resource));
	}
}

// A window is reported on its output alone.
void
foreign_toplevel_update_output(struct window *window, struct output *old_output)
{
	struct wl_resource *resource;
	wl_resource_for_each(resource, &window->foreign.wlr_handles) {
		wlr_handle_send_output(resource, old_output, false);
		wlr_handle_send_output(resource, window->output, true);
		zwlr_foreign_toplevel_handle_v1_send_done(resource);
	}
}

// Clients may bind the wl_output after they received the handles.
void
foreign_toplevel_output_bind(struct output *output,
	struct wl_resource *output_resource)
{
	struct wl_client *client = wl_resource_get_client(output_resource);
	struct window *window;
	wl_list_for_each(window, &core.foreign_toplevel.windows, foreign.link) {
		if (window->output != output) {
			continue;
		}
		struct wl_resource *resource;
		wl_resource_for_each(resource, &window->foreign.wlr_handles) {
			if (wl_resource_get_client(resource) == client) {
				zwlr_foreign_toplevel_handle_v1_send_output_enter(resource,
					output_resource);
				zwlr_foreign_toplevel_handle_v1_send_done(resource);
			}
		}
	}
}

// The workspaces of the windows are sent once the client has their handles.
void
foreign_toplevel_workspaces_bind(struct wl_client *client)
{
	struct window *window;
	wl_list_for_each(window, &core.foreign_toplevel.windows, foreign.link) {
		struct wl_resource *resource;
		wl_resource_for_each(resource, &window->foreign.xfce_handles) {
			if (wl_resource_get_client(resource) == client) {
				xfce_handle_send_workspace(resource, window->workspace, true);
				zwlr_foreign_toplevel_handle_v1_send_done(
					wl_resource_get_user_data(resource));
			}
		}
	}
}

static void
ext_list_stop_req(struct wl_client *client, struct wl_resource *resource)
{
	ext_foreign_toplevel_list_v1_send_finished(resource);
	wl_list_remove(wl_resource_get_link(resource));
	wl_list_init(wl_resource_get_link(resource));
}

static const struct ext_foreign_toplevel_list_v1_interface ext_list_impl = {
	.stop = ext_list_stop_req,
	.destroy = resource_destroy_req,
};

void
wayland_bind_ext_foreign_toplevel_list(struct wl_client *client, void *data,
	uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&ext_foreign_toplevel_list_v1_interface, MIN(version, 1), id);
	assert(resource);
	wl_resource_set_implementation(resource, &ext_list_impl, data, resource_unlink);
	list_append(&core.foreign_toplevel.ext_lists, wl_resource_get_link(resource));
	struct window *window;
	wl_list_for_each(window, &core.foreign_toplevel.windows, foreign.link) {
		ext_handle_create(window, resource);
	}
}

static void
wlr_manager_stop_req(struct wl_client *client, struct wl_resource *resource)
{
	zwlr_foreign_toplevel_manager_v1_send_finished(resource);
	wl_resource_destroy(resource);
}

static const struct zwlr_foreign_toplevel_manager_v1_interface
wlr_manager_impl = {
	.stop = wlr_manager_stop_req,
};

void
wayland_bind_wlr_foreign_toplevel_manager(struct wl_client *client,
	void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&zwlr_foreign_toplevel_manager_v1_interface, MIN(version, 3), id);
	assert(resource);
	wl_resource_set_implementation(resource, &wlr_manager_impl, data,
		resource_unlink);
	list_append(&core.foreign_toplevel.wlr_managers, wl_resource_get_link(resource));
	// Create all handles first, so that the parents can be sent.
	struct window *window;
	wl_list_for_each(window, &core.foreign_toplevel.windows, foreign.link) {
		wlr_handle_create(window, resource);
	}
	wl_list_for_each(window, &core.foreign_toplevel.windows, foreign.link) {
		// The handle created above is the last one.
		struct wl_resource *handle = wl_resource_from_link(
			window->foreign.wlr_handles.prev);
		wlr_handle_send_details(handle, window);
	}
}

// An xfce handle of an inert wlr handle is inert. The initial properties are
// followed by a second done event of the wlr handle.
static void
xfce_manager_get_handle_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id, struct wl_resource *wlr_handle)
{
	struct window *window = wl_resource_get_user_data(wlr_handle);
	if (window && xfce_handle_from_wlr_handle(window, wlr_handle)) {
		wl_resource_post_error(resource,
			XFCE_FOREIGN_TOPLEVEL_MANAGER_PRIVATE_V1_ERROR_ALREADY_BOUND,
			"the wlr handle already has an xfce handle");
		return;
	}
	struct wl_resource *xfce_handle = wl_resource_create(client,
		&xfce_foreign_toplevel_handle_v1_interface,
		wl_resource_get_version(resource), id);
	assert(xfce_handle);
	wl_resource_set_implementation(xfce_handle, &xfce_handle_impl,
		window ? wlr_handle : NULL, resource_unlink);
	if (!window) {
		wl_list_init(wl_resource_get_link(xfce_handle));
		return;
	}
	list_append(&window->foreign.xfce_handles, wl_resource_get_link(xfce_handle));
	xfce_handle_send_state(xfce_handle, window);
	xfce_handle_send_workspace(xfce_handle, window->workspace, true);
	xfce_handle_send_icon(xfce_handle, window);
	zwlr_foreign_toplevel_handle_v1_send_done(wlr_handle);
}

static const struct xfce_foreign_toplevel_manager_private_v1_interface
xfce_manager_impl = {
	.get_xfce_foreign_toplevel_handle = xfce_manager_get_handle_req,
	.destroy = resource_destroy_req,
};

void
wayland_bind_xfce_foreign_toplevel_manager(struct wl_client *client,
	void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&xfce_foreign_toplevel_manager_private_v1_interface, MIN(version, 1), id);
	assert(resource);
	wl_resource_set_implementation(resource, &xfce_manager_impl, data, NULL);
}
