#include <assert.h>
#include <libinput.h>
#include <libseat.h>
#include <libudev.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "config.h"
#include "core.h"
#include "output.h"

static int
input_open_restricted(const char *path, int flags, void *data)
{
	return session_open_input_device(path);
}

static void
input_close_restricted(int fd, void *data)
{
	session_close_input_device(fd);
}

static const struct libinput_interface input_impl = {
	.open_restricted = input_open_restricted,
	.close_restricted = input_close_restricted,
};

static enum input_type
get_device_type(struct libinput_device *device)
{
	if (libinput_device_config_tap_get_finger_count(device) > 0) {
		return INPUT_TYPE_TOUCHPAD;
	}
	if (libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_TOUCH)) {
		return INPUT_TYPE_TOUCHSCREEN;
	}
	if (libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_TABLET_TOOL)
			|| libinput_device_has_capability(device,
				LIBINPUT_DEVICE_CAP_TABLET_PAD)) {
		return INPUT_TYPE_TABLET;
	}
	if (libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_POINTER)) {
		return INPUT_TYPE_POINTER;
	}
	if (libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_KEYBOARD)) {
		return INPUT_TYPE_KEYBOARD;
	}
	return INPUT_TYPE_OTHER;
}

static void
add_tablet(struct libinput_device *device)
{
	struct udev_device *udev_device = libinput_device_get_udev_device(device);
	tablet_create(device, libinput_device_get_device_group(device),
		libinput_device_get_name(device),
		libinput_device_get_id_vendor(device),
		libinput_device_get_id_product(device),
		udev_device_get_devnode(udev_device));
	udev_device_unref(udev_device);
}

static struct tablet *
find_tablet(struct libinput_device *device)
{
	struct tablet *tablet;
	wl_list_for_each(tablet, &core.seat.tablets.tablets, link) {
		if (tablet->device == device) {
			return tablet;
		}
	}
	return NULL;
}

static void
add_tablet_pad(struct libinput_device *device)
{
	int nr_buttons = libinput_device_tablet_pad_get_num_buttons(device);
	int nr_rings = libinput_device_tablet_pad_get_num_rings(device);
	int nr_strips = libinput_device_tablet_pad_get_num_strips(device);
	int nr_dials = libinput_device_tablet_pad_get_num_dials(device);
	int nr_groups = libinput_device_tablet_pad_get_num_mode_groups(device);
	struct tablet_pad_group *groups = calloc((size_t)nr_groups, sizeof(*groups));
	assert(groups);
	for (int i = 0; i < nr_groups; i++) {
		struct libinput_tablet_pad_mode_group *mode_group =
			libinput_device_tablet_pad_get_mode_group(device, (unsigned)i);
		struct tablet_pad_group *group = &groups[i];
		for (int button = 0; button < MIN(nr_buttons, 64); button++) {
			if (libinput_tablet_pad_mode_group_has_button(mode_group,
					(unsigned)button)) {
				group->buttons |= 1ull << button;
			}
		}
		for (int ring = 0; ring < MIN(nr_rings, 32); ring++) {
			if (libinput_tablet_pad_mode_group_has_ring(mode_group,
					(unsigned)ring)) {
				group->rings |= 1u << ring;
			}
		}
		for (int strip = 0; strip < MIN(nr_strips, 32); strip++) {
			if (libinput_tablet_pad_mode_group_has_strip(mode_group,
					(unsigned)strip)) {
				group->strips |= 1u << strip;
			}
		}
		for (int dial = 0; dial < MIN(nr_dials, 32); dial++) {
			if (libinput_tablet_pad_mode_group_has_dial(mode_group,
					(unsigned)dial)) {
				group->dials |= 1u << dial;
			}
		}
		group->nr_modes = libinput_tablet_pad_mode_group_get_num_modes(mode_group);
		group->mode = libinput_tablet_pad_mode_group_get_mode(mode_group);
	}
	struct udev_device *udev_device = libinput_device_get_udev_device(device);
	tablet_pad_create(device, libinput_device_get_device_group(device),
		udev_device_get_devnode(udev_device), (uint32_t)nr_buttons, groups,
		(uint32_t)nr_groups);
	udev_device_unref(udev_device);
}

