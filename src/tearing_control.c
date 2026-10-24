#include <assert.h>

#include "core.h"
#include "surface.h"
#include "tearing-control-v1-protocol.h"

// Lets clients like games show their surfaces as soon as they commit them,
// tearing. The global is only advertised if KMS can.

// The surface goes back to vsync on the next commit.
static void
tearing_control_resource_destroy(struct wl_resource *resource)
{
	struct surface *surface = wl_resource_get_user_data(resource);
	if (!surface) {
		return;
	}
	surface->tearing_control = NULL;
	surface->pending.tearing = false;
}

static void
tearing_control_set_presentation_hint_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t hint)
{
	struct surface *surface = wl_resource_get_user_data(resource);
	if (surface) {
		surface->pending.tearing =
			hint == WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC;
	}
}

static const struct wp_tearing_control_v1_interface tearing_control_impl = {
	.set_presentation_hint = tearing_control_set_presentation_hint_req,
	.destroy = resource_destroy_req,
};

// The object stays alive, but becomes inert.
void
tearing_control_surface_destroy(struct surface *surface)
{
	if (surface->tearing_control) {
		wl_resource_set_user_data(surface->tearing_control, NULL);
	}
}

static void
tearing_control_manager_get_tearing_control_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id, struct wl_resource *surface_resource)
{
	struct surface *surface = wl_resource_get_user_data(surface_resource);
	if (surface->tearing_control) {
		wl_resource_post_error(resource,
			WP_TEARING_CONTROL_MANAGER_V1_ERROR_TEARING_CONTROL_EXISTS,
			"the surface already has a tearing control object");
		return;
	}
	surface->tearing_control = wl_resource_create(client,
		&wp_tearing_control_v1_interface, wl_resource_get_version(resource), id);
	assert(surface->tearing_control);
	wl_resource_set_implementation(surface->tearing_control,
		&tearing_control_impl, surface, tearing_control_resource_destroy);
}

static const struct wp_tearing_control_manager_v1_interface
		tearing_control_manager_impl = {
	.destroy = resource_destroy_req,
	.get_tearing_control = tearing_control_manager_get_tearing_control_req,
};

void
wayland_bind_tearing_control_manager(struct wl_client *client, void *data,
	uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&wp_tearing_control_manager_v1_interface, MIN(version, 1), id);
	assert(resource);
	wl_resource_set_implementation(resource, &tearing_control_manager_impl,
		NULL, NULL);
}
