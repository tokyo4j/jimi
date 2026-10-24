#include <assert.h>
#include <math.h>
#include <stdlib.h>

#include "color.h"
#include "core.h"
#include "output.h"
#include "surface.h"

// Tells clients the colors of the outputs, and lets them describe the colors
// of their surfaces, which the renderer converts into those of the outputs.
// Image descriptions are records, which the resources and the surface states
// reference.

#define COLOR_MANAGER_VERSION 3

static const struct wp_image_description_v1_interface image_description_impl;

// The parameters set so far, which are required unless they have defaults.
struct params_creator {
	struct image_description params;
	bool tf_set;
	bool primaries_set;
	bool luminances_set;
	bool target_primaries_set;
	bool target_luminance_set;
	bool max_cll_set;
	bool max_fall_set;
};

static void
image_description_resource_destroy(struct wl_resource *resource)
{
	image_description_unref(wl_resource_get_user_data(resource));
}

// The new wp_image_description_v1 takes the reference to the record, and is
// ready. Without a record, it has failed for the cause.
static void
image_description_resource_create(struct wl_client *client, uint32_t version,
	uint32_t id, struct image_description *description, uint32_t cause,
	const char *message)
{
	struct wl_resource *resource = wl_resource_create(client,
		&wp_image_description_v1_interface, version, id);
	assert(resource);
	wl_resource_set_implementation(resource, &image_description_impl,
		description, image_description_resource_destroy);
	if (!description) {
		wp_image_description_v1_send_failed(resource, cause, message);
	} else if (version >= 2) {
		wp_image_description_v1_send_ready2(resource,
			(uint32_t)(description->identity >> 32),
			(uint32_t)description->identity);
	} else {
		wp_image_description_v1_send_ready(resource,
			(uint32_t)description->identity);
	}
}

// Chromaticities and luminances are sent in fixed point.
static int32_t
fixed(double value, double factor)
{
	return (int32_t)lround(value * factor);
}

// The chromaticities of red, green, blue and white, in order
static void
primaries_to_fixed(const struct color_primaries *primaries, int32_t xy[8])
{
	const double *colors[] = {primaries->red, primaries->green,
		primaries->blue, primaries->white};
	for (int i = 0; i < 4; i++) {
		xy[i * 2] = fixed(colors[i][0], 1e6);
		xy[i * 2 + 1] = fixed(colors[i][1], 1e6);
	}
}

// Sends all the parameters, after which the info object is destroyed.
static void
image_description_get_information_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id)
{
	struct image_description *description = wl_resource_get_user_data(resource);
	if (!description) {
		wl_resource_post_error(resource, WP_IMAGE_DESCRIPTION_V1_ERROR_NOT_READY,
			"the image description is not ready");
		return;
	}
	if (!description->allows_information) {
		wl_resource_post_error(resource,
			WP_IMAGE_DESCRIPTION_V1_ERROR_NO_INFORMATION,
			"the image description does not allow get_information");
		return;
	}
	struct wl_resource *info = wl_resource_create(client,
		&wp_image_description_info_v1_interface,
		wl_resource_get_version(resource), id);
	assert(info);
	int32_t xy[8];
	primaries_to_fixed(&description->primaries, xy);
	wp_image_description_info_v1_send_primaries(info, xy[0], xy[1], xy[2],
		xy[3], xy[4], xy[5], xy[6], xy[7]);
	if (description->primaries_named) {
		wp_image_description_info_v1_send_primaries_named(info,
			description->primaries_named);
	}
	if (description->tf_named) {
		wp_image_description_info_v1_send_tf_named(info, description->tf_named);
	} else {
		wp_image_description_info_v1_send_tf_power(info,
			(uint32_t)fixed(description->tf_power, 1e4));
	}
	wp_image_description_info_v1_send_luminances(info,
		(uint32_t)fixed(description->min_luminance, 1e4),
		(uint32_t)fixed(description->max_luminance, 1),
		(uint32_t)fixed(description->reference_luminance, 1));
	primaries_to_fixed(&description->target_primaries, xy);
	wp_image_description_info_v1_send_target_primaries(info, xy[0], xy[1],
		xy[2], xy[3], xy[4], xy[5], xy[6], xy[7]);
	wp_image_description_info_v1_send_target_luminance(info,
		(uint32_t)fixed(description->target_min_luminance, 1e4),
		(uint32_t)fixed(description->target_max_luminance, 1));
	if (description->max_cll > 0.0) {
		wp_image_description_info_v1_send_target_max_cll(info,
			(uint32_t)fixed(description->max_cll, 1));
	}
	if (description->max_fall > 0.0) {
		wp_image_description_info_v1_send_target_max_fall(info,
			(uint32_t)fixed(description->max_fall, 1));
	}
	wp_image_description_info_v1_send_done(info);
	wl_resource_destroy(info);
}

