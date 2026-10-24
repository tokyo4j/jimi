#include <assert.h>
#include <time.h>

#include "core.h"
#include "output.h"
#include "surface.h"
#include "presentation-time-protocol.h"

// Tells clients when their content updates are shown. The frames of the
// outputs take the feedbacks of the surfaces they show, which are presented
// at their page flips. Content updates replaced before any frame shows them
// are discarded. The client cursor and the drag icon are drawn outside the
// render lists, so their feedbacks wait until their next commits discard them.

// The frame on the screen of the output since the time shows the content
// updates of the pending feedbacks. Only a surface covering the output is
// scanned out, so the frame shows no other.
void
presentation_feedbacks_send_presented(struct output *output, int64_t time,
	uint32_t seq)
{
	// The page flip events come from the hardware, at the vblanks unless the
	// frame tears.
	uint32_t flags = WP_PRESENTATION_FEEDBACK_KIND_HW_CLOCK
		| WP_PRESENTATION_FEEDBACK_KIND_HW_COMPLETION;
	if (!output->current.tearing) {
		flags |= WP_PRESENTATION_FEEDBACK_KIND_VSYNC;
	}
	if (output->current.buffer) {
		flags |= WP_PRESENTATION_FEEDBACK_KIND_ZERO_COPY;
	}
	uint64_t sec = (uint64_t)(time / 1000000000);
	uint32_t nsec = (uint32_t)(time % 1000000000);
	// The highest refresh rate with adaptive sync, as the protocol allows
	uint32_t refresh = (uint32_t)mode_refresh_interval(output->mode);
	struct wl_resource *resource, *tmp;
	wl_resource_for_each_safe(resource, tmp, &output->pending_feedbacks) {
		struct wl_client *client = wl_resource_get_client(resource);
		struct wl_resource *output_resource;
		// None while the output is disabled, as its resources are inert by
		// the time its last page flip comes.
		wl_resource_for_each(output_resource, &output->resources) {
			if (wl_resource_get_client(output_resource) == client) {
				wp_presentation_feedback_send_sync_output(resource, output_resource);
			}
		}
		// The vblank counter of KMS has 32 bits.
		wp_presentation_feedback_send_presented(resource,
			(uint32_t)(sec >> 32), (uint32_t)sec, nsec, refresh, 0, seq, flags);
		wl_resource_destroy(resource);
	}
}

// The content updates are never shown.
void
presentation_feedbacks_discard(struct wl_list *feedbacks)
{
	struct wl_resource *resource, *tmp;
	wl_resource_for_each_safe(resource, tmp, feedbacks) {
		wp_presentation_feedback_send_discarded(resource);
		wl_resource_destroy(resource);
	}
}

// The feedback is for the next commit of the surface.
static void
presentation_feedback_req(struct wl_client *client,
	struct wl_resource *resource, struct wl_resource *surface_resource, uint32_t id)
{
	struct surface *surface = wl_resource_get_user_data(surface_resource);
	struct wl_resource *feedback = wl_resource_create(client,
		&wp_presentation_feedback_interface, wl_resource_get_version(resource), id);
	assert(feedback);
	wl_resource_set_implementation(feedback, NULL, NULL, resource_unlink);
	list_append(&surface->pending.presentation_feedbacks,
		wl_resource_get_link(feedback));
}

static const struct wp_presentation_interface presentation_impl = {
	.destroy = resource_destroy_req,
	.feedback = presentation_feedback_req,
};

void
wayland_bind_presentation(struct wl_client *client, void *data,
	uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&wp_presentation_interface, MIN(version, 2), id);
	assert(resource);
	wl_resource_set_implementation(resource, &presentation_impl, NULL, NULL);
	// The page flips are timed with it.
	wp_presentation_send_clock_id(resource, CLOCK_MONOTONIC);
}
