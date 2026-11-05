/*
XGPU_VULKAN_WORLD_AO.C

display.ambient_occlusion "world"'s resources in the Vulkan backend
(docs/world-ao.md, "GPU binding layout"): the structure's distance field,
sparse (world_sdf.h): its atlas of bricks, a 3D image of signed bytes
(VK_FORMAT_R8_SNORM) read through a linear sampler clamped to its edge, and
its table of cells, a 3D RGBA8 image read by texel; and the window's block
(struct xgpu_world_ao_block), a uniform buffer; and the static scenery's
pages of the level's occlusion, a 2D array of bytes (R8 unorm) read through
the same sampler. All are in display.ambient_occlusion's set
(XGPU_VULKAN_SET_OCCLUSION), at XGPU_VULKAN_BINDING_WORLD_SDF,
_WORLD_SDF_TABLE, _WORLD_AO and _WORLD_AO_SCENERY, which the core writes
beside the screen's occlusion (xgpu_vulkan.c's occlusion_set) from what this
file hands it.

The field arrives a few slices at a time (the command stream's
WORLD_AO_FIELD records), its table first, then its atlas: each image is
made as its first slices come, and the field is drawn with once both have
had their last (until then, and with none, the draws read 1x1x1 stand-ins:
a table cell far outside, no occlusion). The pages come before the table,
and are read once they have all come (their stand-in a page of none; the
game names a layer to the draws only once the field is whole). The block arrives with each window
(WORLD_AO_WINDOW) and is put in the upload ring of the submission whose
draws read it (written again into a later submission's, should its window
span two); with none, a block of zeros.
*/

#include "xgpu_stream.h"
#include "xgpu_vulkan.h"

#include <stdlib.h>
#include <string.h>

/* a part of the field: its image (NULL none), its size, and whether its
last slices have come */
struct world_ao_part
{
	struct xgpu_vulkan_image *image;
	uint32_t width, height, depth;
	int whole;
};

static struct
{
	/* the table, the atlas, the static scenery's pages and the vehicles'
	occlusion fields' atlas (XGPU_STREAM_WORLD_AO_TABLE, _ATLAS, _SCENERY,
	_OBJECTS), and their stand-ins */
	struct world_ao_part parts[4];
	struct xgpu_vulkan_image *stand_ins[4];
	VkSampler sampler;
	/* the window's block (zeros: none), and where it is in the ring for
	the submission it was written for */
	struct xgpu_world_ao_block block;
	uint64_t block_serial;
	struct xgpu_vulkan_range block_range;
	/* bumped whenever what the set names changes */
	uint64_t number;
} world_ao;

static struct xgpu_vulkan_image *world_ao_image(uint32_t part, uint32_t width, uint32_t height, uint32_t depth)
{
	struct xgpu_vulkan_image_description description;

	memset(&description, 0, sizeof(description));
	description.type = part == XGPU_STREAM_WORLD_AO_SCENERY ? XGPU_VULKAN_IMAGE_2D_ARRAY : XGPU_VULKAN_IMAGE_3D;
	/* (the pages' bytes: R8 unorm, the coverage's format; the vehicles'
	fields' four halves: RGBA16F) */
	description.format = part == XGPU_STREAM_WORLD_AO_TABLE ? XGPU_VULKAN_FORMAT_RGBA8 :
		part == XGPU_STREAM_WORLD_AO_SCENERY ? XGPU_VULKAN_FORMAT_COVERAGE :
		part == XGPU_STREAM_WORLD_AO_OBJECTS ? XGPU_VULKAN_FORMAT_RGBA16F : XGPU_VULKAN_FORMAT_R8_SNORM;
	description.usage = XGPU_VULKAN_USAGE_SAMPLED;
	description.width = width;
	description.height = height;
	description.depth_or_layers = depth;
	description.levels = 1;
	return xgpu_vulkan_image_create(&description);
}

