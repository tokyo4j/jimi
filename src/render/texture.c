#include <assert.h>
#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <X11/Xcursor/Xcursor.h>

#include "config.h"
#include "internal.h"

#define MIN_STAGING_BUFFER_SIZE (4 * 1024 * 1024)

// A persistently mapped buffer, whose bytes the writes of an upload take one
// after another.
struct staging_buffer {
	// texture_upload.staging_buffers, or vk.staging_buffers if unused
	struct wl_list link;
	VkBuffer buffer;
	VkDeviceMemory memory;
	uint8_t *mapped;
	VkDeviceSize size;
	VkDeviceSize used;
};

// The copies recorded between two submissions. Its staging buffers are
// reused once the GPU has executed it, so no copy waits for the previous
// ones.
struct texture_upload {
	struct wl_list link; // vk.uploads
	VkCommandBuffer cmd;
	struct wl_list staging_buffers; // staging_buffer.link
	uint64_t point; // The timeline point of its submission.
};

static void
staging_buffer_destroy(struct staging_buffer *staging)
{
	struct vk *vk = &core.vk;
	wl_list_remove(&staging->link);
	vkDestroyBuffer(vk->dev, staging->buffer, NULL);
	vkFreeMemory(vk->dev, staging->memory, NULL);
	free(staging);
}

// Returns the upload being recorded, beginning one if needed.
static struct texture_upload *
texture_upload_get(void)
{
	struct vk *vk = &core.vk;
	if (vk->upload) {
		return vk->upload;
	}
	struct texture_upload *upload = calloc(1, sizeof(*upload));
	assert(upload);
	wl_list_init(&upload->staging_buffers);
	VkCommandBufferAllocateInfo cbai = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = vk->cmd_pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	assert(vkAllocateCommandBuffers(vk->dev, &cbai, &upload->cmd) == VK_SUCCESS);
	VkCommandBufferBeginInfo begin = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	assert(vkBeginCommandBuffer(upload->cmd, &begin) == VK_SUCCESS);
	vk->upload = upload;
	return upload;
}

// Returns a staging buffer of the upload with the size left, taking the
// smallest unused one or creating one if none is large enough.
static struct staging_buffer *
texture_upload_get_staging(struct texture_upload *upload, VkDeviceSize size)
{
	struct vk *vk = &core.vk;
	struct staging_buffer *staging;
	wl_list_for_each(staging, &upload->staging_buffers, link) {
		if (staging->size - staging->used >= size) {
			return staging;
		}
	}
	struct staging_buffer *smallest = NULL;
	wl_list_for_each(staging, &vk->staging_buffers, link) {
		if (staging->size >= size && (!smallest || staging->size < smallest->size)) {
			smallest = staging;
		}
	}
	if (smallest) {
		wl_list_remove(&smallest->link);
		list_append(&upload->staging_buffers, &smallest->link);
		return smallest;
	}
	// Rounded up, so that a growing window does not need a new one each
	// frame.
	staging = calloc(1, sizeof(*staging));
	assert(staging);
	staging->size = MIN_STAGING_BUFFER_SIZE;
	while (staging->size < size) {
		staging->size *= 2;
	}
	vulkan_create_buffer(staging->size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		&staging->buffer, &staging->memory);
	void *mapped;
	assert(vkMapMemory(vk->dev, staging->memory, 0, VK_WHOLE_SIZE, 0,
		&mapped) == VK_SUCCESS);
	staging->mapped = mapped;
	list_append(&upload->staging_buffers, &staging->link);
	return staging;
}

// Its staging buffers replace the unused ones, as the next uploads likely
// need as much as it did. A staging buffer grown for larger
// writes than it had is freed instead, so that the memory is not held after
// them.
static void
texture_upload_free(struct texture_upload *upload)
{
	struct vk *vk = &core.vk;
	struct staging_buffer *staging, *tmp;
	wl_list_for_each_safe(staging, tmp, &vk->staging_buffers, link) {
		staging_buffer_destroy(staging);
	}
	wl_list_for_each_safe(staging, tmp, &upload->staging_buffers, link) {
		if (staging->size > MIN_STAGING_BUFFER_SIZE
				&& staging->used < staging->size / 4) {
			staging_buffer_destroy(staging);
			continue;
		}
		staging->used = 0;
		wl_list_remove(&staging->link);
		list_append(&vk->staging_buffers, &staging->link);
	}
	vkFreeCommandBuffers(vk->dev, vk->cmd_pool, 1, &upload->cmd);
	free(upload);
}

