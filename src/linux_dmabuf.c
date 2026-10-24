#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core.h"
#include "output.h"
#include "surface.h"
#include "linux-dmabuf-v1-protocol.h"

struct dmabuf_plane {
	int fd;
	uint32_t offset;
	uint32_t stride;
	uint64_t modifier;
	bool set;
};

struct dmabuf_params {
	struct dmabuf_plane planes[MAX_DMABUF_PLANES];
	bool used;
};

static const struct zwp_linux_dmabuf_v1_interface dmabuf_impl;
static const struct zwp_linux_buffer_params_v1_interface params_impl;
static const struct zwp_linux_dmabuf_feedback_v1_interface feedback_impl;

struct dmabuf_format_modifier {
	uint32_t format;
	uint32_t padding;
	uint64_t modifier;
};

static_assert(sizeof(struct dmabuf_format_modifier) == 16,
	"dma-buf feedback table entries are 16 bytes");

static const struct wl_buffer_interface buffer_impl = {
	.destroy = resource_destroy_req,
};

static void
params_resource_destroy(struct wl_resource *resource)
{
	struct dmabuf_params *params = wl_resource_get_user_data(resource);
	for (size_t i = 0; i < ARRAY_LEN(params->planes); i++) {
		if (params->planes[i].fd >= 0) {
			close(params->planes[i].fd);
		}
	}
	free(params);
}

static void
params_add_req(struct wl_client *client, struct wl_resource *resource, int fd,
	uint32_t plane_idx, uint32_t offset, uint32_t stride,
	uint32_t modifier_hi, uint32_t modifier_lo)
{
	struct dmabuf_params *params = wl_resource_get_user_data(resource);
	if (params->used) {
		close(fd);
		wl_resource_post_error(resource,
			ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,
			"buffer parameters were already used");
		return;
	}
	if (plane_idx >= ARRAY_LEN(params->planes)) {
		close(fd);
		wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_IDX,
			"plane index is out of range");
		return;
	}
	if (params->planes[plane_idx].set) {
		close(fd);
		wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_SET,
			"plane was already added");
		return;
	}
	int flags = fcntl(fd, F_GETFD);
	assert(flags >= 0);
	assert(fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0);
	params->planes[plane_idx] = (struct dmabuf_plane){
		.fd = fd,
		.offset = offset,
		.stride = stride,
		.modifier = ((uint64_t)modifier_hi << 32) | modifier_lo,
		.set = true,
	};
}

