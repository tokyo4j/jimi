#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <X11/Xcursor/Xcursor.h>
#include <wayland-server-protocol.h>
#include <xf86drm.h>

#include "box.vert.h"
#include "shadow.frag.h"
#include "solid.frag.h"
#include "texture.frag.h"
#include "texture.vert.h"
#include "internal.h"

uint32_t
vulkan_find_memory_type(VkPhysicalDevice phys, uint32_t bits,
	VkMemoryPropertyFlags flags)
{
	VkPhysicalDeviceMemoryProperties props;
	vkGetPhysicalDeviceMemoryProperties(phys, &props);
	for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
		if ((bits & (1u << i))
				&& ((props.memoryTypes[i].propertyFlags & flags) == flags)) {
			return i;
		}
	}
	assert(!"no matching Vulkan memory type");
	return 0;
}

// Allocates memory with the properties for the buffer, and binds it.
void
vulkan_create_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
	VkMemoryPropertyFlags flags, VkBuffer *buffer, VkDeviceMemory *memory)
{
	struct vk *vk = &core.vk;
	VkBufferCreateInfo bci = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = size,
		.usage = usage,
	};
	assert(vkCreateBuffer(vk->dev, &bci, NULL, buffer) == VK_SUCCESS);
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(vk->dev, *buffer, &req);
	VkMemoryAllocateInfo mai = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = req.size,
		.memoryTypeIndex = vulkan_find_memory_type(vk->phys,
			req.memoryTypeBits, flags),
	};
	assert(vkAllocateMemory(vk->dev, &mai, NULL, memory) == VK_SUCCESS);
	assert(vkBindBufferMemory(vk->dev, *buffer, *memory, 0) == VK_SUCCESS);
}

static bool
has_ext(const VkExtensionProperties *exts, uint32_t count, const char *name)
{
	for (uint32_t i = 0; i < count; i++) {
		if (strcmp(exts[i].extensionName, name) == 0) {
			return true;
		}
	}
	return false;
}

// Submits the texture uploads recorded since the last submission, then the
// command buffer if any, as the next point of the timeline, and replaces the
// sync file with its own. Both also include all earlier submissions. The
// uploads take no submission of their own, as each costs the kernel much.
void
vulkan_submit(VkCommandBuffer cmd, VkFence fence)
{
	struct vk *vk = &core.vk;
	vk->timeline_point++;
	VkCommandBuffer cmds[2];
	uint32_t nr_cmds = 0;
	VkCommandBuffer upload = texture_uploads_end(vk->timeline_point);
	if (upload) {
		cmds[nr_cmds++] = upload;
	}
	if (cmd) {
		cmds[nr_cmds++] = cmd;
	}
	VkSemaphore semaphores[] = {vk->timeline, vk->submitted};
	uint64_t values[] = {vk->timeline_point, 0};
	VkTimelineSemaphoreSubmitInfo timeline = {
		.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
		.signalSemaphoreValueCount = ARRAY_LEN(values),
		.pSignalSemaphoreValues = values,
	};
	VkSubmitInfo submit = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.pNext = &timeline,
		.commandBufferCount = nr_cmds,
		.pCommandBuffers = cmds,
		.signalSemaphoreCount = ARRAY_LEN(semaphores),
		.pSignalSemaphores = semaphores,
	};
	assert(vkQueueSubmit(vk->queue, 1, &submit, fence) == VK_SUCCESS);
	// Exporting unsignals the semaphore for the next submission.
	VkSemaphoreGetFdInfoKHR fd_info = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
		.semaphore = vk->submitted,
		.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
	};
	if (vk->sync_file >= 0) {
		close(vk->sync_file);
	}
	assert(vk->api.vkGetSemaphoreFdKHR(vk->dev, &fd_info, &vk->sync_file)
		== VK_SUCCESS);
}

// Returns a sync file that signals once the last submission completes.
int
vulkan_dup_sync_file(void)
{
	int fd = fcntl(core.vk.sync_file, F_DUPFD_CLOEXEC, 0);
	assert(fd >= 0);
	return fd;
}

// The timeline point of the last submission that the GPU has completed, along
// with the earlier ones.
uint64_t
renderer_completed_point(void)
{
	struct vk *vk = &core.vk;
	uint64_t completed;
	assert(vkGetSemaphoreCounterValue(vk->dev, vk->timeline, &completed)
		== VK_SUCCESS);
	return completed;
}

