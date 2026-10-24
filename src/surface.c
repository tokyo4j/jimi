#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "color.h"
#include "core.h"
#include "image_capture.h"
#include "output.h"
#include "surface.h"

static const struct wl_region_interface region_impl;

// Frame and release callbacks are linked into the lists of the surface state
// by their resources.
static void
callback_create(struct wl_client *client, uint32_t id, struct wl_list *callbacks)
{
	struct wl_resource *resource = wl_resource_create(client,
		&wl_callback_interface, 1, id);
	assert(resource);
	wl_resource_set_implementation(resource, NULL, NULL, resource_unlink);
	list_append(callbacks, wl_resource_get_link(resource));
}

static void
callbacks_destroy(struct wl_list *callbacks)
{
	struct wl_resource *resource, *tmp;
	wl_resource_for_each_safe(resource, tmp, callbacks) {
		wl_resource_destroy(resource);
	}
}

bool
resource_matches_surface(struct wl_resource *resource, struct surface *surface)
{
	return surface && wl_resource_get_client(resource)
			== wl_resource_get_client(surface->resource);
}

// Whether a buffer is attached, or committed even if not applied yet.
bool
surface_has_buffer(struct surface *surface)
{
	return surface->pending.buffer_width > 0;
}

// The parent of a subsurface or popup.
static struct surface *
surface_parent(struct surface *surface)
{
	return surface->role == SURFACE_ROLE_XDG_POPUP
		? surface->xdg->popup->parent : surface->parent;
}

// The surface at the root of the tree of subsurfaces and popups.
struct surface *
surface_root(struct surface *surface)
{
	while (surface_parent(surface)) {
		surface = surface_parent(surface);
	}
	return surface;
}

// Whether the surface is in the tree of subsurfaces and popups of the
// ancestor.
bool
surface_contains(struct surface *ancestor, struct surface *surface)
{
	for (; surface; surface = surface_parent(surface)) {
		if (surface == ancestor) {
			return true;
		}
	}
	return false;
}

// The surface-local coordinates of a point in the layout
void
surface_from_layout(struct surface *surface, double x, double y, double *sx,
	double *sy)
{
	int surface_x, surface_y;
	scene_node_layout_position(&surface->scene.node, &surface_x, &surface_y);
	*sx = x - surface_x;
	*sy = y - surface_y;
}

// Cursors and drag icons are drawn by the pointer, not the scene.
static bool
surface_in_scene(struct surface *surface)
{
	switch (surface->role) {
	case SURFACE_ROLE_NONE:
	case SURFACE_ROLE_CURSOR:
	case SURFACE_ROLE_DND_ICON:
	case SURFACE_ROLE_XDG_SURFACE:
		return false;
	case SURFACE_ROLE_SUBSURFACE:
	case SURFACE_ROLE_XDG_TOPLEVEL:
	case SURFACE_ROLE_XDG_POPUP:
	case SURFACE_ROLE_LAYER_SURFACE:
	case SURFACE_ROLE_INPUT_PANEL:
	case SURFACE_ROLE_LOCK_SURFACE:
	case SURFACE_ROLE_XWAYLAND:
		return true;
	}
	assert(!"unknown surface role");
	return false;
}

static void
surface_update_scene_enabled(struct surface *surface)
{
	bool added = surface->role != SURFACE_ROLE_SUBSURFACE
		|| surface->subsurface->added;
	scene_node_set_enabled(&surface->scene.tree.node, surface->mapped && added);
}

// Where the surface is drawn, in layout coordinates. Empty while it is hidden.
static struct box
surface_shown_box(struct surface *surface)
{
	if (surface == pointer_client_cursor_surface()
			|| surface == pointer_drag_icon_surface()) {
		return pointer_surface_box(surface);
	}
	if (!surface_in_scene(surface)) {
		return (struct box){0};
	}
	struct box box = {.width = surface->width, .height = surface->height};
	return scene_node_layout_box(&surface->scene.node, &box);
}

// The screen area covered by a surface, including the decoration of its
// window.
static struct box
surface_extent(struct surface *surface)
{
	switch (surface->role) {
	case SURFACE_ROLE_NONE:
	case SURFACE_ROLE_XDG_SURFACE:
		return (struct box){0};
	case SURFACE_ROLE_CURSOR:
	case SURFACE_ROLE_DND_ICON:
		return surface_shown_box(surface);
	case SURFACE_ROLE_SUBSURFACE:
	case SURFACE_ROLE_LAYER_SURFACE:
	case SURFACE_ROLE_INPUT_PANEL:
	case SURFACE_ROLE_LOCK_SURFACE:
		return scene_node_extent(&surface->scene.tree.node);
	case SURFACE_ROLE_XDG_TOPLEVEL:
		return scene_node_extent(&surface->xdg->toplevel->window.tree.node);
	case SURFACE_ROLE_XDG_POPUP:
		return scene_node_extent(&surface->xdg->popup->tree.node);
	case SURFACE_ROLE_XWAYLAND: {
		struct window *window = xwayland_surface_window(surface->xwayland);
		return scene_node_extent(window ? &window->tree.node
			: &surface->scene.tree.node);
	}
	}
	assert(!"unknown surface role");
	return (struct box){0};
}

// Cursors may be on the cursor planes of the outputs.
static void
damage_extent(struct surface *surface, const struct box *extent)
{
	if (surface->role == SURFACE_ROLE_CURSOR) {
		damage_cursor(extent, false);
	} else {
		damage_layout_box(extent);
	}
}

