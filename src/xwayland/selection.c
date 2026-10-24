#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xcb/xfixes.h>

#include "surface.h"
#include "xwm.h"

// Larger data is transferred incrementally (INCR), in chunks of this size.
#define INCR_CHUNK_SIZE (64 * 1024)
#define XDND_VERSION 5

// Converts the X11 selection of a source into the fd of a Wayland client.
struct incoming_transfer {
	struct wl_list link; // xwm.incoming
	// Requests the conversion, and receives the data in WL_SELECTION.
	xcb_window_t window;
	int fd;
	struct wl_event_source *source; // Waits for the fd to be writable.
	xcb_get_property_reply_t *chunk; // Being written, or NULL
	int offset; // Written of the chunk
	bool incr;
};

// Converts a Wayland source into a property of an X11 requestor. The source
// is read whole first.
struct outgoing_transfer {
	struct wl_list link; // xwm.outgoing
	xcb_selection_request_event_t request;
	int fd; // Of the source, -1 once read
	struct wl_event_source *source; // Waits for the fd to be readable.
	struct wl_array data;
	size_t offset; // Sent to the requestor
	bool incr;
};

static struct xwm_selection *
selection_from_atom(struct xwm *xwm, xcb_atom_t atom)
{
	for (int type = 0; type < NR_SELECTION_TYPES; type++) {
		if (xwm->selections[type].atom == atom) {
			return &xwm->selections[type];
		}
	}
	return NULL;
}

static enum selection_type
selection_type(struct xwm *xwm, struct xwm_selection *selection)
{
	return (enum selection_type)(selection - xwm->selections);
}

static xcb_atom_t
mime_type_to_atom(struct xwm *xwm, const char *mime_type)
{
	if (strcmp(mime_type, "text/plain;charset=utf-8") == 0) {
		return xwm->atoms[ATOM_UTF8_STRING];
	}
	if (strcmp(mime_type, "text/plain") == 0) {
		return xwm->atoms[ATOM_TEXT];
	}
	xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(xwm->xcb,
		xcb_intern_atom(xwm->xcb, 0, (uint16_t)strlen(mime_type), mime_type), NULL);
	xcb_atom_t atom = reply ? reply->atom : XCB_ATOM_NONE;
	free(reply);
	return atom;
}

// Returns NULL for targets other than MIME types, like TARGETS.
static char *
mime_type_from_atom(struct xwm *xwm, xcb_atom_t atom)
{
	if (atom == xwm->atoms[ATOM_UTF8_STRING]) {
		return strdup("text/plain;charset=utf-8");
	}
	if (atom == xwm->atoms[ATOM_TEXT]) {
		return strdup("text/plain");
	}
	xcb_get_atom_name_reply_t *reply = xcb_get_atom_name_reply(xwm->xcb,
		xcb_get_atom_name(xwm->xcb, atom), NULL);
	if (!reply) {
		return NULL;
	}
	const char *name = xcb_get_atom_name_name(reply);
	size_t length = (size_t)xcb_get_atom_name_name_length(reply);
	char *mime_type = memchr(name, '/', length) ? strndup(name, length) : NULL;
	free(reply);
	return mime_type;
}

// The targets that the MIME types of the source are converted into.
static xcb_atom_t *
source_targets(struct xwm *xwm, struct data_source *source, size_t *count)
{
	*count = source->mime_types.size / sizeof(char *);
	xcb_atom_t *targets = calloc(*count + 1, sizeof(*targets));
	assert(targets);
	size_t i = 0;
	char **mime_type;
	wl_array_for_each(mime_type, &source->mime_types) {
		targets[i++] = mime_type_to_atom(xwm, *mime_type);
	}
	return targets;
}

// X11 clients get the selections of Wayland clients from the window manager.
void
xwm_set_selection(enum selection_type type)
{
	struct xwm *xwm = core.xwayland.xwm;
	struct xwm_selection *selection = &xwm->selections[type];
	struct data_source *source = core.seat.selections[type];
	if (source && source->type == DATA_SOURCE_XWAYLAND) {
		return;
	}
	if (source) {
		xcb_set_selection_owner(xwm->xcb, xwm->window, selection->atom,
			XCB_CURRENT_TIME);
	} else if (selection->owner == xwm->window) {
		// The time does not let it take the selection from a later owner.
		xcb_set_selection_owner(xwm->xcb, XCB_WINDOW_NONE, selection->atom,
			selection->timestamp);
	}
	xcb_flush(xwm->xcb);
}

static void handle_xdnd_selection_owner(struct xwm *xwm, xcb_window_t owner);

