#include <assert.h>
#include <stdlib.h>

#include "core.h"
#include "ext-session-lock-v1-protocol.h"
#include "output.h"
#include "surface.h"

// While the session is locked, the scene shows only the lock layer over the
// background color, and only the lock surfaces take the input. The session is
// locked by one client at a time, which is told so once the outputs show it.

static bool
mapped_lock_surface(struct surface *surface)
{
	return surface && surface->mapped && surface->role == SURFACE_ROLE_LOCK_SURFACE;
}

// The lock surface that takes the keyboard focus instead of the surface while
// the session is locked: the surface itself if it is one, or else the focused
// one, or else any mapped one. NULL if none is mapped.
struct surface *
session_lock_focus(struct surface *surface)
{
	if (mapped_lock_surface(surface)) {
		return surface;
	}
	if (mapped_lock_surface(core.seat.keyboard.focus)) {
		return core.seat.keyboard.focus;
	}
	struct output *output;
	wl_list_for_each(output, &core.outputs, link) {
		if (output->lock_surface && output->lock_surface->surface->mapped) {
			return output->lock_surface->surface;
		}
	}
	return NULL;
}

// Leaves the output to the background color. The resource stays inert.
void
lock_surface_destroy(struct lock_surface *lock_surface)
{
	struct surface *surface = lock_surface->surface;
	surface_set_mapped(surface, false);
	wl_resource_set_user_data(lock_surface->resource, NULL);
	lock_surface->output->lock_surface = NULL;
	scene_node_detach(&surface->scene.tree.node);
	surface->lock_surface = NULL;
	surface->role = SURFACE_ROLE_NONE;
	free(lock_surface);
}

static void
lock_surfaces_destroy(void)
{
	struct output *output;
	wl_list_for_each(output, &core.outputs, link) {
		if (output->lock_surface) {
			lock_surface_destroy(output->lock_surface);
		}
	}
}

// The lock surface covers the output, at its size.
void
output_arrange_lock_surface(struct output *output)
{
	struct lock_surface *lock_surface = output->lock_surface;
	if (!lock_surface) {
		return;
	}
	scene_node_set_position(&lock_surface->surface->scene.tree.node,
		output->box.x, output->box.y);
	if (output->box.width != lock_surface->configured_width
			|| output->box.height != lock_surface->configured_height) {
		ext_session_lock_surface_v1_send_configure(lock_surface->resource,
			++core.serial, (uint32_t)output->box.width,
			(uint32_t)output->box.height);
		lock_surface->configured_width = output->box.width;
		lock_surface->configured_height = output->box.height;
	}
}

bool
lock_surface_validate(struct lock_surface *lock_surface)
{
	if (!lock_surface->acked) {
		wl_resource_post_error(lock_surface->resource,
			EXT_SESSION_LOCK_SURFACE_V1_ERROR_COMMIT_BEFORE_FIRST_ACK,
			"lock surface committed before the first configure is acked");
		return false;
	}
	if (!surface_has_buffer(lock_surface->surface)) {
		wl_resource_post_error(lock_surface->resource,
			EXT_SESSION_LOCK_SURFACE_V1_ERROR_NULL_BUFFER,
			"lock surface committed without a buffer");
		return false;
	}
	return true;
}

// Lock surfaces always have a buffer once committed. The first one mapped
// takes the keyboard focus.
void
lock_surface_commit(struct lock_surface *lock_surface)
{
	struct surface *surface = lock_surface->surface;
	if (surface->mapped) {
		return;
	}
	surface_set_mapped(surface, true);
	if (!core.seat.keyboard.focus) {
		keyboard_focus(surface);
	}
}

static void
lock_surface_resource_destroy(struct wl_resource *resource)
{
	struct lock_surface *lock_surface = wl_resource_get_user_data(resource);
	if (lock_surface) {
		lock_surface_destroy(lock_surface);
	}
}

static void
lock_surface_ack_configure_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t serial)
{
	struct lock_surface *lock_surface = wl_resource_get_user_data(resource);
	if (lock_surface) {
		lock_surface->acked = true;
	}
}

static const struct ext_session_lock_surface_v1_interface lock_surface_impl = {
	.destroy = resource_destroy_req,
	.ack_configure = lock_surface_ack_configure_req,
};

static void
set_layers_enabled(bool enabled)
{
	for (int layer = 0; layer < SCENE_LAYER_LOCK; layer++) {
		scene_node_set_enabled(&core.scene.layers[layer].node, enabled);
	}
}

// Takes the input from the other surfaces, and hides them. Even the outputs
// that show nothing else draw a frame, which tells the client that the
// session is locked.
static void
session_lock(void)
{
	core.session_lock.locked = true;
	set_layers_enabled(false);
	if (core.window_switcher.selected) {
		window_switcher_cancel();
	}
	if (core.seat.pointer.grab.type != POINTER_GRAB_NONE) {
		pointer_cancel_grab();
	}
	// Closes the popups of a grab, as a press outside of them does.
	xdg_popup_handle_press(NULL);
	keyboard_focus(NULL);
	struct output *output;
	wl_list_for_each(output, &core.outputs, link) {
		if (output->enabled) {
			damage_output(output);
		}
	}
}

