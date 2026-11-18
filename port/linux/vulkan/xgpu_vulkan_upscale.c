/*
XGPU_VULKAN_UPSCALE.C

display.upscaler and display.denoiser in the Vulkan backend (PRISM v4 M5,
overhaul_research/prism_v4_renderer/design.md sections 6 and 7;
docs/prism_plugin.md; xgpu_vulkan_upscale.h).

The plugins. display.upscaler's and display.denoiser's "plugin:<name>" are
opened (xgpu_plugin.c) as the game starts, before the instance and the
device are made, so that what their vendor needs of them is enabled (NGX's
extensions, for DLSS): each plugin's instance extensions, then, for the
device chosen, its device extensions and whether it works there. A plugin
whose extension the loader or the device lacks, or that does not work on
the device, or whose call fails, is turned off for the session (its reason
logged), and the renderer's own path is drawn in its place: FSR 3.1 for an
upscaler, the path tracer's own filter for a denoiser. The same library
serves both where both name it.

The view. A player's window's 3D view drawn smaller (UPSCALE BEGIN to END,
xgpu_stream.h) is jittered: from BEGIN every draw into a target of the
view's size (the view, its G-buffer, its passes' targets) is moved by a
sub-pixel offset (xgpu_vulkan_set_jitter), FSR's Halton sequence of 8 times
the area's ratio, from BEGIN to END only; at OPAQUE its colour is kept
(the transparent surfaces' change, the reactive mask, is found from it); at
END its inputs are made by one compute pass (INPUTS,
xgpu_vulkan_upscale_shaders.h): the colour, the depth (reversed), the
motion (the G-buffer's second target's where its pass drew the pixel, else
the camera's, from the depth and SCENE_VIEW's two cameras; none on the
first-person weapon, the stencil's bit 0) and the reactive mask, each at
the render size from 0, 0; then the upscaler (FSR 3.1, or the plugin's
evaluate with those images, the jitter, the matrices and the sizes) writes
the output, at the output's size, which is stretched into the output
target's rectangle, and the view's depth (and stencil) into its depth
target's, nearest (what is drawn after the HUD, Forge's parts and the
debug geometry, is tested against it). The history is dropped where the
guest says the camera has none, and where the sizes or the upscaler change.

Everything is in the backend's commands, in the stream's order, every image
in the GENERAL layout.
*/

#include "xgpu_vulkan_upscale.h"
#include "xgpu_vulkan_fsr3.h"
#include "xgpu_vulkan_upscale_shaders.h"
#include "xgpu_plugin.h"
#include "host.h"
#include "../src/xgpu_stream.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UPSCALE_FUNCTIONS(F) \
	F(vkCreateImage) F(vkDestroyImage) F(vkGetImageMemoryRequirements) F(vkAllocateMemory) F(vkFreeMemory) \
	F(vkBindImageMemory) F(vkCreateImageView) F(vkDestroyImageView) F(vkCreateSampler) F(vkDestroySampler) \
	F(vkCreateDescriptorSetLayout) F(vkDestroyDescriptorSetLayout) F(vkCreatePipelineLayout) \
	F(vkDestroyPipelineLayout) F(vkCreateShaderModule) F(vkDestroyShaderModule) F(vkCreateComputePipelines) \
	F(vkDestroyPipeline) F(vkCreateDescriptorPool) F(vkDestroyDescriptorPool) F(vkResetDescriptorPool) \
	F(vkAllocateDescriptorSets) F(vkUpdateDescriptorSets) F(vkCmdBindPipeline) F(vkCmdBindDescriptorSets) \
	F(vkCmdDispatch) F(vkCmdPipelineBarrier) F(vkDeviceWaitIdle)

#define DECLARE(name) static PFN_##name name;
UPSCALE_FUNCTIONS(DECLARE)
#undef DECLARE

/* the frames an input pass's set and a stencil view are kept for (the
frames in flight and one more) */
#define UPSCALE_SLOTS 3
/* the world unit in metres (FSR's thresholds are in metres) */
#define METERS_PER_UNIT 3.048f
/* the reactive mask: FSR's own generation's rule (its sample's values): the
largest change of the tone-mapped colour past the threshold is the value */
#define REACTIVE_THRESHOLD 0.2f
#define REACTIVE_VALUE 0.9f
#define REACTIVE_SCALE 1.0f
/* the plugins, upscaler's and denoiser's */
#define PLUGIN_UPSCALER 0
#define PLUGIN_DENOISER 1

/* the input pass's uniforms (its GLSL's Inputs, std140) */
struct upscale_inputs_block
{
	int32_t rect[4];
	float jitter[4];
	float now[4][4];
	float last_eye[4];
	float last_inverse[3][4];
	float last_rect[4];
	float reactive[4];
};
_Static_assert(sizeof(struct upscale_inputs_block) == 192, "the input pass's uniforms");

struct upscale_image
{
	VkImage image;
	VkDeviceMemory memory;
	VkImageView view;
	VkFormat format;
	uint32_t width, height;
};

/* the inputs an upscaler is given (the render size's) */
enum
{
	INPUT_COLOR, INPUT_DEPTH, INPUT_MOTION, INPUT_REACTIVE, INPUT_COUNT
};

static const VkFormat input_formats[INPUT_COUNT] =
{
	VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R8_UNORM,
};

/* the guides a denoiser that upscales is given (the render size's, made as
it is first used; a texel each before) */
enum
{
	GUIDE_DIFFUSE, GUIDE_SPECULAR, GUIDE_NORMAL, GUIDE_COUNT
};

static const VkFormat guide_formats[GUIDE_COUNT] =
{
	VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT,
};

/* the input pass's bindings (its GLSL's) */
#define INPUT_BINDINGS 15

static struct
{
	/* ---------- the plugins */
	struct
	{
		struct xgpu_plugin *library;
		/* the library's own where both name the same plugin (not closed twice) */
		int shared;
		void *context;
		int off, created;
		char name[72];
		uint32_t sizes[4], quality;
		int sized;
		int failures_logged;
	} plugins[2];
	const char *instance_extensions[16];
	uint32_t instance_extension_count;
	/* a device extension asked for, of which plugin (for the missing) */
	const char *device_extensions[16];
	int device_extension_plugin[16];
	uint32_t device_extension_count;
	char credits[512];

	/* ---------- the view */
	int made, failed;
	struct xgpu_vulkan_handles handles;
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout pipeline_layout;
	VkPipeline pipeline;
	VkSampler nearest;
	VkDescriptorPool pools[UPSCALE_SLOTS];
	uint64_t pool_serials[UPSCALE_SLOTS];
	/* (each slot's stencil view, let go of once its frame is done) */
	VkImageView stencils[UPSCALE_SLOTS];
	uint32_t slot;
	struct upscale_image inputs[INPUT_COUNT];
	uint32_t input_width, input_height;
	struct upscale_image guides[GUIDE_COUNT];
	uint32_t guide_width, guide_height;
	/* BEGIN's denoiser, to its view's END: the path tracer's light given
	raw to a plugin (xgpu_vulkan_upscale_raw_light) only in a view begun for
	it; PRISM (its own filter) outside one */
	uint32_t denoiser;
	/* BEGIN's view (its target's number; 0 outside a view), and whether the
	G-buffer was drawn for it (xgpu_vulkan_upscale_gbuffer): the guides of a
	plugin that denoises and upscales */
	uint32_t view_number;
	int view_gbuffer;
	/* the last raw light's answer logged */
	int raw_logged, raw_last;
	/* the jitter off (HALO_UPSCALE_JITTER=0: the passthrough plugin's
	identity with the upscaler off, a test's) */
	int no_jitter;
	/* END's work between the GPU time's marks (HALO_UPSCALE_GPU_MARKS=1:
	HALO_VULKAN_GPU_TIMES_FILE's third column, where no G-buffer pass has
	taken them: display.lighting "ce"'s) */
	int gpu_marks;
	/* the output (an image of the core's: stretched into the target) and
	the colour kept at OPAQUE (the view's format and size), and a stand-in
	for the G-buffer's second target */
	struct xgpu_vulkan_image *output, *opaque, *extras_stand_in;
	int opaque_kept;
	/* BEGIN's: the view's target and its jitter; the sequence's index */
	int jittering;
	float jitter[2];
	uint32_t jitter_index;
	/* the last END's sizes and upscaler (a change drops the history) */
	uint32_t last_sizes[4];
	uint32_t last_upscaler;
	/* the last frame's camera block (the matrices' last) */
	float view_last[16], projection_last[16];
	int logged_fallback, logged_upscaled;
} up;