static void
handle_xfixes_selection_notify(struct xwm *xwm,
	xcb_xfixes_selection_notify_event_t *event)
{
	if (event->selection == xwm->atoms[ATOM_XDND_SELECTION]) {
		handle_xdnd_selection_owner(xwm, event->owner);
		return;
	}
	struct xwm_selection *selection = selection_from_atom(xwm, event->selection);
	if (!selection) {
		return;
	}
	selection->owner = event->owner;
	if (event->owner == xwm->window) {
		selection->timestamp = event->selection_timestamp;
		return;
	}
	if (event->owner == XCB_WINDOW_NONE) {
		// The selection goes with its X11 owner.
		enum selection_type type = selection_type(xwm, selection);
		struct data_source *source = core.seat.selections[type];
		if (source && source->type == DATA_SOURCE_XWAYLAND) {
			seat_set_selection(type, NULL);
		}
		return;
	}
	// The selection of the new owner becomes the seat selection once it
	// tells its targets.
	xcb_convert_selection(xwm->xcb, xwm->window, selection->atom,
		xwm->atoms[ATOM_TARGETS], selection->atom, event->timestamp);
}

// The targets of an X11 selection that are MIME types become a source.
static void
handle_targets(struct xwm *xwm, xcb_selection_notify_event_t *event)
{
	struct xwm_selection *selection = selection_from_atom(xwm, event->selection);
	if (!selection || event->target != xwm->atoms[ATOM_TARGETS]
			|| event->property == XCB_ATOM_NONE) {
		return;
	}
	xcb_get_property_reply_t *reply = xcb_get_property_reply(xwm->xcb,
		xcb_get_property(xwm->xcb, 1, xwm->window, event->property,
			XCB_ATOM_ATOM, 0, UINT32_MAX / 4), NULL);
	// The owner may have changed meanwhile.
	if (reply && reply->type == XCB_ATOM_ATOM
			&& selection->owner != XCB_WINDOW_NONE
			&& selection->owner != xwm->window) {
		struct data_source *source = data_source_create(DATA_SOURCE_XWAYLAND, NULL);
		xcb_atom_t *targets = xcb_get_property_value(reply);
		int count = xcb_get_property_value_length(reply) / (int)sizeof(xcb_atom_t);
		for (int i = 0; i < count; i++) {
			char *mime_type = mime_type_from_atom(xwm, targets[i]);
			if (mime_type) {
				data_source_offer(source, mime_type);
				free(mime_type);
			}
		}
		seat_set_selection(selection_type(xwm, selection), source);
	}
	free(reply);
}

static void
destroy_incoming(struct xwm *xwm, struct incoming_transfer *transfer)
{
	if (transfer->source) {
		wl_event_source_remove(transfer->source);
	}
	close(transfer->fd);
	free(transfer->chunk);
	xcb_destroy_window(xwm->xcb, transfer->window);
	wl_list_remove(&transfer->link);
	free(transfer);
}

static int handle_incoming_writable(int fd, uint32_t mask, void *data);

// Writes the chunk as far as the fd takes it, and waits for it to take the
// rest. Each chunk of incremental transfers is deleted once written, which
// asks the owner for the next.
static void
write_incoming(struct xwm *xwm, struct incoming_transfer *transfer)
{
	const char *data = xcb_get_property_value(transfer->chunk);
	int length = xcb_get_property_value_length(transfer->chunk);
	ssize_t written = write(transfer->fd, data + transfer->offset,
		(size_t)(length - transfer->offset));
	if (written < 0 && errno != EAGAIN) {
		destroy_incoming(xwm, transfer);
		return;
	}
	transfer->offset += written > 0 ? (int)written : 0;
	if (transfer->offset < length) {
		if (!transfer->source) {
			transfer->source = wl_event_loop_add_fd(core.wl_loop,
				transfer->fd, WL_EVENT_WRITABLE, handle_incoming_writable, transfer);
			assert(transfer->source);
		}
		return;
	}
	if (!transfer->incr) {
		destroy_incoming(xwm, transfer);
		return;
	}
	if (transfer->source) {
		wl_event_source_remove(transfer->source);
		transfer->source = NULL;
	}
	free(transfer->chunk);
	transfer->chunk = NULL;
	transfer->offset = 0;
	xcb_delete_property(xwm->xcb, transfer->window, xwm->atoms[ATOM_WL_SELECTION]);
}

static int
handle_incoming_writable(int fd, uint32_t mask, void *data)
{
	struct xwm *xwm = core.xwayland.xwm;
	write_incoming(xwm, data);
	xcb_flush(xwm->xcb);
	return 0;
}

// Reads the data, or the next chunk of incremental transfers, which ends with
// an empty one. The data is deleted right away, which starts incremental
// transfers.
static void
read_incoming(struct xwm *xwm, struct incoming_transfer *transfer)
{
	xcb_get_property_reply_t *reply = xcb_get_property_reply(xwm->xcb,
		xcb_get_property(xwm->xcb, !transfer->incr, transfer->window,
			xwm->atoms[ATOM_WL_SELECTION], XCB_GET_PROPERTY_TYPE_ANY, 0,
			UINT32_MAX / 4), NULL);
	if (!reply || (transfer->incr && xcb_get_property_value_length(reply) == 0)) {
		free(reply);
		destroy_incoming(xwm, transfer);
		return;
	}
	if (reply->type == xwm->atoms[ATOM_INCR]) {
		transfer->incr = true;
		free(reply);
		return;
	}
	transfer->chunk = reply;
	write_incoming(xwm, transfer);
}