// Ends the upload being recorded for the submission of the timeline point,
// and returns its command buffer, or VK_NULL_HANDLE if there is none.
VkCommandBuffer
texture_uploads_end(uint64_t point)
{
	struct vk *vk = &core.vk;
	struct texture_upload *upload = vk->upload;
	if (!upload) {
		return VK_NULL_HANDLE;
	}
	assert(vkEndCommandBuffer(upload->cmd) == VK_SUCCESS);
	upload->point = point;
	list_append(&vk->uploads, &upload->link);
	vk->upload = NULL;
	return upload->cmd;
}

// Submits the upload being recorded on its own, when no frame is about to
// take it.
void
texture_uploads_submit(void)
{
	if (core.vk.upload) {
		vulkan_submit(VK_NULL_HANDLE, VK_NULL_HANDLE);
	}
}

// The uploads must have been collected.
void
texture_uploads_fini(void)
{
	struct vk *vk = &core.vk;
	assert(!vk->upload && wl_list_empty(&vk->uploads));
	struct staging_buffer *staging, *tmp;
	wl_list_for_each_safe(staging, tmp, &vk->staging_buffers, link) {
		staging_buffer_destroy(staging);
	}
}

// Also frees a partially created texture.
static void
texture_free(struct texture *tex)
{
	struct vk *vk = &core.vk;
	if (tex->view) {
		vkDestroyImageView(vk->dev, tex->view, NULL);
	}
	if (tex->chroma_view) {
		vkDestroyImageView(vk->dev, tex->chroma_view, NULL);
	}
	if (tex->image) {
		vkDestroyImage(vk->dev, tex->image, NULL);
	}
	for (uint32_t i = 0; i < tex->nr_memories; i++) {
		vkFreeMemory(vk->dev, tex->memories[i], NULL);
	}
	free(tex);
}

// The texture is freed once the GPU is done with the work submitted so far,
// and with the uploads being recorded, which are submitted next. NULL is
// ignored.
void
texture_destroy(struct texture *texture)
{
	struct vk *vk = &core.vk;
	if (!texture) {
		return;
	}
	texture->release_point = vk->timeline_point + (vk->upload ? 1 : 0);
	list_append(&vk->texture_garbage, &texture->link);
}

// Frees the textures and the uploads the GPU is done with.
void
textures_collect_garbage(void)
{
	struct vk *vk = &core.vk;
	if (wl_list_empty(&vk->uploads) && wl_list_empty(&vk->texture_garbage)) {
		return;
	}
	uint64_t completed = renderer_completed_point();
	struct texture_upload *upload, *upload_tmp;
	wl_list_for_each_safe(upload, upload_tmp, &vk->uploads, link) {
		if (upload->point > completed) {
			break;
		}
		wl_list_remove(&upload->link);
		texture_upload_free(upload);
	}
	struct texture *texture, *tmp;
	wl_list_for_each_safe(texture, tmp, &vk->texture_garbage, link) {
		if (texture->release_point > completed) {
			break;
		}
		wl_list_remove(&texture->link);
		texture_free(texture);
	}
}

