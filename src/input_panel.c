#include <assert.h>
#include <stdlib.h>

#include "core.h"
#include "input-method-unstable-v1-protocol.h"
#include "output.h"
#include "surface.h"

// A toplevel panel is centered at the bottom of the usable area. An overlay
// panel is below the cursor rectangle, or above it if only that fits on the
// output.
static struct box
input_panel_box(struct input_panel_surface *panel, struct output *output,
	const struct box *cursor)
{
	struct box box = {
		.width = panel->surface->width,
		.height = panel->surface->height,
	};
	if (panel->type == INPUT_PANEL_TOPLEVEL) {
		const struct box *usable = &output->usable_area;
		box.x = usable->x + (usable->width - box.width) / 2;
		box.y = usable->y + usable->height - box.height;
		return box;
	}
	const struct box *bounds = &output->box;
	box.x = MAX(bounds->x, MIN(cursor->x, bounds->x + bounds->width - box.width));
	box.y = cursor->y + cursor->height;
	if (box.y + box.height > bounds->y + bounds->height
			&& cursor->y - box.height >= bounds->y) {
		box.y = cursor->y - box.height;
	}
	return box;
}

// Maps the panel while it has a type and a buffer and a text input is
// enabled, on the output nearest to the cursor of the text input.
void
input_panel_surface_update(struct input_panel_surface *panel)
{
	struct surface *surface = panel->surface;
	struct text_input *text_input = core.seat.input_method.text_input;
	if (!text_input || panel->type == INPUT_PANEL_NONE || !surface->current.buffer) {
		surface_set_mapped(surface, false);
		return;
	}
	int x, y;
	scene_node_layout_position(&text_input->focus->scene.node, &x, &y);
	struct box cursor = text_input->current.cursor_rectangle;
	cursor.x += x;
	cursor.y += y;
	struct output *output = output_nearest(cursor.x, cursor.y);
	if (!output) {
		surface_set_mapped(surface, false);
		return;
	}
	struct box box = input_panel_box(panel, output, &cursor);
	scene_node_set_position(&surface->scene.tree.node, box.x, box.y);
	surface_set_mapped(surface, true);
}

void
input_panels_update(void)
{
	struct input_panel_surface *panel;
	wl_list_for_each(panel, &core.seat.input_method.panels, link) {
		input_panel_surface_update(panel);
	}
}

// The resource stays inert.
void
input_panel_surface_destroy(struct input_panel_surface *panel)
{
	struct surface *surface = panel->surface;
	surface_set_mapped(surface, false);
	wl_resource_set_user_data(panel->resource, NULL);
	wl_list_remove(&panel->link);
	scene_node_detach(&surface->scene.tree.node);
	surface->input_panel = NULL;
	surface->role = SURFACE_ROLE_NONE;
	free(panel);
}

static void
input_panel_surface_resource_destroy(struct wl_resource *resource)
{
	struct input_panel_surface *panel = wl_resource_get_user_data(resource);
	if (panel) {
		input_panel_surface_destroy(panel);
	}
}

// Toplevel panels are placed on the output of the text input instead.
static void
input_panel_surface_set_toplevel_req(struct wl_client *client,
	struct wl_resource *resource, struct wl_resource *output, uint32_t position)
{
	struct input_panel_surface *panel = wl_resource_get_user_data(resource);
	if (panel) {
		panel->type = INPUT_PANEL_TOPLEVEL;
		input_panel_surface_update(panel);
	}
}

static void
input_panel_surface_set_overlay_panel_req(struct wl_client *client,
	struct wl_resource *resource)
{
	struct input_panel_surface *panel = wl_resource_get_user_data(resource);
	if (panel) {
		panel->type = INPUT_PANEL_OVERLAY;
		input_panel_surface_update(panel);
	}
}

static const struct zwp_input_panel_surface_v1_interface input_panel_surface_impl = {
	.set_toplevel = input_panel_surface_set_toplevel_req,
	.set_overlay_panel = input_panel_surface_set_overlay_panel_req,
};

static void
input_panel_get_input_panel_surface_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id, struct wl_resource *surface_resource)
{
	struct surface *surface = wl_resource_get_user_data(surface_resource);
	if (surface->role != SURFACE_ROLE_NONE) {
		wl_resource_post_error(resource, 0, "wl_surface already has another role");
		return;
	}

	struct input_panel_surface *panel = calloc(1, sizeof(*panel));
	assert(panel);
	panel->surface = surface;
	panel->resource = wl_resource_create(client,
		&zwp_input_panel_surface_v1_interface,
		wl_resource_get_version(resource), id);
	assert(panel->resource);
	wl_resource_set_implementation(panel->resource, &input_panel_surface_impl,
		panel, input_panel_surface_resource_destroy);
	list_append(&core.seat.input_method.panels, &panel->link);
	surface->role = SURFACE_ROLE_INPUT_PANEL;
	surface->input_panel = panel;
	scene_node_reparent(&surface->scene.tree.node,
		&core.scene.layers[SCENE_LAYER_INPUT_PANEL]);
}

static const struct zwp_input_panel_v1_interface input_panel_impl = {
	.get_input_panel_surface = input_panel_get_input_panel_surface_req,
};

void
wayland_bind_input_panel(struct wl_client *client, void *data,
	uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&zwp_input_panel_v1_interface, 1, id);
	assert(resource);
	wl_resource_set_implementation(resource, &input_panel_impl, data, NULL);
}
