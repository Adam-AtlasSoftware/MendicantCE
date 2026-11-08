/*
XGPU_VULKAN_RT.C

display.prism_rt (docs/prism_rt.md; xgpu_vulkan_rt.h): PRISM's hardware ray
tracing in the Vulkan backend, with ray queries in compute passes (no ray
tracing pipeline): portable across NVIDIA, RADV, AMDVLK and Intel, and what
OpenH1 issue #602 plans for its hardware world_trace.

- The device: VK_KHR_acceleration_structure, VK_KHR_ray_query and
  VK_KHR_deferred_host_operations with buffer device addresses, enabled
  where present (xgpu_vulkan.c's device_make; HALO_VULKAN_NO_RT makes it
  without them). Nothing here runs unless the game sends RT_FRAME, which it
  sends only with display.prism_rt on.
- The structure: the BSP's opaque triangles as the game copied them
  (RT_STATIC), one bottom-level structure, built once (PREFER_FAST_TRACE)
  as the set is whole; the last set traces until then.
- The objects: each draw of world AO's pre-pass that has the world's light
  and the stock vertex layout is captured as it is drawn (its vertices
  skinned on the CPU by its own node matrices, the stock vertex program's
  two-node blend), so their permutations, levels of detail and pose are
  what is drawn; all of a window's in one bottom-level structure, built each
  frame (PREFER_FAST_BUILD). The first-person weapon is the pre-pass's
  occluder, never captured. Alpha-tested draws are passed over (counted).
- The frame: one top-level structure a window (the structure, mask 1; the
  objects, mask 4), built after the pre-pass, so the rays see the frame's
  drawn pose. Then two compute passes (xgpu_vulkan_rt_glsl.inc): trace, at
  world AO's texels, and resolve (filtered, accumulated over frames, as world
  AO's pass writes its texels), whose image the surfaces read in world AO's
  place (xgpu_vulkan_world_ao_set_pixels).

Memory a frame comes from one of three slots (the frames in flight and one
more), each reused once its submission is done. GPU time: HALO_VULKAN_STATS
(or debug.gpu_stats) logs the passes' mean every 600 frames.
*/

#include "xgpu_vulkan.h"
#include "xgpu_vulkan_rt.h"
#include "xgpu_vulkan_rt_shaders.h"
#include "host.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- constants */

#define RT_SLOTS 3
#define RT_GEOMETRY_BYTES (24u << 20)
#define RT_STORAGE_BYTES (64u << 20)
#define RT_SCRATCH_BYTES (48u << 20)
#define RT_INSTANCES 16u
#define RT_GARBAGE 256u
/* the objects' triangles a window at most (past it: the farthest drawn
last are left out) */
#define RT_OBJECT_TRIANGLES 600000u
/* gi's probes at most (32 x 16 x 32) */
#define RT_PROBES_MOST (32u * 16u * 32u)

/* the pass block (the GLSL's Pass, std140) */
struct rt_pass
{
	float camera[4][4];
	int32_t corners[4];
	float previous[4][4];
	int32_t previous_corners[4];
	float sun[4];
	float strength[4];
	float rays[4];
	uint32_t info[4];
	int32_t size[4];
	/* gi's probes: the volume's first probe's place (its cell times the
	spacing) and the spacing; the probes a side and all; the first updated
	this frame, the rays a probe, the mood, whether gi is on; the sky's
	radiance; the sun's; the structure's vertices' and the objects'
	vertices' addresses (low, high), the objects' indices' */
	float probe_origin[4];
	int32_t probe_count[4];
	uint32_t probe_info[4];
	float sky[4];
	float sun_radiance[4];
	uint32_t addresses[4];
	uint32_t addresses2[4];
};

/* ---------- the functions */

#define RT_FUNCTIONS(F) \
	F(vkCreateBuffer) F(vkDestroyBuffer) F(vkGetBufferMemoryRequirements) F(vkAllocateMemory) F(vkFreeMemory) \
	F(vkBindBufferMemory) F(vkMapMemory) F(vkCreateDescriptorSetLayout) F(vkCreatePipelineLayout) \
	F(vkCreateShaderModule) F(vkDestroyShaderModule) F(vkCreateComputePipelines) F(vkCreateDescriptorPool) \
	F(vkResetDescriptorPool) F(vkAllocateDescriptorSets) F(vkUpdateDescriptorSets) F(vkCmdBindPipeline) \
	F(vkCmdBindDescriptorSets) F(vkCmdDispatch) F(vkCmdPipelineBarrier) F(vkCreateSampler) F(vkCreateQueryPool) \
	F(vkCmdResetQueryPool) F(vkCmdWriteTimestamp) F(vkCmdFillBuffer) F(vkGetQueryPoolResults) F(vkGetBufferDeviceAddress) \
	F(vkCreateAccelerationStructureKHR) F(vkDestroyAccelerationStructureKHR) \
	F(vkGetAccelerationStructureBuildSizesKHR) F(vkCmdBuildAccelerationStructuresKHR) \
	F(vkGetAccelerationStructureDeviceAddressKHR)

#define DECLARE(name) static PFN_##name name;
RT_FUNCTIONS(DECLARE)
#undef DECLARE

/* ---------- structures */

struct rt_buffer
{
	VkBuffer buffer;
	VkDeviceMemory memory;
	VkDeviceSize size;
	VkDeviceAddress address;
	void *mapped;
};

struct rt_garbage
{
	uint64_t serial;
	struct rt_buffer buffer;
	VkAccelerationStructureKHR structure;
};

struct rt_slot
{
	uint64_t serial;
	struct rt_buffer geometry, instances, storage, scratch;
	VkDeviceSize geometry_used, storage_used, scratch_used;
	uint32_t instances_used;
	VkAccelerationStructureKHR made[8];
	uint32_t made_count;
	VkDescriptorPool pool;
	/* the GPU time of its frame's passes: queries 0 and 1, written */
	int timed;
};

/* a window's captured objects: their world-space vertices and triangles,
in the slot's geometry buffer */
#define RT_CAPTURED_DRAWS 4096u
struct rt_capture
{
	VkDeviceSize vertex_offset, index_offset;
	uint32_t vertex_count, triangle_count;
	uint32_t vertex_room, index_room;
	int open, full;
	/* the occluders' draws: their triangles and bounds (the first-person
	weapon's left out at the window's end) */
	struct
	{
		uint32_t first, count;
		float bounds[6];
	} occluders[RT_CAPTURED_DRAWS];
	uint32_t occluder_count;
};