static const VkImageSubresourceRange color_range = {
	.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	.levelCount = 1,
	.layerCount = 1,
};

// A view of the color, or of a plane of YCbCr.
VkImageView
vulkan_create_image_view(VkImage image, VkFormat format, VkImageAspectFlags aspect)
{
	VkImageViewCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = format,
		.subresourceRange = color_range,
	};
	info.subresourceRange.aspectMask = aspect;
	VkImageView view;
	assert(vkCreateImageView(core.vk.dev, &info, NULL, &view) == VK_SUCCESS);
	return view;
}

// Records the barrier for the whole color image. Its type and subresource
// range are filled in.
void
vulkan_image_barrier(VkCommandBuffer cmd, VkPipelineStageFlags src_stage,
	VkPipelineStageFlags dst_stage, VkImageMemoryBarrier barrier)
{
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.subresourceRange = color_range;
	vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1,
		&barrier);
}

static void
init_device(void)
{
	struct vk *vk = &core.vk;
	VkApplicationInfo ai = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "jimi",
		.apiVersion = VK_API_VERSION_1_3,
	};
	VkInstanceCreateInfo ici = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &ai,
	};
	assert(vkCreateInstance(&ici, NULL, &vk->instance) == VK_SUCCESS);

	uint32_t nr_phys = 0;
	assert(vkEnumeratePhysicalDevices(vk->instance, &nr_phys, NULL) == VK_SUCCESS);
	assert(nr_phys > 0);
	VkPhysicalDevice phys[nr_phys];
	assert(vkEnumeratePhysicalDevices(vk->instance, &nr_phys, phys) == VK_SUCCESS);

	struct stat st;
	assert(fstat(core.kms.fd, &st) == 0);
	uint32_t drm_major = major(st.st_rdev);
	uint32_t drm_minor = minor(st.st_rdev);
	const char *required_dev_exts[] = {
		VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME,
		VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
		VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
		VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
		VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
		VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
		VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
		VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME,
		VK_KHR_GLOBAL_PRIORITY_EXTENSION_NAME,
		VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
	};

	for (uint32_t i = 0; i < nr_phys; i++) {
		uint32_t nr_exts = 0;
		vkEnumerateDeviceExtensionProperties(phys[i], NULL, &nr_exts, NULL);
		VkExtensionProperties exts[nr_exts];
		vkEnumerateDeviceExtensionProperties(phys[i], NULL, &nr_exts, exts);

		bool missing = false;
		for (uint32_t j = 0; j < ARRAY_LEN(required_dev_exts); j++) {
			missing |= !has_ext(exts, nr_exts, required_dev_exts[j]);
		}
		if (missing) {
			continue;
		}

		VkPhysicalDeviceDrmPropertiesEXT drm_props = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT,
		};
		VkPhysicalDeviceProperties2 props = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
			.pNext = &drm_props,
		};
		vkGetPhysicalDeviceProperties2(phys[i], &props);
		// The texture shaders convert the colors by push constants, as
		// most drivers allow.
		if (props.properties.limits.maxPushConstantsSize
				< sizeof(struct texture_push_constants)) {
			continue;
		}
		if ((drm_props.hasPrimary && drm_props.primaryMajor == drm_major
					&& drm_props.primaryMinor == drm_minor)
				|| (drm_props.hasRender && drm_props.renderMajor == drm_major
					&& drm_props.renderMinor == drm_minor)) {
			vk->phys = phys[i];
			break;
		}
	}
	assert(vk->phys);

	uint32_t nr_qfam = 0;
	bool found_qfam = false;
	vkGetPhysicalDeviceQueueFamilyProperties(vk->phys, &nr_qfam, NULL);
	VkQueueFamilyProperties qprops[nr_qfam];
	vkGetPhysicalDeviceQueueFamilyProperties(vk->phys, &nr_qfam, qprops);
	for (uint32_t i = 0; i < nr_qfam; i++) {
		if (qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
			vk->qfam = i;
			found_qfam = true;
			break;
		}
	}
	assert(found_qfam);

	// The GPU preempts the clients for the compositor, so that they cannot
	// delay its frames.
	VkDeviceQueueGlobalPriorityCreateInfoKHR global_priority = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_KHR,
		.globalPriority = VK_QUEUE_GLOBAL_PRIORITY_HIGH_KHR,
	};
	float q_prio = 1.0f;
	VkDeviceQueueCreateInfo qci = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.pNext = &global_priority,
		.queueFamilyIndex = vk->qfam,
		.queueCount = 1,
		.pQueuePriorities = &q_prio,
	};
	VkPhysicalDeviceExtendedDynamicState3FeaturesEXT dyn3_features = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT,
		.extendedDynamicState3ColorBlendEnable = VK_TRUE,
	};
	VkPhysicalDeviceVulkan13Features features13 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
		.pNext = &dyn3_features,
		.dynamicRendering = VK_TRUE,
	};
	VkPhysicalDeviceVulkan12Features features12 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
		.pNext = &features13,
		.timelineSemaphore = VK_TRUE,
	};
	// The YCbCr formats of client dma-bufs are supported with it, though
	// their planes are sampled as they are.
	VkPhysicalDeviceVulkan11Features features11 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
	};
	VkPhysicalDeviceFeatures2 supported = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
		.pNext = &features11,
	};
	vkGetPhysicalDeviceFeatures2(vk->phys, &supported);
	features11 = (VkPhysicalDeviceVulkan11Features){
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
		.pNext = &features12,
		.samplerYcbcrConversion = features11.samplerYcbcrConversion,
	};
	VkPhysicalDeviceFeatures2 features = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
		.pNext = &features11,
	};
	VkDeviceCreateInfo dci = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.pNext = &features,
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &qci,
		.enabledExtensionCount = ARRAY_LEN(required_dev_exts),
		.ppEnabledExtensionNames = required_dev_exts,
	};
	VkResult res = vkCreateDevice(vk->phys, &dci, NULL, &vk->dev);
	// Drivers like i915 and xe require CAP_SYS_NICE for the high priority.
	if (res == VK_ERROR_NOT_PERMITTED_KHR) {
		fprintf(stderr, "Not permitted to use a high-priority GPU queue, "
			"so GPU-heavy clients can delay the frames\n");
		qci.pNext = NULL;
		res = vkCreateDevice(vk->phys, &dci, NULL, &vk->dev);
	}
	assert(res == VK_SUCCESS);
	vkGetDeviceQueue(vk->dev, vk->qfam, 0, &vk->queue);
	vk->api.vkGetMemoryFdPropertiesKHR =
		(PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(vk->dev,
			"vkGetMemoryFdPropertiesKHR");
	assert(vk->api.vkGetMemoryFdPropertiesKHR);
	vk->api.vkGetSemaphoreFdKHR =
		(PFN_vkGetSemaphoreFdKHR)vkGetDeviceProcAddr(vk->dev, "vkGetSemaphoreFdKHR");
	assert(vk->api.vkGetSemaphoreFdKHR);
	vk->api.vkCmdSetColorBlendEnableEXT =
		(PFN_vkCmdSetColorBlendEnableEXT)vkGetDeviceProcAddr(vk->dev,
			"vkCmdSetColorBlendEnableEXT");
	assert(vk->api.vkCmdSetColorBlendEnableEXT);
	vk->api.vkCmdPushDescriptorSetKHR =
		(PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(vk->dev,
			"vkCmdPushDescriptorSetKHR");
	assert(vk->api.vkCmdPushDescriptorSetKHR);

	VkCommandPoolCreateInfo cpci = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = vk->qfam,
	};
	assert(vkCreateCommandPool(vk->dev, &cpci, NULL, &vk->cmd_pool) == VK_SUCCESS);

	VkSemaphoreTypeCreateInfo timeline = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
		.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
	};
	VkSemaphoreCreateInfo sci = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
		.pNext = &timeline,
	};
	assert(vkCreateSemaphore(vk->dev, &sci, NULL, &vk->timeline) == VK_SUCCESS);

	VkExportSemaphoreCreateInfo export = {
		.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
		.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
	};
	sci.pNext = &export;
	assert(vkCreateSemaphore(vk->dev, &sci, NULL, &vk->submitted) == VK_SUCCESS);
	vk->sync_file = -1;
}

