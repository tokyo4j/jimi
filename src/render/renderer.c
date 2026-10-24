#include <assert.h>
#include <math.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <gbm.h>

#include "config.h"
#include "internal.h"

// Each rectangle of the damage costs a draw call per node in it, so more are
// drawn as their extents. The uncovered damage of a node is limited likewise
// (see uncovered_damage()).
#define MAX_DAMAGE_RECTS 16
// Around the lens of the magnifier, in logical pixels, like in KWin
#define MAGNIFIER_FRAME_WIDTH 5
#define MAX_MAGNIFIER_ZOOM 100.0

static uint32_t
msec_now(void)
{
	struct timespec ts;
	assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

// The image being drawn into: the swapchain image of an output, its blend
// buffer, or a texture. The pass draws in the orientation of the layout, and
// the transform rotates and flips the drawing into the image. The colors are
// converted into those of the image description.
struct render_pass {
	VkCommandBuffer cmd;
	VkImage image;
	VkImageView view;
	const struct pixel_format *format;
	const struct image_description *description;
	int width; // In the pixels of the image, before the transform
	int height;
	double scale; // Pixels of the image per logical pixel
	enum wl_output_transform transform;
	// The pixels to draw, in the orientation of the layout and within the
	// size. The rest of the image is kept.
	const pixman_region32_t *damage;
	uint32_t time; // Sent to the frame callbacks of the drawn surfaces
};

// Logical pixels in physical pixels of the scale, rounded half away from zero
// like fractional-scale-v1 clients round their buffer sizes.
static int
physical(double scale, double logical)
{
	return (int)round(logical * scale);
}

// Also drops the damage outside the output, which a new transform may leave.
void
damage_output(struct output *output)
{
	struct box damage = output_physical_box(output);
	output->needs_repaint = true;
	// The cursor is drawn for the scale and the transform.
	output->cursor.outdated = true;
	for (int i = 0; i < NR_SWAPCHAIN_IMAGES; i++) {
		struct swapchain_image *image = &output->swapchain[i];
		if (image->initialized) {
			pixman_region32_reset(&image->damage, &(pixman_box32_t){
				.x2 = damage.width,
				.y2 = damage.height,
			});
		}
	}
	image_capture_damage_output(output, &damage);
}

// The box in layout coordinates in the pixels of the output, with the pixel
// around it covering the rounding of the nodes (see add_scene()). False if
// it is off the output.
static bool
output_damage_box(struct output *output, const struct box *box, struct box *damage)
{
	double x1 = (box->x - output->box.x) * output->scale;
	double y1 = (box->y - output->box.y) * output->scale;
	double x2 = (box->x + box->width - output->box.x) * output->scale;
	double y2 = (box->y + box->height - output->box.y) * output->scale;
	*damage = (struct box){
		.x = (int)floor(x1) - 1,
		.y = (int)floor(y1) - 1,
		.width = (int)ceil(x2) - (int)floor(x1) + 2,
		.height = (int)ceil(y2) - (int)floor(y1) + 2,
	};
	struct box output_box = output_physical_box(output);
	return box_intersection(damage, damage, &output_box);
}

static void
damage_swapchain(struct output *output, const struct box *damage)
{
	output->needs_repaint = true;
	for (int i = 0; i < NR_SWAPCHAIN_IMAGES; i++) {
		struct swapchain_image *image = &output->swapchain[i];
		if (image->initialized) {
			pixman_region32_union_rect(&image->damage, &image->damage,
				damage->x, damage->y, (unsigned)damage->width,
				(unsigned)damage->height);
		}
	}
	image_capture_damage_output(output, damage);
}

// The output under the pointer shows the magnifier.
static bool
output_magnified(struct output *output)
{
	struct pointer *pointer = &core.seat.pointer;
	return core.magnifier.zoom > 1.0
		&& output_nearest(pointer->x, pointer->y) == output;
}

// The lens of the magnifier for the pointer at x, y in the layout, in the
// pixels of the output, in the orientation of the layout: the whole output,
// or the size of the lens centered on the pointer but kept on the output.
static struct box
magnifier_lens(struct output *output, double x, double y)
{
	struct box box = output_physical_box(output);
	if (config.magnifier.fullscreen) {
		return box;
	}
	int width = MIN(physical(output->scale, config.magnifier.width), box.width);
	int height = MIN(physical(output->scale, config.magnifier.height), box.height);
	double pointer_x = (x - output->box.x) * output->scale;
	double pointer_y = (y - output->box.y) * output->scale;
	return (struct box){
		.x = (int)clamp(round(pointer_x - width / 2.0), 0, box.width - width),
		.y = (int)clamp(round(pointer_y - height / 2.0), 0, box.height - height),
		.width = width,
		.height = height,
	};
}

// The lens with the frame around it, which is off the output for fullscreen
// lenses.
static struct box
magnifier_frame(struct output *output, const struct box *lens)
{
	int width = physical(output->scale, MAGNIFIER_FRAME_WIDTH);
	return (struct box){
		.x = lens->x - width,
		.y = lens->y - width,
		.width = lens->width + width * 2,
		.height = lens->height + width * 2,
	};
}

// The magnifier follows the pointer at x, y in the layout, before or after it
// moves.
void
damage_magnifier(double x, double y)
{
	struct output *output = output_nearest(x, y);
	if (core.magnifier.zoom == 1.0 || !output) {
		return;
	}
	struct box lens = magnifier_lens(output, x, y);
	struct box damage = magnifier_frame(output, &lens);
	struct box output_box = output_physical_box(output);
	box_intersection(&damage, &damage, &output_box);
	damage_swapchain(output, &damage);
}

// Zooming out to 1 stops magnifying, even if dividing by the zoom factor
// misses it by the rounding errors. The cursor moves between the cursor planes
// and the images of all outputs, as it may be on several, which are drawn
// again whole.
void
magnifier_set_zoom(double zoom)
{
	core.magnifier.zoom = zoom < 1.0 + 1e-9 ? 1.0 : MIN(zoom, MAX_MAGNIFIER_ZOOM);
	struct output *output;
	wl_list_for_each(output, &core.outputs, link) {
		if (output->enabled) {
			damage_output(output);
		}
	}
}

void
damage_layout_box(const struct box *box)
{
	if (box_empty(box)) {
		return;
	}

	struct output *output;
	wl_list_for_each(output, &core.outputs, link) {
		struct box damage;
		if (output->enabled && output_damage_box(output, box, &damage)) {
			damage_swapchain(output, &damage);
		}
	}
}

// The box of the cursor in the pixels of the output, in the orientation of
// the layout, or an empty box if no cursor is shown.
static struct box
cursor_box(struct output *output)
{
	double scale = output->scale;
	struct surface *client_cursor = pointer_client_cursor_surface();
	enum wp_cursor_shape_device_v1_shape shape = pointer_cursor_shape();
	if (client_cursor) {
		struct box logical = pointer_surface_box(client_cursor);
		return (struct box){
			.x = physical(scale, logical.x - output->box.x),
			.y = physical(scale, logical.y - output->box.y),
			.width = physical(scale, logical.width),
			.height = physical(scale, logical.height),
		};
	}
	// The images of the shapes are loaded for the scale, and drawn
	// unscaled.
	if (shape) {
		struct pointer *pointer = &core.seat.pointer;
		struct server_cursor *cursor = &output->server_cursors[shape];
		return (struct box){
			.x = physical(scale, pointer->x - output->box.x) - cursor->hotspot_x,
			.y = physical(scale, pointer->y - output->box.y) - cursor->hotspot_y,
			.width = (int)cursor->width,
			.height = (int)cursor->height,
		};
	}
	return (struct box){0};
}

// The size of the cursor plane in the orientation of the layout.
static struct box
cursor_plane_size(struct output *output)
{
	struct box size = {
		.width = (int)core.kms.cursor_width,
		.height = (int)core.kms.cursor_height,
	};
	box_transform(&size, &size, output->transform, size.width, size.height);
	return size;
}

// Whether the cursor plane of the output can show the cursor, which is
// otherwise drawn into the images of the swapchain. It always is while
// magnifying, so that the magnifier copies it from them.
static bool
cursor_fits_plane(struct output *output)
{
	struct box box = cursor_box(output);
	struct box plane = cursor_plane_size(output);
	return output->cursor.plane.id && core.magnifier.zoom == 1.0
		&& box.width <= plane.width && box.height <= plane.height;
}

// The cursor is shown in the box, in layout coordinates, before or after it
// changes. The outputs showing it on their cursor planes commit them again,
// and draw it again unless it only moved.
void
damage_cursor(const struct box *box, bool moved)
{
	struct output *output;
	wl_list_for_each(output, &core.outputs, link) {
		if (!output->enabled) {
			continue;
		}
		if (!moved) {
			output->cursor.outdated = true;
		}
		struct box damage;
		if (box_empty(box) || !output_damage_box(output, box, &damage)) {
			continue;
		}
		if (cursor_fits_plane(output)) {
			output->needs_commit = true;
			image_capture_damage_output(output, &damage);
		} else {
			damage_swapchain(output, &damage);
		}
	}
}

// The surfaces on the outputs of the bits wait for frame callbacks.
void
schedule_frames(uint32_t outputs)
{
	struct output *output;
	wl_list_for_each(output, &core.outputs, link) {
		if (outputs & output->bit) {
			output->needs_frame = true;
		}
	}
}

void
renderer_init(void)
{
	vulkan_init();
	formats_init();
	repaint_init();
}

void
renderer_wait_idle(void)
{
	vkDeviceWaitIdle(core.vk.dev);
}

// NULL while the surface has no buffer, or has lost the contents of its shm
// buffer.
static struct texture *
surface_texture(struct surface *surface)
{
	struct client_buffer *buffer = surface->current.buffer;
	if (!buffer) {
		return NULL;
	}
	return buffer->type == CLIENT_BUFFER_SHM ? surface->shm_texture
		: buffer->texture;
}

static const struct fbox whole_texture = {.width = 1.0, .height = 1.0};

// The rows of the matrix that the vertex shaders apply to the normalized
// device coordinates of the pass to transform them into the image.
static void
get_ndc_transform(const struct render_pass *pass, float rows[static 4])
{
	struct matrix matrix;
	matrix_identity(&matrix);
	matrix_transform(&matrix, transform_invert(pass->transform));
	rows[0] = matrix.m[0][0];
	rows[1] = matrix.m[0][1];
	rows[2] = matrix.m[1][0];
	rows[3] = matrix.m[1][1];
}

// The box in pixels of the pass, in pixels of the image.
static VkRect2D
image_rect(const struct render_pass *pass, const struct box *box)
{
	struct box rect;
	box_transform(&rect, box, transform_invert(pass->transform), pass->width,
		pass->height);
	return (VkRect2D){
		.offset = {rect.x, rect.y},
		.extent = {(uint32_t)rect.width, (uint32_t)rect.height},
	};
}

static bool
box_in_region(const pixman_region32_t *region, const struct box *box)
{
	pixman_box32_t rect = {
		.x1 = box->x,
		.y1 = box->y,
		.x2 = box->x + box->width,
		.y2 = box->y + box->height,
	};
	return pixman_region32_contains_rectangle(region, &rect) != PIXMAN_REGION_OUT;
}

// Draws the bound pipeline into the region within the box, scissored to one
// rectangle of the region at a time.
static void
draw_region(const struct render_pass *pass, const struct box *box,
	const pixman_region32_t *region)
{
	int nr_rects;
	const pixman_box32_t *rects = pixman_region32_rectangles(region, &nr_rects);
	for (int i = 0; i < nr_rects; i++) {
		struct box rect = box_from_pixman(&rects[i]);
		if (!box_intersection(&rect, &rect, box)) {
			continue;
		}
		VkRect2D scissor = image_rect(pass, &rect);
		vkCmdSetScissor(pass->cmd, 0, 1, &scissor);
		vkCmdDraw(pass->cmd, 6, 1, 0, 0);
	}
}

// Draws the source, in the unit square of the transformed texture, into the
// region within the box in pixels of the image. The colors of the texture are
// in the image description. Opaque textures are drawn without their alpha
// channel.
static void
draw_texture(const struct render_pass *pass, struct texture *texture,
	const struct image_description *description, const struct box *box,
	const pixman_region32_t *region, enum wl_output_transform transform,
	const struct fbox *source, bool opaque)
{
	if (!texture || !box_in_region(region, box)) {
		return;
	}
	// TODO: Clip partially visible nodes instead of drawing them past output bounds.

	struct vk *vk = &core.vk;
	struct matrix uv_transform;
	matrix_identity(&uv_transform);
	matrix_translate(&uv_transform, 0.5f, 0.5f);
	matrix_transform(&uv_transform, transform_invert(transform));
	matrix_translate(&uv_transform, -0.5f, -0.5f);
	matrix_translate(&uv_transform, (float)source->x, (float)source->y);
	matrix_scale(&uv_transform, (float)source->width, (float)source->height);
	if (texture->y_invert) {
		struct matrix flip;
		matrix_identity(&flip);
		matrix_translate(&flip, 0.5f, 0.5f);
		matrix_transform(&flip, WL_OUTPUT_TRANSFORM_FLIPPED_180);
		matrix_translate(&flip, -0.5f, -0.5f);
		matrix_multiply(&uv_transform, &flip, &uv_transform);
	}

	struct texture_push_constants push = {
		.output_size = {
			(float)pass->width,
			(float)pass->height,
			0.0f,
			0.0f,
		},
		.dst = {
			(float)box->x,
			(float)box->y,
			(float)box->width,
			(float)box->height,
		},
		.uv_transform = uv_transform,
		.opaque = opaque || !texture->format->has_alpha,
	};
	get_ndc_transform(pass, push.ndc_transform);
	if (texture->format->ycbcr_bits) {
		push.ycbcr = true;
		double kr, kb;
		color_ycbcr_coefficients(description, &kr, &kb);
		ycbcr_matrix(texture->format, kr, kb, push.ycbcr_matrix);
	}
	struct color_transform color;
	color_transform_init(&color, description, pass->description);
	if (!color.identity) {
		for (int i = 0; i < 3; i++) {
			for (int j = 0; j < 3; j++) {
				push.color_matrix[i][j] = color.matrix[i][j];
			}
		}
		push.src_tf = color.src_tf;
		push.src_power = color.src_power;
		push.dst_tf = color.dst_tf;
		push.dst_power = color.dst_power;
		push.convert = true;
	}
	vkCmdPushConstants(pass->cmd, vk->texture_pipe_layout,
		VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
		0, sizeof(push), &push);
	VkBool32 blend = !push.opaque;
	vk->api.vkCmdSetColorBlendEnableEXT(pass->cmd, 0, 1, &blend);
	vkCmdBindPipeline(pass->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
		vulkan_pipelines(pass->format->vk)->texture);
	vkCmdBindDescriptorSets(pass->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
		vk->texture_pipe_layout, 0, 1, &texture->descriptor_set, 0, NULL);
	draw_region(pass, box, region);
}

static bool
surface_is_fully_opaque(struct surface *surface)
{
	pixman_box32_t bounds = {
		.x1 = 0,
		.y1 = 0,
		.x2 = surface->width,
		.y2 = surface->height,
	};
	return pixman_region32_contains_rectangle(&surface->current.opaque_region,
		&bounds) == PIXMAN_REGION_IN;
}

// Draws the sRGB color into the region within the box in pixels of the
// image, with the radius and the border width in pixels of the image.
static void
draw_solid_box(const struct render_pass *pass, const struct box *box,
	const pixman_region32_t *region, struct color color, float radius,
	uint32_t corners, float border_width)
{
	if (!box_in_region(region, box)) {
		return;
	}

	struct vk *vk = &core.vk;
	color = color_convert(color, pass->description);
	struct solid_push_constants push = {
		.output_size = {
			(float)pass->width,
			(float)pass->height,
			0.0f,
			0.0f,
		},
		.dst = {
			box->x,
			box->y,
			box->width,
			box->height,
		},
		.color = {color.r, color.g, color.b, color.a},
		.radius = radius,
		.corner_mask = corners,
		.border_width = border_width,
	};
	get_ndc_transform(pass, push.ndc_transform);
	vkCmdPushConstants(pass->cmd, vk->solid_pipe_layout,
		VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
		0, sizeof(push), &push);
	VkBool32 blend = color.a < 1.0f || radius > 0.0f || border_width > 0.0f;
	vk->api.vkCmdSetColorBlendEnableEXT(pass->cmd, 0, 1, &blend);
	vkCmdBindPipeline(pass->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
		vulkan_pipelines(pass->format->vk)->solid);
	draw_region(pass, box, region);
}

// Draws the shadow into the region within the box in pixels of the image,
// which add_scene_node() lays out around the box casting the shadow.
static void
draw_shadow(const struct render_pass *pass, struct scene_shadow *shadow,
	const struct box *box, const pixman_region32_t *region)
{
	if (!box_in_region(region, box)) {
		return;
	}

	struct vk *vk = &core.vk;
	int margin = physical(pass->scale, shadow->blur);
	int offset_x = physical(pass->scale, shadow->offset_x);
	int offset_y = physical(pass->scale, shadow->offset_y);
	struct color color = color_convert(shadow->color, pass->description);
	struct shadow_push_constants push = {
		.output_size = {
			(float)pass->width,
			(float)pass->height,
			0.0f,
			0.0f,
		},
		.dst = {
			box->x,
			box->y,
			box->width,
			box->height,
		},
		.color = {color.r, color.g, color.b, color.a},
		.box = {
			margin - offset_x,
			margin - offset_y,
			box->width - margin * 2,
			box->height - margin * 2,
		},
		.offset = {offset_x, offset_y},
		.radius = shadow->radius * (float)pass->scale,
		.corner_mask = shadow->corners,
		// The gaussian is negligible past 3 sigmas.
		.sigma = (float)(shadow->blur * pass->scale / 3.0),
	};
	get_ndc_transform(pass, push.ndc_transform);
	vkCmdPushConstants(pass->cmd, vk->shadow_pipe_layout,
		VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
		0, sizeof(push), &push);
	VkBool32 blend = VK_TRUE;
	vk->api.vkCmdSetColorBlendEnableEXT(pass->cmd, 0, 1, &blend);
	vkCmdBindPipeline(pass->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
		vulkan_pipelines(pass->format->vk)->shadow);
	draw_region(pass, box, region);
}

// That the buffer of the surface is in
static const struct image_description *
surface_image_description(struct surface *surface)
{
	return surface->current.image_description
		? surface->current.image_description : &srgb_description;
}

// Draws the part of the buffer shown by the viewport into the region within
// the box in pixels of the image.
static void
draw_surface(const struct render_pass *pass, struct surface *surface,
	const struct box *box, const pixman_region32_t *region)
{
	struct texture *texture = surface_texture(surface);
	if (!texture) {
		return;
	}
	struct box buffer = {
		.width = (int)texture->width,
		.height = (int)texture->height,
	};
	box_transform(&buffer, &buffer, surface->current.transform,
		buffer.width, buffer.height);
	struct fbox source = surface_source_box(surface);
	source.x /= buffer.width;
	source.y /= buffer.height;
	source.width /= buffer.width;
	source.height /= buffer.height;
	draw_texture(pass, texture, surface_image_description(surface), box,
		region, surface->current.transform, &source,
		surface_is_fully_opaque(surface));
}

// Draws the drag icon or the cursor of the client at the box in pixels of the
// pass. Unlike the surfaces in render lists, they may be off the image, and
// get their frame callbacks only if they are in it.
static void
draw_pointer_surface(const struct render_pass *pass, struct surface *surface,
	const struct box *box)
{
	struct box image = {.width = pass->width, .height = pass->height};
	if (box_intersects(&image, box)) {
		draw_surface(pass, surface, box, pass->damage);
		callbacks_send_done(&surface->current.frame_callbacks, pass->time);
	}
}

static void
draw_drag_icon(const struct render_pass *pass, struct output *output,
	struct surface *surface)
{
	struct box logical = pointer_surface_box(surface);
	struct box box = {
		.x = physical(pass->scale, logical.x - output->box.x),
		.y = physical(pass->scale, logical.y - output->box.y),
		.width = physical(pass->scale, logical.width),
		.height = physical(pass->scale, logical.height),
	};
	draw_pointer_surface(pass, surface, &box);
}

// Draws the cursor at the box in pixels of the pass.
static void
draw_cursor(const struct render_pass *pass, struct output *output,
	const struct box *box)
{
	struct surface *client_cursor = pointer_client_cursor_surface();
	enum wp_cursor_shape_device_v1_shape shape = pointer_cursor_shape();
	if (client_cursor) {
		draw_pointer_surface(pass, client_cursor, box);
	} else if (shape) {
		draw_texture(pass, output->server_cursors[shape].texture,
			&srgb_description, box, pass->damage,
			WL_OUTPUT_TRANSFORM_NORMAL, &whole_texture, false);
	}
}

// The list is built for an image of the size, in the orientation of the
// layout, and is then drawn into its damage.
static void
render_list_init(struct render_list *list, int width, int height, double scale)
{
	wl_array_init(&list->entries);
	pixman_region32_init_rect(&list->uncovered, 0, 0, (unsigned)width,
		(unsigned)height);
	list->scale = scale;
}

void
render_list_fini(struct render_list *list)
{
	struct render_entry *entry;
	wl_array_for_each(entry, &list->entries) {
		pixman_region32_fini(&entry->visible);
	}
	wl_array_release(&list->entries);
	pixman_region32_fini(&list->uncovered);
}

// Sets the region to the damage within the box that is uncovered, or to all
// the damage within the box if that is split into too many rectangles. The
// nodes drawn later cover the extra pixels again.
static void
uncovered_damage(const struct render_pass *pass, pixman_region32_t *region,
	const pixman_region32_t *uncovered, const struct box *box)
{
	pixman_region32_intersect_rect(region, uncovered, box->x, box->y,
		(unsigned)box->width, (unsigned)box->height);
	pixman_region32_intersect(region, region, pass->damage);
	if (pixman_region32_n_rects(region) > MAX_DAMAGE_RECTS) {
		pixman_region32_intersect_rect(region, pass->damage, box->x, box->y,
			(unsigned)box->width, (unsigned)box->height);
	}
}

// Adds the pixels of the box that the surface fills with opaque colors.
static void
add_surface_opaque_region(pixman_region32_t *opaque, struct surface *surface,
	const struct box *box)
{
	struct texture *texture = surface_texture(surface);
	if (!texture) {
		return;
	}
	// Drawn without blending.
	if (!texture->format->has_alpha || surface_is_fully_opaque(surface)) {
		pixman_region32_union_rect(opaque, opaque, box->x, box->y,
			(unsigned)box->width, (unsigned)box->height);
		return;
	}

	// Linear filtering blends the texels around the opaque region into its
	// edges if the buffer is upscaled, which is rare.
	struct fbox source = surface_source_box(surface);
	if (box->width > source.width || box->height > source.height) {
		return;
	}

	double scale_x = (double)box->width / surface->width;
	double scale_y = (double)box->height / surface->height;
	int nr_rects;
	const pixman_box32_t *rects = pixman_region32_rectangles(
		&surface->current.opaque_region, &nr_rects);
	for (int i = 0; i < nr_rects; i++) {
		// The region may extend past the surface.
		int x1 = (int)ceil(MAX(rects[i].x1, 0) * scale_x);
		int y1 = (int)ceil(MAX(rects[i].y1, 0) * scale_y);
		int x2 = (int)floor(MIN(rects[i].x2, surface->width) * scale_x);
		int y2 = (int)floor(MIN(rects[i].y2, surface->height) * scale_y);
		if (x1 < x2 && y1 < y2) {
			pixman_region32_union_rect(opaque, opaque, box->x + x1,
				box->y + y1, (unsigned)(x2 - x1), (unsigned)(y2 - y1));
		}
	}
}

// Adds the pixels of the box that the rect fills with opaque colors: all but
// its rounded corners, unless it is translucent or only has a border.
static void
add_rect_opaque_region(pixman_region32_t *opaque, double scale,
	struct scene_rect *rect, const struct box *box)
{
	if (rect->color.a < 1.0f || rect->border_width > 0) {
		return;
	}
	pixman_region32_t region;
	pixman_region32_init_rect(&region, box->x, box->y, (unsigned)box->width,
		(unsigned)box->height);
	int radius = (int)ceil(rect->radius * scale);
	int right = box->x + box->width - radius;
	int bottom = box->y + box->height - radius;
	pixman_region32_t corners;
	pixman_region32_init(&corners);
	if (rect->corners & CORNER_TOP_LEFT) {
		pixman_region32_union_rect(&corners, &corners, box->x, box->y,
			(unsigned)radius, (unsigned)radius);
	}
	if (rect->corners & CORNER_TOP_RIGHT) {
		pixman_region32_union_rect(&corners, &corners, right, box->y,
			(unsigned)radius, (unsigned)radius);
	}
	if (rect->corners & CORNER_BOTTOM_RIGHT) {
		pixman_region32_union_rect(&corners, &corners, right, bottom,
			(unsigned)radius, (unsigned)radius);
	}
	if (rect->corners & CORNER_BOTTOM_LEFT) {
		pixman_region32_union_rect(&corners, &corners, box->x, bottom,
			(unsigned)radius, (unsigned)radius);
	}
	pixman_region32_subtract(&region, &region, &corners);
	pixman_region32_union(opaque, opaque, &region);
	pixman_region32_fini(&corners);
	pixman_region32_fini(&region);
}

// Adds the leaf node, at the box in pixels of the image, below the nodes in
// the list, unless they cover it. Then the opaque part of the node covers the
// nodes added after it.
static void
add_render_entry(struct render_list *list, struct scene_node *node,
	const struct box *box)
{
	if (box_empty(box) || !box_in_region(&list->uncovered, box)) {
		return;
	}
	struct render_entry *entry = wl_array_add(&list->entries, sizeof(*entry));
	assert(entry);
	entry->node = node;
	entry->box = *box;
	pixman_region32_init(&entry->visible);
	pixman_region32_intersect_rect(&entry->visible, &list->uncovered,
		box->x, box->y, (unsigned)box->width, (unsigned)box->height);

	pixman_region32_t opaque;
	pixman_region32_init(&opaque);
	if (node->type == SCENE_NODE_SURFACE) {
		struct surface *surface = wl_container_of(node, surface, scene.node);
		add_surface_opaque_region(&opaque, surface, box);
	} else if (node->type == SCENE_NODE_RECT) {
		struct scene_rect *rect = wl_container_of(node, rect, node);
		add_rect_opaque_region(&opaque, list->scale, rect, box);
	}
	pixman_region32_subtract(&list->uncovered, &list->uncovered, &opaque);
	pixman_region32_fini(&opaque);
}

// Adds the node at x, y logical pixels from the top-level node containing it,
// which is at origin_x, origin_y pixels in the image, and the nodes in it from
// the top.
static void
add_scene_node(struct render_list *list, struct scene_node *node,
	int origin_x, int origin_y, int x, int y)
{
	if (!node->enabled) {
		return;
	}

	struct box box = {
		.x = origin_x + physical(list->scale, x),
		.y = origin_y + physical(list->scale, y),
	};
	switch (node->type) {
	case SCENE_NODE_TREE: {
		struct scene_tree *tree = wl_container_of(node, tree, node);
		struct scene_node *child;
		wl_list_for_each_reverse(child, &tree->children, link) {
			add_scene_node(list, child, origin_x, origin_y, x + child->x,
				y + child->y);
		}
		return;
	}
	case SCENE_NODE_SURFACE: {
		// Fractional-scale clients draw buffers of the rounded size.
		struct surface *surface = wl_container_of(node, surface, scene.node);
		box.width = physical(list->scale, surface->width);
		box.height = physical(list->scale, surface->height);
		break;
	}
	case SCENE_NODE_RECT: {
		// Rects fill up to the rounded position of their far edges, so that
		// they meet the surfaces and rects next to them.
		struct scene_rect *rect = wl_container_of(node, rect, node);
		box.width = physical(list->scale, x + rect->width)
			- physical(list->scale, x);
		box.height = physical(list->scale, y + rect->height)
			- physical(list->scale, y);
		break;
	}
	case SCENE_NODE_SHADOW: {
		// The box casting the shadow is rounded like rects, so that it
		// matches the rects filling it. The shadow is drawn around it.
		struct scene_shadow *shadow = wl_container_of(node, shadow, node);
		int margin = physical(list->scale, shadow->blur);
		box.x += physical(list->scale, shadow->offset_x) - margin;
		box.y += physical(list->scale, shadow->offset_y) - margin;
		box.width = physical(list->scale, x + shadow->width)
			- physical(list->scale, x) + margin * 2;
		box.height = physical(list->scale, y + shadow->height)
			- physical(list->scale, y) + margin * 2;
		break;
	}
	case SCENE_NODE_TEXTURE: {
		// The textures are created for the scale of the output the node is
		// mostly on, and scaled on the others.
		struct scene_texture *texture = wl_container_of(node, texture, node);
		box.width = physical(list->scale, texture->width);
		box.height = physical(list->scale, texture->height);
		break;
	}
	case SCENE_NODE_THUMBNAIL: {
		struct scene_thumbnail *thumbnail = wl_container_of(node, thumbnail, node);
		box.width = physical(list->scale, thumbnail->width);
		box.height = physical(list->scale, thumbnail->height);
		break;
	}
	}
	add_render_entry(list, node, &box);
}

// Nodes are positioned in logical pixels. Each top-level node in the layers
// is rounded to physical pixels, and the nodes in it are rounded relative to
// it. The surfaces and the decoration of a window then keep their layout in
// physical pixels wherever the window is, and surfaces are drawn at the size
// of their buffers.
static void
add_scene(struct render_list *list, struct output *output)
{
	// The layers are at the origin of the layout.
	for (size_t i = NR_SCENE_LAYERS; i-- > 0;) {
		if (!core.scene.layers[i].node.enabled) {
			continue;
		}
		struct scene_node *node;
		wl_list_for_each_reverse(node, &core.scene.layers[i].children, link) {
			add_scene_node(list, node,
				physical(list->scale, node->x - output->box.x),
				physical(list->scale, node->y - output->box.y), 0, 0);
		}
	}
}

// The nodes shown on the output, in its physical pixels.
void
render_list_init_output(struct render_list *list, struct output *output)
{
	struct box physical = output_physical_box(output);
	render_list_init(list, physical.width, physical.height, output->scale);
	add_scene(list, output);
}

// The surfaces in the list are shown in an image, whether drawn or scanned
// out, which paces them.
void
send_frame_callbacks(const struct render_list *list, uint32_t time)
{
	struct render_entry *entry;
	wl_array_for_each(entry, &list->entries) {
		if (entry->node->type == SCENE_NODE_SURFACE) {
			struct surface *surface =
				wl_container_of(entry->node, surface, scene.node);
			callbacks_send_done(&surface->current.frame_callbacks, time);
		}
	}
}

// A frame of an output shows the content updates of the surfaces in the
// list, and takes their presentation feedbacks into the feedbacks.
void
take_presentation_feedbacks(const struct render_list *list,
	struct wl_list *feedbacks)
{
	struct render_entry *entry;
	wl_array_for_each(entry, &list->entries) {
		if (entry->node->type == SCENE_NODE_SURFACE) {
			struct surface *surface =
				wl_container_of(entry->node, surface, scene.node);
			list_move(feedbacks, &surface->current.presentation_feedbacks);
		}
	}
}

// Draws the node into the region within the box.
static void
draw_render_entry(const struct render_pass *pass,
	const struct render_entry *entry, const pixman_region32_t *region)
{
	const struct box *box = &entry->box;
	struct scene_node *node = entry->node;
	switch (node->type) {
	case SCENE_NODE_TREE:
		assert(!"trees are not drawn");
		return;
	case SCENE_NODE_SURFACE: {
		struct surface *surface = wl_container_of(node, surface, scene.node);
		draw_surface(pass, surface, box, region);
		return;
	}
	case SCENE_NODE_RECT: {
		struct scene_rect *rect = wl_container_of(node, rect, node);
		draw_solid_box(pass, box, region, rect->color,
			rect->radius * (float)pass->scale, rect->corners,
			(float)physical(pass->scale, rect->border_width));
		return;
	}
	case SCENE_NODE_SHADOW: {
		struct scene_shadow *shadow = wl_container_of(node, shadow, node);
		draw_shadow(pass, shadow, box, region);
		return;
	}
	case SCENE_NODE_TEXTURE: {
		struct scene_texture *texture = wl_container_of(node, texture, node);
		assert(texture->texture);
		draw_texture(pass, texture->texture, &srgb_description, box, region,
			WL_OUTPUT_TRANSFORM_NORMAL, &whole_texture, false);
		return;
	}
	case SCENE_NODE_THUMBNAIL: {
		// Like texture nodes, but the surfaces are drawn into the texture
		// beforehand.
		struct scene_thumbnail *thumbnail = wl_container_of(node, thumbnail, node);
		assert(thumbnail->texture && !thumbnail->outdated);
		draw_texture(pass, thumbnail->texture, &srgb_description, box,
			region, WL_OUTPUT_TRANSFORM_NORMAL, &whole_texture, false);
		return;
	}
	}
}

// Draws the damage of the pass into the visible parts of the nodes of the
// list, from the bottom.
static void
draw_render_list(const struct render_pass *pass, const struct render_list *list)
{
	assert(list->scale == pass->scale);
	const struct render_entry *entries = list->entries.data;
	size_t nr_entries = list->entries.size / sizeof(*entries);
	pixman_region32_t region;
	pixman_region32_init(&region);
	for (size_t i = nr_entries; i-- > 0;) {
		uncovered_damage(pass, &region, &entries[i].visible, &entries[i].box);
		draw_render_entry(pass, &entries[i], &region);
	}
	pixman_region32_fini(&region);
}

static void
transition_dmabuf_texture(VkCommandBuffer cmd, struct texture *texture, bool acquire)
{
	if (!texture || !texture->dmabuf) {
		return;
	}
	struct vk *vk = &core.vk;
	vulkan_image_barrier(cmd, acquire ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
			: VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		acquire ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
			: VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, (VkImageMemoryBarrier){
			.srcAccessMask = acquire ? 0 : VK_ACCESS_SHADER_READ_BIT,
			.dstAccessMask = acquire ? VK_ACCESS_SHADER_READ_BIT : 0,
			.oldLayout = acquire ? VK_IMAGE_LAYOUT_GENERAL
				: VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			.newLayout = acquire ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
				: VK_IMAGE_LAYOUT_GENERAL,
			.srcQueueFamilyIndex = acquire ? VK_QUEUE_FAMILY_FOREIGN_EXT : vk->qfam,
			.dstQueueFamilyIndex = acquire ? vk->qfam : VK_QUEUE_FAMILY_FOREIGN_EXT,
			.image = texture->image,
		});
}

// The dma-bufs of the surfaces in the list. Texture nodes never show
// dma-bufs.
static void
transition_render_list(VkCommandBuffer cmd, const struct render_list *list,
	bool acquire)
{
	struct render_entry *entry;
	wl_array_for_each(entry, &list->entries) {
		if (entry->node->type == SCENE_NODE_SURFACE) {
			struct surface *surface =
				wl_container_of(entry->node, surface, scene.node);
			transition_dmabuf_texture(cmd, surface_texture(surface), acquire);
		}
	}
}

// The dma-bufs drawn on the output: the surfaces in the list, the drag icon
// and the cursor of the client.
static void
transition_dmabufs(VkCommandBuffer cmd, const struct render_list *list, bool acquire)
{
	transition_render_list(cmd, list, acquire);
	struct surface *drag_icon = pointer_drag_icon_surface();
	struct surface *client_cursor = pointer_client_cursor_surface();
	if (drag_icon) {
		transition_dmabuf_texture(cmd, surface_texture(drag_icon), acquire);
	}
	if (client_cursor) {
		transition_dmabuf_texture(cmd, surface_texture(client_cursor), acquire);
	}
}

// Starts drawing into the damage of the pass, and keeps the rest of the
// image.
static void
begin_rendering(const struct render_pass *pass)
{
	struct box image = {.width = pass->width, .height = pass->height};
	box_transform(&image, &image, transform_invert(pass->transform),
		pass->width, pass->height);
	struct box extents = box_from_pixman(pixman_region32_extents(pass->damage));
	VkRenderingAttachmentInfo color = {
		.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
		.imageView = pass->view,
		.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
	};
	VkRenderingInfo rendering = {
		.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
		.renderArea = image_rect(pass, &extents),
		.layerCount = 1,
		.colorAttachmentCount = 1,
		.pColorAttachments = &color,
	};
	vkCmdBeginRendering(pass->cmd, &rendering);
	VkViewport viewport = {0, 0, (float)image.width, (float)image.height,
		0, 1};
	vkCmdSetViewport(pass->cmd, 0, 1, &viewport);
}

// Clears the damage of the pass where it is uncovered, with the sRGB color.
static void
clear_uncovered(const struct render_pass *pass, struct color clear,
	const pixman_region32_t *uncovered)
{
	clear = color_convert(clear, pass->description);
	VkClearAttachment attachment = {
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.clearValue = {.color = {.float32 = {clear.r, clear.g, clear.b,
			clear.a}}},
	};
	pixman_region32_t background;
	pixman_region32_init(&background);
	uncovered_damage(pass, &background, uncovered,
		&(struct box){.width = pass->width, .height = pass->height});
	int nr_rects;
	const pixman_box32_t *rects = pixman_region32_rectangles(&background, &nr_rects);
	for (int i = 0; i < nr_rects; i++) {
		struct box rect = box_from_pixman(&rects[i]);
		VkClearRect clear_rect = {
			.rect = image_rect(pass, &rect),
			.layerCount = 1,
		};
		vkCmdClearAttachments(pass->cmd, 1, &attachment, 1, &clear_rect);
	}
	pixman_region32_fini(&background);
}

// Draws the damage of the pass on the output, with the nodes of its list, and
// the cursor if requested. The drag icon is always drawn.
static void
draw_output(const struct render_pass *pass, struct output *output,
	const struct render_list *list, bool cursor)
{
	transition_dmabufs(pass->cmd, list, true);
	begin_rendering(pass);
	clear_uncovered(pass, config.desktop.background_color, &list->uncovered);
	draw_render_list(pass, list);
	struct surface *drag_icon = pointer_drag_icon_surface();
	if (drag_icon) {
		draw_drag_icon(pass, output, drag_icon);
	}
	if (cursor) {
		struct box box = cursor_box(output);
		draw_cursor(pass, output, &box);
	}
	vkCmdEndRendering(pass->cmd);
	transition_dmabufs(pass->cmd, list, false);
}

// Draws the surfaces of the window, without its decoration, into the damage
// of the pass, which is in VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL. The
// top-left corner of the window geometry is at the origin of the image, and
// the rest of the damage is cleared.
static void
draw_window_surfaces(const struct render_pass *pass, struct window *window)
{
	assert(window->surface);
	// The surface tree is at the top-left corner of the window geometry.
	struct scene_node *surfaces = &window->surface->scene.tree.node;
	struct render_list list;
	render_list_init(&list, pass->width, pass->height, pass->scale);
	add_scene_node(&list, surfaces, 0, 0, surfaces->x, surfaces->y);
	send_frame_callbacks(&list, pass->time);
	transition_render_list(pass->cmd, &list, true);
	begin_rendering(pass);
	clear_uncovered(pass, (struct color){0}, &list.uncovered);
	draw_render_list(pass, &list);
	vkCmdEndRendering(pass->cmd);
	transition_render_list(pass->cmd, &list, false);
	render_list_fini(&list);
}

// Prepares the texture to be drawn into, discarding its contents. It may
// still be sampled by earlier submissions.
static void
begin_texture(VkCommandBuffer cmd, struct texture *texture)
{
	vulkan_image_barrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, (VkImageMemoryBarrier){
			.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = texture->image,
		});
}

