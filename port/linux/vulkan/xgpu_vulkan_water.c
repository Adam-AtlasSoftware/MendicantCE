/*
XGPU_VULKAN_WATER.C

The terrain overhaul's water layer, T7 (xgpu_vulkan_water.h; docs/prism_terrain.md "T7";
overhaul_research/prism_v4_renderer/terrain/design.md section 8): each CE water group's pixels composited by one
compute pass (xgpu_vulkan_water.glsl, tools/vulkan_water_shaders.py) into an image of the colour target's size,
which xgpu_vulkan_stream.c copies into the target. Its device is ray tracing's (as the bake's), its descriptor
sets a ring of four frames' pools (each emptied as its frame comes round again: the frames in flight are fewer), its
constants pushed.

Its tuning (for one run, by the environment; their defaults are the look as judged against Classic, by pictures):
HALO_WATER_RIPPLES (CE's ripples' strength in the normal), HALO_WATER_ABSORB (the absorption a world unit, by CE's
tint), HALO_WATER_SUN (the sun's highlight), HALO_WATER_DEEP (the deep colour's brightness), HALO_WATER_CUBE (CE's
cube's share where the screen has no reflection), HALO_WATER_VIEW (a debug view: 1 the ripples, 2 the base map, 3 the
mask, distance and side, 4 the reflection, 5 the refraction).
*/

#include "xgpu_vulkan_water.h"
#include "host.h"
#include "xgpu_vulkan.h"
#include "xgpu_vulkan_water_shaders.h"

#include <stdlib.h>
#include <string.h>

#define WATER_FUNCTIONS(F) \
	F(vkCreateDescriptorSetLayout) F(vkCreatePipelineLayout) F(vkCreateShaderModule) F(vkDestroyShaderModule) \
	F(vkCreateComputePipelines) F(vkCreateDescriptorPool) F(vkResetDescriptorPool) F(vkAllocateDescriptorSets) \
	F(vkUpdateDescriptorSets) F(vkCmdBindPipeline) F(vkCmdBindDescriptorSets) F(vkCmdPushConstants) \
	F(vkCmdDispatch) F(vkCmdPipelineBarrier) F(vkCreateSampler)

#define DECLARE(name) static PFN_##name name;
WATER_FUNCTIONS(DECLARE)
#undef DECLARE

#define BINDINGS 11
#define FRAMES 4
#define SETS_A_FRAME 32

struct water_push
{
	float camera[4][4];
	int32_t corners[4];
	float plane[4];
	float sun_direction[4];
	float sun_color[4];
	float perpendicular[4];
	float parallel[4];
	float tuning[4];
	float more[4];
	uint32_t flags[4];
	float fog[4];
	float fog_shape[4];
};
typedef char verify_water_push[sizeof(struct water_push) <= 256 ? 1 : -1];

static struct
{
	int made, failed;
	struct xgpu_vulkan_rt_device device;
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout layout;
	VkPipeline pipeline;
	VkDescriptorPool pools[FRAMES];
	uint32_t frame, used;
	VkSampler linear, nearest;
	struct xgpu_vulkan_image *result, *cube_stand_in;
	/* the tuning (the environment's, read once) */
	float ripples, absorb, sun, deep, cube, view;
	uint32_t logged, opaque_logged;
} water;

static float tuning(const char *name, float value)
{
	const char *text = getenv(name);

	return text && *text ? (float)atof(text) : value;
}

