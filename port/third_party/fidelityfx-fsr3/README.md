# FidelityFX Super Resolution 3.1

AMD FidelityFX Super Resolution 3.1's upscaler (FSR 3.1.4), by Advanced
Micro Devices, MIT licensed (see `LICENSE.txt`, the SDK's own, checked
for this version: the MIT licence, "Copyright (C) 2024 Advanced Micro
Devices, Inc.").

Upstream: https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK, tag
`v1.1.4`, commit c6efa6bf7f2027b3ec94f28578bb5965eabb9e55. Only what the
upscaler's Vulkan shaders need is here, each file copied unchanged from the
same place in the SDK:

- `include/FidelityFX/gpu/`: the shared GPU headers (`ffx_core.h` and the
  GLSL half of what it includes), `fsr1/ffx_fsr1.h` (RCAS, the sharpening)
  and `spd/ffx_spd.h` (the single-pass downsampler of the luma pyramids);
- `include/FidelityFX/gpu/fsr3upscaler/`: the upscaler's passes' code and
  its GLSL resource declarations (`ffx_fsr3upscaler_callbacks_glsl.h`);
- `src/backends/vk/shaders/fsr3upscaler/`: the GLSL main function of each
  pass the upscaler runs (prepare inputs, luma pyramid, shading change
  pyramid, shading change, prepare reactivity, luma instability,
  accumulate, RCAS).

Left out: the SDK's C++ host code, its Vulkan backend and its shader
compiler (`FidelityFX_SC`, Windows only). `tools/vulkan_upscale_shaders.py`
compiles the passes with glslang (one permutation: LDR colour, motion at the
render size, unjittered, reversed depth, the reference Lanczos, with and
without sharpening) into `port/linux/vulkan/xgpu_vulkan_upscale_shaders.c`,
and `port/linux/vulkan/xgpu_vulkan_fsr3.c` runs them: its host code is
written after the SDK's `ffx_fsr3upscaler.cpp` (the passes' order, their
constants, the resources and the jitter's Halton sequence), and carries this
licence's notice.

`display.upscaler = "fsr3"` upscales the 3D view with it
(`docs/prism_plugin.md`).

SHA-256 of each file (`sha256sum`, from this folder):

```
a19560f837c09f0c990e98b5925b731135d908e372af41c5895cbfa70cc9e5c8  include/FidelityFX/gpu/ffx_common_types.h
01afe5b65549b2c0951b494ca4bf570ee2aa89a64344e4c7d98ef86e6613ab42  include/FidelityFX/gpu/ffx_core_glsl.h
0e3c50cb0b4592c051d87e028a1dfa54fbd097942ebb3146964db801dec095d0  include/FidelityFX/gpu/ffx_core_gpu_common.h
d10466cbf027c8c23858a25f8e8cbbbf521d29f373db663c9354221651e9a498  include/FidelityFX/gpu/ffx_core_gpu_common_half.h
e439aa5f516b2d2ebf070ff1d24561f04fa5f00262a2e4f760764a7d07de7997  include/FidelityFX/gpu/ffx_core.h
a7c3a1c213ec083bec401181036cffe7a222af2c430c0bab6bd8c14e83e8a567  include/FidelityFX/gpu/ffx_core_portability.h
5d608d9d0645da03a3aa6c729c5ad990c6f222053eab850b8e218607c1baaa9d  include/FidelityFX/gpu/fsr1/ffx_fsr1.h
7212a028c6ff3ca4a76f9d62b7f64f5f5adce0f108067d8995c7bc795234afbc  include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_accumulate.h
875c2b8d8df93716fb46271886c768a9ef40c5ccc11ade39397952b4a4e54321  include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_callbacks_glsl.h
4898aa014fef875d3644b9b3bed03f31a963349e17ad553198f87ae8af82cb3f  include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_common.h
51849bd2f758e2b15375c6a21633fea2f7433a19eb3d74d1d07f8ead36a695ba  include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_debug_view.h
82d13d21e161732e48b267d28906f067a8df306ead5aeeed65a1ef66b3e06003  include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_luma_instability.h
fa6eeaf8197b214ee1853d73da3c66cd0c8fc24444a65237b0023bd68f10d887  include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_luma_pyramid.h
acae0499b8805c363c105895afc28fc90e549b38c8bffdcbec1cf9e2f842abb0  include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_prepare_inputs.h
ddf7a6d9cd6e357328e972e0b654da977edcb2e337c992b06d6256eeae339e03  include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_prepare_reactivity.h
f1d8a9152bd3ceda8a10dafe44e0c6d27109b345fc4a5704f71c6bf23d6c5cb4  include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_rcas.h
d2d01ea2f59658e5494414448f11435879bdac871c3ad7c84b951248bc223702  include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_reproject.h
10c6f7619a6c1ff9983fdff9bac565789b1cda52f97ef60b491498223e7b7097  include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_resources.h
e8de656c71b15537ddd891b7485492b7829e6aec1dea96256fbc246c9a7c7366  include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_sample.h
13e772ff9bd32244fb990704055040f8dbb945c014cc6f0fdbf553188b300e7e  include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_shading_change.h
b1e94d7cbb1b7d6e4556e4a080577a62fde07efbc8379b52223d322ed4b75912  include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_shading_change_pyramid.h
c51f3af61b43fd779a6685202886dbb5dab3bfea989f8b5060c8c9ab4a93207b  include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_upsample.h
d276f32ee18948cb053c991f3c8821f9664c93f12cb2483dd2978f333bca67b2  include/FidelityFX/gpu/spd/ffx_spd.h
82cf74fc23885107c7f69249ac3f1dac55898ff68320eb2c753984cae6b2d78a  LICENSE.txt
73b5be6db479475559d2374957b2854aa6d7f7ddadba76ef5e4ed7f248ea9564  src/backends/vk/shaders/fsr3upscaler/ffx_fsr3upscaler_accumulate_pass.glsl
423345d36c811237af7a41ef5e6220638f5159952e90fa084aa952ddd267581b  src/backends/vk/shaders/fsr3upscaler/ffx_fsr3upscaler_luma_instability_pass.glsl
3e637a702c2cc6b6a222df4327a4ed565f731c6190e958e0ffec5b43741f7ea4  src/backends/vk/shaders/fsr3upscaler/ffx_fsr3upscaler_luma_pyramid_pass.glsl
bdf3b1ca0fdce968afb79f55d667960382139926eccdfdec46e08dbae3982657  src/backends/vk/shaders/fsr3upscaler/ffx_fsr3upscaler_prepare_inputs_pass.glsl
176f6aa5f2f437487222c7fc057ce858795a6677d6ac3e937a1ce36521c6ddfc  src/backends/vk/shaders/fsr3upscaler/ffx_fsr3upscaler_prepare_reactivity_pass.glsl
110b409d5e10b16e5a237c13932a707c1c07932bf790e814f1a9d76f02569ad7  src/backends/vk/shaders/fsr3upscaler/ffx_fsr3upscaler_rcas_pass.glsl
246d24b76e292b7c8c5ba39dfc3a4dff1486701f5fccc6482284afac5a7daaf9  src/backends/vk/shaders/fsr3upscaler/ffx_fsr3upscaler_shading_change_pass.glsl
ce34582f454925499523d30d2619e19aefd8f1c091545c37d4ebd8feea91cf55  src/backends/vk/shaders/fsr3upscaler/ffx_fsr3upscaler_shading_change_pyramid_pass.glsl
```
