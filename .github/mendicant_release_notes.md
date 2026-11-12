MendicantCE is a modified version of [OpenH1](https://github.com/OpenH1/OpenH1) (Halo: Combat Evolved for the Xbox, ported natively), changed by Adam Wiley. **It is not OpenH1's build**: OpenH1's developers did not make it and do not support it, so do not report its problems to OpenH1's tracker or Discord: they go to [this repository's issues](https://github.com/Adam-AtlasSoftware/MendicantCE/issues), which its owner has to have opened (GitHub shows no Issues tab until then).

Source of this build: commit [`@SHORT@`](https://github.com/Adam-AtlasSoftware/MendicantCE/tree/@COMMIT@) of the branch `mendicant-prism` (`SOURCE.txt` in each zip says the same). Licence: AGPL-3.0-or-later. No game data is in the downloads: the game plays from your own copy of Halo: Combat Evolved for the Xbox.

## Downloads

| File | For | How |
| --- | --- | --- |
| `MendicantCE-PRISM-linux-x86_64.zip` | Linux, x86-64, glibc 2.35 or later, Vulkan 1.3 | Unzip into a **new folder of its own** and start `bin/openh1`. There is no installer. Do not unzip it into `~/Applications/OpenH1` or over an OpenH1. |
| `MendicantCE-PRISM-windows-x64.zip` | Windows, x86-64, Vulkan 1.3 | Unzip into a **new folder of its own** and start `halo.exe`. Windows may say it protected your PC (the program is not signed): More info, then Run anyway. |
| `SHA256SUMS` | | `sha256sum -c SHA256SUMS` checks the two. |

`README.md` in each zip says the rest; the main points are below.

## What it adds to OpenH1, all off until you turn it on

- **MENDICANT AI** (the campaign's Marines and Covenant fight as led units, with trained models): **off in this build**. Turn it on under Settings › Profiles › CAMPAIGN AI › MENDICANT AI; it applies from the next map start, on the host of a campaign or co-op game. The trained models come with the build. This differs from the earlier `mendicant-2` and `mendicant-7` releases, which were on another base, had it on by default and had the setting under Player Profile › Video Setup. Do not take a checkpoint saved mid-fight with it on to OpenH1.
- **PRISM** (Settings › Mods › PRISM GRAPHICS, PRISM ASSETS, PRISM RAY TRACING). Works with the game's own data: highlights, sun shadows (and soft and traced with ambient occlusion World), smooth lightmaps, shade depth, bloom, tone, and ray tracing on graphics cards with ray queries.
- **Not in any download**, because they are made from the game's data: PRISM's asset packs. Local reflections need probe files, rebaked lightmaps need rebaked pages, the world remodel needs remodel files, and every row of PRISM ASSETS (texture detail, PBR materials, remodels, material and surface detail) needs a mod that carries the files. Without them those settings draw as the stock game. No pack is published.
- **Theater** (Settings › Mods › RECORD GAMES, THEATER): replays of your games. Voice chat is not recorded.
- **Forge** (`game.forge` in `config.toml`) and **tag packs**.

## Different from OpenH1 on purpose

- It **does not update itself** and never looks at OpenH1's releases; new builds are on this page.
- It **sends no crash reports**.
- **Discord** status and Join are off unless you set your own application's id (`discord.application_id`).
- It does not take `openh1://` links: join an invite by copying its link, or with JOIN BY LINK.
- About, `version` and what a host is told name MendicantCE and this commit.
- A game it hosts tells players of other builds, once, that the server runs a modified OpenH1 and where its source is.

## Known limits

- **The Windows build has not been played.** CI compiles it and starts it without game data or a graphics card, as far as "no maps found"; its window, Vulkan, sound, input, the disc image's file picker, MENDICANT AI and PRISM have not been run on Windows. The Linux build was run on the author's computer, in the menus and in campaign levels.
- **Ray tracing** was measured on two NVIDIA cards on Linux, and on no other maker's cards.
- A PRISM setting that needs a pack and finds none draws the stock game; `halo.log` says so once a map (`prism ...: drawn stock`).
- **Windows:** TEXTURE DETAIL, TEXTURE MEMORY, PBR MATERIALS, REMODELS, MODEL MEMORY and SECONDARY MOTION are not available yet (greyed out), with or without a pack.
- Linux shares the saved games' folder `~/.local/share/halo-linux` with an OpenH1 on the same computer (`paths.saves` moves it).
- Internet games it hosts are public by default and use the same brokers and network version as OpenH1's, so OpenH1 and OpenCE players can join them, at 30 ticks.
- World ambient occlusion is a little lighter than OpenH1's (SHADE DEPTH, new, 0.8 by default).
- If the game does not start on a driver where OpenH1 does, set `HALO_VULKAN_NO_RT=1`.
- Variants saved by earlier Forge builds after moving an AI character hold an inert copy of it: remove the copies with the tools and save again.
