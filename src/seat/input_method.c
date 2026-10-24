#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "core.h"
#include "input-method-unstable-v1-protocol.h"
#include "surface.h"
#include "text-input-unstable-v3-protocol.h"

// zwp_text_input_v1.content_purpose lacks pin, and numbers the purposes after
// it one lower. The content hints of both match.
static uint32_t
content_purpose_v1(uint32_t purpose)
{
	if (purpose < ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_PIN) {
		return purpose;
	}
	if (purpose == ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_PIN) {
		return ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_PASSWORD;
	}
	return purpose - 1;
}

static void
reset_composition(void)
{
	struct input_method *input_method = &core.seat.input_method;
	input_method->preedit_cursor_set = false;
	input_method->delete_before = 0;
	input_method->delete_after = 0;
	free(input_method->preedit_commit);
	input_method->preedit_commit = NULL;
}

// The text input of the context, or NULL if the context is inert.
static struct text_input *
context_text_input(struct wl_resource *context)
{
	struct input_method *input_method = &core.seat.input_method;
	return context == input_method->context ? input_method->text_input : NULL;
}

static void
context_commit_string_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t serial, const char *text)
{
	struct text_input *text_input = context_text_input(resource);
	if (!text_input) {
		return;
	}
	struct input_method *input_method = &core.seat.input_method;
	if (input_method->delete_before || input_method->delete_after) {
		zwp_text_input_v3_send_delete_surrounding_text(text_input->resource,
			input_method->delete_before, input_method->delete_after);
	}
	zwp_text_input_v3_send_commit_string(text_input->resource, text);
	zwp_text_input_v3_send_done(text_input->resource, text_input->serial);
	reset_composition();
}

// The cursor is at the end of the preedit string unless the input method
// sets it.
static void
context_preedit_string_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t serial, const char *text,
	const char *commit)
{
	struct text_input *text_input = context_text_input(resource);
	if (!text_input) {
		return;
	}
	struct input_method *input_method = &core.seat.input_method;
	int32_t cursor = (int32_t)strlen(text);
	if (input_method->preedit_cursor_set) {
		// A negative cursor is hidden.
		cursor = MAX(input_method->preedit_cursor, -1);
	}
	zwp_text_input_v3_send_preedit_string(text_input->resource, text, cursor,
		cursor);
	zwp_text_input_v3_send_done(text_input->resource, text_input->serial);
	input_method->preedit_cursor_set = false;
	free(input_method->preedit_commit);
	input_method->preedit_commit = strdup(commit);
	assert(input_method->preedit_commit);
}

static void
context_preedit_styling_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t index, uint32_t length, uint32_t style)
{
	// Version 1 of text-input-v3 has no preedit styles.
}

static void
context_preedit_cursor_req(struct wl_client *client,
	struct wl_resource *resource, int32_t index)
{
	if (!context_text_input(resource)) {
		return;
	}
	core.seat.input_method.preedit_cursor = index;
	core.seat.input_method.preedit_cursor_set = true;
}

// Deletes the length from the index relative to the cursor along with the
// next commit string. text-input-v3 counts the length before and after the
// cursor, so the range has to contain the cursor.
static void
context_delete_surrounding_text_req(struct wl_client *client,
	struct wl_resource *resource, int32_t index, uint32_t length)
{
	int64_t end = (int64_t)index + length;
	if (!context_text_input(resource) || index > 0 || end < 0) {
		return;
	}
	core.seat.input_method.delete_before = (uint32_t)-index;
	core.seat.input_method.delete_after = (uint32_t)end;
}

static void
context_cursor_position_req(struct wl_client *client,
	struct wl_resource *resource, int32_t index, int32_t anchor)
{
	// text-input-v3 cannot move the cursor.
}

static void
context_modifiers_map_req(struct wl_client *client,
	struct wl_resource *resource, struct wl_array *map)
{
	// Only used by keysym requests, whose modifiers are ignored.
}

// Text inputs have no keysyms, so they are typed with keys instead.
static void
context_keysym_req(struct wl_client *client, struct wl_resource *resource,
	uint32_t serial, uint32_t time, uint32_t sym, uint32_t state, uint32_t modifiers)
{
	if (context_text_input(resource) && state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		keyboard_send_keysym(time, sym);
	}
}

static void
keyboard_grab_resource_destroy(struct wl_resource *resource)
{
	if (core.seat.input_method.keyboard_grab == resource) {
		core.seat.input_method.keyboard_grab = NULL;
	}
}

static const struct wl_keyboard_interface keyboard_grab_impl = {
	.release = resource_destroy_req,
};

// The wl_keyboard has the version of the context.
static void
context_grab_keyboard_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id)
{
	struct wl_resource *keyboard = wl_resource_create(client,
		&wl_keyboard_interface, wl_resource_get_version(resource), id);
	assert(keyboard);
	wl_resource_set_implementation(keyboard, &keyboard_grab_impl, NULL,
		keyboard_grab_resource_destroy);
	if (!context_text_input(resource)) {
		return;
	}
	core.seat.input_method.keyboard_grab = keyboard;
	struct keyboard_source *physical = &core.seat.keyboard.physical;
	keyboard_send_keymap(keyboard, physical);
	wl_keyboard_send_modifiers(keyboard, ++core.serial,
		physical->modifiers.depressed, physical->modifiers.latched,
		physical->modifiers.locked, physical->modifiers.group);
}

static void
context_key_req(struct wl_client *client, struct wl_resource *resource,
	uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
	if (context_text_input(resource)) {
		keyboard_use_source(&core.seat.keyboard.physical);
		keyboard_send_key(time, key, state);
	}
}

