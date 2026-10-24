#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/composite.h>
#include <xcb/xcb_cursor.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xfixes.h>

#include "surface.h"
#include "xwayland-shell-v1-protocol.h"
#include "xwm.h"

static const char *const atom_names[NR_ATOMS] = {
	[ATOM_UTF8_STRING] = "UTF8_STRING",
	[ATOM_WM_PROTOCOLS] = "WM_PROTOCOLS",
	[ATOM_WM_DELETE_WINDOW] = "WM_DELETE_WINDOW",
	[ATOM_WM_TAKE_FOCUS] = "WM_TAKE_FOCUS",
	[ATOM_WM_STATE] = "WM_STATE",
	[ATOM_WM_CHANGE_STATE] = "WM_CHANGE_STATE",
	[ATOM_WM_S0] = "WM_S0",
	[ATOM_NET_WM_CM_S0] = "_NET_WM_CM_S0",
	[ATOM_NET_SUPPORTED] = "_NET_SUPPORTED",
	[ATOM_NET_SUPPORTING_WM_CHECK] = "_NET_SUPPORTING_WM_CHECK",
	[ATOM_NET_ACTIVE_WINDOW] = "_NET_ACTIVE_WINDOW",
	[ATOM_NET_WM_NAME] = "_NET_WM_NAME",
	[ATOM_NET_WM_ICON] = "_NET_WM_ICON",
	[ATOM_NET_WM_STATE] = "_NET_WM_STATE",
	[ATOM_NET_WM_STATE_MAXIMIZED_VERT] = "_NET_WM_STATE_MAXIMIZED_VERT",
	[ATOM_NET_WM_STATE_MAXIMIZED_HORZ] = "_NET_WM_STATE_MAXIMIZED_HORZ",
	[ATOM_NET_WM_STATE_FULLSCREEN] = "_NET_WM_STATE_FULLSCREEN",
	[ATOM_NET_WM_STATE_FOCUSED] = "_NET_WM_STATE_FOCUSED",
	[ATOM_NET_WM_STATE_MODAL] = "_NET_WM_STATE_MODAL",
	[ATOM_NET_WM_MOVERESIZE] = "_NET_WM_MOVERESIZE",
	[ATOM_MOTIF_WM_HINTS] = "_MOTIF_WM_HINTS",
	[ATOM_WL_SURFACE_SERIAL] = "WL_SURFACE_SERIAL",
	[ATOM_CLIPBOARD] = "CLIPBOARD",
	[ATOM_TARGETS] = "TARGETS",
	[ATOM_INCR] = "INCR",
	[ATOM_TEXT] = "TEXT",
	[ATOM_WL_SELECTION] = "WL_SELECTION",
	[ATOM_XDND_SELECTION] = "XdndSelection",
	[ATOM_XDND_AWARE] = "XdndAware",
	[ATOM_XDND_TYPE_LIST] = "XdndTypeList",
	[ATOM_XDND_ENTER] = "XdndEnter",
	[ATOM_XDND_POSITION] = "XdndPosition",
	[ATOM_XDND_STATUS] = "XdndStatus",
	[ATOM_XDND_LEAVE] = "XdndLeave",
	[ATOM_XDND_DROP] = "XdndDrop",
	[ATOM_XDND_FINISHED] = "XdndFinished",
	[ATOM_XDND_ACTION_COPY] = "XdndActionCopy",
	[ATOM_XDND_ACTION_MOVE] = "XdndActionMove",
	[ATOM_XDND_ACTION_ASK] = "XdndActionAsk",
	[ATOM_XDND_ACTION_PRIVATE] = "XdndActionPrivate",
};

// _NET_WM_MOVERESIZE directions
#define MOVERESIZE_SIZE_LEFT 7
#define MOVERESIZE_MOVE 8

// _MOTIF_WM_HINTS
#define MOTIF_HINTS_DECORATIONS (1u << 1)

// Wider or taller _NET_WM_ICON images are ignored, since cairo cannot draw
// them.
#define MAX_NET_WM_ICON_SIZE 32767

// _NET_WM_STATE actions besides removing
#define NET_WM_STATE_ADD 1
#define NET_WM_STATE_TOGGLE 2

static struct xwayland_window *
xwayland_window_from_id(struct xwm *xwm, xcb_window_t id)
{
	struct xwayland_window *xw;
	wl_list_for_each(xw, &xwm->windows, link) {
		if (xw->id == id) {
			return xw;
		}
	}
	return NULL;
}

// Only the 32 bytes of an X11 event are sent.
void
xwm_send_event(struct xwm *xwm, xcb_window_t destination, uint32_t event_mask,
	const void *event, size_t size)
{
	char buf[32] = {0};
	assert(size <= sizeof(buf));
	memcpy(buf, event, size);
	xcb_send_event(xwm->xcb, 0, destination, event_mask, buf);
}

static void
send_wm_message(struct xwayland_window *xw, enum atom protocol)
{
	struct xwm *xwm = core.xwayland.xwm;
	xcb_client_message_event_t event = {
		.response_type = XCB_CLIENT_MESSAGE,
		.format = 32,
		.window = xw->id,
		.type = xwm->atoms[ATOM_WM_PROTOCOLS],
		.data.data32 = {xwm->atoms[protocol], XCB_CURRENT_TIME},
	};
	xwm_send_event(xwm, xw->id, XCB_EVENT_MASK_NO_EVENT, &event, sizeof(event));
}

