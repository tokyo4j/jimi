#include <assert.h>
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <gbm.h>
#include <libdisplay-info/info.h>
#include <libseat.h>
#include <libudev.h>
#include <xf86drm.h>

#include "core.h"
#include "output.h"
#include "surface.h"

// Of the HDR static metadata in CTA-861-G
#define CTA_STATIC_METADATA_TYPE1 0
#define CTA_EOTF_PQ 2
// The bits per color of the link for SDR and HDR
#define SDR_BPC 8
#define HDR_BPC 10
// Without one in the EDID, in cd/m²
#define DEFAULT_MAX_LUMINANCE 1000.0

// The ID of the property of the object, or 0 if it has none. The value of the
// property is stored in the value unless it is NULL.
static uint32_t
find_prop(int fd, uint32_t obj, uint32_t type, const char *name, uint64_t *value)
{
	drmModeObjectProperties *props = drmModeObjectGetProperties(fd, obj, type);
	assert(props);
	uint32_t id = 0;
	for (uint32_t i = 0; i < props->count_props; i++) {
		drmModePropertyRes *p = drmModeGetProperty(fd, props->props[i]);
		assert(p);
		if (strcmp(p->name, name) == 0) {
			id = p->prop_id;
			if (value) {
				*value = props->prop_values[i];
			}
		}
		drmModeFreeProperty(p);
	}
	drmModeFreeObjectProperties(props);
	return id;
}

static uint32_t
prop_id(int fd, uint32_t obj, uint32_t type, const char *name, uint64_t *value)
{
	uint32_t id = find_prop(fd, obj, type, name, value);
	assert(id);
	return id;
}

// The value of the enum of the name in the property, which is false if it has
// none.
static bool
find_enum_value(int fd, uint32_t prop, const char *name, uint64_t *value)
{
	drmModePropertyRes *p = drmModeGetProperty(fd, prop);
	assert(p);
	bool found = false;
	for (int i = 0; i < p->count_enums; i++) {
		if (strcmp(p->enums[i].name, name) == 0) {
			*value = p->enums[i].value;
			found = true;
		}
	}
	drmModeFreeProperty(p);
	return found;
}

// The minimum and the maximum of the range property
static void
prop_range(int fd, uint32_t prop, uint64_t range[2])
{
	drmModePropertyRes *p = drmModeGetProperty(fd, prop);
	assert(p && (p->flags & DRM_MODE_PROP_RANGE) && p->count_values == 2);
	range[0] = p->values[0];
	range[1] = p->values[1];
	drmModeFreeProperty(p);
}

static void
add_prop(drmModeAtomicReq *req, uint32_t obj, uint32_t prop, uint64_t value)
{
	assert(drmModeAtomicAddProperty(req, obj, prop, value) >= 0);
}

static int
crtc_index(drmModeRes *res, uint32_t crtc_id)
{
	for (int i = 0; i < res->count_crtcs; i++) {
		if (res->crtcs[i] == crtc_id) {
			return i;
		}
	}
	assert(!"unknown CRTC");
	return -1;
}

static bool
crtc_used(uint32_t crtc_id)
{
	struct output *output;
	wl_list_for_each(output, &core.outputs, link) {
		if (output->crtc_id == crtc_id) {
			return true;
		}
	}
	return false;
}

static bool
plane_used(uint32_t plane_id)
{
	struct output *output;
	wl_list_for_each(output, &core.outputs, link) {
		if (output->primary.id == plane_id || output->cursor.plane.id == plane_id) {
			return true;
		}
	}
	return false;
}

// The modifiers of each pixel format that the plane can scan out. Each
// modifier in the blob has a bitmask of the formats from its offset.
static void
read_plane_formats(struct plane *plane)
{
	uint64_t blob_id = 0;
	prop_id(core.kms.fd, plane->id, DRM_MODE_OBJECT_PLANE, "IN_FORMATS", &blob_id);
	assert(blob_id);

	drmModePropertyBlobRes *blob = drmModeGetPropertyBlob(core.kms.fd, blob_id);
	assert(blob);
	struct drm_format_modifier_blob *fmt_blob = blob->data;
	uint32_t *formats = (uint32_t *)((char *)fmt_blob + fmt_blob->formats_offset);
	struct drm_format_modifier *mods =
		(struct drm_format_modifier *)((char *)fmt_blob
			+ fmt_blob->modifiers_offset);
	plane->formats = calloc((size_t)fmt_blob->count_formats
		* fmt_blob->count_modifiers, sizeof(*plane->formats));
	assert(plane->formats);
	for (uint32_t i = 0; i < fmt_blob->count_modifiers; i++) {
		for (uint32_t j = 0; j < 64; j++) {
			uint32_t index = (uint32_t)mods[i].offset + j;
			if (!(mods[i].formats & (1ull << j))
					|| index >= fmt_blob->count_formats
					|| !pixel_format_find(formats[index])) {
				continue;
			}
			plane->formats[plane->nr_formats++] = (struct plane_format){
				.format = formats[index],
				.modifier = mods[i].modifier,
			};
		}
	}
	drmModeFreePropertyBlob(blob);
}

// Whether the plane can scan out the format with the modifier.
bool
plane_supports(const struct plane *plane, uint32_t format, uint64_t modifier)
{
	for (uint32_t i = 0; i < plane->nr_formats; i++) {
		if (plane->formats[i].format == format
				&& plane->formats[i].modifier == modifier) {
			return true;
		}
	}
	return false;
}

