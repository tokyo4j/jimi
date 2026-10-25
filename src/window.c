#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "core.h"
#include "image_capture.h"
#include "output.h"
#include "surface.h"

// Returns the serial the client will ack, or 0 if nothing is to be acked, in
// which case the state applies right away.
static uint32_t
window_send_configure(struct window *window)
{
	switch (window->type) {
	case WINDOW_XDG:
		return xdg_toplevel_configure(window);
	case WINDOW_XWAYLAND:
		return xwayland_window_configure(window);
	}
	assert(!"unknown window type");
	return 0;
}

void
window_close(struct window *window)
{
	switch (window->type) {
	case WINDOW_XDG:
		xdg_toplevel_close(window);
		return;
	case WINDOW_XWAYLAND:
		xwayland_window_close(window);
		return;
	}
	assert(!"unknown window type");
}

void
window_init(struct window *window, enum window_type type)
{
	*window = (struct window){
		.type = type,
		.ssd = true,
	};
	wl_list_init(&window->configures);
	wl_list_init(&window->foreign.link);
	wl_list_init(&window->foreign.ext_handles);
	wl_list_init(&window->foreign.wlr_handles);
	wl_list_init(&window->foreign.xfce_handles);
	wl_list_init(&window->capture.sources);
	wl_list_init(&window->capture.sessions);
	list_append(&core.windows, &window->link);
	scene_tree_init(&window->tree, &core.scene.layers[SCENE_LAYER_TOPLEVEL]);
	window_update_visibility(window);
	decoration_init(window);
}

// The surface must have left the tree.
void
window_fini(struct window *window)
{
	assert(!window->mapped);
	assert(core.seat.pointer.hover.window != window);
	window_set_parent(window, NULL);
	struct window_configure *configure, *tmp;
	wl_list_for_each_safe(configure, tmp, &window->configures, link) {
		wl_list_remove(&configure->link);
		free(configure);
	}
	free(window->title);
	free(window->app_id);
	toplevel_icon_unref(window->icon);
	decoration_fini(window);
	scene_tree_fini(&window->tree);
	wl_list_remove(&window->link);
}

// Moving to another output changes the scale the decoration is drawn at, and
// the one a hidden window is told to draw at.
static void
window_set_output(struct window *window, struct output *output)
{
	if (output == window->output) {
		return;
	}
	struct output *old_output = window->output;
	window->output = output;
	foreign_toplevel_update_output(window, old_output);
	image_capture_damage_window(window);
	decoration_update(window);
}

// Floating windows are on the output nearest to their center, while the
// others stay on the output they fill.
static void
window_update_output(struct window *window)
{
	if (!window_floating(window)) {
		return;
	}
	struct box *geometry = &window->pending.geometry;
	window_set_output(window, output_nearest(geometry->x + geometry->width / 2.0,
		geometry->y + geometry->height / 2.0));
}

// The box of the window with its server-side decoration, as committed.
struct box
window_frame_box(struct window *window)
{
	struct box geometry = window->current.geometry;
	if (!window_decorated(window)) {
		return geometry;
	}
	return decoration_frame_box(geometry, window->current.maximized);
}

// Whether the window, or a visible window it is a child of like a game showing
// a dialog, is fullscreen on the output.
static bool
fullscreen_on_output(struct window *window, struct output *output)
{
	for (; window; window = window->parent) {
		struct box frame = window_frame_box(window);
		if (window_visible(window) && window->current.fullscreen
				&& box_intersects(&frame, &output->box)) {
			return true;
		}
	}
	return false;
}

// Fullscreen windows cover the panels of the top layer on their output, as in
// labwc and KWin, while they or their children are the topmost windows there.
// Raising another window on the output shows the panels again.
static void
update_top_layers(void)
{
	struct output *output;
	wl_list_for_each(output, &core.outputs, link) {
		bool fullscreen = false;
		struct window *window;
		wl_list_for_each_reverse(window, &core.windows, link) {
			struct box frame = window_frame_box(window);
			if (window_visible(window) && box_intersects(&frame, &output->box)) {
				fullscreen = fullscreen_on_output(window, output);
				break;
			}
		}
		scene_node_set_enabled(&output->top_layer.node, !fullscreen);
	}
}