static void
damage_surface(struct surface *surface)
{
	struct box extent = surface_extent(surface);
	damage_extent(surface, &extent);
}

// Sends wl_surface.enter or leave with the wl_output resources of the client
// of the surface.
static void
surface_send_output_event(struct surface *surface, struct output *output, bool enter)
{
	struct wl_client *client = wl_resource_get_client(surface->resource);
	struct wl_resource *resource;
	wl_resource_for_each(resource, &output->resources) {
		if (wl_resource_get_client(resource) != client) {
			continue;
		}
		if (enter) {
			wl_surface_send_enter(surface->resource, resource);
		} else {
			wl_surface_send_leave(surface->resource, resource);
		}
	}
}

// Sends wl_surface.enter and leave for the outputs that the surface has come
// onto and gone off.
static void
surface_update_outputs(struct surface *surface)
{
	struct box box = surface_shown_box(surface);
	uint32_t outputs = 0;
	struct output *output;
	wl_list_for_each(output, &core.outputs, link) {
		if (output->enabled && box_intersects(&box, &output->box)) {
			outputs |= output->bit;
		}
	}
	uint32_t changed = outputs ^ surface->outputs;
	wl_list_for_each(output, &core.outputs, link) {
		if (changed & output->bit) {
			surface_send_output_event(surface, output, outputs & output->bit);
		}
	}
	surface->outputs = outputs;
}

// The output that a hidden surface without a parent is to be shown on: that
// of its window, layer surface or lock surface, or else the one under the
// pointer. NULL while no output is enabled.
static struct output *
surface_expected_output(struct surface *surface)
{
	struct window *window = window_from_surface(surface);
	// Windows stay on a disabled output until outputs_update().
	if (window && window->output && window->output->enabled) {
		return window->output;
	}
	if (surface->role == SURFACE_ROLE_LAYER_SURFACE) {
		return surface->layer_surface->output;
	}
	if (surface->role == SURFACE_ROLE_LOCK_SURFACE) {
		return surface->lock_surface->output;
	}
	return output_nearest(core.seat.pointer.x, core.seat.pointer.y);
}

// The output whose scale and transform the surface is told to draw with:
// the one of the highest scale that it is shown on. While it is hidden, that
// of its parent, or else the output it is expected on. NULL while no output
// is enabled.
static struct output *
surface_preferred_output(struct surface *surface)
{
	if (surface->outputs) {
		struct output *preferred = NULL;
		struct output *output;
		wl_list_for_each(output, &core.outputs, link) {
			if ((surface->outputs & output->bit) && (!preferred
					|| output->scale > preferred->scale)) {
				preferred = output;
			}
		}
		return preferred;
	}
	struct surface *parent = surface_parent(surface);
	if (parent) {
		return surface_preferred_output(parent);
	}
	return surface_expected_output(surface);
}

// Tells the surface the scale, transform and colors to draw with. Clients
// without fractional-scale-v1 draw at the next integer scale, and the
// renderer scales them down. Buffers in the transform and the colors of the
// output can be scanned out.
static void
surface_send_preferences(struct surface *surface)
{
	struct output *output = surface_preferred_output(surface);
	if (!output) {
		return;
	}
	bool v6 = wl_resource_get_version(surface->resource) >= 6;
	if (surface->preferred_scale != output->scale) {
		surface->preferred_scale = output->scale;
		fractional_scale_send(surface);
		if (v6) {
			wl_surface_send_preferred_buffer_scale(surface->resource,
				(int32_t)ceil(output->scale));
		}
	}
	if (surface->preferred_transform != output->transform) {
		surface->preferred_transform = output->transform;
		if (v6) {
			wl_surface_send_preferred_buffer_transform(
				surface->resource, output->transform);
		}
	}
	if (surface->preferred_image_description != output->image_description) {
		image_description_unref(surface->preferred_image_description);
		surface->preferred_image_description =
			image_description_ref(output->image_description);
		color_management_send_preferred(surface);
	}
}

// Tells the surfaces the outputs they have come onto and gone off since the
// last call, and the scales and transforms to draw with. Called once per
// event loop iteration, after the scene has changed, and when an output is
// disabled.
void
surfaces_update_outputs(void)
{
	struct surface *surface;
	wl_list_for_each(surface, &core.surfaces, link) {
		surface_update_outputs(surface);
	}
	// Once all the surfaces have their outputs, as hidden ones follow
	// their parents.
	wl_list_for_each(surface, &core.surfaces, link) {
		surface_send_preferences(surface);
	}
}

// Tells the surfaces on the output that they have entered it, when their
// client binds the output after they have.
void
surfaces_send_enter(struct output *output, struct wl_resource *output_resource)
{
	struct wl_client *client = wl_resource_get_client(output_resource);
	struct surface *surface;
	wl_list_for_each(surface, &core.surfaces, link) {
		if ((surface->outputs & output->bit)
				&& wl_resource_get_client(surface->resource) == client) {
			wl_surface_send_enter(surface->resource, output_resource);
		}
	}
}

static struct box
surface_size_from_buffer(int width, int height,
	enum wl_output_transform transform, int scale)
{
	struct box box = {
		.width = width,
		.height = height,
	};
	box_transform(&box, &box, transform, width, height);
	assert(scale > 0);
	assert(box.width % scale == 0 && box.height % scale == 0);
	box.width /= scale;
	box.height /= scale;
	return box;
}

