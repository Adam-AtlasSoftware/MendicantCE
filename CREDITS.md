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