// The CRTCs and planes of the other outputs are used.
static bool
choose_crtc(struct output *output, drmModeConnector *conn, drmModeRes *res)
{
	// TODO: Prefer existing connector/encoder CRTC assignments before picking
	// any free CRTC.
	for (int i = 0; i < conn->count_encoders; i++) {
		drmModeEncoder *enc = drmModeGetEncoder(core.kms.fd, conn->encoders[i]);
		assert(enc);
		for (int j = 0; j < res->count_crtcs; j++) {
			if (!(enc->possible_crtcs & (1 << j))) {
				continue;
			}
			if (crtc_used(res->crtcs[j])) {
				continue;
			}
			output->crtc_id = res->crtcs[j];
			drmModeFreeEncoder(enc);
			return true;
		}
		drmModeFreeEncoder(enc);
	}
	return false;
}

// An unused plane of the type on the CRTC of the output, which can show the
// format, or 0 if there is none.
static uint32_t
find_plane(struct output *output, drmModePlaneRes *planes, drmModeRes *res,
	const char *type_name, uint32_t format)
{
	int crtc_idx = crtc_index(res, output->crtc_id);
	for (uint32_t i = 0; i < planes->count_planes; i++) {
		drmModePlane *plane = drmModeGetPlane(core.kms.fd, planes->planes[i]);
		assert(plane);
		if (plane_used(plane->plane_id)) {
			drmModeFreePlane(plane);
			continue;
		}
		bool possible = plane->possible_crtcs & (1 << crtc_idx);
		bool has_format = false;
		for (uint32_t j = 0; j < plane->count_formats; j++) {
			has_format |= plane->formats[j] == format;
		}
		uint64_t type = 0;
		uint32_t type_prop = prop_id(core.kms.fd, plane->plane_id,
			DRM_MODE_OBJECT_PLANE, "type", &type);
		uint64_t wanted_type = 0;
		assert(find_enum_value(core.kms.fd, type_prop, type_name, &wanted_type));
		uint32_t plane_id = plane->plane_id;
		drmModeFreePlane(plane);
		if (possible && has_format && type == wanted_type) {
			return plane_id;
		}
	}
	return 0;
}

static void
init_plane(struct plane *plane, uint32_t id)
{
	int fd = core.kms.fd;
	plane->id = id;
	plane->props.fb_id = prop_id(fd, id, DRM_MODE_OBJECT_PLANE, "FB_ID", NULL);
	plane->props.in_fence_fd = prop_id(fd, id, DRM_MODE_OBJECT_PLANE, "IN_FENCE_FD", NULL);
	plane->props.crtc_id = prop_id(fd, id, DRM_MODE_OBJECT_PLANE, "CRTC_ID", NULL);
	plane->props.src_x = prop_id(fd, id, DRM_MODE_OBJECT_PLANE, "SRC_X", NULL);
	plane->props.src_y = prop_id(fd, id, DRM_MODE_OBJECT_PLANE, "SRC_Y", NULL);
	plane->props.src_w = prop_id(fd, id, DRM_MODE_OBJECT_PLANE, "SRC_W", NULL);
	plane->props.src_h = prop_id(fd, id, DRM_MODE_OBJECT_PLANE, "SRC_H", NULL);
	plane->props.crtc_x = prop_id(fd, id, DRM_MODE_OBJECT_PLANE, "CRTC_X", NULL);
	plane->props.crtc_y = prop_id(fd, id, DRM_MODE_OBJECT_PLANE, "CRTC_Y", NULL);
	plane->props.crtc_w = prop_id(fd, id, DRM_MODE_OBJECT_PLANE, "CRTC_W", NULL);
	plane->props.crtc_h = prop_id(fd, id, DRM_MODE_OBJECT_PLANE, "CRTC_H", NULL);
	read_plane_formats(plane);
}