static struct incoming_transfer *
incoming_from_window(struct xwm *xwm, xcb_window_t window)
{
	struct incoming_transfer *transfer;
	wl_list_for_each(transfer, &xwm->incoming, link) {
		if (transfer->window == window) {
			return transfer;
		}
	}
	return NULL;
}

// The source is the X11 drag, or the seat selection of an X11 owner, as the
// offers of replaced selections are inert.
void
xwm_data_source_send(struct data_source *source, const char *mime_type, int fd)
{
	struct xwm *xwm = core.xwayland.xwm;
	xcb_atom_t atom = xwm->atoms[ATOM_XDND_SELECTION];
	if (source->type == DATA_SOURCE_XWAYLAND) {
		struct xwm_selection *selection = NULL;
		for (int type = 0; type < NR_SELECTION_TYPES; type++) {
			if (core.seat.selections[type] == source) {
				selection = &xwm->selections[type];
			}
		}
		assert(selection);
		atom = selection->atom;
	}
	assert(fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) == 0);
	struct incoming_transfer *transfer = calloc(1, sizeof(*transfer));
	assert(transfer);
	transfer->fd = fd;
	// A window of each transfer receives its data. As a child of xwm.window,
	// it is not managed.
	transfer->window = xcb_generate_id(xwm->xcb);
	uint32_t event_mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
	xcb_create_window(xwm->xcb, XCB_COPY_FROM_PARENT, transfer->window,
		xwm->window, 0, 0, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_ONLY,
		XCB_COPY_FROM_PARENT, XCB_CW_EVENT_MASK, &event_mask);
	xcb_convert_selection(xwm->xcb, transfer->window, atom,
		mime_type_to_atom(xwm, mime_type), xwm->atoms[ATOM_WL_SELECTION],
		XCB_CURRENT_TIME);
	list_append(&xwm->incoming, &transfer->link);
	xcb_flush(xwm->xcb);
}

static void
handle_selection_notify(struct xwm *xwm, xcb_selection_notify_event_t *event)
{
	if (event->requestor == xwm->window) {
		handle_targets(xwm, event);
		return;
	}
	struct incoming_transfer *transfer = incoming_from_window(xwm, event->requestor);
	if (!transfer) {
		return;
	}
	if (event->property == XCB_ATOM_NONE) {
		destroy_incoming(xwm, transfer);
	} else {
		read_incoming(xwm, transfer);
	}
}

static void
send_selection_notify(struct xwm *xwm, xcb_selection_request_event_t *request,
	bool success)
{
	xcb_selection_notify_event_t event = {
		.response_type = XCB_SELECTION_NOTIFY,
		.time = request->time,
		.requestor = request->requestor,
		.selection = request->selection,
		.target = request->target,
		.property = success ? request->property : XCB_ATOM_NONE,
	};
	xwm_send_event(xwm, request->requestor, XCB_EVENT_MASK_NO_EVENT, &event,
		sizeof(event));
}

static void
destroy_outgoing(struct outgoing_transfer *transfer)
{
	if (transfer->source) {
		wl_event_source_remove(transfer->source);
	}
	if (transfer->fd >= 0) {
		close(transfer->fd);
	}
	wl_array_release(&transfer->data);
	wl_list_remove(&transfer->link);
	free(transfer);
}

// Returns the length of the chunk. An empty one after the data ends
// incremental transfers.
static size_t
send_outgoing_chunk(struct xwm *xwm, struct outgoing_transfer *transfer)
{
	size_t length = MIN(transfer->data.size - transfer->offset,
		(size_t)INCR_CHUNK_SIZE);
	xcb_change_property(xwm->xcb, XCB_PROP_MODE_REPLACE,
		transfer->request.requestor, transfer->request.property,
		transfer->request.target, 8, (uint32_t)length,
		(char *)transfer->data.data + transfer->offset);
	transfer->offset += length;
	return length;
}

// Once the source is read whole, the data goes into the property, or in chunks
// after an INCR property if it is larger. The requestor deletes each property
// to ask for the next chunk.
static int
handle_outgoing_readable(int fd, uint32_t mask, void *data)
{
	struct outgoing_transfer *transfer = data;
	struct xwm *xwm = core.xwayland.xwm;
	size_t size = transfer->data.size;
	assert(wl_array_add(&transfer->data, INCR_CHUNK_SIZE));
	ssize_t length = read(fd, (char *)transfer->data.data + size, INCR_CHUNK_SIZE);
	transfer->data.size = size + (length > 0 ? (size_t)length : 0);
	if (length < 0 && errno == EAGAIN) {
		return 0;
	}
	if (length < 0) {
		send_selection_notify(xwm, &transfer->request, false);
		destroy_outgoing(transfer);
	} else if (length == 0) {
		wl_event_source_remove(transfer->source);
		transfer->source = NULL;
		close(transfer->fd);
		transfer->fd = -1;
		if (transfer->data.size <= INCR_CHUNK_SIZE) {
			send_outgoing_chunk(xwm, transfer);
			send_selection_notify(xwm, &transfer->request, true);
			destroy_outgoing(transfer);
		} else {
			uint32_t incr_size = (uint32_t)transfer->data.size;
			xcb_change_property(xwm->xcb, XCB_PROP_MODE_REPLACE,
				transfer->request.requestor, transfer->request.property,
				xwm->atoms[ATOM_INCR], 32, 1, &incr_size);
			send_selection_notify(xwm, &transfer->request, true);
			transfer->incr = true;
		}
	}
	xcb_flush(xwm->xcb);
	return 0;
}

