#include <assert.h>

#include "core.h"
#include "surface.h"
#include "xdg-dialog-v1-protocol.h"

// Lets clients mark their child toplevels as modal dialogs, which take the
// keyboard focus from their parent and are left out of the window switcher.
// The dialog object becomes inert when its toplevel is destroyed.

static void
dialog_resource_destroy(struct wl_resource *resource)
{
	struct xdg_toplevel *toplevel = wl_resource_get_user_data(resource);
	if (!toplevel) {
		return;
	}
	toplevel->dialog = NULL;
	toplevel->window.modal = false;
}

static void
dialog_set_modal(struct wl_resource *resource, bool modal)
{
	struct xdg_toplevel *toplevel = wl_resource_get_user_data(resource);
	if (toplevel) {
		toplevel->window.modal = modal;
	}
}

static void
dialog_set_modal_req(struct wl_client *client, struct wl_resource *resource)
{
	dialog_set_modal(resource, true);
}

static void
dialog_unset_modal_req(struct wl_client *client, struct wl_resource *resource)
{
	dialog_set_modal(resource, false);
}

static const struct xdg_dialog_v1_interface dialog_impl = {
	.destroy = resource_destroy_req,
	.set_modal = dialog_set_modal_req,
	.unset_modal = dialog_unset_modal_req,
};

static void
wm_dialog_get_xdg_dialog_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id, struct wl_resource *toplevel_resource)
{
	struct xdg_toplevel *toplevel = wl_resource_get_user_data(toplevel_resource);
	if (toplevel && toplevel->dialog) {
		wl_resource_post_error(resource, XDG_WM_DIALOG_V1_ERROR_ALREADY_USED,
			"xdg_toplevel already has a dialog object");
		return;
	}
	struct wl_resource *dialog_resource = wl_resource_create(client,
		&xdg_dialog_v1_interface, wl_resource_get_version(resource), id);
	assert(dialog_resource);
	wl_resource_set_implementation(dialog_resource, &dialog_impl, toplevel,
		dialog_resource_destroy);
	if (toplevel) {
		toplevel->dialog = dialog_resource;
	}
}

static const struct xdg_wm_dialog_v1_interface wm_dialog_impl = {
	.destroy = resource_destroy_req,
	.get_xdg_dialog = wm_dialog_get_xdg_dialog_req,
};

void
wayland_bind_xdg_wm_dialog(struct wl_client *client, void *data,
	uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&xdg_wm_dialog_v1_interface, MIN(version, 1), id);
	assert(resource);
	wl_resource_set_implementation(resource, &wm_dialog_impl, NULL, NULL);
}

void
xdg_dialog_toplevel_destroy(struct xdg_toplevel *toplevel)
{
	if (toplevel->dialog) {
		wl_resource_set_user_data(toplevel->dialog, NULL);
	}
}
