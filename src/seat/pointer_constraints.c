#include <assert.h>
#include <math.h>
#include <stdlib.h>

#include "core.h"
#include "surface.h"
#include "pointer-constraints-unstable-v1-protocol.h"

// Clients lock the pointer in place, e.g. for games to turn the camera by
// relative motion, or confine it to a region of a surface. The constraint of
// the surface under the pointer applies while the pointer is within its region
// and the client has the keyboard focus, so that clients in the background
// cannot take the pointer. There is one seat, so a surface has at most one
// constraint whatever wl_pointer it was created for. Only the motion of mice
// and touchpads is constrained.

// The region of the constraint within the input region of the surface.
static void
constraint_region(struct surface *surface, pixman_region32_t *region)
{
	struct surface_state *state = &surface->current;
	pixman_region32_init_rect(region, 0, 0, (unsigned)surface->width,
		(unsigned)surface->height);
	if (state->input_region_set) {
		pixman_region32_intersect(region, region, &state->input_region);
	}
	if (state->constraint_region_set) {
		pixman_region32_intersect(region, region, &state->constraint_region);
	}
}

static struct pointer_constraint *
applicable_constraint(void)
{
	struct pointer *pointer = &core.seat.pointer;
	struct surface *focus = pointer->focus;
	struct pointer_constraint *constraint = focus ? focus->pointer_constraint : NULL;
	if (!constraint || constraint->defunct
			|| !resource_matches_surface(focus->resource,
				core.seat.keyboard.focus)) {
		return NULL;
	}
	pixman_region32_t region;
	constraint_region(focus, &region);
	bool within = pixman_region32_contains_point(&region,
		(int)floor(pointer->surface_x), (int)floor(pointer->surface_y), NULL);
	pixman_region32_fini(&region);
	return within ? constraint : NULL;
}

// When unlocked, the pointer moves to where the client draws its cursor, if
// the client tells it and the pointer is still on the surface.
static void
release_constraint(void)
{
	struct pointer *pointer = &core.seat.pointer;
	struct pointer_constraint *constraint = pointer->constraint;
	pointer->constraint = NULL;
	struct surface *surface = constraint->surface;
	if (constraint->type != POINTER_CONSTRAINT_LOCK
			|| surface != pointer->focus
			|| !surface->current.cursor_hint_set) {
		return;
	}
	double old_x = pointer->x;
	double old_y = pointer->y;
	pointer_warp(old_x - pointer->surface_x + surface->current.cursor_hint_x,
		old_y - pointer->surface_y + surface->current.cursor_hint_y);
	pointer->surface_x += pointer->x - old_x;
	pointer->surface_y += pointer->y - old_y;
}

// Called whenever the pointer focus is set. A oneshot constraint becomes
// defunct once it stops applying.
void
pointer_constraint_update(void)
{
	struct pointer *pointer = &core.seat.pointer;
	struct pointer_constraint *old = pointer->constraint;
	struct pointer_constraint *constraint = applicable_constraint();
	if (constraint == old) {
		return;
	}
	if (old) {
		release_constraint();
		old->defunct = old->oneshot;
		if (old->type == POINTER_CONSTRAINT_LOCK) {
			zwp_locked_pointer_v1_send_unlocked(old->resource);
		} else {
			zwp_confined_pointer_v1_send_unconfined(old->resource);
		}
	}
	pointer->constraint = constraint;
	if (!constraint) {
		return;
	}
	if (constraint->type == POINTER_CONSTRAINT_LOCK) {
		zwp_locked_pointer_v1_send_locked(constraint->resource);
	} else {
		zwp_confined_pointer_v1_send_confined(constraint->resource);
	}
}

// Moves a point of the layout to the closest point of the region of the
// confinement. The right and bottom edges of its rectangles are exclusive, so
// the point is kept 1/256 inside them, like on the outputs.
void
pointer_confine(double *x, double *y)
{
	struct surface *surface = core.seat.pointer.constraint->surface;
	int surface_x, surface_y;
	scene_node_layout_position(&surface->scene.node, &surface_x, &surface_y);
	double sx = *x - surface_x;
	double sy = *y - surface_y;
	pixman_region32_t region;
	constraint_region(surface, &region);
	int nr_rects;
	pixman_box32_t *rects = pixman_region32_rectangles(&region, &nr_rects);
	double min_distance = INFINITY;
	for (int i = 0; i < nr_rects; i++) {
		double rx = clamp(sx, rects[i].x1, rects[i].x2 - 1 / 256.0);
		double ry = clamp(sy, rects[i].y1, rects[i].y2 - 1 / 256.0);
		double distance = hypot(rx - sx, ry - sy);
		if (distance < min_distance) {
			min_distance = distance;
			*x = surface_x + rx;
			*y = surface_y + ry;
		}
	}
	pixman_region32_fini(&region);
}