// The source writes the data into a pipe. Requestors only read the reply to
// their last request, so their earlier requests fail.
static void
start_outgoing(struct xwm *xwm, xcb_selection_request_event_t *request,
	struct data_source *source, const char *mime_type)
{
	struct outgoing_transfer *transfer, *tmp;
	wl_list_for_each_safe(transfer, tmp, &xwm->outgoing, link) {
		if (transfer->request.requestor == request->requestor) {
			send_selection_notify(xwm, &transfer->request, false);
			destroy_outgoing(transfer);
		}
	}
	int fds[2];
	assert(pipe(fds) == 0);
	assert(fcntl(fds[0], F_SETFD, FD_CLOEXEC) == 0);
	assert(fcntl(fds[0], F_SETFL, O_NONBLOCK) == 0);
	assert(fcntl(fds[1], F_SETFD, FD_CLOEXEC) == 0);
	transfer = calloc(1, sizeof(*transfer));
	assert(transfer);
	transfer->request = *request;
	transfer->fd = fds[0];
	wl_array_init(&transfer->data);
	transfer->source = wl_event_loop_add_fd(core.wl_loop, fds[0],
		WL_EVENT_READABLE, handle_outgoing_readable, transfer);
	assert(transfer->source);
	list_append(&xwm->outgoing, &transfer->link);
	data_source_send(source, mime_type, fds[1]);
}

// X11 clients read the selections regardless of the keyboard focus, as X11
// does not isolate them from each other anyway, e.g. xclip run from a Wayland
// terminal. They read the drag while it is over them or dropped on them.
static struct data_source *
requested_source(struct xwm *xwm, xcb_atom_t atom)
{
	if (atom == xwm->atoms[ATOM_XDND_SELECTION]) {
		return xwm->dnd.source;
	}
	struct xwm_selection *selection = selection_from_atom(xwm, atom);
	if (!selection) {
		return NULL;
	}
	struct data_source *source =
		core.seat.selections[selection_type(xwm, selection)];
	return source && source->type != DATA_SOURCE_XWAYLAND ? source : NULL;
}

static void
handle_selection_request(struct xwm *xwm, xcb_selection_request_event_t *request)
{
	struct data_source *source = requested_source(xwm, request->selection);
	if (!source) {
		send_selection_notify(xwm, request, false);
		return;
	}
	if (request->target == xwm->atoms[ATOM_TARGETS]) {
		size_t count;
		xcb_atom_t *targets = source_targets(xwm, source, &count);
		targets[count] = xwm->atoms[ATOM_TARGETS];
		xcb_change_property(xwm->xcb, XCB_PROP_MODE_REPLACE,
			request->requestor, request->property, XCB_ATOM_ATOM, 32,
			(uint32_t)count + 1, targets);
		free(targets);
		send_selection_notify(xwm, request, true);
		return;
	}
	char *mime_type = mime_type_from_atom(xwm, request->target);
	if (mime_type && data_source_has_mime_type(source, mime_type)) {
		start_outgoing(xwm, request, source, mime_type);
	} else {
		send_selection_notify(xwm, request, false);
	}
	free(mime_type);
}

static bool
handle_property_notify(struct xwm *xwm, xcb_property_notify_event_t *event)
{
	struct incoming_transfer *incoming = incoming_from_window(xwm, event->window);
	if (incoming) {
		if (event->state == XCB_PROPERTY_NEW_VALUE && incoming->incr
				&& !incoming->chunk) {
			read_incoming(xwm, incoming);
		}
		return true;
	}
	struct outgoing_transfer *outgoing;
	wl_list_for_each(outgoing, &xwm->outgoing, link) {
		if (outgoing->request.requestor == event->window
				&& outgoing->request.property == event->atom) {
			if (event->state == XCB_PROPERTY_DELETE && outgoing->incr
					&& send_outgoing_chunk(xwm, outgoing) == 0) {
				destroy_outgoing(outgoing);
			}
			return true;
		}
	}
	return false;
}

// Transfers to destroyed requestors are abandoned.
static void
handle_destroy_notify(struct xwm *xwm, xcb_destroy_notify_event_t *event)
{
	struct outgoing_transfer *transfer, *tmp;
	wl_list_for_each_safe(transfer, tmp, &xwm->outgoing, link) {
		if (transfer->request.requestor == event->window) {
			destroy_outgoing(transfer);
		}
	}
}

