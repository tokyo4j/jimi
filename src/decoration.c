#include <assert.h>

#include "config.h"
#include "core.h"
#include "output.h"

#define TITLEBAR_PADDING 8

static struct texture *
create_button_icon(const struct button_style *style, double scale)
{
	int size = config.decorations.titlebar.icon_size;
	return texture_create_svg(style->icon, size, size, scale, style->icon_color);
}

// The icons are shared by the decorations of the windows on each output,
// which draws them for its scale.
void
decoration_icons_create(struct output *output)
{
	for (int type = 0; type < NR_BUTTON_TYPES; type++) {
		for (int state = 0; state < NR_BUTTON_STATES; state++) {
			for (int activated = 0; activated < 2; activated++) {
				output->button_icons[type][state][activated] =
					create_button_icon(
						&config.decorations.titlebar.button_styles[type][state][activated],
						output->scale);
			}
		}
	}
}

// The textures stay alive until the GPU is done with them.
void
decoration_icons_destroy(struct output *output)
{
	for (int type = 0; type < NR_BUTTON_TYPES; type++) {
		for (int state = 0; state < NR_BUTTON_STATES; state++) {
			for (int activated = 0; activated < 2; activated++) {
				texture_destroy(output->button_icons[type][state][activated]);
			}
		}
	}
}

// The app icon has the same margin on each side, including the title.
static int
app_icon_margin(void)
{
	return (config.decorations.titlebar_height
		- config.decorations.app_icon_size) / 2;
}

// Maximized windows have no borders, so that their titlebar and content fill
// the usable area of the output.
static int
border_width(bool maximized)
{
	return maximized ? 0 : config.decorations.border_width;
}

// Maximized and snapped windows have neither rounded corners nor shadows, so
// that their frames meet the edges of the usable area and each other without
// gaps, and their shadows fall on no panels and windows next to them.
static float
frame_radius(const struct window_state *state)
{
	return window_state_floating(state)
		? (float)config.decorations.corner_radius : 0.0f;
}

// The titlebar follows the rounded corners of the frame inside the border.
static float
titlebar_radius(const struct window_state *state)
{
	return MAX(frame_radius(state) - (float)border_width(state->maximized), 0.0f);
}

// The layout is relative to the top-left corner of the content, where the
// window tree is.
static struct box
frame_box(struct window *window)
{
	return decoration_frame_box((struct box){
		.width = window->current.geometry.width,
		.height = window->current.geometry.height,
	}, window->current.maximized);
}

static struct box
titlebar_box(struct window *window)
{
	return (struct box){
		.y = -config.decorations.titlebar_height,
		.width = window->current.geometry.width,
		.height = config.decorations.titlebar_height,
	};
}

// The decoration parts of the buttons
static const enum decoration_part button_parts[NR_TITLEBAR_BUTTONS] = {
	[TITLEBAR_ITEM_STICKY] = DECORATION_PART_STICKY,
	[TITLEBAR_ITEM_MINIMIZE] = DECORATION_PART_MINIMIZE,
	[TITLEBAR_ITEM_MAXIMIZE] = DECORATION_PART_MAXIMIZE,
	[TITLEBAR_ITEM_CLOSE] = DECORATION_PART_CLOSE,
};

// The window may have no app icon.
static int
item_width(struct window *window, enum titlebar_item item)
{
	if (item == TITLEBAR_ITEM_APP_ICON) {
		return window->decoration.icon.texture
			? config.decorations.app_icon_size + app_icon_margin() * 2 : 0;
	}
	return config.decorations.titlebar.button_width;
}

struct titlebar_layout {
	struct box title; // The space for the title
	// By enum titlebar_item, empty for the items not in the layout or left out
	struct box items[NR_TITLEBAR_ITEMS];
	// The items shown at the left and right ends, which follow the corners of
	// the titlebar, or NR_TITLEBAR_ITEMS if none.
	enum titlebar_item left_end;
	enum titlebar_item right_end;
};

