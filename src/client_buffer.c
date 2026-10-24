#include <assert.h>
#include <poll.h>
#include <stdlib.h>
#include <unistd.h>

#include <gbm.h>
#include <linux/dma-buf.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "core.h"
#include "render.h"
#include "surface.h"

static void
client_buffer_destroy(struct client_buffer *buffer)
{
	assert(!buffer->committed);
	wl_array_release(&buffer->syncobj_releases);
	if (buffer->dmabuf_fd >= 0) {
		close(buffer->dmabuf_fd);
	}
	// No frame shows the buffer, which it would lock.
	if (buffer->fb) {
		drmModeRmFB(core.kms.fd, buffer->fb);
		gbm_bo_destroy(buffer->bo);
	}
	texture_destroy(buffer->texture);
	free(buffer);
}

static void
client_buffer_handle_resource_destroy(struct wl_listener *listener, void *data)
{
	struct client_buffer *buffer =
		wl_container_of(listener, buffer, resource_destroy);
	wl_list_remove(&listener->link);
	buffer->resource = NULL;
	if (buffer->locks == 0) {
		client_buffer_destroy(buffer);
	}
}

// The buffer lives until the client destroys the resource and no surface
// state holds it. A dma-buf takes the fd for its implicit fences.
struct client_buffer *
client_buffer_create(enum client_buffer_type type,
	struct wl_resource *resource, uint32_t width, uint32_t height,
	struct texture *texture, int dmabuf_fd)
{
	struct client_buffer *buffer = calloc(1, sizeof(*buffer));
	assert(buffer);
	buffer->type = type;
	buffer->resource = resource;
	buffer->width = width;
	buffer->height = height;
	wl_list_init(&buffer->release_callbacks);
	wl_array_init(&buffer->syncobj_releases);
	buffer->dmabuf_fd = dmabuf_fd;
	buffer->texture = texture;
	buffer->resource_destroy.notify = client_buffer_handle_resource_destroy;
	wl_resource_add_destroy_listener(resource, &buffer->resource_destroy);
	return buffer;
}

// libwayland creates shm buffers, so they are wrapped on first use.
struct client_buffer *
client_buffer_from_resource(struct wl_resource *resource)
{
	struct wl_listener *listener = wl_resource_get_destroy_listener(resource,
		client_buffer_handle_resource_destroy);
	if (listener) {
		struct client_buffer *buffer =
			wl_container_of(listener, buffer, resource_destroy);
		return buffer;
	}

	struct wl_shm_buffer *shm = wl_shm_buffer_get(resource);
	assert(shm);
	// libwayland only takes the formats that we advertise.
	const struct pixel_format *format =
		pixel_format_find_shm(wl_shm_buffer_get_format(shm));
	assert(format);
	int32_t width = wl_shm_buffer_get_width(shm);
	int32_t height = wl_shm_buffer_get_height(shm);
	assert(width > 0 && height > 0
		&& wl_shm_buffer_get_stride(shm) >= width * (int32_t)format->bytes);
	return client_buffer_create(CLIENT_BUFFER_SHM, resource,
		(uint32_t)width, (uint32_t)height, NULL, -1);
}

// Makes the client wait for the GPU work submitted so far, which may still
// sample the dma-buf, before drawing into it again. Its next drawing covers
// all the planes, so the fences of the first one are enough.
static void
dmabuf_add_render_fence(struct client_buffer *buffer)
{
	if (core.vk.sync_file < 0) {
		return;
	}
	struct dma_buf_import_sync_file import = {
		.flags = DMA_BUF_SYNC_READ,
		.fd = core.vk.sync_file,
	};
	assert(drmIoctl(buffer->dmabuf_fd, DMA_BUF_IOCTL_IMPORT_SYNC_FILE,
		&import) == 0);
}

// Sends wl_buffer.release and the releases of the commits. The release points
// of a dma-buf signal once the GPU work submitted so far completes, and the
// clients without them wait for it through its implicit fences.
static void
client_buffer_send_release(struct client_buffer *buffer)
{
	buffer->committed = false;
	if (buffer->type == CLIENT_BUFFER_DMABUF && buffer->syncobj_releases.size == 0) {
		dmabuf_add_render_fence(buffer);
	}
	if (buffer->resource) {
		wl_buffer_send_release(buffer->resource);
	}
	callbacks_send_done(&buffer->release_callbacks, 0);
	struct syncobj_point *point;
	wl_array_for_each(point, &buffer->syncobj_releases) {
		syncobj_point_signal(point, core.vk.sync_file);
		syncobj_point_unset(point);
	}
	buffer->syncobj_releases.size = 0;
}

// The buffer takes the releases of the commit, and sends them along with its
// own.
void
client_buffer_commit(struct client_buffer *buffer, struct surface_state *pending)
{
	buffer->committed = true;
	list_move(&buffer->release_callbacks, &pending->release_callbacks);
	if (pending->release.timeline) {
		struct syncobj_point *release = wl_array_add(
			&buffer->syncobj_releases, sizeof(*release));
		assert(release);
		*release = pending->release;
		pending->release = (struct syncobj_point){0};
	}
}

void
client_buffer_lock(struct client_buffer *buffer)
{
	buffer->locks++;
}

void
client_buffer_unlock(struct client_buffer *buffer)
{
	assert(buffer->locks > 0);
	buffer->locks--;
	if (buffer->locks > 0) {
		return;
	}
	// An shm buffer is released when uploaded, or else when its commits
	// are dropped.
	if (buffer->committed) {
		client_buffer_send_release(buffer);
	}
	if (!buffer->resource) {
		client_buffer_destroy(buffer);
	}
}

// Copies the contents into the texture, which keeps those of the previous
// buffer outside the damage if it has the same size and format. The client
// may reuse the buffer then.
void
client_buffer_upload_shm(struct client_buffer *buffer,
	struct texture **texture, const pixman_region32_t *damage)
{
	assert(buffer->type == CLIENT_BUFFER_SHM);
	texture_write_shm(texture, wl_shm_buffer_get(buffer->resource), damage);
	client_buffer_send_release(buffer);
}

// Returns a sync file that signals once the client has finished drawing into
// the dma-buf, or -1 if it has. The work of the client covers all the planes,
// so the fences of the first one are enough.
int
client_buffer_fence(struct client_buffer *buffer)
{
	struct dma_buf_export_sync_file export = {
		.flags = DMA_BUF_SYNC_READ,
		.fd = -1,
	};
	assert(drmIoctl(buffer->dmabuf_fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE,
		&export) == 0);
	struct pollfd pollfd = {.fd = export.fd, .events = POLLIN};
	if (poll(&pollfd, 1, 0) == 1) {
		close(export.fd);
		return -1;
	}
	return export.fd;
}
