#ifndef JIMI_SEAT_H
#define JIMI_SEAT_H

#include <stddef.h>

#include "cursor-shape-v1-protocol.h"
#include "tablet-v2-protocol.h"
#include "util.h"
#include "window.h"

#define MAX_PRESSED 32
#define MAX_TOUCH_POINTS 16
#define SEAT_VERSION 8
#define DATA_DEVICE_MANAGER_VERSION 3
#define CURSOR_SHAPE_MANAGER_VERSION 2
#define POINTER_GESTURES_VERSION 3
#define TABLET_MANAGER_VERSION 2
// Indexed by the shapes, which start at 1.
#define NR_CURSOR_SHAPES (WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_RESIZE + 1)
#define SNAP_PREVIEW_DELAY_MS 100
#define DOUBLE_CLICK_TIME_MS 400

struct binding;
struct libinput_device;
struct libinput_device_group;
struct surface;
struct tablet_tool;
struct xdg_popup;
struct xkb_context;
struct xkb_keymap;
struct xkb_state;

struct pointer_button {
	uint32_t button;
	uint32_t serial;
	struct surface *surface;
};

struct pointer_axis_event {
	enum wl_pointer_axis axis;
	double value;
	int32_t value120; // 0 unless from a wheel
};

// What the pointer is over: a surface, or a part of the decoration of a
// window.
struct input_target {
	struct surface *surface; // NULL on a decoration.
	double sx, sy; // Surface-local
	struct window *window; // NULL on surfaces outside of windows.
	enum decoration_part part;
	uint32_t edges; // enum edge, resized by DECORATION_PART_BORDER
};

enum selection_type {
	SELECTION_CLIPBOARD,
	// Set by selecting text, and pasted by the middle button.
	SELECTION_PRIMARY,
	NR_SELECTION_TYPES,
};

enum data_source_type {
	DATA_SOURCE_DATA_DEVICE, // wl_data_source
	DATA_SOURCE_PRIMARY_SELECTION, // zwp_primary_selection_source_v1
	DATA_SOURCE_DATA_CONTROL, // ext_data_control_source_v1
	DATA_SOURCE_XWAYLAND, // The selection of an X11 client
	DATA_SOURCE_XDND, // The drag of an X11 client
};

// Offers data in MIME types as a selection or a drag, which it can be used for
// only once.
struct data_source {
	enum data_source_type type;
	struct wl_resource *resource; // NULL for X11 sources
	struct wl_array mime_types; // char *
	bool used;
	// Of drags, by wl_data_source.set_actions or XdndPosition
	bool actions_set;
	uint32_t actions;
	// data_offer.link. The offers become inert when the source is replaced
	// or destroyed.
	struct wl_list offers;
};

// Started by wl_data_device.start_drag, or by an X11 client (xwm.x11_drag). It
// outlives its origin surface, e.g. a browser window that closes once its last
// tab is dragged into another one.
struct drag {
	struct data_source *source; // NULL for drags within the client.
	struct wl_client *client; // Of the origin, NULL for X11 drags
	struct surface *icon;
	// Includes the offset of the icon when the drag started, so that the
	// offsets committed later move the icon.
	int icon_hotspot_x;
	int icon_hotspot_y;
	struct surface *focus; // Dragged over
};

enum gesture_type {
	GESTURE_SWIPE,
	GESTURE_PINCH,
	GESTURE_HOLD,
	NR_GESTURE_TYPES,
};

enum pointer_constraint_type {
	POINTER_CONSTRAINT_LOCK, // zwp_locked_pointer_v1
	POINTER_CONSTRAINT_CONFINE, // zwp_confined_pointer_v1
};

// Locks the pointer in place, or confines it to a region of the surface. Its
// region and cursor hint are in the state of the surface.
struct pointer_constraint {
	struct wl_resource *resource;
	struct surface *surface; // NULL once destroyed
	enum pointer_constraint_type type;
	// Never applies again once it stops applying.
	bool oneshot;
	bool defunct;
};

