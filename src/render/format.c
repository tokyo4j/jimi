#include <assert.h>
#include <math.h>
#include <stdlib.h>

#include "internal.h"

const struct pixel_format pixel_formats[NR_PIXEL_FORMATS] = {
	{
		.drm = DRM_FORMAT_ARGB8888,
		.opaque = DRM_FORMAT_XRGB8888,
		.shm = WL_SHM_FORMAT_ARGB8888,
		.vk = VK_FORMAT_B8G8R8A8_UNORM,
		.bytes = 4,
		.has_alpha = true,
	},
	{
		.drm = DRM_FORMAT_XRGB8888,
		.opaque = DRM_FORMAT_XRGB8888,
		.shm = WL_SHM_FORMAT_XRGB8888,
		.vk = VK_FORMAT_B8G8R8A8_UNORM,
		.bytes = 4,
		.has_alpha = false,
	},
	{
		.drm = DRM_FORMAT_ABGR8888,
		.opaque = DRM_FORMAT_XBGR8888,
		.shm = WL_SHM_FORMAT_ABGR8888,
		.vk = VK_FORMAT_R8G8B8A8_UNORM,
		.bytes = 4,
		.has_alpha = true,
	},
	{
		.drm = DRM_FORMAT_XBGR8888,
		.opaque = DRM_FORMAT_XBGR8888,
		.shm = WL_SHM_FORMAT_XBGR8888,
		.vk = VK_FORMAT_R8G8B8A8_UNORM,
		.bytes = 4,
		.has_alpha = false,
	},
	{
		.drm = DRM_FORMAT_ARGB2101010,
		.opaque = DRM_FORMAT_XRGB2101010,
		.shm = WL_SHM_FORMAT_ARGB2101010,
		.vk = VK_FORMAT_A2R10G10B10_UNORM_PACK32,
		.bytes = 4,
		.has_alpha = true,
	},
	{
		.drm = DRM_FORMAT_XRGB2101010,
		.opaque = DRM_FORMAT_XRGB2101010,
		.shm = WL_SHM_FORMAT_XRGB2101010,
		.vk = VK_FORMAT_A2R10G10B10_UNORM_PACK32,
		.bytes = 4,
		.has_alpha = false,
	},
	{
		.drm = DRM_FORMAT_ABGR2101010,
		.opaque = DRM_FORMAT_XBGR2101010,
		.shm = WL_SHM_FORMAT_ABGR2101010,
		.vk = VK_FORMAT_A2B10G10R10_UNORM_PACK32,
		.bytes = 4,
		.has_alpha = true,
	},
	{
		.drm = DRM_FORMAT_XBGR2101010,
		.opaque = DRM_FORMAT_XBGR2101010,
		.shm = WL_SHM_FORMAT_XBGR2101010,
		.vk = VK_FORMAT_A2B10G10R10_UNORM_PACK32,
		.bytes = 4,
		.has_alpha = false,
	},
	{
		.drm = DRM_FORMAT_ABGR16161616F,
		.opaque = DRM_FORMAT_XBGR16161616F,
		.shm = WL_SHM_FORMAT_ABGR16161616F,
		.vk = VK_FORMAT_R16G16B16A16_SFLOAT,
		.bytes = 8,
		.has_alpha = true,
	},
	{
		.drm = DRM_FORMAT_XBGR16161616F,
		.opaque = DRM_FORMAT_XBGR16161616F,
		.shm = WL_SHM_FORMAT_XBGR16161616F,
		.vk = VK_FORMAT_R16G16B16A16_SFLOAT,
		.bytes = 8,
		.has_alpha = false,
	},
	// Video, like from hardware decoders. P010 is that of HDR.
	{
		.drm = DRM_FORMAT_NV12,
		.shm = WL_SHM_FORMAT_NV12,
		.vk = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
		.bytes = 1,
		.ycbcr_bits = 8,
		.plane_formats = {VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM},
	},
	{
		.drm = DRM_FORMAT_P010,
		.shm = WL_SHM_FORMAT_P010,
		.vk = VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16,
		.bytes = 2,
		.ycbcr_bits = 10,
		.plane_formats = {VK_FORMAT_R16_UNORM, VK_FORMAT_R16G16_UNORM},
	},
};

