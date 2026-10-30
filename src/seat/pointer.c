#include <assert.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdlib.h>

#include "config.h"
#include "core.h"
#include "output.h"
#include "surface.h"
#include "wlr-layer-shell-unstable-v1-protocol.h"


struct pointer_resource {
	struct wl_resource *resource;
	struct wl_list link; // core.seat.pointer.resources
	// wp_cursor_shape_device_v1 resource links, which are inert once the
	// wl_pointer is destroyed.
	struct wl_list cursor_shape_devices;
	uint32_t enter_serial;
};

static struct surface *
drag_icon(void)
{
	struct pointer_grab *grab = &core.seat.pointer.grab;
	return grab->type == POINTER_GRAB_DND ? grab->drag.icon : NULL;
}

struct surface *
pointer_drag_icon_surface(void)
{
	struct surface *surface = drag_icon();
	return surface && surface->mapped && surface->current.buffer ? surface : NULL;
}

// The surface whose client picks the cursor
static struct surface *
cursor_focus(void)
{
	struct tablet_tool *tool = core.seat.pointer.tablet_tool;
	return tool ? tool->focus : core.seat.pointer.focus;
}

struct surface *
pointer_client_cursor_surface(void)
{
	struct surface *surface = core.seat.pointer.cursor.surface;
	return surface && surface->mapped && surface->current.buffer
		&& resource_matches_surface(surface->resource, cursor_focus())
		? surface : NULL;
}

// The shape drawn from the cursor theme, or 0 if none is. The compositor picks
// it while no client has the pointer, as during grabs.
enum wp_cursor_shape_device_v1_shape
pointer_cursor_shape(void)
{
	struct pointer *pointer = &core.seat.pointer;
	if (pointer_client_cursor_surface()) {
		return 0;
	}
	struct surface *focus = cursor_focus();
	if (pointer->cursor.shape
			&& resource_matches_surface(pointer->cursor.resource, focus)) {
		return pointer->cursor.shape;
	}
	return focus ? 0 : pointer->server_cursor_shape;
}

// The cursor and the drag icon are placed at the pointer by their hotspot,
// and moved by the offsets committed to them.
static struct box
surface_box_at(struct surface *surface, double x, double y)
{
	struct pointer *pointer = &core.seat.pointer;
	bool cursor = surface == pointer->cursor.surface;
	assert(cursor || surface == drag_icon());
	int hotspot_x = cursor ? pointer->cursor.hotspot_x
		: pointer->grab.drag.icon_hotspot_x;
	int hotspot_y = cursor ? pointer->cursor.hotspot_y
		: pointer->grab.drag.icon_hotspot_y;
	return (struct box){
		.x = (int)x - hotspot_x + surface->offset_x,
		.y = (int)y - hotspot_y + surface->offset_y,
		.width = surface->width,
		.height = surface->height,
	};
}

// Where the cursor or the drag icon surface is drawn.
struct box
pointer_surface_box(struct surface *surface)
{
	return surface_box_at(surface, core.seat.pointer.x, core.seat.pointer.y);
}

static struct box
drag_icon_box_at(double x, double y)
{
	struct surface *drag_icon = pointer_drag_icon_surface();
	return drag_icon ? surface_box_at(drag_icon, x, y) : (struct box){0};
}

// Where the cursor is shown with the pointer at the point.
static struct box
cursor_extent_at(double x, double y)
{
	struct surface *surface = pointer_client_cursor_surface();
	if (surface) {
		return surface_box_at(surface, x, y);
	}
	enum wp_cursor_shape_device_v1_shape shape = pointer_cursor_shape();
	if (!shape) {
		return (struct box){0};
	}
	// Each output shows its image, in its physical pixels.
	struct box extent = {0};
	struct output *output;
	wl_list_for_each(output, &core.outputs, link) {
		if (!output->enabled) {
			continue;
		}
		struct server_cursor *server_cursor = &output->server_cursors[shape];
		double x1 = x - server_cursor->hotspot_x / output->scale;
		double y1 = y - server_cursor->hotspot_y / output->scale;
		double x2 = x1 + server_cursor->width / output->scale;
		double y2 = y1 + server_cursor->height / output->scale;
		struct box box = {
			.x = (int)floor(x1),
			.y = (int)floor(y1),
			.width = (int)ceil(x2) - (int)floor(x1),
			.height = (int)ceil(y2) - (int)floor(y1),
		};
		box_union(&extent, &box);
	}
	return extent;
}

// The cursor or the drag icon is shown or hidden, or has changed.
static void
pointer_damage_cursor(void)
{
	struct pointer *pointer = &core.seat.pointer;
	struct box drag_icon = drag_icon_box_at(pointer->x, pointer->y);
	damage_layout_box(&drag_icon);
	struct box cursor = cursor_extent_at(pointer->x, pointer->y);
	damage_cursor(&cursor, false);
}

static void
pointer_set_server_cursor_shape(enum wp_cursor_shape_device_v1_shape shape)
{
	if (core.seat.pointer.server_cursor_shape == shape) {
		return;
	}
	pointer_damage_cursor();
	core.seat.pointer.server_cursor_shape = shape;
	pointer_damage_cursor();
}

static enum wp_cursor_shape_device_v1_shape
pointer_cursor_shape_for_resize_edges(uint32_t edges)
{
	uint32_t horizontal_edges = EDGE_LEFT | EDGE_RIGHT;
	uint32_t vertical_edges = EDGE_TOP | EDGE_BOTTOM;
	if ((edges & horizontal_edges) && (edges & vertical_edges)) {
		bool nwse = ((edges & EDGE_TOP) && (edges & EDGE_LEFT))
			|| ((edges & EDGE_BOTTOM) && (edges & EDGE_RIGHT));
		return nwse ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE
			: WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE;
	}
	if (edges & horizontal_edges) {
		return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE;
	}
	if (edges & vertical_edges) {
		return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE;
	}
	return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
}

static void
damage_cursor_move(double old_x, double old_y)
{
	struct pointer *pointer = &core.seat.pointer;
	struct box old_drag_icon = drag_icon_box_at(old_x, old_y);
	struct box new_drag_icon = drag_icon_box_at(pointer->x, pointer->y);
	struct box old_cursor = cursor_extent_at(old_x, old_y);
	struct box new_cursor = cursor_extent_at(pointer->x, pointer->y);
	damage_layout_box(&old_drag_icon);
	damage_layout_box(&new_drag_icon);
	damage_cursor(&old_cursor, true);
	damage_cursor(&new_cursor, true);
	damage_magnifier(old_x, old_y);
	damage_magnifier(pointer->x, pointer->y);
}

