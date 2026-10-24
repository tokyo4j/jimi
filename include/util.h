#ifndef JIMI_UTIL_H
#define JIMI_UTIL_H

#include <pixman.h>
#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

struct box {
	int x;
	int y;
	int width;
	int height;
};

struct fbox {
	double x;
	double y;
	double width;
	double height;
};

struct matrix {
	// Row-major 4x4 matrix using column vectors.
	float m[4][4];
};

struct color {
	float r;
	float g;
	float b;
	float a;
};

static inline double
clamp(double value, double min, double max)
{
	return MAX(min, MIN(value, max));
}

static inline void
list_append(struct wl_list *list, struct wl_list *link)
{
	wl_list_insert(list->prev, link);
}

uint32_t monotonic_msec(void);
void raise_nofile_limit(void);
void prepare_exec(void);
void spawn(const char *command);
int create_sealed_file(const char *name, const void *data, size_t size);
void resource_destroy_req(struct wl_client *client, struct wl_resource *resource);
void resource_unlink(struct wl_resource *resource);
void callbacks_send_done(struct wl_list *callbacks, uint32_t data);
void list_move(struct wl_list *destination, struct wl_list *source);
bool box_empty(const struct box *box);
bool box_intersects(const struct box *a, const struct box *b);
bool box_intersection(struct box *dest, const struct box *a, const struct box *b);
void box_union(struct box *dest, const struct box *src);
bool box_equal(const struct box *a, const struct box *b);
bool box_contains_point(const struct box *box, double x, double y);
bool box_contains_box(const struct box *box, const struct box *inner);
struct box box_from_pixman(const pixman_box32_t *rect);
void point_transform(double *x, double *y,
	enum wl_output_transform transform, double width, double height);
void box_transform(struct box *dest, const struct box *box,
	enum wl_output_transform transform, int width, int height);
enum wl_output_transform transform_invert(enum wl_output_transform transform);
void matrix_identity(struct matrix *matrix);
void matrix_multiply(struct matrix *dest, const struct matrix *a,
	const struct matrix *b);
void matrix_translate(struct matrix *matrix, float x, float y);
void matrix_scale(struct matrix *matrix, float x, float y);
void matrix_transform(struct matrix *matrix, enum wl_output_transform transform);

#endif
