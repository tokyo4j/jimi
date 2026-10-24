#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "core.h"
#include "image_capture.h"
#include "output.h"
#include "surface.h"
#include "xdg-output-unstable-v1-protocol.h"

// The enabled output containing the point, or else the one nearest to it.
// NULL if no output is enabled.
struct output *
output_nearest(double x, double y)
{
	struct output *nearest = NULL;
	double min_distance = INFINITY;
	struct output *output;
	wl_list_for_each(output, &core.outputs, link) {
		if (!output->enabled) {
			continue;
		}
		if (box_contains_point(&output->box, x, y)) {
			return output;
		}
		double dx = x - clamp(x, output->box.x, output->box.x + output->box.width);
		double dy = y - clamp(y, output->box.y, output->box.y + output->box.height);
		if (dx * dx + dy * dy < min_distance) {
			min_distance = dx * dx + dy * dy;
			nearest = output;
		}
	}
	return nearest;
}

// The enabled output of the name, or NULL.
struct output *
output_find(const char *name)
{
	struct output *output;
	wl_list_for_each(output, &core.outputs, link) {
		if (output->enabled && strcmp(output->name, name) == 0) {
			return output;
		}
	}
	return NULL;
}

// The output in physical pixels, in the orientation of the layout.
struct box
output_physical_box(struct output *output)
{
	struct box box = {
		.width = output->mode->hdisplay,
		.height = output->mode->vdisplay,
	};
	box_transform(&box, &box, output->transform, box.width, box.height);
	return box;
}

// In mHz
int
mode_refresh(const drmModeModeInfo *mode)
{
	return (int)(((int64_t)mode->clock * 1000000 / mode->htotal
		+ mode->vtotal / 2) / mode->vtotal);
}

// In ns. The clock is in kHz.
int64_t
mode_refresh_interval(const drmModeModeInfo *mode)
{
	return (int64_t)mode->htotal * mode->vtotal * 1000000 / mode->clock;
}

// The mode of the size with the closest refresh rate in mHz, or the highest
// one if the rate is 0. NULL if the output has no mode of the size.
const drmModeModeInfo *
output_find_mode(struct output *output, int width, int height, int refresh)
{
	const drmModeModeInfo *found = NULL;
	int min_distance = INT_MAX;
	for (int i = 0; i < output->nr_modes; i++) {
		const drmModeModeInfo *mode = &output->modes[i];
		if (mode->hdisplay != width || mode->vdisplay != height) {
			continue;
		}
		int distance = refresh ? abs(mode_refresh(mode) - refresh)
			: -mode_refresh(mode);
		if (distance < min_distance) {
			found = mode;
			min_distance = distance;
		}
	}
	return found;
}

// The preferred mode, or else the first one.
static const drmModeModeInfo *
preferred_mode(struct output *output)
{
	for (int i = 0; i < output->nr_modes; i++) {
		if (output->modes[i].type & DRM_MODE_TYPE_PREFERRED) {
			return &output->modes[i];
		}
	}
	return &output->modes[0];
}

// Draws the server cursors and the decorations again for the scale and the
// config. The replaced textures stay alive until the GPU is done with them.
static void
update_textures(struct output *output)
{
	server_cursors_destroy(output->server_cursors);
	server_cursors_load(output->server_cursors, output->scale);
	decoration_icons_destroy(output);
	decoration_icons_create(output);
	struct window *window;
	wl_list_for_each(window, &core.windows, link) {
		if (window->output == output) {
			decoration_redraw(window);
		}
	}
}

static void
send_wl_output_state(struct output *output, struct wl_resource *resource)
{
	wl_output_send_geometry(resource, output->box.x, output->box.y,
		output->physical_width, output->physical_height,
		WL_OUTPUT_SUBPIXEL_UNKNOWN, output->make, output->model, output->transform);
	uint32_t flags = WL_OUTPUT_MODE_CURRENT;
	if (output->mode->type & DRM_MODE_TYPE_PREFERRED) {
		flags |= WL_OUTPUT_MODE_PREFERRED;
	}
	wl_output_send_mode(resource, flags, output->mode->hdisplay,
		output->mode->vdisplay, mode_refresh(output->mode));
	if (wl_resource_get_version(resource) >= 2) {
		wl_output_send_scale(resource, (int32_t)ceil(output->scale));
	}
}

static void
send_xdg_output_state(struct output *output, struct wl_resource *resource)
{
	zxdg_output_v1_send_logical_position(resource, output->box.x, output->box.y);
	zxdg_output_v1_send_logical_size(resource, output->box.width,
		output->box.height);
}