static struct tablet_pad *
find_tablet_pad(struct libinput_device *device)
{
	struct tablet_pad *pad;
	wl_list_for_each(pad, &core.seat.tablets.pads, link) {
		if (pad->device == device) {
			return pad;
		}
	}
	return NULL;
}

// Applies the settings of the first matching configuration. Those that the
// device does not support are ignored.
static void
configure_device(struct libinput_device *device)
{
	const char *name = libinput_device_get_name(device);
	fprintf(stderr, "Added input device %s\n", name);
	const struct input_config *input =
		config_find_input(name, get_device_type(device));
	// For the scroll factor and the output mapping
	libinput_device_set_user_data(device, (void *)input);
	if (!input) {
		return;
	}

	if (input->send_events >= 0) {
		libinput_device_config_send_events_set_mode(device,
			(uint32_t)input->send_events);
	}
	if (input->tap >= 0) {
		libinput_device_config_tap_set_enabled(device, input->tap
			? LIBINPUT_CONFIG_TAP_ENABLED : LIBINPUT_CONFIG_TAP_DISABLED);
	}
	if (input->tap_button_map >= 0) {
		libinput_device_config_tap_set_button_map(device, input->tap_button_map);
	}
	if (input->tap_drag >= 0) {
		libinput_device_config_tap_set_drag_enabled(device, input->tap_drag
			? LIBINPUT_CONFIG_DRAG_ENABLED : LIBINPUT_CONFIG_DRAG_DISABLED);
	}
	if (input->tap_drag_lock >= 0) {
		libinput_device_config_tap_set_drag_lock_enabled(device,
			input->tap_drag_lock);
	}
	if (input->three_finger_drag >= 0) {
		libinput_device_config_3fg_drag_set_enabled(device,
			input->three_finger_drag);
	}
	if (input->natural_scroll >= 0) {
		libinput_device_config_scroll_set_natural_scroll_enabled(device,
			input->natural_scroll);
	}
	if (input->left_handed >= 0) {
		libinput_device_config_left_handed_set(device, input->left_handed);
	}
	if (!isnan(input->accel_speed)) {
		libinput_device_config_accel_set_speed(device, input->accel_speed);
	}
	if (input->accel_profile >= 0) {
		libinput_device_config_accel_set_profile(device, input->accel_profile);
	}
	if (input->middle_emulation >= 0) {
		libinput_device_config_middle_emulation_set_enabled(device,
			input->middle_emulation
				? LIBINPUT_CONFIG_MIDDLE_EMULATION_ENABLED
				: LIBINPUT_CONFIG_MIDDLE_EMULATION_DISABLED);
	}
	if (input->disable_while_typing >= 0) {
		libinput_device_config_dwt_set_enabled(device,
			input->disable_while_typing
				? LIBINPUT_CONFIG_DWT_ENABLED : LIBINPUT_CONFIG_DWT_DISABLED);
	}
	if (input->click_method >= 0) {
		libinput_device_config_click_set_method(device, input->click_method);
	}
	if (input->scroll_method >= 0) {
		libinput_device_config_scroll_set_method(device, input->scroll_method);
	}
	if (input->scroll_button >= 0) {
		libinput_device_config_scroll_set_button(device,
			(uint32_t)input->scroll_button);
	}
}

static void
handle_pointer_scroll(struct libinput_event *event,
	enum wl_pointer_axis_source source)
{
	struct libinput_event_pointer *pointer = libinput_event_get_pointer_event(event);
	const struct input_config *input =
		libinput_device_get_user_data(libinput_event_get_device(event));
	double factor = input ? input->scroll_factor : 1.0;
	struct pointer_axis_event axes[2];
	size_t nr_axes = 0;
	for (uint32_t axis = LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL;
			axis <= LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL; axis++) {
		if (!libinput_event_pointer_has_axis(pointer, axis)) {
			continue;
		}
		struct pointer_axis_event *axis_event = &axes[nr_axes++];
		axis_event->axis = axis == LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL
			? WL_POINTER_AXIS_VERTICAL_SCROLL : WL_POINTER_AXIS_HORIZONTAL_SCROLL;
		axis_event->value = factor
			* libinput_event_pointer_get_scroll_value(pointer, axis);
		axis_event->value120 = source == WL_POINTER_AXIS_SOURCE_WHEEL
			? (int32_t)round(factor
				* libinput_event_pointer_get_scroll_value_v120(pointer, axis))
			: 0;
	}
	pointer_handle_scroll(libinput_event_pointer_get_time(pointer), source,
		axes, nr_axes);
}