static void
params_create_common(struct wl_resource *resource, uint32_t buffer_id,
	int32_t width, int32_t height, uint32_t format, uint32_t flags)
{
	struct dmabuf_params *params = wl_resource_get_user_data(resource);
	if (params->used) {
		wl_resource_post_error(resource,
			ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,
			"buffer parameters were already used");
		return;
	}
	params->used = true;

	const struct pixel_format *pixel_format = pixel_format_find(format);
	if (!pixel_format || flags & ~ZWP_LINUX_BUFFER_PARAMS_V1_FLAGS_Y_INVERT) {
		wl_resource_post_error(resource,
			ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT,
			"unsupported dma-buf format or flags");
		return;
	}
	if (width <= 0 || height <= 0) {
		wl_resource_post_error(resource,
			ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_DIMENSIONS,
			"buffer dimensions must be positive");
		return;
	}
	uint32_t nr_planes = 0;
	while (nr_planes < ARRAY_LEN(params->planes) && params->planes[nr_planes].set) {
		nr_planes++;
	}
	if (nr_planes == 0) {
		wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE,
			"plane zero was not added");
		return;
	}
	for (size_t i = nr_planes; i < ARRAY_LEN(params->planes); i++) {
		if (params->planes[i].set) {
			wl_resource_post_error(resource,
				ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE,
				"plane indices must be consecutive");
			return;
		}
	}

	uint64_t modifier = params->planes[0].modifier;
	uint64_t min_stride = (uint64_t)width * pixel_format->bytes;
	for (uint32_t i = 0; i < nr_planes; i++) {
		struct dmabuf_plane *plane = &params->planes[i];
		uint64_t end = (uint64_t)plane->offset
			+ (uint64_t)plane->stride * (uint32_t)height;
		if (plane->modifier != modifier) {
			wl_resource_post_error(resource,
				ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT,
				"all planes must use the same modifier");
			return;
		}
		if (plane->stride == 0 || (i == 0 && plane->stride < min_stride)
				|| end > UINT32_MAX) {
			wl_resource_post_error(resource,
				ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_OUT_OF_BOUNDS,
				"plane layout exceeds supported bounds");
			return;
		}
	}
	const struct dmabuf_modifier *supported = dmabuf_modifier_find(format, modifier);
	if (!supported || nr_planes != supported->nr_planes) {
		wl_resource_post_error(resource,
			ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT,
			"unsupported format, modifier, or plane count");
		return;
	}

	struct dmabuf_attributes attributes = {
		.nr_planes = nr_planes,
		.width = (uint32_t)width,
		.height = (uint32_t)height,
		.format = format,
		.modifier = modifier,
		.y_invert = flags & ZWP_LINUX_BUFFER_PARAMS_V1_FLAGS_Y_INVERT,
	};
	for (uint32_t i = 0; i < nr_planes; i++) {
		attributes.fds[i] = params->planes[i].fd;
		attributes.offsets[i] = params->planes[i].offset;
		attributes.strides[i] = params->planes[i].stride;
	}
	// The texture keeps the memory, so the params close their fds except
	// the one the buffer takes.
	struct texture *texture = texture_import_dmabuf(&attributes);
	if (!texture) {
		if (buffer_id == 0) {
			zwp_linux_buffer_params_v1_send_failed(resource);
		} else {
			wl_resource_post_error(resource,
				ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_WL_BUFFER,
				"dma-buf import failed");
		}
		return;
	}

	struct wl_client *client = wl_resource_get_client(resource);
	struct wl_resource *buffer_resource = wl_resource_create(client,
		&wl_buffer_interface, 1, buffer_id);
	assert(buffer_resource);
	wl_resource_set_implementation(buffer_resource, &buffer_impl, NULL, NULL);
	struct client_buffer *buffer = client_buffer_create(CLIENT_BUFFER_DMABUF,
		buffer_resource, (uint32_t)width, (uint32_t)height, texture,
		params->planes[0].fd);
	buffer->fb = kms_import_dmabuf(&attributes, &buffer->bo);
	params->planes[0].fd = -1;
	if (buffer_id == 0) {
		zwp_linux_buffer_params_v1_send_created(resource, buffer_resource);
	}
}

static void
params_create_req(struct wl_client *client, struct wl_resource *resource,
	int32_t width, int32_t height, uint32_t format, uint32_t flags)
{
	params_create_common(resource, 0, width, height, format, flags);
}

static void
params_create_immed_req(struct wl_client *client, struct wl_resource *resource,
	uint32_t buffer_id, int32_t width, int32_t height, uint32_t format,
	uint32_t flags)
{
	params_create_common(resource, buffer_id, width, height, format, flags);
}

static const struct zwp_linux_buffer_params_v1_interface params_impl = {
	.destroy = resource_destroy_req,
	.add = params_add_req,
	.create = params_create_req,
	.create_immed = params_create_immed_req,
};

static void
dmabuf_create_params_req(struct wl_client *client, struct wl_resource *resource,
	uint32_t id)
{
	struct dmabuf_params *params = calloc(1, sizeof(*params));
	assert(params);
	for (size_t i = 0; i < ARRAY_LEN(params->planes); i++) {
		params->planes[i].fd = -1;
	}
	struct wl_resource *params_resource = wl_resource_create(client,
		&zwp_linux_buffer_params_v1_interface,
		wl_resource_get_version(resource), id);
	assert(params_resource);
	wl_resource_set_implementation(params_resource, &params_impl, params,
		params_resource_destroy);
}

