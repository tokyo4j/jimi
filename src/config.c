#include <assert.h>
#include <errno.h>
#include <libinput.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>
#include <yaml.h>

#include "config.h"

// After util.h, whose MIN and MAX glib redefines
#include <librsvg/rsvg.h>

// 0xrrggbbaa, as colors are written in the file
#define RGBA(rgba) { \
	(float)((rgba) >> 24 & 0xff) / 255.0f, \
	(float)((rgba) >> 16 & 0xff) / 255.0f, \
	(float)((rgba) >> 8 & 0xff) / 255.0f, \
	(float)((rgba) & 0xff) / 255.0f, \
}

static const struct binding default_bindings[] = {
	{.keysym = XKB_KEY_F4, .modifiers = MODIFIER_ALT, .action = ACTION_CLOSE},
	{.keysym = XKB_KEY_q, .modifiers = MODIFIER_SUPER, .action = ACTION_CLOSE},
	{.keysym = XKB_KEY_p, .modifiers = MODIFIER_SUPER, .action = ACTION_QUIT},
	{.keysym = XKB_KEY_s, .modifiers = MODIFIER_SUPER,
		.action = ACTION_EXEC, .command = "alacritty"},
	{.keysym = XKB_KEY_a, .modifiers = MODIFIER_SUPER,
		.action = ACTION_EXEC, .command = "fuzzel"},
	{.keysym = XKB_KEY_Tab, .modifiers = MODIFIER_SUPER,
		.action = ACTION_TOGGLE_MAXIMIZE},
	{.keysym = XKB_KEY_f, .modifiers = MODIFIER_SUPER,
		.action = ACTION_TOGGLE_FULLSCREEN},
	{.keysym = XKB_KEY_Tab, .modifiers = MODIFIER_ALT, .action = ACTION_NEXT_WINDOW},
	{.keysym = XKB_KEY_Tab, .modifiers = MODIFIER_ALT | MODIFIER_SHIFT,
		.action = ACTION_PREVIOUS_WINDOW},
	{.keysym = XKB_KEY_Right, .modifiers = MODIFIER_SUPER | MODIFIER_CTRL,
		.action = ACTION_WORKSPACE, .workspace = WORKSPACE_NEXT},
	{.keysym = XKB_KEY_Left, .modifiers = MODIFIER_SUPER | MODIFIER_CTRL,
		.action = ACTION_WORKSPACE, .workspace = WORKSPACE_PREVIOUS},
	{.keysym = XKB_KEY_Right,
		.modifiers = MODIFIER_SUPER | MODIFIER_CTRL | MODIFIER_SHIFT,
		.action = ACTION_MOVE_TO_WORKSPACE, .workspace = WORKSPACE_NEXT},
	{.keysym = XKB_KEY_Left,
		.modifiers = MODIFIER_SUPER | MODIFIER_CTRL | MODIFIER_SHIFT,
		.action = ACTION_MOVE_TO_WORKSPACE, .workspace = WORKSPACE_PREVIOUS},
	{.keysym = XKB_KEY_0, .modifiers = MODIFIER_SUPER,
		.action = ACTION_TOGGLE_MAGNIFIER},
	{.keysym = XKB_KEY_equal, .modifiers = MODIFIER_SUPER, .action = ACTION_ZOOM_IN},
	{.keysym = XKB_KEY_minus, .modifiers = MODIFIER_SUPER,
		.action = ACTION_ZOOM_OUT},
};

static const char *const default_workspace_names[] = {"1", "2", "3", "4"};

static const struct mouse_binding default_mouse_bindings[] = {
	{.button = BTN_LEFT, .modifiers = MODIFIER_SUPER, .action = MOUSE_ACTION_MOVE},
	{.button = BTN_RIGHT, .modifiers = MODIFIER_SUPER,
		.action = MOUSE_ACTION_RESIZE},
};

// Unset keys keep these defaults, which config.yaml repeats.
static const struct config defaults = {
	.backend = {
		.xwayland = true,
	},
	.keyboard = {
		.repeat_rate = 25,
		.repeat_delay = 600,
	},
	.cursor = {
		.size = 24,
	},
	.desktop = {
		.background_color = RGBA(0x808080ff),
	},
	.windows = {
		.default_ssd = true,
		.focus_follows_mouse = true,
	},
	.snapping = {
		.edge_range = 10,
		.corner_range = 50,
		.unsnap_threshold = 10,
		.preview_color = RGBA(0x3584e44d),
	},
	.night_light = {
		.temperature = 6500,
		.start = 20 * 60,
		.end = 6 * 60,
		.transition = 30,
	},
	.magnifier = {
		.width = 200,
		.height = 200,
		.zoom_factor = 1.2,
	},
	.decorations = {
		.border_width = 2,
		.titlebar_height = 28,
		.corner_radius = 8,
		.resize_margin = 8,
		.shadow_blur = 16,
		.shadow_offset_y = 4,
		.font = "Sans 11",
		.app_icon_size = 20,
		.icon_theme = "hicolor",
		.colors = {
			.active = {
				.border = RGBA(0x999999ff),
				.titlebar = RGBA(0xf2f2f2ff),
				.title = RGBA(0x141414ff),
				.shadow = RGBA(0x00000066),
			},
			.inactive = {
				.border = RGBA(0xbfbfbfff),
				.titlebar = RGBA(0xfafafaff),
				.title = RGBA(0x8c8c8cff),
				.shadow = RGBA(0x00000033),
			},
		},
		.titlebar = {
			.layout = {
				.left = {TITLEBAR_ITEM_APP_ICON, TITLEBAR_ITEM_STICKY},
				.nr_left = 2,
				.right = {
					TITLEBAR_ITEM_MINIMIZE,
					TITLEBAR_ITEM_MAXIMIZE,
					TITLEBAR_ITEM_CLOSE,
				},
				.nr_right = 3,
				.show_title = true,
			},
			.button_width = 32,
			.icon_size = 10,
			// The button styles are set by default_button_rules.
		},
	},
	.window_switcher = {
		.thumbnail_width = 240,
		.thumbnail_height = 160,
		.app_icon_size = 48,
		.font = "Sans 10",
		.title_height = 24,
		.padding = 10,
		.corner_radius = 8,
		.border_width = 2,
		.outline_width = 3,
		.close_button_size = 28,
		.close_icon_size = 14,
		.colors = {
			.background = RGBA(0xf2f2f2e6),
			.selected_background = RGBA(0x0000001a),
			.selected_border = RGBA(0x999999ff),
			.title = RGBA(0x141414ff),
			.outline = RGBA(0x3584e4ff),
		},
	},
};

struct config config;

// The members are -1 for any.
struct button_target {
	int type; // enum button_type
	int state; // enum button_state
	int activated; // bool
};

// Sets the properties it has, which are not NULL, of the button styles that
// the target matches.
struct button_rule {
	struct button_target target;
	const char *icon;
	const struct color *background_color;
	const struct color *icon_color;
};

static const struct color dark_icon_color = RGBA(0x1f2629ff);
static const struct color white_icon_color = RGBA(0xffffffff);