static xcb_atom_t
dnd_action_to_atom(struct xwm *xwm, uint32_t actions)
{
	if (actions & WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY) {
		return xwm->atoms[ATOM_XDND_ACTION_COPY];
	}
	if (actions & WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE) {
		return xwm->atoms[ATOM_XDND_ACTION_MOVE];
	}
	if (actions & WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK) {
		return xwm->atoms[ATOM_XDND_ACTION_ASK];
	}
	return XCB_ATOM_NONE;
}

static uint32_t
dnd_action_from_atom(struct xwm *xwm, xcb_atom_t atom)
{
	if (atom == xwm->atoms[ATOM_XDND_ACTION_COPY]
			|| atom == xwm->atoms[ATOM_XDND_ACTION_PRIVATE]) {
		return WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
	}
	if (atom == xwm->atoms[ATOM_XDND_ACTION_MOVE]) {
		return WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;
	}
	if (atom == xwm->atoms[ATOM_XDND_ACTION_ASK]) {
		return WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK;
	}
	return WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;
}

// The first item of the data is xwm.window, the source of Wayland drags and
// the target of X11 drags.
static void
send_dnd_message(struct xwm *xwm, xcb_window_t window, enum atom type,
	uint32_t data1, uint32_t data2, uint32_t data3, uint32_t data4)
{
	xcb_client_message_event_t event = {
		.response_type = XCB_CLIENT_MESSAGE,
		.format = 32,
		.window = window,
		.type = xwm->atoms[type],
		.data.data32 = {xwm->window, data1, data2, data3, data4},
	};
	xwm_send_event(xwm, window, XCB_EVENT_MASK_NO_EVENT, &event, sizeof(event));
}

// The drag leaves the target, unless it is dropped on it.
static void
end_dnd(struct xwm *xwm)
{
	if (!xwm->dnd.source) {
		return;
	}
	if (!xwm->dnd.dropped) {
		send_dnd_message(xwm, xwm->dnd.target, ATOM_XDND_LEAVE, 0, 0, 0, 0);
	}
	wl_list_remove(&xwm->dnd.source_destroy.link);
	xwm->dnd.source = NULL;
	xwm->dnd.target = XCB_WINDOW_NONE;
	xwm->dnd.accepted = false;
	xwm->dnd.action = WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;
	xwm->dnd.dropped = false;
}

static void
handle_dnd_source_destroy(struct wl_listener *listener, void *data)
{
	struct xwm *xwm = core.xwayland.xwm;
	end_dnd(xwm);
	xcb_flush(xwm->xcb);
}

// Wayland drags enter X11 windows as XDND drags of xwm.window, which tell up
// to 3 targets themselves and more in XdndTypeList. A drop that is not
// finished yet is forgotten.
void
xwm_drag_enter(struct data_source *source, struct surface *surface)
{
	struct xwm *xwm = core.xwayland.xwm;
	struct xwayland_window *xw = surface->xwayland->window;
	assert(xw);
	end_dnd(xwm);
	xwm->dnd.source = source;
	xwm->dnd.target = xw->id;
	xwm->dnd.source_destroy.notify = handle_dnd_source_destroy;
	wl_resource_add_destroy_listener(source->resource, &xwm->dnd.source_destroy);
	xcb_set_selection_owner(xwm->xcb, xwm->window,
		xwm->atoms[ATOM_XDND_SELECTION], XCB_CURRENT_TIME);

	size_t count;
	xcb_atom_t *targets = source_targets(xwm, source, &count);
	xcb_atom_t types[3] = {0};
	if (count > 3) {
		xcb_change_property(xwm->xcb, XCB_PROP_MODE_REPLACE, xwm->window,
			xwm->atoms[ATOM_XDND_TYPE_LIST], XCB_ATOM_ATOM, 32,
			(uint32_t)count, targets);
	} else {
		memcpy(types, targets, count * sizeof(*targets));
	}
	free(targets);
	send_dnd_message(xwm, xwm->dnd.target, ATOM_XDND_ENTER,
		XDND_VERSION << 24 | (count > 3), types[0], types[1], types[2]);
	xwm_drag_motion();
}

void
xwm_drag_motion(void)
{
	struct xwm *xwm = core.xwayland.xwm;
	struct data_source *source = xwm->dnd.source;
	if (!source) {
		return;
	}
	// The root window has the coordinates of the layout.
	int16_t x = (int16_t)core.seat.pointer.x;
	int16_t y = (int16_t)core.seat.pointer.y;
	uint32_t actions = source->actions_set ? source->actions
		: WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
	send_dnd_message(xwm, xwm->dnd.target, ATOM_XDND_POSITION, 0,
		(uint32_t)(uint16_t)x << 16 | (uint16_t)y, XCB_CURRENT_TIME,
		dnd_action_to_atom(xwm, actions));
	xcb_flush(xwm->xcb);
}