/* ---------- the plugins */

static void plugin_log(void *data, const char *line)
{
	(void)data;
	host_logf(HOST_LOG_INFO, "%s", line);
}

static void plugin_off(int which, const char *why)
{
	if (up.plugins[which].off || !up.plugins[which].library)
		return;
	up.plugins[which].off = 1;
	host_logf(HOST_LOG_WARN, "prism plugin %s: turned off: %s; the renderer's own %s is drawn", up.plugins[which].name,
		why, which == PLUGIN_UPSCALER ? "upscaler (FSR 3.1)" : "denoiser");
}

/* a plugin's call failed: off, with its last error */
static void plugin_failed(int which, const char *call)
{
	char why[320];
	const struct prism_plugin *table = up.plugins[which].library->table;

	snprintf(why, sizeof(why), "its %s failed (%s)", call, table->last_error(up.plugins[which].context));
	plugin_off(which, why);
}

static int plugin_ready(int which, uint32_t capabilities)
{
	return up.plugins[which].library && !up.plugins[which].off && up.plugins[which].created &&
		(up.plugins[which].library->table->info.capabilities & capabilities);
}

void xgpu_vulkan_upscale_plugins_open(const char *folder, const char *upscaler, const char *denoiser)
{
	const char *names[2];
	int which;

	names[PLUGIN_UPSCALER] = upscaler;
	names[PLUGIN_DENOISER] = denoiser;
	up.instance_extension_count = 0;
	for (which = 0; which < 2; which++)
	{
		const struct prism_plugin *table;
		const char *wanted[16];
		uint32_t count = 0, index, have;

		if (!names[which] || !names[which][0])
			continue;
		snprintf(up.plugins[which].name, sizeof(up.plugins[which].name), "%s", names[which]);
		if (which == PLUGIN_DENOISER && up.plugins[PLUGIN_UPSCALER].library &&
			!strcmp(names[PLUGIN_UPSCALER], names[PLUGIN_DENOISER]))
		{
			up.plugins[which].library = up.plugins[PLUGIN_UPSCALER].library;
			up.plugins[which].shared = 1;
			continue;
		}
		up.plugins[which].library = xgpu_plugin_open(folder, names[which], plugin_log, NULL);
		if (!up.plugins[which].library)
			continue;
		table = up.plugins[which].library->table;
		if (!(table->info.capabilities & (which == PLUGIN_UPSCALER ? PRISM_CAP_UPSCALE | PRISM_CAP_DENOISE_UPSCALE :
			PRISM_CAP_DENOISE | PRISM_CAP_DENOISE_UPSCALE)))
		{
			plugin_off(which, which == PLUGIN_UPSCALER ? "it does not upscale" : "it does not denoise");
			continue;
		}
		if (!table->instance_extensions(NULL, &count) || count > 16 ||
			(count && !table->instance_extensions(wanted, &count)))
		{
			plugin_failed(which, "instance_extensions");
			continue;
		}
		for (index = 0; index < count; index++)
		{
			for (have = 0; have < up.instance_extension_count && strcmp(up.instance_extensions[have], wanted[index]);
				have++)
				;
			if (have == up.instance_extension_count && up.instance_extension_count < 16)
				up.instance_extensions[up.instance_extension_count++] = wanted[index];
		}
	}
}

uint32_t xgpu_vulkan_upscale_instance_extensions(const char *const **names)
{
	*names = up.instance_extensions;
	return up.instance_extension_count;
}

uint32_t xgpu_vulkan_upscale_device_extensions(VkPhysicalDevice physical, const char **names, uint32_t most)
{
	int which;

	up.device_extension_count = 0;
	for (which = 0; which < 2; which++)
	{
		const struct prism_plugin *table;
		const char *wanted[16];
		uint32_t count = 0, index, have;

		if (!up.plugins[which].library || up.plugins[which].off || up.plugins[which].shared)
			continue;
		table = up.plugins[which].library->table;
		if (!table->device_extensions(physical, NULL, &count) || count > 16 ||
			(count && !table->device_extensions(physical, wanted, &count)))
		{
			plugin_failed(which, "device_extensions");
			continue;
		}
		if (!table->supported(physical))
		{
			plugin_off(which, "it does not work on this graphics card or driver");
			continue;
		}
		for (index = 0; index < count; index++)
		{
			for (have = 0; have < up.device_extension_count && strcmp(up.device_extensions[have], wanted[index]);
				have++)
				;
			if (have == up.device_extension_count && up.device_extension_count < 16)
			{
				up.device_extension_plugin[up.device_extension_count] = which;
				up.device_extensions[up.device_extension_count++] = wanted[index];
			}
		}
	}
	for (which = 0; which < (int)up.device_extension_count && which < (int)most; which++)
		names[which] = up.device_extensions[which];
	return up.device_extension_count < most ? up.device_extension_count : most;
}

void xgpu_vulkan_upscale_extension_missing(const char *extension, int device)
{
	char why[160];
	uint32_t index;
	int which;

	if (device == 2)
		snprintf(why, sizeof(why), "the graphics card's driver made no device with %s and the others it asks for",
			extension);
	else
		snprintf(why, sizeof(why), "the %s has no %s", device ? "graphics card" : "Vulkan loader", extension);
	for (which = 0; which < 2; which++)
	{
		if (!device)
		{
			/* (an instance extension: each plugin that asked for it) */
			plugin_off(which, why);
			continue;
		}
		for (index = 0; index < up.device_extension_count; index++)
		{
			if (up.device_extension_plugin[index] == which && !strcmp(up.device_extensions[index], extension))
				plugin_off(which, why);
		}
	}
	/* (a library both name: off for both) */
	if (up.plugins[PLUGIN_DENOISER].shared && up.plugins[PLUGIN_UPSCALER].off)
		up.plugins[PLUGIN_DENOISER].off = 1;
}

