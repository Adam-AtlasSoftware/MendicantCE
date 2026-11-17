# PRISM's renderer plugins and frame upscaling

PRISM v4 reaches vendor upscalers and denoisers (DLSS SR and RR, NRD, XeSS, FSR 4) through a C interface of its
own, `port/linux/vulkan/prism_plugin.h`. A plugin is a separately built shared library (`.so` on Linux, `.dll` on
Windows) that the renderer opens at run time when a setting names it. Without one, the renderer draws its own path,
and every PRISM gate is met without any plugin: FidelityFX Super Resolution 3.1 is built in.

This document covers the ABI, the licences, the lifecycle and the rules, the tree's passthrough plugin, the host's
loader, the settings, the renderer's frame upscaling (the view drawn smaller, jittered, its inputs, FSR 3.1 or a
plugin) and the denoiser's entry points, the tests and the measurements. The reasons behind the design are in the
PRISM v4 design (sections 6 and 7). The plugins' glue lives outside this tree, in its own repository
(`prism-plugins`, MIT): DLSS's is there, with the tool that builds it on the player's computer.

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
- Each loaded plugin's `credits_text` is logged as it starts (`prism plugin <name>: ready`); the credits screen
  showing it is to come.

## The passthrough plugin

`port/linux/vulkan/plugins/passthrough/prism_plugin_passthrough.c` is the tree's own plugin. It is an "upscaler"
that copies the colour in to the colour out: byte for byte with `vkCmdCopyImage` where the sizes and formats are
the same, else stretched to the nearest texel with `vkCmdBlitImage`; and a "denoiser" that leaves the light it is
given as it is. It exists to exercise the ABI: with it as the upscaler at the screen's size and the jitter off
(`display.render_scale = 1`, `HALO_UPSCALE_JITTER=0`), the picture must be byte-identical to the upscaler off; as the
denoiser, the path tracer's light is its own filter off. It is built on its own as a shared library:

```sh
cc -shared -fPIC -fvisibility=hidden -I build/linux_x86_64/third_party/vulkan-headers -I port/linux/vulkan \
    port/linux/vulkan/plugins/passthrough/prism_plugin_passthrough.c -o passthrough.so
```

## The loader

`port/linux/vulkan/xgpu_plugin.c` (`xgpu_plugin_open(folder, name, log, data)`, `xgpu_plugin_close`) opens
`<folder>/<name>.so` with `dlopen` (`<name>.dll` with `LoadLibraryW` on Windows), runs the checks above, and logs
through a function the caller passes. It is part of the renderer of every Vulkan host. The guest names the plugins
before its window opens (`host_vulkan_plugins_choose`, a host import beside `host_vulkan_device_choose`):
display.upscaler's and display.denoiser's `"plugin:<name>"` names, and the folder, the data folder's `plugins`
(`HALO_PRISM_PLUGINS` names another). With no plugin named, nothing is opened and nothing is asked of the device.

`port/linux/vulkan/xgpu_vulkan_upscale.c` drives the lifecycle:

