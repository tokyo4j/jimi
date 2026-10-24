#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "core.h"
#include "image_capture.h"
#include "output.h"
#include "ext-image-capture-source-v1-protocol.h"
#include "ext-image-copy-capture-v1-protocol.h"

// Outputs and mapped windows are captured through ext-image-copy-capture.
// The renderer draws the source again into the buffer of each frame, with
// the damage accumulated since the last capture. Cursors are never reported
// to cursor sessions.

// An ext_image_capture_source_v1. One of the output and the window is set,
// until the output is disabled or the window unmaps.
struct capture_source {
	struct output *output;
	struct window *window;
	struct wl_list link; // output.capture_sources, window.capture.sources, or empty
};

enum capture_frame_state {
	CAPTURE_FRAME_NEW, // Takes the buffer until the client captures it.
	CAPTURE_FRAME_PENDING, // Captured once the source has damage.
	CAPTURE_FRAME_COPYING, // Until the GPU has drawn into the buffer.
	CAPTURE_FRAME_DONE, // After ready or failed.
};

// An ext_image_copy_capture_frame_v1
struct capture_frame {
	struct wl_resource *resource;
	struct capture_session *session; // NULL once the session is destroyed.
	enum capture_frame_state state;
	struct wl_resource *buffer; // NULL until attached, and once destroyed.
	struct wl_listener buffer_destroy;
	// Sent with ready
	pixman_region32_t damage;
	struct timespec time;
};

static void
capture_source_resource_destroy(struct wl_resource *resource)
{
	struct capture_source *source = wl_resource_get_user_data(resource);
	wl_list_remove(&source->link);
	free(source);
}

static const struct ext_image_capture_source_v1_interface
capture_source_impl = {
	.destroy = resource_destroy_req,
};

static struct capture_source *
capture_source_create(struct wl_client *client, uint32_t id)
{
	struct capture_source *source = calloc(1, sizeof(*source));
	assert(source);
	wl_list_init(&source->link);
	struct wl_resource *resource = wl_resource_create(client,
		&ext_image_capture_source_v1_interface, 1, id);
	assert(resource);
	wl_resource_set_implementation(resource, &capture_source_impl, source,
		capture_source_resource_destroy);
	return source;
}

static void
output_source_manager_create_source_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id, struct wl_resource *output_resource)
{
	struct capture_source *source = capture_source_create(client, id);
	source->output = wl_resource_get_user_data(output_resource);
	if (source->output) {
		list_append(&source->output->capture_sources, &source->link);
	}
}

static const struct ext_output_image_capture_source_manager_v1_interface
output_source_manager_impl = {
	.create_source = output_source_manager_create_source_req,
	.destroy = resource_destroy_req,
};

void
wayland_bind_output_image_capture_source_manager(struct wl_client *client,
	void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&ext_output_image_capture_source_manager_v1_interface, 1, id);
	assert(resource);
	wl_resource_set_implementation(resource, &output_source_manager_impl,
		NULL, NULL);
}

// The source of a closed handle is stopped from the start.
static void
toplevel_source_manager_create_source_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id, struct wl_resource *handle)
{
	struct capture_source *source = capture_source_create(client, id);
	source->window = wl_resource_get_user_data(handle);
	if (source->window) {
		list_append(&source->window->capture.sources, &source->link);
	}
}

static const struct ext_foreign_toplevel_image_capture_source_manager_v1_interface
toplevel_source_manager_impl = {
	.create_source = toplevel_source_manager_create_source_req,
	.destroy = resource_destroy_req,
};

void
wayland_bind_foreign_toplevel_image_capture_source_manager(
	struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&ext_foreign_toplevel_image_capture_source_manager_v1_interface, 1, id);
	assert(resource);
	wl_resource_set_implementation(resource, &toplevel_source_manager_impl,
		NULL, NULL);
}

static void
capture_frame_fail(struct capture_frame *frame,
	enum ext_image_copy_capture_frame_v1_failure_reason reason)
{
	ext_image_copy_capture_frame_v1_send_failed(frame->resource, reason);
	frame->state = CAPTURE_FRAME_DONE;
}