// The window must have a surface with a buffer.
void
window_map(struct window *window)
{
	assert(!window->mapped && window->surface);
	window_update_output(window);
	window->workspace = window->parent
		? window->parent->workspace : core.workspaces.current;
	window->mapped = true;
	window_raise(window);
	window_update_visibility(window);
	surface_set_mapped(window->surface, true);
	foreign_toplevel_map(window);
	// Children of windows on other workspaces wait there, rather than
	// switching to them.
	if (window_on_shown_workspace(window)) {
		window_focus(window);
	}
}

void
window_unmap(struct window *window)
{
	assert(window->mapped);
	// Cleared before unmapping the surface, so that restoring the focus
	// does not pick this window as a modal dialog.
	window->mapped = false;
	surface_set_mapped(window->surface, false);
	// A remapped window is shown again, as clients cannot restore it.
	window->minimized = false;
	window_update_visibility(window);
	window_switcher_remove_window(window);
	// The pointer is only over, and presses, mapped windows.
	if (core.seat.pointer.hover.window == window) {
		core.seat.pointer.hover.window = NULL;
	}
	if (core.seat.pointer.titlebar_press.window == window) {
		core.seat.pointer.titlebar_press.window = NULL;
	}
	foreign_toplevel_unmap(window);
	image_capture_window_unmap(window);
	// The children go to the grandparent, which is stacked below them.
	struct window *child;
	wl_list_for_each(child, &core.windows, link) {
		if (child->parent == window) {
			child->parent = window->parent;
			foreign_toplevel_update_parent(child);
		}
	}
}

// Floating windows pick their geometry, while the others fill a part of their
// output.
bool
window_state_floating(const struct window_state *state)
{
	return !state->maximized && !state->fullscreen && !state->tiled;
}

bool
window_floating(struct window *window)
{
	return window_state_floating(&window->pending);
}

// The geometry the window floats at, or returns to if it is not floating.
static struct box *
window_floating_geometry(struct window *window)
{
	return window_floating(window) ? &window->pending.geometry
		: &window->restore_geometry;
}

// Places the window by a committed state. Moves need no ack, so a floating
// state is at the geometry the window floats at, whatever position it was
// configured at, while the others are at their configured geometry: a
// maximized window stays until the client commits the floating state it is
// restored to. A window moved by the pointer keeps the grabbed point under it,
// whatever size the client commits, as Wayfire does. The resized edges follow
// the committed size, while the opposite edges stay in place.
static void
window_apply(struct window *window, const struct window_state *state,
	int width, int height)
{
	struct box geometry = state->geometry;
	if (window_state_floating(state)) {
		geometry = *window_floating_geometry(window);
		struct box moved = pointer_move_geometry(window, width, height);
		if (!box_empty(&moved)) {
			geometry = moved;
		}
	}
	if (state->resize_edges & EDGE_LEFT) {
		geometry.x += geometry.width - width;
	}
	if (state->resize_edges & EDGE_TOP) {
		geometry.y += geometry.height - height;
	}
	geometry.width = width;
	geometry.height = height;

	struct window_state old = window->current;
	window->current = *state;
	window->current.geometry = geometry;
	scene_node_set_position(&window->tree.node, geometry.x, geometry.y);
	window_update_output(window);
	decoration_update(window);
	window_switcher_update_outline(window);
	update_top_layers();
	if (old.maximized != state->maximized || old.fullscreen != state->fullscreen) {
		foreign_toplevel_update_state(window);
	}
}

// Moves the frame into the usable area. Frames larger than the usable area
// stick to its top-left corner.
static struct box
clamp_frame(struct box frame, const struct box *usable)
{
	frame.x = MAX(usable->x, MIN(frame.x, usable->x + usable->width - frame.width));
	frame.y = MAX(usable->y,
		MIN(frame.y, usable->y + usable->height - frame.height));
	return frame;
}