// The part of the buffer shown on the surface, in the pixels of the buffer
// after its transform.
struct fbox
surface_source_box(struct surface *surface)
{
	struct surface_state *state = &surface->current;
	assert(state->buffer);
	if (state->viewport_source.width >= 0) {
		return (struct fbox){
			.x = state->viewport_source.x * state->scale,
			.y = state->viewport_source.y * state->scale,
			.width = state->viewport_source.width * state->scale,
			.height = state->viewport_source.height * state->scale,
		};
	}
	struct box buffer = {
		.width = (int)state->buffer->width,
		.height = (int)state->buffer->height,
	};
	box_transform(&buffer, &buffer, state->transform, buffer.width, buffer.height);
	return (struct fbox){
		.width = buffer.width,
		.height = buffer.height,
	};
}

// Adds the buffer damage of the applied state to the surface damage.
static void
add_buffer_damage(struct surface *surface, const struct surface_state *state,
	pixman_region32_t *damage)
{
	struct client_buffer *buffer = surface->current.buffer;
	if (!pixman_region32_not_empty(&state->buffer_damage) || !buffer) {
		return;
	}
	struct box buffer_bounds = {
		.width = (int)buffer->width,
		.height = (int)buffer->height,
	};
	struct fbox source = surface_source_box(surface);
	double scale_x = surface->width / source.width;
	double scale_y = surface->height / source.height;
	int nr_rects;
	const pixman_box32_t *rects =
		pixman_region32_rectangles(&state->buffer_damage, &nr_rects);
	for (int i = 0; i < nr_rects; i++) {
		struct box rect = box_from_pixman(&rects[i]);
		if (!box_intersection(&rect, &rect, &buffer_bounds)) {
			continue;
		}
		box_transform(&rect, &rect, surface->current.transform,
			buffer_bounds.width, buffer_bounds.height);
		// Scaled from the source box to the surface, rounded outward.
		int x1 = (int)floor((rect.x - source.x) * scale_x);
		int y1 = (int)floor((rect.y - source.y) * scale_y);
		int x2 = (int)ceil((rect.x + rect.width - source.x) * scale_x);
		int y2 = (int)ceil((rect.y + rect.height - source.y) * scale_y);
		pixman_region32_union_rect(damage, damage, x1, y1,
			(unsigned)(x2 - x1), (unsigned)(y2 - y1));
	}
}

// The damage of the applied state, in surface-local coordinates.
static void
surface_state_damage(struct surface *surface,
	const struct surface_state *state, pixman_region32_t *damage)
{
	pixman_region32_copy(damage, &state->damage);
	add_buffer_damage(surface, state, damage);
	pixman_region32_intersect_rect(damage, damage, 0, 0,
		(unsigned)surface->width, (unsigned)surface->height);
}

// The damage of the applied state, in buffer-local coordinates. The surface
// damage is scaled from the surface to the source box, rounded outward.
static void
surface_state_buffer_damage(struct surface *surface,
	const struct surface_state *state, pixman_region32_t *damage)
{
	struct client_buffer *buffer = surface->current.buffer;
	enum wl_output_transform transform = surface->current.transform;
	struct box transformed = {
		.width = (int)buffer->width,
		.height = (int)buffer->height,
	};
	box_transform(&transformed, &transformed, transform, transformed.width,
		transformed.height);
	struct fbox source = surface_source_box(surface);
	double scale_x = source.width / surface->width;
	double scale_y = source.height / surface->height;
	pixman_region32_t surface_damage;
	pixman_region32_init(&surface_damage);
	pixman_region32_intersect_rect(&surface_damage, &state->damage, 0, 0,
		(unsigned)surface->width, (unsigned)surface->height);
	pixman_region32_copy(damage, &state->buffer_damage);
	int nr_rects;
	const pixman_box32_t *rects =
		pixman_region32_rectangles(&surface_damage, &nr_rects);
	for (int i = 0; i < nr_rects; i++) {
		int x1 = (int)floor(source.x + rects[i].x1 * scale_x);
		int y1 = (int)floor(source.y + rects[i].y1 * scale_y);
		int x2 = (int)ceil(source.x + rects[i].x2 * scale_x);
		int y2 = (int)ceil(source.y + rects[i].y2 * scale_y);
		struct box rect = {
			.x = x1,
			.y = y1,
			.width = x2 - x1,
			.height = y2 - y1,
		};
		box_transform(&rect, &rect, transform_invert(transform),
			transformed.width, transformed.height);
		pixman_region32_union_rect(damage, damage, rect.x, rect.y,
			(unsigned)rect.width, (unsigned)rect.height);
	}
	pixman_region32_fini(&surface_damage);
	pixman_region32_intersect_rect(damage, damage, 0, 0, buffer->width,
		buffer->height);
}

// Damages the outputs where the surface-local damage is shown.
static void
damage_surface_region(struct surface *surface, const pixman_region32_t *damage)
{
	int nr_rects;
	const pixman_box32_t *rects = pixman_region32_rectangles(damage, &nr_rects);
	for (int i = 0; i < nr_rects; i++) {
		struct box rect = box_from_pixman(&rects[i]);
		struct box box = scene_node_layout_box(&surface->scene.node, &rect);
		damage_layout_box(&box);
	}
}

static void
surface_state_set_buffer(struct surface_state *state, struct client_buffer *buffer)
{
	if (buffer) {
		client_buffer_lock(buffer);
	}
	if (state->buffer) {
		client_buffer_unlock(state->buffer);
	}
	state->buffer = buffer;
}

