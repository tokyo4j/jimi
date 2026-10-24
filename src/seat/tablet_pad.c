#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "core.h"
#include "surface.h"

// Tablet pads go to the keyboard focus if its client takes the input of their
// tablet. Libinput switches their modes by the mode toggle buttons. The
// descriptions of their actions that clients give as feedback are ignored.

enum pad_part_type {
	PAD_PART_GROUP,
	PAD_PART_RING,
	PAD_PART_STRIP,
	PAD_PART_DIAL,
};

// A zwp_tablet_pad_group_v2, zwp_tablet_pad_ring_v2, zwp_tablet_pad_strip_v2 or
// zwp_tablet_pad_dial_v2
struct pad_part {
	struct wl_resource *resource;
	struct wl_list link; // tablet_pad_resource.parts
	enum pad_part_type type;
	uint32_t index; // Of the group, ring, strip or dial
};

struct tablet_pad_resource {
	struct wl_resource *resource;
	struct wl_list link; // tablet_pad.resources
	struct tablet_pad *pad; // NULL once the pad is removed
	// pad_part.link, which get no events once the zwp_tablet_pad_v2 is
	// destroyed.
	struct wl_list parts;
};

static void
pad_set_feedback_req(struct wl_client *client, struct wl_resource *resource,
	uint32_t button, const char *description, uint32_t serial)
{
}

static const struct zwp_tablet_pad_v2_interface pad_impl = {
	.set_feedback = pad_set_feedback_req,
	.destroy = resource_destroy_req,
};

static void
pad_resource_destroy(struct wl_resource *resource)
{
	struct tablet_pad_resource *pad_resource = wl_resource_get_user_data(resource);
	struct pad_part *part, *tmp;
	wl_list_for_each_safe(part, tmp, &pad_resource->parts, link) {
		wl_list_remove(&part->link);
		wl_list_init(&part->link);
	}
	wl_list_remove(&pad_resource->link);
	free(pad_resource);
}

static void
part_set_feedback_req(struct wl_client *client, struct wl_resource *resource,
	const char *description, uint32_t serial)
{
}

static const struct zwp_tablet_pad_group_v2_interface group_impl = {
	.destroy = resource_destroy_req,
};

static const struct zwp_tablet_pad_ring_v2_interface ring_impl = {
	.set_feedback = part_set_feedback_req,
	.destroy = resource_destroy_req,
};

static const struct zwp_tablet_pad_strip_v2_interface strip_impl = {
	.set_feedback = part_set_feedback_req,
	.destroy = resource_destroy_req,
};

static const struct zwp_tablet_pad_dial_v2_interface dial_impl = {
	.set_feedback = part_set_feedback_req,
	.destroy = resource_destroy_req,
};

static void
part_resource_destroy(struct wl_resource *resource)
{
	struct pad_part *part = wl_resource_get_user_data(resource);
	wl_list_remove(&part->link);
	free(part);
}

static struct wl_resource *
create_part(struct tablet_pad_resource *pad_resource, enum pad_part_type type,
	uint32_t index)
{
	static const struct {
		const struct wl_interface *interface;
		const void *impl;
	} types[] = {
		[PAD_PART_GROUP] = {&zwp_tablet_pad_group_v2_interface, &group_impl},
		[PAD_PART_RING] = {&zwp_tablet_pad_ring_v2_interface, &ring_impl},
		[PAD_PART_STRIP] = {&zwp_tablet_pad_strip_v2_interface, &strip_impl},
		[PAD_PART_DIAL] = {&zwp_tablet_pad_dial_v2_interface, &dial_impl},
	};
	struct pad_part *part = calloc(1, sizeof(*part));
	assert(part);
	part->resource = wl_resource_create(
		wl_resource_get_client(pad_resource->resource), types[type].interface,
		wl_resource_get_version(pad_resource->resource), 0);
	assert(part->resource);
	wl_resource_set_implementation(part->resource, types[type].impl, part,
		part_resource_destroy);
	part->type = type;
	part->index = index;
	list_append(&pad_resource->parts, &part->link);
	return part->resource;
}