// wl_output.done follows the events of version 3 of zxdg_output_v1 instead of
// zxdg_output_v1.done.
static void
send_state(struct output *output)
{
	struct wl_resource *resource;
	wl_resource_for_each(resource, &output->xdg_resources) {
		send_xdg_output_state(output, resource);
		if (wl_resource_get_version(resource) < 3) {
			zxdg_output_v1_send_done(resource);
		}
	}
	wl_resource_for_each(resource, &output->resources) {
		send_wl_output_state(output, resource);
		if (wl_resource_get_version(resource) >= 2) {
			wl_output_send_done(resource);
		}
	}
}

// Clients may bind a removed global until they learn that it is gone, so it is
// destroyed once all the registries announcing it acknowledge the removal with
// wl_fixes or are destroyed. Until then, binding it creates inert resources.
static void
remove_global(struct wl_global *global)
{
	wl_global_set_user_data(global, NULL);
	wl_global_set_withdrawn_listener(global, wl_global_destroy);
	wl_global_remove(global);
}

// The resources stay until the clients destroy them, without an output.
static void
make_resources_inert(struct wl_list *resources)
{
	struct wl_resource *resource, *tmp;
	wl_resource_for_each_safe(resource, tmp, resources) {
		wl_resource_set_user_data(resource, NULL);
		wl_list_remove(wl_resource_get_link(resource));
		wl_list_init(wl_resource_get_link(resource));
	}
}

// Closes the layer surfaces, the lock surface and the captures of the output,
// removes it from the clients, and turns it off. The windows move off it by
// outputs_update(), drawn with its textures until then.
static void
output_disable(struct output *output)
{
	output->enabled = false;
	layer_surfaces_close(output);
	if (output->lock_surface) {
		lock_surface_destroy(output->lock_surface);
	}
	image_capture_output_disable(output);
	// The surfaces leave it while the clients still have its resources.
	surfaces_update_outputs();
	remove_global(output->global);
	output->global = NULL;
	workspaces_output_disable(output);
	make_resources_inert(&output->resources);
	make_resources_inert(&output->xdg_resources);
	make_resources_inert(&output->color_resources);
	kms_disable(output);
	// The content updates waiting for its frames are never shown.
	presentation_feedbacks_discard(&output->pending_feedbacks);
	swapchain_destroy(output);
	output->scanout_frames = 0;
	dmabuf_feedback_unset_scanout_output(output);
}

// The colors of SDR, or else BT.2020 encoded by PQ, which targets the color
// volume of the monitor and shows SDR white at the SDR luminance. Clients may
// get the parameters.
static struct image_description *
create_image_description(struct output *output)
{
	struct image_description params = srgb_description;
	if (output->hdr) {
		params = (struct image_description){
			.primaries_named = WP_COLOR_MANAGER_V1_PRIMARIES_BT2020,
			.tf_named = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ,
			.min_luminance = output->display_min_luminance,
			.max_luminance = output->display_min_luminance + 10000.0,
			.reference_luminance = output->sdr_luminance,
			.target_primaries = output->display_primaries,
			.target_min_luminance = output->display_min_luminance,
			.target_max_luminance = output->display_max_luminance,
		};
		color_primaries_from_named(params.primaries_named, &params.primaries);
	}
	params.allows_information = true;
	return image_description_create(&params);
}

// The surfaces are told to draw in the new colors, and the night light
// follows them.
static void
update_image_description(struct output *output)
{
	image_description_unref(output->image_description);
	output->image_description = create_image_description(output);
	color_management_output_changed(output);
	night_light_update_output(output);
}

// Applies the state, and tells the clients. A new mode, adaptive sync or HDR
// is set by a modeset at the next repaint, which disables the output if it
// fails. KMS changes adaptive sync without a full modeset if it can.
// outputs_update() follows for the new layout.
void
output_configure(const struct output_state *state)
{
	struct output *output = state->output;
	if (!state->enabled) {
		if (output->enabled) {
			output_disable(output);
		}
		return;
	}
	bool enabling = !output->enabled;
	if (enabling) {
		output->enabled = true;
		output->modeset_done = false;
		output->global = wl_global_create(core.display, &wl_output_interface,
			4, output, wayland_bind_output);
		assert(output->global);
	}
	if (state->mode != output->mode
			|| state->adaptive_sync != output->adaptive_sync
			|| state->hdr != output->hdr) {
		output->mode = state->mode;
		output->adaptive_sync = state->adaptive_sync;
		output->modeset_done = false;
	}
	if (!output->image_description || state->hdr != output->hdr
			|| state->sdr_luminance != output->sdr_luminance) {
		output->hdr = state->hdr;
		output->sdr_luminance = state->sdr_luminance;
		update_image_description(output);
	}
	output->transform = state->transform;
	struct box physical = output_physical_box(output);
	// Rounded up so that the layout covers every physical pixel.
	output->box = (struct box){
		.x = state->x,
		.y = state->y,
		.width = (int)ceil(physical.width / state->scale),
		.height = (int)ceil(physical.height / state->scale),
	};
	// The textures of disabled outputs may be outdated by the config.
	if (enabling || state->scale != output->scale) {
		output->scale = state->scale;
		update_textures(output);
	}
	// Also arranges the windows for the new usable area.
	output_arrange_layer_surfaces(output);
	output_arrange_lock_surface(output);
	send_state(output);
	damage_output(output);
}

