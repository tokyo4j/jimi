#include <assert.h>
#include <stdlib.h>

#include "core.h"
#include "output.h"
#include "surface.h"
#include "wlr-layer-shell-unstable-v1-protocol.h"

static_assert(ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY + 1 == NR_LAYER_SHELL_LAYERS,
	"output.layer_surfaces must have a list for each layer");

// Top layer surfaces are in the tree of their output, which fullscreen windows
// hide.
static struct scene_tree *
layer_scene_tree(struct output *output, uint32_t layer)
{
	switch (layer) {
	case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND:
		return &core.scene.layers[SCENE_LAYER_BACKGROUND];
	case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:
		return &core.scene.layers[SCENE_LAYER_BOTTOM];
	case ZWLR_LAYER_SHELL_V1_LAYER_TOP:
		return &output->top_layer;
	case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:
		return &core.scene.layers[SCENE_LAYER_OVERLAY];
	}
	assert(!"unknown layer");
	return NULL;
}

// Places a layer surface along one axis of the bounds. A surface anchored to
// both edges is centered between its margins, and fills that space if it
// left its size to the compositor.
static void
place_on_axis(int bounds_start, int bounds_size, bool anchor_start,
	bool anchor_end, int margin_start, int margin_end, int desired_size,
	int *start, int *size)
{
	*size = desired_size;
	if (anchor_start && anchor_end) {
		int available = bounds_size - margin_start - margin_end;
		if (!desired_size) {
			*size = MAX(1, available);
		}
		*start = bounds_start + margin_start + (available - *size) / 2;
	} else if (anchor_start) {
		*start = bounds_start + margin_start;
	} else if (anchor_end) {
		*start = bounds_start + bounds_size - margin_end - *size;
	} else {
		*start = bounds_start + (bounds_size - *size) / 2;
	}
}

// An exclusive zone applies to an edge the surface is anchored to alone or
// along with both perpendicular edges, unless the client picked the edge.
static uint32_t
layer_surface_exclusive_edge(const struct layer_surface_state *state)
{
	if (state->exclusive_zone <= 0) {
		return 0;
	}
	if (state->exclusive_edge) {
		return state->exclusive_edge;
	}
	bool top = state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
	bool bottom = state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
	bool left = state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
	bool right = state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
	if (top != bottom && left == right) {
		return top ? ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP
			: ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
	}
	if (left != right && top == bottom) {
		return left ? ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT
			: ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
	}
	return 0;
}

// The exclusive zone includes the margin on its edge.
static void
reserve_exclusive_zone(struct box *usable, const struct layer_surface_state *state)
{
	switch (layer_surface_exclusive_edge(state)) {
	case ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP: {
		int zone = state->exclusive_zone + state->margin.top;
		usable->y += zone;
		usable->height -= zone;
		break;
	}
	case ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM:
		usable->height -= state->exclusive_zone + state->margin.bottom;
		break;
	case ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT: {
		int zone = state->exclusive_zone + state->margin.left;
		usable->x += zone;
		usable->width -= zone;
		break;
	}
	case ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT:
		usable->width -= state->exclusive_zone + state->margin.right;
		break;
	}
}

static void
layer_surface_arrange(struct layer_surface *layer_surface, struct box *usable)
{
	struct layer_surface_state *state = &layer_surface->current;
	struct output *output = layer_surface->output;
	// A negative exclusive zone asks to ignore the other exclusive zones.
	struct box bounds = *usable;
	if (state->exclusive_zone < 0) {
		bounds = output->box;
	}

	struct box box;
	place_on_axis(bounds.x, bounds.width,
		state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT,
		state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
		state->margin.left, state->margin.right, state->width, &box.x, &box.width);
	place_on_axis(bounds.y, bounds.height,
		state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP,
		state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM,
		state->margin.top, state->margin.bottom, state->height, &box.y, &box.height);
	if (!layer_surface->configured
			|| box.width != layer_surface->configured_width
			|| box.height != layer_surface->configured_height) {
		zwlr_layer_surface_v1_send_configure(layer_surface->resource,
			++core.serial, box.width, box.height);
		layer_surface->configured = true;
		layer_surface->configured_width = box.width;
		layer_surface->configured_height = box.height;
	}
	scene_node_set_position(&layer_surface->surface->scene.tree.node, box.x, box.y);
	xdg_popups_update_positions(layer_surface->surface);
	reserve_exclusive_zone(usable, state);
}

