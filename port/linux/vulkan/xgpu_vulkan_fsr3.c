/*
XGPU_VULKAN_FSR3.C

display.upscaler "fsr3" (xgpu_vulkan_fsr3.h): AMD's FidelityFX Super
Resolution 3.1 upscaler, its GLSL passes (port/third_party/fidelityfx-fsr3,
compiled by tools/vulkan_upscale_shaders.py) run by this file in the
backend's commands. Its host half is written after the SDK's
ffx_fsr3upscaler.cpp (FidelityFX SDK v1.1.4; the passes' order and
dispatches, their constants, the resources and their formats, which are
cleared when, the jitter's Halton sequence, RCAS's and SPD's constants), whose
licence follows. Every image is in the GENERAL layout, as the backend's are;
the passes are ordered by a barrier between each.

The resources (the render size's and the output's, made again when either
changes): the dilated motion and depth and the previous frame's nearest depth
(prepare inputs), the farthest depth and luma instability (one 16-bit image,
as the SDK aliases them), the luma of this frame and the last, its pyramid
(SPD's mips, a level of the farthest depth's), the shading change, the
dilated reactive masks, the new locks, the accumulation and the internal
upscaled colour (two of each, read and written in turn), the luma's history
(RGBA8, as the GLSL declares it), the frame's information (the luma's
exposure), the default exposure (none: 1) and transparency mask (none).

Copyright (C) 2024 Advanced Micro Devices, Inc.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files(the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions :

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include "xgpu_vulkan_fsr3.h"
#include "xgpu_vulkan_upscale_shaders.h"
#include "host.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FSR3_FUNCTIONS(F) \
	F(vkCreateImage) F(vkDestroyImage) F(vkGetImageMemoryRequirements) F(vkAllocateMemory) F(vkFreeMemory) \
	F(vkBindImageMemory) F(vkCreateImageView) F(vkDestroyImageView) F(vkCreateSampler) F(vkDestroySampler) \
	F(vkCreateDescriptorSetLayout) F(vkDestroyDescriptorSetLayout) F(vkCreatePipelineLayout) \
	F(vkDestroyPipelineLayout) F(vkCreateShaderModule) F(vkDestroyShaderModule) F(vkCreateComputePipelines) \
	F(vkDestroyPipeline) F(vkCreateDescriptorPool) F(vkDestroyDescriptorPool) F(vkResetDescriptorPool) \
	F(vkAllocateDescriptorSets) F(vkUpdateDescriptorSets) F(vkCmdBindPipeline) F(vkCmdBindDescriptorSets) \
	F(vkCmdDispatch) F(vkCmdPipelineBarrier) F(vkCmdClearColorImage) F(vkDeviceWaitIdle)

#define DECLARE(name) static PFN_##name name;
FSR3_FUNCTIONS(DECLARE)
#undef DECLARE

/* the frames a pass's descriptors are kept for (the frames in flight and one
more), and a frame's sets at most */
#define FSR3_SLOTS 3
#define FSR3_SETS_A_FRAME 9
/* the bindings of the samplers (the GLSL's s_PointClamp, s_LinearClamp) */
#define BINDING_POINT 1000
#define BINDING_LINEAR 1001

/* the resources a pass binds */
enum
{
	R_INPUT_COLOR, R_INPUT_DEPTH, R_INPUT_MOTION, R_INPUT_REACTIVE, R_INPUT_TCR, R_INPUT_EXPOSURE, R_OUTPUT,
	R_DILATED_MOTION, R_DILATED_DEPTH, R_RECONSTRUCTED_DEPTH, R_INTERMEDIATE, R_CURRENT_LUMA, R_PREVIOUS_LUMA,
	R_SPD_ATOMIC, R_FRAME_INFO, R_SPD_MIPS, R_SPD_MIP0, R_SPD_MIP1, R_SPD_MIP2, R_SPD_MIP3, R_SPD_MIP4, R_SPD_MIP5,
	R_FARTHEST_DEPTH_MIP1, R_SHADING_CHANGE, R_DILATED_REACTIVE, R_NEW_LOCKS, R_ACCUMULATION_READ,
	R_ACCUMULATION_WRITE, R_LUMA_HISTORY_READ, R_LUMA_HISTORY_WRITE, R_UPSCALED_READ, R_UPSCALED_WRITE,
	R_CB_MAIN, R_CB_SPD, R_CB_RCAS,
	R_COUNT
};

/* how: sampled, written (a storage image), or a uniform block */
enum
{
	BIND_SAMPLED, BIND_STORAGE, BIND_UNIFORM,
};

struct pass_binding
{
	uint8_t binding, resource, how;
};

#define S(binding, resource) { binding, resource, BIND_SAMPLED }
#define U(binding, resource) { binding, resource, BIND_STORAGE }
#define C(binding, resource) { binding, resource, BIND_UNIFORM }

