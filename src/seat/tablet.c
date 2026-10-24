#include <assert.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "core.h"
#include "surface.h"

// Tablet tools go to the surfaces of the clients that take tablet input. The
// others go to the pointer as if a mouse moved to them, with the tip as its left
// button and the buttons of pens as its right and middle buttons. So they
// operate decorations, the window switcher and the clients without tablet
// input. Either way, the tools move the pointer, which shows their cursor.

struct tablet_tool_resource {
	struct wl_resource *resource;
	struct wl_list link; // tablet_tool.resources
	struct tablet_tool *tool; // NULL once the tool is removed
	// wp_cursor_shape_device_v1 resource links, which are inert once the
	// zwp_tablet_tool_v2 is destroyed.
	struct wl_list cursor_shape_devices;
};

static const struct zwp_tablet_v2_interface tablet_impl = {
	.destroy = resource_destroy_req,
};

static void
send_tablet_added(struct wl_resource *seat, struct tablet *tablet)
{
	struct wl_resource *resource = wl_resource_create(
		wl_resource_get_client(seat), &zwp_tablet_v2_interface,
		wl_resource_get_version(seat), 0);
	assert(resource);
	wl_resource_set_implementation(resource, &tablet_impl, tablet, resource_unlink);
	list_append(&tablet->resources, wl_resource_get_link(resource));
	zwp_tablet_seat_v2_send_tablet_added(seat, resource);
	zwp_tablet_v2_send_name(resource, tablet->name);
	zwp_tablet_v2_send_id(resource, tablet->vendor, tablet->product);
	if (tablet->path) {
		zwp_tablet_v2_send_path(resource, tablet->path);
	}
	zwp_tablet_v2_send_done(resource);
}

static void
tool_set_cursor_req(struct wl_client *client, struct wl_resource *resource,
	uint32_t serial, struct wl_resource *surface, int32_t hotspot_x,
	int32_t hotspot_y)
{
	struct tablet_tool_resource *tool_resource = wl_resource_get_user_data(resource);
	struct tablet_tool *tool = tool_resource->tool;
	if (!tool || tool->proximity_serial != serial
			|| !resource_matches_surface(resource, tool->focus)) {
		return;
	}
	struct surface *cursor = surface ? wl_resource_get_user_data(surface) : NULL;
	if (!cursor) {
		pointer_set_client_cursor(resource, NULL, 0, 0, 0);
		return;
	}
	if (cursor->role != SURFACE_ROLE_NONE && cursor->role != SURFACE_ROLE_CURSOR) {
		wl_resource_post_error(resource, ZWP_TABLET_TOOL_V2_ERROR_ROLE,
			"wl_surface already has another role");
		return;
	}
	cursor->role = SURFACE_ROLE_CURSOR;
	pointer_set_client_cursor(resource, cursor, 0,
		hotspot_x + cursor->offset_x, hotspot_y + cursor->offset_y);
}

static const struct zwp_tablet_tool_v2_interface tool_impl = {
	.set_cursor = tool_set_cursor_req,
	.destroy = resource_destroy_req,
};

static void
tool_resource_destroy(struct wl_resource *resource)
{
	struct tablet_tool_resource *tool_resource = wl_resource_get_user_data(resource);
	if (core.seat.pointer.cursor.resource == resource) {
		pointer_set_client_cursor(NULL, NULL, 0, 0, 0);
	}
	struct wl_resource *device, *tmp;
	wl_resource_for_each_safe(device, tmp, &tool_resource->cursor_shape_devices) {
		wl_resource_set_user_data(device, NULL);
		wl_list_remove(wl_resource_get_link(device));
		wl_list_init(wl_resource_get_link(device));
	}
	wl_list_remove(&tool_resource->link);
	free(tool_resource);
}

