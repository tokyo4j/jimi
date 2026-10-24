#include <assert.h>

#include "core.h"
#include "ext-data-control-v1-protocol.h"

static const struct ext_data_control_offer_v1_interface offer_impl = {
	.receive = data_offer_receive,
	.destroy = resource_destroy_req,
};

static void
send_selection(struct wl_resource *device, enum selection_type type)
{
	struct data_source *source = core.seat.selections[type];
	struct wl_resource *offer = NULL;
	if (source) {
		offer = data_offer_create(device,
			&ext_data_control_offer_v1_interface, &offer_impl, source);
		ext_data_control_device_v1_send_data_offer(device, offer);
		char **mime_type;
		wl_array_for_each(mime_type, &source->mime_types) {
			ext_data_control_offer_v1_send_offer(offer, *mime_type);
		}
	}
	if (type == SELECTION_CLIPBOARD) {
		ext_data_control_device_v1_send_selection(device, offer);
	} else {
		ext_data_control_device_v1_send_primary_selection(device, offer);
	}
}

// Clipboard managers see the selections regardless of the keyboard focus.
void
data_control_send_selection(enum selection_type type)
{
	struct wl_resource *device;
	wl_resource_for_each(device, &core.seat.data_control_devices) {
		send_selection(device, type);
	}
}

// MIME types cannot be added once the source is set as a selection.
static void
source_offer_req(struct wl_client *client, struct wl_resource *resource,
	const char *mime_type)
{
	struct data_source *source = wl_resource_get_user_data(resource);
	if (source->used) {
		wl_resource_post_error(resource,
			EXT_DATA_CONTROL_SOURCE_V1_ERROR_INVALID_OFFER,
			"source is already used");
		return;
	}
	data_source_offer(source, mime_type);
}

static const struct ext_data_control_source_v1_interface source_impl = {
	.offer = source_offer_req,
	.destroy = resource_destroy_req,
};

static void
set_selection(struct wl_resource *device, enum selection_type type,
	struct wl_resource *source_resource)
{
	struct data_source *source = source_resource
		? wl_resource_get_user_data(source_resource) : NULL;
	if (source && source->used) {
		wl_resource_post_error(device, EXT_DATA_CONTROL_DEVICE_V1_ERROR_USED_SOURCE,
			"source is already used");
		return;
	}
	seat_set_selection(type, source);
}

static void
device_set_selection_req(struct wl_client *client,
	struct wl_resource *resource, struct wl_resource *source)
{
	set_selection(resource, SELECTION_CLIPBOARD, source);
}

static void
device_set_primary_selection_req(struct wl_client *client,
	struct wl_resource *resource, struct wl_resource *source)
{
	set_selection(resource, SELECTION_PRIMARY, source);
}

static const struct ext_data_control_device_v1_interface device_impl = {
	.set_selection = device_set_selection_req,
	.destroy = resource_destroy_req,
	.set_primary_selection = device_set_primary_selection_req,
};

static void
manager_create_data_source_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id)
{
	struct wl_resource *source_resource = wl_resource_create(client,
		&ext_data_control_source_v1_interface,
		wl_resource_get_version(resource), id);
	assert(source_resource);
	struct data_source *source = data_source_create(DATA_SOURCE_DATA_CONTROL,
		source_resource);
	wl_resource_set_implementation(source_resource, &source_impl, source,
		data_source_resource_destroy);
}

static void
manager_get_data_device_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id, struct wl_resource *seat)
{
	struct wl_resource *device = wl_resource_create(client,
		&ext_data_control_device_v1_interface,
		wl_resource_get_version(resource), id);
	assert(device);
	wl_resource_set_implementation(device, &device_impl, NULL, resource_unlink);
	wl_list_insert(&core.seat.data_control_devices, wl_resource_get_link(device));
	send_selection(device, SELECTION_CLIPBOARD);
	send_selection(device, SELECTION_PRIMARY);
}

static const struct ext_data_control_manager_v1_interface manager_impl = {
	.create_data_source = manager_create_data_source_req,
	.get_data_device = manager_get_data_device_req,
	.destroy = resource_destroy_req,
};

void
wayland_bind_data_control_manager(struct wl_client *client, void *data,
	uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&ext_data_control_manager_v1_interface, MIN(version, 1), id);
	assert(resource);
	wl_resource_set_implementation(resource, &manager_impl, data, NULL);
}