// A texture of the DRM format, whose contents are undefined until
// texture_write_pixels(), or until the renderer draws into it.
struct texture *
texture_create(uint32_t w, uint32_t h, uint32_t format)
{
	struct vk *vk = &core.vk;
	struct texture *tex = calloc(1, sizeof(*tex));
	assert(tex);
	tex->format = pixel_format_find(format);
	VkImageCreateInfo ici = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = tex->format->vk,
		.extent = {w, h, 1},
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT
			| VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
			| VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
	};
	assert(vkCreateImage(vk->dev, &ici, NULL, &tex->image) == VK_SUCCESS);
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(vk->dev, tex->image, &req);
	VkMemoryAllocateInfo mai = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = req.size,
		.memoryTypeIndex = vulkan_find_memory_type(vk->phys, req.memoryTypeBits,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
	};
	assert(vkAllocateMemory(vk->dev, &mai, NULL, &tex->memories[0]) == VK_SUCCESS);
	tex->nr_memories = 1;
	assert(vkBindImageMemory(vk->dev, tex->image, tex->memories[0], 0)
		== VK_SUCCESS);
	tex->view = vulkan_create_image_view(tex->image, tex->format->vk,
		VK_IMAGE_ASPECT_COLOR_BIT);
	tex->sampler = vk->sampler;
	tex->width = w;
	tex->height = h;
	return tex;
}

static bool
dmabuf_memory_type(uint32_t bits, uint32_t *memory_type_index)
{
	VkPhysicalDeviceMemoryProperties props;
	vkGetPhysicalDeviceMemoryProperties(core.vk.phys, &props);
	for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
		if (bits & (1u << i)) {
			*memory_type_index = i;
			return true;
		}
	}
	return false;
}

static bool
dmabuf_planes_are_disjoint(const struct dmabuf_attributes *attributes)
{
	struct stat first;
	assert(fstat(attributes->fds[0], &first) == 0);
	for (uint32_t i = 1; i < attributes->nr_planes; i++) {
		struct stat plane;
		assert(fstat(attributes->fds[i], &plane) == 0);
		if (first.st_dev != plane.st_dev || first.st_ino != plane.st_ino) {
			return true;
		}
	}
	return false;
}

static VkImageAspectFlagBits
dmabuf_plane_aspect(uint32_t plane)
{
	switch (plane) {
	case 0: return VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT;
	case 1: return VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT;
	case 2: return VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT;
	case 3: return VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT;
	}
	assert(!"invalid dma-buf plane");
	return VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT;
}

