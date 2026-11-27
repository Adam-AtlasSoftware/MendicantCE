/*
PRISM_WORLD_SOURCE.H

Where the enhanced world's geometry comes from, and whether a map gets it
(v4_geopatch PLAN_FINAL.md sections 1.2, 2.5 and 3.4; M2).

The retail map always opens from maps/ unchanged. Our data for it is a
sidecar of our own beside it, <data root>/maps_enhanced/<map>.prsx, opened
and held for the whole map:
the remodel's render geometry (GEOM) and its lightmap vertices (LMUV) per
BSP, decoded at each BSP's load with every REF resolved from the map's own
tags (prism_geom.c). The .pwg files the remodel pipeline makes are its
intermediate on the farm (tools/geopatch/encode.py), never read here.

The map's state is chosen once (prism_world_source.c's choose, at its
first BSP's load, on the loading screen): World Geometry as it loads (a
change applies at the next load), and, at its first ask under Enhanced or
Path Traced lighting (the load, or the first change from Classic in the
map), its classic_world_id, its sidecar opened and verified and held for
the rest of the map whatever the lighting and the packs, and its enhanced
packs looked for. The ladder (prism_world_ladder) is then asked each frame
of that state and the frame's lighting, so that every change of lighting in
the middle of a map, in every direction, draws at once what a load under it
would:

  rung 1  Enhanced geometry (the sidecar): World Geometry Enhanced and the
          sidecar verified, and either the lighting Path Traced, or the
          lighting Enhanced with every BSP's enhanced pack present (a
          shipped bake whose key is that BSP's enhanced world_id). Path
          Traced's geometry never depends on a bake: it traces the enhanced
          world's own scene, made by the renderer with or without a pack
          (and again, never held from another world's bake, when the
          geometry changes in play: xgpu_vulkan_bake.c's
          XGPU_BAKE_FLAG_PATH_TRACED), lit by the bake's light where it has
          it and by its own bounce else;
  rung 2  Classic geometry, lit by Enhanced or Path Traced from the classic
          packs: rung 1 not available (World Geometry Classic, no sidecar or
          a refused one, or Enhanced lighting without the enhanced packs);
  rung 3  Classic geometry and Classic lighting: display.lighting classic.

Classic lighting never draws enhanced geometry (Classic's lightmaps on the
remodel are the mismatch PLAN_FINAL calls the rock bug), and a map loaded
under it opens no sidecar until the lighting changes. Geometry and light
never disagree: a pack is used only for the world_id it was baked from, and
the world_id is the sidecar's actually opened, never a setting's or a
manifest's (prism_identity.h).

debug.world_unbaked (HALO_WORLD_UNBAKED) takes rung 1 without the packs:
the farm's and the developers' (a bake to make, the geometry's checks),
never a player's. With no bake to light it, CE's lightmap passes draw the
enhanced world flat (rasterizer_xbox_environment.c,
prism_world_geometry_own_page_draw): never the BSP's stock lightmap read at
the enhanced world's own coordinates.

A pack the ladder found but the renderer does not load whole (its own key
not the BSP's enhanced world_id though its manifest names it, a damaged
file) drops Enhanced lighting to the classic geometry the moment the
renderer says so (prism_world_source_bake_refused), with a warning: a
player is never left with enhanced geometry and no light of its own. Path
Traced, which needs no pack, still draws the enhanced geometry then.
*/

#ifndef PRISM_WORLD_SOURCE_H
#define PRISM_WORLD_SOURCE_H

#include "cseries.h"

struct structure_bsp;

enum prism_world_rung
{
	PRISM_WORLD_RUNG_NONE,
	PRISM_WORLD_RUNG_ENHANCED,
	PRISM_WORLD_RUNG_CLASSIC_LIT,
	PRISM_WORLD_RUNG_CLASSIC,
};

/* the rung drawn now: the ladder of the map's state and this frame's
lighting (logged as it changes) */
enum prism_world_rung prism_world_rung(void);

/* whether the enhanced geometry is drawn now: prism_world_rung is rung 1 */
boolean prism_world_enhanced(void);

/* the BSP's remodel image (prism_geom.h: the PWG1 layout
prism_world_geometry.c reads) decoded from the sidecar: TRUE with *image
(the caller frees it with free) and *size, or FALSE (the log says why;
the sidecar is then refused for the rest of the session's map) */
boolean prism_world_source_image(struct structure_bsp *bsp, short bsp_index, char const *bsp_name,
	unsigned char **image, unsigned long *size);

/* the BSP's world_id (prism_identity.h) as the bake keys it: the enhanced
one while prism_world_enhanced, else the classic one where the map's
classic_world_id is known (the shipped manifests' "world" line for the
player's base_id, until M7's signed manifest; the farm's
HALO_CLASSIC_WORLD_ID), else all zeros (the bake keyed by the map's
checksum) */
void prism_world_source_world_id(short bsp_index, unsigned char id[32]);

/* the world_id's first 16 hex digits and the sidecar's name for the log and
the bake's note ("" for the classic world) */
char const *prism_world_source_label(short bsp_index);

/* the renderer's verdict on the BSP's enhanced bake (prism_bake.c, each
frame, from the host's status for this BSP and scene): its pack not loaded
whole (a pack the manifest names whose own key is not this BSP's enhanced
world_id, a damaged one, one the renderer cannot use) while the map is drawn
with its enhanced geometry under Enhanced lighting. Enhanced lighting then
draws the classic geometry for the rest of the map (rung 2), with a warning
in the log, lit by its classic packs where it has them. (Never under
debug.world_unbaked, whose rung 1 has no packs by design, nor under Path
Traced, which needs none) */
void prism_world_source_bake_refused(short bsp_index);

/* display.world_geometry asks for the enhanced geometry */
boolean prism_world_geometry_wanted(void);

/* the map's tags are about to go: the sidecar closed, the choice forgotten */
void prism_world_source_map_unloaded(void);

#endif
