# The lighting's and PRISM's settings: what applies in each mode

Settings > Video > **PRISM** holds every PRISM setting and the lighting's (v4 geopatch M3, `work/openh1_eval/
v4_geopatch_runs/PLAN_FINAL.md` sections 4 and 10). Its first row, **Lighting**, chooses how the world is lit; the
three modes never mix, and the rows below it, and the pages they open, follow it. Its second section, **Enhancements**,
is a preset of the enhancement groups and a page for each group.

| Lighting | `display.lighting` | What lights the frame |
| --- | --- | --- |
| Classic | `"classic"` (`"ce_deferred"`, a diagnostic, counts as Classic) | The game's own light and its own options alone: no ray tracing of any kind, no PRISM extras |
| Enhanced | `"enhanced"` (the default) | The release's MAX lightmaps, baked on the farm for the world geometry drawn and shipped (`docs/prism_bake.md`), drawn by the deferred core, with ray traced object shadows where the card traces rays |
| Path Traced | `"pathtraced"` | Every light traced each frame, with the lightmaps' scene and bounced light (`docs/prism_pathtraced.md`) |

The older `"ce"`, `"raster"`, `"rebaked"` and `"path_traced"` are read as `"classic"`, `"enhanced"`, `"enhanced"` and
`"pathtraced"` (in config.toml, written back so, and in `HALO_LIGHTING`).

**Players never bake.** There is no bake option, no bake quality, no rebake and no bake status anywhere: the shipped
MAX lightmaps load where their key is the BSP's (the classic world's, or the enhanced world's `world_id`,
`docs/prism_geometry.md`), and a level without them is drawn in its classic shape. `display.bake`, `_quality`,
`_rebake`, `_indicator`, `_budget`, `_folder` and `display.prism_rig` are no player's settings any more
(`_platform_internal` in `port_config.c`): never read from config.toml (an older file's lines are taken out as it
loads, so Adam's kit config with `bake = "auto"` and `bake_rebake = 1` loads with the bake off and nothing rebaked),
only the environment and the console set them: the farm's `HALO_BAKE=auto`. The only lightmap row is **Lightmap
Downloads**: the classic geometry's and the enhanced geometry's lightmaps, each with its state (INSTALLED, NOT
DOWNLOADED, 4 OF 10 LEVELS); getting and removing them comes with the content downloader (M7).

## The menu tree

The rows of a mode other than the one shown are hidden and keep their values (Defaults leaves them too); some rows
also follow another's value, as Enhanced's traced rows follow its Ray Tracing. A change of Lighting shows on PRISM's
page and the pages it opens at once, before OK. `tools/port_settings.py`'s `LIGHTING_MODE_ROWS` and
`menu_functions.c`'s `lighting_rows` hold the table (C Classic, E Enhanced, P Path Traced; a row in neither shows in
every mode); `tools/test_port_settings.py` checks that they agree and that every page fits, and
`tools/test_prism_enhancements.py` that Classic reaches no ray tracing, that no setting is reachable from two modes'
pages, that no traced row shows with Ray Tracing off and that no bake row exists.

**Settings > Video > PRISM:**

| Section | Row | Shown |
| --- | --- | --- |
| Lighting | Lighting (Classic, Enhanced, Path Traced) | C, E, P |
| | Classic Lighting › | C |
| | Enhanced Lighting › | E |
| | Path Tracing › | P |
| | Image › | C, E, P |
| | Upscaling › | E, P |
| | Lightmap Downloads › | E, P |
| Enhancements | Enhancements (Classic, Remastered, Realistic, Custom) | C, E, P |
| | World Geometry ›, Materials & Terrain ›, Foliage ›, Models & Textures › | C, E, P |

The pages it opens:

| Page | Rows | Shown |
| --- | --- | --- |
| Classic Lighting | Shadow Quality, Smooth Lighting, Dynamic Lights: the game's own options | C |
| Enhanced Lighting | Sky Light, Light Mix (`display.prism_raster_composition`), Object Lighting, Dynamic Lights (`display.enhanced_dynamic_lights`); Ray Tracing (`display.prism_ray_tracing`, greyed where the card cannot trace rays), then while it is on Object Shadows, Object Occlusion (`display.prism_raster_object_occlusion`: the moving objects' traced ambient occlusion, docs/prism_bake.md), Traced Models, Model Cache, Cutout Shadows; Shadow Quality (`display.enhanced_shadow_resolution`) while the object shadows are the game's | E |
| Path Tracing | Profile, Denoiser; Dynamic Lights (`display.pt_dynamic_lights`), Light Brightness, Light Saturation; Traced Models, Model Cache, Cutout Shadows (`display.prism_pt_objects`, `_cache`, `_alpha`) | P |
| Image | Exposure, Exposure Bias, Tone Curve (E, P); Bloom, Tone Grade (every mode) | |
| Upscaling | Upscaler, Upscale Quality | E, P |
| Lightmap Downloads | Classic Geometry, Enhanced Geometry (each its state) | E, P |
| World Geometry | World Geometry (greyed in Classic lighting: "Needs Enhanced or Path Traced lighting"), Geometry Detail, Geometry Blend; Campaign Collision (greyed until M8) | |
| Materials & Terrain | PBR Materials, Material Packs, Terrain, Ground Layers (sand, dirt, mud, rock, snow and ice), Footprints, Water; all but PBR Materials greyed in Classic | |
| Foliage | Grass, Trees, Leaves, Wind: greyed until a content release brings foliage | |
| Models & Textures | Remodels, Textures; Model Memory, Texture Memory, Secondary Motion, Texture Fade-In, Texture Wait, Preload, Detail Steadiness | |

**Each PRISM lighting has its own keys.** A setting the renderer reads by one name under every lighting
(`display.dynamic_lights`, `display.shadow_resolution`, `display.prism_rt_objects`, `_cache`, `_alpha`) is read by the
game as the lighting's own under Enhanced and Path Traced (`port_config.c`'s `config_mode_keys`: the read
`config_string` and the rest take `display.enhanced_dynamic_lights` under Enhanced, `display.pt_dynamic_lights` under
Path Traced, and so on). On the first start of this version each new key takes the older shared one's value from the
file (`config_migrate_enhancements`), so nothing a player chose is lost.

**Classic is CE's own.** Classic's PRISM extras (`display.prism_lightmaps`, `_specular`, `_reflections`, `_shadows` and
their detail, `_baked_shadows`, `_surface_maps`, `_material_overlay`, the remodel's shadow and occlusion shapes, world
ambient occlusion) and Classic's ray tracer (`display.prism_rt` and every `display.prism_rt_*` of its own, `display.
prism_sun_size`) are no player's settings any more: off, the code kept for the farm and for later ports into Enhanced
(a port not ready has no row; it is never left in Classic). `display.graphics`' `"prism"` (Classic with PRISM's extras)
is read as `"enhanced"`. Upscaling stays an Enhanced and Path Traced page: Classic's renderer draws without an
upscaler (`prism_upscale.c`).

## The enhancements

`display.enhancements` (`"realistic"` by default) is a preset of each group's settings, as `display.graphics` is of its
own (the two never share a setting): a setting not set by hand follows it, a setting written at the value the preset
gives it is left to it again, and Custom is only what the row shows while a set one differs. A group that ships in a
later version arrives at the player's level with no menu visit. The preset never sets the lighting mode,
`display.graphics`' settings or collision.

**One registry.** `port_config.c`'s `config_enhancements` (the preset's values, the group, the lighting modes a row
applies in, its class and the content it needs) is the registry; `tools/port_settings.py`'s `ENHANCEMENTS` is its
twin, which writes the groups' rows, checked by `tools/test_prism_enhancements.py`. The rules it keeps:

- only `display.world_geometry` changes the bake (WORLD_BAKED): exactly two world variants per map;
- no BAKE_LAYER row until its layer ships in a BAKE_VERSION; no SIM row in the preset (Campaign Collision is its own);
- a row of content no release has brought yet (`"foliage"`) is greyed, its help saying it arrives with a content
  update.

| Group | Rows | Classic / Remastered / Realistic |
| --- | --- | --- |
| World Geometry | `display.world_geometry` | classic / enhanced / enhanced |
| Materials & Terrain | `display.prism_materials`, `display.material_packs`, `display.terrain`, `display.terrain_layers`, `display.terrain_tracks`, `display.water` | the game's / on / on |
| Foliage | `display.foliage_grass`, `display.foliage_trees`, `display.foliage_leaves`, `display.foliage_wind` (nothing draws them yet) | classic / enhanced / realistic (trees enhanced) |
| Models & Textures | `display.prism_models`, `display.prism_textures` | stock / enhanced / max |

**Adding an enhancement:** classify it (PLAN_FINAL section 4.4); add its row to `config_enhancements` and to
`ENHANCEMENTS` (a new group: its page in `ENHANCEMENT_GROUPS`, which puts its link on PRISM's page); a RUNTIME row needs
nothing else, a BAKE_LAYER row a layer in a planned BAKE_VERSION, static geometry joins the World Geometry switch.

## Defaults

Enhanced everything (PLAN_FINAL section 10): Enhanced lighting, Enhanced world geometry, the enhancements at Realistic,
ray tracing on where the card traces rays; Campaign collision Original until M8's gates pass (`game.campaign_collision`,
always `"original"` in this version).