// Replaces the cursor picked by clients, which is hidden without a surface
// and a shape.
void
pointer_set_client_cursor(struct wl_resource *resource,
	struct surface *surface, enum wp_cursor_shape_device_v1_shape shape,
	int32_t hotspot_x, int32_t hotspot_y)
{
	struct pointer *pointer = &core.seat.pointer;
	pointer_damage_cursor();
	if (pointer->cursor.surface && pointer->cursor.surface != surface) {
		surface_set_mapped(pointer->cursor.surface, false);
	}
	pointer->cursor.resource = resource;
	pointer->cursor.surface = surface;
	pointer->cursor.shape = shape;
	pointer->cursor.hotspot_x = hotspot_x;
	pointer->cursor.hotspot_y = hotspot_y;
	if (surface) {
		surface_set_mapped(surface, surface->current.buffer);
	}
	pointer_damage_cursor();
}

static void
pointer_set_cursor_req(struct wl_client *client, struct wl_resource *resource,
	uint32_t serial, struct wl_resource *surface, int32_t hotspot_x,
	int32_t hotspot_y)
{
	struct pointer_resource *pointer = wl_resource_get_user_data(resource);
	assert(pointer);
	struct surface *cursor = surface ? wl_resource_get_user_data(surface) : NULL;
	bool focused = resource_matches_surface(resource, core.seat.pointer.focus);
	bool current = cursor && cursor == core.seat.pointer.cursor.surface;
	if ((!focused && !current) || pointer->enter_serial != serial) {
		return;
	}

	if (surface && (!cursor || wl_resource_get_client(surface) != client)) {
		return;
	}
	if (!cursor) {
		pointer_set_client_cursor(resource, NULL, 0, 0, 0);
		return;
	}
	if (cursor->role != SURFACE_ROLE_NONE && cursor->role != SURFACE_ROLE_CURSOR) {
		wl_resource_post_error(resource, WL_POINTER_ERROR_ROLE,
			"wl_surface already has another role");
		return;
	}
	cursor->role = SURFACE_ROLE_CURSOR;
	pointer_set_client_cursor(resource, cursor, 0,
		hotspot_x + cursor->offset_x, hotspot_y + cursor->offset_y);
}

static const struct wl_pointer_interface pointer_impl = {
	.set_cursor = pointer_set_cursor_req,
	.release = resource_destroy_req,
};

static void
pointer_resource_destroy(struct wl_resource *resource)
{
	struct pointer_resource *pointer = wl_resource_get_user_data(resource);
	if (core.seat.pointer.cursor.resource == resource) {
		pointer_set_client_cursor(NULL, NULL, 0, 0, 0);
	}
	struct wl_resource *device, *tmp;
	wl_resource_for_each_safe(device, tmp, &pointer->cursor_shape_devices) {
		wl_resource_set_user_data(device, NULL);
		wl_list_remove(wl_resource_get_link(device));
		wl_list_init(wl_resource_get_link(device));
	}
	wl_list_remove(&pointer->link);
	free(pointer);
}

bool
cursor_shape_validate(struct wl_resource *resource, uint32_t shape)
{
	if (!wp_cursor_shape_device_v1_shape_is_valid(shape,
			wl_resource_get_version(resource))) {
		wl_resource_post_error(resource,
			WP_CURSOR_SHAPE_DEVICE_V1_ERROR_INVALID_SHAPE,
			"invalid cursor shape %u", shape);
		return false;
	}
	return true;
}

// Like wl_pointer.set_cursor, but the cursor is drawn from the cursor theme.
static void
cursor_shape_device_set_shape_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t serial, uint32_t shape)
{
	if (!cursor_shape_validate(resource, shape)) {
		return;
	}
	struct pointer_resource *pointer = wl_resource_get_user_data(resource);
	if (!pointer || pointer->enter_serial != serial
			|| !resource_matches_surface(pointer->resource,
				core.seat.pointer.focus)) {
		return;
	}
	pointer_set_client_cursor(pointer->resource, NULL, shape, 0, 0);
}

static const struct wp_cursor_shape_device_v1_interface cursor_shape_device_impl = {
	.destroy = resource_destroy_req,
	.set_shape = cursor_shape_device_set_shape_req,
};

static void
cursor_shape_manager_get_pointer_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id, struct wl_resource *pointer_resource)
{
	struct pointer_resource *pointer = wl_resource_get_user_data(pointer_resource);
	struct wl_resource *device = wl_resource_create(client,
		&wp_cursor_shape_device_v1_interface, wl_resource_get_version(resource), id);
	assert(device);
	wl_resource_set_implementation(device, &cursor_shape_device_impl,
		pointer, resource_unlink);
	list_append(&pointer->cursor_shape_devices, wl_resource_get_link(device));
}

static void
cursor_shape_manager_get_tablet_tool_v2_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id, struct wl_resource *tablet_tool)
{
	tablet_tool_get_cursor_shape_device(client,
		wl_resource_get_version(resource), id, tablet_tool);
}

static const struct wp_cursor_shape_manager_v1_interface
		cursor_shape_manager_impl = {
	.destroy = resource_destroy_req,
	.get_pointer = cursor_shape_manager_get_pointer_req,
	.get_tablet_tool_v2 = cursor_shape_manager_get_tablet_tool_v2_req,
};

void
wayland_bind_cursor_shape_manager(struct wl_client *client, void *data,
	uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&wp_cursor_shape_manager_v1_interface,
		MIN(version, CURSOR_SHAPE_MANAGER_VERSION), id);
	assert(resource);
	wl_resource_set_implementation(resource, &cursor_shape_manager_impl, NULL, NULL);
}

static void
send_pointer_enter(struct pointer_resource *pointer, uint32_t serial)
{
	assert(core.seat.pointer.focus);
	pointer->enter_serial = serial;
	wl_pointer_send_enter(pointer->resource, serial,
		core.seat.pointer.focus->resource,
		wl_fixed_from_double(core.seat.pointer.surface_x),
		wl_fixed_from_double(core.seat.pointer.surface_y));
}

static void
send_pointer_frame(struct wl_resource *resource)
{
	if (wl_resource_get_version(resource) >= WL_POINTER_FRAME_SINCE_VERSION) {
		wl_pointer_send_frame(resource);
	}
}

void
pointer_create_resource(struct wl_client *client, struct wl_resource *seat,
	uint32_t id)
{
	struct pointer_resource *pointer = calloc(1, sizeof(*pointer));
	assert(pointer);
	pointer->resource = wl_resource_create(client, &wl_pointer_interface,
		wl_resource_get_version(seat), id);
	assert(pointer->resource);
	wl_resource_set_implementation(pointer->resource, &pointer_impl, pointer,
		pointer_resource_destroy);
	list_append(&core.seat.pointer.resources, &pointer->link);
	wl_list_init(&pointer->cursor_shape_devices);

	if (resource_matches_surface(pointer->resource, core.seat.pointer.focus)) {
		send_pointer_enter(pointer, ++core.serial);
		send_pointer_frame(pointer->resource);
	}
}

