#ifndef JIMI_PROTOCOLS_H
#define JIMI_PROTOCOLS_H

#include <sys/types.h>

#include "util.h"

struct dmabuf_format_modifier;

struct dmabuf_feedback {
	// Each format with each of its modifiers, by format
	struct dmabuf_format_modifier *table;
	int table_fd;
	uint32_t table_size;
	uint32_t nr_formats; // Entries of the table
	dev_t device;
};

void wayland_init(void);
void wayland_fini(void);

void linux_dmabuf_init(void);
void linux_dmabuf_fini(void);
bool linux_drm_syncobj_supported(void);
void foreign_toplevel_init(void);
void xwayland_init(void);
void xwayland_fini(void);
void xwm_init(int wm_fd);
void xwm_fini(void);
void session_lock_send_locked(void);
void idle_notify_activity(void);
void idle_update_inhibited(void);

void wayland_bind_compositor(struct wl_client *client, void *data,
	uint32_t version, uint32_t id);
void wayland_bind_subcompositor(struct wl_client *client, void *data,
	uint32_t version, uint32_t id);
void wayland_bind_seat(struct wl_client *client, void *data,
	uint32_t version, uint32_t id);
void wayland_bind_data_device_manager(struct wl_client *client, void *data,
	uint32_t version, uint32_t id);
void wayland_bind_primary_selection_device_manager(struct wl_client *client,
	void *data, uint32_t version, uint32_t id);
void wayland_bind_data_control_manager(struct wl_client *client, void *data,
	uint32_t version, uint32_t id);
void wayland_bind_cursor_shape_manager(struct wl_client *client, void *data,
	uint32_t version, uint32_t id);
void wayland_bind_pointer_gestures(struct wl_client *client, void *data,
	uint32_t version, uint32_t id);
void wayland_bind_relative_pointer_manager(struct wl_client *client,
	void *data, uint32_t version, uint32_t id);
void wayland_bind_pointer_constraints(struct wl_client *client, void *data,
	uint32_t version, uint32_t id);
void wayland_bind_tablet_manager(struct wl_client *client, void *data,
	uint32_t version, uint32_t id);
void wayland_bind_text_input_manager(struct wl_client *client, void *data,
	uint32_t version, uint32_t id);
void wayland_bind_input_method(struct wl_client *client, void *data,
	uint32_t version, uint32_t id);
void wayland_bind_input_panel(struct wl_client *client, void *data,
	uint32_t version, uint32_t id);
void wayland_bind_virtual_keyboard_manager(struct wl_client *client,
	void *data, uint32_t version, uint32_t id);
void wayland_bind_virtual_pointer_manager(struct wl_client *client,
	void *data, uint32_t version, uint32_t id);
void wayland_bind_xdg_wm_base(struct wl_client *client, void *data,
	uint32_t version, uint32_t id);
void wayland_bind_xdg_decoration_manager(struct wl_client *client, void *data,
	uint32_t version, uint32_t id);
void wayland_bind_server_decoration_manager(struct wl_client *client,
	void *data, uint32_t version, uint32_t id);
void wayland_bind_layer_shell(struct wl_client *client, void *data,
	uint32_t version, uint32_t id);
void wayland_bind_ext_foreign_toplevel_list(struct wl_client *client,
	void *data, uint32_t version, uint32_t id);
void wayland_bind_wlr_foreign_toplevel_manager(struct wl_client *client,
	void *data, uint32_t version, uint32_t id);
void wayland_bind_ext_workspace_manager(struct wl_client *client, void *data,
	uint32_t version, uint32_t id);
void wayland_bind_session_lock_manager(struct wl_client *client, void *data,
	uint32_t version, uint32_t id);
void wayland_bind_idle_notifier(struct wl_client *client, void *data,
	uint32_t version, uint32_t id);
void wayland_bind_idle_inhibit_manager(struct wl_client *client, void *data,
	uint32_t version, uint32_t id);
void wayland_bind_xwayland_shell(struct wl_client *client, void *data,
	uint32_t version, uint32_t id);
void wayland_bind_linux_dmabuf(struct wl_client *client, void *data,
	uint32_t version, uint32_t id);
void wayland_bind_linux_drm_syncobj_manager(struct wl_client *client,
	void *data, uint32_t version, uint32_t id);
void wayland_bind_viewporter(struct wl_client *client, void *data,
	uint32_t version, uint32_t id);
void wayland_bind_fractional_scale_manager(struct wl_client *client,
	void *data, uint32_t version, uint32_t id);
void wayland_bind_presentation(struct wl_client *client, void *data,
	uint32_t version, uint32_t id);
void wayland_bind_tearing_control_manager(struct wl_client *client,
	void *data, uint32_t version, uint32_t id);
void wayland_bind_color_manager(struct wl_client *client, void *data,
	uint32_t version, uint32_t id);
void wayland_bind_xdg_toplevel_icon_manager(struct wl_client *client,
	void *data, uint32_t version, uint32_t id);
void wayland_bind_xdg_activation(struct wl_client *client, void *data,
	uint32_t version, uint32_t id);
void wayland_bind_xdg_wm_dialog(struct wl_client *client, void *data,
	uint32_t version, uint32_t id);
void wayland_bind_output(struct wl_client *client, void *data,
	uint32_t version, uint32_t id);
void wayland_bind_xdg_output_manager(struct wl_client *client, void *data,
	uint32_t version, uint32_t id);
void wayland_bind_wlr_output_manager(struct wl_client *client, void *data,
	uint32_t version, uint32_t id);
void wayland_bind_output_image_capture_source_manager(
	struct wl_client *client, void *data, uint32_t version, uint32_t id);
void wayland_bind_foreign_toplevel_image_capture_source_manager(
	struct wl_client *client, void *data, uint32_t version, uint32_t id);
void wayland_bind_image_copy_capture_manager(struct wl_client *client,
	void *data, uint32_t version, uint32_t id);

#endif