static void
constraint_resource_destroy(struct wl_resource *resource)
{
	struct pointer_constraint *constraint = wl_resource_get_user_data(resource);
	if (core.seat.pointer.constraint == constraint) {
		release_constraint();
	}
	if (constraint->surface) {
		constraint->surface->pointer_constraint = NULL;
	}
	free(constraint);
}

// The region is the input region if NULL.
static void
constraint_set_region_req(struct wl_client *client,
	struct wl_resource *resource, struct wl_resource *region)
{
	struct pointer_constraint *constraint = wl_resource_get_user_data(resource);
	struct surface *surface = constraint->surface;
	if (surface) {
		region_copy(&surface->pending.constraint_region, region);
		surface->pending.constraint_region_set = region;
	}
}

static void
locked_pointer_set_cursor_position_hint_req(struct wl_client *client,
	struct wl_resource *resource, wl_fixed_t surface_x, wl_fixed_t surface_y)
{
	struct pointer_constraint *constraint = wl_resource_get_user_data(resource);
	struct surface *surface = constraint->surface;
	if (surface) {
		surface->pending.cursor_hint_set = true;
		surface->pending.cursor_hint_x = wl_fixed_to_double(surface_x);
		surface->pending.cursor_hint_y = wl_fixed_to_double(surface_y);
	}
}

static const struct zwp_locked_pointer_v1_interface locked_pointer_impl = {
	.destroy = resource_destroy_req,
	.set_cursor_position_hint = locked_pointer_set_cursor_position_hint_req,
	.set_region = constraint_set_region_req,
};

static const struct zwp_confined_pointer_v1_interface confined_pointer_impl = {
	.destroy = resource_destroy_req,
	.set_region = constraint_set_region_req,
};

// The region of the request applies right away, and the next commits keep it
// until it is set again.
static void
create_constraint(struct wl_client *client, struct wl_resource *manager,
	uint32_t id, struct wl_resource *surface_resource,
	struct wl_resource *region, uint32_t lifetime,
	enum pointer_constraint_type type, const struct wl_interface *interface,
	const void *impl)
{
	struct surface *surface = wl_resource_get_user_data(surface_resource);
	if (surface->pointer_constraint) {
		wl_resource_post_error(manager,
			ZWP_POINTER_CONSTRAINTS_V1_ERROR_ALREADY_CONSTRAINED,
			"the surface already has a pointer constraint");
		return;
	}
	struct pointer_constraint *constraint = calloc(1, sizeof(*constraint));
	assert(constraint);
	constraint->resource = wl_resource_create(client, interface,
		wl_resource_get_version(manager), id);
	assert(constraint->resource);
	wl_resource_set_implementation(constraint->resource, impl, constraint,
		constraint_resource_destroy);
	constraint->surface = surface;
	constraint->type = type;
	constraint->oneshot = lifetime != ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT;
	surface->pointer_constraint = constraint;
	struct surface_state *states[] = {&surface->pending, &surface->current};
	for (size_t i = 0; i < ARRAY_LEN(states); i++) {
		region_copy(&states[i]->constraint_region, region);
		states[i]->constraint_region_set = region;
		states[i]->cursor_hint_set = false;
	}
}

static void
pointer_constraints_lock_pointer_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id, struct wl_resource *surface,
	struct wl_resource *pointer, struct wl_resource *region, uint32_t lifetime)
{
	create_constraint(client, resource, id, surface, region, lifetime,
		POINTER_CONSTRAINT_LOCK, &zwp_locked_pointer_v1_interface,
		&locked_pointer_impl);
}

static void
pointer_constraints_confine_pointer_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id, struct wl_resource *surface,
	struct wl_resource *pointer, struct wl_resource *region, uint32_t lifetime)
{
	create_constraint(client, resource, id, surface, region, lifetime,
		POINTER_CONSTRAINT_CONFINE, &zwp_confined_pointer_v1_interface,
		&confined_pointer_impl);
}

static const struct zwp_pointer_constraints_v1_interface
		pointer_constraints_impl = {
	.destroy = resource_destroy_req,
	.lock_pointer = pointer_constraints_lock_pointer_req,
	.confine_pointer = pointer_constraints_confine_pointer_req,
};

void
wayland_bind_pointer_constraints(struct wl_client *client, void *data,
	uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&zwp_pointer_constraints_v1_interface, 1, id);
	assert(resource);
	wl_resource_set_implementation(resource, &pointer_constraints_impl, NULL, NULL);
}

// The surface was unmapped, which released the constraint. The constraint
// object stays alive, but becomes inert.
void
pointer_constraint_surface_destroy(struct surface *surface)
{
	struct pointer_constraint *constraint = surface->pointer_constraint;
	if (constraint) {
		assert(core.seat.pointer.constraint != constraint);
		constraint->surface = NULL;
	}
}