// NULL if the DRM format is not supported.
const struct pixel_format *
pixel_format_find(uint32_t drm)
{
	for (size_t i = 0; i < ARRAY_LEN(pixel_formats); i++) {
		if (pixel_formats[i].drm == drm) {
			return &pixel_formats[i];
		}
	}
	return NULL;
}

// NULL if the wl_shm format is not supported.
const struct pixel_format *
pixel_format_find_shm(uint32_t shm)
{
	for (size_t i = 0; i < ARRAY_LEN(pixel_formats); i++) {
		if (pixel_formats[i].shm == shm) {
			return &pixel_formats[i];
		}
	}
	return NULL;
}

const struct format_support *
format_support(const struct pixel_format *format)
{
	return &core.vk.formats[format - pixel_formats];
}

// The views of the planes of YCbCr have the formats of the planes.
VkImageCreateFlags
image_create_flags(const struct pixel_format *format)
{
	return format->ycbcr_bits ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;
}

// Of the views of the images of the format, which images of DRM modifiers
// with mutable formats need.
VkImageFormatListCreateInfo
image_format_list(const struct pixel_format *format)
{
	return (VkImageFormatListCreateInfo){
		.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
		.viewFormatCount = format->ycbcr_bits ? 2 : 0,
		.pViewFormats = format->plane_formats,
	};
}

bool
modifier_importable(const struct pixel_format *format, uint64_t modifier,
	VkImageCreateFlags flags, VkImageUsageFlags usage)
{
	VkImageFormatListCreateInfo list = image_format_list(format);
	VkPhysicalDeviceImageDrmFormatModifierInfoEXT mod = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
		.pNext = &list,
		.drmFormatModifier = modifier,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	VkPhysicalDeviceExternalImageFormatInfo ext = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
		.pNext = &mod,
		.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
	};
	VkPhysicalDeviceImageFormatInfo2 info = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
		.pNext = &ext,
		.flags = flags | image_create_flags(format),
		.format = format->vk,
		.type = VK_IMAGE_TYPE_2D,
		.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
		.usage = usage,
	};
	VkExternalImageFormatProperties ext_props = {
		.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
	};
	VkImageFormatProperties2 props = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
		.pNext = &ext_props,
	};
	return vkGetPhysicalDeviceImageFormatProperties2(core.vk.phys, &info,
			&props) == VK_SUCCESS
		&& (ext_props.externalMemoryProperties.externalMemoryFeatures
			& VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT);
}

// The modifiers of the dma-bufs of the format that we can sample from, and
// whether we can draw into them.
static void
init_dmabuf_modifiers(const struct pixel_format *format,
	struct format_support *support)
{
	VkDrmFormatModifierPropertiesListEXT list = {
		.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
	};
	VkFormatProperties2 format_props = {
		.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
		.pNext = &list,
	};
	vkGetPhysicalDeviceFormatProperties2(core.vk.phys, format->vk, &format_props);
	VkDrmFormatModifierPropertiesEXT *props = calloc(
		list.drmFormatModifierCount, sizeof(*props));
	assert(props || list.drmFormatModifierCount == 0);
	list.pDrmFormatModifierProperties = props;
	vkGetPhysicalDeviceFormatProperties2(core.vk.phys, format->vk, &format_props);