static void
set_wm_state(struct xwayland_window *xw, uint32_t state)
{
	struct xwm *xwm = core.xwayland.xwm;
	uint32_t property[] = {state, XCB_WINDOW_NONE};
	xcb_change_property(xwm->xcb, XCB_PROP_MODE_REPLACE, xw->id,
		xwm->atoms[ATOM_WM_STATE], xwm->atoms[ATOM_WM_STATE], 32,
		ARRAY_LEN(property), property);
}

static void
set_net_wm_state(struct xwayland_window *xw)
{
	struct xwm *xwm = core.xwayland.xwm;
	struct window_state *state = &xw->window.pending;
	xcb_atom_t property[5];
	uint32_t length = 0;
	if (state->maximized) {
		property[length++] = xwm->atoms[ATOM_NET_WM_STATE_MAXIMIZED_VERT];
		property[length++] = xwm->atoms[ATOM_NET_WM_STATE_MAXIMIZED_HORZ];
	}
	if (state->fullscreen) {
		property[length++] = xwm->atoms[ATOM_NET_WM_STATE_FULLSCREEN];
	}
	if (xw->window.activated) {
		property[length++] = xwm->atoms[ATOM_NET_WM_STATE_FOCUSED];
	}
	if (xw->window.modal) {
		property[length++] = xwm->atoms[ATOM_NET_WM_STATE_MODAL];
	}
	xcb_change_property(xwm->xcb, XCB_PROP_MODE_REPLACE, xw->id,
		xwm->atoms[ATOM_NET_WM_STATE], XCB_ATOM_ATOM, 32, length, property);
}

// Gives the X11 input focus to the window, or takes it from all windows.
static void
xwm_focus(struct xwm *xwm, struct xwayland_window *xw)
{
	xwm->focus = xw ? xw->id : XCB_WINDOW_NONE;
	xcb_change_property(xwm->xcb, XCB_PROP_MODE_REPLACE, xwm->screen->root,
		xwm->atoms[ATOM_NET_ACTIVE_WINDOW], XCB_ATOM_WINDOW, 32, 1, &xwm->focus);
	if (!xw) {
		// PointerRoot, which xcb does not define. Unlike None, it keeps
		// the keyboard working for grabs.
		xcb_set_input_focus(xwm->xcb, XCB_INPUT_FOCUS_POINTER_ROOT, 1,
			XCB_CURRENT_TIME);
		return;
	}
	if (xw->take_focus) {
		send_wm_message(xw, ATOM_WM_TAKE_FOCUS);
	}
	if (xw->accepts_input) {
		xcb_set_input_focus(xwm->xcb, XCB_INPUT_FOCUS_POINTER_ROOT, xw->id,
			XCB_CURRENT_TIME);
	}
	// X11 delivers input to the top-most window under the pointer, so the
	// focused window is stacked on top as it is in the scene.
	uint32_t stack_mode = XCB_STACK_MODE_ABOVE;
	xcb_configure_window(xwm->xcb, xw->id, XCB_CONFIG_WINDOW_STACK_MODE,
		&stack_mode);
}

// X11 clients ack nothing, so the state applies right away.
uint32_t
xwayland_window_configure(struct window *window)
{
	assert(window->type == WINDOW_XWAYLAND);
	struct xwayland_window *xw = wl_container_of(window, xw, window);
	struct xwm *xwm = core.xwayland.xwm;
	struct box geometry = window->pending.geometry;
	uint32_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y
		| XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT
		| XCB_CONFIG_WINDOW_BORDER_WIDTH;
	uint32_t values[] = {
		(uint32_t)geometry.x,
		(uint32_t)geometry.y,
		(uint32_t)geometry.width,
		(uint32_t)geometry.height,
		0,
	};
	xcb_configure_window(xwm->xcb, xw->id, mask, values);
	// Without a size change, the client expects a synthetic ConfigureNotify
	// in response to its request (ICCCM 4.1.5).
	if (geometry.width == xw->geometry.width
			&& geometry.height == xw->geometry.height) {
		xcb_configure_notify_event_t event = {
			.response_type = XCB_CONFIGURE_NOTIFY,
			.event = xw->id,
			.window = xw->id,
			.x = (int16_t)geometry.x,
			.y = (int16_t)geometry.y,
			.width = (uint16_t)geometry.width,
			.height = (uint16_t)geometry.height,
		};
		xwm_send_event(xwm, xw->id, XCB_EVENT_MASK_STRUCTURE_NOTIFY, &event,
			sizeof(event));
	}
	xw->geometry = geometry;

	set_net_wm_state(xw);
	if (window->activated && xwm->focus != xw->id) {
		xwm_focus(xwm, xw);
	} else if (!window->activated && xwm->focus == xw->id) {
		xwm_focus(xwm, NULL);
	}
	xcb_flush(xwm->xcb);
	return 0;
}

void
xwayland_window_close(struct window *window)
{
	assert(window->type == WINDOW_XWAYLAND);
	struct xwayland_window *xw = wl_container_of(window, xw, window);
	struct xwm *xwm = core.xwayland.xwm;
	if (xw->delete_window) {
		send_wm_message(xw, ATOM_WM_DELETE_WINDOW);
	} else {
		xcb_kill_client(xwm->xcb, xw->id);
	}
	xcb_flush(xwm->xcb);
}

static void
xwayland_window_hide(struct xwayland_window *xw)
{
	if (!xw->override_redirect) {
		if (xw->window.mapped) {
			window_unmap(&xw->window);
		}
	} else if (xw->surface) {
		surface_set_mapped(xw->surface->surface, false);
	}
}