// The configured rules are applied over these, which config.yaml repeats.
static const struct button_rule default_button_rules[] = {
	// A pin, whose head is filled while the window is sticky
	{{BUTTON_STICKY, -1, -1}, .icon =
		"<svg xmlns='http://www.w3.org/2000/svg' viewBox='-5 -5 10 10'>"
		"<circle cx='-0.5' cy='-1.5' r='3' fill='none' stroke='currentColor'/>"
		"<path d='M-0.5 1.5V4.5' stroke='currentColor'"
		" stroke-linecap='square'/></svg>"},
	{{BUTTON_UNSTICK, -1, -1}, .icon =
		"<svg xmlns='http://www.w3.org/2000/svg' viewBox='-5 -5 10 10'>"
		"<circle cx='-0.5' cy='-1.5' r='3' fill='currentColor'"
		" stroke='currentColor'/>"
		"<path d='M-0.5 1.5V4.5' stroke='currentColor'"
		" stroke-linecap='square'/></svg>"},
	{{BUTTON_MINIMIZE, -1, -1}, .icon =
		"<svg xmlns='http://www.w3.org/2000/svg' viewBox='-5 -5 10 10'>"
		"<path d='M-4.5 -0.5H4.5' fill='none' stroke='currentColor'"
		" stroke-linecap='square'/></svg>"},
	{{BUTTON_MAXIMIZE, -1, -1}, .icon =
		"<svg xmlns='http://www.w3.org/2000/svg' viewBox='-5 -5 10 10'>"
		"<path d='M-4.5 -4.5H4.5V4.5H-4.5Z' fill='none' stroke='currentColor'"
		" stroke-linecap='square'/></svg>"},
	// A square with another one behind it
	{{BUTTON_MAXIMIZE_RESTORE, -1, -1}, .icon =
		"<svg xmlns='http://www.w3.org/2000/svg' viewBox='-5 -5 10 10'>"
		"<path d='M-4.5 -2.5H2.5V4.5H-4.5ZM-2.5 -2.5V-4.5H4.5V2.5H2.5'"
		" fill='none' stroke='currentColor' stroke-linecap='square'/></svg>"},
	{{BUTTON_CLOSE, -1, -1}, .icon =
		"<svg xmlns='http://www.w3.org/2000/svg' viewBox='-5 -5 10 10'>"
		"<path d='M-4.5 -4.5L4.5 4.5M4.5 -4.5L-4.5 4.5' fill='none'"
		" stroke='currentColor' stroke-linecap='square'/></svg>"},
	{{-1, BUTTON_STATE_NORMAL, -1},
		.background_color = &(const struct color)RGBA(0x00000000),
		.icon_color = &dark_icon_color},
	{{-1, BUTTON_STATE_NORMAL, false},
		.icon_color = &(const struct color)RGBA(0x8c8c8cff)},
	{{-1, BUTTON_STATE_HOVERED, -1},
		.background_color = &(const struct color)RGBA(0x0000001a),
		.icon_color = &dark_icon_color},
	{{-1, BUTTON_STATE_PRESSED, -1},
		.background_color = &(const struct color)RGBA(0x00000033),
		.icon_color = &dark_icon_color},
	{{BUTTON_CLOSE, BUTTON_STATE_HOVERED, -1},
		.background_color = &(const struct color)RGBA(0xc42b1cff),
		.icon_color = &white_icon_color},
	{{BUTTON_CLOSE, BUTTON_STATE_PRESSED, -1},
		.background_color = &(const struct color)RGBA(0x941f14ff),
		.icon_color = &white_icon_color},
};

static void
apply_button_rule(const struct button_rule *rule)
{
	const struct button_target *target = &rule->target;
	for (int type = 0; type < NR_BUTTON_TYPES; type++) {
		for (int state = 0; state < NR_BUTTON_STATES; state++) {
			for (int activated = 0; activated < 2; activated++) {
				if ((target->type >= 0 && target->type != type)
						|| (target->state >= 0 && target->state != state)
						|| (target->activated >= 0
							&& target->activated != activated)) {
					continue;
				}
				struct button_style *style = &config.decorations.titlebar
					.button_styles[type][state][activated];
				if (rule->icon) {
					style->icon = rule->icon;
				}
				if (rule->background_color) {
					style->background_color = *rule->background_color;
				}
				if (rule->icon_color) {
					style->icon_color = *rule->icon_color;
				}
			}
		}
	}
}

// Reports the first error, which makes the config invalid. The parsing goes
// on, with values returned for the errors, until the config is discarded.
static void
fail(const yaml_node_t *node, const char *format, ...)
{
	if (config.invalid) {
		return;
	}
	config.invalid = true;
	fprintf(stderr, "%s:%zu: ", config.path, node->start_mark.line + 1);
	va_list args;
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
	fputc('\n', stderr);
}

static yaml_node_t *
get_node(int index)
{
	yaml_node_t *node = yaml_document_get_node(config.document, index);
	assert(node);
	return node;
}

static bool
expect_mapping(yaml_node_t *node)
{
	if (node->type != YAML_MAPPING_NODE) {
		fail(node, "expected a mapping");
		return false;
	}
	return true;
}

// 0 if the node is not a sequence
static size_t
sequence_length(yaml_node_t *node)
{
	if (node->type != YAML_SEQUENCE_NODE) {
		fail(node, "expected a sequence");
		return 0;
	}
	return (size_t)(node->data.sequence.items.top - node->data.sequence.items.start);
}

static yaml_node_t *
sequence_item(yaml_node_t *node, size_t index)
{
	return get_node(node->data.sequence.items.start[index]);
}

static const char *
parse_string(yaml_node_t *node)
{
	if (node->type != YAML_SCALAR_NODE) {
		fail(node, "expected a scalar");
		return "";
	}
	return (const char *)node->data.scalar.value;
}

// Returns NULL for null, ~ or an empty value.
static const char *
parse_optional_string(yaml_node_t *node)
{
	const char *value = parse_string(node);
	if (node->type != YAML_SCALAR_NODE) {
		return NULL;
	}
	if (node->data.scalar.style == YAML_PLAIN_SCALAR_STYLE
			&& (strcmp(value, "null") == 0 || strcmp(value, "~") == 0
				|| value[0] == '\0')) {
		return NULL;
	}
	return value;
}

static bool
parse_bool(yaml_node_t *node)
{
	const char *value = parse_string(node);
	if (strcmp(value, "true") == 0) {
		return true;
	}
	if (strcmp(value, "false") != 0) {
		fail(node, "expected true or false");
	}
	return false;
}

static int
parse_int(yaml_node_t *node, int min)
{
	const char *value = parse_string(node);
	char *end;
	errno = 0;
	long result = strtol(value, &end, 10);
	if (end == value || *end != '\0' || errno || result < min || result > INT_MAX) {
		fail(node, "expected an integer of at least %d", min);
		return min;
	}
	return (int)result;
}

static double
parse_double(yaml_node_t *node)
{
	const char *value = parse_string(node);
	char *end;
	double result = strtod(value, &end);
	if (end == value || *end != '\0' || !isfinite(result)) {
		fail(node, "expected a number");
		return 0.0;
	}
	return result;
}

static double
parse_positive_double(yaml_node_t *node)
{
	double result = parse_double(node);
	if (!(result > 0.0)) {
		fail(node, "expected a positive number");
		return 1.0;
	}
	return result;
}

// [a, b]
static void
parse_int_pair(yaml_node_t *node, int min, int *a, int *b)
{
	if (sequence_length(node) != 2) {
		fail(node, "expected a sequence of two integers");
		return;
	}
	*a = parse_int(sequence_item(node, 0), min);
	*b = parse_int(sequence_item(node, 1), min);
}

struct named_value {
	const char *name;
	int value;
};

// Returns the value of the name that the node is.
static int
parse_named_value(yaml_node_t *node, const struct named_value values[],
	size_t nr_values)
{
	const char *value = parse_string(node);
	for (size_t i = 0; i < nr_values; i++) {
		if (strcmp(value, values[i].name) == 0) {
			return values[i].value;
		}
	}
	// "expected a, b or c"
	char expected[256] = "expected";
	size_t length = strlen(expected);
	for (size_t i = 0; i < nr_values; i++) {
		const char *separator = i == 0 ? " " : i + 1 < nr_values ? ", " : " or ";
		length += (size_t)snprintf(expected + length,
			sizeof(expected) - length, "%s%s", separator, values[i].name);
		assert(length < sizeof(expected));
	}
	fail(node, "%s", expected);
	return values[0].value;
}