static int make(void)
{
	VkDescriptorSetLayoutBinding bindings[BINDINGS];
	VkDescriptorSetLayoutCreateInfo set_info;
	VkPushConstantRange push;
	VkPipelineLayoutCreateInfo layout_info;
	VkShaderModuleCreateInfo module_info;
	VkComputePipelineCreateInfo info;
	VkDescriptorPoolSize sizes[2];
	VkDescriptorPoolCreateInfo pool_info;
	VkSamplerCreateInfo sampler;
	VkShaderModule module = VK_NULL_HANDLE;
	struct xgpu_vulkan_image_description cube;
	uint32_t index;

	if (water.made)
		return 1;
	if (water.failed)
		return 0;
	water.failed = 1;
	if (!xgpu_vulkan_rt_device(&water.device))
	{
		host_logf(HOST_LOG_WARN, "prism water: no device for its passes: CE's water's place left as it was");
		return 0;
	}
#define LOAD(name) name = (PFN_##name)water.device.get_device_proc(water.device.device, #name); \
	if (!name) { host_logf(HOST_LOG_WARN, "prism water: no %s", #name); return 0; }
	WATER_FUNCTIONS(LOAD)
#undef LOAD
	memset(bindings, 0, sizeof(bindings));
	for (index = 0; index < BINDINGS; index++)
	{
		bindings[index].binding = index;
		bindings[index].descriptorType = index == BINDINGS - 1 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE :
			VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		bindings[index].descriptorCount = 1;
		bindings[index].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	}
	memset(&set_info, 0, sizeof(set_info));
	set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	set_info.bindingCount = BINDINGS;
	set_info.pBindings = bindings;
	if (vkCreateDescriptorSetLayout(water.device.device, &set_info, NULL, &water.set_layout) != VK_SUCCESS)
		return 0;
	memset(&push, 0, sizeof(push));
	push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	push.size = sizeof(struct water_push);
	memset(&layout_info, 0, sizeof(layout_info));
	layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout_info.setLayoutCount = 1;
	layout_info.pSetLayouts = &water.set_layout;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges = &push;
	if (vkCreatePipelineLayout(water.device.device, &layout_info, NULL, &water.layout) != VK_SUCCESS)
		return 0;
	memset(&module_info, 0, sizeof(module_info));
	module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	module_info.codeSize = xgpu_vulkan_water_composite_count * sizeof(uint32_t);
	module_info.pCode = xgpu_vulkan_water_composite_words;
	if (vkCreateShaderModule(water.device.device, &module_info, NULL, &module) != VK_SUCCESS)
		return 0;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	info.stage.module = module;
	info.stage.pName = "main";
	info.layout = water.layout;
	index = vkCreateComputePipelines(water.device.device, VK_NULL_HANDLE, 1, &info, NULL, &water.pipeline) ==
		VK_SUCCESS;
	vkDestroyShaderModule(water.device.device, module, NULL);
	if (!index)
		return 0;
	memset(sizes, 0, sizeof(sizes));
	sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	sizes[0].descriptorCount = SETS_A_FRAME * (BINDINGS - 1);
	sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	sizes[1].descriptorCount = SETS_A_FRAME;
	memset(&pool_info, 0, sizeof(pool_info));
	pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_info.maxSets = SETS_A_FRAME;
	pool_info.poolSizeCount = 2;
	pool_info.pPoolSizes = sizes;
	for (index = 0; index < FRAMES; index++)
		if (vkCreateDescriptorPool(water.device.device, &pool_info, NULL, &water.pools[index]) != VK_SUCCESS)
			return 0;
	memset(&sampler, 0, sizeof(sampler));
	sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
	sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.maxLod = 16.0f;
	if (vkCreateSampler(water.device.device, &sampler, NULL, &water.linear) != VK_SUCCESS)
		return 0;
	sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
	sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sampler.maxLod = 0.0f;
	if (vkCreateSampler(water.device.device, &sampler, NULL, &water.nearest) != VK_SUCCESS)
		return 0;
	/* (the cube's stand-in where the shader has none: a sky's grey) */
	memset(&cube, 0, sizeof(cube));
	cube.type = XGPU_VULKAN_IMAGE_CUBE;
	cube.format = XGPU_VULKAN_FORMAT_RGBA8;
	cube.usage = XGPU_VULKAN_USAGE_SAMPLED;
	cube.width = cube.height = 1;
	cube.depth_or_layers = 1;
	cube.levels = 1;
	water.cube_stand_in = xgpu_vulkan_image_create(&cube);
	if (!water.cube_stand_in)
		return 0;
	for (index = 0; index < 6; index++)
	{
		static const unsigned char grey[4] = { 150, 165, 180, 255 };

		xgpu_vulkan_image_upload(water.cube_stand_in, 0, index, 0, 0, 0, 1, 1, 1, grey, 4, 4);
	}
	water.ripples = tuning("HALO_WATER_RIPPLES", 0.35f);
	water.absorb = tuning("HALO_WATER_ABSORB", 0.25f);
	water.sun = tuning("HALO_WATER_SUN", 1.5f);
	water.deep = tuning("HALO_WATER_DEEP", 1.0f);
	water.cube = tuning("HALO_WATER_CUBE", 1.0f);
	water.view = tuning("HALO_WATER_VIEW", 0.0f);
	water.failed = 0;
	water.made = 1;
	host_logf(HOST_LOG_INFO, "prism water: the layer's pass is made (ripples %.2f, absorption %.2f, sun %.2f, "
		"deep %.2f, cube %.2f, view %.0f)", water.ripples, water.absorb, water.sun, water.deep, water.cube, water.view);
	return 1;
}

static void barrier(VkCommandBuffer commands)
{
	VkMemoryBarrier memory;

	memset(&memory, 0, sizeof(memory));
	memory.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	memory.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
		VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	memory.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
		VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
		VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1,
		&memory, 0, NULL, 0, NULL);
}

static struct xgpu_vulkan_image *result_fit(uint32_t width, uint32_t height)
{
	const struct xgpu_vulkan_image_description *had = water.result ? xgpu_vulkan_image_description(water.result) :
		NULL;
	struct xgpu_vulkan_image_description wanted;

	if (had && had->width == width && had->height == height)
		return water.result;
	xgpu_vulkan_image_destroy(water.result);
	memset(&wanted, 0, sizeof(wanted));
	wanted.type = XGPU_VULKAN_IMAGE_2D;
	wanted.format = XGPU_VULKAN_FORMAT_RGBA16F;
	wanted.usage = XGPU_VULKAN_USAGE_SAMPLED | XGPU_VULKAN_USAGE_STORAGE;
	wanted.samples = 1;
	wanted.width = width;
	wanted.height = height;
	wanted.depth_or_layers = 1;
	wanted.levels = 1;
	water.result = xgpu_vulkan_image_create(&wanted);
	return water.result;
}

struct xgpu_vulkan_image *xgpu_vulkan_water_composite(const struct xgpu_vulkan_water_job *job)
{
	struct xgpu_vulkan_image *images[BINDINGS];
	VkDescriptorImageInfo infos[BINDINGS];
	VkWriteDescriptorSet writes[BINDINGS];
	VkDescriptorSetAllocateInfo allocate;
	VkDescriptorSet set;
	VkCommandBuffer commands;
	const struct xgpu_vulkan_image_description *description;
	struct water_push push;
	struct xgpu_vulkan_image *result;
	uint32_t index;
	int traced;

	static const struct xgpu_stream_terrain_water opaque_water;
	const struct xgpu_stream_terrain_water *item;

	if (!job || !job->behind || !job->depth || !job->words || !job->extras || !job->view ||
		(job->classes ? 0 : !job->ripples || !job->base || !job->water) || !make())
		return NULL;
	item = job->water ? job->water : &opaque_water;
	description = xgpu_vulkan_image_description(job->behind);
	result = result_fit(description->width, description->height);
	if (!result || water.used >= SETS_A_FRAME)
		return NULL;
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocate.descriptorPool = water.pools[water.frame];
	allocate.descriptorSetCount = 1;
	allocate.pSetLayouts = &water.set_layout;
	if (vkAllocateDescriptorSets(water.device.device, &allocate, &set) != VK_SUCCESS)
		return NULL;
	water.used++;
	images[0] = job->behind;
	images[1] = job->classes ? job->behind : job->ripples;
	images[2] = job->classes ? job->classes : job->base;
	images[3] = job->depth;
	images[4] = job->words;
	images[5] = job->extras;
	images[6] = job->cube ? job->cube : water.cube_stand_in;
	traced = job->traced_up && job->traced_down && job->traced_aux;
	images[7] = traced ? job->traced_up : job->behind;
	images[8] = traced ? job->traced_down : job->behind;
	images[9] = traced ? job->traced_aux : job->behind;
	images[10] = result;
	commands = xgpu_vulkan_compute_commands();
	memset(infos, 0, sizeof(infos));
	memset(writes, 0, sizeof(writes));
	for (index = 0; index < BINDINGS; index++)
	{
		xgpu_vulkan_image_compute_use(images[index], index == BINDINGS - 1);
		infos[index].sampler = index == BINDINGS - 1 ? VK_NULL_HANDLE : index == 0 || index == 6 ? water.linear :
			water.nearest;
		infos[index].imageView = xgpu_vulkan_image_view(images[index]);
		infos[index].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
		if (!infos[index].imageView)
			return NULL;
		writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[index].dstSet = set;
		writes[index].dstBinding = index;
		writes[index].descriptorCount = 1;
		writes[index].descriptorType = index == BINDINGS - 1 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE :
			VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[index].pImageInfo = &infos[index];
	}
	vkUpdateDescriptorSets(water.device.device, BINDINGS, writes, 0, NULL);
	memset(&push, 0, sizeof(push));
	memcpy(push.camera, job->view->now, sizeof(push.camera));
	memcpy(push.corners, job->view->corners, sizeof(push.corners));
	memcpy(push.plane, item->plane, sizeof(push.plane));
	memcpy(push.perpendicular, item->perpendicular, sizeof(push.perpendicular));
	memcpy(push.parallel, item->parallel, sizeof(push.parallel));
	memcpy(push.fog, item->fog, sizeof(push.fog));
	memcpy(push.fog_shape, item->fog_shape, sizeof(push.fog_shape));
	if (job->sun)
	{
		memcpy(push.sun_direction, job->sun_direction, 3 * sizeof(float));
		push.sun_direction[3] = 1.0f;
		memcpy(push.sun_color, job->sun_color, 3 * sizeof(float));
	}
	push.tuning[0] = job->seconds;
	push.tuning[1] = water.ripples;
	push.tuning[2] = water.absorb;
	push.tuning[3] = water.sun;
	push.more[0] = water.deep;
	push.more[1] = job->power > 0.0f ? job->power : 1.0f;
	push.more[2] = water.cube;
	push.more[3] = water.view;
	push.flags[0] = item->flags;
	push.flags[1] = description->width;
	push.flags[2] = description->height;
	push.flags[3] = (uint32_t)(job->pathtraced != 0) | (job->classes ? 2u : 0u) |
		(traced ? 4u | (job->reference ? 8u : 0u) : 0u);
	barrier(commands);
	vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, water.pipeline);
	vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, water.layout, 0, 1, &set, 0, NULL);
	vkCmdPushConstants(commands, water.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
	xgpu_vulkan_breadcrumb(commands, 90, "PRISM's water layer");
	vkCmdDispatch(commands, (description->width + 7) / 8, (description->height + 7) / 8, 1);
	barrier(commands);
	if (job->classes && !water.opaque_logged)
	{
		water.opaque_logged = 1;
		host_logf(HOST_LOG_INFO, "prism water: an opaque water's layer drawn over the frame (%ux%u)", description->width,
			description->height);
	}
	if (!job->classes && water.logged < 2 && (!water.logged || item->fog_shape[2] < 0.0f))
	{
		water.logged = item->fog_shape[2] < 0.0f ? 2 : 1;
		host_logf(HOST_LOG_INFO, "prism water: the window's planar fog %.3f %.3f %.3f density %.3f, depth %.3f, "
			"distance %.3f, the eye %.3f over it", item->fog[0], item->fog[1], item->fog[2],
			item->fog[3], item->fog_shape[0], item->fog_shape[1], item->fog_shape[2]);
		host_logf(HOST_LOG_INFO, "prism water: the first group composited (%ux%u, flags 0x%x, plane %.3f %.3f %.3f "
			"%.3f)", description->width, description->height, item->flags, item->plane[0],
			item->plane[1], item->plane[2], item->plane[3]);
	}
	return result;
}

float xgpu_vulkan_water_ripples(void)
{
	return make() ? water.ripples : tuning("HALO_WATER_RIPPLES", 0.35f);
}

void xgpu_vulkan_water_frame_end(void)
{
	if (!water.made)
		return;
	water.frame = (water.frame + 1) % FRAMES;
	water.used = 0;
	vkResetDescriptorPool(water.device.device, water.pools[water.frame], 0);
}
