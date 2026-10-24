#include <assert.h>
#include <math.h>
#include <stdlib.h>

#include "core.h"
#include "output.h"
#include "wlr-output-management-unstable-v1-protocol.h"

// A zwlr_output_head_v1, with a zwlr_output_mode_v1 for each mode of the
// output. The resources become inert when the output is removed.
struct output_head {
	struct wl_list link; // output.heads
	struct wl_resource *resource;
	struct output *output;
	// Their data are the modes in output.modes. They stay after the head is
	// released, until the client releases them.
	struct wl_list modes; // zwlr_output_mode_v1 resource links
};

// A zwlr_output_configuration_v1. The client enables or disables every
// head, and changes the state of the enabled ones from the current one.
// Configurations made for an older state of the outputs are cancelled.
struct configuration {
	struct wl_resource *resource;
	uint32_t serial; // Of the configuration of the outputs it changes
	struct wl_list heads; // configuration_head.link
	bool used; // Applied or tested
};

struct configuration_head {
	struct wl_list link; // configuration.heads
	// The zwlr_output_configuration_head_v1, or NULL if the head is
	// disabled.
	struct wl_resource *resource;
	struct wl_list output_link; // output.configuration_heads
	// The mode is NULL if no mode matches. The output is NULL once it is
	// removed, which cancels the configuration.
	struct output_state state;
};

// The rest of the state is only sent for enabled outputs.
static void
head_send_state(struct output_head *head)
{
	struct output *output = head->output;
	zwlr_output_head_v1_send_enabled(head->resource, output->enabled);
	if (!output->enabled) {
		return;
	}
	struct wl_resource *mode;
	wl_resource_for_each(mode, &head->modes) {
		if (wl_resource_get_user_data(mode) == output->mode) {
			zwlr_output_head_v1_send_current_mode(head->resource, mode);
		}
	}
	zwlr_output_head_v1_send_position(head->resource, output->box.x, output->box.y);
	zwlr_output_head_v1_send_transform(head->resource, output->transform);
	zwlr_output_head_v1_send_scale(head->resource,
		wl_fixed_from_double(output->scale));
	if (wl_resource_get_version(head->resource) >= 4) {
		zwlr_output_head_v1_send_adaptive_sync(head->resource,
			output->adaptive_sync
				? ZWLR_OUTPUT_HEAD_V1_ADAPTIVE_SYNC_STATE_ENABLED
				: ZWLR_OUTPUT_HEAD_V1_ADAPTIVE_SYNC_STATE_DISABLED);
	}
}

// Tells the managers the new state of the outputs, with a new serial.
void
output_management_update(void)
{
	core.output_management.serial++;
	struct output *output;
	wl_list_for_each(output, &core.outputs, link) {
		struct output_head *head;
		wl_list_for_each(head, &output->heads, link) {
			head_send_state(head);
		}
	}
	struct wl_resource *resource;
	wl_resource_for_each(resource, &core.output_management.managers) {
		zwlr_output_manager_v1_send_done(resource, core.output_management.serial);
	}
}

static void
configuration_head_destroy(struct configuration_head *head)
{
	wl_list_remove(&head->link);
	wl_list_remove(&head->output_link);
	free(head);
}

static void
configuration_head_resource_destroy(struct wl_resource *resource)
{
	configuration_head_destroy(wl_resource_get_user_data(resource));
}

static void
configuration_head_set_mode_req(struct wl_client *client,
	struct wl_resource *resource, struct wl_resource *mode_resource)
{
	struct configuration_head *head = wl_resource_get_user_data(resource);
	const drmModeModeInfo *mode = wl_resource_get_user_data(mode_resource);
	struct output *output = head->state.output;
	// The mode or the output is removed.
	if (!mode || !output) {
		return;
	}
	for (int i = 0; i < output->nr_modes; i++) {
		if (&output->modes[i] == mode) {
			head->state.mode = mode;
			return;
		}
	}
	wl_resource_post_error(resource,
		ZWLR_OUTPUT_CONFIGURATION_HEAD_V1_ERROR_INVALID_MODE,
		"the mode is not of the head");
}

// Only the modes of the output can be set.
static void
configuration_head_set_custom_mode_req(struct wl_client *client,
	struct wl_resource *resource, int32_t width, int32_t height, int32_t refresh)
{
	struct configuration_head *head = wl_resource_get_user_data(resource);
	if (width <= 0 || height <= 0 || refresh < 0) {
		wl_resource_post_error(resource,
			ZWLR_OUTPUT_CONFIGURATION_HEAD_V1_ERROR_INVALID_CUSTOM_MODE,
			"invalid mode %dx%d@%d", width, height, refresh);
		return;
	}
	if (!head->state.output) {
		return;
	}
	head->state.mode = output_find_mode(head->state.output, width, height, refresh);
}