static void
init_output_props(struct output *output)
{
	int fd = core.kms.fd;
	output->props.conn_crtc_id = prop_id(fd, output->conn_id,
		DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", NULL);
	output->props.crtc_active = prop_id(fd, output->crtc_id,
		DRM_MODE_OBJECT_CRTC, "ACTIVE", NULL);
	output->props.crtc_mode_id = prop_id(fd, output->crtc_id,
		DRM_MODE_OBJECT_CRTC, "MODE_ID", NULL);
	output->props.crtc_gamma_lut = find_prop(fd, output->crtc_id,
		DRM_MODE_OBJECT_CRTC, "GAMMA_LUT", NULL);
	if (output->props.crtc_gamma_lut) {
		uint64_t size = 0;
		prop_id(fd, output->crtc_id, DRM_MODE_OBJECT_CRTC, "GAMMA_LUT_SIZE", &size);
		output->gamma_lut_size = (uint32_t)size;
	}
	output->props.crtc_vrr_enabled = find_prop(fd, output->crtc_id,
		DRM_MODE_OBJECT_CRTC, "VRR_ENABLED", NULL);
	uint64_t vrr_capable = 0;
	find_prop(fd, output->conn_id, DRM_MODE_OBJECT_CONNECTOR, "vrr_capable",
		&vrr_capable);
	output->adaptive_sync_supported = output->props.crtc_vrr_enabled && vrr_capable;
	output->props.conn_max_bpc = find_prop(fd, output->conn_id,
		DRM_MODE_OBJECT_CONNECTOR, "max bpc", NULL);
	if (output->props.conn_max_bpc) {
		prop_range(fd, output->props.conn_max_bpc, output->max_bpc_range);
	}
	// Both are needed for HDR.
	uint32_t colorspace = find_prop(fd, output->conn_id,
		DRM_MODE_OBJECT_CONNECTOR, "Colorspace", NULL);
	uint32_t hdr_output_metadata = find_prop(fd, output->conn_id,
		DRM_MODE_OBJECT_CONNECTOR, "HDR_OUTPUT_METADATA", NULL);
	if (colorspace && hdr_output_metadata
			&& find_enum_value(fd, colorspace, "Default",
				&output->colorspace_default)
			&& find_enum_value(fd, colorspace, "BT2020_RGB",
				&output->colorspace_bt2020)) {
		output->props.conn_colorspace = colorspace;
		output->props.conn_hdr_output_metadata = hdr_output_metadata;
	}
}

static void
copy_edid_string(char *dest, size_t size, char *string)
{
	if (string) {
		snprintf(dest, size, "%s", string);
		free(string);
	}
}

// The make, the model, and the color volume of the monitor for HDR. False if
// the monitor cannot show BT.2020 colors encoded by PQ, like those without an
// EDID, whose make and model are unknown.
static bool
read_edid(struct output *output)
{
	strcpy(output->make, "Unknown");
	strcpy(output->model, "Unknown");
	color_primaries_from_named(WP_COLOR_MANAGER_V1_PRIMARIES_BT2020,
		&output->display_primaries);
	output->display_max_luminance = DEFAULT_MAX_LUMINANCE;
	uint64_t blob_id = 0;
	prop_id(core.kms.fd, output->conn_id, DRM_MODE_OBJECT_CONNECTOR, "EDID",
		&blob_id);
	if (!blob_id) {
		return false;
	}
	drmModePropertyBlobRes *blob = drmModeGetPropertyBlob(core.kms.fd, blob_id);
	assert(blob);
	struct di_info *info = di_info_parse_edid(blob->data, blob->length);
	drmModeFreePropertyBlob(blob);
	if (!info) {
		return false;
	}
	copy_edid_string(output->make, sizeof(output->make), di_info_get_make(info));
	copy_edid_string(output->model, sizeof(output->model), di_info_get_model(info));

	const struct di_color_primaries *primaries =
		di_info_get_default_color_primaries(info);
	struct color_primaries *display = &output->display_primaries;
	if (primaries->has_primaries) {
		double *xy[] = {display->red, display->green, display->blue};
		for (int i = 0; i < 3; i++) {
			xy[i][0] = primaries->primary[i].x;
			xy[i][1] = primaries->primary[i].y;
		}
	}
	if (primaries->has_default_white_point) {
		display->white[0] = primaries->default_white.x;
		display->white[1] = primaries->default_white.y;
	}
	const struct di_hdr_static_metadata *metadata =
		di_info_get_hdr_static_metadata(info);
	output->display_min_luminance = metadata->desired_content_min_luminance;
	if (metadata->desired_content_max_luminance > output->display_min_luminance) {
		output->display_max_luminance = metadata->desired_content_max_luminance;
	}
	const struct di_supported_signal_colorimetry *colorimetry =
		di_info_get_supported_signal_colorimetry(info);
	bool hdr = metadata->type1 && metadata->pq && (colorimetry->bt2020_rgb
		|| colorimetry->bt2020_ycc || colorimetry->bt2020_cycc);
	di_info_destroy(info);
	return hdr;
}

static void
page_flip(int fd, unsigned seq, unsigned sec, unsigned usec, unsigned crtc,
	void *data)
{
	struct output *output;
	wl_list_for_each(output, &core.outputs, link) {
		if (output->crtc_id != crtc) {
			continue;
		}
		renderer_handle_page_flip(output,
			(int64_t)sec * 1000000000 + (int64_t)usec * 1000, seq);
		return;
	}
	assert(!"page flip for unknown CRTC");
}

static int
handle_drm_events(int fd, uint32_t mask, void *data)
{
	drmEventContext drm_ev = {
		.version = DRM_EVENT_CONTEXT_VERSION,
		.page_flip_handler2 = page_flip,
	};
	drmHandleEvent(fd, &drm_ev);
	return 0;
}

// Monitors without modes cannot be shown.
static bool
connector_connected(drmModeConnector *conn)
{
	return conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0;
}

// The lowest bit that no connected output has.
static uint32_t
free_output_bit(void)
{
	uint32_t taken = 0;
	struct output *output;
	wl_list_for_each(output, &core.outputs, link) {
		taken |= output->bit;
	}
	for (uint32_t bit = 1; bit; bit <<= 1) {
		if (!(taken & bit)) {
			return bit;
		}
	}
	assert(!"too many outputs");
	return 0;
}

// The output of the connector with a free CRTC and primary plane, or NULL if
// there is none. The cursor is drawn into the images of the swapchain if no
// cursor plane is free.
static struct output *
output_create(drmModeConnector *conn, drmModeRes *res, drmModePlaneRes *planes)
{
	struct output *output = calloc(1, sizeof(*output));
	assert(output);
	output->bit = free_output_bit();
	output->conn_id = conn->connector_id;
	snprintf(output->name, sizeof(output->name), "%s-%u",
		drmModeGetConnectorTypeName(conn->connector_type), conn->connector_type_id);
	uint32_t primary = choose_crtc(output, conn, res)
		? find_plane(output, planes, res, "Primary", SCANOUT_FORMAT)
		: 0;
	if (!primary) {
		fprintf(stderr, "%s has no free CRTC and plane\n", output->name);
		free(output);
		return NULL;
	}
	init_output_props(output);
	init_plane(&output->primary, primary);
	uint32_t cursor = find_plane(output, planes, res, "Cursor", DRM_FORMAT_ARGB8888);
	if (cursor) {
		init_plane(&output->cursor.plane, cursor);
	}
	output->cursor.current = -1;
	wl_list_init(&output->resources);
	wl_list_init(&output->xdg_resources);
	wl_list_init(&output->color_resources);
	wl_list_init(&output->heads);
	wl_list_init(&output->configuration_heads);
	wl_list_init(&output->capture_sources);
	wl_list_init(&output->capture_sessions);
	wl_list_init(&output->pending_feedbacks);
	wl_list_init(&output->queued_feedbacks);
	for (int i = 0; i < NR_LAYER_SHELL_LAYERS; i++) {
		wl_list_init(&output->layer_surfaces[i]);
	}
	output->hdr_supported = read_edid(output) && output->props.conn_colorspace;
	snprintf(output->description, sizeof(output->description),
		"%s %s (%s)", output->make, output->model, output->name);
	output->physical_width = (int)conn->mmWidth;
	output->physical_height = (int)conn->mmHeight;
	output->nr_modes = conn->count_modes;
	output->modes = calloc(conn->count_modes, sizeof(*output->modes));
	assert(output->modes);
	memcpy(output->modes, conn->modes, conn->count_modes * sizeof(*output->modes));
	return output;
}

static void
output_destroy(struct output *output)
{
	wl_list_remove(&output->link);
	if (output->gamma_lut_blob) {
		drmModeDestroyPropertyBlob(core.kms.fd, output->gamma_lut_blob);
	}
	image_description_unref(output->image_description);
	free(output->primary.formats);
	free(output->cursor.plane.formats);
	free(output->modes);
	free(output);
}

static struct output *
find_output(uint32_t conn_id)
{
	struct output *output;
	wl_list_for_each(output, &core.outputs, link) {
		if (output->conn_id == conn_id) {
			return output;
		}
	}
	return NULL;
}

// Adds the outputs of the connected connectors that have none. The
// hotplugged ones are connected right away, while outputs_init() configures
// those found at startup.
static void
add_outputs(bool hotplugged)
{
	int fd = core.kms.fd;
	drmModeRes *res = drmModeGetResources(fd);
	assert(res);
	drmModePlaneRes *planes = drmModeGetPlaneResources(fd);
	assert(planes);
	for (int i = 0; i < res->count_connectors; i++) {
		if (find_output(res->connectors[i])) {
			continue;
		}
		drmModeConnector *conn = drmModeGetConnector(fd, res->connectors[i]);
		assert(conn);
		struct output *output = connector_connected(conn)
			? output_create(conn, res, planes) : NULL;
		drmModeFreeConnector(conn);
		if (!output) {
			continue;
		}
		list_append(&core.outputs, &output->link);
		if (hotplugged) {
			output_connect(output);
		}
	}
	drmModeFreePlaneResources(planes);
	drmModeFreeResources(res);
}

// Removes the outputs of the connectors that are disconnected, or gone like
// those of DisplayPort MST.
static void
remove_outputs(void)
{
	struct output *output, *tmp;
	wl_list_for_each_safe(output, tmp, &core.outputs, link) {
		drmModeConnector *conn = drmModeGetConnector(core.kms.fd, output->conn_id);
		bool connected = conn && connector_connected(conn);
		drmModeFreeConnector(conn);
		if (!connected) {
			output_disconnect(output);
			output_destroy(output);
		}
	}
}

// Connectors are connected and disconnected by hotplug uevents of the device.
static int
handle_udev_events(int fd, uint32_t mask, void *data)
{
	struct udev_device *device = udev_monitor_receive_device(core.kms.monitor);
	if (!device) {
		return 0;
	}
	const char *action = udev_device_get_action(device);
	if (udev_device_get_devnum(device) == core.kms.devnum
			&& action && strcmp(action, "change") == 0) {
		remove_outputs();
		add_outputs(true);
	}
	udev_device_unref(device);
	return 0;
}

// The card of the seat that the firmware booted with, or else the first card
// of the seat.
static struct udev_device *
find_primary_card(void)
{
	struct udev_enumerate *enumerate = udev_enumerate_new(core.session.udev);
	assert(enumerate);
	assert(udev_enumerate_add_match_subsystem(enumerate, "drm") >= 0);
	// Not the render nodes nor the connectors
	assert(udev_enumerate_add_match_property(enumerate, "DEVTYPE",
		"drm_minor") >= 0);
	assert(udev_enumerate_add_match_sysname(enumerate, "card[0-9]*") >= 0);
	assert(udev_enumerate_scan_devices(enumerate) >= 0);

	const char *seat_name = libseat_seat_name(core.session.handle);
	struct udev_device *primary = NULL;
	struct udev_list_entry *entry;
	udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(enumerate)) {
		struct udev_device *card = udev_device_new_from_syspath(
			core.session.udev, udev_list_entry_get_name(entry));
		assert(card);
		const char *seat = udev_device_get_property_value(card, "ID_SEAT");
		struct udev_device *pci = udev_device_get_parent_with_subsystem_devtype(
			card, "pci", NULL);
		const char *boot_vga = pci
			? udev_device_get_sysattr_value(pci, "boot_vga") : NULL;
		bool on_seat = strcmp(seat ? seat : "seat0", seat_name) == 0;
		bool is_boot_vga = boot_vga && strcmp(boot_vga, "1") == 0;
		if (on_seat && (!primary || is_boot_vga)) {
			udev_device_unref(primary);
			primary = card;
		} else {
			udev_device_unref(card);
		}
	}
	udev_enumerate_unref(enumerate);
	assert(primary);
	return primary;
}

