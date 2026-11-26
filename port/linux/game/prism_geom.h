/*
PRISM_GEOM.H

A BSP's enhanced render geometry as the sidecar carries it (its GEOM v1 and
LMUV v1 sections: tools/geopatch/geom.py has both layouts), decoded into the
remodel image prism_world_geometry.c draws: the PWG1 version 2 layout its
reader has always read (that file's header comment), built in memory, never
written to disk.

Every REF op is resolved here from the player's own map, through the
caller's view of the BSP's stock materials and collision vertices
(struct prism_geom_stock), with the exact integer decode of
prism_ref_decode.c, so a referenced value is the retail value bit for bit
and the sidecar carries none. A LIT vertex is the sidecar's own: its grid
position (prsx_grid_bits), its packed normal, binormal and tangent
(prsx_decode_vector_bits) and its base map coordinates (float bits). A
lightmap vertex (LMUV) is the stock vertex's (REF) or ours, in the game's
compressed form (a packed incident radiosity and two 16-bit lightmap
coordinates), decoded by prsx_decode_lightmap_vertex.

The game compresses the image's vertices again as it loads them
(rasterizer_geometry_compress_vertices); the decode is exact and the
compression's floor of (2 s + 1) / d times d / 2 gives s back, so the GPU
draws the sidecar's packed values unchanged.

C89 with no game headers, so the farm's tools and the tests build it as it
is (tools/geopatch/native.py). The sections are untrusted: every count,
offset and index is checked before it is used.

LMUV v1 (little endian):
  "LMU1", u32 version (1), u32 part count (GEOM's), u32 column count
  per part: u32 vertex count (its GEOM part's)
  column table and columns as GEOM's (fourcc, u32 element width, u32 count,
  u32 byte size; byte-plane shuffled by the width; unknown columns ignored):
    LOPS u8   per vertex: 0 LIT, 1 REF
    LREF u32  per REF vertex: the stock lightmap vertex's index in the
              part's own material
    LVTX u32  per LIT vertex, two words: the packed incident radiosity, and
              the lightmap u (low 16 bits) and v (high 16 bits)
*/

#ifndef PRISM_GEOM_H
#define PRISM_GEOM_H

#include <stddef.h>
#include <stdint.h>

enum
{
	PRISM_GEOM_PWG_VERSION = 2,
	PRISM_GEOM_PWG_HEADER_SIZE = 64,
	PRISM_GEOM_PWG_RECORD_SIZE = 64,
	PRISM_GEOM_MAXIMUM_PARTS = 1 << 16,
	/* (the image's 16-bit triangle indices) */
	PRISM_GEOM_MAXIMUM_PART_VERTICES = 0xFFFF,
	PRISM_GEOM_NO_INDEX = 0xFFFF,
};

/* the player's BSP, as the decode reads it */
struct prism_geom_stock
{
	void *context;
	/* a stock material of the BSP (its lightmap and material index): 1 with
	its vertex count, its compressed environment vertices (32 bytes each)
	and its compressed lightmap vertices (8 bytes each; NULL: none), or 0
	if the BSP has no such material */
	int (*material)(void *context, uint32_t lightmap_index, uint32_t material_index, uint32_t *count,
		const unsigned char **vertices, const unsigned char **lightmap_vertices);
	/* the BSP's collision BSP's vertex index as three float bits: 1, or 0
	if there is no such vertex */
	int (*collision_vertex)(void *context, uint32_t index, uint32_t bits[3]);
};

enum prism_geom_result
{
	PRISM_GEOM_OK,
	PRISM_GEOM_ERROR_LAYOUT,
	PRISM_GEOM_ERROR_REFERENCE,
	PRISM_GEOM_ERROR_LIGHTMAP,
	PRISM_GEOM_ERROR_MEMORY,
};

struct prism_geom_report
{
	uint32_t parts;
	/* parts left out: of a PRISM material (no stock pair the renderer can
	draw it with yet), or with more vertices than 16-bit indices reach */
	uint32_t parts_skipped;
	uint32_t vertices;
	uint32_t triangles;
	/* vertices by op (geom.py's numbers: LIT, REF, REFP, REFM, REFC; the
	STOCK_TEX forms counted with their base op) */
	uint32_t by_op[5];
	/* the far level kept (every drawn part has one) */
	int far;
	/* the part and vertex an error was found at */
	uint32_t error_part, error_vertex;
};

const char *prism_geom_result_string(enum prism_geom_result result);

/* GEOM (geom_size bytes) and its LMUV decoded against stock into a PWG1
version 2 image: PRISM_GEOM_OK with *image (malloc'd, the caller frees it)
and *image_size set, or an error and nothing allocated. name goes in the
image's header (informational, at most 47 bytes kept) */
enum prism_geom_result prism_geom_decode(const unsigned char *geom, size_t geom_size, const unsigned char *lmuv,
	size_t lmuv_size, const struct prism_geom_stock *stock, const char *name, unsigned char **image,
	size_t *image_size, struct prism_geom_report *report);

#endif
