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
  and the stock vertex layout is captured as it is drawn, by where its
  vertices, indices and constants already are on the GPU (the mirror's
  segments, the ring; the mesh store's skinned vertices and indices for a
  part a replacement mesh draws): nothing of them is read on the CPU. A
  compute pass (skin) skins each draw's triangles by its own node matrices
  (the stock vertex program's two-node blend) into a triangle soup of its
  own, so their permutations, levels of detail and pose are what is drawn.
  Each draw has its own bottom-level structure, kept from frame to frame
  (an entry, display.prism_rt_cache): a draw whose vertices, indices and
  matrices are as they were is neither skinned nor built again; one that
  moved is skinned and its structure refitted (an update, the same
  triangles), built again every RT_REBUILD_UPDATES refits or where it
  jumped far. The first-person weapon (the pre-pass's occluders all within
  a world unit of the eye: the mask pass, on the GPU) is left out.
  Alpha-tested draws are passed over (counted).
- The frame: one top-level structure a window (the structure, mask 1; each
  object draw's instance, mask 4), built after the pre-pass, so the rays see
  the frame's drawn pose. Then two compute passes (xgpu_vulkan_rt_glsl.inc): trace, at
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
/* a slot's: the windows' jobs and tables (host-visible), the top levels,
the builds' scratch (the objects' structures are the entries', kept in
their pool) */
#define RT_GEOMETRY_BYTES (8u << 20)
#define RT_STORAGE_BYTES (8u << 20)
#define RT_SCRATCH_BYTES (48u << 20)
/* a slot's instances (its windows' together) */
#define RT_INSTANCES 8192u
#define RT_GARBAGE 256u
/* the objects' triangles a window at most (past it: the draws drawn last
are left out) */
#define RT_OBJECT_TRIANGLES 1500000u
/* the objects' entries (each draw's acceleration structure kept from frame
to frame), their hash's buckets, the frames an entry not traced is kept,
the refits before it is built again, how far node 0 may jump and the entry
still be refitted (world units) */
#define RT_ENTRIES 4096u
#define RT_ENTRY_BUCKETS 8192u
#define RT_ENTRY_IDLE_FRAMES 300u
#define RT_REBUILD_UPDATES 48u
#define RT_REFIT_DISTANCE 3.0f
/* the node rows an entry compares (c[60] to c[191]) */
#define RT_NODE_ROWS 132u
/* the entries' memory: chunks of device memory (a larger entry has one of
its own), their free ranges at most */
#define RT_POOL_CHUNK (32ull << 20)
/* the pool's size past which the entries not traced lately are let go of
(and new ones wait) */
#define RT_POOL_MOST (320ull << 20)
#define RT_POOL_CHUNKS 96u
#define RT_POOL_RANGES 1024u
/* a window's jobs (draws skinned) and draws captured at most */
#define RT_JOBS 4096u
#define RT_JOB_WORDS 24u
#define RT_WINDOW_DRAWS 4096u
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
	/* "pathtraced": the bounces, the paths a pixel, ReSTIR's candidates,
	the lights; the mood (0 to 1), the dynamic lights' shadows' strength, the
	albedo the hits take, the spatial neighbours ReSTIR reuses; the lights
	(three vec4s each: place and radius, colour by intensity, direction and
	cone cosine) */
	float pt[4];
	float pt2[4];
	float lights[XGPU_STREAM_RT_LIGHTS * 3][4];
};

/* ---------- the functions */

#define RT_FUNCTIONS(F) \
	F(vkCreateBuffer) F(vkDestroyBuffer) F(vkGetBufferMemoryRequirements) F(vkAllocateMemory) F(vkFreeMemory) \
	F(vkBindBufferMemory) F(vkMapMemory) F(vkCreateDescriptorSetLayout) F(vkCreatePipelineLayout) \
	F(vkCreateShaderModule) F(vkDestroyShaderModule) F(vkCreateComputePipelines) F(vkCreateDescriptorPool) \
	F(vkResetDescriptorPool) F(vkAllocateDescriptorSets) F(vkUpdateDescriptorSets) F(vkCmdBindPipeline) \
	F(vkCmdBindDescriptorSets) F(vkCmdDispatch) F(vkCmdPushConstants) F(vkCmdPipelineBarrier) F(vkCreateSampler) F(vkCreateQueryPool) \
	F(vkCmdResetQueryPool) F(vkCmdWriteTimestamp) F(vkCmdFillBuffer) F(vkGetQueryPoolResults) F(vkGetBufferDeviceAddress) \
	F(vkCreateAccelerationStructureKHR) F(vkDestroyAccelerationStructureKHR) \
	F(vkGetAccelerationStructureBuildSizesKHR) F(vkCmdBuildAccelerationStructuresKHR) \
	F(vkGetAccelerationStructureDeviceAddressKHR) F(vkCmdCopyBuffer)

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
	/* the entries' info as its frame left it (the nodes their vertices
	name, read once the frame is done: entry_rows_learn), and whether it
	was copied */
	struct rt_buffer readback;
	int read_back;
};

/* an object draw's acceleration structure, kept from frame to frame: what
the draw is (its key: identity, primitive, counts, occurrence), its
triangle soup and structure in a range of a pool chunk, how it was last
skinned (its node rows, the rows its vertices name once known, its pose,
its bytes' generations, node 0's place) */
struct rt_entry
{
	int used;
	uint64_t key[8];
	uint32_t next;
	uint32_t triangles;
	uint32_t chunk;
	VkDeviceSize offset, size, structure_offset, structure_size, build_scratch, update_scratch;
	VkAccelerationStructureKHR structure;
	VkDeviceAddress structure_address, soup_address;
	int built, skinned, occluder;
	uint32_t updates;
	float rows[RT_NODE_ROWS][4];
	uint32_t rows_known;
	uint64_t pose;
	uint32_t generation[2];
	float origin[3];
	uint64_t frame, skinned_serial;
	/* this window's: its job (or none), its mode (build, update, none) */
	int job, mode;
};

struct rt_pool_range
{
	VkDeviceSize offset, size;
};

struct rt_pool_chunk
{
	struct rt_buffer buffer;
	struct rt_pool_range free[RT_POOL_RANGES];
	uint32_t free_count;
};

/* an entry's range and structure let go of once the GPU has passed serial */
struct rt_pool_garbage
{
	uint64_t serial;
	uint32_t chunk;
	VkDeviceSize offset, size;
	VkAccelerationStructureKHR structure;
};
#define RT_POOL_GARBAGE 4096u

/* a window's capture: its draws' entries (the occluders' marked), its
jobs (the slot's geometry: RT_JOB_WORDS words each, then the workgroups'
table), the occurrences of each draw's identity so far */
struct rt_capture
{
	int open, full;
	uint64_t serial;
	uint32_t draws[RT_WINDOW_DRAWS];
	uint32_t draw_count;
	uint32_t triangle_count;
	VkDeviceSize jobs_offset;
	uint32_t *jobs;
	uint32_t job_count, job_groups, job_triangles;
	struct
	{
		uint64_t hash;
		uint32_t count, stamp;
	} occurrences[RT_WINDOW_DRAWS * 2];
	uint32_t stamp;
};

