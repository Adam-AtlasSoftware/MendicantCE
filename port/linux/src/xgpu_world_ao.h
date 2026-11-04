/*
XGPU_WORLD_AO.H

display.ambient_occlusion "world" (docs/world-ao.md, #578) as the GPU sees
it, alike in the guest's front end, the command stream and the hosts'
backends (plain floats and fixed-width integers: xgpu_stream.h's rules).

- The field: one 3D texture of the current structure BSP's signed distance
  (world_sdf.h), R8 signed normalized (VK_FORMAT_R8_SNORM,
  MTLPixelFormatR8Snorm), linear filtering, clamped to its edge; a texel's
  value times the block's range is the distance in world units at the
  voxel's centre.
- The block: the field's place and the window's proxies (world_proxies.h's
  struct world_proxy), a uniform buffer (std140: vec4s only).

Bindings (the shaders: workstream D; the backends' descriptors: B the
field, C the block):
- SPIR-V: XGPU_SPIRV_SET_OCCLUSION (#574's set, so that a draw still binds
  at most four sets), bindings XGPU_SPIRV_BINDING_WORLD_SDF and
  XGPU_SPIRV_BINDING_WORLD_AO (xgpu_spirv.h);
- MSL: [[texture(XGPU_MSL_TEXTURE_WORLD_SDF)]] with a constexpr sampler in
  the shader, [[buffer(XGPU_MSL_BUFFER_WORLD_AO)]] (xgpu_msl.h).
*/

#ifndef __HALO_LINUX_XGPU_WORLD_AO_H
#define __HALO_LINUX_XGPU_WORLD_AO_H

#define XGPU_WORLD_AO_PROXIES_MAXIMUM 64
/* a proxy's kind (its [0].w) */
#define XGPU_WORLD_AO_PROXY_SPHERE 1
#define XGPU_WORLD_AO_PROXY_CAPSULE 2
#define XGPU_WORLD_AO_PROXY_BOX 3

/* the uniform block: [0] the field's origin (its first voxel's centre) and
one over the voxel's size; [1] its width, height, depth (in voxels) and
range (world units); [2] the strength on objects, the strength on the level,
the field's fade (0 none yet: the proxies alone), the proxies' count; [3]
the reference's weight (how much of the occlusion at the object's lighting
point its ambient sample already holds) and three spare; then the
proxies, three vec4s each (world_proxies.h's struct world_proxy) */
struct xgpu_world_ao_block
{
	float field[2][4];
	float parameters[2][4];
	float proxies[XGPU_WORLD_AO_PROXIES_MAXIMUM][3][4];
};

#endif
