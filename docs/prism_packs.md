# PRISM's packs: building them from your own game

Most of PRISM's asset settings draw files that are made from the game's own data: remodelled characters, sharper
textures, a remodelled level, lightmaps baked again. Those files are not in any download and never will be,
because they are made of the game's data. The tools that make them are. They ship with the game in
`prism_packs/` and are in the repository as `tools/prism_packs/`. They read your copy of the game, the maps the
game extracted from your disc image at its first start, and build the packs on your computer, into the game's
data folder.

**Everything the tools make is made from your copy of the game. It stays on your computer.** Do not share,
upload or publish the packs, the work folder, or anything in either. Each pack has a `LOCAL_ONLY.txt` that says
so.

## What you need

| | |
| --- | --- |
| The game | MendicantCE, started once so that it has extracted the maps from your disc image. Its data folder holds `maps/` with `a10.map` and the rest. |
| Python | 3.10 or newer, with `numpy`, `pillow`, `scipy`, `scikit-image`, `opencv-python-headless`, `torch` and `spandrel` (`prism_packs/requirements.txt`). |
| Blender | 4.5 LTS, from [blender.org](https://www.blender.org/download/lts/4-5/). It runs headless; you never see it. Other versions may build different shapes or fail. |
| Invader | 0.55 or newer, from [github.com/SnowyMouse/invader](https://github.com/SnowyMouse/invader). Its programs turn the remodels and maps into the tags the game loads (`invader-model`, `invader-bitmap`, `invader-extract`, `invader-edit`, `invader-convert`). |
| The extractor | `bin/openh1-map-extract` in the Linux download. It reads the maps as the game reads them. On Windows it runs through WSL (below). |
| Upscalers | Fetched by the tools when you ask, each with its licence shown (below). |
| A graphics card | Recommended. Blender's Cycles renders the bakes on an NVIDIA (OptiX or CUDA), AMD (HIP), Intel (oneAPI) or Apple card, and on the processor without one, many times slower. The upscalers use a Vulkan card, and PyTorch's CUDA on an NVIDIA card or the processor otherwise. |
| Disk space | About 16 GB for the work folder while it builds the character packs and one level, and about 1 GB of packs; the five levels' remodels and lightmaps add about 2 GB of packs and some GB of work folder. |
| Memory | 16 GB at least; a level remodel takes up to 28 GB on the largest levels. |

## Setting up

**Linux.** Unzip the game, then from its folder:

```
python3 -m venv ~/prism-venv
~/prism-venv/bin/pip install torch torchvision --index-url https://download.pytorch.org/whl/cu128   # NVIDIA; skip for the processor
~/prism-venv/bin/pip install -r share/openh1/prism_packs/requirements.txt
```

Install Blender 4.5 (unpack its archive anywhere) and Invader (your distribution's package, or built from its
source). Then fetch the upscalers. Each one's source, SHA-256 and licence is shown, and nothing is fetched until
you answer yes:

```
~/prism-venv/bin/python share/openh1/prism_packs/prism_packs.py models --data <data folder>
```

**Windows.** Install Python from python.org, Blender 4.5 and Invader's Windows release. The extractor is a Linux
program: install WSL (`wsl --install`, then restart), and copy `bin/openh1-map-extract` from the Linux download
next to `prism_packs\prism_packs.py`. The tools run it through `wsl.exe` on their own. From the game's folder:

```
py -3 -m venv %USERPROFILE%\prism-venv
%USERPROFILE%\prism-venv\Scripts\pip install torch torchvision --index-url https://download.pytorch.org/whl/cu128
%USERPROFILE%\prism-venv\Scripts\pip install -r prism_packs\requirements.txt
%USERPROFILE%\prism-venv\Scripts\python prism_packs\prism_packs.py models --data <data folder>
```

**Where the data folder is.** It is the folder with `maps/` in it. The game names it as it starts, in the line
`data root:` of its log (README.md, "The game's logs", says where the log is), and the `paths.data` setting can
move it.

## Building

```
python prism_packs.py check --data <data folder> --blender <blender> --invader <invader's folder>
python prism_packs.py build --data <data folder> --blender <blender> --invader <invader's folder>
```

`check` lists what this computer still lacks: maps, Python packages, Blender, Invader, the extractor, the
upscalers. It also says whether Blender found a graphics card. `build` builds every pack, or the ones named:

```
python prism_packs.py build characters --data ...                     # every character and texture pack
python prism_packs.py build levels --levels b30 --data ...            # b30's remodel and lightmaps
python prism_packs.py build chief_r3 rocks --data ...                 # these, and what they need first
python prism_packs.py list                                            # what each pack is
python prism_packs.py status --data ...                               # which are built
```

The tools say which step they are on. Each step's output goes to `<work>/logs/<pack>.log`. A build that stops,
whether you stopped it, it failed or the computer went off, goes on from where it was when you start the same
command again: finished steps are remembered in the work folder. `--again <pack>` builds that pack anew.

Options: `--out <folder>` writes the packs somewhere other than the data folder, in the same layout.
`--work <folder>` puts the work folder elsewhere (default `<data>/prism_packs_work`). `--gpu <part of its name>`
chooses the card Cycles uses, `--vulkan-gpu <number>` the upscaler's card and `--torch cpu|cuda` PyTorch's.
`--game` and `--extractor` name the game and the extractor if the tools do not find them beside themselves.

When everything is built, the work folder can be deleted. The packs do not need it. Keep it if you want to build
more levels later without extracting again.

## What it costs

These are long builds, all on your computer. Nothing runs on a server.

- The **character packs** are many Blender sessions. Each remodel is quick, but the
  surface maps are 4096-pixel Cycles bakes. On a graphics card these are fast. On the processor alone they are
  the longest part of the build, and they keep every core busy.
- The **texture packs** run five upscaling models over about forty maps, then a slow material pass per map. With
  a CUDA card this is moderate; on the processor alone expect it to take a long session.
- A **level's lightmaps** are the heaviest pack: every lightmap page of every part of the level is baked again
  at four times its size, at 1024 samples a texel. On a graphics card that takes a sitting; on a processor alone
  it can run through a night per level. `--lightmap-samples 256` makes a noisier bake in a fraction of the time.
- A **level's remodel** is moderate on a graphics card. It needs a lot of memory on the big outdoor levels.
- The game is started hidden, silent and offline a few times (for the models it draws, and for probes). Nothing
  appears on the screen.

## The packs

Names as `prism_packs.py` knows them, what each gives, and what turns it on in the game. Every row of PRISM
ASSETS is in Settings > MODS tab > PRISM ASSETS; the mods can be turned on and off in MANAGE MODS. The tools
write `mods/load_order.toml` with the packs in the order they were made for (Bold last).

| Pack | Writes | Gives | Turn it on |
| --- | --- | --- | --- |
| `class_masks` | `mods/prism_class_masks` | the material classes the overlay reads | with `materials` |
| `materials` | `mods/prism_materials` | fine grain (micro normal, cavity, roughness) on the characters and first-person weapons | MATERIAL DETAIL (`display.prism_material_overlay = true`) |
| `base_maps` | `mods/prism_base_local` | the characters' and first-person weapons' colour maps upscaled 2x or 4x | the mod being on |
| `chief_r2` | `mods/prism_v2r2_chief_3p_model` | the remodelled third-person Chief (round 2) | the mod being on; REMODELS STOCK (MAX draws round 3 in its place) |
| `chief_maps` | `mods/prism_v2r2_chief_3p_maps` | the Chief's maps at 4K, visor and lamps fixed, with his surface map | the mod; SURFACE DETAIL for the surface map (`display.prism_surface_maps = true`) |
| `ar_textures` | `mods/prism_v2r2_ar_textures` | the first-person Assault Rifle's maps at 4K | the mod being on |
| `fp_arms` | `mods/prism_v2r2_fp_hands_ar` | the first-person arms and Assault Rifle remodelled, with surface maps | the mod; SURFACE DETAIL |
| `elite_r2` | `mods/prism_v2_elite_r2` | the Elite and special Elite remodelled, with surface maps | the mod; SURFACE DETAIL |
| `bold` | `mods/prism_v2_tex_bold` | crisper material definition on the Chief, the arms and rifle, the Elites and the Marines | the mod being on (it loads last) |
| `chief_r3` | `mods/prism_r3_chief_meshes`, `mods/prism_r3_chief_materials` | the round 3 Chief (gloves, pouches, lamps, helper bones) and his PBR materials | REMODELS MAX (`display.prism_models = "max"`), PBR MATERIALS (`display.prism_materials = "pbr"`) |
| `cast` | `mods/prism_v3_cast`, `mods/prism_v3_elite_meshes` | the armoured Marine, Grunt and Jackal remodelled; the Elites stitched (left off: turn it on in MANAGE MODS to compare) | REMODELS MAX |
| `rocks` | `mods/prism_v3_rocks` | the campaign's rocks and boulders remodelled | REMODELS MAX |
| `world` | `prism_world/levels/<level>/<bsp>.pwg` | each level's remodelled geometry (a10, a30, a50, b30, c10) | WORLD GEOMETRY Remodel (`display.prism_world_geometry = "remodel"`) |
| `lightmaps` | `prism_lightmaps/rebaked/*.prl` | each level's lightmaps baked again from its own lamps and sky | LIGHTMAPS Rebaked (`display.prism_lightmaps = "rebaked"`) |
| `probes` | `prism/probes/<level>.probes` | local reflections (not in `all`: name it) | REFLECTIONS Local (`display.prism_reflections = "local"`) |
| `sun` | `prism_lightmaps/sun/*.psm` | sharp baked sun shadows (not in `all`) | not read by this build: its setting comes in a later one |

The QUALITY preset PRISM (`display.graphics = "prism"`) turns on surface maps, the smooth lightmaps and the
shadows, but not the packs' own rows above.

**On Windows**, REMODELS, PBR MATERIALS and TEXTURE DETAIL are not in the game's build yet, so `chief_r3`,
`cast` and `rocks` draw nothing there. Everything else applies.

## The upscalers and their licences

The texture packs use upscaling models. None of them is in the game or its repository. `prism_packs.py models`
fetches them from their authors' own pages when you say yes, checks each against the SHA-256 the tools were
written against, and records what it fetched in `<upscalers>/LICENSES.txt`.

| Download | From | Licence |
| --- | --- | --- |
| realesrgan-ncnn-vulkan 20220424, with its models x4plus, x4plus-anime and animevideov3 | Real-ESRGAN's release v0.2.5.0 (Xintao Wang, Tencent ARC Lab) | program MIT, models BSD-3-Clause |
| realesr-general-x4v3, realesr-general-wdn-x4v3 | Real-ESRGAN's release v0.2.5.0 | BSD-3-Clause |
| 4xNomos8kSC | Philip Hofmann, github.com/Phhofm/models | CC-BY-4.0: credit "4xNomos8kSC by Philip Hofmann (Phhofm)" |

`base_maps` needs the first; the texture tiers, and with them `chief_r3` and `bold`, need all four. The results
keep CE's own colours and layout: each upscale is corrected back toward the original (its low band is CE's), and
a map whose colours drift past the tools' checks is left out of its pack.

## Troubleshooting

- **"no maps in .../maps"**: `--data` must be the folder with `maps/` in it, after the game has extracted the maps
  at its first start.
- **"blender: not found"** or **"invader: ... not found"**: give `--blender <the blender program>` and `--invader
  <the folder with invader-model>`. Invader built from source keeps `libinvader` in `lib/` next to `bin/`; the
  tools find it there.
- **"Blender finds no graphics card"**: the bakes run on the processor. If you have a card, update its driver; on
  NVIDIA, check that `nvidia-smi` works and that CUDA programs run.
- **"upscalers: ... not in"**: run `prism_packs.py models --data ...`.
- **The extractor on Windows**: `wsl --status` must show a distribution, and `openh1-map-extract` must be next to
  `prism_packs.py` (or named with `--extractor`). On Linux it is a 32-bit static program: a kernel without 32-bit
  support (rare) cannot start it.
- **A step failed**: the tools print the end of that pack's log and its path. Fix the cause and run the same
  command again; it goes on from the failed step. `--again <pack>` starts that pack over.
- **A pack is refused in the game**: the game's log says so once for each map, in a line that begins `prism` or
  `mods:`. A model or bitmap the tools made is checked against every map's own before it is written, as the game
  checks it; a refusal there stops the build with the maps that refused it.
- **Out of memory in a level remodel**: close other programs, or build that level alone.

## What was and was not run

The tools were run end to end on Linux (x86-64, Blender 4.5.15, Invader 0.55, Python 3.12) on the author's own
maps: every character and texture pack, and b30's remodel and lightmaps (the lightmaps at
`--lightmap-samples 256`), with Cycles and PyTorch on the processor and the Vulkan upscaler on the graphics card.
The packs were loaded in a hidden run of the release build and drew as the author's
own packs do. The other levels' remodels and lightmaps, the sun masks and the probes use the same steps and were
not built in that run. **They have not
been run on Windows**: the Windows paths (WSL for the extractor, junctions for the hidden game runs, the Windows
upscaler) are written for it and not tried. macOS is not supported.

## For developers

`tools/prism_packs/` holds the entry point (`prism_packs.py`), the hidden game runs (`packs_game.py`), the
upscaler fetcher (`packs_fetch.py`) and the art scripts (`art/`), each of which can be run alone with the
folders of `art/packs_env.py` set. `art/` reads recipes: tag paths, names, shape and bake parameters, and
hand-placed feature positions on the game's texture layout. They hold no pixels, vertices or other data of the
game. The extractor's source is `tools/prism_packs/extract/`; a source checkout builds it with
`python tools/x86_game_abi.py --map-extract`. `tools/fork_gate.py` refuses a package with the game's data in it,
and a tools folder with anything but code, recipes and text in it.
