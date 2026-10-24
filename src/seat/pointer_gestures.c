#include <assert.h>

#include "core.h"
#include "surface.h"
#include "pointer-gestures-unstable-v1-protocol.h"

// Touchpad gestures go to the surface under the pointer when they begin until
// they end, even if the pointer focus moves on meanwhile. There is one seat,
// so they go to the gesture objects of the client of the surface whatever
// wl_pointer they were created for.

static void
send_begin(uint32_t time, uint32_t fingers)
{
	struct pointer *pointer = &core.seat.pointer;
	struct surface *surface = pointer->gesture.surface;
	uint32_t serial = ++core.serial;
	struct wl_resource *resource;
	wl_resource_for_each(resource,
			&pointer->gesture_resources[pointer->gesture.type]) {
		if (!resource_matches_surface(resource, surface)) {
			continue;
		}
		switch (pointer->gesture.type) {
		case GESTURE_SWIPE:
			zwp_pointer_gesture_swipe_v1_send_begin(resource, serial, time,
				surface->resource, fingers);
			break;
		case GESTURE_PINCH:
			zwp_pointer_gesture_pinch_v1_send_begin(resource, serial, time,
				surface->resource, fingers);
			break;
		case GESTURE_HOLD:
			zwp_pointer_gesture_hold_v1_send_begin(resource, serial, time,
				surface->resource, fingers);
			break;
		case NR_GESTURE_TYPES:
			assert(!"invalid gesture type");
		}
	}
}

// Ends the gesture in progress, if any.
static void
end_gesture(uint32_t time, bool cancelled)
{
	struct pointer *pointer = &core.seat.pointer;
	struct surface *surface = pointer->gesture.surface;
	if (!surface) {
		return;
	}
	pointer->gesture.surface = NULL;
	uint32_t serial = ++core.serial;
	struct wl_resource *resource;
	wl_resource_for_each(resource,
			&pointer->gesture_resources[pointer->gesture.type]) {
		if (!resource_matches_surface(resource, surface)) {
			continue;
		}
		switch (pointer->gesture.type) {
		case GESTURE_SWIPE:
			zwp_pointer_gesture_swipe_v1_send_end(resource, serial, time, cancelled);
			break;
		case GESTURE_PINCH:
			zwp_pointer_gesture_pinch_v1_send_end(resource, serial, time, cancelled);
			break;
		case GESTURE_HOLD:
			zwp_pointer_gesture_hold_v1_send_end(resource, serial, time, cancelled);
			break;
		case NR_GESTURE_TYPES:
			assert(!"invalid gesture type");
		}
	}
}

// Only one gesture is in progress at a time, so a gesture of another touchpad
// cancels it.
void
pointer_handle_gesture_begin(enum gesture_type type, uint32_t time, uint32_t fingers)
{
	struct pointer *pointer = &core.seat.pointer;
	end_gesture(time, true);
	pointer_update_focus(time, false);
	pointer->gesture.type = type;
	pointer->gesture.surface = pointer->focus;
	if (pointer->focus) {
		send_begin(time, fingers);
	}
}

void
pointer_handle_swipe_update(uint32_t time, double dx, double dy)
{
	struct pointer *pointer = &core.seat.pointer;
	if (pointer->gesture.type != GESTURE_SWIPE) {
		return;
	}
	struct wl_resource *resource;
	wl_resource_for_each(resource, &pointer->gesture_resources[GESTURE_SWIPE]) {
		if (resource_matches_surface(resource, pointer->gesture.surface)) {
			zwp_pointer_gesture_swipe_v1_send_update(resource, time,
				wl_fixed_from_double(dx), wl_fixed_from_double(dy));
		}
	}
}

void
pointer_handle_pinch_update(uint32_t time, double dx, double dy, double scale,
	double rotation)
{
	struct pointer *pointer = &core.seat.pointer;
	if (pointer->gesture.type != GESTURE_PINCH) {
		return;
	}
	struct wl_resource *resource;
	wl_resource_for_each(resource, &pointer->gesture_resources[GESTURE_PINCH]) {
		if (resource_matches_surface(resource, pointer->gesture.surface)) {
			zwp_pointer_gesture_pinch_v1_send_update(resource, time,
				wl_fixed_from_double(dx), wl_fixed_from_double(dy),
				wl_fixed_from_double(scale), wl_fixed_from_double(rotation));
		}
	}
}

// The end of a gesture cancelled by another one is dropped.
void
pointer_handle_gesture_end(enum gesture_type type, uint32_t time, bool cancelled)
{
	if (core.seat.pointer.gesture.type == type) {
		end_gesture(time, cancelled);
	}
}

// The gesture is cancelled when its surface goes away.
void
pointer_gestures_forget_surface(struct surface *surface)
{
	if (!surface_contains(surface, core.seat.pointer.gesture.surface)) {
		return;
	}
	end_gesture(monotonic_msec(), true);
}

static const struct zwp_pointer_gesture_swipe_v1_interface swipe_impl = {
	.destroy = resource_destroy_req,
};

static const struct zwp_pointer_gesture_pinch_v1_interface pinch_impl = {
	.destroy = resource_destroy_req,
};

static const struct zwp_pointer_gesture_hold_v1_interface hold_impl = {
	.destroy = resource_destroy_req,
};

static void
create_gesture(struct wl_client *client, struct wl_resource *manager,
	uint32_t id, enum gesture_type type, const struct wl_interface *interface,
	const void *impl)
{
	struct wl_resource *resource = wl_resource_create(client, interface,
		wl_resource_get_version(manager), id);
	assert(resource);
	wl_resource_set_implementation(resource, impl, NULL, resource_unlink);
	list_append(&core.seat.pointer.gesture_resources[type],
		wl_resource_get_link(resource));
}

static void
pointer_gestures_get_swipe_gesture_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id, struct wl_resource *pointer)
{
	create_gesture(client, resource, id, GESTURE_SWIPE,
		&zwp_pointer_gesture_swipe_v1_interface, &swipe_impl);
}

static void
pointer_gestures_get_pinch_gesture_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id, struct wl_resource *pointer)
{
	create_gesture(client, resource, id, GESTURE_PINCH,
		&zwp_pointer_gesture_pinch_v1_interface, &pinch_impl);
}

static void
pointer_gestures_get_hold_gesture_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id, struct wl_resource *pointer)
{
	create_gesture(client, resource, id, GESTURE_HOLD,
		&zwp_pointer_gesture_hold_v1_interface, &hold_impl);
}

static const struct zwp_pointer_gestures_v1_interface pointer_gestures_impl = {
	.get_swipe_gesture = pointer_gestures_get_swipe_gesture_req,
	.get_pinch_gesture = pointer_gestures_get_pinch_gesture_req,
	.release = resource_destroy_req,
	.get_hold_gesture = pointer_gestures_get_hold_gesture_req,
};

void
wayland_bind_pointer_gestures(struct wl_client *client, void *data,
	uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&zwp_pointer_gestures_v1_interface,
		MIN(version, POINTER_GESTURES_VERSION), id);
	assert(resource);
	wl_resource_set_implementation(resource, &pointer_gestures_impl, NULL, NULL);
}
