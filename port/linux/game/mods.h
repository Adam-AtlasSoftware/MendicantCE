/*
MODS.H

The mods' tags played over a map's (mods.c), and the files they come from
(port/linux/src/port_mods.c).
*/

#ifndef __MODS_H
#define __MODS_H
#pragma once

/* ---------- headers */

#include "cseries.h"

/* ---------- constants */

/* a tag file's header (Halo PC's, big-endian, as the Halo Editing Kit and
Invader write it): its group, its header's size, its group's version and
its signature, then its tag's root */
#define MOD_TAG_FILE_HEADER_SIZE 0x40
#define MOD_TAG_FILE_GROUP_OFFSET 0x24
#define MOD_TAG_FILE_HEADER_SIZE_OFFSET 0x2C
#define MOD_TAG_FILE_VERSION_OFFSET 0x38
#define MOD_TAG_FILE_SIGNATURE_OFFSET 0x3C

/* the largest tag file read whole (the largest of Halo PC's sounds is a few
MB; a bitmap's pixels are read as they are drawn) */
#define MAXIMUM_MOD_TAG_FILE_SIZE (64UL * 1024 * 1024)

/* ---------- structures */

struct sound_permutation;
struct bitmap_data;
struct vertex_buffer;
struct render_skinning;
struct structure_bsp;

/* ---------- prototypes/PORT_MODS.C (the platform layer's: port_mods.h) */

long port_mods_scan(void);
unsigned long port_mods_scans(void);
long port_mods_played(void);
long port_mods_file_find(char const *tag_file, char const *map_name);
int port_mods_file_stamp(long file, unsigned long *size, unsigned long stamp[2]);
int port_mods_file_read(long file, unsigned long offset, void *buffer, unsigned long size);
long port_mods_file_order(long file);
char const *port_mods_file_mod(long file);
char const *port_mods_file_path(long file);
long port_mods_count(void);
long port_mods_at(long position);
char const *port_mods_folder(long index);
int port_mods_describe(long index, char *text, unsigned long size, char const *map_name);
int port_mods_report(char *text, unsigned long size, char const *map_name, int most);

/* ---------- prototypes/MODS.C */

/* the game's (cache_files.c, scenario_tags_load and scenario_tags_unload) */
void mods_tags_loaded(char const *map_name);
void mods_tags_unloaded(void);
/* the game's (cache_files.c, scenario_structure_bsps_read_ahead, and
scenario.c, scenario_switch_structure_bsp): a structure bsp of the map read
where it loads, its lightmap layout known (mod_bitmaps_structure_bsp_read) */
void mods_structure_bsp_read(short structure_bsp_index, struct structure_bsp const *structure_bsp);
/* the console's (hs.c) */
void mods_reload(void);
void mods_enable(boolean enabled);
void mods_list(void);
/* Existing sound-authoring commands and audio.loose_sounds. Loose files
take priority over mod packs and stay enabled when mods_enable is false. */
void loose_sounds_reload(void);
void loose_sounds_enable(boolean enabled);
boolean loose_sounds_enabled(void);
/* the Mods menu's (menu_functions.c): the mods played again as it left them */
void mods_apply(void);
/* for each group's: whether its update is the console's, to say what it did
there */
boolean mods_say(void);

/* for each group's: a tag file's bytes */
unsigned long mods_read_be16(byte const *bytes);
unsigned long mods_read_be32(byte const *bytes);
short mods_read_short(byte const *bytes);
real mods_read_real(byte const *bytes);
boolean mods_in_file(unsigned long offset, unsigned long size, unsigned long file_size);
boolean mods_finite_real(real value);
/* whether the first bytes of a tag file are a header of the group's, of
its version */
boolean mods_tag_file_header_valid(byte const *file, unsigned long group_tag, short version);
/* the file of the mods played that is the tag `tag_index`'s with its
group's `extension` (".sound"), or NONE */
long mods_tag_file_find(long tag_index, char const *extension);
/* the whole file, in a buffer of the game's allocator (free() it); NULL
with *reason saying why not */
byte *mods_tag_file_read(long file, unsigned long *size, char const **reason);
/* what the mods' tags keep in memory, of all their groups: FALSE when
`size` more would be too much */
boolean mods_memory_take(unsigned long size);
void mods_memory_give(unsigned long size);
unsigned long mods_memory_left(void);
/* the map loaded's name ("a10"), and whether its tags are loaded */
char const *mods_map_name(void);
boolean mods_map_loaded(void);
/* how many of the map's bitmaps the mods play (xbox_texture_cache.c) */
long mods_bitmap_count(void);
/* whether the mods are played on the map loaded (not given back by
mods_enable false, nor left off its menu pictures), and how many of its
string lists, bitmaps, sounds and models are theirs (a Save Report's:
report_game.c) */
boolean mods_played_on_map(long counts[4]);

