#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core.h"
#include "ext-data-control-v1-protocol.h"
#include "primary-selection-unstable-v1-protocol.h"
#include "surface.h"

struct data_device {
	struct wl_resource *resource;
	struct wl_list link; // core.seat.data_devices
	struct data_offer *drag_offer;
};

// A wl_data_offer, zwp_primary_selection_offer_v1 or ext_data_control_offer_v1.
// The rest of the state is of wl_data_offers of drags.
struct data_offer {
	struct wl_resource *resource;
	struct wl_list link; // data_source.offers
	struct data_source *source; // NULL once inert
	struct data_device *device;
	char *accepted_mime_type;
	uint32_t enter_serial;
	uint32_t actions;
	uint32_t preferred_action;
	uint32_t action;
	bool drag;
	bool active;
	bool dropped;
	bool finished;
};

static const uint32_t valid_drag_actions =
	WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY
	| WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE
	| WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK;

static const struct wl_data_source_interface data_source_impl;
static const struct wl_data_device_interface data_device_impl;
static const struct wl_data_offer_interface data_offer_impl;

struct data_source *
data_source_create(enum data_source_type type, struct wl_resource *resource)
{
	struct data_source *source = calloc(1, sizeof(*source));
	assert(source);
	source->type = type;
	source->resource = resource;
	wl_array_init(&source->mime_types);
	wl_list_init(&source->offers);
	return source;
}

bool
data_source_has_mime_type(struct data_source *source, const char *mime_type)
{
	char **value;
	wl_array_for_each(value, &source->mime_types) {
		if (strcmp(*value, mime_type) == 0) {
			return true;
		}
	}
	return false;
}

void
data_source_offer(struct data_source *source, const char *mime_type)
{
	if (data_source_has_mime_type(source, mime_type)) {
		return;
	}
	char **value = wl_array_add(&source->mime_types, sizeof(*value));
	assert(value);
	*value = strdup(mime_type);
	assert(*value);
}

// The source writes the data in the MIME type into the fd, which it takes.
void
data_source_send(struct data_source *source, const char *mime_type, int fd)
{
	switch (source->type) {
	case DATA_SOURCE_DATA_DEVICE:
		wl_data_source_send_send(source->resource, mime_type, fd);
		break;
	case DATA_SOURCE_PRIMARY_SELECTION:
		zwp_primary_selection_source_v1_send_send(source->resource, mime_type, fd);
		break;
	case DATA_SOURCE_DATA_CONTROL:
		ext_data_control_source_v1_send_send(source->resource, mime_type, fd);
		break;
	case DATA_SOURCE_XWAYLAND:
	case DATA_SOURCE_XDND:
		xwm_data_source_send(source, mime_type, fd);
		return;
	}
	close(fd);
}

static void
invalidate_offers(struct data_source *source)
{
	struct data_offer *offer, *tmp;
	wl_list_for_each_safe(offer, tmp, &source->offers, link) {
		offer->source = NULL;
		wl_list_remove(&offer->link);
		wl_list_init(&offer->link);
	}
}

// The client destroys the source once it is cancelled. X11 selections are
// destroyed right away, as nothing else holds them, and X11 drags end.
static void
cancel_source(struct data_source *source)
{
	invalidate_offers(source);
	switch (source->type) {
	case DATA_SOURCE_DATA_DEVICE:
		wl_data_source_send_cancelled(source->resource);
		return;
	case DATA_SOURCE_PRIMARY_SELECTION:
		zwp_primary_selection_source_v1_send_cancelled(source->resource);
		return;
	case DATA_SOURCE_DATA_CONTROL:
		ext_data_control_source_v1_send_cancelled(source->resource);
		return;
	case DATA_SOURCE_XWAYLAND:
		data_source_destroy(source);
		return;
	case DATA_SOURCE_XDND:
		xwm_x11_drag_finish(false);
		return;
	}
}

