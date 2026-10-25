#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

#include "config.h"
#include "core.h"
#include "surface.h"

struct keyboard_resource {
	struct wl_resource *resource;
	struct wl_list link; // core.seat.keyboard.resources
};

void
keyboard_send_modifiers(uint32_t depressed, uint32_t latched, uint32_t locked,
	uint32_t group)
{
	if (!core.seat.keyboard.focus) {
		return;
	}

	uint32_t serial = ++core.serial;
	struct keyboard_resource *keyboard;
	wl_list_for_each(keyboard, &core.seat.keyboard.resources, link) {
		if (resource_matches_surface(keyboard->resource, core.seat.keyboard.focus)) {
			wl_keyboard_send_modifiers(keyboard->resource, serial,
				depressed, latched, locked, group);
		}
	}
}

static void
send_current_modifiers_to_focus(void)
{
	struct keyboard_source *source = core.seat.keyboard.source;
	keyboard_send_modifiers(source->modifiers.depressed,
		source->modifiers.latched, source->modifiers.locked,
		source->modifiers.group);
}

static void
send_keyboard_enter(struct wl_resource *resource, struct surface *surface)
{
	struct keyboard_source *source = core.seat.keyboard.source;
	struct wl_array keys = {
		.size = source->nr_pressed_keys * sizeof(source->pressed_keys[0]),
		.data = source->pressed_keys,
	};
	wl_keyboard_send_enter(resource, ++core.serial, surface->resource, &keys);
}

static void
send_enter(struct surface *surface)
{
	struct keyboard_resource *keyboard;
	wl_list_for_each(keyboard, &core.seat.keyboard.resources, link) {
		if (resource_matches_surface(keyboard->resource, surface)) {
			send_keyboard_enter(keyboard->resource, surface);
		}
	}
}

static void
send_leave(struct surface *surface)
{
	uint32_t serial = ++core.serial;
	struct keyboard_resource *keyboard;
	wl_list_for_each(keyboard, &core.seat.keyboard.resources, link) {
		if (resource_matches_surface(keyboard->resource, surface)) {
			wl_keyboard_send_leave(keyboard->resource, serial, surface->resource);
		}
	}
}

static void
keyboard_resource_destroy(struct wl_resource *resource)
{
	struct keyboard_resource *keyboard = wl_resource_get_user_data(resource);
	wl_list_remove(&keyboard->link);
	free(keyboard);
}

static const struct wl_keyboard_interface keyboard_impl = {
	.release = resource_destroy_req,
};

void
keyboard_send_keymap(struct wl_resource *resource, struct keyboard_source *source)
{
	wl_keyboard_send_keymap(resource, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
		source->keymap_fd, source->keymap_size);
	if (wl_resource_get_version(resource) >= WL_KEYBOARD_REPEAT_INFO_SINCE_VERSION) {
		wl_keyboard_send_repeat_info(resource, config.keyboard.repeat_rate,
			config.keyboard.repeat_delay);
	}
}

// Gives every wl_keyboard the keymap of the source, and the focus its keys and
// modifiers. The focus leaves and enters again to replace the pressed keys,
// unless neither source has any.
void
keyboard_use_source(struct keyboard_source *source)
{
	struct keyboard *keyboard = &core.seat.keyboard;
	if (keyboard->source == source) {
		return;
	}
	struct surface *focus = keyboard->focus;
	bool reenter = focus && (keyboard->source->nr_pressed_keys > 0
		|| source->nr_pressed_keys > 0);
	if (reenter) {
		send_leave(focus);
	}
	keyboard->source = source;
	struct keyboard_resource *resource;
	wl_list_for_each(resource, &keyboard->resources, link) {
		keyboard_send_keymap(resource->resource, source);
	}
	if (reenter) {
		send_enter(focus);
	}
	send_current_modifiers_to_focus();
}