static const struct wp_image_description_v1_interface image_description_impl = {
	.destroy = resource_destroy_req,
	.get_information = image_description_get_information_req,
};

// The output is gone once the resource is inert.
static void
output_get_image_description_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id)
{
	struct output *output = wl_resource_get_user_data(resource);
	uint32_t version = wl_resource_get_version(resource);
	if (!output) {
		image_description_resource_create(client, version, id, NULL,
			WP_IMAGE_DESCRIPTION_V1_CAUSE_NO_OUTPUT, "the output is gone");
		return;
	}
	image_description_resource_create(client, version, id,
		image_description_ref(output->image_description), 0, NULL);
}

static const struct wp_color_management_output_v1_interface output_impl = {
	.destroy = resource_destroy_req,
	.get_image_description = output_get_image_description_req,
};

// Followed by wl_output.done.
void
color_management_output_changed(struct output *output)
{
	struct wl_resource *resource;
	wl_resource_for_each(resource, &output->color_resources) {
		wp_color_management_output_v1_send_image_description_changed(resource);
	}
}

// The surface goes back to sRGB on the next commit.
static void
surface_resource_destroy(struct wl_resource *resource)
{
	struct surface *surface = wl_resource_get_user_data(resource);
	if (!surface) {
		return;
	}
	surface->color_management = NULL;
	image_description_unref(surface->pending.image_description);
	surface->pending.image_description = NULL;
}

static void
surface_set_image_description_req(struct wl_client *client,
	struct wl_resource *resource, struct wl_resource *description_resource,
	uint32_t render_intent)
{
	struct surface *surface = wl_resource_get_user_data(resource);
	struct image_description *description =
		wl_resource_get_user_data(description_resource);
	if (!surface) {
		wl_resource_post_error(resource, WP_COLOR_MANAGEMENT_SURFACE_V1_ERROR_INERT,
			"the surface is gone");
		return;
	}
	if (!description) {
		wl_resource_post_error(resource,
			WP_COLOR_MANAGEMENT_SURFACE_V1_ERROR_IMAGE_DESCRIPTION,
			"the image description is not ready");
		return;
	}
	if (render_intent != WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL) {
		wl_resource_post_error(resource,
			WP_COLOR_MANAGEMENT_SURFACE_V1_ERROR_RENDER_INTENT,
			"unsupported rendering intent");
		return;
	}
	image_description_unref(surface->pending.image_description);
	surface->pending.image_description = image_description_ref(description);
}

static void
surface_unset_image_description_req(struct wl_client *client,
	struct wl_resource *resource)
{
	struct surface *surface = wl_resource_get_user_data(resource);
	if (!surface) {
		wl_resource_post_error(resource, WP_COLOR_MANAGEMENT_SURFACE_V1_ERROR_INERT,
			"the surface is gone");
		return;
	}
	image_description_unref(surface->pending.image_description);
	surface->pending.image_description = NULL;
}

static const struct wp_color_management_surface_v1_interface surface_impl = {
	.destroy = resource_destroy_req,
	.set_image_description = surface_set_image_description_req,
	.unset_image_description = surface_unset_image_description_req,
};