// Drag sources of X11 clients are told the target and the action by
// XdndStatus, and the end of the drop by XdndFinished.
static void
send_source_target(struct data_source *source, const char *mime_type)
{
	if (source->type == DATA_SOURCE_XDND) {
		xwm_x11_drag_accept(mime_type != NULL);
	} else {
		wl_data_source_send_target(source->resource, mime_type);
	}
}

static void
send_source_action(struct data_source *source, uint32_t action)
{
	if (source->type == DATA_SOURCE_XDND) {
		xwm_x11_drag_action(action);
	} else if (wl_resource_get_version(source->resource)
			>= WL_DATA_SOURCE_ACTION_SINCE_VERSION) {
		wl_data_source_send_action(source->resource, action);
	}
}

static void
send_source_dnd_finished(struct data_source *source)
{
	if (source->type == DATA_SOURCE_XDND) {
		xwm_x11_drag_finish(true);
	} else if (wl_resource_get_version(source->resource)
			>= WL_DATA_SOURCE_DND_FINISHED_SINCE_VERSION) {
		wl_data_source_send_dnd_finished(source->resource);
	}
}

static void
data_offer_resource_destroy(struct wl_resource *resource)
{
	struct data_offer *offer = wl_resource_get_user_data(resource);
	assert(offer);
	// The target ends the drop on it. Offers too old to be finished finish
	// the drag, and the others cancel it unless they are finished.
	if (offer->dropped && offer->source) {
		if (wl_resource_get_version(offer->resource)
				< WL_DATA_OFFER_FINISH_SINCE_VERSION) {
			send_source_dnd_finished(offer->source);
		} else {
			cancel_source(offer->source);
		}
	}
	if (offer->device && offer->device->drag_offer == offer) {
		offer->device->drag_offer = NULL;
	}
	wl_list_remove(&offer->link);
	free(offer->accepted_mime_type);
	free(offer);
}

static struct data_offer *
create_offer(struct wl_resource *device, const struct wl_interface *interface,
	const void *implementation, struct data_source *source)
{
	struct data_offer *offer = calloc(1, sizeof(*offer));
	assert(offer);
	offer->resource = wl_resource_create(wl_resource_get_client(device),
		interface, wl_resource_get_version(device), 0);
	assert(offer->resource);
	offer->source = source;
	list_append(&source->offers, &offer->link);
	wl_resource_set_implementation(offer->resource, implementation, offer,
		data_offer_resource_destroy);
	return offer;
}

// Creates an offer of the source for the client of the device, to be
// introduced by the device.
struct wl_resource *
data_offer_create(struct wl_resource *device,
	const struct wl_interface *interface, const void *implementation,
	struct data_source *source)
{
	return create_offer(device, interface, implementation, source)->resource;
}

// Also the receive request of zwp_primary_selection_offer_v1 and
// ext_data_control_offer_v1.
void
data_offer_receive(struct wl_client *client, struct wl_resource *resource,
	const char *mime_type, int32_t fd)
{
	struct data_offer *offer = wl_resource_get_user_data(resource);
	assert(offer);
	if (offer->source && data_source_has_mime_type(offer->source, mime_type)) {
		data_source_send(offer->source, mime_type, fd);
	} else {
		close(fd);
	}
}

static void
send_offer_mime_types(struct data_offer *offer)
{
	char **mime_type;
	wl_array_for_each(mime_type, &offer->source->mime_types) {
		wl_data_offer_send_offer(offer->resource, *mime_type);
	}
}

static void
send_selection(struct data_device *device)
{
	struct data_source *source = core.seat.selections[SELECTION_CLIPBOARD];
	if (!source) {
		wl_data_device_send_selection(device->resource, NULL);
		return;
	}

	struct data_offer *offer = create_offer(device->resource,
		&wl_data_offer_interface, &data_offer_impl, source);

	wl_data_device_send_data_offer(device->resource, offer->resource);
	send_offer_mime_types(offer);
	wl_data_device_send_selection(device->resource, offer->resource);
}

