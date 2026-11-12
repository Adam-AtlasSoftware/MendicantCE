# Credits for ported code

Code OpenH1 took from community projects, with the authors who wrote it,
each kept under its licence and with its notices (AGENTS.md, "Third-party
code and licences"). Each file says where it came from; the commits that
brought it in name the project, the commit and the authors. `AUTHORS.md`
credits the art.

## The Theater (replays)

| What | From | Authors | Licence |
| --- | --- | --- | --- |
| The replay recorder, its playback, its cameras (behind, eyes, free, cinematic), keyframes and seeking, the Theater screen, `tools/replay_stats.py` (`port/linux/game/network_replay.c`, `network_replay_playback.c`, `network_replay_store.c`, the hooks in `source/` and `port/linux/game/` marked as the replays', `port/linux/REPLAY.md`, `docs/theater_cinematic_camera.md`) | [SlipstreamCE](https://github.com/thelinkin3000/SlipstreamCE), `f8dbc4411` (its replay and Theater commits, `ad68cd784` to `7607fe9ff`) | thelinkin3000, Carlos Pérez | AGPL-3.0-or-later |
| The auto-director (`port/linux/game/spectate_director.c`), which chooses whom the Theater's DIRECTOR view watches | [NoahSurprenant/OpenCE](https://github.com/NoahSurprenant/OpenCE), branch `spectator-p5`, `333491574` | Noah Surprenant | AGPL-3.0-or-later |

OpenH1's changes to them (its sub-step ticks, its OpenH1 games' replays,
the files' paths, the records in the guest's memory, the screen in
Settings) are listed in `port/linux/REPLAY.md`, "In OpenH1".

## Tag packs and the tag importer

By **Sergio Villanueva (Watanoge)**, from
[Watanoge/halo-content-evolved](https://github.com/Watanoge/halo-content-evolved),
branch `opence_floodfection`, commit `09232e0ce` (OpenCE pull request #523),
AGPL-3.0-or-later. Taken whole with `git cherry-pick -x`, its authorship
kept: `port/linux/game/tag_import.c`, `tag_pack.c`, `tag_pack_conditions.c`,
`level_tag_import.c`, `port/linux/src/port_tag_packs.c`, `tools/tag_pack.py`,
`port/linux/TAG_INJECTION.md`; `tools/test_tag_pack.py` from `c5c0e3c86`.

## Forge

By **SaftiGlas**, from
[SaftiGlas/halo-ce-universal](https://github.com/SaftiGlas/halo-ce-universal),
branch `main`, commit `173fc93ae`, AGPL-3.0-or-later: the in-game map editor
(`port/linux/game/forge*.c`, `halo_mods.c`, `gravity.c`,
`port/linux/include/halo_forge.h`, `halo_mod.h`), its flying camera and keys
(merged into `source/camera/director.c` and `port/linux/src/xinput_sdl.c`
from its `director_forge.c` and `xinput_sdl_forge.c`), the `.mvar` format
(`docs/forge_mvar.md`), its menus' functions and its harness tests
(`tools/harness/tests/*forge*`). Its own importer of the monitor's tags is
replaced by Watanoge's tag packs (`forge_tag_import.c`); its sky tab
(`sky.c`, `skyfx.c`, OpenGL) is not ported.

## PRISM's pack tools

`tools/prism_packs` (the pack tools, `docs/prism_packs.md`) is this fork's own code, AGPL-3.0-or-later,
like the rest: the art scripts, recipes and entry point by Adam Wiley, and the extractor
(`tools/prism_packs/extract`: `map_extract.c` and its lights command, written for an OpenCE checkout and
moved here; its texture decoder is OpenCE's, split out of `xbox_textures.c`). It contains no game data and no
code of other projects. The Linux package's `bin/openh1-map-extract` is that extractor, linked statically
with the GNU C Library (LGPL-2.1-or-later; its notice and licence are in the package's licences).

What the tools use and do not carry. Each is installed or fetched by the player, from its own authors:

| What | Licence | How the tools use it |
| --- | --- | --- |
| [Blender](https://www.blender.org) 4.5 LTS | GPL-2.0-or-later (GPL-3.0 as built) | runs the remodel and bake scripts headless; the player installs it |
| [Invader](https://github.com/SnowyMouse/invader) (Snowy Mouse) | GPL-3.0 | its programs compile and extract tags; the player installs it |
| Python packages: NumPy, SciPy, scikit-image (BSD-3-Clause), Pillow (MIT-CMU), opencv-python-headless (Apache-2.0, MIT), PyTorch and torchvision (BSD-3-Clause), [spandrel](https://github.com/chaiNNer-org/spandrel) (MIT) | as named | imported; the player installs them (`requirements.txt`) |
| [Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN) (Xintao Wang, Liangbin Xie, Chao Dong, Ying Shan; Tencent ARC Lab): realesrgan-ncnn-vulkan 20220424 and the models x4plus, x4plus-anime, animevideov3, realesr-general-x4v3, realesr-general-wdn-x4v3 | program MIT, code and models BSD-3-Clause | fetched from the project's release v0.2.5.0 when the player says yes, its licence shown first (`prism_packs.py models`) |
| 4xNomos8kSC, by Philip Hofmann ([Phhofm/models](https://github.com/Phhofm/models)) | CC-BY-4.0 (credit required) | fetched from its author's page when the player says yes, its licence and credit shown first |

The methods the scripts follow are named in their comments: the guided filter (He, Sun and Tang), CIEDE2000
colour difference, MikkTSpace tangents (Blender's), and Lanczos and box resampling.