// Prepares the texture drawn into to be sampled.
static void
end_texture(VkCommandBuffer cmd, struct texture *texture)
{
	vulkan_image_barrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, (VkImageMemoryBarrier){
			.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = texture->image,
		});
}

// Captures the window into the texture of the thumbnail, scaled to fit the
// window geometry in it.
static void
draw_thumbnail_texture(VkCommandBuffer cmd, struct scene_thumbnail *thumbnail,
	uint32_t time)
{
	struct texture *texture = thumbnail->texture;
	struct box geometry = thumbnail->window->current.geometry;
	assert(!box_empty(&geometry));
	pixman_region32_t damage;
	pixman_region32_init_rect(&damage, 0, 0, texture->width, texture->height);
	struct render_pass pass = {
		.cmd = cmd,
		.image = texture->image,
		.view = texture->view,
		.format = texture->format,
		.description = &srgb_description,
		.width = (int)texture->width,
		.height = (int)texture->height,
		.scale = MIN((double)texture->width / geometry.width,
			(double)texture->height / geometry.height),
		.damage = &damage,
		.time = time,
	};
	begin_texture(cmd, texture);
	draw_window_surfaces(&pass, thumbnail->window);
	end_texture(cmd, texture);
	pixman_region32_fini(&damage);
}

// Draws the surfaces into the textures of the outdated thumbnails in the
// subtree, which are then drawn by any output.
static void
update_thumbnails(VkCommandBuffer cmd, struct scene_node *node, uint32_t time)
{
	if (!node->enabled) {
		return;
	}
	switch (node->type) {
	case SCENE_NODE_TREE: {
		struct scene_tree *tree = wl_container_of(node, tree, node);
		struct scene_node *child;
		wl_list_for_each(child, &tree->children, link) {
			update_thumbnails(cmd, child, time);
		}
		return;
	}
	case SCENE_NODE_THUMBNAIL: {
		struct scene_thumbnail *thumbnail = wl_container_of(node, thumbnail, node);
		if (thumbnail->outdated) {
			draw_thumbnail_texture(cmd, thumbnail, time);
			thumbnail->outdated = false;
		}
		return;
	}
	case SCENE_NODE_SURFACE:
	case SCENE_NODE_RECT:
	case SCENE_NODE_SHADOW:
	case SCENE_NODE_TEXTURE:
		return;
	}
}

