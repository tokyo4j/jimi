#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "core.h"
#include "surface.h"

// The placement of the subsurface, or of the parent itself if NULL. NULL if
// the subsurface is not in the placements.
static struct subsurface_placement *
placement_find(struct wl_array *placements, struct subsurface *subsurface)
{
	struct subsurface_placement *placement;
	wl_array_for_each(placement, placements) {
		if (placement->subsurface == subsurface) {
			return placement;
		}
	}
	return NULL;
}

static void
placement_remove(struct wl_array *placements, struct subsurface *subsurface)
{
	struct subsurface_placement *placement = placement_find(placements, subsurface);
	if (!placement) {
		return;
	}
	char *end = (char *)placements->data + placements->size;
	memmove(placement, placement + 1, end - (char *)(placement + 1));
	placements->size -= sizeof(*placement);
}

static void
placement_insert(struct wl_array *placements, size_t index,
	struct subsurface_placement placement)
{
	assert(wl_array_add(placements, sizeof(placement)));
	struct subsurface_placement *at =
		(struct subsurface_placement *)placements->data + index;
	char *end = (char *)placements->data + placements->size;
	memmove(at + 1, at, end - (char *)(at + 1));
	*at = placement;
}

// A synchronized subsurface caches its commits until the state of its parent
// is applied, and so do the subsurfaces below it.
bool
surface_synchronized(struct surface *surface)
{
	for (; surface->role == SURFACE_ROLE_SUBSURFACE; surface = surface->parent) {
		if (surface->subsurface->synchronized) {
			return true;
		}
	}
	return false;
}

// Applies the cached state once the subsurface is no longer synchronized.
static void
subsurface_flush_cache(struct surface *surface)
{
	struct surface_state *cached = surface->cached;
	if (cached && !surface_synchronized(surface)) {
		surface->cached = NULL;
		transaction_commit(surface, cached);
	}
}

// Subsurfaces are new on top of their siblings, and synchronized.
static struct subsurface *
subsurface_create(struct surface *surface, struct surface *parent,
	struct wl_resource *resource)
{
	assert(surface->role == SURFACE_ROLE_NONE);

	struct subsurface *subsurface = calloc(1, sizeof(*subsurface));
	assert(subsurface);
	scene_node_reparent(&surface->scene.tree.node, &parent->scene.tree);
	list_append(&parent->subsurfaces, &subsurface->parent_link);
	struct subsurface_placement *placement = wl_array_add(
		&parent->pending.subsurfaces, sizeof(*placement));
	assert(placement);
	*placement = (struct subsurface_placement){.subsurface = subsurface};

	subsurface->resource = resource;
	subsurface->surface = surface;
	subsurface->synchronized = true;
	surface->parent = parent;
	surface->role = SURFACE_ROLE_SUBSURFACE;
	surface->subsurface = subsurface;
	return subsurface;
}

// The subsurface leaves the states of its parent at once, and the surface
// applies its cached state on its own.
void
subsurface_destroy(struct subsurface *subsurface)
{
	struct surface *surface = subsurface->surface;
	struct surface *parent = surface->parent;
	surface_set_mapped(surface, false);
	wl_resource_set_user_data(subsurface->resource, NULL);
	placement_remove(&parent->pending.subsurfaces, subsurface);
	placement_remove(&parent->current.subsurfaces, subsurface);
	if (parent->cached) {
		placement_remove(&parent->cached->subsurfaces, subsurface);
	}
	struct transaction_entry *entry;
	wl_list_for_each(entry, &parent->transaction_entries, surface_link) {
		placement_remove(&entry->state->subsurfaces, subsurface);
	}
	wl_list_remove(&subsurface->parent_link);
	scene_node_detach(&surface->scene.tree.node);
	surface->subsurface = NULL;
	surface->role = SURFACE_ROLE_NONE;
	surface->parent = NULL;
	free(subsurface);
	subsurface_flush_cache(surface);
}

static void
subsurface_resource_destroy(struct wl_resource *resource)
{
	struct subsurface *subsurface = wl_resource_get_user_data(resource);
	if (subsurface) {
		subsurface_destroy(subsurface);
	}
}

// The position is in the state of the parent.
static void
subsurface_set_position_req(struct wl_client *client,
	struct wl_resource *resource, int32_t x, int32_t y)
{
	struct subsurface *role = wl_resource_get_user_data(resource);
	if (!role) {
		return;
	}
	struct subsurface_placement *placement = placement_find(
		&role->surface->parent->pending.subsurfaces, role);
	placement->x = x;
	placement->y = y;
}