// The source starts with the keymap, and without pressed keys and modifiers.
static void
init_source(struct keyboard_source *source, struct xkb_keymap *keymap)
{
	char *string = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
	assert(string);
	*source = (struct keyboard_source){
		.keymap_size = (uint32_t)strlen(string) + 1,
	};
	source->keymap_fd = create_sealed_file("jimi-keymap", string,
		source->keymap_size);
	free(string);
}

// The source no longer sends keys, and its pressed keys are released by
// leaving the focus.
void
keyboard_remove_source(struct keyboard_source *source)
{
	if (core.seat.keyboard.source == source) {
		keyboard_use_source(&core.seat.keyboard.physical);
	}
	if (source->keymap_fd >= 0) {
		close(source->keymap_fd);
	}
}

// Replaces the keymap, with which the keys and the modifiers of the source
// start over.
void
keyboard_set_source_keymap(struct keyboard_source *source, struct xkb_keymap *keymap)
{
	keyboard_remove_source(source);
	init_source(source, keymap);
}

void
keyboard_create_resource(struct wl_client *client, struct wl_resource *seat,
	uint32_t id)
{
	struct keyboard_resource *keyboard = calloc(1, sizeof(*keyboard));
	assert(keyboard);
	keyboard->resource = wl_resource_create(client, &wl_keyboard_interface,
		wl_resource_get_version(seat), id);
	assert(keyboard->resource);
	wl_resource_set_implementation(keyboard->resource, &keyboard_impl,
		keyboard, keyboard_resource_destroy);
	list_append(&core.seat.keyboard.resources, &keyboard->link);

	keyboard_send_keymap(keyboard->resource, core.seat.keyboard.source);
	if (resource_matches_surface(keyboard->resource, core.seat.keyboard.focus)) {
		send_keyboard_enter(keyboard->resource, core.seat.keyboard.focus);
		send_current_modifiers_to_focus();
	}
}

// The surfaces of hidden windows, e.g. on other workspaces, stay in the focus
// history, to take the focus back once shown again.
static bool
shown(struct surface *surface)
{
	struct window *window = window_from_surface(surface);
	return !window || window_visible(window);
}

// The shown window of the latest focused surface which has one. It stays
// active while layer surfaces and their popups take the focus.
struct window *
keyboard_active_window(void)
{
	struct surface *surface;
	wl_list_for_each(surface, &core.seat.keyboard.focus_history, focus_link) {
		struct window *window = window_from_surface(surface);
		if (window && window_visible(window)) {
			return window;
		}
	}
	return NULL;
}

static void
update_activated_window(void)
{
	struct window *active = keyboard_active_window();
	struct window *window;
	wl_list_for_each(window, &core.windows, link) {
		window_set_activated(window, window == active);
	}
}

void
keyboard_focus(struct surface *surface)
{
	// A window gives the focus to its modal dialog, while its popups keep it.
	struct window *window = window_from_surface(surface);
	if (window && surface == window->surface) {
		surface = window_focus_target(window)->surface;
	}
	if (surface && !surface->mapped) {
		surface = NULL;
	}
	if (core.session_lock.locked) {
		surface = session_lock_focus(surface);
	} else {
		// Only the popups of an exclusive layer surface can take its focus.
		struct surface *exclusive = layer_surface_exclusive_focus();
		if (exclusive && !surface_contains(exclusive, surface)) {
			surface = exclusive;
		}
	}
	if (core.seat.keyboard.focus == surface) {
		return;
	}
	core.seat.input_serial = 0;
	core.seat.input_client = NULL;

	if (core.seat.keyboard.focus) {
		send_leave(core.seat.keyboard.focus);
	}

	core.seat.keyboard.focus = surface;
	if (surface) {
		wl_list_remove(&surface->focus_link);
		wl_list_insert(&core.seat.keyboard.focus_history, &surface->focus_link);
	}
	// The selection is sent right before the keyboard enters, and the text
	// inputs and the tablet pads enter after it. GTK crashes on a pad
	// mode_switch that comes before the keyboard enter, since it targets the
	// pad events at the surface of the keyboard.
	data_device_keyboard_focus(surface);
	if (surface) {
		send_enter(surface);
		send_current_modifiers_to_focus();
	}
	text_inputs_focus(surface);
	tablet_pads_update_focus();
	update_activated_window();
}

