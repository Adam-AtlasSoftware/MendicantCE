/*
XGPU_VULKAN_WORLD_AO.C

display.ambient_occlusion "world"'s resources in the Vulkan backend
(docs/world-ao.md, "GPU binding layout"): the structure's distance field, a
3D image of signed bytes (VK_FORMAT_R8_SNORM) read through a linear sampler
clamped to its edge, and the window's block (struct xgpu_world_ao_block), a
uniform buffer. Both are in display.ambient_occlusion's set
(XGPU_VULKAN_SET_OCCLUSION), at XGPU_VULKAN_BINDING_WORLD_SDF and
XGPU_VULKAN_BINDING_WORLD_AO, which the core writes beside the screen's
occlusion (xgpu_vulkan.c's occlusion_set) from what this file hands it.

The field arrives a few slices at a time (the command stream's
WORLD_AO_FIELD records): its image is made as its first slices come, and
is drawn with once its last have (until then, and with none, the draws read
a 1x1x1 stand-in of +127: far outside, no occlusion). The block arrives
with each window (WORLD_AO_WINDOW) and is put in the upload ring of the
submission whose draws read it (written again into a later submission's,
should its window span two); with none, a block of zeros.
*/

#include "xgpu_stream.h"
#include "xgpu_vulkan.h"

#include <stdlib.h>
#include <string.h>

static struct
{
	/* the field's image (NULL none), its size, and whether its last
	slices have come; the stand-in */
	struct xgpu_vulkan_image *field;
	uint32_t width, height, depth;
	int whole;
	struct xgpu_vulkan_image *stand_in;
	VkSampler sampler;
	/* the window's block (zeros: none), and where it is in the ring for
	the submission it was written for */
	struct xgpu_world_ao_block block;
	uint64_t block_serial;
	struct xgpu_vulkan_range block_range;
	/* bumped whenever what the set names changes */
	uint64_t number;
} world_ao;

int xgpu_vulkan_world_ao_create(void)
{
	static const signed char far_outside = 127;
	struct xgpu_vulkan_image_description description;
	struct xgpu_vulkan_sampler_description sampler;

	memset(&world_ao, 0, sizeof(world_ao));
	memset(&description, 0, sizeof(description));
	description.type = XGPU_VULKAN_IMAGE_3D;
	description.format = XGPU_VULKAN_FORMAT_R8_SNORM;
	description.usage = XGPU_VULKAN_USAGE_SAMPLED;
	description.width = description.height = description.depth_or_layers = 1;
	description.levels = 1;
	world_ao.stand_in = xgpu_vulkan_image_create(&description);
	if (!world_ao.stand_in)
		return 0;
	xgpu_vulkan_image_upload(world_ao.stand_in, 0, 0, 0, 0, 0, 1, 1, 1, &far_outside, 1, 1);
	memset(&sampler, 0, sizeof(sampler));
	sampler.minify = sampler.magnify = XGPU_VULKAN_FILTER_LINEAR;
	sampler.mip = XGPU_VULKAN_MIP_NONE;
	sampler.address[0] = sampler.address[1] = sampler.address[2] = XGPU_VULKAN_ADDRESS_CLAMP;
	sampler.anisotropy = 1;
	world_ao.sampler = xgpu_vulkan_sampler(&sampler);
	world_ao.number = 1;
	return world_ao.sampler != VK_NULL_HANDLE;
}

void xgpu_vulkan_world_ao_field(uint32_t width, uint32_t height, uint32_t depth, uint32_t z_first, uint32_t z_count,
	const void *voxels)
{
	/* (none: let go of) */
	if (!width || !height || !depth || !voxels)
	{
		if (world_ao.field)
			xgpu_vulkan_image_destroy(world_ao.field);
		world_ao.field = NULL;
		world_ao.whole = 0;
		world_ao.number++;
		return;
	}
	/* (a new field: its image made as its first slices come) */
	if (!world_ao.field || world_ao.width != width || world_ao.height != height || world_ao.depth != depth ||
		z_first == 0)
	{
		struct xgpu_vulkan_image_description description;

		if (world_ao.field)
			xgpu_vulkan_image_destroy(world_ao.field);
		memset(&description, 0, sizeof(description));
		description.type = XGPU_VULKAN_IMAGE_3D;
		description.format = XGPU_VULKAN_FORMAT_R8_SNORM;
		description.usage = XGPU_VULKAN_USAGE_SAMPLED;
		description.width = width;
		description.height = height;
		description.depth_or_layers = depth;
		description.levels = 1;
		world_ao.field = xgpu_vulkan_image_create(&description);
		world_ao.width = width;
		world_ao.height = height;
		world_ao.depth = depth;
		world_ao.whole = 0;
		world_ao.number++;
		if (!world_ao.field)
			return;
	}
	xgpu_vulkan_image_upload(world_ao.field, 0, 0, 0, 0, z_first, width, height, z_count, voxels, width,
		width * height);
	if (z_first + z_count >= depth && !world_ao.whole)
	{
		world_ao.whole = 1;
		world_ao.number++;
	}
}

void xgpu_vulkan_world_ao_window(const struct xgpu_world_ao_block *block)
{
	if (block)
		world_ao.block = *block;
	else
		memset(&world_ao.block, 0, sizeof(world_ao.block));
	/* (written into the ring when a draw's set next names it) */
	world_ao.block_serial = 0;
	world_ao.number++;
}

struct xgpu_vulkan_image *xgpu_vulkan_world_ao_field_image(void)
{
	return world_ao.field && world_ao.whole ? world_ao.field : world_ao.stand_in;
}

VkSampler xgpu_vulkan_world_ao_sampler(void)
{
	return world_ao.sampler;
}

int xgpu_vulkan_world_ao_block(uint64_t serial, VkDescriptorBufferInfo *info)
{
	if (world_ao.block_serial != serial)
	{
		void *bytes = xgpu_vulkan_upload(sizeof(world_ao.block), 256, &world_ao.block_range);

		if (!bytes)
			return 0;
		memcpy(bytes, &world_ao.block, sizeof(world_ao.block));
		world_ao.block_serial = serial;
		world_ao.number++;
	}
	info->buffer = world_ao.block_range.buffer;
	info->offset = world_ao.block_range.offset;
	info->range = sizeof(world_ao.block);
	return 1;
}

uint64_t xgpu_vulkan_world_ao_number(void)
{
	return world_ao.number;
}