static void
arrange_layer_surfaces(struct output *output, bool exclusive, struct box *usable)
{
	for (int layer = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
			layer >= ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND; layer--) {
		struct layer_surface *layer_surface;
		wl_list_for_each(layer_surface, &output->layer_surfaces[layer], link) {
			if (layer_surface->initialized
					&& (layer_surface->current.exclusive_zone > 0)
						== exclusive) {
				layer_surface_arrange(layer_surface, usable);
			}
		}
	}
}

// Surfaces with an exclusive zone are placed first, from the top-most layer
// down, and the others share what they leave of the output.
void
output_arrange_layer_surfaces(struct output *output)
{
	struct box usable = output->box;
	arrange_layer_surfaces(output, true, &usable);
	arrange_layer_surfaces(output, false, &usable);
	if (!box_equal(&output->usable_area, &usable)) {
		output->usable_area = usable;
		windows_arrange(output);
	}
}

static struct layer_surface *
layer_surface_create(struct surface *surface, struct wl_resource *resource,
	struct output *output, uint32_t layer)
{
	assert(surface->role == SURFACE_ROLE_NONE);

	struct layer_surface *layer_surface = calloc(1, sizeof(*layer_surface));
	assert(layer_surface);
	layer_surface->surface = surface;
	layer_surface->resource = resource;
	layer_surface->output = output;
	// Nothing is left from an earlier layer surface.
	surface->pending.layer = (struct layer_surface_state){.layer = layer};
	layer_surface->current = surface->pending.layer;
	list_append(&output->layer_surfaces[layer], &layer_surface->link);
	surface->role = SURFACE_ROLE_LAYER_SURFACE;
	surface->layer_surface = layer_surface;
	scene_node_reparent(&surface->scene.tree.node, layer_scene_tree(output, layer));
	return layer_surface;
}

// Leaves the other layer surfaces as arranged. The resource stays inert.
static void
layer_surface_free(struct layer_surface *layer_surface)
{
	struct surface *surface = layer_surface->surface;
	surface_set_mapped(surface, false);
	xdg_popups_destroy(surface);
	wl_resource_set_user_data(layer_surface->resource, NULL);
	wl_list_remove(&layer_surface->link);
	scene_node_detach(&surface->scene.tree.node);
	surface->layer_surface = NULL;
	surface->role = SURFACE_ROLE_NONE;
	free(layer_surface);
}

void
layer_surface_destroy(struct layer_surface *layer_surface)
{
	struct output *output = layer_surface->output;
	layer_surface_free(layer_surface);
	// Hand the exclusive zone back to the other layer surfaces.
	output_arrange_layer_surfaces(output);
}

// Called when the output is disabled.
void
layer_surfaces_close(struct output *output)
{
	for (int i = 0; i < NR_LAYER_SHELL_LAYERS; i++) {
		struct layer_surface *layer_surface, *tmp;
		wl_list_for_each_safe(layer_surface, tmp, &output->layer_surfaces[i], link) {
			zwlr_layer_surface_v1_send_closed(layer_surface->resource);
			layer_surface_free(layer_surface);
		}
	}
}

bool
layer_surface_validate(struct layer_surface *layer_surface)
{
	struct surface *surface = layer_surface->surface;
	struct layer_surface_state *state = &surface->pending.layer;
	if (surface->pending.buffer && !layer_surface->configured) {
		wl_resource_post_error(layer_surface->resource,
			ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_SURFACE_STATE,
			"buffer attached before the layer surface is configured");
		return false;
	}
	uint32_t horizontal = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT
		| ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
	uint32_t vertical = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP
		| ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
	if ((state->width == 0 && (state->anchor & horizontal) != horizontal)
			|| (state->height == 0
				&& (state->anchor & vertical) != vertical)) {
		wl_resource_post_error(layer_surface->resource,
			ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_SIZE,
			"a zero size requires anchors to both opposite edges");
		return false;
	}
	if ((state->anchor & state->exclusive_edge) != state->exclusive_edge) {
		wl_resource_post_error(layer_surface->resource,
			ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_EXCLUSIVE_EDGE,
			"exclusive edge is not anchored");
		return false;
	}
	return true;
}