static const struct wl_output_interface output_impl = {
	.release = resource_destroy_req,
};

void
wayland_bind_output(struct wl_client *client, void *data, uint32_t version,
	uint32_t id)
{
	struct output *output = data;
	struct wl_resource *resource = wl_resource_create(client, &wl_output_interface,
		MIN(version, 4), id);
	assert(resource);
	// The global is removed.
	if (!output) {
		wl_resource_set_implementation(resource, &output_impl, NULL, NULL);
		return;
	}
	wl_resource_set_implementation(resource, &output_impl, output, resource_unlink);
	list_append(&output->resources, wl_resource_get_link(resource));
	send_wl_output_state(output, resource);
	if (wl_resource_get_version(resource) >= 4) {
		wl_output_send_name(resource, output->name);
		wl_output_send_description(resource, output->description);
	}
	if (wl_resource_get_version(resource) >= 2) {
		wl_output_send_done(resource);
	}
	surfaces_send_enter(output, resource);
	foreign_toplevel_output_bind(output, resource);
	workspaces_output_bind(output, resource);
}

static const struct zxdg_output_v1_interface xdg_output_impl = {
	.destroy = resource_destroy_req,
};

static void
xdg_output_manager_get_xdg_output_req(struct wl_client *client,
	struct wl_resource *manager_resource, uint32_t id,
	struct wl_resource *output_resource)
{
	struct output *output = wl_resource_get_user_data(output_resource);
	uint32_t version = wl_resource_get_version(manager_resource);
	struct wl_resource *resource = wl_resource_create(client,
		&zxdg_output_v1_interface, version, id);
	assert(resource);
	// The output is disabled.
	if (!output) {
		wl_resource_set_implementation(resource, &xdg_output_impl, NULL, NULL);
		return;
	}
	wl_resource_set_implementation(resource, &xdg_output_impl, output,
		resource_unlink);
	list_append(&output->xdg_resources, wl_resource_get_link(resource));
	send_xdg_output_state(output, resource);
	if (version >= 2) {
		zxdg_output_v1_send_name(resource, output->name);
		zxdg_output_v1_send_description(resource, output->description);
	}
	if (version < 3) {
		zxdg_output_v1_send_done(resource);
	} else if (wl_resource_get_version(output_resource) >= 2) {
		wl_output_send_done(output_resource);
	}
}

static const struct zxdg_output_manager_v1_interface xdg_output_manager_impl = {
	.destroy = resource_destroy_req,
	.get_xdg_output = xdg_output_manager_get_xdg_output_req,
};

void
wayland_bind_xdg_output_manager(struct wl_client *client, void *data,
	uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&zxdg_output_manager_v1_interface, version, id);
	assert(resource);
	wl_resource_set_implementation(resource, &xdg_output_manager_impl, NULL, NULL);
}