static void
configuration_head_set_position_req(struct wl_client *client,
	struct wl_resource *resource, int32_t x, int32_t y)
{
	struct configuration_head *head = wl_resource_get_user_data(resource);
	head->state.x = x;
	head->state.y = y;
}

static void
configuration_head_set_transform_req(struct wl_client *client,
	struct wl_resource *resource, int32_t transform)
{
	struct configuration_head *head = wl_resource_get_user_data(resource);
	if (transform < WL_OUTPUT_TRANSFORM_NORMAL
			|| transform > WL_OUTPUT_TRANSFORM_FLIPPED_270) {
		wl_resource_post_error(resource,
			ZWLR_OUTPUT_CONFIGURATION_HEAD_V1_ERROR_INVALID_TRANSFORM,
			"invalid transform %d", transform);
		return;
	}
	head->state.transform = (enum wl_output_transform)transform;
}

// Rounded to a multiple of 1/120 for fractional-scale-v1.
static void
configuration_head_set_scale_req(struct wl_client *client,
	struct wl_resource *resource, wl_fixed_t scale)
{
	struct configuration_head *head = wl_resource_get_user_data(resource);
	if (scale <= 0) {
		wl_resource_post_error(resource,
			ZWLR_OUTPUT_CONFIGURATION_HEAD_V1_ERROR_INVALID_SCALE,
			"invalid scale %f", wl_fixed_to_double(scale));
		return;
	}
	head->state.scale = MAX(round(wl_fixed_to_double(scale) * 120.0), 1.0) / 120.0;
}

static void
configuration_head_set_adaptive_sync_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t state)
{
	struct configuration_head *head = wl_resource_get_user_data(resource);
	if (state != ZWLR_OUTPUT_HEAD_V1_ADAPTIVE_SYNC_STATE_DISABLED
			&& state != ZWLR_OUTPUT_HEAD_V1_ADAPTIVE_SYNC_STATE_ENABLED) {
		wl_resource_post_error(resource,
			ZWLR_OUTPUT_CONFIGURATION_HEAD_V1_ERROR_INVALID_ADAPTIVE_SYNC_STATE,
			"invalid adaptive sync state %u", state);
		return;
	}
	head->state.adaptive_sync =
		state == ZWLR_OUTPUT_HEAD_V1_ADAPTIVE_SYNC_STATE_ENABLED;
}

static const struct zwlr_output_configuration_head_v1_interface
configuration_head_impl = {
	.set_mode = configuration_head_set_mode_req,
	.set_custom_mode = configuration_head_set_custom_mode_req,
	.set_position = configuration_head_set_position_req,
	.set_transform = configuration_head_set_transform_req,
	.set_scale = configuration_head_set_scale_req,
	.set_adaptive_sync = configuration_head_set_adaptive_sync_req,
};

// NULL after a protocol error. The heads of removed outputs have no output.
static struct configuration_head *
configuration_add_head(struct wl_resource *resource,
	struct wl_resource *head_resource)
{
	struct configuration *configuration = wl_resource_get_user_data(resource);
	struct output_head *output_head = wl_resource_get_user_data(head_resource);
	struct output *output = output_head ? output_head->output : NULL;
	if (configuration->used) {
		wl_resource_post_error(resource,
			ZWLR_OUTPUT_CONFIGURATION_V1_ERROR_ALREADY_USED,
			"the configuration has been used");
		return NULL;
	}
	struct configuration_head *head;
	wl_list_for_each(head, &configuration->heads, link) {
		if (output && head->state.output == output) {
			wl_resource_post_error(resource,
				ZWLR_OUTPUT_CONFIGURATION_V1_ERROR_ALREADY_CONFIGURED_HEAD,
				"the head has been configured");
			return NULL;
		}
	}
	head = calloc(1, sizeof(*head));
	assert(head);
	head->state.output = output;
	list_append(&configuration->heads, &head->link);
	if (output) {
		list_append(&output->configuration_heads, &head->output_link);
	} else {
		wl_list_init(&head->output_link);
	}
	return head;
}

