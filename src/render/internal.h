#ifndef JIMI_RENDER_INTERNAL_H
#define JIMI_RENDER_INTERNAL_H

#include "color.h"
#include "core.h"
#include "image_capture.h"
#include "output.h"
#include "surface.h"

// The texture, or the luma and the chroma planes of YCbCr
#define TEXTURE_BINDINGS 2

struct texture {
	VkImage image;
	VkDeviceMemory memories[MAX_DMABUF_PLANES];
	uint32_t nr_memories;
	VkImageView view; // Of the luma plane of YCbCr
	VkImageView chroma_view; // Of YCbCr, or VK_NULL_HANDLE
	VkSampler sampler; // vk.sampler unless the owner picks another one
	uint32_t width;
	uint32_t height;
	const struct pixel_format *format;
	bool dmabuf;
	bool renderable; // A dma-buf that can be drawn into.
	bool y_invert;
	struct wl_list link; // vk.texture_garbage after texture_destroy()
	uint64_t release_point; // The timeline point after which it is unused.
};

struct texture_push_constants {
	float output_size[4];
	float ndc_transform[4]; // The rows of a 2x2 matrix
	float dst[4];
	struct matrix uv_transform;
	// The texture is YCbCr, which the affine matrix converts into RGB.
	uint32_t ycbcr;
	uint32_t padding[3];
	float ycbcr_matrix[3][4]; // The rows
	// Of struct color_transform, unless the colors are kept. The rows are
	// padded to 4 floats.
	float color_matrix[3][4];
	uint32_t src_tf; // enum color_tf
	float src_power;
	uint32_t dst_tf;
	float dst_power;
	uint32_t convert; // Otherwise, the colors are kept.
	uint32_t opaque; // The alpha channel is ignored.
};

struct solid_push_constants {
	float output_size[4];
	float ndc_transform[4]; // The rows of a 2x2 matrix
	int32_t dst[4];
	float color[4];
	float radius;
	uint32_t corner_mask; // enum corner
	float border_width; // 0 fills the box.
};

struct shadow_push_constants {
	float output_size[4];
	float ndc_transform[4]; // The rows of a 2x2 matrix
	int32_t dst[4];
	float color[4];
	int32_t box[4]; // Casting the shadow, relative to dst
	int32_t offset[2]; // Of the shadow from the box
	float radius;
	uint32_t corner_mask; // enum corner
	float sigma; // Of the gaussian blur
};

// A leaf node of the scene shown in the image.
struct render_entry {
	struct scene_node *node;
	struct box box; // In pixels of the image
	// The pixels of the box not covered by the opaque nodes above.
	pixman_region32_t visible;
};

// The nodes shown in an image, top-to-bottom. The nodes entirely covered by
// opaque nodes above are left out, and their surfaces get no frame callbacks
// until they show again.
struct render_list {
	struct wl_array entries; // struct render_entry
	// The pixels of the image not covered by any opaque node, where the
	// background shows.
	pixman_region32_t uncovered;
	double scale; // Pixels of the image per logical pixel
};

static_assert(sizeof(struct solid_push_constants) == 76,
	"solid push constants must match solid shaders");
static_assert(sizeof(struct shadow_push_constants) == 100,
	"shadow push constants must match shadow shaders");
static_assert(sizeof(struct texture_push_constants) == 248,
	"texture push constants must match texture shaders");

uint32_t vulkan_find_memory_type(VkPhysicalDevice phys, uint32_t bits,
	VkMemoryPropertyFlags flags);
void vulkan_create_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
	VkMemoryPropertyFlags flags, VkBuffer *buffer, VkDeviceMemory *memory);
void vulkan_submit(VkCommandBuffer cmd, VkFence fence);
int vulkan_dup_sync_file(void);
uint64_t renderer_completed_point(void);
VkImageView vulkan_create_image_view(VkImage image, VkFormat format,
	VkImageAspectFlags aspect);
const struct pipelines *vulkan_pipelines(VkFormat format);
void vulkan_image_barrier(VkCommandBuffer cmd, VkPipelineStageFlags src_stage,
	VkPipelineStageFlags dst_stage, VkImageMemoryBarrier barrier);

void vulkan_init(void);
void formats_init(void);
void formats_fini(void);
VkImageCreateFlags image_create_flags(const struct pixel_format *format);
VkImageFormatListCreateInfo image_format_list(const struct pixel_format *format);
bool modifier_importable(const struct pixel_format *format, uint64_t modifier,
	VkImageCreateFlags flags, VkImageUsageFlags usage);
void ycbcr_matrix(const struct pixel_format *format, double kr, double kb,
	float matrix[3][4]);
void vulkan_fini(void);
void swapchain_update(struct output *output);
void render_list_init_output(struct render_list *list, struct output *output);
void render_list_fini(struct render_list *list);
void send_frame_callbacks(const struct render_list *list, uint32_t time);
void take_presentation_feedbacks(const struct render_list *list,
	struct wl_list *feedbacks);
void draw_swapchain_image(struct output *output, struct swapchain_image *image,
	const struct render_list *list);
struct surface *scanout_surface(struct output *output,
	const struct render_list *list);
void update_cursor_plane(struct output *output, const struct frame *frame);
void frame_release(struct frame *frame);
void repaint_init(void);
void repaint_fini(void);
void text_fini(void);
VkCommandBuffer texture_uploads_end(uint64_t point);
void texture_uploads_submit(void);
void texture_uploads_fini(void);
void textures_collect_garbage(void);

#endif
