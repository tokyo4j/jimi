#ifndef JIMI_CONFIG_H
#define JIMI_CONFIG_H

#include <stddef.h>

#include "util.h"
#include "window.h"

struct yaml_document_s;

enum modifier {
	MODIFIER_SHIFT = 1u << 0,
	MODIFIER_CTRL = 1u << 1,
	MODIFIER_ALT = 1u << 2,
	MODIFIER_SUPER = 1u << 3,
};

enum action {
	ACTION_QUIT,
	ACTION_RELOAD, // Applies the config file again.
	ACTION_EXEC,
	ACTION_CLOSE,
	ACTION_TOGGLE_FULLSCREEN,
	ACTION_TOGGLE_MAXIMIZE,
	ACTION_TOGGLE_STICKY,
	// Switch windows until the modifiers are released.
	ACTION_NEXT_WINDOW,
	ACTION_PREVIOUS_WINDOW,
	ACTION_WORKSPACE, // Switch to the workspace.
	// Move the active window to the workspace, without switching to it.
	ACTION_MOVE_TO_WORKSPACE,
	// Magnify 2x, or stop magnifying.
	ACTION_TOGGLE_MAGNIFIER,
	// Multiply or divide the zoom of the magnifier by its zoom factor.
	ACTION_ZOOM_IN,
	ACTION_ZOOM_OUT,
};

// The workspaces of key bindings, besides the indices of
// config.workspaces.names. They stop at the first and the last workspace.
enum {
	WORKSPACE_NEXT = -1,
	WORKSPACE_PREVIOUS = -2,
};

struct key_binding {
	// Lowercase. Matched against the keysym of the key without modifiers.
	uint32_t keysym;
	uint32_t modifiers; // enum modifier, all of which must be pressed
	enum action action;
	const char *command; // For ACTION_EXEC
	// For ACTION_WORKSPACE and ACTION_MOVE_TO_WORKSPACE, an index of
	// config.workspaces.names, WORKSPACE_NEXT or WORKSPACE_PREVIOUS
	int workspace;
};

enum mouse_action {
	MOUSE_ACTION_MOVE,
	MOUSE_ACTION_RESIZE,
};

// Pressing the button over a window starts the action on it.
struct mouse_binding {
	uint32_t button; // e.g. BTN_LEFT
	uint32_t modifiers; // enum modifier, all of which must be pressed
	enum mouse_action action;
};

// The reference white of BT.2408, in cd/m²
#define DEFAULT_SDR_LUMINANCE 203.0

struct output_config {
	const char *name; // The DRM connector, e.g. "HDMI-A-1"
	// The mode in physical pixels, or 0 for the preferred mode.
	int width;
	int height;
	int refresh; // mHz, or 0 for any
	double scale; // A multiple of 1/120
	enum wl_output_transform transform;
	// Otherwise, placed right of the other outputs when enabled, and kept
	// where it is while enabled.
	bool positioned;
	int x;
	int y;
	bool adaptive_sync; // If the output supports it
	bool hdr; // If the output supports it
	double sdr_luminance; // Of SDR white with HDR, in cd/m²
};

enum input_type {
	INPUT_TYPE_KEYBOARD,
	INPUT_TYPE_POINTER, // Other than touchpads
	INPUT_TYPE_TOUCHPAD,
	INPUT_TYPE_TOUCHSCREEN,
	INPUT_TYPE_TABLET,
	INPUT_TYPE_OTHER, // e.g. switches, only matched by name
};

// Configures the libinput devices it matches. The settings are -1, or NAN for
// accel_speed, if unset, which keeps the defaults of libinput.
struct input_config {
	const char *name; // Of the device, or NULL for any
	int type; // enum input_type, or -1 for any
	int send_events; // enum libinput_config_send_events_mode
	int tap; // bool
	int tap_button_map; // enum libinput_config_tap_button_map
	int tap_drag; // bool
	int tap_drag_lock; // enum libinput_config_drag_lock_state
	int three_finger_drag; // enum libinput_config_3fg_drag_state
	int natural_scroll; // bool
	int left_handed; // bool
	double accel_speed; // From -1 to 1
	int accel_profile; // enum libinput_config_accel_profile
	int middle_emulation; // bool
	int disable_while_typing; // bool
	int click_method; // enum libinput_config_click_method
	int scroll_method; // enum libinput_config_scroll_method
	int scroll_button; // e.g. BTN_MIDDLE, for LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN
	double scroll_factor; // Multiplies the scroll distance.
	// The output that absolute devices, like touchscreens and tablets,
	// span, or NULL for the output at the origin of the layout
	const char *map_to_output;
};

// The decorations of the active window are distinguished from the others by
// their colors.
struct decoration_colors {
	struct color border;
	struct color titlebar;
	struct color title;
	struct color shadow;
};

// The items are at the left and the right end of the titlebar, and the title
// is centered in it but slides away from them. Each item is listed at most
// once across both sides.
struct titlebar_layout_config {
	enum titlebar_item left[NR_TITLEBAR_ITEMS]; // Left to right
	int nr_left;
	enum titlebar_item right[NR_TITLEBAR_ITEMS]; // Left to right
	int nr_right;
	bool show_title;
};