// Sends leave to the old focus and enter to the new one, at the position of the
// pointer on it.
static void
move_focus(struct surface *surface)
{
	pointer_damage_cursor();
	struct surface *old = core.seat.pointer.focus;
	uint32_t serial = ++core.serial;
	if (old) {
		struct pointer_resource *pointer;
		wl_list_for_each(pointer, &core.seat.pointer.resources, link) {
			if (resource_matches_surface(pointer->resource, old)) {
				wl_pointer_send_leave(pointer->resource, serial, old->resource);
				send_pointer_frame(pointer->resource);
			}
		}
	}

	core.seat.pointer.focus = surface;
	if (!surface) {
		pointer_damage_cursor();
		return;
	}

	struct pointer_resource *pointer;
	wl_list_for_each(pointer, &core.seat.pointer.resources, link) {
		if (resource_matches_surface(pointer->resource, surface)) {
			send_pointer_enter(pointer, serial);
			send_pointer_frame(pointer->resource);
		}
	}
	pointer_damage_cursor();
}

// The constraint of the focus applies only at some positions on it, so it is
// updated along with them.
void
pointer_focus(struct surface *surface, double sx, double sy)
{
	core.seat.pointer.surface_x = sx;
	core.seat.pointer.surface_y = sy;
	if (core.seat.pointer.focus != surface) {
		move_focus(surface);
	}
	pointer_constraint_update();
}

static void
send_pointer_frames(void)
{
	struct pointer_resource *pointer;
	wl_list_for_each(pointer, &core.seat.pointer.resources, link) {
		if (resource_matches_surface(pointer->resource, core.seat.pointer.focus)) {
			send_pointer_frame(pointer->resource);
		}
	}
}

static void
send_pointer_motion(uint32_t time)
{
	if (!core.seat.pointer.focus) {
		return;
	}
	struct pointer_resource *pointer;
	wl_list_for_each(pointer, &core.seat.pointer.resources, link) {
		if (resource_matches_surface(pointer->resource, core.seat.pointer.focus)) {
			wl_pointer_send_motion(pointer->resource, time,
				wl_fixed_from_double(core.seat.pointer.surface_x),
				wl_fixed_from_double(core.seat.pointer.surface_y));
			send_pointer_frame(pointer->resource);
		}
	}
}

static uint32_t
send_pointer_button(uint32_t time, uint32_t button, uint32_t state)
{
	if (!core.seat.pointer.focus) {
		return 0;
	}
	uint32_t serial = 0;
	struct pointer_resource *pointer;
	wl_list_for_each(pointer, &core.seat.pointer.resources, link) {
		if (!resource_matches_surface(pointer->resource, core.seat.pointer.focus)) {
			continue;
		}
		if (!serial) {
			serial = ++core.serial;
		}
		wl_pointer_send_button(pointer->resource, serial, time, button, state);
		send_pointer_frame(pointer->resource);
	}
	if (serial) {
		core.seat.input_serial = serial;
		core.seat.input_client =
			wl_resource_get_client(core.seat.pointer.focus->resource);
	}
	return serial;
}

static void
send_pointer_axis(uint32_t time, enum wl_pointer_axis_source source,
	const struct pointer_axis_event axes[], size_t nr_axes)
{
	if (!core.seat.pointer.focus) {
		return;
	}

	struct pointer_resource *pointer;
	wl_list_for_each(pointer, &core.seat.pointer.resources, link) {
		if (!resource_matches_surface(pointer->resource, core.seat.pointer.focus)) {
			continue;
		}
		uint32_t version = wl_resource_get_version(pointer->resource);
		if (version >= WL_POINTER_AXIS_SOURCE_SINCE_VERSION) {
			wl_pointer_send_axis_source(pointer->resource, source);
		}
		for (size_t i = 0; i < nr_axes; i++) {
			const struct pointer_axis_event *axis = &axes[i];
			uint32_t orientation = axis->axis;
			if (axis->value == 0.0) {
				if (version >= WL_POINTER_AXIS_STOP_SINCE_VERSION) {
					wl_pointer_send_axis_stop(pointer->resource, time, orientation);
				} else {
					wl_pointer_send_axis(pointer->resource, time, orientation, 0);
				}
				continue;
			}
			if (axis->value120 != 0
					&& version >= WL_POINTER_AXIS_VALUE120_SINCE_VERSION) {
				wl_pointer_send_axis_value120(pointer->resource, orientation,
					axis->value120);
			} else if (version >= WL_POINTER_AXIS_DISCRETE_SINCE_VERSION
					&& axis->value120 / 120 != 0) {
				wl_pointer_send_axis_discrete(pointer->resource, orientation,
					axis->value120 / 120);
			}
			wl_pointer_send_axis(pointer->resource, time, orientation,
				wl_fixed_from_double(axis->value));
		}
		send_pointer_frame(pointer->resource);
	}
}

// Moves the pointer to the closest point on the outputs, like
// wlr_output_layout_closest_point(). The right and bottom edges of an output
// are exclusive, so the point is kept 1/256 inside them, which is small enough
// for high-resolution mice and large enough to survive wl_fixed_t rounding.
// The pointer moves freely while no output is enabled.
static void
constrain_pointer(void)
{
	struct pointer *pointer = &core.seat.pointer;
	struct output *output = output_nearest(pointer->x, pointer->y);
	if (!output) {
		return;
	}
	pointer->x = clamp(pointer->x, output->box.x,
		output->box.x + output->box.width - 1 / 256.0);
	pointer->y = clamp(pointer->y, output->box.y,
		output->box.y + output->box.height - 1 / 256.0);
}

// Keeps the pointer on the outputs after they change, and draws the cursor
// for the output under it. A moved window snaps to the edges found by the
// next motion, since its output may be gone.
void
pointer_handle_output_change(void)
{
	struct pointer_grab *grab = &core.seat.pointer.grab;
	if (grab->type == POINTER_GRAB_MOVE) {
		grab->move.snap_output = NULL;
		grab->move.snap_edges = EDGE_NONE;
		wl_event_source_timer_update(grab->move.snap_preview_timer, 0);
		scene_node_set_enabled(&core.snap_preview.node, false);
	}
	pointer_warp(core.seat.pointer.x, core.seat.pointer.y);
}

// Clicking a window, an interactive layer surface or a lock surface focuses
// it.
static void
focus_root_for_surface(struct surface *surface)
{
	struct window *window = window_from_surface(surface);
	if (window) {
		window_focus(window);
		return;
	}
	struct surface *root = surface_root(surface);
	if (root->role == SURFACE_ROLE_LOCK_SURFACE
			|| (root->role == SURFACE_ROLE_LAYER_SURFACE
				&& root->layer_surface->current.keyboard_interactivity
					!= ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE)) {
		keyboard_focus(root);
	}
}

static struct pointer_button *
button_for_serial(struct surface *ancestor, uint32_t serial)
{
	for (size_t i = 0; i < core.seat.pointer.nr_pressed; i++) {
		struct pointer_button *pressed = &core.seat.pointer.pressed[i];
		if (pressed->serial == serial
				&& surface_contains(ancestor, pressed->surface)) {
			return pressed;
		}
	}
	return NULL;
}