// Returns NULL if the dma-buf cannot be imported. The format and the modifier
// must be supported.
struct texture *
texture_import_dmabuf(const struct dmabuf_attributes *attributes)
{
	struct vk *vk = &core.vk;
	struct texture *tex = calloc(1, sizeof(*tex));
	assert(tex);
	tex->format = pixel_format_find(attributes->format);
	// Image capture draws into renderable dma-bufs.
	tex->renderable = dmabuf_modifier_find(attributes->format,
		attributes->modifier)->renderable;
	VkSubresourceLayout plane_layouts[MAX_DMABUF_PLANES] = {0};
	for (uint32_t i = 0; i < attributes->nr_planes; i++) {
		plane_layouts[i].offset = attributes->offsets[i];
		plane_layouts[i].rowPitch = attributes->strides[i];
	}
	VkImageFormatListCreateInfo format_list = image_format_list(tex->format);
	VkImageDrmFormatModifierExplicitCreateInfoEXT modifier = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
		.pNext = &format_list,
		.drmFormatModifier = attributes->modifier,
		.drmFormatModifierPlaneCount = attributes->nr_planes,
		.pPlaneLayouts = plane_layouts,
	};
	VkExternalMemoryImageCreateInfo external = {
		.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
		.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
		.pNext = &modifier,
	};
	VkImageCreateInfo image_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.pNext = &external,
		.flags = VK_IMAGE_CREATE_ALIAS_BIT | image_create_flags(tex->format),
		.imageType = VK_IMAGE_TYPE_2D,
		.format = tex->format->vk,
		.extent = {attributes->width, attributes->height, 1},
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
		.usage = VK_IMAGE_USAGE_SAMPLED_BIT | (tex->renderable
			? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0),
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	bool disjoint = dmabuf_planes_are_disjoint(attributes);
	if (disjoint) {
		image_info.flags |= VK_IMAGE_CREATE_DISJOINT_BIT;
	}
	if (vkCreateImage(vk->dev, &image_info, NULL, &tex->image) != VK_SUCCESS) {
		texture_free(tex);
		return NULL;
	}

	uint32_t memory_count = disjoint ? attributes->nr_planes : 1;
	VkBindImageMemoryInfo bind_infos[MAX_DMABUF_PLANES] = {0};
	VkBindImagePlaneMemoryInfo plane_infos[MAX_DMABUF_PLANES] = {0};
	for (uint32_t i = 0; i < memory_count; i++) {
		uint32_t plane = disjoint ? i : 0;
		int fd = fcntl(attributes->fds[plane], F_DUPFD_CLOEXEC, 0);
		if (fd < 0) {
			texture_free(tex);
			return NULL;
		}
		VkMemoryFdPropertiesKHR fd_props = {
			.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
		};
		if (vk->api.vkGetMemoryFdPropertiesKHR(vk->dev,
				VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
				fd, &fd_props) != VK_SUCCESS) {
			close(fd);
			texture_free(tex);
			return NULL;
		}
		VkImagePlaneMemoryRequirementsInfo plane_requirements = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_PLANE_MEMORY_REQUIREMENTS_INFO,
			.planeAspect = dmabuf_plane_aspect(plane),
		};
		VkImageMemoryRequirementsInfo2 requirements_info = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
			.image = tex->image,
			.pNext = disjoint ? &plane_requirements : NULL,
		};
		VkMemoryRequirements2 requirements = {
			.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
		};
		vkGetImageMemoryRequirements2(vk->dev, &requirements_info, &requirements);
		uint32_t memory_type_index;
		if (!dmabuf_memory_type(requirements.memoryRequirements.memoryTypeBits
				& fd_props.memoryTypeBits, &memory_type_index)) {
			close(fd);
			texture_free(tex);
			return NULL;
		}
		VkMemoryDedicatedAllocateInfo dedicated = {
			.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
			.image = tex->image,
		};
		VkImportMemoryFdInfoKHR import = {
			.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
			.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
			.fd = fd,
			.pNext = &dedicated,
		};
		VkMemoryAllocateInfo memory_info = {
			.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
			.pNext = &import,
			.allocationSize = requirements.memoryRequirements.size,
			.memoryTypeIndex = memory_type_index,
		};
		if (vkAllocateMemory(vk->dev, &memory_info, NULL,
				&tex->memories[i]) != VK_SUCCESS) {
			close(fd);
			texture_free(tex);
			return NULL;
		}
		tex->nr_memories++;
		bind_infos[i] = (VkBindImageMemoryInfo){
			.sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO,
			.image = tex->image,
			.memory = tex->memories[i],
		};
		if (disjoint) {
			plane_infos[i] = (VkBindImagePlaneMemoryInfo){
				.sType = VK_STRUCTURE_TYPE_BIND_IMAGE_PLANE_MEMORY_INFO,
				.planeAspect = dmabuf_plane_aspect(i),
			};
			bind_infos[i].pNext = &plane_infos[i];
		}
	}
	if (disjoint) {
		if (vkBindImageMemory2(vk->dev, memory_count, bind_infos) != VK_SUCCESS) {
			texture_free(tex);
			return NULL;
		}
	} else if (vkBindImageMemory(vk->dev, tex->image, tex->memories[0], 0)
			!= VK_SUCCESS) {
		texture_free(tex);
		return NULL;
	}

	if (tex->format->ycbcr_bits) {
		tex->view = vulkan_create_image_view(tex->image,
			tex->format->plane_formats[0], VK_IMAGE_ASPECT_PLANE_0_BIT);
		tex->chroma_view = vulkan_create_image_view(tex->image,
			tex->format->plane_formats[1], VK_IMAGE_ASPECT_PLANE_1_BIT);
	} else {
		tex->view = vulkan_create_image_view(tex->image, tex->format->vk,
			VK_IMAGE_ASPECT_COLOR_BIT);
	}
	tex->sampler = vk->sampler;
	tex->width = attributes->width;
	tex->height = attributes->height;
	tex->dmabuf = true;
	tex->y_invert = attributes->y_invert;
	return tex;
}

