#include <assert.h>
#include <stdlib.h>

#include "core.h"
#include "output.h"
#include "surface.h"
#include "xdg-shell-protocol.h"

static_assert((int)EDGE_TOP == (int)XDG_TOPLEVEL_RESIZE_EDGE_TOP
	&& (int)EDGE_BOTTOM == (int)XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM
	&& (int)EDGE_LEFT == (int)XDG_TOPLEVEL_RESIZE_EDGE_LEFT
	&& (int)EDGE_RIGHT == (int)XDG_TOPLEVEL_RESIZE_EDGE_RIGHT,
	"edges must match xdg_toplevel resize edges");

// Anchors and gravities share their values, which point to edges or corners.
static uint32_t
positioner_edges(uint32_t anchor)
{
	static const uint32_t edges[] = {
		[XDG_POSITIONER_ANCHOR_NONE] = EDGE_NONE,
		[XDG_POSITIONER_ANCHOR_TOP] = EDGE_TOP,
		[XDG_POSITIONER_ANCHOR_BOTTOM] = EDGE_BOTTOM,
		[XDG_POSITIONER_ANCHOR_LEFT] = EDGE_LEFT,
		[XDG_POSITIONER_ANCHOR_RIGHT] = EDGE_RIGHT,
		[XDG_POSITIONER_ANCHOR_TOP_LEFT] = EDGE_TOP | EDGE_LEFT,
		[XDG_POSITIONER_ANCHOR_BOTTOM_LEFT] = EDGE_BOTTOM | EDGE_LEFT,
		[XDG_POSITIONER_ANCHOR_TOP_RIGHT] = EDGE_TOP | EDGE_RIGHT,
		[XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT] = EDGE_BOTTOM | EDGE_RIGHT,
	};
	static_assert((int)XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT
		== (int)XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT,
		"xdg_positioner anchors and gravities must share values");
	assert(anchor < ARRAY_LEN(edges));
	return edges[anchor];
}

static void
xdg_positioner_resource_destroy(struct wl_resource *resource)
{
	free(wl_resource_get_user_data(resource));
}

static void
xdg_positioner_set_size_req(struct wl_client *client,
	struct wl_resource *resource, int32_t width, int32_t height)
{
	if (width <= 0 || height <= 0) {
		wl_resource_post_error(resource, XDG_POSITIONER_ERROR_INVALID_INPUT,
			"positioner size must be positive");
		return;
	}
	struct xdg_positioner_state *positioner = wl_resource_get_user_data(resource);
	positioner->width = width;
	positioner->height = height;
	positioner->size_set = true;
}

static void
xdg_positioner_set_anchor_rect_req(struct wl_client *client,
	struct wl_resource *resource,
	int32_t x, int32_t y, int32_t width, int32_t height)
{
	if (width < 0 || height < 0) {
		wl_resource_post_error(resource, XDG_POSITIONER_ERROR_INVALID_INPUT,
			"positioner anchor rectangle size must not be negative");
		return;
	}
	struct xdg_positioner_state *positioner = wl_resource_get_user_data(resource);
	positioner->anchor_rect = (struct box){
		.x = x,
		.y = y,
		.width = width,
		.height = height,
	};
	positioner->anchor_rect_set = true;
}

static void
xdg_positioner_set_anchor_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t anchor)
{
	if (!xdg_positioner_anchor_is_valid(anchor, wl_resource_get_version(resource))) {
		wl_resource_post_error(resource, XDG_POSITIONER_ERROR_INVALID_INPUT,
			"invalid positioner anchor");
		return;
	}
	struct xdg_positioner_state *positioner = wl_resource_get_user_data(resource);
	positioner->anchor = positioner_edges(anchor);
}

static void
xdg_positioner_set_gravity_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t gravity)
{
	if (!xdg_positioner_gravity_is_valid(gravity,
			wl_resource_get_version(resource))) {
		wl_resource_post_error(resource, XDG_POSITIONER_ERROR_INVALID_INPUT,
			"invalid positioner gravity");
		return;
	}
	struct xdg_positioner_state *positioner = wl_resource_get_user_data(resource);
	positioner->gravity = positioner_edges(gravity);
}

static void
xdg_positioner_set_constraint_adjustment_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t adjustment)
{
	if (!xdg_positioner_constraint_adjustment_is_valid(adjustment,
			wl_resource_get_version(resource))) {
		wl_resource_post_error(resource, XDG_POSITIONER_ERROR_INVALID_INPUT,
			"invalid positioner constraint adjustment");
		return;
	}
	struct xdg_positioner_state *positioner = wl_resource_get_user_data(resource);
	positioner->constraint_adjustment = adjustment;
}

static void
xdg_positioner_set_offset_req(struct wl_client *client,
	struct wl_resource *resource, int32_t x, int32_t y)
{
	struct xdg_positioner_state *positioner = wl_resource_get_user_data(resource);
	positioner->offset_x = x;
	positioner->offset_y = y;
}

static void
xdg_positioner_set_reactive_req(struct wl_client *client,
	struct wl_resource *resource)
{
	struct xdg_positioner_state *positioner = wl_resource_get_user_data(resource);
	positioner->reactive = true;
}

static void
xdg_positioner_set_parent_size_req(struct wl_client *client,
	struct wl_resource *resource, int32_t width, int32_t height)
{
	if (width < 0 || height < 0) {
		wl_resource_post_error(resource, XDG_POSITIONER_ERROR_INVALID_INPUT,
			"positioner parent size must not be negative");
		return;
	}
	struct xdg_positioner_state *positioner = wl_resource_get_user_data(resource);
	positioner->parent_width = width;
	positioner->parent_height = height;
	positioner->parent_size_set = true;
}

static void
xdg_positioner_set_parent_configure_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t value)
{
	struct xdg_positioner_state *positioner = wl_resource_get_user_data(resource);
	positioner->parent_configure = value;
}