static struct pointer_button *
button_pressed_on(struct surface *ancestor)
{
	for (size_t i = 0; i < core.seat.pointer.nr_pressed; i++) {
		struct pointer_button *pressed = &core.seat.pointer.pressed[i];
		if (surface_contains(ancestor, pressed->surface)) {
			return pressed;
		}
	}
	return NULL;
}

static struct pointer_button *
pressed_button(uint32_t button)
{
	for (size_t i = 0; i < core.seat.pointer.nr_pressed; i++) {
		if (core.seat.pointer.pressed[i].button == button) {
			return &core.seat.pointer.pressed[i];
		}
	}
	return NULL;
}

// Only presses delivered to a client are tracked, so that exactly their
// releases are delivered as well.
static void
forward_button_press(uint32_t time, uint32_t button)
{
	uint32_t serial = send_pointer_button(time, button,
		WL_POINTER_BUTTON_STATE_PRESSED);
	if (!serial) {
		return;
	}
	struct pointer_button *pressed = pressed_button(button);
	if (!pressed) {
		if (core.seat.pointer.nr_pressed == ARRAY_LEN(core.seat.pointer.pressed)) {
			return;
		}
		pressed = &core.seat.pointer.pressed[core.seat.pointer.nr_pressed++];
	}
	*pressed = (struct pointer_button){
		.button = button,
		.serial = serial,
		.surface = core.seat.pointer.focus,
	};
}

static void
forget_pressed_button(struct pointer_button *pressed)
{
	*pressed = core.seat.pointer.pressed[--core.seat.pointer.nr_pressed];
}

// While buttons delivered to a client are held, the pointer stays on the
// surface that received them (implicit grab). All of them went to the same
// client, since the grab keeps other clients from receiving presses.
struct surface *
pointer_implicit_grab_surface(void)
{
	struct pointer *pointer = &core.seat.pointer;
	return pointer->nr_pressed ? pointer->pressed[0].surface : NULL;
}

// Focuses a grabbing surface even if the pointer is outside of it.
static void
focus_grab_surface(struct surface *surface)
{
	double sx, sy;
	surface_from_layout(surface, core.seat.pointer.x, core.seat.pointer.y, &sx, &sy);
	pointer_focus(surface, sx, sy);
}

// Walks the enabled scene layers from the top. The windows are walked in
// core.windows, which is stacked like their scene layer. X11 drags skip
// override-redirect X11 windows, as their sources look past their drag icons.
struct input_target
input_target_at(double x, double y, bool skip_unmanaged)
{
	struct input_target target = {0};
	for (int layer = NR_SCENE_LAYERS - 1; layer >= 0; layer--) {
		if (!core.scene.layers[layer].node.enabled
				|| (layer == SCENE_LAYER_UNMANAGED && skip_unmanaged)) {
			continue;
		}
		if (layer == SCENE_LAYER_TOPLEVEL) {
			struct window *window;
			wl_list_for_each_reverse(window, &core.windows, link) {
				if (window_at(window, x, y, &target)) {
					return target;
				}
			}
			continue;
		}
		target.surface = scene_surface_at(&core.scene.layers[layer].node,
			x, y, &target.sx, &target.sy);
		if (target.surface) {
			return target;
		}
	}
	return target;
}

// The buttons of the decorations show whether the pointer is over them.
static void
set_hover(struct window *window, enum decoration_part part)
{
	struct pointer *pointer = &core.seat.pointer;
	struct window *old = pointer->hover.window;
	if (old == window && pointer->hover.part == part) {
		return;
	}
	pointer->hover.window = window;
	pointer->hover.part = part;
	if (old) {
		decoration_update_buttons(old);
	}
	if (window && window != old) {
		decoration_update_buttons(window);
	}
}

// An xdg_popup grab confines the pointer to the surfaces of its client, but
// lets it move between them even during an implicit grab, so that menus can be
// operated by press-drag-release. Decorations take no part in grabs.
static void
pick_pointer_focus(struct surface *grab)
{
	struct xdg_popup *popup = core.seat.pointer.grabbed_popup;
	struct input_target target = {0};
	if (!grab || popup) {
		target = input_target_at(core.seat.pointer.x, core.seat.pointer.y, false);
	}
	pointer_set_server_cursor_shape(pointer_cursor_shape_for_resize_edges(
		popup ? EDGE_NONE : target.edges));
	set_hover(target.window, target.part);

	struct surface *surface = target.surface;
	if (popup && surface && wl_resource_get_client(surface->resource)
			!= wl_resource_get_client(popup->resource)) {
		surface = NULL;
	}
	if (!surface && grab) {
		focus_grab_surface(grab);
	} else {
		pointer_focus(surface, target.sx, target.sy);
	}
}

// Focuses the surface under the pointer. Input events do so before they are
// delivered, and the event loop after each iteration, for the scene changes
// under a stationary pointer. While switching windows, the pointer is
// unfocused and hovers the items of the window switcher instead. It also stays
// unfocused while a tablet tool moves it for a surface.
void
pointer_update_focus(uint32_t time, bool send_motion)
{
	if (core.window_switcher.selected) {
		pointer_set_server_cursor_shape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
		set_hover(NULL, DECORATION_PART_NONE);
		pointer_focus(NULL, 0.0, 0.0);
		window_switcher_update_hover();
		return;
	}
	if (core.seat.pointer.tablet_tool) {
		return;
	}
	switch (core.seat.pointer.grab.type) {
	case POINTER_GRAB_NONE:
		pick_pointer_focus(pointer_implicit_grab_surface());
		if (core.xwayland.xwm) {
			xwm_x11_drag_update();
		}
		break;
	case POINTER_GRAB_MOVE:
	case POINTER_GRAB_RESIZE:
		// The pointer stays unfocused.
		break;
	case POINTER_GRAB_DECORATION_BUTTON: {
		// The pointer stays unfocused, and the button shows as pressed
		// only while the pointer is over it.
		struct input_target target = input_target_at(core.seat.pointer.x,
			core.seat.pointer.y, false);
		set_hover(target.window, target.part);
		break;
	}
	case POINTER_GRAB_DND: {
		// The pointer stays unfocused, and the drag enters the surface
		// under it instead.
		struct input_target target = input_target_at(core.seat.pointer.x,
			core.seat.pointer.y, false);
		drag_motion(&core.seat.pointer.grab.drag, target.surface,
			target.sx, target.sy, time, send_motion);
		break;
	}
	}
	if (send_motion) {
		send_pointer_motion(time);
	}
}

static int
show_snap_preview(void *data)
{
	scene_rect_set_color(&core.snap_preview, config.snapping.preview_color);
	scene_node_set_enabled(&core.snap_preview.node, true);
	return 0;
}