void xgpu_vulkan_upscale_plugins_create(void)
{
	struct prism_plugin_vulkan vulkan;
	struct xgpu_vulkan_handles handles;
	size_t used = 0;
	int which;

	if (!xgpu_vulkan_handles(&handles))
		return;
	memset(&vulkan, 0, sizeof(vulkan));
	vulkan.size = sizeof(vulkan);
	vulkan.instance = handles.instance;
	vulkan.physical = handles.physical;
	vulkan.device = handles.device;
	vulkan.get_instance_proc = handles.get_instance_proc;
	vulkan.get_device_proc = handles.get_device_proc;
	vulkan.queue_family = handles.queue_family;
	vulkan.queue = handles.queue;
	up.credits[0] = 0;
	for (which = 0; which < 2; which++)
	{
		const struct prism_plugin *table;

		if (!up.plugins[which].library || up.plugins[which].off)
			continue;
		if (up.plugins[which].shared)
		{
			up.plugins[which].off = up.plugins[PLUGIN_UPSCALER].off;
			up.plugins[which].created = up.plugins[PLUGIN_UPSCALER].created;
			up.plugins[which].context = up.plugins[PLUGIN_UPSCALER].context;
			continue;
		}
		table = up.plugins[which].library->table;
		if (!table->create(&vulkan, &up.plugins[which].context))
		{
			plugin_failed(which, "create");
			continue;
		}
		up.plugins[which].created = 1;
		host_logf(HOST_LOG_INFO, "prism plugin %s: ready (%s, %s; %s)%s%s", up.plugins[which].name,
			table->info.vendor, table->info.version, table->info.licence_notice, table->info.credits_text[0] ?
			"; credits: " : "", table->info.credits_text);
		if (table->info.credits_text[0] && used + strlen(table->info.credits_text) + 2 < sizeof(up.credits))
		{
			used += (size_t)snprintf(up.credits + used, sizeof(up.credits) - used, "%s%s", used ? "\n" : "",
				table->info.credits_text);
		}
	}
}

const char *xgpu_vulkan_upscale_credits(void)
{
	return up.credits[0] ? up.credits : NULL;
}

/* ---------- made once */

static int failed(const char *what)
{
	host_logf(HOST_LOG_WARN, "upscaler: %s: the view is drawn without upscaling", what);
	up.failed = 1;
	return 0;
}

