#include <assert.h>
#include <stdlib.h>

#include "config.h"
#include "core.h"
#include "output.h"
#include "surface.h"
#include "ext-workspace-v1-protocol.h"

// Each window is on one of the workspaces of config.workspaces.names, or on all
// of them if sticky, and all outputs show the same one. The clients of
// ext-workspace-v1 see them in a single group of all outputs, and can switch
// between them.

// The objects of a manager resource: the group, and the handle of each
// workspace. They become inert, without user data, once the manager is
// destroyed.
struct workspace_manager {
	struct wl_list link; // core.workspaces.managers
	struct wl_resource *resource;
	struct wl_resource *group; // NULL once destroyed
	struct wl_resource **handles; // By workspace, NULL once destroyed
	int pending; // Activated by the next commit, or -1
};

// Shows the windows on the shown workspace, and hides the others, which lose
// their popups. Switching windows ends, since it lists the windows on the
// workspace.
static void
update_visibility(void)
{
	if (core.window_switcher.selected) {
		window_switcher_cancel();
	}
	struct window *window;
	wl_list_for_each(window, &core.windows, link) {
		window_update_visibility(window);
		if (window->mapped && !window_visible(window)) {
			xdg_popups_dismiss(window->surface);
		}
	}
}

// Along with its parents and children, which are on the same workspace.
static void
set_family_workspace(struct window *window, int workspace)
{
	struct window *root = window;
	while (root->parent) {
		root = root->parent;
	}
	struct window *iter;
	wl_list_for_each(iter, &core.windows, link) {
		if (window_has_ancestor(iter, root)) {
			iter->workspace = workspace;
			// The sticky button shows whether the window is sticky.
			decoration_update_buttons(iter);
		}
	}
}

bool
window_on_shown_workspace(struct window *window)
{
	return window->workspace == core.workspaces.current
		|| window->workspace == WORKSPACE_STICKY;
}

// The window leaves along with its parents and children, without switching
// to the workspace.
void
window_set_workspace(struct window *window, int workspace)
{
	if (window->workspace == workspace) {
		return;
	}
	set_family_workspace(window, workspace);
	update_visibility();
	keyboard_update_focus();
}

// Windows that are no longer sticky stay on the shown workspace.
void
window_set_sticky(struct window *window, bool sticky)
{
	if ((window->workspace == WORKSPACE_STICKY) == sticky) {
		return;
	}
	window_set_workspace(window,
		sticky ? WORKSPACE_STICKY : core.workspaces.current);
}

static void
send_state(struct wl_resource *handle, int workspace)
{
	ext_workspace_handle_v1_send_state(handle,
		workspace == core.workspaces.current
			? EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE : 0);
}

// Without moving the keyboard focus. A window moved by the pointer comes
// along, unless it is sticky.
void
workspace_show(int workspace)
{
	core.workspaces.current = workspace;
	struct pointer_grab *grab = &core.seat.pointer.grab;
	if (grab->type == POINTER_GRAB_MOVE
			&& grab->move.window->workspace != WORKSPACE_STICKY) {
		set_family_workspace(grab->move.window, workspace);
	}
	update_visibility();
	struct workspace_manager *manager;
	wl_list_for_each(manager, &core.workspaces.managers, link) {
		for (size_t i = 0; i < config.workspaces.nr_names; i++) {
			if (manager->handles[i]) {
				send_state(manager->handles[i], (int)i);
			}
		}
		ext_workspace_manager_v1_send_done(manager->resource);
	}
}

// The focus returns to the window which had it last on the workspace.
void
workspace_switch(int workspace)
{
	if (workspace == core.workspaces.current) {
		return;
	}
	workspace_show(workspace);
	keyboard_update_focus();
}

// The group has the outputs of the client.
static void
group_send_output(struct wl_resource *group, struct output *output, bool enter)
{
	struct wl_client *client = wl_resource_get_client(group);
	struct wl_resource *output_resource;
	wl_resource_for_each(output_resource, &output->resources) {
		if (wl_resource_get_client(output_resource) != client) {
			continue;
		}
		if (enter) {
			ext_workspace_group_handle_v1_send_output_enter(group, output_resource);
		} else {
			ext_workspace_group_handle_v1_send_output_leave(group, output_resource);
		}
	}
}

