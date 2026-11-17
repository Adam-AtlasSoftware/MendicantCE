# The speedrun state block

LiveSplit's Halo autosplitters read fixed addresses, which move with every GOAT H1 build. Instead, the game keeps one
small block in its memory that a script finds by its signature, then reads at fixed offsets. It needs no sockets or
files. The code is `port/linux/game/speedrun_state.c`; the layout's header is `port/linux/game/speedrun_state.h`;
`docs/speedrun_state.asl` is a sample LiveSplit script to adapt.

## Finding it

Scan the game's process memory for the 16 bytes `GOAT H1 SPEEDRUN` (`47 4F 41 54 20 48 31 20 53 50 45 45 44 52 55
4E`, no NUL). The block starts there. Check `version` (1) and `size` before reading further. The magic is written
by the game's first frame, so a scan before then finds nothing: retry. The executable never holds the magic whole,
so the scan finds only the live block.

## Layout (version 1)

Little-endian. `u32` is a 32-bit unsigned number. The strings end with a NUL.

| Offset | Field | Type | Meaning |
| --- | --- | --- | --- |
| 0x00 | magic | char[16] | `GOAT H1 SPEEDRUN` |
| 0x10 | version | u32 | 1. Changes when a field moves or changes meaning |
| 0x14 | size | u32 | The block's size. Fields added at the end keep the version and grow it |
| 0x18 | sequence | u32 | Odd while the game writes the block. Read it again if it was odd or changed |
| 0x1C | frame | u32 | Main loop frames since the game started (a heartbeat) |
| 0x20 | flags | u32 | 1 loading, 2 cutscene, 4 main menu, 8 in game |
| 0x24 | run | u32 | Runs begun since the game started |
| 0x28 | run_ticks | u32 | Ticks of the current run, 30 a second |
| 0x2C | levels_completed | u32 | Levels won in the current run |
| 0x30 | game_ticks | u32 | The level's own clock. A checkpoint revert takes it back |
| 0x34 | ticks_per_second | u32 | 30 |
| 0x38 | sim_rate | u32 | `game.sim_rate` in effect: 30, 60 or 120 |
| 0x3C | settings | u32 | 1 Enhanced graphics, 2 Custom graphics (neither: Classic), 4 enhanced animations, 8 mods change the map, 16 Custom Edition map, 32 MCC map, 64 PRACTICE mark |
| 0x40 | build_number | u32 | The release's build number, 0 for a local build |
| 0x44 | build | char[44] | The build's label, such as `GOAT H1 \| build 123 (release)` |
| 0x70 | map | char[64] | The map played, such as `levels\a10\a10` |
| 0xB0 | (end) | | |

## What the fields mean

- **Loading** is on from the moment a map starts to load (or the old one to be disposed of) until the new map's
  first picture. With no display (a dedicated server, tests), it ends at the map's first tick instead.
- **Cutscene** follows the game's own cutscene state.
- **run_ticks** counts the ticks the game actually runs. A checkpoint revert or a core load takes the level's clock
  (`game_ticks`) back, but takes nothing from `run_ticks`, and ticks played again after a revert count again.
  Pause, loads and skipped cutscenes run no ticks, so they add none. The main menu's ticks never count.
- **A run begins** when a map other than the main menu loads from the main menu (a new game, a mission chosen, a
  saved game resumed), or when the map being played loads again (a restart). `run` goes up, and `run_ticks` and
  `levels_completed` go back to 0. Winning a level that leads to the next keeps the run going. Back at the main
  menu, the run's numbers stay as they ended.
- **levels_completed** goes up as each level is won, the last one (The Maw) included.
- **settings** and the build are read as each map begins, as the game reads its rules then, but for the PRACTICE
  mark (64), set each frame: on from the first use of a practice key, a cheat, `game_speed`, a core's load or
  `debug.skip_cinematics` (`port/linux/game/practice.c`), off only at the level's next fresh start (a map loaded or
  started over). The HUD shows it as PRACTICE and demos record it. **sim_rate** is the
  rate the game's next tick simulates at, each frame (30 in a game that must stay as it was, such as a Legacy
  network game or a saved film).

The game writes the block from its main loop only, with plain stores. Nothing of the game's simulation reads it
back (the state file's writer and the practice keys read its flags), and it is
not part of the game state, so saved games, reverts and demos neither keep nor change it, and games play the same
with it as without.

## Where it lives, and who can read it

The game is a 32-bit guest inside a 64-bit host process (`halo` on Linux, `halo.exe` on Windows). The block is in
the guest's memory (the guest image's zeroed data), which is the host process's own memory, so a reader scans that
process.

- **Windows**: the guest's memory is reserved below 4 GB at fixed addresses, and guest addresses are process
  addresses. LiveSplit reads it with ordinary process reads. The guest image is not a Windows module, so scan the
  process's memory pages, not `game.MainModule` (the sample does). If something already held those addresses at
  start-up, `halo.exe` starts a second copy of itself and waits for it: the game then runs in the second process,
  and the first holds no block.
- **Linux**: on x86-64 the guest's addresses are the process's own (on 64-bit ARM, with the base register, the base
  plus them). A reader running as the same user reads `/proc/<pid>/mem` or calls `process_vm_readv`. Where Yama's
  `ptrace_scope` is 1 (Ubuntu's and others' default), only a parent of the game, or a reader with `CAP_SYS_PTRACE`,
  may read it. `tools/speedrun_state_read.py` is a small reader.
- **macOS**: the signed app has the hardened runtime and no `get-task-allow`, so other processes are very likely
  refused its memory (`task_for_pid`). Treat macOS as unsupported for external readers.
- **Android and iOS**: no external readers.

## The sample script

`docs/speedrun_state.asl` finds the block, then:

- **start**: when the run's first tick runs (`run_ticks` goes from 0 to more).
- **split**: when `levels_completed` goes up.
- **isLoading**: while the loading flag is on (for load-removed time).
- **reset**: when `run` changes (a new run began).

To time by the game's ticks instead, uncomment its `gameTime`, which turns `run_ticks` into time.

## The state file, for stream overlays

With `display.state_file` on (off by default; Settings > Interface > Streaming & Speedrun), the game also writes
what this block says, and the first player's score, kills and deaths, to `game_state.json` in the data root
(`port/linux/game/stream_state.c`), for OBS text sources and scene switchers:

```json
{"state":"playing","map":"levels\\a10\\a10","mode":"campaign","clock":95,"score":0,"kills":3,"deaths":1,"practice":false}
```

`state` is `loading`, `menu`, `cutscene`, `paused` or `playing`; `clock` is the level's clock in seconds. The
file is written only when it changes, at most four times a second, to `game_state.json.tmp` and then renamed
over the file, so a reader never sees half of it. Nothing is sent over the network.