static void
create_pipeline(VkGraphicsPipelineCreateInfo gpci,
	const uint32_t *vert_spv, size_t vert_size,
	const uint32_t *frag_spv, size_t frag_size,
	VkPipelineLayout layout, VkPipeline *pipeline)
{
	struct vk *vk = &core.vk;
	VkShaderModuleCreateInfo smci = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = vert_size,
		.pCode = vert_spv,
	};
	VkShaderModule vert, frag;
	assert(vkCreateShaderModule(vk->dev, &smci, NULL, &vert) == VK_SUCCESS);
	smci.codeSize = frag_size;
	smci.pCode = frag_spv;
	assert(vkCreateShaderModule(vk->dev, &smci, NULL, &frag) == VK_SUCCESS);
	VkPipelineShaderStageCreateInfo stages[] = {
		{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_VERTEX_BIT,
			.module = vert,
			.pName = "main",
		},
		{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
			.module = frag,
			.pName = "main",
		},
	};
	gpci.stageCount = ARRAY_LEN(stages);
	gpci.pStages = stages;
	gpci.layout = layout;
	assert(vkCreateGraphicsPipelines(vk->dev, VK_NULL_HANDLE, 1, &gpci, NULL,
		pipeline) == VK_SUCCESS);
	vkDestroyShaderModule(vk->dev, vert, NULL);
	vkDestroyShaderModule(vk->dev, frag, NULL);
}

