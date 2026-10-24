#ifndef JIMI_IMAGE_CAPTURE_H
#define JIMI_IMAGE_CAPTURE_H

#include <vulkan/vulkan.h>

#include "util.h"

struct capture_frame;
struct output;
struct texture;
struct window;

// An ext_image_copy_capture_session_v1. Its frames are captured when the
// output showing the source is repainted, if the source has changed since the
// last capture.
struct capture_session {
	struct wl_resource *resource;
	struct capture_frame *frame; // NULL if none
	// The source, where one of them is set. Both are NULL once the session
	// stops, and for the sessions of cursor sessions, which are never
	// captured since the cursor never enters the captured area.
	struct output *output;
	struct window *window;
	struct wl_list link; // output.capture_sessions, window.capture.sessions, or empty
	bool paint_cursors;
	bool stopped;
	// The buffer size last sent, which the source has in the pixels of its
	// output.
	int width;
	int height;
	pixman_region32_t damage; // Buffer pixels changed since the last capture.
	// Readable once the GPU has drawn the last capture into the buffer.
	// NULL unless a copy is in flight.
	struct wl_event_source *copy_source;
	int copy_fd;
	// Owned by the renderer, which draws each frame with them.
	struct {
		VkCommandBuffer cmd;
		uint64_t copy_point; // The timeline point of the last copy.
		// Frames for shm buffers are drawn into the texture, which is read
		// back through the host-visible buffer. NULL until needed.
		struct texture *texture;
		VkBuffer readback;
		VkDeviceMemory readback_memory;
		const uint8_t *readback_pixels;
	} render;
};

void image_capture_damage_output(struct output *output, const struct box *damage);
void image_capture_damage_window(struct window *window);
void image_capture_output_disable(struct output *output);
void image_capture_window_unmap(struct window *window);
void image_capture_repaint(struct output *output);

void renderer_capture_init(struct capture_session *session);
int renderer_capture(struct capture_session *session, struct wl_resource *buffer);
void renderer_capture_read(struct capture_session *session,
	struct wl_shm_buffer *shm);
void renderer_capture_fini(struct capture_session *session);

#endif
