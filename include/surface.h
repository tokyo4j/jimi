#ifndef JIMI_SURFACE_H
#define JIMI_SURFACE_H

#include <pixman.h>

#include "scene.h"
#include "util.h"
#include "window.h"

struct gbm_bo;
struct image_description;
struct output;
struct pointer_constraint;
struct texture;
struct transaction;
struct xdg_toplevel_decoration;
struct xwayland_surface;

enum client_buffer_type {
	CLIENT_BUFFER_SHM,
	CLIENT_BUFFER_DMABUF,
};

// A DRM syncobj timeline imported by linux-drm-syncobj-v1.
struct syncobj_timeline {
	uint32_t handle;
	uint32_t refs;
};

struct syncobj_point {
	struct syncobj_timeline *timeline; // Referenced, NULL if unset.
	uint64_t value;
};

// A wl_buffer and the texture showing it. A dma-buf is imported once, and
// the contents of an shm buffer are copied to the texture on each commit.
struct client_buffer {
	enum client_buffer_type type;
	struct wl_resource *resource; // NULL once the client destroys it.
	struct wl_listener resource_destroy;
	uint32_t width;
	uint32_t height;
	// Surface states holding the buffer. Once the last one lets go, a
	// committed dma-buf is released, and a destroyed buffer is freed.
	uint32_t locks;
	bool committed; // Since the last release
	// The releases of the commits, sent along with wl_buffer.release.
	struct wl_list release_callbacks; // wl_callback resource links
	struct wl_array syncobj_releases; // struct syncobj_point
	int dmabuf_fd; // For the implicit fences, or -1.
	// Of a dma-buf. shm buffers are copied into the textures of surfaces.
	struct texture *texture;
	// Scans out the dma-buf on primary planes, ignoring its alpha channel.
	// 0 if KMS cannot, like for shm buffers.
	uint32_t fb;
	struct gbm_bo *bo; // Referenced by the framebuffer, or NULL without one
	bool scanout_rejected; // By a commit, so it is drawn instead.
};

enum surface_role {
	SURFACE_ROLE_NONE,
	SURFACE_ROLE_CURSOR, // No role object; the pointer holds the state.
	SURFACE_ROLE_DND_ICON, // No role object; the pointer holds the state.
	SURFACE_ROLE_SUBSURFACE,
	SURFACE_ROLE_XDG_SURFACE, // xdg_surface without a toplevel or popup yet
	SURFACE_ROLE_XDG_TOPLEVEL,
	SURFACE_ROLE_XDG_POPUP,
	SURFACE_ROLE_LAYER_SURFACE,
	SURFACE_ROLE_INPUT_PANEL,
	SURFACE_ROLE_LOCK_SURFACE,
	SURFACE_ROLE_XWAYLAND,
};

struct xdg_surface_state {
	struct box window_geometry;
	uint32_t configure_serial; // Last acked configure.
};

struct layer_surface_state {
	uint32_t layer;
	uint32_t anchor;
	int32_t exclusive_zone;
	uint32_t exclusive_edge; // 0 means deduced from the anchor.
	uint32_t keyboard_interactivity;
	int width; // 0 means the width between the anchored edges.
	int height; // 0 means the height between the anchored edges.
	struct {
		int top;
		int right;
		int bottom;
		int left;
	} margin;
};

// The stacking position of a subsurface, or of the parent itself.
struct subsurface_placement {
	struct subsurface *subsurface; // NULL for the parent.
	int x;
	int y;
};