// Cascades the frame from the center of the usable area as labwc does. While
// it covers another visible window whose top-left corner no window above
// covers, it moves to that corner shifted by the height of the titlebar and
// the border, so that the title of the covered window stays visible. A frame
// moved out of the usable area goes back to the center.
static struct box
cascade_frame(struct window *window, struct box center, const struct box *usable)
{
	int offset = config.decorations.titlebar_height
		+ config.decorations.border_width + 5;

	// Each move goes further to the bottom-right, so the loop ends.
	struct box frame = center;
	bool moved = true;
	while (moved) {
		moved = false;
		// The bounding box of the windows above, which is enough here.
		struct box covered = {0};
		struct window *other;
		wl_list_for_each_reverse(other, &core.windows, link) {
			if (other == window || !window_visible(other)) {
				continue;
			}
			struct box other_frame = window_frame_box(other);
			if (!box_intersects(&frame, &other_frame)) {
				continue;
			}
			if (box_contains_box(&frame, &other_frame)
					&& !box_contains_point(&covered, other_frame.x, other_frame.y)) {
				frame.x = other_frame.x + offset;
				frame.y = other_frame.y + offset;
				if (box_contains_box(usable, &frame)) {
					moved = true;
				} else {
					frame = center;
				}
				break;
			}
			box_union(&covered, &other_frame);
		}
	}
	return frame;
}

// The floating geometry of the size in the usable area of the output of the
// window. Children, like dialogs, are centered on the frame of their parent,
// while the others are cascaded from the center of the usable area. Windows
// of unknown size (0x0) stay there, on the output, until the client picks the
// size. Windows placed while no output is enabled go to the nearest output
// once one is. The decoration mode must be known, since the frame is placed.
static struct box
window_initial_geometry(struct window *window, int width, int height)
{
	struct output *output = window->output;
	struct box content = {.width = width, .height = height};
	if (!output) {
		return content;
	}
	struct box usable = output->usable_area;
	struct box frame = window->ssd ? decoration_frame_box(content, false) : content;
	struct box center = window->parent ? window_frame_box(window->parent) : usable;
	frame.x = center.x + (center.width - frame.width) / 2;
	frame.y = center.y + (center.height - frame.height) / 2;
	frame = clamp_frame(frame, &usable);
	if (!window->parent) {
		frame = cascade_frame(window, frame, &usable);
	}
	return window->ssd ? decoration_content_box(frame, false) : frame;
}

// Called when the client commits a new size or acks the configure with the
// serial. Clients that ack no configures pass 0. Acking a configure of
// unknown size (0x0), the client picked the size of the floating window. The
// window is then placed if its floating geometry is still unknown: under the
// grabbed point if moved by the pointer, or else by window_initial_geometry().
void
window_commit(struct window *window, uint32_t serial, int width, int height)
{
	struct window_state state = window->current;
	struct window_configure *configure, *tmp;
	wl_list_for_each_safe(configure, tmp, &window->configures, link) {
		if (configure->serial > serial) {
			break;
		}
		state = configure->state;
		wl_list_remove(&configure->link);
		free(configure);
	}
	struct box *floating = window_floating_geometry(window);
	if (box_empty(&state.geometry) && box_empty(floating)) {
		struct box moved = pointer_move_geometry(window, width, height);
		*floating = box_empty(&moved)
			? window_initial_geometry(window, width, height) : moved;
	}
	window_apply(window, &state, width, height);

	// A floating client that acks configures may pick another size than
	// configured, which the next configures keep.
	if (serial && wl_list_empty(&window->configures)
			&& !window->configure_scheduled && window_floating(window)) {
		window->pending.geometry = window->current.geometry;
	}
}

void
window_schedule_configure(struct window *window)
{
	window->configure_scheduled = true;
}

// Sends one configure for all the changes made to each window since the last
// flush.
void
windows_flush_configures(void)
{
	struct window *window;
	wl_list_for_each(window, &core.windows, link) {
		if (!window->configure_scheduled) {
			continue;
		}
		window->configure_scheduled = false;
		uint32_t serial = window_send_configure(window);
		if (serial) {
			struct window_configure *configure = calloc(1, sizeof(*configure));
			assert(configure);
			configure->serial = serial;
			configure->state = window->pending;
			list_append(&window->configures, &configure->link);
		} else if (wl_list_empty(&window->configures)) {
			// Otherwise, the last configure carries the state as well
			// and applies it once acked.
			window_apply(window, &window->pending, window->current.geometry.width,
				window->current.geometry.height);
		}
	}
}