static const struct xdg_positioner_interface xdg_positioner_impl = {
	.destroy = resource_destroy_req,
	.set_size = xdg_positioner_set_size_req,
	.set_anchor_rect = xdg_positioner_set_anchor_rect_req,
	.set_anchor = xdg_positioner_set_anchor_req,
	.set_gravity = xdg_positioner_set_gravity_req,
	.set_constraint_adjustment = xdg_positioner_set_constraint_adjustment_req,
	.set_offset = xdg_positioner_set_offset_req,
	.set_reactive = xdg_positioner_set_reactive_req,
	.set_parent_size = xdg_positioner_set_parent_size_req,
	.set_parent_configure = xdg_positioner_set_parent_configure_req,
};

static void
xdg_wm_base_create_positioner_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id)
{
	struct xdg_positioner_state *positioner = calloc(1, sizeof(*positioner));
	assert(positioner);
	struct wl_resource *pos = wl_resource_create(client, &xdg_positioner_interface,
		wl_resource_get_version(resource), id);
	assert(pos);
	wl_resource_set_implementation(pos, &xdg_positioner_impl, positioner,
		xdg_positioner_resource_destroy);
}

static bool
xdg_positioner_is_complete(struct xdg_positioner_state *positioner)
{
	return positioner->size_set && positioner->anchor_rect_set;
}

// Places the popup relative to the parent's window geometry. The anchor is
// the point of the anchor rectangle at its edges, or at its center along the
// axes without them, and the gravity is the direction the popup extends in
// from it.
static struct box
xdg_positioner_place(struct xdg_positioner_state *positioner,
	uint32_t anchor, uint32_t gravity)
{
	struct box *rect = &positioner->anchor_rect;
	int width = positioner->width;
	int height = positioner->height;
	int x = anchor & EDGE_LEFT ? rect->x
		: anchor & EDGE_RIGHT ? rect->x + rect->width
		: rect->x + rect->width / 2;
	int y = anchor & EDGE_TOP ? rect->y
		: anchor & EDGE_BOTTOM ? rect->y + rect->height
		: rect->y + rect->height / 2;
	x -= gravity & EDGE_LEFT ? width : gravity & EDGE_RIGHT ? 0 : width / 2;
	y -= gravity & EDGE_TOP ? height : gravity & EDGE_BOTTOM ? 0 : height / 2;
	return (struct box){
		.x = x + positioner->offset_x,
		.y = y + positioner->offset_y,
		.width = width,
		.height = height,
	};
}

// Swaps the edges along the axis of the pair of edges.
static uint32_t
flip_edges(uint32_t edges, uint32_t pair)
{
	return edges & pair ? edges ^ pair : edges;
}

static bool
fits_x(const struct box *box, const struct box *bounds)
{
	return box->x >= bounds->x && box->x + box->width <= bounds->x + bounds->width;
}

static bool
fits_y(const struct box *box, const struct box *bounds)
{
	return box->y >= bounds->y && box->y + box->height <= bounds->y + bounds->height;
}

// The window geometry of an xdg surface, or the bounds of any other surface.
static struct box
surface_xdg_window_geometry(struct surface *surface)
{
	bool xdg = surface->role == SURFACE_ROLE_XDG_TOPLEVEL
		|| surface->role == SURFACE_ROLE_XDG_POPUP;
	if (xdg && !box_empty(&surface->xdg->current.window_geometry)) {
		return surface->xdg->current.window_geometry;
	}
	return (struct box){
		.width = surface->width,
		.height = surface->height,
	};
}

// Applies the constraint adjustments in the order the protocol lists them:
// flip, slide, then resize.
static struct box
xdg_positioner_geometry(struct xdg_positioner_state *positioner,
	struct surface *parent)
{
	// Constrain to the output under the anchor rectangle, in the positioner's
	// coordinate space.
	struct box parent_geometry = surface_xdg_window_geometry(parent);
	int parent_x, parent_y;
	scene_node_layout_position(&parent->scene.node, &parent_x, &parent_y);
	parent_x += parent_geometry.x;
	parent_y += parent_geometry.y;
	struct box *rect = &positioner->anchor_rect;
	struct output *output = output_nearest(parent_x + rect->x + rect->width / 2.0,
		parent_y + rect->y + rect->height / 2.0);
	uint32_t anchor = positioner->anchor;
	uint32_t gravity = positioner->gravity;
	struct box geometry = xdg_positioner_place(positioner, anchor, gravity);
	// Nothing constrains the popup while no output is enabled.
	if (!output) {
		return geometry;
	}
	struct box bounds = {
		.x = output->box.x - parent_x,
		.y = output->box.y - parent_y,
		.width = output->box.width,
		.height = output->box.height,
	};

	uint32_t adjustment = positioner->constraint_adjustment;
	if (!fits_x(&geometry, &bounds)
			&& (adjustment & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X)) {
		struct box flipped = xdg_positioner_place(positioner,
			flip_edges(anchor, EDGE_LEFT | EDGE_RIGHT),
			flip_edges(gravity, EDGE_LEFT | EDGE_RIGHT));
		if (fits_x(&flipped, &bounds)) {
			geometry.x = flipped.x;
		}
	}
	if (!fits_y(&geometry, &bounds)
			&& (adjustment & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y)) {
		struct box flipped = xdg_positioner_place(positioner,
			flip_edges(anchor, EDGE_TOP | EDGE_BOTTOM),
			flip_edges(gravity, EDGE_TOP | EDGE_BOTTOM));
		if (fits_y(&flipped, &bounds)) {
			geometry.y = flipped.y;
		}
	}