// The geometry of the window resized to the pointer, within its size limits.
static struct box
resize_geometry(const struct pointer_grab *grab)
{
	struct window *window = grab->window;
	struct box start = grab->resize.start_geometry;
	int dx = (int)round(core.seat.pointer.x - grab->resize.start_x);
	int dy = (int)round(core.seat.pointer.y - grab->resize.start_y);
	int width = start.width;
	int height = start.height;
	uint32_t edges = grab->resize.edges;
	if (edges & EDGE_LEFT) {
		width = MAX(1, width - dx);
	} else if (edges & EDGE_RIGHT) {
		width = MAX(1, width + dx);
	}
	if (edges & EDGE_TOP) {
		height = MAX(1, height - dy);
	} else if (edges & EDGE_BOTTOM) {
		height = MAX(1, height + dy);
	}
	// Maximum sizes of 0, or below the minimum sizes, are ignored.
	width = MAX(width, MAX(1, window->min_width));
	height = MAX(height, MAX(1, window->min_height));
	if (window->max_width >= MAX(1, window->min_width)) {
		width = MIN(width, window->max_width);
	}
	if (window->max_height >= MAX(1, window->min_height)) {
		height = MIN(height, window->max_height);
	}
	struct box geometry = {
		.x = start.x,
		.y = start.y,
		.width = width,
		.height = height,
	};
	if (edges & EDGE_LEFT) {
		geometry.x += start.width - width;
	}
	if (edges & EDGE_TOP) {
		geometry.y += start.height - height;
	}
	return geometry;
}

// The window is configured at most once per refresh of its output, since
// pointer motion can come hundreds of times per second. Releasing the button
// configures the size at the pointer in the end. Unthrottled while no output
// is enabled.
static void
resize_by_motion(uint32_t time)
{
	struct pointer_grab *grab = &core.seat.pointer.grab;
	struct output *output = grab->window->output;
	if (output && time - grab->resize.last_configure_time
			< 1000000 / (uint32_t)mode_refresh(output->mode)) {
		return;
	}
	struct box geometry = resize_geometry(grab);
	if (geometry.width == grab->resize.configured_width
			&& geometry.height == grab->resize.configured_height) {
		return;
	}
	window_resize(grab->window, geometry, grab->resize.edges, true);
	grab->resize.configured_width = geometry.width;
	grab->resize.configured_height = geometry.height;
	grab->resize.last_configure_time = time;
}

// The pointer leaves the surface during moves and resizes, even the one which
// requested them, and the cursor of the compositor shows the grab, a move only
// once the window moves. A window snapped by the move returns to where it
// floated before.
static void
start_move(struct window *window, uint32_t button)
{
	assert(core.seat.pointer.grab.type == POINTER_GRAB_NONE);
	assert(!box_empty(&window->current.geometry));
	window_save_restore_geometry(window);
	pointer_focus(NULL, 0.0, 0.0);
	pointer_set_server_cursor_shape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
	core.seat.pointer.grab = (struct pointer_grab){
		.type = POINTER_GRAB_MOVE,
		.button = button,
		.window = window,
		.move = {
			.start_x = core.seat.pointer.x,
			.start_y = core.seat.pointer.y,
			.start_geometry = window->current.geometry,
			.snap_preview_timer = wl_event_loop_add_timer(core.wl_loop,
				show_snap_preview, NULL),
		},
	};
	assert(core.seat.pointer.grab.move.snap_preview_timer);
}

static void
start_resize(struct window *window, uint32_t button, uint32_t edges)
{
	assert(core.seat.pointer.grab.type == POINTER_GRAB_NONE);
	struct box geometry = window->current.geometry;
	assert(!box_empty(&geometry));
	pointer_focus(NULL, 0.0, 0.0);
	pointer_set_server_cursor_shape(pointer_cursor_shape_for_resize_edges(edges));
	core.seat.pointer.grab = (struct pointer_grab){
		.type = POINTER_GRAB_RESIZE,
		.button = button,
		.window = window,
		.resize = {
			.edges = edges,
			.start_x = core.seat.pointer.x,
			.start_y = core.seat.pointer.y,
			.start_geometry = geometry,
			.configured_width = geometry.width,
			.configured_height = geometry.height,
		},
	};
}

// The button is clicked when released on it.
static void
click_decoration_button(struct window *window, enum decoration_part part)
{
	struct input_target target = input_target_at(core.seat.pointer.x,
		core.seat.pointer.y, false);
	if (target.window != window || target.part != part) {
		return;
	}
	switch (part) {
	case DECORATION_PART_MINIMIZE:
		window_set_minimized(window, true);
		return;
	case DECORATION_PART_STICKY:
		window_set_sticky(window, window->workspace != WORKSPACE_STICKY);
		return;
	case DECORATION_PART_MAXIMIZE:
		window_set_maximized(window, !window->pending.maximized);
		return;
	case DECORATION_PART_CLOSE:
		window_close(window);
		return;
	case DECORATION_PART_NONE:
	case DECORATION_PART_TITLEBAR:
	case DECORATION_PART_BORDER:
		break;
	}
	assert(!"decoration button grab without a button");
}

// Releasing the button of the grab completes it, while cancelling it ends it
// without its action.
static void
end_grab(bool cancelled)
{
	struct pointer_grab grab = core.seat.pointer.grab;
	// The drag icon goes away.
	pointer_damage_cursor();
	core.seat.pointer.grab = (struct pointer_grab){0};
	switch (grab.type) {
	case POINTER_GRAB_NONE:
		assert(!"ending no grab");
		return;
	case POINTER_GRAB_MOVE:
		wl_event_source_remove(grab.move.snap_preview_timer);
		scene_node_set_enabled(&core.snap_preview.node, false);
		if (!cancelled && grab.move.snap_edges) {
			window_snap(grab.window, grab.move.snap_output,
				grab.move.snap_edges);
		}
		return;
	case POINTER_GRAB_RESIZE: {
		struct box geometry = cancelled
			? grab.window->pending.geometry : resize_geometry(&grab);
		window_resize(grab.window, geometry, grab.resize.edges, false);
		return;
	}
	case POINTER_GRAB_DECORATION_BUTTON:
		decoration_update_buttons(grab.window);
		if (!cancelled) {
			click_decoration_button(grab.window, grab.decoration_button);
		}
		return;
	case POINTER_GRAB_DND:
		if (cancelled) {
			drag_cancel(&grab.drag);
		} else {
			drag_drop(&grab.drag);
		}
		if (grab.drag.icon) {
			surface_set_mapped(grab.drag.icon, false);
		}
		return;
	}
	assert(!"unknown pointer grab type");
}

void
pointer_cancel_grab(void)
{
	end_grab(true);
}

// Moves the pointer without input events, e.g. to a touch point taken for a
// grab.
void
pointer_warp(double x, double y)
{
	double old_x = core.seat.pointer.x;
	double old_y = core.seat.pointer.y;
	core.seat.pointer.x = x;
	core.seat.pointer.y = y;
	constrain_pointer();
	damage_cursor_move(old_x, old_y);
}

// A tablet tool moves the pointer while it gives its events to a surface, or
// stops to (NULL). The pointer leaves the surfaces meanwhile, so that their
// clients get the tool alone, and the client of the surface picks the cursor.
void
pointer_set_tablet_tool(struct tablet_tool *tool)
{
	if (tool) {
		pointer_focus(NULL, 0.0, 0.0);
		set_hover(NULL, DECORATION_PART_NONE);
	}
	pointer_damage_cursor();
	core.seat.pointer.tablet_tool = tool;
	pointer_damage_cursor();
}