// Shows the window while it is mapped in the X server and its surface has a
// buffer.
static void
xwayland_window_update(struct xwayland_window *xw)
{
	struct surface *surface = xw->surface ? xw->surface->surface : NULL;
	if (!xw->mapped || !surface || !surface->current.buffer) {
		xwayland_window_hide(xw);
		return;
	}
	struct scene_node *node = &surface->scene.tree.node;
	if (xw->override_redirect) {
		scene_node_reparent(node, &core.scene.layers[SCENE_LAYER_UNMANAGED]);
		scene_node_set_position(node, xw->geometry.x, xw->geometry.y);
		surface_set_mapped(surface, true);
		return;
	}
	struct window *window = &xw->window;
	scene_node_reparent(node, &window->tree);
	scene_node_set_position(node, 0, 0);
	window_commit(window, 0, surface->width, surface->height);
	if (!window->mapped) {
		window_map(window);
	}
}

static void
xwayland_window_associate(struct xwayland_window *xw, struct xwayland_surface *xs)
{
	assert(!xw->surface && !xs->window);
	xw->surface = xs;
	xs->window = xw;
	xw->window.surface = xs->surface;
	xwayland_window_update(xw);
}

// Xwayland creates a new wl_surface each time the window is mapped.
static void
xwayland_window_dissociate(struct xwayland_window *xw)
{
	struct xwayland_surface *xs = xw->surface;
	if (!xs) {
		return;
	}
	xwayland_window_hide(xw);
	scene_node_detach(&xs->surface->scene.tree.node);
	xs->window = NULL;
	xw->surface = NULL;
	xw->serial = 0;
	xw->window.surface = NULL;
}

static void
xwayland_window_create(struct xwm *xwm, xcb_create_notify_event_t *event)
{
	struct xwayland_window *xw = calloc(1, sizeof(*xw));
	assert(xw);
	xw->id = event->window;
	xw->geometry = (struct box){
		.x = event->x,
		.y = event->y,
		.width = event->width,
		.height = event->height,
	};
	xw->override_redirect = event->override_redirect;
	xw->accepts_input = true;
	window_init(&xw->window, WINDOW_XWAYLAND);
	xw->window.pending.geometry = xw->geometry;
	list_append(&xwm->windows, &xw->link);

	uint32_t event_mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
	xcb_change_window_attributes(xwm->xcb, xw->id, XCB_CW_EVENT_MASK, &event_mask);
}

static void
xwayland_window_destroy(struct xwayland_window *xw)
{
	struct xwm *xwm = core.xwayland.xwm;
	xwayland_window_dissociate(xw);
	if (xwm->focus == xw->id) {
		xwm->focus = XCB_WINDOW_NONE;
	}
	window_fini(&xw->window);
	wl_list_remove(&xw->link);
	free(xw);
}

static void
read_title(struct xwayland_window *xw, const char *value, int length)
{
	char *title = strndup(value, (size_t)length);
	assert(title);
	window_set_title(&xw->window, title);
	free(title);
}

// WM_CLASS holds the instance and the class name, which is the app ID.
static void
read_class(struct xwayland_window *xw, const char *value, int length)
{
	size_t instance_length = strnlen(value, (size_t)length);
	if (instance_length + 1 >= (size_t)length) {
		return;
	}
	char *class = strndup(value + instance_length + 1,
		(size_t)length - instance_length - 1);
	assert(class);
	window_set_app_id(&xw->window, class);
	free(class);
}

// Premultiplies the alpha of an ARGB8888 pixel.
static uint32_t
premultiply(uint32_t pixel)
{
	uint32_t alpha = pixel >> 24;
	uint32_t result = alpha << 24;
	for (int shift = 0; shift < 24; shift += 8) {
		uint32_t channel = (pixel >> shift & 0xff) * alpha / 0xff;
		result |= channel << shift;
	}
	return result;
}

// _NET_WM_ICON holds images of any sizes one after another, each as its width,
// its height and its non-premultiplied ARGB8888 pixels by rows. Only the
// largest is kept, which is scaled down to be drawn.
static void
read_icon(struct xwayland_window *xw, xcb_get_property_reply_t *reply)
{
	const uint32_t *data = xcb_get_property_value(reply);
	size_t length = reply->type == XCB_ATOM_CARDINAL && reply->format == 32
		? (size_t)xcb_get_property_value_length(reply) / sizeof(uint32_t)
		: 0;
	const uint32_t *largest = NULL;
	uint64_t largest_area = 0;
	for (size_t i = 0; length - i >= 2;) {
		uint32_t width = data[i];
		uint32_t height = data[i + 1];
		uint64_t area = (uint64_t)width * height;
		if (area > length - i - 2) {
			break;
		}
		if (area > largest_area && width <= MAX_NET_WM_ICON_SIZE
				&& height <= MAX_NET_WM_ICON_SIZE) {
			largest = &data[i];
			largest_area = area;
		}
		i += 2 + area;
	}
	if (!largest) {
		window_set_icon(&xw->window, NULL);
		return;
	}

	struct toplevel_icon *icon = toplevel_icon_create();
	icon->width = (int)largest[0];
	icon->height = (int)largest[1];
	icon->pixels = malloc(largest_area * sizeof(uint32_t));
	assert(icon->pixels);
	for (uint64_t i = 0; i < largest_area; i++) {
		icon->pixels[i] = premultiply(largest[2 + i]);
	}
	window_set_icon(&xw->window, icon);
	toplevel_icon_unref(icon);
}