void
xwm_drag_leave(void)
{
	struct xwm *xwm = core.xwayland.xwm;
	end_dnd(xwm);
	xcb_flush(xwm->xcb);
}

// Returns whether the target takes the drop, which it finishes once it reads
// the data. Otherwise, the drag is cancelled, which leaves the target.
bool
xwm_drag_drop(void)
{
	struct xwm *xwm = core.xwayland.xwm;
	if (!xwm->dnd.source || !xwm->dnd.accepted
			|| xwm->dnd.action == WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE) {
		return false;
	}
	xwm->dnd.dropped = true;
	send_dnd_message(xwm, xwm->dnd.target, ATOM_XDND_DROP, 0,
		XCB_CURRENT_TIME, 0, 0);
	xcb_flush(xwm->xcb);
	return true;
}

// The target tells whether it takes the drop and the action, but not the MIME
// type, so the source is told the first one.
static void
handle_dnd_status(struct xwm *xwm, const uint32_t data[5])
{
	struct data_source *source = xwm->dnd.source;
	if (!source || xwm->dnd.dropped || data[0] != xwm->dnd.target) {
		return;
	}
	bool accepted = data[1] & 1;
	uint32_t action = accepted ? dnd_action_from_atom(xwm, data[4])
		: WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;
	if (accepted != xwm->dnd.accepted) {
		char **mime_types = source->mime_types.data;
		wl_data_source_send_target(source->resource,
			accepted && source->mime_types.size ? mime_types[0] : NULL);
	}
	if (action != xwm->dnd.action && wl_resource_get_version(
			source->resource) >= WL_DATA_SOURCE_ACTION_SINCE_VERSION) {
		wl_data_source_send_action(source->resource, action);
	}
	xwm->dnd.accepted = accepted;
	xwm->dnd.action = action;
}

static void
handle_dnd_finished(struct xwm *xwm, const uint32_t data[5])
{
	struct data_source *source = xwm->dnd.source;
	if (!xwm->dnd.dropped || data[0] != xwm->dnd.target) {
		return;
	}
	if (!(data[1] & 1)) {
		wl_data_source_send_cancelled(source->resource);
	} else if (wl_resource_get_version(source->resource)
			>= WL_DATA_SOURCE_DND_FINISHED_SINCE_VERSION) {
		wl_data_source_send_dnd_finished(source->resource);
	}
	end_dnd(xwm);
}

// While mapped, xwm.window is stacked above the X11 windows.
static void
map_wm_window(struct xwm *xwm, bool mapped)
{
	if (xwm->x11_drag.window_mapped == mapped) {
		return;
	}
	xwm->x11_drag.window_mapped = mapped;
	if (mapped) {
		uint32_t stack_mode = XCB_STACK_MODE_ABOVE;
		xcb_configure_window(xwm->xcb, xwm->window,
			XCB_CONFIG_WINDOW_STACK_MODE, &stack_mode);
		xcb_map_window(xwm->xcb, xwm->window);
	} else {
		xcb_unmap_window(xwm->xcb, xwm->window);
	}
}

// X11 drags start by a button held on an X11 window, and the X11 client runs
// them by the implicit grab of the pointer. Outside of the X11 windows, which
// the client finds by itself, xwm.window is mapped over them to take the drag.
// Like the client, it looks past override-redirect windows, such as the drag
// icon. The drag ends with the grab, unless the client then drops it on
// xwm.window or leaves it.
void
xwm_x11_drag_update(void)
{
	struct xwm *xwm = core.xwayland.xwm;
	if (!xwm->x11_drag.drag.source || xwm->x11_drag.dropped) {
		return;
	}
	struct surface *grab = pointer_implicit_grab_surface();
	if (!grab || grab->role != SURFACE_ROLE_XWAYLAND) {
		map_wm_window(xwm, false);
		if (!xwm->x11_drag.entered) {
			drag_cancel(&xwm->x11_drag.drag);
		}
	} else {
		struct input_target target = input_target_at(core.seat.pointer.x,
			core.seat.pointer.y, true);
		map_wm_window(xwm, !target.surface
			|| target.surface->role != SURFACE_ROLE_XWAYLAND);
	}
	xcb_flush(xwm->xcb);
}

// X11 clients own XdndSelection from when they start dragging, and the window
// manager owns it for Wayland drags over X11 windows. The previous drag is
// over then.
static void
handle_xdnd_selection_owner(struct xwm *xwm, xcb_window_t owner)
{
	if (xwm->x11_drag.drag.source) {
		drag_cancel(&xwm->x11_drag.drag);
	}
	struct surface *grab = pointer_implicit_grab_surface();
	if (owner == XCB_WINDOW_NONE || owner == xwm->window || !grab
			|| grab->role != SURFACE_ROLE_XWAYLAND) {
		return;
	}
	xwm->x11_drag.drag.source = data_source_create(DATA_SOURCE_XDND, NULL);
	xwm->x11_drag.owner = owner;
	xwm_x11_drag_update();
}