// Absolute devices span the output they are mapped to while it is enabled,
// and else the output at the origin of the layout. They report points on the
// panel, which turns with the transform of the output: touchscreens and screen
// tablets are built into it, other tablets are turned along to match its
// aspect ratio, and the windows of virtual machines show the scanned out image.
static bool
layout_point(struct libinput_device *device, double normalized_x,
	double normalized_y, double *x, double *y)
{
	const struct input_config *input = libinput_device_get_user_data(device);
	struct output *output = input && input->map_to_output
		? output_find(input->map_to_output) : NULL;
	if (!output) {
		output = output_nearest(0, 0);
	}
	if (!output) {
		return false;
	}
	point_transform(&normalized_x, &normalized_y, output->transform, 1.0, 1.0);
	*x = output->box.x + normalized_x * output->box.width;
	*y = output->box.y + normalized_y * output->box.height;
	return true;
}

static void
handle_touch(struct libinput_event *event)
{
	struct libinput_event_touch *touch = libinput_event_get_touch_event(event);
	uint32_t time = libinput_event_touch_get_time(touch);
	int32_t id = libinput_event_touch_get_seat_slot(touch);
	enum libinput_event_type type = libinput_event_get_type(event);
	if (type == LIBINPUT_EVENT_TOUCH_UP) {
		touch_handle_up(id, time);
		return;
	}
	if (type == LIBINPUT_EVENT_TOUCH_CANCEL) {
		touch_handle_cancel(id, time);
		return;
	}
	double x, y;
	if (!layout_point(libinput_event_get_device(event),
			libinput_event_touch_get_x_transformed(touch, 1.0),
			libinput_event_touch_get_y_transformed(touch, 1.0), &x, &y)) {
		return;
	}
	if (type == LIBINPUT_EVENT_TOUCH_DOWN) {
		touch_handle_down(id, x, y, time);
	} else {
		touch_handle_motion(id, x, y, time);
	}
}

// Totems are not supported.
static struct tablet_tool *
get_tablet_tool(struct libinput_event_tablet_tool *event)
{
	struct libinput_tablet_tool *handle = libinput_event_tablet_tool_get_tool(event);
	struct tablet_tool *tool = libinput_tablet_tool_get_user_data(handle);
	if (tool) {
		return tool;
	}
	static const enum zwp_tablet_tool_v2_type types[] = {
		[LIBINPUT_TABLET_TOOL_TYPE_PEN] = ZWP_TABLET_TOOL_V2_TYPE_PEN,
		[LIBINPUT_TABLET_TOOL_TYPE_ERASER] = ZWP_TABLET_TOOL_V2_TYPE_ERASER,
		[LIBINPUT_TABLET_TOOL_TYPE_BRUSH] = ZWP_TABLET_TOOL_V2_TYPE_BRUSH,
		[LIBINPUT_TABLET_TOOL_TYPE_PENCIL] = ZWP_TABLET_TOOL_V2_TYPE_PENCIL,
		[LIBINPUT_TABLET_TOOL_TYPE_AIRBRUSH] = ZWP_TABLET_TOOL_V2_TYPE_AIRBRUSH,
		[LIBINPUT_TABLET_TOOL_TYPE_MOUSE] = ZWP_TABLET_TOOL_V2_TYPE_MOUSE,
		[LIBINPUT_TABLET_TOOL_TYPE_LENS] = ZWP_TABLET_TOOL_V2_TYPE_LENS,
	};
	enum libinput_tablet_tool_type type = libinput_tablet_tool_get_type(handle);
	if ((size_t)type >= ARRAY_LEN(types) || !types[type]) {
		return NULL;
	}
	uint32_t capabilities = 0;
	if (libinput_tablet_tool_has_tilt(handle)) {
		capabilities |= 1u << ZWP_TABLET_TOOL_V2_CAPABILITY_TILT;
	}
	if (libinput_tablet_tool_has_pressure(handle)) {
		capabilities |= 1u << ZWP_TABLET_TOOL_V2_CAPABILITY_PRESSURE;
	}
	if (libinput_tablet_tool_has_distance(handle)) {
		capabilities |= 1u << ZWP_TABLET_TOOL_V2_CAPABILITY_DISTANCE;
	}
	if (libinput_tablet_tool_has_rotation(handle)) {
		capabilities |= 1u << ZWP_TABLET_TOOL_V2_CAPABILITY_ROTATION;
	}
	if (libinput_tablet_tool_has_slider(handle)) {
		capabilities |= 1u << ZWP_TABLET_TOOL_V2_CAPABILITY_SLIDER;
	}
	if (libinput_tablet_tool_has_wheel(handle)) {
		capabilities |= 1u << ZWP_TABLET_TOOL_V2_CAPABILITY_WHEEL;
	}
	// libinput tells tools without serials apart only on their tablet.
	uint64_t serial = libinput_tablet_tool_get_serial(handle);
	struct tablet *owner = serial ? NULL : find_tablet(
		libinput_event_get_device(libinput_event_tablet_tool_get_base_event(event)));
	tool = tablet_tool_create(owner, types[type], serial,
		libinput_tablet_tool_get_tool_id(handle), capabilities);
	libinput_tablet_tool_set_user_data(handle, tool);
	return tool;
}

