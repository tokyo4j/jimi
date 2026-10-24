#include <assert.h>
#include <stdlib.h>

#include "core.h"
#include "surface.h"

static void
scene_node_init(struct scene_node *node,
	enum scene_node_type type, struct scene_tree *parent)
{
	*node = (struct scene_node){
		.type = type,
		.parent = parent,
		.enabled = true,
	};
	wl_list_init(&node->link);
	if (parent) {
		list_append(&parent->children, &node->link);
	}
}

void
scene_tree_init(struct scene_tree *tree, struct scene_tree *parent)
{
	*tree = (struct scene_tree){0};
	scene_node_init(&tree->node, SCENE_NODE_TREE, parent);
	wl_list_init(&tree->children);
}

void
scene_init(void)
{
	scene_tree_init(&core.scene.tree, NULL);
	for (size_t i = 0; i < NR_SCENE_LAYERS; i++) {
		scene_tree_init(&core.scene.layers[i], &core.scene.tree);
	}
}

void
scene_tree_fini(struct scene_tree *tree)
{
	assert(tree);
	assert(wl_list_empty(&tree->children));
	scene_node_detach(&tree->node);
}

void
scene_rect_init(struct scene_rect *rect, struct scene_tree *parent,
	struct color color, float radius, uint32_t corners)
{
	*rect = (struct scene_rect){
		.color = color,
		.radius = radius,
		.corners = corners,
	};
	scene_node_init(&rect->node, SCENE_NODE_RECT, parent);
}

void
scene_shadow_init(struct scene_shadow *shadow, struct scene_tree *parent,
	struct color color, int blur, int offset_x, int offset_y, uint32_t corners)
{
	*shadow = (struct scene_shadow){
		.corners = corners,
		.color = color,
		.blur = blur,
		.offset_x = offset_x,
		.offset_y = offset_y,
	};
	scene_node_init(&shadow->node, SCENE_NODE_SHADOW, parent);
}

void
scene_texture_init(struct scene_texture *node, struct scene_tree *parent)
{
	*node = (struct scene_texture){0};
	scene_node_init(&node->node, SCENE_NODE_TEXTURE, parent);
}

void
scene_thumbnail_init(struct scene_thumbnail *thumbnail,
	struct scene_tree *parent, struct window *window)
{
	*thumbnail = (struct scene_thumbnail){
		.window = window,
		.outdated = true,
	};
	scene_node_init(&thumbnail->node, SCENE_NODE_THUMBNAIL, parent);
}

// The tree of a surface lives as long as the surface. Its role places it in
// the scene, and subsurfaces in the tree of their parent.
void
scene_surface_init(struct surface *surface)
{
	scene_tree_init(&surface->scene.tree, NULL);
	scene_node_init(&surface->scene.node, SCENE_NODE_SURFACE, &surface->scene.tree);
	scene_node_set_enabled(&surface->scene.tree.node, false);
}

void
scene_surface_fini(struct surface *surface)
{
	assert(wl_list_empty(&surface->subsurfaces));
	assert(wl_list_empty(&surface->popups));
	scene_node_detach(&surface->scene.node);
	scene_tree_fini(&surface->scene.tree);
}

// Stacks the content node and the subsurface trees in the tree of the surface
// as placed by its current state.
void
scene_surface_reconfigure(struct surface *surface)
{
	struct scene_node *previous = NULL;
	struct subsurface_placement *placement;
	wl_array_for_each(placement, &surface->current.subsurfaces) {
		struct scene_node *node = &surface->scene.node;
		if (placement->subsurface) {
			node = &placement->subsurface->surface->scene.tree.node;
			scene_node_set_position(node, placement->x, placement->y);
		}
		if (previous) {
			scene_node_place_above(node, previous);
		}
		previous = node;
	}
}

static struct box
scene_node_extent_at(struct scene_node *node, int offset_x, int offset_y)
{
	if (!node->enabled) {
		return (struct box){0};
	}

	struct box box = {.x = offset_x + node->x, .y = offset_y + node->y};
	switch (node->type) {
	case SCENE_NODE_TREE: {
		struct scene_tree *tree = wl_container_of(node, tree, node);
		struct box extent = {0};
		struct scene_node *child;
		wl_list_for_each(child, &tree->children, link) {
			struct box child_extent = scene_node_extent_at(child, box.x, box.y);
			box_union(&extent, &child_extent);
		}
		return extent;
	}
	case SCENE_NODE_SURFACE: {
		struct surface *surface = wl_container_of(node, surface, scene.node);
		box.width = surface->width;
		box.height = surface->height;
		return box;
	}
	case SCENE_NODE_RECT: {
		struct scene_rect *rect = wl_container_of(node, rect, node);
		box.width = rect->width;
		box.height = rect->height;
		return box;
	}
	case SCENE_NODE_SHADOW: {
		struct scene_shadow *shadow = wl_container_of(node, shadow, node);
		box.x += shadow->offset_x - shadow->blur;
		box.y += shadow->offset_y - shadow->blur;
		box.width = shadow->width + shadow->blur * 2;
		box.height = shadow->height + shadow->blur * 2;
		return box;
	}
	case SCENE_NODE_TEXTURE: {
		struct scene_texture *texture = wl_container_of(node, texture, node);
		box.width = texture->width;
		box.height = texture->height;
		return box;
	}
	case SCENE_NODE_THUMBNAIL: {
		struct scene_thumbnail *thumbnail = wl_container_of(node, thumbnail, node);
		box.width = thumbnail->width;
		box.height = thumbnail->height;
		return box;
	}
	}

	assert(!"unknown scene node type");
	return (struct box){0};
}