// The surface that a touch point or a tablet tool at the point gives its own
// events to if its client takes them, rather than moving the pointer. The
// pointer takes them over the decorations, during the grabs of the compositor,
// and while switching windows. An xdg_popup grab confines them to the surfaces
// of its client, like the pointer.
struct surface *
direct_input_surface_at(double x, double y)
{
	struct pointer *pointer = &core.seat.pointer;
	if (core.window_switcher.selected || pointer->grab.type != POINTER_GRAB_NONE) {
		return NULL;
	}
	struct surface *surface = input_target_at(x, y, false).surface;
	struct xdg_popup *popup = pointer->grabbed_popup;
	if (surface && popup && wl_resource_get_client(surface->resource)
			!= wl_resource_get_client(popup->resource)) {
		return NULL;
	}
	return surface;
}

// Pressing a surface with a touch point or a tablet tool closes the popups of
// a grab above it, and focuses its window like a click. Returns the serial of
// the press, which the caller sends.
uint32_t
direct_input_press(struct surface *surface)
{
	xdg_popup_handle_press(surface);
	if (!core.seat.pointer.grabbed_popup) {
		focus_root_for_surface(surface);
	}
	uint32_t serial = ++core.serial;
	core.seat.input_serial = serial;
	core.seat.input_client = wl_resource_get_client(surface->resource);
	return serial;
}

// Clients without serials, like X11 ones, start grabs by any press on them.
uint32_t
input_press_serial(struct surface *ancestor)
{
	struct pointer_button *pressed = button_pressed_on(ancestor);
	if (pressed) {
		return pressed->serial;
	}
	uint32_t serial = touch_press_serial(ancestor);
	return serial ? serial : tablet_press_serial(ancestor);
}

// Takes the press of the serial on the surface, which a client starts a grab
// by: a button of the pointer, or a touch point or the tip of a tablet tool,
// which then moves the pointer and holds its left button until lifted. Returns
// the button whose release ends the grab, or 0 if there is no such press.
static uint32_t
take_press(struct surface *ancestor, uint32_t serial)
{
	struct pointer_button *pressed = button_for_serial(ancestor, serial);
	if (pressed) {
		return pressed->button;
	}
	if (touch_take_point(ancestor, serial) || tablet_take_tool(ancestor, serial)) {
		return BTN_LEFT;
	}
	return 0;
}

void
pointer_request_move(struct window *window, uint32_t serial)
{
	if (core.seat.pointer.grab.type != POINTER_GRAB_NONE) {
		return;
	}
	uint32_t button = take_press(window->surface, serial);
	if (button) {
		start_move(window, button);
	}
}

void
pointer_request_resize(struct window *window, uint32_t serial, uint32_t edges)
{
	uint32_t allowed_edges = EDGE_TOP | EDGE_BOTTOM | EDGE_LEFT | EDGE_RIGHT;
	if (!edges || (edges & ~allowed_edges)
			|| ((edges & EDGE_TOP) && (edges & EDGE_BOTTOM))
			|| ((edges & EDGE_LEFT) && (edges & EDGE_RIGHT))
			|| core.seat.pointer.grab.type != POINTER_GRAB_NONE) {
		return;
	}
	uint32_t button = take_press(window->surface, serial);
	if (button) {
		start_resize(window, button, edges);
	}
}

// The drag is started by a press on the origin, and ends when it is released.
// As during moves and resizes, the pointer leaves the origin, so that clients
// get the drag events alone and no pointer motion, buttons, or axes, which
// would make a browser attach and detach dragged tabs repeatedly.
bool
pointer_start_drag(struct surface *origin, uint32_t serial,
	struct surface *icon, struct data_source *source)
{
	if (core.seat.pointer.grab.type != POINTER_GRAB_NONE) {
		return false;
	}
	uint32_t button = take_press(origin, serial);
	if (!button) {
		return false;
	}
	pointer_focus(NULL, 0.0, 0.0);
	pointer_set_server_cursor_shape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
	core.seat.pointer.grab = (struct pointer_grab){
		.type = POINTER_GRAB_DND,
		.button = button,
		.drag = {
			.source = source,
			.client = wl_resource_get_client(origin->resource),
			.icon = icon,
		},
	};
	if (icon) {
		assert(icon->role == SURFACE_ROLE_NONE
			|| icon->role == SURFACE_ROLE_DND_ICON);
		icon->role = SURFACE_ROLE_DND_ICON;
		core.seat.pointer.grab.drag.icon_hotspot_x = icon->offset_x;
		core.seat.pointer.grab.drag.icon_hotspot_y = icon->offset_y;
		surface_set_mapped(icon, icon->current.buffer);
	}
	pointer_damage_cursor();
	return true;
}

// The edges of the box that the point is within the range of, at most one of
// each axis.
static uint32_t
edges_in_range(const struct box *box, double x, double y, int range)
{
	uint32_t edges = EDGE_NONE;
	if (x < box->x + range) {
		edges |= EDGE_LEFT;
	} else if (x >= box->x + box->width - range) {
		edges |= EDGE_RIGHT;
	}
	if (y < box->y + range) {
		edges |= EDGE_TOP;
	} else if (y >= box->y + box->height - range) {
		edges |= EDGE_BOTTOM;
	}
	return edges;
}

// The edges of the usable area of the output that the pointer is at: an edge,
// or a corner where the pointer is at an edge and within the corner range of
// the other edge. The bottom edge alone is not snapped to. Panels outside of
// the usable area count as its edges.
static uint32_t
snap_edges_at_pointer(struct output *output)
{
	double x = core.seat.pointer.x;
	double y = core.seat.pointer.y;
	struct box *area = &output->usable_area;
	uint32_t edges = edges_in_range(area, x, y, config.snapping.edge_range);
	if (config.snapping.edge_range == 0 || edges == EDGE_NONE) {
		return EDGE_NONE;
	}
	edges |= edges_in_range(area, x, y, config.snapping.corner_range);
	return edges == EDGE_BOTTOM ? EDGE_NONE : edges;
}

// Floating windows snap to the edges the pointer is at, which the preview
// shows below the window. The preview appears once the pointer stays at the
// edges for the delay, so that it does not flash while the pointer passes
// them, e.g. between outputs. Once shown, it follows the edges right away.
static void
update_snap_edges(void)
{
	struct pointer_grab *grab = &core.seat.pointer.grab;
	struct window *window = grab->window;
	struct output *output = output_nearest(core.seat.pointer.x, core.seat.pointer.y);
	uint32_t edges = output && window_floating(window)
		? snap_edges_at_pointer(output) : EDGE_NONE;
	if (output == grab->move.snap_output && edges == grab->move.snap_edges) {
		return;
	}
	grab->move.snap_output = output;
	grab->move.snap_edges = edges;
	struct scene_rect *preview = &core.snap_preview;
	if (edges == EDGE_NONE) {
		wl_event_source_timer_update(grab->move.snap_preview_timer, 0);
		scene_node_set_enabled(&preview->node, false);
		return;
	}
	struct box box = window_snap_box(output, edges);
	scene_node_place_below(&preview->node, &window->tree.node);
	scene_node_set_position(&preview->node, box.x, box.y);
	scene_rect_set_size(preview, box.width, box.height);
	if (!preview->node.enabled) {
		wl_event_source_timer_update(grab->move.snap_preview_timer,
			SNAP_PREVIEW_DELAY_MS);
	}
}

