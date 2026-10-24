#include <assert.h>
#include <stdlib.h>

#include "config.h"
#include "core.h"
#include "surface.h"
#include "server-decoration-protocol.h"
#include "xdg-decoration-unstable-v1-protocol.h"

// Requests a decoration mode for the window of a toplevel and reports the
// mode back in the toplevel's configures.
struct xdg_toplevel_decoration {
	struct xdg_toplevel *toplevel;
	struct wl_resource *resource;
	bool configured;
	bool requested; // A request awaits its configure.
	bool ssd; // The configured mode.
};

// The toplevel goes back to client-side decorations.
static void
xdg_toplevel_decoration_resource_destroy(struct wl_resource *resource)
{
	struct xdg_toplevel_decoration *decoration = wl_resource_get_user_data(resource);
	assert(decoration);
	struct xdg_toplevel *toplevel = decoration->toplevel;
	if (toplevel) {
		assert(toplevel->decoration == decoration);
		toplevel->decoration = NULL;
		window_set_ssd(&toplevel->window, false);
	}
	free(decoration);
}

static void
xdg_toplevel_decoration_request(struct xdg_toplevel_decoration *decoration, bool ssd)
{
	struct window *window = &decoration->toplevel->window;
	window_set_ssd(window, ssd);
	decoration->requested = true;
	window_schedule_configure(window);
}

static void
xdg_toplevel_decoration_set_mode_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t mode)
{
	if (mode != ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE
			&& mode != ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE) {
		wl_resource_post_error(resource,
			ZXDG_TOPLEVEL_DECORATION_V1_ERROR_INVALID_MODE,
			"invalid decoration mode");
		return;
	}
	xdg_toplevel_decoration_request(wl_resource_get_user_data(resource),
		mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

static void
xdg_toplevel_decoration_unset_mode_req(struct wl_client *client,
	struct wl_resource *resource)
{
	xdg_toplevel_decoration_request(wl_resource_get_user_data(resource),
		config.windows.default_ssd);
}

static const struct zxdg_toplevel_decoration_v1_interface
xdg_toplevel_decoration_impl = {
	.destroy = resource_destroy_req,
	.set_mode = xdg_toplevel_decoration_set_mode_req,
	.unset_mode = xdg_toplevel_decoration_unset_mode_req,
};

static void
xdg_decoration_manager_get_toplevel_decoration_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id, struct wl_resource *toplevel_resource)
{
	struct xdg_toplevel *toplevel = wl_resource_get_user_data(toplevel_resource);
	if (!toplevel) {
		wl_resource_post_error(resource, ZXDG_TOPLEVEL_DECORATION_V1_ERROR_ORPHANED,
			"xdg_toplevel has been destroyed");
		return;
	}
	if (toplevel->decoration) {
		wl_resource_post_error(resource,
			ZXDG_TOPLEVEL_DECORATION_V1_ERROR_ALREADY_CONSTRUCTED,
			"xdg_toplevel already has a decoration object");
		return;
	}
	struct surface *surface = toplevel->base->surface;
	if (wl_resource_get_version(resource) == 1 && surface_has_buffer(surface)) {
		wl_resource_post_error(resource,
			ZXDG_TOPLEVEL_DECORATION_V1_ERROR_UNCONFIGURED_BUFFER,
			"xdg_toplevel already has a buffer");
		return;
	}

	struct xdg_toplevel_decoration *decoration = calloc(1, sizeof(*decoration));
	assert(decoration);
	struct wl_resource *decoration_resource = wl_resource_create(client,
		&zxdg_toplevel_decoration_v1_interface,
		wl_resource_get_version(resource), id);
	assert(decoration_resource);
	decoration->toplevel = toplevel;
	decoration->resource = decoration_resource;
	toplevel->decoration = decoration;
	wl_resource_set_implementation(decoration_resource,
		&xdg_toplevel_decoration_impl, decoration,
		xdg_toplevel_decoration_resource_destroy);
	window_set_ssd(&toplevel->window, config.windows.default_ssd);
	window_schedule_configure(&toplevel->window);
}

static const struct zxdg_decoration_manager_v1_interface
xdg_decoration_manager_impl = {
	.destroy = resource_destroy_req,
	.get_toplevel_decoration = xdg_decoration_manager_get_toplevel_decoration_req,
};

void
wayland_bind_xdg_decoration_manager(struct wl_client *client, void *data,
	uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&zxdg_decoration_manager_v1_interface, MIN(version, 2), id);
	assert(resource);
	wl_resource_set_implementation(resource, &xdg_decoration_manager_impl,
		data, NULL);
}

bool
xdg_toplevel_decoration_outdated(struct xdg_toplevel *toplevel)
{
	struct xdg_toplevel_decoration *decoration = toplevel->decoration;
	return decoration && (!decoration->configured || decoration->requested
		|| decoration->ssd != toplevel->window.ssd);
}