// The part of the pad, or NULL if the client destroyed it.
static struct wl_resource *
find_part(struct tablet_pad_resource *pad_resource, enum pad_part_type type,
	uint32_t index)
{
	struct pad_part *part;
	wl_list_for_each(part, &pad_resource->parts, link) {
		if (part->type == type && part->index == index) {
			return part->resource;
		}
	}
	return NULL;
}

static void
send_group(struct tablet_pad_resource *pad_resource, uint32_t index)
{
	struct tablet_pad_group *group = &pad_resource->pad->groups[index];
	struct wl_resource *resource = create_part(pad_resource, PAD_PART_GROUP, index);
	zwp_tablet_pad_v2_send_group(pad_resource->resource, resource);

	struct wl_array buttons;
	wl_array_init(&buttons);
	for (uint32_t button = 0; button < 64; button++) {
		if (group->buttons & (1ull << button)) {
			uint32_t *entry = wl_array_add(&buttons, sizeof(*entry));
			assert(entry);
			*entry = button;
		}
	}
	zwp_tablet_pad_group_v2_send_buttons(resource, &buttons);
	wl_array_release(&buttons);

	for (uint32_t i = 0; i < 32; i++) {
		if (group->rings & (1u << i)) {
			zwp_tablet_pad_group_v2_send_ring(resource,
				create_part(pad_resource, PAD_PART_RING, i));
		}
		if (group->strips & (1u << i)) {
			zwp_tablet_pad_group_v2_send_strip(resource,
				create_part(pad_resource, PAD_PART_STRIP, i));
		}
		if ((group->dials & (1u << i)) && wl_resource_get_version(resource)
				>= ZWP_TABLET_PAD_GROUP_V2_DIAL_SINCE_VERSION) {
			zwp_tablet_pad_group_v2_send_dial(resource,
				create_part(pad_resource, PAD_PART_DIAL, i));
		}
	}
	if (group->nr_modes > 1) {
		zwp_tablet_pad_group_v2_send_modes(resource, group->nr_modes);
	}
	zwp_tablet_pad_group_v2_send_done(resource);
}

static struct tablet *
pad_tablet(struct tablet_pad *pad)
{
	struct tablet *tablet;
	wl_list_for_each(tablet, &core.seat.tablets.tablets, link) {
		if (tablet->group == pad->group) {
			return tablet;
		}
	}
	return NULL;
}

// Entering tells the modes of the groups. A client that destroyed its
// zwp_tablet_v2 for the tablet of the pad is not entered.
static void
send_enter(struct tablet_pad_resource *pad_resource, uint32_t time)
{
	struct tablet_pad *pad = pad_resource->pad;
	struct wl_resource *tablet = tablet_resource_for(pad_tablet(pad),
		wl_resource_get_client(pad_resource->resource));
	if (!tablet) {
		return;
	}
	zwp_tablet_pad_v2_send_enter(pad_resource->resource, ++core.serial,
		tablet, pad->focus->resource);
	struct pad_part *part;
	wl_list_for_each(part, &pad_resource->parts, link) {
		if (part->type == PAD_PART_GROUP) {
			zwp_tablet_pad_group_v2_send_mode_switch(part->resource, time,
				++core.serial, pad->groups[part->index].mode);
		}
	}
}

static bool
takes_pad_input(struct tablet_pad *pad, struct surface *surface)
{
	struct tablet *tablet = pad_tablet(pad);
	struct wl_client *client = wl_resource_get_client(surface->resource);
	if (!tablet || !tablet_resource_for(tablet, client)) {
		return false;
	}
	struct tablet_pad_resource *pad_resource;
	wl_list_for_each(pad_resource, &pad->resources, link) {
		if (wl_resource_get_client(pad_resource->resource) == client) {
			return true;
		}
	}
	return false;
}

