#ifndef JIMI_WINDOW_H
#define JIMI_WINDOW_H

#include "scene.h"
#include "util.h"

struct input_target;
struct output;
struct surface;
struct texture;

// The bits match xdg_toplevel.resize_edge.
enum edge {
	EDGE_NONE = 0,
	EDGE_TOP = 1u << 0,
	EDGE_BOTTOM = 1u << 1,
	EDGE_LEFT = 1u << 2,
	EDGE_RIGHT = 1u << 3,
};

// The workspace of sticky windows, which are shown on all workspaces
#define WORKSPACE_STICKY -1

enum window_type {
	WINDOW_XDG,
	WINDOW_XWAYLAND,
};

enum decoration_part {
	DECORATION_PART_NONE,
	DECORATION_PART_TITLEBAR,
	DECORATION_PART_BORDER, // Grabs past the frame to resize it.
	// The buttons
	DECORATION_PART_CLOSE,
	DECORATION_PART_MAXIMIZE,
	DECORATION_PART_MINIMIZE,
	DECORATION_PART_STICKY,
};

// Laid out at the ends of the titlebar by
// config.decorations.titlebar.layout. The buttons come first, so that they
// index the arrays of the buttons.
enum titlebar_item {
	TITLEBAR_ITEM_STICKY,
	TITLEBAR_ITEM_MINIMIZE,
	TITLEBAR_ITEM_MAXIMIZE,
	TITLEBAR_ITEM_CLOSE,
	NR_TITLEBAR_BUTTONS,
	TITLEBAR_ITEM_APP_ICON = NR_TITLEBAR_BUTTONS,
	NR_TITLEBAR_ITEMS,
};

// The buttons by how they are styled. The maximize button of maximized
// windows is styled as a restore button, and the sticky button of sticky
// windows as an unstick button.
enum button_type {
	BUTTON_STICKY,
	BUTTON_UNSTICK,
	BUTTON_MINIMIZE,
	BUTTON_MAXIMIZE,
	BUTTON_MAXIMIZE_RESTORE,
	BUTTON_CLOSE,
	NR_BUTTON_TYPES,
};

enum button_state {
	BUTTON_STATE_NORMAL,
	BUTTON_STATE_HOVERED,
	BUTTON_STATE_PRESSED,
	NR_BUTTON_STATES,
};

// An item of the window switcher, or its close button, under the pointer.
struct window_switcher_target {
	struct window *window; // Of the item, or NULL.
	bool close;
};

// An icon set by xdg-toplevel-icon or _NET_WM_ICON, shared by its owner and
// the windows it is set on. The xdg-toplevel-icon object becomes immutable
// once set.
struct toplevel_icon {
	int refs;
	bool immutable;
	char *name; // In the icon theme, or NULL.
	// The largest image, copied since the client may destroy it.
	// Premultiplied ARGB8888 without padding, or NULL.
	uint32_t *pixels;
	int width;
	int height;
};

// Laid out around the content of the window. The nodes are enabled while
// they are visible.
struct decoration {
	struct scene_tree tree; // In window.tree, below the surface tree.
	struct scene_shadow shadow; // Of the frame, while it floats.
	struct scene_rect border_top; // Also behind the titlebar.
	struct scene_rect border_left;
	struct scene_rect border_right;
	struct scene_rect border_bottom;
	struct scene_rect titlebar;
	struct scene_texture icon; // Owns its texture.
	double icon_scale; // The scale the icon is rendered at, or 0 if outdated.
	struct scene_texture title; // Owns its texture.
	double title_scale; // The scale the title is rendered at, or 0 if outdated.
	int title_width; // Of the whole text at title_scale, or 0 if not shown.
	bool title_activated; // The activation the title is rendered for.
	// By enum titlebar_item
	struct scene_rect buttons[NR_TITLEBAR_BUTTONS];
	struct scene_texture button_icons[NR_TITLEBAR_BUTTONS]; // output.button_icons
};

struct window_state {
	struct box geometry; // Content box in layout coordinates.
	bool maximized;
	bool fullscreen;
	// The edges of the usable area of the output that a snapped window
	// touches, e.g. EDGE_LEFT | EDGE_TOP | EDGE_BOTTOM for the left half.
	uint32_t tiled; // enum edge
	bool resizing; // By the pointer.
	// The edges the last interactive resize moved. The opposite edges stay
	// in place when the client commits a size, even after the resize ends,
	// as the client may pick another size than configured at its end.
	uint32_t resize_edges; // enum edge
};

struct window_configure {
	struct wl_list link; // window.configures
	uint32_t serial;
	struct window_state state;
};