// The double-buffered state of a surface and its role. The pending state
// keeps the persistent values after each commit takes the state out of it,
// so commits are checked against it.
struct surface_state {
	// Set by wl_surface.attach. Otherwise, the commit keeps the buffer.
	bool buffer_attached;
	// Locked. Only the current state holds a buffer that is not attached.
	struct client_buffer *buffer;
	// The size of the buffer the surface has after the commit, 0x0 without
	// a buffer.
	uint32_t buffer_width;
	uint32_t buffer_height;
	// The releases for the attached buffer, which takes them on commit.
	struct wl_list release_callbacks; // wl_callback resource links
	struct syncobj_point release; // Set by linux-drm-syncobj-v1
	// Signals once the attached buffer is drawn. Set by
	// linux-drm-syncobj-v1, or else the dma-buf has implicit fences.
	struct syncobj_point acquire;
	struct wl_list frame_callbacks; // wl_callback resource links
	// Wait in the current state until a frame of an output shows the surface.
	// wp_presentation_feedback resource links
	struct wl_list presentation_feedbacks;
	enum wl_output_transform transform;
	int scale;
	int dx, dy; // Offset delta.
	// Set by wp_viewport. The source is in the coordinates of the buffer
	// after its transform and scale. Negative widths leave them unset.
	struct fbox viewport_source;
	int viewport_width;
	int viewport_height;
	// Set by wp_tearing_control_v1. The content may be shown as soon as it
	// is committed, tearing.
	bool tearing;
	// Referenced. Set by wp_color_management_surface_v1, or NULL for sRGB.
	struct image_description *image_description;
	pixman_region32_t input_region;
	bool input_region_set; // false means the protocol's infinite default.
	pixman_region32_t opaque_region;
	// Set by zwp_pointer_constraints_v1, for the constraint of the surface.
	pixman_region32_t constraint_region;
	bool constraint_region_set; // false means the input region.
	// Where the client draws its cursor while the pointer is locked, which
	// the pointer moves to when unlocked.
	bool cursor_hint_set;
	double cursor_hint_x, cursor_hint_y;
	pixman_region32_t damage; // Surface-local
	pixman_region32_t buffer_damage; // Buffer-local
	// The subsurfaces and the surface itself, bottom-to-top.
	struct wl_array subsurfaces; // struct subsurface_placement
	// The states of the roles, which keep their current ones themselves.
	struct xdg_surface_state xdg;
	struct toplevel_icon *toplevel_icon; // Referenced; set by xdg-toplevel-icon
	struct layer_surface_state layer;
	uint64_t xwayland_serial; // 0 unless set since the last commit.
};

// A committed state waiting in a transaction until it can be applied.
struct transaction_entry {
	struct wl_list link; // transaction.entries
	struct surface *surface; // NULL once destroyed
	struct wl_list surface_link; // surface.transaction_entries
	struct surface_state *state; // NULL once the surface is destroyed
	// Readable once the attached buffer is ready to be sampled.
	struct wl_event_source *fence;
};

struct surface {
	struct wl_resource *resource;
	struct wl_list link; // core.surfaces

	struct surface_state pending, current;
	// Committed while the subsurface is synchronized, and applied with the
	// state of the parent.
	struct surface_state *cached;
	// Committed states waiting to be applied, in commit order.
	struct wl_list transaction_entries; // transaction_entry.surface_link

	struct {
		struct scene_tree tree; // Contains the content node and subsurface trees.
		struct scene_node node; // type=SCENE_NODE_SURFACE
	} scene;

	int width;
	int height;
	int offset_x, offset_y; // Accumulated surface content offset.
	// The contents of the current shm buffer, copied as far as it is
	// damaged. NULL with other buffers, or once the contents are lost.
	struct texture *shm_texture;
	struct wl_resource *viewport; // wp_viewport
	struct wl_resource *fractional_scale; // wp_fractional_scale_v1
	struct wl_resource *tearing_control; // wp_tearing_control_v1
	struct wl_resource *syncobj; // wp_linux_drm_syncobj_surface_v1
	struct pointer_constraint *pointer_constraint;
	struct wl_list dmabuf_feedbacks; // zwp_linux_dmabuf_feedback_v1 resource links
	struct wl_resource *color_management; // wp_color_management_surface_v1
	// wp_color_management_surface_feedback_v1 resource links
	struct wl_list color_feedbacks;
	// The output whose primary plane the dma-buf feedback suggests
	// allocating buffers for, or NULL.
	struct output *scanout_output;
	// The scale, transform and colors the surface is told to draw with, set
	// by surfaces_update_outputs(). The scale is 0 and the image
	// description is NULL until an output is enabled.
	double preferred_scale;
	enum wl_output_transform preferred_transform;
	struct image_description *preferred_image_description; // Referenced
	// output.bit of the outputs the surface has entered, as told by
	// wl_surface.enter and leave.
	uint32_t outputs;
	bool mapped;
	struct surface *parent; // The parent of a subsurface.
	struct wl_list subsurfaces; // subsurface.parent_link
	struct wl_list popups; // xdg_popup.parent_link
	struct wl_list focus_link; // keyboard.focus_history

	enum surface_role role;
	union {
		struct subsurface *subsurface; // SUBSURFACE
		struct xdg_surface *xdg; // XDG_SURFACE, XDG_TOPLEVEL, XDG_POPUP
		struct layer_surface *layer_surface; // LAYER_SURFACE
		struct input_panel_surface *input_panel; // INPUT_PANEL
		struct lock_surface *lock_surface; // LOCK_SURFACE
		struct xwayland_surface *xwayland; // XWAYLAND
	};
};

