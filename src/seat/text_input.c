#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "core.h"
#include "surface.h"
#include "text-input-unstable-v3-protocol.h"

// Enable, disable, enter and leave invalidate the state.
static void
text_input_state_reset(struct text_input_state *state, bool enabled)
{
	free(state->surrounding_text);
	*state = (struct text_input_state){.enabled = enabled};
}

static void
text_input_set_focus(struct text_input *text_input, struct surface *surface)
{
	if (text_input->focus == surface) {
		return;
	}
	if (text_input->focus) {
		zwp_text_input_v3_send_leave(text_input->resource,
			text_input->focus->resource);
	}
	text_input->focus = surface;
	text_input_state_reset(&text_input->pending, false);
	text_input_state_reset(&text_input->current, false);
	text_input->enable_requested = false;
	if (surface) {
		zwp_text_input_v3_send_enter(text_input->resource, surface->resource);
	}
}

// The text inputs follow the keyboard focus. The input method commits its
// preedit string to the text input losing the focus.
void
text_inputs_focus(struct surface *surface)
{
	struct input_method *input_method = &core.seat.input_method;
	if (input_method->text_input && input_method->text_input->focus != surface) {
		input_method_commit_preedit();
		input_method_deactivate();
	}
	struct text_input *text_input;
	wl_list_for_each(text_input, &core.seat.text_inputs, link) {
		text_input_set_focus(text_input,
			resource_matches_surface(text_input->resource, surface)
				? surface : NULL);
	}
}

static void
text_input_resource_destroy(struct wl_resource *resource)
{
	struct text_input *text_input = wl_resource_get_user_data(resource);
	if (core.seat.input_method.text_input == text_input) {
		input_method_deactivate();
	}
	wl_list_remove(&text_input->link);
	text_input_state_reset(&text_input->pending, false);
	text_input_state_reset(&text_input->current, false);
	free(text_input);
}

static void
text_input_enable_req(struct wl_client *client, struct wl_resource *resource)
{
	struct text_input *text_input = wl_resource_get_user_data(resource);
	text_input_state_reset(&text_input->pending, true);
	text_input->enable_requested = true;
}

static void
text_input_disable_req(struct wl_client *client, struct wl_resource *resource)
{
	struct text_input *text_input = wl_resource_get_user_data(resource);
	text_input_state_reset(&text_input->pending, false);
	text_input->enable_requested = false;
}

static void
text_input_set_surrounding_text_req(struct wl_client *client,
	struct wl_resource *resource, const char *text, int32_t cursor, int32_t anchor)
{
	struct text_input *text_input = wl_resource_get_user_data(resource);
	free(text_input->pending.surrounding_text);
	text_input->pending.surrounding_text = strdup(text);
	assert(text_input->pending.surrounding_text);
	text_input->pending.surrounding_cursor = (uint32_t)cursor;
	text_input->pending.surrounding_anchor = (uint32_t)anchor;
}

static void
text_input_set_text_change_cause_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t cause)
{
	// input-method-v1 has no way to tell the input method.
}

static void
text_input_set_content_type_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t hint, uint32_t purpose)
{
	struct text_input *text_input = wl_resource_get_user_data(resource);
	text_input->pending.content_hint = hint;
	text_input->pending.content_purpose = purpose;
}

static void
text_input_set_cursor_rectangle_req(struct wl_client *client,
	struct wl_resource *resource, int32_t x, int32_t y, int32_t width,
	int32_t height)
{
	struct text_input *text_input = wl_resource_get_user_data(resource);
	text_input->pending.cursor_rectangle = (struct box){
		.x = x,
		.y = y,
		.width = width,
		.height = height,
	};
}

// The pending state stays as it is after the commit. Only one text input is
// enabled at a time, and the others enabled meanwhile are ignored.
static void
text_input_commit_req(struct wl_client *client, struct wl_resource *resource)
{
	struct text_input *text_input = wl_resource_get_user_data(resource);
	text_input->serial++;
	bool restart = text_input->enable_requested;
	text_input->enable_requested = false;
	// Requests are ignored between leave and enter.
	if (!text_input->focus) {
		return;
	}
	free(text_input->current.surrounding_text);
	text_input->current = text_input->pending;
	if (text_input->pending.surrounding_text) {
		text_input->current.surrounding_text =
			strdup(text_input->pending.surrounding_text);
		assert(text_input->current.surrounding_text);
	}

	struct input_method *input_method = &core.seat.input_method;
	if (input_method->text_input == text_input) {
		if (!text_input->current.enabled) {
			input_method_deactivate();
		} else if (restart) {
			input_method_reset();
		} else {
			input_method_update();
		}
	} else if (text_input->current.enabled && !input_method->text_input) {
		input_method_activate(text_input);
	}
}

static const struct zwp_text_input_v3_interface text_input_impl = {
	.destroy = resource_destroy_req,
	.enable = text_input_enable_req,
	.disable = text_input_disable_req,
	.set_surrounding_text = text_input_set_surrounding_text_req,
	.set_text_change_cause = text_input_set_text_change_cause_req,
	.set_content_type = text_input_set_content_type_req,
	.set_cursor_rectangle = text_input_set_cursor_rectangle_req,
	.commit = text_input_commit_req,
};

static void
text_input_manager_get_text_input_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id, struct wl_resource *seat)
{
	struct text_input *text_input = calloc(1, sizeof(*text_input));
	assert(text_input);
	text_input->resource = wl_resource_create(client,
		&zwp_text_input_v3_interface, wl_resource_get_version(resource), id);
	assert(text_input->resource);
	wl_resource_set_implementation(text_input->resource, &text_input_impl,
		text_input, text_input_resource_destroy);
	list_append(&core.seat.text_inputs, &text_input->link);
	if (resource_matches_surface(text_input->resource, core.seat.keyboard.focus)) {
		text_input_set_focus(text_input, core.seat.keyboard.focus);
	}
}

static const struct zwp_text_input_manager_v3_interface text_input_manager_impl = {
	.destroy = resource_destroy_req,
	.get_text_input = text_input_manager_get_text_input_req,
};

void
wayland_bind_text_input_manager(struct wl_client *client, void *data,
	uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&zwp_text_input_manager_v3_interface, MIN(version, 1), id);
	assert(resource);
	wl_resource_set_implementation(resource, &text_input_manager_impl, data, NULL);
}