static int memory_type(uint32_t bits)
{
	uint32_t index;

	for (index = 0; index < up.handles.memory->memoryTypeCount; index++)
	{
		if ((bits & (1u << index)) &&
			(up.handles.memory->memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
			return (int)index;
	}
	for (index = 0; index < up.handles.memory->memoryTypeCount; index++)
	{
		if (bits & (1u << index))
			return (int)index;
	}
	return -1;
}

static int make(void)
{
	static const VkDescriptorType types[INPUT_BINDINGS] =
	{
		VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
	};
	VkDescriptorSetLayoutBinding bindings[INPUT_BINDINGS];
	VkDescriptorSetLayoutCreateInfo set_info;
	VkPipelineLayoutCreateInfo layout_info;
	VkShaderModuleCreateInfo module_info;
	VkComputePipelineCreateInfo info;
	VkSamplerCreateInfo sampler;
	VkDescriptorPoolSize sizes[3];
	VkDescriptorPoolCreateInfo pool_info;
	struct xgpu_vulkan_image_description description;
	VkShaderModule module;
	uint32_t index;
	VkResult result;

	if (up.made)
		return 1;
	if (up.failed || !xgpu_vulkan_handles(&up.handles))
		return 0;
#define LOAD(name) name = (PFN_##name)up.handles.get_device_proc(up.handles.device, #name); \
	if (!name) return failed("the device has no " #name);
	UPSCALE_FUNCTIONS(LOAD)
#undef LOAD
	memset(bindings, 0, sizeof(bindings));
	for (index = 0; index < INPUT_BINDINGS; index++)
	{
		bindings[index].binding = index;
		bindings[index].descriptorType = types[index];
		bindings[index].descriptorCount = 1;
		bindings[index].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	}
	memset(&set_info, 0, sizeof(set_info));
	set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	set_info.bindingCount = INPUT_BINDINGS;
	set_info.pBindings = bindings;
	if (vkCreateDescriptorSetLayout(up.handles.device, &set_info, NULL, &up.set_layout) != VK_SUCCESS)
		return failed("its set's layout cannot be made");
	memset(&layout_info, 0, sizeof(layout_info));
	layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout_info.setLayoutCount = 1;
	layout_info.pSetLayouts = &up.set_layout;
	if (vkCreatePipelineLayout(up.handles.device, &layout_info, NULL, &up.pipeline_layout) != VK_SUCCESS)
		return failed("its pipeline's layout cannot be made");
	memset(&module_info, 0, sizeof(module_info));
	module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	module_info.codeSize = xgpu_vulkan_upscale_programs[XGPU_VULKAN_UPSCALE_INPUTS].count * sizeof(uint32_t);
	module_info.pCode = xgpu_vulkan_upscale_programs[XGPU_VULKAN_UPSCALE_INPUTS].words;
	if (vkCreateShaderModule(up.handles.device, &module_info, NULL, &module) != VK_SUCCESS)
		return failed("its shader cannot be made");
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	info.stage.module = module;
	info.stage.pName = "main";
	info.layout = up.pipeline_layout;
	result = vkCreateComputePipelines(up.handles.device, VK_NULL_HANDLE, 1, &info, NULL, &up.pipeline);
	vkDestroyShaderModule(up.handles.device, module, NULL);
	if (result != VK_SUCCESS)
		return failed("its pipeline cannot be made");
	memset(&sampler, 0, sizeof(sampler));
	sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
	sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	if (vkCreateSampler(up.handles.device, &sampler, NULL, &up.nearest) != VK_SUCCESS)
		return failed("its sampler cannot be made");
	memset(sizes, 0, sizeof(sizes));
	sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	sizes[0].descriptorCount = 7;
	sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	sizes[1].descriptorCount = 7;
	sizes[2].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	sizes[2].descriptorCount = 1;
	memset(&pool_info, 0, sizeof(pool_info));
	pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_info.maxSets = 1;
	pool_info.poolSizeCount = 3;
	pool_info.pPoolSizes = sizes;
	for (index = 0; index < UPSCALE_SLOTS; index++)
	{
		if (vkCreateDescriptorPool(up.handles.device, &pool_info, NULL, &up.pools[index]) != VK_SUCCESS)
			return failed("its descriptor pools cannot be made");
	}
	/* (the G-buffer's second target's stand-in, where none was drawn: never
	read, of its type) */
	memset(&description, 0, sizeof(description));
	description.type = XGPU_VULKAN_IMAGE_2D;
	description.format = XGPU_VULKAN_FORMAT_RGBA32_UINT;
	description.usage = XGPU_VULKAN_USAGE_SAMPLED;
	description.width = description.height = 1;
	description.depth_or_layers = 1;
	description.levels = 1;
	up.extras_stand_in = xgpu_vulkan_image_create(&description);
	if (!up.extras_stand_in)
		return failed("its stand-in cannot be made");
	{
		const char *jitter = getenv("HALO_UPSCALE_JITTER");

		up.no_jitter = jitter && !strcmp(jitter, "0");
		up.gpu_marks = getenv("HALO_UPSCALE_GPU_MARKS") && !strcmp(getenv("HALO_UPSCALE_GPU_MARKS"), "1");
	}
	up.made = 1;
	return 1;
}

/* ---------- the images */

static void image_release(struct upscale_image *image)
{
	if (image->view)
		vkDestroyImageView(up.handles.device, image->view, NULL);
	if (image->image)
		vkDestroyImage(up.handles.device, image->image, NULL);
	if (image->memory)
		vkFreeMemory(up.handles.device, image->memory, NULL);
	memset(image, 0, sizeof(*image));
}

static int image_make(struct upscale_image *made, VkFormat format, uint32_t width, uint32_t height)
{
	VkImageCreateInfo info;
	VkMemoryRequirements requirements;
	VkMemoryAllocateInfo allocate;
	VkImageViewCreateInfo view;
	int type;

	memset(made, 0, sizeof(*made));
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	info.imageType = VK_IMAGE_TYPE_2D;
	info.format = format;
	info.extent.width = width;
	info.extent.height = height;
	info.extent.depth = 1;
	info.mipLevels = 1;
	info.arrayLayers = 1;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (vkCreateImage(up.handles.device, &info, NULL, &made->image) != VK_SUCCESS)
		return 0;
	vkGetImageMemoryRequirements(up.handles.device, made->image, &requirements);
	type = memory_type(requirements.memoryTypeBits);
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = (uint32_t)type;
	if (type < 0 || vkAllocateMemory(up.handles.device, &allocate, NULL, &made->memory) != VK_SUCCESS ||
		vkBindImageMemory(up.handles.device, made->image, made->memory, 0) != VK_SUCCESS)
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
	view.subresourceRange.levelCount = 1;
	view.subresourceRange.layerCount = 1;
	if (vkCreateImageView(up.handles.device, &view, NULL, &made->view) != VK_SUCCESS)
	{
		image_release(made);
		return 0;
	}
	made->format = format;
	made->width = width;
	made->height = height;
	return 1;
}

/* the inputs of the render size and the output of the output's (the device
idle as they are made again), each GENERAL */
static int images_fit(VkCommandBuffer commands, uint32_t width, uint32_t height, uint32_t output_width,
	uint32_t output_height)
{
	struct xgpu_vulkan_image_description description;
	const struct xgpu_vulkan_image_description *had;
	VkImageMemoryBarrier barriers[INPUT_COUNT];
	uint32_t index;

	if (up.input_width != width || up.input_height != height)
	{
		if (up.input_width)
			vkDeviceWaitIdle(up.handles.device);
		for (index = 0; index < INPUT_COUNT; index++)
			image_release(&up.inputs[index]);
		up.input_width = up.input_height = 0;
		for (index = 0; index < INPUT_COUNT; index++)
		{
			if (!image_make(&up.inputs[index], input_formats[index], width, height))
				return failed("its inputs cannot be made");
		}
		memset(barriers, 0, sizeof(barriers));
		for (index = 0; index < INPUT_COUNT; index++)
		{
			barriers[index].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			barriers[index].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
			barriers[index].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			barriers[index].newLayout = VK_IMAGE_LAYOUT_GENERAL;
			barriers[index].srcQueueFamilyIndex = barriers[index].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barriers[index].image = up.inputs[index].image;
			barriers[index].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			barriers[index].subresourceRange.levelCount = 1;
			barriers[index].subresourceRange.layerCount = 1;
		}
		vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
			NULL, 0, NULL, INPUT_COUNT, barriers);
		up.input_width = width;
		up.input_height = height;
	}
	had = up.output ? xgpu_vulkan_image_description(up.output) : NULL;
	if (!had || had->width != output_width || had->height != output_height)
	{
		if (up.output)
			xgpu_vulkan_image_destroy(up.output);
		memset(&description, 0, sizeof(description));
		description.type = XGPU_VULKAN_IMAGE_2D;
		description.format = XGPU_VULKAN_FORMAT_RGBA16F;
		description.usage = XGPU_VULKAN_USAGE_SAMPLED | XGPU_VULKAN_USAGE_STORAGE;
		description.width = output_width;
		description.height = output_height;
		description.depth_or_layers = 1;
		description.levels = 1;
		up.output = xgpu_vulkan_image_create(&description);
		if (!up.output)
			return failed("its output cannot be made");
	}
	return 1;
}

/* the guides of the render size (wanted) or a texel each (not), GENERAL */
static int guides_fit(VkCommandBuffer commands, uint32_t width, uint32_t height, int wanted)
{
	VkImageMemoryBarrier barriers[GUIDE_COUNT];
	uint32_t index;

	if (!wanted)
		width = height = 1;
	if (up.guide_width == width && up.guide_height == height)
		return 1;
	if (up.guide_width)
		vkDeviceWaitIdle(up.handles.device);
	for (index = 0; index < GUIDE_COUNT; index++)
		image_release(&up.guides[index]);
	up.guide_width = up.guide_height = 0;
	for (index = 0; index < GUIDE_COUNT; index++)
	{
		if (!image_make(&up.guides[index], guide_formats[index], width, height))
			return failed("its guides cannot be made");
	}
	memset(barriers, 0, sizeof(barriers));
	for (index = 0; index < GUIDE_COUNT; index++)
	{
		barriers[index].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barriers[index].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		barriers[index].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		barriers[index].newLayout = VK_IMAGE_LAYOUT_GENERAL;
		barriers[index].srcQueueFamilyIndex = barriers[index].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barriers[index].image = up.guides[index].image;
		barriers[index].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barriers[index].subresourceRange.levelCount = 1;
		barriers[index].subresourceRange.layerCount = 1;
	}
	vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL,
		0, NULL, GUIDE_COUNT, barriers);
	up.guide_width = width;
	up.guide_height = height;
	return 1;
}

/* ---------- the camera */

/* the inverse of a 3x3 matrix's columns a, b, c (rows of the inverse); 0
where it has none */
static int inverse3(const float a[3], const float b[3], const float c[3], float rows[3][4])
{
	float cross[3][3];
	float determinant;
	int axis;

	/* (rows of the inverse: b x c, c x a, a x b over the determinant) */
	cross[0][0] = b[1] * c[2] - b[2] * c[1];
	cross[0][1] = b[2] * c[0] - b[0] * c[2];
	cross[0][2] = b[0] * c[1] - b[1] * c[0];
	cross[1][0] = c[1] * a[2] - c[2] * a[1];
	cross[1][1] = c[2] * a[0] - c[0] * a[2];
	cross[1][2] = c[0] * a[1] - c[1] * a[0];
	cross[2][0] = a[1] * b[2] - a[2] * b[1];
	cross[2][1] = a[2] * b[0] - a[0] * b[2];
	cross[2][2] = a[0] * b[1] - a[1] * b[0];
	determinant = a[0] * cross[0][0] + a[1] * cross[0][1] + a[2] * cross[0][2];
	if (fabsf(determinant) < 1e-12f)
		return 0;
	for (axis = 0; axis < 3; axis++)
	{
		rows[axis][0] = cross[axis][0] / determinant;
		rows[axis][1] = cross[axis][1] / determinant;
		rows[axis][2] = cross[axis][2] / determinant;
		rows[axis][3] = 0.0f;
	}
	return 1;
}

/* a camera block's (SCENE_VIEW's) world-to-view and view-to-clip matrices,
column-major (right-handed, looking down -z; the clip's depth reversed, 1
at the near plane, as the view's depth is kept) */
static void camera_matrices(const float camera[4][4], float view[16], float projection[16])
{
	float right[3], up_axis[3], back[3], length, tan_x, tan_y, near_plane = camera[0][3], far_plane = camera[1][3];
	int axis;

	length = sqrtf(camera[2][0] * camera[2][0] + camera[2][1] * camera[2][1] + camera[2][2] * camera[2][2]);
	tan_x = length;
	for (axis = 0; axis < 3; axis++)
		right[axis] = length > 0.0f ? camera[2][axis] / length : 0.0f;
	length = sqrtf(camera[3][0] * camera[3][0] + camera[3][1] * camera[3][1] + camera[3][2] * camera[3][2]);
	tan_y = length;
	/* (the screen's y grows down: up is its change's opposite) */
	for (axis = 0; axis < 3; axis++)
		up_axis[axis] = length > 0.0f ? -camera[3][axis] / length : 0.0f;
	length = sqrtf(camera[1][0] * camera[1][0] + camera[1][1] * camera[1][1] + camera[1][2] * camera[1][2]);
	for (axis = 0; axis < 3; axis++)
		back[axis] = length > 0.0f ? -camera[1][axis] / length : 0.0f;
	memset(view, 0, 16 * sizeof(float));
	for (axis = 0; axis < 3; axis++)
	{
		view[axis * 4 + 0] = right[axis];
		view[axis * 4 + 1] = up_axis[axis];
		view[axis * 4 + 2] = back[axis];
	}
	view[12] = -(right[0] * camera[0][0] + right[1] * camera[0][1] + right[2] * camera[0][2]);
	view[13] = -(up_axis[0] * camera[0][0] + up_axis[1] * camera[0][1] + up_axis[2] * camera[0][2]);
	view[14] = -(back[0] * camera[0][0] + back[1] * camera[0][1] + back[2] * camera[0][2]);
	view[15] = 1.0f;
	memset(projection, 0, 16 * sizeof(float));
	if (tan_x <= 0.0f || tan_y <= 0.0f || far_plane <= near_plane)
		return;
	projection[0] = 1.0f / tan_x;
	projection[5] = 1.0f / tan_y;
	/* (reversed: z_clip / w = near (far - z) / (z (far - near)) for the view's
	-z = z: 1 at near, 0 at far) */
	projection[10] = near_plane / (far_plane - near_plane);
	projection[11] = -1.0f;
	projection[14] = near_plane * far_plane / (far_plane - near_plane);
}

/* ---------- the view */

void xgpu_vulkan_upscale_begin(const struct xgpu_stream_upscale *record, struct xgpu_vulkan_image *view)
{
	const struct xgpu_vulkan_image_description *description;
	uint32_t render_width, output_width, phases;

	up.opaque_kept = 0;
	up.jittering = 0;
	up.denoiser = record->denoiser;
	up.view_number = record->view;
	up.view_gbuffer = 0;
	xgpu_vulkan_set_jitter(0.0f, 0.0f, 0, 0);
	/* (an upscaler, or the denoiser's plugin where it denoises and upscales:
	Ray Reconstruction resolves the jitter as an upscaler does) */
	if (!view || (record->upscaler == XGPU_UPSCALE_UPSCALER_NONE &&
		(record->denoiser != XGPU_UPSCALE_DENOISER_PLUGIN ||
		!plugin_ready(PLUGIN_DENOISER, PRISM_CAP_DENOISE_UPSCALE))))
		return;
	description = xgpu_vulkan_image_description(view);
	/* (the sequence of the last frame's sizes: the view's target's width
	over the output's, which BEGIN does not know yet) */
	render_width = up.last_sizes[0] ? up.last_sizes[0] : description->width;
	output_width = up.last_sizes[2] ? up.last_sizes[2] : description->width;
	phases = xgpu_vulkan_fsr3_jitter_phases(render_width, output_width);
	xgpu_vulkan_fsr3_jitter(up.jitter_index++, phases, up.jitter);
	if (up.no_jitter)
		up.jitter[0] = up.jitter[1] = 0.0f;
	up.jittering = 1;
	xgpu_vulkan_set_jitter(up.jitter[0], up.jitter[1], description->width, description->height);
}

void xgpu_vulkan_upscale_opaque(const struct xgpu_stream_upscale *record, struct xgpu_vulkan_image *view)
{
	const struct xgpu_vulkan_image_description *description, *had;
	struct xgpu_vulkan_image_description made;
	int32_t whole[4];

	if (!view || !up.jittering)
		return;
	description = xgpu_vulkan_image_description(view);
	had = up.opaque ? xgpu_vulkan_image_description(up.opaque) : NULL;
	if (!had || had->width != description->width || had->height != description->height ||
		had->format != description->format)
	{
		if (up.opaque)
			xgpu_vulkan_image_destroy(up.opaque);
		memset(&made, 0, sizeof(made));
		made.type = XGPU_VULKAN_IMAGE_2D;
		made.format = description->format;
		made.usage = XGPU_VULKAN_USAGE_SAMPLED;
		made.width = description->width;
		made.height = description->height;
		made.depth_or_layers = 1;
		made.levels = 1;
		up.opaque = xgpu_vulkan_image_create(&made);
		if (!up.opaque)
			return;
	}
	whole[0] = whole[1] = 0;
	whole[2] = (int32_t)description->width;
	whole[3] = (int32_t)description->height;
	xgpu_vulkan_image_copy_rectangle(view, 0, up.opaque, whole);
	up.opaque_kept = 1;
}

/* the input pass: the view's colour, depth, motion and reactive mask at the
render size */
static int inputs_record(VkCommandBuffer commands, const struct xgpu_stream_upscale *record,
	struct xgpu_vulkan_image *view, struct xgpu_vulkan_image *view_depth, struct xgpu_vulkan_image *extras,
	struct xgpu_vulkan_image *words, struct xgpu_vulkan_image *base, uint32_t width, uint32_t height)
{
	struct upscale_inputs_block *block;
	struct xgpu_vulkan_range range;
	VkDescriptorSetAllocateInfo allocate;
	VkDescriptorSet set;
	VkWriteDescriptorSet writes[INPUT_BINDINGS];
	VkDescriptorImageInfo images[INPUT_BINDINGS];
	VkDescriptorBufferInfo buffer;
	VkImageViewCreateInfo stencil_info;
	VkImageView stencil = VK_NULL_HANDLE;
	VkMemoryBarrier memory;
	uint32_t index, flags = 0;
	int history = (record->flags & XGPU_UPSCALE_FLAG_HISTORY) != 0;

	/* (this slot's set and stencil view free: its frame's commands done) */
	up.slot = (up.slot + 1) % UPSCALE_SLOTS;
	if (up.pool_serials[up.slot] && xgpu_vulkan_completed() < up.pool_serials[up.slot])
		xgpu_vulkan_wait(up.pool_serials[up.slot]);
	vkResetDescriptorPool(up.handles.device, up.pools[up.slot], 0);
	if (up.stencils[up.slot])
	{
		vkDestroyImageView(up.handles.device, up.stencils[up.slot], NULL);
		up.stencils[up.slot] = VK_NULL_HANDLE;
	}
	up.pool_serials[up.slot] = xgpu_vulkan_serial();
	/* (the depth's stencil, the first-person weapon's mark, through a view
	of its own, made each frame: the target's image may be made again) */
	memset(&stencil_info, 0, sizeof(stencil_info));
	stencil_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	stencil_info.image = xgpu_vulkan_image_handle(view_depth);
	stencil_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
	stencil_info.format = xgpu_vulkan_depth_format();
	stencil_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
	stencil_info.subresourceRange.levelCount = 1;
	stencil_info.subresourceRange.layerCount = 1;
	if (vkCreateImageView(up.handles.device, &stencil_info, NULL, &stencil) != VK_SUCCESS)
		return failed("the depth's stencil cannot be read");
	up.stencils[up.slot] = stencil;
	block = xgpu_vulkan_upload(sizeof(*block), 256, &range);
	if (!block)
		return 0;
	memset(block, 0, sizeof(*block));
	block->rect[0] = record->view_corners[0];
	block->rect[1] = record->view_corners[1];
	block->rect[2] = (int32_t)width;
	block->rect[3] = (int32_t)height;
	if (extras)
		flags |= 1u;
	if (history)
		flags |= 2u;
	if (up.opaque_kept && up.opaque)
		flags |= 4u;
	if (words && base)
		flags |= 8u;
	block->jitter[0] = up.jitter[0];
	block->jitter[1] = up.jitter[1];
	block->jitter[2] = (float)flags;
	memcpy(block->now, record->now, sizeof(block->now));
	block->last_eye[0] = record->last[0][0];
	block->last_eye[1] = record->last[0][1];
	block->last_eye[2] = record->last[0][2];
	if (!inverse3(record->last[1], record->last[2], record->last[3], block->last_inverse))
		block->jitter[2] = (float)(flags & ~2u);
	block->last_rect[0] = (float)record->view_corners[0];
	block->last_rect[1] = (float)record->view_corners[1];
	block->last_rect[2] = (float)width;
	block->last_rect[3] = (float)height;
	block->reactive[0] = REACTIVE_THRESHOLD;
	block->reactive[1] = REACTIVE_VALUE;
	block->reactive[2] = REACTIVE_SCALE;
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocate.descriptorPool = up.pools[up.slot];
	allocate.descriptorSetCount = 1;
	allocate.pSetLayouts = &up.set_layout;
	if (vkAllocateDescriptorSets(up.handles.device, &allocate, &set) != VK_SUCCESS)
		return failed("its descriptor set cannot be had");
	memset(images, 0, sizeof(images));
	images[0].imageView = xgpu_vulkan_image_view(view);
	images[1].imageView = xgpu_vulkan_image_view((flags & 4u) ? up.opaque : view);
	images[2].imageView = xgpu_vulkan_image_view(view_depth);
	images[3].imageView = stencil;
	images[4].imageView = xgpu_vulkan_image_view(extras ? extras : up.extras_stand_in);
	for (index = 0; index < 5; index++)
		images[index].sampler = up.nearest;
	for (index = 0; index < INPUT_COUNT; index++)
		images[5 + index].imageView = up.inputs[index].view;
	/* (the G-buffer's words and base where its guides are wanted, else
	stand-ins of their types, never read) */
	images[10].imageView = xgpu_vulkan_image_view((flags & 8u) ? words : up.extras_stand_in);
	images[10].sampler = up.nearest;
	images[11].imageView = xgpu_vulkan_image_view((flags & 8u) ? base : view);
	images[11].sampler = up.nearest;
	for (index = 0; index < GUIDE_COUNT; index++)
		images[12 + index].imageView = up.guides[index].view;
	for (index = 0; index < INPUT_BINDINGS; index++)
		images[index].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	memset(writes, 0, sizeof(writes));
	for (index = 0; index < INPUT_BINDINGS; index++)
	{
		writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[index].dstSet = set;
		writes[index].dstBinding = index;
		writes[index].descriptorCount = 1;
		writes[index].descriptorType = index < 5 || index == 10 || index == 11 ?
			VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : index == 9 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER :
			VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		if (index != 9)
			writes[index].pImageInfo = &images[index];
	}
	memset(&buffer, 0, sizeof(buffer));
	buffer.buffer = range.buffer;
	buffer.offset = range.offset;
	buffer.range = sizeof(*block);
	writes[9].pBufferInfo = &buffer;
	vkUpdateDescriptorSets(up.handles.device, INPUT_BINDINGS, writes, 0, NULL);
	vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, up.pipeline);
	vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, up.pipeline_layout, 0, 1, &set, 0, NULL);
	vkCmdDispatch(commands, (width + 7) / 8, (height + 7) / 8, 1);
	memset(&memory, 0, sizeof(memory));
	memory.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	memory.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	memory.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
	vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
		VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &memory, 0, NULL, 0, NULL);
	return 1;
}

