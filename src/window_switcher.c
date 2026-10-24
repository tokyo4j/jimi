#include <assert.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdlib.h>

#include "config.h"
#include "core.h"
#include "output.h"
#include "surface.h"

#define ALL_CORNERS (CORNER_TOP_LEFT | CORNER_TOP_RIGHT | CORNER_BOTTOM_RIGHT \
	| CORNER_BOTTOM_LEFT)

enum close_button_state {
	CLOSE_BUTTON_NORMAL,
	CLOSE_BUTTON_HOVERED,
	CLOSE_BUTTON_PRESSED,
	NR_CLOSE_BUTTON_STATES,
};

// The backgrounds of the close button, by state.
static const struct color close_button_colors[NR_CLOSE_BUTTON_STATES] = {
	{0.0f, 0.0f, 0.0f, 0.6f},
	{0.77f, 0.17f, 0.11f, 1.0f},
	{0.58f, 0.12f, 0.08f, 1.0f},
};

// A cross on a 10x10 grid, like the close button of decorations.
static const char close_icon_svg[] =
	"<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 10 10'>"
	"<path d='M0.5 0.5L9.5 9.5M9.5 0.5L0.5 9.5' fill='none'"
	" stroke='currentColor' stroke-linecap='square'/></svg>";

// A window in the grid: its live thumbnail with its app icon on the bottom
// edge, and its title below them. The textures are drawn for the scale of
// the output of the grid.
struct window_switcher_item {
	struct wl_list link; // core.window_switcher.items
	struct window *window;
	struct scene_tree tree; // In core.window_switcher.tree
	// Covers the item, so that the pointer is over it anywhere in it.
	// Transparent unless the window is selected, as the border.
	struct scene_rect background;
	struct scene_rect border;
	struct scene_thumbnail thumbnail; // Owns its texture.
	struct scene_texture icon; // Owns its texture.
	struct scene_texture title; // Owns its texture.
	// Shown while the item is hovered.
	struct scene_rect close_button;
	struct scene_texture close_icon; // core.window_switcher.close_icon
};

static int
item_width(void)
{
	return config.window_switcher.thumbnail_width
		+ config.window_switcher.padding * 2;
}

static int
item_height(void)
{
	return config.window_switcher.thumbnail_height
		+ config.window_switcher.app_icon_size / 2
		+ config.window_switcher.title_height
		+ config.window_switcher.padding * 2;
}

// The bottom edge of the thumbnails in the items, where the app icons are
// centered.
static int
thumbnail_bottom(void)
{
	return config.window_switcher.padding + config.window_switcher.thumbnail_height;
}

void
window_switcher_init(void)
{
	int radius = config.window_switcher.corner_radius;
	scene_rect_init(&core.window_switcher.outline,
		&core.scene.layers[SCENE_LAYER_OSD],
		config.window_switcher.colors.outline, 0.0f, 0);
	scene_rect_set_border_width(&core.window_switcher.outline,
		config.window_switcher.outline_width);
	scene_node_set_enabled(&core.window_switcher.outline.node, false);
	scene_tree_init(&core.window_switcher.tree, &core.scene.layers[SCENE_LAYER_OSD]);
	scene_node_set_enabled(&core.window_switcher.tree.node, false);
	scene_rect_init(&core.window_switcher.background, &core.window_switcher.tree,
		config.window_switcher.colors.background,
		(float)(radius + config.window_switcher.padding), ALL_CORNERS);
	wl_list_init(&core.window_switcher.items);
}

static struct window_switcher_item *
find_item(struct window *window)
{
	struct window_switcher_item *item;
	wl_list_for_each(item, &core.window_switcher.items, link) {
		if (item->window == window) {
			return item;
		}
	}
	return NULL;
}