// The offset of the grabbed point from the content of a window resized from
// the size along an axis. It keeps its proportion of the content, but stays
// on the decoration before the content, e.g. the titlebar.
static double
scaled_grab_offset(double offset, int size, int new_size)
{
	return offset < 0 ? offset : offset * new_size / size;
}

// The geometry of the size at which the grabbed point of the window is under
// the pointer, or an empty box if the window is not moved by the pointer. The
// size may differ from the one at the start, as for a restored window, or be
// unknown (0x0) until the client picks it.
struct box
pointer_move_geometry(struct window *window, int width, int height)
{
	struct pointer_grab *grab = &core.seat.pointer.grab;
	if (grab->type != POINTER_GRAB_MOVE || grab->window != window) {
		return (struct box){0};
	}
	struct box *start = &grab->move.start_geometry;
	return (struct box){
		.x = (int)round(core.seat.pointer.x
			- scaled_grab_offset(grab->move.start_x - start->x,
				start->width, width)),
		.y = (int)round(core.seat.pointer.y
			- scaled_grab_offset(grab->move.start_y - start->y,
				start->height, height)),
		.width = width,
		.height = height,
	};
}

// A snapped or maximized window stays until dragged past the threshold, and is
// then restored under the grabbed point. Fullscreen windows stay. The cursor
// shows the grab once the window moves.
static void
move_by_motion(void)
{
	struct pointer_grab *grab = &core.seat.pointer.grab;
	struct window *window = grab->window;
	if (window->pending.maximized || window->pending.tiled) {
		double dx = core.seat.pointer.x - grab->move.start_x;
		double dy = core.seat.pointer.y - grab->move.start_y;
		if (hypot(dx, dy) < config.snapping.unsnap_threshold) {
			return;
		}
		window_unsnap(window);
	}
	if (window_floating(window)) {
		pointer_set_server_cursor_shape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING);
		struct box geometry = pointer_move_geometry(window,
			window->pending.geometry.width, window->pending.geometry.height);
		window_move(window, geometry.x, geometry.y);
	}
	update_snap_edges();
}

static void
handle_grab_motion(uint32_t time)
{
	struct pointer_grab *grab = &core.seat.pointer.grab;
	switch (grab->type) {
	case POINTER_GRAB_NONE:
	case POINTER_GRAB_DECORATION_BUTTON:
	case POINTER_GRAB_DND:
		return;
	case POINTER_GRAB_MOVE:
		move_by_motion();
		return;
	case POINTER_GRAB_RESIZE:
		resize_by_motion(time);
		return;
	}
	assert(!"unknown pointer grab type");
}

// With focus-follows-mouse, moving the pointer into another window gives it
// or its modal dialog the keyboard focus without raising it. Moving within a
// window, including its decoration, keeps the focus where it is, e.g. on a
// dialog over the window.
// Windows coming under the pointer by changes of the scene are not focused.
static void
focus_entered_window(struct window *old)
{
	struct window *window = core.seat.pointer.hover.window;
	if (!config.windows.focus_follows_mouse || !window || window == old
			|| core.seat.pointer.grab.type != POINTER_GRAB_NONE
			|| core.seat.pointer.grabbed_popup) {
		return;
	}
	keyboard_focus(window->surface);
}

// To a point in the layout. Moving the pointer takes it back from a tablet
// tool.
void
pointer_handle_motion_absolute(double x, double y, uint32_t time)
{
	struct pointer *pointer = &core.seat.pointer;
	if (pointer->tablet_tool) {
		tablet_tool_leave(pointer->tablet_tool, time);
	}
	double old_x = pointer->x;
	double old_y = pointer->y;
	pointer->x = x;
	pointer->y = y;
	constrain_pointer();
	if (pointer->x != old_x || pointer->y != old_y) {
		damage_cursor_move(old_x, old_y);
	}
	handle_grab_motion(time);
	struct window *old_hover = pointer->hover.window;
	pointer_update_focus(time, true);
	focus_entered_window(old_hover);
}

// By mice and touchpads, which the constraint of the focus locks or confines.
// The relative motion ends with the wl_pointer.frame of the motion, or of its
// own while the pointer is locked.
void
pointer_handle_motion(double dx, double dy, double dx_unaccel,
	double dy_unaccel, uint64_t time_usec)
{
	struct pointer *pointer = &core.seat.pointer;
	relative_pointers_send_motion(time_usec, dx, dy, dx_unaccel, dy_unaccel);
	struct pointer_constraint *constraint = pointer->constraint;
	if (constraint && constraint->type == POINTER_CONSTRAINT_LOCK) {
		send_pointer_frames();
		return;
	}
	double x = pointer->x + dx;
	double y = pointer->y + dy;
	if (constraint) {
		pointer_confine(&x, &y);
	}
	pointer_handle_motion_absolute(x, y, (uint32_t)(time_usec / 1000));
}

// A press on the titlebar of the window soon after the last one makes a
// double-click, and the press after it starts over.
static bool
double_click_titlebar(struct window *window, uint32_t time)
{
	struct pointer *pointer = &core.seat.pointer;
	bool double_click = pointer->titlebar_press.window == window
		&& time - pointer->titlebar_press.time < DOUBLE_CLICK_TIME_MS;
	pointer->titlebar_press.window = double_click ? NULL : window;
	pointer->titlebar_press.time = time;
	return double_click;
}

// Pressing the left button on a decoration starts a grab, except that
// double-clicking the titlebar toggles maximization.
static void
press_decoration(uint32_t time, uint32_t button)
{
	struct input_target target = input_target_at(core.seat.pointer.x,
		core.seat.pointer.y, false);
	if (target.part == DECORATION_PART_NONE) {
		return;
	}
	struct window *window = target.window;
	window_focus(window);
	if (button != BTN_LEFT) {
		return;
	}
	switch (target.part) {
	case DECORATION_PART_TITLEBAR:
		if (double_click_titlebar(window, time)) {
			window_set_maximized(window, !window->pending.maximized);
		} else {
			start_move(window, button);
		}
		return;
	case DECORATION_PART_BORDER:
		start_resize(window, button, target.edges);
		return;
	case DECORATION_PART_CLOSE:
	case DECORATION_PART_MAXIMIZE:
	case DECORATION_PART_MINIMIZE:
	case DECORATION_PART_STICKY:
		core.seat.pointer.grab = (struct pointer_grab){
			.type = POINTER_GRAB_DECORATION_BUTTON,
			.button = button,
			.window = window,
			.decoration_button = target.part,
		};
		decoration_update_buttons(window);
		return;
	case DECORATION_PART_NONE:
		break;
	}
	assert(!"unknown decoration part");
}