void
kms_init(void)
{
	// TODO: Support multiple DRM cards.
	struct udev_device *card = find_primary_card();
	core.session.drm_device_id = libseat_open_device(core.session.handle,
		udev_device_get_devnode(card), &core.kms.fd);
	assert(core.session.drm_device_id >= 0);
	udev_device_unref(card);
	wl_list_init(&core.outputs);

	int fd = core.kms.fd;
	uint64_t cap = 0;
	assert(drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1) == 0);
	assert(drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) == 0);
	assert(drmGetCap(fd, DRM_CAP_ADDFB2_MODIFIERS, &cap) == 0 && cap);
	// The page flips are timed with the clock of the frame scheduling.
	assert(drmGetCap(fd, DRM_CAP_TIMESTAMP_MONOTONIC, &cap) == 0 && cap);
	core.kms.cursor_width =
		drmGetCap(fd, DRM_CAP_CURSOR_WIDTH, &cap) == 0 ? cap : 64;
	core.kms.cursor_height =
		drmGetCap(fd, DRM_CAP_CURSOR_HEIGHT, &cap) == 0 ? cap : 64;
	core.kms.async_page_flip =
		drmGetCap(fd, DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP, &cap) == 0 && cap;
	struct stat st;
	assert(fstat(fd, &st) == 0);
	core.kms.devnum = st.st_rdev;
	add_outputs(false);

	core.gbm = gbm_create_device(core.kms.fd);
	assert(core.gbm);
	core.kms.source = wl_event_loop_add_fd(core.wl_loop, core.kms.fd,
		WL_EVENT_READABLE, handle_drm_events, NULL);
	assert(core.kms.source);

	core.kms.monitor = udev_monitor_new_from_netlink(core.session.udev, "udev");
	assert(core.kms.monitor);
	assert(udev_monitor_filter_add_match_subsystem_devtype(core.kms.monitor,
		"drm", NULL) >= 0);
	assert(udev_monitor_enable_receiving(core.kms.monitor) >= 0);
	core.kms.monitor_source = wl_event_loop_add_fd(core.wl_loop,
		udev_monitor_get_fd(core.kms.monitor), WL_EVENT_READABLE,
		handle_udev_events, NULL);
	assert(core.kms.monitor_source);
}

