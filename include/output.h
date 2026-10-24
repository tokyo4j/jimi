#ifndef JIMI_OUTPUT_H
#define JIMI_OUTPUT_H

#include <xf86drmMode.h>

#include "color.h"
#include "render.h"
#include "seat.h"
#include "util.h"
#include "window.h"

#define NR_LAYER_SHELL_LAYERS 4
#define MAX_OUTPUTS 32 // One bit each in surface.outputs

struct client_buffer;

// What the configuration and output management clients choose for an output.
// The rest is ignored for disabled outputs.
struct output_state {
	struct output *output;
	bool enabled;
	const drmModeModeInfo *mode; // One of output.modes
	int x; // In the layout
	int y;
	double scale;
	enum wl_output_transform transform;
	// Only for outputs that support them
	bool adaptive_sync;
	bool hdr;
	double sdr_luminance; // Of SDR white with HDR, in cd/m²
};

struct plane_format {
	uint32_t format;
	uint64_t modifier;
};

// A plane on the CRTC of an output, and the IDs of its properties.
struct plane {
	uint32_t id;
	struct {
		uint32_t fb_id;
		uint32_t in_fence_fd;
		uint32_t crtc_id;
		uint32_t src_x;
		uint32_t src_y;
		uint32_t src_w;
		uint32_t src_h;
		uint32_t crtc_x;
		uint32_t crtc_y;
		uint32_t crtc_w;
		uint32_t crtc_h;
	} props;
	// The pixel formats and modifiers that it can scan out, which the
	// renderer may not support.
	struct plane_format *formats;
	uint32_t nr_formats;
};

// Shows the cursor above the images of the swapchain, so that moving it draws
// nothing. The cursor is drawn into an image of the size of the plane, at its
// top-left corner in the orientation of the layout.
struct cursor_plane {
	// No plane if the CRTC has none, or the renderer cannot draw for it.
	struct plane plane;
	struct swapchain_image images[NR_CURSOR_IMAGES];
	int current; // Drawn last, or -1
	// The cursor has changed since the image was drawn, other than by
	// moving.
	bool outdated;
	// What the commits show: the current image at the box in the pixels of
	// the mode, or nothing.
	bool visible;
	struct box box;
};

// What the primary plane shows: an image of the swapchain, or the dma-buf of
// a surface scanned out directly. Neither is set for no frame.
struct frame {
	struct swapchain_image *image;
	struct client_buffer *buffer; // Locked by each frame showing it
	// Shown at once rather than at the next vblank, tearing, as the surface
	// that it scans out asks for. Cleared for commits that cannot be.
	bool tearing;
	// Prepared while the session is locked, so it hides the other surfaces.
	bool locked;
};

static inline bool
frame_empty(const struct frame *frame)
{
	return !frame->image && !frame->buffer;
}