// The descriptions of the outputs are parametric.
static void
feedback_get_preferred_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id)
{
	struct surface *surface = wl_resource_get_user_data(resource);
	if (!surface) {
		wl_resource_post_error(resource,
			WP_COLOR_MANAGEMENT_SURFACE_FEEDBACK_V1_ERROR_INERT,
			"the surface is gone");
		return;
	}
	// Without an output, the surface is preferred in sRGB.
	struct image_description *preferred = surface->preferred_image_description;
	if (preferred) {
		image_description_ref(preferred);
	} else {
		struct image_description params = srgb_description;
		params.allows_information = true;
		preferred = image_description_create(&params);
	}
	image_description_resource_create(client,
		wl_resource_get_version(resource), id, preferred, 0, NULL);
}

static const struct wp_color_management_surface_feedback_v1_interface
		feedback_impl = {
	.destroy = resource_destroy_req,
	.get_preferred = feedback_get_preferred_req,
	.get_preferred_parametric = feedback_get_preferred_req,
};

// Sent when the output that the surface is preferred on has changed, or its
// image description.
void
color_management_send_preferred(struct surface *surface)
{
	uint64_t identity = surface->preferred_image_description->identity;
	struct wl_resource *resource;
	wl_resource_for_each(resource, &surface->color_feedbacks) {
		if (wl_resource_get_version(resource) >= 2) {
			wp_color_management_surface_feedback_v1_send_preferred_changed2(
				resource, (uint32_t)(identity >> 32), (uint32_t)identity);
		} else {
			wp_color_management_surface_feedback_v1_send_preferred_changed(
				resource, (uint32_t)identity);
		}
	}
}

// The objects stay alive, but become inert.
void
color_management_surface_destroy(struct surface *surface)
{
	if (surface->color_management) {
		wl_resource_set_user_data(surface->color_management, NULL);
	}
	struct wl_resource *resource, *tmp;
	wl_resource_for_each_safe(resource, tmp, &surface->color_feedbacks) {
		wl_resource_set_user_data(resource, NULL);
		wl_list_remove(wl_resource_get_link(resource));
		wl_list_init(wl_resource_get_link(resource));
	}
}

static void
params_resource_destroy(struct wl_resource *resource)
{
	free(wl_resource_get_user_data(resource));
}

static bool
check_unset(struct wl_resource *resource, bool *set)
{
	if (*set) {
		wl_resource_post_error(resource,
			WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_ALREADY_SET,
			"the property is already set");
		return false;
	}
	*set = true;
	return true;
}

static void
params_set_tf_named_req(struct wl_client *client, struct wl_resource *resource,
	uint32_t tf)
{
	struct params_creator *creator = wl_resource_get_user_data(resource);
	bool supported = false;
	switch (tf) {
	case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_COMPOUND_POWER_2_4:
		supported = wl_resource_get_version(resource) >= 2;
		break;
	case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22:
	case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA28:
	case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_BT1886:
	case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR:
	case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ:
		supported = true;
		break;
	}
	if (!supported) {
		wl_resource_post_error(resource,
			WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_TF,
			"unsupported transfer function");
		return;
	}
	if (check_unset(resource, &creator->tf_set)) {
		creator->params.tf_named = tf;
	}
}

static void
params_set_tf_power_req(struct wl_client *client, struct wl_resource *resource,
	uint32_t eexp)
{
	struct params_creator *creator = wl_resource_get_user_data(resource);
	if (eexp < 10000 || eexp > 100000) {
		wl_resource_post_error(resource,
			WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_TF,
			"the exponent must be from 1 to 10");
		return;
	}
	if (check_unset(resource, &creator->tf_set)) {
		creator->params.tf_power = eexp / 1e4;
	}
}

static void
params_set_primaries_named_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t primaries)
{
	struct params_creator *creator = wl_resource_get_user_data(resource);
	struct color_primaries named;
	if (!color_primaries_from_named(primaries, &named)) {
		wl_resource_post_error(resource,
			WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_PRIMARIES_NAMED,
			"unsupported primaries");
		return;
	}
	if (check_unset(resource, &creator->primaries_set)) {
		creator->params.primaries_named = primaries;
		creator->params.primaries = named;
	}
}

static struct color_primaries
primaries_from_fixed(int32_t r_x, int32_t r_y, int32_t g_x, int32_t g_y,
	int32_t b_x, int32_t b_y, int32_t w_x, int32_t w_y)
{
	return (struct color_primaries){
		.red = {r_x / 1e6, r_y / 1e6},
		.green = {g_x / 1e6, g_y / 1e6},
		.blue = {b_x / 1e6, b_y / 1e6},
		.white = {w_x / 1e6, w_y / 1e6},
	};
}