// The pipelines draw into the images that we scan out, and into the blend
// buffers and textures.
static const VkFormat render_formats[NR_RENDER_FORMATS] = {
	VK_FORMAT_B8G8R8A8_UNORM,
	VK_FORMAT_A2R10G10B10_UNORM_PACK32,
	VK_FORMAT_R16G16B16A16_SFLOAT,
};

const struct pipelines *
vulkan_pipelines(VkFormat format)
{
	for (size_t i = 0; i < ARRAY_LEN(core.vk.pipelines); i++) {
		if (core.vk.pipelines[i].format == format) {
			return &core.vk.pipelines[i];
		}
	}
	assert(!"cannot draw into the format");
	return NULL;
}

// also creates the descriptor set layout and sampler (TODO: split it?)
static void
init_pipelines(void)
{
	struct vk *vk = &core.vk;

	// The texture, or the luma and the chroma planes of YCbCr
	VkDescriptorSetLayoutBinding bindings[TEXTURE_BINDINGS];
	for (uint32_t i = 0; i < TEXTURE_BINDINGS; i++) {
		bindings[i] = (VkDescriptorSetLayoutBinding){
			.binding = i,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.descriptorCount = 1,
			.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
		};
	}
	VkDescriptorSetLayoutCreateInfo dlci = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
		.bindingCount = TEXTURE_BINDINGS,
		.pBindings = bindings,
	};
	assert(vkCreateDescriptorSetLayout(vk->dev, &dlci, NULL,
		&vk->descriptor_set_layout) == VK_SUCCESS);

	VkSamplerCreateInfo sci = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_LINEAR,
		.minFilter = VK_FILTER_LINEAR,
		.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.maxLod = 1.0f,
	};
	assert(vkCreateSampler(vk->dev, &sci, NULL, &vk->sampler) == VK_SUCCESS);
	sci.magFilter = VK_FILTER_NEAREST;
	sci.minFilter = VK_FILTER_NEAREST;
	assert(vkCreateSampler(vk->dev, &sci, NULL, &vk->nearest_sampler) == VK_SUCCESS);

	VkPushConstantRange push_range = {
		.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
		.offset = 0,
		.size = sizeof(struct texture_push_constants),
	};
	VkPipelineLayoutCreateInfo plci = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &vk->descriptor_set_layout,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &push_range,
	};
	assert(vkCreatePipelineLayout(vk->dev, &plci, NULL,
		&vk->texture_pipe_layout) == VK_SUCCESS);
	VkPushConstantRange solid_push_range = {
		.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
		.offset = 0,
		.size = sizeof(struct solid_push_constants),
	};
	VkPipelineLayoutCreateInfo splci = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &solid_push_range,
	};
	assert(vkCreatePipelineLayout(vk->dev, &splci, NULL,
		&vk->solid_pipe_layout) == VK_SUCCESS);
	VkPushConstantRange shadow_push_range = {
		.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
		.offset = 0,
		.size = sizeof(struct shadow_push_constants),
	};
	VkPipelineLayoutCreateInfo shplci = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &shadow_push_range,
	};
	assert(vkCreatePipelineLayout(vk->dev, &shplci, NULL,
		&vk->shadow_pipe_layout) == VK_SUCCESS);

	VkPipelineVertexInputStateCreateInfo vertex = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
	};
	VkPipelineInputAssemblyStateCreateInfo assembly = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
	};
	VkPipelineViewportStateCreateInfo viewport = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1,
		.scissorCount = 1,
	};
	VkPipelineRasterizationStateCreateInfo raster = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = VK_CULL_MODE_NONE,
		.frontFace = VK_FRONT_FACE_CLOCKWISE,
		.lineWidth = 1.0f,
	};
	VkPipelineMultisampleStateCreateInfo ms = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
	};
	VkPipelineColorBlendAttachmentState blend_att = {
		.blendEnable = VK_TRUE,
		.srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
		.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
		.colorBlendOp = VK_BLEND_OP_ADD,
		.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
		.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
		.alphaBlendOp = VK_BLEND_OP_ADD,
		.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
			| VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
	};
	VkPipelineColorBlendStateCreateInfo blend = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &blend_att,
	};
	VkDynamicState dyns[] = {
		VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR,
		VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT,
	};
	VkPipelineDynamicStateCreateInfo dyn = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		.dynamicStateCount = ARRAY_LEN(dyns),
		.pDynamicStates = dyns,
	};
	VkPipelineRenderingCreateInfo rendering = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
		.colorAttachmentCount = 1,
	};
	VkGraphicsPipelineCreateInfo gpci = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.pNext = &rendering,
		.pVertexInputState = &vertex,
		.pInputAssemblyState = &assembly,
		.pViewportState = &viewport,
		.pRasterizationState = &raster,
		.pMultisampleState = &ms,
		.pColorBlendState = &blend,
		.pDynamicState = &dyn,
	};
	for (size_t i = 0; i < ARRAY_LEN(render_formats); i++) {
		struct pipelines *pipelines = &vk->pipelines[i];
		pipelines->format = render_formats[i];
		rendering.pColorAttachmentFormats = &pipelines->format;
		create_pipeline(gpci, texture_vert_spv, sizeof(texture_vert_spv),
			texture_frag_spv, sizeof(texture_frag_spv),
			vk->texture_pipe_layout, &pipelines->texture);
		create_pipeline(gpci, box_vert_spv, sizeof(box_vert_spv),
			solid_frag_spv, sizeof(solid_frag_spv), vk->solid_pipe_layout,
			&pipelines->solid);
		create_pipeline(gpci, box_vert_spv, sizeof(box_vert_spv),
			shadow_frag_spv, sizeof(shadow_frag_spv),
			vk->shadow_pipe_layout, &pipelines->shadow);
	}
}

