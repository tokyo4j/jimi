#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>

#include "core.h"
#include "xdg-activation-v1-protocol.h"

// Lets clients pass the keyboard focus to a window, possibly of another
// client, e.g. a launcher to the app it starts, or a chat client to the
// browser that opens a link. A token is granted for the latest input event to
// the requesting client, and one window can use it within
// ACTIVATION_TOKEN_TIMEOUT_SEC. A newer token replaces it.

#define ACTIVATION_TOKEN_TIMEOUT_SEC 30

// The state of an xdg_activation_token_v1, which becomes inert once
// committed.
struct activation_token_request {
	// Its serial is of the latest input event to the client.
	bool granted;
};

static time_t
now_sec(void)
{
	struct timespec ts;
	assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
	return ts.tv_sec;
}

static void
generate_token(char token[ACTIVATION_TOKEN_SIZE])
{
	uint8_t bytes[(ACTIVATION_TOKEN_SIZE - 1) / 2];
	assert(getrandom(bytes, sizeof(bytes), 0) == sizeof(bytes));
	for (size_t i = 0; i < sizeof(bytes); i++) {
		sprintf(&token[i * 2], "%02x", bytes[i]);
	}
}

static struct activation_token_request *
request_from_resource(struct wl_resource *resource)
{
	struct activation_token_request *request = wl_resource_get_user_data(resource);
	if (!request) {
		wl_resource_post_error(resource, XDG_ACTIVATION_TOKEN_V1_ERROR_ALREADY_USED,
			"the activation token has already been committed");
	}
	return request;
}

static void
activation_token_set_serial_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t serial, struct wl_resource *seat)
{
	struct activation_token_request *request = request_from_resource(resource);
	if (request) {
		request->granted = serial == core.seat.input_serial
			&& client == core.seat.input_client;
	}
}

// The app ID and the surface do not matter, as the token is only granted for
// the latest input event.
static void
activation_token_set_app_id_req(struct wl_client *client,
	struct wl_resource *resource, const char *app_id)
{
	request_from_resource(resource);
}

static void
activation_token_set_surface_req(struct wl_client *client,
	struct wl_resource *resource, struct wl_resource *surface)
{
	request_from_resource(resource);
}

static void
activation_token_commit_req(struct wl_client *client, struct wl_resource *resource)
{
	struct activation_token_request *request = request_from_resource(resource);
	if (!request) {
		return;
	}
	// Requests that are not granted get a token too, which is never
	// accepted.
	char token[ACTIVATION_TOKEN_SIZE];
	generate_token(token);
	if (request->granted) {
		memcpy(core.activation_token.name, token, sizeof(token));
		core.activation_token.time = now_sec();
	}
	xdg_activation_token_v1_send_done(resource, token);
	free(request);
	wl_resource_set_user_data(resource, NULL);
}

static const struct xdg_activation_token_v1_interface activation_token_impl = {
	.set_serial = activation_token_set_serial_req,
	.set_app_id = activation_token_set_app_id_req,
	.set_surface = activation_token_set_surface_req,
	.commit = activation_token_commit_req,
	.destroy = resource_destroy_req,
};

static void
activation_token_resource_destroy(struct wl_resource *resource)
{
	free(wl_resource_get_user_data(resource));
}

static void
activation_get_activation_token_req(struct wl_client *client,
	struct wl_resource *resource, uint32_t id)
{
	struct wl_resource *token_resource = wl_resource_create(client,
		&xdg_activation_token_v1_interface, wl_resource_get_version(resource), id);
	assert(token_resource);
	struct activation_token_request *request = calloc(1, sizeof(*request));
	assert(request);
	wl_resource_set_implementation(token_resource, &activation_token_impl,
		request, activation_token_resource_destroy);
}

// Raises the window of the surface and gives it the keyboard focus, restoring
// it if minimized. Windows that are not mapped yet are focused anyway once
// mapped.
static void
activation_activate_req(struct wl_client *client,
	struct wl_resource *resource, const char *token,
	struct wl_resource *surface_resource)
{
	if (!core.activation_token.name[0]
			|| strcmp(token, core.activation_token.name) != 0
			|| now_sec() - core.activation_token.time
				>= ACTIVATION_TOKEN_TIMEOUT_SEC) {
		return;
	}
	core.activation_token.name[0] = '\0';
	struct window *window = window_from_surface(
		wl_resource_get_user_data(surface_resource));
	if (window && window->mapped) {
		window_focus(window);
	}
}

static const struct xdg_activation_v1_interface activation_impl = {
	.get_activation_token = activation_get_activation_token_req,
	.activate = activation_activate_req,
	.destroy = resource_destroy_req,
};

void
wayland_bind_xdg_activation(struct wl_client *client, void *data,
	uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&xdg_activation_v1_interface, MIN(version, 1), id);
	assert(resource);
	wl_resource_set_implementation(resource, &activation_impl, NULL, NULL);
}