static void
send_tool_added(struct wl_resource *seat, struct tablet_tool *tool)
{
	struct tablet_tool_resource *tool_resource = calloc(1, sizeof(*tool_resource));
	assert(tool_resource);
	tool_resource->resource = wl_resource_create(wl_resource_get_client(seat),
		&zwp_tablet_tool_v2_interface, wl_resource_get_version(seat), 0);
	assert(tool_resource->resource);
	wl_resource_set_implementation(tool_resource->resource, &tool_impl,
		tool_resource, tool_resource_destroy);
	tool_resource->tool = tool;
	wl_list_init(&tool_resource->cursor_shape_devices);
	list_append(&tool->resources, &tool_resource->link);

	struct wl_resource *resource = tool_resource->resource;
	zwp_tablet_seat_v2_send_tool_added(seat, resource);
	zwp_tablet_tool_v2_send_type(resource, tool->type);
	if (tool->hardware_serial) {
		zwp_tablet_tool_v2_send_hardware_serial(resource,
			(uint32_t)(tool->hardware_serial >> 32),
			(uint32_t)tool->hardware_serial);
	}
	if (tool->hardware_id) {
		zwp_tablet_tool_v2_send_hardware_id_wacom(resource,
			(uint32_t)(tool->hardware_id >> 32), (uint32_t)tool->hardware_id);
	}
	for (uint32_t capability = ZWP_TABLET_TOOL_V2_CAPABILITY_TILT;
			capability <= ZWP_TABLET_TOOL_V2_CAPABILITY_WHEEL; capability++) {
		if (tool->capabilities & (1u << capability)) {
			zwp_tablet_tool_v2_send_capability(resource, capability);
		}
	}
	zwp_tablet_tool_v2_send_done(resource);
}

static const struct zwp_tablet_seat_v2_interface tablet_seat_impl = {
	.destroy = resource_destroy_req,
};

static void
tablet_manager_get_tablet_seat_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id, struct wl_resource *seat)
{
	struct wl_resource *tablet_seat = wl_resource_create(client,
		&zwp_tablet_seat_v2_interface, wl_resource_get_version(resource), id);
	assert(tablet_seat);
	wl_resource_set_implementation(tablet_seat, &tablet_seat_impl, NULL,
		resource_unlink);
	list_append(&core.seat.tablets.seats, wl_resource_get_link(tablet_seat));
	struct tablet *tablet;
	wl_list_for_each(tablet, &core.seat.tablets.tablets, link) {
		send_tablet_added(tablet_seat, tablet);
	}
	struct tablet_tool *tool;
	wl_list_for_each(tool, &core.seat.tablets.tools, link) {
		send_tool_added(tablet_seat, tool);
	}
	struct tablet_pad *pad;
	wl_list_for_each(pad, &core.seat.tablets.pads, link) {
		tablet_pad_send_added(tablet_seat, pad);
	}
}

static const struct zwp_tablet_manager_v2_interface tablet_manager_impl = {
	.get_tablet_seat = tablet_manager_get_tablet_seat_req,
	.destroy = resource_destroy_req,
};

void
wayland_bind_tablet_manager(struct wl_client *client, void *data,
	uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&zwp_tablet_manager_v2_interface, MIN(version, TABLET_MANAGER_VERSION), id);
	assert(resource);
	wl_resource_set_implementation(resource, &tablet_manager_impl, NULL, NULL);
}

static void
cursor_shape_device_set_shape_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t serial, uint32_t shape)
{
	if (!cursor_shape_validate(resource, shape)) {
		return;
	}
	struct tablet_tool_resource *tool_resource = wl_resource_get_user_data(resource);
	struct tablet_tool *tool = tool_resource ? tool_resource->tool : NULL;
	if (!tool || tool->proximity_serial != serial
			|| !resource_matches_surface(resource, tool->focus)) {
		return;
	}
	pointer_set_client_cursor(tool_resource->resource, NULL, shape, 0, 0);
}

static const struct wp_cursor_shape_device_v1_interface cursor_shape_device_impl = {
	.destroy = resource_destroy_req,
	.set_shape = cursor_shape_device_set_shape_req,
};

void
tablet_tool_get_cursor_shape_device(struct wl_client *client, uint32_t version,
	uint32_t id, struct wl_resource *tool)
{
	struct tablet_tool_resource *tool_resource = wl_resource_get_user_data(tool);
	struct wl_resource *device = wl_resource_create(client,
		&wp_cursor_shape_device_v1_interface, version, id);
	assert(device);
	wl_resource_set_implementation(device, &cursor_shape_device_impl,
		tool_resource, resource_unlink);
	list_append(&tool_resource->cursor_shape_devices, wl_resource_get_link(device));
}

// The pads of the tablet may enter the keyboard focus then.
struct tablet *
tablet_create(struct libinput_device *device,
	struct libinput_device_group *group, const char *name, uint32_t vendor,
	uint32_t product, const char *path)
{
	struct tablet *tablet = calloc(1, sizeof(*tablet));
	assert(tablet);
	tablet->device = device;
	tablet->group = group;
	wl_list_init(&tablet->resources);
	tablet->name = strdup(name);
	assert(tablet->name);
	tablet->vendor = vendor;
	tablet->product = product;
	if (path) {
		tablet->path = strdup(path);
		assert(tablet->path);
	}
	list_append(&core.seat.tablets.tablets, &tablet->link);
	struct wl_resource *seat;
	wl_resource_for_each(seat, &core.seat.tablets.seats) {
		send_tablet_added(seat, tablet);
	}
	tablet_pads_update_focus();
	return tablet;
}