// A connected connector. Its CRTC and plane are kept for it while it is
// connected, even while it is disabled.
struct output {
	struct wl_list link; // core.outputs
	// A bit no other connected output has, for surface.outputs
	uint32_t bit;
	// Enabled outputs are in the layout and scanned out, and the others are
	// only shown to the output managers.
	bool enabled;
	struct wl_global *global; // NULL while disabled
	struct wl_list resources; // wl_output resource links
	struct wl_list xdg_resources; // zxdg_output_v1 resource links
	// wp_color_management_output_v1 resource links
	struct wl_list color_resources;
	struct wl_list heads; // output_head.link
	// Of the configurations for the output, which are cancelled when it is
	// removed.
	struct wl_list configuration_heads; // configuration_head.output_link
	char name[32]; // The DRM connector, e.g. "HDMI-A-1"
	// From the EDID: the manufacturer, and the monitor name or else the
	// product code.
	char make[32];
	char model[32];
	char description[128];
	int physical_width; // mm
	int physical_height; // mm
	uint32_t conn_id;
	uint32_t crtc_id;
	struct plane primary;
	struct cursor_plane cursor;
	uint32_t mode_blob;
	drmModeModeInfo *modes; // Of the connector
	int nr_modes;
	// One of the modes. The mode, the box, the scale, the transform,
	// adaptive sync and HDR are kept while disabled, to enable the output
	// with them again.
	const drmModeModeInfo *mode;
	struct {
		uint32_t conn_crtc_id;
		// 0 if the connector cannot show HDR
		uint32_t conn_colorspace;
		uint32_t conn_hdr_output_metadata;
		uint32_t conn_max_bpc; // 0 if the connector has no max bpc
		uint32_t crtc_active;
		uint32_t crtc_mode_id;
		uint32_t crtc_gamma_lut; // 0 if the CRTC has no gamma LUT
		uint32_t crtc_vrr_enabled; // 0 if the CRTC has no adaptive sync
	} props;
	// Of the Colorspace property, for SDR and HDR
	uint64_t colorspace_default;
	uint64_t colorspace_bt2020;
	uint64_t max_bpc_range[2];
	// The monitor and the CRTC can vary the refresh rate, up to that of the
	// mode, by extending the vblanks until the next commit.
	bool adaptive_sync_supported;
	// Set by the modesets. Frames are then drawn as soon as something
	// changes, rather than for the vblanks of the mode.
	bool adaptive_sync;
	// The monitor and the connector can show BT.2020 colors encoded by PQ,
	// though the primary plane may not scan out HDR_SCANOUT_FORMAT, or the
	// renderer may not draw it.
	bool hdr_supported;
	// The color volume of the monitor from the EDID, which HDR targets
	struct color_primaries display_primaries;
	double display_min_luminance; // cd/m²
	double display_max_luminance;
	// Set by the modesets. The images are then in HDR_SCANOUT_FORMAT, which
	// the colors are encoded into from the blend buffer.
	bool hdr;
	uint32_t hdr_metadata_blob; // While HDR is on the screen, or 0
	// Of SDR white with HDR, in cd/m²
	double sdr_luminance;
	// The colors that the images are in, SDR or HDR. Told to the clients.
	struct image_description *image_description;
	// With HDR, of the size of the mode. The colors are blended in it
	// linearly before they are encoded into the images.
	struct texture *blend_buffer;
	uint32_t gamma_lut_size; // Entries of the gamma LUT
	// Warms the colors for the night light, or 0 to show them as they are.
	uint32_t gamma_lut_blob;
	struct swapchain_image swapchain[NR_SWAPCHAIN_IMAGES];
	struct frame current; // On the screen
	// Committed, until its page flip. The current frame again for commits
	// that only change the cursor plane.
	struct frame pending;
	// Prepared ahead while a page flip is pending, and committed after it.
	struct frame queued;
	// The presentation feedbacks of the surfaces that the pending frame
	// shows, presented at its page flip. A frame that KMS rejects leaves
	// them to the frame drawn again.
	struct wl_list pending_feedbacks; // wp_presentation_feedback resource links
	// Those of the queued frame, which become pending when it is committed.
	struct wl_list queued_feedbacks; // wp_presentation_feedback resource links
	int64_t last_flip; // CLOCK_MONOTONIC ns, at the end of the vblank
	// Predicted from the last frames, from starting to draw one until the
	// GPU finishes it, in ns.
	int64_t render_time;
	// Counts up to SCANOUT_FEEDBACK_FRAMES for frames with a surface that
	// may be scanned out, and back down to 0 for frames without one, when
	// the scanout dma-buf feedback is withdrawn.
	int scanout_frames;
	// The box in the layout, in logical pixels. The mode has the size in
	// physical pixels.
	struct box box;
	// The box when the windows were last laid out, which they move along
	// from after the outputs change.
	struct box last_box;
	// Physical pixels per logical pixel. A multiple of 1/120, which
	// fractional-scale-v1 tells clients.
	double scale;
	// Applied to the layout to draw the images, like wl_output.transform.
	enum wl_output_transform transform;
	// Loaded and drawn for the scale while enabled.
	struct server_cursor server_cursors[NR_CURSOR_SHAPES];
	// By config.decorations.titlebar.button_styles
	struct texture *button_icons[NR_BUTTON_TYPES][NR_BUTTON_STATES][2];
	struct box usable_area; // Not covered by exclusive zones of layer surfaces.
	// layer_surface.link, bottom-to-top, by zwlr_layer_shell_v1.layer
	struct wl_list layer_surfaces[NR_LAYER_SHELL_LAYERS];
	// Contains the surfaces of the top layer on the output. Disabled while a
	// fullscreen window or one of its children is the topmost window on the
	// output, so that the window covers the panels.
	struct scene_tree top_layer; // In SCENE_LAYER_TOP
	struct lock_surface *lock_surface; // NULL unless the session is locked
	struct wl_list capture_sources; // capture_source.link
	struct wl_list capture_sessions; // capture_session.link
	// The images of the swapchain have damage, which the next frame draws.
	bool needs_repaint;
	// Surfaces on the output wait for frame callbacks or presentation
	// feedbacks, which the next frame sends even if it has nothing to draw.
	bool needs_frame;
	// The cursor plane or the gamma LUT has changed since the last commit.
	bool needs_commit;
	bool modeset_done; // Cleared when the mode changes.
};

void outputs_init(void);
void outputs_update(void);
void outputs_reload(void);
void output_connect(struct output *output);
void output_disconnect(struct output *output);
struct output *output_nearest(double x, double y);
struct output *output_find(const char *name);
struct box output_physical_box(struct output *output);
int mode_refresh(const drmModeModeInfo *mode);
int64_t mode_refresh_interval(const drmModeModeInfo *mode);
const drmModeModeInfo *output_find_mode(struct output *output, int width,
	int height, int refresh);
struct output_state output_current_state(struct output *output);
void output_configure(const struct output_state *state);

void output_management_add_output(struct output *output);
void output_management_remove_output(struct output *output);
void output_management_update(void);

void color_management_output_changed(struct output *output);

void night_light_init(void);
void night_light_update(void);
void night_light_update_output(struct output *output);
void night_light_fini(void);

#endif
