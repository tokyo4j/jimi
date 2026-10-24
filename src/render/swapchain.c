#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <gbm.h>
#include <xf86drm.h>

#include "internal.h"

// The images are drawn into, and the magnifier copies from them.
#define SCANOUT_USAGE \
	(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)

static void
swapchain_image_init_vulkan(struct swapchain_image *image, VkFormat format,
	uint32_t w, uint32_t h, int dmabuf_fd, const VkSubresourceLayout *layouts)
{
	struct vk *vk = &core.vk;
	VkImageDrmFormatModifierExplicitCreateInfoEXT mod = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
		.drmFormatModifier = image->modifier,
		.drmFormatModifierPlaneCount = image->nr_planes,
		.pPlaneLayouts = layouts,
	};
	VkExternalMemoryImageCreateInfo ext = {
		.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
		.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
		.pNext = &mod,
	};
	VkImageCreateInfo ici = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.pNext = &ext,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = format,
		.extent = {w, h, 1},
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
		.usage = SCANOUT_USAGE,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	assert(vkCreateImage(vk->dev, &ici, NULL, &image->vk_image) == VK_SUCCESS);

	VkMemoryFdPropertiesKHR fd_props = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
	};
	assert(vk->api.vkGetMemoryFdPropertiesKHR(vk->dev,
		VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, dmabuf_fd,
		&fd_props) == VK_SUCCESS);

	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(vk->dev, image->vk_image, &req);
	VkMemoryDedicatedAllocateInfo dedicated = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
		.image = image->vk_image,
	};
	VkImportMemoryFdInfoKHR import = {
		.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
		.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
		.fd = dmabuf_fd,
		.pNext = &dedicated,
	};
	VkMemoryAllocateInfo mai = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.pNext = &import,
		.allocationSize = MAX(req.size, 1),
		.memoryTypeIndex = vulkan_find_memory_type(vk->phys,
			req.memoryTypeBits & fd_props.memoryTypeBits, 0),
	};
	assert(vkAllocateMemory(vk->dev, &mai, NULL, &image->memory) == VK_SUCCESS);
	assert(vkBindImageMemory(vk->dev, image->vk_image, image->memory, 0)
		== VK_SUCCESS);

	image->view = vulkan_create_image_view(image->vk_image, format,
		VK_IMAGE_ASPECT_COLOR_BIT);

	VkCommandBufferAllocateInfo cbai = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = vk->cmd_pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	assert(vkAllocateCommandBuffers(vk->dev, &cbai, &image->cmd) == VK_SUCCESS);
	VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	assert(vkCreateFence(vk->dev, &fci, NULL, &image->render_fence) == VK_SUCCESS);
}

// The modifiers of the format that the plane can scan out, and that we can
// also render to and copy from. The array has room for those of the plane.
static uint32_t
scanout_modifiers(const struct plane *plane, uint32_t format, uint64_t modifiers[])
{
	const struct pixel_format *pixel_format = pixel_format_find(format);
	uint32_t nr_modifiers = 0;
	for (uint32_t i = 0; i < plane->nr_formats; i++) {
		if (plane->formats[i].format == format && modifier_importable(pixel_format,
					plane->formats[i].modifier, 0, SCANOUT_USAGE)) {
			modifiers[nr_modifiers++] = plane->formats[i].modifier;
		}
	}
	return nr_modifiers;
}

// Whether we can draw the images for the plane to scan out in the format.
bool
scanout_format_supported(const struct plane *plane, uint32_t format)
{
	uint64_t modifiers[plane->nr_formats];
	return scanout_modifiers(plane, format, modifiers) > 0;
}

// A buffer object of the format and the size for the plane to scan out, with
// one of its modifiers that we can also render to and copy from, or NULL if
// there is none.
struct gbm_bo *
scanout_bo_create(const struct plane *plane, uint32_t format, uint32_t width,
	uint32_t height)
{
	uint64_t modifiers[plane->nr_formats];
	uint32_t nr_modifiers = scanout_modifiers(plane, format, modifiers);
	if (nr_modifiers == 0) {
		return NULL;
	}
	struct gbm_bo *bo = gbm_bo_create_with_modifiers(core.gbm, width, height,
		format, modifiers, nr_modifiers);
	assert(bo);
	return bo;
}