// Lays the items out at the ends of the titlebar, and leaves the space between
// them for the title. The title is padded from the items except the app icon,
// which has its own margin.
static struct titlebar_layout
get_titlebar_layout(struct window *window)
{
	const struct titlebar_layout_config *config_layout =
		&config.decorations.titlebar.layout;
	const enum titlebar_item *left_items = config_layout->left;
	const enum titlebar_item *right_items = config_layout->right;
	int nr_left = config_layout->nr_left;
	int nr_right = config_layout->nr_right;
	struct box titlebar = titlebar_box(window);
	struct titlebar_layout layout = {
		.left_end = NR_TITLEBAR_ITEMS,
		.right_end = NR_TITLEBAR_ITEMS,
	};
	int widths[NR_TITLEBAR_ITEMS] = {0};
	int total_width = 0;
	for (int i = 0; i < nr_left; i++) {
		widths[left_items[i]] = item_width(window, left_items[i]);
		total_width += widths[left_items[i]];
	}
	for (int i = 0; i < nr_right; i++) {
		widths[right_items[i]] = item_width(window, right_items[i]);
		total_width += widths[right_items[i]];
	}
	// Left out in this order until the rest fit
	static const enum titlebar_item dropped_items[] = {
		TITLEBAR_ITEM_APP_ICON,
		TITLEBAR_ITEM_STICKY,
		TITLEBAR_ITEM_MINIMIZE,
		TITLEBAR_ITEM_MAXIMIZE,
		TITLEBAR_ITEM_CLOSE,
	};
	for (size_t i = 0; i < ARRAY_LEN(dropped_items)
			&& total_width > titlebar.width; i++) {
		total_width -= widths[dropped_items[i]];
		widths[dropped_items[i]] = 0;
	}

	// The items shown next to the title at each side
	enum titlebar_item left_inner = NR_TITLEBAR_ITEMS;
	enum titlebar_item right_inner = NR_TITLEBAR_ITEMS;
	int left = titlebar.x;
	for (int i = 0; i < nr_left; i++) {
		enum titlebar_item item = left_items[i];
		if (widths[item] > 0) {
			if (layout.left_end == NR_TITLEBAR_ITEMS) {
				layout.left_end = item;
			}
			left_inner = item;
		}
		layout.items[item] = (struct box){left, titlebar.y, widths[item],
			titlebar.height};
		left += widths[item];
	}
	int right = titlebar.x + titlebar.width;
	for (int i = nr_right - 1; i >= 0; i--) {
		enum titlebar_item item = right_items[i];
		if (widths[item] > 0) {
			if (layout.right_end == NR_TITLEBAR_ITEMS) {
				layout.right_end = item;
			}
			right_inner = item;
		}
		right -= widths[item];
		layout.items[item] = (struct box){right, titlebar.y, widths[item],
			titlebar.height};
	}
	left += left_inner == TITLEBAR_ITEM_APP_ICON ? 0 : TITLEBAR_PADDING;
	right -= right_inner == TITLEBAR_ITEM_APP_ICON ? 0 : TITLEBAR_PADDING;
	// The title has no space if the padding does not fit.
	layout.title = (struct box){left, titlebar.y, MAX(right - left, 0),
		titlebar.height};
	return layout;
}

static const struct decoration_colors *
activation_colors(struct window *window)
{
	return window->activated ? &config.decorations.colors.active
		: &config.decorations.colors.inactive;
}

// Windows are decorated once they have a size, for the scale of their
// output. Windows have no output while no output is enabled.
static bool
decoration_visible(struct window *window)
{
	return window_decorated(window) && window->output
		&& !box_empty(&window->current.geometry);
}

void
decoration_init(struct window *window)
{
	struct decoration *decoration = &window->decoration;
	scene_tree_init(&decoration->tree, &window->tree);
	scene_node_set_enabled(&decoration->tree.node, false);
	// The colors and the radii are set by decoration_update().
	struct color color = {0};
	scene_shadow_init(&decoration->shadow, &decoration->tree, color,
		config.decorations.shadow_blur, config.decorations.shadow_offset_x,
		config.decorations.shadow_offset_y, CORNER_TOP_LEFT | CORNER_TOP_RIGHT);
	scene_rect_init(&decoration->border_top, &decoration->tree, color, 0.0f,
		CORNER_TOP_LEFT | CORNER_TOP_RIGHT);
	scene_rect_init(&decoration->border_left, &decoration->tree, color, 0.0f, 0);
	scene_rect_init(&decoration->border_right, &decoration->tree, color, 0.0f, 0);
	scene_rect_init(&decoration->border_bottom, &decoration->tree, color, 0.0f, 0);
	scene_rect_init(&decoration->titlebar, &decoration->tree, color, 0.0f,
		CORNER_TOP_LEFT | CORNER_TOP_RIGHT);
	scene_texture_init(&decoration->icon, &decoration->tree);
	scene_node_set_enabled(&decoration->icon.node, false);
	scene_texture_init(&decoration->title, &decoration->tree);
	scene_node_set_enabled(&decoration->title.node, false);
	// The colors of the buttons are set by decoration_update_buttons(), and
	// their corners by decoration_update(), which hides those not in the
	// layout.
	for (int item = 0; item < NR_TITLEBAR_BUTTONS; item++) {
		scene_rect_init(&decoration->buttons[item], &decoration->tree, color,
			0.0f, 0);
		scene_texture_init(&decoration->button_icons[item], &decoration->tree);
	}
}