static void
capture_frame_send_ready(struct capture_frame *frame)
{
	ext_image_copy_capture_frame_v1_send_transform(frame->resource,
		WL_OUTPUT_TRANSFORM_NORMAL);
	int nr_rects;
	const pixman_box32_t *rects =
		pixman_region32_rectangles(&frame->damage, &nr_rects);
	for (int i = 0; i < nr_rects; i++) {
		struct box rect = box_from_pixman(&rects[i]);
		ext_image_copy_capture_frame_v1_send_damage(frame->resource,
			rect.x, rect.y, rect.width, rect.height);
	}
	uint64_t sec = (uint64_t)frame->time.tv_sec;
	ext_image_copy_capture_frame_v1_send_presentation_time(frame->resource,
		(uint32_t)(sec >> 32), (uint32_t)sec, (uint32_t)frame->time.tv_nsec);
	ext_image_copy_capture_frame_v1_send_ready(frame->resource);
	frame->state = CAPTURE_FRAME_DONE;
}

static void
capture_frame_handle_buffer_destroy(struct wl_listener *listener, void *data)
{
	struct capture_frame *frame = wl_container_of(listener, frame, buffer_destroy);
	wl_list_remove(&listener->link);
	frame->buffer = NULL;
}

static bool
capture_frame_check_new(struct capture_frame *frame)
{
	if (frame->state != CAPTURE_FRAME_NEW) {
		wl_resource_post_error(frame->resource,
			EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_ALREADY_CAPTURED,
			"frame was already captured");
		return false;
	}
	return true;
}

static void
capture_frame_attach_buffer_req(struct wl_client *client,
	struct wl_resource *resource, struct wl_resource *buffer)
{
	struct capture_frame *frame = wl_resource_get_user_data(resource);
	if (!capture_frame_check_new(frame)) {
		return;
	}
	if (frame->buffer) {
		wl_list_remove(&frame->buffer_destroy.link);
	}
	frame->buffer = buffer;
	wl_resource_add_destroy_listener(buffer, &frame->buffer_destroy);
}

// The whole buffer is drawn anyway.
static void
capture_frame_damage_buffer_req(struct wl_client *client,
	struct wl_resource *resource, int32_t x, int32_t y, int32_t width,
	int32_t height)
{
	struct capture_frame *frame = wl_resource_get_user_data(resource);
	if (!capture_frame_check_new(frame)) {
		return;
	}
	if (x < 0 || y < 0 || width <= 0 || height <= 0) {
		wl_resource_post_error(resource,
			EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_INVALID_BUFFER_DAMAGE,
			"invalid buffer damage");
	}
}

// The frame is captured when the output showing the source is repainted.
static void
capture_frame_capture_req(struct wl_client *client, struct wl_resource *resource)
{
	struct capture_frame *frame = wl_resource_get_user_data(resource);
	if (!capture_frame_check_new(frame)) {
		return;
	}
	if (!frame->buffer) {
		wl_resource_post_error(resource,
			EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_NO_BUFFER,
			"no buffer was attached");
		return;
	}
	if (!frame->session || frame->session->stopped) {
		capture_frame_fail(frame,
			EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED);
		return;
	}
	frame->state = CAPTURE_FRAME_PENDING;
}

static const struct ext_image_copy_capture_frame_v1_interface
capture_frame_impl = {
	.destroy = resource_destroy_req,
	.attach_buffer = capture_frame_attach_buffer_req,
	.damage_buffer = capture_frame_damage_buffer_req,
	.capture = capture_frame_capture_req,
};

// A copy in flight still finishes, but the damage it would have reported goes
// to the next frame.
static void
capture_frame_resource_destroy(struct wl_resource *resource)
{
	struct capture_frame *frame = wl_resource_get_user_data(resource);
	struct capture_session *session = frame->session;
	if (session) {
		if (frame->state == CAPTURE_FRAME_COPYING) {
			pixman_region32_union(&session->damage, &session->damage,
				&frame->damage);
		}
		session->frame = NULL;
	}
	if (frame->buffer) {
		wl_list_remove(&frame->buffer_destroy.link);
	}
	pixman_region32_fini(&frame->damage);
	free(frame);
}

