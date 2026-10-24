#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

#include "core.h"
#include "virtual-keyboard-unstable-v1-protocol.h"

static void
virtual_keyboard_keymap_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t format, int32_t fd, uint32_t size)
{
	struct keyboard_source *source = wl_resource_get_user_data(resource);
	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
		wl_resource_post_error(resource,
			ZWP_VIRTUAL_KEYBOARD_V1_ERROR_INVALID_KEYMAP_FORMAT,
			"unsupported keymap format");
		close(fd);
		return;
	}
	// Keymaps are tens of KiB, so the limit only rejects the clients making
	// the compositor allocate an arbitrary size.
	if (size > 1024 * 1024) {
		wl_resource_post_error(resource,
			ZWP_VIRTUAL_KEYBOARD_V1_ERROR_INVALID_KEYMAP_FORMAT,
			"keymap too large");
		close(fd);
		return;
	}
	// Read rather than mapped, which would crash on a file shorter than the
	// size.
	char *string = malloc((size_t)size + 1);
	assert(string);
	bool complete = pread(fd, string, size, 0) == (ssize_t)size;
	close(fd);
	string[size] = '\0';
	struct xkb_keymap *keymap = complete
		? xkb_keymap_new_from_string(core.seat.keyboard.context, string,
			XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS)
		: NULL;
	free(string);
	if (!keymap) {
		wl_resource_post_error(resource,
			ZWP_VIRTUAL_KEYBOARD_V1_ERROR_INVALID_KEYMAP_FORMAT, "invalid keymap");
		return;
	}
	keyboard_set_source_keymap(source, keymap);
	xkb_keymap_unref(keymap);
}

static bool
has_keymap(struct wl_resource *resource)
{
	struct keyboard_source *source = wl_resource_get_user_data(resource);
	if (source->keymap_fd < 0) {
		wl_resource_post_error(resource,
			ZWP_VIRTUAL_KEYBOARD_V1_ERROR_NO_KEYMAP, "no keymap is set");
		return false;
	}
	return true;
}

static void
virtual_keyboard_key_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t time, uint32_t key, uint32_t state)
{
	if (has_keymap(resource)) {
		keyboard_send_source_key(wl_resource_get_user_data(resource), time,
			key, state == WL_KEYBOARD_KEY_STATE_PRESSED);
	}
}

static void
virtual_keyboard_modifiers_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t depressed, uint32_t latched,
	uint32_t locked, uint32_t group)
{
	if (has_keymap(resource)) {
		keyboard_set_source_modifiers(wl_resource_get_user_data(resource),
			depressed, latched, locked, group);
	}
}

static const struct zwp_virtual_keyboard_v1_interface virtual_keyboard_impl = {
	.keymap = virtual_keyboard_keymap_req,
	.key = virtual_keyboard_key_req,
	.modifiers = virtual_keyboard_modifiers_req,
	.destroy = resource_destroy_req,
};

static void
virtual_keyboard_resource_destroy(struct wl_resource *resource)
{
	struct keyboard_source *source = wl_resource_get_user_data(resource);
	keyboard_remove_source(source);
	free(source);
}

// The keys of a virtual keyboard go to the focus with its own keymap, which
// replaces the one of the physical keyboards until they send a key again.
// Unlike theirs, they do not run bindings.
static void
manager_create_virtual_keyboard_req(struct wl_client *client,
	struct wl_resource *resource, struct wl_resource *seat, uint32_t id)
{
	struct wl_resource *virtual_keyboard = wl_resource_create(client,
		&zwp_virtual_keyboard_v1_interface, wl_resource_get_version(resource), id);
	assert(virtual_keyboard);
	struct keyboard_source *source = calloc(1, sizeof(*source));
	assert(source);
	source->keymap_fd = -1;
	wl_resource_set_implementation(virtual_keyboard, &virtual_keyboard_impl,
		source, virtual_keyboard_resource_destroy);
}

static const struct zwp_virtual_keyboard_manager_v1_interface manager_impl = {
	.create_virtual_keyboard = manager_create_virtual_keyboard_req,
};

void
wayland_bind_virtual_keyboard_manager(struct wl_client *client, void *data,
	uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&zwp_virtual_keyboard_manager_v1_interface, 1, id);
	assert(resource);
	wl_resource_set_implementation(resource, &manager_impl, NULL, NULL);
}