static pixman_region32_t *
region_from_resource(struct wl_resource *resource)
{
	assert(wl_resource_instance_of(resource, &wl_region_interface, &region_impl));
	pixman_region32_t *region = wl_resource_get_user_data(resource);
	assert(region);
	return region;
}

// Copies the wl_region, or clears the region if NULL.
void
region_copy(pixman_region32_t *dest, struct wl_resource *resource)
{
	if (resource) {
		pixman_region32_copy(dest, region_from_resource(resource));
	} else {
		pixman_region32_clear(dest);
	}
}

// Rectangles without area are ignored.
static void
region_add_rect(pixman_region32_t *region, int32_t x, int32_t y,
	int32_t width, int32_t height)
{
	if (width <= 0 || height <= 0) {
		return;
	}
	pixman_region32_union_rect(region, region, x, y, (unsigned)width,
		(unsigned)height);
}

// The subsurfaces are shown once the state of their parent includes them.
static void
apply_subsurface_state(struct surface *surface, struct surface_state *state)
{
	struct wl_array *placements = &surface->current.subsurfaces;
	assert(wl_array_copy(placements, &state->subsurfaces) == 0);
	struct subsurface_placement *placement;
	wl_array_for_each(placement, placements) {
		struct subsurface *subsurface = placement->subsurface;
		if (subsurface && !subsurface->added) {
			subsurface->added = true;
			surface_update_scene_enabled(subsurface->surface);
		}
	}
	scene_surface_reconfigure(surface);
}

// Roles decide when their surface is mapped. Unmapping dismisses the popups
// placed on the surface.
void
surface_set_mapped(struct surface *surface, bool mapped)
{
	if (!mapped && surface->mapped) {
		xdg_popups_dismiss(surface);
	}
	if (surface->mapped == mapped) {
		return;
	}
	damage_surface(surface);
	surface->mapped = mapped;
	surface_update_scene_enabled(surface);
	if (!mapped) {
		keyboard_forget_surface(surface);
		pointer_forget_surface(surface);
		touch_forget_surface(surface);
		tablet_forget_surface(surface);
	}
	damage_surface(surface);
}

// The viewport destination, or else its source, overrides the size of the
// buffer.
static void
surface_update_size(struct surface *surface)
{
	struct surface_state *state = &surface->current;
	if (!state->buffer) {
		return;
	}
	struct box size = surface_size_from_buffer((int)state->buffer->width,
		(int)state->buffer->height, state->transform, state->scale);
	if (state->viewport_width >= 0) {
		size.width = state->viewport_width;
		size.height = state->viewport_height;
	} else if (state->viewport_source.width >= 0) {
		// viewport_validate() made sure that it is integral.
		size.width = (int)state->viewport_source.width;
		size.height = (int)state->viewport_source.height;
	}
	surface->width = size.width;
	surface->height = size.height;
}

static void
surface_role_destroy(struct surface *surface)
{
	switch (surface->role) {
	case SURFACE_ROLE_NONE:
	case SURFACE_ROLE_CURSOR:
	case SURFACE_ROLE_DND_ICON:
		// No role object; the pointer lets go of the surface.
		return;
	case SURFACE_ROLE_SUBSURFACE:
		subsurface_destroy(surface->subsurface);
		return;
	case SURFACE_ROLE_XDG_SURFACE:
	case SURFACE_ROLE_XDG_TOPLEVEL:
	case SURFACE_ROLE_XDG_POPUP:
		xdg_surface_destroy(surface->xdg);
		return;
	case SURFACE_ROLE_LAYER_SURFACE:
		layer_surface_destroy(surface->layer_surface);
		return;
	case SURFACE_ROLE_INPUT_PANEL:
		input_panel_surface_destroy(surface->input_panel);
		return;
	case SURFACE_ROLE_LOCK_SURFACE:
		lock_surface_destroy(surface->lock_surface);
		return;
	case SURFACE_ROLE_XWAYLAND:
		xwayland_surface_destroy(surface->xwayland);
		return;
	}
	assert(!"unknown surface role");
}

// Checks the pending state against the role before the commit applies it.
static bool
surface_role_validate(struct surface *surface)
{
	switch (surface->role) {
	case SURFACE_ROLE_NONE:
	case SURFACE_ROLE_CURSOR:
	case SURFACE_ROLE_DND_ICON:
	case SURFACE_ROLE_SUBSURFACE:
	case SURFACE_ROLE_XDG_SURFACE:
	case SURFACE_ROLE_XDG_TOPLEVEL:
	case SURFACE_ROLE_INPUT_PANEL:
	case SURFACE_ROLE_XWAYLAND:
		return true;
	case SURFACE_ROLE_XDG_POPUP:
		return xdg_popup_validate(surface->xdg->popup);
	case SURFACE_ROLE_LAYER_SURFACE:
		return layer_surface_validate(surface->layer_surface);
	case SURFACE_ROLE_LOCK_SURFACE:
		return lock_surface_validate(surface->lock_surface);
	}
	assert(!"unknown surface role");
	return false;
}