// The framebuffer to scan out the buffer object, or 0 if KMS cannot.
uint32_t
kms_add_fb(struct gbm_bo *bo)
{
	uint32_t handles[MAX_DMABUF_PLANES] = {0};
	uint32_t strides[MAX_DMABUF_PLANES] = {0};
	uint32_t offsets[MAX_DMABUF_PLANES] = {0};
	uint64_t modifiers[MAX_DMABUF_PLANES] = {0};
	int nr_planes = gbm_bo_get_plane_count(bo);
	assert(nr_planes > 0 && nr_planes <= MAX_DMABUF_PLANES);
	for (int i = 0; i < nr_planes; i++) {
		handles[i] = gbm_bo_get_handle_for_plane(bo, i).u32;
		assert(handles[i]);
		strides[i] = gbm_bo_get_stride_for_plane(bo, i);
		offsets[i] = gbm_bo_get_offset(bo, i);
		modifiers[i] = gbm_bo_get_modifier(bo);
	}
	uint32_t fb = 0;
	if (drmModeAddFB2WithModifiers(core.kms.fd, gbm_bo_get_width(bo),
			gbm_bo_get_height(bo), gbm_bo_get_format(bo), handles, strides,
			offsets, modifiers, &fb, DRM_MODE_FB_MODIFIERS) != 0) {
		return 0;
	}
	return fb;
}

// Imports the client dma-buf to scan it out on primary planes, and returns
// its framebuffer, or 0 if KMS cannot. The alpha channel is ignored by the
// opaque format, as only opaque surfaces are scanned out, and YCbCr is not.
// The format must be supported. The buffer object is imported by gbm,
// whose driver may share the GEM handles of the device file with the
// renderer, like libdrm_amdgpu does, and counts their references.
uint32_t
kms_import_dmabuf(const struct dmabuf_attributes *attributes, struct gbm_bo **bo)
{
	*bo = NULL;
	uint32_t format = pixel_format_find(attributes->format)->opaque;
	if (attributes->y_invert || !format) {
		return 0;
	}
	struct gbm_import_fd_modifier_data data = {
		.width = attributes->width,
		.height = attributes->height,
		.format = format,
		.num_fds = attributes->nr_planes,
		.modifier = attributes->modifier,
	};
	for (uint32_t i = 0; i < attributes->nr_planes; i++) {
		data.fds[i] = attributes->fds[i];
		data.strides[i] = (int)attributes->strides[i];
		data.offsets[i] = (int)attributes->offsets[i];
	}
	*bo = gbm_bo_import(core.gbm, GBM_BO_IMPORT_FD_MODIFIER, &data,
		GBM_BO_USE_SCANOUT);
	if (!*bo) {
		return 0;
	}
	uint32_t fb = kms_add_fb(*bo);
	if (!fb) {
		gbm_bo_destroy(*bo);
		*bo = NULL;
	}
	return fb;
}