// Starts recording the commands to draw into the image, acquired from KMS.
static void
begin_image(struct swapchain_image *image)
{
	struct vk *vk = &core.vk;
	if (image->initialized) {
		assert(vkWaitForFences(vk->dev, 1, &image->render_fence,
			VK_TRUE, UINT64_MAX) == VK_SUCCESS);
		assert(vkResetFences(vk->dev, 1, &image->render_fence) == VK_SUCCESS);
	}
	assert(vkResetCommandBuffer(image->cmd, 0) == VK_SUCCESS);
	VkCommandBufferBeginInfo begin = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	};
	assert(vkBeginCommandBuffer(image->cmd, &begin) == VK_SUCCESS);
	vulkan_image_barrier(image->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, (VkImageMemoryBarrier){
			.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT,
			.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			.oldLayout = image->initialized ? VK_IMAGE_LAYOUT_GENERAL
				: VK_IMAGE_LAYOUT_UNDEFINED,
			.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
			.dstQueueFamilyIndex = vk->qfam,
			.image = image->vk_image,
		});
}

// Releases the image to KMS, and submits the commands. The sync file of the
// image signals once the GPU finishes them.
static void
end_image(struct swapchain_image *image)
{
	struct vk *vk = &core.vk;
	vulkan_image_barrier(image->cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, (VkImageMemoryBarrier){
			.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			.newLayout = VK_IMAGE_LAYOUT_GENERAL,
			.srcQueueFamilyIndex = vk->qfam,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
			.image = image->vk_image,
		});
	assert(vkEndCommandBuffer(image->cmd) == VK_SUCCESS);
	vulkan_submit(image->cmd, image->render_fence);
	image->render_fd = vulkan_dup_sync_file();
	image->initialized = true;
}

