#include <assert.h>

#include "protocols.h"
#include "seat.h"

static void
seat_get_pointer_req(struct wl_client *client, struct wl_resource *resource,
	uint32_t id)
{
	pointer_create_resource(client, resource, id);
}

static void
seat_get_keyboard_req(struct wl_client *client, struct wl_resource *resource,
	uint32_t id)
{
	keyboard_create_resource(client, resource, id);
}

static void
seat_get_touch_req(struct wl_client *client, struct wl_resource *resource,
	uint32_t id)
{
	touch_create_resource(client, resource, id);
}

static const struct wl_seat_interface seat_impl = {
	.get_pointer = seat_get_pointer_req,
	.get_keyboard = seat_get_keyboard_req,
	.get_touch = seat_get_touch_req,
	.release = resource_destroy_req,
};

void
wayland_bind_seat(struct wl_client *client, void *data, uint32_t version,
	uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client, &wl_seat_interface,
		MIN(version, SEAT_VERSION), id);
	assert(resource);
	wl_resource_set_implementation(resource, &seat_impl, data, NULL);
	// Whether or not the devices are present, so that clients take touch
	// input from touchscreens plugged later.
	wl_seat_send_capabilities(resource, WL_SEAT_CAPABILITY_POINTER
		| WL_SEAT_CAPABILITY_KEYBOARD | WL_SEAT_CAPABILITY_TOUCH);
	if (wl_resource_get_version(resource) >= WL_SEAT_NAME_SINCE_VERSION) {
		wl_seat_send_name(resource, "seat0");
	}
}
