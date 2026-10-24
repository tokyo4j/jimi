#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "core.h"
#include "output.h"
#include "wlr-virtual-pointer-unstable-v1-protocol.h"

// Moves the pointer of the seat along with the physical pointer devices.
struct virtual_pointer {
	char *output_name; // NULL unless created for an output
	// Released when the virtual pointer is destroyed.
	uint32_t pressed[MAX_PRESSED];
	size_t nr_pressed;
	// The axes of the frame, scrolled by its end.
	enum wl_pointer_axis_source axis_source;
	uint32_t axis_time;
	struct pointer_axis_event axes[2];
	size_t nr_axes;
};

static void
virtual_pointer_motion_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t time, wl_fixed_t dx, wl_fixed_t dy)
{
	pointer_handle_motion(wl_fixed_to_double(dx), wl_fixed_to_double(dy),
		wl_fixed_to_double(dx), wl_fixed_to_double(dy), (uint64_t)time * 1000);
}

// Absolute motion spans the output of the virtual pointer while it is enabled,
// and else the bounding box of the outputs. Unlike absolute devices, the
// clients give points in the layout, regardless of the transforms.
static struct box
absolute_box(struct virtual_pointer *pointer)
{
	struct output *mapped = pointer->output_name
		? output_find(pointer->output_name) : NULL;
	if (mapped) {
		return mapped->box;
	}
	struct box box = {0};
	struct output *output;
	wl_list_for_each(output, &core.outputs, link) {
		if (output->enabled) {
			box_union(&box, &output->box);
		}
	}
	return box;
}

static void
virtual_pointer_motion_absolute_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t time, uint32_t x, uint32_t y,
	uint32_t x_extent, uint32_t y_extent)
{
	struct box box = absolute_box(wl_resource_get_user_data(resource));
	if (x_extent == 0 || y_extent == 0 || box_empty(&box)) {
		return;
	}
	pointer_handle_motion_absolute(box.x + (double)x / x_extent * box.width,
		box.y + (double)y / y_extent * box.height, time);
}

// Presses of pressed buttons and releases of released ones are ignored, so
// that the buttons left pressed are known.
static void
virtual_pointer_button_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t time, uint32_t button, uint32_t state)
{
	struct virtual_pointer *pointer = wl_resource_get_user_data(resource);
	bool pressed = state == WL_POINTER_BUTTON_STATE_PRESSED;
	size_t i = 0;
	while (i < pointer->nr_pressed && pointer->pressed[i] != button) {
		i++;
	}
	bool was_pressed = i < pointer->nr_pressed;
	if (pressed == was_pressed) {
		return;
	}
	if (pressed) {
		if (pointer->nr_pressed == ARRAY_LEN(pointer->pressed)) {
			return;
		}
		pointer->pressed[pointer->nr_pressed++] = button;
	} else {
		pointer->pressed[i] = pointer->pressed[--pointer->nr_pressed];
	}
	pointer_handle_button(time, button, pressed);
}

static bool
validate_axis(struct wl_resource *resource, uint32_t axis)
{
	if (axis > WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
		wl_resource_post_error(resource, ZWLR_VIRTUAL_POINTER_V1_ERROR_INVALID_AXIS,
			"invalid axis %u", axis);
		return false;
	}
	return true;
}

// The last value of an axis in the frame counts.
static void
set_axis(struct wl_resource *resource, uint32_t time, uint32_t axis,
	double value, int32_t value120)
{
	struct virtual_pointer *pointer = wl_resource_get_user_data(resource);
	size_t i = 0;
	while (i < pointer->nr_axes && pointer->axes[i].axis != axis) {
		i++;
	}
	pointer->nr_axes = MAX(pointer->nr_axes, i + 1);
	pointer->axes[i] = (struct pointer_axis_event){
		.axis = axis,
		.value = value,
		.value120 = value120,
	};
	pointer->axis_time = time;
}

static void
virtual_pointer_axis_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t time, uint32_t axis, wl_fixed_t value)
{
	if (validate_axis(resource, axis)) {
		set_axis(resource, time, axis, wl_fixed_to_double(value), 0);
	}
}