static const struct zwp_linux_dmabuf_feedback_v1_interface feedback_impl = {
	.destroy = resource_destroy_req,
};

// Sends the entries of the format table that the plane can scan out as their
// opaque formats, or all of them without a plane.
static void
send_tranche(struct wl_resource *resource, struct wl_array *device,
	const struct plane *plane)
{
	struct dmabuf_feedback *feedback = &core.dmabuf_feedback;
	struct wl_array indices;
	wl_array_init(&indices);
	for (uint32_t i = 0; i < feedback->nr_formats; i++) {
		const struct dmabuf_format_modifier *entry = &feedback->table[i];
		if (plane && !plane_supports(plane,
				pixel_format_find(entry->format)->opaque, entry->modifier)) {
			continue;
		}
		uint16_t *index = wl_array_add(&indices, sizeof(*index));
		assert(index);
		*index = (uint16_t)i;
	}
	if (indices.size > 0) {
		zwp_linux_dmabuf_feedback_v1_send_tranche_target_device(resource, device);
		zwp_linux_dmabuf_feedback_v1_send_tranche_flags(resource, plane
			? ZWP_LINUX_DMABUF_FEEDBACK_V1_TRANCHE_FLAGS_SCANOUT : 0);
		zwp_linux_dmabuf_feedback_v1_send_tranche_formats(resource, &indices);
		zwp_linux_dmabuf_feedback_v1_send_tranche_done(resource);
	}
	wl_array_release(&indices);
}

// The formats that the primary plane of the output can scan out are
// preferred if it is set.
static void
feedback_send(struct wl_resource *resource, struct output *scanout_output)
{
	struct dmabuf_feedback *feedback = &core.dmabuf_feedback;
	struct wl_array device = {
		.size = sizeof(feedback->device),
		.data = &feedback->device,
	};
	zwp_linux_dmabuf_feedback_v1_send_main_device(resource, &device);
	zwp_linux_dmabuf_feedback_v1_send_format_table(resource,
		feedback->table_fd, feedback->table_size);
	if (scanout_output) {
		send_tranche(resource, &device, &scanout_output->primary);
	}
	send_tranche(resource, &device, NULL);
	zwp_linux_dmabuf_feedback_v1_send_done(resource);
}

static void
dmabuf_get_default_feedback_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id)
{
	struct wl_resource *feedback_resource = wl_resource_create(client,
		&zwp_linux_dmabuf_feedback_v1_interface,
		wl_resource_get_version(resource), id);
	assert(feedback_resource);
	wl_resource_set_implementation(feedback_resource, &feedback_impl, NULL, NULL);
	feedback_send(feedback_resource, NULL);
}

// Sent again when the surface is suggested to allocate for scanout, or not
// anymore.
static void
dmabuf_get_surface_feedback_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id, struct wl_resource *surface_resource)
{
	struct surface *surface = wl_resource_get_user_data(surface_resource);
	struct wl_resource *feedback_resource = wl_resource_create(client,
		&zwp_linux_dmabuf_feedback_v1_interface,
		wl_resource_get_version(resource), id);
	assert(feedback_resource);
	wl_resource_set_implementation(feedback_resource, &feedback_impl, NULL,
		resource_unlink);
	wl_list_insert(&surface->dmabuf_feedbacks,
		wl_resource_get_link(feedback_resource));
	feedback_send(feedback_resource, surface->scanout_output);
}

// Clients reallocate their buffers for new feedback, so it is sent only when
// it changes.
void
dmabuf_feedback_set_scanout_output(struct surface *surface, struct output *output)
{
	if (surface->scanout_output == output) {
		return;
	}
	surface->scanout_output = output;
	struct wl_resource *resource;
	wl_resource_for_each(resource, &surface->dmabuf_feedbacks) {
		feedback_send(resource, output);
	}
}