static void
send_selection_to_client(struct wl_client *client)
{
	struct data_device *device;
	wl_list_for_each(device, &core.seat.data_devices, link) {
		if (wl_resource_get_client(device->resource) == client) {
			send_selection(device);
		}
	}
}

// The focused client, data control clients, and X11 clients through the
// window manager see the selections.
static void
broadcast_selection(enum selection_type type)
{
	struct wl_client *client = core.seat.selection_client;
	if (client && type == SELECTION_CLIPBOARD) {
		send_selection_to_client(client);
	} else if (client) {
		primary_selection_send(client);
	}
	data_control_send_selection(type);
	if (core.xwayland.xwm) {
		xwm_set_selection(type);
	}
}

// Setting the source again keeps it, as GTK does after offering more MIME
// types.
void
seat_set_selection(enum selection_type type, struct data_source *source)
{
	struct data_source *old = core.seat.selections[type];
	core.seat.selections[type] = source;
	if (source) {
		source->used = true;
	}
	if (old && old != source) {
		cancel_source(old);
	}
	broadcast_selection(type);
}

void
data_source_destroy(struct data_source *source)
{
	invalidate_offers(source);
	for (int type = 0; type < NR_SELECTION_TYPES; type++) {
		if (core.seat.selections[type] == source) {
			core.seat.selections[type] = NULL;
			broadcast_selection(type);
		}
	}
	char **mime_type;
	wl_array_for_each(mime_type, &source->mime_types) {
		free(*mime_type);
	}
	wl_array_release(&source->mime_types);
	free(source);
}

static void
leave_drag_focus(struct drag *drag)
{
	if (!drag->focus) {
		return;
	}
	if (drag->focus->role == SURFACE_ROLE_XWAYLAND) {
		if (core.xwayland.xwm) {
			xwm_drag_leave();
		}
	} else {
		struct wl_client *client = wl_resource_get_client(drag->focus->resource);
		struct data_device *device;
		wl_list_for_each(device, &core.seat.data_devices, link) {
			if (wl_resource_get_client(device->resource) != client) {
				continue;
			}
			wl_data_device_send_leave(device->resource);
			if (device->drag_offer) {
				device->drag_offer->active = false;
				device->drag_offer->device = NULL;
				device->drag_offer = NULL;
			}
		}
	}
	if (drag->source) {
		send_source_target(drag->source, NULL);
	}
	drag->focus = NULL;
}

static uint32_t
choose_drag_action(struct data_offer *offer)
{
	uint32_t source_actions = offer->source->actions_set
		? offer->source->actions : WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
	uint32_t actions = source_actions & offer->actions;
	if (offer->preferred_action & actions) {
		return offer->preferred_action;
	}
	if (actions & WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY) {
		return WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
	}
	if (actions & WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE) {
		return WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;
	}
	if (actions & WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK) {
		return WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK;
	}
	return WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;
}

static void
update_drag_action(struct data_offer *offer)
{
	uint32_t action = choose_drag_action(offer);
	if (offer->action == action) {
		return;
	}
	offer->action = action;
	send_source_action(offer->source, action);
	if (wl_resource_get_version(offer->resource)
			>= WL_DATA_OFFER_ACTION_SINCE_VERSION) {
		wl_data_offer_send_action(offer->resource, action);
	}
}

static void
send_source_actions(struct data_offer *offer)
{
	if (wl_resource_get_version(offer->resource)
			>= WL_DATA_OFFER_SOURCE_ACTIONS_SINCE_VERSION) {
		uint32_t actions = offer->source->actions_set
			? offer->source->actions : WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
		wl_data_offer_send_source_actions(offer->resource, actions);
	}
}

