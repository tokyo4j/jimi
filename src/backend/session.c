#include <assert.h>
#include <errno.h>
#include <libseat.h>
#include <libudev.h>

#include "core.h"

// The first activation comes before the backends are initialized.
static void
session_enable(struct libseat *seat, void *data)
{
	core.session.active = true;
	if (core.input.libinput) {
		kms_resume();
		input_resume();
	}
}

// Another session takes the devices, e.g. by switching VTs.
static void
session_disable(struct libseat *seat, void *data)
{
	core.session.active = false;
	input_suspend();
	libseat_disable_seat(seat);
}

static const struct libseat_seat_listener session_listener = {
	.enable_seat = session_enable,
	.disable_seat = session_disable,
};

static int
handle_session_events(int fd, uint32_t mask, void *data)
{
	assert(libseat_dispatch(core.session.handle, 0) >= 0);
	return 0;
}

void
session_init(void)
{
	core.session.drm_device_id = -1;
	for (uint32_t i = 0; i < ARRAY_LEN(core.session.input_devices); i++) {
		core.session.input_devices[i].fd = -1;
		core.session.input_devices[i].id = -1;
	}

	core.session.handle = libseat_open_seat(&session_listener, NULL);
	assert(core.session.handle);
	while (!core.session.active) {
		assert(libseat_dispatch(core.session.handle, -1) >= 0);
	}
	core.session.source = wl_event_loop_add_fd(core.wl_loop,
		libseat_get_fd(core.session.handle), WL_EVENT_READABLE,
		handle_session_events, NULL);
	assert(core.session.source);
	core.session.udev = udev_new();
	assert(core.session.udev);
}

int
session_open_input_device(const char *path)
{
	int fd = -1;
	int id = libseat_open_device(core.session.handle, path, &fd);
	if (id < 0) {
		return -errno;
	}
	assert(fd >= 0);
	for (uint32_t i = 0; i < ARRAY_LEN(core.session.input_devices); i++) {
		if (core.session.input_devices[i].fd < 0) {
			core.session.input_devices[i].fd = fd;
			core.session.input_devices[i].id = id;
			return fd;
		}
	}
	assert(!"too many libinput fds");
	return -EMFILE;
}

void
session_close_input_device(int fd)
{
	for (uint32_t i = 0; i < ARRAY_LEN(core.session.input_devices); i++) {
		if (core.session.input_devices[i].fd == fd) {
			libseat_close_device(core.session.handle,
				core.session.input_devices[i].id);
			core.session.input_devices[i].fd = -1;
			core.session.input_devices[i].id = -1;
			return;
		}
	}
	assert(!"unknown libinput fd");
}

// Fails on seats without VTs.
void
session_switch_vt(int vt)
{
	libseat_switch_session(core.session.handle, vt);
}

void
session_fini(void)
{
	assert(core.session.drm_device_id < 0);
	for (uint32_t i = 0; i < ARRAY_LEN(core.session.input_devices); i++) {
		assert(core.session.input_devices[i].fd < 0);
		assert(core.session.input_devices[i].id < 0);
	}
	udev_unref(core.session.udev);
	wl_event_source_remove(core.session.source);
	libseat_close_seat(core.session.handle);
}