// The static metadata that HDR is shown with. The images target the color
// volume of the monitor, which it then shows as they are.
static uint32_t
create_hdr_metadata_blob(struct output *output)
{
	const struct color_primaries *primaries = &output->display_primaries;
	const double *xy[] = {primaries->red, primaries->green, primaries->blue};
	struct hdr_output_metadata metadata = {
		.metadata_type = CTA_STATIC_METADATA_TYPE1,
		.hdmi_metadata_type1 = {
			.eotf = CTA_EOTF_PQ,
			.metadata_type = CTA_STATIC_METADATA_TYPE1,
			// In 0.00002 units
			.white_point = {
				.x = (uint16_t)lround(primaries->white[0] * 50000.0),
				.y = (uint16_t)lround(primaries->white[1] * 50000.0),
			},
			.max_display_mastering_luminance =
				(uint16_t)lround(output->display_max_luminance),
			// In 0.0001 cd/m²
			.min_display_mastering_luminance =
				(uint16_t)lround(output->display_min_luminance * 10000.0),
		},
	};
	for (int i = 0; i < 3; i++) {
		metadata.hdmi_metadata_type1.display_primaries[i].x =
			(uint16_t)lround(xy[i][0] * 50000.0);
		metadata.hdmi_metadata_type1.display_primaries[i].y =
			(uint16_t)lround(xy[i][1] * 50000.0);
	}
	uint32_t blob = 0;
	assert(drmModeCreatePropertyBlob(core.kms.fd, &metadata, sizeof(metadata),
		&blob) == 0);
	return blob;
}

// HDR is shown with the metadata blob, or else SDR if it is 0. The link has
// the bits per color of the images.
static void
add_mode_props(drmModeAtomicReq *req, struct output *output,
	uint32_t mode_blob, bool adaptive_sync, uint32_t hdr_metadata_blob)
{
	add_prop(req, output->conn_id, output->props.conn_crtc_id, output->crtc_id);
	add_prop(req, output->crtc_id, output->props.crtc_active, 1);
	add_prop(req, output->crtc_id, output->props.crtc_mode_id, mode_blob);
	if (output->props.crtc_vrr_enabled) {
		add_prop(req, output->crtc_id, output->props.crtc_vrr_enabled,
			adaptive_sync);
	}
	if (output->props.conn_colorspace) {
		add_prop(req, output->conn_id, output->props.conn_colorspace,
			hdr_metadata_blob ? output->colorspace_bt2020
				: output->colorspace_default);
		add_prop(req, output->conn_id,
			output->props.conn_hdr_output_metadata, hdr_metadata_blob);
	}
	if (output->props.conn_max_bpc) {
		uint64_t bpc = hdr_metadata_blob ? HDR_BPC : SDR_BPC;
		bpc = MIN(MAX(bpc, output->max_bpc_range[0]), output->max_bpc_range[1]);
		add_prop(req, output->conn_id, output->props.conn_max_bpc, bpc);
	}
}

// Shows the whole framebuffer unscaled at the box in the pixels of the mode,
// once the fence signals unless it is -1.
static void
add_plane_props(drmModeAtomicReq *req, struct output *output,
	const struct plane *plane, uint32_t fb, int fence_fd, const struct box *box)
{
	add_prop(req, plane->id, plane->props.fb_id, fb);
	add_prop(req, plane->id, plane->props.crtc_id, output->crtc_id);
	add_prop(req, plane->id, plane->props.src_x, 0);
	add_prop(req, plane->id, plane->props.src_y, 0);
	add_prop(req, plane->id, plane->props.src_w, (uint64_t)box->width << 16);
	add_prop(req, plane->id, plane->props.src_h, (uint64_t)box->height << 16);
	// The position is signed.
	add_prop(req, plane->id, plane->props.crtc_x, (uint64_t)box->x);
	add_prop(req, plane->id, plane->props.crtc_y, (uint64_t)box->y);
	add_prop(req, plane->id, plane->props.crtc_w, (uint64_t)box->width);
	add_prop(req, plane->id, plane->props.crtc_h, (uint64_t)box->height);
	if (fence_fd >= 0) {
		add_prop(req, plane->id, plane->props.in_fence_fd, (uint64_t)fence_fd);
	}
}

static void
add_plane_off_props(drmModeAtomicReq *req, const struct plane *plane)
{
	add_prop(req, plane->id, plane->props.fb_id, 0);
	add_prop(req, plane->id, plane->props.crtc_id, 0);
}

// The primary plane covers the whole mode.
static void
add_primary_props(drmModeAtomicReq *req, struct output *output,
	const drmModeModeInfo *mode, uint32_t fb, int fence_fd)
{
	struct box box = {.width = mode->hdisplay, .height = mode->vdisplay};
	add_plane_props(req, output, &output->primary, fb, fence_fd, &box);
}