static void
update_focus(struct tablet_pad *pad)
{
	struct surface *surface = core.seat.keyboard.focus;
	if (surface && !takes_pad_input(pad, surface)) {
		surface = NULL;
	}
	if (pad->focus == surface) {
		return;
	}
	struct tablet_pad_resource *pad_resource;
	if (pad->focus) {
		uint32_t serial = ++core.serial;
		wl_list_for_each(pad_resource, &pad->resources, link) {
			if (resource_matches_surface(pad_resource->resource, pad->focus)) {
				zwp_tablet_pad_v2_send_leave(pad_resource->resource, serial,
					pad->focus->resource);
			}
		}
	}
	pad->focus = surface;
	if (!surface) {
		return;
	}
	uint32_t time = monotonic_msec();
	wl_list_for_each(pad_resource, &pad->resources, link) {
		if (resource_matches_surface(pad_resource->resource, surface)) {
			send_enter(pad_resource, time);
		}
	}
}

// The keyboard focus or the tablets have changed.
void
tablet_pads_update_focus(void)
{
	struct tablet_pad *pad;
	wl_list_for_each(pad, &core.seat.tablets.pads, link) {
		update_focus(pad);
	}
}

void
tablet_pad_send_added(struct wl_resource *seat, struct tablet_pad *pad)
{
	struct tablet_pad_resource *pad_resource = calloc(1, sizeof(*pad_resource));
	assert(pad_resource);
	pad_resource->resource = wl_resource_create(wl_resource_get_client(seat),
		&zwp_tablet_pad_v2_interface, wl_resource_get_version(seat), 0);
	assert(pad_resource->resource);
	wl_resource_set_implementation(pad_resource->resource, &pad_impl,
		pad_resource, pad_resource_destroy);
	pad_resource->pad = pad;
	wl_list_init(&pad_resource->parts);
	list_append(&pad->resources, &pad_resource->link);

	zwp_tablet_seat_v2_send_pad_added(seat, pad_resource->resource);
	if (pad->path) {
		zwp_tablet_pad_v2_send_path(pad_resource->resource, pad->path);
	}
	if (pad->nr_buttons > 0) {
		zwp_tablet_pad_v2_send_buttons(pad_resource->resource, pad->nr_buttons);
	}
	for (uint32_t i = 0; i < pad->nr_groups; i++) {
		send_group(pad_resource, i);
	}
	zwp_tablet_pad_v2_send_done(pad_resource->resource);

	if (resource_matches_surface(pad_resource->resource, pad->focus)) {
		send_enter(pad_resource, monotonic_msec());
	} else {
		update_focus(pad);
	}
}

// Takes the groups.
struct tablet_pad *
tablet_pad_create(struct libinput_device *device,
	struct libinput_device_group *group, const char *path,
	uint32_t nr_buttons, struct tablet_pad_group *groups, uint32_t nr_groups)
{
	struct tablet_pad *pad = calloc(1, sizeof(*pad));
	assert(pad);
	pad->device = device;
	pad->group = group;
	wl_list_init(&pad->resources);
	if (path) {
		pad->path = strdup(path);
		assert(pad->path);
	}
	pad->nr_buttons = nr_buttons;
	pad->groups = groups;
	pad->nr_groups = nr_groups;
	list_append(&core.seat.tablets.pads, &pad->link);
	struct wl_resource *seat;
	wl_resource_for_each(seat, &core.seat.tablets.seats) {
		tablet_pad_send_added(seat, pad);
	}
	return pad;
}

void
tablet_pad_destroy(struct tablet_pad *pad)
{
	wl_list_remove(&pad->link);
	struct tablet_pad_resource *pad_resource, *tmp;
	wl_list_for_each_safe(pad_resource, tmp, &pad->resources, link) {
		if (resource_matches_surface(pad_resource->resource, pad->focus)) {
			zwp_tablet_pad_v2_send_leave(pad_resource->resource,
				++core.serial, pad->focus->resource);
		}
		zwp_tablet_pad_v2_send_removed(pad_resource->resource);
		pad_resource->pad = NULL;
		wl_list_remove(&pad_resource->link);
		wl_list_init(&pad_resource->link);
	}
	free(pad->path);
	free(pad->groups);
	free(pad);
}