static struct
{
	int tried, made;
	struct xgpu_vulkan_rt_device device;
	VkDeviceSize scratch_alignment;
	VkDescriptorSetLayout trace_layout, resolve_layout, probes_layout;
	VkPipelineLayout trace_pipeline_layout, resolve_pipeline_layout, probes_pipeline_layout;
	VkPipeline trace, resolve, probes;
	/* gi's probes (RT_PROBES_MOST of them, four vec4s each: their
	irradiance's spherical harmonics, a channel each, then the place they
	were found at and whether they are valid), zeroed once; the frames whose
	probes were updated */
	struct rt_buffer probe_buffer;
	int probes_zeroed;
	uint32_t probe_frame;
	VkSampler nearest, linear;
	VkQueryPool queries;
	struct rt_slot slots[RT_SLOTS];
	uint32_t slot;
	int slot_ready;
	struct rt_garbage garbage[RT_GARBAGE];
	uint32_t garbage_count;
	/* the frame's settings (RT_FRAME) and whether one came this frame */
	struct xgpu_stream_rt_frame frame;
	int frame_active;
	/* the structure: the set being received and the one traced */
	uint32_t receiving_generation;
	float *receiving;
	uint32_t receiving_count, receiving_total;
	struct rt_buffer static_storage, static_vertices;
	VkAccelerationStructureKHR static_structure;
	VkDeviceAddress static_address;
	uint32_t static_triangles, static_generation;
	struct rt_capture capture;
	/* the passes' images: raw rays, the accumulated state (two in turn) and
	the surfaces' */
	struct xgpu_vulkan_image *raw, *state[2], *result;
	uint32_t image_width, image_height, state_index;
	int history_valid;
	/* statistics (600 frames) */
	uint64_t frames, draws, skipped, triangles, object_triangles_most, full_frames;
	double milliseconds, milliseconds_most;
	uint64_t timed_frames;
	int statistics;
	int logged_first, logged_gi;
} rt;

/* ---------- memory */

static int memory_type(uint32_t bits, VkMemoryPropertyFlags wanted)
{
	uint32_t index;

	for (index = 0; index < rt.device.memory->memoryTypeCount; index++)
	{
		if ((bits & (1u << index)) && (rt.device.memory->memoryTypes[index].propertyFlags & wanted) == wanted)
			return (int)index;
	}
	return -1;
}

static int buffer_make(VkDeviceSize size, VkBufferUsageFlags usage, int host, struct rt_buffer *made)
{
	VkBufferCreateInfo info;
	VkMemoryRequirements requirements;
	VkMemoryAllocateFlagsInfo flags;
	VkMemoryAllocateInfo allocate;
	VkBufferDeviceAddressInfo address;
	int type;

	memset(made, 0, sizeof(*made));
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	info.size = size;
	info.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (vkCreateBuffer(rt.device.device, &info, NULL, &made->buffer) != VK_SUCCESS)
		return 0;
	vkGetBufferMemoryRequirements(rt.device.device, made->buffer, &requirements);
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
	if (type < 0 || vkAllocateMemory(rt.device.device, &allocate, NULL, &made->memory) != VK_SUCCESS)
	{
		vkDestroyBuffer(rt.device.device, made->buffer, NULL);
		memset(made, 0, sizeof(*made));
		return 0;
	}
	if (vkBindBufferMemory(rt.device.device, made->buffer, made->memory, 0) != VK_SUCCESS ||
		(host && vkMapMemory(rt.device.device, made->memory, 0, VK_WHOLE_SIZE, 0, &made->mapped) != VK_SUCCESS))
	{
		vkDestroyBuffer(rt.device.device, made->buffer, NULL);
		vkFreeMemory(rt.device.device, made->memory, NULL);
		memset(made, 0, sizeof(*made));
		return 0;
	}
	made->size = size;
	memset(&address, 0, sizeof(address));
	address.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
	address.buffer = made->buffer;
	made->address = vkGetBufferDeviceAddress(rt.device.device, &address);
	return 1;
}

static void buffer_free(struct rt_buffer *buffer)
{
	if (buffer->buffer)
		vkDestroyBuffer(rt.device.device, buffer->buffer, NULL);
	if (buffer->memory)
		vkFreeMemory(rt.device.device, buffer->memory, NULL);
	memset(buffer, 0, sizeof(*buffer));
}

/* (after the submission being recorded, which may use them) */
static void garbage_add(struct rt_buffer *buffer, VkAccelerationStructureKHR structure)
{
	struct rt_garbage *item;

	if (rt.garbage_count == RT_GARBAGE)
	{
		/* (full: the oldest waited for) */
		xgpu_vulkan_wait(rt.garbage[0].serial);
	}
	if (rt.garbage_count == RT_GARBAGE)
		return;
	item = &rt.garbage[rt.garbage_count++];
	memset(item, 0, sizeof(*item));
	item->serial = xgpu_vulkan_serial();
	if (buffer)
		item->buffer = *buffer;
	item->structure = structure;
	if (buffer)
		memset(buffer, 0, sizeof(*buffer));
}