void
vulkan_init(void)
{
	struct vk *vk = &core.vk;
	wl_list_init(&vk->texture_garbage);
	wl_list_init(&vk->uploads);
	wl_list_init(&vk->staging_buffers);
	init_device();
	init_pipelines();
}


void
vulkan_fini(void)
{
	struct vk *vk = &core.vk;
	formats_fini();
	for (size_t i = 0; i < ARRAY_LEN(vk->pipelines); i++) {
		vkDestroyPipeline(vk->dev, vk->pipelines[i].texture, NULL);
		vkDestroyPipeline(vk->dev, vk->pipelines[i].solid, NULL);
		vkDestroyPipeline(vk->dev, vk->pipelines[i].shadow, NULL);
	}
	vkDestroyPipelineLayout(vk->dev, vk->texture_pipe_layout, NULL);
	vkDestroyPipelineLayout(vk->dev, vk->solid_pipe_layout, NULL);
	vkDestroyPipelineLayout(vk->dev, vk->shadow_pipe_layout, NULL);
	vkDestroySampler(vk->dev, vk->sampler, NULL);
	vkDestroySampler(vk->dev, vk->nearest_sampler, NULL);
	vkDestroyDescriptorSetLayout(vk->dev, vk->descriptor_set_layout, NULL);
	vkDestroySemaphore(vk->dev, vk->timeline, NULL);
	vkDestroySemaphore(vk->dev, vk->submitted, NULL);
	if (vk->sync_file >= 0) {
		close(vk->sync_file);
	}
	vkDestroyCommandPool(vk->dev, vk->cmd_pool, NULL);
	vkDestroyDevice(vk->dev, NULL);
	vkDestroyInstance(vk->instance, NULL);
}