static struct prism_plugin_image plugin_image(const struct upscale_image *image)
{
	struct prism_plugin_image made;

	memset(&made, 0, sizeof(made));
	made.image = image->image;
	made.view = image->view;
	made.format = image->format;
	made.layout = VK_IMAGE_LAYOUT_GENERAL;
	made.width = image->width;
	made.height = image->height;
	return made;
}

/* the plugin's quality for a ratio of the render size to the output's (its
vendor's names: DLSS's 0.67, 0.58, 0.5, 0.33) */
static uint32_t plugin_quality(uint32_t render_width, uint32_t output_width)
{
	float ratio = output_width ? (float)render_width / (float)output_width : 1.0f;

	return ratio > 0.99f ? PRISM_QUALITY_NATIVE : ratio > 0.62f ? PRISM_QUALITY_QUALITY :
		ratio > 0.54f ? PRISM_QUALITY_BALANCED : ratio > 0.41f ? PRISM_QUALITY_PERFORMANCE :
		PRISM_QUALITY_ULTRA_PERFORMANCE;
}

/* the frame upscaled by the upscaler's plugin; 0 where it was not (the
plugin then off) */
static int plugin_upscale(int which, VkCommandBuffer commands, const struct xgpu_stream_upscale *record,
	int reset, int guides, uint32_t width, uint32_t height, uint32_t output_width, uint32_t output_height)
{
	const struct prism_plugin *table = up.plugins[which].library->table;
	void *context = up.plugins[which].context;
	struct prism_plugin_frame frame;
	uint32_t quality = plugin_quality(width, output_width);
	/* (the library both name sized once: its context is one) */
	int sizing = up.plugins[which].shared ? PLUGIN_UPSCALER : which;

	if (!up.plugins[sizing].sized || up.plugins[sizing].sizes[0] != width || up.plugins[sizing].sizes[1] != height ||
		up.plugins[sizing].sizes[2] != output_width || up.plugins[sizing].sizes[3] != output_height ||
		up.plugins[sizing].quality != quality)
	{
		/* (the queue idle, as the ABI asks: the work so far submitted and
		waited for) */
		xgpu_vulkan_wait(xgpu_vulkan_fence());
		if (!table->resize(context, width, height, output_width, output_height, quality))
		{
			plugin_failed(which, "resize");
			return 0;
		}
		up.plugins[sizing].sizes[0] = width;
		up.plugins[sizing].sizes[1] = height;
		up.plugins[sizing].sizes[2] = output_width;
		up.plugins[sizing].sizes[3] = output_height;
		up.plugins[sizing].quality = quality;
		up.plugins[sizing].sized = 1;
		reset = 1;
		/* (the commands again: the fence ended them) */
		commands = xgpu_vulkan_compute_commands();
	}
	memset(&frame, 0, sizeof(frame));
	frame.size = sizeof(frame);
	frame.commands = commands;
	frame.color_in = plugin_image(&up.inputs[INPUT_COLOR]);
	frame.depth = plugin_image(&up.inputs[INPUT_DEPTH]);
	frame.motion = plugin_image(&up.inputs[INPUT_MOTION]);
	frame.reactive_mask = plugin_image(&up.inputs[INPUT_REACTIVE]);
	frame.transparency_mask = frame.reactive_mask;
	if (guides)
	{
		frame.diffuse_albedo = plugin_image(&up.guides[GUIDE_DIFFUSE]);
		frame.specular_albedo = plugin_image(&up.guides[GUIDE_SPECULAR]);
		frame.normal_roughness = plugin_image(&up.guides[GUIDE_NORMAL]);
	}
	frame.color_out.image = xgpu_vulkan_image_handle(up.output);
	frame.color_out.view = xgpu_vulkan_image_view(up.output);
	frame.color_out.format = VK_FORMAT_R16G16B16A16_SFLOAT;
	frame.color_out.layout = VK_IMAGE_LAYOUT_GENERAL;
	frame.color_out.width = output_width;
	frame.color_out.height = output_height;
	frame.jitter[0] = up.jitter[0];
	frame.jitter[1] = up.jitter[1];
	frame.exposure = 1.0f;
	frame.pre_exposure = 1.0f;
	frame.sharpness = record->sharpness;
	frame.frame_time_ms = record->frame_milliseconds;
	camera_matrices(record->now, frame.view, frame.projection);
	if (reset)
	{
		memcpy(frame.view_prev, frame.view, sizeof(frame.view_prev));
		memcpy(frame.projection_prev, frame.projection, sizeof(frame.projection_prev));
	}
	else
	{
		memcpy(frame.view_prev, up.view_last, sizeof(frame.view_prev));
		memcpy(frame.projection_prev, up.projection_last, sizeof(frame.projection_prev));
	}
	memcpy(up.view_last, frame.view, sizeof(up.view_last));
	memcpy(up.projection_last, frame.projection, sizeof(up.projection_last));
	frame.reset = reset ? 1u : 0u;
	frame.render_width = width;
	frame.render_height = height;
	frame.output_width = output_width;
	frame.output_height = output_height;
	if (!table->evaluate(context, &frame))
	{
		plugin_failed(which, "evaluate");
		return 0;
	}
	return 1;
}

