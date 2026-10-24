#include <assert.h>
#include <inttypes.h>
#include <stdio.h>

#include "core.h"
#include "output.h"
#include "ext-foreign-toplevel-list-v1-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-protocol.h"

// Mapped windows have a handle for each list and manager resource. Handles
// have the window as user data until it unmaps, after which they are inert.
// Handles of the ext protocol have no requests to act on the window, but
// image capture sources are created from them.

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
wlr_handle_create(struct window *window, struct wl_resource *manager)
{
	struct wl_resource *resource = wl_resource_create(
		wl_resource_get_client(manager), &zwlr_foreign_toplevel_handle_v1_interface,
		wl_resource_get_version(manager), 0);
	assert(resource);
	wl_resource_set_implementation(resource, &wlr_handle_impl, window,
		resource_unlink);
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
