# The lighting's and PRISM's settings: what applies in each mode

Settings > Video > **PRISM** holds every PRISM setting and the lighting's. Its first row, **Lighting**, chooses how
the world is lit, and the rows below it, and the pages they open, follow it:

| Lighting | `display.lighting` | What lights the frame |
| --- | --- | --- |
| Classic | `"ce"` (`"ce_deferred"`, a diagnostic, counts as Classic) | The game's own light, with this port's and PRISM's extras on it |
| PRISM Rebaked | `"raster"` | The level's light baked on this machine (`docs/prism_bake.md`), drawn by the deferred core |
| PRISM Path Traced | `"pathtraced"` | Every light traced each frame, with the bake's scene and bounced light (`docs/prism_pathtraced.md`) |

`"classic"`, `"rebaked"` and `"path_traced"` are read as `"ce"`, `"raster"` and `"pathtraced"` (in config.toml, where
they are written back as those, and in `HALO_LIGHTING`).

**Enhanced is not a lighting mode.** It is `display.graphics`, the quality preset (Settings > Video > Advanced
Graphics > Quality: Xbox, Enhanced, PRISM Extras): anti-aliasing, texture filtering and model detail, which reach every
mode, and Classic's lighting settings (shadow size, per-pixel model light, the enhanced dynamic lights; PRISM Extras
adds the smoothed lightmaps, ambient occlusion, highlights and soft shadows), which reach Classic alone, but for the
enhanced dynamic lights, which reach every mode (below).

## The menu tree

The rows of a mode other than the one shown are hidden and keep their values (Defaults leaves them too); some rows
also follow another's value, as Frame Rate Limit follows VSync. A change of Lighting shows on PRISM's page and the
pages it opens at once, before OK. `tools/port_settings.py`'s `LIGHTING_MODE_ROWS` and `menu_functions.c`'s
`lighting_rows` hold the table (C Classic, R Rebaked, P Path Traced; a row in neither shows in every mode);
`tools/test_port_settings.py` checks that they agree, that every page fits the 4:3 and 16:9 layouts in every mode,
and that every lighting and PRISM row is under Video > PRISM.

**Settings > Video:** Display (Window, Resolution or Window Size, Refresh Rate, Frame Rate & Sync ›, Field of View,
First Person ›); Graphics (High-Res Text, **PRISM ›**, Brightness ›, Advanced Graphics ›). Android has no PRISM row:
its light is Classic's, and its Advanced Graphics keeps Classic's lighting rows (its Quality and Battery Saver are on
Video).

**Settings > Video > PRISM:**

| Section | Row | Shown |
| --- | --- | --- |
| Lighting | Lighting (`display.lighting`: Classic, PRISM Rebaked, Path Traced) | C, R, P |
| | Classic Lighting › | C |
| | Rebaked Lighting › | R |
| | Path Tracing › | P |
| | Ray Tracing › | C, R, P |
| | Image › | C, R, P |
| | Upscaling › | R, P |
| | Light Bake › | R, P (Path Traced reads the bake) |
| World and Assets | Materials & Textures › | C, R, P |
| | Models & Streaming › | C, R, P |
| | World & Terrain › | C, R, P |

The pages it opens:

| Page | Section | Rows | Shown |
| --- | --- | --- | --- |
| Classic Lighting | Classic Lighting | Shadow Quality, Smooth Lighting, Dynamic Lights, Ambient Occlusion, Shade Depth (with World) | C |
| | PRISM Extras for Classic | Lightmaps, Model Highlights, Model Reflections, Sun Shadows, Sun Shadow Detail and Edges (with Sun), Baked Shadows, Shadow Depth (with Sharp) | C |
| Rebaked Lighting | Rebaked Lighting | Composition, Object Light | R |
| | Shadows | Object Shadows (traced or Classic's), Shadow Quality (while Classic's) | R |
| | Moving Lights | Dynamic Lights (`display.dynamic_lights`) | R |
| Path Tracing | Path Tracing | Profile, Denoiser (PRISM, Off, DLSS RR once its plugin is ready) | P |
| | Moving Lights | Dynamic Lights (`display.dynamic_lights`), Brightness (`display.prism_dynamic_lights`), Colour (`display.prism_light_saturation`) | P |
| Ray Tracing | Traced Light and Shadows | Ray Tracing (`display.prism_rt`: Off, Shadows, Shadows + GI, All Lights) | C |
| | | Quality, Ray Resolution, Denoiser, Light Freedom, Traced Sun, Traced Level, Traced Light Look › | C, with ray tracing on |
| | Traced Scene | Traced Models, Keep Shapes | C with ray tracing on; R with Object Shadows traced; P |
| | | Cutout Shadows (the moving objects' under R and P: the level's are always in the bake, no rebake) | C with ray tracing on; R; P |
| Traced Light Look | Look, Lights | Style, Lightmap Hold, Exposure, Tone Curve; Lamps, Lamp Brightness, Sun Size, Bounced Colour, Bounces | C (opened from Ray Tracing) |
| Image | Exposure | Exposure, Exposure Bias, Tone Curve | R, P |
| | Post Effects | Bloom, Tone Grade | C, R, P |
| Upscaling | Resolution | Upscaler (`display.upscaler`: Off, FSR 3.1, DLSS once its plugin is ready), Upscale Quality (while an upscaler is on) | R, P |
| Light Bake | Light Bake | Sky Light (applied at once: the layered bake recombines it, no rebake). No Bake Quality: a release ships every level at MAX (`display.bake_quality`, the quality of a bake made here, is config.toml's) | R, P |
| | Levels | Each campaign level's bake (Shipped, Checking, Baked, Baking 37%, Loading, Part Baked, Not Baked, No Map, Failed), Rebake Local (asks first; the shipped light kept) | R, P |
| Materials & Textures | Materials | Material Packs (`display.material_packs`) | R, P |
| | | PBR Materials (`display.prism_materials`) | C, R, P |
| | | Material Detail, Surface Detail (the model shaders' forward light) | C |
| | Textures | Texture Detail, Texture Memory, Texture Fade-In, Texture Wait | C, R, P |
| Models & Streaming | Remodels | Remodels, Model Memory, Secondary Motion | C, R, P |
| | Streaming | Preload, Detail Steadiness | C, R, P |
| World & Terrain | World Geometry | World Geometry, Geometry Detail, Geometry Blend | C, R, P |
| | | Geometry Shadows (with Classic's Sun shadows) | C |
| | Terrain and Water | Terrain (`display.terrain`), Ground Layers (`display.terrain_layers`), Footprints (`display.terrain_tracks`), Water (`display.water`); the last three dimmed while Terrain is off | R, P |

`display.prism_rt` is the ray tracer over Classic's light (`docs/prism_rt.md`); both PRISM modes turn it off
(`prism_rt.c`), and its "All Lights" (`"pathtraced"`) is not PRISM Path Traced. Traced Light Look is its look (style,
hold, exposure, tone curve, lamps, sun size, bounced colour, bounces): none of it reaches the PRISM modes (the bake
fixes the lamps, their brightness, the bounced colour and the sun's disc).

**The materials in Path Traced.** Material Packs and PBR Materials reach both PRISM modes: the material pack's and the
mods' materials are drawn into the G-buffer (`prism_materials.c`, read wherever `prism_deferred_wanted`, which is
true for `"raster"` and `"pathtraced"` alike), and the path tracer takes each pixel's colour, normal, roughness and
metal from that G-buffer. Rebaked's Composition (game shaders or PRISM's materials) is Rebaked's alone: Path Traced
always lights PRISM's materials. Material Detail and Surface Detail change the model shaders' forward light, which
only Classic draws.

**Dynamic Lights** (`display.dynamic_lights`, Classic or Enhanced) reaches every mode, so it has a row on each mode's
lighting page (Classic Lighting, Rebaked Lighting, Path Tracing): under Classic it is how the game draws its lights;
under both PRISM modes the game gives the bake's frame and the path tracer the lights it submits (world_scene's list,
`docs/prism_pathtraced.md`), and Enhanced adds the plasma bolts' and needles' lights to them and draws their glows. A
Pro 2v2 game holds all three at Classic.

**The light bake needs no asking** (`docs/prism_bake.md`, "The bake on the main menu" and "The shipped bake"): a
release ships every campaign level's light baked at MAX for both worlds, used wherever it matches the player's maps
(SHIPPED on Light Bake, checked once in the background: CHECKING); under both PRISM modes the main menu bakes only what
it does not cover (a mod, a custom map), as other games compile their shaders, with a line and a bar under the menu; a
level started before its bake is done asks (PLAY NOW with Classic's light while it bakes in the background, or WAIT).
No setting a player has bakes again: Sky Light and the lamps' brightness are recombined from the layered bake at once,
Cutout Shadows is always in the bake (the setting chooses the moving objects' cutouts), a better bake serves a lesser
quality, and World Geometry switches between the shipped worlds (it asks REBAKE or UNDO only where the shipped light
lacks that world, and rebakes only the BSPs it changes). The performance settings (Path Traced's quality, the dynamic
lights, the shadows, the denoiser, the upscaler) scale the runtime light alone, never the bake. Light Bake's old Bake (While Playing, When Asked, Never), Rebake Lightmaps and
Progress on Screen rows are gone: `display.bake` `"ask"` is read as `"auto"` (and written so), `"off"` and
`display.bake_indicator` stay config.toml's.

The terrain overhaul's other settings (`display.terrain_quality`, `display.terrain_drifts`, `display.weather`,
`display.terrain_pack_folder`) are config.toml's alone until they draw something (`docs/prism_terrain.md`).

**Every mode, outside PRISM:** Advanced Graphics (Quality, Battery Saver, Render Resolution, Anti-Aliasing (no MSAA
under the PRISM modes), Texture Filtering, Model Detail, Smooth Motion, Shield Glow, Glow Intensity, Tick Rate,
Animations); Brightness (gamma, HDR). The Mods tab keeps the mods themselves (Manage Mods, the mods folder, the
Theater, the maps), not PRISM's settings.

## config.toml

A new file's `[display]` is written in groups, each titled with the modes it reaches (the window, the frame rate,
brightness, graphics quality, the lighting, PRISM's image, the light bake, Rebaked, Path Traced, ray tracing, Classic's
lighting, post effects, the remodel and assets, the view, the rest), and each lighting setting's comment ends with
the modes it reaches (`# Lighting: ...`). A file of an older version loads as it is, every value kept: a setting it
lacks is added at its section's end. No key was renamed; `display.bake`'s `"ask"` is read as `"auto"`.