// Moves the pending geometry to the position. Moves need no ack, so a floating
// window moves right away.
static void
window_set_position(struct window *window, int x, int y)
{
	window->pending.geometry.x = x;
	window->pending.geometry.y = y;
	struct window_state state = window->current;
	window_apply(window, &state, state.geometry.width, state.geometry.height);
	window_schedule_configure(window);
}

// Moved by the user, the window stays even if the output it is away from
// returns.
void
window_move(struct window *window, int x, int y)
{
	window->away.output_name[0] = '\0';
	window_set_position(window, x, y);
}

// Resizing the edges moves the opposite ones as well until the client
// commits the new size. Resized windows float, even if snapped before.
void
window_resize(struct window *window, struct box geometry, uint32_t edges,
	bool resizing)
{
	window->away.output_name[0] = '\0';
	window->pending.geometry = geometry;
	window->pending.tiled = EDGE_NONE;
	window->pending.resizing = resizing;
	window->pending.resize_edges = edges;
	window_schedule_configure(window);
}

// The part of the usable area of the output that touches its tiled edges: along
// each axis, the whole if both edges are tiled, or else the half at one of them.
static struct box
tiled_box(struct output *output, uint32_t tiled)
{
	struct box box = output->usable_area;
	if (!(tiled & EDGE_LEFT)) {
		box.x += box.width / 2;
		box.width -= box.width / 2;
	} else if (!(tiled & EDGE_RIGHT)) {
		box.width /= 2;
	}
	if (!(tiled & EDGE_TOP)) {
		box.y += box.height / 2;
		box.height -= box.height / 2;
	} else if (!(tiled & EDGE_BOTTOM)) {
		box.height /= 2;
	}
	return box;
}

// Fullscreen windows cover their output, and the frames of maximized and
// snapped windows fill the whole or a part of its usable area, while the
// others return to their floating geometry. The client is configured for it.
// Windows that are not floating stay while no output is enabled.
static void
window_arrange(struct window *window)
{
	struct output *output = window->output;
	struct window_state *pending = &window->pending;
	window_schedule_configure(window);
	if (window_floating(window)) {
		pending->geometry = window->restore_geometry;
		return;
	}
	if (!output) {
		return;
	}
	if (pending->fullscreen) {
		pending->geometry = output->box;
		return;
	}
	struct box box = pending->maximized ? output->usable_area
		: tiled_box(output, pending->tiled);
	if (window->ssd) {
		box = decoration_content_box(box, pending->maximized);
	}
	box.width = MAX(1, box.width);
	box.height = MAX(1, box.height);
	pending->geometry = box;
}

// Windows that are not floating follow the usable area of their output.
void
windows_arrange(struct output *output)
{
	struct window *window;
	wl_list_for_each(window, &core.windows, link) {
		if (!window_floating(window) && window->output == output) {
			window_arrange(window);
		}
	}
}

// Puts a new window on the output of its parent, or else on the one under the
// pointer, and arranges it there. It floats at the size, or at the one the
// client picks if the size is 0x0. Windows that are not floating, like those
// that asked to be maximized before the placement, return to that floating
// geometry.
void
window_place(struct window *window, int width, int height)
{
	// The client draws its first buffer for the output.
	window_set_output(window, window->parent ? window->parent->output
		: output_nearest(core.seat.pointer.x, core.seat.pointer.y));
	window->restore_geometry = window_initial_geometry(window, width, height);
	window_arrange(window);
}

// Moves the geometry the window floats at, or returns to, into the usable
// area of its output if its center is out of it, as after the output
// shrinks. Frames larger than the usable area stick to its top-left corner.
static void
window_set_floating_geometry(struct window *window, struct box geometry)
{
	struct box usable = window->output->usable_area;
	if (!box_contains_point(&usable, geometry.x + geometry.width / 2.0,
			geometry.y + geometry.height / 2.0)) {
		struct box frame = window->ssd
			? decoration_frame_box(geometry, false) : geometry;
		frame = clamp_frame(frame, &usable);
		geometry = window->ssd ? decoration_content_box(frame, false) : frame;
	}
	if (window_floating(window)) {
		window_set_position(window, geometry.x, geometry.y);
	} else {
		window->restore_geometry = geometry;
	}
}