// How a button looks in a state, on an active or inactive window.
struct button_style {
	// An SVG document, which fills the square of the icon size in the
	// middle of the button, and draws in the icon color with currentColor.
	const char *icon;
	struct color background_color;
	struct color icon_color;
};

struct config {
	char *path; // Of the file, for the errors.
	// The strings below point into it, until the config is replaced.
	struct yaml_document_s *document;
	bool invalid; // Set by the first error in the file.
	struct {
		bool xwayland;
		// Keeps Xwayland running without X11 clients, instead of starting
		// it on the first connection and stopping it after the last one.
		bool xwayland_persistence;
	} backend;
	const char **autostart; // Shell commands
	size_t nr_autostart;
	struct output_config *outputs;
	size_t nr_outputs;
	struct input_config *inputs;
	size_t nr_inputs;
	struct {
		// NULL for the defaults of xkbcommon.
		const char *layout;
		const char *variant;
		const char *options;
		int repeat_rate;
		int repeat_delay;
	} keyboard;
	struct {
		const char *theme; // NULL for the default theme.
		int size;
	} cursor;
	struct {
		struct color background_color;
	} desktop;
	struct {
		// Used for clients that negotiate decorations without preferring
		// a mode. Others decorate themselves.
		bool default_ssd;
		// Moving the pointer into a window focuses it, besides clicking.
		bool focus_follows_mouse;
	} windows;
	// Each window is on one of them, and all outputs show the same one.
	struct {
		const char **names;
		size_t nr_names;
	} workspaces;
	// A moved window snaps to the edge or the corner of the usable area of
	// an output that the pointer is released at.
	struct {
		// From the edges. 0 disables snapping.
		int edge_range;
		// Along the edges, from the corners
		int corner_range;
		// Dragged distance to restore a snapped or maximized window
		int unsnap_threshold;
		struct color preview_color; // Shows where the window snaps to.
	} snapping;
	struct {
		int border_width;
		int titlebar_height;
		int corner_radius; // At the top corners
		int resize_margin; // Grabs past the frame to resize it.
		// The distance past the frame over which its shadow fades out. 0
		// disables the shadows.
		int shadow_blur;
		// Of the shadow from the frame
		int shadow_offset_x;
		int shadow_offset_y;
		const char *font;
		int app_icon_size; // At the left of the titlebar.
		const char *icon_theme; // Falls back to hicolor.
		struct {
			struct decoration_colors active;
			struct decoration_colors inactive;
		} colors;
		struct {
			struct titlebar_layout_config layout;
			int button_width;
			int icon_size; // Of the icons of the buttons
			// By the button, its state and the activation of the window
			struct button_style
				button_styles[NR_BUTTON_TYPES][NR_BUTTON_STATES][2];
		} titlebar;
	} decorations;
	// Shows the windows as a grid of items in the middle of the output under
	// the pointer while switching them.
	struct {
		// The largest size of the thumbnails, which keep the aspect ratio
		// of the windows and are not enlarged.
		int thumbnail_width;
		int thumbnail_height;
		int app_icon_size; // Centered on the bottom edge of the thumbnail
		const char *font;
		int title_height; // Below the app icon
		int padding; // Around the grid, and inside the items
		// Of the items. The grid is rounded by this plus the padding.
		int corner_radius;
		int border_width; // Of the item of the selected window
		int outline_width; // Inside the frame of the selected window
		// At the top-right corner of the thumbnail area of the hovered
		// item
		int close_button_size;
		int close_icon_size;
		struct {
			struct color background;
			struct color selected_background;
			struct color selected_border;
			struct color title;
			struct color outline;
		} colors;
	} window_switcher;
	// Warms the colors of the outputs at night, by the local time.
	struct {
		// Of white at night, in K. 6500 shows the colors as they are,
		// which disables the night light.
		int temperature;
		// Minutes from midnight. The night lasts all day if they are
		// equal.
		int start;
		int end;
		// The minutes over which the temperature changes from the start
		// and the end. Not longer than the night or the day, unless the
		// night lasts all day.
		int transition;
	} night_light;
	// Magnifies the area around the pointer on the output under it, like
	// the magnifier and zoom effects of KWin.
	struct {
		// Magnifies the whole output instead of a lens around the pointer.
		bool fullscreen;
		// Of the lens, in logical pixels
		int width;
		int height;
		double zoom_factor; // Greater than 1
	} magnifier;
	struct key_binding *key_bindings;
	size_t nr_key_bindings;
	struct mouse_binding *mouse_bindings;
	size_t nr_mouse_bindings;
};

extern struct config config;

void config_load(const char *path);
bool config_reload(void);
void config_fini(void);
// NULL if the output is not configured.
const struct output_config *config_find_output(const char *name);
// The first entry matching the device, or NULL
const struct input_config *config_find_input(const char *name, enum input_type type);

#endif