void
decoration_fini(struct window *window)
{
	struct decoration *decoration = &window->decoration;
	texture_destroy(decoration->icon.texture);
	texture_destroy(decoration->title.texture);
	// The nodes in the tree go away along with it.
	scene_node_detach(&decoration->tree.node);
}

static void
place_rect(struct scene_rect *rect, struct box box)
{
	scene_node_set_position(&rect->node, box.x, box.y);
	scene_rect_set_size(rect, box.width, box.height);
}

// Looks the icon up and renders it again when it is outdated or the scale
// changes. The window may have no icon.
static void
update_icon_texture(struct window *window)
{
	struct scene_texture *icon = &window->decoration.icon;
	double scale = window->output->scale;
	if (window->decoration.icon_scale == scale) {
		return;
	}
	window->decoration.icon_scale = scale;
	texture_destroy(icon->texture);
	int size = config.decorations.app_icon_size;
	scene_texture_set_texture(icon, window_create_icon(window, size, scale),
		size, size);
}

// The title is centered in the titlebar, but slides away from the items at
// the sides, and is shortened only when it does not fit between them. It is
// measured and rendered again when it is outdated or the scale changes, and
// rendered again when it is shortened differently or its color changes.
static void
place_title(struct window *window, struct box titlebar, struct box space)
{
	struct decoration *decoration = &window->decoration;
	struct scene_texture *title = &decoration->title;
	double scale = window->output->scale;
	bool outdated = decoration->title_scale != scale;
	if (outdated) {
		bool shown = config.decorations.titlebar.layout.show_title
			&& window->title && window->title[0] != '\0';
		decoration->title_scale = scale;
		decoration->title_width = shown
			? text_width(window->title, config.decorations.font, scale) : 0;
	}
	int width = MIN(decoration->title_width, space.width);
	int x = titlebar.x + (titlebar.width - width) / 2;
	x = MAX(MIN(x, space.x + space.width - width), space.x);
	scene_node_set_position(&title->node, x, space.y);
	if (!outdated && title->width == width
			&& decoration->title_activated == window->activated) {
		return;
	}
	decoration->title_activated = window->activated;
	texture_destroy(title->texture);
	int height = config.decorations.titlebar_height;
	struct texture *texture = NULL;
	if (width > 0) {
		texture = texture_create_text(window->title, config.decorations.font,
			activation_colors(window)->title, width, height, scale);
	}
	scene_texture_set_texture(title, texture, width, height);
	scene_node_set_enabled(&title->node, texture);
}