// "#rrggbb" or "#rrggbbaa"
static struct color
parse_color(yaml_node_t *node)
{
	const char *value = parse_string(node);
	size_t length = strlen(value);
	char *end;
	unsigned long rgba = strtoul(value + 1, &end, 16);
	if (value[0] != '#' || (length != 7 && length != 9) || *end != '\0') {
		fail(node, "expected a color of the form #rrggbb or #rrggbbaa");
		return (struct color){0};
	}
	if (length == 7) {
		rgba = rgba << 8 | 0xff;
	}
	return (struct color)RGBA(rgba);
}

static void
parse_backend(yaml_node_t *node)
{
	if (!expect_mapping(node)) {
		return;
	}
	for (yaml_node_pair_t *pair = node->data.mapping.pairs.start;
			pair < node->data.mapping.pairs.top; pair++) {
		yaml_node_t *key = get_node(pair->key);
		yaml_node_t *value = get_node(pair->value);
		const char *name = parse_string(key);
		if (strcmp(name, "xwayland") == 0) {
			config.backend.xwayland = parse_bool(value);
		} else if (strcmp(name, "xwayland_persistence") == 0) {
			config.backend.xwayland_persistence = parse_bool(value);
		} else {
			fail(key, "unknown key %s", name);
		}
	}
}

static void
parse_autostart(yaml_node_t *node)
{
	config.nr_autostart = sequence_length(node);
	config.autostart = calloc(config.nr_autostart, sizeof(*config.autostart));
	assert(config.autostart || config.nr_autostart == 0);
	for (size_t i = 0; i < config.nr_autostart; i++) {
		config.autostart[i] = parse_string(sequence_item(node, i));
	}
}

// "preferred", "WIDTHxHEIGHT" or "WIDTHxHEIGHT@REFRESH" in Hz
static void
parse_mode(yaml_node_t *node, struct output_config *output)
{
	const char *value = parse_string(node);
	if (strcmp(value, "preferred") == 0) {
		return;
	}
	int size_length = 0;
	bool valid = sscanf(value, "%dx%d%n", &output->width, &output->height,
		&size_length) == 2 && output->width > 0 && output->height > 0;
	const char *rest = value + size_length;
	double refresh = 0.0;
	if (valid && rest[0] == '@') {
		char *end;
		refresh = strtod(rest + 1, &end);
		valid = *end == '\0' && refresh > 0.0 && isfinite(refresh);
	} else {
		valid = valid && rest[0] == '\0';
	}
	if (!valid) {
		fail(node, "expected preferred or a mode such as 1920x1080@60");
	}
	output->refresh = (int)round(refresh * 1000.0);
}

static enum wl_output_transform
parse_transform(yaml_node_t *node)
{
	static const struct named_value transforms[] = {
		{"normal", WL_OUTPUT_TRANSFORM_NORMAL},
		{"90", WL_OUTPUT_TRANSFORM_90},
		{"180", WL_OUTPUT_TRANSFORM_180},
		{"270", WL_OUTPUT_TRANSFORM_270},
		{"flipped", WL_OUTPUT_TRANSFORM_FLIPPED},
		{"flipped-90", WL_OUTPUT_TRANSFORM_FLIPPED_90},
		{"flipped-180", WL_OUTPUT_TRANSFORM_FLIPPED_180},
		{"flipped-270", WL_OUTPUT_TRANSFORM_FLIPPED_270},
	};
	return parse_named_value(node, transforms, ARRAY_LEN(transforms));
}

static void
parse_output(yaml_node_t *node, struct output_config *output)
{
	*output = (struct output_config){
		.scale = 1.0,
		.sdr_luminance = DEFAULT_SDR_LUMINANCE,
	};
	if (!expect_mapping(node)) {
		return;
	}
	for (yaml_node_pair_t *pair = node->data.mapping.pairs.start;
			pair < node->data.mapping.pairs.top; pair++) {
		yaml_node_t *key = get_node(pair->key);
		yaml_node_t *value = get_node(pair->value);
		const char *name = parse_string(key);
		if (strcmp(name, "name") == 0) {
			output->name = parse_string(value);
		} else if (strcmp(name, "mode") == 0) {
			parse_mode(value, output);
		} else if (strcmp(name, "scale") == 0) {
			// fractional-scale-v1 tells clients the scale in 1/120ths.
			output->scale = round(parse_positive_double(value) * 120.0) / 120.0;
			if (output->scale == 0.0) {
				fail(value, "the scale is too small");
			}
		} else if (strcmp(name, "transform") == 0) {
			output->transform = parse_transform(value);
		} else if (strcmp(name, "position") == 0) {
			parse_int_pair(value, INT_MIN, &output->x, &output->y);
			output->positioned = true;
		} else if (strcmp(name, "adaptive_sync") == 0) {
			output->adaptive_sync = parse_bool(value);
		} else if (strcmp(name, "hdr") == 0) {
			output->hdr = parse_bool(value);
		} else if (strcmp(name, "sdr_luminance") == 0) {
			output->sdr_luminance = parse_positive_double(value);
		} else {
			fail(key, "unknown key %s", name);
		}
	}
	if (!output->name) {
		fail(node, "the output has no name");
	}
}

static void
parse_outputs(yaml_node_t *node)
{
	config.nr_outputs = sequence_length(node);
	config.outputs = calloc(config.nr_outputs, sizeof(*config.outputs));
	assert(config.outputs || config.nr_outputs == 0);
	for (size_t i = 0; i < config.nr_outputs; i++) {
		parse_output(sequence_item(node, i), &config.outputs[i]);
	}
}

static uint32_t
parse_mouse_button(yaml_node_t *node)
{
	static const struct named_value buttons[] = {
		{"Left", BTN_LEFT},
		{"Right", BTN_RIGHT},
		{"Middle", BTN_MIDDLE},
		{"Side", BTN_SIDE},
		{"Extra", BTN_EXTRA},
	};
	return (uint32_t)parse_named_value(node, buttons, ARRAY_LEN(buttons));
}