void
xwm_x11_drag_forget_surface(struct surface *surface)
{
	struct drag *drag = &core.xwayland.xwm->x11_drag.drag;
	if (surface_contains(surface, drag->focus)) {
		drag_motion(drag, NULL, 0.0, 0.0, 0, false);
	}
}

// Answers each position, and tells changes of the Wayland target meanwhile, so
// that the client knows them when the button is released. Bit 1 asks for a
// position at every motion.
static void
send_x11_drag_status(struct xwm *xwm)
{
	if (!xwm->x11_drag.entered) {
		return;
	}
	bool accepted = xwm->x11_drag.accepted && xwm->x11_drag.action
		!= WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;
	send_dnd_message(xwm, xwm->x11_drag.owner, ATOM_XDND_STATUS,
		1 << 1 | accepted, 0, 0,
		accepted ? dnd_action_to_atom(xwm, xwm->x11_drag.action) : XCB_ATOM_NONE);
}

void
xwm_x11_drag_accept(bool accepted)
{
	struct xwm *xwm = core.xwayland.xwm;
	if (xwm->x11_drag.accepted != accepted) {
		xwm->x11_drag.accepted = accepted;
		send_x11_drag_status(xwm);
		xcb_flush(xwm->xcb);
	}
}

void
xwm_x11_drag_action(uint32_t action)
{
	struct xwm *xwm = core.xwayland.xwm;
	if (xwm->x11_drag.action != action) {
		xwm->x11_drag.action = action;
		send_x11_drag_status(xwm);
		xcb_flush(xwm->xcb);
	}
}

// The Wayland side ends the drag. The client is told whether the drop is
// taken, if it has dropped.
void
xwm_x11_drag_finish(bool success)
{
	struct xwm *xwm = core.xwayland.xwm;
	struct data_source *source = xwm->x11_drag.drag.source;
	if (xwm->x11_drag.dropped) {
		send_dnd_message(xwm, xwm->x11_drag.owner, ATOM_XDND_FINISHED,
			success, success ? dnd_action_to_atom(xwm,
				xwm->x11_drag.action) : XCB_ATOM_NONE, 0, 0);
	}
	map_wm_window(xwm, false);
	memset(&xwm->x11_drag, 0, sizeof(xwm->x11_drag));
	data_source_destroy(source);
	xcb_flush(xwm->xcb);
}

// The client tells up to 3 types itself, and more in XdndTypeList. The unused
// ones are None.
static void
handle_x11_drag_enter(struct xwm *xwm, const uint32_t data[5])
{
	xcb_get_property_reply_t *reply = NULL;
	const xcb_atom_t *types = &data[2];
	int count = 3;
	if (data[1] & 1) {
		reply = xcb_get_property_reply(xwm->xcb,
			xcb_get_property(xwm->xcb, 0, data[0],
				xwm->atoms[ATOM_XDND_TYPE_LIST], XCB_ATOM_ATOM, 0,
				UINT32_MAX / 4), NULL);
		types = reply ? xcb_get_property_value(reply) : NULL;
		count = reply ? xcb_get_property_value_length(reply)
			/ (int)sizeof(xcb_atom_t) : 0;
	}
	for (int i = 0; i < count; i++) {
		char *mime_type = types[i] != XCB_ATOM_NONE
			? mime_type_from_atom(xwm, types[i]) : NULL;
		if (mime_type) {
			data_source_offer(xwm->x11_drag.drag.source, mime_type);
			free(mime_type);
		}
	}
	free(reply);
	xwm->x11_drag.entered = true;
}

