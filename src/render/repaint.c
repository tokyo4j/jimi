#include <assert.h>
#include <linux/sync_file.h>
#include <sys/ioctl.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include <gbm.h>

#include "internal.h"

// A frame is drawn as late as it can be to be ready for the next vblank,
// rather than as soon as the previous one is on the screen, so that it shows
// the latest commits. How long before the vblank that is comes from the
// render times of the last frames.
//
// A frame that takes longer than a refresh cycle is drawn ahead while the
// previous one waits for its vblank, like when the GPU has to preempt
// GPU-heavy clients first. That costs a refresh cycle of latency. KWin calls
// it triple buffering.
//
// With adaptive sync, the vblanks last until the next commit, up to the
// lowest refresh rate of the monitor. A frame is then drawn as soon as
// something changes, but to be ready no earlier than a refresh cycle of the
// mode after the last page flip, like in KWin. It is never drawn ahead, as
// the refresh rate follows the frames instead.
//
// A surface scanned out that asks for tearing is shown at once by async page
// flips, which KMS applies outside the vblanks. Its frames are then drawn as
// soon as something changes, once the previous one is on the screen.

// For KMS to apply a commit before the vblank, after the GPU finishes.
#define REPAINT_MARGIN_NS 1000000
// After this many refresh cycles without a frame, the vblanks may have
// drifted from the last page flip, and the GPU may have clocked down.
#define IDLE_REFRESH_CYCLES 100
// Clients reallocate their buffers for new dma-buf feedback, so a surface
// covering an output is suggested to allocate for scanout only after this
// many frames, and the suggestion is withdrawn only after as many frames
// without one, like in wlroots.
#define SCANOUT_FEEDBACK_FRAMES 30