static void
parse_input(yaml_node_t *node, struct input_config *input)
{
	*input = (struct input_config){
		.type = -1,
		.send_events = -1,
		.tap = -1,
		.tap_button_map = -1,
		.tap_drag = -1,
		.tap_drag_lock = -1,
		.three_finger_drag = -1,
		.natural_scroll = -1,
		.left_handed = -1,
		.accel_speed = NAN,
		.accel_profile = -1,
		.middle_emulation = -1,
		.disable_while_typing = -1,
		.click_method = -1,
		.scroll_method = -1,
		.scroll_button = -1,
		.scroll_factor = 1.0,
	};
	if (!expect_mapping(node)) {
		return;
	}
	for (yaml_node_pair_t *pair = node->data.mapping.pairs.start;
			pair < node->data.mapping.pairs.top; pair++) {
		yaml_node_t *key = get_node(pair->key);
		yaml_node_t *value = get_node(pair->value);
		const char *name = parse_string(key);
		if (strcmp(name, "name") == 0) {
			input->name = parse_string(value);
		} else if (strcmp(name, "type") == 0) {
			static const struct named_value types[] = {
				{"keyboard", INPUT_TYPE_KEYBOARD},
				{"pointer", INPUT_TYPE_POINTER},
				{"touchpad", INPUT_TYPE_TOUCHPAD},
				{"touchscreen", INPUT_TYPE_TOUCHSCREEN},
				{"tablet", INPUT_TYPE_TABLET},
			};
			input->type = parse_named_value(value, types, ARRAY_LEN(types));
		} else if (strcmp(name, "send_events") == 0) {
			static const struct named_value modes[] = {
				{"enabled", LIBINPUT_CONFIG_SEND_EVENTS_ENABLED},
				{"disabled", LIBINPUT_CONFIG_SEND_EVENTS_DISABLED},
				{"disabled_on_external_mouse",
					LIBINPUT_CONFIG_SEND_EVENTS_DISABLED_ON_EXTERNAL_MOUSE},
			};
			input->send_events = parse_named_value(value, modes, ARRAY_LEN(modes));
		} else if (strcmp(name, "tap") == 0) {
			input->tap = parse_bool(value);
		} else if (strcmp(name, "tap_button_map") == 0) {
			static const struct named_value maps[] = {
				{"lrm", LIBINPUT_CONFIG_TAP_MAP_LRM},
				{"lmr", LIBINPUT_CONFIG_TAP_MAP_LMR},
			};
			input->tap_button_map = parse_named_value(value, maps, ARRAY_LEN(maps));
		} else if (strcmp(name, "tap_drag") == 0) {
			input->tap_drag = parse_bool(value);
		} else if (strcmp(name, "tap_drag_lock") == 0) {
			static const struct named_value states[] = {
				{"disabled", LIBINPUT_CONFIG_DRAG_LOCK_DISABLED},
				{"timeout", LIBINPUT_CONFIG_DRAG_LOCK_ENABLED_TIMEOUT},
				{"sticky", LIBINPUT_CONFIG_DRAG_LOCK_ENABLED_STICKY},
			};
			input->tap_drag_lock = parse_named_value(value, states,
				ARRAY_LEN(states));
		} else if (strcmp(name, "three_finger_drag") == 0) {
			static const struct named_value states[] = {
				{"disabled", LIBINPUT_CONFIG_3FG_DRAG_DISABLED},
				{"three_fingers", LIBINPUT_CONFIG_3FG_DRAG_ENABLED_3FG},
				{"four_fingers", LIBINPUT_CONFIG_3FG_DRAG_ENABLED_4FG},
			};
			input->three_finger_drag = parse_named_value(value, states,
				ARRAY_LEN(states));
		} else if (strcmp(name, "natural_scroll") == 0) {
			input->natural_scroll = parse_bool(value);
		} else if (strcmp(name, "left_handed") == 0) {
			input->left_handed = parse_bool(value);
		} else if (strcmp(name, "accel_speed") == 0) {
			input->accel_speed = parse_double(value);
			if (input->accel_speed < -1.0 || input->accel_speed > 1.0) {
				fail(value, "expected a number from -1 to 1");
			}
		} else if (strcmp(name, "accel_profile") == 0) {
			static const struct named_value profiles[] = {
				{"flat", LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT},
				{"adaptive", LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE},
			};
			input->accel_profile = parse_named_value(value, profiles,
				ARRAY_LEN(profiles));
		} else if (strcmp(name, "middle_emulation") == 0) {
			input->middle_emulation = parse_bool(value);
		} else if (strcmp(name, "disable_while_typing") == 0) {
			input->disable_while_typing = parse_bool(value);
		} else if (strcmp(name, "click_method") == 0) {
			static const struct named_value methods[] = {
				{"none", LIBINPUT_CONFIG_CLICK_METHOD_NONE},
				{"button_areas", LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS},
				{"clickfinger", LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER},
			};
			input->click_method = parse_named_value(value, methods,
				ARRAY_LEN(methods));
		} else if (strcmp(name, "scroll_method") == 0) {
			static const struct named_value methods[] = {
				{"none", LIBINPUT_CONFIG_SCROLL_NO_SCROLL},
				{"two_finger", LIBINPUT_CONFIG_SCROLL_2FG},
				{"edge", LIBINPUT_CONFIG_SCROLL_EDGE},
				{"on_button_down", LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN},
			};
			input->scroll_method = parse_named_value(value, methods,
				ARRAY_LEN(methods));
		} else if (strcmp(name, "scroll_button") == 0) {
			input->scroll_button = (int)parse_mouse_button(value);
		} else if (strcmp(name, "scroll_factor") == 0) {
			input->scroll_factor = parse_positive_double(value);
		} else if (strcmp(name, "map_to_output") == 0) {
			input->map_to_output = parse_string(value);
		} else {
			fail(key, "unknown key %s", name);
		}
	}
}

static void
parse_inputs(yaml_node_t *node)
{
	config.nr_inputs = sequence_length(node);
	config.inputs = calloc(config.nr_inputs, sizeof(*config.inputs));
	assert(config.inputs || config.nr_inputs == 0);
	for (size_t i = 0; i < config.nr_inputs; i++) {
		parse_input(sequence_item(node, i), &config.inputs[i]);
	}
}

static void
parse_keyboard(yaml_node_t *node)
{
	if (!expect_mapping(node)) {
		return;
	}
	for (yaml_node_pair_t *pair = node->data.mapping.pairs.start;
			pair < node->data.mapping.pairs.top; pair++) {
		yaml_node_t *key = get_node(pair->key);
		yaml_node_t *value = get_node(pair->value);
		const char *name = parse_string(key);
		if (strcmp(name, "layout") == 0) {
			config.keyboard.layout = parse_optional_string(value);
		} else if (strcmp(name, "variant") == 0) {
			config.keyboard.variant = parse_optional_string(value);
		} else if (strcmp(name, "options") == 0) {
			config.keyboard.options = parse_optional_string(value);
		} else if (strcmp(name, "repeat_rate") == 0) {
			config.keyboard.repeat_rate = parse_int(value, 0);
		} else if (strcmp(name, "repeat_delay") == 0) {
			config.keyboard.repeat_delay = parse_int(value, 0);
		} else {
			fail(key, "unknown key %s", name);
		}
	}

	// Invalid names fail here rather than when the keymap is created.
	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	assert(context);
	struct xkb_rule_names names = {
		.layout = config.keyboard.layout,
		.variant = config.keyboard.variant,
		.options = config.keyboard.options,
	};
	struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, &names,
		XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!keymap) {
		fail(node, "invalid layout, variant or options");
	}
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
}

static void
parse_cursor(yaml_node_t *node)
{
	if (!expect_mapping(node)) {
		return;
	}
	for (yaml_node_pair_t *pair = node->data.mapping.pairs.start;
			pair < node->data.mapping.pairs.top; pair++) {
		yaml_node_t *key = get_node(pair->key);
		yaml_node_t *value = get_node(pair->value);
		const char *name = parse_string(key);
		if (strcmp(name, "theme") == 0) {
			config.cursor.theme = parse_optional_string(value);
		} else if (strcmp(name, "size") == 0) {
			config.cursor.size = parse_int(value, 1);
		} else {
			fail(key, "unknown key %s", name);
		}
	}
}

static void
parse_desktop(yaml_node_t *node)
{
	if (!expect_mapping(node)) {
		return;
	}
	for (yaml_node_pair_t *pair = node->data.mapping.pairs.start;
			pair < node->data.mapping.pairs.top; pair++) {
		yaml_node_t *key = get_node(pair->key);
		yaml_node_t *value = get_node(pair->value);
		const char *name = parse_string(key);
		if (strcmp(name, "background_color") == 0) {
			config.desktop.background_color = parse_color(value);
		} else {
			fail(key, "unknown key %s", name);
		}
	}
}

static void
parse_windows(yaml_node_t *node)
{
	if (!expect_mapping(node)) {
		return;
	}
	for (yaml_node_pair_t *pair = node->data.mapping.pairs.start;
			pair < node->data.mapping.pairs.top; pair++) {
		yaml_node_t *key = get_node(pair->key);
		yaml_node_t *value = get_node(pair->value);
		const char *name = parse_string(key);
		if (strcmp(name, "default_decoration") == 0) {
			const char *decoration = parse_string(value);
			if (strcmp(decoration, "server") != 0
					&& strcmp(decoration, "client") != 0) {
				fail(value, "expected server or client");
			}
			config.windows.default_ssd = strcmp(decoration, "server") == 0;
		} else if (strcmp(name, "focus_follows_mouse") == 0) {
			config.windows.focus_follows_mouse = parse_bool(value);
		} else {
			fail(key, "unknown key %s", name);
		}
	}
}