	// Sliding keeps the top-left corner visible if the popup is too large.
	if (!fits_x(&geometry, &bounds)
			&& (adjustment & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X)) {
		geometry.x = MAX(bounds.x,
			MIN(geometry.x, bounds.x + bounds.width - geometry.width));
	}
	if (!fits_y(&geometry, &bounds)
			&& (adjustment & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y)) {
		geometry.y = MAX(bounds.y,
			MIN(geometry.y, bounds.y + bounds.height - geometry.height));
	}

	if (!fits_x(&geometry, &bounds)
			&& (adjustment & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_X)) {
		int right = bounds.x + bounds.width;
		int end = MIN(geometry.x + geometry.width, right);
		geometry.x = MAX(bounds.x, MIN(geometry.x, right - 1));
		geometry.width = MAX(1, end - geometry.x);
	}
	if (!fits_y(&geometry, &bounds)
			&& (adjustment & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_Y)) {
		int bottom = bounds.y + bounds.height;
		int end = MIN(geometry.y + geometry.height, bottom);
		geometry.y = MAX(bounds.y, MIN(geometry.y, bottom - 1));
		geometry.height = MAX(1, end - geometry.y);
	}
	return geometry;
}

// The tree of an xdg surface is at the top-left corner of its window
// geometry, so the content is placed relative to it.
static void
xdg_surface_update_content_position(struct xdg_surface *xdg)
{
	struct surface *surface = xdg->surface;
	struct box geometry = surface_xdg_window_geometry(surface);
	scene_node_set_position(&surface->scene.tree.node,
		surface->offset_x - geometry.x, surface->offset_y - geometry.y);
}

static struct xdg_toplevel *
xdg_toplevel_create(struct xdg_surface *base, struct wl_resource *resource)
{
	struct surface *surface = base->surface;
	assert(surface->role == SURFACE_ROLE_XDG_SURFACE);

	struct xdg_toplevel *toplevel = calloc(1, sizeof(*toplevel));
	assert(toplevel);
	toplevel->base = base;
	toplevel->resource = resource;
	base->toplevel = toplevel;
	base->initialized = false;
	base->configure_sent = false;
	// Nothing is left from an earlier toplevel.
	toplevel_icon_unref(surface->pending.toplevel_icon);
	surface->pending.toplevel_icon = NULL;
	surface->role = SURFACE_ROLE_XDG_TOPLEVEL;

	struct window *window = &toplevel->window;
	window_init(window, WINDOW_XDG);
	window->surface = surface;
	window->ssd = server_decoration_surface_ssd(surface);
	scene_node_reparent(&surface->scene.tree.node, &window->tree);
	return toplevel;
}

static void
xdg_toplevel_destroy(struct xdg_toplevel *toplevel)
{
	struct xdg_surface *base = toplevel->base;
	struct surface *surface = base->surface;
	struct window *window = &toplevel->window;
	if (window->mapped) {
		window_unmap(window);
	}
	xdg_toplevel_decoration_toplevel_destroy(toplevel);
	xdg_dialog_toplevel_destroy(toplevel);
	xdg_popups_destroy(surface);
	wl_resource_set_user_data(toplevel->resource, NULL);
	scene_node_detach(&surface->scene.tree.node);
	window_fini(window);
	base->toplevel = NULL;
	surface->role = SURFACE_ROLE_XDG_SURFACE;
	free(toplevel);
}

// The initial commit without a buffer asks for a configure, and the first
// buffer maps the window. Floating windows are configured with 0x0 so that
// the client picks the size, by which they are placed at the first buffer.
void
xdg_toplevel_commit(struct xdg_toplevel *toplevel, const struct surface_state *state)
{
	struct xdg_surface *base = toplevel->base;
	struct surface *surface = base->surface;
	struct window *window = &toplevel->window;
	base->current = state->xdg;
	window_set_icon(window, state->toplevel_icon);
	if (!surface->current.buffer) {
		if (window->mapped) {
			window_unmap(window);
			base->initialized = false;
			base->configure_sent = false;
		} else if (!base->initialized) {
			base->initialized = true;
			window_place(window, 0, 0);
			window_schedule_configure(window);
		}
		return;
	}

	xdg_surface_update_content_position(base);
	struct box geometry = surface_xdg_window_geometry(surface);
	window_commit(window, base->current.configure_serial, geometry.width,
		geometry.height);
	if (!window->mapped) {
		window_map(window);
	}
	xdg_popups_update_positions(surface);
}

// Compares with the last configure, or with the committed state once the
// client acked all configures. Activation always compares with the last value
// sent. The position is not configured.
static bool
xdg_toplevel_needs_configure(struct xdg_toplevel *toplevel)
{
	struct window *window = &toplevel->window;
	struct window_state *pending = &window->pending;
	struct window_state *sent = &window->current;
	if (!wl_list_empty(&window->configures)) {
		struct window_configure *last = wl_container_of(
			window->configures.prev, last, link);
		sent = &last->state;
	}
	return !toplevel->base->configure_sent
		|| xdg_toplevel_decoration_outdated(toplevel)
		|| pending->geometry.width != sent->geometry.width
		|| pending->geometry.height != sent->geometry.height
		|| window->activated != toplevel->configured_activated
		|| pending->maximized != sent->maximized
		|| pending->fullscreen != sent->fullscreen
		|| pending->tiled != sent->tiled
		|| pending->resizing != sent->resizing;
}

static void
array_add_uint32(struct wl_array *array, uint32_t value)
{
	uint32_t *entry = wl_array_add(array, sizeof(*entry));
	assert(entry);
	*entry = value;
}