// Clients may bind the wl_output after they received the group.
void
workspaces_output_bind(struct output *output, struct wl_resource *output_resource)
{
	struct wl_client *client = wl_resource_get_client(output_resource);
	struct workspace_manager *manager;
	wl_list_for_each(manager, &core.workspaces.managers, link) {
		if (manager->group && wl_resource_get_client(manager->group) == client) {
			ext_workspace_group_handle_v1_send_output_enter(
				manager->group, output_resource);
			ext_workspace_manager_v1_send_done(manager->resource);
		}
	}
}

// Before its resources become inert
void
workspaces_output_disable(struct output *output)
{
	struct workspace_manager *manager;
	wl_list_for_each(manager, &core.workspaces.managers, link) {
		if (manager->group) {
			group_send_output(manager->group, output, false);
			ext_workspace_manager_v1_send_done(manager->resource);
		}
	}
}

static void
group_create_workspace_req(struct wl_client *client,
	struct wl_resource *resource, const char *name)
{
	// The workspaces are configured.
}

static const struct ext_workspace_group_handle_v1_interface group_impl = {
	.create_workspace = group_create_workspace_req,
	.destroy = resource_destroy_req,
};

static void
group_resource_destroy(struct wl_resource *resource)
{
	struct workspace_manager *manager = wl_resource_get_user_data(resource);
	if (manager) {
		manager->group = NULL;
	}
}

static int
handle_workspace(struct workspace_manager *manager, struct wl_resource *handle)
{
	for (size_t i = 0; i < config.workspaces.nr_names; i++) {
		if (manager->handles[i] == handle) {
			return (int)i;
		}
	}
	assert(!"handle of no workspace");
	return -1;
}

static void
handle_activate_req(struct wl_client *client, struct wl_resource *resource)
{
	struct workspace_manager *manager = wl_resource_get_user_data(resource);
	if (manager) {
		manager->pending = handle_workspace(manager, resource);
	}
}

// One workspace is always shown, so they are only deactivated by activating
// another.
static void
handle_deactivate_req(struct wl_client *client, struct wl_resource *resource)
{
}

// There is only one group.
static void
handle_assign_req(struct wl_client *client, struct wl_resource *resource,
	struct wl_resource *group)
{
}

// The workspaces are configured.
static void
handle_remove_req(struct wl_client *client, struct wl_resource *resource)
{
}

static const struct ext_workspace_handle_v1_interface handle_impl = {
	.destroy = resource_destroy_req,
	.activate = handle_activate_req,
	.deactivate = handle_deactivate_req,
	.assign = handle_assign_req,
	.remove = handle_remove_req,
};

static void
handle_resource_destroy(struct wl_resource *resource)
{
	struct workspace_manager *manager = wl_resource_get_user_data(resource);
	if (manager) {
		manager->handles[handle_workspace(manager, resource)] = NULL;
	}
}

static void
manager_commit_req(struct wl_client *client, struct wl_resource *resource)
{
	struct workspace_manager *manager = wl_resource_get_user_data(resource);
	if (manager->pending >= 0) {
		workspace_switch(manager->pending);
		manager->pending = -1;
	}
}

static void
manager_stop_req(struct wl_client *client, struct wl_resource *resource)
{
	ext_workspace_manager_v1_send_finished(resource);
	wl_resource_destroy(resource);
}

static const struct ext_workspace_manager_v1_interface manager_impl = {
	.commit = manager_commit_req,
	.stop = manager_stop_req,
};

static void
manager_resource_destroy(struct wl_resource *resource)
{
	struct workspace_manager *manager = wl_resource_get_user_data(resource);
	wl_list_remove(&manager->link);
	if (manager->group) {
		wl_resource_set_user_data(manager->group, NULL);
	}
	for (size_t i = 0; i < config.workspaces.nr_names; i++) {
		if (manager->handles[i]) {
			wl_resource_set_user_data(manager->handles[i], NULL);
		}
	}
	free(manager->handles);
	free(manager);
}