static void
params_set_primaries_req(struct wl_client *client,
	struct wl_resource *resource, int32_t r_x, int32_t r_y, int32_t g_x,
	int32_t g_y, int32_t b_x, int32_t b_y, int32_t w_x, int32_t w_y)
{
	struct params_creator *creator = wl_resource_get_user_data(resource);
	if (check_unset(resource, &creator->primaries_set)) {
		creator->params.primaries = primaries_from_fixed(r_x, r_y, g_x, g_y,
			b_x, b_y, w_x, w_y);
	}
}

static void
params_set_luminances_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t min_lum, uint32_t max_lum,
	uint32_t reference_lum)
{
	struct params_creator *creator = wl_resource_get_user_data(resource);
	double min = min_lum / 1e4;
	if (max_lum <= min || reference_lum <= min) {
		wl_resource_post_error(resource,
			WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_LUMINANCE,
			"the luminances must be above the minimum");
		return;
	}
	if (check_unset(resource, &creator->luminances_set)) {
		creator->params.min_luminance = min;
		creator->params.max_luminance = max_lum;
		creator->params.reference_luminance = reference_lum;
	}
}

static void
params_set_mastering_display_primaries_req(struct wl_client *client,
	struct wl_resource *resource, int32_t r_x, int32_t r_y, int32_t g_x,
	int32_t g_y, int32_t b_x, int32_t b_y, int32_t w_x, int32_t w_y)
{
	struct params_creator *creator = wl_resource_get_user_data(resource);
	if (check_unset(resource, &creator->target_primaries_set)) {
		creator->params.target_primaries = primaries_from_fixed(r_x, r_y,
			g_x, g_y, b_x, b_y, w_x, w_y);
	}
}

static void
params_set_mastering_luminance_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t min_lum, uint32_t max_lum)
{
	struct params_creator *creator = wl_resource_get_user_data(resource);
	double min = min_lum / 1e4;
	if (max_lum <= min) {
		wl_resource_post_error(resource,
			WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_LUMINANCE,
			"the maximum must be above the minimum");
		return;
	}
	if (check_unset(resource, &creator->target_luminance_set)) {
		creator->params.target_min_luminance = min;
		creator->params.target_max_luminance = max_lum;
	}
}

static void
params_set_max_cll_req(struct wl_client *client, struct wl_resource *resource,
	uint32_t max_cll)
{
	struct params_creator *creator = wl_resource_get_user_data(resource);
	if (check_unset(resource, &creator->max_cll_set)) {
		creator->params.max_cll = max_cll;
	}
}

static void
params_set_max_fall_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t max_fall)
{
	struct params_creator *creator = wl_resource_get_user_data(resource);
	if (check_unset(resource, &creator->max_fall_set)) {
		creator->params.max_fall = max_fall;
	}
}

// Version 1 also requires the content light levels within the mastering
// luminances.
static bool
light_levels_valid(const struct params_creator *creator, uint32_t version)
{
	const struct image_description *params = &creator->params;
	if (creator->max_cll_set && creator->max_fall_set
			&& params->max_fall > params->max_cll) {
		return false;
	}
	if (version >= 2) {
		return true;
	}
	double min = params->target_min_luminance;
	double max = params->target_max_luminance;
	return (!creator->max_cll_set
			|| (params->max_cll > min && params->max_cll <= max))
		&& (!creator->max_fall_set
			|| (params->max_fall > min && params->max_fall <= max));
}