static void
send_wm_capabilities(struct xdg_toplevel *toplevel)
{
	if (toplevel->wm_capabilities_sent
			|| wl_resource_get_version(toplevel->resource)
				< XDG_TOPLEVEL_WM_CAPABILITIES_SINCE_VERSION) {
		return;
	}
	struct wl_array capabilities;
	wl_array_init(&capabilities);
	array_add_uint32(&capabilities, XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE);
	array_add_uint32(&capabilities, XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN);
	array_add_uint32(&capabilities, XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE);
	xdg_toplevel_send_wm_capabilities(toplevel->resource, &capabilities);
	wl_array_release(&capabilities);
	toplevel->wm_capabilities_sent = true;
}

// Toplevels are configured only after their initial commit. The first
// configure then carries all the state set until then.
uint32_t
xdg_toplevel_configure(struct window *window)
{
	assert(window->type == WINDOW_XDG);
	struct xdg_toplevel *toplevel = wl_container_of(window, toplevel, window);
	if (!toplevel->base->initialized || !xdg_toplevel_needs_configure(toplevel)) {
		return 0;
	}

	struct window_state *state = &window->pending;
	struct wl_array states;
	wl_array_init(&states);
	if (window->activated) {
		array_add_uint32(&states, XDG_TOPLEVEL_STATE_ACTIVATED);
	}
	if (state->maximized) {
		array_add_uint32(&states, XDG_TOPLEVEL_STATE_MAXIMIZED);
	}
	if (state->fullscreen) {
		array_add_uint32(&states, XDG_TOPLEVEL_STATE_FULLSCREEN);
	}
	if (state->resizing) {
		array_add_uint32(&states, XDG_TOPLEVEL_STATE_RESIZING);
	}
	if (wl_resource_get_version(toplevel->resource)
			>= XDG_TOPLEVEL_STATE_TILED_LEFT_SINCE_VERSION) {
		if (state->tiled & EDGE_LEFT) {
			array_add_uint32(&states, XDG_TOPLEVEL_STATE_TILED_LEFT);
		}
		if (state->tiled & EDGE_RIGHT) {
			array_add_uint32(&states, XDG_TOPLEVEL_STATE_TILED_RIGHT);
		}
		if (state->tiled & EDGE_TOP) {
			array_add_uint32(&states, XDG_TOPLEVEL_STATE_TILED_TOP);
		}
		if (state->tiled & EDGE_BOTTOM) {
			array_add_uint32(&states, XDG_TOPLEVEL_STATE_TILED_BOTTOM);
		}
	}
	send_wm_capabilities(toplevel);
	xdg_toplevel_send_configure(toplevel->resource, state->geometry.width,
		state->geometry.height, &states);
	wl_array_release(&states);
	if (xdg_toplevel_decoration_outdated(toplevel)) {
		xdg_toplevel_decoration_configure(toplevel);
	}
	toplevel->configured_activated = window->activated;
	toplevel->base->configure_sent = true;
	uint32_t serial = ++core.serial;
	xdg_surface_send_configure(toplevel->base->resource, serial);
	return serial;
}

void
xdg_toplevel_close(struct window *window)
{
	assert(window->type == WINDOW_XDG);
	struct xdg_toplevel *toplevel = wl_container_of(window, toplevel, window);
	xdg_toplevel_send_close(toplevel->resource);
}

static void
xdg_popup_configure(struct xdg_popup *popup)
{
	xdg_popup_send_configure(popup->resource, popup->geometry.x, popup->geometry.y,
		popup->geometry.width, popup->geometry.height);
	popup->base->configure_sent = true;
	xdg_surface_send_configure(popup->base->resource, ++core.serial);
}

static void
xdg_popup_update_position(struct xdg_popup *popup)
{
	struct surface *parent = popup->parent;
	assert(parent);
	bool geometry_changed = false;
	if (popup->positioner.reactive && !popup->dismissed) {
		struct box geometry = xdg_positioner_geometry(&popup->positioner, parent);
		geometry_changed = !box_equal(&geometry, &popup->geometry);
		popup->geometry = geometry;
	}

	// Popups of layer surfaces are placed in the scene layer above the
	// windows, so they are positioned in layout coordinates.
	int x = popup->geometry.x;
	int y = popup->geometry.y;
	if (parent->role == SURFACE_ROLE_LAYER_SURFACE) {
		x += parent->scene.tree.node.x;
		y += parent->scene.tree.node.y;
	}
	scene_node_set_position(&popup->tree.node, x, y);
	if (geometry_changed) {
		xdg_popup_configure(popup);
	}
}

void
xdg_popups_update_positions(struct surface *parent)
{
	struct xdg_popup *popup;
	wl_list_for_each(popup, &parent->popups, parent_link) {
		xdg_popup_update_position(popup);
		xdg_popups_update_positions(popup->base->surface);
	}
}

void
xdg_popups_dismiss(struct surface *parent)
{
	struct xdg_popup *popup;
	wl_list_for_each_reverse(popup, &parent->popups, parent_link) {
		xdg_popup_dismiss(popup);
	}
}

// Popups cannot outlive the role of their parent, which places them. Their
// resources stay alive but become inert until the client destroys them.
void
xdg_popups_destroy(struct surface *parent)
{
	struct xdg_popup *popup, *tmp;
	wl_list_for_each_safe(popup, tmp, &parent->popups, parent_link) {
		xdg_popup_dismiss(popup);
		xdg_popup_destroy(popup);
	}
}

// A popup without a parent is kept out of the scene until a layer surface
// adopts it.
static struct xdg_popup *
xdg_popup_create(struct xdg_surface *base, struct wl_resource *resource,
	struct surface *parent, struct xdg_positioner_state positioner)
{
	struct surface *surface = base->surface;
	assert(surface->role == SURFACE_ROLE_XDG_SURFACE);

	struct xdg_popup *popup = calloc(1, sizeof(*popup));
	assert(popup);
	popup->base = base;
	popup->resource = resource;
	popup->positioner = positioner;
	wl_list_init(&popup->parent_link);
	base->popup = popup;
	base->configure_sent = false;
	surface->role = SURFACE_ROLE_XDG_POPUP;