// Returns whether the key was pressed.
static bool
release_key(struct keyboard_source *source, uint32_t key)
{
	for (size_t i = 0; i < source->nr_pressed_keys; i++) {
		if (source->pressed_keys[i] == key) {
			source->pressed_keys[i] =
				source->pressed_keys[--source->nr_pressed_keys];
			return true;
		}
	}
	return false;
}

// Returns whether the key was released, and is pressed now.
static bool
press_key(struct keyboard_source *source, uint32_t key)
{
	for (size_t i = 0; i < source->nr_pressed_keys; i++) {
		if (source->pressed_keys[i] == key) {
			return false;
		}
	}
	if (source->nr_pressed_keys == ARRAY_LEN(source->pressed_keys)) {
		return false;
	}
	source->pressed_keys[source->nr_pressed_keys++] = key;
	return true;
}

// The modifiers of bindings, by the bits of enum modifier
static const char *const modifier_names[] = {
	XKB_MOD_NAME_SHIFT,
	XKB_MOD_NAME_CTRL,
	XKB_MOD_NAME_ALT,
	XKB_MOD_NAME_LOGO,
};

// The modifiers of bindings, as enum modifier. Latched modifiers apply as
// well, but not locked ones like Caps Lock.
uint32_t
keyboard_pressed_modifiers(void)
{
	uint32_t modifiers = 0;
	for (size_t i = 0; i < ARRAY_LEN(modifier_names); i++) {
		if (xkb_state_mod_name_is_active(core.seat.keyboard.state, modifier_names[i],
				XKB_STATE_MODS_DEPRESSED | XKB_STATE_MODS_LATCHED) > 0) {
			modifiers |= 1u << i;
		}
	}
	return modifiers;
}

// The modifiers of bindings, as enum modifier, which the key uses to produce
// its keysyms, like Shift for '!' on a us layout.
static uint32_t
consumed_modifiers(xkb_keycode_t keycode)
{
	uint32_t modifiers = 0;
	for (size_t i = 0; i < ARRAY_LEN(modifier_names); i++) {
		xkb_mod_index_t index = xkb_keymap_mod_get_index(
			core.seat.keyboard.keymap, modifier_names[i]);
		if (xkb_state_mod_index_is_consumed(core.seat.keyboard.state,
				keycode, index) > 0) {
			modifiers |= 1u << i;
		}
	}
	return modifiers;
}

static const struct key_binding *
find_binding_by_keysyms(const xkb_keysym_t *keysyms, int nr_keysyms,
		uint32_t modifiers)
{
	for (size_t i = 0; i < config.nr_key_bindings; i++) {
		const struct key_binding *binding = &config.key_bindings[i];
		if (binding->modifiers != modifiers) {
			continue;
		}
		for (int j = 0; j < nr_keysyms; j++) {
			if (keysyms[j] == binding->keysym) {
				return binding;
			}
		}
	}
	return NULL;
}