// Transient windows, like dialogs, stay above their parent.
static void
read_transient_for(struct xwayland_window *xw, xcb_get_property_reply_t *reply)
{
	struct xwm *xwm = core.xwayland.xwm;
	struct xwayland_window *parent = NULL;
	if (reply->type == XCB_ATOM_WINDOW
			&& xcb_get_property_value_length(reply)
				>= (int)sizeof(xcb_window_t)) {
		xcb_window_t *id = xcb_get_property_value(reply);
		parent = xwayland_window_from_id(xwm, *id);
	}
	struct window *window = &xw->window;
	struct window *parent_window = parent && !parent->override_redirect
		? &parent->window : NULL;
	if (!window_has_ancestor(parent_window, window)) {
		window_set_parent(window, parent_window);
	}
}

static void
read_hints(struct xwayland_window *xw, xcb_get_property_reply_t *reply)
{
	xcb_icccm_wm_hints_t hints;
	xw->accepts_input = !xcb_icccm_get_wm_hints_from_reply(&hints, reply)
		|| !(hints.flags & XCB_ICCCM_WM_HINT_INPUT) || hints.input;
}

static void
read_normal_hints(struct xwayland_window *xw, xcb_get_property_reply_t *reply)
{
	struct window *window = &xw->window;
	xcb_size_hints_t hints;
	if (!xcb_icccm_get_wm_size_hints_from_reply(&hints, reply)) {
		hints.flags = 0;
	}
	xw->has_position = hints.flags & (XCB_ICCCM_SIZE_HINT_US_POSITION
		| XCB_ICCCM_SIZE_HINT_P_POSITION);
	bool min_size = hints.flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE;
	bool max_size = hints.flags & XCB_ICCCM_SIZE_HINT_P_MAX_SIZE;
	window->min_width = min_size ? hints.min_width : 0;
	window->min_height = min_size ? hints.min_height : 0;
	window->max_width = max_size ? hints.max_width : 0;
	window->max_height = max_size ? hints.max_height : 0;
}

static void
read_protocols(struct xwayland_window *xw, xcb_get_property_reply_t *reply)
{
	struct xwm *xwm = core.xwayland.xwm;
	xw->delete_window = false;
	xw->take_focus = false;
	if (reply->type != XCB_ATOM_ATOM) {
		return;
	}
	xcb_atom_t *atoms = xcb_get_property_value(reply);
	int length = xcb_get_property_value_length(reply) / (int)sizeof(xcb_atom_t);
	for (int i = 0; i < length; i++) {
		if (atoms[i] == xwm->atoms[ATOM_WM_DELETE_WINDOW]) {
			xw->delete_window = true;
		} else if (atoms[i] == xwm->atoms[ATOM_WM_TAKE_FOCUS]) {
			xw->take_focus = true;
		}
	}
}

// Clients without decorations of their own keep the server-side ones.
static void
read_motif_hints(struct xwayland_window *xw, xcb_get_property_reply_t *reply)
{
	// flags, functions, decorations, input mode and status
	uint32_t *hints = xcb_get_property_value(reply);
	bool ssd = true;
	if (xcb_get_property_value_length(reply) >= 5 * (int)sizeof(uint32_t)
			&& (hints[0] & MOTIF_HINTS_DECORATIONS)) {
		ssd = hints[2] != 0;
	}
	window_set_ssd(&xw->window, ssd);
}

// The state the client asks for before it maps the window. Later requests
// are client messages.
static void
read_net_wm_state(struct xwayland_window *xw, xcb_get_property_reply_t *reply)
{
	struct xwm *xwm = core.xwayland.xwm;
	xcb_atom_t *atoms = xcb_get_property_value(reply);
	int length = reply->type == XCB_ATOM_ATOM
		? xcb_get_property_value_length(reply) / (int)sizeof(xcb_atom_t) : 0;
	bool maximized_vert = false;
	bool maximized_horz = false;
	bool modal = false;
	for (int i = 0; i < length; i++) {
		if (atoms[i] == xwm->atoms[ATOM_NET_WM_STATE_FULLSCREEN]) {
			window_set_fullscreen(&xw->window, true);
		} else if (atoms[i] == xwm->atoms[ATOM_NET_WM_STATE_MAXIMIZED_VERT]) {
			maximized_vert = true;
		} else if (atoms[i] == xwm->atoms[ATOM_NET_WM_STATE_MAXIMIZED_HORZ]) {
			maximized_horz = true;
		} else if (atoms[i] == xwm->atoms[ATOM_NET_WM_STATE_MODAL]) {
			modal = true;
		}
	}
	if (maximized_vert && maximized_horz) {
		window_set_maximized(&xw->window, true);
	}
	// Set even without the property, since the client may have dropped it
	// while the window was withdrawn.
	xw->window.modal = modal;
}

static void
read_property(struct xwayland_window *xw, xcb_atom_t atom,
	xcb_get_property_reply_t *reply)
{
	struct xwm *xwm = core.xwayland.xwm;
	const char *value = xcb_get_property_value(reply);
	int length = xcb_get_property_value_length(reply);
	if (atom == xwm->atoms[ATOM_NET_WM_NAME]) {
		xw->has_net_wm_name = length > 0;
		read_title(xw, value, length);
	} else if (atom == XCB_ATOM_WM_NAME && !xw->has_net_wm_name) {
		read_title(xw, value, length);
	} else if (atom == XCB_ATOM_WM_CLASS) {
		read_class(xw, value, length);
	} else if (atom == xwm->atoms[ATOM_NET_WM_ICON]) {
		read_icon(xw, reply);
	} else if (atom == XCB_ATOM_WM_TRANSIENT_FOR) {
		read_transient_for(xw, reply);
	} else if (atom == XCB_ATOM_WM_HINTS) {
		read_hints(xw, reply);
	} else if (atom == XCB_ATOM_WM_NORMAL_HINTS) {
		read_normal_hints(xw, reply);
	} else if (atom == xwm->atoms[ATOM_WM_PROTOCOLS]) {
		read_protocols(xw, reply);
	} else if (atom == xwm->atoms[ATOM_MOTIF_WM_HINTS]) {
		read_motif_hints(xw, reply);
	} else if (atom == xwm->atoms[ATOM_NET_WM_STATE]) {
		read_net_wm_state(xw, reply);
	}
}

