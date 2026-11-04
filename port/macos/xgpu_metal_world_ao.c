/*
XGPU_METAL_WORLD_AO.C

display.ambient_occlusion "world"'s resources in the Metal backend
(docs/world-ao.md, "GPU binding layout"), as the Vulkan backend has them
(port/linux/vulkan/xgpu_vulkan_world_ao.c): the structure's distance field,
a 3D texture of signed bytes (MTLPixelFormatR8Snorm) that the shaders read
through a constexpr linear sampler clamped to its edge, at
XGPU_METAL_TEXTURE_WORLD_SDF; and the window's block (struct
xgpu_world_ao_block) at XGPU_METAL_BUFFER_WORLD_AO (xgpu_metal_set_world_ao).

The field arrives a few slices at a time (the command stream's
WORLD_AO_FIELD records): its texture is made as its first slices come and
bound once its last have; until then, and with none, a 1x1x1 stand-in of
+127 (far outside: no occlusion). The block arrives with each window
(WORLD_AO_WINDOW) and is copied into the frame's upload ring; with none, a
block of zeros of the backend's own.
*/

#include "xgpu_metal.h"
#include "xgpu_metal_bindings.h"
#include "xgpu_stream.h"

#include <string.h>

static struct
{
	/* the field's texture (0 none), its size, whether its last slices
	have come; the stand-in, and the block of zeros */
	uint32_t field;
	uint32_t width, height, depth;
	int whole;
	uint32_t stand_in;
	uint32_t zeros;
	struct xgpu_metal_source block;
	int made;
} world_ao;

static uint32_t world_ao_texture(uint32_t width, uint32_t height, uint32_t depth)
{
	struct xgpu_metal_texture_description description;

	memset(&description, 0, sizeof(description));
	description.type = XGPU_METAL_TEXTURE_3D;
	description.format = XGPU_METAL_FORMAT_R8_SNORM;
	description.usage = XGPU_METAL_USAGE_SAMPLED;
	description.width = (uint16_t)width;
	description.height = (uint16_t)height;
	description.depth_or_layers = (uint16_t)depth;
	description.levels = 1;
	return xgpu_metal_texture_create(&description);
}

/* the stand-in and the zeros, made the first time */
static void world_ao_make(void)
{
	static const signed char far_outside = 127;

	if (world_ao.made)
		return;
	world_ao.made = 1;
	world_ao.stand_in = world_ao_texture(1, 1, 1);
	if (world_ao.stand_in)
		xgpu_metal_texture_upload(world_ao.stand_in, 0, 0, 0, 0, 0, 1, 1, 1, &far_outside, 1, 1);
	world_ao.zeros = xgpu_metal_buffer_create(sizeof(struct xgpu_world_ao_block));
	if (world_ao.zeros)
	{
		static const struct xgpu_world_ao_block none;

		xgpu_metal_buffer_write(world_ao.zeros, 0, &none, sizeof(none), 1);
	}
	world_ao.block.kind = XGPU_METAL_SOURCE_BUFFER;
	world_ao.block.handle = world_ao.zeros;
	world_ao.block.offset = 0;
	world_ao.block.size = world_ao.zeros ? sizeof(struct xgpu_world_ao_block) : 0;
	if (!world_ao.zeros)
		world_ao.block.kind = XGPU_METAL_SOURCE_NONE;
}

static void world_ao_bind(void)
{
	xgpu_metal_set_world_ao(world_ao.field && world_ao.whole ? world_ao.field : world_ao.stand_in, world_ao.block);
}

void xgpu_metal_world_ao_field(uint32_t width, uint32_t height, uint32_t depth, uint32_t z_first, uint32_t z_count,
	const void *voxels)
{
	world_ao_make();
	/* (none: let go of) */
	if (!width || !height || !depth || !voxels)
	{
		if (world_ao.field)
			xgpu_metal_texture_destroy(world_ao.field);
		world_ao.field = 0;
		world_ao.whole = 0;
		world_ao_bind();
		return;
	}
	/* (a new field: its texture made as its first slices come) */
	if (!world_ao.field || world_ao.width != width || world_ao.height != height || world_ao.depth != depth ||
		z_first == 0)
	{
		if (world_ao.field)
			xgpu_metal_texture_destroy(world_ao.field);
		world_ao.field = world_ao_texture(width, height, depth);
		world_ao.width = width;
		world_ao.height = height;
		world_ao.depth = depth;
		world_ao.whole = 0;
		world_ao_bind();
		if (!world_ao.field)
			return;
	}
	xgpu_metal_texture_upload(world_ao.field, 0, 0, 0, 0, z_first, width, height, z_count, voxels, width,
		width * height);
	if (z_first + z_count >= depth && !world_ao.whole)
	{
		world_ao.whole = 1;
		world_ao_bind();
	}
}

void xgpu_metal_world_ao_window(const struct xgpu_world_ao_block *block)
{
	world_ao_make();
	if (block)
	{
		world_ao.block = xgpu_metal_stream(block, sizeof(*block));
	}
	else
	{
		world_ao.block.kind = world_ao.zeros ? XGPU_METAL_SOURCE_BUFFER : XGPU_METAL_SOURCE_NONE;
		world_ao.block.handle = world_ao.zeros;
		world_ao.block.offset = 0;
		world_ao.block.size = world_ao.zeros ? sizeof(*block) : 0;
	}
	world_ao_bind();
}