static void
parse_workspaces(yaml_node_t *node)
{
	if (!expect_mapping(node)) {
		return;
	}
	for (yaml_node_pair_t *pair = node->data.mapping.pairs.start;
			pair < node->data.mapping.pairs.top; pair++) {
		yaml_node_t *key = get_node(pair->key);
		yaml_node_t *value = get_node(pair->value);
		const char *name = parse_string(key);
		if (strcmp(name, "names") == 0) {
			size_t nr_names = sequence_length(value);
			if (nr_names == 0) {
				fail(value, "expected at least one workspace");
				continue;
			}
			free(config.workspaces.names);
			config.workspaces.nr_names = nr_names;
			config.workspaces.names = calloc(nr_names,
				sizeof(*config.workspaces.names));
			assert(config.workspaces.names);
			for (size_t i = 0; i < nr_names; i++) {
				config.workspaces.names[i] = parse_string(sequence_item(value, i));
			}
		} else {
			fail(key, "unknown key %s", name);
		}
	}
}

static void
parse_snapping(yaml_node_t *node)
{
	if (!expect_mapping(node)) {
		return;
	}
	for (yaml_node_pair_t *pair = node->data.mapping.pairs.start;
			pair < node->data.mapping.pairs.top; pair++) {
		yaml_node_t *key = get_node(pair->key);
		yaml_node_t *value = get_node(pair->value);
		const char *name = parse_string(key);
		if (strcmp(name, "edge_range") == 0) {
			config.snapping.edge_range = parse_int(value, 0);
		} else if (strcmp(name, "corner_range") == 0) {
			config.snapping.corner_range = parse_int(value, 0);
		} else if (strcmp(name, "unsnap_threshold") == 0) {
			config.snapping.unsnap_threshold = parse_int(value, 0);
		} else if (strcmp(name, "preview_color") == 0) {
			config.snapping.preview_color = parse_color(value);
		} else {
			fail(key, "unknown key %s", name);
		}
	}
}

static void
parse_magnifier(yaml_node_t *node)
{
	if (!expect_mapping(node)) {
		return;
	}
	for (yaml_node_pair_t *pair = node->data.mapping.pairs.start;
			pair < node->data.mapping.pairs.top; pair++) {
		yaml_node_t *key = get_node(pair->key);
		yaml_node_t *value = get_node(pair->value);
		const char *name = parse_string(key);
		if (strcmp(name, "fullscreen") == 0) {
			config.magnifier.fullscreen = parse_bool(value);
		} else if (strcmp(name, "size") == 0) {
			parse_int_pair(value, 1, &config.magnifier.width,
				&config.magnifier.height);
		} else if (strcmp(name, "zoom_factor") == 0) {
			config.magnifier.zoom_factor = parse_double(value);
			if (!(config.magnifier.zoom_factor > 1.0)) {
				fail(value, "expected a number greater than 1");
			}
		} else {
			fail(key, "unknown key %s", name);
		}
	}
}

// "HH:MM", as minutes from midnight
static int
parse_time_of_day(yaml_node_t *node)
{
	const char *value = parse_string(node);
	int hour, minute, length = 0;
	if (sscanf(value, "%2d:%2d%n", &hour, &minute, &length) != 2
			|| value[length] != '\0' || hour < 0 || hour > 23
			|| minute < 0 || minute > 59) {
		fail(node, "expected a time of the form HH:MM");
		return 0;
	}
	return hour * 60 + minute;
}

static void
parse_night_light(yaml_node_t *node)
{
	if (!expect_mapping(node)) {
		return;
	}
	for (yaml_node_pair_t *pair = node->data.mapping.pairs.start;
			pair < node->data.mapping.pairs.top; pair++) {
		yaml_node_t *key = get_node(pair->key);
		yaml_node_t *value = get_node(pair->value);
		const char *name = parse_string(key);
		if (strcmp(name, "temperature") == 0) {
			config.night_light.temperature = parse_int(value, 1667);
			if (config.night_light.temperature > 6500) {
				fail(value, "expected at most 6500");
			}
		} else if (strcmp(name, "start") == 0) {
			config.night_light.start = parse_time_of_day(value);
		} else if (strcmp(name, "end") == 0) {
			config.night_light.end = parse_time_of_day(value);
		} else if (strcmp(name, "transition") == 0) {
			config.night_light.transition = parse_int(value, 1);
		} else {
			fail(key, "unknown key %s", name);
		}
	}
	int night = (config.night_light.end - config.night_light.start
		+ 24 * 60) % (24 * 60);
	if (night && config.night_light.transition > MIN(night, 24 * 60 - night)) {
		fail(node, "the transition is longer than the night or the day");
	}
}

static void
parse_titlebar_items(yaml_node_t *node,
	enum titlebar_item items[NR_TITLEBAR_ITEMS], int *nr_items)
{
	static const struct named_value names[] = {
		{"app_icon", TITLEBAR_ITEM_APP_ICON},
		{"sticky", TITLEBAR_ITEM_STICKY},
		{"minimize", TITLEBAR_ITEM_MINIMIZE},
		{"maximize", TITLEBAR_ITEM_MAXIMIZE},
		{"close", TITLEBAR_ITEM_CLOSE},
	};
	bool listed[NR_TITLEBAR_ITEMS] = {0};
	size_t length = sequence_length(node);
	for (size_t i = 0; i < length; i++) {
		yaml_node_t *item_node = sequence_item(node, i);
		enum titlebar_item item = parse_named_value(item_node, names,
			ARRAY_LEN(names));
		// Which also keeps the items within the array
		if (listed[item]) {
			fail(item_node, "%s is listed twice", parse_string(item_node));
			return;
		}
		listed[item] = true;
		items[i] = item;
	}
	*nr_items = (int)length;
}

// The keys left out keep the defaults, but each item is still listed at most
// once across both sides.
static void
parse_titlebar_layout(yaml_node_t *node)
{
	struct titlebar_layout_config *layout = &config.decorations.titlebar.layout;
	if (!expect_mapping(node)) {
		return;
	}
	for (yaml_node_pair_t *pair = node->data.mapping.pairs.start;
			pair < node->data.mapping.pairs.top; pair++) {
		yaml_node_t *key = get_node(pair->key);
		yaml_node_t *value = get_node(pair->value);
		const char *name = parse_string(key);
		if (strcmp(name, "left") == 0) {
			parse_titlebar_items(value, layout->left, &layout->nr_left);
		} else if (strcmp(name, "right") == 0) {
			parse_titlebar_items(value, layout->right, &layout->nr_right);
		} else if (strcmp(name, "show_title") == 0) {
			layout->show_title = parse_bool(value);
		} else {
			fail(key, "unknown key %s", name);
		}
	}

	bool left[NR_TITLEBAR_ITEMS] = {0};
	for (int i = 0; i < layout->nr_left; i++) {
		left[layout->left[i]] = true;
	}
	for (int i = 0; i < layout->nr_right; i++) {
		if (left[layout->right[i]]) {
			fail(node, "an item is listed in both sides");
		}
	}
}