// The texture of the magnifier, of the size in the pixels of the image and its
// format. It is sampled with the nearest filter, so that each pixel of the
// lens is shown as a block of pixels rather than blurred.
static struct texture *
magnifier_texture(uint32_t width, uint32_t height, const struct pixel_format *format)
{
	struct texture *texture = core.magnifier.texture;
	if (!texture || texture->width != width || texture->height != height
			|| texture->format != format) {
		texture_destroy(texture);
		texture = texture_create(width, height, format->drm);
		texture_set_sampler(texture, core.vk.nearest_sampler);
		core.magnifier.texture = texture;
	}
	return texture;
}

// Copies the lens of the image, which shows the scene there, into the texture
// of the magnifier, in the orientation of the image like a surface with the
// transform of the output.
static void
copy_lens(VkCommandBuffer cmd, VkImage image, struct texture *texture, VkRect2D rect)
{
	vulkan_image_barrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, (VkImageMemoryBarrier){
			.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = image,
		});
	// The texture may still be sampled by earlier submissions.
	vulkan_image_barrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, (VkImageMemoryBarrier){
			.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = texture->image,
		});
	VkImageCopy copy = {
		.srcSubresource = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.layerCount = 1,
		},
		.srcOffset = {rect.offset.x, rect.offset.y, 0},
		.dstSubresource = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.layerCount = 1,
		},
		.extent = {rect.extent.width, rect.extent.height, 1},
	};
	vkCmdCopyImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		texture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
	vulkan_image_barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, (VkImageMemoryBarrier){
			.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT
				| VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = image,
		});
	vulkan_image_barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, (VkImageMemoryBarrier){
			.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = texture->image,
		});
}