// Takes the buffer object to draw into and scan out.
static void
swapchain_image_init(struct swapchain_image *image, struct gbm_bo *bo)
{
	uint32_t w = gbm_bo_get_width(bo);
	uint32_t h = gbm_bo_get_height(bo);
	*image = (struct swapchain_image){0};
	image->render_fd = -1;
	pixman_region32_init(&image->damage);

	image->bo = bo;
	image->modifier = gbm_bo_get_modifier(image->bo);
	image->nr_planes = gbm_bo_get_plane_count(image->bo);
	assert(image->nr_planes > 0 && image->nr_planes <= MAX_DMABUF_PLANES);
	image->fb = kms_add_fb(image->bo);
	assert(image->fb);

	VkSubresourceLayout layouts[MAX_DMABUF_PLANES] = {0};
	for (uint32_t i = 0; i < image->nr_planes; i++) {
		layouts[i].offset = gbm_bo_get_offset(image->bo, i);
		layouts[i].rowPitch = gbm_bo_get_stride_for_plane(image->bo, i);
	}
	int dmabuf_fd = -1;
	assert(drmPrimeHandleToFD(core.kms.fd,
		gbm_bo_get_handle_for_plane(image->bo, 0).u32, DRM_CLOEXEC,
		&dmabuf_fd) == 0);
	swapchain_image_init_vulkan(image,
		pixel_format_find(gbm_bo_get_format(bo))->vk, w, h, dmabuf_fd, layouts);
}

static void
swapchain_image_fini(struct swapchain_image *image)
{
	struct vk *vk = &core.vk;
	if (!image->bo) {
		return;
	}
	if (image->render_fd >= 0) {
		close(image->render_fd);
	}
	vkDestroyFence(vk->dev, image->render_fence, NULL);
	vkFreeCommandBuffers(vk->dev, vk->cmd_pool, 1, &image->cmd);
	drmModeRmFB(core.kms.fd, image->fb);
	vkDestroyImageView(vk->dev, image->view, NULL);
	vkDestroyImage(vk->dev, image->vk_image, NULL);
	vkFreeMemory(vk->dev, image->memory, NULL);
	gbm_bo_destroy(image->bo);
	pixman_region32_fini(&image->damage);
	*image = (struct swapchain_image){0};
}

// The frames of the output are dropped along with the images.
static void
swapchain_fini(struct output *output)
{
	frame_release(&output->current);
	frame_release(&output->pending);
	frame_release(&output->queued);
	for (uint32_t i = 0; i < NR_SWAPCHAIN_IMAGES; i++) {
		swapchain_image_fini(&output->swapchain[i]);
	}
	texture_destroy(output->blend_buffer);
	output->blend_buffer = NULL;
	struct cursor_plane *cursor = &output->cursor;
	for (uint32_t i = 0; i < NR_CURSOR_IMAGES; i++) {
		swapchain_image_fini(&cursor->images[i]);
	}
	cursor->current = -1;
	cursor->visible = false;
}

// The cursor plane is given up if we cannot draw for it.
static void
cursor_images_init(struct output *output)
{
	struct cursor_plane *cursor = &output->cursor;
	if (!cursor->plane.id) {
		return;
	}
	for (uint32_t i = 0; i < NR_CURSOR_IMAGES; i++) {
		struct gbm_bo *bo = scanout_bo_create(&cursor->plane,
			DRM_FORMAT_ARGB8888, core.kms.cursor_width, core.kms.cursor_height);
		if (!bo) {
			cursor->plane.id = 0;
			return;
		}
		swapchain_image_init(&cursor->images[i], bo);
	}
	cursor->outdated = true;
}

// Creates the images for the size of the mode and for SDR or HDR, unless
// they are for them already, along with those of the cursor plane and the
// blend buffer of HDR. Called only before modesets, while no page flip is
// pending. Removing the framebuffers on the screen turns the output off until
// the modeset.
void
swapchain_update(struct output *output)
{
	uint32_t width = output->mode->hdisplay;
	uint32_t height = output->mode->vdisplay;
	uint32_t format = output->hdr ? HDR_SCANOUT_FORMAT : SCANOUT_FORMAT;
	struct gbm_bo *old = output->swapchain[0].bo;
	if (old && gbm_bo_get_width(old) == width
			&& gbm_bo_get_height(old) == height
			&& gbm_bo_get_format(old) == format) {
		return;
	}
	// The GPU may still draw into the old images.
	renderer_wait_idle();
	swapchain_fini(output);
	for (uint32_t i = 0; i < NR_SWAPCHAIN_IMAGES; i++) {
		struct gbm_bo *bo = scanout_bo_create(&output->primary, format, width,
			height);
		assert(bo);
		swapchain_image_init(&output->swapchain[i], bo);
	}
	cursor_images_init(output);
	if (output->hdr) {
		output->blend_buffer = texture_create(width, height,
			DRM_FORMAT_ABGR16161616F);
	}
}

// Destroys the images of the disabled output, which is off.
void
swapchain_destroy(struct output *output)
{
	// The GPU may still draw into the images.
	renderer_wait_idle();
	swapchain_fini(output);
}
