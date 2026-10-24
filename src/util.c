#define _GNU_SOURCE

#include <assert.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "util.h"

// The time of now in the clock of the input events of libinput
uint32_t
monotonic_msec(void)
{
	struct timespec now;
	assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
	return (uint32_t)(now.tv_sec * 1000 + now.tv_nsec / 1000000);
}

static struct rlimit original_nofile_limit;

// The compositor holds a file descriptor for each dma-buf buffer of the
// clients, which can exceed the default soft limit of 1024.
void
raise_nofile_limit(void)
{
	assert(getrlimit(RLIMIT_NOFILE, &original_nofile_limit) == 0);
	struct rlimit limit = original_nofile_limit;
	limit.rlim_cur = limit.rlim_max;
	assert(setrlimit(RLIMIT_NOFILE, &limit) == 0);
}

// Undoes what the spawned process would inherit from the compositor across
// exec: the raised limit, which breaks those using select() with file
// descriptors above 1024, and the blocked and the ignored signals.
void
prepare_exec(void)
{
	assert(setrlimit(RLIMIT_NOFILE, &original_nofile_limit) == 0);
	sigset_t signals;
	sigemptyset(&signals);
	assert(sigprocmask(SIG_SETMASK, &signals, NULL) == 0);
	signal(SIGPIPE, SIG_DFL);
}

// Runs the shell command in a process detached from the compositor.
void
spawn(const char *command)
{
	fprintf(stderr, "Launching: %s\n", command);
	pid_t pid = fork();
	assert(pid >= 0);
	if (pid > 0) {
		assert(waitpid(pid, NULL, 0) == pid);
		return;
	}
	// Fork twice, so that the command is not left as a zombie.
	pid = fork();
	if (pid != 0) {
		_exit(pid < 0);
	}
	prepare_exec();
	execlp("sh", "sh", "-c", command, (char *)NULL);
	perror("execlp sh");
	_exit(127);
}

// Returns a read-only file holding the data, which can be shared with clients.
int
create_sealed_file(const char *name, const void *data, size_t size)
{
	int fd = memfd_create(name, MFD_CLOEXEC | MFD_ALLOW_SEALING);
	assert(fd >= 0);
	assert(ftruncate(fd, (off_t)size) == 0);
	void *map = mmap(NULL, size, PROT_WRITE, MAP_SHARED, fd, 0);
	assert(map != MAP_FAILED);
	memcpy(map, data, size);
	assert(munmap(map, size) == 0);
	assert(fcntl(fd, F_ADD_SEALS,
		F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_WRITE | F_SEAL_SEAL) == 0);
	return fd;
}