// The first commit after creation or unmapping asks for a configure, which
// the arrangement sends. The surface maps with the arranged position.
void
layer_surface_commit(struct layer_surface *layer_surface,
	const struct surface_state *state)
{
	struct surface *surface = layer_surface->surface;
	struct layer_surface_state old = layer_surface->current;
	layer_surface->current = state->layer;
	uint32_t layer = layer_surface->current.layer;
	if (layer != old.layer) {
		wl_list_remove(&layer_surface->link);
		list_append(&layer_surface->output->layer_surfaces[layer],
			&layer_surface->link);
		scene_node_reparent(&surface->scene.tree.node,
			layer_scene_tree(layer_surface->output, layer));
	}

	bool unmap = surface->mapped && !surface->current.buffer;
	if (unmap) {
		surface_set_mapped(surface, false);
		layer_surface->initialized = false;
		layer_surface->configured = false;
	} else {
		layer_surface->initialized = true;
	}
	output_arrange_layer_surfaces(layer_surface->output);
	if (!surface->current.buffer) {
		return;
	}

	uint32_t interactivity = layer_surface->current.keyboard_interactivity;
	bool exclusive = interactivity
		== ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE;
	if (!surface->mapped) {
		surface_set_mapped(surface, true);
		if (exclusive) {
			keyboard_focus(surface);
		}
	} else if (exclusive && (old.keyboard_interactivity != interactivity
			|| old.layer != layer)) {
		keyboard_focus(surface);
	} else if (interactivity == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE
			&& old.keyboard_interactivity != interactivity) {
		keyboard_forget_surface(surface);
	}
}

// Only surfaces shown above the windows can take the keyboard exclusively.
struct surface *
layer_surface_exclusive_focus(void)
{
	for (int layer = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
			layer >= ZWLR_LAYER_SHELL_V1_LAYER_TOP; layer--) {
		struct output *output;
		wl_list_for_each(output, &core.outputs, link) {
			if (layer == ZWLR_LAYER_SHELL_V1_LAYER_TOP
					&& !output->top_layer.node.enabled) {
				continue;
			}
			struct layer_surface *layer_surface;
			wl_list_for_each_reverse(layer_surface,
					&output->layer_surfaces[layer], link) {
				uint32_t interactivity =
					layer_surface->current.keyboard_interactivity;
				if (layer_surface->surface->mapped && interactivity
						== ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE) {
					return layer_surface->surface;
				}
			}
		}
	}
	return NULL;
}

static void
layer_surface_resource_destroy(struct wl_resource *resource)
{
	struct layer_surface *layer_surface = wl_resource_get_user_data(resource);
	if (layer_surface) {
		layer_surface_destroy(layer_surface);
	}
}

// The pending state of the layer surface, or NULL if the resource is inert.
static struct layer_surface_state *
layer_surface_pending(struct wl_resource *resource)
{
	struct layer_surface *layer_surface = wl_resource_get_user_data(resource);
	return layer_surface ? &layer_surface->surface->pending.layer : NULL;
}

static void
layer_surface_set_size_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t width, uint32_t height)
{
	struct layer_surface_state *pending = layer_surface_pending(resource);
	if (pending) {
		pending->width = (int)width;
		pending->height = (int)height;
	}
}

static void
layer_surface_set_anchor_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t anchor)
{
	if (!zwlr_layer_surface_v1_anchor_is_valid(anchor,
			wl_resource_get_version(resource))) {
		wl_resource_post_error(resource, ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_ANCHOR,
			"invalid layer surface anchor");
		return;
	}
	struct layer_surface_state *pending = layer_surface_pending(resource);
	if (pending) {
		pending->anchor = anchor;
	}
}

static void
layer_surface_set_exclusive_zone_req(struct wl_client *client,
	struct wl_resource *resource, int32_t zone)
{
	struct layer_surface_state *pending = layer_surface_pending(resource);
	if (pending) {
		pending->exclusive_zone = zone;
	}
}

static void
layer_surface_set_margin_req(struct wl_client *client,
	struct wl_resource *resource, int32_t top, int32_t right, int32_t bottom,
	int32_t left)
{
	struct layer_surface_state *pending = layer_surface_pending(resource);
	if (pending) {
		pending->margin.top = top;
		pending->margin.right = right;
		pending->margin.bottom = bottom;
		pending->margin.left = left;
	}
}

static void
layer_surface_set_keyboard_interactivity_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t interactivity)
{
	if (!zwlr_layer_surface_v1_keyboard_interactivity_is_valid(interactivity,
			wl_resource_get_version(resource))) {
		wl_resource_post_error(resource,
			ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_KEYBOARD_INTERACTIVITY,
			"invalid keyboard interactivity");
		return;
	}
	struct layer_surface_state *pending = layer_surface_pending(resource);
	if (pending) {
		pending->keyboard_interactivity = interactivity;
	}
}