	scene_tree_init(&popup->tree, NULL);
	scene_node_reparent(&surface->scene.tree.node, &popup->tree);
	if (parent) {
		xdg_popup_set_parent(popup, parent);
	}
	return popup;
}

// Popups of layer surfaces stay above the windows even if their parent is
// below them.
static struct scene_tree *
xdg_popup_parent_tree(struct surface *parent)
{
	switch (parent->role) {
	case SURFACE_ROLE_XDG_TOPLEVEL:
		return &parent->xdg->toplevel->window.tree;
	case SURFACE_ROLE_XDG_POPUP:
		return &parent->xdg->popup->tree;
	case SURFACE_ROLE_LAYER_SURFACE:
		return &core.scene.layers[SCENE_LAYER_POPUP];
	case SURFACE_ROLE_NONE:
	case SURFACE_ROLE_CURSOR:
	case SURFACE_ROLE_DND_ICON:
	case SURFACE_ROLE_SUBSURFACE:
	case SURFACE_ROLE_XDG_SURFACE:
	case SURFACE_ROLE_INPUT_PANEL:
	case SURFACE_ROLE_LOCK_SURFACE:
	case SURFACE_ROLE_XWAYLAND:
		break;
	}
	assert(!"invalid popup parent");
	return NULL;
}

static struct xdg_popup *
xdg_popup_parent_popup(struct xdg_popup *popup)
{
	struct surface *parent = popup->parent;
	return parent->role == SURFACE_ROLE_XDG_POPUP ? parent->xdg->popup : NULL;
}

// A grab nests on the parent popup's grab, or starts a new one from a
// toplevel or layer surface when no grab is active. Clients may request it
// before a layer surface adopts the popup, so it waits for the parent.
static void
xdg_popup_grab(struct xdg_popup *popup)
{
	struct surface *parent = popup->parent;
	if (!parent) {
		popup->grab_pending = true;
		return;
	}
	if (!parent->mapped
			|| core.seat.pointer.grabbed_popup != xdg_popup_parent_popup(popup)) {
		wl_resource_post_error(popup->base->resource,
			XDG_WM_BASE_ERROR_INVALID_SURFACE_STATE,
			"popup parent does not own the active grab");
		return;
	}
	// Like wlroots and KWin, the serial is not checked. Toolkits pass
	// serials of pointer, keyboard or touch events on any surface of the
	// popup chain, and clicking outside dismisses the popups anyway.
	popup->grabbed = true;
	core.seat.pointer.grabbed_popup = popup;
}

void
xdg_popup_set_parent(struct xdg_popup *popup, struct surface *parent)
{
	assert(!popup->parent);
	popup->parent = parent;
	list_append(&parent->popups, &popup->parent_link);
	scene_node_reparent(&popup->tree.node, xdg_popup_parent_tree(parent));
	popup->geometry = xdg_positioner_geometry(&popup->positioner, parent);
	xdg_popup_update_position(popup);
	if (popup->grab_pending) {
		popup->grab_pending = false;
		xdg_popup_grab(popup);
	}
}

// Hands the pointer grab back to the parent popup if it still holds one.
static void
xdg_popup_ungrab(struct xdg_popup *popup)
{
	if (core.seat.pointer.grabbed_popup != popup) {
		return;
	}
	struct xdg_popup *parent = xdg_popup_parent_popup(popup);
	core.seat.pointer.grabbed_popup = parent && parent->grabbed
		&& !parent->dismissed ? parent : NULL;
}

void
xdg_popup_destroy(struct xdg_popup *popup)
{
	struct xdg_surface *base = popup->base;
	struct surface *surface = base->surface;
	surface_set_mapped(surface, false);
	xdg_popups_destroy(surface);
	xdg_popup_ungrab(popup);
	wl_resource_set_user_data(popup->resource, NULL);
	wl_list_remove(&popup->parent_link);
	scene_node_detach(&surface->scene.tree.node);
	scene_tree_fini(&popup->tree);
	base->popup = NULL;
	surface->role = SURFACE_ROLE_XDG_SURFACE;
	free(popup);
}

static void
xdg_popup_dismiss_tree(struct xdg_popup *popup)
{
	if (popup->dismissed) {
		return;
	}
	struct xdg_popup *child;
	wl_list_for_each_reverse(child, &popup->base->surface->popups, parent_link) {
		xdg_popup_dismiss_tree(child);
	}
	xdg_popup_ungrab(popup);
	popup->dismissed = true;
	popup->grabbed = false;
	xdg_popup_send_popup_done(popup->resource);
	surface_set_mapped(popup->base->surface, false);
}

void
xdg_popup_dismiss(struct xdg_popup *popup)
{
	if (popup->dismissed) {
		return;
	}
	// The focus goes back at once rather than through each dismissed popup.
	keyboard_forget_surface(popup->base->surface);
	xdg_popup_dismiss_tree(popup);
}

// Pressing outside of the popups of a grab closes them, and pressing a popup
// closes those above it. Returns whether the press is on no surface, which
// only closes the popups.
bool
xdg_popup_handle_press(struct surface *surface)
{
	struct xdg_popup *popup = core.seat.pointer.grabbed_popup;
	if (!popup || popup->dismissed) {
		return false;
	}
	struct xdg_popup *dismiss = popup;
	for (;;) {
		if (surface && surface_contains(popup->base->surface, surface)) {
			if (popup != core.seat.pointer.grabbed_popup) {
				xdg_popup_dismiss(dismiss);
			}
			return false;
		}
		dismiss = popup;
		popup = xdg_popup_parent_popup(popup);
		if (!popup || !popup->grabbed || popup->dismissed) {
			xdg_popup_dismiss(dismiss);
			return surface == NULL;
		}
	}
}