// Replaces the contents within the region with the pixels of the texture's
// size and format. A NULL region replaces all of them, which are undefined
// before.
void
texture_write_pixels(struct texture *texture, const uint8_t *pixels,
	uint32_t stride, const pixman_region32_t *region)
{
	assert(!texture->dmabuf);
	pixman_box32_t whole = {
		.x2 = (int32_t)texture->width,
		.y2 = (int32_t)texture->height,
	};
	const pixman_box32_t *rects = &whole;
	int nr_rects = 1;
	if (region) {
		rects = pixman_region32_rectangles(region, &nr_rects);
	}
	if (nr_rects == 0) {
		return;
	}
	uint32_t bytes = texture->format->bytes;
	VkDeviceSize size = 0;
	for (int i = 0; i < nr_rects; i++) {
		assert(rects[i].x1 >= 0 && rects[i].y1 >= 0
			&& rects[i].x2 <= whole.x2 && rects[i].y2 <= whole.y2);
		size += (VkDeviceSize)(rects[i].x2 - rects[i].x1)
			* (VkDeviceSize)(rects[i].y2 - rects[i].y1) * bytes;
	}
	struct texture_upload *upload = texture_upload_get();
	struct staging_buffer *staging = texture_upload_get_staging(upload, size);

	// The rectangles are packed into the staging buffer one after another.
	VkBufferImageCopy *copies = calloc((size_t)nr_rects, sizeof(*copies));
	assert(copies);
	VkDeviceSize offset = staging->used;
	for (int i = 0; i < nr_rects; i++) {
		uint32_t w = (uint32_t)(rects[i].x2 - rects[i].x1);
		uint32_t h = (uint32_t)(rects[i].y2 - rects[i].y1);
		copies[i] = (VkBufferImageCopy){
			.bufferOffset = offset,
			.imageSubresource = {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.layerCount = 1,
			},
			.imageOffset = {rects[i].x1, rects[i].y1, 0},
			.imageExtent = {w, h, 1},
		};
		const uint8_t *src = pixels + (size_t)rects[i].y1 * stride
			+ (size_t)rects[i].x1 * bytes;
		uint8_t *dst = staging->mapped + offset;
		for (uint32_t y = 0; y < h; y++) {
			memcpy(dst + (size_t)y * w * bytes, src + (size_t)y * stride,
				(size_t)w * bytes);
		}
		offset += (VkDeviceSize)w * h * bytes;
	}
	staging->used = offset;

	// The old contents are discarded if all of them are replaced.
	vulkan_image_barrier(upload->cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, (VkImageMemoryBarrier){
			.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			.oldLayout = region ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
				: VK_IMAGE_LAYOUT_UNDEFINED,
			.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = texture->image,
		});
	vkCmdCopyBufferToImage(upload->cmd, staging->buffer, texture->image,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)nr_rects, copies);
	vulkan_image_barrier(upload->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, (VkImageMemoryBarrier){
			.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = texture->image,
		});
	free(copies);
}

// Copies the shm buffer into the texture. A texture of the same size and
// format keeps the contents it was written last, so only the damage is
// copied into it. Otherwise, it is replaced by a new one, which is written
// whole.
void
texture_write_shm(struct texture **texture, struct wl_shm_buffer *shm,
	const pixman_region32_t *damage)
{
	uint32_t width = (uint32_t)wl_shm_buffer_get_width(shm);
	uint32_t height = (uint32_t)wl_shm_buffer_get_height(shm);
	const struct pixel_format *format =
		pixel_format_find_shm(wl_shm_buffer_get_format(shm));
	struct texture *tex = *texture;
	bool kept = tex && tex->width == width && tex->height == height
		&& tex->format == format;
	if (!kept) {
		texture_destroy(tex);
		tex = texture_create(width, height, format->drm);
		*texture = tex;
	}
	wl_shm_buffer_begin_access(shm);
	texture_write_pixels(tex, wl_shm_buffer_get_data(shm),
		(uint32_t)wl_shm_buffer_get_stride(shm), kept ? damage : NULL);
	wl_shm_buffer_end_access(shm);
}