// The part of an xdg toplevel or a managed X11 window that does not depend
// on its protocol.
struct window {
	enum window_type type;
	struct wl_list link; // core.windows, bottom-to-top
	struct surface *surface; // NULL while an X11 window has no wl_surface.
	// At the top-left corner of current.geometry. Contains the surface tree
	// and the xdg popup trees.
	struct scene_tree tree;
	// The output the window is on, whose scale its surfaces and decoration
	// are drawn at: the one it fills, or else the one nearest to its center
	// when the user or the client last moved it. The window moves along with
	// the output. NULL until the window is placed, and while no output is
	// enabled.
	struct output *output;
	// An index of config.workspaces.names, or WORKSPACE_STICKY. Windows
	// are on the workspace of their parent, so that they move along with
	// it.
	int workspace;
	struct window_state pending; // What the compositor wants.
	struct window_state current; // What the client has committed.
	struct wl_list configures; // window_configure.link, oldest first
	bool configure_scheduled;
	// The floating geometry to return to. Its size is 0x0 while unknown,
	// so that the client picks it, as for windows that start maximized.
	struct box restore_geometry;
	// Where the window was relative to the output it had to leave when the
	// output was disabled. It returns there once the output is enabled
	// again, unless the user moves it before.
	struct {
		char output_name[32]; // Empty unless away
		int x;
		int y;
	} away;
	struct window *parent;
	// A modal dialog takes the keyboard focus from its parent while it is
	// visible, and is left out of the window switcher.
	bool modal;
	char *title;
	char *app_id;
	// Shown instead of the icon of the app ID. NULL if unset.
	struct toplevel_icon *icon;
	int min_width;
	int min_height;
	int max_width;
	int max_height;
	bool mapped;
	bool activated; // Follows keyboard focus history immediately.
	bool minimized; // Hidden until restored. Unmapping clears it.
	bool ssd; // Server-side decorated.
	struct decoration decoration;
	// Handles of the foreign toplevel protocols, which list mapped windows.
	struct {
		struct wl_list link; // core.foreign_toplevel.windows
		struct wl_list ext_handles; // ext_foreign_toplevel_handle_v1 resource links
		struct wl_list wlr_handles; // zwlr_foreign_toplevel_handle_v1 resource links
		uint64_t identifier; // Unique to each mapping.
	} foreign;
	// Image capture of the mapped window.
	struct {
		struct wl_list sources; // capture_source.link
		struct wl_list sessions; // capture_session.link
	} capture;
};

void window_init(struct window *window, enum window_type type);
void window_fini(struct window *window);
void window_place(struct window *window, int width, int height);
void window_map(struct window *window);
void window_unmap(struct window *window);
void window_commit(struct window *window, uint32_t serial, int width, int height);
void window_schedule_configure(struct window *window);
void windows_flush_configures(void);
void windows_arrange(struct output *output);
void windows_update_layout(void);
void window_move(struct window *window, int x, int y);
void window_resize(struct window *window, struct box geometry, uint32_t edges,
	bool resizing);
bool window_state_floating(const struct window_state *state);
bool window_floating(struct window *window);
void window_save_restore_geometry(struct window *window);
void window_snap(struct window *window, struct output *output, uint32_t edges);
void window_unsnap(struct window *window);
struct box window_snap_box(struct output *output, uint32_t edges);
void window_set_maximized(struct window *window, bool maximized);
void window_set_fullscreen(struct window *window, bool fullscreen);
void window_set_minimized(struct window *window, bool minimized);
void window_set_activated(struct window *window, bool activated);
void window_set_ssd(struct window *window, bool ssd);
void window_set_title(struct window *window, const char *title);
void window_set_app_id(struct window *window, const char *app_id);
void window_set_icon(struct window *window, struct toplevel_icon *icon);
struct texture *window_create_icon(struct window *window, int size, double scale);
bool window_has_ancestor(struct window *window, struct window *ancestor);
void window_set_parent(struct window *window, struct window *parent);
bool window_listed(struct window *window);
void window_set_workspace(struct window *window, int workspace);
void window_set_sticky(struct window *window, bool sticky);
bool window_on_shown_workspace(struct window *window);
void window_raise(struct window *window);
void window_focus(struct window *window);
struct window *window_focus_target(struct window *window);
void window_close(struct window *window);
bool window_visible(struct window *window);
void window_update_visibility(struct window *window);
bool window_decorated(struct window *window);
struct box window_frame_box(struct window *window);
struct window *window_from_surface(struct surface *surface);
bool window_at(struct window *window, double x, double y,
	struct input_target *target);

// The shells behind the window types
uint32_t xdg_toplevel_configure(struct window *window);
void xdg_toplevel_close(struct window *window);
uint32_t xwayland_window_configure(struct window *window);
void xwayland_window_close(struct window *window);

void decoration_icons_create(struct output *output);
void decoration_icons_destroy(struct output *output);
void decoration_init(struct window *window);
void decoration_fini(struct window *window);
void decoration_update(struct window *window);
void decoration_redraw(struct window *window);
void decoration_update_title(struct window *window);
void decoration_update_icon(struct window *window);
void decoration_update_buttons(struct window *window);
enum decoration_part decoration_part_at(struct window *window, double x,
	double y, uint32_t *edges);
struct box decoration_frame_box(struct box content, bool maximized);
struct box decoration_content_box(struct box frame, bool maximized);

void window_switcher_init(void);
void window_switcher_step(bool forward);
void window_switcher_finish(void);
void window_switcher_cancel(void);
void window_switcher_remove_window(struct window *window);
void window_switcher_update_thumbnail(struct window *window);
void window_switcher_update_title(struct window *window);
void window_switcher_update_icon(struct window *window);
void window_switcher_update_outline(struct window *window);
void window_switcher_update_hover(void);
void window_switcher_handle_button(uint32_t button, bool pressed);

void icon_themes_init(void);
void icon_themes_fini(void);
char *icon_lookup(const char *name, int size);
char *app_icon_lookup(const char *app_id, int size);

struct toplevel_icon *toplevel_icon_create(void);
struct toplevel_icon *toplevel_icon_ref(struct toplevel_icon *icon);
void toplevel_icon_unref(struct toplevel_icon *icon);

void foreign_toplevel_map(struct window *window);
void foreign_toplevel_unmap(struct window *window);
void foreign_toplevel_update_title(struct window *window);
void foreign_toplevel_update_app_id(struct window *window);
void foreign_toplevel_update_state(struct window *window);
void foreign_toplevel_update_parent(struct window *window);
void foreign_toplevel_update_output(struct window *window,
	struct output *old_output);
void foreign_toplevel_output_bind(struct output *output,
	struct wl_resource *resource);

void workspace_show(int workspace);
void workspace_switch(int workspace);
void workspaces_reload(size_t nr_old);
void workspaces_output_bind(struct output *output, struct wl_resource *resource);
void workspaces_output_disable(struct output *output);

#endif