static void
tablet_tool_destroy(struct tablet_tool *tool)
{
	assert(!tool->focus);
	struct tablet_tool_resource *tool_resource, *tmp;
	wl_list_for_each_safe(tool_resource, tmp, &tool->resources, link) {
		zwp_tablet_tool_v2_send_removed(tool_resource->resource);
		tool_resource->tool = NULL;
		wl_list_remove(&tool_resource->link);
		wl_list_init(&tool_resource->link);
	}
	wl_list_remove(&tool->link);
	free(tool);
}

// libinput takes the tools out of proximity before removing their tablet. Its
// pads leave their focus, which they entered with the tablet.
void
tablet_destroy(struct tablet *tablet)
{
	wl_list_remove(&tablet->link);
	tablet_pads_update_focus();
	struct tablet_tool *tool, *tmp_tool;
	wl_list_for_each_safe(tool, tmp_tool, &core.seat.tablets.tools, link) {
		assert(tool->tablet != tablet);
		if (tool->owner == tablet) {
			tablet_tool_destroy(tool);
		}
	}
	struct wl_resource *resource, *tmp;
	wl_resource_for_each_safe(resource, tmp, &tablet->resources) {
		zwp_tablet_v2_send_removed(resource);
		wl_resource_set_user_data(resource, NULL);
		wl_list_remove(wl_resource_get_link(resource));
		wl_list_init(wl_resource_get_link(resource));
	}
	free(tablet->name);
	free(tablet->path);
	free(tablet);
}

struct tablet_tool *
tablet_tool_create(struct tablet *owner, enum zwp_tablet_tool_v2_type type,
	uint64_t hardware_serial, uint64_t hardware_id, uint32_t capabilities)
{
	struct tablet_tool *tool = calloc(1, sizeof(*tool));
	assert(tool);
	wl_list_init(&tool->resources);
	tool->type = type;
	tool->hardware_serial = hardware_serial;
	tool->hardware_id = hardware_id;
	tool->capabilities = capabilities;
	tool->owner = owner;
	list_append(&core.seat.tablets.tools, &tool->link);
	struct wl_resource *seat;
	wl_resource_for_each(seat, &core.seat.tablets.seats) {
		send_tool_added(seat, tool);
	}
	return tool;
}

// The zwp_tablet_v2 of the client for the tablet, or NULL
struct wl_resource *
tablet_resource_for(struct tablet *tablet, struct wl_client *client)
{
	struct wl_resource *resource;
	wl_resource_for_each(resource, &tablet->resources) {
		if (wl_resource_get_client(resource) == client) {
			return resource;
		}
	}
	return NULL;
}

static bool
takes_tablet_input(struct tablet_tool *tool, struct surface *surface)
{
	struct wl_client *client = wl_resource_get_client(surface->resource);
	if (!tablet_resource_for(tool->tablet, client)) {
		return false;
	}
	struct tablet_tool_resource *tool_resource;
	wl_list_for_each(tool_resource, &tool->resources, link) {
		if (wl_resource_get_client(tool_resource->resource) == client) {
			return true;
		}
	}
	return false;
}

// The tool leaves its focus as if lifted from the tablet, and moves the
// pointer instead.
void
tablet_tool_leave(struct tablet_tool *tool, uint32_t time)
{
	if (!tool->focus) {
		return;
	}
	struct tablet_tool_resource *tool_resource;
	wl_list_for_each(tool_resource, &tool->resources, link) {
		struct wl_resource *resource = tool_resource->resource;
		if (!resource_matches_surface(resource, tool->focus)) {
			continue;
		}
		if (tool->down) {
			zwp_tablet_tool_v2_send_up(resource);
		}
		zwp_tablet_tool_v2_send_proximity_out(resource);
		zwp_tablet_tool_v2_send_frame(resource, time);
	}
	tool->focus = NULL;
	pointer_set_tablet_tool(NULL);
}