static void
context_modifiers_req(struct wl_client *client, struct wl_resource *resource,
	uint32_t serial, uint32_t depressed, uint32_t latched, uint32_t locked,
	uint32_t group)
{
	if (context_text_input(resource)) {
		keyboard_use_source(&core.seat.keyboard.physical);
		keyboard_send_modifiers(depressed, latched, locked, group);
	}
}

static void
context_language_req(struct wl_client *client, struct wl_resource *resource,
	uint32_t serial, const char *language)
{
	// Version 1 of text-input-v3 has no language.
}

static void
context_text_direction_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t serial, uint32_t direction)
{
	// text-input-v3 has no text direction.
}

static const struct zwp_input_method_context_v1_interface context_impl = {
	.destroy = resource_destroy_req,
	.commit_string = context_commit_string_req,
	.preedit_string = context_preedit_string_req,
	.preedit_styling = context_preedit_styling_req,
	.preedit_cursor = context_preedit_cursor_req,
	.delete_surrounding_text = context_delete_surrounding_text_req,
	.cursor_position = context_cursor_position_req,
	.modifiers_map = context_modifiers_map_req,
	.keysym = context_keysym_req,
	.grab_keyboard = context_grab_keyboard_req,
	.key = context_key_req,
	.modifiers = context_modifiers_req,
	.language = context_language_req,
	.text_direction = context_text_direction_req,
};

// The keyboard grab ends along with its context.
static void
context_resource_destroy(struct wl_resource *resource)
{
	struct input_method *input_method = &core.seat.input_method;
	if (input_method->context == resource) {
		input_method->context = NULL;
		input_method->keyboard_grab = NULL;
	}
}

// The commit state tells the input method the serial of the text input,
// which it sends back with its requests. They are applied regardless.
static void
send_state(void)
{
	struct input_method *input_method = &core.seat.input_method;
	struct text_input *text_input = input_method->text_input;
	struct text_input_state *state = &text_input->current;
	if (state->surrounding_text) {
		zwp_input_method_context_v1_send_surrounding_text(
			input_method->context, state->surrounding_text,
			state->surrounding_cursor, state->surrounding_anchor);
	}
	zwp_input_method_context_v1_send_content_type(input_method->context,
		state->content_hint, content_purpose_v1(state->content_purpose));
	zwp_input_method_context_v1_send_commit_state(input_method->context,
		text_input->serial);
}

// The zwp_input_method_v1 in use, or NULL
static struct wl_resource *
input_method_resource(void)
{
	struct wl_list *resources = &core.seat.input_method.resources;
	return wl_list_empty(resources) ? NULL : wl_resource_from_link(resources->next);
}

// Activates the input method for the text input, if one is bound.
static void
create_context(void)
{
	struct input_method *input_method = &core.seat.input_method;
	struct wl_resource *resource = input_method_resource();
	if (!resource) {
		return;
	}
	input_method->context = wl_resource_create(wl_resource_get_client(resource),
		&zwp_input_method_context_v1_interface, 1, 0);
	assert(input_method->context);
	wl_resource_set_implementation(input_method->context, &context_impl,
		NULL, context_resource_destroy);
	zwp_input_method_v1_send_activate(resource, input_method->context);
	send_state();
}

// The text input is enabled on the keyboard focus.
void
input_method_activate(struct text_input *text_input)
{
	struct input_method *input_method = &core.seat.input_method;
	assert(!input_method->text_input);
	input_method->text_input = text_input;
	create_context();
	input_panels_update();
}

// The text input is disabled or loses the focus.
void
input_method_deactivate(void)
{
	struct input_method *input_method = &core.seat.input_method;
	if (input_method->context) {
		zwp_input_method_v1_send_deactivate(input_method_resource(),
			input_method->context);
	}
	input_method->context = NULL;
	input_method->keyboard_grab = NULL;
	input_method->text_input = NULL;
	reset_composition();
	input_panels_update();
}

// The text input committed its state.
void
input_method_update(void)
{
	if (core.seat.input_method.context) {
		send_state();
	}
	input_panels_update();
}

// The text input was enabled again, e.g. for another text field.
void
input_method_reset(void)
{
	reset_composition();
	if (core.seat.input_method.context) {
		zwp_input_method_context_v1_send_reset(core.seat.input_method.context);
	}
	input_method_update();
}

void
input_method_commit_preedit(void)
{
	struct input_method *input_method = &core.seat.input_method;
	struct text_input *text_input = input_method->text_input;
	if (!input_method->preedit_commit || !input_method->preedit_commit[0]) {
		return;
	}
	zwp_text_input_v3_send_commit_string(text_input->resource,
		input_method->preedit_commit);
	zwp_text_input_v3_send_done(text_input->resource, text_input->serial);
}

// zwp_input_method_v1 has no destructor, so the client is gone along with
// its context and keyboard grab. The next input method takes over.
static void
input_method_resource_destroy(struct wl_resource *resource)
{
	struct input_method *input_method = &core.seat.input_method;
	bool used = resource == input_method_resource();
	resource_unlink(resource);
	if (!used) {
		return;
	}
	input_method->context = NULL;
	input_method->keyboard_grab = NULL;
	reset_composition();
	if (input_method->text_input) {
		create_context();
	}
}

void
wayland_bind_input_method(struct wl_client *client, void *data,
	uint32_t version, uint32_t id)
{
	struct input_method *input_method = &core.seat.input_method;
	struct wl_resource *resource = wl_resource_create(client,
		&zwp_input_method_v1_interface, 1, id);
	assert(resource);
	wl_resource_set_implementation(resource, NULL, NULL,
		input_method_resource_destroy);
	list_append(&input_method->resources, wl_resource_get_link(resource));
	if (resource == input_method_resource() && input_method->text_input) {
		create_context();
	}
}