// Draws the lens back magnified, with the frame around it. The pointer is the
// fixed point of the magnification, so that it is over what it shows under it.
static void
draw_magnifier(const struct render_pass *pass, struct output *output,
	const struct box *lens)
{
	VkRect2D rect = image_rect(pass, lens);
	struct texture *texture = magnifier_texture(rect.extent.width,
		rect.extent.height, pass->format);
	copy_lens(pass->cmd, pass->image, texture, rect);

	struct box frame = magnifier_frame(output, lens);
	pixman_region32_t damage;
	pixman_region32_init_rect(&damage, frame.x, frame.y,
		(unsigned)frame.width, (unsigned)frame.height);
	pixman_region32_intersect_rect(&damage, &damage, 0, 0,
		(unsigned)pass->width, (unsigned)pass->height);
	struct render_pass lens_pass = *pass;
	lens_pass.damage = &damage;
	begin_rendering(&lens_pass);
	if (!config.magnifier.fullscreen) {
		draw_solid_box(&lens_pass, &frame, &damage,
			(struct color){0.0f, 0.0f, 0.0f, 1.0f}, 0.0f, 0,
			(float)(lens->x - frame.x));
	}
	struct pointer *pointer = &core.seat.pointer;
	double x = ((pointer->x - output->box.x) * output->scale - lens->x)
		/ lens->width;
	double y = ((pointer->y - output->box.y) * output->scale - lens->y)
		/ lens->height;
	double size = 1.0 / core.magnifier.zoom;
	struct fbox source = {
		.x = x - x * size,
		.y = y - y * size,
		.width = size,
		.height = size,
	};
	draw_texture(&lens_pass, texture, pass->description, lens, &damage,
		output->transform, &source, true);
	vkCmdEndRendering(pass->cmd);
	pixman_region32_fini(&damage);
}