// The next item after the item, or before it if backward, wrapping around.
// Without an item, starts from the first one, or the last one if backward.
static struct window_switcher_item *
next_item(struct window_switcher_item *item, bool forward)
{
	struct wl_list *items = &core.window_switcher.items;
	assert(!wl_list_empty(items));
	struct wl_list *link = item ? &item->link : items;
	link = forward ? link->next : link->prev;
	if (link == items) {
		link = forward ? link->next : link->prev;
	}
	return wl_container_of(link, item, link);
}

// Fits the window into the thumbnail size, bottom-aligned and centered
// horizontally, and draws it again. The texture is replaced when the size
// changes.
static void
update_thumbnail(struct window_switcher_item *item)
{
	struct scene_thumbnail *thumbnail = &item->thumbnail;
	struct box geometry = item->window->current.geometry;
	int max_width = config.window_switcher.thumbnail_width;
	int max_height = config.window_switcher.thumbnail_height;
	double fit = MIN(1.0, MIN((double)max_width / geometry.width,
		(double)max_height / geometry.height));
	int width = MAX(1, (int)round(geometry.width * fit));
	int height = MAX(1, (int)round(geometry.height * fit));
	scene_node_set_position(&thumbnail->node,
		config.window_switcher.padding + (max_width - width) / 2,
		thumbnail_bottom() - height);
	if (thumbnail->texture && thumbnail->width == width
			&& thumbnail->height == height) {
		scene_thumbnail_update(thumbnail);
		return;
	}
	texture_destroy(thumbnail->texture);
	double scale = core.window_switcher.output->scale;
	struct texture *texture = texture_create(
		(uint32_t)round(width * scale), (uint32_t)round(height * scale),
		DRM_FORMAT_ARGB8888);
	scene_thumbnail_set_texture(thumbnail, texture, width, height);
}

// Centered on the bottom edge of the thumbnail. The window may have no icon.
static void
update_icon(struct window_switcher_item *item)
{
	struct scene_texture *icon = &item->icon;
	texture_destroy(icon->texture);
	int size = config.window_switcher.app_icon_size;
	struct texture *texture = window_create_icon(item->window, size,
		core.window_switcher.output->scale);
	scene_texture_set_texture(icon, texture, size, size);
	scene_node_set_enabled(&icon->node, texture);
	scene_node_set_position(&icon->node, (item_width() - size) / 2,
		thumbnail_bottom() - size / 2);
}

// Centered below the app icon. The window may have no title.
static void
update_title(struct window_switcher_item *item)
{
	struct scene_texture *title = &item->title;
	texture_destroy(title->texture);
	int max_width = item_width() - config.window_switcher.padding * 2;
	int height = config.window_switcher.title_height;
	double scale = core.window_switcher.output->scale;
	const char *text = item->window->title;
	int width = 0;
	struct texture *texture = NULL;
	if (text && text[0] != '\0') {
		width = MIN(text_width(text, config.window_switcher.font, scale), max_width);
		texture = texture_create_text(text, config.window_switcher.font,
			config.window_switcher.colors.title, width, height, scale);
	}
	scene_texture_set_texture(title, texture, width, height);
	scene_node_set_enabled(&title->node, texture);
	scene_node_set_position(&title->node, (item_width() - width) / 2,
		thumbnail_bottom() + config.window_switcher.app_icon_size / 2);
}

static void
init_highlight(struct window_switcher_item *item)
{
	float radius = (float)config.window_switcher.corner_radius;
	scene_rect_init(&item->background, &item->tree,
		(struct color){0}, radius, ALL_CORNERS);
	scene_rect_set_size(&item->background, item_width(), item_height());
	scene_rect_init(&item->border, &item->tree,
		config.window_switcher.colors.selected_border, radius, ALL_CORNERS);
	scene_rect_set_size(&item->border, item_width(), item_height());
	scene_rect_set_border_width(&item->border, config.window_switcher.border_width);
	scene_node_set_enabled(&item->border.node, false);
}