// The configured state, or else the preferred mode at scale 1.
static struct output_state
configured_state(struct output *output)
{
	// Unless the config positions it, an enabled output stays where it is,
	// and the others are placed right of the enabled outputs.
	int x = 0;
	int y = 0;
	if (output->enabled) {
		x = output->box.x;
		y = output->box.y;
	} else {
		struct output *other;
		wl_list_for_each(other, &core.outputs, link) {
			if (other->enabled) {
				x = MAX(x, other->box.x + other->box.width);
			}
		}
	}
	struct output_state state = {
		.output = output,
		.enabled = true,
		.mode = preferred_mode(output),
		.x = x,
		.y = y,
		.scale = 1.0,
		.transform = WL_OUTPUT_TRANSFORM_NORMAL,
		.sdr_luminance = DEFAULT_SDR_LUMINANCE,
	};
	const struct output_config *output_config = config_find_output(output->name);
	if (!output_config) {
		return state;
	}
	if (output_config->width) {
		const drmModeModeInfo *mode = output_find_mode(output,
			output_config->width, output_config->height, output_config->refresh);
		if (mode) {
			state.mode = mode;
		} else {
			fprintf(stderr, "%s has no mode %dx%d\n", output->name,
				output_config->width, output_config->height);
		}
	}
	state.scale = output_config->scale;
	state.transform = output_config->transform;
	if (output_config->adaptive_sync && output->adaptive_sync_supported) {
		state.adaptive_sync = true;
	} else if (output_config->adaptive_sync) {
		fprintf(stderr, "%s does not support adaptive sync\n", output->name);
	}
	// The primary plane may not scan out the images of HDR that the
	// renderer can draw.
	if (output_config->hdr && output->hdr_supported
			&& scanout_format_supported(&output->primary, HDR_SCANOUT_FORMAT)) {
		state.hdr = true;
	} else if (output_config->hdr) {
		fprintf(stderr, "%s does not support HDR\n", output->name);
	}
	state.sdr_luminance = output_config->sdr_luminance;
	if (output_config->positioned) {
		state.x = output_config->x;
		state.y = output_config->y;
	}
	return state;
}

// The state to change the output from. Outputs that have never been enabled
// have the configured state, disabled.
struct output_state
output_current_state(struct output *output)
{
	if (!output->mode) {
		struct output_state state = configured_state(output);
		state.enabled = false;
		return state;
	}
	return (struct output_state){
		.output = output,
		.enabled = output->enabled,
		.mode = output->mode,
		.x = output->box.x,
		.y = output->box.y,
		.scale = output->scale,
		.transform = output->transform,
		.adaptive_sync = output->adaptive_sync,
		.hdr = output->hdr,
		.sdr_luminance = output->sdr_luminance,
	};
}

// Enables the output as configured, or else with the preferred mode in SDR,
// and shows it to the output managers. It stays disabled if KMS cannot show
// it along with the other outputs, e.g. for lack of bandwidth.
static void
output_add(struct output *output)
{
	scene_tree_init(&output->top_layer, &core.scene.layers[SCENE_LAYER_TOP]);
	struct output_state state = configured_state(output);
	if (!kms_test(&state, 1)) {
		state.mode = preferred_mode(output);
		state.hdr = false;
		state.enabled = kms_test(&state, 1);
		if (!state.enabled) {
			fprintf(stderr, "%s cannot be enabled\n", output->name);
		}
	}
	output_configure(&state);
	output_management_add_output(output);
}

// Configures the outputs connected at startup.
void
outputs_init(void)
{
	struct output *output;
	wl_list_for_each(output, &core.outputs, link) {
		output_add(output);
		output->last_box = output->box;
	}
}

// Moves the windows and the pointer for the new layout of the outputs, and
// tells the output managers. Switching windows is cancelled, since the grid
// is on an output.
void
outputs_update(void)
{
	if (core.window_switcher.selected) {
		window_switcher_cancel();
	}
	windows_update_layout();
	struct output *output;
	wl_list_for_each(output, &core.outputs, link) {
		output->last_box = output->box;
	}
	pointer_handle_output_change();
	output_management_update();
}

// Applies the config to the connected outputs, unless KMS cannot show them
// so, and draws the cursors and the decorations on them again for the config.
void
outputs_reload(void)
{
	struct output_state states[MAX_OUTPUTS];
	int nr_states = 0;
	struct output *output;
	wl_list_for_each(output, &core.outputs, link) {
		states[nr_states++] = configured_state(output);
	}
	if (kms_test(states, nr_states)) {
		for (int i = 0; i < nr_states; i++) {
			output_configure(&states[i]);
		}
		outputs_update();
	} else {
		fprintf(stderr, "Cannot show the outputs as configured\n");
	}
	wl_list_for_each(output, &core.outputs, link) {
		if (output->enabled) {
			update_textures(output);
			// The decorations may take more or less of the usable
			// area.
			windows_arrange(output);
			damage_output(output);
		}
	}
}

// Called once the connector of the output is connected.
void
output_connect(struct output *output)
{
	output_add(output);
	outputs_update();
}

// Called before the output is freed, once the windows leave it.
void
output_disconnect(struct output *output)
{
	if (output->enabled) {
		output_disable(output);
	}
	output_management_remove_output(output);
	outputs_update();
	server_cursors_destroy(output->server_cursors);
	decoration_icons_destroy(output);
	scene_tree_fini(&output->top_layer);
}
