/*
XGPU_METAL_BINDINGS.H

Where the Metal backend (xgpu_metal.m) binds what a draw's shaders read: the
argument table indices of the MSL emitter's (port/linux/src/xgpu_msl.h)
[[buffer(n)]], [[texture(n)]] and [[sampler(n)]] attributes, and of the
vertex descriptor's buffers. Plain #defines, usable from C, Objective-C and
MSL. xgpu_metal_shaders.c checks that they are the emitter's.
*/

#ifndef __HALO_MACOS_XGPU_METAL_BINDINGS_H
#define __HALO_MACOS_XGPU_METAL_BINDINGS_H

/* the vertex streams: stream n is buffer XGPU_METAL_BUFFER_STREAM(n), and
attribute n of the vertex descriptor is [[attribute(n)]] */
#define XGPU_METAL_STREAM_COUNT 16
#define XGPU_METAL_BUFFER_STREAM(stream) (stream)
/* the vertex constants, 192 float4 (3 KB: setVertexBytes;
XGPU_MSL_BUFFER_CONSTANTS) */
#define XGPU_METAL_BUFFER_VERTEX_CONSTANTS 16
#define XGPU_METAL_VERTEX_CONSTANT_COUNT 192
/* the draw's other uniforms (the emitter's struct xgpu_msl_uniforms), bound
to both stages (setVertexBytes, setFragmentBytes; XGPU_MSL_BUFFER_UNIFORMS) */
#define XGPU_METAL_BUFFER_DRAW_UNIFORMS 17
/* the enhanced dynamic lights' block and their grid's cells, for the pixel
shaders that read them (xgpu_metal_set_dynamic_lights;
XGPU_MSL_BUFFER_DYNAMIC_LIGHTS, _DYNAMIC_LIGHT_CELLS) */
#define XGPU_METAL_BUFFER_DYNAMIC_LIGHTS 18
#define XGPU_METAL_BUFFER_DYNAMIC_LIGHT_CELLS 22
/* (19: the emitter's sample counter, which the macOS app does not use) */
/* the values of the inputs no stream feeds, 16 float4 (setVertexBytes),
read by the vertex descriptor at a constant step: the shaders read every
input as [[attribute(n)]] whether a stream feeds it or not */
#define XGPU_METAL_BUFFER_CONSTANT_ATTRIBUTES 20
/* (the VR mode's uniforms, should the macOS app ever draw both eyes) */
#define XGPU_METAL_BUFFER_VIEW_UNIFORMS 21

/* texture stage n samples texture n with sampler n; past the game's four,
display.ambient_occlusion's occlusion, which the draws that light by it
read (XGPU_MSL_TEXTURE_AMBIENT_OCCLUSION) */
#define XGPU_METAL_TEXTURE_STAGE(stage) (stage)
#define XGPU_METAL_SAMPLER_STAGE(stage) (stage)
#define XGPU_METAL_TEXTURE_AMBIENT_OCCLUSION 4
#define XGPU_METAL_TEXTURE_STAGE_COUNT 5
/* display.ambient_occlusion "world" (port/linux/src/xgpu_stream.h): the
field past the stages (XGPU_MSL_TEXTURE_WORLD_SDF; workstream D raises
XGPU_METAL_TEXTURE_STAGE_COUNT when it binds it) and the window's block
(XGPU_MSL_BUFFER_WORLD_AO) */
#define XGPU_METAL_TEXTURE_WORLD_SDF 5
#define XGPU_METAL_BUFFER_WORLD_AO 23

#endif