// The names of the shapes in cursor themes, and their legacy X11 names for
// older themes.
static const char *const shape_names[NR_CURSOR_SHAPES][2] = {
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT] = {"default", "left_ptr"},
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CONTEXT_MENU] = {"context-menu", NULL},
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_HELP] = {"help", "question_arrow"},
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER] = {"pointer", "hand2"},
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_PROGRESS] = {"progress", "left_ptr_watch"},
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_WAIT] = {"wait", "watch"},
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CELL] = {"cell", "plus"},
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR] = {"crosshair", "cross"},
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT] = {"text", "xterm"},
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_VERTICAL_TEXT] = {"vertical-text", NULL},
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALIAS] = {"alias", "dnd-link"},
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COPY] = {"copy", "dnd-copy"},
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE] = {"move", "dnd-move"},
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NO_DROP] = {"no-drop", "dnd-none"},
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NOT_ALLOWED] = {"not-allowed", "crossed_circle"},
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB] = {"grab", "hand1"},
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING] = {"grabbing", "fleur"},
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_E_RESIZE] = {"e-resize", "right_side"},
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_N_RESIZE] = {"n-resize", "top_side"},
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NE_RESIZE] = {"ne-resize", "top_right_corner"},
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NW_RESIZE] = {"nw-resize", "top_left_corner"},
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_S_RESIZE] = {"s-resize", "bottom_side"},
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SE_RESIZE] = {"se-resize", "bottom_right_corner"},
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SW_RESIZE] = {"sw-resize", "bottom_left_corner"},
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_W_RESIZE] = {"w-resize", "left_side"},
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE] = {"ew-resize", "size_hor"},
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE] = {"ns-resize", "size_ver"},
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE] = {"nesw-resize", "size_bdiag"},
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE] = {"nwse-resize", "size_fdiag"},
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COL_RESIZE] = {"col-resize", "sb_h_double_arrow"},
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ROW_RESIZE] = {"row-resize", "sb_v_double_arrow"},
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_SCROLL] = {"all-scroll", "fleur"},
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ZOOM_IN] = {"zoom-in", NULL},
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ZOOM_OUT] = {"zoom-out", NULL},
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DND_ASK] = {"dnd-ask", NULL},
	[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_RESIZE] = {"all-resize", "fleur"},
};

// Shapes missing from the theme look like the default one.
static void
server_cursor_load(struct server_cursor *cursor,
	enum wp_cursor_shape_device_v1_shape shape, const char *theme, int size)
{
	const char *const names[] = {
		shape_names[shape][0],
		shape_names[shape][1],
		shape_names[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT][0],
		shape_names[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT][1],
	};
	XcursorImage *image = NULL;
	for (size_t i = 0; i < ARRAY_LEN(names) && !image; i++) {
		if (names[i]) {
			image = XcursorLibraryLoadImage(names[i], theme, size);
		}
	}
	assert(image);
	cursor->width = image->width;
	cursor->height = image->height;
	cursor->hotspot_x = image->xhot;
	cursor->hotspot_y = image->yhot;
	cursor->texture = texture_create(image->width, image->height,
		DRM_FORMAT_ARGB8888);
	texture_write_pixels(cursor->texture, (const uint8_t *)image->pixels,
		(uint32_t)image->width * sizeof(*image->pixels), NULL);
	XcursorImageDestroy(image);
}

// The images are for the scale, and drawn unscaled.
void
server_cursors_load(struct server_cursor cursors[], double scale)
{
	const char *theme = config.cursor.theme;
	int scaled_size = (int)round(config.cursor.size * scale);
	for (int shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
			shape < NR_CURSOR_SHAPES; shape++) {
		server_cursor_load(&cursors[shape], shape, theme, scaled_size);
	}
}

void
server_cursors_destroy(struct server_cursor cursors[])
{
	for (int shape = 0; shape < NR_CURSOR_SHAPES; shape++) {
		texture_destroy(cursors[shape].texture);
	}
}