// Drag sources of X11 clients also change their actions while dragging, by the
// modifiers that the user holds.
void
data_source_set_actions(struct data_source *source, uint32_t actions)
{
	if (source->actions_set && source->actions == actions) {
		return;
	}
	source->actions_set = true;
	source->actions = actions;
	struct data_offer *offer;
	wl_list_for_each(offer, &source->offers, link) {
		if (offer->active) {
			send_source_actions(offer);
			update_drag_action(offer);
		}
	}
}

static struct data_offer *
create_drag_offer(struct data_device *device, struct data_source *source,
	uint32_t enter_serial)
{
	if (!source) {
		return NULL;
	}
	struct data_offer *offer = create_offer(device->resource,
		&wl_data_offer_interface, &data_offer_impl, source);
	offer->device = device;
	offer->enter_serial = enter_serial;
	offer->drag = true;
	offer->active = true;
	offer->actions = wl_resource_get_version(offer->resource)
		>= WL_DATA_OFFER_ACTION_SINCE_VERSION ? 0
		: WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;

	wl_data_device_send_data_offer(device->resource, offer->resource);
	send_source_actions(offer);
	send_offer_mime_types(offer);
	update_drag_action(offer);
	return offer;
}

static void
enter_drag_focus(struct drag *drag, struct surface *surface, double sx, double sy)
{
	drag->focus = surface;
	if (!surface) {
		return;
	}
	// X11 windows take drags by XDND from the window manager, as Xwayland
	// has no data devices.
	if (surface->role == SURFACE_ROLE_XWAYLAND) {
		if (drag->source && core.xwayland.xwm) {
			xwm_drag_enter(drag->source, surface);
		}
		return;
	}
	struct wl_client *client = wl_resource_get_client(surface->resource);
	uint32_t serial = ++core.serial;
	struct data_device *device;
	wl_list_for_each(device, &core.seat.data_devices, link) {
		if (wl_resource_get_client(device->resource) != client) {
			continue;
		}
		device->drag_offer = create_drag_offer(device, drag->source, serial);
		wl_data_device_send_enter(device->resource, serial, surface->resource,
			wl_fixed_from_double(sx), wl_fixed_from_double(sy),
			device->drag_offer ? device->drag_offer->resource : NULL);
	}
}

// The pointer ends the drag without a drop.
void
drag_cancel(struct drag *drag)
{
	leave_drag_focus(drag);
	if (drag->source) {
		cancel_source(drag->source);
	}
}

void
data_device_init(void)
{
	wl_list_init(&core.seat.data_devices);
	wl_list_init(&core.seat.primary_selection_devices);
	wl_list_init(&core.seat.data_control_devices);
}

void
data_device_fini(void)
{
	assert(wl_list_empty(&core.seat.data_devices));
	assert(wl_list_empty(&core.seat.primary_selection_devices));
	assert(wl_list_empty(&core.seat.data_control_devices));
	for (int type = 0; type < NR_SELECTION_TYPES; type++) {
		assert(!core.seat.selections[type]);
	}
}

void
data_device_keyboard_focus(struct surface *surface)
{
	struct wl_client *client = surface
		? wl_resource_get_client(surface->resource) : NULL;
	if (core.seat.selection_client == client) {
		return;
	}
	core.seat.selection_client = client;
	if (client) {
		send_selection_to_client(client);
		primary_selection_send(client);
	}
}

// The pointer moves the drag over the surface. Drags without a source only
// enter the surfaces of their client.
void
drag_motion(struct drag *drag, struct surface *surface, double sx, double sy,
	uint32_t time, bool send_motion)
{
	if (!drag->source && surface
			&& wl_resource_get_client(surface->resource) != drag->client) {
		surface = NULL;
	}
	if (drag->focus != surface) {
		leave_drag_focus(drag);
		enter_drag_focus(drag, surface, sx, sy);
		return;
	}
	if (!send_motion || !surface) {
		return;
	}
	if (surface->role == SURFACE_ROLE_XWAYLAND) {
		if (core.xwayland.xwm) {
			xwm_drag_motion();
		}
		return;
	}
	struct wl_client *client = wl_resource_get_client(surface->resource);
	struct data_device *device;
	wl_list_for_each(device, &core.seat.data_devices, link) {
		if (wl_resource_get_client(device->resource) == client) {
			wl_data_device_send_motion(device->resource, time,
				wl_fixed_from_double(sx), wl_fixed_from_double(sy));
		}
	}
}