static void
configuration_enable_head_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id, struct wl_resource *head_resource)
{
	struct configuration_head *head = configuration_add_head(resource,
		head_resource);
	if (!head) {
		return;
	}
	if (head->state.output) {
		head->state = output_current_state(head->state.output);
		head->state.enabled = true;
	}
	head->resource = wl_resource_create(client,
		&zwlr_output_configuration_head_v1_interface,
		wl_resource_get_version(resource), id);
	assert(head->resource);
	wl_resource_set_implementation(head->resource, &configuration_head_impl,
		head, configuration_head_resource_destroy);
}

static void
configuration_disable_head_req(struct wl_client *client,
	struct wl_resource *resource, struct wl_resource *head_resource)
{
	configuration_add_head(resource, head_resource);
}

// Custom modes must be modes of the output, adaptive sync must be supported,
// some output must stay enabled, since nothing could be seen to enable one
// again, and KMS must be able to show the outputs.
static void
configuration_use(struct wl_resource *resource, bool apply)
{
	struct configuration *configuration = wl_resource_get_user_data(resource);
	if (configuration->used) {
		wl_resource_post_error(resource,
			ZWLR_OUTPUT_CONFIGURATION_V1_ERROR_ALREADY_USED,
			"the configuration has been used");
		return;
	}
	configuration->used = true;
	// The heads may have changed since.
	if (configuration->serial != core.output_management.serial) {
		zwlr_output_configuration_v1_send_cancelled(resource);
		return;
	}
	// The heads are added once each.
	if (wl_list_length(&configuration->heads) != wl_list_length(&core.outputs)) {
		wl_resource_post_error(resource,
			ZWLR_OUTPUT_CONFIGURATION_V1_ERROR_UNCONFIGURED_HEAD,
			"not all heads are configured");
		return;
	}
	struct output_state states[MAX_OUTPUTS];
	int nr_states = 0;
	bool enabled = false;
	struct configuration_head *head;
	wl_list_for_each(head, &configuration->heads, link) {
		// Only buggy clients configure removed heads with the serial
		// sent after they were removed.
		if (!head->state.output) {
			zwlr_output_configuration_v1_send_cancelled(resource);
			return;
		}
		if (head->state.enabled && (!head->state.mode
				|| (head->state.adaptive_sync
					&& !head->state.output->adaptive_sync_supported))) {
			zwlr_output_configuration_v1_send_failed(resource);
			return;
		}
		enabled |= head->state.enabled;
		states[nr_states++] = head->state;
	}
	if (!enabled || !kms_test(states, nr_states)) {
		zwlr_output_configuration_v1_send_failed(resource);
		return;
	}
	zwlr_output_configuration_v1_send_succeeded(resource);
	if (!apply) {
		return;
	}
	for (int i = 0; i < nr_states; i++) {
		output_configure(&states[i]);
	}
	outputs_update();
}

static void
configuration_apply_req(struct wl_client *client, struct wl_resource *resource)
{
	configuration_use(resource, true);
}

static void
configuration_test_req(struct wl_client *client, struct wl_resource *resource)
{
	configuration_use(resource, false);
}

static const struct zwlr_output_configuration_v1_interface
configuration_impl = {
	.enable_head = configuration_enable_head_req,
	.disable_head = configuration_disable_head_req,
	.apply = configuration_apply_req,
	.test = configuration_test_req,
	.destroy = resource_destroy_req,
};

// The configuration heads are destroyed with the configuration.
static void
configuration_resource_destroy(struct wl_resource *resource)
{
	struct configuration *configuration = wl_resource_get_user_data(resource);
	struct configuration_head *head, *tmp;
	wl_list_for_each_safe(head, tmp, &configuration->heads, link) {
		if (head->resource) {
			wl_resource_destroy(head->resource);
		} else {
			configuration_head_destroy(head);
		}
	}
	free(configuration);
}

static void
manager_create_configuration_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id, uint32_t serial)
{
	struct configuration *configuration = calloc(1, sizeof(*configuration));
	assert(configuration);
	configuration->serial = serial;
	wl_list_init(&configuration->heads);
	configuration->resource = wl_resource_create(client,
		&zwlr_output_configuration_v1_interface,
		wl_resource_get_version(resource), id);
	assert(configuration->resource);
	wl_resource_set_implementation(configuration->resource,
		&configuration_impl, configuration, configuration_resource_destroy);
}

static void
manager_stop_req(struct wl_client *client, struct wl_resource *resource)
{
	zwlr_output_manager_v1_send_finished(resource);
	wl_resource_destroy(resource);
}

static const struct zwlr_output_manager_v1_interface manager_impl = {
	.create_configuration = manager_create_configuration_req,
	.stop = manager_stop_req,
};

static const struct zwlr_output_mode_v1_interface mode_impl = {
	.release = resource_destroy_req,
};