/* each pass's bindings (its pass file's FSR3UPSCALER_BIND_*), ended by one
of resource R_COUNT */
static const struct pass_binding pass_prepare_inputs[] =
{
	S(0, R_INPUT_MOTION), S(1, R_INPUT_DEPTH), S(2, R_INPUT_COLOR), U(3, R_DILATED_MOTION), U(4, R_DILATED_DEPTH),
	U(5, R_RECONSTRUCTED_DEPTH), U(6, R_INTERMEDIATE), U(7, R_CURRENT_LUMA), C(8, R_CB_MAIN), { 0, R_COUNT, 0 },
};
static const struct pass_binding pass_luma_pyramid[] =
{
	S(0, R_CURRENT_LUMA), S(1, R_INTERMEDIATE), U(2, R_SPD_ATOMIC), U(3, R_FRAME_INFO), U(4, R_SPD_MIP0),
	U(5, R_SPD_MIP1), U(6, R_SPD_MIP2), U(7, R_SPD_MIP3), U(8, R_SPD_MIP4), U(9, R_SPD_MIP5),
	U(10, R_FARTHEST_DEPTH_MIP1), C(11, R_CB_MAIN), C(12, R_CB_SPD), { 0, R_COUNT, 0 },
};
static const struct pass_binding pass_shading_change_pyramid[] =
{
	S(0, R_CURRENT_LUMA), S(1, R_PREVIOUS_LUMA), S(2, R_DILATED_MOTION), S(3, R_INPUT_EXPOSURE), U(4, R_SPD_ATOMIC),
	U(5, R_SPD_MIP0), U(6, R_SPD_MIP1), U(7, R_SPD_MIP2), U(8, R_SPD_MIP3), U(9, R_SPD_MIP4), U(10, R_SPD_MIP5),
	C(11, R_CB_MAIN), C(12, R_CB_SPD), { 0, R_COUNT, 0 },
};
static const struct pass_binding pass_shading_change[] =
{
	S(0, R_SPD_MIPS), U(1, R_SHADING_CHANGE), C(2, R_CB_MAIN), { 0, R_COUNT, 0 },
};
static const struct pass_binding pass_prepare_reactivity[] =
{
	S(0, R_RECONSTRUCTED_DEPTH), S(1, R_DILATED_MOTION), S(2, R_DILATED_DEPTH), S(3, R_INPUT_REACTIVE),
	S(4, R_INPUT_TCR), S(5, R_ACCUMULATION_READ), S(6, R_SHADING_CHANGE), S(7, R_CURRENT_LUMA),
	S(8, R_INPUT_EXPOSURE), U(9, R_DILATED_REACTIVE), U(10, R_NEW_LOCKS), U(11, R_ACCUMULATION_WRITE),
	C(12, R_CB_MAIN), { 0, R_COUNT, 0 },
};
static const struct pass_binding pass_luma_instability[] =
{
	S(0, R_INPUT_EXPOSURE), S(1, R_DILATED_REACTIVE), S(2, R_DILATED_MOTION), S(3, R_FRAME_INFO),
	S(4, R_LUMA_HISTORY_READ), S(5, R_FARTHEST_DEPTH_MIP1), S(6, R_CURRENT_LUMA), U(7, R_LUMA_HISTORY_WRITE),
	U(8, R_INTERMEDIATE), C(9, R_CB_MAIN), { 0, R_COUNT, 0 },
};
/* (binding 4, the Lanczos table, is not read with the reference Lanczos:
the transparency mask's stand-in there) */
static const struct pass_binding pass_accumulate[] =
{
	S(0, R_INPUT_EXPOSURE), S(1, R_DILATED_REACTIVE), S(2, R_DILATED_MOTION), S(3, R_UPSCALED_READ),
	S(4, R_INPUT_TCR), S(5, R_FARTHEST_DEPTH_MIP1), S(6, R_CURRENT_LUMA), S(7, R_INTERMEDIATE), S(8, R_INPUT_COLOR),
	U(9, R_UPSCALED_WRITE), U(10, R_OUTPUT), U(11, R_NEW_LOCKS), C(12, R_CB_MAIN), { 0, R_COUNT, 0 },
};
/* (with sharpening the accumulation writes no output: the pass's binding
10 is still declared, and given it) */
static const struct pass_binding pass_rcas[] =
{
	S(0, R_INPUT_EXPOSURE), S(1, R_UPSCALED_WRITE), U(2, R_OUTPUT), C(3, R_CB_MAIN), C(4, R_CB_RCAS),
	{ 0, R_COUNT, 0 },
};

#undef S
#undef U
#undef C

/* the passes, in xgpu_vulkan_upscale_shaders.h's order */
enum
{
	PASS_PREPARE_INPUTS, PASS_LUMA_PYRAMID, PASS_SHADING_CHANGE_PYRAMID, PASS_SHADING_CHANGE,
	PASS_PREPARE_REACTIVITY, PASS_LUMA_INSTABILITY, PASS_ACCUMULATE, PASS_ACCUMULATE_SHARPEN, PASS_RCAS,
	PASS_COUNT
};

static const struct pass_binding *const pass_bindings[PASS_COUNT] =
{
	pass_prepare_inputs, pass_luma_pyramid, pass_shading_change_pyramid, pass_shading_change,
	pass_prepare_reactivity, pass_luma_instability, pass_accumulate, pass_accumulate, pass_rcas,
};

/* the upscaler's constants (the GLSL's cbFSR3Upscaler, std140; the SDK's
Fsr3UpscalerConstants) */
struct fsr3_constants
{
	int32_t render_size[2], previous_render_size[2];
	int32_t upscale_size[2], previous_upscale_size[2];
	int32_t max_render_size[2], max_upscale_size[2];
	float device_to_view_depth[4];
	float jitter[2], previous_jitter[2];
	float motion_vector_scale[2], downscale_factor[2];
	float motion_vector_jitter_cancellation[2];
	float tan_half_fov, jitter_phase_count;
	float delta_time, delta_pre_exposure, view_space_to_meters, frame_index;
	float velocity_factor, reactiveness_scale, shading_change_scale, accumulation_added_per_frame;
	float min_disocclusion_accumulation;
	float pad[3];
};
_Static_assert(sizeof(struct fsr3_constants) == 160, "cbFSR3Upscaler's std140 layout");

/* SPD's (cbSPD) and RCAS's (cbRCAS) */
struct fsr3_spd_constants
{
	uint32_t mips, work_groups, work_group_offset[2], render_size[2], pad[2];
};
struct fsr3_rcas_constants
{
	uint32_t config[4];
};

struct fsr3_image
{
	VkImage image;
	VkDeviceMemory memory;
	VkImageView view;
	/* (SPD's mips: each level's own, written) */
	VkImageView levels[6];
	uint32_t level_count;
};

/* the resources the upscaler makes (R_* index the bound ones) */
enum
{
	I_DILATED_MOTION, I_DILATED_DEPTH, I_RECONSTRUCTED_DEPTH, I_INTERMEDIATE, I_LUMA_1, I_LUMA_2, I_SPD_ATOMIC,
	I_FRAME_INFO, I_SPD_MIPS, I_FARTHEST_DEPTH_MIP1, I_SHADING_CHANGE, I_DILATED_REACTIVE, I_NEW_LOCKS,
	I_ACCUMULATION_1, I_ACCUMULATION_2, I_LUMA_HISTORY_1, I_LUMA_HISTORY_2, I_UPSCALED_1, I_UPSCALED_2,
	I_DEFAULT_EXPOSURE, I_DEFAULT_ZERO,
	I_COUNT
};