bool
xdg_popup_validate(struct xdg_popup *popup)
{
	if (!popup->parent) {
		wl_resource_post_error(popup->base->resource,
			XDG_WM_BASE_ERROR_INVALID_POPUP_PARENT,
			"xdg_popup committed without a parent");
		return false;
	}
	return true;
}

// The initial commit asks for a configure, and the first buffer maps the
// popup.
void
xdg_popup_commit(struct xdg_popup *popup, const struct surface_state *state)
{
	struct xdg_surface *base = popup->base;
	struct surface *surface = base->surface;
	base->current = state->xdg;
	if (!base->configure_sent) {
		xdg_popup_configure(popup);
	}
	xdg_surface_update_content_position(base);
	if (!surface->current.buffer) {
		if (surface->mapped) {
			// Unmapping a grabbing popup ends its grab.
			if (popup->grabbed) {
				xdg_popup_dismiss(popup);
			}
			surface_set_mapped(surface, false);
			// A remapped popup starts over with an initial configure.
			base->configure_sent = false;
		}
		return;
	}
	// A dismissed popup stays hidden until the client destroys it.
	if (!surface->mapped && !popup->dismissed) {
		surface_set_mapped(surface, true);
		if (popup->grabbed) {
			keyboard_focus(surface);
		}
	}
	xdg_popups_update_positions(surface);
}

void
xdg_surface_destroy(struct xdg_surface *xdg)
{
	struct surface *surface = xdg->surface;
	if (surface->role == SURFACE_ROLE_XDG_TOPLEVEL) {
		xdg_toplevel_destroy(xdg->toplevel);
	} else if (surface->role == SURFACE_ROLE_XDG_POPUP) {
		xdg_popup_destroy(xdg->popup);
	}
	assert(surface->role == SURFACE_ROLE_XDG_SURFACE);
	wl_resource_set_user_data(xdg->resource, NULL);
	surface->role = SURFACE_ROLE_NONE;
	surface->xdg = NULL;
	free(xdg);
}

static void
xdg_surface_resource_destroy(struct wl_resource *resource)
{
	struct xdg_surface *xdg = wl_resource_get_user_data(resource);
	if (xdg) {
		xdg_surface_destroy(xdg);
	}
}

static void
xdg_toplevel_resource_destroy(struct wl_resource *resource)
{
	struct xdg_toplevel *toplevel = wl_resource_get_user_data(resource);
	if (toplevel) {
		xdg_toplevel_destroy(toplevel);
	}
}

static void
xdg_toplevel_set_fullscreen_req(struct wl_client *client,
	struct wl_resource *resource, struct wl_resource *output)
{
	struct xdg_toplevel *toplevel = wl_resource_get_user_data(resource);
	if (toplevel) {
		window_set_fullscreen(&toplevel->window, true);
	}
}

static void
xdg_toplevel_unset_fullscreen_req(struct wl_client *client,
	struct wl_resource *resource)
{
	struct xdg_toplevel *toplevel = wl_resource_get_user_data(resource);
	if (toplevel) {
		window_set_fullscreen(&toplevel->window, false);
	}
}

static void
xdg_toplevel_set_maximized_req(struct wl_client *client,
	struct wl_resource *resource)
{
	struct xdg_toplevel *toplevel = wl_resource_get_user_data(resource);
	if (toplevel) {
		window_set_maximized(&toplevel->window, true);
	}
}

static void
xdg_toplevel_unset_maximized_req(struct wl_client *client,
	struct wl_resource *resource)
{
	struct xdg_toplevel *toplevel = wl_resource_get_user_data(resource);
	if (toplevel) {
		window_set_maximized(&toplevel->window, false);
	}
}

static void
xdg_toplevel_set_title_req(struct wl_client *client, struct wl_resource *resource,
	const char *title)
{
	struct xdg_toplevel *toplevel = wl_resource_get_user_data(resource);
	if (toplevel) {
		window_set_title(&toplevel->window, title);
	}
}

static void
xdg_toplevel_set_app_id_req(struct wl_client *client,
	struct wl_resource *resource, const char *app_id)
{
	struct xdg_toplevel *toplevel = wl_resource_get_user_data(resource);
	if (toplevel) {
		window_set_app_id(&toplevel->window, app_id);
	}
}

static void
xdg_toplevel_set_parent_req(struct wl_client *client,
	struct wl_resource *resource, struct wl_resource *parent_resource)
{
	struct xdg_toplevel *toplevel = wl_resource_get_user_data(resource);
	struct xdg_toplevel *parent = parent_resource
		? wl_resource_get_user_data(parent_resource) : NULL;
	if (!toplevel) {
		return;
	}
	if (parent_resource && (!parent || parent == toplevel)) {
		wl_resource_post_error(resource, XDG_TOPLEVEL_ERROR_INVALID_PARENT,
			"invalid xdg_toplevel parent");
		return;
	}
	if (parent && window_has_ancestor(&parent->window, &toplevel->window)) {
		wl_resource_post_error(resource, XDG_TOPLEVEL_ERROR_INVALID_PARENT,
			"xdg_toplevel parent would create a cycle");
		return;
	}
	window_set_parent(&toplevel->window, parent ? &parent->window : NULL);
}

static void
xdg_toplevel_set_min_size_req(struct wl_client *client,
	struct wl_resource *resource, int32_t width, int32_t height)
{
	if (width < 0 || height < 0) {
		wl_resource_post_error(resource, XDG_TOPLEVEL_ERROR_INVALID_SIZE,
			"toplevel minimum size must not be negative");
		return;
	}
	struct xdg_toplevel *toplevel = wl_resource_get_user_data(resource);
	if (toplevel) {
		toplevel->window.min_width = width;
		toplevel->window.min_height = height;
	}
}