// Places the window on the output as it was on the output it is away from.
// The window is left without an output while no output is enabled.
static void
window_place_away(struct window *window, struct output *output)
{
	window_set_output(window, output);
	if (!output) {
		return;
	}
	struct box geometry = *window_floating_geometry(window);
	geometry.x = output->box.x + window->away.x;
	geometry.y = output->box.y + window->away.y;
	window_set_floating_geometry(window, geometry);
	if (!window_floating(window)) {
		window_arrange(window);
	}
}

// The windows follow their outputs after the outputs change. Those on
// disabled outputs go to the enabled outputs nearest to them, where they are
// as on their outputs, and come back when their outputs are enabled again.
void
windows_update_layout(void)
{
	struct window *window;
	wl_list_for_each(window, &core.windows, link) {
		struct output *output = window->output;
		bool away = window->away.output_name[0] != '\0';
		struct output *home = away ? output_find(window->away.output_name) : NULL;
		if (home) {
			window_place_away(window, home);
			window->away.output_name[0] = '\0';
		} else if (output && output->enabled) {
			if (box_equal(&output->box, &output->last_box)) {
				continue;
			}
			// The usable area of the output has arranged the
			// windows that are not floating.
			struct box geometry = *window_floating_geometry(window);
			geometry.x += output->box.x - output->last_box.x;
			geometry.y += output->box.y - output->last_box.y;
			window_set_floating_geometry(window, geometry);
		} else if (output || away) {
			struct box geometry = *window_floating_geometry(window);
			if (!away) {
				snprintf(window->away.output_name,
					sizeof(window->away.output_name), "%s", output->name);
				window->away.x = geometry.x - output->box.x;
				window->away.y = geometry.y - output->box.y;
			}
			window_place_away(window, output_nearest(
				geometry.x + geometry.width / 2.0,
				geometry.y + geometry.height / 2.0));
		}
	}
}

// Saved before the window leaves the floating state, where it stays on its
// output.
void
window_save_restore_geometry(struct window *window)
{
	if (window_floating(window)) {
		window->restore_geometry = window->pending.geometry;
		window_update_output(window);
	}
}

// The edges of the usable area that a window snapped to the edges touches.
// Snapping to the left or right edge fills the height.
static uint32_t
snap_tiled_edges(uint32_t edges)
{
	if (!(edges & (EDGE_LEFT | EDGE_RIGHT))) {
		edges |= EDGE_LEFT | EDGE_RIGHT;
	}
	if (!(edges & (EDGE_TOP | EDGE_BOTTOM))) {
		edges |= EDGE_TOP | EDGE_BOTTOM;
	}
	return edges;
}

// Snaps a floating window to the edges of the usable area of the output: to
// the half at an edge, or to the quarter at a corner like EDGE_TOP | EDGE_LEFT.
// Snapping to the top edge maximizes the window instead. The window returns to
// the restore geometry, which the move saved before the window was dragged to
// the edges.
void
window_snap(struct window *window, struct output *output, uint32_t edges)
{
	assert(edges != EDGE_NONE);
	if (!window_floating(window)) {
		return;
	}
	window->away.output_name[0] = '\0';
	window_set_output(window, output);
	if (edges == EDGE_TOP) {
		window->pending.maximized = true;
	} else {
		window->pending.tiled = snap_tiled_edges(edges);
	}
	window_arrange(window);
}

// Returns a snapped or maximized window to its floating geometry.
void
window_unsnap(struct window *window)
{
	window->pending.maximized = false;
	window->pending.tiled = EDGE_NONE;
	window_arrange(window);
}

// The frame box of a window snapped to the edges of the output. Maximized by
// the top edge, it fills the usable area.
struct box
window_snap_box(struct output *output, uint32_t edges)
{
	if (edges == EDGE_TOP) {
		return output->usable_area;
	}
	return tiled_box(output, snap_tiled_edges(edges));
}

void
window_set_maximized(struct window *window, bool maximized)
{
	if (window->pending.maximized == maximized) {
		return;
	}
	window_save_restore_geometry(window);
	window->pending.maximized = maximized;
	window_arrange(window);
}