	support->modifiers = calloc(list.drmFormatModifierCount,
		sizeof(*support->modifiers));
	assert(support->modifiers || list.drmFormatModifierCount == 0);
	for (uint32_t i = 0; i < list.drmFormatModifierCount; i++) {
		uint64_t modifier = props[i].drmFormatModifier;
		uint32_t nr_planes = props[i].drmFormatModifierPlaneCount;
		VkImageCreateFlags flags = VK_IMAGE_CREATE_ALIAS_BIT
			| (nr_planes > 1 ? VK_IMAGE_CREATE_DISJOINT_BIT : 0);
		VkFormatFeatureFlags features = props[i].drmFormatModifierTilingFeatures;
		if (nr_planes <= MAX_DMABUF_PLANES
				&& (features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)
				&& modifier_importable(format, modifier, flags,
					VK_IMAGE_USAGE_SAMPLED_BIT)) {
			support->modifiers[support->nr_modifiers++] = (struct dmabuf_modifier){
					.modifier = modifier,
					.nr_planes = nr_planes,
					.renderable = (features & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)
						&& modifier_importable(format, modifier, flags,
							VK_IMAGE_USAGE_SAMPLED_BIT
								| VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT),
				};
		}
	}
	free(props);
	// YCbCr is only for dma-bufs.
	VkFormatFeatureFlags shm_features = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT
		| VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
	support->shm = !format->ycbcr_bits
		&& (format_props.formatProperties.optimalTilingFeatures
			& shm_features) == shm_features;
}

// Finds what we can do with the client buffers of each format. Those of
// ARGB8888 and XRGB8888, which all clients can draw, are always supported.
void
formats_init(void)
{
	for (size_t i = 0; i < ARRAY_LEN(pixel_formats); i++) {
		init_dmabuf_modifiers(&pixel_formats[i], &core.vk.formats[i]);
	}
	const struct format_support *argb8888 =
		format_support(pixel_format_find(DRM_FORMAT_ARGB8888));
	assert(argb8888->nr_modifiers > 0 && argb8888->shm);
}

// NULL if client dma-bufs with the format and the modifier cannot be
// imported.
const struct dmabuf_modifier *
dmabuf_modifier_find(uint32_t drm_format, uint64_t modifier)
{
	const struct pixel_format *format = pixel_format_find(drm_format);
	if (!format) {
		return NULL;
	}
	const struct format_support *support = format_support(format);
	for (uint32_t i = 0; i < support->nr_modifiers; i++) {
		if (support->modifiers[i].modifier == modifier) {
			return &support->modifiers[i];
		}
	}
	return NULL;
}

void
formats_fini(void)
{
	for (size_t i = 0; i < ARRAY_LEN(pixel_formats); i++) {
		free(core.vk.formats[i].modifiers);
	}
}

// The affine matrix from the sampled luma and chroma of the YCbCr format in
// the limited range to R'G'B', for the coefficients of red and blue of the
// luma. The values are in the most significant bits of their bytes.
void
ycbcr_matrix(const struct pixel_format *format, double kr, double kb,
	float matrix[3][4])
{
	uint32_t bits = format->ycbcr_bits;
	uint32_t storage = 8 * format->bytes;
	double kg = 1.0 - kr - kb;
	// The codes of the values sampled as 1.0, and of the steps of 8 bits
	double max_code = (pow(2.0, storage) - 1.0) / pow(2.0, storage - bits);
	double step = pow(2.0, bits - 8.0);
	// Luma spans 16 to 235, and chroma 16 to 240 around 128, in steps.
	double luma_scale = max_code / (219.0 * step);
	double luma_offset = -16.0 / 219.0;
	double chroma_scale = max_code / (224.0 * step);
	double chroma_offset = -128.0 / 224.0;
	// Of Y'CbCr, with chroma from -0.5 to 0.5
	double to_rgb[3][3] = {
		{1.0, 0.0, 2.0 * (1.0 - kr)},
		{1.0, -2.0 * (1.0 - kb) * kb / kg, -2.0 * (1.0 - kr) * kr / kg},
		{1.0, 2.0 * (1.0 - kb), 0.0},
	};
	for (int i = 0; i < 3; i++) {
		matrix[i][0] = (float)(to_rgb[i][0] * luma_scale);
		matrix[i][1] = (float)(to_rgb[i][1] * chroma_scale);
		matrix[i][2] = (float)(to_rgb[i][2] * chroma_scale);
		matrix[i][3] = (float)(to_rgb[i][0] * luma_offset
			+ (to_rgb[i][1] + to_rgb[i][2]) * chroma_offset);
	}
}