// At the top-right corner of the thumbnail area
static void
init_close_button(struct window_switcher_item *item)
{
	int button_size = config.window_switcher.close_button_size;
	int icon_size = config.window_switcher.close_icon_size;
	int x = config.window_switcher.padding
		+ config.window_switcher.thumbnail_width - button_size;
	int y = config.window_switcher.padding;
	scene_rect_init(&item->close_button, &item->tree,
		close_button_colors[CLOSE_BUTTON_NORMAL],
		(float)button_size / 2.0f, ALL_CORNERS);
	scene_rect_set_size(&item->close_button, button_size, button_size);
	scene_node_set_position(&item->close_button.node, x, y);
	scene_node_set_enabled(&item->close_button.node, false);
	scene_texture_init(&item->close_icon, &item->tree);
	scene_texture_set_texture(&item->close_icon,
		core.window_switcher.close_icon, icon_size, icon_size);
	scene_node_set_position(&item->close_icon.node,
		x + (button_size - icon_size) / 2, y + (button_size - icon_size) / 2);
	scene_node_set_enabled(&item->close_icon.node, false);
}

static void
add_item(struct window *window)
{
	struct window_switcher_item *item = calloc(1, sizeof(*item));
	assert(item);
	item->window = window;
	scene_tree_init(&item->tree, &core.window_switcher.tree);
	init_highlight(item);
	scene_thumbnail_init(&item->thumbnail, &item->tree, window);
	scene_texture_init(&item->icon, &item->tree);
	scene_texture_init(&item->title, &item->tree);
	init_close_button(item);
	update_thumbnail(item);
	update_icon(item);
	update_title(item);
	list_append(&core.window_switcher.items, &item->link);
}

// The targets on the item are forgotten.
static void
remove_item(struct window_switcher_item *item)
{
	struct window_switcher_target *hover = &core.window_switcher.hover;
	struct window_switcher_target *press = &core.window_switcher.press;
	if (hover->window == item->window) {
		*hover = (struct window_switcher_target){0};
	}
	if (press->window == item->window) {
		*press = (struct window_switcher_target){0};
	}
	texture_destroy(item->thumbnail.texture);
	texture_destroy(item->icon.texture);
	texture_destroy(item->title.texture);
	// The nodes in the tree go away along with it.
	scene_node_detach(&item->tree.node);
	wl_list_remove(&item->link);
	free(item);
}

// Highlights the item of the selected window.
static void
update_highlight(void)
{
	struct window_switcher_item *item;
	wl_list_for_each(item, &core.window_switcher.items, link) {
		bool selected = item->window == core.window_switcher.selected;
		scene_rect_set_color(&item->background, selected
			? config.window_switcher.colors.selected_background
			: (struct color){0});
		scene_node_set_enabled(&item->border.node, selected);
	}
}

// Fits the outline to the frame of the selected window.
static void
update_outline(void)
{
	struct scene_rect *outline = &core.window_switcher.outline;
	struct box frame = window_frame_box(core.window_switcher.selected);
	scene_node_set_position(&outline->node, frame.x, frame.y);
	scene_rect_set_size(outline, frame.width, frame.height);
}

// Lays the items out in rows of as many as fit in the usable area of the
// output, which are filled evenly, and centers the grid in it.
// TODO: Scroll the rows that do not fit in the output.
static void
arrange_items(void)
{
	struct box area = core.window_switcher.output->usable_area;
	int padding = config.window_switcher.padding;
	int nr_items = wl_list_length(&core.window_switcher.items);
	int max_columns = MAX(1, (area.width - padding * 2) / item_width());
	int nr_rows = (nr_items + max_columns - 1) / max_columns;
	int nr_columns = (nr_items + nr_rows - 1) / nr_rows;
	int index = 0;
	struct window_switcher_item *item;
	wl_list_for_each(item, &core.window_switcher.items, link) {
		scene_node_set_position(&item->tree.node,
			padding + index % nr_columns * item_width(),
			padding + index / nr_columns * item_height());
		index++;
	}
	int width = nr_columns * item_width() + padding * 2;
	int height = nr_rows * item_height() + padding * 2;
	scene_rect_set_size(&core.window_switcher.background, width, height);
	scene_node_set_position(&core.window_switcher.tree.node,
		area.x + (area.width - width) / 2, area.y + (area.height - height) / 2);
}

