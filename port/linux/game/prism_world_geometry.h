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
/* port: the BSP's remodel taken up or let go of for this frame (by the
frame's first reader, SCENE_STATIC's, before the structure is drawn); TRUE
where display.prism_world_geometry asks for it */
boolean prism_world_geometry_prepare(void);

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

/* display.prism_rt (prism_rt.c): the remodel's triangles of a BSP
material (all its parts'), in the world, nine floats each, for the ray
tracer's structure: its near level's (far_level 0) or its far level's,
written to `to` (at most `most`; NULL: counted). -1 where the material has
no remodel (or none is loaded) */
long prism_world_geometry_triangles(
	short lightmap_index,
	short material_index,
	int far_level,
	float *to,
	long most);
/* PRISM v4's SCENE_STATIC (prism_scene_static.c): a remodelled surface's
triangles of a level (PRISM_WORLD_GEOMETRY_LEVEL_*; `surface` its index
among its material's), at most `most`, each three corners' places (9
floats), base map coordinates and lightmap coordinates (6 floats each;
positions NULL: only counted), each wound to its vertices' normals (its
corners' cross product out of the level);
how many (0: the far level is none apart from the near), -1 where the
material has no remodel */
enum
{
	PRISM_WORLD_GEOMETRY_LEVEL_NEAR,
	PRISM_WORLD_GEOMETRY_LEVEL_FAR
};
long prism_world_geometry_surface_triangles(
	short lightmap_index,
	short material_index,
	long surface,
	int level,
	float *positions,
	float *base,
	float *lightmap,
	long most);
/* ... from its skip-th triangle of that level on (a surface past a slice of
the scene sent in parts) */
long prism_world_geometry_surface_triangles_from(
	short lightmap_index,
	short material_index,
	long surface,
	int level,
	float *positions,
	float *base,
	float *lightmap,
	long skip,
	long most);
/* ... the level the surface is drawn at for this frame's camera (1 far, 0
near: the rule the drawn surfaces follow, off screen too), -1 where it has
no far level */
int prism_world_geometry_surface_level(
	short lightmap_index,
	short material_index,
	long surface);

/* the map's tags are about to go: everything made of them freed */
void prism_world_geometry_map_unloaded(void);

/* whether the remodel is drawn for the BSP now (structure_culling.c then
skips no surface for facing away) */
int prism_world_geometry_drawn(void);

/* (v4 geopatch M4) the enhanced world's own lightmap pages (LMUV v2, our
layout): how many the loaded remodel has (0: none, its lightmap coordinates
are CE's), each one's width and height written to sizes (at most most).
They follow the BSP's own lightmap pages: own page k is page (the BSP's
lightmap bitmap count + k) in SCENE_STATIC's pages and the G-buffer's */
long prism_world_geometry_own_pages(unsigned long (*sizes)[2], long most);
/* a remodelled material's own page (its index among the own pages), or
NONE: it is not drawn from the remodel, or its coordinates are CE's */
short prism_world_geometry_material_page(short lightmap_index, short material_index);
/* whether the draw of vertex_buffer (a pass's vertices, from
prism_world_geometry_chunk) is a remodelled material on the enhanced
world's own page: its lightmap coordinates are on that page, never on the
BSP's stock lightmap bitmap the pass has bound, so CE's lightmap passes
(rasterizer_xbox_environment.c) must not read that bitmap at them */
int prism_world_geometry_own_page_draw(struct vertex_buffer const *vertex_buffer);

#endif /* __PRISM_WORLD_GEOMETRY_H */