// The client asks for an action, which the Wayland target can take, or copy
// instead, as XDND allows.
static void
handle_x11_drag_position(struct xwm *xwm, const uint32_t data[5])
{
	struct drag *drag = &xwm->x11_drag.drag;
	data_source_set_actions(drag->source, dnd_action_from_atom(xwm, data[4])
		| WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
	struct input_target target = input_target_at(core.seat.pointer.x,
		core.seat.pointer.y, true);
	struct surface *surface = target.surface
		&& target.surface->role != SURFACE_ROLE_XWAYLAND
		? target.surface : NULL;
	drag_motion(drag, surface, target.sx, target.sy, data[3], true);
	send_x11_drag_status(xwm);
}

static void
handle_x11_drag_leave(struct xwm *xwm)
{
	xwm->x11_drag.entered = false;
	drag_motion(&xwm->x11_drag.drag, NULL, 0.0, 0.0, 0, false);
	xwm_x11_drag_update();
}

// The drop goes to the Wayland target, which finishes it once it has read the
// data.
static void
handle_x11_drag_drop(struct xwm *xwm)
{
	struct drag drag = xwm->x11_drag.drag;
	xwm->x11_drag.drag.focus = NULL;
	xwm->x11_drag.entered = false;
	xwm->x11_drag.dropped = true;
	map_wm_window(xwm, false);
	drag_drop(&drag);
}

// xwm.window is the source of Wayland drags and the target of X11 drags, which
// tell their client window first.
static bool
handle_client_message(struct xwm *xwm, xcb_client_message_event_t *event)
{
	if (event->window != xwm->window) {
		return false;
	}
	const uint32_t *data = event->data.data32;
	xcb_atom_t type = event->type;
	if (type == xwm->atoms[ATOM_XDND_STATUS]) {
		handle_dnd_status(xwm, data);
		return true;
	}
	if (type == xwm->atoms[ATOM_XDND_FINISHED]) {
		handle_dnd_finished(xwm, data);
		return true;
	}
	if (!xwm->x11_drag.drag.source || data[0] != xwm->x11_drag.owner) {
		return true;
	}
	if (type == xwm->atoms[ATOM_XDND_ENTER]) {
		handle_x11_drag_enter(xwm, data);
	} else if (type == xwm->atoms[ATOM_XDND_POSITION]) {
		handle_x11_drag_position(xwm, data);
	} else if (type == xwm->atoms[ATOM_XDND_LEAVE]) {
		handle_x11_drag_leave(xwm);
	} else if (type == xwm->atoms[ATOM_XDND_DROP]) {
		handle_x11_drag_drop(xwm);
	}
	return true;
}

// Returns whether the event is only of the selections.
bool
xwm_handle_selection_event(struct xwm *xwm, xcb_generic_event_t *event)
{
	uint8_t type = event->response_type & ~0x80;
	if (type == xwm->xfixes_first_event + XCB_XFIXES_SELECTION_NOTIFY) {
		handle_xfixes_selection_notify(xwm,
			(xcb_xfixes_selection_notify_event_t *)event);
		return true;
	}
	switch (type) {
	case XCB_SELECTION_NOTIFY:
		handle_selection_notify(xwm, (xcb_selection_notify_event_t *)event);
		return true;
	case XCB_SELECTION_REQUEST:
		handle_selection_request(xwm, (xcb_selection_request_event_t *)event);
		return true;
	case XCB_PROPERTY_NOTIFY:
		return handle_property_notify(xwm, (xcb_property_notify_event_t *)event);
	case XCB_CLIENT_MESSAGE:
		return handle_client_message(xwm, (xcb_client_message_event_t *)event);
	case XCB_DESTROY_NOTIFY:
		handle_destroy_notify(xwm, (xcb_destroy_notify_event_t *)event);
		return false;
	}
	return false;
}

// XFixes reports the owners of the selections and of XdndSelection, and the
// window manager takes the selections of Wayland clients. xwm.window is the
// target of X11 drags.
void
xwm_selection_init(struct xwm *xwm)
{
	wl_list_init(&xwm->incoming);
	wl_list_init(&xwm->outgoing);
	xwm->selections[SELECTION_CLIPBOARD].atom = xwm->atoms[ATOM_CLIPBOARD];
	xwm->selections[SELECTION_PRIMARY].atom = XCB_ATOM_PRIMARY;
	const xcb_query_extension_reply_t *xfixes =
		xcb_get_extension_data(xwm->xcb, &xcb_xfixes_id);
	assert(xfixes && xfixes->present);
	xwm->xfixes_first_event = xfixes->first_event;
	free(xcb_xfixes_query_version_reply(xwm->xcb,
		xcb_xfixes_query_version(xwm->xcb, XCB_XFIXES_MAJOR_VERSION,
			XCB_XFIXES_MINOR_VERSION), NULL));
	uint32_t mask = XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER
		| XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_WINDOW_DESTROY
		| XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_CLIENT_CLOSE;
	for (int type = 0; type < NR_SELECTION_TYPES; type++) {
		xcb_xfixes_select_selection_input(xwm->xcb, xwm->window,
			xwm->selections[type].atom, mask);
		xwm_set_selection(type);
	}
	xcb_xfixes_select_selection_input(xwm->xcb, xwm->window,
		xwm->atoms[ATOM_XDND_SELECTION], mask);
	uint32_t version = XDND_VERSION;
	xcb_change_property(xwm->xcb, XCB_PROP_MODE_REPLACE, xwm->window,
		xwm->atoms[ATOM_XDND_AWARE], XCB_ATOM_ATOM, 32, 1, &version);
}

// The selections of X11 clients go with the window manager.
void
xwm_selection_fini(struct xwm *xwm)
{
	struct incoming_transfer *incoming, *incoming_tmp;
	wl_list_for_each_safe(incoming, incoming_tmp, &xwm->incoming, link) {
		destroy_incoming(xwm, incoming);
	}
	struct outgoing_transfer *outgoing, *outgoing_tmp;
	wl_list_for_each_safe(outgoing, outgoing_tmp, &xwm->outgoing, link) {
		destroy_outgoing(outgoing);
	}
	end_dnd(xwm);
	if (xwm->x11_drag.drag.source) {
		drag_cancel(&xwm->x11_drag.drag);
	}
	for (int type = 0; type < NR_SELECTION_TYPES; type++) {
		struct data_source *source = core.seat.selections[type];
		if (source && source->type == DATA_SOURCE_XWAYLAND) {
			seat_set_selection(type, NULL);
		}
	}
}