// The listed windows on the shown workspace, minimized or not
static bool
listed(struct window *window)
{
	return window_listed(window) && window_on_shown_workspace(window);
}

// Lists the windows, top-to-bottom, on the output.
static void
show_grid(struct output *output)
{
	core.window_switcher.output = output;
	int icon_size = config.window_switcher.close_icon_size;
	core.window_switcher.close_icon = texture_create_svg(close_icon_svg,
		icon_size, icon_size, output->scale, (struct color){1.0f, 1.0f, 1.0f, 1.0f});
	struct window *window;
	wl_list_for_each_reverse(window, &core.windows, link) {
		if (listed(window)) {
			add_item(window);
		}
	}
	arrange_items();
	scene_node_set_enabled(&core.window_switcher.tree.node, true);
}

static void
hide_grid(void)
{
	struct window_switcher_item *item, *tmp;
	wl_list_for_each_safe(item, tmp, &core.window_switcher.items, link) {
		remove_item(item);
	}
	scene_node_set_enabled(&core.window_switcher.tree.node, false);
	texture_destroy(core.window_switcher.close_icon);
	core.window_switcher.close_icon = NULL;
	core.window_switcher.output = NULL;
}

// Stacks the windows in the scene as in core.windows, except that the selected
// window is raised along with its children.
static void
restack_windows(void)
{
	struct window *below = NULL;
	struct window *window;
	wl_list_for_each(window, &core.windows, link) {
		if (below) {
			scene_node_place_above(&window->tree.node, &below->tree.node);
		}
		below = window;
	}
	struct window *selected = core.window_switcher.selected;
	if (!selected) {
		return;
	}
	wl_list_for_each(window, &core.windows, link) {
		if (window_has_ancestor(window, selected)) {
			scene_node_raise_to_top(&window->tree.node);
		}
	}
}

static void
select_window(struct window *selected)
{
	core.window_switcher.selected = selected;
	struct window *window;
	wl_list_for_each(window, &core.windows, link) {
		window_update_visibility(window);
	}
	restack_windows();
	scene_node_set_enabled(&core.window_switcher.outline.node, selected);
	if (selected) {
		update_highlight();
		update_outline();
	}
}

static bool
has_listed_windows(void)
{
	struct window *window;
	wl_list_for_each(window, &core.windows, link) {
		if (listed(window)) {
			return true;
		}
	}
	return false;
}

// Selects the next window in the grid, starting from the active one. The
// keyboard and the pointer are unfocused until switching ends, and the menus
// of a popup grab are closed as by a click outside of them. Switching does
// not start during pointer grabs, or while no output is enabled. The grid is
// shown on the output under the pointer.
void
window_switcher_step(bool forward)
{
	struct window *current = core.window_switcher.selected;
	if (!current) {
		struct output *output = output_nearest(core.seat.pointer.x,
			core.seat.pointer.y);
		if (core.seat.pointer.grab.type != POINTER_GRAB_NONE
				|| !has_listed_windows() || !output) {
			return;
		}
		// Switching from a modal dialog starts from its parent.
		current = keyboard_active_window();
		while (current && !listed(current)) {
			current = current->parent;
		}
		struct xdg_popup *popup = core.seat.pointer.grabbed_popup;
		if (popup) {
			xdg_popups_dismiss(surface_root(popup->base->surface));
		}
		keyboard_focus(NULL);
		show_grid(output);
	}
	struct window_switcher_item *next = next_item(find_item(current), forward);
	select_window(next->window);
}

// Focuses and raises the window.
static void
switch_to_window(struct window *window)
{
	hide_grid();
	select_window(NULL);
	window_focus(window);
}