/* the view stretched into the window as it is (linear), where no upscaler
can run: the window is never left without its view */
static void stretch(const struct xgpu_stream_upscale *record, struct xgpu_vulkan_image *view,
	struct xgpu_vulkan_image *output)
{
	if (view != output && record->view_corners[2] > record->view_corners[0] &&
		record->view_corners[3] > record->view_corners[1] && record->output_corners[2] > record->output_corners[0] &&
		record->output_corners[3] > record->output_corners[1])
		xgpu_vulkan_image_blit(view, record->view_corners, output, record->output_corners, 1);
}

void xgpu_vulkan_upscale_end(const struct xgpu_stream_upscale *record, struct xgpu_vulkan_image *view,
	struct xgpu_vulkan_image *view_depth, struct xgpu_vulkan_image *output, struct xgpu_vulkan_image *output_depth,
	struct xgpu_vulkan_image *gbuffer_extras, struct xgpu_vulkan_image *gbuffer_words,
	struct xgpu_vulkan_image *gbuffer_base)
{
	VkCommandBuffer commands;
	uint32_t width, height, output_width, output_height, upscaler = record->upscaler;
	int32_t from[4], to[4];
	int reset, done = 0, denoise_upscale;
	VkMemoryBarrier memory;

	/* (the view over: the path tracer's light is given raw to a plugin only
	between a view's BEGIN and its END, so that a frame with no view begun,
	split screen's, a replay's held view, a movie's or one with neither an
	upscaler nor a denoiser's plugin, has the tracer's own filter) */
	up.denoiser = XGPU_UPSCALE_DENOISER_PRISM;
	up.view_number = 0;
	up.view_gbuffer = 0;
	xgpu_vulkan_set_jitter(0.0f, 0.0f, 0, 0);
	if (!up.jittering || !view || !view_depth || !output)
	{
		up.jittering = 0;
		return;
	}
	up.jittering = 0;
	/* (a view jittered for the denoiser's plugin with no upscaler: FSR 3.1
	at its own size resolves it where the plugin does not) */
	if (upscaler == XGPU_UPSCALE_UPSCALER_NONE)
		upscaler = XGPU_UPSCALE_UPSCALER_FSR3;
	if (record->view_corners[2] <= record->view_corners[0] || record->view_corners[3] <= record->view_corners[1] ||
		record->output_corners[2] <= record->output_corners[0] ||
		record->output_corners[3] <= record->output_corners[1] || !make())
	{
		stretch(record, view, output);
		return;
	}
	width = (uint32_t)(record->view_corners[2] - record->view_corners[0]);
	height = (uint32_t)(record->view_corners[3] - record->view_corners[1]);
	output_width = (uint32_t)(record->output_corners[2] - record->output_corners[0]);
	output_height = (uint32_t)(record->output_corners[3] - record->output_corners[1]);
	/* (a plugin's, where its plugin is ready; else FSR 3.1's) */
	if (upscaler == XGPU_UPSCALE_UPSCALER_PLUGIN && !plugin_ready(PLUGIN_UPSCALER, PRISM_CAP_UPSCALE |
		PRISM_CAP_DENOISE_UPSCALE))
	{
		if (!up.logged_fallback)
			host_logf(HOST_LOG_INFO, "upscaler: no plugin is ready: FSR 3.1 upscales the view");
		up.logged_fallback = 1;
		upscaler = XGPU_UPSCALE_UPSCALER_FSR3;
	}
	/* (the denoiser's plugin, where it denoises and upscales and the view had
	its G-buffer, the guides: Ray Reconstruction, in the upscaler's place) */
	denoise_upscale = record->denoiser == XGPU_UPSCALE_DENOISER_PLUGIN &&
		plugin_ready(PLUGIN_DENOISER, PRISM_CAP_DENOISE_UPSCALE) && gbuffer_extras && gbuffer_words && gbuffer_base;
	if (denoise_upscale)
		upscaler = XGPU_UPSCALE_UPSCALERS;
	reset = !(record->flags & XGPU_UPSCALE_FLAG_HISTORY) || up.last_sizes[0] != width ||
		up.last_sizes[1] != height || up.last_sizes[2] != output_width || up.last_sizes[3] != output_height ||
		up.last_upscaler != upscaler;
	up.last_sizes[0] = width;
	up.last_sizes[1] = height;
	up.last_sizes[2] = output_width;
	up.last_sizes[3] = output_height;
	up.last_upscaler = upscaler;
	/* the images, marked as a copy marks them (the view's pass stores them;
	the output written) */
	xgpu_vulkan_image_compute_use(view, 0);
	xgpu_vulkan_image_compute_use(view_depth, 0);
	if (up.opaque_kept)
		xgpu_vulkan_image_compute_use(up.opaque, 0);
	if (gbuffer_extras)
		xgpu_vulkan_image_compute_use(gbuffer_extras, 0);
	if (denoise_upscale)
	{
		xgpu_vulkan_image_compute_use(gbuffer_words, 0);
		xgpu_vulkan_image_compute_use(gbuffer_base, 0);
	}
	commands = xgpu_vulkan_compute_commands();
	if (up.gpu_marks)
		xgpu_vulkan_gpu_time_mark(0);
	if (!images_fit(commands, width, height, output_width, output_height) ||
		!guides_fit(commands, width, height, denoise_upscale))
	{
		stretch(record, view, output);
		return;
	}
	xgpu_vulkan_image_compute_use(up.output, 1);
	xgpu_vulkan_image_compute_use(up.extras_stand_in, 0);
	if (!inputs_record(commands, record, view, view_depth, gbuffer_extras, denoise_upscale ? gbuffer_words : NULL,
		denoise_upscale ? gbuffer_base : NULL, width, height))
	{
		stretch(record, view, output);
		return;
	}
	if (denoise_upscale)
		done = plugin_upscale(PLUGIN_DENOISER, commands, record, reset, 1, width, height, output_width, output_height);
	else if (upscaler == XGPU_UPSCALE_UPSCALER_PLUGIN)
		done = plugin_upscale(PLUGIN_UPSCALER, commands, record, reset, 0, width, height, output_width,
			output_height);
	if (!done)
	{
		struct xgpu_vulkan_fsr3_frame frame;
		float tan_y = sqrtf(record->now[3][0] * record->now[3][0] + record->now[3][1] * record->now[3][1] +
			record->now[3][2] * record->now[3][2]);

		memset(&frame, 0, sizeof(frame));
		frame.commands = xgpu_vulkan_compute_commands();
		frame.color = up.inputs[INPUT_COLOR].view;
		frame.depth = up.inputs[INPUT_DEPTH].view;
		frame.motion = up.inputs[INPUT_MOTION].view;
		frame.reactive = up.inputs[INPUT_REACTIVE].view;
		frame.output = xgpu_vulkan_image_view(up.output);
		frame.render_width = width;
		frame.render_height = height;
		frame.output_width = output_width;
		frame.output_height = output_height;
		frame.jitter[0] = up.jitter[0];
		frame.jitter[1] = up.jitter[1];
		frame.near_plane = record->now[0][3];
		frame.far_plane = record->now[1][3];
		frame.field_of_view = tan_y > 0.0f ? 2.0f * atanf(tan_y) : 1.0f;
		frame.frame_milliseconds = record->frame_milliseconds;
		frame.sharpness = record->sharpness;
		frame.reset = reset || upscaler != XGPU_UPSCALE_UPSCALER_FSR3;
		frame.meters_per_unit = METERS_PER_UNIT;
		if (!xgpu_vulkan_fsr3_dispatch(&frame))
		{
			stretch(record, view, output);
			return;
		}
	}
	/* (the output written before it is read) */
	commands = xgpu_vulkan_compute_commands();
	memset(&memory, 0, sizeof(memory));
	memory.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	memory.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
	memory.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT |
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &memory, 0, NULL, 0, NULL);
	/* the output into the window's rectangle, and the view's depth into its
	depth's */
	from[0] = from[1] = 0;
	from[2] = (int32_t)output_width;
	from[3] = (int32_t)output_height;
	memcpy(to, record->output_corners, sizeof(to));
	if (!xgpu_vulkan_image_blit(up.output, from, output, to, 0))
	{
		failed("its output cannot be copied into the window");
		stretch(record, view, output);
		return;
	}
	if (output_depth && output_depth != view_depth && !xgpu_vulkan_image_blit(view_depth, record->view_corners,
		output_depth, record->output_corners, 0) && !up.logged_upscaled)
		host_logf(HOST_LOG_WARN, "upscaler: the view's depth cannot be stretched into the window's: what is drawn "
			"after the HUD is not tested against it");
	if (up.gpu_marks)
		xgpu_vulkan_gpu_time_mark(1);
	if (!up.logged_upscaled)
	{
		host_logf(HOST_LOG_INFO, "upscaler: %s%s: %ux%u upscaled to %ux%u", denoise_upscale ?
			up.plugins[PLUGIN_DENOISER].name : upscaler == XGPU_UPSCALE_UPSCALER_PLUGIN ?
			up.plugins[PLUGIN_UPSCALER].name : "FSR 3.1", denoise_upscale ? " (denoising, the G-buffer's guides)" : "",
			width, height, output_width, output_height);
		up.logged_upscaled = 1;
	}
}