static void
capture_session_create_frame_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id)
{
	struct capture_session *session = wl_resource_get_user_data(resource);
	if (session->frame) {
		wl_resource_post_error(resource,
			EXT_IMAGE_COPY_CAPTURE_SESSION_V1_ERROR_DUPLICATE_FRAME,
			"the previous frame was not destroyed");
		return;
	}
	struct capture_frame *frame = calloc(1, sizeof(*frame));
	assert(frame);
	frame->session = session;
	frame->buffer_destroy.notify = capture_frame_handle_buffer_destroy;
	pixman_region32_init(&frame->damage);
	frame->resource = wl_resource_create(client,
		&ext_image_copy_capture_frame_v1_interface,
		wl_resource_get_version(resource), id);
	assert(frame->resource);
	wl_resource_set_implementation(frame->resource, &capture_frame_impl,
		frame, capture_frame_resource_destroy);
	session->frame = frame;
}

static const struct ext_image_copy_capture_session_v1_interface
capture_session_impl = {
	.create_frame = capture_session_create_frame_req,
	.destroy = resource_destroy_req,
};

static void
capture_session_end_copy(struct capture_session *session)
{
	wl_event_source_remove(session->copy_source);
	close(session->copy_fd);
	session->copy_source = NULL;
}

// The frame fails, as it would never be captured.
static void
capture_session_resource_destroy(struct wl_resource *resource)
{
	struct capture_session *session = wl_resource_get_user_data(resource);
	struct capture_frame *frame = session->frame;
	if (frame) {
		if (frame->state == CAPTURE_FRAME_PENDING
				|| frame->state == CAPTURE_FRAME_COPYING) {
			capture_frame_fail(frame,
				EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED);
		}
		frame->session = NULL;
	}
	if (session->copy_source) {
		capture_session_end_copy(session);
	}
	renderer_capture_fini(session);
	wl_list_remove(&session->link);
	pixman_region32_fini(&session->damage);
	free(session);
}

static struct capture_session *
capture_session_create(struct wl_client *client, uint32_t version, uint32_t id)
{
	struct capture_session *session = calloc(1, sizeof(*session));
	assert(session);
	wl_list_init(&session->link);
	pixman_region32_init(&session->damage);
	session->resource = wl_resource_create(client,
		&ext_image_copy_capture_session_v1_interface, version, id);
	assert(session->resource);
	wl_resource_set_implementation(session->resource, &capture_session_impl,
		session, capture_session_resource_destroy);
	renderer_capture_init(session);
	return session;
}

// Buffers of both formats can be shm buffers, or dma-bufs with the
// modifiers the renderer can draw into. The frames are in sRGB.
static void
capture_session_send_constraints(struct capture_session *session)
{
	struct wl_resource *resource = session->resource;
	static const uint32_t formats[] = {
		DRM_FORMAT_XRGB8888,
		DRM_FORMAT_ARGB8888,
	};
	ext_image_copy_capture_session_v1_send_shm_format(resource,
		WL_SHM_FORMAT_XRGB8888);
	ext_image_copy_capture_session_v1_send_shm_format(resource,
		WL_SHM_FORMAT_ARGB8888);

	// Both formats have the same Vulkan format, and thus the same modifiers.
	const struct format_support *support =
		format_support(pixel_format_find(DRM_FORMAT_ARGB8888));
	struct wl_array modifiers;
	wl_array_init(&modifiers);
	for (uint32_t i = 0; i < support->nr_modifiers; i++) {
		if (support->modifiers[i].renderable) {
			uint64_t *modifier = wl_array_add(&modifiers, sizeof(*modifier));
			assert(modifier);
			*modifier = support->modifiers[i].modifier;
		}
	}
	if (modifiers.size > 0) {
		struct wl_array device = {
			.size = sizeof(core.dmabuf_feedback.device),
			.data = &core.dmabuf_feedback.device,
		};
		ext_image_copy_capture_session_v1_send_dmabuf_device(resource, &device);
		for (size_t i = 0; i < ARRAY_LEN(formats); i++) {
			ext_image_copy_capture_session_v1_send_dmabuf_format(resource,
				formats[i], &modifiers);
		}
	}
	wl_array_release(&modifiers);

	ext_image_copy_capture_session_v1_send_buffer_size(resource,
		(uint32_t)session->width, (uint32_t)session->height);
	ext_image_copy_capture_session_v1_send_done(resource);
}

