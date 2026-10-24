#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "core.h"
#include "surface.h"
#include "xdg-toplevel-icon-v1-protocol.h"

// Lets clients set the icons of their toplevels by name or by pixels. The
// icon applies on the next commit of the toplevel.

struct toplevel_icon *
toplevel_icon_create(void)
{
	struct toplevel_icon *icon = calloc(1, sizeof(*icon));
	assert(icon);
	icon->refs = 1;
	return icon;
}

// NULL is passed through.
struct toplevel_icon *
toplevel_icon_ref(struct toplevel_icon *icon)
{
	if (icon) {
		icon->refs++;
	}
	return icon;
}

// NULL is ignored.
void
toplevel_icon_unref(struct toplevel_icon *icon)
{
	if (!icon) {
		return;
	}
	assert(icon->refs > 0);
	if (--icon->refs > 0) {
		return;
	}
	free(icon->name);
	free(icon->pixels);
	free(icon);
}

static bool
check_mutable(struct wl_resource *resource)
{
	struct toplevel_icon *icon = wl_resource_get_user_data(resource);
	if (icon->immutable) {
		wl_resource_post_error(resource, XDG_TOPLEVEL_ICON_V1_ERROR_IMMUTABLE,
			"the icon has been set on a toplevel");
		return false;
	}
	return true;
}

static void
icon_resource_destroy(struct wl_resource *resource)
{
	toplevel_icon_unref(wl_resource_get_user_data(resource));
}

static void
icon_set_name_req(struct wl_client *client, struct wl_resource *resource,
	const char *name)
{
	if (!check_mutable(resource)) {
		return;
	}
	struct toplevel_icon *icon = wl_resource_get_user_data(resource);
	char *copy = strdup(name);
	assert(copy);
	free(icon->name);
	icon->name = copy;
}

// Only the largest buffer is kept, which is scaled down to be drawn. Its
// scale does not matter, as icons are drawn at fractional scales.
static void
icon_add_buffer_req(struct wl_client *client, struct wl_resource *resource,
	struct wl_resource *buffer, int32_t scale)
{
	if (!check_mutable(resource)) {
		return;
	}
	struct wl_shm_buffer *shm = wl_shm_buffer_get(buffer);
	if (!shm || wl_shm_buffer_get_width(shm) != wl_shm_buffer_get_height(shm)) {
		wl_resource_post_error(resource, XDG_TOPLEVEL_ICON_V1_ERROR_INVALID_BUFFER,
			"the icon buffer must be a square wl_shm buffer");
		return;
	}
	struct toplevel_icon *icon = wl_resource_get_user_data(resource);
	int size = wl_shm_buffer_get_width(shm);
	if (size < icon->width) {
		return;
	}
	uint32_t *pixels = malloc((size_t)size * (size_t)size * 4);
	assert(pixels);
	// Only the formats advertised by wl_shm are possible.
	bool opaque = wl_shm_buffer_get_format(shm) == WL_SHM_FORMAT_XRGB8888;
	int32_t stride = wl_shm_buffer_get_stride(shm);
	wl_shm_buffer_begin_access(shm);
	const uint8_t *data = wl_shm_buffer_get_data(shm);
	for (int y = 0; y < size; y++) {
		const uint32_t *row = (const uint32_t *)(data + y * stride);
		for (int x = 0; x < size; x++) {
			pixels[y * size + x] = opaque ? row[x] | 0xff000000 : row[x];
		}
	}
	wl_shm_buffer_end_access(shm);
	free(icon->pixels);
	icon->pixels = pixels;
	icon->width = size;
	icon->height = size;
}

static const struct xdg_toplevel_icon_v1_interface icon_impl = {
	.destroy = resource_destroy_req,
	.set_name = icon_set_name_req,
	.add_buffer = icon_add_buffer_req,
};

static void
manager_create_icon_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id)
{
	struct toplevel_icon *icon = toplevel_icon_create();
	struct wl_resource *icon_resource = wl_resource_create(client,
		&xdg_toplevel_icon_v1_interface, wl_resource_get_version(resource), id);
	assert(icon_resource);
	wl_resource_set_implementation(icon_resource, &icon_impl, icon,
		icon_resource_destroy);
}

// An icon without a name or buffers unsets the icon like NULL.
static void
manager_set_icon_req(struct wl_client *client, struct wl_resource *resource,
	struct wl_resource *toplevel_resource, struct wl_resource *icon_resource)
{
	struct xdg_toplevel *toplevel = wl_resource_get_user_data(toplevel_resource);
	struct toplevel_icon *icon = icon_resource
		? wl_resource_get_user_data(icon_resource) : NULL;
	if (icon) {
		icon->immutable = true;
		if (!icon->name && !icon->pixels) {
			icon = NULL;
		}
	}
	if (!toplevel) {
		return;
	}
	struct surface_state *pending = &toplevel->base->surface->pending;
	toplevel_icon_unref(pending->toplevel_icon);
	pending->toplevel_icon = toplevel_icon_ref(icon);
}

static const struct xdg_toplevel_icon_manager_v1_interface manager_impl = {
	.destroy = resource_destroy_req,
	.create_icon = manager_create_icon_req,
	.set_icon = manager_set_icon_req,
};

// Clients with scalable icons draw them at the size in the titlebar.
void
wayland_bind_xdg_toplevel_icon_manager(struct wl_client *client, void *data,
	uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&xdg_toplevel_icon_manager_v1_interface, MIN(version, 1), id);
	assert(resource);
	wl_resource_set_implementation(resource, &manager_impl, NULL, NULL);
	xdg_toplevel_icon_manager_v1_send_icon_size(resource,
		config.decorations.app_icon_size);
	xdg_toplevel_icon_manager_v1_send_done(resource);
}