/* ---------- display.denoiser's plugin */

void xgpu_vulkan_upscale_gbuffer(uint32_t target)
{
	up.view_gbuffer = up.view_number && target == up.view_number;
}

int xgpu_vulkan_upscale_raw_light(void)
{
	/* (in a view begun for the denoiser's plugin: one that denoises takes the
	light as it is traced; one that denoises and upscales takes it at END,
	where the view has its G-buffer's guides, else FSR 3.1 resolves the view
	and the tracer's own filter is kept) */
	int raw = up.denoiser == XGPU_UPSCALE_DENOISER_PLUGIN && (plugin_ready(PLUGIN_DENOISER, PRISM_CAP_DENOISE) ||
		(plugin_ready(PLUGIN_DENOISER, PRISM_CAP_DENOISE_UPSCALE) && up.view_gbuffer));

	/* (each change logged, where a denoiser's plugin is named) */
	if (up.plugins[PLUGIN_DENOISER].library && (!up.raw_logged || raw != up.raw_last))
	{
		if (raw)
			host_logf(HOST_LOG_INFO, "denoiser: the path tracer's light given raw to %s",
				up.plugins[PLUGIN_DENOISER].name);
		else
			host_logf(HOST_LOG_INFO, "denoiser: the path tracer's own filter (%s not given the light)",
				up.plugins[PLUGIN_DENOISER].name);
		up.raw_logged = 1;
		up.raw_last = raw;
	}
	return raw;
}

