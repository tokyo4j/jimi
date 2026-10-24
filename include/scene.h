#ifndef JIMI_SCENE_H
#define JIMI_SCENE_H

#include "util.h"

struct surface;
struct texture;
struct window;

// Surface nodes are only embedded in struct surface. Rect, shadow, texture
// and thumbnail nodes are embedded in named fields of their owner, which tells
// them apart by their addresses, e.g. when hit-testing.
enum scene_node_type {
	SCENE_NODE_TREE,
	SCENE_NODE_SURFACE,
	SCENE_NODE_RECT,
	SCENE_NODE_SHADOW,
	SCENE_NODE_TEXTURE,
	SCENE_NODE_THUMBNAIL,
};

enum corner {
	CORNER_TOP_LEFT = 1u << 0,
	CORNER_TOP_RIGHT = 1u << 1,
	CORNER_BOTTOM_RIGHT = 1u << 2,
	CORNER_BOTTOM_LEFT = 1u << 3,
};

// Children of the scene root, bottom-to-top.
enum scene_layer {
	SCENE_LAYER_BACKGROUND,
	SCENE_LAYER_BOTTOM,
	SCENE_LAYER_TOPLEVEL, // Only window.tree and core.snap_preview
	SCENE_LAYER_UNMANAGED, // Override-redirect X11 windows.
	SCENE_LAYER_TOP,
	SCENE_LAYER_OVERLAY,
	SCENE_LAYER_POPUP, // Popups of layer surfaces.
	SCENE_LAYER_INPUT_PANEL, // Surfaces of the input method.
	SCENE_LAYER_OSD, // Only core.window_switcher.outline and tree
	// Lock surfaces. The layers below are disabled while the session is
	// locked.
	SCENE_LAYER_LOCK,
	NR_SCENE_LAYERS,
};

struct scene_node {
	enum scene_node_type type;
	struct scene_tree *parent;
	struct wl_list link; // scene_tree.children
	int x;
	int y;
	bool enabled;
};

struct scene_tree {
	struct scene_node node;
	struct wl_list children; // scene_node.link
};

struct scene_rect {
	struct scene_node node;
	int width;
	int height;
	struct color color;
	float radius;
	uint32_t corners; // enum corner, the ones rounded by the radius
	// Only the border of the width inside the edges is filled, or the whole
	// rect if 0.
	int border_width;
};

// The blurred shadow of a box at the node, drawn around it. The box itself is
// left out, so that the shadow does not show through what fills it.
struct scene_shadow {
	struct scene_node node;
	int width; // Of the box
	int height;
	float radius;
	uint32_t corners; // enum corner, the ones of the box rounded by the radius
	struct color color;
	int blur; // The distance past the box over which the shadow fades out.
	// Of the shadow from the box
	int offset_x;
	int offset_y;
};

struct scene_texture {
	struct scene_node node;
	struct texture *texture;
	int width;
	int height;
};

// Shows the surfaces of a window without its decoration, scaled to the size.
// They are drawn into the texture before the node is drawn, again after each
// scene_thumbnail_update().
struct scene_thumbnail {
	struct scene_node node;
	struct window *window;
	struct texture *texture; // Owned by the owner of the node.
	int width;
	int height;
	bool outdated;
};

struct scene {
	struct scene_tree tree;
	struct scene_tree layers[NR_SCENE_LAYERS]; // In the tree
};

void scene_init(void);
void scene_tree_init(struct scene_tree *tree, struct scene_tree *parent);
void scene_tree_fini(struct scene_tree *tree);
void scene_surface_init(struct surface *surface);
void scene_surface_fini(struct surface *surface);
void scene_surface_reconfigure(struct surface *surface);
void scene_node_set_position(struct scene_node *node, int x, int y);
void scene_node_set_enabled(struct scene_node *node, bool enabled);
void scene_node_detach(struct scene_node *node);
void scene_node_reparent(struct scene_node *node, struct scene_tree *parent);
void scene_node_raise_to_top(struct scene_node *node);
void scene_node_place_above(struct scene_node *node, struct scene_node *sibling);
void scene_node_place_below(struct scene_node *node, struct scene_node *sibling);
void scene_node_layout_position(struct scene_node *node, int *x, int *y);
struct box scene_node_extent(struct scene_node *node);
struct box scene_node_layout_box(struct scene_node *node, const struct box *local);
struct scene_node *scene_node_at(struct scene_node *node, double x, double y);
struct surface *scene_surface_at(struct scene_node *node, double x, double y,
	double *sx, double *sy);
void scene_rect_init(struct scene_rect *rect, struct scene_tree *parent,
	struct color color, float radius, uint32_t corners);
void scene_rect_set_size(struct scene_rect *rect, int width, int height);
void scene_rect_set_color(struct scene_rect *rect, struct color color);
void scene_rect_set_radius(struct scene_rect *rect, float radius);
void scene_rect_set_corners(struct scene_rect *rect, uint32_t corners);
void scene_rect_set_border_width(struct scene_rect *rect, int border_width);
void scene_shadow_init(struct scene_shadow *shadow, struct scene_tree *parent,
	struct color color, int blur, int offset_x, int offset_y, uint32_t corners);
void scene_shadow_set_size(struct scene_shadow *shadow, int width, int height);
void scene_shadow_set_radius(struct scene_shadow *shadow, float radius);
void scene_shadow_set_color(struct scene_shadow *shadow, struct color color);
void scene_texture_init(struct scene_texture *node, struct scene_tree *parent);
void scene_texture_set_texture(struct scene_texture *node,
	struct texture *texture, int width, int height);
void scene_thumbnail_init(struct scene_thumbnail *thumbnail,
	struct scene_tree *parent, struct window *window);
void scene_thumbnail_set_texture(struct scene_thumbnail *thumbnail,
	struct texture *texture, int width, int height);
void scene_thumbnail_update(struct scene_thumbnail *thumbnail);

#endif