static struct button_target
parse_button_target(yaml_node_t *node)
{
	struct button_target target = {-1, -1, -1};
	if (!expect_mapping(node)) {
		return target;
	}
	for (yaml_node_pair_t *pair = node->data.mapping.pairs.start;
			pair < node->data.mapping.pairs.top; pair++) {
		yaml_node_t *key = get_node(pair->key);
		yaml_node_t *value = get_node(pair->value);
		const char *name = parse_string(key);
		if (strcmp(name, "button") == 0) {
			static const struct named_value types[] = {
				{"sticky", BUTTON_STICKY},
				{"unstick", BUTTON_UNSTICK},
				{"minimize", BUTTON_MINIMIZE},
				{"maximize", BUTTON_MAXIMIZE},
				{"maximize_restore", BUTTON_MAXIMIZE_RESTORE},
				{"close", BUTTON_CLOSE},
			};
			target.type = parse_named_value(value, types, ARRAY_LEN(types));
		} else if (strcmp(name, "state") == 0) {
			static const struct named_value states[] = {
				{"normal", BUTTON_STATE_NORMAL},
				{"hovered", BUTTON_STATE_HOVERED},
				{"pressed", BUTTON_STATE_PRESSED},
			};
			target.state = parse_named_value(value, states, ARRAY_LEN(states));
		} else if (strcmp(name, "window_state") == 0) {
			static const struct named_value activations[] = {
				{"active", true},
				{"inactive", false},
			};
			target.activated = parse_named_value(value, activations,
				ARRAY_LEN(activations));
		} else {
			fail(key, "unknown key %s", name);
		}
	}
	return target;
}

// Malformed documents fail here rather than when the icons are drawn.
static const char *
parse_icon(yaml_node_t *node)
{
	const char *value = parse_string(node);
	GError *error = NULL;
	RsvgHandle *handle = rsvg_handle_new_from_data((const guint8 *)value,
		strlen(value), &error);
	if (!handle) {
		fail(node, "invalid SVG: %s", error->message);
		g_error_free(error);
		return NULL;
	}
	g_object_unref(handle);
	return value;
}

static void
parse_button_rule(yaml_node_t *node)
{
	struct button_rule rule = {.target = {-1, -1, -1}};
	struct color background_color;
	struct color icon_color;
	if (!expect_mapping(node)) {
		return;
	}
	for (yaml_node_pair_t *pair = node->data.mapping.pairs.start;
			pair < node->data.mapping.pairs.top; pair++) {
		yaml_node_t *key = get_node(pair->key);
		yaml_node_t *value = get_node(pair->value);
		const char *name = parse_string(key);
		if (strcmp(name, "target") == 0) {
			rule.target = parse_button_target(value);
		} else if (strcmp(name, "icon") == 0) {
			rule.icon = parse_icon(value);
		} else if (strcmp(name, "background_color") == 0) {
			background_color = parse_color(value);
			rule.background_color = &background_color;
		} else if (strcmp(name, "icon_color") == 0) {
			icon_color = parse_color(value);
			rule.icon_color = &icon_color;
		} else {
			fail(key, "unknown key %s", name);
		}
	}

	apply_button_rule(&rule);
}

static void
parse_titlebar(yaml_node_t *node)
{
	if (!expect_mapping(node)) {
		return;
	}
	for (yaml_node_pair_t *pair = node->data.mapping.pairs.start;
			pair < node->data.mapping.pairs.top; pair++) {
		yaml_node_t *key = get_node(pair->key);
		yaml_node_t *value = get_node(pair->value);
		const char *name = parse_string(key);
		if (strcmp(name, "layout") == 0) {
			parse_titlebar_layout(value);
		} else if (strcmp(name, "button_width") == 0) {
			config.decorations.titlebar.button_width = parse_int(value, 1);
		} else if (strcmp(name, "icon_size") == 0) {
			config.decorations.titlebar.icon_size = parse_int(value, 1);
		} else if (strcmp(name, "buttons") == 0) {
			// Later rules override earlier ones.
			size_t length = sequence_length(value);
			for (size_t i = 0; i < length; i++) {
				parse_button_rule(sequence_item(value, i));
			}
		} else {
			fail(key, "unknown key %s", name);
		}
	}
}

static void
parse_activation_colors(yaml_node_t *node, struct decoration_colors *colors)
{
	if (!expect_mapping(node)) {
		return;
	}
	for (yaml_node_pair_t *pair = node->data.mapping.pairs.start;
			pair < node->data.mapping.pairs.top; pair++) {
		yaml_node_t *key = get_node(pair->key);
		yaml_node_t *value = get_node(pair->value);
		const char *name = parse_string(key);
		if (strcmp(name, "border") == 0) {
			colors->border = parse_color(value);
		} else if (strcmp(name, "titlebar") == 0) {
			colors->titlebar = parse_color(value);
		} else if (strcmp(name, "title") == 0) {
			colors->title = parse_color(value);
		} else if (strcmp(name, "shadow") == 0) {
			colors->shadow = parse_color(value);
		} else {
			fail(key, "unknown key %s", name);
		}
	}
}

static void
parse_decoration_colors(yaml_node_t *node)
{
	if (!expect_mapping(node)) {
		return;
	}
	for (yaml_node_pair_t *pair = node->data.mapping.pairs.start;
			pair < node->data.mapping.pairs.top; pair++) {
		yaml_node_t *key = get_node(pair->key);
		yaml_node_t *value = get_node(pair->value);
		const char *name = parse_string(key);
		if (strcmp(name, "active") == 0) {
			parse_activation_colors(value, &config.decorations.colors.active);
		} else if (strcmp(name, "inactive") == 0) {
			parse_activation_colors(value, &config.decorations.colors.inactive);
		} else {
			fail(key, "unknown key %s", name);
		}
	}
}

static void
parse_decorations(yaml_node_t *node)
{
	if (!expect_mapping(node)) {
		return;
	}
	for (yaml_node_pair_t *pair = node->data.mapping.pairs.start;
			pair < node->data.mapping.pairs.top; pair++) {
		yaml_node_t *key = get_node(pair->key);
		yaml_node_t *value = get_node(pair->value);
		const char *name = parse_string(key);
		if (strcmp(name, "border_width") == 0) {
			config.decorations.border_width = parse_int(value, 0);
		} else if (strcmp(name, "titlebar_height") == 0) {
			config.decorations.titlebar_height = parse_int(value, 1);
		} else if (strcmp(name, "corner_radius") == 0) {
			config.decorations.corner_radius = parse_int(value, 0);
		} else if (strcmp(name, "resize_margin") == 0) {
			config.decorations.resize_margin = parse_int(value, 0);
		} else if (strcmp(name, "shadow_blur") == 0) {
			config.decorations.shadow_blur = parse_int(value, 0);
		} else if (strcmp(name, "shadow_offset") == 0) {
			parse_int_pair(value, INT_MIN, &config.decorations.shadow_offset_x,
				&config.decorations.shadow_offset_y);
		} else if (strcmp(name, "font") == 0) {
			config.decorations.font = parse_string(value);
		} else if (strcmp(name, "app_icon_size") == 0) {
			config.decorations.app_icon_size = parse_int(value, 1);
		} else if (strcmp(name, "icon_theme") == 0) {
			config.decorations.icon_theme = parse_string(value);
		} else if (strcmp(name, "colors") == 0) {
			parse_decoration_colors(value);
		} else if (strcmp(name, "titlebar") == 0) {
			parse_titlebar(value);
		} else {
			fail(key, "unknown key %s", name);
		}
	}
}

static void
parse_window_switcher_colors(yaml_node_t *node)
{
	if (!expect_mapping(node)) {
		return;
	}
	for (yaml_node_pair_t *pair = node->data.mapping.pairs.start;
			pair < node->data.mapping.pairs.top; pair++) {
		yaml_node_t *key = get_node(pair->key);
		yaml_node_t *value = get_node(pair->value);
		const char *name = parse_string(key);
		if (strcmp(name, "background") == 0) {
			config.window_switcher.colors.background = parse_color(value);
		} else if (strcmp(name, "selected_background") == 0) {
			config.window_switcher.colors.selected_background = parse_color(value);
		} else if (strcmp(name, "selected_border") == 0) {
			config.window_switcher.colors.selected_border = parse_color(value);
		} else if (strcmp(name, "title") == 0) {
			config.window_switcher.colors.title = parse_color(value);
		} else if (strcmp(name, "outline") == 0) {
			config.window_switcher.colors.outline = parse_color(value);
		} else {
			fail(key, "unknown key %s", name);
		}
	}
}