static struct
{
	/* made (the passes), failed (logged once: not tried again) */
	int made, failed;
	struct xgpu_vulkan_handles handles;
	PFN_vkGetPhysicalDeviceFormatProperties format_properties;
	VkDescriptorSetLayout set_layouts[PASS_COUNT];
	VkPipelineLayout pipeline_layouts[PASS_COUNT];
	VkPipeline pipelines[PASS_COUNT];
	VkSampler point, linear;
	VkDescriptorPool pools[FSR3_SLOTS];
	uint64_t pool_serials[FSR3_SLOTS];
	uint32_t slot;
	/* the resources, of these sizes (0: none) */
	struct fsr3_image images[I_COUNT];
	uint32_t render_width, render_height, output_width, output_height;
	/* the state between frames (the SDK's context's) */
	struct fsr3_constants constants;
	int first;
	uint32_t frame;
} fsr3;

/* ---------- the jitter */

/* the Halton sequence's index-th number of a base */
static float halton(int32_t index, int32_t base)
{
	float f = 1.0f, result = 0.0f;
	int32_t current;

	for (current = index; current > 0;)
	{
		f /= (float)base;
		result += f * (float)(current % base);
		current = (int32_t)floorf((float)current / (float)base);
	}
	return result;
}

uint32_t xgpu_vulkan_fsr3_jitter_phases(uint32_t render_width, uint32_t output_width)
{
	float ratio = render_width ? (float)output_width / (float)render_width : 1.0f;

	return (uint32_t)(8.0f * ratio * ratio);
}

void xgpu_vulkan_fsr3_jitter(uint32_t index, uint32_t phases, float jitter[2])
{
	if (!phases)
		phases = 1;
	jitter[0] = halton((int32_t)(index % phases) + 1, 2) - 0.5f;
	jitter[1] = halton((int32_t)(index % phases) + 1, 3) - 0.5f;
}

/* ---------- made once */

static int failed(const char *what)
{
	host_logf(HOST_LOG_WARN, "fsr3: %s: the view is drawn without the upscaler", what);
	fsr3.failed = 1;
	return 0;
}

static int memory_type(uint32_t bits, VkMemoryPropertyFlags wanted)
{
	uint32_t index;

	for (index = 0; index < fsr3.handles.memory->memoryTypeCount; index++)
	{
		if ((bits & (1u << index)) && (fsr3.handles.memory->memoryTypes[index].propertyFlags & wanted) == wanted)
			return (int)index;
	}
	return -1;
}

