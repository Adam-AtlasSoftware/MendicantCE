# MendicantCE

MendicantCE is a modified version of [OpenH1](https://github.com/OpenH1/OpenH1),
the native port of Halo: Combat Evolved (the Xbox game) made from its
decompilation. It was changed by Adam Wiley in November 2026, starting from
OpenH1's commit `afa1611ce`; `SOURCE.txt`, which comes with every build, names
the commit a build was made from and its date.

**It is not OpenH1's build.** OpenH1's developers did not make it and do not
support it. Do not report its problems to OpenH1's issue tracker or Discord,
and do not expect OpenH1's downloads, installer, Scoop bucket or updates to
give you this build: they give you OpenH1.

- Source, releases and this file: <https://github.com/Adam-AtlasSoftware/MendicantCE>,
  branch `mendicant-prism`.
- Licence: `AGPL-3.0-or-later`, as OpenH1's ([LICENSE](LICENSE)). No warranty.
- The game's data is not included, in the repository or in any build. The
  game plays from your own copy of Halo: Combat Evolved for the Xbox, as
  OpenH1 does ("Game data" in OpenH1's README, [README.md](README.md) here,
  `README.OpenH1.md` in a build).

The rest of this repository's documents are OpenH1's, kept as they are so
that its changes can still be merged: its README, `port/linux/README.md`
(`README.linux.md` in a build), `port/windows/README.md`, the website. Where
they speak of downloads, installing, updates, build numbers, crash reports,
Discord and where to get help, they describe OpenH1's releases. What follows
says what is different in this build.

## What it adds

Everything it adds is off until you turn it on: a fresh build with its
settings as they come plays as OpenH1 does.

| | What | Where |
| --- | --- | --- |
| **MENDICANT AI** | The campaign's Marines and Covenant fight as units, led through drills, with trained models choosing the drills and each soldier's way of fighting; and an "authenticity layer": soldier character, firing personality, reactions to grenades, convoys, spacing, vehicle lanes | Settings › Profiles › CAMPAIGN AI › MENDICANT AI. From the next map start; on the host of a campaign or co-op game |
| **PRISM** | Graphics beyond the Xbox's, drawn by the Vulkan renderer: highlights, sun shadows, smoother lightmaps, tone, bloom, and ray traced shadows and light on cards that have ray queries | Settings › Mods › PRISM GRAPHICS, PRISM ASSETS, PRISM RAY TRACING |
| **Theater** | Games recorded as replays and watched again, with free and cinematic cameras | Settings › Mods › RECORD GAMES and THEATER |
| **Forge** | An in-game map editor for multiplayer maps, with AI characters | `game.forge` in `config.toml`, then FORGE on the main menu |
| **Tag packs** | Tags of one map brought into another, built on your computer from your own maps | `port/linux/TAG_INJECTION.md` |

Who wrote the Theater, Forge and the tag importer, and under which licences:
[CREDITS.md](CREDITS.md).

### MENDICANT AI

- It is **off** in this build (`ai.mendicant.enabled = false`). The earlier
  `mendicant-2` and `mendicant-7` releases, which were built on another base,
  had it on, and had the setting under Player Profile › Video Setup.
- The trained models come with the build (`mendicant_models.aibw`, beside the
  game's other files), so TRAINED MODELS on plays with them. A mod's
  `tags/mendicant/models.aibw` takes their place. The game's log says which
  file was taken, or that none was found (then the scripted drill decides,
  and only the Covenant are led).
- A checkpoint saved mid-fight with it on should not be loaded in OpenH1.

### PRISM: what works in a download, and what needs files no download has

Works with the game's own data, nothing else needed: HIGHLIGHTS, SHADOWS
(Sun; Soft and Traced with ambient occlusion World), SHADOW DETAIL, SHADOW
SOFTNESS, LIGHTMAPS Smooth, SHADE DEPTH, BLOOM, TONE, and PRISM RAY TRACING
(greyed out where the graphics card or its driver has no ray queries).

Needs files that are made from the game's data, and so are in no download
and not in the repository:

| Setting | Needs |
| --- | --- |
| REFLECTIONS Local | `<data>/prism/probes/<map>.probes`, made by `tools/prism_probes.py` from a source checkout on Linux |
| LIGHTMAPS Rebaked | `<data>/prism_lightmaps/rebaked/*.prl`; without them it draws as Smooth |
| WORLD GEOMETRY Remodel, GEOMETRY DETAIL, GEOMETRY SHADOWS | `<data>/prism_world/<bsp>.pwg`; a level without one is drawn as the Xbox's |
| Every row of PRISM ASSETS (material and surface detail, texture detail, PBR materials, remodels) | a mod with `textures/`, `meshes/`, `materials/`, a material overlay or surface maps ([docs/mods.md](docs/mods.md)) |

No such pack is published. The formats are documented (`docs/mods.md`) and
the tools that read and check them are in `tools/`; the tools that built the
author's own packs from the game's data are not part of this repository.

**On Windows**, TEXTURE DETAIL, TEXTURE MEMORY, PBR MATERIALS, REMODELS, MODEL
MEMORY and SECONDARY MOTION are not available yet, with or without a pack:
their rows are greyed out and the log says so. The rest of PRISM is the same
as on Linux.

World ambient occlusion (an OpenH1 setting) is a little lighter here than in
OpenH1: `display.ambient_occlusion_strength` (SHADE DEPTH) is new, and 0.8 by
default.

The Vulkan device is made with the ray tracing extensions wherever the
driver has them, also with ray tracing off. If this build does not start on
a driver on which OpenH1 does, set `HALO_VULKAN_NO_RT=1` in the environment:
the extensions are then not asked for.

## Getting and starting it

Builds are on <https://github.com/Adam-AtlasSoftware/MendicantCE/releases>,
for Linux (x86-64) and Windows (x86-64). Each needs a graphics card and
driver with Vulkan 1.3.

- **Linux:** unzip into a new folder of its own and start `bin/openh1`.
  There is no installer. Do **not** unzip it into `~/Applications/OpenH1` or
  into a folder that holds OpenH1: the files have the same names. The build
  keeps its settings (`config.toml`) in its own folder. It is built on
  Ubuntu 22.04 and needs glibc 2.35 or later.
- **Windows:** unzip into a new folder of its own and start `halo.exe`.
  Windows may say that it protected your PC, as it does of any program that
  is not signed: More info, then Run anyway.

At its first start the game asks for your copy of the game's data, as OpenH1
does.

What this build does differently from an OpenH1 build, on purpose:

- **It does not update itself.** New builds are on the releases page above.
  It never looks at OpenH1's releases, as a game or as a dedicated server,
  and the Check for Updates setting is gone.
- **It sends no crash reports**, anywhere. A crash leaves `halo.log` and
  `debug.txt` on your computer.
- **Discord status and Join are off** unless you put your own Discord
  application's id in `discord.application_id`: it does not use OpenH1's
  application.
- **It does not take `openh1://` links.** Join an invite by copying its link
  (the game joins the link on the clipboard when it comes to the front) or
  with JOIN BY LINK.
- **Its name.** Settings › Profiles › About, the `version` command and what a
  host is told say MendicantCE and the commit, not OpenH1 and a build number.
- **A host says what it runs.** Players of other builds who join a game this
  build hosts are told once, in the text chat, that the server runs
  MendicantCE, a modified OpenH1, and where its source is
  (`network.opence_welcome = false` turns it off; if you host for others over
  a network, the licence asks you to offer them the source).

What it still shares with an OpenH1 on the same computer:

- on Linux, the folder of saved games, profiles and caches,
  `~/.local/share/halo-linux` (`paths.saves` in `config.toml` names another:
  set it if you keep both and want their saves apart);
- internet play: the same brokers and server browser, and the same network
  version, so its players and OpenH1's meet. Games this build hosts are
  public by default, as OpenH1's are. OpenH1's current builds play a newer
  version of its Standard game, so they join a game of this build as OpenCE
  players do, at 30 ticks; messages that say "Update OpenH1" then mean that
  the other game is newer than this build.

## Building it

As OpenH1 ("Build the game" in [README.md](README.md)). The fork's own switch,
`HALO_FORK_MENDICANT`, is in every build (`tools/build_common.py`); a release
is built by `.github/workflows/mendicant.yml`, which also checks that a
package cannot update itself or pass for OpenH1 (`tools/fork_gate.py`).

The design notes of MENDICANT AI are in
[docs/mendicant_port.md](docs/mendicant_port.md), PRISM's ray tracing in
[docs/prism_rt.md](docs/prism_rt.md), the Theater in
[port/linux/REPLAY.md](port/linux/REPLAY.md).