// Applies the role's part of the state once the surface state is applied.
// The role maps the surface when it has a buffer and the role is ready.
static void
surface_role_commit(struct surface *surface, const struct surface_state *state)
{
	switch (surface->role) {
	case SURFACE_ROLE_NONE:
	case SURFACE_ROLE_XDG_SURFACE:
		return;
	case SURFACE_ROLE_CURSOR:
	case SURFACE_ROLE_DND_ICON:
		pointer_surface_commit(surface);
		return;
	case SURFACE_ROLE_SUBSURFACE:
		surface_set_mapped(surface, surface->current.buffer);
		return;
	case SURFACE_ROLE_XDG_TOPLEVEL:
		xdg_toplevel_commit(surface->xdg->toplevel, state);
		return;
	case SURFACE_ROLE_XDG_POPUP:
		xdg_popup_commit(surface->xdg->popup, state);
		return;
	case SURFACE_ROLE_LAYER_SURFACE:
		layer_surface_commit(surface->layer_surface, state);
		return;
	case SURFACE_ROLE_INPUT_PANEL:
		input_panel_surface_update(surface->input_panel);
		return;
	case SURFACE_ROLE_LOCK_SURFACE:
		lock_surface_commit(surface->lock_surface);
		return;
	case SURFACE_ROLE_XWAYLAND:
		xwayland_surface_commit(surface->xwayland, state);
		return;
	}
	assert(!"unknown surface role");
}

// The surface itself is placed between the subsurfaces below and above it.
static void
surface_state_init(struct surface_state *state)
{
	*state = (struct surface_state){
		.scale = 1,
		.viewport_source = {.width = -1.0, .height = -1.0},
		.viewport_width = -1,
		.viewport_height = -1,
	};
	wl_list_init(&state->release_callbacks);
	wl_list_init(&state->frame_callbacks);
	wl_list_init(&state->presentation_feedbacks);
	pixman_region32_init(&state->input_region);
	pixman_region32_init(&state->opaque_region);
	pixman_region32_init(&state->constraint_region);
	pixman_region32_init(&state->damage);
	pixman_region32_init(&state->buffer_damage);
	wl_array_init(&state->subsurfaces);
	struct subsurface_placement *parent =
		wl_array_add(&state->subsurfaces, sizeof(*parent));
	assert(parent);
	*parent = (struct subsurface_placement){0};
}

static void
surface_state_fini(struct surface_state *state)
{
	callbacks_destroy(&state->frame_callbacks);
	presentation_feedbacks_discard(&state->presentation_feedbacks);
	callbacks_destroy(&state->release_callbacks);
	surface_state_set_buffer(state, NULL);
	syncobj_point_unset(&state->release);
	syncobj_point_unset(&state->acquire);
	pixman_region32_fini(&state->input_region);
	pixman_region32_fini(&state->opaque_region);
	pixman_region32_fini(&state->constraint_region);
	pixman_region32_fini(&state->damage);
	pixman_region32_fini(&state->buffer_damage);
	wl_array_release(&state->subsurfaces);
	toplevel_icon_unref(state->toplevel_icon);
	image_description_unref(state->image_description);
}

void
surface_state_destroy(struct surface_state *state)
{
	surface_state_fini(state);
	free(state);
}

// Takes the committed state out of the pending state, which keeps the
// persistent values for the next commits.
static struct surface_state *
surface_take_pending(struct surface *surface)
{
	struct surface_state *pending = &surface->pending;
	struct surface_state *state = calloc(1, sizeof(*state));
	assert(state);
	surface_state_init(state);
	if (pending->buffer) {
		client_buffer_commit(pending->buffer, pending);
	}
	state->buffer_attached = pending->buffer_attached;
	state->buffer = pending->buffer;
	state->buffer_width = pending->buffer_width;
	state->buffer_height = pending->buffer_height;
	state->acquire = pending->acquire;
	list_move(&state->frame_callbacks, &pending->frame_callbacks);
	list_move(&state->presentation_feedbacks, &pending->presentation_feedbacks);
	state->transform = pending->transform;
	state->scale = pending->scale;
	state->dx = pending->dx;
	state->dy = pending->dy;
	state->viewport_source = pending->viewport_source;
	state->viewport_width = pending->viewport_width;
	state->viewport_height = pending->viewport_height;
	state->tearing = pending->tearing;
	state->image_description = pending->image_description
		? image_description_ref(pending->image_description) : NULL;
	pixman_region32_copy(&state->input_region, &pending->input_region);
	state->input_region_set = pending->input_region_set;
	pixman_region32_copy(&state->opaque_region, &pending->opaque_region);
	pixman_region32_copy(&state->constraint_region, &pending->constraint_region);
	state->constraint_region_set = pending->constraint_region_set;
	state->cursor_hint_set = pending->cursor_hint_set;
	state->cursor_hint_x = pending->cursor_hint_x;
	state->cursor_hint_y = pending->cursor_hint_y;
	pixman_region32_copy(&state->damage, &pending->damage);
	pixman_region32_copy(&state->buffer_damage, &pending->buffer_damage);
	assert(wl_array_copy(&state->subsurfaces, &pending->subsurfaces) == 0);
	state->xdg = pending->xdg;
	state->toplevel_icon = toplevel_icon_ref(pending->toplevel_icon);
	state->layer = pending->layer;
	state->xwayland_serial = pending->xwayland_serial;

	pending->buffer_attached = false;
	pending->buffer = NULL;
	pending->acquire = (struct syncobj_point){0};
	pending->dx = 0;
	pending->dy = 0;
	pixman_region32_clear(&pending->damage);
	pixman_region32_clear(&pending->buffer_damage);
	pending->xwayland_serial = 0;
	return state;
}

