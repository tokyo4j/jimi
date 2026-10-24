#include <assert.h>

#include "core.h"
#include "surface.h"
#include "relative-pointer-unstable-v1-protocol.h"

// The motion of mice and touchpads, even while the pointer is locked or kept
// on the outputs, e.g. for games to turn the camera. There is one seat, so it
// goes to the relative pointers of the client of the focus whatever wl_pointer
// they were created for. The caller ends it with wl_pointer.frame.
void
relative_pointers_send_motion(uint64_t time_usec, double dx, double dy,
	double dx_unaccel, double dy_unaccel)
{
	struct wl_resource *resource;
	wl_resource_for_each(resource, &core.seat.pointer.relative_pointers) {
		if (resource_matches_surface(resource, core.seat.pointer.focus)) {
			zwp_relative_pointer_v1_send_relative_motion(resource,
				(uint32_t)(time_usec >> 32), (uint32_t)time_usec,
				wl_fixed_from_double(dx), wl_fixed_from_double(dy),
				wl_fixed_from_double(dx_unaccel), wl_fixed_from_double(dy_unaccel));
		}
	}
}

static const struct zwp_relative_pointer_v1_interface relative_pointer_impl = {
	.destroy = resource_destroy_req,
};

static void
relative_pointer_manager_get_relative_pointer_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id, struct wl_resource *pointer)
{
	struct wl_resource *relative_pointer = wl_resource_create(client,
		&zwp_relative_pointer_v1_interface, wl_resource_get_version(resource), id);
	assert(relative_pointer);
	wl_resource_set_implementation(relative_pointer, &relative_pointer_impl,
		NULL, resource_unlink);
	list_append(&core.seat.pointer.relative_pointers,
		wl_resource_get_link(relative_pointer));
}

static const struct zwp_relative_pointer_manager_v1_interface
		relative_pointer_manager_impl = {
	.destroy = resource_destroy_req,
	.get_relative_pointer = relative_pointer_manager_get_relative_pointer_req,
};

void
wayland_bind_relative_pointer_manager(struct wl_client *client, void *data,
	uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&zwp_relative_pointer_manager_v1_interface, 1, id);
	assert(resource);
	wl_resource_set_implementation(resource, &relative_pointer_manager_impl,
		NULL, NULL);
}
