#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <xf86drm.h>

#include "core.h"
#include "surface.h"
#include "linux-drm-syncobj-v1-protocol.h"

// Explicit synchronization: along with each buffer, the client sets the
// timeline point that signals once it has finished drawing into the buffer,
// and the one we signal once the buffer is released. The timelines are DRM
// syncobjs imported into the device of the outputs.

static const struct wp_linux_drm_syncobj_timeline_v1_interface
		syncobj_timeline_impl;
static const struct wp_linux_drm_syncobj_surface_v1_interface
		syncobj_surface_impl;

static void
syncobj_timeline_unref(struct syncobj_timeline *timeline)
{
	assert(timeline->refs > 0);
	if (--timeline->refs > 0) {
		return;
	}
	assert(drmSyncobjDestroy(core.kms.fd, timeline->handle) == 0);
	free(timeline);
}

void
syncobj_point_set(struct syncobj_point *point,
	struct syncobj_timeline *timeline, uint64_t value)
{
	timeline->refs++;
	syncobj_point_unset(point);
	*point = (struct syncobj_point){.timeline = timeline, .value = value};
}

// Unsets the point, if set.
void
syncobj_point_unset(struct syncobj_point *point)
{
	if (point->timeline) {
		syncobj_timeline_unref(point->timeline);
	}
	*point = (struct syncobj_point){0};
}

// Returns an eventfd that becomes readable once the point signals, or -1 if
// it has. The client may not have submitted the work for the point yet.
int
syncobj_point_fence(const struct syncobj_point *point)
{
	uint32_t handle = point->timeline->handle;
	uint64_t value = point->value;
	if (drmSyncobjTimelineWait(core.kms.fd, &handle, &value, 1, 0,
			DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, NULL) == 0) {
		return -1;
	}
	int fd = eventfd(0, EFD_CLOEXEC);
	assert(fd >= 0);
	assert(drmSyncobjEventfd(core.kms.fd, handle, value, fd, 0) == 0);
	return fd;
}

// Signals the point once the sync file signals, or at once if it is -1.
void
syncobj_point_signal(const struct syncobj_point *point, int sync_file)
{
	uint32_t handle = point->timeline->handle;
	uint64_t value = point->value;
	if (sync_file < 0) {
		assert(drmSyncobjTimelineSignal(core.kms.fd, &handle, &value, 1) == 0);
		return;
	}
	struct drm_syncobj_handle import = {
		.handle = handle,
		.flags = DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE
			| DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_TIMELINE,
		.fd = sync_file,
		.point = value,
	};
	assert(drmIoctl(core.kms.fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &import) == 0);
}

static void
syncobj_timeline_resource_destroy(struct wl_resource *resource)
{
	syncobj_timeline_unref(wl_resource_get_user_data(resource));
}

static const struct wp_linux_drm_syncobj_timeline_v1_interface
		syncobj_timeline_impl = {
	.destroy = resource_destroy_req,
};

static struct syncobj_timeline *
syncobj_timeline_from_resource(struct wl_resource *resource)
{
	assert(wl_resource_instance_of(resource,
		&wp_linux_drm_syncobj_timeline_v1_interface,
		&syncobj_timeline_impl));
	return wl_resource_get_user_data(resource);
}

// The surface of the resource, or NULL with an error if it is destroyed.
static struct surface *
surface_from_syncobj_surface(struct wl_resource *resource)
{
	struct surface *surface = wl_resource_get_user_data(resource);
	if (!surface) {
		wl_resource_post_error(resource,
			WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_SURFACE,
			"the wl_surface was destroyed");
	}
	return surface;
}

static void
syncobj_surface_set_acquire_point_req(struct wl_client *client,
	struct wl_resource *resource, struct wl_resource *timeline,
	uint32_t point_hi, uint32_t point_lo)
{
	struct surface *surface = surface_from_syncobj_surface(resource);
	if (surface) {
		syncobj_point_set(&surface->pending.acquire,
			syncobj_timeline_from_resource(timeline),
			(uint64_t)point_hi << 32 | point_lo);
	}
}

static void
syncobj_surface_set_release_point_req(struct wl_client *client,
	struct wl_resource *resource, struct wl_resource *timeline,
	uint32_t point_hi, uint32_t point_lo)
{
	struct surface *surface = surface_from_syncobj_surface(resource);
	if (surface) {
		syncobj_point_set(&surface->pending.release,
			syncobj_timeline_from_resource(timeline),
			(uint64_t)point_hi << 32 | point_lo);
	}
}

static const struct wp_linux_drm_syncobj_surface_v1_interface
		syncobj_surface_impl = {
	.destroy = resource_destroy_req,
	.set_acquire_point = syncobj_surface_set_acquire_point_req,
	.set_release_point = syncobj_surface_set_release_point_req,
};