1. Before the instance: each plugin opened, its instance extensions asked for; the core (`xgpu_vulkan.c`) adds
   those the loader has (`xgpu_vulkan_options`' `plugin_instance_extensions`).
2. Before the device, for the device chosen: each plugin's device extensions, then `supported`; the core adds those
   the device has (`plugin_device_extensions`). An extension the loader or the device lacks turns its plugin off
   (`plugin_missing`), logged: NGX's ask for `VK_NVX_binary_import`, `VK_NVX_image_view_handle`,
   `VK_EXT_buffer_device_address` and `VK_KHR_push_descriptor` on an NVIDIA card's device, and
   `VK_KHR_get_physical_device_properties2` on the instance.
3. After the device: `create` with the instance, device, the loader's functions and the queue.
4. `resize` before the first frame and whenever the render or output size or the quality changes (the quality the
   ratio of the sizes: 0.67 and more Quality, 0.58 Balanced, 0.5 Performance, less Ultra Performance, 1 Native),
   with the queue idle (the work so far submitted and waited for).
5. `evaluate` at each view's end (below), in the frame's commands.
6. `destroy` as the game exits, before the device.

The same library serves both settings where both name it (one context). A plugin that fails any call is turned off
for the session with its reason, and FSR 3.1 (an upscaler's place) or the path tracer's own filter (a denoiser's)
is drawn instead.

## The settings

| Setting | Values | What it does |
| --- | --- | --- |
| `display.upscaler` | `"off"` (default), `"fsr3"`, `"plugin:<name>"` | The 3D view drawn smaller and upscaled before the HUD: FSR 3.1, built in, or a plugin opened as the game starts |
| `display.render_scale` | `0.33` to `1.0`; `0` (default) is `0.67` | The share of the screen's pixels the view is drawn at each way, under an upscaler (iOS: its own meaning, the whole game's) |
| `display.upscaler_sharpness` | `0` (default) to `1` | FSR's RCAS sharpening; a plugin's `sharpness` |
| `display.denoiser` | `"prism"` (default), `"plugin:<name>"`, `"off"` | What filters the path tracer's light: its own filter, a plugin, or none |

Each has its environment variable (`HALO_UPSCALER`, `HALO_RENDER_SCALE`, `HALO_UPSCALER_SHARPNESS`,
`HALO_DENOISER`) and the console's form (`prism_set "display.upscaler fsr3"`), and all four are on Settings'
PRISM LIGHTING page (UPSCALING). A plugin is opened as the game starts: choosing one in the menus takes effect at the
next start. Under an upscaler there is no multisampling or supersampling (`display.anti_aliasing`'s `msaa*` and
`ssaa2x` are drawn as off) and no FXAA or SMAA after it: the upscaler is the view's anti-aliasing.

## The frame upscaling

A player's window's frame (its mirror's view and its own) is drawn at the render scale and its 3D view upscaled into
the window as it is whole and tone mapped, before its HUD (`port/linux/game/prism_upscale.c`; `source/render/
render.c`'s hooks: the window's frame begins, its opaque surfaces end, its view ends). The guest's half
(`port/linux/src/d3d8_device.c`, `halo_upscale_view_*`): while the view is drawn the screen's scale is the view's
(the screen's times the ratio), so that every target of the screen's the view draws (the back buffer, its depth
buffer, the secondary target of the mirror's and active camouflage's views, the screen effects' copies) is an entry
of that scale, a target of the backend's of its own, and the game's viewports, clears, quads and texture coordinates
scale with it as they do with supersampling's; the HUD's targets are the screen's own. The maps' textures are
sampled with a mip bias of log2 of the ratio meanwhile (as sharp as the screen's pixels would sample them), and the
models' detail is measured at the screen's scale. One player's window only (split screen's are drawn as without), not
the VR build's, nor a view the replay holds or a movie plays over.

The stream's record, `UPSCALE` (`xgpu_stream.h`; version 35), in three phases:

- **BEGIN**, as the frame's views begin: the view's colour and depth targets. The backend jitters every draw into a
  target of the view's size from here on (the view, its G-buffer, its passes' targets; not the shadow maps, nor the
  mirror's smaller target): the viewport's corner moved by a sub-pixel offset, FSR's Halton sequence (bases 2 and 3,
  8 times the area's ratio long), `xgpu_vulkan_set_jitter`. Nothing is jittered unless an upscaler (or a denoiser
  that upscales) is on.
- **OPAQUE**, once the view's opaque surfaces are drawn and before its transparent ones: the view's colour kept.
- **END**, once the view is whole and tone mapped: its targets and the output's (the window's at the screen's
  scale), their corners, the camera this frame and last (SCENE_VIEW's layout), the sharpness and the frame's time.

At END one compute pass (`xgpu_vulkan_upscale_shaders.c`'s INPUTS, generated by `tools/vulkan_upscale_shaders.py`)
gives an upscaler its inputs at the render size from 0, 0:

- the colour, as drawn (LDR, its alpha kept);
- the depth, reversed as the backend keeps it (1 at the near plane);
- the motion in pixels, to where each pixel's surface was the frame before, not jittered: the G-buffer's second
  target's (display.lighting `"ce_deferred"` or `"raster"`: the camera's and each object's) where its pass drew the
  pixel with history, else the camera's, from the depth and the two cameras; none on the first-person weapon (the
  stencil's bit 0, which the game sets for it: it moves with the camera);
- the reactive mask: where the transparent surfaces, the effects and the fog changed the colour from what OPAQUE
  kept, FSR's own generation's rule (the largest change of the tone-mapped colour; 0.9 past 0.2, else 0).

Then the upscaler writes the output (four half floats, the output's size): **FSR 3.1** (`xgpu_vulkan_fsr3.c`, its
passes AMD's, `port/third_party/fidelityfx-fsr3`), or the upscaler's **plugin** (`evaluate`, given those images,
the jitter, exposure 1, the camera's matrices, the reset and the sizes; one that fails is FSR 3.1 from then on;
where FSR 3.1 cannot run either, its passes or images not made, the view is stretched into the window as it is).
The output is stretched into the window's rectangle of the screen's target, and the view's depth and stencil into
its depth target's (nearest: what is drawn after the HUD, Forge's parts and the debug geometry, is tested against
it). The history is dropped where the guest says the camera has none (the view not drawn the frame before, or of
other corners), and where the sizes or the upscaler change.

## The denoiser's entry points

display.denoiser `"plugin:<name>"` gives the path tracer's light (display.prism_rt `"pathtraced"`) to a plugin, its
own filter then off (the light traced raw, `xgpu_vulkan_rt.c`):

- a plugin that **denoises** (`PRISM_CAP_DENOISE`, NRD's kind) is given the path pass's two raw images, the light
  and the light added (`diffuse_radiance`, `specular_radiance`: RGBA16F), before the passes that carry them on
  (`xgpu_vulkan_upscale_denoise`), denoised in place;
- one that **denoises and upscales** (`PRISM_CAP_DENOISE_UPSCALE`: DLSS Ray Reconstruction) takes the whole view at
  END in the upscaler's place, its noisy colour with the G-buffer's guides (the diffuse albedo: an object's base
  colour, the level's the base target's, less where metal; the specular albedo: F0, the specular scale's dielectric
  or the base colour where metal; the shading normal and roughness), the depth, motion and jitter. The view must have
  its G-buffer (display.lighting `"ce_deferred"` or `"raster"`): where it has none, FSR 3.1 resolves the view.

display.denoiser `"off"` traces the light raw, the tracer's own filter off whatever display.prism_rt_denoiser
says (`port/linux/game/prism_rt.c`): a diagnostic, and the light a plugin is given.

The path tracer of display.prism_rt is the mode PRISM v4's M3 replaces; the hooks are its light's and the view's, not
its own, so that M3's path tracer takes them as they are.

## Tests

```sh
python -m pytest -q tools/test_prism_plugin.py
```

It builds the passthrough plugin and several broken ones, checks that the passthrough exports its entry alone,
records its commands over a fake device (one copy for equal sizes, one blit otherwise, every image left in its
layout; as a denoiser, nothing), checks that each broken plugin, a missing one and a wrong ABI are refused with a
logged reason, and that no name touches no file. It needs a C compiler, `nm` and Vulkan's headers (the build's or
the system's), and no GPU or game data.

```sh
python -m pytest -q tools/test_upscale.py
```

It checks FSR 3.1's vendored files against their README's SHA-256, that the generated SPIR-V is still its GLSL's
(`tools/vulkan_upscale_shaders.py --check`, where glslang and SPIRV-Tools are installed), FSR's host half over a fake
core (its jitter the SDK's Halton sequence, of 8 times the area's ratio; no dispatch without a device), and the
settings' entries, README rows and menu values.