// The linear colors of the HDR output, with its reference white at 1.0, which
// its blend buffer blends.
static struct image_description
blend_description(struct output *output)
{
	const struct image_description *description = output->image_description;
	return (struct image_description){
		.primaries = description->primaries,
		.tf_named = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR,
		.max_luminance = description->reference_luminance,
		.reference_luminance = description->reference_luminance,
	};
}

// Draws the damage of the image, or the whole image until it is initialized,
// with the nodes of the list of the output, and the magnifier if the output
// shows it. The cursor is drawn in unless the cursor plane can show it. HDR
// outputs blend the colors in their blend buffers first.
void
draw_swapchain_image(struct output *output, struct swapchain_image *image,
	const struct render_list *list)
{
	struct box physical = output_physical_box(output);
	pixman_region32_t *damage = &image->damage;
	bool magnified = output_magnified(output);
	struct box lens = {0};
	if (magnified) {
		struct pointer *pointer = &core.seat.pointer;
		lens = magnifier_lens(output, pointer->x, pointer->y);
		// The image shows the lens magnified, but the magnifier copies
		// the scene from it.
		pixman_region32_union_rect(damage, damage, lens.x, lens.y,
			(unsigned)lens.width, (unsigned)lens.height);
	}
	if (!image->initialized) {
		pixman_region32_reset(damage, &(pixman_box32_t){
			.x2 = physical.width,
			.y2 = physical.height,
		});
	} else if (pixman_region32_n_rects(damage) > MAX_DAMAGE_RECTS) {
		pixman_box32_t extents = *pixman_region32_extents(damage);
		pixman_region32_reset(damage, &extents);
	}
	assert(pixman_region32_not_empty(damage));
	begin_image(image);
	struct render_pass pass = {
		.cmd = image->cmd,
		.image = image->vk_image,
		.view = image->view,
		.format = pixel_format_find(gbm_bo_get_format(image->bo)),
		.description = output->image_description,
		.width = physical.width,
		.height = physical.height,
		.scale = output->scale,
		.transform = output->transform,
		.damage = damage,
		.time = msec_now(),
	};
	update_thumbnails(image->cmd, &core.scene.tree.node, pass.time);
	image->cursor_drawn = !cursor_fits_plane(output);
	// With HDR, the scene is drawn into the blend buffer, which blends the
	// colors linearly, and then encoded into the image.
	struct texture *blend_buffer = output->blend_buffer;
	struct image_description blend = blend_description(output);
	struct render_pass scene_pass = pass;
	if (blend_buffer) {
		scene_pass.image = blend_buffer->image;
		scene_pass.view = blend_buffer->view;
		scene_pass.format = blend_buffer->format;
		scene_pass.description = &blend;
		begin_texture(image->cmd, blend_buffer);
	}
	draw_output(&scene_pass, output, list, image->cursor_drawn);
	if (magnified) {
		draw_magnifier(&scene_pass, output, &lens);
	}
	if (blend_buffer) {
		end_texture(image->cmd, blend_buffer);
		begin_rendering(&pass);
		draw_texture(&pass, blend_buffer, &blend, &physical, damage,
			output->transform, &whole_texture, true);
		vkCmdEndRendering(image->cmd);
	}
	end_image(image);
	pixman_region32_clear(damage);
}