// Bindings match the keysym the key produces without the modifiers it uses,
// so that Super+Shift+minus matches Super+equal on a jp layout. Otherwise,
// they match the keysym of the key without modifiers, so that Shift does not
// change it, like Alt+Shift+Tab, which produces ISO_Left_Tab.
static const struct key_binding *
find_binding(uint32_t key)
{
	struct xkb_state *state = core.seat.keyboard.state;
	uint32_t modifiers = keyboard_pressed_modifiers();
	const xkb_keysym_t *keysyms;
	int nr_keysyms = xkb_state_key_get_syms(state, key + 8, &keysyms);
	const struct key_binding *binding = find_binding_by_keysyms(keysyms,
		nr_keysyms, modifiers & ~consumed_modifiers(key + 8));
	if (binding) {
		return binding;
	}
	xkb_layout_index_t layout = xkb_state_key_get_layout(state, key + 8);
	nr_keysyms = xkb_keymap_key_get_syms_by_level(core.seat.keyboard.keymap,
		key + 8, layout, 0, &keysyms);
	return find_binding_by_keysyms(keysyms, nr_keysyms, modifiers);
}

// The next and previous workspaces stop at the first and the last one.
static int
binding_workspace(const struct key_binding *binding)
{
	int last = (int)config.workspaces.nr_names - 1;
	switch (binding->workspace) {
	case WORKSPACE_NEXT:
		return MIN(core.workspaces.current + 1, last);
	case WORKSPACE_PREVIOUS:
		return MAX(core.workspaces.current - 1, 0);
	}
	return binding->workspace;
}

static void
run_binding(const struct key_binding *binding)
{
	struct window *window = keyboard_active_window();
	switch (binding->action) {
	case ACTION_QUIT:
		core.running = false;
		return;
	case ACTION_RELOAD:
		core.reload = true;
		return;
	case ACTION_EXEC:
		spawn(binding->command);
		return;
	case ACTION_CLOSE:
		if (window) {
			window_close(window);
		}
		return;
	case ACTION_TOGGLE_FULLSCREEN:
		if (window) {
			window_set_fullscreen(window, !window->pending.fullscreen);
		}
		return;
	case ACTION_TOGGLE_MAXIMIZE:
		if (window) {
			window_set_maximized(window, !window->pending.maximized);
		}
		return;
	case ACTION_TOGGLE_STICKY:
		if (window) {
			window_set_sticky(window, window->workspace != WORKSPACE_STICKY);
		}
		return;
	case ACTION_NEXT_WINDOW:
		window_switcher_step(true);
		return;
	case ACTION_PREVIOUS_WINDOW:
		window_switcher_step(false);
		return;
	case ACTION_WORKSPACE:
		workspace_switch(binding_workspace(binding));
		return;
	case ACTION_MOVE_TO_WORKSPACE:
		if (window) {
			window_set_workspace(window, binding_workspace(binding));
		}
		return;
	case ACTION_TOGGLE_MAGNIFIER:
		magnifier_set_zoom(core.magnifier.zoom > 1.0 ? 1.0 : 2.0);
		return;
	case ACTION_ZOOM_IN:
		magnifier_set_zoom(core.magnifier.zoom * config.magnifier.zoom_factor);
		return;
	case ACTION_ZOOM_OUT:
		magnifier_set_zoom(core.magnifier.zoom / config.magnifier.zoom_factor);
		return;
	}
	assert(!"unknown action");
}

// The timer is disarmed by 0, so the delay and the interval are at least 1 ms.
static void
arm_repeat_timer(int ms)
{
	wl_event_source_timer_update(core.seat.keyboard.repeat_timer, MAX(ms, 1));
}

static int
handle_repeat_timer(void *data)
{
	(void)data;
	if (core.session_lock.locked) {
		return 0;
	}
	run_binding(core.seat.keyboard.repeat_binding);
	arm_repeat_timer(1000 / config.keyboard.repeat_rate);
	return 0;
}

// Bindings repeat like the keys clients repeat, unless the keymap does not
// repeat the key.
static void
start_repeating_binding(const struct key_binding *binding, uint32_t key)
{
	struct keyboard *keyboard = &core.seat.keyboard;
	if (config.keyboard.repeat_rate > 0
			&& xkb_keymap_key_repeats(keyboard->keymap, key + 8)) {
		keyboard->repeat_binding = binding;
		arm_repeat_timer(config.keyboard.repeat_delay);
	}
}