// The cursor plane shows what the renderer has set for the commit.
static void
add_cursor_props(drmModeAtomicReq *req, struct output *output)
{
	struct cursor_plane *cursor = &output->cursor;
	if (!cursor->plane.id) {
		return;
	}
	if (!cursor->visible) {
		add_plane_off_props(req, &cursor->plane);
		return;
	}
	struct swapchain_image *image = &cursor->images[cursor->current];
	add_plane_props(req, output, &cursor->plane, image->fb, image->render_fd,
		&cursor->box);
}

static void
add_off_props(drmModeAtomicReq *req, struct output *output)
{
	add_plane_off_props(req, &output->primary);
	if (output->cursor.plane.id) {
		add_plane_off_props(req, &output->cursor.plane);
	}
	add_prop(req, output->conn_id, output->props.conn_crtc_id, 0);
	add_prop(req, output->crtc_id, output->props.crtc_active, 0);
	add_prop(req, output->crtc_id, output->props.crtc_mode_id, 0);
	// Whatever shows the CRTC and the connector next starts without the
	// night light, adaptive sync and HDR.
	if (output->props.crtc_gamma_lut) {
		add_prop(req, output->crtc_id, output->props.crtc_gamma_lut, 0);
	}
	if (output->props.crtc_vrr_enabled) {
		add_prop(req, output->crtc_id, output->props.crtc_vrr_enabled, 0);
	}
	if (output->props.conn_colorspace) {
		add_prop(req, output->conn_id, output->props.conn_colorspace,
			output->colorspace_default);
		add_prop(req, output->conn_id, output->props.conn_hdr_output_metadata, 0);
	}
}

// The state of the output in the states, or NULL if they leave it as it is.
static const struct output_state *
find_state(const struct output_state *states, int nr_states, struct output *output)
{
	for (int i = 0; i < nr_states; i++) {
		if (states[i].output == output) {
			return &states[i];
		}
	}
	return NULL;
}

// Whether KMS can show the outputs with the states and the other outputs as
// they are, with framebuffers like those of their swapchains. Nothing can be
// tested while the session is inactive, so the modesets after it resumes tell
// instead.
bool
kms_test(const struct output_state *states, int nr_states)
{
	if (!core.session.active) {
		return true;
	}
	drmModeAtomicReq *req = drmModeAtomicAlloc();
	assert(req);
	// For the outputs whose modes, adaptive sync or HDR are not on the
	// screen
	struct {
		struct gbm_bo *bo;
		uint32_t fb;
		uint32_t mode_blob;
		uint32_t hdr_metadata_blob;
	} modesets[MAX_OUTPUTS];
	int nr_modesets = 0;
	struct output *output;
	wl_list_for_each(output, &core.outputs, link) {
		const struct output_state *state = find_state(states, nr_states, output);
		bool enabled = state ? state->enabled : output->enabled;
		const drmModeModeInfo *mode = state ? state->mode : output->mode;
		bool adaptive_sync = state ? state->adaptive_sync : output->adaptive_sync;
		bool hdr = state ? state->hdr : output->hdr;
		if (!enabled) {
			add_off_props(req, output);
			continue;
		}
		// On the screen already
		if (output->enabled && output->modeset_done
				&& mode == output->mode
				&& adaptive_sync == output->adaptive_sync
				&& hdr == output->hdr) {
			continue;
		}
		struct gbm_bo *bo = scanout_bo_create(&output->primary,
			hdr ? HDR_SCANOUT_FORMAT : SCANOUT_FORMAT, mode->hdisplay,
			mode->vdisplay);
		assert(bo);
		uint32_t fb = kms_add_fb(bo);
		assert(fb);
		uint32_t mode_blob = 0;
		assert(drmModeCreatePropertyBlob(core.kms.fd, mode, sizeof(*mode),
			&mode_blob) == 0);
		uint32_t hdr_metadata_blob = hdr ? create_hdr_metadata_blob(output) : 0;
		add_mode_props(req, output, mode_blob, adaptive_sync, hdr_metadata_blob);
		add_primary_props(req, output, mode, fb, -1);
		modesets[nr_modesets].bo = bo;
		modesets[nr_modesets].fb = fb;
		modesets[nr_modesets].mode_blob = mode_blob;
		modesets[nr_modesets].hdr_metadata_blob = hdr_metadata_blob;
		nr_modesets++;
	}
	bool ok = drmModeAtomicCommit(core.kms.fd, req,
		DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET, NULL) == 0;
	if (!ok) {
		fprintf(stderr, "KMS rejected the outputs: %s\n", strerror(errno));
	}
	drmModeAtomicFree(req);
	for (int i = 0; i < nr_modesets; i++) {
		drmModeDestroyPropertyBlob(core.kms.fd, modesets[i].mode_blob);
		if (modesets[i].hdr_metadata_blob) {
			drmModeDestroyPropertyBlob(core.kms.fd, modesets[i].hdr_metadata_blob);
		}
		drmModeRmFB(core.kms.fd, modesets[i].fb);
		gbm_bo_destroy(modesets[i].bo);
	}
	return ok;
}

// The frame on the primary plane, the cursor plane and the gamma LUT. An image
// of the swapchain is shown once the GPU finishes it, unless its sync file is
// closed as it has been on the screen already. A client buffer is ready, as
// the state of its surface was applied only then. The same gamma LUT again
// changes nothing.
static void
add_frame_props(drmModeAtomicReq *req, struct output *output,
	const struct frame *frame)
{
	if (frame->image) {
		add_primary_props(req, output, output->mode, frame->image->fb,
			frame->image->render_fd);
	} else {
		add_primary_props(req, output, output->mode, frame->buffer->fb, -1);
	}
	add_cursor_props(req, output);
	if (output->props.crtc_gamma_lut) {
		add_prop(req, output->crtc_id, output->props.crtc_gamma_lut,
			output->gamma_lut_blob);
	}
}