// _NET_WM_ICON is read whole, as it holds images.
static xcb_get_property_cookie_t
get_property(struct xwm *xwm, xcb_window_t window, xcb_atom_t atom)
{
	uint32_t length = atom == xwm->atoms[ATOM_NET_WM_ICON] ? UINT32_MAX : 256;
	return xcb_get_property(xwm->xcb, 0, window, atom, XCB_ATOM_ANY, 0, length);
}

static void
read_properties(struct xwm *xwm, struct xwayland_window *xw)
{
	const xcb_atom_t atoms[] = {
		xwm->atoms[ATOM_NET_WM_NAME],
		XCB_ATOM_WM_NAME,
		XCB_ATOM_WM_CLASS,
		xwm->atoms[ATOM_NET_WM_ICON],
		XCB_ATOM_WM_TRANSIENT_FOR,
		XCB_ATOM_WM_HINTS,
		XCB_ATOM_WM_NORMAL_HINTS,
		xwm->atoms[ATOM_WM_PROTOCOLS],
		xwm->atoms[ATOM_MOTIF_WM_HINTS],
		xwm->atoms[ATOM_NET_WM_STATE],
	};
	xcb_get_property_cookie_t cookies[ARRAY_LEN(atoms)];
	for (size_t i = 0; i < ARRAY_LEN(atoms); i++) {
		cookies[i] = get_property(xwm, xw->id, atoms[i]);
	}
	for (size_t i = 0; i < ARRAY_LEN(atoms); i++) {
		xcb_get_property_reply_t *reply =
			xcb_get_property_reply(xwm->xcb, cookies[i], NULL);
		if (reply) {
			read_property(xw, atoms[i], reply);
			free(reply);
		}
	}
}

static void
handle_create_notify(struct xwm *xwm, xcb_create_notify_event_t *event)
{
	if (event->window != xwm->window) {
		xwayland_window_create(xwm, event);
	}
}

static void
handle_destroy_notify(struct xwm *xwm, xcb_destroy_notify_event_t *event)
{
	struct xwayland_window *xw = xwayland_window_from_id(xwm, event->window);
	if (xw) {
		xwayland_window_destroy(xw);
	}
}

// Maximized and fullscreen windows keep their geometry, which the configure
// tells the client.
static void
handle_configure_request(struct xwm *xwm, xcb_configure_request_event_t *event)
{
	struct xwayland_window *xw = xwayland_window_from_id(xwm, event->window);
	if (!xw) {
		return;
	}
	struct window *window = &xw->window;
	struct box *geometry = &window->pending.geometry;
	if (window_floating(window)) {
		if (event->value_mask & XCB_CONFIG_WINDOW_X) {
			geometry->x = event->x;
		}
		if (event->value_mask & XCB_CONFIG_WINDOW_Y) {
			geometry->y = event->y;
		}
		if (event->value_mask & XCB_CONFIG_WINDOW_WIDTH) {
			geometry->width = event->width;
		}
		if (event->value_mask & XCB_CONFIG_WINDOW_HEIGHT) {
			geometry->height = event->height;
		}
	}
	window_schedule_configure(window);
}

static void
handle_configure_notify(struct xwm *xwm, xcb_configure_notify_event_t *event)
{
	struct xwayland_window *xw = xwayland_window_from_id(xwm, event->window);
	if (!xw) {
		return;
	}
	xw->geometry = (struct box){
		.x = event->x,
		.y = event->y,
		.width = event->width,
		.height = event->height,
	};
	if (xw->override_redirect && xw->surface) {
		scene_node_set_position(&xw->surface->surface->scene.tree.node,
			event->x, event->y);
	}
}

// Windows without a position of their own are placed when first mapped.
static void
handle_map_request(struct xwm *xwm, xcb_map_request_event_t *event)
{
	struct xwayland_window *xw = xwayland_window_from_id(xwm, event->window);
	if (!xw) {
		return;
	}
	read_properties(xwm, xw);
	struct window *window = &xw->window;
	if (!xw->has_position && box_empty(&window->current.geometry)) {
		window_place(window, window->pending.geometry.width,
			window->pending.geometry.height);
	}
	window_schedule_configure(window);
	set_wm_state(xw, XCB_ICCCM_WM_STATE_NORMAL);
	xcb_map_window(xwm->xcb, xw->id);
}

static void
handle_map_notify(struct xwm *xwm, xcb_map_notify_event_t *event)
{
	struct xwayland_window *xw = xwayland_window_from_id(xwm, event->window);
	if (!xw) {
		return;
	}
	xw->mapped = true;
	xw->override_redirect = event->override_redirect;
	xwayland_window_update(xw);
}

static void
handle_unmap_notify(struct xwm *xwm, xcb_unmap_notify_event_t *event)
{
	struct xwayland_window *xw = xwayland_window_from_id(xwm, event->window);
	if (!xw) {
		return;
	}
	xw->mapped = false;
	xwayland_window_dissociate(xw);
	if (!xw->override_redirect) {
		set_wm_state(xw, XCB_ICCCM_WM_STATE_WITHDRAWN);
	}
}

