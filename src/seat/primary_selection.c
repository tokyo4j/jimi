#include <assert.h>

#include "core.h"
#include "primary-selection-unstable-v1-protocol.h"

static const struct zwp_primary_selection_offer_v1_interface offer_impl = {
	.receive = data_offer_receive,
	.destroy = resource_destroy_req,
};

static void
send_selection(struct wl_resource *device)
{
	struct data_source *source = core.seat.selections[SELECTION_PRIMARY];
	if (!source) {
		zwp_primary_selection_device_v1_send_selection(device, NULL);
		return;
	}
	struct wl_resource *offer = data_offer_create(device,
		&zwp_primary_selection_offer_v1_interface, &offer_impl, source);
	zwp_primary_selection_device_v1_send_data_offer(device, offer);
	char **mime_type;
	wl_array_for_each(mime_type, &source->mime_types) {
		zwp_primary_selection_offer_v1_send_offer(offer, *mime_type);
	}
	zwp_primary_selection_device_v1_send_selection(device, offer);
}

// Sends the primary selection to the devices of the client.
void
primary_selection_send(struct wl_client *client)
{
	struct wl_resource *device;
	wl_resource_for_each(device, &core.seat.primary_selection_devices) {
		if (wl_resource_get_client(device) == client) {
			send_selection(device);
		}
	}
}

static void
source_offer_req(struct wl_client *client, struct wl_resource *resource,
	const char *mime_type)
{
	data_source_offer(wl_resource_get_user_data(resource), mime_type);
}

static const struct zwp_primary_selection_source_v1_interface source_impl = {
	.offer = source_offer_req,
	.destroy = resource_destroy_req,
};

// Like wl_data_device.set_selection, but sources used already are ignored, as
// the protocol has no error for them.
static void
device_set_selection_req(struct wl_client *client,
	struct wl_resource *resource, struct wl_resource *source_resource,
	uint32_t serial)
{
	if (!input_serial_matches_client(client, serial)) {
		return;
	}
	struct data_source *source = source_resource
		? wl_resource_get_user_data(source_resource) : NULL;
	if (source && source->used
			&& source != core.seat.selections[SELECTION_PRIMARY]) {
		return;
	}
	seat_set_selection(SELECTION_PRIMARY, source);
}

static const struct zwp_primary_selection_device_v1_interface device_impl = {
	.set_selection = device_set_selection_req,
	.destroy = resource_destroy_req,
};

static void
manager_create_source_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id)
{
	struct wl_resource *source_resource = wl_resource_create(client,
		&zwp_primary_selection_source_v1_interface,
		wl_resource_get_version(resource), id);
	assert(source_resource);
	struct data_source *source = data_source_create(
		DATA_SOURCE_PRIMARY_SELECTION, source_resource);
	wl_resource_set_implementation(source_resource, &source_impl, source,
		data_source_resource_destroy);
}

static void
manager_get_device_req(struct wl_client *client, struct wl_resource *resource,
	uint32_t id, struct wl_resource *seat)
{
	struct wl_resource *device = wl_resource_create(client,
		&zwp_primary_selection_device_v1_interface,
		wl_resource_get_version(resource), id);
	assert(device);
	wl_resource_set_implementation(device, &device_impl, NULL, resource_unlink);
	wl_list_insert(&core.seat.primary_selection_devices,
		wl_resource_get_link(device));
	if (core.seat.selection_client == client) {
		send_selection(device);
	}
}

static const struct zwp_primary_selection_device_manager_v1_interface
manager_impl = {
	.create_source = manager_create_source_req,
	.get_device = manager_get_device_req,
	.destroy = resource_destroy_req,
};

void
wayland_bind_primary_selection_device_manager(struct wl_client *client,
	void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&zwp_primary_selection_device_manager_v1_interface, MIN(version, 1), id);
	assert(resource);
	wl_resource_set_implementation(resource, &manager_impl, data, NULL);
}