struct subsurface {
	struct surface *surface;
	struct wl_resource *resource;
	struct wl_list parent_link; // surface.subsurfaces
	// Commits are cached until the state of the parent is applied.
	bool synchronized;
	// Shown once the state of the parent includes it.
	bool added;
};

struct xdg_positioner_state {
	int width;
	int height;
	struct box anchor_rect;
	uint32_t anchor; // enum edge
	uint32_t gravity; // enum edge
	uint32_t constraint_adjustment;
	int offset_x;
	int offset_y;
	int parent_width;
	int parent_height;
	uint32_t parent_configure;
	bool size_set;
	bool anchor_rect_set;
	bool reactive;
	bool parent_size_set;
};

struct xdg_surface {
	struct surface *surface;
	struct wl_resource *resource;
	struct xdg_surface_state current;
	// Set by the initial commit, which the client repeats after unmapping a
	// toplevel. Toplevels are configured only after it.
	bool initialized;
	bool configure_sent;
	union {
		struct xdg_toplevel *toplevel; // surface->role == XDG_TOPLEVEL
		struct xdg_popup *popup; // surface->role == XDG_POPUP
	};
};

struct xdg_toplevel {
	struct xdg_surface *base;
	struct wl_resource *resource;
	struct window window;
	struct xdg_toplevel_decoration *decoration;
	struct wl_resource *dialog; // xdg_dialog_v1, or NULL
	bool configured_activated; // Activation in the last configure sent.
	bool wm_capabilities_sent;
};

struct xdg_popup {
	struct xdg_surface *base;
	struct wl_resource *resource;
	// The xdg surface or layer surface the popup is placed on. NULL until a
	// layer surface adopts a popup created without a parent.
	struct surface *parent;
	struct wl_list parent_link; // surface.popups
	// At the top-left corner of the window geometry. Contains the surface
	// tree and the trees of the child popups.
	struct scene_tree tree;
	struct xdg_positioner_state positioner;
	struct box geometry; // Relative to the parent's window geometry.
	bool grabbed;
	bool dismissed;
	// A grab requested before a layer surface adopted the popup.
	bool grab_pending;
};

struct layer_surface {
	struct surface *surface;
	struct wl_resource *resource;
	struct output *output;
	struct wl_list link; // output.layer_surfaces[current.layer]
	struct layer_surface_state current;
	// Cleared by unmapping, after which the client starts over with an
	// initial commit.
	bool initialized;
	bool configured;
	int configured_width;
	int configured_height;
};

enum input_panel_type {
	INPUT_PANEL_NONE, // Not shown until the type is set.
	INPUT_PANEL_TOPLEVEL, // A keyboard at the bottom of the output
	INPUT_PANEL_OVERLAY, // Near the cursor of the text input
};

// A surface of the input method, which is shown while a text input is
// enabled.
struct input_panel_surface {
	struct surface *surface;
	struct wl_resource *resource;
	struct wl_list link; // input_method.panels
	enum input_panel_type type;
};

void surface_set_mapped(struct surface *surface, bool mapped);
struct fbox surface_source_box(struct surface *surface);
void surfaces_update_outputs(void);
void surfaces_send_enter(struct output *output, struct wl_resource *output_resource);
struct surface *surface_root(struct surface *surface);
bool surface_contains(struct surface *ancestor, struct surface *surface);
void surface_from_layout(struct surface *surface, double x, double y,
	double *sx, double *sy);
bool resource_matches_surface(struct wl_resource *resource, struct surface *surface);
bool surface_has_buffer(struct surface *surface);
void region_copy(pixman_region32_t *dest, struct wl_resource *resource);
void surface_apply_state(struct surface *surface, struct surface_state *state);
void surface_state_destroy(struct surface_state *state);

void transaction_commit(struct surface *surface, struct surface_state *state);
void transactions_apply_ready(void);
void transactions_forget_surface(struct surface *surface);

struct client_buffer *client_buffer_create(enum client_buffer_type type,
	struct wl_resource *resource, uint32_t width, uint32_t height,
	struct texture *texture, int dmabuf_fd);
struct client_buffer *client_buffer_from_resource(struct wl_resource *resource);
void client_buffer_lock(struct client_buffer *buffer);
void client_buffer_unlock(struct client_buffer *buffer);
void client_buffer_commit(struct client_buffer *buffer,
	struct surface_state *pending);