static int64_t
nsec_now(void)
{
	struct timespec ts;
	assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
	return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

// How long before the vblank to wake up for a frame. The page flip
// timestamps are at the end of the vblanks, and a frame has to be ready
// before their start. Frames that only change the cursor plane or the gamma
// LUT draw at most the cursor, so they are committed as late as they can be.
static int64_t
frame_budget(struct output *output)
{
	const drmModeModeInfo *mode = output->mode;
	int64_t vblank = (int64_t)mode->htotal
		* (mode->vtotal - mode->vdisplay) * 1000000 / mode->clock;
	int64_t render_time = output->needs_repaint ? output->render_time : 0;
	return core.repaint_timer.delay + render_time + vblank + REPAINT_MARGIN_NS;
}

// When to draw the next frame, or -1 if nothing has changed and no surface
// waits for a frame callback, the frame has to wait for a page flip, or the
// output waits for its modeset. The vblanks are extrapolated from the last
// page flip, unless the frame on the screen tears or adaptive sync delays
// them for the frame.
static int64_t
frame_due_time(struct output *output, int64_t now)
{
	bool changed = output->needs_repaint || output->needs_frame
		|| output->needs_commit;
	if (!changed || !frame_empty(&output->queued) || !output->modeset_done) {
		return -1;
	}
	int64_t interval = mode_refresh_interval(output->mode);
	// Like KWin, the frame is drawn right away when the vblanks are
	// unknown, or do not matter as the frame on the screen tears.
	if (output->current.tearing
			|| now - output->last_flip > IDLE_REFRESH_CYCLES * interval) {
		return frame_empty(&output->pending) ? now : -1;
	}
	int64_t budget = frame_budget(output);
	if (output->adaptive_sync) {
		return frame_empty(&output->pending)
			? MAX(now, output->last_flip + interval - budget) : -1;
	}
	// The cursor plane and the gamma LUT are only committed after the page
	// flip.
	bool ahead = output->needs_repaint && budget > interval;
	if (!frame_empty(&output->pending) && !ahead) {
		return -1;
	}
	int64_t next_vblank = output->last_flip
		+ ((now - output->last_flip) / interval + 1) * interval;
	int64_t target = ahead ? next_vblank + interval : next_vblank;
	// A frame drawn late may still be in time, as the budget is
	// pessimistic.
	return MAX(now, target - budget);
}

// Lets go of the frame, whose image the GPU may still be drawing.
void
frame_release(struct frame *frame)
{
	if (frame->image && frame->image->render_fd >= 0) {
		close(frame->image->render_fd);
		frame->image->render_fd = -1;
	}
	if (frame->buffer) {
		client_buffer_unlock(frame->buffer);
	}
	*frame = (struct frame){0};
}

// Another frame showing the same, which locks its client buffer again.
static struct frame
frame_copy(const struct frame *frame)
{
	if (frame->buffer) {
		client_buffer_lock(frame->buffer);
	}
	return *frame;
}

// Suggests the surface that may be scanned out to allocate for the primary
// plane once the output has had one for SCANOUT_FEEDBACK_FRAMES frames, and
// withdraws the suggestions once it has had none for as many.
static void
update_scanout_feedback(struct output *output, struct surface *surface)
{
	if (surface) {
		output->scanout_frames = MIN(output->scanout_frames + 1,
			SCANOUT_FEEDBACK_FRAMES);
		if (output->scanout_frames == SCANOUT_FEEDBACK_FRAMES) {
			dmabuf_feedback_set_scanout_output(surface, output);
		}
	} else if (output->scanout_frames > 0 && --output->scanout_frames == 0) {
		dmabuf_feedback_unset_scanout_output(output);
	}
}

// Scans out the dma-buf of the surface covering the output, which is then
// shown as if drawn, or else draws the frame into an image of the swapchain
// that is not on the screen or pending. Modesets always draw, as KMS may
// reject the dma-buf. The presentation feedbacks of the surfaces that the
// frame shows are taken into the feedbacks.
static struct frame
prepare_frame(struct output *output, struct wl_list *feedbacks)
{
	int64_t start = nsec_now();
	// The captures show what the output is about to show.
	image_capture_repaint(output);
	output->needs_repaint = false;
	output->needs_frame = false;
	struct render_list list;
	render_list_init_output(&list, output);
	send_frame_callbacks(&list, (uint32_t)(start / 1000000));
	take_presentation_feedbacks(&list, feedbacks);
	struct surface *surface = output->modeset_done
		? scanout_surface(output, &list) : NULL;
	update_scanout_feedback(output, surface);
	struct client_buffer *buffer = surface ? surface->current.buffer : NULL;
	// KMS adds framebuffers that any of its planes can scan out, so the
	// format and the modifier are checked against the primary plane.
	if (buffer && buffer->fb && !buffer->scanout_rejected
			&& plane_supports(&output->primary, gbm_bo_get_format(buffer->bo),
				gbm_bo_get_modifier(buffer->bo))) {
		client_buffer_lock(buffer);
		render_list_fini(&list);
		return (struct frame){
			.buffer = buffer,
			.tearing = core.kms.async_page_flip && surface->current.tearing,
			.locked = core.session_lock.locked,
		};
	}
	if (!output->modeset_done) {
		swapchain_update(output);
	}
	struct swapchain_image *image = output->swapchain;
	while (image == output->current.image || image == output->pending.image) {
		image++;
	}
	image->render_start = start;
	draw_swapchain_image(output, image, &list);
	render_list_fini(&list);
	return (struct frame){
		.image = image,
		.locked = core.session_lock.locked,
	};
}

// Commits the frame with the cursor plane and the gamma LUT. A frame that KMS
// rejects is dropped and drawn again for the next vblank, never scanning out
// its dma-buf again unless KMS shows it already, so it is not to blame, like
// when only the cursor plane changes. The frame drawn again presents the
// feedbacks that the dropped one took.
//
// A tearing frame waits for the next vblank instead if KMS cannot show it at
// once, like when the cursor plane changes. So does the same dma-buf again,
// which only paces the frame callbacks.
static void
commit_frame(struct output *output, struct frame frame)
{
	update_cursor_plane(output, &frame);
	output->needs_commit = false;
	frame.tearing = frame.tearing && frame.buffer != output->current.buffer;
	bool ok = kms_page_flip(output, &frame);
	if (!ok && frame.tearing) {
		frame.tearing = false;
		ok = kms_page_flip(output, &frame);
	}
	if (ok) {
		output->pending = frame;
		return;
	}
	if (frame.buffer && frame.buffer != output->current.buffer) {
		frame.buffer->scanout_rejected = true;
	}
	frame_release(&frame);
	damage_output(output);
}

// Sets the modes of the outputs that need them in one commit, since KMS may
// reject the states in between, like when two outputs swap modes over the
// bandwidth, or else one by one, disabling the outputs whose modes it rejects.
// It waits for their page flips, which may still show old images.
static void
modeset_outputs(void)
{
	struct output *outputs[MAX_OUTPUTS];
	int nr_outputs = 0;
	struct output *output;
	wl_list_for_each(output, &core.outputs, link) {
		if (!output->enabled || output->modeset_done) {
			continue;
		}
		if (!frame_empty(&output->pending)) {
			return;
		}
		outputs[nr_outputs++] = output;
	}
	if (nr_outputs == 0) {
		return;
	}
	for (int i = 0; i < nr_outputs; i++) {
		outputs[i]->pending = prepare_frame(outputs[i],
			&outputs[i]->pending_feedbacks);
		update_cursor_plane(outputs[i], &outputs[i]->pending);
		outputs[i]->needs_commit = false;
	}
	bool ok = kms_modeset(outputs, nr_outputs);
	for (int i = 0; i < nr_outputs; i++) {
		if (ok || kms_modeset(&outputs[i], 1)) {
			outputs[i]->modeset_done = true;
			continue;
		}
		frame_release(&outputs[i]->pending);
		output_configure(&(struct output_state){.output = outputs[i]});
		outputs_update();
	}
}

// Prepares the frame, and commits it unless it waits for a page flip. If
// nothing else has changed, the current frame is committed again with the
// cursor plane and the gamma LUT, and paces the surfaces waiting for frame
// callbacks like a drawn frame.
static void
repaint_output(struct output *output)
{
	if (!output->needs_repaint) {
		if (output->needs_frame) {
			output->needs_frame = false;
			struct render_list list;
			render_list_init_output(&list, output);
			send_frame_callbacks(&list, (uint32_t)(nsec_now() / 1000000));
			take_presentation_feedbacks(&list, &output->pending_feedbacks);
			render_list_fini(&list);
		}
		commit_frame(output, frame_copy(&output->current));
		return;
	}
	if (!frame_empty(&output->pending)) {
		output->queued = prepare_frame(output, &output->queued_feedbacks);
	} else {
		commit_frame(output, prepare_frame(output, &output->pending_feedbacks));
	}
}

// The prediction follows a later wakeup at once, and an earlier one slowly,
// like the render times. The loop may also wake up for other events before
// the timer expires.
static void
update_repaint_delay(void)
{
	int64_t now = nsec_now();
	int64_t wakeup = core.repaint_timer.wakeup;
	if (!wakeup || now < wakeup) {
		return;
	}
	int64_t delay = now - wakeup;
	core.repaint_timer.delay = MAX(delay,
		core.repaint_timer.delay - (core.repaint_timer.delay - delay) / 16);
}

// Sets the modes, draws the frames that are due, and arms the timer for the
// next one. The texture uploads wait for the submission of the next frame,
// unless no output is about to draw one.
void
renderer_repaint_outputs(void)
{
	update_repaint_delay();
	textures_collect_garbage();
	if (core.session.active) {
		modeset_outputs();
	}
	int64_t wakeup = 0;
	bool repainting = false;
	struct output *output;
	wl_list_for_each(output, &core.outputs, link) {
		if (!output->enabled || !core.session.active) {
			continue;
		}
		int64_t now = nsec_now();
		if (frame_due_time(output, now) == now) {
			repaint_output(output);
		}
		// Again, as a frame that KMS rejects is drawn again.
		int64_t due = frame_due_time(output, nsec_now());
		if (due >= 0) {
			wakeup = wakeup ? MIN(wakeup, due) : due;
		} else if (!output->needs_repaint) {
			// Damage makes the captures wait for the frame.
			image_capture_repaint(output);
		}
		repainting = repainting || output->needs_repaint;
	}
	// Disarmed if no frame is scheduled. A wakeup that has passed is never
	// due again, so the same one is still armed, like across the input
	// events of a refresh cycle.
	if (wakeup != core.repaint_timer.wakeup) {
		struct itimerspec spec = {
			.it_value = {
				.tv_sec = wakeup / 1000000000,
				.tv_nsec = wakeup % 1000000000,
			},
		};
		assert(timerfd_settime(core.repaint_timer.fd, TFD_TIMER_ABSTIME,
			&spec, NULL) == 0);
		core.repaint_timer.wakeup = wakeup;
	}
	if (!repainting) {
		texture_uploads_submit();
	}
}

// When the GPU finished, from the timestamp of the signaled sync file. A sync
// file can merge several fences.
static int64_t
sync_file_signal_time(int fd)
{
	struct sync_fence_info fences[4];
	struct sync_file_info info = {
		.num_fences = ARRAY_LEN(fences),
		.sync_fence_info = (uintptr_t)fences,
	};
	assert(ioctl(fd, SYNC_IOC_FILE_INFO, &info) == 0 && info.status == 1);
	int64_t time = 0;
	for (uint32_t i = 0; i < info.num_fences; i++) {
		time = MAX(time, (int64_t)fences[i].timestamp_ns);
	}
	return time;
}

// The prediction follows a slower frame at once, and a faster one slowly, so
// that a few fast frames do not make the next slow one miss its vblank.
static void
update_render_time(struct output *output, struct swapchain_image *image)
{
	int64_t render_time = sync_file_signal_time(image->render_fd)
		- image->render_start;
	output->render_time = MAX(render_time,
		output->render_time - (output->render_time - render_time) / 16);
}

// The page flip waited for the GPU to finish the image of the frame, which is
// on the screen at the time, and the previous frame is not. Commits the frame
// prepared ahead, or drops it if the output is disabled, inactive or needs a
// modeset, all of which damage the whole output before it shows frames again.
// The presentation feedbacks of a dropped frame wait for the frame drawn
// again.
void
renderer_handle_page_flip(struct output *output, int64_t time, uint32_t seq)
{
	frame_release(&output->current);
	output->current = output->pending;
	output->pending = (struct frame){0};
	output->last_flip = time;
	presentation_feedbacks_send_presented(output, time, seq);
	list_move(&output->pending_feedbacks, &output->queued_feedbacks);
	struct swapchain_image *image = output->current.image;
	// Commits that only change the cursor plane show the image again.
	if (image && image->render_fd >= 0) {
		update_render_time(output, image);
		close(image->render_fd);
		image->render_fd = -1;
	}
	if (frame_empty(&output->queued)) {
		return;
	}
	if (output->enabled && core.session.active && output->modeset_done) {
		commit_frame(output, output->queued);
	} else {
		frame_release(&output->queued);
	}
	output->queued = (struct frame){0};
}

static int
handle_repaint_timer(int fd, uint32_t mask, void *data)
{
	uint64_t expirations;
	assert(read(fd, &expirations, sizeof(expirations)) == sizeof(expirations));
	return 0;
}

void
repaint_init(void)
{
	core.repaint_timer.fd = timerfd_create(CLOCK_MONOTONIC,
		TFD_CLOEXEC | TFD_NONBLOCK);
	assert(core.repaint_timer.fd >= 0);
	core.repaint_timer.source = wl_event_loop_add_fd(core.wl_loop,
		core.repaint_timer.fd, WL_EVENT_READABLE, handle_repaint_timer, NULL);
	assert(core.repaint_timer.source);
}

void
repaint_fini(void)
{
	wl_event_source_remove(core.repaint_timer.source);
	close(core.repaint_timer.fd);
}