static void
xdg_toplevel_set_max_size_req(struct wl_client *client,
	struct wl_resource *resource, int32_t width, int32_t height)
{
	if (width < 0 || height < 0) {
		wl_resource_post_error(resource, XDG_TOPLEVEL_ERROR_INVALID_SIZE,
			"toplevel maximum size must not be negative");
		return;
	}
	struct xdg_toplevel *toplevel = wl_resource_get_user_data(resource);
	if (toplevel) {
		toplevel->window.max_width = width;
		toplevel->window.max_height = height;
	}
}

static void
xdg_toplevel_move_req(struct wl_client *client, struct wl_resource *resource,
	struct wl_resource *seat, uint32_t serial)
{
	struct xdg_toplevel *toplevel = wl_resource_get_user_data(resource);
	if (!toplevel) {
		return;
	}
	pointer_request_move(&toplevel->window, serial);
}

static void
xdg_toplevel_resize_req(struct wl_client *client, struct wl_resource *resource,
	struct wl_resource *seat, uint32_t serial, uint32_t edges)
{
	struct xdg_toplevel *toplevel = wl_resource_get_user_data(resource);
	if (!toplevel) {
		return;
	}
	pointer_request_resize(&toplevel->window, serial, edges);
}

static void
xdg_toplevel_show_window_menu_req(struct wl_client *client,
	struct wl_resource *resource, struct wl_resource *seat, uint32_t serial,
	int32_t x, int32_t y)
{
	// Jimi has no compositor-provided window menu.
}

static void
xdg_toplevel_set_minimized_req(struct wl_client *client,
	struct wl_resource *resource)
{
	struct xdg_toplevel *toplevel = wl_resource_get_user_data(resource);
	if (toplevel) {
		window_set_minimized(&toplevel->window, true);
	}
}

static const struct xdg_toplevel_interface xdg_toplevel_impl = {
	.destroy = resource_destroy_req,
	.set_parent = xdg_toplevel_set_parent_req,
	.set_title = xdg_toplevel_set_title_req,
	.set_app_id = xdg_toplevel_set_app_id_req,
	.show_window_menu = xdg_toplevel_show_window_menu_req,
	.move = xdg_toplevel_move_req,
	.resize = xdg_toplevel_resize_req,
	.set_max_size = xdg_toplevel_set_max_size_req,
	.set_min_size = xdg_toplevel_set_min_size_req,
	.set_maximized = xdg_toplevel_set_maximized_req,
	.unset_maximized = xdg_toplevel_unset_maximized_req,
	.set_fullscreen = xdg_toplevel_set_fullscreen_req,
	.unset_fullscreen = xdg_toplevel_unset_fullscreen_req,
	.set_minimized = xdg_toplevel_set_minimized_req,
};

static void
xdg_popup_resource_destroy(struct wl_resource *resource)
{
	struct xdg_popup *popup = wl_resource_get_user_data(resource);
	if (popup) {
		xdg_popup_destroy(popup);
	}
}

static void
xdg_popup_destroy_req(struct wl_client *client, struct wl_resource *resource)
{
	struct xdg_popup *popup = wl_resource_get_user_data(resource);
	if (popup && (!wl_list_empty(&popup->base->surface->popups)
			|| (core.seat.pointer.grabbed_popup
				&& core.seat.pointer.grabbed_popup != popup
				&& surface_contains(popup->base->surface,
					core.seat.pointer.grabbed_popup->base->surface)))) {
		wl_resource_post_error(popup->base->resource,
			XDG_WM_BASE_ERROR_NOT_THE_TOPMOST_POPUP,
			"tried to destroy a non-topmost popup");
		return;
	}
	wl_resource_destroy(resource);
}

static void
xdg_popup_grab_req(struct wl_client *client, struct wl_resource *resource,
	struct wl_resource *seat, uint32_t serial)
{
	struct xdg_popup *popup = wl_resource_get_user_data(resource);
	if (!popup) {
		return;
	}
	if (popup->base->surface->mapped) {
		wl_resource_post_error(resource, XDG_POPUP_ERROR_INVALID_GRAB,
			"cannot grab an already mapped popup");
		return;
	}
	xdg_popup_grab(popup);
}

static void
xdg_popup_reposition_req(struct wl_client *client,
	struct wl_resource *resource, struct wl_resource *positioner, uint32_t token)
{
	struct xdg_popup *popup = wl_resource_get_user_data(resource);
	struct xdg_positioner_state *positioner_state =
		wl_resource_get_user_data(positioner);
	if (!popup) {
		return;
	}
	if (!positioner_state || !xdg_positioner_is_complete(positioner_state)) {
		wl_resource_post_error(popup->base->resource,
			XDG_WM_BASE_ERROR_INVALID_POSITIONER, "xdg_positioner is incomplete");
		return;
	}
	popup->positioner = *positioner_state;
	if (!popup->parent) {
		// The geometry is computed once a layer surface adopts the popup.
		return;
	}
	popup->geometry = xdg_positioner_geometry(positioner_state, popup->parent);
	xdg_popup_update_position(popup);
	xdg_popup_send_repositioned(resource, token);
	xdg_popup_configure(popup);
}

static const struct xdg_popup_interface xdg_popup_impl = {
	.destroy = xdg_popup_destroy_req,
	.grab = xdg_popup_grab_req,
	.reposition = xdg_popup_reposition_req,
};

static void
xdg_surface_get_toplevel_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id)
{
	struct wl_resource *toplevel_resource = wl_resource_create(client,
		&xdg_toplevel_interface, wl_resource_get_version(resource), id);
	assert(toplevel_resource);
	wl_resource_set_implementation(toplevel_resource, &xdg_toplevel_impl,
		NULL, xdg_toplevel_resource_destroy);
	struct xdg_surface *xdg = wl_resource_get_user_data(resource);
	if (!xdg) {
		// The wl_surface is gone, so the new object is inert as well.
		return;
	}
	if (xdg->surface->role != SURFACE_ROLE_XDG_SURFACE) {
		wl_resource_post_error(resource, XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED,
			"xdg_surface already has a role object");
		return;
	}

	struct xdg_toplevel *toplevel = xdg_toplevel_create(xdg, toplevel_resource);
	wl_resource_set_user_data(toplevel_resource, toplevel);
}