// Only one tool has a focus at a time: the one moving the pointer.
static void
set_focus(struct tablet_tool *tool, struct surface *surface, uint32_t time)
{
	if (tool->focus == surface) {
		return;
	}
	tablet_tool_leave(tool, time);
	if (!surface) {
		return;
	}
	if (core.seat.pointer.tablet_tool) {
		tablet_tool_leave(core.seat.pointer.tablet_tool, time);
	}
	tool->focus = surface;
	tool->proximity_serial = ++core.serial;
	struct wl_resource *tablet = tablet_resource_for(tool->tablet,
		wl_resource_get_client(surface->resource));
	struct tablet_tool_resource *tool_resource;
	wl_list_for_each(tool_resource, &tool->resources, link) {
		if (resource_matches_surface(tool_resource->resource, surface)) {
			zwp_tablet_tool_v2_send_proximity_in(tool_resource->resource,
				tool->proximity_serial, tablet, surface->resource);
		}
	}
	pointer_set_tablet_tool(tool);
}

static void
send_axes(struct wl_resource *resource, struct tablet_tool *tool,
	const struct tablet_tool_axes *axes)
{
	double sx, sy;
	surface_from_layout(tool->focus, tool->x, tool->y, &sx, &sy);
	zwp_tablet_tool_v2_send_motion(resource, wl_fixed_from_double(sx),
		wl_fixed_from_double(sy));
	if (axes->changed & (1u << ZWP_TABLET_TOOL_V2_CAPABILITY_PRESSURE)) {
		zwp_tablet_tool_v2_send_pressure(resource,
			(uint32_t)round(axes->pressure * 65535));
	}
	if (axes->changed & (1u << ZWP_TABLET_TOOL_V2_CAPABILITY_DISTANCE)) {
		zwp_tablet_tool_v2_send_distance(resource,
			(uint32_t)round(axes->distance * 65535));
	}
	if (axes->changed & (1u << ZWP_TABLET_TOOL_V2_CAPABILITY_TILT)) {
		zwp_tablet_tool_v2_send_tilt(resource, wl_fixed_from_double(axes->tilt_x),
			wl_fixed_from_double(axes->tilt_y));
	}
	if (axes->changed & (1u << ZWP_TABLET_TOOL_V2_CAPABILITY_ROTATION)) {
		zwp_tablet_tool_v2_send_rotation(resource,
			wl_fixed_from_double(axes->rotation));
	}
	if (axes->changed & (1u << ZWP_TABLET_TOOL_V2_CAPABILITY_SLIDER)) {
		zwp_tablet_tool_v2_send_slider(resource,
			(int32_t)round(axes->slider * 65535));
	}
	if (axes->changed & (1u << ZWP_TABLET_TOOL_V2_CAPABILITY_WHEEL)) {
		zwp_tablet_tool_v2_send_wheel(resource,
			wl_fixed_from_double(axes->wheel), axes->wheel_clicks);
	}
}

// The tool picks the surface under it while it presses nothing, so that the
// surface it pressed gets the release, like an implicit grab. The caller ends
// the frame of the focus.
static void
move_tool(struct tablet_tool *tool, const struct tablet_tool_axes *axes,
	uint32_t time)
{
	tool->x = axes->x;
	tool->y = axes->y;
	if (!tool->down && tool->nr_pressed_buttons == 0) {
		struct surface *surface = direct_input_surface_at(tool->x, tool->y);
		set_focus(tool, surface && takes_tablet_input(tool, surface)
			? surface : NULL, time);
	}
	if (!tool->focus) {
		pointer_handle_motion_absolute(tool->x, tool->y, time);
		return;
	}
	pointer_warp(tool->x, tool->y);
	struct tablet_tool_resource *tool_resource;
	wl_list_for_each(tool_resource, &tool->resources, link) {
		if (resource_matches_surface(tool_resource->resource, tool->focus)) {
			send_axes(tool_resource->resource, tool, axes);
		}
	}
}

static void
send_frame(struct tablet_tool *tool, uint32_t time)
{
	struct tablet_tool_resource *tool_resource;
	wl_list_for_each(tool_resource, &tool->resources, link) {
		if (resource_matches_surface(tool_resource->resource, tool->focus)) {
			zwp_tablet_tool_v2_send_frame(tool_resource->resource, time);
		}
	}
}

void
tablet_tool_handle_proximity_in(struct tablet_tool *tool,
	struct tablet *tablet, const struct tablet_tool_axes *axes, uint32_t time)
{
	tool->tablet = tablet;
	move_tool(tool, axes, time);
	send_frame(tool, time);
}

// libinput lifts the tip and releases the buttons before.
void
tablet_tool_handle_proximity_out(struct tablet_tool *tool, uint32_t time)
{
	tablet_tool_leave(tool, time);
	tool->tablet = NULL;
}