static struct prism_plugin_image core_image(struct xgpu_vulkan_image *image, VkFormat format)
{
	struct prism_plugin_image made;
	const struct xgpu_vulkan_image_description *description = xgpu_vulkan_image_description(image);

	memset(&made, 0, sizeof(made));
	made.image = xgpu_vulkan_image_handle(image);
	made.view = xgpu_vulkan_image_view(image);
	made.format = format;
	made.layout = VK_IMAGE_LAYOUT_GENERAL;
	made.width = description->width;
	made.height = description->height;
	return made;
}

int xgpu_vulkan_upscale_denoise(VkCommandBuffer commands, struct xgpu_vulkan_image *light,
	struct xgpu_vulkan_image *added, uint32_t width, uint32_t height)
{
	const struct prism_plugin *table;
	struct prism_plugin_frame frame;

	if (up.denoiser != XGPU_UPSCALE_DENOISER_PLUGIN || !plugin_ready(PLUGIN_DENOISER, PRISM_CAP_DENOISE) || !light ||
		!added)
		return 0;
	table = up.plugins[PLUGIN_DENOISER].library->table;
	memset(&frame, 0, sizeof(frame));
	frame.size = sizeof(frame);
	frame.commands = commands;
	frame.diffuse_radiance = core_image(light, VK_FORMAT_R16G16B16A16_SFLOAT);
	frame.specular_radiance = core_image(added, VK_FORMAT_R16G16B16A16_SFLOAT);
	frame.exposure = frame.pre_exposure = 1.0f;
	frame.render_width = frame.output_width = width;
	frame.render_height = frame.output_height = height;
	if (!table->evaluate(up.plugins[PLUGIN_DENOISER].context, &frame))
	{
		plugin_failed(PLUGIN_DENOISER, "evaluate");
		return 0;
	}
	return 1;
}

/* ---------- the end */

void xgpu_vulkan_upscale_shutdown(void)
{
	uint32_t index;
	int which;

	xgpu_vulkan_fsr3_shutdown();
	if (up.handles.device)
	{
		vkDeviceWaitIdle(up.handles.device);
		for (index = 0; index < INPUT_COUNT; index++)
			image_release(&up.inputs[index]);
		for (index = 0; index < GUIDE_COUNT; index++)
			image_release(&up.guides[index]);
		for (index = 0; index < UPSCALE_SLOTS; index++)
		{
			if (up.stencils[index])
				vkDestroyImageView(up.handles.device, up.stencils[index], NULL);
			if (up.pools[index])
				vkDestroyDescriptorPool(up.handles.device, up.pools[index], NULL);
		}
		if (up.pipeline)
			vkDestroyPipeline(up.handles.device, up.pipeline, NULL);
		if (up.pipeline_layout)
			vkDestroyPipelineLayout(up.handles.device, up.pipeline_layout, NULL);
		if (up.set_layout)
			vkDestroyDescriptorSetLayout(up.handles.device, up.set_layout, NULL);
		if (up.nearest)
			vkDestroySampler(up.handles.device, up.nearest, NULL);
	}
	for (which = 0; which < 2; which++)
	{
		if (up.plugins[which].library && !up.plugins[which].shared)
		{
			if (up.plugins[which].created)
				up.plugins[which].library->table->destroy(up.plugins[which].context);
			xgpu_plugin_close(up.plugins[which].library);
		}
	}
	memset(&up, 0, sizeof(up));
}