void
tablet_pad_handle_mode(struct tablet_pad *pad, uint32_t group, uint32_t mode,
	uint32_t time)
{
	assert(group < pad->nr_groups);
	if (pad->groups[group].mode == mode) {
		return;
	}
	pad->groups[group].mode = mode;
	struct tablet_pad_resource *pad_resource;
	wl_list_for_each(pad_resource, &pad->resources, link) {
		if (!resource_matches_surface(pad_resource->resource, pad->focus)) {
			continue;
		}
		struct wl_resource *resource =
			find_part(pad_resource, PAD_PART_GROUP, group);
		if (resource) {
			zwp_tablet_pad_group_v2_send_mode_switch(resource, time,
				++core.serial, mode);
		}
	}
}

void
tablet_pad_handle_button(struct tablet_pad *pad, uint32_t button,
	bool pressed, uint32_t time)
{
	uint32_t state = pressed ? ZWP_TABLET_PAD_V2_BUTTON_STATE_PRESSED
		: ZWP_TABLET_PAD_V2_BUTTON_STATE_RELEASED;
	struct tablet_pad_resource *pad_resource;
	wl_list_for_each(pad_resource, &pad->resources, link) {
		if (resource_matches_surface(pad_resource->resource, pad->focus)) {
			zwp_tablet_pad_v2_send_button(pad_resource->resource, time,
				button, state);
		}
	}
}

// At a negative angle, the finger has left the ring.
void
tablet_pad_handle_ring(struct tablet_pad *pad, uint32_t ring, double degrees,
	bool finger, uint32_t time)
{
	struct tablet_pad_resource *pad_resource;
	wl_list_for_each(pad_resource, &pad->resources, link) {
		if (!resource_matches_surface(pad_resource->resource, pad->focus)) {
			continue;
		}
		struct wl_resource *resource = find_part(pad_resource, PAD_PART_RING, ring);
		if (!resource) {
			continue;
		}
		if (finger) {
			zwp_tablet_pad_ring_v2_send_source(resource,
				ZWP_TABLET_PAD_RING_V2_SOURCE_FINGER);
		}
		if (degrees < 0) {
			zwp_tablet_pad_ring_v2_send_stop(resource);
		} else {
			zwp_tablet_pad_ring_v2_send_angle(resource,
				wl_fixed_from_double(degrees));
		}
		zwp_tablet_pad_ring_v2_send_frame(resource, time);
	}
}

// The position is from 0 to 1. At a negative position, the finger has left
// the strip.
void
tablet_pad_handle_strip(struct tablet_pad *pad, uint32_t strip,
	double position, bool finger, uint32_t time)
{
	struct tablet_pad_resource *pad_resource;
	wl_list_for_each(pad_resource, &pad->resources, link) {
		if (!resource_matches_surface(pad_resource->resource, pad->focus)) {
			continue;
		}
		struct wl_resource *resource =
			find_part(pad_resource, PAD_PART_STRIP, strip);
		if (!resource) {
			continue;
		}
		if (finger) {
			zwp_tablet_pad_strip_v2_send_source(resource,
				ZWP_TABLET_PAD_STRIP_V2_SOURCE_FINGER);
		}
		if (position < 0) {
			zwp_tablet_pad_strip_v2_send_stop(resource);
		} else {
			zwp_tablet_pad_strip_v2_send_position(resource,
				(uint32_t)round(position * 65535));
		}
		zwp_tablet_pad_strip_v2_send_frame(resource, time);
	}
}

void
tablet_pad_handle_dial(struct tablet_pad *pad, uint32_t dial, double value120,
	uint32_t time)
{
	struct tablet_pad_resource *pad_resource;
	wl_list_for_each(pad_resource, &pad->resources, link) {
		if (!resource_matches_surface(pad_resource->resource, pad->focus)) {
			continue;
		}
		struct wl_resource *resource = find_part(pad_resource, PAD_PART_DIAL, dial);
		if (!resource) {
			continue;
		}
		zwp_tablet_pad_dial_v2_send_delta(resource, (int32_t)round(value120));
		zwp_tablet_pad_dial_v2_send_frame(resource, time);
	}
}
