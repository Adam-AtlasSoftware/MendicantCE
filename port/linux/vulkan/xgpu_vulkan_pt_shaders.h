/*
XGPU_VULKAN_PT_SHADERS.H

PRISM Path Traced's compute shaders as SPIR-V (xgpu_vulkan_pt.c): made by
tools/vulkan_pt_shaders.py from xgpu_vulkan_pt.glsl and the baker's
BAKE_COMMON (xgpu_vulkan_bake_glsl.inc).
*/

#ifndef __HALO_LINUX_XGPU_VULKAN_PT_SHADERS_H
#define __HALO_LINUX_XGPU_VULKAN_PT_SHADERS_H

#include <stddef.h>
#include <stdint.h>

extern const uint32_t xgpu_vulkan_pt_light_words[];
extern const size_t xgpu_vulkan_pt_light_count;
extern const uint32_t xgpu_vulkan_pt_temporal_words[];
extern const size_t xgpu_vulkan_pt_temporal_count;
extern const uint32_t xgpu_vulkan_pt_fix_words[];
extern const size_t xgpu_vulkan_pt_fix_count;
extern const uint32_t xgpu_vulkan_pt_atrous_words[];
extern const size_t xgpu_vulkan_pt_atrous_count;
extern const uint32_t xgpu_vulkan_pt_composite_words[];
extern const size_t xgpu_vulkan_pt_composite_count;
extern const uint32_t xgpu_vulkan_pt_reference_words[];
extern const size_t xgpu_vulkan_pt_reference_count;
extern const uint32_t xgpu_vulkan_pt_reference_stats_words[];
extern const size_t xgpu_vulkan_pt_reference_stats_count;
extern const uint32_t xgpu_vulkan_pt_lamps_words[];
extern const size_t xgpu_vulkan_pt_lamps_count;

#endif