// The window manager sets _NET_WM_STATE itself, so only its initial value
// is read.
static void
handle_property_notify(struct xwm *xwm, xcb_property_notify_event_t *event)
{
	struct xwayland_window *xw = xwayland_window_from_id(xwm, event->window);
	if (!xw || event->atom == xwm->atoms[ATOM_NET_WM_STATE]) {
		return;
	}
	xcb_get_property_reply_t *reply = xcb_get_property_reply(xwm->xcb,
		get_property(xwm, xw->id, event->atom), NULL);
	if (reply) {
		read_property(xw, event->atom, reply);
		free(reply);
	}
}

static void
handle_surface_serial(struct xwayland_window *xw, uint64_t serial)
{
	if (xw->serial) {
		return;
	}
	xw->serial = serial;
	struct xwayland_surface *xs;
	wl_list_for_each(xs, &core.xwayland.surfaces, link) {
		if (xs->serial == serial && !xs->window) {
			xwayland_window_associate(xw, xs);
			return;
		}
	}
}

static bool
net_wm_state_apply(uint32_t action, bool value)
{
	return action == NET_WM_STATE_TOGGLE ? !value : action == NET_WM_STATE_ADD;
}

// Requests to add, remove or toggle up to two states.
static void
handle_net_wm_state(struct xwm *xwm, struct xwayland_window *xw,
	const uint32_t data[5])
{
	struct window *window = &xw->window;
	bool maximized = false;
	bool fullscreen = false;
	bool modal = false;
	for (int i = 1; i <= 2; i++) {
		if (data[i] == xwm->atoms[ATOM_NET_WM_STATE_FULLSCREEN]) {
			fullscreen = true;
		} else if (data[i] == xwm->atoms[ATOM_NET_WM_STATE_MODAL]) {
			modal = true;
		} else if (data[i] == xwm->atoms[ATOM_NET_WM_STATE_MAXIMIZED_VERT]
				|| data[i] == xwm->atoms[ATOM_NET_WM_STATE_MAXIMIZED_HORZ]) {
			maximized = true;
		}
	}
	if (fullscreen) {
		window_set_fullscreen(window,
			net_wm_state_apply(data[0], window->pending.fullscreen));
	}
	if (maximized) {
		window_set_maximized(window,
			net_wm_state_apply(data[0], window->pending.maximized));
	}
	// Modality is not configured, so it is reported right away.
	if (modal) {
		window->modal = net_wm_state_apply(data[0], window->modal);
		set_net_wm_state(xw);
	}
}

// Moves and resizes follow the pointer while the press on the window is held.
static void
handle_net_wm_moveresize(struct xwayland_window *xw, uint32_t direction)
{
	static const uint32_t edges[] = {
		EDGE_TOP | EDGE_LEFT,
		EDGE_TOP,
		EDGE_TOP | EDGE_RIGHT,
		EDGE_RIGHT,
		EDGE_BOTTOM | EDGE_RIGHT,
		EDGE_BOTTOM,
		EDGE_BOTTOM | EDGE_LEFT,
		EDGE_LEFT,
	};
	static_assert(ARRAY_LEN(edges) == MOVERESIZE_SIZE_LEFT + 1,
		"resize directions precede the move");
	if (!xw->surface || xw->override_redirect) {
		return;
	}
	uint32_t serial = input_press_serial(xw->surface->surface);
	if (direction == MOVERESIZE_MOVE) {
		pointer_request_move(&xw->window, serial);
	} else if (direction < ARRAY_LEN(edges)) {
		pointer_request_resize(&xw->window, serial, edges[direction]);
	}
}

static void
handle_client_message(struct xwm *xwm, xcb_client_message_event_t *event)
{
	struct xwayland_window *xw = xwayland_window_from_id(xwm, event->window);
	if (!xw) {
		return;
	}
	const uint32_t *data = event->data.data32;
	if (event->type == xwm->atoms[ATOM_WL_SURFACE_SERIAL]) {
		handle_surface_serial(xw, (uint64_t)data[1] << 32 | data[0]);
	} else if (event->type == xwm->atoms[ATOM_NET_WM_STATE]) {
		handle_net_wm_state(xwm, xw, data);
	} else if (event->type == xwm->atoms[ATOM_NET_WM_MOVERESIZE]) {
		handle_net_wm_moveresize(xw, data[2]);
	} else if (event->type == xwm->atoms[ATOM_WM_CHANGE_STATE]
			&& data[0] == XCB_ICCCM_WM_STATE_ICONIC) {
		window_set_minimized(&xw->window, true);
	}
}

static void
handle_event(struct xwm *xwm, xcb_generic_event_t *event)
{
	if (xwm_handle_selection_event(xwm, event)) {
		return;
	}
	switch (event->response_type & ~0x80) {
	case XCB_CREATE_NOTIFY:
		handle_create_notify(xwm, (xcb_create_notify_event_t *)event);
		break;
	case XCB_DESTROY_NOTIFY:
		handle_destroy_notify(xwm, (xcb_destroy_notify_event_t *)event);
		break;
	case XCB_CONFIGURE_REQUEST:
		handle_configure_request(xwm, (xcb_configure_request_event_t *)event);
		break;
	case XCB_CONFIGURE_NOTIFY:
		handle_configure_notify(xwm, (xcb_configure_notify_event_t *)event);
		break;
	case XCB_MAP_REQUEST:
		handle_map_request(xwm, (xcb_map_request_event_t *)event);
		break;
	case XCB_MAP_NOTIFY:
		handle_map_notify(xwm, (xcb_map_notify_event_t *)event);
		break;
	case XCB_UNMAP_NOTIFY:
		handle_unmap_notify(xwm, (xcb_unmap_notify_event_t *)event);
		break;
	case XCB_PROPERTY_NOTIFY:
		handle_property_notify(xwm, (xcb_property_notify_event_t *)event);
		break;
	case XCB_CLIENT_MESSAGE:
		handle_client_message(xwm, (xcb_client_message_event_t *)event);
		break;
	default:
		// Errors, e.g. for windows destroyed meanwhile, are ignored.
		break;
	}
}