// Fills in the defaults, which follow the transfer function, and the
// primaries and luminances of the primary color volume. PQ always spans
// 10000 cd/m² from the minimum.
static void
params_create_req(struct wl_client *client, struct wl_resource *resource,
	uint32_t id)
{
	struct params_creator *creator = wl_resource_get_user_data(resource);
	uint32_t version = wl_resource_get_version(resource);
	struct image_description *params = &creator->params;
	if (!creator->tf_set || !creator->primaries_set) {
		wl_resource_post_error(resource,
			WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INCOMPLETE_SET,
			"the transfer function and the primaries are required");
		return;
	}
	if (!creator->luminances_set) {
		image_description_set_default_luminances(params);
	}
	if (params->tf_named == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ) {
		params->max_luminance = params->min_luminance + 10000.0;
	}
	if (!creator->target_primaries_set) {
		params->target_primaries = params->primaries;
	}
	if (!creator->target_luminance_set) {
		params->target_min_luminance = params->min_luminance;
		params->target_max_luminance = params->max_luminance;
	}
	if (!light_levels_valid(creator, version)) {
		wl_resource_post_error(resource,
			WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_LUMINANCE,
			"invalid content light levels");
		return;
	}
	double matrix[3][3];
	struct image_description *description = NULL;
	if (color_primaries_to_xyz(&params->primaries, matrix)) {
		description = image_description_create(params);
	}
	image_description_resource_create(client, version, id, description,
		WP_IMAGE_DESCRIPTION_V1_CAUSE_UNSUPPORTED,
		"the primaries do not span a color space");
	wl_resource_destroy(resource);
}

static const struct wp_image_description_creator_params_v1_interface
		params_impl = {
	.create = params_create_req,
	.set_tf_named = params_set_tf_named_req,
	.set_tf_power = params_set_tf_power_req,
	.set_primaries_named = params_set_primaries_named_req,
	.set_primaries = params_set_primaries_req,
	.set_luminances = params_set_luminances_req,
	.set_mastering_display_primaries = params_set_mastering_display_primaries_req,
	.set_mastering_luminance = params_set_mastering_luminance_req,
	.set_max_cll = params_set_max_cll_req,
	.set_max_fall = params_set_max_fall_req,
};

static void
manager_get_output_req(struct wl_client *client, struct wl_resource *resource,
	uint32_t id, struct wl_resource *output_resource)
{
	struct output *output = wl_resource_get_user_data(output_resource);
	struct wl_resource *color_resource = wl_resource_create(client,
		&wp_color_management_output_v1_interface,
		wl_resource_get_version(resource), id);
	assert(color_resource);
	// The output is disabled.
	if (!output) {
		wl_resource_set_implementation(color_resource, &output_impl, NULL, NULL);
		return;
	}
	wl_resource_set_implementation(color_resource, &output_impl, output,
		resource_unlink);
	list_append(&output->color_resources, wl_resource_get_link(color_resource));
}

static void
manager_get_surface_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id, struct wl_resource *surface_resource)
{
	struct surface *surface = wl_resource_get_user_data(surface_resource);
	if (surface->color_management) {
		wl_resource_post_error(resource, WP_COLOR_MANAGER_V1_ERROR_SURFACE_EXISTS,
			"the surface already has a color management surface");
		return;
	}
	surface->color_management = wl_resource_create(client,
		&wp_color_management_surface_v1_interface,
		wl_resource_get_version(resource), id);
	assert(surface->color_management);
	wl_resource_set_implementation(surface->color_management, &surface_impl,
		surface, surface_resource_destroy);
}

static void
manager_get_surface_feedback_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id, struct wl_resource *surface_resource)
{
	struct surface *surface = wl_resource_get_user_data(surface_resource);
	struct wl_resource *feedback = wl_resource_create(client,
		&wp_color_management_surface_feedback_v1_interface,
		wl_resource_get_version(resource), id);
	assert(feedback);
	wl_resource_set_implementation(feedback, &feedback_impl, surface,
		resource_unlink);
	list_append(&surface->color_feedbacks, wl_resource_get_link(feedback));
}

static void
manager_create_icc_creator_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id)
{
	wl_resource_post_error(resource, WP_COLOR_MANAGER_V1_ERROR_UNSUPPORTED_FEATURE,
		"ICC profiles are not supported");
}

static void
manager_create_parametric_creator_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id)
{
	struct params_creator *creator = calloc(1, sizeof(*creator));
	assert(creator);
	struct wl_resource *creator_resource = wl_resource_create(client,
		&wp_image_description_creator_params_v1_interface,
		wl_resource_get_version(resource), id);
	assert(creator_resource);
	wl_resource_set_implementation(creator_resource, &params_impl, creator,
		params_resource_destroy);
}