static bool
get_tablet_tool_axes(struct libinput_event_tablet_tool *event,
	struct tablet_tool_axes *axes)
{
	struct libinput_device *device = libinput_event_get_device(
		libinput_event_tablet_tool_get_base_event(event));
	if (!layout_point(device,
			libinput_event_tablet_tool_get_x_transformed(event, 1.0),
			libinput_event_tablet_tool_get_y_transformed(event, 1.0),
			&axes->x, &axes->y)) {
		return false;
	}
	axes->changed = 0;
	if (libinput_event_tablet_tool_pressure_has_changed(event)) {
		axes->changed |= 1u << ZWP_TABLET_TOOL_V2_CAPABILITY_PRESSURE;
		axes->pressure = libinput_event_tablet_tool_get_pressure(event);
	}
	if (libinput_event_tablet_tool_distance_has_changed(event)) {
		axes->changed |= 1u << ZWP_TABLET_TOOL_V2_CAPABILITY_DISTANCE;
		axes->distance = libinput_event_tablet_tool_get_distance(event);
	}
	if (libinput_event_tablet_tool_tilt_x_has_changed(event)
			|| libinput_event_tablet_tool_tilt_y_has_changed(event)) {
		axes->changed |= 1u << ZWP_TABLET_TOOL_V2_CAPABILITY_TILT;
		axes->tilt_x = libinput_event_tablet_tool_get_tilt_x(event);
		axes->tilt_y = libinput_event_tablet_tool_get_tilt_y(event);
	}
	if (libinput_event_tablet_tool_rotation_has_changed(event)) {
		axes->changed |= 1u << ZWP_TABLET_TOOL_V2_CAPABILITY_ROTATION;
		axes->rotation = libinput_event_tablet_tool_get_rotation(event);
	}
	if (libinput_event_tablet_tool_slider_has_changed(event)) {
		axes->changed |= 1u << ZWP_TABLET_TOOL_V2_CAPABILITY_SLIDER;
		axes->slider = libinput_event_tablet_tool_get_slider_position(event);
	}
	if (libinput_event_tablet_tool_wheel_has_changed(event)) {
		axes->changed |= 1u << ZWP_TABLET_TOOL_V2_CAPABILITY_WHEEL;
		axes->wheel = libinput_event_tablet_tool_get_wheel_delta(event);
		axes->wheel_clicks =
			libinput_event_tablet_tool_get_wheel_delta_discrete(event);
	}
	return true;
}