static int
handle_events(int fd, uint32_t mask, void *data)
{
	struct xwm *xwm = core.xwayland.xwm;
	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		// Xwayland has exited, and its Wayland connection goes with it.
		wl_client_destroy(core.xwayland.client);
		return 0;
	}
	int count = 0;
	xcb_generic_event_t *event;
	while ((event = xcb_poll_for_event(xwm->xcb))) {
		handle_event(xwm, event);
		free(event);
		count++;
	}
	xcb_flush(xwm->xcb);
	return count;
}

static void
intern_atoms(struct xwm *xwm)
{
	xcb_intern_atom_cookie_t cookies[NR_ATOMS];
	for (size_t i = 0; i < NR_ATOMS; i++) {
		cookies[i] = xcb_intern_atom(xwm->xcb, 0,
			(uint16_t)strlen(atom_names[i]), atom_names[i]);
	}
	for (size_t i = 0; i < NR_ATOMS; i++) {
		xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(xwm->xcb,
			cookies[i], NULL);
		assert(reply);
		xwm->atoms[i] = reply->atom;
		free(reply);
	}
}

// The window manager announces itself with a window of its own. Mapped as the
// target of X11 drags, it covers all X11 coordinates. Being input-only, it has
// no surface.
static void
create_wm_window(struct xwm *xwm)
{
	static const char name[] = "jimi";
	xcb_window_t root = xwm->screen->root;
	xwm->window = xcb_generate_id(xwm->xcb);
	xcb_create_window(xwm->xcb, XCB_COPY_FROM_PARENT, xwm->window, root,
		0, 0, INT16_MAX, INT16_MAX, 0, XCB_WINDOW_CLASS_INPUT_ONLY,
		XCB_COPY_FROM_PARENT, 0, NULL);
	xcb_change_property(xwm->xcb, XCB_PROP_MODE_REPLACE, xwm->window,
		xwm->atoms[ATOM_NET_WM_NAME], xwm->atoms[ATOM_UTF8_STRING], 8,
		(uint32_t)strlen(name), name);
	xcb_change_property(xwm->xcb, XCB_PROP_MODE_REPLACE, root,
		xwm->atoms[ATOM_NET_SUPPORTING_WM_CHECK], XCB_ATOM_WINDOW, 32, 1,
		&xwm->window);
	xcb_change_property(xwm->xcb, XCB_PROP_MODE_REPLACE, xwm->window,
		xwm->atoms[ATOM_NET_SUPPORTING_WM_CHECK], XCB_ATOM_WINDOW, 32, 1,
		&xwm->window);
	xcb_set_selection_owner(xwm->xcb, xwm->window, xwm->atoms[ATOM_WM_S0],
		XCB_CURRENT_TIME);
	// Clients use translucent windows while a compositing manager runs.
	xcb_set_selection_owner(xwm->xcb, xwm->window,
		xwm->atoms[ATOM_NET_WM_CM_S0], XCB_CURRENT_TIME);

	xcb_atom_t supported[] = {
		xwm->atoms[ATOM_NET_ACTIVE_WINDOW],
		xwm->atoms[ATOM_NET_WM_ICON],
		xwm->atoms[ATOM_NET_WM_STATE],
		xwm->atoms[ATOM_NET_WM_STATE_MAXIMIZED_VERT],
		xwm->atoms[ATOM_NET_WM_STATE_MAXIMIZED_HORZ],
		xwm->atoms[ATOM_NET_WM_STATE_FULLSCREEN],
		xwm->atoms[ATOM_NET_WM_STATE_FOCUSED],
		xwm->atoms[ATOM_NET_WM_STATE_MODAL],
		xwm->atoms[ATOM_NET_WM_MOVERESIZE],
	};
	xcb_change_property(xwm->xcb, XCB_PROP_MODE_REPLACE, root,
		xwm->atoms[ATOM_NET_SUPPORTED], XCB_ATOM_ATOM, 32,
		ARRAY_LEN(supported), supported);
}

// X11 windows without a cursor of their own show the root window's.
static void
set_root_cursor(struct xwm *xwm)
{
	xcb_cursor_context_t *context;
	if (xcb_cursor_context_new(xwm->xcb, xwm->screen, &context) < 0) {
		return;
	}
	xwm->cursor = xcb_cursor_load_cursor(context, "left_ptr");
	xcb_change_window_attributes(xwm->xcb, xwm->screen->root, XCB_CW_CURSOR,
		&xwm->cursor);
	xcb_cursor_context_free(context);
}