void
keyboard_send_key(uint32_t time, uint32_t key, uint32_t state)
{
	if (!core.seat.keyboard.focus) {
		return;
	}
	uint32_t serial = ++core.serial;
	bool sent = false;
	struct keyboard_resource *resource;
	wl_list_for_each(resource, &core.seat.keyboard.resources, link) {
		if (resource_matches_surface(resource->resource, core.seat.keyboard.focus)) {
			wl_keyboard_send_key(resource->resource, serial, time, key, state);
			sent = true;
		}
	}
	if (sent) {
		core.seat.input_serial = serial;
		core.seat.input_client = wl_resource_get_client(
			core.seat.keyboard.focus->resource);
	}
}

// The keyboard grab of the input method takes the keys and the modifiers from
// the focus.
static void
send_key_to_grab_or_focus(uint32_t time, uint32_t key, bool pressed)
{
	uint32_t state = pressed ? WL_KEYBOARD_KEY_STATE_PRESSED
		: WL_KEYBOARD_KEY_STATE_RELEASED;
	struct wl_resource *grab = core.seat.input_method.keyboard_grab;
	if (grab) {
		wl_keyboard_send_key(grab, ++core.serial, time, key, state);
	} else {
		keyboard_send_key(time, key, state);
	}
}

static void
send_modifiers_to_grab_or_focus(void)
{
	struct wl_resource *grab = core.seat.input_method.keyboard_grab;
	if (grab) {
		struct keyboard_source *physical = &core.seat.keyboard.physical;
		wl_keyboard_send_modifiers(grab, ++core.serial,
			physical->modifiers.depressed, physical->modifiers.latched,
			physical->modifiers.locked, physical->modifiers.group);
	} else {
		send_current_modifiers_to_focus();
	}
}

// Ignores a key pressed or released again, as the protocol does not prevent a
// virtual keyboard from sending it.
void
keyboard_send_source_key(struct keyboard_source *source, uint32_t time,
	uint32_t key, bool pressed)
{
	keyboard_use_source(source);
	if (pressed ? press_key(source, key) : release_key(source, key)) {
		keyboard_send_key(time, key, pressed ? WL_KEYBOARD_KEY_STATE_PRESSED
			: WL_KEYBOARD_KEY_STATE_RELEASED);
	}
}

void
keyboard_set_source_modifiers(struct keyboard_source *source,
	uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group)
{
	source->modifiers.depressed = depressed;
	source->modifiers.latched = latched;
	source->modifiers.locked = locked;
	source->modifiers.group = group;
	if (core.seat.keyboard.source == source) {
		send_current_modifiers_to_focus();
	} else {
		keyboard_use_source(source);
	}
}

struct keysym_key {
	xkb_keysym_t keysym;
	xkb_layout_index_t layout;
	xkb_keycode_t keycode; // XKB_KEYCODE_INVALID until found.
	xkb_mod_mask_t modifiers; // Selecting the level of the keysym
};

static void
find_keysym_key(struct xkb_keymap *keymap, xkb_keycode_t keycode, void *data)
{
	struct keysym_key *key = data;
	if (key->keycode != XKB_KEYCODE_INVALID) {
		return;
	}
	xkb_level_index_t nr_levels =
		xkb_keymap_num_levels_for_key(keymap, keycode, key->layout);
	for (xkb_level_index_t level = 0; level < nr_levels; level++) {
		const xkb_keysym_t *keysyms;
		int nr_keysyms = xkb_keymap_key_get_syms_by_level(keymap, keycode,
			key->layout, level, &keysyms);
		if (nr_keysyms != 1 || keysyms[0] != key->keysym) {
			continue;
		}
		xkb_mod_mask_t modifiers = 0;
		if (xkb_keymap_key_get_mods_for_level(keymap, keycode, key->layout,
				level, &modifiers, 1) == 0 && level > 0) {
			continue;
		}
		key->keycode = keycode;
		key->modifiers = modifiers;
		return;
	}
}

