#ifndef JIMI_BACKEND_H
#define JIMI_BACKEND_H

#include <sys/types.h>

#include "util.h"

#define MAX_INPUT_DEVICES 128

struct dmabuf_attributes;
struct frame;
struct gbm_bo;
struct libinput;
struct libseat;
struct output;
struct output_state;
struct plane;
struct udev;
struct udev_monitor;

struct session {
	struct libseat *handle;
	struct wl_event_source *source;
	struct udev *udev; // Finds the devices of the seat.
	int drm_device_id;
	struct {
		int fd;
		int id;
	} input_devices[MAX_INPUT_DEVICES];
	bool active;
};

struct kms {
	int fd;
	dev_t devnum;
	struct wl_event_source *source;
	// Tells when connectors change.
	struct udev_monitor *monitor;
	struct wl_event_source *monitor_source;
	// The size of the framebuffers of the cursor planes
	uint32_t cursor_width;
	uint32_t cursor_height;
	// Commits can show frames at once rather than at the next vblank,
	// tearing.
	bool async_page_flip;
};

struct input {
	struct libinput *libinput;
	struct wl_event_source *source;
};

void session_init(void);
void session_fini(void);
int session_open_input_device(const char *path);
void session_close_input_device(int fd);
void session_switch_vt(int vt);

void kms_init(void);
void kms_disable(struct output *output);
void kms_restore(void);
void kms_resume(void);
void kms_fini(void);
bool plane_supports(const struct plane *plane, uint32_t format, uint64_t modifier);
uint32_t kms_add_fb(struct gbm_bo *bo);
uint32_t kms_import_dmabuf(const struct dmabuf_attributes *attributes,
	struct gbm_bo **bo);
bool kms_test(const struct output_state *states, int nr_states);
bool kms_modeset(struct output **outputs, int nr_outputs);
bool kms_page_flip(struct output *output, const struct frame *frame);

void input_init(void);
void input_suspend(void);
void input_resume(void);
void input_fini(void);

#endif
