#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "core.h"
#include "surface.h"
#include "xdg-decoration-unstable-v1-protocol.h"
#include "xdg-shell-protocol.h"
#include "xdg-output-unstable-v1-protocol.h"
#include "server-decoration-protocol.h"
#include "wlr-layer-shell-unstable-v1-protocol.h"
#include "ext-foreign-toplevel-list-v1-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-protocol.h"
#include "ext-workspace-v1-protocol.h"
#include "wlr-output-management-unstable-v1-protocol.h"
#include "viewporter-protocol.h"
#include "fractional-scale-v1-protocol.h"
#include "presentation-time-protocol.h"
#include "tearing-control-v1-protocol.h"
#include "color-management-v1-protocol.h"
#include "xdg-toplevel-icon-v1-protocol.h"
#include "xdg-activation-v1-protocol.h"
#include "xdg-dialog-v1-protocol.h"
#include "ext-image-capture-source-v1-protocol.h"
#include "ext-image-copy-capture-v1-protocol.h"
#include "pointer-gestures-unstable-v1-protocol.h"
#include "relative-pointer-unstable-v1-protocol.h"
#include "pointer-constraints-unstable-v1-protocol.h"
#include "tablet-v2-protocol.h"
#include "text-input-unstable-v3-protocol.h"
#include "input-method-unstable-v1-protocol.h"
#include "virtual-keyboard-unstable-v1-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-protocol.h"
#include "linux-dmabuf-v1-protocol.h"
#include "linux-drm-syncobj-v1-protocol.h"
#include "primary-selection-unstable-v1-protocol.h"
#include "ext-data-control-v1-protocol.h"
#include "ext-session-lock-v1-protocol.h"
#include "ext-idle-notify-v1-protocol.h"
#include "idle-inhibit-unstable-v1-protocol.h"

static void
fixes_destroy_registry_req(struct wl_client *client,
	struct wl_resource *resource, struct wl_resource *registry)
{
	wl_resource_destroy(registry);
}

// The removed global is destroyed once all the clients acknowledge it.
static void
fixes_ack_global_remove_req(struct wl_client *client,
	struct wl_resource *resource, struct wl_resource *registry, uint32_t name)
{
	wl_fixes_handle_ack_global_remove(resource, registry, name);
}

static const struct wl_fixes_interface fixes_impl = {
	.destroy = resource_destroy_req,
	.destroy_registry = fixes_destroy_registry_req,
	.ack_global_remove = fixes_ack_global_remove_req,
};

static void
bind_fixes(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&wl_fixes_interface, MIN(version, 2), id);
	assert(resource);
	wl_resource_set_implementation(resource, &fixes_impl, NULL, NULL);
}

// Logs the messages to /tmp/jimi.log when JIMI_PROTOCOL_LOG is set. Like
// WAYLAND_DEBUG=server, but to a file, so that they are not mixed with our
// messages on stderr.
static void
log_protocol_message(void *data, enum wl_protocol_logger_type type,
	const struct wl_protocol_logger_message *message)
{
	FILE *log = data;
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	// Xwayland has our pid, as we create its socket.
	pid_t pid;
	wl_client_get_credentials(wl_resource_get_client(message->resource),
		&pid, NULL, NULL);
	fprintf(log, "[%7lld.%03ld] {pid %d} %s%s#%u.%s(",
		(long long)now.tv_sec * 1000 + now.tv_nsec / 1000000,
		now.tv_nsec / 1000 % 1000, pid,
		type == WL_PROTOCOL_LOGGER_EVENT ? " -> " : "",
		wl_resource_get_class(message->resource),
		wl_resource_get_id(message->resource), message->message->name);

	const char *signature = message->message->signature;
	for (int i = 0; i < message->arguments_count; i++) {
		// Skips the version and the nullability.
		while (isdigit(*signature) || *signature == '?') {
			signature++;
		}
		const union wl_argument *arg = &message->arguments[i];
		const struct wl_interface *interface = message->message->types[i];
		fputs(i > 0 ? ", " : "", log);
		switch (*signature++) {
		case 'i':
			fprintf(log, "%d", arg->i);
			break;
		case 'u':
			fprintf(log, "%u", arg->u);
			break;
		case 'f':
			fprintf(log, "%f", wl_fixed_to_double(arg->f));
			break;
		case 's':
			fprintf(log, arg->s ? "\"%s\"" : "nil", arg->s);
			break;
		case 'o':
			// The objects of a server are the first members of the
			// resources.
			if (arg->o) {
				struct wl_resource *resource = (struct wl_resource *)arg->o;
				fprintf(log, "%s#%u", wl_resource_get_class(resource),
					wl_resource_get_id(resource));
			} else {
				fputs("nil", log);
			}
			break;
		case 'n':
			fprintf(log, "new id %s#%u",
				interface ? interface->name : "[unknown]", arg->n);
			break;
		case 'a':
			fprintf(log, "array[%zu]", arg->a->size);
			break;
		case 'h':
			fprintf(log, "fd %d", arg->h);
			break;
		}
	}
	fputs(")\n", log);

	// Starts over at 100 MiB, as /tmp is often in memory.
	if (ftell(log) > 100 << 20) {
		rewind(log);
		assert(ftruncate(fileno(log), 0) == 0);
	}
}