// Folds an older committed state into a newer one, which then applies both.
// The newer content update replaces the older one before any frame shows it.
static void
surface_state_merge(struct surface_state *state, struct surface_state *older)
{
	wl_list_insert_list(&state->frame_callbacks, &older->frame_callbacks);
	wl_list_init(&older->frame_callbacks);
	presentation_feedbacks_discard(&older->presentation_feedbacks);
	if (!state->buffer_attached) {
		state->buffer_attached = older->buffer_attached;
		state->buffer = older->buffer;
		state->acquire = older->acquire;
		older->buffer = NULL;
		older->acquire = (struct syncobj_point){0};
	}
	state->dx += older->dx;
	state->dy += older->dy;
	pixman_region32_union(&state->damage, &state->damage, &older->damage);
	pixman_region32_union(&state->buffer_damage, &state->buffer_damage,
		&older->buffer_damage);
	surface_state_destroy(older);
}

static void
surface_resource_destroy(struct wl_resource *resource)
{
	struct surface *surface = wl_resource_get_user_data(resource);
	surface_role_destroy(surface);
	surface_set_mapped(surface, false);
	// Subsurfaces outlive the roles of their parent, but not the parent.
	// Their resources stay alive but become inert until the client destroys
	// them.
	struct subsurface *subsurface, *tmp;
	wl_list_for_each_safe(subsurface, tmp, &surface->subsurfaces, parent_link) {
		subsurface_destroy(subsurface);
	}
	// Destroying the subsurface role applied the cached state.
	assert(!surface->cached);
	transactions_forget_surface(surface);
	scene_surface_fini(surface);
	wl_list_remove(&surface->link);
	pointer_surface_destroy(surface);
	server_decoration_surface_destroy(surface);
	viewport_surface_destroy(surface);
	fractional_scale_surface_destroy(surface);
	tearing_control_surface_destroy(surface);
	syncobj_surface_destroy(surface);
	dmabuf_feedback_surface_destroy(surface);
	color_management_surface_destroy(surface);
	image_description_unref(surface->preferred_image_description);
	idle_inhibitors_surface_destroy(surface);
	pointer_constraint_surface_destroy(surface);
	surface_state_fini(&surface->pending);
	surface_state_fini(&surface->current);
	texture_destroy(surface->shm_texture);
	free(surface);
}

static void
surface_attach_req(struct wl_client *client, struct wl_resource *resource,
	struct wl_resource *buffer_resource, int32_t x, int32_t y)
{
	struct surface *surface = wl_resource_get_user_data(resource);
	struct surface_state *pending = &surface->pending;
	if (wl_resource_get_version(resource) >= 5) {
		if (x != 0 || y != 0) {
			wl_resource_post_error(resource, WL_SURFACE_ERROR_INVALID_OFFSET,
				"wl_surface.attach offset must be zero");
			return;
		}
	}
	if (buffer_resource && !xdg_toplevel_decoration_allows_buffer(surface)) {
		return;
	}
	if (wl_resource_get_version(resource) < 5) {
		pending->dx = x;
		pending->dy = y;
	}
	struct client_buffer *buffer = buffer_resource
		? client_buffer_from_resource(buffer_resource) : NULL;
	surface_state_set_buffer(pending, buffer);
	pending->buffer_attached = true;
	pending->buffer_width = buffer ? buffer->width : 0;
	pending->buffer_height = buffer ? buffer->height : 0;
}

static void
surface_frame_req(struct wl_client *client, struct wl_resource *resource,
	uint32_t callback)
{
	struct surface *surface = wl_resource_get_user_data(resource);
	callback_create(client, callback, &surface->pending.frame_callbacks);
}

static bool
surface_validate_pending(struct surface *surface)
{
	struct surface_state *pending = &surface->pending;
	if (!wl_list_empty(&pending->release_callbacks) && !pending->buffer) {
		wl_resource_post_error(surface->resource, WL_SURFACE_ERROR_NO_BUFFER,
			"wl_surface.get_release requires a non-null attached buffer");
		return false;
	}
	if (pending->buffer_width % (uint32_t)pending->scale != 0
			|| pending->buffer_height % (uint32_t)pending->scale != 0) {
		wl_resource_post_error(surface->resource, WL_SURFACE_ERROR_INVALID_SIZE,
			"buffer dimensions must be a multiple of the buffer scale");
		return false;
	}
	if (pending->buffer_width > 0) {
		struct box size = surface_size_from_buffer(
			(int)pending->buffer_width, (int)pending->buffer_height,
			pending->transform, pending->scale);
		if (!viewport_validate(surface, size.width, size.height)) {
			return false;
		}
	}
	if (surface->syncobj && !syncobj_surface_validate(surface)) {
		return false;
	}
	return surface_role_validate(surface);
}

// A dma-buf is sampled until it is replaced, and the damage of an shm buffer
// is copied into the texture of the surface. The contents of an shm buffer
// that the client destroyed since the commit are lost, so the next one is
// copied whole.
static void
surface_upload_buffer(struct surface *surface, struct surface_state *state)
{
	struct client_buffer *buffer = surface->current.buffer;
	if (!buffer || buffer->type != CLIENT_BUFFER_SHM || !buffer->resource) {
		texture_destroy(surface->shm_texture);
		surface->shm_texture = NULL;
		return;
	}
	pixman_region32_t damage;
	pixman_region32_init(&damage);
	surface_state_buffer_damage(surface, state, &damage);
	client_buffer_upload_shm(buffer, &surface->shm_texture, &damage);
	pixman_region32_fini(&damage);
}