// The size of the source in the pixels of the output showing it. The window
// is drawn like fractional-scale clients round their buffer sizes.
static void
capture_session_source_size(struct capture_session *session, int *width, int *height)
{
	if (session->output) {
		struct box box = output_physical_box(session->output);
		*width = box.width;
		*height = box.height;
		return;
	}
	struct window *window = session->window;
	// Windows have no output while no output is enabled.
	double scale = window->output ? window->output->scale : 1.0;
	*width = (int)round(window->current.geometry.width * scale);
	*height = (int)round(window->current.geometry.height * scale);
}

static void
capture_session_damage_buffer(struct capture_session *session)
{
	pixman_region32_reset(&session->damage, &(pixman_box32_t){
		.x2 = session->width,
		.y2 = session->height,
	});
}

// Sends the constraints for the buffer size, and damages the whole buffer.
static void
capture_session_resize(struct capture_session *session, int width, int height)
{
	session->width = width;
	session->height = height;
	capture_session_damage_buffer(session);
	capture_session_send_constraints(session);
}

static void
capture_session_stop(struct capture_session *session)
{
	session->output = NULL;
	session->window = NULL;
	session->stopped = true;
	wl_list_remove(&session->link);
	wl_list_init(&session->link);
	ext_image_copy_capture_session_v1_send_stopped(session->resource);
	struct capture_frame *frame = session->frame;
	if (frame && (frame->state == CAPTURE_FRAME_PENDING
			|| frame->state == CAPTURE_FRAME_COPYING)) {
		capture_frame_fail(frame,
			EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED);
	}
}

// Shm buffers are filled once the GPU is done.
static int
handle_copied(int fd, uint32_t mask, void *data)
{
	struct capture_session *session = data;
	capture_session_end_copy(session);
	struct capture_frame *frame = session->frame;
	if (!frame || frame->state != CAPTURE_FRAME_COPYING) {
		return 0;
	}
	struct wl_shm_buffer *shm =
		frame->buffer ? wl_shm_buffer_get(frame->buffer) : NULL;
	if (shm) {
		renderer_capture_read(session, shm);
	}
	capture_frame_send_ready(frame);
	return 0;
}

// Captures the pending frame if the source has damage, and the last copy is
// done.
static void
capture_session_capture(struct capture_session *session)
{
	struct capture_frame *frame = session->frame;
	if (!frame || frame->state != CAPTURE_FRAME_PENDING
			|| session->copy_source
			|| !pixman_region32_not_empty(&session->damage)) {
		return;
	}
	// The client reallocates its buffers for the new size.
	int width, height;
	capture_session_source_size(session, &width, &height);
	if (width != session->width || height != session->height) {
		capture_session_resize(session, width, height);
		capture_frame_fail(frame,
			EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS);
		return;
	}
	int fd = frame->buffer ? renderer_capture(session, frame->buffer) : -1;
	if (fd < 0) {
		capture_frame_fail(frame,
			EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS);
		return;
	}
	session->copy_fd = fd;
	session->copy_source = wl_event_loop_add_fd(core.wl_loop, fd,
		WL_EVENT_READABLE, handle_copied, session);
	assert(session->copy_source);
	frame->state = CAPTURE_FRAME_COPYING;
	pixman_region32_copy(&frame->damage, &session->damage);
	pixman_region32_clear(&session->damage);
	assert(clock_gettime(CLOCK_MONOTONIC, &frame->time) == 0);
}

// Starts with the constraints and full damage, or stopped if the output of
// the source is disabled or its window has unmapped.
static void
capture_manager_create_session_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id,
	struct wl_resource *source_resource, uint32_t options)
{
	if (options & ~EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS) {
		wl_resource_post_error(resource,
			EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_ERROR_INVALID_OPTION,
			"invalid options");
		return;
	}
	struct capture_source *source = wl_resource_get_user_data(source_resource);
	struct capture_session *session = capture_session_create(client,
		wl_resource_get_version(resource), id);
	session->paint_cursors =
		options & EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS;
	if (source->output) {
		session->output = source->output;
		list_append(&source->output->capture_sessions, &session->link);
	} else if (source->window) {
		session->window = source->window;
		list_append(&source->window->capture.sessions, &session->link);
	} else {
		session->stopped = true;
		ext_image_copy_capture_session_v1_send_stopped(session->resource);
		return;
	}
	int width, height;
	capture_session_source_size(session, &width, &height);
	capture_session_resize(session, width, height);
}