// Lays the decoration out around the committed size of the window, and
// shows the committed maximized state and the activation. The textures are
// for the scale of the output of the window.
void
decoration_update(struct window *window)
{
	struct decoration *decoration = &window->decoration;
	bool visible = decoration_visible(window);
	scene_node_set_enabled(&decoration->tree.node, visible);
	if (!visible) {
		return;
	}

	bool maximized = window->current.maximized;
	int width = window->current.geometry.width;
	int height = window->current.geometry.height;
	int border = border_width(maximized);
	struct box frame = frame_box(window);
	struct box titlebar = titlebar_box(window);
	float radius = frame_radius(&window->current);
	const struct decoration_colors *colors = activation_colors(window);
	scene_rect_set_color(&decoration->border_top, colors->border);
	scene_rect_set_color(&decoration->border_left, colors->border);
	scene_rect_set_color(&decoration->border_right, colors->border);
	scene_rect_set_color(&decoration->border_bottom, colors->border);
	scene_rect_set_color(&decoration->titlebar, colors->titlebar);
	scene_shadow_set_color(&decoration->shadow, colors->shadow);
	scene_node_set_enabled(&decoration->shadow.node,
		config.decorations.shadow_blur > 0
			&& window_state_floating(&window->current));
	scene_node_set_position(&decoration->shadow.node, frame.x, frame.y);
	scene_shadow_set_size(&decoration->shadow, frame.width, frame.height);
	scene_shadow_set_radius(&decoration->shadow, radius);
	// The top border extends behind the titlebar to round its corners.
	place_rect(&decoration->border_top, (struct box){
		.x = frame.x,
		.y = frame.y,
		.width = frame.width,
		.height = border + titlebar.height,
	});
	scene_rect_set_radius(&decoration->border_top, radius);
	place_rect(&decoration->titlebar, titlebar);
	scene_rect_set_radius(&decoration->titlebar, titlebar_radius(&window->current));
	place_rect(&decoration->border_left, (struct box){
		.x = frame.x,
		.y = 0,
		.width = border,
		.height = height,
	});
	place_rect(&decoration->border_right, (struct box){
		.x = width,
		.y = 0,
		.width = border,
		.height = height,
	});
	place_rect(&decoration->border_bottom, (struct box){
		.x = frame.x,
		.y = height,
		.width = frame.width,
		.height = border,
	});

	// The app icon takes space only if the window has one.
	update_icon_texture(window);
	struct titlebar_layout layout = get_titlebar_layout(window);
	place_title(window, titlebar, layout.title);
	struct box icon = layout.items[TITLEBAR_ITEM_APP_ICON];
	int icon_margin = app_icon_margin();
	scene_node_set_enabled(&decoration->icon.node, !box_empty(&icon));
	scene_node_set_position(&decoration->icon.node, icon.x + icon_margin,
		icon.y + icon_margin);
	for (enum titlebar_item item = 0; item < NR_TITLEBAR_BUTTONS; item++) {
		struct box box = layout.items[item];
		bool shown = !box_empty(&box);
		scene_node_set_enabled(&decoration->buttons[item].node, shown);
		scene_node_set_enabled(&decoration->button_icons[item].node, shown);
		place_rect(&decoration->buttons[item], box);
		scene_rect_set_corners(&decoration->buttons[item],
			(item == layout.left_end ? CORNER_TOP_LEFT : 0)
			| (item == layout.right_end ? CORNER_TOP_RIGHT : 0));
		scene_rect_set_radius(&decoration->buttons[item],
			titlebar_radius(&window->current));
		// In the middle of the button, at whole pixels
		int icon_size = config.decorations.titlebar.icon_size;
		scene_node_set_position(&decoration->button_icons[item].node,
			box.x + (box.width - icon_size) / 2,
			box.y + (box.height - icon_size) / 2);
	}
	decoration_update_buttons(window);
}

// A button is pressed while the pointer grabbed by it is still over it, and
// hovered while the pointer is over it without a grab.
static enum button_state
button_state(struct window *window, enum decoration_part button)
{
	struct pointer *pointer = &core.seat.pointer;
	bool over = pointer->hover.window == window && pointer->hover.part == button;
	switch (pointer->grab.type) {
	case POINTER_GRAB_NONE:
		return over ? BUTTON_STATE_HOVERED : BUTTON_STATE_NORMAL;
	case POINTER_GRAB_DECORATION_BUTTON: {
		bool grabbed = pointer->grab.decoration_button.window == window
			&& pointer->grab.decoration_button.part == button;
		return over && grabbed ? BUTTON_STATE_PRESSED : BUTTON_STATE_NORMAL;
	}
	case POINTER_GRAB_MOVE:
	case POINTER_GRAB_RESIZE:
	case POINTER_GRAB_DND:
		return BUTTON_STATE_NORMAL;
	}
	assert(!"unknown pointer grab type");
	return BUTTON_STATE_NORMAL;
}

// The maximize button of maximized windows is a restore button, and the
// sticky button of sticky windows is an unstick button.
static enum button_type
button_type(struct window *window, enum decoration_part button)
{
	switch (button) {
	case DECORATION_PART_CLOSE:
		return BUTTON_CLOSE;
	case DECORATION_PART_MAXIMIZE:
		return window->current.maximized ? BUTTON_MAXIMIZE_RESTORE : BUTTON_MAXIMIZE;
	case DECORATION_PART_MINIMIZE:
		return BUTTON_MINIMIZE;
	case DECORATION_PART_STICKY:
		return window->workspace == WORKSPACE_STICKY ? BUTTON_UNSTICK : BUTTON_STICKY;
	case DECORATION_PART_NONE:
	case DECORATION_PART_TITLEBAR:
	case DECORATION_PART_BORDER:
		break;
	}
	assert(!"not a button");
	return BUTTON_CLOSE;
}