enum pointer_grab_type {
	POINTER_GRAB_NONE,
	POINTER_GRAB_MOVE,
	POINTER_GRAB_RESIZE,
	POINTER_GRAB_DECORATION_BUTTON,
	POINTER_GRAB_DND,
};

// A grab of the compositor, which ends when the button is released.
struct pointer_grab {
	enum pointer_grab_type type;
	uint32_t button;
	union {
		struct {
			struct window *window;
			double start_x, start_y;
			struct box start_geometry; // Committed
			// Snapped to when released
			struct output *snap_output;
			uint32_t snap_edges; // enum edge
			// Shows core.snap_preview once the snapping edges stay.
			struct wl_event_source *snap_preview_timer;
		} move;
		struct {
			struct window *window;
			uint32_t edges; // enum edge
			double start_x, start_y;
			struct box start_geometry;
			int configured_width;
			int configured_height;
			uint32_t last_configure_time; // ms, of the motion
		} resize;
		struct {
			struct window *window;
			enum decoration_part part;
		} decoration_button;
		struct drag drag;
	};
};

// The keymap, the pressed keys and the modifiers which clients see, of the
// physical keyboards or of a virtual keyboard.
struct keyboard_source {
	int keymap_fd; // -1 until a virtual keyboard sets its keymap
	uint32_t keymap_size;
	// Sent to clients, unlike the keys of bindings.
	uint32_t pressed_keys[MAX_PRESSED];
	size_t nr_pressed_keys;
	struct {
		uint32_t depressed;
		uint32_t latched;
		uint32_t locked;
		uint32_t group;
	} modifiers;
};

struct keyboard {
	struct wl_list resources; // keyboard_resource.link
	struct surface *focus;
	// The surfaces which had the focus and can take it back, the latest
	// first. The focus returns to the latest one when it leaves a surface.
	struct wl_list focus_history; // surface.focus_link
	struct xkb_context *context;
	// Shared by the physical keyboards
	struct xkb_keymap *keymap;
	struct xkb_state *state;
	struct keyboard_source physical;
	// Whose keymap the wl_keyboards have, and whose keys and modifiers the
	// focus has.
	struct keyboard_source *source;
	// The binding of the pressed key runs again by the repeat info of
	// wl_keyboard, until any key is pressed or released.
	const struct binding *repeat_binding;
	struct wl_event_source *repeat_timer;
};

struct pointer {
	struct wl_list resources; // pointer_resource.link
	struct surface *focus;
	double x;
	double y;
	double surface_x;
	double surface_y;
	// Moves the pointer while it gives its events to a surface. The
	// pointer stays unfocused meanwhile, and the client of the surface
	// picks the cursor through the tool.
	struct tablet_tool *tablet_tool;
	// Picked by the client of the focus, or of the focus of the tablet
	// tool, as a surface, a shape drawn from the cursor theme, or neither
	// to hide it.
	struct {
		// The wl_pointer or the zwp_tablet_tool_v2 that set it.
		struct wl_resource *resource;
		struct surface *surface;
		enum wp_cursor_shape_device_v1_shape shape; // 0 for none
		// Includes the offset of the surface when it was set, so that
		// the offsets committed later move the cursor.
		int32_t hotspot_x;
		int32_t hotspot_y;
	} cursor;
	// Picked by the compositor while no client has the pointer.
	enum wp_cursor_shape_device_v1_shape server_cursor_shape;
	// The window and its decoration part under the pointer. Kept during
	// move, resize, and drag grabs, and cleared by implicit grabs.
	struct {
		struct window *window;
		enum decoration_part part;
	} hover;
	// The last press on a titlebar, which a press on it soon after makes a
	// double-click.
	struct {
		struct window *window; // Cleared when it is unmapped.
		uint32_t time; // ms
	} titlebar_press;
	// Buttons delivered to a client. While any is held, the pointer stays
	// on the surface that received them (implicit grab), unless moves,
	// resizes, drags, or window switching take the pointer.
	struct pointer_button pressed[MAX_PRESSED];
	size_t nr_pressed;
	struct pointer_grab grab;
	// The top-most popup of the xdg_popup grab chain.
	struct xdg_popup *grabbed_popup;
	// zwp_pointer_gesture_*_v1 resource links, by enum gesture_type
	struct wl_list gesture_resources[NR_GESTURE_TYPES];
	// The touchpad gesture in progress goes to the surface under the
	// pointer when it began.
	struct {
		enum gesture_type type;
		struct surface *surface; // NULL while no gesture is in progress
	} gesture;
	// zwp_relative_pointer_v1 resource links
	struct wl_list relative_pointers;
	// The constraint of the focus that applies, or NULL.
	struct pointer_constraint *constraint;
};