/* ---------- prototypes/MOD_SOUNDS.C */

/* every one of a group's: the map's tags played from the mods' files (or,
remove_all, none); as the map loads (at_load) nothing plays yet, and
nothing is said at the console. How many of its tags the mods play */
/* Loose sounds remain independent of remove_all (which disables packs). */
long mod_sounds_update(boolean remove_all, boolean at_load);
void mod_sounds_tags_unloaded(void);
/* (sound_cache_start_loading_sound) whether a permutation is a mod's,
read at once into buffer */
boolean mod_sounds_read(struct sound_permutation const *permutation, void *buffer);
/* Whether the map's sound tag `tag_index` plays from a mod's or a loose tag file (an override) */
boolean mod_sounds_replaced(long tag_index);

/* ---------- prototypes/MOD_BITMAPS.C */

long mod_bitmaps_update(boolean remove_all, boolean at_load);
void mod_bitmaps_tags_unloaded(void);
/* (texture_cache_start_loading_bitmap) whether a bitmap is a mod's, its
pixels read at once into buffer and made the hardware's */
boolean mod_bitmaps_read(struct bitmap_data *bitmap, void *buffer);
/* whether a bitmap is a mod's (whose linear rows are padded as they load) */
boolean mod_bitmaps_contain(struct bitmap_data const *bitmap);
/* a structure bsp of the map read (as the map loads, or switched to): its
lightmap layout known, the mods' lightmaps for it are taken or left out.
How many of the map's bitmaps the mods play then */
long mod_bitmaps_structure_bsp_read(short structure_bsp_index, struct structure_bsp const *structure_bsp);
/* the mods' lightmaps waiting for their bsp to be read (they are the
map's until then) */
long mod_bitmaps_waiting_count(void);

/* ---------- prototypes/MOD_LIGHTMAPS.C */

/* "lm1-" and 16 hexadecimal digits */
#define MOD_LIGHTMAP_FINGERPRINT_TEXT_SIZE 21
/* (the picture check's: docs/mods.md) */
#define MOD_LIGHTMAP_MINIMUM_CORRELATION 0.9f

/* the structure bsp's lightmap layout fingerprint, and as text */
unsigned long long mod_lightmap_fingerprint(struct structure_bsp const *structure_bsp);
void mod_lightmap_fingerprint_text(unsigned long long fingerprint, char text[MOD_LIGHTMAP_FINGERPRINT_TEXT_SIZE]);
/* the fingerprint a lightmap's layout file holds, lower case: FALSE if it
holds none */
boolean mod_lightmap_sidecar_parse(char const *bytes, unsigned long size, char text[MOD_LIGHTMAP_FINGERPRINT_TEXT_SIZE]);
/* the bytes of a page's first level, 0 for a format the picture check
cannot read */
unsigned long mod_lightmap_page_size(struct bitmap_data const *bitmap);
/* the luminance of a page's first level (0 to 255, width x height of them),
from its pixels: FALSE for a format it cannot read */
boolean mod_lightmap_page_luminance(struct bitmap_data const *bitmap, byte const *pixels, real *luminance);
/* the correlation of the map's page with the mod's of the same layout (1:
the same picture), each reduced to the map's size and blurred */
real mod_lightmap_page_correlation(real const *map_luminance, long map_width, long map_height,
	real const *mod_luminance, long mod_width, long mod_height);

/* ---------- prototypes/MOD_STRINGS.C */

long mod_strings_update(boolean remove_all, boolean at_load);
void mod_strings_tags_unloaded(void);

/* ---------- prototypes/MOD_MODELS.C */

long mod_models_update(boolean remove_all, boolean at_load);
/* (before the tags go: the map's models are given back, unless give_back
is FALSE (the tags are gone already), and the mods' buffers let go) */
void mod_models_tags_unloaded(boolean give_back);
/* (custom_edition_part_palette) the nodes a mod's part of a model of many
nodes is drawn with, by its vertex buffer: their count, 0 for any other */
short mod_models_part_palette(struct vertex_buffer const *vertex_buffer, byte const **nodes);

/* ---------- prototypes/MOD_MATERIALS.C */

struct shader;

long mod_materials_update(boolean remove_all, boolean at_load);
void mod_materials_tags_unloaded(void);
/* (mods.c, as a map loads) whether a mod that is on has a surface map for
the map */
boolean mod_materials_surface_maps_any(void);

/* ---------- prototypes/MOD_TEXTURES.C (display.prism_textures: the
replacements the host draws, port/linux/vulkan/xgpu_texture_store.h) */