void
wayland_init(void)
{
	if (getenv("JIMI_PROTOCOL_LOG")) {
		FILE *log = fopen("/tmp/jimi.log", "w");
		assert(log);
		// Keeps the messages until the crash.
		setvbuf(log, NULL, _IOLBF, 0);
		assert(wl_display_add_protocol_logger(core.display,
			log_protocol_message, log));
	}
	scene_init();
	// Colored by the config when shown
	scene_rect_init(&core.snap_preview, &core.scene.layers[SCENE_LAYER_TOPLEVEL],
		(struct color){0}, 0.0f, 0);
	scene_node_set_enabled(&core.snap_preview.node, false);
	window_switcher_init();
	wl_list_init(&core.surfaces);
	wl_list_init(&core.transactions);
	wl_list_init(&core.windows);
	wl_list_init(&core.workspaces.managers);
	wl_list_init(&core.server_decorations);
	wl_list_init(&core.output_management.managers);
	wl_list_init(&core.seat.touch.resources);
	wl_list_init(&core.seat.tablets.seats);
	wl_list_init(&core.seat.tablets.tablets);
	wl_list_init(&core.seat.tablets.tools);
	wl_list_init(&core.seat.tablets.pads);
	wl_list_init(&core.seat.text_inputs);
	wl_list_init(&core.seat.input_method.resources);
	wl_list_init(&core.seat.input_method.panels);
	wl_list_init(&core.idle.notifications);
	wl_list_init(&core.idle.inhibitors);
	data_device_init();
	foreign_toplevel_init();
	assert(wl_display_init_shm(core.display) == 0);
	// libwayland advertises ARGB8888 and XRGB8888 itself.
	for (size_t i = 0; i < ARRAY_LEN(pixel_formats); i++) {
		const struct pixel_format *format = &pixel_formats[i];
		if (format_support(format)->shm
				&& format->shm != WL_SHM_FORMAT_ARGB8888
				&& format->shm != WL_SHM_FORMAT_XRGB8888) {
			wl_display_add_shm_format(core.display, format->shm);
		}
	}
	linux_dmabuf_init();
	assert(wl_global_create(core.display, &zwp_linux_dmabuf_v1_interface, 4,
		NULL, wayland_bind_linux_dmabuf));
	if (linux_drm_syncobj_supported()) {
		assert(wl_global_create(core.display,
			&wp_linux_drm_syncobj_manager_v1_interface, 1, NULL,
			wayland_bind_linux_drm_syncobj_manager));
	}
	assert(wl_global_create(core.display, &wl_fixes_interface, 2, NULL, bind_fixes));
	assert(wl_global_create(core.display, &wl_compositor_interface, 7, NULL,
		wayland_bind_compositor));
	assert(wl_global_create(core.display, &wl_subcompositor_interface, 1, NULL,
		wayland_bind_subcompositor));
	assert(wl_global_create(core.display, &wp_viewporter_interface, 1, NULL,
		wayland_bind_viewporter));
	assert(wl_global_create(core.display,
		&wp_fractional_scale_manager_v1_interface, 1, NULL,
		wayland_bind_fractional_scale_manager));
	assert(wl_global_create(core.display, &wp_presentation_interface, 2, NULL,
		wayland_bind_presentation));
	if (core.kms.async_page_flip) {
		assert(wl_global_create(core.display,
			&wp_tearing_control_manager_v1_interface, 1, NULL,
			wayland_bind_tearing_control_manager));
	}
	assert(wl_global_create(core.display, &wp_color_manager_v1_interface, 3,
		NULL, wayland_bind_color_manager));
	assert(wl_global_create(core.display, &wl_seat_interface, SEAT_VERSION, NULL,
		wayland_bind_seat));
	assert(wl_global_create(core.display, &wl_data_device_manager_interface,
		DATA_DEVICE_MANAGER_VERSION, NULL, wayland_bind_data_device_manager));
	assert(wl_global_create(core.display,
		&zwp_primary_selection_device_manager_v1_interface, 1, NULL,
		wayland_bind_primary_selection_device_manager));
	assert(wl_global_create(core.display,
		&ext_data_control_manager_v1_interface, 1, NULL,
		wayland_bind_data_control_manager));
	assert(wl_global_create(core.display,
		&wp_cursor_shape_manager_v1_interface, CURSOR_SHAPE_MANAGER_VERSION,
		NULL, wayland_bind_cursor_shape_manager));
	assert(wl_global_create(core.display, &zwp_pointer_gestures_v1_interface,
		POINTER_GESTURES_VERSION, NULL, wayland_bind_pointer_gestures));
	assert(wl_global_create(core.display,
		&zwp_relative_pointer_manager_v1_interface, 1, NULL,
		wayland_bind_relative_pointer_manager));
	assert(wl_global_create(core.display,
		&zwp_pointer_constraints_v1_interface, 1, NULL,
		wayland_bind_pointer_constraints));
	assert(wl_global_create(core.display, &zwp_tablet_manager_v2_interface,
		TABLET_MANAGER_VERSION, NULL, wayland_bind_tablet_manager));
	assert(wl_global_create(core.display,
		&zwp_text_input_manager_v3_interface, 1, NULL,
		wayland_bind_text_input_manager));
	assert(wl_global_create(core.display, &zwp_input_method_v1_interface, 1,
		NULL, wayland_bind_input_method));
	assert(wl_global_create(core.display, &zwp_input_panel_v1_interface, 1,
		NULL, wayland_bind_input_panel));
	assert(wl_global_create(core.display,
		&zwp_virtual_keyboard_manager_v1_interface, 1, NULL,
		wayland_bind_virtual_keyboard_manager));
	assert(wl_global_create(core.display,
		&zwlr_virtual_pointer_manager_v1_interface, 2, NULL,
		wayland_bind_virtual_pointer_manager));
	assert(wl_global_create(core.display, &xdg_wm_base_interface, 6, NULL,
		wayland_bind_xdg_wm_base));
	assert(wl_global_create(core.display,
		&zxdg_decoration_manager_v1_interface, 2, NULL,
		wayland_bind_xdg_decoration_manager));
	assert(wl_global_create(core.display,
		&xdg_toplevel_icon_manager_v1_interface, 1, NULL,
		wayland_bind_xdg_toplevel_icon_manager));
	assert(wl_global_create(core.display, &xdg_activation_v1_interface, 1,
		NULL, wayland_bind_xdg_activation));
	assert(wl_global_create(core.display, &xdg_wm_dialog_v1_interface, 1,
		NULL, wayland_bind_xdg_wm_dialog));
	assert(wl_global_create(core.display,
		&org_kde_kwin_server_decoration_manager_interface, 1, NULL,
		wayland_bind_server_decoration_manager));
	assert(wl_global_create(core.display, &zwlr_layer_shell_v1_interface, 5,
		NULL, wayland_bind_layer_shell));
	assert(wl_global_create(core.display,
		&ext_session_lock_manager_v1_interface, 1, NULL,
		wayland_bind_session_lock_manager));
	assert(wl_global_create(core.display, &ext_idle_notifier_v1_interface, 2,
		NULL, wayland_bind_idle_notifier));
	assert(wl_global_create(core.display,
		&zwp_idle_inhibit_manager_v1_interface, 1, NULL,
		wayland_bind_idle_inhibit_manager));
	assert(wl_global_create(core.display,
		&ext_foreign_toplevel_list_v1_interface, 1, NULL,
		wayland_bind_ext_foreign_toplevel_list));
	assert(wl_global_create(core.display,
		&zwlr_foreign_toplevel_manager_v1_interface, 3, NULL,
		wayland_bind_wlr_foreign_toplevel_manager));
	assert(wl_global_create(core.display, &ext_workspace_manager_v1_interface,
		1, NULL, wayland_bind_ext_workspace_manager));
	assert(wl_global_create(core.display, &zxdg_output_manager_v1_interface, 3, NULL,
		wayland_bind_xdg_output_manager));
	assert(wl_global_create(core.display, &zwlr_output_manager_v1_interface, 4, NULL,
		wayland_bind_wlr_output_manager));
	assert(wl_global_create(core.display,
		&ext_output_image_capture_source_manager_v1_interface, 1, NULL,
		wayland_bind_output_image_capture_source_manager));
	assert(wl_global_create(core.display,
		&ext_foreign_toplevel_image_capture_source_manager_v1_interface, 1, NULL,
		wayland_bind_foreign_toplevel_image_capture_source_manager));
	assert(wl_global_create(core.display,
		&ext_image_copy_capture_manager_v1_interface, 1, NULL,
		wayland_bind_image_copy_capture_manager));
	core.socket = wl_display_add_socket_auto(core.display);
	assert(core.socket);
	setenv("WAYLAND_DISPLAY", core.socket, 1);
	printf("WAYLAND_DISPLAY=%s\n", core.socket);
	fflush(stdout);
	if (config.backend.xwayland) {
		xwayland_init();
	}
}

void
wayland_fini(void)
{
	wl_display_destroy_clients(core.display);
	assert(wl_list_empty(&core.transactions));
	xwayland_fini();
	data_device_fini();
	linux_dmabuf_fini();
}