static const struct mouse_binding *
find_mouse_binding(uint32_t button)
{
	uint32_t modifiers = keyboard_pressed_modifiers();
	for (size_t i = 0; i < config.nr_mouse_bindings; i++) {
		const struct mouse_binding *binding = &config.mouse_bindings[i];
		if (binding->button == button && binding->modifiers == modifiers) {
			return binding;
		}
	}
	return NULL;
}

// The corner of the window nearest to the pointer
static uint32_t
nearest_corner(struct window *window)
{
	struct box *geometry = &window->current.geometry;
	double center_x = geometry->x + geometry->width / 2.0;
	double center_y = geometry->y + geometry->height / 2.0;
	return (core.seat.pointer.x < center_x ? EDGE_LEFT : EDGE_RIGHT)
		| (core.seat.pointer.y < center_y ? EDGE_TOP : EDGE_BOTTOM);
}

// Pressing the button of a mouse binding over a window focuses it and starts
// moving or resizing it, instead of being delivered. Resizing grabs the
// nearest corner. Fullscreen windows are neither moved nor resized, and
// maximized windows are not resized.
static bool
press_mouse_binding(uint32_t button)
{
	const struct mouse_binding *binding = find_mouse_binding(button);
	if (!binding) {
		return false;
	}
	struct window *window = input_target_at(core.seat.pointer.x,
		core.seat.pointer.y, false).window;
	if (!window || window->pending.fullscreen
			|| (binding->action == MOUSE_ACTION_RESIZE
				&& window->pending.maximized)) {
		return false;
	}
	window_focus(window);
	switch (binding->action) {
	case MOUSE_ACTION_MOVE:
		start_move(window, button);
		return true;
	case MOUSE_ACTION_RESIZE:
		start_resize(window, button, nearest_corner(window));
		return true;
	}
	assert(!"unknown mouse action");
}

// During a grab, the presses of other buttons go to the focus. Mouse bindings
// do not apply over the popups of a grab. Buttons go to the window switcher
// while switching windows.
static void
handle_button_press(uint32_t time, uint32_t button)
{
	if (core.window_switcher.selected) {
		window_switcher_handle_button(button, true);
		return;
	}
	switch (core.seat.pointer.grab.type) {
	case POINTER_GRAB_NONE:
		break;
	case POINTER_GRAB_MOVE:
	case POINTER_GRAB_RESIZE:
	case POINTER_GRAB_DECORATION_BUTTON:
	case POINTER_GRAB_DND:
		forward_button_press(time, button);
		return;
	}
	if (xdg_popup_handle_press(core.seat.pointer.focus)) {
		return;
	}
	if (!core.seat.pointer.grabbed_popup && press_mouse_binding(button)) {
		return;
	}
	if (core.seat.pointer.focus) {
		if (!core.seat.pointer.grabbed_popup) {
			focus_root_for_surface(core.seat.pointer.focus);
		}
		forward_button_press(time, button);
		return;
	}
	press_decoration(time, button);
}

// The release is sent before the grab ends, so it reaches the client that
// received the press rather than whatever is under the pointer now.
static void
handle_button_release(uint32_t time, uint32_t button)
{
	struct pointer_button *pressed = pressed_button(button);
	if (pressed) {
		send_pointer_button(time, button, WL_POINTER_BUTTON_STATE_RELEASED);
		forget_pressed_button(pressed);
	}
	if (core.seat.pointer.grab.type != POINTER_GRAB_NONE
			&& button == core.seat.pointer.grab.button) {
		end_grab(false);
	}
	if (core.window_switcher.selected) {
		window_switcher_handle_button(button, false);
	}
}

void
pointer_handle_button(uint32_t time, uint32_t button, bool pressed)
{
	pointer_update_focus(time, false);
	if (pressed) {
		handle_button_press(time, button);
	} else {
		handle_button_release(time, button);
	}
}

void
pointer_handle_scroll(uint32_t time, enum wl_pointer_axis_source source,
	const struct pointer_axis_event axes[], size_t nr_axes)
{
	pointer_update_focus(time, false);
	send_pointer_axis(time, source, axes, nr_axes);
}

void
pointer_init(void)
{
	wl_list_init(&core.seat.pointer.resources);
	for (int type = 0; type < NR_GESTURE_TYPES; type++) {
		wl_list_init(&core.seat.pointer.gesture_resources[type]);
	}
	wl_list_init(&core.seat.pointer.relative_pointers);
	core.seat.pointer.server_cursor_shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
	struct output *output = output_nearest(0, 0);
	if (output) {
		core.seat.pointer.x = output->box.x + output->box.width / 2.0;
		core.seat.pointer.y = output->box.y + output->box.height / 2.0;
	}
}

// Cursors and drag icons are shown while they have a buffer and are in use.
void
pointer_surface_commit(struct surface *surface)
{
	bool used = surface == core.seat.pointer.cursor.surface
		|| surface == drag_icon();
	surface_set_mapped(surface, used && surface->current.buffer);
}

// Surfaces are always unmapped before they are destroyed, so this is where the
// pointer lets go of them. Releases of the forgotten buttons are not delivered
// anywhere. The pointer focus and the drag move to what is under the pointer at
// the end of the event loop iteration, once the scene has settled.
void
pointer_forget_surface(struct surface *surface)
{
	for (size_t i = 0; i < core.seat.pointer.nr_pressed;) {
		struct pointer_button *pressed = &core.seat.pointer.pressed[i];
		if (surface_contains(surface, pressed->surface)) {
			forget_pressed_button(pressed);
		} else {
			i++;
		}
	}
	pointer_gestures_forget_surface(surface);
	struct pointer_grab *grab = &core.seat.pointer.grab;
	// The grab of a window ends when it is unmapped.
	if (grab->window && grab->window->surface == surface) {
		pointer_cancel_grab();
	}
	if (grab->type == POINTER_GRAB_DND
			&& surface_contains(surface, grab->drag.focus)) {
		drag_motion(&grab->drag, NULL, 0.0, 0.0, 0, false);
	}
	if (core.xwayland.xwm) {
		xwm_x11_drag_forget_surface(surface);
	}
	if (surface_contains(surface, core.seat.pointer.focus)) {
		pointer_focus(NULL, 0.0, 0.0);
	}
}

// Unmapping already dropped every other reference to the surface.
void
pointer_surface_destroy(struct surface *surface)
{
	if (core.seat.pointer.cursor.surface == surface) {
		pointer_set_client_cursor(NULL, NULL, 0, 0, 0);
	}
	if (drag_icon() == surface) {
		core.seat.pointer.grab.drag.icon = NULL;
	}
}

void
pointer_fini(void)
{
	assert(wl_list_empty(&core.seat.pointer.resources));
	assert(core.seat.pointer.grab.type == POINTER_GRAB_NONE);
}
