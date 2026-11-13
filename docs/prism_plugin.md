# PRISM's renderer plugins

PRISM v4 reaches vendor upscalers and denoisers (DLSS SR and RR, NRD, XeSS, FSR 4) through a C interface of its
own, `port/linux/vulkan/prism_plugin.h`. A plugin is a separately built shared library (`.so` on Linux, `.dll` on
Windows) that the renderer opens at run time when a setting names it. Without one, the renderer draws its own path,
and every PRISM gate is met without any plugin.

This document covers the ABI, the licences, the lifecycle and the rules, the tree's passthrough plugin and the
host's loader. The reasons behind the design are in the PRISM v4 design (sections 6.3 and 6.4).

## The licence split

- `prism_plugin.h` is offered under the MIT licence or the AGPL 3.0 or later (`SPDX-License-Identifier: MIT OR
  AGPL-3.0-or-later`). Plugin glue written against it can therefore be MIT and never needs to be AGPL.
- The rest of the game, the loader (`xgpu_plugin.c`) and the passthrough plugin included, is AGPL-3.0-or-later.
- No vendor's code or header is in the tree, and none may be. NVIDIA's RTX SDK licence forbids using its SDKs in a
  way that makes them subject to a licence such as the AGPL, so its headers cannot be in the game or in an
  AGPL-licensed plugin.
- Vendor plugins are built by the player, outside the tree (option A of the design's section 6.4): MIT glue in a
  separate repository, and a helper that downloads the vendor's SDK from the vendor, shows its licence, asks for
  consent and builds the plugin on the player's computer. No plugin binary is distributed with the game.

## The ABI

`PRISM_PLUGIN_ABI` is 1. A plugin exports one symbol, `prism_plugin_entry` (`PRISM_PLUGIN_ENTRY_SYMBOL`), of type
`prism_plugin_entry_function`: given the host's ABI it returns its table (`struct prism_plugin`), or NULL if it
cannot serve that ABI. The table holds:

- `info`: the ABI and size it was built with, its name (the one the setting names), vendor, version, a notice of
  the licences of what it contains, the text the credits screen shows for it, its capabilities
  (`PRISM_CAP_UPSCALE`, `_DENOISE`, `_DENOISE_UPSCALE`, `_FRAME_GENERATE`), and the sizes of the Vulkan handover,
  an image and a frame it was built with;
- the calls, each returning 1 on success and 0 on failure: `instance_extensions`, `device_extensions`,
  `supported`, `create`, `resize`, `evaluate`, `destroy` (no result) and `last_error` (why the last call failed).

`struct prism_plugin_frame` hands over a frame: the command buffer to record into (the host submits it), the colour
in and out, and the guides (depth, motion, normal and roughness, the albedos, the specular hit distance, the
demodulated radiance and the sun's visibility for a denoiser, the reactive and transparency masks), with the jitter,
exposure, matrices, reset flag and sizes. Every image is given with the layout it is in; the plugin's commands must
leave each image in that layout.

## The lifecycle

1. Before the host makes its Vulkan instance and device: `instance_extensions` and `device_extensions` (a vendor
   may need extensions enabled at creation), each called first for the count and then for the names; then
   `supported` for the device chosen.
2. After the device is made: `create`, with the instance, device, the loader's functions and the queue the host
   submits on.
3. `resize` before the first frame and whenever the render or output size or the quality changes, with the host's
   queue idle.
4. `evaluate` once a frame.
5. `destroy` at the end, with the queue idle.

## The rules

- Nothing is opened unless a setting names a plugin (`display.upscaler = "plugin:<name>"` or
  `display.denoiser = "plugin:<name>"`). An empty name opens no file.
- The host checks the plugin before using it: a plain file name, the library opened, the entry found, a table
  given for the host's ABI, the info of that ABI and no smaller than the host's, the sizes of the structures it
  expects no larger than the host's, every call and every string present, at least one capability. A failure
  closes the library and logs one line saying why; a missing plugin is one line too.
- A call that fails at run time turns the plugin off for the session, with its `last_error` in the log, and the
  renderer draws its own path in its place.
- The credits screen shows each loaded plugin's `credits_text`.

## The passthrough plugin

`port/linux/vulkan/plugins/passthrough/prism_plugin_passthrough.c` is the tree's own plugin. It is an "upscaler"
that copies the colour in to the colour out: byte for byte with `vkCmdCopyImage` where the sizes and formats are
the same, else stretched to the nearest texel with `vkCmdBlitImage`. It exists to exercise the ABI: with it
selected, the picture must be byte-identical to the upscaler off. It is built on its own as a shared library:

```sh
cc -shared -fPIC -fvisibility=hidden -I build/linux_x86_64/third_party/vulkan-headers -I port/linux/vulkan \
    port/linux/vulkan/plugins/passthrough/prism_plugin_passthrough.c -o passthrough.so
```

## The loader

`port/linux/vulkan/plugins/loader/xgpu_plugin.c` (`xgpu_plugin_open(folder, name, log, data)`,
`xgpu_plugin_close`) opens `<folder>/<name>.so` with `dlopen` (`<name>.dll` with `LoadLibraryW` on Windows), runs
the checks above, and logs through a function the caller passes. It is not yet part of the game's build or called
by the renderer: it sits outside `port/linux/vulkan` itself, every `.c` file of which the hosts compile, until the
renderer's settings and its use come with PRISM v4's milestone M5, where the plugins' folder is `<data>/plugins/`.

## Tests

```sh
python -m pytest -q tools/test_prism_plugin.py
```

It builds the passthrough plugin and several broken ones, checks that the passthrough exports its entry alone,
records its commands over a fake device (one copy for equal sizes, one blit otherwise, every image left in its
layout), checks that each broken plugin, a missing one and a wrong ABI are refused with a logged reason, and that no
name touches no file. It needs a C compiler, `nm` and Vulkan's headers (the build's or the system's), and no GPU or
game data.
