# Credits for ported code

Code taken from other projects into OpenH1, with the authors who wrote
it. Each file keeps its authors' notices and says where it came from; the
commits that brought it in name the project, the commit and the authors.
`AUTHORS.md` credits art.

## The Theater (replays)

| What | From | Authors | Licence |
| --- | --- | --- | --- |
| The replay recorder, its playback, its cameras (behind, eyes, free, cinematic), keyframes and seeking, the Theater screen, `tools/replay_stats.py` (`port/linux/game/network_replay.c`, `network_replay_playback.c`, `network_replay_store.c`, the hooks in `source/` and `port/linux/game/` marked as the replays', `port/linux/REPLAY.md`, `docs/theater_cinematic_camera.md`) | [SlipstreamCE](https://github.com/thelinkin3000/SlipstreamCE), `f8dbc4411` (its replay and Theater commits, `ad68cd784` to `7607fe9ff`) | thelinkin3000, Carlos Pérez | AGPL-3.0-or-later |
| The auto-director (`port/linux/game/spectate_director.c`), which chooses whom the Theater's DIRECTOR view watches | [NoahSurprenant/OpenCE](https://github.com/NoahSurprenant/OpenCE), branch `spectator-p5`, `333491574` | Noah Surprenant | AGPL-3.0-or-later |

OpenH1's changes to them (its sub-step ticks, its OpenH1 games' replays,
the files' paths, the records in the guest's memory, the screen in
Settings) are listed in `port/linux/REPLAY.md`, "In OpenH1".