static void
head_send_mode(struct output_head *head, drmModeModeInfo *mode)
{
	struct wl_resource *resource = wl_resource_create(
		wl_resource_get_client(head->resource), &zwlr_output_mode_v1_interface,
		wl_resource_get_version(head->resource), 0);
	assert(resource);
	wl_resource_set_implementation(resource, &mode_impl, mode, resource_unlink);
	list_append(&head->modes, wl_resource_get_link(resource));
	zwlr_output_head_v1_send_mode(head->resource, resource);
	zwlr_output_mode_v1_send_size(resource, mode->hdisplay, mode->vdisplay);
	zwlr_output_mode_v1_send_refresh(resource, mode_refresh(mode));
	if (mode->type & DRM_MODE_TYPE_PREFERRED) {
		zwlr_output_mode_v1_send_preferred(resource);
	}
}

static const struct zwlr_output_head_v1_interface head_impl = {
	.release = resource_destroy_req,
};

static void
head_resource_destroy(struct wl_resource *resource)
{
	struct output_head *head = wl_resource_get_user_data(resource);
	// The output is removed.
	if (!head) {
		return;
	}
	struct wl_resource *mode, *tmp;
	wl_resource_for_each_safe(mode, tmp, &head->modes) {
		wl_list_remove(wl_resource_get_link(mode));
		wl_list_init(wl_resource_get_link(mode));
	}
	wl_list_remove(&head->link);
	free(head);
}

static void
manager_send_head(struct wl_resource *manager, struct output *output)
{
	struct output_head *head = calloc(1, sizeof(*head));
	assert(head);
	head->output = output;
	wl_list_init(&head->modes);
	head->resource = wl_resource_create(wl_resource_get_client(manager),
		&zwlr_output_head_v1_interface, wl_resource_get_version(manager), 0);
	assert(head->resource);
	wl_resource_set_implementation(head->resource, &head_impl, head,
		head_resource_destroy);
	list_append(&output->heads, &head->link);

	zwlr_output_manager_v1_send_head(manager, head->resource);
	zwlr_output_head_v1_send_name(head->resource, output->name);
	zwlr_output_head_v1_send_description(head->resource, output->description);
	if (output->physical_width > 0 && output->physical_height > 0) {
		zwlr_output_head_v1_send_physical_size(head->resource,
			output->physical_width, output->physical_height);
	}
	if (wl_resource_get_version(head->resource) >= 2) {
		zwlr_output_head_v1_send_make(head->resource, output->make);
		zwlr_output_head_v1_send_model(head->resource, output->model);
	}
	for (int i = 0; i < output->nr_modes; i++) {
		head_send_mode(head, &output->modes[i]);
	}
	head_send_state(head);
}

// The managers are told by output_management_update().
void
output_management_add_output(struct output *output)
{
	struct wl_resource *manager;
	wl_resource_for_each(manager, &core.output_management.managers) {
		manager_send_head(manager, output);
	}
}

// The heads and the modes of the output are finished, and the
// configurations for it lose it.
void
output_management_remove_output(struct output *output)
{
	struct output_head *head, *tmp;
	wl_list_for_each_safe(head, tmp, &output->heads, link) {
		struct wl_resource *mode, *tmp_mode;
		wl_resource_for_each_safe(mode, tmp_mode, &head->modes) {
			zwlr_output_mode_v1_send_finished(mode);
			wl_resource_set_user_data(mode, NULL);
			wl_list_remove(wl_resource_get_link(mode));
			wl_list_init(wl_resource_get_link(mode));
		}
		zwlr_output_head_v1_send_finished(head->resource);
		wl_resource_set_user_data(head->resource, NULL);
		wl_list_remove(&head->link);
		free(head);
	}
	struct configuration_head *configuration_head, *tmp_configuration_head;
	wl_list_for_each_safe(configuration_head, tmp_configuration_head,
			&output->configuration_heads, output_link) {
		configuration_head->state.output = NULL;
		wl_list_remove(&configuration_head->output_link);
		wl_list_init(&configuration_head->output_link);
	}
}

void
wayland_bind_wlr_output_manager(struct wl_client *client, void *data,
	uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&zwlr_output_manager_v1_interface, version, id);
	assert(resource);
	wl_resource_set_implementation(resource, &manager_impl, NULL, resource_unlink);
	list_append(&core.output_management.managers, wl_resource_get_link(resource));
	struct output *output;
	wl_list_for_each(output, &core.outputs, link) {
		manager_send_head(resource, output);
	}
	zwlr_output_manager_v1_send_done(resource, core.output_management.serial);
}
