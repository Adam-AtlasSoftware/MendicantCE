/*
XGPU_VULKAN_STORE.H

PRISM's texture store in the Vulkan backend (xgpu_vulkan_store.c): the
portable core (xgpu_texture_store.h) over the Vulkan core's images, a
staging buffer of its own and the device's memory budget. Render thread.
*/

#ifndef __HALO_LINUX_XGPU_VULKAN_STORE_H
#define __HALO_LINUX_XGPU_VULKAN_STORE_H

#include <stdint.h>

#include "xgpu_stream.h"
#include "xgpu_texture_store.h"

/* the store can be had here (the stream's capability): not on Windows,
whose guest paths are not opened by the host yet */
int xgpu_vulkan_store_available(void);
/* TEXTURE_STORE_MAP (host_render_stream.h's texture_store_map) */
void xgpu_vulkan_store_map(void *context, const struct xgpu_stream_texture_store_map *record,
	const struct xgpu_stream_texture_store_query *queries, const char *strings,
	struct xgpu_stream_texture_store_answer *answers);
/* the frame's start: the store's uploads, its budget and work */
void xgpu_vulkan_store_frame(void);
/* an id drawn this frame: 1 and its view where it is usable (drawn stock
otherwise) */
int xgpu_vulkan_store_use(uint32_t id, struct xgpu_texture_store_view *view);
/* changes whenever what an id draws may have (xgpu_texture_store_epoch) */
uint32_t xgpu_vulkan_store_epoch(void);
/* a line of the store's statistics (debug.gpu_stats), into text */
int xgpu_vulkan_store_statistics(char *text, unsigned long size);
/* the store's image's format of a replacement's (XGPU_KTX2_FORMAT_*) */
uint8_t xgpu_vulkan_store_format(uint8_t format);

#endif
