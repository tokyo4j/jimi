#ifndef JIMI_RENDER_H
#define JIMI_RENDER_H

#include <drm_fourcc.h>
#include <vulkan/vulkan.h>

#include "util.h"

#define NR_SWAPCHAIN_IMAGES 3
#define NR_CURSOR_IMAGES 2
#define MAX_DMABUF_PLANES 4
// Of the images of the swapchains, which SDR and HDR outputs show
#define SCANOUT_FORMAT DRM_FORMAT_XRGB8888
#define HDR_SCANOUT_FORMAT DRM_FORMAT_XRGB2101010
#define NR_PIXEL_FORMATS 12
// Of the images that we draw into: those of the swapchains of SDR and HDR
// outputs, and the blend buffers of HDR outputs
#define NR_RENDER_FORMATS 3

struct gbm_bo;
struct output;
struct plane;
struct texture;
struct texture_upload;
struct wl_shm_buffer;

// The formats of client buffers and of the images that we draw. Planes scan
// out those with alpha as the opaque formats, which ignore it.
struct pixel_format {
	uint32_t drm; // DRM_FORMAT_*
	// The DRM format without alpha, or 0 for YCbCr, which is not scanned
	// out.
	uint32_t opaque;
	uint32_t shm; // enum wl_shm_format
	VkFormat vk;
	uint32_t bytes; // Per pixel, of the first plane
	bool has_alpha;
	// Of the values of 2-plane YCbCr 4:2:0, in the most significant bits of
	// their bytes, or 0 for RGB. The planes are sampled by views of their
	// formats, the luma and then the chroma.
	uint32_t ycbcr_bits;
	VkFormat plane_formats[2];
};

struct dmabuf_modifier {
	uint64_t modifier;
	uint32_t nr_planes;
	bool renderable; // Imported dma-bufs can also be drawn into.
};

// What the renderer can do with client buffers of a pixel format
struct format_support {
	// Of client dma-bufs that we can sample from
	struct dmabuf_modifier *modifiers;
	uint32_t nr_modifiers;
	bool shm; // shm buffers can be copied into textures.
};

// Draw into images of the format.
struct pipelines {
	VkFormat format;
	VkPipeline texture;
	VkPipeline solid;
	VkPipeline shadow;
};

struct vk {
	VkInstance instance;
	VkPhysicalDevice phys;
	VkDevice dev;
	VkQueue queue;
	uint32_t qfam;
	VkCommandPool cmd_pool;
	// Being recorded, submitted with the next frame or other drawing.
	struct texture_upload *upload;
	struct wl_list uploads; // texture_upload.link, submitted, by point
	struct wl_list staging_buffers; // staging_buffer.link, unused
	// Of the textures, pushed with each draw
	VkDescriptorSetLayout descriptor_set_layout;
	VkSampler sampler; // Linear
	VkSampler nearest_sampler;
	VkPipelineLayout texture_pipe_layout;
	VkPipelineLayout solid_pipe_layout;
	VkPipelineLayout shadow_pipe_layout;
	struct pipelines pipelines[NR_RENDER_FORMATS];
	struct format_support formats[NR_PIXEL_FORMATS]; // By pixel_formats
	struct {
		PFN_vkGetMemoryFdPropertiesKHR vkGetMemoryFdPropertiesKHR;
		PFN_vkGetSemaphoreFdKHR vkGetSemaphoreFdKHR;
		PFN_vkCmdSetColorBlendEnableEXT vkCmdSetColorBlendEnableEXT;
		PFN_vkCmdPushDescriptorSetKHR vkCmdPushDescriptorSetKHR;
	} api;
	// Each queue submission signals the next point when it completes.
	VkSemaphore timeline;
	uint64_t timeline_point; // Of the last submission.
	// Each queue submission signals it, and exports it as sync_file.
	VkSemaphore submitted;
	// Signals once the last submission completes, along with the earlier
	// ones. -1 until the first submission.
	int sync_file;
	struct wl_list texture_garbage; // texture.link, by release point
};

struct dmabuf_attributes {
	int fds[MAX_DMABUF_PLANES];
	uint32_t offsets[MAX_DMABUF_PLANES];
	uint32_t strides[MAX_DMABUF_PLANES];
	uint32_t nr_planes;
	uint32_t width;
	uint32_t height;
	uint32_t format;
	uint64_t modifier;
	bool y_invert;
};

struct swapchain_image {
	struct gbm_bo *bo;
	uint32_t fb;
	uint64_t modifier;
	uint32_t nr_planes;
	VkImage vk_image;
	VkDeviceMemory memory;
	VkImageView view;
	VkCommandBuffer cmd;
	VkFence render_fence;
	// Signals once the GPU finishes drawing the image. Kept until the image
	// is on the screen, to tell when the GPU finished.
	int render_fd;
	int64_t render_start; // CLOCK_MONOTONIC ns
	bool initialized;
	// The cursor is drawn into the image, rather than shown on the cursor
	// plane.
	bool cursor_drawn;
	// The pixels that do not match the scene, in the orientation of the
	// layout. Not tracked until the image is initialized, as it is then
	// drawn whole.
	pixman_region32_t damage;
};

struct server_cursor {
	struct texture *texture;
	uint32_t width;
	uint32_t height;
	int32_t hotspot_x;
	int32_t hotspot_y;
};

extern const struct pixel_format pixel_formats[NR_PIXEL_FORMATS];

const struct pixel_format *pixel_format_find(uint32_t drm);
const struct pixel_format *pixel_format_find_shm(uint32_t shm);
const struct format_support *format_support(const struct pixel_format *format);
struct texture *texture_create(uint32_t width, uint32_t height, uint32_t format);
int text_width(const char *text, const char *font, double scale);
struct texture *texture_create_text(const char *text, const char *font,
	struct color color, int width, int height, double scale);
struct texture *texture_create_svg(const char *data, int width, int height,
	double scale, struct color color);
struct texture *texture_create_image_file(const char *path, int width,
	int height, double scale);
struct texture *texture_create_image_pixels(const uint32_t *pixels,
	int image_width, int image_height, int width, int height, double scale);
struct texture *texture_import_dmabuf(const struct dmabuf_attributes *attributes);
void texture_write_pixels(struct texture *texture, const uint8_t *pixels,
	uint32_t stride, const pixman_region32_t *region);
void texture_write_shm(struct texture **texture, struct wl_shm_buffer *shm,
	const pixman_region32_t *damage);
void texture_destroy(struct texture *texture);
void server_cursors_load(struct server_cursor cursors[], double scale);
void server_cursors_destroy(struct server_cursor cursors[]);
const struct dmabuf_modifier *dmabuf_modifier_find(uint32_t format,
	uint64_t modifier);

void damage_layout_box(const struct box *box);
void damage_cursor(const struct box *box, bool moved);
void damage_output(struct output *output);
void schedule_frames(uint32_t outputs);
void damage_magnifier(double x, double y);
void magnifier_set_zoom(double zoom);

void renderer_init(void);
void renderer_repaint_outputs(void);
void renderer_handle_page_flip(struct output *output, int64_t time, uint32_t seq);
void renderer_wait_idle(void);
void renderer_fini(void);
bool scanout_format_supported(const struct plane *plane, uint32_t format);
struct gbm_bo *scanout_bo_create(const struct plane *plane, uint32_t format,
	uint32_t width, uint32_t height);
void swapchain_destroy(struct output *output);

#endif