// Returns whether any data device of the focus takes the drop.
static bool
drop_on_data_devices(struct drag *drag)
{
	bool success = false;
	struct wl_client *client = wl_resource_get_client(drag->focus->resource);
	struct data_device *device;
	wl_list_for_each(device, &core.seat.data_devices, link) {
		if (wl_resource_get_client(device->resource) != client) {
			continue;
		}
		struct data_offer *offer = device->drag_offer;
		if (offer) {
			offer->active = false;
		}
		if (drag->source && (!offer || !offer->accepted_mime_type
				|| offer->action == WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE)) {
			continue;
		}
		if (offer) {
			offer->dropped = true;
		}
		wl_data_device_send_drop(device->resource);
		success = true;
	}
	return success;
}

// The pointer ends the drag by releasing its button.
void
drag_drop(struct drag *drag)
{
	bool success = false;
	if (drag->focus && drag->focus->role == SURFACE_ROLE_XWAYLAND) {
		success = core.xwayland.xwm && xwm_drag_drop();
	} else if (drag->focus) {
		success = drop_on_data_devices(drag);
	}
	if (!success) {
		// The target that did not take the drop is left.
		drag_cancel(drag);
		return;
	}
	if (drag->source && drag->source->type == DATA_SOURCE_DATA_DEVICE
			&& wl_resource_get_version(drag->source->resource)
				>= WL_DATA_SOURCE_DND_DROP_PERFORMED_SINCE_VERSION) {
		wl_data_source_send_dnd_drop_performed(drag->source->resource);
	}
}

static void
data_source_offer_req(struct wl_client *client, struct wl_resource *resource,
	const char *mime_type)
{
	struct data_source *source = wl_resource_get_user_data(resource);
	assert(source);
	data_source_offer(source, mime_type);
}

static void
data_source_set_actions_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t actions)
{
	struct data_source *source = wl_resource_get_user_data(resource);
	assert(source);
	if ((actions & ~valid_drag_actions) || source->actions_set || source->used) {
		wl_resource_post_error(resource, WL_DATA_SOURCE_ERROR_INVALID_ACTION_MASK,
			"invalid or repeated drag actions");
		return;
	}
	data_source_set_actions(source, actions);
}

// Also the destructor of zwp_primary_selection_source_v1 and
// ext_data_control_source_v1.
void
data_source_resource_destroy(struct wl_resource *resource)
{
	struct data_source *source = wl_resource_get_user_data(resource);
	assert(source);
	struct pointer_grab *grab = &core.seat.pointer.grab;
	if (grab->type == POINTER_GRAB_DND && grab->drag.source == source) {
		// The drag ends without telling the source, which is gone.
		grab->drag.source = NULL;
		pointer_cancel_grab();
	}
	data_source_destroy(source);
}

static const struct wl_data_source_interface data_source_impl = {
	.offer = data_source_offer_req,
	.destroy = resource_destroy_req,
	.set_actions = data_source_set_actions_req,
};