// The surfaces suggested to allocate for the primary plane of the output are
// not anymore.
void
dmabuf_feedback_unset_scanout_output(struct output *output)
{
	struct surface *surface;
	wl_list_for_each(surface, &core.surfaces, link) {
		if (surface->scanout_output == output) {
			dmabuf_feedback_set_scanout_output(surface, NULL);
		}
	}
}

// The feedback resources stay until the client destroys them, without a
// surface.
void
dmabuf_feedback_surface_destroy(struct surface *surface)
{
	struct wl_resource *resource, *tmp;
	wl_resource_for_each_safe(resource, tmp, &surface->dmabuf_feedbacks) {
		wl_list_remove(wl_resource_get_link(resource));
		wl_list_init(wl_resource_get_link(resource));
	}
}

static const struct zwp_linux_dmabuf_v1_interface dmabuf_impl = {
	.destroy = resource_destroy_req,
	.create_params = dmabuf_create_params_req,
	.get_default_feedback = dmabuf_get_default_feedback_req,
	.get_surface_feedback = dmabuf_get_surface_feedback_req,
};

// Creates the format table of the feedback, which lists each format with each
// of its modifiers.
void
linux_dmabuf_init(void)
{
	struct dmabuf_feedback *feedback = &core.dmabuf_feedback;
	for (size_t i = 0; i < ARRAY_LEN(pixel_formats); i++) {
		feedback->nr_formats += format_support(&pixel_formats[i])->nr_modifiers;
	}
	assert(feedback->nr_formats <= (uint32_t)UINT16_MAX + 1);
	feedback->table_size = feedback->nr_formats
		* sizeof(struct dmabuf_format_modifier);
	feedback->table = calloc(feedback->nr_formats, sizeof(*feedback->table));
	assert(feedback->table);
	struct dmabuf_format_modifier *entry = feedback->table;
	for (size_t i = 0; i < ARRAY_LEN(pixel_formats); i++) {
		const struct format_support *support = format_support(&pixel_formats[i]);
		for (uint32_t j = 0; j < support->nr_modifiers; j++) {
			*entry++ = (struct dmabuf_format_modifier){
				.format = pixel_formats[i].drm,
				.modifier = support->modifiers[j].modifier,
			};
		}
	}
	feedback->table_fd = create_sealed_file("jimi-dmabuf-feedback",
		feedback->table, feedback->table_size);
	struct stat stat;
	assert(fstat(core.kms.fd, &stat) == 0);
	feedback->device = stat.st_rdev;
}

void
wayland_bind_linux_dmabuf(struct wl_client *client, void *data,
	uint32_t version, uint32_t id)
{
	uint32_t bind_version = MIN(version, 4);
	struct wl_resource *resource = wl_resource_create(client,
		&zwp_linux_dmabuf_v1_interface, bind_version, id);
	assert(resource);
	wl_resource_set_implementation(resource, &dmabuf_impl, data, NULL);
	if (bind_version >= 4) {
		// These clients get the same list from the feedback object.
		return;
	}
	struct dmabuf_feedback *feedback = &core.dmabuf_feedback;
	for (uint32_t i = 0; i < feedback->nr_formats; i++) {
		const struct dmabuf_format_modifier *entry = &feedback->table[i];
		if (i == 0 || entry->format != entry[-1].format) {
			zwp_linux_dmabuf_v1_send_format(resource, entry->format);
		}
		zwp_linux_dmabuf_v1_send_modifier(resource, entry->format,
			(uint32_t)(entry->modifier >> 32), (uint32_t)entry->modifier);
	}
}

void
linux_dmabuf_fini(void)
{
	close(core.dmabuf_feedback.table_fd);
	free(core.dmabuf_feedback.table);
}
