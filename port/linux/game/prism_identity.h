/*
PRISM_IDENTITY.H

The three identities of the enhanced world (v4_geopatch PLAN_FINAL.md
section 2.5), none circular:

- base_id: the SHA-256 of a retail map's inflated body (the bytes after its
  0x800-byte header, as its zlib stream inflates). It names the player's
  file, so a recompressed copy of the same map has the same one. Computed
  while a slot is unpacked (map_unpack.c) or from the slot when that did
  not (prism_base_id_of_file), and kept in the slot's source record keyed by
  the map file's path, size and modification time (xbox_files.c).
- classic_world_id: the farm's hash of a map version's bake inputs; the
  signed manifest maps each supported base_id to it and the sidecar's header
  carries it. Two versions with equal inputs share it.
- world_id[variant][bsp], what a bake depends on, per BSP:
    classic:  H("PRSX-WORLD-C" || classic_world_id || bsp || SCENE_VERSION)
    enhanced: H("PRSX-WORLD-E" || classic_world_id || bsp || SCENE_VERSION ||
                RENDER_FORMAT || each BAKE_RELEVANT section's sha256_decoded
                for that BSP (its own and the all-BSP ones), in directory order,
                which prsx_open has made canonical: sorted, each key once)
  bsp as u16, versions as u32, little endian; H = SHA-256. The engine
  computes the enhanced one from the directory of the sidecar it opened,
  never from a setting or the manifest. Collision (sim), runtime sections,
  the format number and the file's own SHA never enter it, so a collision
  change or a new runtime section never re-farms a bake.

file_sha (a whole file's SHA-256) is for download integrity only and lives
in the manifest.

tools/geopatch/identity.py computes the same ids in Python;
tools/test_prism_world_ids.py checks both on synthetic maps and sidecars.
*/

#ifndef PRISM_IDENTITY_H
#define PRISM_IDENTITY_H

#include <stdint.h>
#include <stdio.h>

#include "mcc_sha256.h"
#include "prism_prsx.h"

/* the engine's derivation of the bake scene from the map (static scenery
rules, alpha coverage, SCEN install, triangle order and filtering): bumped
when that code changes, which re-farms the affected variant */
#define PRISM_SCENE_VERSION 1u
/* GEOM's meaning to the renderer */
#define PRISM_RENDER_FORMAT 1u

enum
{
	PRISM_ID_SIZE = 32,
	PRISM_MAP_HEADER_SIZE = 0x800,
};

enum prism_world_variant
{
	PRISM_WORLD_CLASSIC,
	PRISM_WORLD_ENHANCED,
};

/* ---------- base_id, streamed over the inflated body */

struct prism_base_id
{
	struct mcc_sha256 sha;
};

void prism_base_id_begin(struct prism_base_id *context);
void prism_base_id_add(struct prism_base_id *context, const void *bytes, unsigned long size);
void prism_base_id_end(struct prism_base_id *context, unsigned char id[PRISM_ID_SIZE]);

/* a map file's base_id: an Xbox disc map (header + zlib stream), or a map
already inflated (a cache slot: the header's file_length bytes are read,
and the zeros a slot carries past them are not). 1, or 0 if it is not a
whole map */
int prism_base_id_of_file(FILE *file, unsigned char id[PRISM_ID_SIZE]);

/* ---------- world_id */

void prism_world_id_classic(const unsigned char classic_world_id[PRISM_ID_SIZE], uint16_t bsp,
	unsigned char id[PRISM_ID_SIZE]);
/* from an opened sidecar's directory (its header's classic_world_id) */
void prism_world_id_enhanced(const struct prsx *prsx, uint16_t bsp, unsigned char id[PRISM_ID_SIZE]);
/* whether a directory entry enters the enhanced world_id of bsp */
int prism_world_id_counts(const struct prsx_entry *entry, uint16_t bsp);

/* 64 lowercase hex digits and a terminator */
void prism_id_hex(const unsigned char id[PRISM_ID_SIZE], char text[2 * PRISM_ID_SIZE + 1]);
/* 1 and id, or 0 if text is not 64 hex digits */
int prism_id_parse(const char *text, unsigned char id[PRISM_ID_SIZE]);

#endif
