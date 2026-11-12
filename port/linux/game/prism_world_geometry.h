/*
PRISM_WORLD_GEOMETRY.H

PRISM v2's remodelled level geometry, drawn in place of the structure BSP's
own (display.prism_world_geometry): prism_world_geometry.c.
*/

#ifndef __PRISM_WORLD_GEOMETRY_H
#define __PRISM_WORLD_GEOMETRY_H
#pragma once

struct vertex_buffer;

/* once a frame, before the structure's surfaces are drawn
(structure_render_preprocess): reads the setting, and takes up the
remodel of the BSP the frame draws when it is on */
void prism_world_geometry_frame(void);

/* the vertices a material's chunk of visible surfaces is drawn from
(structure_render_pass): `stock`, the material's own, or the remodel's,
whose draws (prism_world_geometry_draw) draw the remodel's triangles of
just those surfaces. `surface_indices` ascend */
struct vertex_buffer const *prism_world_geometry_chunk(
	short lightmap_index,
	short material_index,
	long const *surface_indices,
	long surface_count,
	struct vertex_buffer const *stock);

/* TRUE when the draw is the remodel's (the vertices of the last chunk
prism_world_geometry_chunk gave): drawn here, from its own triangles, and
not from the frame's dynamic ones (rasterizer_xbox_draw_primitives.c) */
int prism_world_geometry_draw(
	struct vertex_buffer const *vertex_buffer0,
	struct vertex_buffer const *vertex_buffer1);

/* the map's tags are about to go: everything made of them freed */
void prism_world_geometry_map_unloaded(void);

/* whether the remodel is drawn for the BSP now (structure_culling.c then
skips no surface for facing away) */
int prism_world_geometry_drawn(void);

#endif /* __PRISM_WORLD_GEOMETRY_H */