int xgpu_vulkan_world_ao_create(void)
{
	/* (a cell far outside; a voxel +127, far outside) */
	static const unsigned char outside_cell[4] = { 0, 0, 0, 0 };
	static const signed char far_outside = 127;
	static const unsigned char none = 255;
	/* (a texel of no occlusion: t and v 0) */
	static const unsigned short no_occlusion[4] = { 0, 0, 0, 0 };
	struct xgpu_vulkan_sampler_description sampler;

	memset(&world_ao, 0, sizeof(world_ao));
	world_ao.stand_ins[XGPU_STREAM_WORLD_AO_TABLE] = world_ao_image(XGPU_STREAM_WORLD_AO_TABLE, 1, 1, 1);
	world_ao.stand_ins[XGPU_STREAM_WORLD_AO_ATLAS] = world_ao_image(XGPU_STREAM_WORLD_AO_ATLAS, 1, 1, 1);
	world_ao.stand_ins[XGPU_STREAM_WORLD_AO_SCENERY] = world_ao_image(XGPU_STREAM_WORLD_AO_SCENERY, 1, 1, 1);
	world_ao.stand_ins[XGPU_STREAM_WORLD_AO_OBJECTS] = world_ao_image(XGPU_STREAM_WORLD_AO_OBJECTS, 1, 1, 1);
	if (!world_ao.stand_ins[0] || !world_ao.stand_ins[1] || !world_ao.stand_ins[2] || !world_ao.stand_ins[3])
		return 0;
	xgpu_vulkan_image_upload(world_ao.stand_ins[XGPU_STREAM_WORLD_AO_OBJECTS], 0, 0, 0, 0, 0, 1, 1, 1, no_occlusion, 8, 8);
	xgpu_vulkan_image_upload(world_ao.stand_ins[XGPU_STREAM_WORLD_AO_SCENERY], 0, 0, 0, 0, 0, 1, 1, 1, &none, 1, 1);
	xgpu_vulkan_image_upload(world_ao.stand_ins[XGPU_STREAM_WORLD_AO_TABLE], 0, 0, 0, 0, 0, 1, 1, 1, outside_cell, 4, 4);
	xgpu_vulkan_image_upload(world_ao.stand_ins[XGPU_STREAM_WORLD_AO_ATLAS], 0, 0, 0, 0, 0, 1, 1, 1, &far_outside, 1, 1);
	memset(&sampler, 0, sizeof(sampler));
	sampler.minify = sampler.magnify = XGPU_VULKAN_FILTER_LINEAR;
	sampler.mip = XGPU_VULKAN_MIP_NONE;
	sampler.address[0] = sampler.address[1] = sampler.address[2] = XGPU_VULKAN_ADDRESS_CLAMP;
	sampler.anisotropy = 1;
	world_ao.sampler = xgpu_vulkan_sampler(&sampler);
	world_ao.number = 1;
	return world_ao.sampler != VK_NULL_HANDLE;
}

static void world_ao_part_release(struct world_ao_part *part)
{
	if (part->image)
		xgpu_vulkan_image_destroy(part->image);
	memset(part, 0, sizeof(*part));
}

void xgpu_vulkan_world_ao_field(uint32_t part_index, uint32_t width, uint32_t height, uint32_t depth, uint32_t z_first,
	uint32_t z_count, const void *data)
{
	struct world_ao_part *part;
	uint32_t texel = XGPU_STREAM_WORLD_AO_TEXEL(part_index);

	/* (none: every part let go of) */
	if (!width || !height || !depth || !data || part_index > XGPU_STREAM_WORLD_AO_OBJECTS)
	{
		world_ao_part_release(&world_ao.parts[0]);
		world_ao_part_release(&world_ao.parts[1]);
		world_ao_part_release(&world_ao.parts[2]);
		world_ao_part_release(&world_ao.parts[3]);
		world_ao.number++;
		return;
	}
	part = &world_ao.parts[part_index];
	/* (a new field's table comes first: the atlas before it is another's) */
	if (part_index == XGPU_STREAM_WORLD_AO_TABLE && z_first == 0)
		world_ao_part_release(&world_ao.parts[XGPU_STREAM_WORLD_AO_ATLAS]);
	/* (a new field's part: its image made as its first slices come) */
	if (!part->image || part->width != width || part->height != height || part->depth != depth || z_first == 0)
	{
		world_ao_part_release(part);
		part->image = world_ao_image(part_index, width, height, depth);
		part->width = width;
		part->height = height;
		part->depth = depth;
		world_ao.number++;
		if (!part->image)
			return;
	}
	/* (the pages a layer at a time) */
	if (part_index == XGPU_STREAM_WORLD_AO_SCENERY)
	{
		uint32_t layer;

		for (layer = 0; layer < z_count; layer++)
			xgpu_vulkan_image_upload(part->image, 0, z_first + layer, 0, 0, 0, width, height, 1,
				(const unsigned char *)data + (size_t)layer * width * height, width, width * height);
	}
	else
	{
		xgpu_vulkan_image_upload(part->image, 0, 0, 0, 0, z_first, width, height, z_count, data, width * texel,
			width * height * texel);
	}
	if (z_first + z_count >= depth && !part->whole)
	{
		part->whole = 1;
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

/* the field's part, where both are whole; else its stand-in */
static struct xgpu_vulkan_image *world_ao_part_image(uint32_t part)
{
	return world_ao.parts[0].image && world_ao.parts[0].whole && world_ao.parts[1].image && world_ao.parts[1].whole ?
		world_ao.parts[part].image : world_ao.stand_ins[part];
}

struct xgpu_vulkan_image *xgpu_vulkan_world_ao_field_image(void)
{
	return world_ao_part_image(XGPU_STREAM_WORLD_AO_ATLAS);
}

struct xgpu_vulkan_image *xgpu_vulkan_world_ao_table_image(void)
{
	return world_ao_part_image(XGPU_STREAM_WORLD_AO_TABLE);
}

struct xgpu_vulkan_image *xgpu_vulkan_world_ao_scenery_image(void)
{
	struct world_ao_part const *part = &world_ao.parts[XGPU_STREAM_WORLD_AO_SCENERY];

	return part->image && part->whole ? part->image : world_ao.stand_ins[XGPU_STREAM_WORLD_AO_SCENERY];
}

struct xgpu_vulkan_image *xgpu_vulkan_world_ao_objects_image(void)
{
	struct world_ao_part const *part = &world_ao.parts[XGPU_STREAM_WORLD_AO_OBJECTS];

	return part->image && part->whole ? part->image : world_ao.stand_ins[XGPU_STREAM_WORLD_AO_OBJECTS];
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