static void
data_device_start_drag_req(struct wl_client *client,
	struct wl_resource *resource, struct wl_resource *source_resource,
	struct wl_resource *origin_resource, struct wl_resource *icon_resource,
	uint32_t serial)
{
	struct data_device *device = wl_resource_get_user_data(resource);
	assert(device);
	struct data_source *source = source_resource
		? wl_resource_get_user_data(source_resource) : NULL;
	struct surface *origin = wl_resource_get_user_data(origin_resource);
	struct surface *icon = icon_resource
		? wl_resource_get_user_data(icon_resource) : NULL;
	if (source && source->used) {
		wl_resource_post_error(resource, WL_DATA_DEVICE_ERROR_USED_SOURCE,
			"data source has already been used");
		return;
	}
	if (icon && icon->role != SURFACE_ROLE_NONE
			&& icon->role != SURFACE_ROLE_DND_ICON) {
		wl_resource_post_error(resource, WL_DATA_DEVICE_ERROR_ROLE,
			"drag icon surface already has another role");
		return;
	}
	if (!pointer_start_drag(origin, serial, icon, source)) {
		return;
	}
	if (source) {
		source->used = true;
	}
}

// Clients set the selections by the input events that they receive while they
// have the keyboard focus.
bool
input_serial_matches_client(struct wl_client *client, uint32_t serial)
{
	return core.seat.input_serial == serial
		&& core.seat.input_client == client && core.seat.keyboard.focus
		&& wl_resource_get_client(core.seat.keyboard.focus->resource)
			== client;
}

static void
data_device_set_selection_req(struct wl_client *client,
	struct wl_resource *resource, struct wl_resource *source_resource,
	uint32_t serial)
{
	struct data_device *device = wl_resource_get_user_data(resource);
	assert(device);
	if (!input_serial_matches_client(client, serial)) {
		return;
	}
	struct data_source *source = source_resource
		? wl_resource_get_user_data(source_resource) : NULL;
	if (source && source->used
			&& source != core.seat.selections[SELECTION_CLIPBOARD]) {
		wl_resource_post_error(resource, WL_DATA_DEVICE_ERROR_USED_SOURCE,
			"data source has already been used");
		return;
	}
	seat_set_selection(SELECTION_CLIPBOARD, source);
}

static const struct wl_data_device_interface data_device_impl = {
	.start_drag = data_device_start_drag_req,
	.set_selection = data_device_set_selection_req,
	.release = resource_destroy_req,
};

static void
data_device_resource_destroy(struct wl_resource *resource)
{
	struct data_device *device = wl_resource_get_user_data(resource);
	assert(device);
	if (device->drag_offer) {
		device->drag_offer->device = NULL;
	}
	wl_list_remove(&device->link);
	free(device);
	// A drag without a source ends with the client, like others end with
	// their source.
	struct pointer_grab *grab = &core.seat.pointer.grab;
	if (grab->type == POINTER_GRAB_DND && !grab->drag.source
			&& grab->drag.client == wl_resource_get_client(resource)) {
		pointer_cancel_grab();
	}
}

static void
data_offer_accept_req(struct wl_client *client, struct wl_resource *resource,
	uint32_t serial, const char *mime_type)
{
	struct data_offer *offer = wl_resource_get_user_data(resource);
	assert(offer);
	if (offer->finished) {
		wl_resource_post_error(resource, WL_DATA_OFFER_ERROR_INVALID_FINISH,
			"drag offer is already finished");
		return;
	}
	if (!offer->drag || !offer->active
			|| serial != offer->enter_serial || !offer->source) {
		return;
	}
	if (mime_type && !data_source_has_mime_type(offer->source, mime_type)) {
		mime_type = NULL;
	}
	char *accepted = mime_type ? strdup(mime_type) : NULL;
	assert(!mime_type || accepted);
	free(offer->accepted_mime_type);
	offer->accepted_mime_type = accepted;
	send_source_target(offer->source, mime_type);
}

static void
data_offer_receive_req(struct wl_client *client, struct wl_resource *resource,
	const char *mime_type, int32_t fd)
{
	struct data_offer *offer = wl_resource_get_user_data(resource);
	assert(offer);
	if (offer->finished) {
		close(fd);
		wl_resource_post_error(resource, WL_DATA_OFFER_ERROR_INVALID_FINISH,
			"drag offer is already finished");
		return;
	}
	if (offer->drag && !offer->active && !offer->dropped) {
		close(fd);
		return;
	}
	data_offer_receive(client, resource, mime_type, fd);
}