// The sibling is the parent, as NULL, or another subsurface of the parent.
static bool
subsurface_sibling_role(struct subsurface *role,
	struct wl_resource *sibling_resource, struct subsurface **sibling_role)
{
	struct surface *sibling = wl_resource_get_user_data(sibling_resource);
	if (!sibling || sibling == role->surface) {
		return false;
	}
	if (sibling == role->surface->parent) {
		*sibling_role = NULL;
		return true;
	}
	if (sibling->role != SURFACE_ROLE_SUBSURFACE
			|| sibling->parent != role->surface->parent) {
		return false;
	}
	*sibling_role = sibling->subsurface;
	return true;
}

// The order is in the state of the parent.
static void
subsurface_place(struct wl_resource *resource,
	struct wl_resource *sibling_resource, bool above)
{
	struct subsurface *role = wl_resource_get_user_data(resource);
	if (!role) {
		return;
	}
	struct subsurface *sibling;
	if (!subsurface_sibling_role(role, sibling_resource, &sibling)) {
		wl_resource_post_error(resource, WL_SUBSURFACE_ERROR_BAD_SURFACE,
			"sibling is not the parent or a sibling");
		return;
	}
	struct wl_array *placements = &role->surface->parent->pending.subsurfaces;
	struct subsurface_placement placement = *placement_find(placements, role);
	placement_remove(placements, role);
	struct subsurface_placement *at = placement_find(placements, sibling);
	size_t index = (size_t)(at - (struct subsurface_placement *)placements->data);
	placement_insert(placements, above ? index + 1 : index, placement);
}

static void
subsurface_place_above_req(struct wl_client *client,
	struct wl_resource *resource, struct wl_resource *sibling_resource)
{
	subsurface_place(resource, sibling_resource, true);
}

static void
subsurface_place_below_req(struct wl_client *client,
	struct wl_resource *resource, struct wl_resource *sibling_resource)
{
	subsurface_place(resource, sibling_resource, false);
}

static void
subsurface_set_sync_req(struct wl_client *client, struct wl_resource *resource)
{
	struct subsurface *role = wl_resource_get_user_data(resource);
	if (role) {
		role->synchronized = true;
	}
}

static void
subsurface_set_desync_req(struct wl_client *client, struct wl_resource *resource)
{
	struct subsurface *role = wl_resource_get_user_data(resource);
	if (role) {
		role->synchronized = false;
		subsurface_flush_cache(role->surface);
	}
}

static const struct wl_subsurface_interface subsurface_impl = {
	.destroy = resource_destroy_req,
	.set_position = subsurface_set_position_req,
	.place_above = subsurface_place_above_req,
	.place_below = subsurface_place_below_req,
	.set_sync = subsurface_set_sync_req,
	.set_desync = subsurface_set_desync_req,
};

static void
subcompositor_get_subsurface_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id, struct wl_resource *surface_resource,
	struct wl_resource *parent_resource)
{
	struct surface *surface = wl_resource_get_user_data(surface_resource);
	struct surface *parent = wl_resource_get_user_data(parent_resource);
	if (!surface || surface->role != SURFACE_ROLE_NONE) {
		wl_resource_post_error(resource, WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE,
			"wl_surface already has a role");
		return;
	}
	if (!parent || surface_contains(surface, parent)) {
		wl_resource_post_error(resource, WL_SUBCOMPOSITOR_ERROR_BAD_PARENT,
			"wl_surface parent is invalid");
		return;
	}

	struct wl_resource *subsurface = wl_resource_create(client,
		&wl_subsurface_interface, 1, id);
	assert(subsurface);
	struct subsurface *role = subsurface_create(surface, parent, subsurface);
	wl_resource_set_implementation(subsurface, &subsurface_impl, role,
		subsurface_resource_destroy);
}

static const struct wl_subcompositor_interface subcompositor_impl = {
	.destroy = resource_destroy_req,
	.get_subsurface = subcompositor_get_subsurface_req,
};

void
wayland_bind_subcompositor(struct wl_client *client, void *data, uint32_t version,
	uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&wl_subcompositor_interface, MIN(version, 1), id);
	assert(resource);
	wl_resource_set_implementation(resource, &subcompositor_impl, data, NULL);
}