static bool
viewport_changed(const struct surface_state *a, const struct surface_state *b)
{
	return a->viewport_source.x != b->viewport_source.x
		|| a->viewport_source.y != b->viewport_source.y
		|| a->viewport_source.width != b->viewport_source.width
		|| a->viewport_source.height != b->viewport_source.height
		|| a->viewport_width != b->viewport_width
		|| a->viewport_height != b->viewport_height;
}

// Applies a committed state once its buffer is ready and the states committed
// before it are applied.
void
surface_apply_state(struct surface *surface, struct surface_state *state)
{
	struct surface_state *current = &surface->current;
	bool frame_requested = !wl_list_empty(&state->frame_callbacks);
	bool feedback_requested = !wl_list_empty(&state->presentation_feedbacks);
	bool offset_changed = (state->dx != 0 || state->dy != 0)
		&& surface->role != SURFACE_ROLE_SUBSURFACE;
	// These redraw the whole surface even if its extent stays the same. A
	// new opaque region may turn the blending of the whole surface on or off.
	bool reshaped = offset_changed
		|| state->transform != current->transform
		|| state->scale != current->scale
		|| viewport_changed(state, current)
		|| !pixman_region32_equal(&state->opaque_region,
			&current->opaque_region)
		|| state->image_description != current->image_description;
	struct box old_extent = surface_extent(surface);

	list_move(&current->frame_callbacks, &state->frame_callbacks);
	// The content update replaces those that no frame has shown.
	presentation_feedbacks_discard(&current->presentation_feedbacks);
	list_move(&current->presentation_feedbacks, &state->presentation_feedbacks);
	current->transform = state->transform;
	current->scale = state->scale;
	current->viewport_source = state->viewport_source;
	current->viewport_width = state->viewport_width;
	current->viewport_height = state->viewport_height;
	current->tearing = state->tearing;
	image_description_unref(current->image_description);
	current->image_description = state->image_description
		? image_description_ref(state->image_description) : NULL;
	if (offset_changed) {
		surface->offset_x += state->dx;
		surface->offset_y += state->dy;
	}
	pixman_region32_copy(&current->input_region, &state->input_region);
	current->input_region_set = state->input_region_set;
	pixman_region32_copy(&current->opaque_region, &state->opaque_region);
	pixman_region32_copy(&current->constraint_region, &state->constraint_region);
	current->constraint_region_set = state->constraint_region_set;
	current->cursor_hint_set = state->cursor_hint_set;
	current->cursor_hint_x = state->cursor_hint_x;
	current->cursor_hint_y = state->cursor_hint_y;
	if (state->buffer_attached) {
		surface_state_set_buffer(current, state->buffer);
	}
	current->buffer_width = state->buffer_width;
	current->buffer_height = state->buffer_height;
	if (reshaped || state->buffer_attached) {
		surface_update_size(surface);
	}
	if (state->buffer_attached) {
		surface_upload_buffer(surface, state);
	}
	pixman_region32_t content_damage;
	pixman_region32_init(&content_damage);
	surface_state_damage(surface, state, &content_damage);
	// Damage off the surface damages nothing.
	bool content_damaged = pixman_region32_not_empty(&content_damage);
	apply_subsurface_state(surface, state);
	surface_role_commit(surface, state);

	struct box new_extent = surface_extent(surface);
	if (reshaped || !box_equal(&old_extent, &new_extent)) {
		damage_extent(surface, &old_extent);
		damage_extent(surface, &new_extent);
	} else if (surface_in_scene(surface)) {
		damage_surface_region(surface, &content_damage);
	} else if (content_damaged || (frame_requested && surface->mapped)) {
		// Cursors and drag icons send their frame callbacks when drawn.
		damage_extent(surface, &new_extent);
	}
	pixman_region32_fini(&content_damage);
	// The frames of the outputs send the frame callbacks and the
	// presentation feedbacks, even if the damage is off them or there is
	// none, as clients wait for them.
	if ((frame_requested || feedback_requested) && surface_in_scene(surface)) {
		schedule_frames(surface->outputs);
	}
	// Drawing the thumbnail or capturing the window also paces the window
	// while it is hidden.
	struct window *window = window_from_surface(surface);
	if (window) {
		window_switcher_update_thumbnail(window);
		image_capture_damage_window(window);
	}
}

// A synchronized subsurface keeps its commits until the state of its parent
// is applied, and a desynchronized one applies what is left with its own.
static void
surface_commit_req(struct wl_client *client, struct wl_resource *resource)
{
	struct surface *surface = wl_resource_get_user_data(resource);
	struct surface_state *pending = &surface->pending;
	// A buffer destroyed before the commit counts as no buffer.
	if (pending->buffer && !pending->buffer->resource) {
		surface_state_set_buffer(pending, NULL);
		pending->buffer_width = 0;
		pending->buffer_height = 0;
	}
	if (!surface_validate_pending(surface)) {
		return;
	}

	struct surface_state *state = surface_take_pending(surface);
	if (surface->cached) {
		surface_state_merge(state, surface->cached);
		surface->cached = NULL;
	}
	if (surface_synchronized(surface)) {
		surface->cached = state;
	} else {
		transaction_commit(surface, state);
	}
}

static void
surface_damage_req(struct wl_client *client, struct wl_resource *resource,
	int32_t x, int32_t y, int32_t width, int32_t height)
{
	struct surface *surface = wl_resource_get_user_data(resource);
	region_add_rect(&surface->pending.damage, x, y, width, height);
}

static void
surface_damage_buffer_req(struct wl_client *client,
	struct wl_resource *resource, int32_t x, int32_t y,
	int32_t width, int32_t height)
{
	struct surface *surface = wl_resource_get_user_data(resource);
	region_add_rect(&surface->pending.buffer_damage, x, y, width, height);
}

