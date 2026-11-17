/*
XGPU_VULKAN_PT.C

PRISM Path Traced (overhaul_research/prism_v4_renderer/design.md, sections
1.5 and 3.8; docs/prism_pathtraced.md), in the Vulkan backend: display.
lighting "pathtraced", the deferred core's opaque pixels lit by the path
tracer, every CE light off. It traces the scene the bake was made from, by
the bake's own functions (xgpu_vulkan_pt.glsl, built on the baker's
BAKE_COMMON), and reads the bake where the bake has the light already:

  light: at each opaque pixel of the G-buffer, the sun (a ray near the eye
    and on the objects, the baked sun mask beyond), the sky's domes, the
    lamps, the fixtures and the dynamic lights (resampled importance over
    candidates from the bake's light-importance grid, one shadow ray), the
    ambient floor, the indirect light (the bake's indirect layer at the
    pixel in "performance" where the level has its texel, else a bounce
    whose hit is lit from the bake) and the reflections (a GGX ray below a
    roughness, its hit lit from the bake; the bake's probes above it);
  the denoiser (ReLAX-class, written from the published methods): temporal
    (a slow and a fast history, the fast one holding the slow one, the
    moments' variance, a firefly clamp, no history across a change of
    instance, normal or plane: none through an opening), fix (a wide gather
    where the history is short), atrous (a-trous steps stopped at the
    plane, the normal, the instance and the luminance by the variance), and
    the sun's visibility through its own history and a filter as wide as
    its penumbra;
  composite: the frame, Raster's form (the diffuse light by the base colour
    less the specular's share, the specular light, the emissive light).

debug.pt_reference in this mode (RT_FRAME's request, prism_rt.c) traces the
same at every pixel unbiased with no bake (the pass "reference"), summed
until its estimated relative MSE is under the gates' bound (0.001, docs/
prism_gates.md) or its frames are done, then written as a linear OpenEXR
file and held.

Each pass's GPU time is measured by timestamps (HALO_PT_TIMES_FILE: a line
a frame; the log: their means every 600 frames).
*/

#include "xgpu_vulkan.h"
#include "xgpu_vulkan_pt.h"
#include "xgpu_vulkan_pt_shaders.h"
#include "xgpu_vulkan_bake.h"
#include "xgpu_vulkan_rt.h"
#include "xgpu_vulkan_scene.h"
#include "host.h"
#include "xgpu_exr.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- constants */

/* the job blocks' ring (each a frame's: the block, then its dynamic
lights), the descriptor sets' */
#define JOB_SLOTS 16u
#define JOB_BYTES 4096u
#define JOB_LIGHTS_OFFSET 1024u
#define LIGHTS_MOST 64u
#define SET_SLOTS 16u
/* the bindings (xgpu_vulkan_pt.glsl's) */
#define BINDINGS 33u
/* the reference's floats a pixel (the GLSL's SUMS), its noise's bound (the
gates' reference_noise), the frames between its noise's measures */
#define SUMS 12u
#define REFERENCE_NOISE 0.001
#define REFERENCE_MEASURE 32u
#define REFERENCE_LEAST 64u
/* the passes timed, the timestamps a frame (a begin and each pass's end),
the frames' slots of them */
#define TIMED 5u
#define STAMPS (TIMED + 1u)
#define TIME_SLOTS 8u

enum
{
	PASS_LIGHT,
	PASS_TEMPORAL,
	PASS_FIX,
	PASS_ATROUS,
	PASS_COMPOSITE,
	PASS_REFERENCE,
	PASS_REFERENCE_STATS,
	PASSES
};

/* the images: their bindings */
enum
{
	IMAGE_NOISY_DIFFUSE = 4,
	IMAGE_NOISY_SPECULAR,
	IMAGE_CLEAN_DIFFUSE,
	IMAGE_CLEAN_SPECULAR,
	IMAGE_SUN_RAW,
	IMAGE_GUIDE_NOW,
	IMAGE_GUIDE_LAST,
	IMAGE_DIFF_LAST,
	IMAGE_DIFF_NOW,
	IMAGE_SPEC_LAST,
	IMAGE_SPEC_NOW,
	IMAGE_MOMENTS_LAST,
	IMAGE_MOMENTS_NOW,
	IMAGE_SUN_LAST,
	IMAGE_SUN_NOW,
	IMAGE_DIFF_A,
	IMAGE_DIFF_B,
	IMAGE_SPEC_A,
	IMAGE_SPEC_B,
	IMAGE_SUN_A,
	IMAGE_SUN_B,
	IMAGE_FRAME,
	IMAGE_LIGHT_IN,
	IMAGE_SPEC_MOMENTS_LAST,
	IMAGE_SPEC_MOMENTS_NOW,
	IMAGE_RESERVOIR_POINT_LAST,
	IMAGE_RESERVOIR_POINT_NOW,
	IMAGE_RESERVOIR_INFO_LAST,
	IMAGE_RESERVOIR_INFO_NOW,
};

/* the job block's flags (xgpu_vulkan_pt.glsl's FLAG_*) */
enum
{
	FLAG_HISTORY = 1,
	FLAG_PERFORMANCE = 2,
	FLAG_DENOISER_OFF = 4,
	FLAG_REFERENCE = 8,
};

#define PT_FUNCTIONS(F) \
	F(vkCreateBuffer) F(vkDestroyBuffer) F(vkGetBufferMemoryRequirements) F(vkAllocateMemory) F(vkFreeMemory) \
	F(vkBindBufferMemory) F(vkMapMemory) F(vkCreateDescriptorSetLayout) F(vkDestroyDescriptorSetLayout) \
	F(vkCreatePipelineLayout) F(vkDestroyPipelineLayout) F(vkCreateShaderModule) F(vkDestroyShaderModule) \
	F(vkCreateComputePipelines) F(vkDestroyPipeline) F(vkCreateDescriptorPool) F(vkDestroyDescriptorPool) \
	F(vkAllocateDescriptorSets) F(vkUpdateDescriptorSets) F(vkCmdBindPipeline) F(vkCmdBindDescriptorSets) \
	F(vkCmdDispatch) F(vkCmdPushConstants) F(vkCmdPipelineBarrier) F(vkCreateSampler) F(vkDestroySampler) \
	F(vkCreateQueryPool) F(vkDestroyQueryPool) F(vkCmdResetQueryPool) F(vkCmdWriteTimestamp) F(vkGetQueryPoolResults) \
	F(vkGetBufferDeviceAddress) F(vkCmdCopyBuffer) F(vkCmdFillBuffer)

#define DECLARE(name) static PFN_##name name;
PT_FUNCTIONS(DECLARE)
#undef DECLARE

/* ---------- structures */

struct pt_buffer
{
	VkBuffer buffer;
	VkDeviceMemory memory;
	VkDeviceSize size;
	VkDeviceAddress address;
	void *mapped;
};