static struct
{
	int tried, made;
	struct xgpu_vulkan_rt_device device;
	VkDeviceSize scratch_alignment;
	VkDescriptorSetLayout trace_layout, resolve_layout, probes_layout;
	VkPipelineLayout trace_pipeline_layout, resolve_pipeline_layout, probes_pipeline_layout;
	VkPipeline trace, resolve, probes;
	/* "pathtraced": the path tracer and its denoiser's two halves */
	VkDescriptorSetLayout path_layout, accumulate_layout, atrous_layout;
	VkPipelineLayout path_pipeline_layout, accumulate_pipeline_layout, atrous_pipeline_layout;
	VkPipeline path, accumulate, atrous;
	/* the objects' passes (no descriptors: buffers by their addresses) */
	VkDescriptorSetLayout skin_layout, reset_layout, mask_layout;
	VkPipelineLayout skin_pipeline_layout, reset_pipeline_layout, mask_pipeline_layout;
	VkPipeline skin, reset, mask;
	/* the objects' entries, their hash's buckets (index + 1, 0 none), their
	info on the GPU (eight words each: skin's), the pool their soups and
	structures are of, its garbage */
	struct rt_entry *entries;
	uint32_t buckets[RT_ENTRY_BUCKETS];
	uint32_t entry_count, entry_cursor;
	struct rt_buffer info;
	int info_zeroed;
	struct rt_pool_chunk *chunks[RT_POOL_CHUNKS];
	struct rt_pool_garbage pool_garbage[RT_POOL_GARBAGE];
	uint32_t pool_garbage_count;
	VkDeviceSize pool_bytes;
	int pool_full;
	uint64_t object_frame;
	/* the top level's sizes for RT_INSTANCES instances */
	VkAccelerationStructureBuildSizesInfoKHR tlas_sizes;
	/* the buffers' device addresses lately asked for */
	struct
	{
		VkBuffer buffer;
		VkDeviceAddress address;
	} addresses[16];
	uint32_t address_next;
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
	/* "pathtraced"'s images (every pixel's): the paths' samples (raw), their
	history (two in turn), the spatial passes' other pair, ReSTIR's
	reservoirs (two in turn); whether their history is valid */
	struct xgpu_vulkan_image *pt_raw[2], *pt_state[2][2], *pt_temporary[2], *pt_reservoirs[2];
	uint32_t pt_width, pt_height, pt_index;
	int pt_history_valid, logged_pt;
	/* statistics (600 frames) */
	uint64_t frames, draws, skipped, triangles, object_triangles_most, full_frames;
	uint64_t jobs, skinned_triangles, builds, updates, reused, evicted, stale, deferred, pieces;
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

static int pipeline_make_pushed(const VkDescriptorType *types, uint32_t count, const uint32_t *words,
	size_t word_count, uint32_t push_bytes, VkDescriptorSetLayout *set_layout, VkPipelineLayout *layout,
	VkPipeline *pipeline)
{
	VkPushConstantRange push;
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
	if (push_bytes)
	{
		memset(&push, 0, sizeof(push));
		push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		push.size = push_bytes;
		layout_info.pushConstantRangeCount = 1;
		layout_info.pPushConstantRanges = &push;
	}
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

static int pipeline_make(const VkDescriptorType *types, uint32_t count, const uint32_t *words, size_t word_count,
	VkDescriptorSetLayout *set_layout, VkPipelineLayout *layout, VkPipeline *pipeline)
{
	return pipeline_make_pushed(types, count, words, word_count, 0, set_layout, layout, pipeline);
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
	/* "pathtraced" (xgpu_vulkan_rt_glsl.inc's path, accumulate, atrous) */
	static const VkDescriptorType path_types[] =
	{
		VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
		VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
	};
	static const VkDescriptorType accumulate_types[] =
	{
		VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
	};
	static const VkDescriptorType atrous_types[] =
	{
		VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
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
		!pipeline_make(path_types, 8, xgpu_vulkan_rt_path_words, xgpu_vulkan_rt_path_count,
		&rt.path_layout, &rt.path_pipeline_layout, &rt.path) ||
		!pipeline_make(accumulate_types, 8, xgpu_vulkan_rt_accumulate_words, xgpu_vulkan_rt_accumulate_count,
		&rt.accumulate_layout, &rt.accumulate_pipeline_layout, &rt.accumulate) ||
		!pipeline_make_pushed(atrous_types, 7, xgpu_vulkan_rt_atrous_words, xgpu_vulkan_rt_atrous_count, 8,
		&rt.atrous_layout, &rt.atrous_pipeline_layout, &rt.atrous) ||
		!pipeline_make_pushed(NULL, 0, xgpu_vulkan_rt_skin_words, xgpu_vulkan_rt_skin_count, 32, &rt.skin_layout,
		&rt.skin_pipeline_layout, &rt.skin) ||
		!pipeline_make_pushed(NULL, 0, xgpu_vulkan_rt_reset_words, xgpu_vulkan_rt_reset_count, 32, &rt.reset_layout,
		&rt.reset_pipeline_layout, &rt.reset) ||
		!pipeline_make_pushed(NULL, 0, xgpu_vulkan_rt_mask_words, xgpu_vulkan_rt_mask_count, 48, &rt.mask_layout,
		&rt.mask_pipeline_layout, &rt.mask) ||
		!buffer_make((VkDeviceSize)RT_ENTRIES * 32, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
		VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 0, &rt.info) ||
		!(rt.entries = calloc(RT_ENTRIES, sizeof(*rt.entries))) ||
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
			VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1,
			&slot->instances) ||
			!buffer_make((VkDeviceSize)RT_ENTRIES * 32, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1, &slot->readback) ||
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

static void entry_rows_learn(struct rt_slot *slot);
static void pool_collect(int all);
static void entries_age(void);
static void entries_clear(void);

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
	entry_rows_learn(slot);
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
	pool_collect(0);
	rt.object_frame++;
	entries_age();
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

int xgpu_vulkan_rt_stock_models(void)
{
	return (rt.frame.flags & XGPU_RT_FLAG_STOCK_MODELS) != 0;
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
		/* (the map's objects with it) */
		entries_clear();
		break;
	}
}

/* ---------- the objects' entries */

/* a buffer's device address (the last few asked for kept) */
static VkDeviceAddress buffer_address(VkBuffer buffer)
{
	VkBufferDeviceAddressInfo info;
	uint32_t index;

	if (!buffer)
		return 0;
	for (index = 0; index < 16; index++)
		if (rt.addresses[index].buffer == buffer)
			return rt.addresses[index].address;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
	info.buffer = buffer;
	index = rt.address_next++ % 16;
	rt.addresses[index].buffer = buffer;
	rt.addresses[index].address = vkGetBufferDeviceAddress(rt.device.device, &info);
	return rt.addresses[index].address;
}

/* bytes of the pool (256-aligned): a chunk's free range, first fit, or a
new chunk (one of its own for a range past RT_POOL_CHUNK); 0 if none */
static int pool_take(VkDeviceSize size, uint32_t *chunk_index, VkDeviceSize *offset)
{
	uint32_t index, range;

	size = align_up(size, 256);
	for (index = 0; index < RT_POOL_CHUNKS; index++)
	{
		struct rt_pool_chunk *chunk = rt.chunks[index];

		if (!chunk)
			continue;
		for (range = 0; range < chunk->free_count; range++)
		{
			if (chunk->free[range].size < size)
				continue;
			*chunk_index = index;
			*offset = chunk->free[range].offset;
			chunk->free[range].offset += size;
			chunk->free[range].size -= size;
			if (!chunk->free[range].size)
				chunk->free[range] = chunk->free[--chunk->free_count];
			return 1;
		}
	}
	for (index = 0; index < RT_POOL_CHUNKS && rt.chunks[index]; index++)
	{
	}
	if (index == RT_POOL_CHUNKS || rt.pool_bytes >= RT_POOL_MOST)
		return 0;
	{
		struct rt_pool_chunk *chunk = calloc(1, sizeof(*chunk));
		VkDeviceSize bytes = size > RT_POOL_CHUNK ? size : RT_POOL_CHUNK;

		if (!chunk || !buffer_make(bytes, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
			VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0,
			&chunk->buffer))
		{
			free(chunk);
			return 0;
		}
		rt.chunks[index] = chunk;
		rt.pool_bytes += bytes;
		chunk->free[0].offset = size;
		chunk->free[0].size = bytes - size;
		chunk->free_count = chunk->free[0].size ? 1 : 0;
		*chunk_index = index;
		*offset = 0;
		return 1;
	}
}

/* a range given back to its chunk (merged with its neighbours) */
static void pool_give(uint32_t chunk_index, VkDeviceSize offset, VkDeviceSize size)
{
	struct rt_pool_chunk *chunk = chunk_index < RT_POOL_CHUNKS ? rt.chunks[chunk_index] : NULL;
	uint32_t range;

	if (!chunk || !size)
		return;
	size = align_up(size, 256);
	for (range = 0; range < chunk->free_count; range++)
	{
		if (chunk->free[range].offset + chunk->free[range].size == offset)
		{
			chunk->free[range].size += size;
			offset = chunk->free[range].offset;
			size = chunk->free[range].size;
			chunk->free[range] = chunk->free[--chunk->free_count];
			range = (uint32_t)-1;
			continue;
		}
		if (offset + size == chunk->free[range].offset)
		{
			size += chunk->free[range].size;
			chunk->free[range] = chunk->free[--chunk->free_count];
			range = (uint32_t)-1;
			continue;
		}
	}
	if (chunk->free_count < RT_POOL_RANGES)
	{
		chunk->free[chunk->free_count].offset = offset;
		chunk->free[chunk->free_count].size = size;
		chunk->free_count++;
	}
}

/* the entries' ranges and structures the GPU has passed, let go of (all:
the device idle) */
static void pool_collect(int all)
{
	uint64_t done = xgpu_vulkan_completed();
	uint32_t index = 0;

	while (index < rt.pool_garbage_count)
	{
		struct rt_pool_garbage *item = &rt.pool_garbage[index];

		if (all || item->serial <= done)
		{
			if (item->structure)
				vkDestroyAccelerationStructureKHR(rt.device.device, item->structure, NULL);
			pool_give(item->chunk, item->offset, item->size);
			*item = rt.pool_garbage[--rt.pool_garbage_count];
			continue;
		}
		index++;
	}
	/* (a chunk all free let go of: the pool shrinks as the entries go) */
	for (index = 0; index < RT_POOL_CHUNKS; index++)
	{
		struct rt_pool_chunk *chunk = rt.chunks[index];
		uint32_t pending;

		if (!chunk || chunk->free_count != 1 || chunk->free[0].offset != 0 ||
			chunk->free[0].size != chunk->buffer.size)
			continue;
		for (pending = 0; pending < rt.pool_garbage_count && rt.pool_garbage[pending].chunk != index; pending++)
		{
		}
		if (pending < rt.pool_garbage_count)
			continue;
		rt.pool_bytes -= chunk->buffer.size;
		garbage_add(&chunk->buffer, VK_NULL_HANDLE);
		free(chunk);
		rt.chunks[index] = NULL;
	}
}

static uint64_t key_hash(const uint64_t key[8])
{
	uint64_t hash = 1469598103934665603ull;
	int word;

	for (word = 0; word < 8; word++)
		hash = (hash ^ key[word]) * 1099511628211ull;
	return hash ^ (hash >> 29);
}

static void entry_unlink(uint32_t index)
{
	struct rt_entry *entry = &rt.entries[index];
	uint32_t *link = &rt.buckets[key_hash(entry->key) % RT_ENTRY_BUCKETS];

	while (*link && *link != index + 1)
		link = &rt.entries[*link - 1].next;
	if (*link)
		*link = entry->next;
}

/* an entry let go of (its range and structure once the work recorded so
far is done) */
static void entry_free(uint32_t index)
{
	struct rt_entry *entry = &rt.entries[index];

	if (!entry->used)
		return;
	entry_unlink(index);
	if (rt.pool_garbage_count == RT_POOL_GARBAGE)
	{
		xgpu_vulkan_wait(rt.pool_garbage[0].serial);
		pool_collect(0);
	}
	if (rt.pool_garbage_count < RT_POOL_GARBAGE)
	{
		struct rt_pool_garbage *item = &rt.pool_garbage[rt.pool_garbage_count++];

		item->serial = xgpu_vulkan_serial();
		item->chunk = entry->chunk;
		item->offset = entry->offset;
		item->size = entry->size;
		item->structure = entry->structure;
	}
	memset(entry, 0, sizeof(*entry));
	rt.entry_count--;
}

/* the entries not traced for RT_ENTRY_IDLE_FRAMES let go of, a few a frame */
static void entries_age(void)
{
	uint32_t look;

	if (!rt.entries)
		return;
	for (look = 0; look < 64; look++)
	{
		uint32_t index = rt.entry_cursor++ % RT_ENTRIES;
		struct rt_entry *entry = &rt.entries[index];

		if (entry->used && entry->frame + RT_ENTRY_IDLE_FRAMES < rt.object_frame)
		{
			entry_free(index);
			rt.evicted++;
		}
	}
	/* (the pool at its most and an entry refused room: every entry not
	traced in the last frames) */
	if (rt.pool_full)
	{
		rt.pool_full = 0;
		uint32_t index;

		for (index = 0; index < RT_ENTRIES; index++)
		{
			struct rt_entry *entry = &rt.entries[index];

			if (entry->used && entry->frame + 2 < rt.object_frame)
			{
				entry_free(index);
				rt.evicted++;
			}
		}
	}
}

/* every entry let go of (the map's structure released: its objects with it) */
static void entries_clear(void)
{
	uint32_t index;

	if (!rt.entries)
		return;
	for (index = 0; index < RT_ENTRIES; index++)
		entry_free(index);
	memset(rt.buckets, 0, sizeof(rt.buckets));
}

/* the rows each entry's vertices name, learnt from a frame's info once it
is done (the slot taken again): until then every node row is compared */
static void entry_rows_learn(struct rt_slot *slot)
{
	const uint32_t *info = slot->readback.mapped;
	uint32_t index;

	if (!slot->read_back || !info)
		return;
	slot->read_back = 0;
	for (index = 0; index < RT_ENTRIES; index++)
	{
		struct rt_entry *entry = &rt.entries[index];
		uint32_t most;

		if (!entry->used || entry->rows_known || !entry->skinned || entry->skinned_serial > slot->serial)
			continue;
		most = info[index * 8 + 6];
		if (most)
			entry->rows_known = most - 1 + 3 < RT_NODE_ROWS ? most - 1 + 3 : RT_NODE_ROWS;
	}
}

/* the entry of a key, made (its range taken, its structure made, its build
sizes found) if it has none; NULL if it cannot be */
static struct rt_entry *entry_get(const uint64_t key[8], uint32_t triangles, uint32_t *made_index)
{
	uint64_t hash = key_hash(key);
	uint32_t index = rt.buckets[hash % RT_ENTRY_BUCKETS], look;
	VkAccelerationStructureGeometryKHR geometry;
	VkAccelerationStructureBuildGeometryInfoKHR info;
	VkAccelerationStructureBuildSizesInfoKHR sizes;
	VkAccelerationStructureCreateInfoKHR create;
	VkAccelerationStructureDeviceAddressInfoKHR address;
	struct rt_entry *entry;

	while (index)
	{
		entry = &rt.entries[index - 1];
		if (!memcmp(entry->key, key, sizeof(entry->key)))
		{
			*made_index = index - 1;
			return entry;
		}
		index = entry->next;
	}
	/* (a free entry; none: the one idle longest let go of) */
	if (rt.entry_count == RT_ENTRIES)
	{
		uint32_t oldest = 0;

		for (look = 1; look < RT_ENTRIES; look++)
			if (rt.entries[look].frame < rt.entries[oldest].frame)
				oldest = look;
		if (rt.entries[oldest].frame >= rt.object_frame)
			return NULL;
		entry_free(oldest);
		rt.evicted++;
	}
	for (look = 0; look < RT_ENTRIES && rt.entries[look].used; look++)
	{
	}
	if (look == RT_ENTRIES)
		return NULL;
	entry = &rt.entries[look];
	memset(entry, 0, sizeof(*entry));
	memcpy(entry->key, key, sizeof(entry->key));
	entry->triangles = triangles;
	memset(&geometry, 0, sizeof(geometry));
	geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
	geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
	geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
	geometry.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
	geometry.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
	geometry.geometry.triangles.vertexStride = 12;
	geometry.geometry.triangles.maxVertex = triangles * 3 - 1;
	geometry.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
	info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
		VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
	info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	info.geometryCount = 1;
	info.pGeometries = &geometry;
	memset(&sizes, 0, sizeof(sizes));
	sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
	vkGetAccelerationStructureBuildSizesKHR(rt.device.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info,
		&triangles, &sizes);
	entry->structure_offset = align_up((VkDeviceSize)entry->triangles * 36, 256);
	entry->structure_size = sizes.accelerationStructureSize;
	entry->build_scratch = sizes.buildScratchSize;
	entry->update_scratch = sizes.updateScratchSize;
	entry->size = entry->structure_offset + align_up(entry->structure_size, 256);
	if (!pool_take(entry->size, &entry->chunk, &entry->offset))
	{
		rt.pool_full = 1;
		memset(entry, 0, sizeof(*entry));
		return NULL;
	}
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
	create.buffer = rt.chunks[entry->chunk]->buffer.buffer;
	create.offset = entry->offset + entry->structure_offset;
	create.size = entry->structure_size;
	create.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	if (vkCreateAccelerationStructureKHR(rt.device.device, &create, NULL, &entry->structure) != VK_SUCCESS)
	{
		pool_give(entry->chunk, entry->offset, entry->size);
		memset(entry, 0, sizeof(*entry));
		return NULL;
	}
	memset(&address, 0, sizeof(address));
	address.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
	address.accelerationStructure = entry->structure;
	entry->structure_address = vkGetAccelerationStructureDeviceAddressKHR(rt.device.device, &address);
	entry->soup_address = rt.chunks[entry->chunk]->buffer.address + entry->offset;
	entry->used = 1;
	entry->job = -1;
	entry->next = rt.buckets[hash % RT_ENTRY_BUCKETS];
	rt.buckets[hash % RT_ENTRY_BUCKETS] = look + 1;
	rt.entry_count++;
	*made_index = look;
	return entry;
}

/* ---------- the objects' capture */

/* a window's places in the slot's geometry from its jobs': the workgroups'
table, the instances' table (each object instance's soup), the entries the
mask pass reads; returns the end */
static VkDeviceSize capture_offsets(VkDeviceSize jobs, VkDeviceSize *groups, VkDeviceSize *table, VkDeviceSize *map)
{
	*groups = jobs + (VkDeviceSize)RT_JOBS * RT_JOB_WORDS * 4;
	*table = align_up(*groups + ((VkDeviceSize)RT_OBJECT_TRIANGLES / 64 + RT_JOBS) * 8, 256);
	*map = align_up(*table + (VkDeviceSize)RT_WINDOW_DRAWS * 8, 256);
	return *map + (VkDeviceSize)RT_WINDOW_DRAWS * 4;
}

void xgpu_vulkan_rt_window_begin(void)
{
	struct rt_slot *slot = &rt.slots[rt.slot];
	struct rt_capture *capture = &rt.capture;
	VkDeviceSize groups, table, map;

	capture->open = capture->full = 0;
	capture->draw_count = capture->triangle_count = 0;
	capture->job_count = capture->job_groups = capture->job_triangles = 0;
	capture->jobs = NULL;
	if (!xgpu_vulkan_rt_active() || !rt.entries)
		return;
	/* (the slot's geometry left after the windows before: the jobs, then
	the workgroups' table, then the instances' table and the entries the
	mask pass reads) */
	capture->jobs_offset = align_up(slot->geometry_used, 256);
	if (capture_offsets(capture->jobs_offset, &groups, &table, &map) > slot->geometry.size)
		return;
	capture->jobs = (uint32_t *)((uint8_t *)slot->geometry.mapped + capture->jobs_offset);
	if (++capture->stamp == 0)
	{
		memset(capture->occurrences, 0, sizeof(capture->occurrences));
		capture->stamp = 1;
	}
	capture->serial = xgpu_vulkan_serial();
	capture->open = 1;
}

void xgpu_vulkan_rt_skipped(void)
{
	if (rt.capture.open)
		rt.skipped++;
}

/* the draw's occurrence in the window so far (another object of the same
model, drawn from the same vertices) */
static uint32_t occurrence(uint64_t hash)
{
	struct rt_capture *capture = &rt.capture;
	uint32_t size = sizeof(capture->occurrences) / sizeof(capture->occurrences[0]);
	uint32_t at = (uint32_t)(hash % size), look;

	for (look = 0; look < size; look++, at = (at + 1) % size)
	{
		if (capture->occurrences[at].stamp != capture->stamp)
		{
			capture->occurrences[at].stamp = capture->stamp;
			capture->occurrences[at].hash = hash;
			capture->occurrences[at].count = 1;
			return 0;
		}
		if (capture->occurrences[at].hash == hash)
			return capture->occurrences[at].count++;
	}
	return 0;
}

void xgpu_vulkan_rt_capture(const struct xgpu_vulkan_rt_draw *draw)
{
	struct rt_capture *capture = &rt.capture;
	uint64_t key[8];
	uint32_t triangles, index, rows;
	VkDeviceAddress vertices, indices = 0, constants = 0;
	struct rt_entry *entry;
	int changed, cache = !(rt.frame.flags & XGPU_RT_FLAG_NO_CACHE);

	if (!capture->open || capture->full || !draw->vertex_buffer || !draw->count || !draw->stride ||
		draw->serial != capture->serial || (!draw->index_buffer && draw->cpu_indices && draw->index_size != 2))
		return;
	triangles = draw->primitive == XGPU_VULKAN_RT_LIST ? draw->count / 3 : draw->count >= 3 ? draw->count - 2 : 0;
	if (!triangles)
		return;
	/* (the position read as words: the vertices' place and layout 4-aligned) */
	if ((draw->stride & 3) || (draw->position_offset & 3) || (draw->vertex_offset & 3) ||
		((draw->index_buffer || draw->cpu_indices) && draw->index_size != 2 && draw->index_size != 4))
		return;
	if (capture->draw_count == RT_WINDOW_DRAWS || capture->job_count == RT_JOBS ||
		capture->triangle_count + triangles > RT_OBJECT_TRIANGLES)
	{
		capture->full = 1;
		rt.full_frames++;
		return;
	}
	vertices = buffer_address(draw->vertex_buffer);
	if (draw->index_buffer)
		indices = buffer_address(draw->index_buffer);
	if (draw->constants_buffer)
		constants = buffer_address(draw->constants_buffer);
	if (!vertices || (draw->index_buffer && !indices) || (draw->constants_buffer && !constants))
		return;
	/* the entry: the draw's identity, layout and counts, and its occurrence */
	key[0] = draw->identity[0];
	key[1] = draw->identity[1];
	key[2] = draw->identity[2];
	key[3] = draw->identity[3];
	key[4] = ((uint64_t)draw->count << 32) | ((uint64_t)draw->primitive << 24) | ((uint64_t)draw->index_size << 16) |
		draw->stride;
	key[5] = ((uint64_t)(uint32_t)draw->position_offset << 40) ^ ((uint64_t)(uint32_t)draw->nodes_offset << 20) ^
		(uint64_t)(uint32_t)draw->weight_offset ^ ((uint64_t)(draw->constants_buffer != VK_NULL_HANDLE) << 63);
	key[6] = (uint64_t)(uint32_t)draw->index_add | ((uint64_t)draw->vertex_count << 32);
	key[7] = 0;
	key[7] = occurrence(key_hash(key));
	entry = entry_get(key, triangles, &index);
	if (!entry)
	{
		rt.stale++;
		return;
	}
	entry->occluder = draw->occluder;
	/* whether it moved: its bytes, its pose, its node rows (those its
	vertices name, once known) */
	rows = entry->rows_known ? entry->rows_known : RT_NODE_ROWS;
	changed = !cache || !entry->skinned || draw->volatile_source || entry->generation[0] != draw->generation[0] ||
		entry->generation[1] != draw->generation[1] || entry->pose != draw->pose ||
		(draw->constants && memcmp(entry->rows, draw->constants[60], rows * sizeof(entry->rows[0])));
	entry->frame = rt.object_frame;
	entry->mode = 0;
	entry->job = -1;
	if (changed)
	{
		uint32_t *job = capture->jobs + (size_t)capture->job_count * RT_JOB_WORDS;
		VkDeviceAddress vertex0 = vertices + draw->vertex_offset;
		uint64_t index_first = draw->index_first, index0;
		float dx = draw->origin[0] - entry->origin[0], dy = draw->origin[1] - entry->origin[1],
			dz = draw->origin[2] - entry->origin[2];

		/* (the CPU's indices, copied into the ring now: only where skinned) */
		if (!draw->index_buffer && draw->cpu_indices)
		{
			struct xgpu_vulkan_range range;
			void *to = xgpu_vulkan_upload(draw->count * 2, 16, &range);

			if (!to || xgpu_vulkan_serial() != capture->serial || !(indices = buffer_address(range.buffer)))
			{
				entry->skinned = 0;
				return;
			}
			memcpy(to, draw->cpu_indices, (size_t)draw->count * 2);
			index_first = range.offset / 2;
		}
		index0 = indices + index_first * draw->index_size;
		job[0] = (uint32_t)vertex0;
		job[1] = (uint32_t)(vertex0 >> 32);
		job[2] = (uint32_t)(index0 & ~(uint64_t)3);
		job[3] = (uint32_t)(index0 >> 32);
		job[4] = (uint32_t)(constants + draw->constants_offset);
		job[5] = constants ? (uint32_t)((constants + draw->constants_offset) >> 32) : 0;
		if (!constants)
			job[4] = 0;
		job[6] = (uint32_t)entry->soup_address;
		job[7] = (uint32_t)(entry->soup_address >> 32);
		job[8] = draw->stride;
		job[9] = (uint32_t)draw->position_offset;
		job[10] = draw->nodes_offset >= 0 ? (uint32_t)draw->nodes_offset : 0xffffffffu;
		job[11] = draw->weight_offset >= 0 ? (uint32_t)draw->weight_offset : 0xffffffffu;
		job[12] = indices ? draw->index_size : 0;
		job[13] = draw->primitive == XGPU_VULKAN_RT_LIST ? 0 : draw->primitive == XGPU_VULKAN_RT_STRIP ? 1 : 2;
		job[14] = triangles;
		job[15] = indices ? (uint32_t)((index0 & 3) / draw->index_size) : 0;
		job[16] = (uint32_t)draw->index_add;
		job[17] = draw->vertex_count;
		job[18] = index;
		job[19] = 0;
		entry->job = (int)capture->job_count++;
		capture->job_groups += (triangles + 63) / 64;
		capture->job_triangles += triangles;
		/* (refitted where it was built, did not jump far and has not been
		refitted too often; else built again) */
		entry->mode = entry->built && entry->updates < RT_REBUILD_UPDATES &&
			dx * dx + dy * dy + dz * dz < RT_REFIT_DISTANCE * RT_REFIT_DISTANCE && !draw->volatile_source ? 2 : 1;
		entry->generation[0] = draw->generation[0];
		entry->generation[1] = draw->generation[1];
		entry->pose = draw->pose;
		memcpy(entry->origin, draw->origin, sizeof(entry->origin));
		if (draw->constants)
			memcpy(entry->rows, draw->constants[60], sizeof(entry->rows));
		entry->skinned = 1;
		entry->skinned_serial = capture->serial;
	}
	capture->draws[capture->draw_count++] = index | (draw->occluder ? 0x80000000u : 0);
	if (!draw->constants_buffer)
		rt.pieces++;
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

/* the window's top-level structure: of the count instances objects_record
wrote from the slot's first unused (identity: all are in the world) */
static VkAccelerationStructureKHR scene_build(VkCommandBuffer commands, struct rt_slot *slot, uint32_t first,
	uint32_t count)
{
	VkAccelerationStructureGeometryKHR geometry;
	VkAccelerationStructureBuildGeometryInfoKHR info;
	VkAccelerationStructureBuildSizesInfoKHR sizes;
	VkAccelerationStructureBuildRangeInfoKHR range;
	const VkAccelerationStructureBuildRangeInfoKHR *ranges = &range;
	VkDeviceAddress address, scratch;

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
	/* (the sizes of the most instances a slot has, found once: the driver's
	answer took a share of the render thread's frame) */
	if (!rt.tlas_sizes.accelerationStructureSize)
	{
		uint32_t most = RT_INSTANCES;

		memset(&rt.tlas_sizes, 0, sizeof(rt.tlas_sizes));
		rt.tlas_sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
		vkGetAccelerationStructureBuildSizesKHR(rt.device.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info,
			&most, &rt.tlas_sizes);
	}
	sizes = rt.tlas_sizes;
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
			"%llu frames' capture full; objects: %.1f skinned a frame (%.0f triangles), %.1f built, %.1f refitted, "
			"%.1f kept, %.2f deferred, %llu evicted, %llu without an entry, %.1f a frame replacement meshes' pieces; "
			"%u entries, %.1f MB",
			rt.timed_frames ? rt.milliseconds / (double)rt.timed_frames : 0.0,
			rt.milliseconds_most, (unsigned long long)rt.timed_frames, rt.static_triangles,
			(double)rt.triangles / 600.0, (unsigned long long)rt.object_triangles_most, (double)rt.draws / 600.0,
			(double)rt.skipped / 600.0, (unsigned long long)rt.full_frames, (double)rt.jobs / 600.0,
			(double)rt.skinned_triangles / 600.0, (double)rt.builds / 600.0, (double)rt.updates / 600.0,
			(double)rt.reused / 600.0, (double)rt.deferred / 600.0, (unsigned long long)rt.evicted,
			(unsigned long long)rt.stale, (double)rt.pieces / 600.0, rt.entry_count, (double)rt.pool_bytes / 1048576.0);
	if (getenv("HALO_PRISM_RT_DEBUG"))
	{
		uint32_t index;

		for (index = 0; index < RT_ENTRIES; index++)
		{
			const struct rt_entry *entry = &rt.entries[index];

			const uint32_t *info = rt.slots[0].readback.mapped;

			if (entry->used && info)
				host_logf(HOST_LOG_INFO, "prism rt: entry %u info: %08x %08x %08x %08x %08x %08x most %u", index,
					info[index * 8], info[index * 8 + 1], info[index * 8 + 2], info[index * 8 + 3], info[index * 8 + 4],
					info[index * 8 + 5], info[index * 8 + 6]);
			if (entry->used)
				host_logf(HOST_LOG_INFO, "prism rt: entry %u: %u triangles, %llu bytes (structure %llu), key %016llx %016llx "
					"%016llx %016llx %016llx %016llx %016llx %llu, built %d, updates %u, rows %u, frame %llu, chunk %u", index,
					entry->triangles, (unsigned long long)entry->size, (unsigned long long)entry->structure_size,
					(unsigned long long)entry->key[0], (unsigned long long)entry->key[1], (unsigned long long)entry->key[2],
					(unsigned long long)entry->key[3], (unsigned long long)entry->key[4], (unsigned long long)entry->key[5],
					(unsigned long long)entry->key[6], (unsigned long long)entry->key[7], entry->built, entry->updates,
					entry->rows_known, (unsigned long long)entry->frame, entry->chunk);
		}
	}
	rt.frames = rt.draws = rt.skipped = rt.triangles = rt.object_triangles_most = rt.full_frames = 0;
	rt.jobs = rt.skinned_triangles = rt.builds = rt.updates = rt.reused = rt.evicted = rt.stale = rt.deferred = 0;
	rt.pieces = 0;
	rt.milliseconds = rt.milliseconds_most = 0.0;
	rt.timed_frames = 0;
}

/* "pathtraced": a storage descriptor's or a sampled one's write, and a
uniform buffer's or storage buffer's (the sets' writes below) */
static void write_buffer(VkWriteDescriptorSet *write, VkDescriptorSet set, uint32_t binding, VkDescriptorType type,
	const VkDescriptorBufferInfo *info)
{
	memset(write, 0, sizeof(*write));
	write->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write->dstSet = set;
	write->dstBinding = binding;
	write->descriptorCount = 1;
	write->descriptorType = type;
	write->pBufferInfo = info;
}

static void write_scene(VkWriteDescriptorSet *write, VkWriteDescriptorSetAccelerationStructureKHR *scene_write,
	VkDescriptorSet set, uint32_t binding, const VkAccelerationStructureKHR *scene)
{
	memset(scene_write, 0, sizeof(*scene_write));
	scene_write->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
	scene_write->accelerationStructureCount = 1;
	scene_write->pAccelerationStructures = scene;
	memset(write, 0, sizeof(*write));
	write->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write->pNext = scene_write;
	write->dstSet = set;
	write->dstBinding = binding;
	write->descriptorCount = 1;
	write->descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
}

static void compute_barrier(VkCommandBuffer commands)
{
	barrier_global(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
}

/* the window's objects recorded (the scene's top level built after): the
jobs' entries' info emptied, their triangles skinned, their structures
built or refitted (one call), the instances written (the structure's first,
where it has one, then each captured draw's whose entry has a structure:
custom index its place in the table, the table its soup's address), the
occluders near the eye masked out (on the GPU), the entries' info copied
for the CPU (entry_rows_learn); the instances' count, the table's address */
static uint32_t objects_record(VkCommandBuffer commands, struct rt_slot *slot, const float eye[3],
	VkDeviceAddress *table_address)
{
	static VkAccelerationStructureBuildGeometryInfoKHR infos[RT_JOBS];
	static VkAccelerationStructureGeometryKHR geometries[RT_JOBS];
	static VkAccelerationStructureBuildRangeInfoKHR ranges[RT_JOBS];
	static const VkAccelerationStructureBuildRangeInfoKHR *range_pointers[RT_JOBS];
	struct rt_capture *capture = &rt.capture;
	VkAccelerationStructureInstanceKHR *instances;
	VkDeviceSize groups_offset, table_offset, map_offset;
	uint32_t *groups, *map, first = slot->instances_used, count = 0, objects = 0, built = 0, job, draw;
	uint64_t *table;
	struct
	{
		VkDeviceAddress jobs, groups, info;
		uint32_t count, pad;
	} push;
	struct
	{
		VkDeviceAddress instances, entries, info;
		uint32_t count, pad;
		float eye[4];
	} mask;

	*table_address = 0;
	if (!capture->jobs || first + 1 > RT_INSTANCES)
		return 0;
	/* (a submission begun since the draws were captured: their ranges are
	another's; skinned again next frame) */
	if (xgpu_vulkan_serial() != capture->serial)
	{
		for (job = 0; job < capture->job_count; job++)
		{
			struct rt_entry *entry = &rt.entries[capture->jobs[job * RT_JOB_WORDS + 18]];

			entry->skinned = 0;
			entry->mode = 0;
		}
		capture->job_count = capture->job_groups = 0;
	}
	slot->geometry_used = capture_offsets(capture->jobs_offset, &groups_offset, &table_offset, &map_offset);
	groups = (uint32_t *)((uint8_t *)slot->geometry.mapped + groups_offset);
	table = (uint64_t *)((uint8_t *)slot->geometry.mapped + table_offset);
	map = (uint32_t *)((uint8_t *)slot->geometry.mapped + map_offset);
	/* the workgroups' table: each a job's and its place in it */
	{
		uint32_t at = 0, group;

		for (job = 0; job < capture->job_count; job++)
		{
			uint32_t triangles = capture->jobs[job * RT_JOB_WORDS + 14];

			for (group = 0; group < (triangles + 63) / 64; group++)
			{
				groups[at * 2] = job;
				groups[at * 2 + 1] = group;
				at++;
			}
		}
	}
	/* the builds and refits: each with its scratch (none left: it waits for
	the next frame, its last structure traced meanwhile) */
	for (job = 0; job < capture->job_count; job++)
	{
		uint32_t index = capture->jobs[job * RT_JOB_WORDS + 18];
		struct rt_entry *entry = &rt.entries[index];
		VkDeviceAddress scratch;

		if (entry->mode == 0)
			continue;
		scratch = slot_scratch(slot, entry->mode == 1 ? entry->build_scratch : entry->update_scratch);
		if (!scratch)
		{
			entry->skinned = 0;
			entry->mode = 0;
			rt.deferred++;
			continue;
		}
		memset(&geometries[built], 0, sizeof(geometries[built]));
		geometries[built].sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
		geometries[built].geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
		geometries[built].flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
		geometries[built].geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
		geometries[built].geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
		geometries[built].geometry.triangles.vertexData.deviceAddress = entry->soup_address;
		geometries[built].geometry.triangles.vertexStride = 12;
		geometries[built].geometry.triangles.maxVertex = entry->triangles * 3 - 1;
		geometries[built].geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
		memset(&infos[built], 0, sizeof(infos[built]));
		infos[built].sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
		infos[built].type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
		infos[built].flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
			VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
		infos[built].mode = entry->mode == 1 ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR :
			VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
		infos[built].srcAccelerationStructure = entry->mode == 2 ? entry->structure : VK_NULL_HANDLE;
		infos[built].dstAccelerationStructure = entry->structure;
		infos[built].geometryCount = 1;
		infos[built].pGeometries = &geometries[built];
		infos[built].scratchData.deviceAddress = scratch;
		memset(&ranges[built], 0, sizeof(ranges[built]));
		ranges[built].primitiveCount = entry->triangles;
		range_pointers[built] = &ranges[built];
		if (entry->mode == 1)
		{
			entry->built = 1;
			entry->updates = 0;
			rt.builds++;
		}
		else
		{
			entry->updates++;
			rt.updates++;
		}
		built++;
	}
	/* the instances: the structure's, then the objects' with a structure */
	instances = (VkAccelerationStructureInstanceKHR *)slot->instances.mapped + first;
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
	for (draw = 0; draw < capture->draw_count && first + count < RT_INSTANCES; draw++)
	{
		uint32_t index = capture->draws[draw] & 0x7fffffffu;
		struct rt_entry *entry = &rt.entries[index];
		VkAccelerationStructureInstanceKHR *instance = &instances[count];

		if (!entry->used || !entry->built)
			continue;
		if (entry->mode == 0 && entry->job < 0)
			rt.reused++;
		memset(instance, 0, sizeof(*instance));
		instance->transform.matrix[0][0] = instance->transform.matrix[1][1] = instance->transform.matrix[2][2] = 1.0f;
		instance->mask = 0x04;
		instance->instanceCustomIndex = objects + 1;
		instance->flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR |
			VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;
		instance->accelerationStructureReference = entry->structure_address;
		table[objects] = entry->soup_address;
		map[objects] = capture->draws[draw];
		objects++;
		count++;
	}
	slot->instances_used += count;
	*table_address = slot->geometry.address + table_offset;
	/* (the entries' info zeroed once) */
	if (!rt.info_zeroed)
	{
		vkCmdFillBuffer(commands, rt.info.buffer, 0, VK_WHOLE_SIZE, 0);
		rt.info_zeroed = 1;
	}
	/* the last frames' traces and builds done with what this one writes; the
	early commands' skinnings (the mesh store's) and copies seen */
	barrier_global(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
		VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
		VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
		VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
	if (capture->job_count)
	{
		push.jobs = slot->geometry.address + capture->jobs_offset;
		push.groups = slot->geometry.address + groups_offset;
		push.info = rt.info.address;
		push.count = capture->job_count;
		push.pad = 0;
		vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, rt.reset);
		vkCmdPushConstants(commands, rt.reset_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
		vkCmdDispatch(commands, (capture->job_count + 63) / 64, 1, 1);
		compute_barrier(commands);
		vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, rt.skin);
		vkCmdPushConstants(commands, rt.skin_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
		vkCmdDispatch(commands, capture->job_groups, 1, 1);
		barrier_global(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
			VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
			VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT);
		rt.jobs += capture->job_count;
		rt.skinned_triangles += capture->job_triangles;
	}
	if (built)
	{
		vkCmdBuildAccelerationStructuresKHR(commands, built, infos, range_pointers);
		barrier_global(commands, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
			VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
	}
	if (objects)
	{
		mask.instances = slot->instances.address + (VkDeviceSize)(first + (rt.static_structure ? 1 : 0)) *
			sizeof(VkAccelerationStructureInstanceKHR);
		mask.entries = slot->geometry.address + map_offset;
		mask.info = rt.info.address;
		mask.count = objects;
		mask.pad = 0;
		mask.eye[0] = eye[0];
		mask.eye[1] = eye[1];
		mask.eye[2] = eye[2];
		mask.eye[3] = 0.0f;
		vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, rt.mask);
		vkCmdPushConstants(commands, rt.mask_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(mask), &mask);
		vkCmdDispatch(commands, (objects + 63) / 64, 1, 1);
		barrier_global(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
			VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_SHADER_READ_BIT);
	}
	/* (the info for the CPU: the nodes the entries' vertices name) */
	if (capture->job_count && !slot->read_back)
	{
		VkBufferCopy copy;

		memset(&copy, 0, sizeof(copy));
		copy.size = rt.info.size;
		vkCmdCopyBuffer(commands, rt.info.buffer, slot->readback.buffer, 1, &copy);
		barrier_global(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
			VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
		slot->read_back = 1;
	}
	return count;
}

/* the path tracer's images made (or kept) at the window's size; 0 if they
cannot be */
static int pathtraced_images(uint32_t width, uint32_t height)
{
	int index, pair;

	if (width != rt.pt_width || height != rt.pt_height)
		rt.pt_history_valid = 0;
	for (index = 0; index < 2; index++)
	{
		rt.pt_raw[index] = image_storage(rt.pt_raw[index], XGPU_VULKAN_FORMAT_RGBA16F, width, height);
		rt.pt_temporary[index] = image_storage(rt.pt_temporary[index], XGPU_VULKAN_FORMAT_RGBA16F, width, height);
		rt.pt_reservoirs[index] = image_storage(rt.pt_reservoirs[index], XGPU_VULKAN_FORMAT_RGBA16F, width, height);
		for (pair = 0; pair < 2; pair++)
			rt.pt_state[index][pair] = image_storage(rt.pt_state[index][pair], XGPU_VULKAN_FORMAT_RGBA16F, width,
				height);
		if (!rt.pt_raw[index] || !rt.pt_temporary[index] || !rt.pt_reservoirs[index] || !rt.pt_state[index][0] ||
			!rt.pt_state[index][1])
			return 0;
	}
	rt.result = image_storage(rt.result, XGPU_VULKAN_FORMAT_RGBA32F, width, height);
	if (!rt.result)
		return 0;
	rt.pt_width = width;
	rt.pt_height = height;
	return 1;
}

/* "pathtraced"'s passes recorded (the scene built): gi's probes, the
paths, the temporal pass, the three spatial ones, the last into the image
the surfaces read; 0 if the sets cannot be had */
static int pathtraced_record(VkCommandBuffer commands, struct rt_slot *slot, VkAccelerationStructureKHR scene,
	struct xgpu_vulkan_image *inputs, const struct xgpu_vulkan_range *uniforms, uint32_t uniform_bytes, int gi,
	uint32_t probes_updated, uint32_t width, uint32_t height)
{
	VkDescriptorSetLayout layouts[6];
	VkDescriptorSet sets[6];
	VkDescriptorSetAllocateInfo allocate;
	VkWriteDescriptorSet writes[40];
	VkDescriptorImageInfo images[32];
	VkWriteDescriptorSetAccelerationStructureKHR scene_writes[2];
	VkDescriptorBufferInfo buffer, probe_buffer;
	uint32_t now = rt.pt_index, last = rt.pt_index ^ 1u, count = 0, image = 0, step;
	struct xgpu_vulkan_image *from[3][2], *to[3][2];
	int32_t push[2];

	layouts[0] = rt.probes_layout;
	layouts[1] = rt.path_layout;
	layouts[2] = rt.accumulate_layout;
	layouts[3] = layouts[4] = layouts[5] = rt.atrous_layout;
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocate.descriptorPool = slot->pool;
	allocate.descriptorSetCount = 6;
	allocate.pSetLayouts = layouts;
	if (vkAllocateDescriptorSets(rt.device.device, &allocate, sets) != VK_SUCCESS)
		return 0;
	memset(&buffer, 0, sizeof(buffer));
	buffer.buffer = uniforms->buffer;
	buffer.offset = uniforms->offset;
	buffer.range = uniform_bytes;
	memset(&probe_buffer, 0, sizeof(probe_buffer));
	probe_buffer.buffer = rt.probe_buffer.buffer;
	probe_buffer.range = VK_WHOLE_SIZE;
#define IMAGE(set, binding, what, sampler, type) \
	write_image(&writes[count++], &images[image++], sets[set], binding, what, sampler, type)
#define SAMPLED VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
#define STORAGE VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
	/* the probes */
	write_scene(&writes[count++], &scene_writes[0], sets[0], 0, &scene);
	write_buffer(&writes[count++], sets[0], 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &probe_buffer);
	write_buffer(&writes[count++], sets[0], 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &buffer);
	/* the paths */
	IMAGE(1, 0, inputs, rt.nearest, SAMPLED);
	write_scene(&writes[count++], &scene_writes[1], sets[1], 1, &scene);
	IMAGE(1, 2, rt.pt_raw[0], VK_NULL_HANDLE, STORAGE);
	write_buffer(&writes[count++], sets[1], 3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &buffer);
	write_buffer(&writes[count++], sets[1], 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &probe_buffer);
	IMAGE(1, 5, rt.pt_raw[1], VK_NULL_HANDLE, STORAGE);
	IMAGE(1, 6, rt.pt_reservoirs[last], rt.nearest, SAMPLED);
	IMAGE(1, 7, rt.pt_reservoirs[now], VK_NULL_HANDLE, STORAGE);
	/* the temporal pass */
	IMAGE(2, 0, inputs, rt.nearest, SAMPLED);
	IMAGE(2, 1, rt.pt_raw[0], rt.nearest, SAMPLED);
	IMAGE(2, 2, rt.pt_raw[1], rt.nearest, SAMPLED);
	write_buffer(&writes[count++], sets[2], 3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &buffer);
	IMAGE(2, 4, rt.pt_state[last][0], rt.nearest, SAMPLED);
	IMAGE(2, 5, rt.pt_state[last][1], rt.nearest, SAMPLED);
	IMAGE(2, 6, rt.pt_state[now][0], VK_NULL_HANDLE, STORAGE);
	IMAGE(2, 7, rt.pt_state[now][1], VK_NULL_HANDLE, STORAGE);
	/* the spatial passes: the state into the other pair, it into the raw
	pair (read by now), it into the surfaces' image */
	from[0][0] = rt.pt_state[now][0]; from[0][1] = rt.pt_state[now][1];
	to[0][0] = rt.pt_temporary[0]; to[0][1] = rt.pt_temporary[1];
	from[1][0] = rt.pt_temporary[0]; from[1][1] = rt.pt_temporary[1];
	to[1][0] = rt.pt_raw[0]; to[1][1] = rt.pt_raw[1];
	from[2][0] = rt.pt_raw[0]; from[2][1] = rt.pt_raw[1];
	to[2][0] = rt.pt_temporary[0]; to[2][1] = rt.pt_temporary[1];
	for (step = 0; step < 3; step++)
	{
		IMAGE(3 + step, 0, inputs, rt.nearest, SAMPLED);
		IMAGE(3 + step, 1, from[step][0], rt.nearest, SAMPLED);
		IMAGE(3 + step, 2, from[step][1], rt.nearest, SAMPLED);
		write_buffer(&writes[count++], sets[3 + step], 3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &buffer);
		IMAGE(3 + step, 4, to[step][0], VK_NULL_HANDLE, STORAGE);
		IMAGE(3 + step, 5, to[step][1], VK_NULL_HANDLE, STORAGE);
		IMAGE(3 + step, 6, rt.result, VK_NULL_HANDLE, STORAGE);
	}
#undef IMAGE
#undef SAMPLED
#undef STORAGE
	vkUpdateDescriptorSets(rt.device.device, count, writes, 0, NULL);
	if (gi && probes_updated)
	{
		vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, rt.probes);
		vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, rt.probes_pipeline_layout, 0, 1, &sets[0], 0,
			NULL);
		vkCmdDispatch(commands, probes_updated, 1, 1);
		compute_barrier(commands);
		rt.probe_frame++;
	}
	vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, rt.path);
	vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, rt.path_pipeline_layout, 0, 1, &sets[1], 0, NULL);
	vkCmdDispatch(commands, (width + 7) / 8, (height + 7) / 8, 1);
	compute_barrier(commands);
	vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, rt.accumulate);
	vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, rt.accumulate_pipeline_layout, 0, 1, &sets[2], 0,
		NULL);
	vkCmdDispatch(commands, (width + 7) / 8, (height + 7) / 8, 1);
	compute_barrier(commands);
	vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, rt.atrous);
	for (step = 0; step < 3; step++)
	{
		push[0] = 1 << step;
		push[1] = step == 2;
		vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, rt.atrous_pipeline_layout, 0, 1,
			&sets[3 + step], 0, NULL);
		vkCmdPushConstants(commands, rt.atrous_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
		vkCmdDispatch(commands, (width + 7) / 8, (height + 7) / 8, 1);
		compute_barrier(commands);
	}
	return 1;
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
	struct xgpu_vulkan_image *history, *state;
	uint32_t scale, out_width, out_height, quality, probes_updated = 0;
	int gi, pathtraced;
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
	/* ("pathtraced": at every pixel, which its surfaces read as their own,
	prism_rt.c's prism_rt_full_resolution; the shadows' passes not drawn) */
	pathtraced = rt.frame.mode == XGPU_RT_PATHTRACED && scale == 1;
	if (pathtraced)
	{
		if (!pathtraced_images(out_width, out_height))
			return NULL;
	}
	else
	{
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
	}
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
	/* "pathtraced": bounces, paths a pixel and ReSTIR's candidates by
	quality (medium, low, high, ultra); the lights; the mood, the dynamic
	lights' shadows' share of their light taken, the hits' albedo (a grey:
	CE's textures are not traced yet), ReSTIR's spatial neighbours; its
	history; its debug views (1 the factor of CE's light, 2 the light added) */
	if (pathtraced)
	{
		static const float bounces[4] = { 1.0f, 1.0f, 2.0f, 3.0f };
		static const float paths[4] = { 1.0f, 1.0f, 2.0f, 4.0f };
		static const float candidates[4] = { 8.0f, 4.0f, 16.0f, 32.0f };
		static const float neighbours[4] = { 2.0f, 1.0f, 3.0f, 4.0f };
		uint32_t light, count = rt.frame.light_count < XGPU_STREAM_RT_LIGHTS ? rt.frame.light_count :
			XGPU_STREAM_RT_LIGHTS;

		block.pt[0] = bounces[quality];
		block.pt[1] = paths[quality];
		block.pt[2] = candidates[quality];
		/* (the lights that reach somewhere: a fixture's light, radius 0, is
		in CE's lightmap, never added again) */
		{
			uint32_t kept = 0;

			for (light = 0; light < count; light++)
			{
				const float *l = rt.frame.lights[light];

				if (!(l[3] > 0.05f) || !(l[7] > 0.0f) || l[4] + l[5] + l[6] <= 0.0f)
					continue;
				memcpy(block.lights[kept * 3], l, 12 * sizeof(float));
				kept++;
			}
			count = kept;
		}
		block.pt[3] = (float)count;
		block.pt2[0] = (float)(rt.frame.mood <= 100 ? rt.frame.mood : 100) / 100.0f;
		block.pt2[1] = 0.6f;
		block.pt2[2] = 0.45f;
		/* (display.prism_rt_rate "half", or quality low: half the pixels a
		frame, a checkerboard: the sign) */
		block.pt2[3] = rt.frame.rate == 1 || quality == 1 ? -neighbours[quality] : neighbours[quality];
		block.info[2] = rt.pt_history_valid && pass->history[0] != 0.0f ? 1u : 0u;
		block.info[3] = rt.frame.view == XGPU_RT_VIEW_VISIBILITY || rt.frame.view == XGPU_RT_VIEW_AO ? 1u :
			rt.frame.view == XGPU_RT_VIEW_GI ? 2u : 0u;
		/* (the sun's rays a pixel: one at medium, the paths' own) */
		block.rays[0] = quality == 3 ? 2.0f : 1.0f;
	}
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
		/* (the objects' instances' table: each one's triangle soup) */
		{
			VkDeviceSize groups, table, map;

			(void)capture_offsets(rt.capture.jobs_offset, &groups, &table, &map);
			block.addresses[2] = (uint32_t)(slot->geometry.address + table);
			block.addresses[3] = (uint32_t)((slot->geometry.address + table) >> 32);
		}
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
	if (pathtraced)
	{
		int index;

		for (index = 0; index < 2; index++)
		{
			xgpu_vulkan_image_compute_use(rt.pt_raw[index], 1);
			xgpu_vulkan_image_compute_use(rt.pt_temporary[index], 1);
			xgpu_vulkan_image_compute_use(rt.pt_reservoirs[index], 1);
			xgpu_vulkan_image_compute_use(rt.pt_state[index][0], 1);
			xgpu_vulkan_image_compute_use(rt.pt_state[index][1], 1);
		}
	}
	else
	{
		xgpu_vulkan_image_compute_use(rt.raw, 1);
		xgpu_vulkan_image_compute_use(history, 0);
		xgpu_vulkan_image_compute_use(state, 1);
	}
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
	/* the objects' structures, then the scene's */
	{
		uint32_t first = slot->instances_used, count;
		VkDeviceAddress table;

		count = objects_record(commands, slot, pass->camera[0], &table);
		scene = scene_build(commands, slot, first, count);
		if (!scene)
			return NULL;
	}
	/* the scene, and the pre-pass's inputs (its pass's end barrier made
	them visible), read by the trace */
	barrier_global(commands, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
		VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_SHADER_READ_BIT);
	if (pathtraced)
	{
		if (!pathtraced_record(commands, slot, scene, inputs, &uniforms, sizeof(block), gi, probes_updated, out_width,
			out_height))
			return NULL;
		barrier_global(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
		if (rt.queries && !slot->timed)
		{
			vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, rt.queries, rt.slot * 2 + 1);
			slot->timed = 1;
		}
		slot->serial = xgpu_vulkan_serial();
		rt.pt_index ^= 1u;
		rt.pt_history_valid = 1;
		statistics_frame();
		if (!rt.logged_pt)
		{
			rt.logged_pt = 1;
			host_logf(HOST_LOG_INFO, "prism rt: pathtraced: first window %ux%u, %.0f bounces, %.0f paths a pixel, %.0f "
				"candidates, %u lights, structure %u triangles, objects %u triangles", width, height,
				(double)block.pt[0], (double)block.pt[1], (double)block.pt[2], (unsigned)block.pt[3],
				rt.static_triangles, rt.capture.triangle_count);
		}
		if (getenv("HALO_PRISM_RT_DEBUG") && rt.frame.light_count && (rt.frame.frame % 300) == 0)
		{
			uint32_t light;

			for (light = 0; light < rt.frame.light_count && light < 6; light++)
			{
				const float *l = rt.frame.lights[light];

				host_logf(HOST_LOG_INFO, "prism rt: light %u: at %.2f %.2f %.2f radius %.2f colour %.3f %.3f %.3f "
					"intensity %.3f cone %.2f (eye %.2f %.2f %.2f)", light, (double)l[0], (double)l[1], (double)l[2],
					(double)l[3], (double)l[4], (double)l[5], (double)l[6], (double)l[7], (double)l[11],
					(double)pass->camera[0][0], (double)pass->camera[0][1], (double)pass->camera[0][2]);
			}
		}
		return rt.result;
	}
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