void
window_set_fullscreen(struct window *window, bool fullscreen)
{
	if (window->pending.fullscreen == fullscreen) {
		return;
	}
	window_save_restore_geometry(window);
	window->pending.fullscreen = fullscreen;
	window_arrange(window);
}

// Only listed windows are minimized, since unmapping clears the state and
// modal dialogs could not be restored. Restoring raises and focuses the window.
void
window_set_minimized(struct window *window, bool minimized)
{
	if (window->minimized == minimized || !window_listed(window)) {
		return;
	}
	if (!minimized) {
		window_focus(window);
		return;
	}
	window->minimized = true;
	window_update_visibility(window);
	xdg_popups_dismiss(window->surface);
	keyboard_forget_surface(window->surface);
	foreign_toplevel_update_state(window);
}

// Activation follows the keyboard focus history.
void
window_set_activated(struct window *window, bool activated)
{
	if (window->activated == activated) {
		return;
	}
	window->activated = activated;
	decoration_update(window);
	foreign_toplevel_update_state(window);
	window_schedule_configure(window);
}

void
window_set_ssd(struct window *window, bool ssd)
{
	if (window->ssd == ssd) {
		return;
	}
	window->ssd = ssd;
	decoration_update(window);
	window_switcher_update_outline(window);
	// The decoration takes space from the output.
	if (!window_floating(window)) {
		window_arrange(window);
	}
}

void
window_set_title(struct window *window, const char *title)
{
	if (window->title && strcmp(window->title, title) == 0) {
		return;
	}
	char *copy = strdup(title);
	assert(copy);
	free(window->title);
	window->title = copy;
	decoration_update_title(window);
	window_switcher_update_title(window);
	foreign_toplevel_update_title(window);
}

void
window_set_app_id(struct window *window, const char *app_id)
{
	if (window->app_id && strcmp(window->app_id, app_id) == 0) {
		return;
	}
	char *copy = strdup(app_id);
	assert(copy);
	free(window->app_id);
	window->app_id = copy;
	decoration_update_icon(window);
	window_switcher_update_icon(window);
	foreign_toplevel_update_app_id(window);
}

// The icon is shown instead of the one of the app ID, until it is unset.
void
window_set_icon(struct window *window, struct toplevel_icon *icon)
{
	if (window->icon == icon) {
		return;
	}
	toplevel_icon_unref(window->icon);
	window->icon = toplevel_icon_ref(icon);
	decoration_update_icon(window);
	window_switcher_update_icon(window);
	foreign_toplevel_update_icon(window);
}

// The icon of the size in logical pixels, rendered for the scale, or NULL if
// the window has none. The icon set by xdg-toplevel-icon comes first,
// preferring its name to its pixels. Otherwise, the icon is found by the app
// ID.
struct texture *
window_create_icon(struct window *window, int size, double scale)
{
	int scaled_size = (int)round(size * scale);
	struct toplevel_icon *icon = window->icon;
	char *path = NULL;
	if (icon && icon->name) {
		path = icon_lookup(icon->name, scaled_size);
	}
	if (!path && icon && icon->pixels) {
		return texture_create_image_pixels(icon->pixels, icon->width,
			icon->height, size, size, scale);
	}
	if (!path && window->app_id) {
		path = app_icon_lookup(window->app_id, scaled_size);
	}
	if (!path) {
		return NULL;
	}
	struct texture *texture = texture_create_image_file(path, size, size, scale);
	free(path);
	return texture;
}

bool
window_has_ancestor(struct window *window, struct window *ancestor)
{
	for (; window; window = window->parent) {
		if (window == ancestor) {
			return true;
		}
	}
	return false;
}

// Children are stacked above their parent. Only mapped windows are parents,
// so that unmapping hands the children over to the grandparent.
void
window_set_parent(struct window *window, struct window *parent)
{
	assert(!window_has_ancestor(parent, window));
	if (parent && !parent->mapped) {
		parent = NULL;
	}
	if (window->parent == parent) {
		return;
	}
	window->parent = parent;
	if (parent) {
		window_raise(window);
		if (window->mapped) {
			window_set_workspace(window, parent->workspace);
		}
	}
	update_top_layers();
	foreign_toplevel_update_parent(window);
}

