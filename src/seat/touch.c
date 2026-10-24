#include <assert.h>
#include <linux/input-event-codes.h>
#include <stdlib.h>

#include "core.h"
#include "surface.h"

// Touch points go to the surfaces of the clients that take wl_touch input. The
// others go to the pointer as if a mouse moved to them and held its left
// button, which only one point does at a time. So they operate decorations,
// the window switcher and the clients without touch input.

struct touch_resource {
	struct wl_resource *resource;
	struct wl_list link; // core.seat.touch.resources
	bool frame_pending; // Sent events since the last wl_touch.frame
};

static const struct wl_touch_interface touch_impl = {
	.release = resource_destroy_req,
};

static void
touch_resource_destroy(struct wl_resource *resource)
{
	struct touch_resource *touch = wl_resource_get_user_data(resource);
	wl_list_remove(&touch->link);
	free(touch);
}

void
touch_create_resource(struct wl_client *client, struct wl_resource *seat,
	uint32_t id)
{
	struct touch_resource *touch = calloc(1, sizeof(*touch));
	assert(touch);
	touch->resource = wl_resource_create(client, &wl_touch_interface,
		wl_resource_get_version(seat), id);
	assert(touch->resource);
	wl_resource_set_implementation(touch->resource, &touch_impl, touch,
		touch_resource_destroy);
	list_append(&core.seat.touch.resources, &touch->link);
}

static bool
takes_touch(struct surface *surface)
{
	struct touch_resource *touch;
	wl_list_for_each(touch, &core.seat.touch.resources, link) {
		if (resource_matches_surface(touch->resource, surface)) {
			return true;
		}
	}
	return false;
}

static struct touch_point *
find_point(int32_t id)
{
	for (size_t i = 0; i < core.seat.touch.nr_points; i++) {
		if (core.seat.touch.points[i].id == id) {
			return &core.seat.touch.points[i];
		}
	}
	return NULL;
}

static struct touch_point *
pointer_point(void)
{
	for (size_t i = 0; i < core.seat.touch.nr_points; i++) {
		if (!core.seat.touch.points[i].surface) {
			return &core.seat.touch.points[i];
		}
	}
	return NULL;
}

static void
add_point(const struct touch_point *point)
{
	struct touch *touch = &core.seat.touch;
	assert(touch->nr_points < ARRAY_LEN(touch->points));
	touch->points[touch->nr_points++] = *point;
}

static void
remove_point(struct touch_point *point)
{
	*point = core.seat.touch.points[--core.seat.touch.nr_points];
}

static void
send_down(const struct touch_point *point, uint32_t time)
{
	double sx, sy;
	surface_from_layout(point->surface, point->x, point->y, &sx, &sy);
	struct touch_resource *touch;
	wl_list_for_each(touch, &core.seat.touch.resources, link) {
		if (resource_matches_surface(touch->resource, point->surface)) {
			wl_touch_send_down(touch->resource, point->serial, time,
				point->surface->resource, point->id,
				wl_fixed_from_double(sx), wl_fixed_from_double(sy));
			touch->frame_pending = true;
		}
	}
}

static void
send_motion(const struct touch_point *point, uint32_t time)
{
	double sx, sy;
	surface_from_layout(point->surface, point->x, point->y, &sx, &sy);
	struct touch_resource *touch;
	wl_list_for_each(touch, &core.seat.touch.resources, link) {
		if (resource_matches_surface(touch->resource, point->surface)) {
			wl_touch_send_motion(touch->resource, time, point->id,
				wl_fixed_from_double(sx), wl_fixed_from_double(sy));
			touch->frame_pending = true;
		}
	}
}

static void
send_up(const struct touch_point *point, uint32_t time)
{
	uint32_t serial = ++core.serial;
	struct touch_resource *touch;
	wl_list_for_each(touch, &core.seat.touch.resources, link) {
		if (resource_matches_surface(touch->resource, point->surface)) {
			wl_touch_send_up(touch->resource, serial, time, point->id);
			touch->frame_pending = true;
		}
	}
}