void
tablet_tool_handle_axis(struct tablet_tool *tool,
	const struct tablet_tool_axes *axes, uint32_t time)
{
	move_tool(tool, axes, time);
	send_frame(tool, time);
}

void
tablet_tool_handle_tip(struct tablet_tool *tool,
	const struct tablet_tool_axes *axes, bool down, uint32_t time)
{
	move_tool(tool, axes, time);
	tool->down = down;
	if (!tool->focus) {
		pointer_handle_button(time, BTN_LEFT, down);
		return;
	}
	if (down) {
		tool->down_serial = direct_input_press(tool->focus);
	}
	struct tablet_tool_resource *tool_resource;
	wl_list_for_each(tool_resource, &tool->resources, link) {
		struct wl_resource *resource = tool_resource->resource;
		if (!resource_matches_surface(resource, tool->focus)) {
			continue;
		}
		if (down) {
			zwp_tablet_tool_v2_send_down(resource, tool->down_serial);
		} else {
			zwp_tablet_tool_v2_send_up(resource);
		}
		zwp_tablet_tool_v2_send_frame(resource, time);
	}
}

// The buttons of pens press the right and middle buttons of the pointer.
// Those of tablet mice are mouse buttons already.
static uint32_t
pointer_button_of(uint32_t button)
{
	switch (button) {
	case BTN_STYLUS:
		return BTN_RIGHT;
	case BTN_STYLUS2:
		return BTN_MIDDLE;
	}
	return button;
}

void
tablet_tool_handle_button(struct tablet_tool *tool, uint32_t button,
	bool pressed, uint32_t time)
{
	tool->nr_pressed_buttons += pressed ? 1 : -1;
	if (!tool->focus) {
		pointer_handle_button(time, pointer_button_of(button), pressed);
		return;
	}
	uint32_t serial = pressed ? direct_input_press(tool->focus) : ++core.serial;
	uint32_t state = pressed ? ZWP_TABLET_TOOL_V2_BUTTON_STATE_PRESSED
		: ZWP_TABLET_TOOL_V2_BUTTON_STATE_RELEASED;
	struct tablet_tool_resource *tool_resource;
	wl_list_for_each(tool_resource, &tool->resources, link) {
		if (resource_matches_surface(tool_resource->resource, tool->focus)) {
			zwp_tablet_tool_v2_send_button(tool_resource->resource, serial,
				button, state);
			zwp_tablet_tool_v2_send_frame(tool_resource->resource, time);
		}
	}
}

// The compositor takes the tool for a grab when its tip is down on the
// surface. The tool leaves the surface, and moves the pointer instead until
// the tip is lifted.
bool
tablet_take_tool(struct surface *ancestor, uint32_t serial)
{
	struct tablet_tool *tool;
	wl_list_for_each(tool, &core.seat.tablets.tools, link) {
		if (tool->down && tool->down_serial == serial
				&& surface_contains(ancestor, tool->focus)) {
			tablet_tool_leave(tool, monotonic_msec());
			pointer_warp(tool->x, tool->y);
			return true;
		}
	}
	return false;
}

uint32_t
tablet_press_serial(struct surface *ancestor)
{
	struct tablet_tool *tool;
	wl_list_for_each(tool, &core.seat.tablets.tools, link) {
		if (tool->down && surface_contains(ancestor, tool->focus)) {
			return tool->down_serial;
		}
	}
	return 0;
}

void
tablet_forget_surface(struct surface *surface)
{
	struct tablet_tool *tool;
	wl_list_for_each(tool, &core.seat.tablets.tools, link) {
		if (surface_contains(surface, tool->focus)) {
			tablet_tool_leave(tool, monotonic_msec());
		}
	}
}

// The clients are gone.
void
tablets_fini(void)
{
	assert(wl_list_empty(&core.seat.tablets.seats));
	struct tablet_tool *tool, *tmp_tool;
	wl_list_for_each_safe(tool, tmp_tool, &core.seat.tablets.tools, link) {
		tablet_tool_destroy(tool);
	}
	struct tablet_pad *pad, *tmp_pad;
	wl_list_for_each_safe(pad, tmp_pad, &core.seat.tablets.pads, link) {
		tablet_pad_destroy(pad);
	}
	struct tablet *tablet, *tmp;
	wl_list_for_each_safe(tablet, tmp, &core.seat.tablets.tablets, link) {
		tablet_destroy(tablet);
	}
}