static void garbage_collect(void)
{
	uint64_t done = xgpu_vulkan_completed();
	uint32_t index = 0;

	while (index < rt.garbage_count)
	{
		struct rt_garbage *item = &rt.garbage[index];

		if (item->serial <= done)
		{
			if (item->structure)
				vkDestroyAccelerationStructureKHR(rt.device.device, item->structure, NULL);
			buffer_free(&item->buffer);
			*item = rt.garbage[--rt.garbage_count];
			continue;
		}
		index++;
	}
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
	if (vkCreateShaderModule(rt.device.device, &info, NULL, &module) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	return module;
}

static int pipeline_make(const VkDescriptorType *types, uint32_t count, const uint32_t *words, size_t word_count,
	VkDescriptorSetLayout *set_layout, VkPipelineLayout *layout, VkPipeline *pipeline)
{
	VkDescriptorSetLayoutBinding bindings[8];
	VkDescriptorSetLayoutCreateInfo set_info;
	VkPipelineLayoutCreateInfo layout_info;
	VkComputePipelineCreateInfo info;
	VkShaderModule module;
	uint32_t index;
	VkResult result;

	memset(bindings, 0, sizeof(bindings));
	for (index = 0; index < count; index++)
	{
		bindings[index].binding = index;
		bindings[index].descriptorType = types[index];
		bindings[index].descriptorCount = 1;
		bindings[index].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	}
	memset(&set_info, 0, sizeof(set_info));
	set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	set_info.bindingCount = count;
	set_info.pBindings = bindings;
	if (vkCreateDescriptorSetLayout(rt.device.device, &set_info, NULL, set_layout) != VK_SUCCESS)
		return 0;
	memset(&layout_info, 0, sizeof(layout_info));
	layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout_info.setLayoutCount = 1;
	layout_info.pSetLayouts = set_layout;
	if (vkCreatePipelineLayout(rt.device.device, &layout_info, NULL, layout) != VK_SUCCESS)
		return 0;
	module = shader_make(words, word_count);
	if (!module)
		return 0;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	info.stage.module = module;
	info.stage.pName = "main";
	info.layout = *layout;
	result = vkCreateComputePipelines(rt.device.device, VK_NULL_HANDLE, 1, &info, NULL, pipeline);
	vkDestroyShaderModule(rt.device.device, module, NULL);
	return result == VK_SUCCESS;
}

static int make(void)
{
	static const VkDescriptorType trace_types[] =
	{
		VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
		VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
	};
	static const VkDescriptorType probes_types[] =
	{
		VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
	};
	static const VkDescriptorType resolve_types[] =
	{
		VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
	};
	VkPhysicalDeviceAccelerationStructurePropertiesKHR structure_properties;
	VkPhysicalDeviceProperties2 properties;
	PFN_vkGetPhysicalDeviceProperties2 get_properties;
	VkSamplerCreateInfo sampler;
	uint32_t index;

	if (rt.tried)
		return rt.made;
	rt.tried = 1;
	if (!xgpu_vulkan_rt_device(&rt.device))
		return 0;
#define LOAD(name) name = (PFN_##name)rt.device.get_device_proc(rt.device.device, #name); \
	if (!name) { host_logf(HOST_LOG_WARN, "prism rt: no %s: no ray tracing", #name); return 0; }
	RT_FUNCTIONS(LOAD)
#undef LOAD
	rt.scratch_alignment = 256;
	get_properties = rt.device.get_physical_properties2;
	if (get_properties)
	{
		memset(&structure_properties, 0, sizeof(structure_properties));
		structure_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
		memset(&properties, 0, sizeof(properties));
		properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
		properties.pNext = &structure_properties;
		get_properties(rt.device.physical, &properties);
		if (structure_properties.minAccelerationStructureScratchOffsetAlignment > rt.scratch_alignment)
			rt.scratch_alignment = structure_properties.minAccelerationStructureScratchOffsetAlignment;
	}
	if (!pipeline_make(trace_types, 5, xgpu_vulkan_rt_trace_words, xgpu_vulkan_rt_trace_count, &rt.trace_layout,
		&rt.trace_pipeline_layout, &rt.trace) ||
		!pipeline_make(resolve_types, 6, xgpu_vulkan_rt_resolve_words, xgpu_vulkan_rt_resolve_count,
		&rt.resolve_layout, &rt.resolve_pipeline_layout, &rt.resolve) ||
		!pipeline_make(probes_types, 3, xgpu_vulkan_rt_probes_words, xgpu_vulkan_rt_probes_count,
		&rt.probes_layout, &rt.probes_pipeline_layout, &rt.probes) ||
		!buffer_make((VkDeviceSize)RT_PROBES_MOST * 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
		VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0, &rt.probe_buffer))
	{
		host_logf(HOST_LOG_WARN, "prism rt: its compute pipelines cannot be made: no ray tracing");
		return 0;
	}
	memset(&sampler, 0, sizeof(sampler));
	sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
	sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.maxLod = 0.0f;
	if (vkCreateSampler(rt.device.device, &sampler, NULL, &rt.nearest) != VK_SUCCESS)
		return 0;
	sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
	if (vkCreateSampler(rt.device.device, &sampler, NULL, &rt.linear) != VK_SUCCESS)
		return 0;
	for (index = 0; index < RT_SLOTS; index++)
	{
		struct rt_slot *slot = &rt.slots[index];
		VkDescriptorPoolSize sizes[5];
		VkDescriptorPoolCreateInfo pool;

		if (!buffer_make(RT_GEOMETRY_BYTES, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1, &slot->geometry) ||
			!buffer_make(RT_INSTANCES * sizeof(VkAccelerationStructureInstanceKHR),
			VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, 1, &slot->instances) ||
			!buffer_make(RT_STORAGE_BYTES, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, 0, &slot->storage) ||
			!buffer_make(RT_SCRATCH_BYTES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0, &slot->scratch))
		{
			host_logf(HOST_LOG_WARN, "prism rt: no memory for its buffers: no ray tracing");
			return 0;
		}
		sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		sizes[0].descriptorCount = 64;
		sizes[1].type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
		sizes[1].descriptorCount = 24;
		sizes[2].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		sizes[2].descriptorCount = 48;
		sizes[3].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		sizes[3].descriptorCount = 48;
		sizes[4].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		sizes[4].descriptorCount = 32;
		memset(&pool, 0, sizeof(pool));
		pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		pool.maxSets = 48;
		pool.poolSizeCount = 5;
		pool.pPoolSizes = sizes;
		if (vkCreateDescriptorPool(rt.device.device, &pool, NULL, &slot->pool) != VK_SUCCESS)
			return 0;
	}
	if (rt.device.timestamp_period > 0.0f)
	{
		VkQueryPoolCreateInfo info;

		memset(&info, 0, sizeof(info));
		info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
		info.queryType = VK_QUERY_TYPE_TIMESTAMP;
		info.queryCount = RT_SLOTS * 2;
		if (vkCreateQueryPool(rt.device.device, &info, NULL, &rt.queries) != VK_SUCCESS)
			rt.queries = VK_NULL_HANDLE;
	}
	{
		const char *stats = getenv("HALO_VULKAN_STATS"), *gpu = getenv("HALO_GPU_STATS");

		rt.statistics = (stats && *stats && strcmp(stats, "0")) || (gpu && *gpu && strcmp(gpu, "0"));
	}
	rt.made = 1;
	host_logf(HOST_LOG_INFO, "prism rt: made (ray queries in compute; %u MB a frame's slot, %u slots)",
		(unsigned)((RT_GEOMETRY_BYTES + RT_STORAGE_BYTES + RT_SCRATCH_BYTES) >> 20), (unsigned)RT_SLOTS);
	return 1;
}

int xgpu_vulkan_rt_available(void)
{
	struct xgpu_vulkan_rt_device device;

	/* (the device's alone: its buffers and pipelines are made with the
	first frame that traces, never for a game that does not) */
	return xgpu_vulkan_rt_device(&device);
}

/* ---------- the frame's slot */

static void slot_timing_read(struct rt_slot *slot, uint32_t index)
{
	uint64_t times[2];

	if (!slot->timed || !rt.queries)
		return;
	slot->timed = 0;
	if (vkGetQueryPoolResults(rt.device.device, rt.queries, index * 2, 2, sizeof(times), times, sizeof(times[0]),
		VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && times[1] >= times[0])
	{
		double milliseconds = (double)(times[1] - times[0]) * (double)rt.device.timestamp_period * 1e-6;

		rt.milliseconds += milliseconds;
		if (milliseconds > rt.milliseconds_most)
			rt.milliseconds_most = milliseconds;
		rt.timed_frames++;
	}
}

/* the next slot, its last frame done (waited for if it is not: it never is
with three) and its structures and sets let go */
static struct rt_slot *slot_take(void)
{
	struct rt_slot *slot;
	uint32_t index;

	rt.slot = (rt.slot + 1) % RT_SLOTS;
	slot = &rt.slots[rt.slot];
	if (slot->serial && slot->serial < xgpu_vulkan_serial() && slot->serial > xgpu_vulkan_completed())
		xgpu_vulkan_wait(slot->serial);
	if (slot->serial >= xgpu_vulkan_serial() && slot->serial)
		return NULL;
	slot_timing_read(slot, rt.slot);
	for (index = 0; index < slot->made_count; index++)
		vkDestroyAccelerationStructureKHR(rt.device.device, slot->made[index], NULL);
	slot->made_count = 0;
	vkResetDescriptorPool(rt.device.device, slot->pool, 0);
	slot->geometry_used = slot->storage_used = slot->scratch_used = 0;
	slot->instances_used = 0;
	return slot;
}

static VkDeviceSize align_up(VkDeviceSize value, VkDeviceSize alignment)
{
	return (value + alignment - 1) & ~(alignment - 1);
}

/* ---------- RT_FRAME and RT_STATIC */

void xgpu_vulkan_rt_frame(const struct xgpu_stream_rt_frame *record)
{
	if (!make())
		return;
	garbage_collect();
	rt.frame = *record;
	rt.frame_active = record->mode != XGPU_RT_OFF;
	rt.slot_ready = 0;
	if (rt.frame_active && slot_take())
		rt.slot_ready = 1;
	if (!rt.slot_ready)
		rt.frame_active = 0;
}

int xgpu_vulkan_rt_active(void)
{
	return rt.made && rt.frame_active && rt.slot_ready;
}

/* a whole set built: one bottom-level structure, its triangles in a buffer
of their own until the build is done */
static void static_build(void)
{
	VkAccelerationStructureGeometryKHR geometry;
	VkAccelerationStructureBuildGeometryInfoKHR info;
	VkAccelerationStructureBuildSizesInfoKHR sizes;
	VkAccelerationStructureBuildRangeInfoKHR range;
	const VkAccelerationStructureBuildRangeInfoKHR *ranges = &range;
	VkAccelerationStructureCreateInfoKHR create;
	VkAccelerationStructureDeviceAddressInfoKHR address;
	VkMemoryBarrier barrier;
	struct rt_buffer vertices, scratch, storage;
	VkAccelerationStructureKHR structure = VK_NULL_HANDLE;
	VkCommandBuffer commands;
	uint32_t count = rt.receiving_count;

	if (!count)
		return;
	if (!buffer_make((VkDeviceSize)count * 36, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1, &vertices))
		return;
	memcpy(vertices.mapped, rt.receiving, (size_t)count * 36);
	memset(&geometry, 0, sizeof(geometry));
	geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
	geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
	geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
	geometry.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
	geometry.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
	geometry.geometry.triangles.vertexData.deviceAddress = vertices.address;
	geometry.geometry.triangles.vertexStride = 12;
	geometry.geometry.triangles.maxVertex = count * 3 - 1;
	geometry.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
	info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
	info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	info.geometryCount = 1;
	info.pGeometries = &geometry;
	memset(&sizes, 0, sizeof(sizes));
	sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
	vkGetAccelerationStructureBuildSizesKHR(rt.device.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info,
		&count, &sizes);
	if (!buffer_make(sizes.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, 0,
		&storage))
	{
		buffer_free(&vertices);
		return;
	}
	if (!buffer_make(sizes.buildScratchSize + rt.scratch_alignment, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0, &scratch))
	{
		buffer_free(&vertices);
		buffer_free(&storage);
		return;
	}
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
	create.buffer = storage.buffer;
	create.size = sizes.accelerationStructureSize;
	create.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	if (vkCreateAccelerationStructureKHR(rt.device.device, &create, NULL, &structure) != VK_SUCCESS)
	{
		buffer_free(&vertices);
		buffer_free(&storage);
		buffer_free(&scratch);
		return;
	}
	info.dstAccelerationStructure = structure;
	info.scratchData.deviceAddress = align_up(scratch.address, rt.scratch_alignment);
	memset(&range, 0, sizeof(range));
	range.primitiveCount = count;
	commands = xgpu_vulkan_compute_commands();
	vkCmdBuildAccelerationStructuresKHR(commands, 1, &info, &ranges);
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
	barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
	vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
		VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier,
		0, NULL, 0, NULL);
	/* (the last set's after this submission, the build's scratch too; its
	triangles kept, which gi's rays read the faces' normals of) */
	if (rt.static_structure)
		garbage_add(&rt.static_storage, rt.static_structure);
	if (rt.static_vertices.buffer)
		garbage_add(&rt.static_vertices, VK_NULL_HANDLE);
	garbage_add(&scratch, VK_NULL_HANDLE);
	rt.static_vertices = vertices;
	rt.static_storage = storage;
	rt.static_structure = structure;
	memset(&address, 0, sizeof(address));
	address.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
	address.accelerationStructure = structure;
	rt.static_address = vkGetAccelerationStructureDeviceAddressKHR(rt.device.device, &address);
	rt.static_triangles = count;
	rt.static_generation = rt.receiving_generation;
	host_logf(HOST_LOG_INFO, "prism rt: the structure's acceleration structure: %u triangles, %.1f MB "
		"(scratch %.1f MB)", count, (double)sizes.accelerationStructureSize / 1048576.0,
		(double)sizes.buildScratchSize / 1048576.0);
}

void xgpu_vulkan_rt_static(const struct xgpu_stream_rt_static *record, const float *positions)
{
	if (!make())
		return;
	switch (record->phase)
	{
	case XGPU_RT_STATIC_BEGIN:
		free(rt.receiving);
		rt.receiving = record->triangles ? malloc((size_t)record->triangles * 36) : NULL;
		rt.receiving_generation = record->generation;
		rt.receiving_total = rt.receiving ? record->triangles : 0;
		rt.receiving_count = 0;
		break;
	case XGPU_RT_STATIC_TRIANGLES:
		if (!rt.receiving || record->generation != rt.receiving_generation || !positions ||
			record->first > rt.receiving_total || record->count > rt.receiving_total - record->first)
			break;
		memcpy(rt.receiving + (size_t)record->first * 9, positions, (size_t)record->count * 36);
		if (record->first + record->count > rt.receiving_count)
			rt.receiving_count = record->first + record->count;
		break;
	case XGPU_RT_STATIC_END:
		if (rt.receiving && record->generation == rt.receiving_generation)
			static_build();
		free(rt.receiving);
		rt.receiving = NULL;
		rt.receiving_count = rt.receiving_total = 0;
		break;
	default:
		free(rt.receiving);
		rt.receiving = NULL;
		rt.receiving_count = rt.receiving_total = 0;
		if (rt.static_structure)
			garbage_add(&rt.static_storage, rt.static_structure);
		if (rt.static_vertices.buffer)
			garbage_add(&rt.static_vertices, VK_NULL_HANDLE);
		rt.static_structure = VK_NULL_HANDLE;
		rt.static_address = 0;
		rt.static_triangles = 0;
		rt.history_valid = 0;
		break;
	}
}

/* ---------- the objects' capture */

void xgpu_vulkan_rt_window_begin(void)
{
	struct rt_slot *slot = &rt.slots[rt.slot];

	memset(&rt.capture, 0, sizeof(rt.capture));
	if (!xgpu_vulkan_rt_active())
		return;
	/* (the slot's geometry left after the windows before: vertices in its
	first two thirds, triangles' indices in the rest) */
	{
		VkDeviceSize left = slot->geometry.size - slot->geometry_used;

		rt.capture.vertex_offset = align_up(slot->geometry_used, 256);
		rt.capture.vertex_room = (uint32_t)((left * 5 / 9) / 12);
		rt.capture.index_offset = align_up(rt.capture.vertex_offset + (VkDeviceSize)rt.capture.vertex_room * 12, 256);
		if (rt.capture.index_offset >= slot->geometry.size)
			return;
		rt.capture.index_room = (uint32_t)((slot->geometry.size - rt.capture.index_offset) / 12);
		if (rt.capture.index_room > RT_OBJECT_TRIANGLES)
			rt.capture.index_room = RT_OBJECT_TRIANGLES;
		rt.capture.open = 1;
	}
}

void xgpu_vulkan_rt_skipped(void)
{
	if (rt.capture.open)
		rt.skipped++;
}

static inline float rt_read_float(const uint8_t *bytes)
{
	float value;

	memcpy(&value, bytes, sizeof(value));
	return value;
}

void xgpu_vulkan_rt_capture(const struct xgpu_vulkan_rt_draw *draw)
{
	struct rt_slot *slot = &rt.slots[rt.slot];
	struct rt_capture *capture = &rt.capture;
	uint32_t lowest = 0xffffffffu, highest = 0, index, vertices, triangles = 0, first;
	float *to_vertices, bounds[6] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
	uint32_t *to_indices;

	if (!capture->open || capture->full || !draw->vertices || !draw->count || !draw->stride || !draw->constants)
		return;
	/* the vertices it names */
	if (draw->indices)
	{
		for (index = 0; index < draw->count; index++)
		{
			int32_t vertex = (int32_t)draw->indices[index] - draw->index_base;

			if (vertex < 0)
				return;
			if ((uint32_t)vertex < lowest)
				lowest = (uint32_t)vertex;
			if ((uint32_t)vertex > highest)
				highest = (uint32_t)vertex;
		}
	}
	else
	{
		lowest = 0;
		highest = draw->count - 1;
	}
	vertices = highest - lowest + 1;
	if (draw->occluder && capture->occluder_count == RT_CAPTURED_DRAWS)
		return;
	if ((uint64_t)(highest + 1) * draw->stride > draw->bytes || vertices > 65536)
		return;
	if (capture->vertex_count + vertices > capture->vertex_room)
	{
		capture->full = 1;
		rt.full_frames++;
		return;
	}
	first = capture->vertex_count;
	to_vertices = (float *)((uint8_t *)slot->geometry.mapped + capture->vertex_offset) + (size_t)first * 3;
	/* skinned as the stock vertex program skins it: two nodes' matrices
	(three rows each from c[60]), the first's weight, the second's the rest */
	for (index = 0; index < vertices; index++)
	{
		const uint8_t *vertex = draw->vertices + (size_t)(lowest + index) * draw->stride;
		float p[3], w = 1.0f, out[3];
		unsigned n0 = 0, n1 = 0;
		int axis;

		p[0] = rt_read_float(vertex + draw->position_offset);
		p[1] = rt_read_float(vertex + draw->position_offset + 4);
		p[2] = rt_read_float(vertex + draw->position_offset + 8);
		if (draw->nodes_offset >= 0)
		{
			n0 = vertex[draw->nodes_offset];
			n1 = vertex[draw->nodes_offset + 1];
		}
		if (draw->weight_offset >= 0)
		{
			int16_t weight;

			memcpy(&weight, vertex + draw->weight_offset, sizeof(weight));
			w = (float)weight / 32767.0f;
		}
		if (n0 > 189)
			n0 = 0;
		if (n1 > 189)
			n1 = 0;
		for (axis = 0; axis < 3; axis++)
		{
			const float *a = draw->constants[60 + n0 + axis], *b = draw->constants[60 + n1 + axis];
			float pa = a[0] * p[0] + a[1] * p[1] + a[2] * p[2] + a[3];
			float pb = b[0] * p[0] + b[1] * p[1] + b[2] * p[2] + b[3];

			out[axis] = w * pa + (1.0f - w) * pb;
		}
		if (!isfinite(out[0]) || !isfinite(out[1]) || !isfinite(out[2]))
			out[0] = out[1] = out[2] = 0.0f;
		if (draw->occluder)
		{
			for (axis = 0; axis < 3; axis++)
			{
				if (!index || out[axis] < bounds[axis])
					bounds[axis] = out[axis];
				if (!index || out[axis] > bounds[axis + 3])
					bounds[axis + 3] = out[axis];
			}
		}
		to_vertices[index * 3] = out[0];
		to_vertices[index * 3 + 1] = out[1];
		to_vertices[index * 3 + 2] = out[2];
	}
	/* its triangles, a list (a strip's degenerate joins left out) */
	to_indices = (uint32_t *)((uint8_t *)slot->geometry.mapped + capture->index_offset) +
		(size_t)capture->triangle_count * 3;
#define RT_VERTEX(i) (first + (uint32_t)((draw->indices ? (int32_t)draw->indices[i] - draw->index_base : (int32_t)(i)) - \
	(int32_t)lowest))
	{
		uint32_t count = draw->count, room = capture->index_room - capture->triangle_count, t;
		uint32_t total = draw->primitive == XGPU_VULKAN_RT_LIST ? count / 3 : count >= 3 ? count - 2 : 0;

		for (t = 0; t < total && triangles < room; t++)
		{
			uint32_t a, b, c;

			if (draw->primitive == XGPU_VULKAN_RT_LIST)
			{
				a = RT_VERTEX(t * 3);
				b = RT_VERTEX(t * 3 + 1);
				c = RT_VERTEX(t * 3 + 2);
			}
			else if (draw->primitive == XGPU_VULKAN_RT_STRIP)
			{
				a = RT_VERTEX(t);
				b = RT_VERTEX(t + 1);
				c = RT_VERTEX(t + 2);
			}
			else
			{
				a = RT_VERTEX(0);
				b = RT_VERTEX(t + 1);
				c = RT_VERTEX(t + 2);
			}
			if (a == b || b == c || a == c)
				continue;
			to_indices[triangles * 3] = a;
			to_indices[triangles * 3 + 1] = b;
			to_indices[triangles * 3 + 2] = c;
			triangles++;
		}
		if (t < total)
		{
			capture->full = 1;
			rt.full_frames++;
		}
	}
#undef RT_VERTEX
	if (draw->occluder && triangles)
	{
		capture->occluders[capture->occluder_count].first = capture->triangle_count;
		capture->occluders[capture->occluder_count].count = triangles;
		memcpy(capture->occluders[capture->occluder_count].bounds, bounds, sizeof(bounds));
		capture->occluder_count++;
	}
	capture->vertex_count += vertices;
	capture->triangle_count += triangles;
	rt.draws++;
}

/* ---------- a window's structures and passes */

static void barrier_global(VkCommandBuffer commands, VkPipelineStageFlags from, VkAccessFlags from_access,
	VkPipelineStageFlags to, VkAccessFlags to_access)
{
	VkMemoryBarrier barrier;

	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	barrier.srcAccessMask = from_access;
	barrier.dstAccessMask = to_access;
	vkCmdPipelineBarrier(commands, from, to, 0, 1, &barrier, 0, NULL, 0, NULL);
}

/* an acceleration structure of the slot's storage (NULL: no room) */
static VkAccelerationStructureKHR slot_structure(struct rt_slot *slot, VkAccelerationStructureTypeKHR type,
	VkDeviceSize size, VkDeviceAddress *address)
{
	VkAccelerationStructureCreateInfoKHR create;
	VkAccelerationStructureDeviceAddressInfoKHR info;
	VkAccelerationStructureKHR structure = VK_NULL_HANDLE;
	VkDeviceSize offset = align_up(slot->storage_used, 256);

	if (offset + size > slot->storage.size || slot->made_count == sizeof(slot->made) / sizeof(slot->made[0]))
		return VK_NULL_HANDLE;
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
	create.buffer = slot->storage.buffer;
	create.offset = offset;
	create.size = size;
	create.type = type;
	if (vkCreateAccelerationStructureKHR(rt.device.device, &create, NULL, &structure) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	slot->storage_used = offset + size;
	slot->made[slot->made_count++] = structure;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
	info.accelerationStructure = structure;
	*address = vkGetAccelerationStructureDeviceAddressKHR(rt.device.device, &info);
	return structure;
}

static VkDeviceAddress slot_scratch(struct rt_slot *slot, VkDeviceSize size)
{
	VkDeviceSize offset = align_up(slot->scratch_used, rt.scratch_alignment);

	if (offset + size > slot->scratch.size)
		return 0;
	slot->scratch_used = offset + size;
	return align_up(slot->scratch.address + offset, rt.scratch_alignment);
}

/* the occluders all within 1 world unit of the eye (the first-person
weapon and hands, which CE never let cast shadows) taken out of the
window's triangles */
static void first_person_drop(struct rt_slot *slot, const float eye[3])
{
	uint32_t *indices = (uint32_t *)((uint8_t *)slot->geometry.mapped + rt.capture.index_offset);
	uint32_t index, removed = 0;

	for (index = rt.capture.occluder_count; index-- > 0;)
	{
		const float *bounds = rt.capture.occluders[index].bounds;
		uint32_t first = rt.capture.occluders[index].first, count = rt.capture.occluders[index].count;
		float far = 0.0f;
		int axis;

		for (axis = 0; axis < 3; axis++)
		{
			float a = fabsf(bounds[axis] - eye[axis]), b = fabsf(bounds[axis + 3] - eye[axis]);

			far += (a > b ? a : b) * (a > b ? a : b);
		}
		if (far > 1.0f)
			continue;
		memmove(indices + (size_t)first * 3, indices + (size_t)(first + count) * 3,
			(size_t)(rt.capture.triangle_count - first - count) * 12);
		rt.capture.triangle_count -= count;
		removed += count;
	}
	(void)removed;
}

/* the objects' bottom-level structure (0: none) */
static VkDeviceAddress objects_build(VkCommandBuffer commands, struct rt_slot *slot)
{
	VkAccelerationStructureGeometryKHR geometry;
	VkAccelerationStructureBuildGeometryInfoKHR info;
	VkAccelerationStructureBuildSizesInfoKHR sizes;
	VkAccelerationStructureBuildRangeInfoKHR range;
	const VkAccelerationStructureBuildRangeInfoKHR *ranges = &range;
	VkDeviceAddress address = 0, scratch;
	uint32_t count = rt.capture.triangle_count;

	if (!count)
		return 0;
	memset(&geometry, 0, sizeof(geometry));
	geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
	geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
	geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
	geometry.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
	geometry.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
	geometry.geometry.triangles.vertexData.deviceAddress = slot->geometry.address + rt.capture.vertex_offset;
	geometry.geometry.triangles.vertexStride = 12;
	geometry.geometry.triangles.maxVertex = rt.capture.vertex_count ? rt.capture.vertex_count - 1 : 0;
	geometry.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
	geometry.geometry.triangles.indexData.deviceAddress = slot->geometry.address + rt.capture.index_offset;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
	info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
	info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	info.geometryCount = 1;
	info.pGeometries = &geometry;
	memset(&sizes, 0, sizeof(sizes));
	sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
	vkGetAccelerationStructureBuildSizesKHR(rt.device.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info,
		&count, &sizes);
	info.dstAccelerationStructure = slot_structure(slot, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
		sizes.accelerationStructureSize, &address);
	scratch = slot_scratch(slot, sizes.buildScratchSize);
	if (!info.dstAccelerationStructure || !scratch)
		return 0;
	info.scratchData.deviceAddress = scratch;
	memset(&range, 0, sizeof(range));
	range.primitiveCount = count;
	vkCmdBuildAccelerationStructuresKHR(commands, 1, &info, &ranges);
	return address;
}

/* the window's top-level structure: the structure's and the objects'
instances (identity: both are in world space) */
static VkAccelerationStructureKHR scene_build(VkCommandBuffer commands, struct rt_slot *slot,
	VkDeviceAddress objects)
{
	VkAccelerationStructureInstanceKHR *instances =
		(VkAccelerationStructureInstanceKHR *)slot->instances.mapped + slot->instances_used;
	VkAccelerationStructureGeometryKHR geometry;
	VkAccelerationStructureBuildGeometryInfoKHR info;
	VkAccelerationStructureBuildSizesInfoKHR sizes;
	VkAccelerationStructureBuildRangeInfoKHR range;
	const VkAccelerationStructureBuildRangeInfoKHR *ranges = &range;
	VkDeviceAddress address, scratch;
	uint32_t count = 0, first = slot->instances_used;

	if (first + 2 > RT_INSTANCES)
		return VK_NULL_HANDLE;
	if (rt.static_structure)
	{
		memset(&instances[count], 0, sizeof(instances[count]));
		instances[count].transform.matrix[0][0] = instances[count].transform.matrix[1][1] =
			instances[count].transform.matrix[2][2] = 1.0f;
		instances[count].mask = 0x01;
		instances[count].instanceCustomIndex = 0;
		instances[count].flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
		instances[count].accelerationStructureReference = rt.static_address;
		count++;
	}
	if (objects)
	{
		memset(&instances[count], 0, sizeof(instances[count]));
		instances[count].transform.matrix[0][0] = instances[count].transform.matrix[1][1] =
			instances[count].transform.matrix[2][2] = 1.0f;
		instances[count].mask = 0x04;
		instances[count].instanceCustomIndex = 1;
		instances[count].flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
		instances[count].accelerationStructureReference = objects;
		count++;
	}
	slot->instances_used += count;
	memset(&geometry, 0, sizeof(geometry));
	geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
	geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
	geometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
	geometry.geometry.instances.data.deviceAddress = slot->instances.address +
		first * sizeof(VkAccelerationStructureInstanceKHR);
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
	info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
	info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
	info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	info.geometryCount = 1;
	info.pGeometries = &geometry;
	memset(&sizes, 0, sizeof(sizes));
	sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
	vkGetAccelerationStructureBuildSizesKHR(rt.device.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info,
		&count, &sizes);
	info.dstAccelerationStructure = slot_structure(slot, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
		sizes.accelerationStructureSize, &address);
	scratch = slot_scratch(slot, sizes.buildScratchSize);
	if (!info.dstAccelerationStructure || !scratch)
		return VK_NULL_HANDLE;
	info.scratchData.deviceAddress = scratch;
	memset(&range, 0, sizeof(range));
	range.primitiveCount = count;
	vkCmdBuildAccelerationStructuresKHR(commands, 1, &info, &ranges);
	return info.dstAccelerationStructure;
}

static struct xgpu_vulkan_image *image_storage(struct xgpu_vulkan_image *image, uint8_t format, uint32_t width,
	uint32_t height)
{
	struct xgpu_vulkan_image_description description;

	if (image && xgpu_vulkan_image_description(image)->width == width &&
		xgpu_vulkan_image_description(image)->height == height)
		return image;
	if (image)
		xgpu_vulkan_image_destroy(image);
	memset(&description, 0, sizeof(description));
	description.type = XGPU_VULKAN_IMAGE_2D;
	description.format = format;
	description.usage = XGPU_VULKAN_USAGE_SAMPLED | XGPU_VULKAN_USAGE_STORAGE;
	description.samples = 1;
	description.width = width;
	description.height = height;
	description.depth_or_layers = 1;
	description.levels = 1;
	return xgpu_vulkan_image_create(&description);
}

static void write_image(VkWriteDescriptorSet *write, VkDescriptorImageInfo *image, VkDescriptorSet set,
	uint32_t binding, struct xgpu_vulkan_image *from, VkSampler sampler, VkDescriptorType type)
{
	memset(image, 0, sizeof(*image));
	image->sampler = sampler;
	image->imageView = xgpu_vulkan_image_view(from);
	image->imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	memset(write, 0, sizeof(*write));
	write->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write->dstSet = set;
	write->dstBinding = binding;
	write->descriptorCount = 1;
	write->descriptorType = type;
	write->pImageInfo = image;
}

static void statistics_frame(void)
{
	rt.frames++;
	rt.triangles += rt.capture.triangle_count;
	if (rt.capture.triangle_count > rt.object_triangles_most)
		rt.object_triangles_most = rt.capture.triangle_count;
	if (rt.frames < 600)
		return;
	if (rt.statistics)
		host_logf(HOST_LOG_INFO, "prism rt: 600 frames: its passes %.3f ms a frame on the GPU (most %.3f; %llu timed), "
			"structure %u triangles, objects %.0f triangles a frame (most %llu) of %.1f draws, %.1f draws passed over, "
			"%llu frames' capture full", rt.timed_frames ? rt.milliseconds / (double)rt.timed_frames : 0.0,
			rt.milliseconds_most, (unsigned long long)rt.timed_frames, rt.static_triangles,
			(double)rt.triangles / 600.0, (unsigned long long)rt.object_triangles_most, (double)rt.draws / 600.0,
			(double)rt.skipped / 600.0, (unsigned long long)rt.full_frames);
	rt.frames = rt.draws = rt.skipped = rt.triangles = rt.object_triangles_most = rt.full_frames = 0;
	rt.milliseconds = rt.milliseconds_most = 0.0;
	rt.timed_frames = 0;
}

struct xgpu_vulkan_image *xgpu_vulkan_rt_window_end(struct xgpu_vulkan_image *inputs, uint32_t width,
	uint32_t height, const struct xgpu_world_ao_pass *pass)
{
	/* rays a pixel: the sun's and the occlusion's, by quality (medium, low,
	high, ultra) */
	static const float sun_rays[4] = { 1.0f, 1.0f, 2.0f, 4.0f };
	static const float ao_rays[4] = { 1.0f, 1.0f, 2.0f, 4.0f };
	struct rt_slot *slot = &rt.slots[rt.slot];
	struct rt_pass block;
	struct xgpu_vulkan_range uniforms;
	VkDescriptorSetAllocateInfo allocate;
	VkDescriptorSetLayout layouts[3];
	VkDescriptorSet sets[3];
	VkWriteDescriptorSet writes[14];
	VkDescriptorImageInfo images[10];
	VkDescriptorBufferInfo buffer, probe_buffer;
	VkWriteDescriptorSetAccelerationStructureKHR scene_write;
	VkAccelerationStructureKHR scene;
	VkCommandBuffer commands;
	VkDeviceAddress objects;
	struct xgpu_vulkan_image *history, *state;
	uint32_t scale, out_width, out_height, quality, probes_updated = 0;
	int gi;
	void *bytes;

	if (!rt.capture.open || !xgpu_vulkan_rt_active() || !inputs)
	{
		rt.capture.open = 0;
		return NULL;
	}
	rt.capture.open = 0;
	scale = pass->camera[3][3] >= 2.0f ? 2u : 1u;
	out_width = (width + scale - 1) / scale;
	out_height = (height + scale - 1) / scale;
	if (out_width != rt.image_width || out_height != rt.image_height)
		rt.history_valid = 0;
	rt.raw = image_storage(rt.raw, XGPU_VULKAN_FORMAT_RGBA16F, out_width, out_height);
	rt.state[0] = image_storage(rt.state[0], XGPU_VULKAN_FORMAT_RGBA32F, out_width, out_height);
	rt.state[1] = image_storage(rt.state[1], XGPU_VULKAN_FORMAT_RGBA32F, out_width, out_height);
	rt.result = image_storage(rt.result, XGPU_VULKAN_FORMAT_RGBA32F, out_width, out_height);
	if (!rt.raw || !rt.state[0] || !rt.state[1] || !rt.result)
		return NULL;
	rt.image_width = out_width;
	rt.image_height = out_height;
	/* the block (first: its upload may submit the recording) */
	memset(&block, 0, sizeof(block));
	memcpy(block.camera, pass->camera, sizeof(block.camera));
	memcpy(block.corners, pass->corners, sizeof(block.corners));
	memcpy(block.previous, pass->previous, sizeof(block.previous));
	memcpy(block.previous_corners, pass->previous_corners, sizeof(block.previous_corners));
	memcpy(block.sun, rt.frame.sun, sizeof(block.sun));
	quality = rt.frame.quality < 4 ? rt.frame.quality : 0;
	block.strength[0] = rt.frame.settings[0];
	block.strength[1] = rt.frame.settings[1];
	/* (the sun's disc, widened: about 1.2 degrees) */
	block.strength[2] = 0.02f;
	/* (the new frame's share of the history) */
	block.strength[3] = rt.frame.denoiser == 1 ? 0.15f : 0.25f;
	block.rays[0] = sun_rays[quality];
	block.rays[1] = ao_rays[quality];
	block.rays[2] = 60.0f;
	block.rays[3] = 800.0f;
	block.info[0] = rt.frame.frame;
	block.info[1] = rt.frame.denoiser;
	block.info[2] = rt.history_valid && pass->history[0] != 0.0f ? 1u : 0u;
	/* (the debug views: the light's factor alone, the occlusion alone, the
structure's and the objects' visibility of the sun as they are) */
	block.info[3] = rt.frame.view == XGPU_RT_VIEW_VISIBILITY ? 1u : rt.frame.view == XGPU_RT_VIEW_AO ? 2u :
		rt.frame.view == XGPU_RT_VIEW_ALBEDO ? 3u : rt.frame.view == XGPU_RT_VIEW_PROBES ? 4u :
		rt.frame.view == XGPU_RT_VIEW_GI ? 2u : 0u;
	block.size[0] = (int32_t)width;
	block.size[1] = (int32_t)height;
	block.size[2] = (int32_t)scale;
	/* gi's probes: a volume about the camera, its sides and spacing by
	quality (medium 24 x 12 x 24 at 1.5 world units), a quarter of them
	updated a frame; the sky's outdoor radiance (its ambient radiosity) and
	the sun's (its colour by its power) */
	gi = rt.frame.mode >= XGPU_RT_GI && rt.static_vertices.buffer;
	{
		static const int32_t sides[4][3] = { { 24, 12, 24 }, { 16, 8, 16 }, { 32, 16, 32 }, { 32, 16, 32 } };
		static const float spacings[4] = { 1.5f, 2.0f, 1.5f, 1.0f };
		static const uint32_t probe_rays[4] = { 128, 64, 192, 256 };
		float spacing = spacings[quality];
		int axis;

		for (axis = 0; axis < 3; axis++)
		{
			block.probe_count[axis] = sides[quality][axis];
			block.probe_origin[axis] = (floorf(pass->camera[0][axis] / spacing) - (float)(sides[quality][axis] / 2)) *
				spacing;
		}
		block.probe_origin[3] = spacing;
		block.probe_count[3] = sides[quality][0] * sides[quality][1] * sides[quality][2];
		probes_updated = (uint32_t)block.probe_count[3] / 4u;
		block.probe_info[0] = (rt.probe_frame * probes_updated) % (uint32_t)block.probe_count[3];
		block.probe_info[1] = probe_rays[quality];
		block.probe_info[2] = rt.frame.mood;
		block.probe_info[3] = gi ? 1u : 0u;
		block.sky[0] = rt.frame.sky[0] * rt.frame.sky[3];
		block.sky[1] = rt.frame.sky[1] * rt.frame.sky[3];
		block.sky[2] = rt.frame.sky[2] * rt.frame.sky[3];
		block.sun_radiance[0] = rt.frame.sun_color[0] * rt.frame.sun_color[3];
		block.sun_radiance[1] = rt.frame.sun_color[1] * rt.frame.sun_color[3];
		block.sun_radiance[2] = rt.frame.sun_color[2] * rt.frame.sun_color[3];
		block.addresses[0] = (uint32_t)rt.static_vertices.address;
		block.addresses[1] = (uint32_t)(rt.static_vertices.address >> 32);
		block.addresses[2] = (uint32_t)(slot->geometry.address + rt.capture.vertex_offset);
		block.addresses[3] = (uint32_t)((slot->geometry.address + rt.capture.vertex_offset) >> 32);
		block.addresses2[0] = (uint32_t)(slot->geometry.address + rt.capture.index_offset);
		block.addresses2[1] = (uint32_t)((slot->geometry.address + rt.capture.index_offset) >> 32);
	}
	if (gi && !rt.logged_gi)
	{
		rt.logged_gi = 1;
		host_logf(HOST_LOG_INFO, "prism rt: gi's probes: %dx%dx%d at %.2f wu, %u rays, %u a frame; sky %.3f %.3f %.3f, "
			"sun %.3f %.3f %.3f", block.probe_count[0], block.probe_count[1], block.probe_count[2],
			(double)block.probe_origin[3], block.probe_info[1], probes_updated, (double)block.sky[0], (double)block.sky[1],
			(double)block.sky[2], (double)block.sun_radiance[0], (double)block.sun_radiance[1],
			(double)block.sun_radiance[2]);
	}
	bytes = xgpu_vulkan_upload(sizeof(block), 256, &uniforms);
	if (!bytes)
		return NULL;
	memcpy(bytes, &block, sizeof(block));
	history = rt.state[rt.state_index];
	state = rt.state[rt.state_index ^ 1];
	/* the images, marked as a copy marks them (the pre-pass's inputs stored) */
	xgpu_vulkan_image_compute_use(inputs, 0);
	xgpu_vulkan_image_compute_use(rt.raw, 1);
	xgpu_vulkan_image_compute_use(history, 0);
	xgpu_vulkan_image_compute_use(state, 1);
	xgpu_vulkan_image_compute_use(rt.result, 1);
	commands = xgpu_vulkan_compute_commands();
	if (rt.queries && !slot->timed)
	{
		vkCmdResetQueryPool(commands, rt.queries, rt.slot * 2, 2);
		vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, rt.queries, rt.slot * 2);
	}
	/* (the probes zeroed once: none valid) */
	if (!rt.probes_zeroed)
	{
		vkCmdFillBuffer(commands, rt.probe_buffer.buffer, 0, VK_WHOLE_SIZE, 0);
		barrier_global(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
		rt.probes_zeroed = 1;
	}
	/* the vertices written by the CPU, read by the builds */
	barrier_global(commands, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_WRITE_BIT,
		VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_SHADER_READ_BIT);
	first_person_drop(slot, pass->camera[0]);
	objects = objects_build(commands, slot);
	if (objects)
		barrier_global(commands, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
			VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
			VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
	slot->geometry_used = rt.capture.index_offset + (VkDeviceSize)rt.capture.triangle_count * 12;
	scene = scene_build(commands, slot, objects);
	if (!scene)
		return NULL;
	/* the scene, and the pre-pass's inputs (its pass's end barrier made
	them visible), read by the trace */
	barrier_global(commands, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
		VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_SHADER_READ_BIT);
	/* the sets */
	layouts[0] = rt.trace_layout;
	layouts[1] = rt.resolve_layout;
	layouts[2] = rt.probes_layout;
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocate.descriptorPool = slot->pool;
	allocate.descriptorSetCount = 3;
	allocate.pSetLayouts = layouts;
	if (vkAllocateDescriptorSets(rt.device.device, &allocate, sets) != VK_SUCCESS)
		return NULL;
	memset(&buffer, 0, sizeof(buffer));
	buffer.buffer = uniforms.buffer;
	buffer.offset = uniforms.offset;
	buffer.range = sizeof(block);
	memset(&scene_write, 0, sizeof(scene_write));
	scene_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
	scene_write.accelerationStructureCount = 1;
	scene_write.pAccelerationStructures = &scene;
	write_image(&writes[0], &images[0], sets[0], 0, inputs, rt.nearest, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	memset(&writes[1], 0, sizeof(writes[1]));
	writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[1].pNext = &scene_write;
	writes[1].dstSet = sets[0];
	writes[1].dstBinding = 1;
	writes[1].descriptorCount = 1;
	writes[1].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
	write_image(&writes[2], &images[2], sets[0], 2, rt.raw, VK_NULL_HANDLE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
	memset(&writes[3], 0, sizeof(writes[3]));
	writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[3].dstSet = sets[0];
	writes[3].dstBinding = 3;
	writes[3].descriptorCount = 1;
	writes[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	writes[3].pBufferInfo = &buffer;
	write_image(&writes[4], &images[4], sets[1], 0, inputs, rt.nearest, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	write_image(&writes[5], &images[5], sets[1], 1, rt.raw, rt.nearest, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	write_image(&writes[6], &images[6], sets[1], 2, history, rt.linear, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	write_image(&writes[7], &images[7], sets[1], 3, state, VK_NULL_HANDLE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
	write_image(&writes[8], &images[8], sets[1], 4, rt.result, VK_NULL_HANDLE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
	writes[9] = writes[3];
	writes[9].dstSet = sets[1];
	writes[9].dstBinding = 5;
	/* (the probes: the trace reads them, the probes' pass writes them) */
	memset(&probe_buffer, 0, sizeof(probe_buffer));
	probe_buffer.buffer = rt.probe_buffer.buffer;
	probe_buffer.range = VK_WHOLE_SIZE;
	writes[10] = writes[3];
	writes[10].dstBinding = 4;
	writes[10].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	writes[10].pBufferInfo = &probe_buffer;
	writes[11] = writes[1];
	writes[11].dstSet = sets[2];
	writes[11].dstBinding = 0;
	writes[12] = writes[10];
	writes[12].dstSet = sets[2];
	writes[12].dstBinding = 1;
	writes[13] = writes[3];
	writes[13].dstSet = sets[2];
	writes[13].dstBinding = 2;
	vkUpdateDescriptorSets(rt.device.device, 14, writes, 0, NULL);
	/* gi: the probes updated, a workgroup each, before the trace reads them */
	if (gi && probes_updated)
	{
		vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, rt.probes);
		vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, rt.probes_pipeline_layout, 0, 1, &sets[2], 0,
			NULL);
		vkCmdDispatch(commands, probes_updated, 1, 1);
		barrier_global(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
		rt.probe_frame++;
	}
	/* the trace, then the resolve */
	vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, rt.trace);
	vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, rt.trace_pipeline_layout, 0, 1, &sets[0], 0,
		NULL);
	vkCmdDispatch(commands, (out_width + 7) / 8, (out_height + 7) / 8, 1);
	barrier_global(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
	vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, rt.resolve);
	vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, rt.resolve_pipeline_layout, 0, 1, &sets[1], 0,
		NULL);
	vkCmdDispatch(commands, (out_width + 7) / 8, (out_height + 7) / 8, 1);
	/* what it wrote, read by the surfaces' draws (and the next frame's
	resolve) */
	barrier_global(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
	if (rt.queries && !slot->timed)
	{
		vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, rt.queries, rt.slot * 2 + 1);
		slot->timed = 1;
	}
	slot->serial = xgpu_vulkan_serial();
	rt.state_index ^= 1;
	rt.history_valid = 1;
	statistics_frame();
	if (!rt.logged_first)
	{
		rt.logged_first = 1;
		host_logf(HOST_LOG_INFO, "prism rt: first traced window %ux%u (scale %u): structure %u triangles, objects %u "
			"triangles of %llu draws", width, height, scale, rt.static_triangles, rt.capture.triangle_count,
			(unsigned long long)rt.draws);
	}
	return rt.result;
}
