#include <assert.h>
#include <math.h>

#include "core.h"
#include "surface.h"
#include "fractional-scale-v1-protocol.h"

// Tells clients the scale to draw their surfaces at, which they do with
// buffers of the scaled size and a wp_viewport.

static void
fractional_scale_resource_destroy(struct wl_resource *resource)
{
	struct surface *surface = wl_resource_get_user_data(resource);
	if (surface) {
		surface->fractional_scale = NULL;
	}
}

static const struct wp_fractional_scale_v1_interface fractional_scale_impl = {
	.destroy = resource_destroy_req,
};

void
fractional_scale_send(struct surface *surface)
{
	// The scale is in 1/120ths.
	if (surface->fractional_scale && surface->preferred_scale > 0.0) {
		wp_fractional_scale_v1_send_preferred_scale(surface->fractional_scale,
			(uint32_t)round(surface->preferred_scale * 120.0));
	}
}

// The object stays alive, but becomes inert.
void
fractional_scale_surface_destroy(struct surface *surface)
{
	if (surface->fractional_scale) {
		wl_resource_set_user_data(surface->fractional_scale, NULL);
	}
}

static void
fractional_scale_manager_get_fractional_scale_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id, struct wl_resource *surface_resource)
{
	struct surface *surface = wl_resource_get_user_data(surface_resource);
	if (surface->fractional_scale) {
		wl_resource_post_error(resource,
			WP_FRACTIONAL_SCALE_MANAGER_V1_ERROR_FRACTIONAL_SCALE_EXISTS,
			"the surface already has a fractional scale object");
		return;
	}
	surface->fractional_scale = wl_resource_create(client,
		&wp_fractional_scale_v1_interface, wl_resource_get_version(resource), id);
	assert(surface->fractional_scale);
	wl_resource_set_implementation(surface->fractional_scale,
		&fractional_scale_impl, surface, fractional_scale_resource_destroy);
	fractional_scale_send(surface);
}

static const struct wp_fractional_scale_manager_v1_interface
		fractional_scale_manager_impl = {
	.destroy = resource_destroy_req,
	.get_fractional_scale = fractional_scale_manager_get_fractional_scale_req,
};

void
wayland_bind_fractional_scale_manager(struct wl_client *client, void *data,
	uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&wp_fractional_scale_manager_v1_interface, MIN(version, 1), id);
	assert(resource);
	wl_resource_set_implementation(resource, &fractional_scale_manager_impl,
		NULL, NULL);
}