static struct wl_resource *
handle_create(struct workspace_manager *manager, int workspace)
{
	struct wl_resource *handle = wl_resource_create(
		wl_resource_get_client(manager->resource),
		&ext_workspace_handle_v1_interface,
		wl_resource_get_version(manager->resource), 0);
	assert(handle);
	wl_resource_set_implementation(handle, &handle_impl, manager,
		handle_resource_destroy);
	ext_workspace_manager_v1_send_workspace(manager->resource, handle);
	ext_workspace_handle_v1_send_name(handle, config.workspaces.names[workspace]);
	// Ordered in one dimension, without a geometric meaning
	uint32_t coordinate = (uint32_t)workspace;
	struct wl_array coordinates = {
		.size = sizeof(coordinate),
		.data = &coordinate,
	};
	ext_workspace_handle_v1_send_coordinates(handle, &coordinates);
	send_state(handle, workspace);
	ext_workspace_handle_v1_send_capabilities(handle,
		EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE);
	if (manager->group) {
		ext_workspace_group_handle_v1_send_workspace_enter(manager->group, handle);
	}
	return handle;
}

// Applies the workspaces of the config, which had nr_old of them before. The
// windows on the removed workspaces move to the last one, which is also shown
// instead of a removed one. The clients get the added and the removed
// workspaces, and the names.
void
workspaces_reload(size_t nr_old)
{
	size_t nr = config.workspaces.nr_names;
	int last = (int)nr - 1;
	struct window *window;
	wl_list_for_each(window, &core.windows, link) {
		window->workspace = MIN(window->workspace, last);
	}
	struct workspace_manager *manager;
	wl_list_for_each(manager, &core.workspaces.managers, link) {
		for (size_t i = nr; i < nr_old; i++) {
			struct wl_resource *handle = manager->handles[i];
			if (!handle) {
				continue;
			}
			if (manager->group) {
				ext_workspace_group_handle_v1_send_workspace_leave(
					manager->group, handle);
			}
			ext_workspace_handle_v1_send_removed(handle);
			wl_resource_set_user_data(handle, NULL);
		}
		manager->handles = realloc(manager->handles, nr * sizeof(*manager->handles));
		assert(manager->handles);
		for (size_t i = 0; i < MIN(nr, nr_old); i++) {
			if (manager->handles[i]) {
				ext_workspace_handle_v1_send_name(manager->handles[i],
					config.workspaces.names[i]);
			}
		}
		for (size_t i = nr_old; i < nr; i++) {
			manager->handles[i] = handle_create(manager, (int)i);
		}
		manager->pending = MIN(manager->pending, last);
	}
	// Also tells the clients the states.
	workspace_show(MIN(core.workspaces.current, last));
	keyboard_update_focus();
}

void
wayland_bind_ext_workspace_manager(struct wl_client *client, void *data,
	uint32_t version, uint32_t id)
{
	struct workspace_manager *manager = calloc(1, sizeof(*manager));
	assert(manager);
	manager->handles = calloc(config.workspaces.nr_names, sizeof(*manager->handles));
	assert(manager->handles);
	manager->pending = -1;
	manager->resource = wl_resource_create(client,
		&ext_workspace_manager_v1_interface, MIN(version, 1), id);
	assert(manager->resource);
	wl_resource_set_implementation(manager->resource, &manager_impl, manager,
		manager_resource_destroy);
	list_append(&core.workspaces.managers, &manager->link);

	manager->group = wl_resource_create(client,
		&ext_workspace_group_handle_v1_interface,
		wl_resource_get_version(manager->resource), 0);
	assert(manager->group);
	wl_resource_set_implementation(manager->group, &group_impl, manager,
		group_resource_destroy);
	ext_workspace_manager_v1_send_workspace_group(manager->resource, manager->group);
	ext_workspace_group_handle_v1_send_capabilities(manager->group, 0);
	struct output *output;
	wl_list_for_each(output, &core.outputs, link) {
		group_send_output(manager->group, output, true);
	}
	for (size_t i = 0; i < config.workspaces.nr_names; i++) {
		manager->handles[i] = handle_create(manager, (int)i);
	}
	ext_workspace_manager_v1_send_done(manager->resource);
}