// The surface whose buffer the primary plane can show instead of an image of
// the swapchain, if KMS can scan it out, or NULL if there is none. It has to
// be the only node in the list of the output, covering it opaquely with the
// whole buffer, unscaled, in the orientation and the colors of the output.
// Neither the cursor, the drag icon nor the magnifier can be drawn over it.
struct surface *
scanout_surface(struct output *output, const struct render_list *list)
{
	struct box physical = output_physical_box(output);
	struct box cursor = cursor_box(output);
	if (pointer_drag_icon_surface() || output_magnified(output)
			|| (!cursor_fits_plane(output)
				&& box_intersects(&cursor, &physical))) {
		return NULL;
	}
	const struct render_entry *entry = list->entries.data;
	if (list->entries.size != sizeof(*entry)
			|| pixman_region32_not_empty(&list->uncovered)
			|| entry->node->type != SCENE_NODE_SURFACE
			|| !box_equal(&entry->box, &physical)) {
		return NULL;
	}
	struct surface *surface = wl_container_of(entry->node, surface, scene.node);
	struct color_transform color;
	color_transform_init(&color, surface_image_description(surface),
		output->image_description);
	if (surface->current.transform != output->transform || !color.identity) {
		return NULL;
	}
	// The whole buffer is shown, and has the size of the mode, as the
	// output does in its orientation. Surfaces without buffers cover
	// nothing.
	struct client_buffer *buffer = surface->current.buffer;
	struct fbox source = surface_source_box(surface);
	bool unscaled = buffer->width == output->mode->hdisplay
		&& buffer->height == output->mode->vdisplay
		&& source.x == 0 && source.y == 0
		&& source.width == physical.width
		&& source.height == physical.height;
	return unscaled ? surface : NULL;
}

// Draws the cursor into the next image of the cursor plane, at its top-left
// corner in the orientation of the layout.
static void
draw_cursor_image(struct output *output, const struct box *box)
{
	struct cursor_plane *cursor = &output->cursor;
	cursor->current = (cursor->current + 1) % NR_CURSOR_IMAGES;
	struct swapchain_image *image = &cursor->images[cursor->current];
	// Kept for the commits while the image is current.
	if (image->render_fd >= 0) {
		close(image->render_fd);
	}
	begin_image(image);
	struct box size = cursor_plane_size(output);
	pixman_region32_t damage;
	pixman_region32_init_rect(&damage, 0, 0, (unsigned)size.width,
		(unsigned)size.height);
	struct render_pass pass = {
		.cmd = image->cmd,
		.image = image->vk_image,
		.view = image->view,
		.format = pixel_format_find(gbm_bo_get_format(image->bo)),
		.description = output->image_description,
		.width = size.width,
		.height = size.height,
		.scale = output->scale,
		.transform = output->transform,
		.damage = &damage,
		.time = msec_now(),
	};
	struct surface *client_cursor = pointer_client_cursor_surface();
	struct texture *texture = client_cursor ? surface_texture(client_cursor) : NULL;
	transition_dmabuf_texture(image->cmd, texture, true);
	begin_rendering(&pass);
	clear_uncovered(&pass, (struct color){0}, &damage);
	draw_cursor(&pass, output, &(struct box){
		.width = box->width,
		.height = box->height,
	});
	vkCmdEndRendering(image->cmd);
	transition_dmabuf_texture(image->cmd, texture, false);
	end_image(image);
	pixman_region32_fini(&damage);
}

// Sets what the cursor plane shows with the frame in the next commit: the
// cursor if it is on the output and not drawn into the image of the frame.
// The image of the cursor is drawn again if it is outdated.
void
update_cursor_plane(struct output *output, const struct frame *frame)
{
	struct cursor_plane *cursor = &output->cursor;
	struct box box = cursor_box(output);
	struct box plane = cursor_plane_size(output);
	plane.x = box.x;
	plane.y = box.y;
	struct box output_box = output_physical_box(output);
	// The cursor may not fit anymore if the image was drawn ahead.
	bool drawn = frame->image && frame->image->cursor_drawn;
	cursor->visible = !drawn && cursor_fits_plane(output)
		&& !box_empty(&box) && box_intersects(&plane, &output_box);
	if (!cursor->visible) {
		return;
	}
	if (cursor->outdated) {
		draw_cursor_image(output, &box);
		cursor->outdated = false;
	}
	box_transform(&cursor->box, &plane, transform_invert(output->transform),
		output_box.width, output_box.height);
}

void
renderer_capture_init(struct capture_session *session)
{
	struct vk *vk = &core.vk;
	VkCommandBufferAllocateInfo cbai = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = vk->cmd_pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	assert(vkAllocateCommandBuffers(vk->dev, &cbai, &session->render.cmd)
		== VK_SUCCESS);
}