static void
parse_window_switcher(yaml_node_t *node)
{
	if (!expect_mapping(node)) {
		return;
	}
	for (yaml_node_pair_t *pair = node->data.mapping.pairs.start;
			pair < node->data.mapping.pairs.top; pair++) {
		yaml_node_t *key = get_node(pair->key);
		yaml_node_t *value = get_node(pair->value);
		const char *name = parse_string(key);
		if (strcmp(name, "thumbnail_size") == 0) {
			parse_int_pair(value, 1, &config.window_switcher.thumbnail_width,
				&config.window_switcher.thumbnail_height);
		} else if (strcmp(name, "app_icon_size") == 0) {
			config.window_switcher.app_icon_size = parse_int(value, 1);
		} else if (strcmp(name, "font") == 0) {
			config.window_switcher.font = parse_string(value);
		} else if (strcmp(name, "title_height") == 0) {
			config.window_switcher.title_height = parse_int(value, 1);
		} else if (strcmp(name, "padding") == 0) {
			config.window_switcher.padding = parse_int(value, 0);
		} else if (strcmp(name, "corner_radius") == 0) {
			config.window_switcher.corner_radius = parse_int(value, 0);
		} else if (strcmp(name, "border_width") == 0) {
			config.window_switcher.border_width = parse_int(value, 0);
		} else if (strcmp(name, "outline_width") == 0) {
			config.window_switcher.outline_width = parse_int(value, 1);
		} else if (strcmp(name, "close_button_size") == 0) {
			config.window_switcher.close_button_size = parse_int(value, 1);
		} else if (strcmp(name, "close_icon_size") == 0) {
			config.window_switcher.close_icon_size = parse_int(value, 1);
		} else if (strcmp(name, "colors") == 0) {
			parse_window_switcher_colors(value);
		} else {
			fail(key, "unknown key %s", name);
		}
	}
}

static uint32_t
parse_modifiers(yaml_node_t *node)
{
	uint32_t modifiers = 0;
	size_t length = sequence_length(node);
	for (size_t i = 0; i < length; i++) {
		yaml_node_t *item = sequence_item(node, i);
		const char *value = parse_string(item);
		if (strcmp(value, "Shift") == 0) {
			modifiers |= MODIFIER_SHIFT;
		} else if (strcmp(value, "Ctrl") == 0) {
			modifiers |= MODIFIER_CTRL;
		} else if (strcmp(value, "Alt") == 0) {
			modifiers |= MODIFIER_ALT;
		} else if (strcmp(value, "Super") == 0) {
			modifiers |= MODIFIER_SUPER;
		} else {
			fail(item, "expected Shift, Ctrl, Alt or Super");
		}
	}
	return modifiers;
}

static enum action
parse_action(yaml_node_t *node)
{
	static const struct named_value actions[] = {
		{"quit", ACTION_QUIT},
		{"reload", ACTION_RELOAD},
		{"exec", ACTION_EXEC},
		{"close", ACTION_CLOSE},
		{"toggle_fullscreen", ACTION_TOGGLE_FULLSCREEN},
		{"toggle_maximize", ACTION_TOGGLE_MAXIMIZE},
		{"toggle_sticky", ACTION_TOGGLE_STICKY},
		{"next_window", ACTION_NEXT_WINDOW},
		{"previous_window", ACTION_PREVIOUS_WINDOW},
		{"workspace", ACTION_WORKSPACE},
		{"move_to_workspace", ACTION_MOVE_TO_WORKSPACE},
		{"toggle_magnifier", ACTION_TOGGLE_MAGNIFIER},
		{"zoom_in", ACTION_ZOOM_IN},
		{"zoom_out", ACTION_ZOOM_OUT},
	};
	return parse_named_value(node, actions, ARRAY_LEN(actions));
}

// "next", "previous", or the number of the workspace from 1
static int
parse_binding_workspace(yaml_node_t *node)
{
	const char *value = parse_string(node);
	if (strcmp(value, "next") == 0) {
		return WORKSPACE_NEXT;
	}
	if (strcmp(value, "previous") == 0) {
		return WORKSPACE_PREVIOUS;
	}
	return parse_int(node, 1) - 1;
}

static void
parse_binding(yaml_node_t *node, struct binding *binding)
{
	*binding = (struct binding){.keysym = XKB_KEY_NoSymbol};
	bool has_action = false;
	bool has_workspace = false;
	if (!expect_mapping(node)) {
		return;
	}
	for (yaml_node_pair_t *pair = node->data.mapping.pairs.start;
			pair < node->data.mapping.pairs.top; pair++) {
		yaml_node_t *key = get_node(pair->key);
		yaml_node_t *value = get_node(pair->value);
		const char *name = parse_string(key);
		if (strcmp(name, "key") == 0) {
			binding->keysym = xkb_keysym_to_lower(xkb_keysym_from_name(
				parse_string(value), XKB_KEYSYM_CASE_INSENSITIVE));
			if (binding->keysym == XKB_KEY_NoSymbol) {
				fail(value, "unknown keysym %s", parse_string(value));
			}
		} else if (strcmp(name, "modifiers") == 0) {
			binding->modifiers = parse_modifiers(value);
		} else if (strcmp(name, "action") == 0) {
			binding->action = parse_action(value);
			has_action = true;
		} else if (strcmp(name, "command") == 0) {
			binding->command = parse_string(value);
		} else if (strcmp(name, "workspace") == 0) {
			binding->workspace = parse_binding_workspace(value);
			has_workspace = true;
		} else {
			fail(key, "unknown key %s", name);
		}
	}
	if (binding->keysym == XKB_KEY_NoSymbol) {
		fail(node, "the binding has no key");
	}
	if (!has_action) {
		fail(node, "the binding has no action");
	}
	if (binding->action == ACTION_EXEC && !binding->command) {
		fail(node, "the exec action has no command");
	}
	if ((binding->action == ACTION_WORKSPACE
			|| binding->action == ACTION_MOVE_TO_WORKSPACE)
			&& !has_workspace) {
		fail(node, "the workspace actions need a workspace");
	}
}

static void
parse_bindings(yaml_node_t *node)
{
	free(config.bindings);
	config.nr_bindings = sequence_length(node);
	config.bindings = calloc(config.nr_bindings, sizeof(*config.bindings));
	assert(config.bindings || config.nr_bindings == 0);
	for (size_t i = 0; i < config.nr_bindings; i++) {
		parse_binding(sequence_item(node, i), &config.bindings[i]);
	}
}

static enum mouse_action
parse_mouse_action(yaml_node_t *node)
{
	static const struct named_value actions[] = {
		{"move", MOUSE_ACTION_MOVE},
		{"resize", MOUSE_ACTION_RESIZE},
	};
	return parse_named_value(node, actions, ARRAY_LEN(actions));
}

static void
parse_mouse_binding(yaml_node_t *node, struct mouse_binding *binding)
{
	*binding = (struct mouse_binding){0};
	bool has_button = false;
	bool has_action = false;
	if (!expect_mapping(node)) {
		return;
	}
	for (yaml_node_pair_t *pair = node->data.mapping.pairs.start;
			pair < node->data.mapping.pairs.top; pair++) {
		yaml_node_t *key = get_node(pair->key);
		yaml_node_t *value = get_node(pair->value);
		const char *name = parse_string(key);
		if (strcmp(name, "button") == 0) {
			binding->button = parse_mouse_button(value);
			has_button = true;
		} else if (strcmp(name, "modifiers") == 0) {
			binding->modifiers = parse_modifiers(value);
		} else if (strcmp(name, "action") == 0) {
			binding->action = parse_mouse_action(value);
			has_action = true;
		} else {
			fail(key, "unknown key %s", name);
		}
	}
	if (!has_button) {
		fail(node, "the mouse binding has no button");
	}
	if (!has_action) {
		fail(node, "the mouse binding has no action");
	}
}