static void
xdg_surface_set_window_geometry_req(struct wl_client *client,
	struct wl_resource *resource, int32_t x, int32_t y, int32_t width,
	int32_t height)
{
	struct xdg_surface *xdg = wl_resource_get_user_data(resource);
	if (!xdg || xdg->surface->role == SURFACE_ROLE_XDG_SURFACE) {
		wl_resource_post_error(resource, XDG_SURFACE_ERROR_NOT_CONSTRUCTED,
			"xdg_surface must have a role");
		return;
	}
	if (width <= 0 || height <= 0) {
		wl_resource_post_error(resource, XDG_SURFACE_ERROR_INVALID_SIZE,
			"invalid window geometry size");
		return;
	}
	xdg->surface->pending.xdg.window_geometry = (struct box){
		.x = x,
		.y = y,
		.width = width,
		.height = height,
	};
}

static void
xdg_surface_ack_configure_req(struct wl_client *client, struct wl_resource *resource,
	uint32_t serial)
{
	struct xdg_surface *xdg = wl_resource_get_user_data(resource);
	if (xdg) {
		xdg->surface->pending.xdg.configure_serial = serial;
	}
}

static void
xdg_surface_get_popup_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id, struct wl_resource *parent,
	struct wl_resource *positioner)
{
	struct wl_resource *popup_resource = wl_resource_create(client,
		&xdg_popup_interface, wl_resource_get_version(resource), id);
	assert(popup_resource);
	wl_resource_set_implementation(popup_resource, &xdg_popup_impl, NULL,
		xdg_popup_resource_destroy);
	struct xdg_surface *xdg = wl_resource_get_user_data(resource);
	if (!xdg) {
		// The wl_surface is gone, so the new object is inert as well.
		return;
	}
	if (xdg->surface->role != SURFACE_ROLE_XDG_SURFACE) {
		wl_resource_post_error(resource, XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED,
			"xdg_surface already has a role object");
		return;
	}

	struct xdg_positioner_state *positioner_state =
		wl_resource_get_user_data(positioner);
	if (!positioner_state || !xdg_positioner_is_complete(positioner_state)) {
		wl_resource_post_error(resource, XDG_WM_BASE_ERROR_INVALID_POSITIONER,
			"xdg_positioner is incomplete");
		return;
	}

	// Without a parent, the popup waits for zwlr_layer_surface_v1.get_popup.
	struct surface *parent_surface = NULL;
	if (parent) {
		struct xdg_surface *parent_xdg = wl_resource_get_user_data(parent);
		parent_surface = parent_xdg ? parent_xdg->surface : NULL;
		if (!parent_surface
				|| (parent_surface->role != SURFACE_ROLE_XDG_TOPLEVEL
					&& parent_surface->role != SURFACE_ROLE_XDG_POPUP)) {
			wl_resource_post_error(resource, XDG_WM_BASE_ERROR_INVALID_POPUP_PARENT,
				"xdg_popup parent has no xdg role");
			return;
		}
	}

	struct xdg_popup *popup = xdg_popup_create(xdg, popup_resource,
		parent_surface, *positioner_state);
	wl_resource_set_user_data(popup_resource, popup);
}

static const struct xdg_surface_interface xdg_surface_impl = {
	.destroy = resource_destroy_req,
	.get_toplevel = xdg_surface_get_toplevel_req,
	.get_popup = xdg_surface_get_popup_req,
	.set_window_geometry = xdg_surface_set_window_geometry_req,
	.ack_configure = xdg_surface_ack_configure_req,
};

static void
xdg_wm_base_get_xdg_surface_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id, struct wl_resource *surface_resource)
{
	struct surface *surface = wl_resource_get_user_data(surface_resource);
	if (surface->role != SURFACE_ROLE_NONE) {
		wl_resource_post_error(resource, XDG_WM_BASE_ERROR_ROLE,
			"wl_surface already has another role");
		return;
	}

	struct wl_resource *xdg_resource = wl_resource_create(client,
		&xdg_surface_interface, wl_resource_get_version(resource), id);
	assert(xdg_resource);
	struct xdg_surface *xdg = calloc(1, sizeof(*xdg));
	assert(xdg);
	xdg->surface = surface;
	xdg->resource = xdg_resource;
	// Nothing is left from an earlier xdg_surface.
	surface->pending.xdg = (struct xdg_surface_state){0};
	surface->role = SURFACE_ROLE_XDG_SURFACE;
	surface->xdg = xdg;
	wl_resource_set_implementation(xdg_resource, &xdg_surface_impl, xdg,
		xdg_surface_resource_destroy);
}

static void
xdg_wm_base_pong_req(struct wl_client *client, struct wl_resource *resource,
	uint32_t serial)
{
	// Jimi does not send ping events, so there is no pending ping to match.
}

static const struct xdg_wm_base_interface xdg_wm_base_impl = {
	.destroy = resource_destroy_req,
	.create_positioner = xdg_wm_base_create_positioner_req,
	.get_xdg_surface = xdg_wm_base_get_xdg_surface_req,
	.pong = xdg_wm_base_pong_req,
};

void
wayland_bind_xdg_wm_base(struct wl_client *client, void *data, uint32_t version,
	uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client, &xdg_wm_base_interface,
		MIN(version, 6), id);
	assert(resource);
	wl_resource_set_implementation(resource, &xdg_wm_base_impl, data, NULL);
}