void
xwm_init(int wm_fd)
{
	struct xwm *xwm = calloc(1, sizeof(*xwm));
	assert(xwm);
	wl_list_init(&xwm->windows);
	xwm->xcb = xcb_connect_to_fd(wm_fd, NULL);
	assert(!xcb_connection_has_error(xwm->xcb));
	xwm->screen = xcb_setup_roots_iterator(xcb_get_setup(xwm->xcb)).data;
	core.xwayland.xwm = xwm;

	intern_atoms(xwm);
	uint32_t event_mask = XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY
		| XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT
		| XCB_EVENT_MASK_PROPERTY_CHANGE;
	xcb_change_window_attributes(xwm->xcb, xwm->screen->root,
		XCB_CW_EVENT_MASK, &event_mask);
	xcb_composite_redirect_subwindows(xwm->xcb, xwm->screen->root,
		XCB_COMPOSITE_REDIRECT_MANUAL);
	create_wm_window(xwm);
	xwm_selection_init(xwm);
	// Without persistence, Xwayland exits once only the window manager is
	// connected.
	xcb_xfixes_set_client_disconnect_mode(xwm->xcb,
		XCB_XFIXES_CLIENT_DISCONNECT_FLAGS_TERMINATE);
	set_root_cursor(xwm);
	xwm_focus(xwm, NULL);
	xcb_flush(xwm->xcb);

	xwm->source = wl_event_loop_add_fd(core.wl_loop, wm_fd,
		WL_EVENT_READABLE, handle_events, NULL);
	assert(xwm->source);
	// Replies to the requests above may have queued events already.
	wl_event_source_check(xwm->source);
}

void
xwm_fini(void)
{
	struct xwm *xwm = core.xwayland.xwm;
	xwm_selection_fini(xwm);
	struct xwayland_window *xw, *tmp;
	wl_list_for_each_safe(xw, tmp, &xwm->windows, link) {
		xwayland_window_destroy(xw);
	}
	wl_event_source_remove(xwm->source);
	if (xwm->cursor) {
		xcb_free_cursor(xwm->xcb, xwm->cursor);
	}
	xcb_disconnect(xwm->xcb);
	free(xwm);
	core.xwayland.xwm = NULL;
}

struct window *
xwayland_surface_window(struct xwayland_surface *xs)
{
	struct xwayland_window *xw = xs->window;
	return xw && !xw->override_redirect ? &xw->window : NULL;
}

// Xwayland may commit a serial before the window manager is ready.
static struct xwayland_window *
xwayland_window_from_serial(uint64_t serial)
{
	struct xwm *xwm = core.xwayland.xwm;
	if (!xwm) {
		return NULL;
	}
	struct xwayland_window *xw;
	wl_list_for_each(xw, &xwm->windows, link) {
		if (xw->serial == serial && !xw->surface) {
			return xw;
		}
	}
	return NULL;
}

void
xwayland_surface_commit(struct xwayland_surface *xs,
	const struct surface_state *state)
{
	if (state->xwayland_serial) {
		xs->serial = state->xwayland_serial;
		struct xwayland_window *xw = xwayland_window_from_serial(xs->serial);
		if (xw) {
			xwayland_window_associate(xw, xs);
			return;
		}
	}
	if (xs->window) {
		xwayland_window_update(xs->window);
	}
}

void
xwayland_surface_destroy(struct xwayland_surface *xs)
{
	struct surface *surface = xs->surface;
	if (xs->window) {
		xwayland_window_dissociate(xs->window);
	}
	wl_list_remove(&xs->link);
	wl_resource_set_user_data(xs->resource, NULL);
	surface->role = SURFACE_ROLE_NONE;
	surface->xwayland = NULL;
	free(xs);
}

static void
xwayland_surface_set_serial_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t serial_lo, uint32_t serial_hi)
{
	struct xwayland_surface *xs = wl_resource_get_user_data(resource);
	if (!xs) {
		return;
	}
	if (xs->serial) {
		wl_resource_post_error(resource,
			XWAYLAND_SURFACE_V1_ERROR_ALREADY_ASSOCIATED,
			"surface is already associated with an X11 window");
		return;
	}
	xs->surface->pending.xwayland_serial = (uint64_t)serial_hi << 32 | serial_lo;
}

static void
xwayland_surface_resource_destroy(struct wl_resource *resource)
{
	struct xwayland_surface *xs = wl_resource_get_user_data(resource);
	if (xs) {
		xwayland_surface_destroy(xs);
	}
}

static const struct xwayland_surface_v1_interface xwayland_surface_impl = {
	.set_serial = xwayland_surface_set_serial_req,
	.destroy = resource_destroy_req,
};

// The surface stays out of the scene until it is associated with a mapped
// X11 window.
static void
xwayland_shell_get_xwayland_surface_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id, struct wl_resource *surface_resource)
{
	struct surface *surface = wl_resource_get_user_data(surface_resource);
	if (surface->role != SURFACE_ROLE_NONE) {
		wl_resource_post_error(resource, XWAYLAND_SHELL_V1_ERROR_ROLE,
			"wl_surface already has another role");
		return;
	}
	struct wl_resource *xs_resource = wl_resource_create(client,
		&xwayland_surface_v1_interface, wl_resource_get_version(resource), id);
	assert(xs_resource);
	struct xwayland_surface *xs = calloc(1, sizeof(*xs));
	assert(xs);
	xs->surface = surface;
	xs->resource = xs_resource;
	list_append(&core.xwayland.surfaces, &xs->link);
	surface->role = SURFACE_ROLE_XWAYLAND;
	surface->xwayland = xs;
	wl_resource_set_implementation(xs_resource, &xwayland_surface_impl, xs,
		xwayland_surface_resource_destroy);
}

static const struct xwayland_shell_v1_interface xwayland_shell_impl = {
	.destroy = resource_destroy_req,
	.get_xwayland_surface = xwayland_shell_get_xwayland_surface_req,
};

void
wayland_bind_xwayland_shell(struct wl_client *client, void *data,
	uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&xwayland_shell_v1_interface, MIN(version, 1), id);
	assert(resource);
	wl_resource_set_implementation(resource, &xwayland_shell_impl, data, NULL);
}
