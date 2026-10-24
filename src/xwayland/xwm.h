#ifndef JIMI_XWAYLAND_XWM_H
#define JIMI_XWAYLAND_XWM_H

#include <xcb/xcb.h>

#include "core.h"

enum atom {
	ATOM_UTF8_STRING,
	ATOM_WM_PROTOCOLS,
	ATOM_WM_DELETE_WINDOW,
	ATOM_WM_TAKE_FOCUS,
	ATOM_WM_STATE,
	ATOM_WM_CHANGE_STATE,
	ATOM_WM_S0,
	ATOM_NET_WM_CM_S0,
	ATOM_NET_SUPPORTED,
	ATOM_NET_SUPPORTING_WM_CHECK,
	ATOM_NET_ACTIVE_WINDOW,
	ATOM_NET_WM_NAME,
	ATOM_NET_WM_ICON,
	ATOM_NET_WM_STATE,
	ATOM_NET_WM_STATE_MAXIMIZED_VERT,
	ATOM_NET_WM_STATE_MAXIMIZED_HORZ,
	ATOM_NET_WM_STATE_FULLSCREEN,
	ATOM_NET_WM_STATE_FOCUSED,
	ATOM_NET_WM_STATE_MODAL,
	ATOM_NET_WM_MOVERESIZE,
	ATOM_MOTIF_WM_HINTS,
	ATOM_WL_SURFACE_SERIAL,
	ATOM_CLIPBOARD,
	ATOM_TARGETS,
	ATOM_INCR,
	ATOM_TEXT,
	ATOM_WL_SELECTION,
	ATOM_XDND_SELECTION,
	ATOM_XDND_AWARE,
	ATOM_XDND_TYPE_LIST,
	ATOM_XDND_ENTER,
	ATOM_XDND_POSITION,
	ATOM_XDND_STATUS,
	ATOM_XDND_LEAVE,
	ATOM_XDND_DROP,
	ATOM_XDND_FINISHED,
	ATOM_XDND_ACTION_COPY,
	ATOM_XDND_ACTION_MOVE,
	ATOM_XDND_ACTION_ASK,
	ATOM_XDND_ACTION_PRIVATE,
	NR_ATOMS,
};

// The X11 selection bridged with the seat selection of the same type.
struct xwm_selection {
	xcb_atom_t atom; // CLIPBOARD or PRIMARY
	xcb_window_t owner; // Reported by XFixes
	// When xwm.window took the ownership for the seat selection.
	xcb_timestamp_t timestamp;
};

struct xwm {
	xcb_connection_t *xcb;
	xcb_screen_t *screen;
	xcb_atom_t atoms[NR_ATOMS];
	// Owns the window manager selections, and the selections and the drags
	// of Wayland clients. It also takes the drags of X11 clients outside of
	// X11 windows, mapped over them.
	xcb_window_t window;
	xcb_window_t focus; // The window with the X11 input focus.
	xcb_cursor_t cursor;
	struct wl_event_source *source;
	struct wl_list windows; // xwayland_window.link
	uint8_t xfixes_first_event;
	struct xwm_selection selections[NR_SELECTION_TYPES];
	struct wl_list incoming; // incoming_transfer.link
	struct wl_list outgoing; // outgoing_transfer.link
	// A Wayland drag over an X11 window, or dropped on it until the window
	// finishes the drop.
	struct {
		struct data_source *source; // NULL while there is none.
		struct wl_listener source_destroy;
		xcb_window_t target;
		// By XdndStatus
		bool accepted;
		uint32_t action; // enum wl_data_device_manager_dnd_action
		bool dropped;
	} dnd;
	// An X11 drag, which xwm.window takes outside of X11 windows and passes
	// to the Wayland surfaces under the pointer. A drop lasts until the
	// Wayland target finishes it.
	struct {
		struct drag drag; // drag.source is NULL while there is none.
		xcb_window_t owner; // Of XdndSelection, which sends the messages.
		bool window_mapped; // xwm.window
		// In xwm.window, from XdndEnter until XdndLeave or XdndDrop
		bool entered;
		// By the Wayland target, told by XdndStatus
		bool accepted;
		uint32_t action; // enum wl_data_device_manager_dnd_action
		bool dropped;
	} x11_drag;
};

// An X11 window. Override-redirect windows, like menus and tooltips, are shown
// where the client puts them. The others are managed as windows.
struct xwayland_window {
	struct wl_list link; // xwm.windows
	xcb_window_t id;
	struct window window;
	struct xwayland_surface *surface; // NULL until associated
	uint64_t serial; // Of WL_SURFACE_SERIAL, 0 until received.
	struct box geometry; // What the X server has.
	bool override_redirect;
	bool mapped; // In the X server
	bool has_net_wm_name; // _NET_WM_NAME takes precedence over WM_NAME.
	bool accepts_input; // WM_HINTS
	bool has_position; // WM_NORMAL_HINTS
	bool delete_window; // WM_PROTOCOLS
	bool take_focus; // WM_PROTOCOLS
};

// The xwayland_surface_v1 role. Xwayland associates the surface with the X11
// window that sent WL_SURFACE_SERIAL with the same serial.
struct xwayland_surface {
	struct wl_list link; // core.xwayland.surfaces
	struct surface *surface;
	struct wl_resource *resource;
	uint64_t serial; // 0 until committed
	struct xwayland_window *window; // NULL until associated
};

void xwm_send_event(struct xwm *xwm, xcb_window_t destination,
	uint32_t event_mask, const void *event, size_t size);

void xwm_selection_init(struct xwm *xwm);
void xwm_selection_fini(struct xwm *xwm);
bool xwm_handle_selection_event(struct xwm *xwm, xcb_generic_event_t *event);

#endif