void client_buffer_upload_shm(struct client_buffer *buffer,
	struct texture **texture, const pixman_region32_t *damage);
int client_buffer_fence(struct client_buffer *buffer);

void subsurface_destroy(struct subsurface *subsurface);
bool surface_synchronized(struct surface *surface);

void syncobj_point_set(struct syncobj_point *point,
	struct syncobj_timeline *timeline, uint64_t value);
void syncobj_point_unset(struct syncobj_point *point);
int syncobj_point_fence(const struct syncobj_point *point);
void syncobj_point_signal(const struct syncobj_point *point, int sync_file);
bool syncobj_surface_validate(struct surface *surface);
void syncobj_surface_destroy(struct surface *surface);

void color_management_send_preferred(struct surface *surface);
void color_management_surface_destroy(struct surface *surface);

void dmabuf_feedback_set_scanout_output(struct surface *surface,
	struct output *output);
void dmabuf_feedback_unset_scanout_output(struct output *output);
void dmabuf_feedback_surface_destroy(struct surface *surface);

bool viewport_validate(struct surface *surface, int width, int height);
void viewport_surface_destroy(struct surface *surface);
void fractional_scale_send(struct surface *surface);
void fractional_scale_surface_destroy(struct surface *surface);
void tearing_control_surface_destroy(struct surface *surface);
void presentation_feedbacks_send_presented(struct output *output,
	int64_t time, uint32_t seq);
void presentation_feedbacks_discard(struct wl_list *feedbacks);

void xdg_surface_destroy(struct xdg_surface *xdg);
void xdg_toplevel_commit(struct xdg_toplevel *toplevel,
	const struct surface_state *state);
void xdg_popup_destroy(struct xdg_popup *popup);
void xdg_popup_set_parent(struct xdg_popup *popup, struct surface *parent);
void xdg_popup_dismiss(struct xdg_popup *popup);
bool xdg_popup_validate(struct xdg_popup *popup);
void xdg_popup_commit(struct xdg_popup *popup, const struct surface_state *state);
bool xdg_popup_handle_press(struct surface *surface);
void xdg_popups_update_positions(struct surface *parent);
void xdg_popups_dismiss(struct surface *parent);
void xdg_popups_destroy(struct surface *parent);

void xdg_toplevel_decoration_configure(struct xdg_toplevel *toplevel);
bool xdg_toplevel_decoration_outdated(struct xdg_toplevel *toplevel);
void xdg_toplevel_decoration_toplevel_destroy(struct xdg_toplevel *toplevel);
void xdg_dialog_toplevel_destroy(struct xdg_toplevel *toplevel);
bool xdg_toplevel_decoration_allows_buffer(struct surface *surface);
bool server_decoration_surface_ssd(struct surface *surface);
void server_decoration_surface_destroy(struct surface *surface);

void layer_surface_destroy(struct layer_surface *layer_surface);
void layer_surfaces_close(struct output *output);
void output_arrange_layer_surfaces(struct output *output);
bool layer_surface_validate(struct layer_surface *layer_surface);
void layer_surface_commit(struct layer_surface *layer_surface,
	const struct surface_state *state);
struct surface *layer_surface_exclusive_focus(void);

void input_panel_surface_destroy(struct input_panel_surface *panel);
void input_panel_surface_update(struct input_panel_surface *panel);
void input_panels_update(void);

// Covers an output while the session is locked, for the client that locked it.
struct lock_surface {
	struct surface *surface;
	struct wl_resource *resource;
	struct output *output;
	// The client may commit buffers once it acks a configure.
	bool acked;
	int configured_width;
	int configured_height;
};

void lock_surface_destroy(struct lock_surface *lock_surface);
bool lock_surface_validate(struct lock_surface *lock_surface);
void lock_surface_commit(struct lock_surface *lock_surface);
void output_arrange_lock_surface(struct output *output);
struct surface *session_lock_focus(struct surface *surface);

void idle_inhibitors_surface_destroy(struct surface *surface);
void pointer_constraint_surface_destroy(struct surface *surface);

void xwayland_surface_commit(struct xwayland_surface *xwayland_surface,
	const struct surface_state *state);
void xwayland_surface_destroy(struct xwayland_surface *xwayland_surface);
struct window *xwayland_surface_window(struct xwayland_surface *xwayland_surface);

#endif
