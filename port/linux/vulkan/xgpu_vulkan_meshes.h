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
/* (after it) the meshes drawn lately still to come (xgpu_mesh_store_pending) */
uint32_t xgpu_vulkan_meshes_pending(void);
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
/* port: the same part's pieces at its mesh's least detailed level, skinned
for the traced scene alone (the frame's own draws keep the level drawn):
where the level drawn is refused (display.prism_rt_objects "auto"'s reach or
share), as the mesh store draws no stock part in a carrier's place to trace
instead. Returns the pieces, or -1 where there is none coarser (or none
resident yet) */
int xgpu_vulkan_meshes_rt_pieces_coarse(struct xgpu_vulkan_meshes_rt_piece *pieces, int most);
/* port: whether the resolver (xgpu_geometry.h) chooses the objects' levels
(0: HALO_GEOMETRY_RESOLVER=legacy, the choices before it) */
int xgpu_vulkan_meshes_resolved(void);
/* port: whether remodels are drawn now (the store holds a set with a mesh
it accepted): the frame's traced scene then leaves out the bake's static
scenery a mesh may draw (xgpu_vulkan_bake.c), as the game's
prism_bake_object_static does on the same frame */
int xgpu_vulkan_meshes_drawn(void);
/* the part drawn now traced as (XGPU_GEOMETRY_* variant, level): noted for
the resolver's statistics and log */
void xgpu_vulkan_meshes_note_traced(int variant, uint32_t lod);
/* the part drawn now is one the remodel draws (its instance's mesh in, the
part's region and permutation in the mesh) */
int xgpu_vulkan_meshes_part_remodelled(void);
/* the part drawn now drawn as its remodel (1) or stock after all (0: its
skinning could not be had), noted for the view or the shadow */
void xgpu_vulkan_meshes_note_drawn(int remodel);

#endif