static void
data_offer_finish_req(struct wl_client *client, struct wl_resource *resource)
{
	struct data_offer *offer = wl_resource_get_user_data(resource);
	assert(offer);
	if (offer->finished || !offer->drag || !offer->dropped
			|| !offer->accepted_mime_type
			|| offer->action == WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE
			|| offer->action == WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK) {
		wl_resource_post_error(resource, WL_DATA_OFFER_ERROR_INVALID_FINISH,
			"drag offer cannot be finished");
		return;
	}
	// The source may be destroyed after the drop, before the target finishes.
	if (offer->source) {
		send_source_dnd_finished(offer->source);
	}
	offer->dropped = false;
	offer->finished = true;
}

static void
data_offer_set_actions_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t actions, uint32_t preferred_action)
{
	struct data_offer *offer = wl_resource_get_user_data(resource);
	assert(offer);
	if (offer->finished) {
		wl_resource_post_error(resource, WL_DATA_OFFER_ERROR_INVALID_FINISH,
			"drag offer is already finished");
		return;
	}
	if (actions & ~valid_drag_actions) {
		wl_resource_post_error(resource, WL_DATA_OFFER_ERROR_INVALID_ACTION_MASK,
			"invalid drag action mask");
		return;
	}
	if ((preferred_action & ~valid_drag_actions)
			|| (preferred_action && (!(preferred_action & actions)
				|| (preferred_action & (preferred_action - 1))))) {
		wl_resource_post_error(resource, WL_DATA_OFFER_ERROR_INVALID_ACTION,
			"invalid preferred drag action");
		return;
	}
	if (!offer->drag) {
		wl_resource_post_error(resource, WL_DATA_OFFER_ERROR_INVALID_OFFER,
			"actions can only be set on drag offers");
		return;
	}
	if (!offer->source) {
		return;
	}
	offer->actions = actions;
	offer->preferred_action = preferred_action;
	update_drag_action(offer);
}

static const struct wl_data_offer_interface data_offer_impl = {
	.accept = data_offer_accept_req,
	.receive = data_offer_receive_req,
	.destroy = resource_destroy_req,
	.finish = data_offer_finish_req,
	.set_actions = data_offer_set_actions_req,
};

static void
data_device_manager_create_data_source_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id)
{
	struct wl_resource *source_resource = wl_resource_create(client,
		&wl_data_source_interface, wl_resource_get_version(resource), id);
	assert(source_resource);
	struct data_source *source = data_source_create(DATA_SOURCE_DATA_DEVICE,
		source_resource);
	wl_resource_set_implementation(source_resource, &data_source_impl,
		source, data_source_resource_destroy);
}

static void
data_device_manager_get_data_device_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id, struct wl_resource *seat)
{
	struct data_device *device = calloc(1, sizeof(*device));
	assert(device);
	device->resource = wl_resource_create(client,
		&wl_data_device_interface, wl_resource_get_version(resource), id);
	assert(device->resource);
	wl_resource_set_implementation(device->resource, &data_device_impl,
		device, data_device_resource_destroy);
	list_append(&core.seat.data_devices, &device->link);
	if (core.seat.selection_client == client) {
		send_selection(device);
	}
}

static const struct wl_data_device_manager_interface data_device_manager_impl = {
	.create_data_source = data_device_manager_create_data_source_req,
	.get_data_device = data_device_manager_get_data_device_req,
};

void
wayland_bind_data_device_manager(struct wl_client *client, void *data,
	uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&wl_data_device_manager_interface,
		MIN(version, DATA_DEVICE_MANAGER_VERSION), id);
	assert(resource);
	wl_resource_set_implementation(resource, &data_device_manager_impl, data, NULL);
}
