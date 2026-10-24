#include <assert.h>
#include <stdlib.h>
#include <unistd.h>

#include "core.h"
#include "surface.h"

// Committed states applied together once the buffers attached by them are
// ready to be sampled, like mutter and KWin do. Meanwhile, the surfaces keep
// showing their previous buffers, so a slow client delays neither the page
// flips nor the other clients. A commit applies the state of the surface
// followed by the cached states of its synchronized subsurfaces.
struct transaction {
	struct wl_list link; // core.transactions, in commit order
	struct wl_list entries; // transaction_entry.link, parents first
};

// An fd that becomes readable once the attached buffer can be sampled, or -1
// if it already can. Clients of linux-drm-syncobj-v1 set an acquire point,
// and the others leave implicit fences on their dma-bufs.
static int
state_fence(struct surface_state *state)
{
	if (!state->buffer || state->buffer->type != CLIENT_BUFFER_DMABUF) {
		return -1;
	}
	if (state->acquire.timeline) {
		return syncobj_point_fence(&state->acquire);
	}
	return client_buffer_fence(state->buffer);
}

static int
handle_fence(int fd, uint32_t mask, void *data)
{
	struct transaction_entry *entry = data;
	wl_event_source_remove(entry->fence);
	entry->fence = NULL;
	transactions_apply_ready();
	return 0;
}

// The cached states of the synchronized subsurfaces are applied after the
// state of their parent.
static void
transaction_add(struct transaction *transaction, struct surface *surface,
	struct surface_state *state)
{
	struct transaction_entry *entry = calloc(1, sizeof(*entry));
	assert(entry);
	entry->surface = surface;
	entry->state = state;
	list_append(&transaction->entries, &entry->link);
	list_append(&surface->transaction_entries, &entry->surface_link);
	int fence = state_fence(state);
	if (fence >= 0) {
		// The event loop keeps its own duplicate of the fd.
		entry->fence = wl_event_loop_add_fd(core.wl_loop, fence,
			WL_EVENT_READABLE, handle_fence, entry);
		assert(entry->fence);
		close(fence);
	}

	struct subsurface *subsurface;
	wl_list_for_each(subsurface, &surface->subsurfaces, parent_link) {
		struct surface *child = subsurface->surface;
		struct surface_state *cached = child->cached;
		if (cached) {
			child->cached = NULL;
			transaction_add(transaction, child, cached);
		}
	}
}

// Applies the state, or queues it until it is ready.
void
transaction_commit(struct surface *surface, struct surface_state *state)
{
	struct transaction *transaction = calloc(1, sizeof(*transaction));
	assert(transaction);
	wl_list_init(&transaction->entries);
	transaction_add(transaction, surface, state);
	list_append(&core.transactions, &transaction->link);
	transactions_apply_ready();
}

// Each surface applies its states in commit order.
static bool
transaction_ready(struct transaction *transaction)
{
	struct transaction_entry *entry;
	wl_list_for_each(entry, &transaction->entries, link) {
		if (entry->fence) {
			return false;
		}
		if (entry->surface && entry->surface->transaction_entries.next
				!= &entry->surface_link) {
			return false;
		}
	}
	return true;
}

static void
transaction_apply(struct transaction *transaction)
{
	struct transaction_entry *entry, *tmp;
	wl_list_for_each_safe(entry, tmp, &transaction->entries, link) {
		if (entry->surface) {
			wl_list_remove(&entry->surface_link);
			surface_apply_state(entry->surface, entry->state);
			surface_state_destroy(entry->state);
		}
		wl_list_remove(&entry->link);
		free(entry);
	}
	wl_list_remove(&transaction->link);
	free(transaction);
}

// A transaction can only wait for earlier ones, so a single pass in commit
// order applies all the ready ones.
void
transactions_apply_ready(void)
{
	struct transaction *transaction, *tmp;
	wl_list_for_each_safe(transaction, tmp, &core.transactions, link) {
		if (transaction_ready(transaction)) {
			transaction_apply(transaction);
		}
	}
}

// Drops the states of the destroyed surface from their transactions, which
// then no longer wait for its buffers.
void
transactions_forget_surface(struct surface *surface)
{
	struct transaction_entry *entry, *tmp;
	wl_list_for_each_safe(entry, tmp, &surface->transaction_entries, surface_link) {
		wl_list_remove(&entry->surface_link);
		entry->surface = NULL;
		surface_state_destroy(entry->state);
		entry->state = NULL;
		if (entry->fence) {
			wl_event_source_remove(entry->fence);
			entry->fence = NULL;
		}
	}
	transactions_apply_ready();
}