struct touch_point {
	int32_t id; // The seat slot of libinput
	// Takes the events of the point, or NULL if the point moves the
	// pointer instead, which only one point does at a time.
	struct surface *surface;
	uint32_t serial; // Of wl_touch.down
	double x, y;
};

struct touch {
	struct wl_list resources; // touch_resource.link
	struct touch_point points[MAX_TOUCH_POINTS];
	size_t nr_points;
};

struct tablet {
	struct libinput_device *device; // Identifies the tablet to the backend.
	// The tablet and the pads of a device group are parts of one device.
	struct libinput_device_group *group;
	struct wl_list link; // core.seat.tablets.tablets
	struct wl_list resources; // zwp_tablet_v2 resource links
	char *name;
	uint32_t vendor;
	uint32_t product;
	char *path; // NULL if unknown
};

// The axes of a tablet tool event. Only those that changed are sent.
struct tablet_tool_axes {
	double x, y; // In the layout
	uint32_t changed; // 1 << enum zwp_tablet_tool_v2_capability
	double pressure; // From 0 to 1
	double distance; // From 0 to 1
	double tilt_x, tilt_y; // Degrees
	double rotation; // Degrees
	double slider; // From -1 to 1
	double wheel; // Degrees
	int32_t wheel_clicks;
};

// A pen, an eraser, or another tool used on tablets.
struct tablet_tool {
	struct wl_list link; // core.seat.tablets.tools
	struct wl_list resources; // tablet_tool_resource.link
	enum zwp_tablet_tool_v2_type type;
	uint64_t hardware_serial; // 0 if unknown
	uint64_t hardware_id; // Wacom tool ID, 0 if unknown
	uint32_t capabilities; // 1 << enum zwp_tablet_tool_v2_capability
	// The tablet that libinput tells the tool apart on, which removes the
	// tool with it, or NULL if the tool has a hardware serial.
	struct tablet *owner;
	struct tablet *tablet; // In proximity of, or NULL
	double x, y;
	// Takes the events of the tool, or NULL while the tool moves the
	// pointer instead. Kept while the tool presses.
	struct surface *focus;
	uint32_t proximity_serial;
	bool down; // The tip touches the tablet.
	uint32_t down_serial;
	int nr_pressed_buttons;
};

// The buttons, rings, strips and dials of a pad that switch modes together.
struct tablet_pad_group {
	uint64_t buttons; // 1 << index. Pads have fewer than 64 buttons.
	uint32_t rings; // 1 << index
	uint32_t strips; // 1 << index
	uint32_t dials; // 1 << index
	uint32_t nr_modes;
	uint32_t mode;
};

// The buttons, rings, strips and dials of a tablet, which go to the keyboard
// focus.
struct tablet_pad {
	struct libinput_device *device; // Identifies the pad to the backend.
	struct libinput_device_group *group; // Of its tablet
	struct wl_list link; // core.seat.tablets.pads
	struct wl_list resources; // tablet_pad_resource.link
	char *path; // NULL if unknown
	uint32_t nr_buttons;
	struct tablet_pad_group *groups;
	uint32_t nr_groups;
	// The keyboard focus if its client takes the input of the pad, which
	// needs the tablet of the pad, or else NULL.
	struct surface *focus;
};

struct tablets {
	struct wl_list seats; // zwp_tablet_seat_v2 resource links
	struct wl_list tablets; // tablet.link
	struct wl_list tools; // tablet_tool.link
	struct wl_list pads; // tablet_pad.link
};