/* the job block (xgpu_vulkan_pt.glsl's Job, field for field) */
struct pt_job
{
	VkDeviceAddress pages, charts, vpages, table, tiles, indirect, sun, masks;
	VkDeviceAddress bricks, coarse, probes, lightmaps, reflections, reflection_table, reflection_data, lights;
	VkDeviceAddress sums, stats, unused[6];
	float camera[4][4];
	float last_camera[4][4];
	int32_t corners[4];
	int32_t last_corners[4];
	uint32_t counts[4];
	uint32_t more[4];
	float volume[4];
	uint32_t frame[4];
	float settings[4];
	float denoise[4];
	float extra[4];
};

struct pt_push
{
	VkDeviceAddress scene, job;
	uint32_t parameters[4];
	float values[4];
};

/* ---------- globals */

static struct
{
	int made, failed;
	struct xgpu_vulkan_rt_device device;
	VkDescriptorSetLayout layout;
	VkPipelineLayout pipeline_layout;
	VkPipeline pipelines[PASSES];
	VkDescriptorPool pool;
	VkDescriptorSet sets[SET_SLOTS];
	uint64_t set_serial[SET_SLOTS];
	uint32_t set_next;
	VkSampler sampler;
	struct pt_buffer jobs[JOB_SLOTS];
	uint64_t job_serial[JOB_SLOTS];
	uint32_t job_next;
	/* the images at the window's size (each its binding's), the last
	frame's and this frame's histories by parity */
	uint32_t width, height;
	struct xgpu_vulkan_image *noisy_diffuse, *noisy_specular, *clean_diffuse, *clean_specular, *sun_raw;
	struct xgpu_vulkan_image *guide[2], *diff[2], *spec[2], *moments[2], *spec_moments[2], *sun[2];
	struct xgpu_vulkan_image *reservoir_point[2], *reservoir_info[2];
	struct xgpu_vulkan_image *diff_a, *diff_b, *spec_a, *spec_b, *sun_a, *sun_b, *out;
	uint32_t parity;
	int history_valid;
	uint32_t frame;
	uint64_t scene_seen;
	uint64_t last_serial;
	/* the timing: a query pool of TIME_SLOTS frames' STAMPS, each slot's
	submission (0 free), the means' sums */
	VkQueryPool queries;
	double timestamp_period;
	uint64_t time_serial[TIME_SLOTS];
	uint32_t time_next;
	double time_sums[TIMED + 1];
	uint32_t time_frames;
	FILE *times_file;
	int times_file_tried;
	/* the reference: its sums (SUMS floats a pixel), its noise's partial
	sums and their readback, the sums' readback for the file */
	struct pt_buffer sums, stats, stats_readback, sums_readback;
	uint32_t ref_frames, ref_count, ref_generation, ref_width, ref_height, ref_paths;
	float ref_camera[4][4];
	uint64_t ref_scene;
	int ref_seen, ref_stopped, ref_converged, ref_copied;
	uint64_t ref_stats_serial, ref_sums_serial;
	uint32_t ref_stats_frames, ref_stats_groups;
	double ref_noise;
	char ref_path[XGPU_RT_REFERENCE_PATH];
	int logged_first, logged_no_bake;
} pt;

/* ---------- memory */

static int memory_type(uint32_t bits, VkMemoryPropertyFlags wanted)
{
	uint32_t index;

	for (index = 0; index < pt.device.memory->memoryTypeCount; index++)
	{
		if ((bits & (1u << index)) && (pt.device.memory->memoryTypes[index].propertyFlags & wanted) == wanted)
			return (int)index;
	}
	return -1;
}

static int buffer_make(VkDeviceSize size, int host, struct pt_buffer *made)
{
	VkBufferCreateInfo info;
	VkMemoryRequirements requirements;
	VkMemoryAllocateFlagsInfo flags;
	VkMemoryAllocateInfo allocate;
	VkBufferDeviceAddressInfo address;
	int type;

	memset(made, 0, sizeof(*made));
	if (size < 16)
		size = 16;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	info.size = size;
	info.usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
		VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (vkCreateBuffer(pt.device.device, &info, NULL, &made->buffer) != VK_SUCCESS)
		return 0;
	vkGetBufferMemoryRequirements(pt.device.device, made->buffer, &requirements);
	type = memory_type(requirements.memoryTypeBits, host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	memset(&flags, 0, sizeof(flags));
	flags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
	flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.pNext = &flags;
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = (uint32_t)type;
	if (type < 0 || vkAllocateMemory(pt.device.device, &allocate, NULL, &made->memory) != VK_SUCCESS)
	{
		vkDestroyBuffer(pt.device.device, made->buffer, NULL);
		memset(made, 0, sizeof(*made));
		return 0;
	}
	if (vkBindBufferMemory(pt.device.device, made->buffer, made->memory, 0) != VK_SUCCESS ||
		(host && vkMapMemory(pt.device.device, made->memory, 0, VK_WHOLE_SIZE, 0, &made->mapped) != VK_SUCCESS))
	{
		vkDestroyBuffer(pt.device.device, made->buffer, NULL);
		vkFreeMemory(pt.device.device, made->memory, NULL);
		memset(made, 0, sizeof(*made));
		return 0;
	}
	made->size = size;
	memset(&address, 0, sizeof(address));
	address.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
	address.buffer = made->buffer;
	made->address = vkGetBufferDeviceAddress(pt.device.device, &address);
	return 1;
}

/* let go of once the GPU is done with every submission so far (the
reference's buffers, made again rarely: when the window's size changes) */
static void buffer_free(struct pt_buffer *buffer)
{
	if (!buffer->buffer)
		return;
	xgpu_vulkan_wait(xgpu_vulkan_serial() > 0 ? xgpu_vulkan_serial() - 1 : 0);
	vkDestroyBuffer(pt.device.device, buffer->buffer, NULL);
	vkFreeMemory(pt.device.device, buffer->memory, NULL);
	memset(buffer, 0, sizeof(*buffer));
}

static void barrier(VkCommandBuffer commands)
{
	VkMemoryBarrier memory;

	memset(&memory, 0, sizeof(memory));
	memory.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	memory.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
	memory.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
		VK_ACCESS_TRANSFER_WRITE_BIT;
	vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &memory, 0, NULL, 0, NULL);
}

/* ---------- made once */

