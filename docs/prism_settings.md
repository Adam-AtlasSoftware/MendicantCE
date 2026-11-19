# The lighting's settings: what applies in each mode

Settings > Video > **Lighting** chooses how the world is lit, and the settings shown follow it:

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
adds the smoothed lightmaps, ambient occlusion, highlights and soft shadows), which reach Classic alone.

## Where each setting is, and when it is shown

The rows of a mode other than the one shown are hidden and keep their values (Defaults leaves them too); some rows
also follow another's value, as Frame Rate Limit follows VSync. The change of Lighting on Video shows on the pages it
opens at once, before OK. `tools/port_settings.py`'s `LIGHTING_MODE_ROWS` and `menu_functions.c`'s `lighting_rows`
hold the table (C Classic, R Rebaked, P Path Traced); `tools/test_port_settings.py` checks that they agree and that
every page fits in every mode.

**Settings > Video:** Lighting; Lighting Settings ›; Ray Tracing ›; Brightness ›; Advanced Graphics ›.

**Lighting Settings:**

| Section | Rows | Shown |
| --- | --- | --- |
| Classic Lighting | Shadow Quality, Smooth Lighting, Dynamic Lights, Ambient Occlusion, Shade Depth (with World) | C |
| PRISM Extras for Classic | Lightmaps, Model Highlights, Model Reflections, Sun Shadows, Sun Shadow Detail and Edges (with Sun), Baked Shadows, Shadow Depth (with Sharp) | C |
| Rebaked Lighting | Composition, Object Light, Shadow Quality (while Object Shadows are Classic's) | R |
| Path Tracing | Profile, Denoiser | P |
| Image | Exposure, Exposure Bias, Tone Curve | R, P |
| Light Bake | Bake, Bake Quality, Sky Light, Rebake Lightmaps, Progress on Screen | R, P (Path Traced reads the bake) |

**Ray Tracing:**

| Section | Rows | Shown |
| --- | --- | --- |
| Ray Tracing | Ray Tracing (`display.prism_rt`: Off, Shadows, Shadows + GI, All Lights) | C |
| | Object Shadows (traced or Classic's) | R |
| | Quality, Ray Resolution, Denoiser, Light Freedom, Traced Sun, Traced Level, Traced Light Look › | C, with ray tracing on |
| Traced Scene | Traced Models, Keep Shapes | C with ray tracing on; R with Object Shadows traced; P |
| | Cutout Shadows (in the bake: a change bakes again) | C with ray tracing on; R; P |

`display.prism_rt` is the ray tracer over Classic's light (`docs/prism_rt.md`); both PRISM modes turn it off
(`prism_rt.c`), and its "All Lights" (`"pathtraced"`) is not PRISM Path Traced. Traced Light Look is its look (style,
hold, exposure, tone curve, lamps, sun size, bounced colour, bounces): none of it reaches the PRISM modes (the bake
fixes the lamps, their brightness, the bounced colour and the sun's disc).

**Every mode:** Advanced Graphics (Quality, Battery Saver, Render Resolution, Anti-Aliasing (no MSAA under the PRISM
modes), Texture Filtering, Model Detail, Smooth Motion, Shield Glow, Glow Intensity, Tick Rate, Animations);
Brightness (gamma, HDR); Mods > PRISM Graphics (World Geometry, Geometry Detail, Bloom, Tone Grade; Geometry Shadows
with Classic's Sun shadows); Mods > PRISM Assets (textures, remodels, PBR materials, streaming; Material Detail and
Surface Detail under Classic, Material Packs under the PRISM modes). Android has no Lighting choice: its light is
Classic's, and its Advanced Graphics keeps Classic's lighting rows (its Quality and Battery Saver are on Video).

## config.toml

A new file's `[display]` is written in groups, each titled with the modes it reaches (the window, the frame rate,
brightness, graphics quality, the lighting, PRISM's image, the light bake, Rebaked, Path Traced, ray tracing, Classic's
lighting, post effects, the remodel and assets, the view, the rest), and each lighting setting's comment ends with
the modes it reaches (`# Lighting: ...`). A file of an older version loads as it is, every value kept: a setting it
lacks is added at its section's end. No key was renamed.