struct text_input_state {
	bool enabled;
	char *surrounding_text; // NULL if the client does not send it.
	uint32_t surrounding_cursor; // Byte offsets in the surrounding text
	uint32_t surrounding_anchor;
	uint32_t content_hint; // enum zwp_text_input_v3_content_hint
	uint32_t content_purpose; // enum zwp_text_input_v3_content_purpose
	struct box cursor_rectangle; // Surface-local
};

// A zwp_text_input_v3, which follows the keyboard focus into the surfaces of
// its client.
struct text_input {
	struct wl_resource *resource;
	struct wl_list link; // seat.text_inputs
	struct surface *focus; // Entered, or NULL
	struct text_input_state pending, current;
	// An enable request awaits the commit, which then starts the input
	// method over even if the text input is already enabled.
	bool enable_requested;
	uint32_t serial; // The number of commits
};

// The input method client composes text for the enabled text input on the
// keyboard focus.
struct input_method {
	// zwp_input_method_v1 resource links. Only the first is used, e.g. until
	// the instance of the input method that it replaces exits.
	struct wl_list resources;
	// The enabled text input on the keyboard focus, or NULL. Others enabled
	// meanwhile are ignored.
	struct text_input *text_input;
	// The zwp_input_method_context_v1 of the text input, or NULL. Contexts
	// of earlier text inputs are inert.
	struct wl_resource *context;
	// A wl_keyboard grabbed by the context, which takes the keys instead of
	// the keyboard focus. The input method forwards the keys it does not use.
	struct wl_resource *keyboard_grab;
	// The input method sets these before the preedit or commit string they
	// apply to.
	int32_t preedit_cursor;
	bool preedit_cursor_set;
	uint32_t delete_before;
	uint32_t delete_after;
	// Replaces the preedit string when the text input loses the focus.
	char *preedit_commit;
	struct wl_list panels; // input_panel_surface.link
};

struct seat {
	struct keyboard keyboard;
	struct pointer pointer;
	struct touch touch;
	struct tablets tablets;
	// The last input event delivered to a client, whose serial allows the
	// client to set the selection.
	uint32_t input_serial;
	struct wl_client *input_client;
	struct wl_list data_devices; // data_device.link
	// zwp_primary_selection_device_v1 resource links
	struct wl_list primary_selection_devices;
	struct wl_list data_control_devices; // ext_data_control_device_v1 resource links
	struct data_source *selections[NR_SELECTION_TYPES];
	// The client of the keyboard focus, which is sent the selections.
	struct wl_client *selection_client;
	struct wl_list text_inputs; // text_input.link
	struct input_method input_method;
};

void keyboard_init(void);
void keyboard_reload(void);
void keyboard_fini(void);
void keyboard_create_resource(struct wl_client *client,
	struct wl_resource *seat, uint32_t id);
void keyboard_send_keymap(struct wl_resource *resource,
	struct keyboard_source *source);
void keyboard_use_source(struct keyboard_source *source);
void keyboard_set_source_keymap(struct keyboard_source *source,
	struct xkb_keymap *keymap);
void keyboard_send_source_key(struct keyboard_source *source, uint32_t time,
	uint32_t key, bool pressed);
void keyboard_set_source_modifiers(struct keyboard_source *source,
	uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group);
void keyboard_remove_source(struct keyboard_source *source);
void keyboard_focus(struct surface *surface);
void keyboard_handle_key(uint32_t time, uint32_t key, bool pressed);
void keyboard_send_key(uint32_t time, uint32_t key, uint32_t state);
void keyboard_send_keysym(uint32_t time, uint32_t keysym);
void keyboard_send_modifiers(uint32_t depressed, uint32_t latched,
	uint32_t locked, uint32_t group);
uint32_t keyboard_pressed_modifiers(void);
void keyboard_forget_surface(struct surface *surface);
void keyboard_restore_focus(void);
void keyboard_update_focus(void);
struct window *keyboard_active_window(void);

