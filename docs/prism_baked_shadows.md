# PRISM baked shadows (`display.prism_baked_shadows`)

The sun's shadows in the level's baked light (the structure's lightmap pages) made as sharp as the ray tracing's,
off by default. The ray tracing (`docs/prism_rt.md`) and the cascaded sun shadows (`display.prism_shadows
"cascaded"`) only ever darken the level where an *object* blocks the sun: the structure's own shadows are the
lightmap's, never darkened again. So with every other PRISM feature on, a cliff's shadow on the sand is still as
soft as a lightmap texel (CE's texels are 0.25 to 0.75 world units outside on b30 and a30, 0.6 to 2.3 m; the rebaked
pages' four times finer), while a Marine beside it casts a crisp traced one. This feature sharpens the former.

## What is drawn

Each page that has a sun mask (`<data>/prism_lightmaps/sun/<map>_<bsp>_<page>.psm`, made on the player's machine
from their own map, never shipped) is drawn with its light multiplied, at each pixel, by

    f = (A + S v_fine) / (A + S v_page)

- `v_fine`: the sun's visibility baked at 8 or 16 times the page's density (16 where CE's texel is over 0.06 world
  units; b30's and a30's outside are 16), from the remodel's shape where `display.prism_world_geometry` has one
  (the `.pwg`'s near triangles; CE's own surfaces for the rest; alpha-tested surfaces cut out by their base map's
  alpha, transparent ones passing light), through a sun disc as wide as the ray tracing's (0.02 radian radius,
  2.3 degrees across) or the sky light's own, whichever is wider;
- `v_page`: the visibility the drawn page itself holds: read from its own light about the bake's shadow edges
  (the rebaked page's for `"rebaked"`, the stock page's for `"stock"` and `"smooth"`; elsewhere the coarse bake's);
- `S`, `A`: the sun's direct light unshadowed and everything else (the sky, bounce, lamps), from the relight
  calibration's light groups at CE's size in CE's units (`art_pipeline/relight`), the sun's cut to what the page
  holds where it holds less;
- coded as `log2(f)` in a byte (1/8 to 8, 128 exactly 1), and as the page's encoding has it (`f ^ (1 / gamma)`, the
  fit's gamma).

`f` is 1 wherever the page already holds the sun as finely as the bake (all lit, all shadowed, no sun): only light
near a shadow's edge moves from one side to the other, so each area keeps CE's brightness, colour and mood (b30's
pages' mean `f` in linear light 0.995 to 1.021; whole frames within 2%, below). Brightening is held back where the
page holds no sun at all (the fine bake and the page's geometry disagree: trusted to the page), and the sun's
share is cut where the page holds less light than the sun would give it.

With `display.prism_shadows "cascaded"` or `display.prism_rt`, the bake's sharp visibility also replaces the
lightmap's dominant direction's guess of "already in the structure's shadow", so a moving caster never darkens the
structure's own shadow again, now to the bake's edge.

A light more than 0.1 radian wide (c10's and a50's: their sky lights are 20 and 50 degree domes, which
`render_sky_sun` takes for the sun) is not sharpened: CE's soft shadows are right there, and a finer bake only
moved the pages' mood (c10's pages' mean `f` 0.70 to 1.20).

## The file and the image

`.psm`: a 56-byte header (`"PSM1"`, version 1, CE's page size, the factor, the tile size in CE texels (4), the
stock page's hash as `prism_lightmaps.c` computes it, the tiles across and down, the tiles stored, the deflated and
inflated sizes, a CRC-32), then deflated: an entry per tile of 4 by 4 CE texels (the stored tile's number + 1, or 0
with the tile's even visibility) and the stored tiles (RGBA: `f` against the stock page, against the rebaked page,
the visibility, 255). Only tiles a shadow's edge crosses are stored: b30's outside BSP 10% of its tiles, a30's 18
and 26%.

`prism_lightmaps.c`'s worker lays the page's image out with the mask below the light (the rebaked page's two
halves, the smooth copy, or the stock texels), the entries, then the tiles aligned to 64 so the host's levels never
mix two, and the layout in the last row (`xgpu.h`, `XGPU_LIGHTMAP_FILTER_SUN`). The draw (keyed with
`XGPU_LIGHTMAP_FILTER_SUN`, draw by draw, only for a page whose image has it) reads the layout (4 fetches), the
tile's entry (1 fetch) and the tile at the level of the pixel's footprint (1 sample). `display.prism_baked_shadow_strength`
scales `log2(f)` as the image is laid out (the pages are laid out again when it changes). Off (the default), nothing
is read and no draw is keyed differently.

Host images over 8 MB are now uploaded in bands (`xgpu_vulkan_textures.c`): an upload may take a quarter of the
128 MB ring, and a 512 by 256 page rebaked with its mask is 4096 by 1984 (32 MB); before, it silently stayed
unwritten (black).

## Making the masks

```sh
# per BSP whose sky has a sun (relight's calibration bake and fit already made: art_pipeline/relight/relight_level.sh)
blender -b --factory-startup --python art_pipeline/relight/sunmask_bake.py -- --gltf <bsp.gltf> \
    --lights <map>_lights.json --bsp <n> --calib <relight bake dir> --out <dir> [--pwg <bsp.pwg>] --gpu 2060
python3 art_pipeline/relight/sunmask_encode.py --gltf <bsp.gltf> --bake <dir> --map <map> --bsp <n> \
    --out <data root> --calib <relight bake dir>
```

The bake's sun is `render_sky_sun`'s (the sky light of most power times brightest channel; its yaw and pitch, checked
against the game's log on b30, a30 and a50). Times on the RTX 2060 (Cycles, OptiX, 64 samples): b30's outside 4 min,
a30's two BSPs 4 min each, a10's three sunlit BSPs 2 to 3 min; the encode 1 to 2 min a BSP on the CPU.

## Measured

The kit's settings (everything PRISM on), b30 at tick 1350, the game's camera stopped, uncapped, RTX 2060 at 1080p
(another process kept it 87% busy before the run: the first round's figures are noise and left out); GPU ms a
frame (`debug.gpu_stats`), off against `"sharp"`: RT off 13.66 against 13.74 (+0.08), RT shadows 15.63 against
15.71 (+0.08). GPU memory: b30's outside BSP 48 MB of tiles, a30's 108 and 160 MB, a10's 26 to 101 MB (only the
current BSP's pages are resident); on disk 23 MB for all six BSPs.

Frames against the rebaked kit with the setting off (mean linear luminance): b30 0.983 to 1.000 (the lightmap pass
alone 0.983 to 1.0005), a30 0.998 to 1.000, a10 1.000. With it off, stopped frames at a fixed camera on b30 are byte
for byte the base build's (stock, and the kit's settings).
