#include <assert.h>
#include <limits.h>
#include <stdlib.h>

#include "core.h"
#include "ext-idle-notify-v1-protocol.h"
#include "idle-inhibit-unstable-v1-protocol.h"
#include "surface.h"

// Tells a client when the user has been inactive for the timeout, and when
// they are active again.
struct idle_notification {
	struct wl_resource *resource;
	struct wl_list link; // core.idle.notifications
	uint32_t timeout; // ms
	// Idles even while the inhibitors are active, as only the input of
	// the user counts.
	bool ignores_inhibitors;
	struct wl_event_source *timer;
	bool idle;
};

// Counts the timeout from now, unless the notification is idle already or
// inhibited.
static void
idle_notification_arm(struct idle_notification *notification)
{
	bool inhibited = core.idle.inhibited && !notification->ignores_inhibitors;
	// The timer is disarmed by 0, so a timeout of 0 idles after 1 ms.
	int ms = notification->idle || inhibited ? 0
		: (int)MIN(MAX(notification->timeout, 1), INT_MAX);
	wl_event_source_timer_update(notification->timer, ms);
}

static int
handle_idle_timer(void *data)
{
	struct idle_notification *notification = data;
	notification->idle = true;
	ext_idle_notification_v1_send_idled(notification->resource);
	return 0;
}

// Any input of the user resumes the notifications, and counts their timeouts
// again.
void
idle_notify_activity(void)
{
	struct idle_notification *notification;
	wl_list_for_each(notification, &core.idle.notifications, link) {
		if (notification->idle) {
			notification->idle = false;
			ext_idle_notification_v1_send_resumed(notification->resource);
		}
		idle_notification_arm(notification);
	}
}

static void
idle_notification_resource_destroy(struct wl_resource *resource)
{
	struct idle_notification *notification = wl_resource_get_user_data(resource);
	wl_list_remove(&notification->link);
	wl_event_source_remove(notification->timer);
	free(notification);
}

static const struct ext_idle_notification_v1_interface idle_notification_impl = {
	.destroy = resource_destroy_req,
};

// There is only one seat.
static void
create_idle_notification(struct wl_client *client,
	struct wl_resource *resource, uint32_t id, uint32_t timeout,
	bool ignores_inhibitors)
{
	struct idle_notification *notification = calloc(1, sizeof(*notification));
	assert(notification);
	notification->resource = wl_resource_create(client,
		&ext_idle_notification_v1_interface, wl_resource_get_version(resource), id);
	assert(notification->resource);
	wl_resource_set_implementation(notification->resource,
		&idle_notification_impl, notification, idle_notification_resource_destroy);
	notification->timeout = timeout;
	notification->ignores_inhibitors = ignores_inhibitors;
	notification->timer = wl_event_loop_add_timer(core.wl_loop,
		handle_idle_timer, notification);
	assert(notification->timer);
	list_append(&core.idle.notifications, &notification->link);
	idle_notification_arm(notification);
}

static void
idle_notifier_get_idle_notification_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id, uint32_t timeout,
	struct wl_resource *seat)
{
	create_idle_notification(client, resource, id, timeout, false);
}

static void
idle_notifier_get_input_idle_notification_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id, uint32_t timeout,
	struct wl_resource *seat)
{
	create_idle_notification(client, resource, id, timeout, true);
}

static const struct ext_idle_notifier_v1_interface idle_notifier_impl = {
	.destroy = resource_destroy_req,
	.get_idle_notification = idle_notifier_get_idle_notification_req,
	.get_input_idle_notification = idle_notifier_get_input_idle_notification_req,
};

void
wayland_bind_idle_notifier(struct wl_client *client, void *data,
	uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&ext_idle_notifier_v1_interface, MIN(version, 2), id);
	assert(resource);
	wl_resource_set_implementation(resource, &idle_notifier_impl, data, NULL);
}

// The inhibitors keep the notifications that honor them from idling while
// their surfaces are shown. Called once per event loop iteration, after the
// surfaces are told the outputs that they are on.
void
idle_update_inhibited(void)
{
	bool inhibited = false;
	struct wl_resource *resource;
	wl_resource_for_each(resource, &core.idle.inhibitors) {
		struct surface *surface = wl_resource_get_user_data(resource);
		if (surface && surface->outputs) {
			inhibited = true;
			break;
		}
	}
	if (inhibited == core.idle.inhibited) {
		return;
	}
	core.idle.inhibited = inhibited;
	struct idle_notification *notification;
	wl_list_for_each(notification, &core.idle.notifications, link) {
		if (!notification->ignores_inhibitors) {
			idle_notification_arm(notification);
		}
	}
}

// The inhibitors of the surface stay inert.
void
idle_inhibitors_surface_destroy(struct surface *surface)
{
	struct wl_resource *resource;
	wl_resource_for_each(resource, &core.idle.inhibitors) {
		if (wl_resource_get_user_data(resource) == surface) {
			wl_resource_set_user_data(resource, NULL);
		}
	}
}

static const struct zwp_idle_inhibitor_v1_interface idle_inhibitor_impl = {
	.destroy = resource_destroy_req,
};

static void
idle_inhibit_manager_create_inhibitor_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id, struct wl_resource *surface_resource)
{
	struct wl_resource *inhibitor = wl_resource_create(client,
		&zwp_idle_inhibitor_v1_interface, wl_resource_get_version(resource), id);
	assert(inhibitor);
	wl_resource_set_implementation(inhibitor, &idle_inhibitor_impl,
		wl_resource_get_user_data(surface_resource), resource_unlink);
	list_append(&core.idle.inhibitors, wl_resource_get_link(inhibitor));
}

static const struct zwp_idle_inhibit_manager_v1_interface idle_inhibit_manager_impl = {
	.destroy = resource_destroy_req,
	.create_inhibitor = idle_inhibit_manager_create_inhibitor_req,
};

void
wayland_bind_idle_inhibit_manager(struct wl_client *client, void *data,
	uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&zwp_idle_inhibit_manager_v1_interface, 1, id);
	assert(resource);
	wl_resource_set_implementation(resource, &idle_inhibit_manager_impl, data, NULL);
}
