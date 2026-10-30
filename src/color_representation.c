#include <assert.h>

#include "color-representation-v1-protocol.h"
#include "core.h"
#include "surface.h"

// Lets clients describe how the pixels of their buffers encode colors: how
// YCbCr converts into RGB, and how the colors include alpha. The renderer
// decodes them, and the image description of the surface then tells what the
// RGB colors are.

// Each of them is supported with both ranges, and the identity only with the
// full range, as RGB has it.
static const uint32_t ycbcr_coefficients[] = {
	WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_BT709,
	WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_FCC,
	WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_BT601,
	WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_SMPTE240,
	WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_BT2020,
};

static bool
coefficients_and_range_supported(uint32_t coefficients, uint32_t range)
{
	if (coefficients == WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_IDENTITY) {
		return range == WP_COLOR_REPRESENTATION_SURFACE_V1_RANGE_FULL;
	}
	if (range != WP_COLOR_REPRESENTATION_SURFACE_V1_RANGE_FULL
			&& range != WP_COLOR_REPRESENTATION_SURFACE_V1_RANGE_LIMITED) {
		return false;
	}
	for (size_t i = 0; i < ARRAY_LEN(ycbcr_coefficients); i++) {
		if (ycbcr_coefficients[i] == coefficients) {
			return true;
		}
	}
	return false;
}

// The surface goes back to the default representation on the next commit.
static void
surface_resource_destroy(struct wl_resource *resource)
{
	struct surface *surface = wl_resource_get_user_data(resource);
	if (!surface) {
		return;
	}
	surface->color_representation = NULL;
	surface->pending.representation = (struct color_representation){0};
}

// The pending representation of the surface, or NULL once it is gone.
static struct color_representation *
pending_representation(struct wl_resource *resource)
{
	struct surface *surface = wl_resource_get_user_data(resource);
	if (!surface) {
		wl_resource_post_error(resource,
			WP_COLOR_REPRESENTATION_SURFACE_V1_ERROR_INERT,
			"the surface is gone");
		return NULL;
	}
	return &surface->pending.representation;
}

// All the alpha modes are supported.
static void
surface_set_alpha_mode_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t alpha_mode)
{
	struct color_representation *representation =
		pending_representation(resource);
	if (!representation) {
		return;
	}
	if (!wp_color_representation_surface_v1_alpha_mode_is_valid(alpha_mode,
			wl_resource_get_version(resource))) {
		wl_resource_post_error(resource,
			WP_COLOR_REPRESENTATION_SURFACE_V1_ERROR_ALPHA_MODE,
			"unsupported alpha mode");
		return;
	}
	representation->alpha_mode = alpha_mode;
}

static void
surface_set_coefficients_and_range_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t coefficients, uint32_t range)
{
	struct color_representation *representation =
		pending_representation(resource);
	if (!representation) {
		return;
	}
	if (!coefficients_and_range_supported(coefficients, range)) {
		wl_resource_post_error(resource,
			WP_COLOR_REPRESENTATION_SURFACE_V1_ERROR_COEFFICIENTS,
			"unsupported coefficients and range");
		return;
	}
	representation->coefficients = coefficients;
	representation->range = range;
}

// All the locations are supported.
static void
surface_set_chroma_location_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t chroma_location)
{
	struct color_representation *representation =
		pending_representation(resource);
	if (!representation) {
		return;
	}
	if (!wp_color_representation_surface_v1_chroma_location_is_valid(
			chroma_location, wl_resource_get_version(resource))) {
		wl_resource_post_error(resource,
			WP_COLOR_REPRESENTATION_SURFACE_V1_ERROR_CHROMA_LOCATION,
			"invalid chroma location");
		return;
	}
	representation->chroma_location = chroma_location;
}

static const struct wp_color_representation_surface_v1_interface surface_impl = {
	.destroy = resource_destroy_req,
	.set_alpha_mode = surface_set_alpha_mode_req,
	.set_coefficients_and_range = surface_set_coefficients_and_range_req,
	.set_chroma_location = surface_set_chroma_location_req,
};

// The identity is for RGB buffers, and the other coefficients and the chroma
// locations are for YCbCr, which are all 4:2:0. Without a buffer, anything
// goes.
bool
color_representation_validate(struct surface *surface)
{
	const struct surface_state *pending = &surface->pending;
	const struct color_representation *representation = &pending->representation;
	if (pending->buffer_width == 0) {
		return true;
	}
	bool rgb_only = representation->coefficients
		== WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_IDENTITY;
	bool ycbcr_only = (representation->coefficients && !rgb_only)
		|| representation->chroma_location;
	if (pending->buffer_ycbcr ? rgb_only : ycbcr_only) {
		wl_resource_post_error(surface->color_representation,
			WP_COLOR_REPRESENTATION_SURFACE_V1_ERROR_PIXEL_FORMAT,
			"the representation does not match the pixel format");
		return false;
	}
	return true;
}

// The object stays alive, but becomes inert.
void
color_representation_surface_destroy(struct surface *surface)
{
	if (surface->color_representation) {
		wl_resource_set_user_data(surface->color_representation, NULL);
	}
}

static void
manager_get_surface_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id, struct wl_resource *surface_resource)
{
	struct surface *surface = wl_resource_get_user_data(surface_resource);
	if (surface->color_representation) {
		wl_resource_post_error(resource,
			WP_COLOR_REPRESENTATION_MANAGER_V1_ERROR_SURFACE_EXISTS,
			"the surface already has a color representation surface");
		return;
	}
	surface->color_representation = wl_resource_create(client,
		&wp_color_representation_surface_v1_interface,
		wl_resource_get_version(resource), id);
	assert(surface->color_representation);
	wl_resource_set_implementation(surface->color_representation,
		&surface_impl, surface, surface_resource_destroy);
}

static const struct wp_color_representation_manager_v1_interface manager_impl = {
	.destroy = resource_destroy_req,
	.get_surface = manager_get_surface_req,
};

void
wayland_bind_color_representation_manager(struct wl_client *client,
	void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&wp_color_representation_manager_v1_interface, version, id);
	assert(resource);
	wl_resource_set_implementation(resource, &manager_impl, NULL, NULL);
	static const uint32_t alpha_modes[] = {
		WP_COLOR_REPRESENTATION_SURFACE_V1_ALPHA_MODE_PREMULTIPLIED_ELECTRICAL,
		WP_COLOR_REPRESENTATION_SURFACE_V1_ALPHA_MODE_PREMULTIPLIED_OPTICAL,
		WP_COLOR_REPRESENTATION_SURFACE_V1_ALPHA_MODE_STRAIGHT,
	};
	for (size_t i = 0; i < ARRAY_LEN(alpha_modes); i++) {
		wp_color_representation_manager_v1_send_supported_alpha_mode(
			resource, alpha_modes[i]);
	}
	wp_color_representation_manager_v1_send_supported_coefficients_and_ranges(
		resource, WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_IDENTITY,
		WP_COLOR_REPRESENTATION_SURFACE_V1_RANGE_FULL);
	for (size_t i = 0; i < ARRAY_LEN(ycbcr_coefficients); i++) {
		wp_color_representation_manager_v1_send_supported_coefficients_and_ranges(
			resource, ycbcr_coefficients[i],
			WP_COLOR_REPRESENTATION_SURFACE_V1_RANGE_FULL);
		wp_color_representation_manager_v1_send_supported_coefficients_and_ranges(
			resource, ycbcr_coefficients[i],
			WP_COLOR_REPRESENTATION_SURFACE_V1_RANGE_LIMITED);
	}
	wp_color_representation_manager_v1_send_done(resource);
}