// Shows the state of the buttons by the pointer, the maximized and sticky
// states and the activation of the window.
void
decoration_update_buttons(struct window *window)
{
	if (!decoration_visible(window)) {
		return;
	}
	struct decoration *decoration = &window->decoration;
	bool activated = window->activated;
	for (int item = 0; item < NR_TITLEBAR_BUTTONS; item++) {
		enum decoration_part button = button_parts[item];
		enum button_type type = button_type(window, button);
		enum button_state state = button_state(window, button);
		scene_rect_set_color(&decoration->buttons[item],
			config.decorations.titlebar.button_styles[type][state][activated]
				.background_color);
		scene_texture_set_texture(&decoration->button_icons[item],
			window->output->button_icons[type][state][activated],
			config.decorations.titlebar.icon_size,
			config.decorations.titlebar.icon_size);
	}
}

// Renders the title again for its new text.
void
decoration_update_title(struct window *window)
{
	window->decoration.title_scale = 0.0;
	decoration_update(window);
}

// Looks the icon up again for its new source.
void
decoration_update_icon(struct window *window)
{
	window->decoration.icon_scale = 0.0;
	decoration_update(window);
}

// Renders the title and looks the icon up again, for a new scale or config.
void
decoration_redraw(struct window *window)
{
	window->decoration.title_scale = 0.0;
	window->decoration.icon_scale = 0.0;
	decoration_update(window);
}

// The part at the point in layout coordinates, and the edges resized there.
enum decoration_part
decoration_part_at(struct window *window, double x, double y, uint32_t *edges)
{
	*edges = EDGE_NONE;
	if (!decoration_visible(window)) {
		return DECORATION_PART_NONE;
	}

	// Relative to the top-left corner of the content.
	struct box content = window->current.geometry;
	double local_x = x - content.x;
	double local_y = y - content.y;
	struct box titlebar = titlebar_box(window);
	if (box_contains_point(&titlebar, local_x, local_y)) {
		struct titlebar_layout layout = get_titlebar_layout(window);
		for (int item = 0; item < NR_TITLEBAR_BUTTONS; item++) {
			if (box_contains_point(&layout.items[item], local_x, local_y)) {
				return button_parts[item];
			}
		}
		return DECORATION_PART_TITLEBAR;
	}

	// Maximized windows are not resized by their edges.
	if (window->current.maximized) {
		return DECORATION_PART_NONE;
	}
	struct box frame = frame_box(window);
	int margin = config.decorations.resize_margin;
	struct box grab = {
		.x = frame.x - margin,
		.y = frame.y - margin,
		.width = frame.width + margin * 2,
		.height = frame.height + margin * 2,
	};
	if (!box_contains_point(&grab, local_x, local_y)) {
		return DECORATION_PART_NONE;
	}
	if (local_x < 0) {
		*edges |= EDGE_LEFT;
	} else if (local_x >= content.width) {
		*edges |= EDGE_RIGHT;
	}
	if (local_y < titlebar.y) {
		*edges |= EDGE_TOP;
	} else if (local_y >= content.height) {
		*edges |= EDGE_BOTTOM;
	}
	return *edges ? DECORATION_PART_BORDER : DECORATION_PART_NONE;
}

// The frame box of a decorated window around the content box. The border
// surrounds the titlebar and the content.
struct box
decoration_frame_box(struct box content, bool maximized)
{
	int border = border_width(maximized);
	int titlebar_height = config.decorations.titlebar_height;
	return (struct box){
		.x = content.x - border,
		.y = content.y - titlebar_height - border,
		.width = content.width + border * 2,
		.height = titlebar_height + content.height + border * 2,
	};
}

// The content box of a decorated window whose frame fills the box.
struct box
decoration_content_box(struct box frame, bool maximized)
{
	int border = border_width(maximized);
	int titlebar_height = config.decorations.titlebar_height;
	return (struct box){
		.x = frame.x + border,
		.y = frame.y + border + titlebar_height,
		.width = frame.width - border * 2,
		.height = frame.height - titlebar_height - border * 2,
	};
}