// The configured mode must precede the matching xdg_surface.configure.
void
xdg_toplevel_decoration_configure(struct xdg_toplevel *toplevel)
{
	struct xdg_toplevel_decoration *decoration = toplevel->decoration;
	if (!decoration) {
		return;
	}
	decoration->ssd = toplevel->window.ssd;
	decoration->configured = true;
	decoration->requested = false;
	zxdg_toplevel_decoration_v1_send_configure(decoration->resource,
		decoration->ssd ? ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE
			: ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
}

void
xdg_toplevel_decoration_toplevel_destroy(struct xdg_toplevel *toplevel)
{
	struct xdg_toplevel_decoration *decoration = toplevel->decoration;
	if (!decoration) {
		return;
	}
	toplevel->decoration = NULL;
	decoration->toplevel = NULL;
	wl_resource_post_error(decoration->resource,
		ZXDG_TOPLEVEL_DECORATION_V1_ERROR_ORPHANED,
		"xdg_toplevel destroyed before its decoration object");
	wl_resource_destroy(decoration->resource);
}

bool
xdg_toplevel_decoration_allows_buffer(struct surface *surface)
{
	if (surface->role != SURFACE_ROLE_XDG_TOPLEVEL
			|| !surface->xdg->toplevel->decoration) {
		return true;
	}
	struct xdg_toplevel_decoration *decoration = surface->xdg->toplevel->decoration;
	if (wl_resource_get_version(decoration->resource) >= 2
			|| decoration->configured) {
		return true;
	}
	wl_resource_post_error(decoration->resource,
		ZXDG_TOPLEVEL_DECORATION_V1_ERROR_UNCONFIGURED_BUFFER,
		"buffer attached before initial decoration configure");
	return false;
}

// Clients may request a mode before their surface becomes a toplevel, so the
// requested mode is kept until the window is created.
struct server_decoration {
	struct wl_list link; // core.server_decorations
	struct wl_resource *resource;
	struct surface *surface;
	bool ssd; // The requested mode.
};

static struct server_decoration *
server_decoration_from_surface(struct surface *surface)
{
	struct server_decoration *decoration;
	wl_list_for_each(decoration, &core.server_decorations, link) {
		if (decoration->surface == surface) {
			return decoration;
		}
	}
	return NULL;
}

static void
server_decoration_resource_destroy(struct wl_resource *resource)
{
	struct server_decoration *decoration = wl_resource_get_user_data(resource);
	if (!decoration) {
		return;
	}
	wl_list_remove(&decoration->link);
	free(decoration);
}

static void
server_decoration_send_mode(struct server_decoration *decoration)
{
	org_kde_kwin_server_decoration_send_mode(decoration->resource,
		decoration->ssd ? ORG_KDE_KWIN_SERVER_DECORATION_MODE_SERVER
			: ORG_KDE_KWIN_SERVER_DECORATION_MODE_CLIENT);
}

// The mode is sent back as the acknowledgement.
static void
server_decoration_request_mode_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t mode)
{
	struct server_decoration *decoration = wl_resource_get_user_data(resource);
	if (!decoration || mode > ORG_KDE_KWIN_SERVER_DECORATION_MODE_SERVER) {
		return;
	}
	struct surface *surface = decoration->surface;
	decoration->ssd = mode == ORG_KDE_KWIN_SERVER_DECORATION_MODE_SERVER;
	if (surface->role == SURFACE_ROLE_XDG_TOPLEVEL) {
		window_set_ssd(&surface->xdg->toplevel->window, decoration->ssd);
	}
	server_decoration_send_mode(decoration);
}

static const struct org_kde_kwin_server_decoration_interface
server_decoration_impl = {
	.release = resource_destroy_req,
	.request_mode = server_decoration_request_mode_req,
};

static void
server_decoration_manager_create_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id, struct wl_resource *surface_resource)
{
	struct surface *surface = wl_resource_get_user_data(surface_resource);
	assert(surface);
	if (server_decoration_from_surface(surface)) {
		wl_resource_post_error(resource, 0,
			"wl_surface already has a decoration object");
		return;
	}

	struct server_decoration *decoration = calloc(1, sizeof(*decoration));
	assert(decoration);
	decoration->resource = wl_resource_create(client,
		&org_kde_kwin_server_decoration_interface,
		wl_resource_get_version(resource), id);
	assert(decoration->resource);
	decoration->surface = surface;
	decoration->ssd = config.windows.default_ssd;
	if (surface->role == SURFACE_ROLE_XDG_TOPLEVEL) {
		window_set_ssd(&surface->xdg->toplevel->window, decoration->ssd);
	}
	list_append(&core.server_decorations, &decoration->link);
	wl_resource_set_implementation(decoration->resource,
		&server_decoration_impl, decoration, server_decoration_resource_destroy);
	server_decoration_send_mode(decoration);
}

static const struct org_kde_kwin_server_decoration_manager_interface
server_decoration_manager_impl = {
	.create = server_decoration_manager_create_req,
};

void
wayland_bind_server_decoration_manager(struct wl_client *client, void *data,
	uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&org_kde_kwin_server_decoration_manager_interface, MIN(version, 1), id);
	assert(resource);
	wl_resource_set_implementation(resource, &server_decoration_manager_impl,
		data, NULL);
	org_kde_kwin_server_decoration_manager_send_default_mode(resource,
		config.windows.default_ssd
			? ORG_KDE_KWIN_SERVER_DECORATION_MANAGER_MODE_SERVER
			: ORG_KDE_KWIN_SERVER_DECORATION_MANAGER_MODE_CLIENT);
}

// Clients decorate themselves unless they negotiated otherwise.
bool
server_decoration_surface_ssd(struct surface *surface)
{
	struct server_decoration *decoration = server_decoration_from_surface(surface);
	return decoration && decoration->ssd;
}

void
server_decoration_surface_destroy(struct surface *surface)
{
	struct server_decoration *decoration = server_decoration_from_surface(surface);
	if (!decoration) {
		return;
	}
	wl_list_remove(&decoration->link);
	wl_resource_set_user_data(decoration->resource, NULL);
	free(decoration);
}