// Types the keysym on the focus with a key of the current layout of the
// physical keyboards producing it, and the modifiers selecting its level.
void
keyboard_send_keysym(uint32_t time, uint32_t keysym)
{
	struct keyboard *keyboard = &core.seat.keyboard;
	struct keysym_key key = {
		.keysym = keysym,
		.layout = keyboard->physical.modifiers.group,
		.keycode = XKB_KEYCODE_INVALID,
	};
	xkb_keymap_key_for_each(keyboard->keymap, find_keysym_key, &key);
	if (key.keycode == XKB_KEYCODE_INVALID) {
		return;
	}
	keyboard_use_source(&keyboard->physical);
	keyboard_send_modifiers(key.modifiers, 0, 0, keyboard->physical.modifiers.group);
	keyboard_send_key(time, key.keycode - 8, WL_KEYBOARD_KEY_STATE_PRESSED);
	keyboard_send_key(time, key.keycode - 8, WL_KEYBOARD_KEY_STATE_RELEASED);
	send_current_modifiers_to_focus();
}

// Handles a key of the physical keyboards.
void
keyboard_handle_key(uint32_t time, uint32_t key, bool pressed)
{
	struct keyboard_source *physical = &core.seat.keyboard.physical;
	keyboard_use_source(physical);
	wl_event_source_timer_update(core.seat.keyboard.repeat_timer, 0);
	xkb_state_update_key(core.seat.keyboard.state, key + 8,
		pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
	uint32_t depressed = xkb_state_serialize_mods(core.seat.keyboard.state,
		XKB_STATE_MODS_DEPRESSED);
	uint32_t latched = xkb_state_serialize_mods(core.seat.keyboard.state,
		XKB_STATE_MODS_LATCHED);
	uint32_t locked = xkb_state_serialize_mods(core.seat.keyboard.state,
		XKB_STATE_MODS_LOCKED);
	uint32_t group = xkb_state_serialize_layout(core.seat.keyboard.state,
		XKB_STATE_LAYOUT_EFFECTIVE);
	bool modifiers_changed = depressed != physical->modifiers.depressed
		|| latched != physical->modifiers.latched
		|| locked != physical->modifiers.locked
		|| group != physical->modifiers.group;
	physical->modifiers.depressed = depressed;
	physical->modifiers.latched = latched;
	physical->modifiers.locked = locked;
	physical->modifiers.group = group;

	// The keys of bindings are not sent to clients, from press to release,
	// and neither is Escape, which cancels window switching, nor the keys
	// switching VTs, which the keymap gives to Ctrl+Alt+F<n>. Bindings do
	// not apply while the session is locked.
	xkb_keysym_t keysym = pressed
		? xkb_state_key_get_one_sym(core.seat.keyboard.state, key + 8)
		: XKB_KEY_NoSymbol;
	const struct key_binding *binding = pressed && !core.session_lock.locked
		? find_binding(key) : NULL;
	if (keysym == XKB_KEY_Escape && core.window_switcher.selected) {
		window_switcher_cancel();
	} else if (keysym >= XKB_KEY_XF86Switch_VT_1
			&& keysym <= XKB_KEY_XF86Switch_VT_12) {
		session_switch_vt((int)(keysym - XKB_KEY_XF86Switch_VT_1 + 1));
	} else if (binding) {
		run_binding(binding);
		start_repeating_binding(binding, key);
	} else if (pressed ? press_key(physical, key) : release_key(physical, key)) {
		send_key_to_grab_or_focus(time, key, pressed);
	}
	if (modifiers_changed) {
		send_modifiers_to_grab_or_focus();
	}
	// Releasing the modifiers ends window switching. The focus it returns
	// to enters without the released key.
	if (core.window_switcher.selected && !keyboard_pressed_modifiers()) {
		window_switcher_finish();
	}
}

// The keymap and the state of the physical keyboards, from the config
static void
create_keymap(void)
{
	struct keyboard *keyboard = &core.seat.keyboard;
	struct xkb_rule_names names = {
		.layout = config.keyboard.layout,
		.variant = config.keyboard.variant,
		.options = config.keyboard.options,
	};
	// The config has checked the names.
	keyboard->keymap = xkb_keymap_new_from_names(keyboard->context, &names,
		XKB_KEYMAP_COMPILE_NO_FLAGS);
	assert(keyboard->keymap);
	keyboard->state = xkb_state_new(keyboard->keymap);
	assert(keyboard->state);
}

void
keyboard_init(void)
{
	struct keyboard *keyboard = &core.seat.keyboard;
	wl_list_init(&keyboard->resources);
	wl_list_init(&keyboard->focus_history);
	keyboard->context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	assert(keyboard->context);
	create_keymap();
	init_source(&keyboard->physical, keyboard->keymap);
	keyboard->source = &keyboard->physical;
	keyboard->repeat_timer = wl_event_loop_add_timer(core.wl_loop,
		handle_repeat_timer, NULL);
	assert(keyboard->repeat_timer);
}

// Replaces the keymap of the physical keyboards by the config, while their
// keys are released. The clients and the keyboard grab of the input method
// get the keymap and the repeat info again.
void
keyboard_reload(void)
{
	struct keyboard *keyboard = &core.seat.keyboard;
	xkb_state_unref(keyboard->state);
	xkb_keymap_unref(keyboard->keymap);
	create_keymap();
	keyboard_set_source_keymap(&keyboard->physical, keyboard->keymap);
	struct keyboard_resource *resource;
	wl_list_for_each(resource, &keyboard->resources, link) {
		keyboard_send_keymap(resource->resource, keyboard->source);
	}
	send_current_modifiers_to_focus();
	if (core.seat.input_method.keyboard_grab) {
		keyboard_send_keymap(core.seat.input_method.keyboard_grab,
			&keyboard->physical);
	}
}

// The surface and its popups can no longer take the focus, e.g. when
// unmapped or minimized. If they have it, it returns to the surface which had
// it before, as to a window from a launcher or from a popup.
void
keyboard_forget_surface(struct surface *surface)
{
	struct surface *entry, *tmp;
	wl_list_for_each_safe(entry, tmp, &core.seat.keyboard.focus_history,
			focus_link) {
		if (surface_contains(surface, entry)) {
			wl_list_remove(&entry->focus_link);
			wl_list_init(&entry->focus_link);
		}
	}
	update_activated_window();
	if (surface_contains(surface, core.seat.keyboard.focus)) {
		keyboard_restore_focus();
	}
}

// Gives the focus to the latest shown surface which had it.
void
keyboard_restore_focus(void)
{
	struct surface *surface;
	wl_list_for_each(surface, &core.seat.keyboard.focus_history, focus_link) {
		if (shown(surface)) {
			keyboard_focus(surface);
			return;
		}
	}
	keyboard_focus(NULL);
}

// After windows are shown or hidden by workspaces, the focus moves from a
// hidden window, or to a shown one if no surface has it, and the active window
// is the latest shown one.
void
keyboard_update_focus(void)
{
	struct surface *focus = core.seat.keyboard.focus;
	if (!focus || !shown(focus)) {
		keyboard_restore_focus();
	}
	update_activated_window();
}

void
keyboard_fini(void)
{
	assert(wl_list_empty(&core.seat.keyboard.resources));
	wl_event_source_remove(core.seat.keyboard.repeat_timer);
	close(core.seat.keyboard.physical.keymap_fd);
	xkb_state_unref(core.seat.keyboard.state);
	xkb_keymap_unref(core.seat.keyboard.keymap);
	xkb_context_unref(core.seat.keyboard.context);
}