// Implements the requests which only destroy their object.
void
resource_destroy_req(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

// Removes the resource from the list linked by wl_resource_get_link().
void
resource_unlink(struct wl_resource *resource)
{
	wl_list_remove(wl_resource_get_link(resource));
}

// Sends wl_callback.done to the callbacks linked by their resources, and
// destroys them.
void
callbacks_send_done(struct wl_list *callbacks, uint32_t data)
{
	struct wl_resource *resource, *tmp;
	wl_resource_for_each_safe(resource, tmp, callbacks) {
		wl_callback_send_done(resource, data);
		wl_resource_destroy(resource);
	}
}

void
list_move(struct wl_list *destination, struct wl_list *source)
{
	assert(destination != source);
	if (!wl_list_empty(source)) {
		wl_list_insert_list(destination->prev, source);
		wl_list_init(source);
	}
}

// Transform matrices use top-left normalized coordinates. Rotations are
// centered by the caller when transforming a finite box.
static const struct matrix transform_matrices[] = {
	[WL_OUTPUT_TRANSFORM_NORMAL] = {
		.m = {
			{1.0f, 0.0f, 0.0f, 0.0f},
			{0.0f, 1.0f, 0.0f, 0.0f},
			{0.0f, 0.0f, 1.0f, 0.0f},
			{0.0f, 0.0f, 0.0f, 1.0f},
		},
	},
	[WL_OUTPUT_TRANSFORM_90] = {
		.m = {
			{0.0f, -1.0f, 0.0f, 0.0f},
			{1.0f, 0.0f, 0.0f, 0.0f},
			{0.0f, 0.0f, 1.0f, 0.0f},
			{0.0f, 0.0f, 0.0f, 1.0f},
		},
	},
	[WL_OUTPUT_TRANSFORM_180] = {
		.m = {
			{-1.0f, 0.0f, 0.0f, 0.0f},
			{0.0f, -1.0f, 0.0f, 0.0f},
			{0.0f, 0.0f, 1.0f, 0.0f},
			{0.0f, 0.0f, 0.0f, 1.0f},
		},
	},
	[WL_OUTPUT_TRANSFORM_270] = {
		.m = {
			{0.0f, 1.0f, 0.0f, 0.0f},
			{-1.0f, 0.0f, 0.0f, 0.0f},
			{0.0f, 0.0f, 1.0f, 0.0f},
			{0.0f, 0.0f, 0.0f, 1.0f},
		},
	},
	[WL_OUTPUT_TRANSFORM_FLIPPED] = {
		.m = {
			{-1.0f, 0.0f, 0.0f, 0.0f},
			{0.0f, 1.0f, 0.0f, 0.0f},
			{0.0f, 0.0f, 1.0f, 0.0f},
			{0.0f, 0.0f, 0.0f, 1.0f},
		},
	},
	[WL_OUTPUT_TRANSFORM_FLIPPED_90] = {
		.m = {
			{0.0f, 1.0f, 0.0f, 0.0f},
			{1.0f, 0.0f, 0.0f, 0.0f},
			{0.0f, 0.0f, 1.0f, 0.0f},
			{0.0f, 0.0f, 0.0f, 1.0f},
		},
	},
	[WL_OUTPUT_TRANSFORM_FLIPPED_180] = {
		.m = {
			{1.0f, 0.0f, 0.0f, 0.0f},
			{0.0f, -1.0f, 0.0f, 0.0f},
			{0.0f, 0.0f, 1.0f, 0.0f},
			{0.0f, 0.0f, 0.0f, 1.0f},
		},
	},
	[WL_OUTPUT_TRANSFORM_FLIPPED_270] = {
		.m = {
			{0.0f, -1.0f, 0.0f, 0.0f},
			{-1.0f, 0.0f, 0.0f, 0.0f},
			{0.0f, 0.0f, 1.0f, 0.0f},
			{0.0f, 0.0f, 0.0f, 1.0f},
		},
	},
};

enum wl_output_transform
transform_invert(enum wl_output_transform transform)
{
	if ((transform & WL_OUTPUT_TRANSFORM_90)
			&& !(transform & WL_OUTPUT_TRANSFORM_FLIPPED)) {
		transform ^= WL_OUTPUT_TRANSFORM_180;
	}
	return transform;
}

void
matrix_identity(struct matrix *matrix)
{
	*matrix = (struct matrix){
		.m = {
			{1.0f, 0.0f, 0.0f, 0.0f},
			{0.0f, 1.0f, 0.0f, 0.0f},
			{0.0f, 0.0f, 1.0f, 0.0f},
			{0.0f, 0.0f, 0.0f, 1.0f},
		},
	};
}

void
matrix_multiply(struct matrix *dest, const struct matrix *a, const struct matrix *b)
{
	struct matrix product = {0};
	for (int i = 0; i < 4; i++) {
		for (int j = 0; j < 4; j++) {
			for (int k = 0; k < 4; k++) {
				product.m[i][j] += a->m[i][k] * b->m[k][j];
			}
		}
	}
	*dest = product;
}

void
matrix_translate(struct matrix *matrix, float x, float y)
{
	struct matrix translation = {0};
	matrix_identity(&translation);
	translation.m[0][3] = x;
	translation.m[1][3] = y;
	matrix_multiply(matrix, matrix, &translation);
}

void
matrix_scale(struct matrix *matrix, float x, float y)
{
	struct matrix scale = {0};
	matrix_identity(&scale);
	scale.m[0][0] = x;
	scale.m[1][1] = y;
	matrix_multiply(matrix, matrix, &scale);
}

void
matrix_transform(struct matrix *matrix, enum wl_output_transform transform)
{
	assert(transform >= WL_OUTPUT_TRANSFORM_NORMAL
		&& transform <= WL_OUTPUT_TRANSFORM_FLIPPED_270);
	matrix_multiply(matrix, matrix, &transform_matrices[transform]);
}

// Transforms a point in a space of the size, which is before the transform.
void
point_transform(double *x, double *y, enum wl_output_transform transform,
	double width, double height)
{
	double src_x = *x;
	double src_y = *y;
	switch (transform) {
	case WL_OUTPUT_TRANSFORM_NORMAL:
		break;
	case WL_OUTPUT_TRANSFORM_90:
		*x = height - src_y;
		*y = src_x;
		break;
	case WL_OUTPUT_TRANSFORM_180:
		*x = width - src_x;
		*y = height - src_y;
		break;
	case WL_OUTPUT_TRANSFORM_270:
		*x = src_y;
		*y = width - src_x;
		break;
	case WL_OUTPUT_TRANSFORM_FLIPPED:
		*x = width - src_x;
		break;
	case WL_OUTPUT_TRANSFORM_FLIPPED_90:
		*x = src_y;
		*y = src_x;
		break;
	case WL_OUTPUT_TRANSFORM_FLIPPED_180:
		*y = height - src_y;
		break;
	case WL_OUTPUT_TRANSFORM_FLIPPED_270:
		*x = height - src_y;
		*y = width - src_x;
		break;
	default:
		assert(!"unknown transform");
	}
}

// Transforms the opposite corners of the box, like point_transform().
void
box_transform(struct box *dest, const struct box *box,
	enum wl_output_transform transform, int width, int height)
{
	struct box src = box ? *box : (struct box){0};
	double x1 = src.x;
	double y1 = src.y;
	double x2 = src.x + src.width;
	double y2 = src.y + src.height;
	point_transform(&x1, &y1, transform, width, height);
	point_transform(&x2, &y2, transform, width, height);
	*dest = (struct box){
		.x = (int)MIN(x1, x2),
		.y = (int)MIN(y1, y2),
		.width = transform % 2 == 0 ? src.width : src.height,
		.height = transform % 2 == 0 ? src.height : src.width,
	};
}

bool
box_empty(const struct box *box)
{
	return box == NULL || box->width <= 0 || box->height <= 0;
}

bool
box_intersects(const struct box *a, const struct box *b)
{
	if (box_empty(a) || box_empty(b)) {
		return false;
	}
	return a->x < b->x + b->width && b->x < a->x + a->width
		&& a->y < b->y + b->height && b->y < a->y + a->height;
}

bool
box_intersection(struct box *dest, const struct box *a, const struct box *b)
{
	if (box_empty(a) || box_empty(b)) {
		*dest = (struct box){0};
		return false;
	}

	int x1 = MAX(a->x, b->x);
	int y1 = MAX(a->y, b->y);
	int x2 = MIN(a->x + a->width, b->x + b->width);
	int y2 = MIN(a->y + a->height, b->y + b->height);
	*dest = (struct box){
		.x = x1,
		.y = y1,
		.width = x2 - x1,
		.height = y2 - y1,
	};
	if (box_empty(dest)) {
		*dest = (struct box){0};
		return false;
	}
	return true;
}

void
box_union(struct box *dest, const struct box *src)
{
	if (box_empty(src)) {
		return;
	}
	if (box_empty(dest)) {
		*dest = *src;
		return;
	}

	int x1 = MIN(dest->x, src->x);
	int y1 = MIN(dest->y, src->y);
	int x2 = MAX(dest->x + dest->width, src->x + src->width);
	int y2 = MAX(dest->y + dest->height, src->y + src->height);
	*dest = (struct box){
		.x = x1,
		.y = y1,
		.width = x2 - x1,
		.height = y2 - y1,
	};
}

bool
box_equal(const struct box *a, const struct box *b)
{
	if (box_empty(a)) {
		return box_empty(b);
	}
	if (box_empty(b)) {
		return false;
	}
	return a->x == b->x && a->y == b->y
		&& a->width == b->width && a->height == b->height;
}

bool
box_contains_point(const struct box *box, double x, double y)
{
	if (box_empty(box)) {
		return false;
	}
	return x >= box->x && x < box->x + box->width
		&& y >= box->y && y < box->y + box->height;
}

bool
box_contains_box(const struct box *box, const struct box *inner)
{
	if (box_empty(box) || box_empty(inner)) {
		return false;
	}
	return inner->x >= box->x && inner->y >= box->y
		&& inner->x + inner->width <= box->x + box->width
		&& inner->y + inner->height <= box->y + box->height;
}

struct box
box_from_pixman(const pixman_box32_t *rect)
{
	return (struct box){
		.x = rect->x1,
		.y = rect->y1,
		.width = rect->x2 - rect->x1,
		.height = rect->y2 - rect->y1,
	};
}