void
window_switcher_finish(void)
{
	switch_to_window(core.window_switcher.selected);
}

// Returns the focus to the surface which had it before switching.
void
window_switcher_cancel(void)
{
	hide_grid();
	select_window(NULL);
	keyboard_restore_focus();
}

// Unmapped windows leave the grid, and the selection moves from them to the
// next window. Unmapping the last window cancels switching.
void
window_switcher_remove_window(struct window *window)
{
	struct window_switcher_item *item = find_item(window);
	if (!item) {
		return;
	}
	struct window_switcher_item *next = next_item(item, true);
	if (next == item) {
		window_switcher_cancel();
		return;
	}
	remove_item(item);
	arrange_items();
	if (window == core.window_switcher.selected) {
		select_window(next->window);
	}
	window_switcher_update_hover();
}

// Called on each commit to the surfaces of the window.
void
window_switcher_update_thumbnail(struct window *window)
{
	struct window_switcher_item *item = find_item(window);
	if (item) {
		update_thumbnail(item);
	}
}

void
window_switcher_update_title(struct window *window)
{
	struct window_switcher_item *item = find_item(window);
	if (item) {
		update_title(item);
	}
}

void
window_switcher_update_icon(struct window *window)
{
	struct window_switcher_item *item = find_item(window);
	if (item) {
		update_icon(item);
	}
}

// Called when the frame of the window changes.
void
window_switcher_update_outline(struct window *window)
{
	if (window == core.window_switcher.selected) {
		update_outline();
	}
}

static bool
same_target(struct window_switcher_target a, struct window_switcher_target b)
{
	return a.window == b.window && a.close == b.close;
}

// The item at the point in layout coordinates, and whether the point is on
// its close button.
static struct window_switcher_target
target_at(double x, double y)
{
	struct scene_node *node = scene_node_at(&core.window_switcher.tree.node, x, y);
	struct window_switcher_item *item;
	wl_list_for_each(item, &core.window_switcher.items, link) {
		if (node && node->parent == &item->tree) {
			return (struct window_switcher_target){
				.window = item->window,
				.close = node == &item->close_button.node
					|| node == &item->close_icon.node,
			};
		}
	}
	return (struct window_switcher_target){0};
}

// The hovered item shows its close button, which is hovered while the
// pointer is over it, and pressed while the button pressed on it is held
// there.
static void
update_close_buttons(void)
{
	struct window_switcher_target hover = core.window_switcher.hover;
	enum close_button_state state = CLOSE_BUTTON_NORMAL;
	if (hover.close) {
		state = same_target(hover, core.window_switcher.press)
			? CLOSE_BUTTON_PRESSED : CLOSE_BUTTON_HOVERED;
	}
	struct window_switcher_item *item;
	wl_list_for_each(item, &core.window_switcher.items, link) {
		bool shown = item->window == hover.window;
		scene_node_set_enabled(&item->close_button.node, shown);
		scene_node_set_enabled(&item->close_icon.node, shown);
		scene_rect_set_color(&item->close_button, close_button_colors[state]);
	}
}

void
window_switcher_update_hover(void)
{
	core.window_switcher.hover = target_at(core.seat.pointer.x, core.seat.pointer.y);
	update_close_buttons();
}

// Clicking an item with the left button switches to its window. Clicking its
// close button, or clicking it with the middle button, closes the window.
void
window_switcher_handle_button(uint32_t button, bool pressed)
{
	if (button != BTN_LEFT && button != BTN_MIDDLE) {
		return;
	}
	struct window_switcher_target hover = core.window_switcher.hover;
	bool clicked = !pressed && hover.window
		&& same_target(core.window_switcher.press, hover);
	core.window_switcher.press = pressed ? hover
		: (struct window_switcher_target){0};
	update_close_buttons();
	if (!clicked) {
		return;
	}
	if (hover.close || button == BTN_MIDDLE) {
		window_close(hover.window);
	} else {
		switch_to_window(hover.window);
	}
}