// Ends all the touch points of the client, which get no more events.
static void
cancel_client(struct wl_client *client)
{
	struct touch_resource *touch;
	wl_list_for_each(touch, &core.seat.touch.resources, link) {
		if (wl_resource_get_client(touch->resource) == client) {
			wl_touch_send_cancel(touch->resource);
		}
	}
	for (size_t i = 0; i < core.seat.touch.nr_points;) {
		struct touch_point *point = &core.seat.touch.points[i];
		if (point->surface
				&& wl_resource_get_client(point->surface->resource) == client) {
			remove_point(point);
		} else {
			i++;
		}
	}
}

// The point goes to the surface under it for its lifetime, like an implicit
// grab. Pressing the surface may close popups and so forget other points, so
// it is done before the point is added.
void
touch_handle_down(int32_t id, double x, double y, uint32_t time)
{
	struct touch *touch = &core.seat.touch;
	if (touch->nr_points == ARRAY_LEN(touch->points)) {
		return;
	}
	struct surface *surface = direct_input_surface_at(x, y);
	if (!surface || !takes_touch(surface)) {
		if (pointer_point()) {
			return;
		}
		add_point(&(struct touch_point){.id = id, .x = x, .y = y});
		pointer_handle_motion_absolute(x, y, time);
		pointer_handle_button(time, BTN_LEFT, true);
		return;
	}
	struct touch_point point = {
		.id = id,
		.surface = surface,
		.serial = direct_input_press(surface),
		.x = x,
		.y = y,
	};
	add_point(&point);
	send_down(&point, time);
}

void
touch_handle_motion(int32_t id, double x, double y, uint32_t time)
{
	struct touch_point *point = find_point(id);
	if (!point) {
		return;
	}
	point->x = x;
	point->y = y;
	if (point->surface) {
		send_motion(point, time);
	} else {
		pointer_handle_motion_absolute(x, y, time);
	}
}

void
touch_handle_up(int32_t id, uint32_t time)
{
	struct touch_point *point = find_point(id);
	if (!point) {
		return;
	}
	struct touch_point up = *point;
	remove_point(point);
	if (up.surface) {
		send_up(&up, time);
	} else {
		pointer_handle_button(time, BTN_LEFT, false);
	}
}

// Cancelling a point cancels all those of its client.
void
touch_handle_cancel(int32_t id, uint32_t time)
{
	struct touch_point *point = find_point(id);
	if (!point) {
		return;
	}
	if (point->surface) {
		cancel_client(wl_resource_get_client(point->surface->resource));
		return;
	}
	remove_point(point);
	pointer_handle_button(time, BTN_LEFT, false);
}

void
touch_handle_frame(void)
{
	struct touch_resource *touch;
	wl_list_for_each(touch, &core.seat.touch.resources, link) {
		if (touch->frame_pending) {
			wl_touch_send_frame(touch->resource);
			touch->frame_pending = false;
		}
	}
}

// The compositor takes the touch point for a grab, cancelling the points of
// its client. The point then moves the pointer, unless another point already
// does.
bool
touch_take_point(struct surface *ancestor, uint32_t serial)
{
	if (pointer_point()) {
		return false;
	}
	for (size_t i = 0; i < core.seat.touch.nr_points; i++) {
		struct touch_point point = core.seat.touch.points[i];
		if (point.serial != serial || !surface_contains(ancestor, point.surface)) {
			continue;
		}
		cancel_client(wl_resource_get_client(point.surface->resource));
		point.surface = NULL;
		add_point(&point);
		pointer_warp(point.x, point.y);
		return true;
	}
	return false;
}

uint32_t
touch_press_serial(struct surface *ancestor)
{
	for (size_t i = 0; i < core.seat.touch.nr_points; i++) {
		struct touch_point *point = &core.seat.touch.points[i];
		if (surface_contains(ancestor, point->surface)) {
			return point->serial;
		}
	}
	return 0;
}

// The points on the surface get no more events, like the forgotten buttons of
// the pointer.
void
touch_forget_surface(struct surface *surface)
{
	for (size_t i = 0; i < core.seat.touch.nr_points;) {
		struct touch_point *point = &core.seat.touch.points[i];
		if (surface_contains(surface, point->surface)) {
			remove_point(point);
		} else {
			i++;
		}
	}
}