// The origin of the parent of the node in layout coordinates. Returns false if
// the node or an ancestor is detached, or an ancestor is disabled.
static bool
scene_node_parent_origin(struct scene_node *node, int *x, int *y)
{
	*x = 0;
	*y = 0;
	for (struct scene_tree *parent = node->parent; parent;
			parent = parent->node.parent) {
		if (!parent->node.enabled) {
			return false;
		}
		*x += parent->node.x;
		*y += parent->node.y;
		node = &parent->node;
	}
	return node == &core.scene.tree.node;
}

struct box
scene_node_extent(struct scene_node *node)
{
	int x, y;
	if (!scene_node_parent_origin(node, &x, &y)) {
		return (struct box){0};
	}
	return scene_node_extent_at(node, x, y);
}

static void
damage_node(struct scene_node *node)
{
	struct box extent = scene_node_extent(node);
	damage_layout_box(&extent);
}

struct box
scene_node_layout_box(struct scene_node *node, const struct box *local)
{
	int x, y;
	if (box_empty(local) || !node->enabled
			|| !scene_node_parent_origin(node, &x, &y)) {
		return (struct box){0};
	}

	struct box box = *local;
	if (node->type == SCENE_NODE_SURFACE) {
		struct surface *surface = wl_container_of(node, surface, scene.node);
		struct box bounds = {
			.width = surface->width,
			.height = surface->height,
		};
		if (!box_intersection(&box, &box, &bounds)) {
			return (struct box){0};
		}
	}
	box.x += x + node->x;
	box.y += y + node->y;
	return box;
}

void
scene_node_layout_position(struct scene_node *node, int *x, int *y)
{
	*x = node->x;
	*y = node->y;
	for (struct scene_tree *tree = node->parent; tree; tree = tree->node.parent) {
		*x += tree->node.x;
		*y += tree->node.y;
	}
}

void
scene_node_detach(struct scene_node *node)
{
	if (!node->parent) {
		return;
	}
	damage_node(node);
	wl_list_remove(&node->link);
	wl_list_init(&node->link);
	node->parent = NULL;
	damage_node(node);
}

void
scene_node_reparent(struct scene_node *node, struct scene_tree *parent)
{
	if (node->parent == parent) {
		return;
	}
	scene_node_detach(node);
	node->parent = parent;
	list_append(&parent->children, &node->link);
	damage_node(node);
}

void
scene_node_set_enabled(struct scene_node *node, bool enabled)
{
	if (node->enabled == enabled) {
		return;
	}
	// Note: either of these boxes is empty
	damage_node(node);
	node->enabled = enabled;
	damage_node(node);
}

void
scene_node_set_position(struct scene_node *node, int x, int y)
{
	if (node->x == x && node->y == y) {
		return;
	}
	damage_node(node);
	node->x = x;
	node->y = y;
	damage_node(node);
}

void
scene_rect_set_size(struct scene_rect *rect, int width, int height)
{
	if (rect->width == width && rect->height == height) {
		return;
	}
	damage_node(&rect->node);
	rect->width = width;
	rect->height = height;
	damage_node(&rect->node);
}

void
scene_rect_set_color(struct scene_rect *rect, struct color color)
{
	if (rect->color.r == color.r && rect->color.g == color.g
			&& rect->color.b == color.b && rect->color.a == color.a) {
		return;
	}
	rect->color = color;
	damage_node(&rect->node);
}

void
scene_rect_set_radius(struct scene_rect *rect, float radius)
{
	if (rect->radius == radius) {
		return;
	}
	rect->radius = radius;
	damage_node(&rect->node);
}

void
scene_rect_set_corners(struct scene_rect *rect, uint32_t corners)
{
	if (rect->corners == corners) {
		return;
	}
	rect->corners = corners;
	damage_node(&rect->node);
}

void
scene_rect_set_border_width(struct scene_rect *rect, int border_width)
{
	if (rect->border_width == border_width) {
		return;
	}
	rect->border_width = border_width;
	damage_node(&rect->node);
}

void
scene_shadow_set_size(struct scene_shadow *shadow, int width, int height)
{
	if (shadow->width == width && shadow->height == height) {
		return;
	}
	damage_node(&shadow->node);
	shadow->width = width;
	shadow->height = height;
	damage_node(&shadow->node);
}