static void
handle_tablet_tool(struct libinput_event *event)
{
	struct libinput_event_tablet_tool *tool_event =
		libinput_event_get_tablet_tool_event(event);
	struct tablet_tool *tool = get_tablet_tool(tool_event);
	struct tablet_tool_axes axes;
	if (!tool || !get_tablet_tool_axes(tool_event, &axes)) {
		return;
	}
	uint32_t time = libinput_event_tablet_tool_get_time(tool_event);
	switch (libinput_event_get_type(event)) {
	case LIBINPUT_EVENT_TABLET_TOOL_PROXIMITY:
		if (libinput_event_tablet_tool_get_proximity_state(tool_event)
				== LIBINPUT_TABLET_TOOL_PROXIMITY_STATE_IN) {
			tablet_tool_handle_proximity_in(tool,
				find_tablet(libinput_event_get_device(event)), &axes, time);
		} else {
			tablet_tool_handle_proximity_out(tool, time);
		}
		return;
	case LIBINPUT_EVENT_TABLET_TOOL_AXIS:
		tablet_tool_handle_axis(tool, &axes, time);
		return;
	case LIBINPUT_EVENT_TABLET_TOOL_TIP:
		tablet_tool_handle_tip(tool, &axes,
			libinput_event_tablet_tool_get_tip_state(tool_event)
				== LIBINPUT_TABLET_TOOL_TIP_DOWN, time);
		return;
	case LIBINPUT_EVENT_TABLET_TOOL_BUTTON:
		tablet_tool_handle_button(tool,
			libinput_event_tablet_tool_get_button(tool_event),
			libinput_event_tablet_tool_get_button_state(tool_event)
				== LIBINPUT_BUTTON_STATE_PRESSED, time);
		return;
	default:
		break;
	}
	assert(!"not a tablet tool event");
}

// The mode of the group of the pad feature switches before its event.
static void
handle_tablet_pad(struct libinput_event *event)
{
	struct libinput_event_tablet_pad *pad_event =
		libinput_event_get_tablet_pad_event(event);
	struct tablet_pad *pad = find_tablet_pad(libinput_event_get_device(event));
	uint32_t time = libinput_event_tablet_pad_get_time(pad_event);
	tablet_pad_handle_mode(pad, libinput_tablet_pad_mode_group_get_index(
			libinput_event_tablet_pad_get_mode_group(pad_event)),
		libinput_event_tablet_pad_get_mode(pad_event), time);
	switch (libinput_event_get_type(event)) {
	case LIBINPUT_EVENT_TABLET_PAD_BUTTON:
		tablet_pad_handle_button(pad,
			libinput_event_tablet_pad_get_button_number(pad_event),
			libinput_event_tablet_pad_get_button_state(pad_event)
				== LIBINPUT_BUTTON_STATE_PRESSED, time);
		return;
	case LIBINPUT_EVENT_TABLET_PAD_RING:
		tablet_pad_handle_ring(pad,
			libinput_event_tablet_pad_get_ring_number(pad_event),
			libinput_event_tablet_pad_get_ring_position(pad_event),
			libinput_event_tablet_pad_get_ring_source(pad_event)
				== LIBINPUT_TABLET_PAD_RING_SOURCE_FINGER, time);
		return;
	case LIBINPUT_EVENT_TABLET_PAD_STRIP:
		tablet_pad_handle_strip(pad,
			libinput_event_tablet_pad_get_strip_number(pad_event),
			libinput_event_tablet_pad_get_strip_position(pad_event),
			libinput_event_tablet_pad_get_strip_source(pad_event)
				== LIBINPUT_TABLET_PAD_STRIP_SOURCE_FINGER, time);
		return;
	case LIBINPUT_EVENT_TABLET_PAD_DIAL:
		tablet_pad_handle_dial(pad,
			libinput_event_tablet_pad_get_dial_number(pad_event),
			libinput_event_tablet_pad_get_dial_delta_v120(pad_event), time);
		return;
	default:
		break;
	}
	assert(!"not a tablet pad event");
}

static void
handle_gesture_begin(struct libinput_event *event, enum gesture_type type)
{
	struct libinput_event_gesture *gesture = libinput_event_get_gesture_event(event);
	pointer_handle_gesture_begin(type, libinput_event_gesture_get_time(gesture),
		(uint32_t)libinput_event_gesture_get_finger_count(gesture));
}

static void
handle_gesture_end(struct libinput_event *event, enum gesture_type type)
{
	struct libinput_event_gesture *gesture = libinput_event_get_gesture_event(event);
	pointer_handle_gesture_end(type, libinput_event_gesture_get_time(gesture),
		libinput_event_gesture_get_cancelled(gesture));
}

