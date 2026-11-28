/*
PRISM_RELIEF_GEN.H

The relief of an environment shader's surface (docs/prism_materials.md, "The
relief tier"): the designs CE paints on a surface (Forerunner metal's
plates, grooves, inlays and glyphs) raised and sunk, as a normal map the
G-buffer pass draws in place of the vertices' normal, made on the player's
machine from the player's own bitmaps as the map loads (prism_relief.c),
kept in the data folder's cache and never shipped.

- Where the shader has a bump map with relief in it, its height is found
  again from its slopes (a Poisson solve on the tile, its edges wrapping),
  the designs' scale kept (a band of a twelfth of the tile and finer), its
  plateaus flattened (and the P8 palette's speckle with them: two medians)
  and its walls made steeper; the normal map is at the bump map's
  coordinates, its depth CE's bump map's own times PRISM_RELIEF_GAIN_EIGHTHS
  over 8. CE's bump maps carry the designs where they have any; the base
  map's painted cracks and mottling raised beside them were noise.
- Where it has none (or a flat one), the base map's design (its dark seams
  and grooves sunk, its light plates raised, edges kept: a median, then the
  same band and shaping), at the base map's coordinates, shallower; where
  the base map has no design either, no relief at all.

Every step is integer arithmetic in a fixed order (C89, no game headers, no
floating point, no platform maths), so every machine makes the same bytes
from the same bitmaps (tools/test_prism_relief.py pins them): the bake never
reads the relief (its texels' light keeps its direction, which the frame
reads at the relief's normal: docs/prism_bake.md, "The estimator"), but a
cache made on one machine, a test's or a farm's frame, is the same as
another's. PRISM_RELIEF_VERSION is in every cache file's key: bump it with
any change to what is made.
*/

#ifndef PRISM_RELIEF_GEN_H
#define PRISM_RELIEF_GEN_H

#include <stdint.h>

#define PRISM_RELIEF_VERSION 1u
/* the largest side made (a larger bitmap's first level no larger) */
#define PRISM_RELIEF_SIDE_MAXIMUM 1024u
/* the relief's depth over the bump map's own (the slopes CE's tool made from
its artist's height map), in eighths */
#define PRISM_RELIEF_GAIN_EIGHTHS 16

struct prism_relief_source
{
	/* the base map's first level (ARGB words, as xgpu_texture_argb_map
	decodes them), NULL none */
	const uint32_t *base;
	unsigned long base_width, base_height;
	/* CE's bump map's (x red, y green, z blue, 128 flat, y down the
	picture), NULL none */
	const uint32_t *bump;
	unsigned long bump_width, bump_height;
};

/* why a shader has no relief */
enum
{
	PRISM_RELIEF_MADE,
	PRISM_RELIEF_NO_DESIGN,
	PRISM_RELIEF_SIZE,
	PRISM_RELIEF_MEMORY
};

struct prism_relief
{
	/* the normal map's texels, RG bytes (x, y; 128 flat; CE's bump maps'
	convention), width x height of them (malloc's) */
	unsigned char *normal;
	unsigned long width, height;
	/* nonzero: at the bump map's coordinates; else the base map's */
	int at_bump;
	/* what it was made of (the log's): the bump map's relief's height,
	1/256ths of a texel from its lowest to its highest (2nd to 98th
	percentile), 0 none; the base map's design's range of luminance, 0 to
	255; and the depth drawn, 1/256ths of a texel */
	long bump_range, base_range, depth;
};

/* the relief of a shader's bitmaps; PRISM_RELIEF_MADE (and *relief's
normal set: prism_relief_free it), or why none */
int prism_relief_make(const struct prism_relief_source *source, struct prism_relief *relief);
void prism_relief_free(struct prism_relief *relief);

/* the normal map as a KTX2 file of BC5 (UNORM) blocks, every level (each
the box average of the one above), as the texture store reads it
(xgpu_ktx2.c); malloc's, its size in *size; NULL no memory */
unsigned char *prism_relief_ktx2(const unsigned char *normal, unsigned long width, unsigned long height,
	unsigned long *size);

/* the relief's key: PRISM_RELIEF_VERSION and every value it is made of
(FNV-1a, 64 bits) */
uint64_t prism_relief_key(const struct prism_relief_source *source);

#endif
