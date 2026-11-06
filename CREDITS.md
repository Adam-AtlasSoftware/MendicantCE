# Credits

Code OpenH1 took from community projects, each kept under its licence and
with its notices (AGENTS.md, "Third-party code and licences"). `AUTHORS.md`
credits the art.

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