// Mapped windows are listed by the window switcher, except modal dialogs,
// which are reached through their parent.
bool
window_listed(struct window *window)
{
	return window->mapped && !(window->modal && window->parent);
}

// Raises the window along with its children, which stay above it.
void
window_raise(struct window *window)
{
	wl_list_remove(&window->link);
	list_append(&core.windows, &window->link);
	scene_node_raise_to_top(&window->tree.node);
	// Raised children move behind the window, so each scan stops at it.
	for (;;) {
		struct window *child = NULL;
		struct window *iter;
		wl_list_for_each(iter, &core.windows, link) {
			if (iter == window) {
				break;
			}
			if (iter->parent == window) {
				child = iter;
				break;
			}
		}
		if (!child) {
			break;
		}
		window_raise(child);
	}
	update_top_layers();
}

// Raises the window and gives it or its modal dialog the keyboard focus,
// switching to its workspace and restoring it if minimized. Windows are not
// focused while switching windows, which ends by focusing the selected one.
void
window_focus(struct window *window)
{
	if (core.window_switcher.selected) {
		return;
	}
	if (!window_on_shown_workspace(window)) {
		workspace_show(window->workspace);
	}
	if (window->minimized) {
		window->minimized = false;
		window_update_visibility(window);
		foreign_toplevel_update_state(window);
	}
	window_raise(window);
	keyboard_focus(window->surface);
}

// The topmost visible modal dialog of the window, or of that dialog in turn,
// which takes the keyboard focus for the window. Otherwise, the window itself.
struct window *
window_focus_target(struct window *window)
{
	struct window *iter;
	wl_list_for_each_reverse(iter, &core.windows, link) {
		if (iter->parent == window && iter->modal && window_visible(iter)) {
			return window_focus_target(iter);
		}
	}
	return window;
}

// Windows on other workspaces are hidden, and so are minimized windows,
// except the one selected while switching windows.
bool
window_visible(struct window *window)
{
	if (!window->mapped || !window_on_shown_workspace(window)) {
		return false;
	}
	return !window->minimized || window == core.window_switcher.selected;
}

void
window_update_visibility(struct window *window)
{
	scene_node_set_enabled(&window->tree.node, window_visible(window));
	update_top_layers();
}

// Server-side decorations are hidden while fullscreen.
bool
window_decorated(struct window *window)
{
	return window->ssd && !window->current.fullscreen;
}

// Popups belong to the window of their parent.
struct window *
window_from_surface(struct surface *surface)
{
	if (!surface) {
		return NULL;
	}
	struct surface *root = surface_root(surface);
	switch (root->role) {
	case SURFACE_ROLE_XDG_TOPLEVEL:
		return &root->xdg->toplevel->window;
	case SURFACE_ROLE_XWAYLAND:
		return xwayland_surface_window(root->xwayland);
	case SURFACE_ROLE_NONE:
	case SURFACE_ROLE_CURSOR:
	case SURFACE_ROLE_DND_ICON:
	case SURFACE_ROLE_SUBSURFACE:
	case SURFACE_ROLE_XDG_SURFACE:
	case SURFACE_ROLE_XDG_POPUP:
	case SURFACE_ROLE_LAYER_SURFACE:
	case SURFACE_ROLE_INPUT_PANEL:
	case SURFACE_ROLE_LOCK_SURFACE:
		return NULL;
	}
	assert(!"unknown surface role");
	return NULL;
}

// The surface or the decoration part of the window at the point. Popups of
// the window cover its decoration.
bool
window_at(struct window *window, double x, double y, struct input_target *target)
{
	if (!window_visible(window)) {
		return false;
	}
	double sx, sy;
	struct surface *surface = scene_surface_at(&window->tree.node, x, y, &sx, &sy);
	if (surface) {
		*target = (struct input_target){
			.surface = surface,
			.sx = sx,
			.sy = sy,
			.window = window,
		};
		return true;
	}
	uint32_t edges;
	enum decoration_part part = decoration_part_at(window, x, y, &edges);
	if (part == DECORATION_PART_NONE) {
		return false;
	}
	*target = (struct input_target){
		.window = window,
		.part = part,
		.edges = edges,
	};
	return true;
}