// The points set since the last commit are dropped, and the next buffers are
// synchronized implicitly.
static void
syncobj_surface_resource_destroy(struct wl_resource *resource)
{
	struct surface *surface = wl_resource_get_user_data(resource);
	if (!surface) {
		return;
	}
	surface->syncobj = NULL;
	syncobj_point_unset(&surface->pending.acquire);
	syncobj_point_unset(&surface->pending.release);
}

// Checks the points of the commit against the attached buffer.
bool
syncobj_surface_validate(struct surface *surface)
{
	struct surface_state *pending = &surface->pending;
	struct syncobj_point *acquire = &pending->acquire;
	struct syncobj_point *release = &pending->release;
	if (!pending->buffer) {
		if (acquire->timeline || release->timeline) {
			wl_resource_post_error(surface->syncobj,
				WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_BUFFER,
				"timeline points set without a buffer");
			return false;
		}
		return true;
	}
	if (pending->buffer->type != CLIENT_BUFFER_DMABUF) {
		wl_resource_post_error(surface->syncobj,
			WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_UNSUPPORTED_BUFFER,
			"only dma-bufs support explicit synchronization");
		return false;
	}
	if (!acquire->timeline) {
		wl_resource_post_error(surface->syncobj,
			WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_ACQUIRE_POINT,
			"buffer attached without an acquire point");
		return false;
	}
	if (!release->timeline) {
		wl_resource_post_error(surface->syncobj,
			WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_RELEASE_POINT,
			"buffer attached without a release point");
		return false;
	}
	if (acquire->timeline == release->timeline && acquire->value >= release->value) {
		wl_resource_post_error(surface->syncobj,
			WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_CONFLICTING_POINTS,
			"acquire point is not before the release point");
		return false;
	}
	return true;
}

// The resource stays alive, but becomes inert.
void
syncobj_surface_destroy(struct surface *surface)
{
	if (surface->syncobj) {
		wl_resource_set_user_data(surface->syncobj, NULL);
	}
}

static void
syncobj_manager_get_surface_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id, struct wl_resource *surface_resource)
{
	struct surface *surface = wl_resource_get_user_data(surface_resource);
	if (surface->syncobj) {
		wl_resource_post_error(resource,
			WP_LINUX_DRM_SYNCOBJ_MANAGER_V1_ERROR_SURFACE_EXISTS,
			"the surface already has a syncobj surface");
		return;
	}
	surface->syncobj = wl_resource_create(client,
		&wp_linux_drm_syncobj_surface_v1_interface,
		wl_resource_get_version(resource), id);
	assert(surface->syncobj);
	wl_resource_set_implementation(surface->syncobj, &syncobj_surface_impl,
		surface, syncobj_surface_resource_destroy);
}

static void
syncobj_manager_import_timeline_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id, int fd)
{
	uint32_t handle;
	int ret = drmSyncobjFDToHandle(core.kms.fd, fd, &handle);
	close(fd);
	if (ret != 0) {
		wl_resource_post_error(resource,
			WP_LINUX_DRM_SYNCOBJ_MANAGER_V1_ERROR_INVALID_TIMELINE,
			"the fd is not a DRM syncobj");
		return;
	}
	struct syncobj_timeline *timeline = calloc(1, sizeof(*timeline));
	assert(timeline);
	timeline->handle = handle;
	timeline->refs = 1;
	struct wl_resource *timeline_resource = wl_resource_create(client,
		&wp_linux_drm_syncobj_timeline_v1_interface,
		wl_resource_get_version(resource), id);
	assert(timeline_resource);
	wl_resource_set_implementation(timeline_resource, &syncobj_timeline_impl,
		timeline, syncobj_timeline_resource_destroy);
}

static const struct wp_linux_drm_syncobj_manager_v1_interface
		syncobj_manager_impl = {
	.destroy = resource_destroy_req,
	.get_surface = syncobj_manager_get_surface_req,
	.import_timeline = syncobj_manager_import_timeline_req,
};

// Drivers without timeline syncobjs, and kernels without syncobj eventfds
// (before 6.6), leave clients to implicit synchronization.
bool
linux_drm_syncobj_supported(void)
{
	uint64_t timeline;
	if (drmGetCap(core.kms.fd, DRM_CAP_SYNCOBJ_TIMELINE, &timeline) != 0
			|| !timeline) {
		return false;
	}
	// Only kernels supporting eventfds fail on the invalid handle 0 with
	// ENOENT; older ones reject the unknown ioctl
	return drmSyncobjEventfd(core.kms.fd, 0, 0, -1, 0) != 0 && errno == ENOENT;
}

void
wayland_bind_linux_drm_syncobj_manager(struct wl_client *client, void *data,
	uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&wp_linux_drm_syncobj_manager_v1_interface, MIN(version, 1), id);
	assert(resource);
	wl_resource_set_implementation(resource, &syncobj_manager_impl, data, NULL);
}