static int
handle_input_events(int fd, uint32_t mask, void *data)
{
	assert(libinput_dispatch(core.input.libinput) == 0);
	// Events other than adding and removing devices are input of the user.
	bool activity = false;
	struct libinput_event *event;
	while ((event = libinput_get_event(core.input.libinput))) {
		enum libinput_event_type type = libinput_event_get_type(event);
		activity = activity || (type != LIBINPUT_EVENT_DEVICE_ADDED
			&& type != LIBINPUT_EVENT_DEVICE_REMOVED);
		switch (type) {
		case LIBINPUT_EVENT_DEVICE_ADDED: {
			struct libinput_device *device = libinput_event_get_device(event);
			configure_device(device);
			if (libinput_device_has_capability(device,
					LIBINPUT_DEVICE_CAP_TABLET_TOOL)) {
				add_tablet(device);
			}
			if (libinput_device_has_capability(device,
					LIBINPUT_DEVICE_CAP_TABLET_PAD)) {
				add_tablet_pad(device);
			}
			break;
		}
		case LIBINPUT_EVENT_DEVICE_REMOVED: {
			struct libinput_device *device = libinput_event_get_device(event);
			struct tablet *tablet = find_tablet(device);
			if (tablet) {
				tablet_destroy(tablet);
			}
			struct tablet_pad *pad = find_tablet_pad(device);
			if (pad) {
				tablet_pad_destroy(pad);
			}
			break;
		}
		case LIBINPUT_EVENT_KEYBOARD_KEY: {
			struct libinput_event_keyboard *keyboard =
				libinput_event_get_keyboard_event(event);
			uint32_t key = libinput_event_keyboard_get_key(keyboard);
			bool pressed = libinput_event_keyboard_get_key_state(keyboard)
				== LIBINPUT_KEY_STATE_PRESSED;
			// The keyboards share a state, which only the first press
			// and the last release of a key among them change.
			uint32_t count = libinput_event_keyboard_get_seat_key_count(keyboard);
			if (count != (pressed ? 1 : 0)) {
				break;
			}
			keyboard_handle_key(libinput_event_keyboard_get_time(keyboard),
				key, pressed);
			break;
		}
		case LIBINPUT_EVENT_POINTER_MOTION: {
			struct libinput_event_pointer *pointer =
				libinput_event_get_pointer_event(event);
			pointer_handle_motion(libinput_event_pointer_get_dx(pointer),
				libinput_event_pointer_get_dy(pointer),
				libinput_event_pointer_get_dx_unaccelerated(pointer),
				libinput_event_pointer_get_dy_unaccelerated(pointer),
				libinput_event_pointer_get_time_usec(pointer));
			break;
		}
		case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE: {
			struct libinput_event_pointer *pointer =
				libinput_event_get_pointer_event(event);
			double x, y;
			if (layout_point(libinput_event_get_device(event),
					libinput_event_pointer_get_absolute_x_transformed(pointer, 1.0),
					libinput_event_pointer_get_absolute_y_transformed(pointer, 1.0),
					&x, &y)) {
				pointer_handle_motion_absolute(x, y,
					libinput_event_pointer_get_time(pointer));
			}
			break;
		}
		case LIBINPUT_EVENT_POINTER_BUTTON: {
			struct libinput_event_pointer *pointer =
				libinput_event_get_pointer_event(event);
			bool pressed = libinput_event_pointer_get_button_state(pointer)
				== LIBINPUT_BUTTON_STATE_PRESSED;
			pointer_handle_button(libinput_event_pointer_get_time(pointer),
				libinput_event_pointer_get_button(pointer), pressed);
			break;
		}
		case LIBINPUT_EVENT_POINTER_AXIS:
			// The modern scroll events below are also accompanied by this
			// legacy event. Forwarding both would duplicate scroll input.
			break;
		case LIBINPUT_EVENT_POINTER_SCROLL_WHEEL:
			handle_pointer_scroll(event, WL_POINTER_AXIS_SOURCE_WHEEL);
			break;
		case LIBINPUT_EVENT_POINTER_SCROLL_FINGER:
			handle_pointer_scroll(event, WL_POINTER_AXIS_SOURCE_FINGER);
			break;
		case LIBINPUT_EVENT_POINTER_SCROLL_CONTINUOUS:
			handle_pointer_scroll(event, WL_POINTER_AXIS_SOURCE_CONTINUOUS);
			break;
		case LIBINPUT_EVENT_GESTURE_SWIPE_BEGIN:
			handle_gesture_begin(event, GESTURE_SWIPE);
			break;
		case LIBINPUT_EVENT_GESTURE_SWIPE_UPDATE: {
			struct libinput_event_gesture *gesture =
				libinput_event_get_gesture_event(event);
			pointer_handle_swipe_update(libinput_event_gesture_get_time(gesture),
				libinput_event_gesture_get_dx(gesture),
				libinput_event_gesture_get_dy(gesture));
			break;
		}
		case LIBINPUT_EVENT_GESTURE_SWIPE_END:
			handle_gesture_end(event, GESTURE_SWIPE);
			break;
		case LIBINPUT_EVENT_GESTURE_PINCH_BEGIN:
			handle_gesture_begin(event, GESTURE_PINCH);
			break;
		case LIBINPUT_EVENT_GESTURE_PINCH_UPDATE: {
			struct libinput_event_gesture *gesture =
				libinput_event_get_gesture_event(event);
			pointer_handle_pinch_update(libinput_event_gesture_get_time(gesture),
				libinput_event_gesture_get_dx(gesture),
				libinput_event_gesture_get_dy(gesture),
				libinput_event_gesture_get_scale(gesture),
				libinput_event_gesture_get_angle_delta(gesture));
			break;
		}
		case LIBINPUT_EVENT_GESTURE_PINCH_END:
			handle_gesture_end(event, GESTURE_PINCH);
			break;
		case LIBINPUT_EVENT_GESTURE_HOLD_BEGIN:
			handle_gesture_begin(event, GESTURE_HOLD);
			break;
		case LIBINPUT_EVENT_GESTURE_HOLD_END:
			handle_gesture_end(event, GESTURE_HOLD);
			break;
		case LIBINPUT_EVENT_TOUCH_DOWN:
		case LIBINPUT_EVENT_TOUCH_MOTION:
		case LIBINPUT_EVENT_TOUCH_UP:
		case LIBINPUT_EVENT_TOUCH_CANCEL:
			handle_touch(event);
			break;
		case LIBINPUT_EVENT_TOUCH_FRAME:
			touch_handle_frame();
			break;
		case LIBINPUT_EVENT_TABLET_TOOL_PROXIMITY:
		case LIBINPUT_EVENT_TABLET_TOOL_AXIS:
		case LIBINPUT_EVENT_TABLET_TOOL_TIP:
		case LIBINPUT_EVENT_TABLET_TOOL_BUTTON:
			handle_tablet_tool(event);
			break;
		case LIBINPUT_EVENT_TABLET_PAD_BUTTON:
		case LIBINPUT_EVENT_TABLET_PAD_RING:
		case LIBINPUT_EVENT_TABLET_PAD_STRIP:
		case LIBINPUT_EVENT_TABLET_PAD_DIAL:
			handle_tablet_pad(event);
			break;
		default:
			break;
		}
		libinput_event_destroy(event);
	}
	if (activity) {
		idle_notify_activity();
	}
	return 0;
}

void
input_init(void)
{
	core.input.libinput = libinput_udev_create_context(&input_impl, NULL,
		core.session.udev);
	assert(core.input.libinput);
	assert(libinput_udev_assign_seat(core.input.libinput,
		libseat_seat_name(core.session.handle)) == 0);
	core.input.source = wl_event_loop_add_fd(core.wl_loop,
		libinput_get_fd(core.input.libinput), WL_EVENT_READABLE,
		handle_input_events, NULL);
	assert(core.input.source);
}

// Closing the devices releases their pressed keys and buttons.
void
input_suspend(void)
{
	libinput_suspend(core.input.libinput);
	handle_input_events(-1, 0, NULL);
}

// The devices are added again.
void
input_resume(void)
{
	assert(libinput_resume(core.input.libinput) == 0);
}

void
input_fini(void)
{
	wl_event_source_remove(core.input.source);
	libinput_unref(core.input.libinput);
}