void
scene_shadow_set_radius(struct scene_shadow *shadow, float radius)
{
	if (shadow->radius == radius) {
		return;
	}
	shadow->radius = radius;
	damage_node(&shadow->node);
}

void
scene_shadow_set_color(struct scene_shadow *shadow, struct color color)
{
	if (shadow->color.r == color.r && shadow->color.g == color.g
			&& shadow->color.b == color.b && shadow->color.a == color.a) {
		return;
	}
	shadow->color = color;
	damage_node(&shadow->node);
}

// The texture is drawn at the size.
void
scene_texture_set_texture(struct scene_texture *node, struct texture *texture,
	int width, int height)
{
	if (node->texture == texture && node->width == width && node->height == height) {
		return;
	}
	damage_node(&node->node);
	node->texture = texture;
	node->width = width;
	node->height = height;
	damage_node(&node->node);
}

// The surfaces are drawn into the texture, which is drawn at the size.
void
scene_thumbnail_set_texture(struct scene_thumbnail *thumbnail,
	struct texture *texture, int width, int height)
{
	damage_node(&thumbnail->node);
	thumbnail->texture = texture;
	thumbnail->width = width;
	thumbnail->height = height;
	thumbnail->outdated = true;
	damage_node(&thumbnail->node);
}

// Draws the surfaces into the texture again, as they have changed.
void
scene_thumbnail_update(struct scene_thumbnail *thumbnail)
{
	thumbnail->outdated = true;
	damage_node(&thumbnail->node);
}

void
scene_node_raise_to_top(struct scene_node *node)
{
	assert(node->parent);
	if (node->parent->children.prev == &node->link) {
		return;
	}
	damage_node(node);
	wl_list_remove(&node->link);
	list_append(&node->parent->children, &node->link);
	damage_node(node);
}

void
scene_node_place_above(struct scene_node *node, struct scene_node *sibling)
{
	assert(node->parent && node->parent == sibling->parent);
	if (node->link.prev == &sibling->link) {
		return;
	}
	damage_node(node);
	wl_list_remove(&node->link);
	wl_list_insert(&sibling->link, &node->link);
	damage_node(node);
}

void
scene_node_place_below(struct scene_node *node, struct scene_node *sibling)
{
	assert(node->parent && node->parent == sibling->parent);
	if (node->link.next == &sibling->link) {
		return;
	}
	damage_node(node);
	wl_list_remove(&node->link);
	wl_list_insert(sibling->link.prev, &node->link);
	damage_node(node);
}

static struct scene_node *
scene_node_at_offset(struct scene_node *node, int offset_x, int offset_y,
	double x, double y)
{
	if (!node->enabled || node->type == SCENE_NODE_SHADOW) {
		return NULL;
	}

	if (node->type == SCENE_NODE_TREE) {
		struct scene_tree *tree = wl_container_of(node, tree, node);
		struct scene_node *child;
		wl_list_for_each_reverse(child, &tree->children, link) {
			struct scene_node *found = scene_node_at_offset(child,
				offset_x + node->x, offset_y + node->y, x, y);
			if (found) {
				return found;
			}
		}
		return NULL;
	}

	struct box box = scene_node_extent_at(node, offset_x, offset_y);
	if (!box_contains_point(&box, x, y)) {
		return NULL;
	}
	if (node->type == SCENE_NODE_SURFACE) {
		// TODO: As explained somewhere else, we are not clipping the client
		// surface and it may spill outside the decoration.
		struct surface *surface = wl_container_of(node, surface, scene.node);
		int surface_x = (int)(x - box.x);
		int surface_y = (int)(y - box.y);
		if (surface->current.input_region_set
				&& !pixman_region32_contains_point(
					&surface->current.input_region, surface_x, surface_y, NULL)) {
			return NULL;
		}
	}
	return node;
}

// The top-most node in the subtree of the node at the point, in layout
// coordinates. Trees and shadows are never found, and surfaces only within
// their input region. Rects are found even where they are transparent, so
// that they can take the pointer between other nodes.
struct scene_node *
scene_node_at(struct scene_node *node, double x, double y)
{
	/* Scene nodes use integer positions, so hit testing follows the renderer's
	 * accumulated integer coordinates and keeps the local remainder here. */
	int offset_x = 0;
	int offset_y = 0;
	if (node->parent) {
		scene_node_layout_position(&node->parent->node, &offset_x, &offset_y);
	}
	return scene_node_at_offset(node, offset_x, offset_y, x, y);
}

// The surface at the point, unless another node covers it there, and the
// point in surface-local coordinates.
struct surface *
scene_surface_at(struct scene_node *node, double x, double y, double *sx, double *sy)
{
	struct scene_node *found = scene_node_at(node, x, y);
	if (!found || found->type != SCENE_NODE_SURFACE) {
		return NULL;
	}
	struct surface *surface = wl_container_of(found, surface, scene.node);
	int surface_x, surface_y;
	scene_node_layout_position(found, &surface_x, &surface_y);
	*sx = x - surface_x;
	*sy = y - surface_y;
	return surface;
}