static void
cursor_session_get_capture_session_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id)
{
	bool *has_session = wl_resource_get_user_data(resource);
	if (*has_session) {
		wl_resource_post_error(resource,
			EXT_IMAGE_COPY_CAPTURE_CURSOR_SESSION_V1_ERROR_DUPLICATE_SESSION,
			"the capture session was already created");
		return;
	}
	*has_session = true;
	// Without a source, the session is never captured.
	struct capture_session *session = capture_session_create(client,
		wl_resource_get_version(resource), id);
	capture_session_resize(session, 1, 1);
}

static const struct ext_image_copy_capture_cursor_session_v1_interface
cursor_session_impl = {
	.destroy = resource_destroy_req,
	.get_capture_session = cursor_session_get_capture_session_req,
};

static void
cursor_session_resource_destroy(struct wl_resource *resource)
{
	free(wl_resource_get_user_data(resource));
}

// The cursor never enters the captured area.
static void
capture_manager_create_pointer_cursor_session_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id,
	struct wl_resource *source_resource, struct wl_resource *pointer)
{
	bool *has_session = calloc(1, sizeof(*has_session));
	assert(has_session);
	struct wl_resource *cursor_session = wl_resource_create(client,
		&ext_image_copy_capture_cursor_session_v1_interface,
		wl_resource_get_version(resource), id);
	assert(cursor_session);
	wl_resource_set_implementation(cursor_session, &cursor_session_impl,
		has_session, cursor_session_resource_destroy);
}

static const struct ext_image_copy_capture_manager_v1_interface
capture_manager_impl = {
	.create_session = capture_manager_create_session_req,
	.create_pointer_cursor_session =
		capture_manager_create_pointer_cursor_session_req,
	.destroy = resource_destroy_req,
};

void
wayland_bind_image_copy_capture_manager(struct wl_client *client,
	void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&ext_image_copy_capture_manager_v1_interface, 1, id);
	assert(resource);
	wl_resource_set_implementation(resource, &capture_manager_impl, NULL, NULL);
}

// The damage is in the pixels of the output.
void
image_capture_damage_output(struct output *output, const struct box *damage)
{
	struct capture_session *session;
	wl_list_for_each(session, &output->capture_sessions, link) {
		pixman_region32_union_rect(&session->damage, &session->damage,
			damage->x, damage->y, (unsigned)damage->width, (unsigned)damage->height);
	}
}

// Called on each commit to the surfaces of the window, and when it moves to
// another output.
void
image_capture_damage_window(struct window *window)
{
	struct capture_session *session;
	wl_list_for_each(session, &window->capture.sessions, link) {
		capture_session_damage_buffer(session);
	}
}

// The sources lose their output or window, and the sessions stop.
static void
stop_captures(struct wl_list *sources, struct wl_list *sessions)
{
	struct capture_source *source, *tmp_source;
	wl_list_for_each_safe(source, tmp_source, sources, link) {
		source->output = NULL;
		source->window = NULL;
		wl_list_remove(&source->link);
		wl_list_init(&source->link);
	}
	struct capture_session *session, *tmp_session;
	wl_list_for_each_safe(session, tmp_session, sessions, link) {
		capture_session_stop(session);
	}
}

void
image_capture_output_disable(struct output *output)
{
	stop_captures(&output->capture_sources, &output->capture_sessions);
}

void
image_capture_window_unmap(struct window *window)
{
	stop_captures(&window->capture.sources, &window->capture.sessions);
}

// Captures the sources shown on the output, which is about to be repainted.
// A window is shown on the output under its center.
void
image_capture_repaint(struct output *output)
{
	struct capture_session *session;
	wl_list_for_each(session, &output->capture_sessions, link) {
		capture_session_capture(session);
	}
	struct window *window;
	wl_list_for_each(window, &core.windows, link) {
		if (window->output != output) {
			continue;
		}
		wl_list_for_each(session, &window->capture.sessions, link) {
			capture_session_capture(session);
		}
	}
}
