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

prism_world_choose's ladder, decided once a map (at its first BSP's load,
on the loading screen) and latched for every BSP of it:

  rung 1  Enhanced geometry (the sidecar), lit by Enhanced or Path Traced
          from the enhanced packs: the sidecar opened and verified, and every
          BSP's enhanced pack present (a shipped bake whose key is that BSP's
          enhanced world_id);
  rung 2  Classic geometry, lit by Enhanced or Path Traced from the classic
          packs: rung 1 not available;
  rung 3  Classic geometry and Classic lighting: display.lighting classic.

Classic lighting never opens a sidecar and never draws enhanced geometry
(Classic's lightmaps on the remodel are the mismatch PLAN_FINAL calls the
rock bug): a change of lighting to Classic in the middle of a map draws the
map's own geometry at once, and the enhanced again when Enhanced or Path
Traced is chosen back. A change of World Geometry applies at the next map
load. Geometry and light never disagree: a pack is used only for the
world_id it was baked from, and the world_id is the sidecar's actually
opened, never a setting's or a manifest's (prism_identity.h).

debug.world_unbaked (HALO_WORLD_UNBAKED) takes rung 1 without the packs:
the farm's and the developers' (a bake to make, the geometry's checks),
never a player's.
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

/* the map's rung, chosen at its first ask (prism_world_choose) */
enum prism_world_rung prism_world_rung(void);

/* whether the enhanced geometry is drawn now: the map latched to rung 1 and
the lighting Enhanced or Path Traced */
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

/* display.world_geometry asks for the enhanced geometry */
boolean prism_world_geometry_wanted(void);

/* the map's tags are about to go: the sidecar closed, the choice forgotten */
void prism_world_source_map_unloaded(void);

#endif