static void
session_unlock(void)
{
	core.session_lock.locked = false;
	lock_surfaces_destroy();
	set_layers_enabled(true);
	keyboard_update_focus();
}

// Tells the client that the session is locked once every enabled output shows
// a frame prepared while it is. Called once per event loop iteration, after
// the page flips.
void
session_lock_send_locked(void)
{
	struct wl_resource *resource = core.session_lock.resource;
	if (!resource || core.session_lock.locked_sent) {
		return;
	}
	struct output *output;
	wl_list_for_each(output, &core.outputs, link) {
		if (output->enabled && !output->current.locked) {
			return;
		}
	}
	ext_session_lock_v1_send_locked(resource);
	core.session_lock.locked_sent = true;
}

static void
lock_resource_destroy(struct wl_resource *resource)
{
	if (resource == core.session_lock.resource) {
		core.session_lock.resource = NULL;
	}
}

static void
lock_destroy_req(struct wl_client *client, struct wl_resource *resource)
{
	if (resource == core.session_lock.resource && core.session_lock.locked_sent) {
		wl_resource_post_error(resource, EXT_SESSION_LOCK_V1_ERROR_INVALID_DESTROY,
			"the session is locked, which requires unlock_and_destroy");
		return;
	}
	wl_resource_destroy(resource);
}

// Lock surfaces of a finished lock, or for a disabled output, stay inert.
static void
lock_get_lock_surface_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id,
	struct wl_resource *surface_resource, struct wl_resource *output_resource)
{
	struct surface *surface = wl_resource_get_user_data(surface_resource);
	struct output *output = wl_resource_get_user_data(output_resource);
	bool active = resource == core.session_lock.resource && output;
	if (surface->role != SURFACE_ROLE_NONE) {
		wl_resource_post_error(resource, EXT_SESSION_LOCK_V1_ERROR_ROLE,
			"wl_surface already has another role");
		return;
	}
	if (surface_has_buffer(surface)) {
		wl_resource_post_error(resource,
			EXT_SESSION_LOCK_V1_ERROR_ALREADY_CONSTRUCTED,
			"wl_surface already has a buffer");
		return;
	}
	if (active && output->lock_surface) {
		wl_resource_post_error(resource, EXT_SESSION_LOCK_V1_ERROR_DUPLICATE_OUTPUT,
			"the output already has a lock surface");
		return;
	}

	struct wl_resource *lock_resource = wl_resource_create(client,
		&ext_session_lock_surface_v1_interface,
		wl_resource_get_version(resource), id);
	assert(lock_resource);
	if (!active) {
		wl_resource_set_implementation(lock_resource, &lock_surface_impl,
			NULL, NULL);
		return;
	}
	struct lock_surface *lock_surface = calloc(1, sizeof(*lock_surface));
	assert(lock_surface);
	lock_surface->surface = surface;
	lock_surface->resource = lock_resource;
	lock_surface->output = output;
	wl_resource_set_implementation(lock_resource, &lock_surface_impl,
		lock_surface, lock_surface_resource_destroy);
	output->lock_surface = lock_surface;
	surface->role = SURFACE_ROLE_LOCK_SURFACE;
	surface->lock_surface = lock_surface;
	scene_node_reparent(&surface->scene.tree.node,
		&core.scene.layers[SCENE_LAYER_LOCK]);
	output_arrange_lock_surface(output);
}

static void
lock_unlock_and_destroy_req(struct wl_client *client, struct wl_resource *resource)
{
	if (resource != core.session_lock.resource || !core.session_lock.locked_sent) {
		wl_resource_post_error(resource, EXT_SESSION_LOCK_V1_ERROR_INVALID_UNLOCK,
			"the session was not locked by this object");
		return;
	}
	session_unlock();
	wl_resource_destroy(resource);
}

static const struct ext_session_lock_v1_interface lock_impl = {
	.destroy = lock_destroy_req,
	.get_lock_surface = lock_get_lock_surface_req,
	.unlock_and_destroy = lock_unlock_and_destroy_req,
};

// Another client may lock the session again once the client that locked it
// is gone without unlocking it, and replaces its lock surfaces.
static void
manager_lock_req(struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
	struct wl_resource *lock_resource = wl_resource_create(client,
		&ext_session_lock_v1_interface, wl_resource_get_version(resource), id);
	assert(lock_resource);
	wl_resource_set_implementation(lock_resource, &lock_impl, NULL,
		lock_resource_destroy);
	if (core.session_lock.resource) {
		ext_session_lock_v1_send_finished(lock_resource);
		return;
	}
	core.session_lock.resource = lock_resource;
	core.session_lock.locked_sent = false;
	if (core.session_lock.locked) {
		lock_surfaces_destroy();
	} else {
		session_lock();
	}
}

static const struct ext_session_lock_manager_v1_interface manager_impl = {
	.destroy = resource_destroy_req,
	.lock = manager_lock_req,
};

void
wayland_bind_session_lock_manager(struct wl_client *client, void *data,
	uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&ext_session_lock_manager_v1_interface, 1, id);
	assert(resource);
	wl_resource_set_implementation(resource, &manager_impl, data, NULL);
}