static void
surface_set_input_region_req(struct wl_client *client,
	struct wl_resource *resource, struct wl_resource *region)
{
	struct surface *surface = wl_resource_get_user_data(resource);
	region_copy(&surface->pending.input_region, region);
	surface->pending.input_region_set = region;
}

static void
surface_set_opaque_region_req(struct wl_client *client,
	struct wl_resource *resource, struct wl_resource *region)
{
	struct surface *surface = wl_resource_get_user_data(resource);
	region_copy(&surface->pending.opaque_region, region);
}

static void
surface_set_buffer_transform_req(struct wl_client *client,
	struct wl_resource *resource, int32_t transform)
{
	if (transform < WL_OUTPUT_TRANSFORM_NORMAL
			|| transform > WL_OUTPUT_TRANSFORM_FLIPPED_270) {
		wl_resource_post_error(resource, WL_SURFACE_ERROR_INVALID_TRANSFORM,
			"invalid buffer transform");
		return;
	}
	struct surface *surface = wl_resource_get_user_data(resource);
	surface->pending.transform = transform;
}

static void
surface_set_buffer_scale_req(struct wl_client *client,
	struct wl_resource *resource, int32_t scale)
{
	if (scale <= 0) {
		wl_resource_post_error(resource, WL_SURFACE_ERROR_INVALID_SCALE,
			"buffer scale must be positive");
		return;
	}
	struct surface *surface = wl_resource_get_user_data(resource);
	surface->pending.scale = scale;
}

static void
surface_offset_req(struct wl_client *client, struct wl_resource *resource,
	int32_t x, int32_t y)
{
	struct surface *surface = wl_resource_get_user_data(resource);
	surface->pending.dx = x;
	surface->pending.dy = y;
}

static void
surface_get_release_req(struct wl_client *client, struct wl_resource *resource,
	uint32_t callback)
{
	struct surface *surface = wl_resource_get_user_data(resource);
	callback_create(client, callback, &surface->pending.release_callbacks);
}

static const struct wl_surface_interface surface_impl = {
	.destroy = resource_destroy_req,
	.attach = surface_attach_req,
	.damage = surface_damage_req,
	.frame = surface_frame_req,
	.set_opaque_region = surface_set_opaque_region_req,
	.set_input_region = surface_set_input_region_req,
	.commit = surface_commit_req,
	.set_buffer_transform = surface_set_buffer_transform_req,
	.set_buffer_scale = surface_set_buffer_scale_req,
	.damage_buffer = surface_damage_buffer_req,
	.offset = surface_offset_req,
	.get_release = surface_get_release_req,
};

static void
region_add_req(struct wl_client *client, struct wl_resource *resource,
	int32_t x, int32_t y, int32_t width, int32_t height)
{
	region_add_rect(region_from_resource(resource), x, y, width, height);
}

static void
region_subtract_req(struct wl_client *client, struct wl_resource *resource,
	int32_t x, int32_t y, int32_t width, int32_t height)
{
	if (width <= 0 || height <= 0) {
		return;
	}
	pixman_region32_t *region = region_from_resource(resource);
	pixman_region32_t rect;
	pixman_region32_init_rect(&rect, x, y, width, height);
	pixman_region32_subtract(region, region, &rect);
	pixman_region32_fini(&rect);
}

static const struct wl_region_interface region_impl = {
	.destroy = resource_destroy_req,
	.add = region_add_req,
	.subtract = region_subtract_req,
};

static void
region_resource_destroy(struct wl_resource *resource)
{
	pixman_region32_t *region = region_from_resource(resource);
	pixman_region32_fini(region);
	free(region);
}

static void
compositor_create_surface_req(struct wl_client *client, struct wl_resource *resource,
	uint32_t id)
{
	struct surface *surface = calloc(1, sizeof(*surface));
	assert(surface);
	surface_state_init(&surface->pending);
	surface_state_init(&surface->current);
	wl_list_init(&surface->transaction_entries);
	wl_list_init(&surface->subsurfaces);
	wl_list_init(&surface->popups);
	wl_list_init(&surface->focus_link);
	wl_list_init(&surface->dmabuf_feedbacks);
	wl_list_init(&surface->color_feedbacks);
	scene_surface_init(surface);
	list_append(&core.surfaces, &surface->link);
	surface->resource = wl_resource_create(client, &wl_surface_interface,
		wl_resource_get_version(resource), id);
	assert(surface->resource);
	wl_resource_set_implementation(surface->resource, &surface_impl, surface,
		surface_resource_destroy);
}

static void
compositor_create_region_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id)
{
	pixman_region32_t *data = calloc(1, sizeof(*data));
	assert(data);
	pixman_region32_init(data);
	struct wl_resource *region = wl_resource_create(client, &wl_region_interface,
		wl_resource_get_version(resource), id);
	assert(region);
	wl_resource_set_implementation(region, &region_impl, data,
		region_resource_destroy);
}

static const struct wl_compositor_interface compositor_impl = {
	.create_surface = compositor_create_surface_req,
	.create_region = compositor_create_region_req,
	.release = resource_destroy_req,
};

void
wayland_bind_compositor(struct wl_client *client, void *data, uint32_t version,
	uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&wl_compositor_interface, MIN(version, 7), id);
	assert(resource);
	wl_resource_set_implementation(resource, &compositor_impl, data, NULL);
}