void pointer_init(void);
void pointer_fini(void);
void pointer_create_resource(struct wl_client *client,
	struct wl_resource *seat, uint32_t id);
void pointer_focus(struct surface *surface, double sx, double sy);
void pointer_update_focus(uint32_t time, bool send_motion);
struct input_target input_target_at(double x, double y, bool skip_unmanaged);
struct surface *pointer_implicit_grab_surface(void);
void pointer_handle_motion(double dx, double dy, double dx_unaccel,
	double dy_unaccel, uint64_t time_usec);
void pointer_handle_output_change(void);
void pointer_handle_motion_absolute(double x, double y, uint32_t time);
void pointer_handle_button(uint32_t time, uint32_t button, bool pressed);
void pointer_handle_scroll(uint32_t time, enum wl_pointer_axis_source source,
	const struct pointer_axis_event axes[], size_t nr_axes);
void pointer_warp(double x, double y);
void pointer_set_tablet_tool(struct tablet_tool *tool);
struct surface *direct_input_surface_at(double x, double y);
uint32_t direct_input_press(struct surface *surface);
uint32_t input_press_serial(struct surface *ancestor);
void pointer_request_move(struct window *window, uint32_t serial);
void pointer_request_resize(struct window *window, uint32_t serial, uint32_t edges);
bool pointer_start_drag(struct surface *origin, uint32_t serial,
	struct surface *icon, struct data_source *source);
void pointer_cancel_grab(void);
struct box pointer_move_geometry(struct window *window, int width, int height);
void pointer_set_client_cursor(struct wl_resource *resource,
	struct surface *surface, enum wp_cursor_shape_device_v1_shape shape,
	int32_t hotspot_x, int32_t hotspot_y);
bool cursor_shape_validate(struct wl_resource *resource, uint32_t shape);
struct surface *pointer_client_cursor_surface(void);
struct surface *pointer_drag_icon_surface(void);
struct box pointer_surface_box(struct surface *surface);
enum wp_cursor_shape_device_v1_shape pointer_cursor_shape(void);
void pointer_surface_commit(struct surface *surface);
void pointer_forget_surface(struct surface *surface);
void pointer_surface_destroy(struct surface *surface);

void pointer_handle_gesture_begin(enum gesture_type type, uint32_t time,
	uint32_t fingers);
void pointer_handle_swipe_update(uint32_t time, double dx, double dy);
void pointer_handle_pinch_update(uint32_t time, double dx, double dy,
	double scale, double rotation);
void pointer_handle_gesture_end(enum gesture_type type, uint32_t time,
	bool cancelled);
void pointer_gestures_forget_surface(struct surface *surface);

void relative_pointers_send_motion(uint64_t time_usec, double dx, double dy,
	double dx_unaccel, double dy_unaccel);
void pointer_constraint_update(void);
void pointer_confine(double *x, double *y);

void touch_create_resource(struct wl_client *client, struct wl_resource *seat,
	uint32_t id);
void touch_handle_down(int32_t id, double x, double y, uint32_t time);
void touch_handle_motion(int32_t id, double x, double y, uint32_t time);
void touch_handle_up(int32_t id, uint32_t time);
void touch_handle_cancel(int32_t id, uint32_t time);
void touch_handle_frame(void);
bool touch_take_point(struct surface *ancestor, uint32_t serial);
uint32_t touch_press_serial(struct surface *ancestor);
void touch_forget_surface(struct surface *surface);

struct tablet *tablet_create(struct libinput_device *device,
	struct libinput_device_group *group, const char *name, uint32_t vendor,
	uint32_t product, const char *path);
struct wl_resource *tablet_resource_for(struct tablet *tablet,
	struct wl_client *client);
void tablet_destroy(struct tablet *tablet);
struct tablet_tool *tablet_tool_create(struct tablet *owner,
	enum zwp_tablet_tool_v2_type type, uint64_t hardware_serial,
	uint64_t hardware_id, uint32_t capabilities);
void tablet_tool_handle_proximity_in(struct tablet_tool *tool,
	struct tablet *tablet, const struct tablet_tool_axes *axes, uint32_t time);
