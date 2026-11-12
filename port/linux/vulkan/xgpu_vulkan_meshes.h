/*
XGPU_VULKAN_MESHES.H

PRISM's mesh store in the Vulkan backend (xgpu_vulkan_meshes.c): the portable
core (xgpu_mesh_store.h) over the Vulkan core's mesh buffers and skinning,
the stream's MESH_STORE_MAP, MESH_INSTANCE and MESH_DRAW, and the swap of a
model part's draw for the mesh's pieces. Render thread.
*/

#ifndef __HALO_LINUX_XGPU_VULKAN_MESHES_H
#define __HALO_LINUX_XGPU_VULKAN_MESHES_H

#include <stdint.h>

#include "xgpu_stream.h"
#include "xgpu_vulkan.h"

/* the store can be had here (the stream's capability): not on Windows,
whose guest paths are not opened by the host yet, nor where
HALO_MESH_STORE_OFF is set */
int xgpu_vulkan_meshes_available(void);
/* MESH_STORE_MAP, MESH_INSTANCE, MESH_DRAW (host_render_stream.h's) */
void xgpu_vulkan_meshes_map(void *context, const struct xgpu_stream_mesh_store_map *record,
	const struct xgpu_stream_mesh_store_query *queries, const char *strings,
	struct xgpu_stream_mesh_store_answer *answers);
void xgpu_vulkan_meshes_instance(void *context, const struct xgpu_stream_mesh_instance *record,
	const float (*matrices)[3][4]);
void xgpu_vulkan_meshes_draw_part(void *context, const struct xgpu_stream_mesh_draw *record);
/* a record that changes a draw's state (constants, textures, pipeline,
dynamic state) came: the next draw of a part is a pass of its own, not
the rest of one drawn in pieces */
void xgpu_vulkan_meshes_state_changed(void);

/* what the draw that follows is, by the part it is of (MESH_DRAW):
XGPU_VULKAN_MESHES_STOCK drawn as it is; _SKIP not drawn (a part the mesh
replaces that is not its shader's carrier, or the rest of a part drawn
already); _SWAP drawn as the mesh's pieces (xgpu_vulkan_meshes_prepare, then
xgpu_vulkan_meshes_piece) */
enum
{
	XGPU_VULKAN_MESHES_STOCK,
	XGPU_VULKAN_MESHES_SKIP,
	XGPU_VULKAN_MESHES_SWAP,
};
/* the draw's verdict: its pipeline's vertex layout must be the stock
compressed model vertex's (one stream of 32 bytes) for a swap, its
primitives triangles */
int xgpu_vulkan_meshes_verdict(const struct xgpu_vulkan_pipeline_description *pipeline, uint32_t draw_bindings,
	int triangles);
/* the swap's skinned vertices and constants (the draw's, node 0's matrix
the identity) in the submission being recorded, and with wide their wide
copy too (a draw with a material, by its wide pipeline: the pieces then
bind it in place of the stock layout's; the mesh is skinned wide from then
on, so its other draws share the one skinning): 0 if they cannot be (then
drawn stock) */
int xgpu_vulkan_meshes_prepare(const float (*constants)[4], uint32_t constant_count, int wide);
/* the swap's pieces: the draw of piece index (from 0) made from the stock
draw; 0 once there are no more */
int xgpu_vulkan_meshes_piece(uint32_t index, struct xgpu_vulkan_draw *draw);
/* the frame's start: the store's copies and budget */
void xgpu_vulkan_meshes_frame(void);
/* a line of the store's statistics (debug.gpu_stats), into text */
int xgpu_vulkan_meshes_statistics(char *text, unsigned long size);

/* display.prism_rt: the pieces of the part being drawn, for a pre-pass
twin's capture (xgpu_vulkan_stream.c's prepass_rt_capture), where a
replacement mesh draws it: its instance's level skinned in the submission
being recorded (shared with the frame's own draw of it), each piece's
skinned vertices (the stock layout's 32 bytes, in the world) and its 32-bit
indices, a triangle list, each index plus index_add the vertex; identity
names the piece from one frame to the next (the instance, the mesh's level
and the piece), pose changes as its bones move, origin is the instance's
node 0. Returns the pieces (at most most), 0 where the part is a carrier's
(the carrier's pieces stand for it) or its pieces were captured already in
the window (window: the pre-passes' count), -1 where the part is drawn
stock (captured as drawn) */
#define XGPU_VULKAN_MESHES_RT_PIECES 16
struct xgpu_vulkan_meshes_rt_piece
{
	VkBuffer vertices;
	uint64_t vertex_offset;
	uint32_t vertex_count;
	VkBuffer indices;
	uint32_t index_first, index_count;
	int32_t index_add;
	uint64_t identity[2];
	uint64_t pose;
	float origin[3];
};
int xgpu_vulkan_meshes_rt_pieces(uint32_t window, struct xgpu_vulkan_meshes_rt_piece *pieces, int most);

#endif
