# The SPIR-V shaders' corpus

The SPIR-V emitter (`port/linux/src/xgpu_spirv.c`) writes the NV2A's programs
as SPIR-V modules for the Vulkan backend. `spirv_corpus` writes the same
variants as the Metal shaders' corpus (`tools/msl_corpus`), one `.spv` file
each:

- every captured vertex program, from immediate mode (all floats) and from
  streams (its NORMPACKED3 inputs), unlit, lit for the enhanced dynamic
  lights and lit for each pixel; and from streams with its other inputs
  unnormalized shorts;
- every captured pixel shader key, and again counting its samples in the shader (where it is a visibility
  test's) and drawing its border colors in the shader (where it samples a 2D
  or 3D texture);
- `msl_corpus`'s programs made up to use every part of the intermediate form
  (`tools/msl_corpus/synthetic.c`), plus layered texture lookups in each
  stage with projected, bump-offset and dependent coordinates;
- the enhanced dynamic lights' glows (`xgpu_spirv_halo_vertex_shader` and
  `xgpu_spirv_halo_pixel_shader`), flat, for both eyes and foveated, and in
  the VR mode's eyes as their bolts' scattered light (`glow-*`);
- the VR mode's effects (`effect-*`): an effect's program, with one map and
  two, soft and thick, with each fade, for both eyes and foveated
  (`tools/test_vr_effects.py` checks these alone);
- display.hdr's tags (`hdr-*`, `port/linux/src/xgpu_hdr.h`): every ceiling,
  expanded or not, and the alpha limit over each of eleven blends, reading
  the pixel drawn over from a target of one sample and of four, and the
  dynamic lights past white;
- display.prism.material_overlay's (`overlay*`, `port/linux/src/xgpu.h`): a
  model's program lit for each pixel with two tiles and with four and a class
  mask, in each debug view, by the point lights, the enhanced dynamic lights
  and the world's occlusion, with a reflection cube (signed and alpha-killed
  too) and without;
- anti-aliasing's passes (FXAA and SMAA's three, and their vertex shader)
  and display.hdr's tone map (of one sample and of more) and encoding, as
  `tools/vulkan_post_shaders.py` generated them
  (`port/linux/vulkan/xgpu_vulkan_post_shaders.c`).

Every program is checked flat, for both eyes, and for either eye alone,
with and without the foveated insets. The visibility tests also check the
indexed storage buffer counter and exclusion of the inset views.

Identical modules are written once; `index.txt` names each variant's file.
The captures are what the renderer's test capture writes
(`HALO_TEST_CAPTURE`): `vsNNN.bin` and `ps_XXXXXXXX.key`, as for `msl_corpus`.

On Linux, with SPIRV-Tools' `spirv-val`:

    tools/spirv_corpus/build.sh . build/spirv_corpus
    build/spirv_corpus/spirv_corpus <capture dir> <corpus dir>
    sh tools/spirv_corpus/check.sh <corpus dir> <log>

`check.sh` validates each module for Vulkan 1.1, logs every message and
exits nonzero if any module fails.