// Waits for the GPU to finish the last capture of the session, which its sync
// file has usually signaled already.
static void
wait_capture(struct capture_session *session)
{
	VkSemaphoreWaitInfo info = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
		.semaphoreCount = 1,
		.pSemaphores = &core.vk.timeline,
		.pValues = &session->render.copy_point,
	};
	assert(vkWaitSemaphores(core.vk.dev, &info, UINT64_MAX) == VK_SUCCESS);
}

static void
destroy_readback(struct capture_session *session)
{
	struct vk *vk = &core.vk;
	texture_destroy(session->render.texture);
	vkDestroyBuffer(vk->dev, session->render.readback, NULL);
	vkFreeMemory(vk->dev, session->render.readback_memory, NULL);
	session->render.texture = NULL;
	session->render.readback = VK_NULL_HANDLE;
	session->render.readback_memory = VK_NULL_HANDLE;
	session->render.readback_pixels = NULL;
}

void
renderer_capture_fini(struct capture_session *session)
{
	struct vk *vk = &core.vk;
	wait_capture(session);
	destroy_readback(session);
	vkFreeCommandBuffers(vk->dev, vk->cmd_pool, 1, &session->render.cmd);
}

// The texture that frames for shm buffers are drawn into, and read back from
// through a host-visible buffer. Both are recreated for a new buffer size.
static struct texture *
readback_texture(struct capture_session *session)
{
	struct vk *vk = &core.vk;
	uint32_t width = (uint32_t)session->width;
	uint32_t height = (uint32_t)session->height;
	struct texture *texture = session->render.texture;
	if (texture && texture->width == width && texture->height == height) {
		return texture;
	}
	destroy_readback(session);
	session->render.texture = texture_create(width, height, DRM_FORMAT_ARGB8888);
	VkDeviceSize size = (VkDeviceSize)width * height * 4;
	// The CPU reads the pixels, which is slow from uncached memory.
	vulkan_create_buffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
			| VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
			| VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
		&session->render.readback, &session->render.readback_memory);
	void *pixels;
	assert(vkMapMemory(vk->dev, session->render.readback_memory, 0, size, 0,
		&pixels) == VK_SUCCESS);
	session->render.readback_pixels = pixels;
	return session->render.texture;
}

// The texture to draw the frame for the buffer into, or NULL if the buffer
// does not match the constraints sent for the session.
static struct texture *
capture_target(struct capture_session *session, struct wl_resource *buffer)
{
	struct wl_shm_buffer *shm = wl_shm_buffer_get(buffer);
	if (shm) {
		uint32_t format = wl_shm_buffer_get_format(shm);
		bool valid = (format == WL_SHM_FORMAT_XRGB8888
				|| format == WL_SHM_FORMAT_ARGB8888)
			&& wl_shm_buffer_get_width(shm) == session->width
			&& wl_shm_buffer_get_height(shm) == session->height
			&& wl_shm_buffer_get_stride(shm) >= session->width * 4;
		return valid ? readback_texture(session) : NULL;
	}
	// Client dma-bufs of the 8-bit formats are drawn into directly.
	struct texture *texture = client_buffer_from_resource(buffer)->texture;
	bool valid = texture->renderable
		&& texture->format->vk == VK_FORMAT_B8G8R8A8_UNORM
		&& texture->width == (uint32_t)session->width
		&& texture->height == (uint32_t)session->height;
	return valid ? texture : NULL;
}

// Copies the texture that the frame was drawn into to the host-visible buffer.
static void
record_readback(VkCommandBuffer cmd, struct capture_session *session)
{
	struct texture *texture = session->render.texture;
	vulkan_image_barrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, (VkImageMemoryBarrier){
			.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = texture->image,
		});
	VkBufferImageCopy copy = {
		.imageSubresource = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.layerCount = 1,
		},
		.imageExtent = {texture->width, texture->height, 1},
	};
	vkCmdCopyImageToBuffer(cmd, texture->image,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, session->render.readback, 1, &copy);
	VkBufferMemoryBarrier barrier = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_HOST_READ_BIT,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.buffer = session->render.readback,
		.size = VK_WHOLE_SIZE,
	};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &barrier, 0, NULL);
}

// Draws the source of the session into the buffer, at the scale of the output
// showing it. Returns a sync file that signals once the GPU is done, after
// which renderer_capture_read() copies the frame into shm buffers. Returns -1
// if the buffer does not match the constraints of the session.
int
renderer_capture(struct capture_session *session, struct wl_resource *buffer)
{
	struct vk *vk = &core.vk;
	// The resources of the last capture are reused.
	wait_capture(session);
	struct texture *target = capture_target(session, buffer);
	if (!target) {
		return -1;
	}

	VkCommandBuffer cmd = session->render.cmd;
	assert(vkResetCommandBuffer(cmd, 0) == VK_SUCCESS);
	VkCommandBufferBeginInfo begin = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	assert(vkBeginCommandBuffer(cmd, &begin) == VK_SUCCESS);
	struct output *output = session->window ? session->window->output
		: session->output;
	// The whole buffer is drawn, as the client may have changed it.
	pixman_region32_t damage;
	pixman_region32_init_rect(&damage, 0, 0, (unsigned)session->width,
		(unsigned)session->height);
	struct render_pass pass = {
		.cmd = cmd,
		.image = target->image,
		.view = target->view,
		.format = target->format,
		.description = &srgb_description,
		.width = session->width,
		.height = session->height,
		.scale = output->scale,
		.damage = &damage,
		.time = msec_now(),
	};
	// A dma-buf is acquired from its client, and its contents are
	// discarded, like those of the texture.
	vulkan_image_barrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, (VkImageMemoryBarrier){
			.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			.srcQueueFamilyIndex = target->dmabuf
				? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = target->dmabuf
				? vk->qfam : VK_QUEUE_FAMILY_IGNORED,
			.image = target->image,
		});
	if (session->window) {
		draw_window_surfaces(&pass, session->window);
	} else {
		// The buffer has the size of the output.
		struct render_list list;
		render_list_init_output(&list, output);
		send_frame_callbacks(&list, pass.time);
		update_thumbnails(cmd, &core.scene.tree.node, pass.time);
		draw_output(&pass, output, &list, session->paint_cursors);
		render_list_fini(&list);
	}
	pixman_region32_fini(&damage);
	if (target->dmabuf) {
		vulkan_image_barrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, (VkImageMemoryBarrier){
				.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
				.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				.newLayout = VK_IMAGE_LAYOUT_GENERAL,
				.srcQueueFamilyIndex = vk->qfam,
				.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
				.image = target->image,
			});
	} else {
		record_readback(cmd, session);
	}
	assert(vkEndCommandBuffer(cmd) == VK_SUCCESS);

	vulkan_submit(cmd, VK_NULL_HANDLE);
	session->render.copy_point = vk->timeline_point;
	return vulkan_dup_sync_file();
}

// Copies the last frame drawn for an shm buffer into it.
void
renderer_capture_read(struct capture_session *session, struct wl_shm_buffer *shm)
{
	wait_capture(session);
	struct texture *texture = session->render.texture;
	size_t row_size = (size_t)texture->width * 4;
	size_t stride = (size_t)wl_shm_buffer_get_stride(shm);
	wl_shm_buffer_begin_access(shm);
	uint8_t *pixels = wl_shm_buffer_get_data(shm);
	for (uint32_t y = 0; y < texture->height; y++) {
		memcpy(pixels + y * stride,
			session->render.readback_pixels + y * row_size, row_size);
	}
	wl_shm_buffer_end_access(shm);
}

void
renderer_fini(void)
{
	struct output *output;
	wl_list_for_each(output, &core.outputs, link) {
		server_cursors_destroy(output->server_cursors);
		decoration_icons_destroy(output);
	}
	texture_destroy(core.magnifier.texture);
	texture_uploads_submit();
	renderer_wait_idle();
	textures_collect_garbage();
	assert(wl_list_empty(&core.vk.texture_garbage));
	texture_uploads_fini();
	text_fini();
	repaint_fini();
	vulkan_fini();
}