static void
layer_surface_get_popup_req(struct wl_client *client,
	struct wl_resource *resource, struct wl_resource *popup_resource)
{
	struct layer_surface *layer_surface = wl_resource_get_user_data(resource);
	struct xdg_popup *popup = wl_resource_get_user_data(popup_resource);
	// Popups that are inert or created with an xdg parent are left as is.
	if (!layer_surface || !popup || popup->parent) {
		return;
	}
	xdg_popup_set_parent(popup, layer_surface->surface);
}

static void
layer_surface_ack_configure_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t serial)
{
	// Layer surfaces are placed by the committed state alone, so there is
	// nothing to match the ack with.
}

static void
layer_surface_set_layer_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t layer)
{
	if (!zwlr_layer_shell_v1_layer_is_valid(layer,
			wl_resource_get_version(resource))) {
		wl_resource_post_error(resource, ZWLR_LAYER_SHELL_V1_ERROR_INVALID_LAYER,
			"invalid layer");
		return;
	}
	struct layer_surface_state *pending = layer_surface_pending(resource);
	if (pending) {
		pending->layer = layer;
	}
}

static void
layer_surface_set_exclusive_edge_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t edge)
{
	if (!zwlr_layer_surface_v1_anchor_is_valid(edge,
			wl_resource_get_version(resource)) || (edge & (edge - 1))) {
		wl_resource_post_error(resource,
			ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_EXCLUSIVE_EDGE,
			"exclusive edge must be a single edge");
		return;
	}
	struct layer_surface_state *pending = layer_surface_pending(resource);
	if (pending) {
		pending->exclusive_edge = edge;
	}
}

static const struct zwlr_layer_surface_v1_interface layer_surface_impl = {
	.set_size = layer_surface_set_size_req,
	.set_anchor = layer_surface_set_anchor_req,
	.set_exclusive_zone = layer_surface_set_exclusive_zone_req,
	.set_margin = layer_surface_set_margin_req,
	.set_keyboard_interactivity = layer_surface_set_keyboard_interactivity_req,
	.get_popup = layer_surface_get_popup_req,
	.ack_configure = layer_surface_ack_configure_req,
	.destroy = resource_destroy_req,
	.set_layer = layer_surface_set_layer_req,
	.set_exclusive_edge = layer_surface_set_exclusive_edge_req,
};

static void
layer_shell_get_layer_surface_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id,
	struct wl_resource *surface_resource, struct wl_resource *output_resource,
	uint32_t layer, const char *namespace)
{
	struct surface *surface = wl_resource_get_user_data(surface_resource);
	if (surface->role != SURFACE_ROLE_NONE) {
		wl_resource_post_error(resource, ZWLR_LAYER_SHELL_V1_ERROR_ROLE,
			"wl_surface already has another role");
		return;
	}
	if (surface_has_buffer(surface)) {
		wl_resource_post_error(resource,
			ZWLR_LAYER_SHELL_V1_ERROR_ALREADY_CONSTRUCTED,
			"wl_surface already has a buffer");
		return;
	}
	if (!zwlr_layer_shell_v1_layer_is_valid(layer,
			wl_resource_get_version(resource))) {
		wl_resource_post_error(resource,
			ZWLR_LAYER_SHELL_V1_ERROR_INVALID_LAYER, "invalid layer");
		return;
	}

	struct wl_resource *layer_resource = wl_resource_create(client,
		&zwlr_layer_surface_v1_interface, wl_resource_get_version(resource), id);
	assert(layer_resource);
	// Without an output, the surface goes where the user is.
	struct output *output = output_resource
		? wl_resource_get_user_data(output_resource)
		: output_nearest(core.seat.pointer.x, core.seat.pointer.y);
	// The output is disabled, or no output is enabled.
	if (!output) {
		wl_resource_set_implementation(layer_resource, &layer_surface_impl,
			NULL, NULL);
		zwlr_layer_surface_v1_send_closed(layer_resource);
		return;
	}
	struct layer_surface *layer_surface = layer_surface_create(surface,
		layer_resource, output, layer);
	wl_resource_set_implementation(layer_resource, &layer_surface_impl,
		layer_surface, layer_surface_resource_destroy);
}

static const struct zwlr_layer_shell_v1_interface layer_shell_impl = {
	.get_layer_surface = layer_shell_get_layer_surface_req,
	.destroy = resource_destroy_req,
};

void
wayland_bind_layer_shell(struct wl_client *client, void *data,
	uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&zwlr_layer_shell_v1_interface, MIN(version, 5), id);
	assert(resource);
	wl_resource_set_implementation(resource, &layer_shell_impl, data, NULL);
}
