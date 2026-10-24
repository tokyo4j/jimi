#include <assert.h>
#include <math.h>

#include "core.h"
#include "surface.h"
#include "viewporter-protocol.h"

// Crops and scales the buffer of a surface. Fractional-scale-v1 clients
// set the destination to the size of the surface in logical pixels.

static void
viewport_set_state(struct surface *surface, struct fbox source, int width,
	int height)
{
	surface->pending.viewport_source = source;
	surface->pending.viewport_width = width;
	surface->pending.viewport_height = height;
}

// The surface goes back to the size of its buffer on the next commit.
static void
viewport_resource_destroy(struct wl_resource *resource)
{
	struct surface *surface = wl_resource_get_user_data(resource);
	if (!surface) {
		return;
	}
	surface->viewport = NULL;
	viewport_set_state(surface, (struct fbox){.width = -1.0, .height = -1.0},
		-1, -1);
}

static struct surface *
viewport_surface(struct wl_resource *resource)
{
	struct surface *surface = wl_resource_get_user_data(resource);
	if (!surface) {
		wl_resource_post_error(resource, WP_VIEWPORT_ERROR_NO_SURFACE,
			"the wl_surface was destroyed");
	}
	return surface;
}

static void
viewport_set_source_req(struct wl_client *client,
	struct wl_resource *resource, wl_fixed_t x, wl_fixed_t y,
	wl_fixed_t width, wl_fixed_t height)
{
	struct surface *surface = viewport_surface(resource);
	if (!surface) {
		return;
	}
	struct fbox source = {
		.x = wl_fixed_to_double(x),
		.y = wl_fixed_to_double(y),
		.width = wl_fixed_to_double(width),
		.height = wl_fixed_to_double(height),
	};
	bool unset = source.x == -1.0 && source.y == -1.0
		&& source.width == -1.0 && source.height == -1.0;
	if (!unset && (source.x < 0.0 || source.y < 0.0
			|| source.width <= 0.0 || source.height <= 0.0)) {
		wl_resource_post_error(resource, WP_VIEWPORT_ERROR_BAD_VALUE,
			"invalid source rectangle");
		return;
	}
	viewport_set_state(surface, source, surface->pending.viewport_width,
		surface->pending.viewport_height);
}

static void
viewport_set_destination_req(struct wl_client *client,
	struct wl_resource *resource, int32_t width, int32_t height)
{
	struct surface *surface = viewport_surface(resource);
	if (!surface) {
		return;
	}
	bool unset = width == -1 && height == -1;
	if (!unset && (width <= 0 || height <= 0)) {
		wl_resource_post_error(resource, WP_VIEWPORT_ERROR_BAD_VALUE,
			"invalid destination size");
		return;
	}
	viewport_set_state(surface, surface->pending.viewport_source, width, height);
}

static const struct wp_viewport_interface viewport_impl = {
	.destroy = resource_destroy_req,
	.set_source = viewport_set_source_req,
	.set_destination = viewport_set_destination_req,
};

// Checks the viewport that the commit applies against the buffer, whose size
// is in surface-local coordinates before the viewport.
bool
viewport_validate(struct surface *surface, int width, int height)
{
	struct surface_state *state = &surface->pending;
	struct fbox source = state->viewport_source;
	if (source.width < 0.0) {
		return true;
	}
	// A viewport is alive while it sets a source.
	assert(surface->viewport);
	if (state->viewport_width < 0 && (source.width != floor(source.width)
			|| source.height != floor(source.height))) {
		wl_resource_post_error(surface->viewport, WP_VIEWPORT_ERROR_BAD_SIZE,
			"source size must be integral without a destination size");
		return false;
	}
	// TODO: Firefox rounds the offset and the size of the source to wl_fixed
	// separately, so its far edge may overshoot the buffer by one unit.
	// Restore the check once it is fixed:
	// https://bugzilla.mozilla.org/show_bug.cgi?id=2062135
	//
	// if (source.x + source.width > width
	// 		|| source.y + source.height > height) {
	// 	wl_resource_post_error(surface->viewport,
	// 		WP_VIEWPORT_ERROR_OUT_OF_BUFFER,
	// 		"source rectangle extends outside of the buffer");
	// 	return false;
	// }
	return true;
}

// The viewport stays alive, but becomes inert.
void
viewport_surface_destroy(struct surface *surface)
{
	if (surface->viewport) {
		wl_resource_set_user_data(surface->viewport, NULL);
	}
}

static void
viewporter_get_viewport_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id, struct wl_resource *surface_resource)
{
	struct surface *surface = wl_resource_get_user_data(surface_resource);
	if (surface->viewport) {
		wl_resource_post_error(resource, WP_VIEWPORTER_ERROR_VIEWPORT_EXISTS,
			"the surface already has a viewport");
		return;
	}
	surface->viewport = wl_resource_create(client, &wp_viewport_interface,
		wl_resource_get_version(resource), id);
	assert(surface->viewport);
	wl_resource_set_implementation(surface->viewport, &viewport_impl,
		surface, viewport_resource_destroy);
}

static const struct wp_viewporter_interface viewporter_impl = {
	.destroy = resource_destroy_req,
	.get_viewport = viewporter_get_viewport_req,
};

void
wayland_bind_viewporter(struct wl_client *client, void *data,
	uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&wp_viewporter_interface, MIN(version, 1), id);
	assert(resource);
	wl_resource_set_implementation(resource, &viewporter_impl, NULL, NULL);
}