long mod_textures_update(boolean remove_all, boolean at_load);
void mod_textures_tags_unloaded(void);
/* (main_game_render, once a frame) */
void mod_textures_frame(void);

/* ---------- prototypes/MOD_MESHES.C (display.prism_models: the meshes the
host draws in place of models' parts, port/linux/vulkan/xgpu_mesh_store.h) */

long mod_meshes_update(boolean remove_all, boolean at_load);
void mod_meshes_tags_unloaded(void);
/* (main_game_render, once a frame) */
void mod_meshes_frame(void);
/* (models.c's render_model, around its parts) the model drawn, its nodes'
matrices sent where a mod's mesh replaces it; and the drawing done. The
identifier is render_model's (an object's index, a deleted object's for a
ghost, 0 for a draw that is no object's: the sky), which names the instance
and is never looked up; the function values are render_model's own, the
draw's (never NULL there) */
void mod_meshes_model_begin(long model_index, struct render_skinning const *skinning, real level_of_detail_pixels,
	unsigned long flags, long unique_identifier, real const *function_values);
void mod_meshes_model_end(void);
/* whether a model tag is drawn by a mod's mesh (the host accepted its file
for this map: drawn from the store once it is in, the stock part until
then): the bake's static scenery leaves such a placement out of the scene
the frame traces, which traces it as drawn (prism_bake.c) */
boolean mod_meshes_model_replaced(long model_index);
/* (models.c's render_model_parts, around an opaque, decal or shadow part's
draws) the part the draws that follow are of; and none */
void mod_meshes_part(short region_index, short permutation_index, short geometry_index, short part_index,
	short shader_index);
void mod_meshes_part_end(void);
/* (a pass that draws a part's stock buffers again later in the frame: the
fog screen's queued models) the part named now, as its MESH_DRAW had it
(FALSE, the key zero: none); and that part named again around the later
draw (NULL after it: none), so that the host draws the part's mesh there
too, never the stock part under it */
#define MOD_MESHES_PART_KEY 7
boolean mod_meshes_part_key(unsigned long key[MOD_MESHES_PART_KEY]);
void mod_meshes_part_name_again(unsigned long const *key);
/* (rasterizer_xbox_models.c, around a model shader's draws) the shader's
material overlay armed for the draws that follow, if a mod has one and
display.prism_material_overlay is on (its base map and the draw's
permutation pick the bitmap its class mask is of); and disarmed */
void mod_materials_draw_begin(struct shader const *shader, real camera_distance, boolean has_multipurpose,
	long base_map_index, short permutation_index);
void mod_materials_draw_end(void);
/* (the script function prism_set) */
void mod_materials_prism_set(char const *text);
/* PRISM v2's model light (prism_light.c) */
void prism_light_map_loaded(void);
void prism_light_map_unloaded(void);
void prism_light_window_begin(float const camera[3]);
void prism_light_object_begin(long object_index);
int prism_light_draw(float uniforms[][4], unsigned int *probes);
int prism_light_wanted(void);

/* ---------- prototypes/MOD_PBR.C (display.prism_materials: a mod's
materials, xgpu.h's "PBR materials") */

long mod_pbr_update(boolean remove_all, boolean at_load);
void mod_pbr_tags_unloaded(void);
/* (mod_textures.c, once a frame: TRUE if the materials were read again) */
boolean mod_pbr_frame(void);
/* (mod_textures.c) the images' files, and the store's ids of them */
long mod_pbr_texture_count(void);
long mod_pbr_texture_file(long index);
void mod_pbr_textures_answered(unsigned long const *ids, long count);
/* (mod_materials.c's draw_begin and draw_end) */
boolean mod_pbr_draw_begin(struct shader const *shader);
void mod_pbr_draw_end(void);

/* ---------- prototypes/PRISM_MATERIALS.C (PRISM v4's materials of the pack
and mod tiers: docs/prism_materials.md) */

long prism_materials_update(boolean remove_all, boolean at_load);
void prism_materials_tags_unloaded(void);
/* (mod_textures.c, once a frame: TRUE if the materials were read again) */
boolean prism_materials_frame(void);
/* (mod_textures.c) the images' files, and the store's ids of them */
long prism_materials_texture_count(void);
long prism_materials_texture_file(long index);
void prism_materials_textures_answered(unsigned long const *ids, long count);
/* (prism_deferred.c) moves as the materials are read and answered; a
shader's maps sent after its SCENE_MATERIAL (forget: the automatic tier's
where it has none) */
unsigned long prism_materials_generation(void);
void prism_materials_scene(void const *shader, unsigned long material_id, boolean forget);
/* (mod_materials.c's prism_set "material_tiers list") */
void prism_materials_describe(void);

#endif