static int make(void)
{
	static const VkFormat linear_formats[] =
	{
		VK_FORMAT_R32_SFLOAT, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16_SFLOAT, VK_FORMAT_R8_UNORM,
		VK_FORMAT_R16G16B16A16_SFLOAT,
	};
	VkPhysicalDeviceSubgroupProperties subgroups;
	VkPhysicalDeviceProperties2 properties;
	PFN_vkGetPhysicalDeviceProperties2 get_properties2;
	VkSamplerCreateInfo sampler;
	VkDescriptorPoolSize sizes[4];
	VkDescriptorPoolCreateInfo pool_info;
	uint32_t pass, index;

	if (fsr3.made)
		return 1;
	if (fsr3.failed || !xgpu_vulkan_handles(&fsr3.handles))
		return 0;
#define LOAD(name) name = (PFN_##name)fsr3.handles.get_device_proc(fsr3.handles.device, #name); \
	if (!name) return failed("the device has no " #name);
	FSR3_FUNCTIONS(LOAD)
#undef LOAD
	fsr3.format_properties = (PFN_vkGetPhysicalDeviceFormatProperties)fsr3.handles.get_instance_proc(
		fsr3.handles.instance, "vkGetPhysicalDeviceFormatProperties");
	get_properties2 = (PFN_vkGetPhysicalDeviceProperties2)fsr3.handles.get_instance_proc(fsr3.handles.instance,
		"vkGetPhysicalDeviceProperties2");
	if (!fsr3.format_properties || !get_properties2)
		return failed("the instance has no vkGetPhysicalDeviceFormatProperties");
	/* (SPD's quad operations in compute shaders, and the formats sampled
	linearly) */
	memset(&subgroups, 0, sizeof(subgroups));
	subgroups.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
	memset(&properties, 0, sizeof(properties));
	properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
	properties.pNext = &subgroups;
	get_properties2(fsr3.handles.physical, &properties);
	if (!(subgroups.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) ||
		!(subgroups.supportedOperations & VK_SUBGROUP_FEATURE_QUAD_BIT))
		return failed("the device has no subgroups' quad operations in compute shaders");
	for (index = 0; index < sizeof(linear_formats) / sizeof(linear_formats[0]); index++)
	{
		VkFormatProperties format;

		fsr3.format_properties(fsr3.handles.physical, linear_formats[index], &format);
		if (!(format.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ||
			!(format.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))
			return failed("the device cannot sample or write one of its formats");
	}
	for (pass = 0; pass < PASS_COUNT; pass++)
	{
		const struct pass_binding *binding;
		VkDescriptorSetLayoutBinding bindings[20];
		VkDescriptorSetLayoutCreateInfo set_info;
		VkPipelineLayoutCreateInfo layout_info;
		VkShaderModuleCreateInfo module_info;
		VkComputePipelineCreateInfo info;
		VkShaderModule module;
		uint32_t count = 0;
		VkResult result;

		memset(bindings, 0, sizeof(bindings));
		for (binding = pass_bindings[pass]; binding->resource != R_COUNT; binding++)
		{
			bindings[count].binding = binding->binding;
			bindings[count].descriptorType = binding->how == BIND_SAMPLED ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE :
				binding->how == BIND_STORAGE ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
			bindings[count].descriptorCount = 1;
			bindings[count].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
			count++;
		}
		bindings[count].binding = BINDING_POINT;
		bindings[count].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
		bindings[count].descriptorCount = 1;
		bindings[count].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		count++;
		bindings[count] = bindings[count - 1];
		bindings[count].binding = BINDING_LINEAR;
		count++;
		memset(&set_info, 0, sizeof(set_info));
		set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		set_info.bindingCount = count;
		set_info.pBindings = bindings;
		if (vkCreateDescriptorSetLayout(fsr3.handles.device, &set_info, NULL, &fsr3.set_layouts[pass]) != VK_SUCCESS)
			return failed("its sets' layouts cannot be made");
		memset(&layout_info, 0, sizeof(layout_info));
		layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		layout_info.setLayoutCount = 1;
		layout_info.pSetLayouts = &fsr3.set_layouts[pass];
		if (vkCreatePipelineLayout(fsr3.handles.device, &layout_info, NULL, &fsr3.pipeline_layouts[pass]) != VK_SUCCESS)
			return failed("its pipelines' layouts cannot be made");
		memset(&module_info, 0, sizeof(module_info));
		module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		module_info.codeSize = xgpu_vulkan_upscale_programs[pass].count * sizeof(uint32_t);
		module_info.pCode = xgpu_vulkan_upscale_programs[pass].words;
		if (vkCreateShaderModule(fsr3.handles.device, &module_info, NULL, &module) != VK_SUCCESS)
			return failed("its shaders cannot be made");
		memset(&info, 0, sizeof(info));
		info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
		info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
		info.stage.module = module;
		info.stage.pName = "main";
		info.layout = fsr3.pipeline_layouts[pass];
		result = vkCreateComputePipelines(fsr3.handles.device, VK_NULL_HANDLE, 1, &info, NULL, &fsr3.pipelines[pass]);
		vkDestroyShaderModule(fsr3.handles.device, module, NULL);
		if (result != VK_SUCCESS)
			return failed("its pipelines cannot be made");
	}
	memset(&sampler, 0, sizeof(sampler));
	sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
	sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.maxLod = VK_LOD_CLAMP_NONE;
	if (vkCreateSampler(fsr3.handles.device, &sampler, NULL, &fsr3.point) != VK_SUCCESS)
		return failed("its samplers cannot be made");
	sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
	sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	if (vkCreateSampler(fsr3.handles.device, &sampler, NULL, &fsr3.linear) != VK_SUCCESS)
		return failed("its samplers cannot be made");
	memset(sizes, 0, sizeof(sizes));
	sizes[0].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	sizes[0].descriptorCount = FSR3_SETS_A_FRAME * 12;
	sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	sizes[1].descriptorCount = FSR3_SETS_A_FRAME * 10;
	sizes[2].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	sizes[2].descriptorCount = FSR3_SETS_A_FRAME * 2;
	sizes[3].type = VK_DESCRIPTOR_TYPE_SAMPLER;
	sizes[3].descriptorCount = FSR3_SETS_A_FRAME * 2;
	memset(&pool_info, 0, sizeof(pool_info));
	pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_info.maxSets = FSR3_SETS_A_FRAME;
	pool_info.poolSizeCount = 4;
	pool_info.pPoolSizes = sizes;
	for (index = 0; index < FSR3_SLOTS; index++)
	{
		if (vkCreateDescriptorPool(fsr3.handles.device, &pool_info, NULL, &fsr3.pools[index]) != VK_SUCCESS)
			return failed("its descriptor pools cannot be made");
	}
	fsr3.made = 1;
	host_logf(HOST_LOG_INFO, "fsr3: FidelityFX Super Resolution 3.1.4's passes made");
	return 1;
}

/* ---------- the resources */

static void image_release(struct fsr3_image *image)
{
	uint32_t level;

	if (image->view)
		vkDestroyImageView(fsr3.handles.device, image->view, NULL);
	for (level = 0; level < 6; level++)
	{
		if (image->levels[level])
			vkDestroyImageView(fsr3.handles.device, image->levels[level], NULL);
	}
	if (image->image)
		vkDestroyImage(fsr3.handles.device, image->image, NULL);
	if (image->memory)
		vkFreeMemory(fsr3.handles.device, image->memory, NULL);
	memset(image, 0, sizeof(*image));
}

static int image_make(struct fsr3_image *made, VkFormat format, uint32_t width, uint32_t height, uint32_t levels)
{
	VkImageCreateInfo info;
	VkMemoryRequirements requirements;
	VkMemoryAllocateInfo allocate;
	VkImageViewCreateInfo view;
	uint32_t level;
	int type;

	memset(made, 0, sizeof(*made));
	if (!width)
		width = 1;
	if (!height)
		height = 1;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	info.imageType = VK_IMAGE_TYPE_2D;
	info.format = format;
	info.extent.width = width;
	info.extent.height = height;
	info.extent.depth = 1;
	info.mipLevels = levels;
	info.arrayLayers = 1;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (vkCreateImage(fsr3.handles.device, &info, NULL, &made->image) != VK_SUCCESS)
		return 0;
	vkGetImageMemoryRequirements(fsr3.handles.device, made->image, &requirements);
	type = memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (type < 0)
		type = memory_type(requirements.memoryTypeBits, 0);
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = (uint32_t)type;
	if (type < 0 || vkAllocateMemory(fsr3.handles.device, &allocate, NULL, &made->memory) != VK_SUCCESS ||
		vkBindImageMemory(fsr3.handles.device, made->image, made->memory, 0) != VK_SUCCESS)
	{
		image_release(made);
		return 0;
	}
	memset(&view, 0, sizeof(view));
	view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view.image = made->image;
	view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view.format = format;
	view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view.subresourceRange.levelCount = levels;
	view.subresourceRange.layerCount = 1;
	if (vkCreateImageView(fsr3.handles.device, &view, NULL, &made->view) != VK_SUCCESS)
	{
		image_release(made);
		return 0;
	}
	made->level_count = levels;
	if (levels > 1)
	{
		for (level = 0; level < 6 && level < levels; level++)
		{
			view.subresourceRange.baseMipLevel = level;
			view.subresourceRange.levelCount = 1;
			if (vkCreateImageView(fsr3.handles.device, &view, NULL, &made->levels[level]) != VK_SUCCESS)
			{
				image_release(made);
				return 0;
			}
		}
	}
	return 1;
}

static void resources_release(void)
{
	uint32_t index;

	for (index = 0; index < I_COUNT; index++)
		image_release(&fsr3.images[index]);
	fsr3.render_width = fsr3.render_height = fsr3.output_width = fsr3.output_height = 0;
}

/* the resources of these sizes (the device idle as they are made again) */
static int resources_fit(VkCommandBuffer commands, uint32_t render_width, uint32_t render_height,
	uint32_t output_width, uint32_t output_height)
{
	static const struct
	{
		uint8_t image;
		/* 0 the render size, 1 its half, 2 the output size, 3 one texel */
		uint8_t size;
		VkFormat format;
	} kinds[I_COUNT] =
	{
		{ I_DILATED_MOTION, 0, VK_FORMAT_R16G16_SFLOAT },
		{ I_DILATED_DEPTH, 0, VK_FORMAT_R32_SFLOAT },
		{ I_RECONSTRUCTED_DEPTH, 0, VK_FORMAT_R32_UINT },
		{ I_INTERMEDIATE, 0, VK_FORMAT_R16_SFLOAT },
		{ I_LUMA_1, 0, VK_FORMAT_R16_SFLOAT },
		{ I_LUMA_2, 0, VK_FORMAT_R16_SFLOAT },
		{ I_SPD_ATOMIC, 3, VK_FORMAT_R32_UINT },
		{ I_FRAME_INFO, 3, VK_FORMAT_R32G32B32A32_SFLOAT },
		{ I_SPD_MIPS, 1, VK_FORMAT_R16G16_SFLOAT },
		{ I_FARTHEST_DEPTH_MIP1, 1, VK_FORMAT_R16_SFLOAT },
		{ I_SHADING_CHANGE, 1, VK_FORMAT_R8_UNORM },
		{ I_DILATED_REACTIVE, 0, VK_FORMAT_R8G8B8A8_UNORM },
		{ I_NEW_LOCKS, 2, VK_FORMAT_R8_UNORM },
		{ I_ACCUMULATION_1, 0, VK_FORMAT_R8_UNORM },
		{ I_ACCUMULATION_2, 0, VK_FORMAT_R8_UNORM },
		{ I_LUMA_HISTORY_1, 0, VK_FORMAT_R8G8B8A8_UNORM },
		{ I_LUMA_HISTORY_2, 0, VK_FORMAT_R8G8B8A8_UNORM },
		{ I_UPSCALED_1, 2, VK_FORMAT_R16G16B16A16_SFLOAT },
		{ I_UPSCALED_2, 2, VK_FORMAT_R16G16B16A16_SFLOAT },
		{ I_DEFAULT_EXPOSURE, 3, VK_FORMAT_R32G32_SFLOAT },
		{ I_DEFAULT_ZERO, 3, VK_FORMAT_R8_UNORM },
	};
	VkImageMemoryBarrier barriers[I_COUNT];
	VkClearColorValue zero;
	VkImageSubresourceRange range;
	uint32_t index;

	if (fsr3.render_width == render_width && fsr3.render_height == render_height &&
		fsr3.output_width == output_width && fsr3.output_height == output_height)
		return 1;
	if (fsr3.render_width)
		vkDeviceWaitIdle(fsr3.handles.device);
	resources_release();
	for (index = 0; index < I_COUNT; index++)
	{
		uint32_t width = kinds[index].size == 0 ? render_width : kinds[index].size == 1 ? render_width / 2 :
			kinds[index].size == 2 ? output_width : 1;
		uint32_t height = kinds[index].size == 0 ? render_height : kinds[index].size == 1 ? render_height / 2 :
			kinds[index].size == 2 ? output_height : 1;
		uint32_t levels = 1;

		/* (SPD's mips: the whole chain of the render size's half, at least
		the six it writes) */
		if (kinds[index].image == I_SPD_MIPS)
		{
			uint32_t most = width > height ? width : height;

			for (levels = 1; most > 1; most >>= 1)
				levels++;
			if (levels < 6)
				levels = 6;
		}
		if (!image_make(&fsr3.images[kinds[index].image], kinds[index].format, width, height, levels))
		{
			resources_release();
			return failed("its images cannot be made");
		}
	}
	/* every image GENERAL, and emptied (the SDK clears the accumulation and
	the luma as it first runs; the defaults are zero: exposure 1, no mask) */
	memset(barriers, 0, sizeof(barriers));
	for (index = 0; index < I_COUNT; index++)
	{
		barriers[index].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barriers[index].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barriers[index].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		barriers[index].newLayout = VK_IMAGE_LAYOUT_GENERAL;
		barriers[index].srcQueueFamilyIndex = barriers[index].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barriers[index].image = fsr3.images[index].image;
		barriers[index].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barriers[index].subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
		barriers[index].subresourceRange.layerCount = 1;
	}
	vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
		NULL, I_COUNT, barriers);
	memset(&zero, 0, sizeof(zero));
	memset(&range, 0, sizeof(range));
	range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	range.levelCount = VK_REMAINING_MIP_LEVELS;
	range.layerCount = 1;
	for (index = 0; index < I_COUNT; index++)
		vkCmdClearColorImage(commands, fsr3.images[index].image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
	fsr3.render_width = render_width;
	fsr3.render_height = render_height;
	fsr3.output_width = output_width;
	fsr3.output_height = output_height;
	fsr3.first = 1;
	memset(&fsr3.constants, 0, sizeof(fsr3.constants));
	return 1;
}

/* ---------- a frame */

static void clear_image(VkCommandBuffer commands, uint32_t image, float value)
{
	VkClearColorValue color;
	VkImageSubresourceRange range;

	color.float32[0] = color.float32[1] = color.float32[2] = color.float32[3] = value;
	memset(&range, 0, sizeof(range));
	range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	range.levelCount = VK_REMAINING_MIP_LEVELS;
	range.layerCount = 1;
	vkCmdClearColorImage(commands, fsr3.images[image].image, VK_IMAGE_LAYOUT_GENERAL, &color, 1, &range);
}

static void barrier(VkCommandBuffer commands, VkPipelineStageFlags from, VkAccessFlags from_access)
{
	VkMemoryBarrier memory;

	memset(&memory, 0, sizeof(memory));
	memory.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	memory.srcAccessMask = from_access;
	memory.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
		VK_ACCESS_TRANSFER_WRITE_BIT;
	vkCmdPipelineBarrier(commands, from, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1,
		&memory, 0, NULL, 0, NULL);
}

/* half of a float's bits (RCAS's packed sharpness) */
static uint32_t half_bits(float value)
{
	uint32_t bits, sign, exponent, mantissa;

	memcpy(&bits, &value, sizeof(bits));
	sign = (bits >> 16) & 0x8000u;
	exponent = (bits >> 23) & 0xffu;
	mantissa = bits & 0x7fffffu;
	if (exponent < 113)
		return sign;
	if (exponent > 142)
		return sign | 0x7c00u;
	return sign | ((exponent - 112) << 10) | (mantissa >> 13);
}

static void dispatch(VkCommandBuffer commands, int pass, VkDescriptorSet set, uint32_t x, uint32_t y)
{
	vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, fsr3.pipelines[pass]);
	vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, fsr3.pipeline_layouts[pass], 0, 1, &set, 0,
		NULL);
	vkCmdDispatch(commands, x, y, 1);
	barrier(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
}

int xgpu_vulkan_fsr3_dispatch(const struct xgpu_vulkan_fsr3_frame *frame)
{
	VkImageView views[R_COUNT];
	VkDescriptorBufferInfo buffers[3];
	struct xgpu_vulkan_range ranges[3];
	struct fsr3_constants *constants;
	struct fsr3_spd_constants *spd;
	struct fsr3_rcas_constants *rcas;
	VkDescriptorSetLayout layouts[PASS_COUNT];
	VkDescriptorSet sets[PASS_COUNT];
	VkDescriptorSetAllocateInfo allocate;
	VkCommandBuffer commands = frame->commands;
	uint32_t pass, odd, phases, src_x, src_y, dst_x, dst_y, spd_groups[2], spd_offset[2], spd_mips, resolution;
	int reset, sharpen = frame->sharpness > 0.0f;
	float aspect, horizontal;

	if (!make() || !frame->color || !frame->depth || !frame->motion || !frame->reactive || !frame->output ||
		!frame->render_width || !frame->render_height || !frame->output_width || !frame->output_height)
		return 0;
	if (!resources_fit(commands, frame->render_width, frame->render_height, frame->output_width,
		frame->output_height))
		return 0;
	/* (this slot's sets free: its frame's commands done) */
	fsr3.slot = (fsr3.slot + 1) % FSR3_SLOTS;
	if (fsr3.pool_serials[fsr3.slot] && xgpu_vulkan_completed() < fsr3.pool_serials[fsr3.slot])
		xgpu_vulkan_wait(fsr3.pool_serials[fsr3.slot]);
	vkResetDescriptorPool(fsr3.handles.device, fsr3.pools[fsr3.slot], 0);
	fsr3.pool_serials[fsr3.slot] = xgpu_vulkan_serial();
	odd = fsr3.frame & 1u;
	reset = frame->reset || fsr3.first;

	/* the constants (the SDK's fsr3upscalerDispatch) */
	constants = xgpu_vulkan_upload(sizeof(*constants), 256, &ranges[0]);
	spd = xgpu_vulkan_upload(sizeof(*spd), 256, &ranges[1]);
	rcas = xgpu_vulkan_upload(sizeof(*rcas), 256, &ranges[2]);
	if (!constants || !spd || !rcas)
		return 0;
	fsr3.constants.previous_jitter[0] = fsr3.constants.jitter[0];
	fsr3.constants.previous_jitter[1] = fsr3.constants.jitter[1];
	fsr3.constants.jitter[0] = frame->jitter[0];
	fsr3.constants.jitter[1] = frame->jitter[1];
	fsr3.constants.previous_render_size[0] = fsr3.constants.render_size[0];
	fsr3.constants.previous_render_size[1] = fsr3.constants.render_size[1];
	fsr3.constants.render_size[0] = (int32_t)frame->render_width;
	fsr3.constants.render_size[1] = (int32_t)frame->render_height;
	fsr3.constants.max_render_size[0] = (int32_t)frame->render_width;
	fsr3.constants.max_render_size[1] = (int32_t)frame->render_height;
	fsr3.constants.max_upscale_size[0] = (int32_t)frame->output_width;
	fsr3.constants.max_upscale_size[1] = (int32_t)frame->output_height;
	aspect = (float)frame->render_width / (float)frame->render_height;
	horizontal = atanf(tanf(frame->field_of_view * 0.5f) * aspect) * 2.0f;
	fsr3.constants.tan_half_fov = tanf(horizontal * 0.5f);
	fsr3.constants.view_space_to_meters = frame->meters_per_unit > 0.0f ? frame->meters_per_unit : 1.0f;
	/* (reversed depth, not infinite: the SDK's
	setupDeviceDepthToViewSpaceDepthParams) */
	{
		float near_plane = frame->near_plane, far_plane = frame->far_plane;
		float q = near_plane / (far_plane - near_plane);
		float cot = cosf(0.5f * frame->field_of_view) / sinf(0.5f * frame->field_of_view);

		fsr3.constants.device_to_view_depth[0] = -q;
		fsr3.constants.device_to_view_depth[1] = q * far_plane;
		fsr3.constants.device_to_view_depth[2] = 1.0f / (cot / aspect);
		fsr3.constants.device_to_view_depth[3] = 1.0f / cot;
	}
	fsr3.constants.previous_upscale_size[0] = fsr3.constants.upscale_size[0];
	fsr3.constants.previous_upscale_size[1] = fsr3.constants.upscale_size[1];
	fsr3.constants.upscale_size[0] = (int32_t)frame->output_width;
	fsr3.constants.upscale_size[1] = (int32_t)frame->output_height;
	fsr3.constants.downscale_factor[0] = (float)frame->render_width / (float)frame->output_width;
	fsr3.constants.downscale_factor[1] = (float)frame->render_height / (float)frame->output_height;
	fsr3.constants.delta_pre_exposure = 1.0f;
	/* (the motion in pixels of the render size: the scale 1, over it) */
	fsr3.constants.motion_vector_scale[0] = 1.0f / (float)frame->render_width;
	fsr3.constants.motion_vector_scale[1] = 1.0f / (float)frame->render_height;
	fsr3.constants.motion_vector_jitter_cancellation[0] = fsr3.constants.motion_vector_jitter_cancellation[1] = 0.0f;
	phases = xgpu_vulkan_fsr3_jitter_phases(frame->render_width, frame->output_width);
	if (reset || fsr3.constants.jitter_phase_count == 0.0f)
		fsr3.constants.jitter_phase_count = (float)phases;
	else if ((float)phases > fsr3.constants.jitter_phase_count)
		fsr3.constants.jitter_phase_count += 1.0f;
	else if ((float)phases < fsr3.constants.jitter_phase_count)
		fsr3.constants.jitter_phase_count -= 1.0f;
	fsr3.constants.delta_time = frame->frame_milliseconds / 1000.0f;
	if (fsr3.constants.delta_time < 0.0f)
		fsr3.constants.delta_time = 0.0f;
	if (fsr3.constants.delta_time > 1.0f)
		fsr3.constants.delta_time = 1.0f;
	fsr3.constants.frame_index = reset ? 0.0f : fsr3.constants.frame_index + 1.0f;
	fsr3.constants.velocity_factor = 1.0f;
	fsr3.constants.reactiveness_scale = 1.0f;
	fsr3.constants.shading_change_scale = 1.0f;
	fsr3.constants.accumulation_added_per_frame = 1.0f / 3.0f;
	fsr3.constants.min_disocclusion_accumulation = -1.0f / 3.0f;
	*constants = fsr3.constants;
	/* (SPD over the render size: ffxSpdSetup's) */
	spd_offset[0] = spd_offset[1] = 0;
	spd_groups[0] = (frame->render_width - 1) / 64 + 1;
	spd_groups[1] = (frame->render_height - 1) / 64 + 1;
	resolution = frame->render_width > frame->render_height ? frame->render_width : frame->render_height;
	for (spd_mips = 0; resolution > 1 && spd_mips < 12; resolution >>= 1)
		spd_mips++;
	memset(spd, 0, sizeof(*spd));
	spd->mips = spd_mips;
	spd->work_groups = spd_groups[0] * spd_groups[1];
	spd->work_group_offset[0] = spd_offset[0];
	spd->work_group_offset[1] = spd_offset[1];
	spd->render_size[0] = frame->render_width;
	spd->render_size[1] = frame->render_height;
	/* (RCAS: the sharpness in stops, 0 the most: FsrRcasCon's) */
	{
		float stops = -2.0f * frame->sharpness + 2.0f, linear = exp2f(-stops);
		uint32_t half = half_bits(linear);

		memcpy(&rcas->config[0], &linear, sizeof(linear));
		rcas->config[1] = half | (half << 16);
		rcas->config[2] = rcas->config[3] = 0;
	}

	/* the views each pass binds (the frame's pair of each read and written in
	turn) */
	memset(views, 0, sizeof(views));
	views[R_INPUT_COLOR] = frame->color;
	views[R_INPUT_DEPTH] = frame->depth;
	views[R_INPUT_MOTION] = frame->motion;
	views[R_INPUT_REACTIVE] = frame->reactive;
	views[R_INPUT_TCR] = fsr3.images[I_DEFAULT_ZERO].view;
	views[R_INPUT_EXPOSURE] = fsr3.images[I_DEFAULT_EXPOSURE].view;
	views[R_OUTPUT] = frame->output;
	views[R_DILATED_MOTION] = fsr3.images[I_DILATED_MOTION].view;
	views[R_DILATED_DEPTH] = fsr3.images[I_DILATED_DEPTH].view;
	views[R_RECONSTRUCTED_DEPTH] = fsr3.images[I_RECONSTRUCTED_DEPTH].view;
	views[R_INTERMEDIATE] = fsr3.images[I_INTERMEDIATE].view;
	views[R_CURRENT_LUMA] = fsr3.images[odd ? I_LUMA_2 : I_LUMA_1].view;
	views[R_PREVIOUS_LUMA] = fsr3.images[odd ? I_LUMA_1 : I_LUMA_2].view;
	views[R_SPD_ATOMIC] = fsr3.images[I_SPD_ATOMIC].view;
	views[R_FRAME_INFO] = fsr3.images[I_FRAME_INFO].view;
	views[R_SPD_MIPS] = fsr3.images[I_SPD_MIPS].view;
	for (pass = 0; pass < 6; pass++)
		views[R_SPD_MIP0 + pass] = fsr3.images[I_SPD_MIPS].levels[pass];
	views[R_FARTHEST_DEPTH_MIP1] = fsr3.images[I_FARTHEST_DEPTH_MIP1].view;
	views[R_SHADING_CHANGE] = fsr3.images[I_SHADING_CHANGE].view;
	views[R_DILATED_REACTIVE] = fsr3.images[I_DILATED_REACTIVE].view;
	views[R_NEW_LOCKS] = fsr3.images[I_NEW_LOCKS].view;
	views[R_ACCUMULATION_READ] = fsr3.images[odd ? I_ACCUMULATION_2 : I_ACCUMULATION_1].view;
	views[R_ACCUMULATION_WRITE] = fsr3.images[odd ? I_ACCUMULATION_1 : I_ACCUMULATION_2].view;
	views[R_LUMA_HISTORY_READ] = fsr3.images[odd ? I_LUMA_HISTORY_2 : I_LUMA_HISTORY_1].view;
	views[R_LUMA_HISTORY_WRITE] = fsr3.images[odd ? I_LUMA_HISTORY_1 : I_LUMA_HISTORY_2].view;
	views[R_UPSCALED_READ] = fsr3.images[odd ? I_UPSCALED_2 : I_UPSCALED_1].view;
	views[R_UPSCALED_WRITE] = fsr3.images[odd ? I_UPSCALED_1 : I_UPSCALED_2].view;
	memset(buffers, 0, sizeof(buffers));
	buffers[0].buffer = ranges[0].buffer;
	buffers[0].offset = ranges[0].offset;
	buffers[0].range = sizeof(*constants);
	buffers[1].buffer = ranges[1].buffer;
	buffers[1].offset = ranges[1].offset;
	buffers[1].range = sizeof(*spd);
	buffers[2].buffer = ranges[2].buffer;
	buffers[2].offset = ranges[2].offset;
	buffers[2].range = sizeof(*rcas);

	/* the sets */
	for (pass = 0; pass < PASS_COUNT; pass++)
		layouts[pass] = fsr3.set_layouts[pass];
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocate.descriptorPool = fsr3.pools[fsr3.slot];
	allocate.descriptorSetCount = PASS_COUNT;
	allocate.pSetLayouts = layouts;
	if (vkAllocateDescriptorSets(fsr3.handles.device, &allocate, sets) != VK_SUCCESS)
		return failed("its descriptor sets cannot be had");
	for (pass = 0; pass < PASS_COUNT; pass++)
	{
		VkWriteDescriptorSet writes[24];
		VkDescriptorImageInfo images[24];
		const struct pass_binding *binding;
		uint32_t count = 0;

		memset(writes, 0, sizeof(writes));
		memset(images, 0, sizeof(images));
		for (binding = pass_bindings[pass]; binding->resource != R_COUNT; binding++)
		{
			writes[count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			writes[count].dstSet = sets[pass];
			writes[count].dstBinding = binding->binding;
			writes[count].descriptorCount = 1;
			if (binding->how == BIND_UNIFORM)
			{
				writes[count].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
				writes[count].pBufferInfo = &buffers[binding->resource - R_CB_MAIN];
			}
			else
			{
				writes[count].descriptorType = binding->how == BIND_SAMPLED ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE :
					VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
				images[count].imageView = views[binding->resource];
				images[count].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
				writes[count].pImageInfo = &images[count];
			}
			count++;
		}
		writes[count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[count].dstSet = sets[pass];
		writes[count].dstBinding = BINDING_POINT;
		writes[count].descriptorCount = 1;
		writes[count].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
		images[count].sampler = fsr3.point;
		writes[count].pImageInfo = &images[count];
		count++;
		writes[count] = writes[count - 1];
		writes[count].dstBinding = BINDING_LINEAR;
		images[count].sampler = fsr3.linear;
		writes[count].pImageInfo = &images[count];
		count++;
		vkUpdateDescriptorSets(fsr3.handles.device, count, writes, 0, NULL);
	}

	/* the clears (as the SDK schedules them: the history, the scene's
	luminance and the frame's information on a reset; the nearest depth of
	the frame before and SPD's counter each frame), after the images'
	first clear where they were just made */
	if (fsr3.first)
		barrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
	if (reset)
	{
		clear_image(commands, odd ? I_ACCUMULATION_2 : I_ACCUMULATION_1, 0.0f);
		clear_image(commands, I_SPD_MIPS, 0.0f);
		{
			VkClearColorValue info;
			VkImageSubresourceRange range;

			memset(&info, 0, sizeof(info));
			info.float32[0] = -1.0f;
			info.float32[1] = 1.0f;
			memset(&range, 0, sizeof(range));
			range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			range.levelCount = 1;
			range.layerCount = 1;
			vkCmdClearColorImage(commands, fsr3.images[I_FRAME_INFO].image, VK_IMAGE_LAYOUT_GENERAL, &info, 1, &range);
		}
	}
	/* (reversed depth: the nearest is cleared to 0, as uint bits) */
	clear_image(commands, I_RECONSTRUCTED_DEPTH, 0.0f);
	clear_image(commands, I_SPD_ATOMIC, 0.0f);
	barrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);

	/* the passes (ffx_fsr3upscaler.cpp's order and dispatches) */
	src_x = (frame->render_width + 7) / 8;
	src_y = (frame->render_height + 7) / 8;
	dst_x = (frame->output_width + 7) / 8;
	dst_y = (frame->output_height + 7) / 8;
	dispatch(commands, PASS_PREPARE_INPUTS, sets[PASS_PREPARE_INPUTS], src_x, src_y);
	dispatch(commands, PASS_LUMA_PYRAMID, sets[PASS_LUMA_PYRAMID], spd_groups[0], spd_groups[1]);
	dispatch(commands, PASS_SHADING_CHANGE_PYRAMID, sets[PASS_SHADING_CHANGE_PYRAMID], spd_groups[0], spd_groups[1]);
	dispatch(commands, PASS_SHADING_CHANGE, sets[PASS_SHADING_CHANGE], ((frame->render_width / 2) + 7) / 8,
		((frame->render_height / 2) + 7) / 8);
	dispatch(commands, PASS_PREPARE_REACTIVITY, sets[PASS_PREPARE_REACTIVITY], src_x, src_y);
	dispatch(commands, PASS_LUMA_INSTABILITY, sets[PASS_LUMA_INSTABILITY], src_x, src_y);
	dispatch(commands, sharpen ? PASS_ACCUMULATE_SHARPEN : PASS_ACCUMULATE,
		sets[sharpen ? PASS_ACCUMULATE_SHARPEN : PASS_ACCUMULATE], dst_x, dst_y);
	if (sharpen)
		dispatch(commands, PASS_RCAS, sets[PASS_RCAS], (frame->output_width + 15) / 16,
			(frame->output_height + 15) / 16);
	fsr3.first = 0;
	fsr3.frame++;
	return 1;
}

void xgpu_vulkan_fsr3_shutdown(void)
{
	uint32_t index;

	if (!fsr3.handles.device)
		return;
	resources_release();
	for (index = 0; index < PASS_COUNT; index++)
	{
		if (fsr3.pipelines[index])
			vkDestroyPipeline(fsr3.handles.device, fsr3.pipelines[index], NULL);
		if (fsr3.pipeline_layouts[index])
			vkDestroyPipelineLayout(fsr3.handles.device, fsr3.pipeline_layouts[index], NULL);
		if (fsr3.set_layouts[index])
			vkDestroyDescriptorSetLayout(fsr3.handles.device, fsr3.set_layouts[index], NULL);
	}
	for (index = 0; index < FSR3_SLOTS; index++)
	{
		if (fsr3.pools[index])
			vkDestroyDescriptorPool(fsr3.handles.device, fsr3.pools[index], NULL);
	}
	if (fsr3.point)
		vkDestroySampler(fsr3.handles.device, fsr3.point, NULL);
	if (fsr3.linear)
		vkDestroySampler(fsr3.handles.device, fsr3.linear, NULL);
	memset(&fsr3, 0, sizeof(fsr3));
}