// Shows the pending frames of the outputs with their modes, adaptive sync and
// HDR in one commit. False if KMS rejects it. Modesets block, since nonblocking
// ones fail with EBUSY while the CRTCs that they affect have page flips
// pending, like those of the other outputs sharing the bandwidth.
bool
kms_modeset(struct output **outputs, int nr_outputs)
{
	drmModeAtomicReq *req = drmModeAtomicAlloc();
	assert(req);
	for (int i = 0; i < nr_outputs; i++) {
		struct output *output = outputs[i];
		// The CRTC and the connector hold their own references to the
		// old blobs until the commit replaces them.
		if (output->mode_blob) {
			drmModeDestroyPropertyBlob(core.kms.fd, output->mode_blob);
		}
		assert(drmModeCreatePropertyBlob(core.kms.fd, output->mode,
			sizeof(*output->mode), &output->mode_blob) == 0);
		if (output->hdr_metadata_blob) {
			drmModeDestroyPropertyBlob(core.kms.fd, output->hdr_metadata_blob);
		}
		output->hdr_metadata_blob = output->hdr
			? create_hdr_metadata_blob(output) : 0;
		add_mode_props(req, output, output->mode_blob,
			output->adaptive_sync, output->hdr_metadata_blob);
		add_frame_props(req, output, &output->pending);
	}
	bool ok = drmModeAtomicCommit(core.kms.fd, req,
		DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_PAGE_FLIP_EVENT, NULL) == 0;
	if (!ok) {
		fprintf(stderr, "Modeset failed: %s\n", strerror(errno));
	}
	drmModeAtomicFree(req);
	return ok;
}

// Shows the frame and the cursor plane from the next vblank, or a tearing
// frame at once. False if KMS rejects the commit.
//
// KMS rejects async page flips that change anything but the framebuffer of
// the primary plane, like the cursor plane, and some drivers also those that
// change its modifier or pitch. These are not logged, as the frame is shown
// from the next vblank instead.
bool
kms_page_flip(struct output *output, const struct frame *frame)
{
	drmModeAtomicReq *req = drmModeAtomicAlloc();
	assert(req);
	add_frame_props(req, output, frame);
	uint32_t flags = DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT;
	if (frame->tearing) {
		flags |= DRM_MODE_PAGE_FLIP_ASYNC;
	}
	bool ok = drmModeAtomicCommit(core.kms.fd, req, flags, NULL) == 0;
	if (!ok && !frame->tearing) {
		fprintf(stderr, "%s: page flip failed: %s\n", output->name, strerror(errno));
	}
	drmModeAtomicFree(req);
	return ok;
}

// A blocking commit, which waits for the flips of the output.
static void
commit_off(struct output *output)
{
	drmModeAtomicReq *req = drmModeAtomicAlloc();
	assert(req);
	add_off_props(req, output);
	drmModeAtomicCommit(core.kms.fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	drmModeAtomicFree(req);
	if (output->mode_blob) {
		drmModeDestroyPropertyBlob(core.kms.fd, output->mode_blob);
		output->mode_blob = 0;
	}
	if (output->hdr_metadata_blob) {
		drmModeDestroyPropertyBlob(core.kms.fd, output->hdr_metadata_blob);
		output->hdr_metadata_blob = 0;
	}
}

// Turns the disabled output off once its pending page flip is done, handling
// the flips of the other outputs meanwhile. The output cannot be changed
// while the session is inactive.
void
kms_disable(struct output *output)
{
	assert(!output->enabled);
	struct pollfd pollfd = {.fd = core.kms.fd, .events = POLLIN};
	while (!frame_empty(&output->pending)) {
		assert(poll(&pollfd, 1, -1) >= 0 || errno == EINTR);
		handle_drm_events(core.kms.fd, 0, NULL);
	}
	if (core.session.active) {
		commit_off(output);
	}
}

// Turns the outputs off at exit, before the clients are destroyed, so that
// they let go of the client buffers that they show.
void
kms_restore(void)
{
	// TODO: Restore saved CRTC state precisely rather than only disabling our
	// objects.
	struct output *output;
	wl_list_for_each(output, &core.outputs, link) {
		if (output->enabled) {
			commit_off(output);
			swapchain_destroy(output);
		}
	}
}

// Other sessions may have changed the CRTCs, so the enabled outputs are
// set up again by a modeset at the next repaint.
void
kms_resume(void)
{
	struct output *output;
	wl_list_for_each(output, &core.outputs, link) {
		if (output->enabled) {
			output->modeset_done = false;
			damage_output(output);
		}
	}
}

void
kms_fini(void)
{
	struct output *output, *tmp;
	wl_list_for_each_safe(output, tmp, &core.outputs, link) {
		output_destroy(output);
	}
	wl_event_source_remove(core.kms.monitor_source);
	udev_monitor_unref(core.kms.monitor);
	wl_event_source_remove(core.kms.source);
	gbm_device_destroy(core.gbm);
	libseat_close_device(core.session.handle, core.session.drm_device_id);
	core.session.drm_device_id = -1;
}