static void
virtual_pointer_axis_discrete_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t time, uint32_t axis,
	wl_fixed_t value, int32_t discrete)
{
	if (validate_axis(resource, axis)) {
		set_axis(resource, time, axis, wl_fixed_to_double(value), discrete * 120);
	}
}

static void
virtual_pointer_axis_stop_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t time, uint32_t axis)
{
	if (validate_axis(resource, axis)) {
		set_axis(resource, time, axis, 0.0, 0);
	}
}

static void
virtual_pointer_axis_source_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t axis_source)
{
	if (axis_source > WL_POINTER_AXIS_SOURCE_WHEEL_TILT) {
		wl_resource_post_error(resource,
			ZWLR_VIRTUAL_POINTER_V1_ERROR_INVALID_AXIS_SOURCE,
			"invalid axis source %u", axis_source);
		return;
	}
	struct virtual_pointer *pointer = wl_resource_get_user_data(resource);
	pointer->axis_source = axis_source;
}

// Motion and buttons take effect right away, but the axes of the frame scroll
// together, from a wheel unless the frame tells another source.
static void
virtual_pointer_frame_req(struct wl_client *client, struct wl_resource *resource)
{
	struct virtual_pointer *pointer = wl_resource_get_user_data(resource);
	if (pointer->nr_axes > 0) {
		pointer_handle_scroll(pointer->axis_time, pointer->axis_source,
			pointer->axes, pointer->nr_axes);
	}
	pointer->nr_axes = 0;
	pointer->axis_source = WL_POINTER_AXIS_SOURCE_WHEEL;
}

static const struct zwlr_virtual_pointer_v1_interface virtual_pointer_impl = {
	.motion = virtual_pointer_motion_req,
	.motion_absolute = virtual_pointer_motion_absolute_req,
	.button = virtual_pointer_button_req,
	.axis = virtual_pointer_axis_req,
	.frame = virtual_pointer_frame_req,
	.axis_source = virtual_pointer_axis_source_req,
	.axis_stop = virtual_pointer_axis_stop_req,
	.axis_discrete = virtual_pointer_axis_discrete_req,
	.destroy = resource_destroy_req,
};

// The buttons left pressed are released, so that they do not hold grabs.
static void
virtual_pointer_resource_destroy(struct wl_resource *resource)
{
	struct virtual_pointer *pointer = wl_resource_get_user_data(resource);
	uint32_t time = monotonic_msec();
	for (size_t i = 0; i < pointer->nr_pressed; i++) {
		pointer_handle_button(time, pointer->pressed[i], false);
	}
	free(pointer->output_name);
	free(pointer);
}

// The seat is ignored, as there is only one.
static void
manager_create_virtual_pointer_with_output_req(struct wl_client *client,
	struct wl_resource *resource, struct wl_resource *seat,
	struct wl_resource *output_resource, uint32_t id)
{
	struct wl_resource *virtual_pointer = wl_resource_create(client,
		&zwlr_virtual_pointer_v1_interface, wl_resource_get_version(resource), id);
	assert(virtual_pointer);
	struct virtual_pointer *pointer = calloc(1, sizeof(*pointer));
	assert(pointer);
	struct output *output = output_resource
		? wl_resource_get_user_data(output_resource) : NULL;
	if (output) {
		pointer->output_name = strdup(output->name);
		assert(pointer->output_name);
	}
	pointer->axis_source = WL_POINTER_AXIS_SOURCE_WHEEL;
	wl_resource_set_implementation(virtual_pointer, &virtual_pointer_impl,
		pointer, virtual_pointer_resource_destroy);
}

static void
manager_create_virtual_pointer_req(struct wl_client *client,
	struct wl_resource *resource, struct wl_resource *seat, uint32_t id)
{
	manager_create_virtual_pointer_with_output_req(client, resource, seat, NULL, id);
}

static const struct zwlr_virtual_pointer_manager_v1_interface manager_impl = {
	.create_virtual_pointer = manager_create_virtual_pointer_req,
	.create_virtual_pointer_with_output =
		manager_create_virtual_pointer_with_output_req,
	.destroy = resource_destroy_req,
};

void
wayland_bind_virtual_pointer_manager(struct wl_client *client, void *data,
	uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&zwlr_virtual_pointer_manager_v1_interface, version, id);
	assert(resource);
	wl_resource_set_implementation(resource, &manager_impl, NULL, NULL);
}