static VkShaderModule shader_make(const uint32_t *words, size_t count)
{
	VkShaderModuleCreateInfo info;
	VkShaderModule module = VK_NULL_HANDLE;

	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	info.codeSize = count * sizeof(uint32_t);
	info.pCode = words;
	if (vkCreateShaderModule(pt.device.device, &info, NULL, &module) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	return module;
}

static int pipeline_make(int which, const uint32_t *words, size_t word_count)
{
	VkComputePipelineCreateInfo info;
	VkShaderModule module = shader_make(words, word_count);
	VkResult result;

	if (!module)
		return 0;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	info.stage.module = module;
	info.stage.pName = "main";
	info.layout = pt.pipeline_layout;
	result = vkCreateComputePipelines(pt.device.device, VK_NULL_HANDLE, 1, &info, NULL, &pt.pipelines[which]);
	vkDestroyShaderModule(pt.device.device, module, NULL);
	return result == VK_SUCCESS;
}

static int make(void)
{
	VkDescriptorSetLayoutBinding bindings[BINDINGS];
	VkDescriptorSetLayoutCreateInfo set_info;
	VkPushConstantRange push;
	VkPipelineLayoutCreateInfo layout_info;
	VkDescriptorPoolSize sizes[2];
	VkDescriptorPoolCreateInfo pool_info;
	VkDescriptorSetAllocateInfo allocate;
	VkDescriptorSetLayout layouts[SET_SLOTS];
	VkSamplerCreateInfo sampler;
	VkQueryPoolCreateInfo queries;
	uint32_t index;

	if (pt.made)
		return 1;
	if (pt.failed)
		return 0;
	pt.failed = 1;
	if (!xgpu_vulkan_rt_device(&pt.device))
		return 0;
#define LOAD(name) name = (PFN_##name)pt.device.get_device_proc(pt.device.device, #name); \
	if (!name) { host_logf(HOST_LOG_WARN, "prism pt: no %s: no path tracing", #name); return 0; }
	PT_FUNCTIONS(LOAD)
#undef LOAD
	memset(bindings, 0, sizeof(bindings));
	for (index = 0; index < BINDINGS; index++)
	{
		bindings[index].binding = index;
		bindings[index].descriptorType = index < 4 || index == IMAGE_LIGHT_IN ?
			VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		bindings[index].descriptorCount = 1;
		bindings[index].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	}
	memset(&set_info, 0, sizeof(set_info));
	set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	set_info.bindingCount = BINDINGS;
	set_info.pBindings = bindings;
	if (vkCreateDescriptorSetLayout(pt.device.device, &set_info, NULL, &pt.layout) != VK_SUCCESS)
		return 0;
	memset(&push, 0, sizeof(push));
	push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	push.size = sizeof(struct pt_push);
	memset(&layout_info, 0, sizeof(layout_info));
	layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout_info.setLayoutCount = 1;
	layout_info.pSetLayouts = &pt.layout;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges = &push;
	if (vkCreatePipelineLayout(pt.device.device, &layout_info, NULL, &pt.pipeline_layout) != VK_SUCCESS)
		return 0;
	if (!pipeline_make(PASS_LIGHT, xgpu_vulkan_pt_light_words, xgpu_vulkan_pt_light_count) ||
		!pipeline_make(PASS_TEMPORAL, xgpu_vulkan_pt_temporal_words, xgpu_vulkan_pt_temporal_count) ||
		!pipeline_make(PASS_FIX, xgpu_vulkan_pt_fix_words, xgpu_vulkan_pt_fix_count) ||
		!pipeline_make(PASS_ATROUS, xgpu_vulkan_pt_atrous_words, xgpu_vulkan_pt_atrous_count) ||
		!pipeline_make(PASS_COMPOSITE, xgpu_vulkan_pt_composite_words, xgpu_vulkan_pt_composite_count) ||
		!pipeline_make(PASS_REFERENCE, xgpu_vulkan_pt_reference_words, xgpu_vulkan_pt_reference_count) ||
		!pipeline_make(PASS_REFERENCE_STATS, xgpu_vulkan_pt_reference_stats_words,
		xgpu_vulkan_pt_reference_stats_count))
	{
		host_logf(HOST_LOG_WARN, "prism pt: its passes cannot be made: no path tracing");
		return 0;
	}
	memset(sizes, 0, sizeof(sizes));
	sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	sizes[0].descriptorCount = 5 * SET_SLOTS;
	sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	sizes[1].descriptorCount = (BINDINGS - 5) * SET_SLOTS;
	memset(&pool_info, 0, sizeof(pool_info));
	pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_info.maxSets = SET_SLOTS;
	pool_info.poolSizeCount = 2;
	pool_info.pPoolSizes = sizes;
	if (vkCreateDescriptorPool(pt.device.device, &pool_info, NULL, &pt.pool) != VK_SUCCESS)
		return 0;
	for (index = 0; index < SET_SLOTS; index++)
		layouts[index] = pt.layout;
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocate.descriptorPool = pt.pool;
	allocate.descriptorSetCount = SET_SLOTS;
	allocate.pSetLayouts = layouts;
	if (vkAllocateDescriptorSets(pt.device.device, &allocate, pt.sets) != VK_SUCCESS)
		return 0;
	memset(&sampler, 0, sizeof(sampler));
	sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
	sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.maxLod = 0.0f;
	if (vkCreateSampler(pt.device.device, &sampler, NULL, &pt.sampler) != VK_SUCCESS)
		return 0;
	for (index = 0; index < JOB_SLOTS; index++)
		if (!buffer_make(JOB_BYTES, 1, &pt.jobs[index]))
			return 0;
	pt.timestamp_period = pt.device.timestamp_period;
	if (pt.timestamp_period > 0.0)
	{
		memset(&queries, 0, sizeof(queries));
		queries.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
		queries.queryType = VK_QUERY_TYPE_TIMESTAMP;
		queries.queryCount = TIME_SLOTS * STAMPS;
		if (vkCreateQueryPool(pt.device.device, &queries, NULL, &pt.queries) != VK_SUCCESS)
			pt.queries = VK_NULL_HANDLE;
	}
	pt.made = 1;
	pt.failed = 0;
	host_logf(HOST_LOG_INFO, "prism pt: ready");
	return 1;
}

/* ---------- images */

static struct xgpu_vulkan_image *image_make(struct xgpu_vulkan_image *image, uint8_t format, uint32_t width,
	uint32_t height)
{
	struct xgpu_vulkan_image_description wanted;

	if (image && xgpu_vulkan_image_description(image)->width == width &&
		xgpu_vulkan_image_description(image)->height == height)
		return image;
	if (image)
		xgpu_vulkan_image_destroy(image);
	memset(&wanted, 0, sizeof(wanted));
	wanted.type = XGPU_VULKAN_IMAGE_2D;
	wanted.format = format;
	wanted.usage = XGPU_VULKAN_USAGE_SAMPLED | XGPU_VULKAN_USAGE_STORAGE;
	wanted.samples = 1;
	wanted.width = width;
	wanted.height = height;
	wanted.depth_or_layers = 1;
	wanted.levels = 1;
	return xgpu_vulkan_image_create(&wanted);
}

/* every image at the window's size: 1 if they are (the histories begun
again where they were made) */
static int images_make(uint32_t width, uint32_t height)
{
	struct xgpu_vulkan_image **half[] =
	{
		&pt.noisy_diffuse, &pt.noisy_specular, &pt.clean_diffuse, &pt.clean_specular, &pt.sun_raw, &pt.diff[0],
		&pt.diff[1], &pt.spec[0], &pt.spec[1], &pt.moments[0], &pt.moments[1], &pt.spec_moments[0],
		&pt.spec_moments[1], &pt.sun[0], &pt.sun[1], &pt.diff_a, &pt.diff_b, &pt.spec_a, &pt.spec_b, &pt.sun_a,
		&pt.sun_b, &pt.out,
	};
	uint32_t index;

	if (width == pt.width && height == pt.height && pt.out)
		return 1;
	for (index = 0; index < sizeof(half) / sizeof(half[0]); index++)
		if (!(*half[index] = image_make(*half[index], XGPU_VULKAN_FORMAT_RGBA16F, width, height)))
			return 0;
	for (index = 0; index < 2; index++)
		if (!(pt.guide[index] = image_make(pt.guide[index], XGPU_VULKAN_FORMAT_RGBA32_UINT, width, height)) ||
			!(pt.reservoir_info[index] = image_make(pt.reservoir_info[index], XGPU_VULKAN_FORMAT_RGBA32_UINT, width,
			height)) ||
			!(pt.reservoir_point[index] = image_make(pt.reservoir_point[index], XGPU_VULKAN_FORMAT_RGBA32F, width,
			height)))
			return 0;
	pt.width = width;
	pt.height = height;
	pt.history_valid = 0;
	return 1;
}

/* ---------- the timing */

static void times_collect(void)
{
	uint32_t slot;

	if (!pt.queries)
		return;
	for (slot = 0; slot < TIME_SLOTS; slot++)
	{
		uint64_t stamps[STAMPS];
		uint32_t index;

		if (!pt.time_serial[slot] || xgpu_vulkan_completed() < pt.time_serial[slot])
			continue;
		pt.time_serial[slot] = 0;
		if (vkGetQueryPoolResults(pt.device.device, pt.queries, slot * STAMPS, STAMPS, sizeof(stamps), stamps,
			sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
			continue;
		if (!pt.times_file_tried)
		{
			const char *path = getenv("HALO_PT_TIMES_FILE");

			pt.times_file_tried = 1;
			pt.times_file = path && *path ? fopen(path, "w") : NULL;
		}
		{
			double ms[TIMED + 1];

			for (index = 0; index < TIMED; index++)
			{
				ms[index] = (double)(stamps[index + 1] - stamps[index]) * pt.timestamp_period * 1.0e-6;
				pt.time_sums[index] += ms[index];
			}
			ms[TIMED] = (double)(stamps[TIMED] - stamps[0]) * pt.timestamp_period * 1.0e-6;
			pt.time_sums[TIMED] += ms[TIMED];
			pt.time_frames++;
			if (pt.times_file)
			{
				fprintf(pt.times_file, "%.4f %.4f %.4f %.4f %.4f %.4f\n", ms[TIMED], ms[0], ms[1], ms[2], ms[3], ms[4]);
				fflush(pt.times_file);
			}
		}
		if (pt.time_frames >= 600)
		{
			double n = (double)pt.time_frames;

			host_logf(HOST_LOG_INFO, "prism pt: %u frames: %.3f ms a frame (light %.3f, temporal %.3f, fix %.3f, "
				"atrous %.3f, composite %.3f)", pt.time_frames, pt.time_sums[TIMED] / n, pt.time_sums[0] / n,
				pt.time_sums[1] / n, pt.time_sums[2] / n, pt.time_sums[3] / n, pt.time_sums[4] / n);
			memset(pt.time_sums, 0, sizeof(pt.time_sums));
			pt.time_frames = 0;
		}
	}
}

/* ---------- the reference */

/* the sums' copy written once the GPU has made it */
static void reference_write(void)
{
	static const char *const names[] =
	{
		"light.R", "light.G", "light.B", "spec.R", "spec.G", "spec.B", "Z", "samples", "noise",
	};
	struct xgpu_exr_attribute attributes[3];
	const float *sums = (const float *)pt.sums_readback.mapped;
	uint64_t texel, count = (uint64_t)pt.ref_width * pt.ref_height;
	float *pixels;
	char error[256];
	uint32_t n = pt.ref_count;

	if (!pt.ref_sums_serial || xgpu_vulkan_completed() < pt.ref_sums_serial)
		return;
	pt.ref_sums_serial = 0;
	if (!sums || !pt.ref_path[0] || !(pixels = (float *)malloc((size_t)count * 9 * sizeof(float))))
	{
		host_logf(HOST_LOG_WARN, "prism pt: debug.pt_reference: the sum cannot be written%s",
			pt.ref_path[0] ? "" : " (no file named)");
		return;
	}
	for (texel = 0; texel < count; texel++)
	{
		const float *s = sums + texel * SUMS;
		float *to = pixels + texel * 9;
		int channel;

		for (channel = 0; channel < 6; channel++)
			to[channel] = s[channel];
		to[6] = s[8];
		to[7] = s[8] > 0.0f ? (float)n : 0.0f;
		/* (the luminance's mean's relative standard error) */
		to[8] = n > 1 && s[6] > 1.0e-6f ? sqrtf(fmaxf(s[7], 0.0f) / ((float)n * (float)(n - 1))) / s[6] : 0.0f;
	}
	attributes[0].name = "prismReferenceFrames";
	attributes[0].value = (int32_t)n;
	attributes[1].name = "prismReferenceNoisePpm";
	attributes[1].value = (int32_t)(pt.ref_noise * 1.0e6 + 0.5);
	attributes[2].name = "prismReferenceGeneration";
	attributes[2].value = (int32_t)pt.ref_generation;
	if (xgpu_exr_write(pt.ref_path, pt.ref_width, pt.ref_height, 9, names, pixels, attributes, 3, error, sizeof(error)))
		host_logf(HOST_LOG_INFO, "prism pt: debug.pt_reference: %u frames of %u paths summed (estimated relative "
			"MSE %.6f), written to %s", n, pt.ref_paths, pt.ref_noise, pt.ref_path);
	else
		host_logf(HOST_LOG_WARN, "prism pt: debug.pt_reference: %s", error);
	free(pixels);
}

/* its noise measured (the partial sums the GPU made): the estimated
relative MSE of the frame, and whether it is under the bound */
static void reference_noise_collect(void)
{
	const float *partial = (const float *)pt.stats_readback.mapped;
	double variance = 0.0, square = 0.0;
	uint32_t index;

	if (!pt.ref_stats_serial || xgpu_vulkan_completed() < pt.ref_stats_serial || !partial)
		return;
	pt.ref_stats_serial = 0;
	for (index = 0; index < pt.ref_stats_groups; index++)
	{
		variance += partial[index * 2];
		square += partial[index * 2 + 1];
	}
	pt.ref_noise = square > 0.0 ? variance / square : 1.0;
	if ((pt.ref_stats_frames % 512u) == 0u || pt.ref_noise <= REFERENCE_NOISE)
		host_logf(HOST_LOG_INFO, "prism pt: debug.pt_reference: %u frames, estimated relative MSE %.6f",
			pt.ref_stats_frames, pt.ref_noise);
	if (pt.ref_noise <= REFERENCE_NOISE && pt.ref_stats_frames >= REFERENCE_LEAST && !pt.ref_stopped)
	{
		pt.ref_stopped = 1;
		pt.ref_converged = 1;
	}
}

/* this frame's request (RT_FRAME's, prism_rt.c's reference_frame): 1 if the
frame is the reference's, its sums made at the size and its count the
frame's number in the sum (held once it is stopped) */
static int reference_take(const struct xgpu_stream_scene_view *view, uint64_t scene, uint32_t width, uint32_t height)
{
	uint32_t generation_unused, materials_count;
	const float *materials;
	const uint32_t *alpha;
	uint32_t alpha_words, frames, generation;
	const struct xgpu_stream_rt_frame *frame = NULL;
	int restart = 0, row, column;

	(void)xgpu_vulkan_rt_scene(&generation_unused, &materials, &materials_count, &alpha, &alpha_words, &frame);
	if (!frame || !(frame->flags & XGPU_RT_FLAG_REFERENCE) || !(frame->spare[0] >= 1.0f))
	{
		pt.ref_seen = 0;
		return 0;
	}
	frames = frame->spare[0] > 1048576.0f ? 1048576u : (uint32_t)frame->spare[0];
	generation = frame->spare[1] >= 0.0f && frame->spare[1] < 16777216.0f ? (uint32_t)frame->spare[1] : 0u;
	if (!pt.ref_seen || generation != pt.ref_generation || frames != pt.ref_frames || scene != pt.ref_scene)
		restart = 1;
	if (width != pt.ref_width || height != pt.ref_height || !pt.sums.buffer)
	{
		uint32_t groups = (width * height + 255u) / 256u;

		buffer_free(&pt.sums);
		buffer_free(&pt.stats);
		buffer_free(&pt.stats_readback);
		buffer_free(&pt.sums_readback);
		if (!buffer_make((VkDeviceSize)width * height * SUMS * 4, 0, &pt.sums) ||
			!buffer_make((VkDeviceSize)groups * 8, 0, &pt.stats) ||
			!buffer_make((VkDeviceSize)groups * 8, 1, &pt.stats_readback) ||
			!buffer_make((VkDeviceSize)width * height * SUMS * 4, 1, &pt.sums_readback))
		{
			host_logf(HOST_LOG_WARN, "prism pt: debug.pt_reference: no memory for its sums");
			pt.ref_seen = 0;
			return 0;
		}
		pt.ref_width = width;
		pt.ref_height = height;
		pt.ref_stats_groups = groups;
		restart = 1;
	}
	if (pt.ref_seen && !restart)
		for (row = 0; row < 4 && !restart; row++)
			for (column = 0; column < 4 && !restart; column++)
				if (fabsf(pt.ref_camera[row][column] - view->now[row][column]) >
					1.0e-4f * fmaxf(1.0f, fabsf(pt.ref_camera[row][column])))
				{
					host_logf(HOST_LOG_INFO, "prism pt: debug.pt_reference: the camera moved after %u frames: summed "
						"again", pt.ref_count);
					restart = 1;
				}
	if (restart)
	{
		const char *path = frame->reference_file ? host_pointer(frame->reference_file) : NULL;
		const char *paths = getenv("HALO_PT_REFERENCE_PATHS");

		pt.ref_generation = generation;
		pt.ref_frames = frames;
		pt.ref_scene = scene;
		pt.ref_count = 0;
		pt.ref_stopped = pt.ref_converged = pt.ref_copied = 0;
		pt.ref_noise = 1.0;
		pt.ref_stats_serial = pt.ref_sums_serial = 0;
		pt.ref_paths = paths ? (uint32_t)atoi(paths) : 1u;
		if (pt.ref_paths < 1u || pt.ref_paths > 64u)
			pt.ref_paths = 1u;
		memcpy(pt.ref_camera, view->now, sizeof(pt.ref_camera));
		pt.ref_path[0] = 0;
		if (path && memchr(path, 0, XGPU_RT_REFERENCE_PATH))
			snprintf(pt.ref_path, sizeof(pt.ref_path), "%s", path);
	}
	pt.ref_seen = 1;
	return 1;
}

/* ---------- the frame */

static void write_image(VkWriteDescriptorSet *write, VkDescriptorImageInfo *info, VkDescriptorSet set, uint32_t binding,
	struct xgpu_vulkan_image *image, int sampled)
{
	memset(info, 0, sizeof(*info));
	info->sampler = sampled ? pt.sampler : VK_NULL_HANDLE;
	info->imageView = xgpu_vulkan_image_view(image);
	info->imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	memset(write, 0, sizeof(*write));
	write->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write->dstSet = set;
	write->dstBinding = binding;
	write->descriptorCount = 1;
	write->descriptorType = sampled ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	write->pImageInfo = info;
}

static void dispatch(VkCommandBuffer commands, int pass, VkDeviceAddress scene, VkDeviceAddress job, uint32_t p0,
	uint32_t p1, uint32_t p2, uint32_t p3, uint32_t groups_x, uint32_t groups_y)
{
	struct pt_push push;

	memset(&push, 0, sizeof(push));
	push.scene = scene;
	push.job = job;
	push.parameters[0] = p0;
	push.parameters[1] = p1;
	push.parameters[2] = p2;
	push.parameters[3] = p3;
	vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pt.pipelines[pass]);
	vkCmdPushConstants(commands, pt.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
	vkCmdDispatch(commands, groups_x, groups_y, 1);
}

static void stamp(VkCommandBuffer commands, uint32_t slot, uint32_t index)
{
	if (pt.queries && slot < TIME_SLOTS)
		vkCmdWriteTimestamp(commands, index ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			pt.queries, slot * STAMPS + index);
}

struct xgpu_vulkan_image *xgpu_vulkan_pt_frame(struct xgpu_vulkan_image *light, struct xgpu_vulkan_image *base,
	struct xgpu_vulkan_image *additions, struct xgpu_vulkan_image *words, struct xgpu_vulkan_image *extras,
	const struct xgpu_stream_scene_view *view, uint32_t phase, int *whole)
{
	struct xgpu_vulkan_bake_lookup lookup;
	const struct xgpu_vulkan_image_description *description;
	struct pt_job *job;
	struct pt_buffer *slot_buffer;
	VkDescriptorImageInfo infos[BINDINGS];
	VkWriteDescriptorSet writes[BINDINGS];
	struct xgpu_vulkan_image *bound[BINDINGS];
	VkDescriptorSet set;
	VkCommandBuffer commands;
	uint32_t index, slot, set_slot, time_slot = TIME_SLOTS, now, last, light_count = 0, serial = 0, dynamic = 0;
	uint32_t groups_x, groups_y, pt_view, flags = 0, source;
	const struct xgpu_stream_scene_light *lights;
	float *dynamic_lights;
	int reference, denoiser_off, steps, step;
	uint64_t scene;

	if (whole)
		*whole = 0;
	if (!light || !base || !additions || !words || !extras || !view || !make())
		return NULL;
	times_collect();
	reference_noise_collect();
	reference_write();
	if (!xgpu_vulkan_bake_lookup(&lookup))
	{
		if (!pt.logged_no_bake)
			host_logf(HOST_LOG_INFO, "prism pt: no bake's scene to trace yet: CE's frame until it is made");
		pt.logged_no_bake = 1;
		pt.history_valid = 0;
		return NULL;
	}
	pt.logged_no_bake = 0;
	scene = lookup.scene;
	description = xgpu_vulkan_image_description(light);
	if (!images_make(description->width, description->height))
		return NULL;
	/* (the job block's slot and the descriptor set's, each once the GPU is
	done with its last frame) */
	slot = pt.job_next;
	set_slot = pt.set_next;
	if (pt.job_serial[slot] > xgpu_vulkan_completed() || pt.set_serial[set_slot] > xgpu_vulkan_completed())
	{
		host_logf(HOST_LOG_WARN, "prism pt: the frames' ring is full: the frame waits");
		xgpu_vulkan_wait(pt.job_serial[slot] > pt.set_serial[set_slot] ? pt.job_serial[slot] : pt.set_serial[set_slot]);
	}
	pt.job_next = (slot + 1) % JOB_SLOTS;
	pt.set_next = (set_slot + 1) % SET_SLOTS;
	slot_buffer = &pt.jobs[slot];
	job = (struct pt_job *)slot_buffer->mapped;
	memset(job, 0, sizeof(*job));
	pt_view = (phase & XGPU_GBUFFER_PHASE_VIEW_MASK) >> XGPU_GBUFFER_PHASE_VIEW_SHIFT;
	if (pt_view < XGPU_GBUFFER_VIEW_PT_DIFFUSE || pt_view > XGPU_GBUFFER_VIEW_PT_LAST)
		pt_view = 0;
	denoiser_off = (phase & XGPU_GBUFFER_PHASE_PT_DENOISER_OFF) != 0;
	if (((phase & XGPU_GBUFFER_PHASE_PT_PROFILE_MASK) >> XGPU_GBUFFER_PHASE_PT_PROFILE_SHIFT) ==
		XGPU_PT_PROFILE_PERFORMANCE)
		flags |= FLAG_PERFORMANCE;
	if (denoiser_off)
		flags |= FLAG_DENOISER_OFF;
	/* (the history: the last frame's, drawn by this, of the same window at
	the same size, from the same bake's scene; a frame is a submission or a
	few, so a gap of more than 16 is frames this mode did not draw) */
	if (pt.history_valid && (view->flags & XGPU_SCENE_VIEW_HISTORY) && scene == pt.scene_seen &&
		pt.last_serial + 16 >= xgpu_vulkan_serial())
		flags |= FLAG_HISTORY;
	reference = reference_take(view, scene, description->width, description->height);
	if (reference)
		flags |= FLAG_REFERENCE;
	job->pages = lookup.pages;
	job->charts = lookup.charts;
	job->vpages = lookup.vpages;
	job->table = lookup.table;
	job->tiles = lookup.tiles;
	job->indirect = lookup.indirect;
	job->sun = lookup.sun;
	job->masks = lookup.masks;
	job->bricks = lookup.bricks;
	job->coarse = lookup.coarse;
	job->probes = lookup.probes;
	job->lightmaps = lookup.lightmaps;
	job->reflections = lookup.reflections;
	job->reflection_table = lookup.reflection_table;
	job->reflection_data = lookup.reflection_data;
	job->sums = pt.sums.address;
	job->stats = pt.stats.address;
	memcpy(job->camera, view->now, sizeof(job->camera));
	memcpy(job->last_camera, (view->flags & XGPU_SCENE_VIEW_HISTORY) ? view->last : view->now, sizeof(job->last_camera));
	memcpy(job->corners, view->corners, sizeof(job->corners));
	memcpy(job->last_corners, (view->flags & XGPU_SCENE_VIEW_HISTORY) ? view->last_corners : view->corners,
		sizeof(job->last_corners));
	job->counts[0] = lookup.page_count;
	job->counts[1] = description->width;
	job->counts[2] = description->height;
	job->counts[3] = lookup.brick_entries;
	job->more[0] = lookup.coarse_entries;
	job->more[1] = lookup.reflection_count;
	job->more[2] = lookup.reflection_entries;
	job->more[3] = lookup.reflection_levels | (lookup.reflection_side << 16);
	job->volume[0] = lookup.probe_spacing;
	job->volume[1] = lookup.coarse_spacing;
	job->frame[0] = ++pt.frame;
	job->frame[1] = flags;
	job->frame[2] = pt_view;
	/* the settings: the sun's ray within 60 world units (the baked mask
	beyond, design section 3.8), the reflections traced below a roughness
	of 0.35, the firefly clamp at 4 times the brightest neighbour, the
	luminance's edge stop at 4 standard deviations */
	job->settings[0] = 60.0f;
	job->settings[1] = 0.35f;
	job->settings[2] = 4.0f;
	job->settings[3] = 4.0f;
	/* the histories: the slow one at most 32 frames, the fast 4, the fast
	one's hold at 2 standard deviations of its mean */
	job->denoise[0] = 32.0f;
	job->denoise[1] = 4.0f;
	job->denoise[3] = 2.0f;
	/* the lamps' candidates a pixel; the reference's paths a pixel a frame;
	ReSTIR DI's temporal reuse, its reservoirs' confidence at most 8 frames
	(HALO_PT_RESTIR 0: none, each pixel's own candidates alone) */
	job->extra[0] = 8.0f;
	job->extra[1] = (float)(pt.ref_paths ? pt.ref_paths : 1u);
	{
		static int restir = -1;

		if (restir < 0)
			restir = getenv("HALO_PT_RESTIR") ? atoi(getenv("HALO_PT_RESTIR")) != 0 : 1;
		job->extra[2] = restir ? 8.0f : 0.0f;
	}
	/* (this frame's dynamic lights: SCENE_LIGHTS' dynamic class, those on) */
	dynamic_lights = (float *)((char *)slot_buffer->mapped + JOB_LIGHTS_OFFSET);
	lights = xgpu_vulkan_scene_light_table(&light_count, &serial);
	for (index = 0; lights && index < light_count && dynamic < LIGHTS_MOST; index++)
	{
		const struct xgpu_stream_scene_light *l = &lights[index];
		float *to = dynamic_lights + dynamic * 12;

		if (l->light_class != XGPU_SCENE_LIGHT_CLASS_DYNAMIC || !(l->flags & XGPU_SCENE_LIGHT_FLAG_ON) ||
			(l->type != XGPU_SCENE_LIGHT_POINT && l->type != XGPU_SCENE_LIGHT_SPOT) || !(l->position[3] > 0.0f))
			continue;
		memcpy(to, l->position, 4 * sizeof(float));
		to[4] = l->radiance[0] * l->radiance[3];
		to[5] = l->radiance[1] * l->radiance[3];
		to[6] = l->radiance[2] * l->radiance[3];
		to[7] = 0.0f;
		memcpy(to + 8, l->direction, 3 * sizeof(float));
		to[11] = l->type == XGPU_SCENE_LIGHT_SPOT ? l->direction[3] : 2.0f;
		dynamic++;
	}
	job->volume[2] = (float)dynamic;
	job->lights = dynamic ? slot_buffer->address + JOB_LIGHTS_OFFSET : 0;
	/* the images: this frame's histories at its parity, the last frame's at
	the other */
	now = pt.parity;
	last = pt.parity ^ 1u;
	memset(bound, 0, sizeof(bound));
	bound[0] = words;
	bound[1] = extras;
	bound[2] = base;
	bound[3] = additions;
	bound[IMAGE_NOISY_DIFFUSE] = pt.noisy_diffuse;
	bound[IMAGE_NOISY_SPECULAR] = pt.noisy_specular;
	bound[IMAGE_CLEAN_DIFFUSE] = pt.clean_diffuse;
	bound[IMAGE_CLEAN_SPECULAR] = pt.clean_specular;
	bound[IMAGE_SUN_RAW] = pt.sun_raw;
	bound[IMAGE_GUIDE_NOW] = pt.guide[now];
	bound[IMAGE_GUIDE_LAST] = pt.guide[last];
	bound[IMAGE_DIFF_LAST] = pt.diff[last];
	bound[IMAGE_DIFF_NOW] = pt.diff[now];
	bound[IMAGE_SPEC_LAST] = pt.spec[last];
	bound[IMAGE_SPEC_NOW] = pt.spec[now];
	bound[IMAGE_MOMENTS_LAST] = pt.moments[last];
	bound[IMAGE_MOMENTS_NOW] = pt.moments[now];
	bound[IMAGE_SUN_LAST] = pt.sun[last];
	bound[IMAGE_SUN_NOW] = pt.sun[now];
	bound[IMAGE_DIFF_A] = pt.diff_a;
	bound[IMAGE_DIFF_B] = pt.diff_b;
	bound[IMAGE_SPEC_A] = pt.spec_a;
	bound[IMAGE_SPEC_B] = pt.spec_b;
	bound[IMAGE_SUN_A] = pt.sun_a;
	bound[IMAGE_SUN_B] = pt.sun_b;
	bound[IMAGE_FRAME] = pt.out;
	bound[IMAGE_LIGHT_IN] = light;
	bound[IMAGE_SPEC_MOMENTS_LAST] = pt.spec_moments[last];
	bound[IMAGE_SPEC_MOMENTS_NOW] = pt.spec_moments[now];
	bound[IMAGE_RESERVOIR_POINT_LAST] = pt.reservoir_point[last];
	bound[IMAGE_RESERVOIR_POINT_NOW] = pt.reservoir_point[now];
	bound[IMAGE_RESERVOIR_INFO_LAST] = pt.reservoir_info[last];
	bound[IMAGE_RESERVOIR_INFO_NOW] = pt.reservoir_info[now];
	set = pt.sets[set_slot];
	for (index = 0; index < BINDINGS; index++)
		write_image(&writes[index], &infos[index], set, index, bound[index], index < 4 || index == IMAGE_LIGHT_IN);
	vkUpdateDescriptorSets(pt.device.device, BINDINGS, writes, 0, NULL);
	commands = xgpu_vulkan_compute_commands();
	for (index = 0; index < BINDINGS; index++)
		xgpu_vulkan_image_compute_use(bound[index], !(index < 4 || index == IMAGE_LIGHT_IN));
	groups_x = (description->width + 7) / 8;
	groups_y = (description->height + 7) / 8;
	if (pt.queries)
	{
		time_slot = pt.time_next;
		if (pt.time_serial[time_slot])
			time_slot = TIME_SLOTS;
		else
		{
			pt.time_next = (time_slot + 1) % TIME_SLOTS;
			vkCmdResetQueryPool(commands, pt.queries, time_slot * STAMPS, STAMPS);
		}
	}
	barrier(commands);
	vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pt.pipeline_layout, 0, 1, &set, 0, NULL);
	stamp(commands, time_slot, 0);
	if (reference)
	{
		/* the reference: summed while it runs, its noise measured every
		REFERENCE_MEASURE frames, its sums copied for the file once it
		stops; its means drawn */
		int adding = !pt.ref_stopped && pt.ref_count < pt.ref_frames;

		if (adding)
		{
			pt.ref_count++;
			dispatch(commands, PASS_REFERENCE, scene, slot_buffer->address, pt.ref_count, 0, 0, 0, groups_x, groups_y);
			if (pt.ref_count >= pt.ref_frames)
				pt.ref_stopped = 1;
		}
		stamp(commands, time_slot, 1);
		barrier(commands);
		if (adding && !pt.ref_stats_serial && ((pt.ref_count % REFERENCE_MEASURE) == 0u || pt.ref_stopped))
		{
			VkBufferCopy copy;

			dispatch(commands, PASS_REFERENCE_STATS, scene, slot_buffer->address, pt.ref_count,
				description->width * description->height, 0, 0, pt.ref_stats_groups, 1);
			barrier(commands);
			memset(&copy, 0, sizeof(copy));
			copy.size = (VkDeviceSize)pt.ref_stats_groups * 8;
			vkCmdCopyBuffer(commands, pt.stats.buffer, pt.stats_readback.buffer, 1, &copy);
			pt.ref_stats_serial = xgpu_vulkan_serial();
			pt.ref_stats_frames = pt.ref_count;
		}
		/* (stopped and its last noise measured: the sums copied for the file,
		once) */
		if (pt.ref_stopped && !pt.ref_copied && !pt.ref_stats_serial && pt.ref_count)
		{
			VkBufferCopy copy;

			memset(&copy, 0, sizeof(copy));
			copy.size = (VkDeviceSize)description->width * description->height * SUMS * 4;
			vkCmdCopyBuffer(commands, pt.sums.buffer, pt.sums_readback.buffer, 1, &copy);
			pt.ref_sums_serial = xgpu_vulkan_serial();
			pt.ref_copied = 1;
			host_logf(HOST_LOG_INFO, "prism pt: debug.pt_reference: stopped after %u frames (%s)", pt.ref_count,
				pt.ref_converged ? "its noise under the bound" : "its frames done");
		}
		barrier(commands);
		for (index = 2; index < TIMED; index++)
			stamp(commands, time_slot, index);
		dispatch(commands, PASS_COMPOSITE, scene, slot_buffer->address, 3, 0, 0, 0, groups_x, groups_y);
		stamp(commands, time_slot, TIMED);
		pt.history_valid = 0;
	}
	else
	{
		/* the light; the denoiser (temporal, fix, atrous: the one call site
		of the denoise step, where the plugins' denoise entry points go);
		the frame */
		dispatch(commands, PASS_LIGHT, scene, slot_buffer->address, 0, 0, 0, 0, groups_x, groups_y);
		stamp(commands, time_slot, 1);
		barrier(commands);
		if (!denoiser_off)
		{
			dispatch(commands, PASS_TEMPORAL, scene, slot_buffer->address, 0, 0, 0, 0, groups_x, groups_y);
			stamp(commands, time_slot, 2);
			barrier(commands);
			dispatch(commands, PASS_FIX, scene, slot_buffer->address, 0, 0, 0, 0, groups_x, groups_y);
			stamp(commands, time_slot, 3);
			barrier(commands);
			/* (the fix wrote b: the steps from b to a and back; the specular's
			the first three, its reach narrowed by its roughness) */
			steps = 5;
			for (step = 0; step < steps; step++)
			{
				dispatch(commands, PASS_ATROUS, scene, slot_buffer->address, (uint32_t)step, (step & 1) == 0 ? 1u : 0u,
					step < 3 ? 7u : 5u, 0, groups_x, groups_y);
				barrier(commands);
			}
			stamp(commands, time_slot, 4);
			/* (an even count of steps ends in b, an odd one in a) */
			source = (steps & 1) == 0 ? 1u : 0u;
		}
		else
		{
			stamp(commands, time_slot, 2);
			stamp(commands, time_slot, 3);
			stamp(commands, time_slot, 4);
			source = 2u;
		}
		dispatch(commands, PASS_COMPOSITE, scene, slot_buffer->address, source, 0, 0, 0, groups_x, groups_y);
		stamp(commands, time_slot, TIMED);
		pt.history_valid = !denoiser_off;
		pt.parity ^= 1u;
	}
	barrier(commands);
	pt.job_serial[slot] = xgpu_vulkan_serial();
	pt.set_serial[set_slot] = xgpu_vulkan_serial();
	if (time_slot < TIME_SLOTS)
		pt.time_serial[time_slot] = xgpu_vulkan_serial();
	pt.last_serial = xgpu_vulkan_serial();
	pt.scene_seen = scene;
	if (!pt.logged_first)
	{
		pt.logged_first = 1;
		host_logf(HOST_LOG_INFO, "prism pt: first frame %ux%u (%s, denoiser %s): the bake's %u of %u tiles, %u pages, "
			"%u dynamic lights, the volume %s, %u reflection probes", description->width, description->height,
			(flags & FLAG_PERFORMANCE) ? "performance" : "quality", denoiser_off ? "off" : "prism", lookup.tiles_done,
			lookup.tiles_planned, lookup.page_count, dynamic, lookup.probes ? "made" : "not made yet",
			lookup.reflection_count);
	}
	if (whole)
		*whole = 1;
	return pt.out;
}

void xgpu_vulkan_pt_shutdown(void)
{
	uint32_t index;
	struct xgpu_vulkan_image **images[] =
	{
		&pt.noisy_diffuse, &pt.noisy_specular, &pt.clean_diffuse, &pt.clean_specular, &pt.sun_raw, &pt.guide[0],
		&pt.guide[1], &pt.diff[0], &pt.diff[1], &pt.spec[0], &pt.spec[1], &pt.moments[0], &pt.moments[1],
		&pt.spec_moments[0], &pt.spec_moments[1], &pt.sun[0], &pt.sun[1], &pt.diff_a, &pt.diff_b, &pt.spec_a, &pt.spec_b,
		&pt.sun_a, &pt.sun_b, &pt.out, &pt.reservoir_point[0], &pt.reservoir_point[1], &pt.reservoir_info[0],
		&pt.reservoir_info[1],
	};
	struct pt_buffer *buffers[] = { &pt.sums, &pt.stats, &pt.stats_readback, &pt.sums_readback };

	if (!pt.made)
		return;
	for (index = 0; index < sizeof(images) / sizeof(images[0]); index++)
	{
		if (*images[index])
			xgpu_vulkan_image_destroy(*images[index]);
		*images[index] = NULL;
	}
	for (index = 0; index < sizeof(buffers) / sizeof(buffers[0]); index++)
		if (buffers[index]->buffer)
		{
			vkDestroyBuffer(pt.device.device, buffers[index]->buffer, NULL);
			vkFreeMemory(pt.device.device, buffers[index]->memory, NULL);
			memset(buffers[index], 0, sizeof(*buffers[index]));
		}
	for (index = 0; index < JOB_SLOTS; index++)
		if (pt.jobs[index].buffer)
		{
			vkDestroyBuffer(pt.device.device, pt.jobs[index].buffer, NULL);
			vkFreeMemory(pt.device.device, pt.jobs[index].memory, NULL);
			memset(&pt.jobs[index], 0, sizeof(pt.jobs[index]));
		}
	for (index = 0; index < PASSES; index++)
		if (pt.pipelines[index])
			vkDestroyPipeline(pt.device.device, pt.pipelines[index], NULL);
	if (pt.pipeline_layout)
		vkDestroyPipelineLayout(pt.device.device, pt.pipeline_layout, NULL);
	if (pt.layout)
		vkDestroyDescriptorSetLayout(pt.device.device, pt.layout, NULL);
	if (pt.pool)
		vkDestroyDescriptorPool(pt.device.device, pt.pool, NULL);
	if (pt.sampler)
		vkDestroySampler(pt.device.device, pt.sampler, NULL);
	if (pt.queries)
		vkDestroyQueryPool(pt.device.device, pt.queries, NULL);
	if (pt.times_file)
		fclose(pt.times_file);
	memset(&pt, 0, sizeof(pt));
}