void tablet_tool_handle_proximity_out(struct tablet_tool *tool, uint32_t time);
void tablet_tool_handle_axis(struct tablet_tool *tool,
	const struct tablet_tool_axes *axes, uint32_t time);
void tablet_tool_handle_tip(struct tablet_tool *tool,
	const struct tablet_tool_axes *axes, bool down, uint32_t time);
void tablet_tool_handle_button(struct tablet_tool *tool, uint32_t button,
	bool pressed, uint32_t time);
void tablet_tool_leave(struct tablet_tool *tool, uint32_t time);
void tablet_tool_get_cursor_shape_device(struct wl_client *client,
	uint32_t version, uint32_t id, struct wl_resource *tool);
bool tablet_take_tool(struct surface *ancestor, uint32_t serial);
uint32_t tablet_press_serial(struct surface *ancestor);
void tablet_forget_surface(struct surface *surface);
void tablets_fini(void);

struct tablet_pad *tablet_pad_create(struct libinput_device *device,
	struct libinput_device_group *group, const char *path,
	uint32_t nr_buttons, struct tablet_pad_group *groups, uint32_t nr_groups);
void tablet_pad_destroy(struct tablet_pad *pad);
void tablet_pad_send_added(struct wl_resource *seat, struct tablet_pad *pad);
void tablet_pad_handle_mode(struct tablet_pad *pad, uint32_t group,
	uint32_t mode, uint32_t time);
void tablet_pad_handle_button(struct tablet_pad *pad, uint32_t button,
	bool pressed, uint32_t time);
void tablet_pad_handle_ring(struct tablet_pad *pad, uint32_t ring,
	double degrees, bool finger, uint32_t time);
void tablet_pad_handle_strip(struct tablet_pad *pad, uint32_t strip,
	double position, bool finger, uint32_t time);
void tablet_pad_handle_dial(struct tablet_pad *pad, uint32_t dial,
	double value120, uint32_t time);
void tablet_pads_update_focus(void);

void data_device_init(void);
void data_device_fini(void);
void data_device_keyboard_focus(struct surface *surface);
bool input_serial_matches_client(struct wl_client *client, uint32_t serial);
struct data_source *data_source_create(enum data_source_type type,
	struct wl_resource *resource);
void data_source_offer(struct data_source *source, const char *mime_type);
void data_source_set_actions(struct data_source *source, uint32_t actions);
bool data_source_has_mime_type(struct data_source *source, const char *mime_type);
void data_source_send(struct data_source *source, const char *mime_type, int fd);
void data_source_destroy(struct data_source *source);
void data_source_resource_destroy(struct wl_resource *resource);
struct wl_resource *data_offer_create(struct wl_resource *device,
	const struct wl_interface *interface, const void *implementation,
	struct data_source *source);
void data_offer_receive(struct wl_client *client, struct wl_resource *resource,
	const char *mime_type, int32_t fd);
void seat_set_selection(enum selection_type type, struct data_source *source);
void drag_motion(struct drag *drag, struct surface *surface, double sx,
	double sy, uint32_t time, bool send_motion);
void drag_drop(struct drag *drag);
void drag_cancel(struct drag *drag);

void primary_selection_send(struct wl_client *client);
void data_control_send_selection(enum selection_type type);

void xwm_set_selection(enum selection_type type);
void xwm_data_source_send(struct data_source *source, const char *mime_type, int fd);
void xwm_drag_enter(struct data_source *source, struct surface *surface);
void xwm_drag_motion(void);
void xwm_drag_leave(void);
bool xwm_drag_drop(void);
void xwm_x11_drag_update(void);
void xwm_x11_drag_forget_surface(struct surface *surface);
void xwm_x11_drag_accept(bool accepted);
void xwm_x11_drag_action(uint32_t action);
void xwm_x11_drag_finish(bool success);

void text_inputs_focus(struct surface *surface);

void input_method_activate(struct text_input *text_input);
void input_method_deactivate(void);
void input_method_update(void);
void input_method_reset(void);
void input_method_commit_preedit(void);

#endif