static void
parse_mouse_bindings(yaml_node_t *node)
{
	free(config.mouse_bindings);
	config.nr_mouse_bindings = sequence_length(node);
	config.mouse_bindings = calloc(config.nr_mouse_bindings,
		sizeof(*config.mouse_bindings));
	assert(config.mouse_bindings || config.nr_mouse_bindings == 0);
	for (size_t i = 0; i < config.nr_mouse_bindings; i++) {
		parse_mouse_binding(sequence_item(node, i), &config.mouse_bindings[i]);
	}
}

static void
parse_root(yaml_node_t *node)
{
	if (!expect_mapping(node)) {
		return;
	}
	for (yaml_node_pair_t *pair = node->data.mapping.pairs.start;
			pair < node->data.mapping.pairs.top; pair++) {
		yaml_node_t *key = get_node(pair->key);
		yaml_node_t *value = get_node(pair->value);
		const char *name = parse_string(key);
		if (strcmp(name, "backend") == 0) {
			parse_backend(value);
		} else if (strcmp(name, "autostart") == 0) {
			parse_autostart(value);
		} else if (strcmp(name, "outputs") == 0) {
			parse_outputs(value);
		} else if (strcmp(name, "inputs") == 0) {
			parse_inputs(value);
		} else if (strcmp(name, "keyboard") == 0) {
			parse_keyboard(value);
		} else if (strcmp(name, "cursor") == 0) {
			parse_cursor(value);
		} else if (strcmp(name, "desktop") == 0) {
			parse_desktop(value);
		} else if (strcmp(name, "windows") == 0) {
			parse_windows(value);
		} else if (strcmp(name, "workspaces") == 0) {
			parse_workspaces(value);
		} else if (strcmp(name, "snapping") == 0) {
			parse_snapping(value);
		} else if (strcmp(name, "night_light") == 0) {
			parse_night_light(value);
		} else if (strcmp(name, "magnifier") == 0) {
			parse_magnifier(value);
		} else if (strcmp(name, "decorations") == 0) {
			parse_decorations(value);
		} else if (strcmp(name, "window_switcher") == 0) {
			parse_window_switcher(value);
		} else if (strcmp(name, "bindings") == 0) {
			parse_bindings(value);
		} else if (strcmp(name, "mouse_bindings") == 0) {
			parse_mouse_bindings(value);
		} else {
			fail(key, "unknown key %s", name);
		}
	}
}

// Resets the config to the defaults, keeping the path.
static void
set_defaults(void)
{
	char *path = config.path;
	config = defaults;
	config.path = path;
	// Copied to be freed like the configured bindings which replace them.
	config.nr_bindings = ARRAY_LEN(default_bindings);
	config.bindings = malloc(sizeof(default_bindings));
	assert(config.bindings);
	memcpy(config.bindings, default_bindings, sizeof(default_bindings));
	config.nr_mouse_bindings = ARRAY_LEN(default_mouse_bindings);
	config.mouse_bindings = malloc(sizeof(default_mouse_bindings));
	assert(config.mouse_bindings);
	memcpy(config.mouse_bindings, default_mouse_bindings,
		sizeof(default_mouse_bindings));
	config.workspaces.nr_names = ARRAY_LEN(default_workspace_names);
	config.workspaces.names = malloc(sizeof(default_workspace_names));
	assert(config.workspaces.names);
	memcpy(config.workspaces.names, default_workspace_names,
		sizeof(default_workspace_names));

	for (size_t i = 0; i < ARRAY_LEN(default_button_rules); i++) {
		apply_button_rule(&default_button_rules[i]);
	}
	// Empty, without the file
	config.document = calloc(1, sizeof(*config.document));
	assert(config.document);
}

// Parses the file over the defaults. Returns false if the file is invalid or
// missing, leaving the config partly parsed.
static bool
load(void)
{
	set_defaults();
	FILE *file = fopen(config.path, "r");
	if (!file) {
		fprintf(stderr, "%s: %s\n", config.path, strerror(errno));
		return false;
	}
	yaml_parser_t parser;
	assert(yaml_parser_initialize(&parser));
	yaml_parser_set_input_file(&parser, file);
	bool loaded = yaml_parser_load(&parser, config.document);
	if (!loaded) {
		fprintf(stderr, "%s:%zu: %s\n", config.path,
			parser.problem_mark.line + 1, parser.problem);
	}
	yaml_parser_delete(&parser);
	fclose(file);
	if (!loaded) {
		return false;
	}

	yaml_node_t *root = yaml_document_get_root_node(config.document);
	if (root) {
		parse_root(root);
	}
	if (config.invalid) {
		return false;
	}
	// Checked once both are parsed, in either order.
	for (size_t i = 0; i < config.nr_bindings; i++) {
		const struct binding *binding = &config.bindings[i];
		if ((binding->action == ACTION_WORKSPACE
				|| binding->action == ACTION_MOVE_TO_WORKSPACE)
				&& binding->workspace >= (int)config.workspaces.nr_names) {
			fprintf(stderr, "%s: a binding goes to workspace %d of %zu\n",
				config.path, binding->workspace + 1, config.workspaces.nr_names);
			return false;
		}
	}
	return true;
}

// Frees what the config has allocated, except the path.
static void
free_config(struct config *c)
{
	free(c->autostart);
	free(c->outputs);
	free(c->inputs);
	free(c->bindings);
	free(c->mouse_bindings);
	free(c->workspaces.names);
	yaml_document_delete(c->document);
	free(c->document);
}

// Loads the file at the path, or $XDG_CONFIG_HOME/jimi/config.yaml by
// default. The defaults are used instead if the file is invalid or missing.
void
config_load(const char *path)
{
	if (path) {
		config.path = strdup(path);
	} else {
		const char *config_home = getenv("XDG_CONFIG_HOME");
		char default_path[PATH_MAX];
		int length = config_home && config_home[0]
			? snprintf(default_path, sizeof(default_path),
				"%s/jimi/config.yaml", config_home)
			: snprintf(default_path, sizeof(default_path),
				"%s/.config/jimi/config.yaml", getenv("HOME"));
		assert(length > 0 && (size_t)length < sizeof(default_path));
		config.path = strdup(default_path);
	}
	assert(config.path);
	if (!load()) {
		fprintf(stderr, "Using the default config\n");
		free_config(&config);
		set_defaults();
	}
}

// Loads the file again, or keeps the config if the file is invalid or
// missing.
bool
config_reload(void)
{
	struct config old = config;
	if (!load()) {
		fprintf(stderr, "Keeping the previous config\n");
		free_config(&config);
		config = old;
		return false;
	}
	free_config(&old);
	return true;
}

void
config_fini(void)
{
	free_config(&config);
	free(config.path);
}

const struct output_config *
config_find_output(const char *name)
{
	for (size_t i = 0; i < config.nr_outputs; i++) {
		if (strcmp(config.outputs[i].name, name) == 0) {
			return &config.outputs[i];
		}
	}
	return NULL;
}

const struct input_config *
config_find_input(const char *name, enum input_type type)
{
	for (size_t i = 0; i < config.nr_inputs; i++) {
		const struct input_config *input = &config.inputs[i];
		if ((!input->name || strcmp(input->name, name) == 0)
				&& (input->type < 0 || input->type == (int)type)) {
			return input;
		}
	}
	return NULL;
}