// R = G = B = 1.0 is shown at 80 cd/m², and the reference white is assumed
// at 203 cd/m², like on Windows.
static void
manager_create_windows_scrgb_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id)
{
	struct image_description params = {
		.primaries_named = WP_COLOR_MANAGER_V1_PRIMARIES_SRGB,
		.tf_named = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR,
		.max_luminance = 80.0,
		.reference_luminance = 203.0,
		.target_max_luminance = 10000.0,
	};
	color_primaries_from_named(params.primaries_named, &params.primaries);
	params.target_primaries = params.primaries;
	image_description_resource_create(client, wl_resource_get_version(resource), id,
		image_description_create(&params), 0, NULL);
}

static void
manager_create_windows_bt2100_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id)
{
	struct image_description params = {
		.primaries_named = WP_COLOR_MANAGER_V1_PRIMARIES_BT2020,
		.tf_named = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ,
	};
	color_primaries_from_named(params.primaries_named, &params.primaries);
	image_description_set_default_luminances(&params);
	params.target_primaries = params.primaries;
	params.target_min_luminance = params.min_luminance;
	params.target_max_luminance = params.max_luminance;
	image_description_resource_create(client, wl_resource_get_version(resource), id,
		image_description_create(&params), 0, NULL);
}

// No protocol that we support creates image description references.
static void
manager_get_image_description_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id, struct wl_resource *reference)
{
	assert(!"image description references are never created");
}

static const struct wp_color_manager_v1_interface manager_impl = {
	.destroy = resource_destroy_req,
	.get_output = manager_get_output_req,
	.get_surface = manager_get_surface_req,
	.get_surface_feedback = manager_get_surface_feedback_req,
	.create_icc_creator = manager_create_icc_creator_req,
	.create_parametric_creator = manager_create_parametric_creator_req,
	.create_windows_scrgb = manager_create_windows_scrgb_req,
	.get_image_description = manager_get_image_description_req,
	.create_windows_bt2100 = manager_create_windows_bt2100_req,
};

// The transfer functions that the shaders have, and all the named primaries.
// Those that are deprecated in the version are not advertised.
void
wayland_bind_color_manager(struct wl_client *client, void *data,
	uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&wp_color_manager_v1_interface, MIN(version, COLOR_MANAGER_VERSION), id);
	assert(resource);
	wl_resource_set_implementation(resource, &manager_impl, NULL, NULL);
	version = wl_resource_get_version(resource);
	wp_color_manager_v1_send_supported_intent(resource,
		WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
	static const uint32_t features[] = {
		WP_COLOR_MANAGER_V1_FEATURE_PARAMETRIC,
		WP_COLOR_MANAGER_V1_FEATURE_SET_PRIMARIES,
		WP_COLOR_MANAGER_V1_FEATURE_SET_TF_POWER,
		WP_COLOR_MANAGER_V1_FEATURE_SET_LUMINANCES,
		WP_COLOR_MANAGER_V1_FEATURE_SET_MASTERING_DISPLAY_PRIMARIES,
		WP_COLOR_MANAGER_V1_FEATURE_WINDOWS_SCRGB,
	};
	for (size_t i = 0; i < ARRAY_LEN(features); i++) {
		wp_color_manager_v1_send_supported_feature(resource, features[i]);
	}
	if (version >= 3) {
		wp_color_manager_v1_send_supported_feature(resource,
			WP_COLOR_MANAGER_V1_FEATURE_WINDOWS_BT2100);
	}
	static const uint32_t tfs[] = {
		WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22,
		WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA28,
		WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_BT1886,
		WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR,
		WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ,
	};
	for (size_t i = 0; i < ARRAY_LEN(tfs); i++) {
		wp_color_manager_v1_send_supported_tf_named(resource, tfs[i]);
	}
	if (version >= 2) {
		wp_color_manager_v1_send_supported_tf_named(resource,
			WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_COMPOUND_POWER_2_4);
	}
	struct color_primaries primaries;
	for (uint32_t named = 1;
			color_primaries_from_named(named, &primaries); named++) {
		wp_color_manager_v1_send_supported_primaries_named(resource, named);
	}
	wp_color_manager_v1_send_done(resource);
}
