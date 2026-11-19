/* The plugin loader (xgpu_plugin.c) and the passthrough plugin, without a
GPU (tools/test_prism_plugin.py):

  plugin_test open <folder> <name>        open, print the info, close
  plugin_test abi <folder> <name> <abi>   open asking for another ABI
  plugin_test empty <folder>              NULL and "" names: no file touched
  plugin_test record <folder> <name>      create and evaluate over a fake
                                          device: the commands recorded

Each line the loader logs is printed "log: ...". Linked with
-Wl,--wrap=fopen,--wrap=dlopen, so that every file the loader touches is
counted. */

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xgpu_plugin.h"

static int touched;

FILE *__real_fopen(const char *path, const char *mode);
void *__real_dlopen(const char *path, int flags);

FILE *__wrap_fopen(const char *path, const char *mode)
{
	touched++;
	return __real_fopen(path, mode);
}

void *__wrap_dlopen(const char *path, int flags)
{
	touched++;
	return __real_dlopen(path, flags);
}

static void log_line(void *data, const char *line)
{
	(void)data;
	printf("log: %s\n", line);
}

/* ---------- a fake device: the commands counted */

static int barriers, copies, blits, layout_balance;

static VKAPI_ATTR void VKAPI_CALL fake_barrier(VkCommandBuffer commands, VkPipelineStageFlags from,
	VkPipelineStageFlags to, VkDependencyFlags flags, uint32_t memory_count, const VkMemoryBarrier *memory,
	uint32_t buffer_count, const VkBufferMemoryBarrier *buffers, uint32_t image_count,
	const VkImageMemoryBarrier *images)
{
	uint32_t index;

	(void)commands; (void)from; (void)to; (void)flags; (void)memory_count; (void)memory; (void)buffer_count;
	(void)buffers;
	barriers++;
	for (index = 0; index < image_count; index++)
	{
		layout_balance += (int)images[index].newLayout;
		layout_balance -= (int)images[index].oldLayout;
	}
}

static VKAPI_ATTR void VKAPI_CALL fake_copy(VkCommandBuffer commands, VkImage from, VkImageLayout from_layout,
	VkImage to, VkImageLayout to_layout, uint32_t count, const VkImageCopy *regions)
{
	(void)commands; (void)from; (void)to;
	if (from_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && to_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
		count == 1 && regions[0].extent.width == 64 && regions[0].extent.height == 32)
		copies++;
}

static VKAPI_ATTR void VKAPI_CALL fake_blit(VkCommandBuffer commands, VkImage from, VkImageLayout from_layout,
	VkImage to, VkImageLayout to_layout, uint32_t count, const VkImageBlit *regions, VkFilter filter)
{
	(void)commands; (void)from; (void)to; (void)from_layout; (void)to_layout;
	if (count == 1 && filter == VK_FILTER_NEAREST && regions[0].dstOffsets[1].x == 128)
		blits++;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL fake_device_proc(VkDevice device, const char *name)
{
	(void)device;
	if (!strcmp(name, "vkCmdPipelineBarrier"))
		return (PFN_vkVoidFunction)fake_barrier;
	if (!strcmp(name, "vkCmdCopyImage"))
		return (PFN_vkVoidFunction)fake_copy;
	if (!strcmp(name, "vkCmdBlitImage"))
		return (PFN_vkVoidFunction)fake_blit;
	return NULL;
}

static int record(const struct prism_plugin *table)
{
	struct prism_plugin_vulkan vulkan;
	struct prism_plugin_frame frame;
	void *context = NULL;

	memset(&vulkan, 0, sizeof(vulkan));
	vulkan.size = sizeof(vulkan);
	vulkan.device = (VkDevice)(uintptr_t)0x1000;
	vulkan.get_device_proc = fake_device_proc;
	if (!table->create(&vulkan, &context))
	{
		printf("create failed: %s\n", table->last_error(NULL));
		return 1;
	}
	memset(&frame, 0, sizeof(frame));
	frame.size = sizeof(frame);
	frame.commands = (VkCommandBuffer)(uintptr_t)0x2000;
	frame.color_in.image = (VkImage)(uintptr_t)0x3000;
	frame.color_in.format = VK_FORMAT_R16G16B16A16_SFLOAT;
	frame.color_in.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	frame.color_in.width = 64;
	frame.color_in.height = 32;
	frame.color_out = frame.color_in;
	frame.color_out.image = (VkImage)(uintptr_t)0x4000;
	frame.color_out.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	if (!table->resize(context, 64, 32, 64, 32, PRISM_QUALITY_NATIVE) || !table->evaluate(context, &frame))
		return 1;
	frame.color_out.width = 128;
	frame.color_out.height = 64;
	if (!table->evaluate(context, &frame))
		return 1;
	/* (a denoiser's frame: its radiance alone, nothing recorded) */
	{
		struct prism_plugin_frame light;
		int before = copies + blits + barriers;

		memset(&light, 0, sizeof(light));
		light.size = sizeof(light);
		light.commands = frame.commands;
		light.diffuse_radiance = frame.color_in;
		light.specular_radiance = frame.color_in;
		if (!table->evaluate(context, &light))
			return 1;
		printf("denoised with %d commands\n", copies + blits + barriers - before);
	}
	frame.size = 4;
	if (table->evaluate(context, &frame))
		return 1;
	printf("refused small frame: %s\n", table->last_error(context));
	table->destroy(context);
	printf("copies %d blits %d barriers %d balance %d\n", copies, blits, barriers, layout_balance);
	return 0;
}

int main(int argc, char **argv)
{
	struct xgpu_plugin *plugin;

	if (argc >= 3 && !strcmp(argv[1], "empty"))
	{
		touched = 0;
		if (xgpu_plugin_open(argv[2], NULL, log_line, NULL) || xgpu_plugin_open(argv[2], "", log_line, NULL))
			return 1;
		printf("touched %d\n", touched);
		return touched != 0;
	}
	if (argc < 4)
		return 2;
	plugin = !strcmp(argv[1], "abi") && argc >= 5 ?
		xgpu_plugin_open_abi(argv[2], argv[3], (uint32_t)strtoul(argv[4], NULL, 0), log_line, NULL) :
		xgpu_plugin_open(argv[2], argv[3], log_line, NULL);
	if (!plugin)
	{
		printf("refused\n");
		return 3;
	}
	printf("name %s vendor %s version %s capabilities %u abi %u\n", plugin->table->info.name,
		plugin->table->info.vendor, plugin->table->info.version, plugin->table->info.capabilities,
		plugin->table->info.abi);
	if (!strcmp(argv[1], "record") && record(plugin->table))
	{
		xgpu_plugin_close(plugin);
		return 4;
	}
	xgpu_plugin_close(plugin);
	return 0;
}
