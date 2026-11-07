"""The Vulkan core's pass/load actions and layered image views over a fake Vulkan,
and what a draw costs the render thread: the texture stages looked up again,
and the guest's memory found mapped without a system call.

Compiles the actual core; no GPU, display, game data or Vulkan loader is
needed. Vulkan headers come from configure's downloaded host dependencies
or the system installation.
"""

import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import zlib

import pytest

ROOT = Path(__file__).resolve().parent.parent


def test_vulkan_passes(tmp_path):
    cc = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
    if not cc or not sys.platform.startswith("linux"):
        pytest.skip("needs a C compiler on Linux")
    candidates = [ROOT / f"build/{name}/third_party/vulkan-headers" for name in ("linux_x86_64", "linux_arm64")]
    headers = next((path for path in candidates + [Path("/usr/include")]
                    if (path / "vulkan/vulkan_core.h").exists()), None)
    if headers is None:
        pytest.skip("needs Vulkan headers (configure.py fetches them for the Linux build)")
    binary = tmp_path / "vulkan_passes"
    subprocess.run([
        cc, "-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
        f"-I{headers}", f"-I{ROOT / 'port/android/host'}", f"-I{ROOT / 'port/android/include'}",
        f"-I{ROOT / 'port/linux/src'}", str(ROOT / "port/linux/vulkan/tests/passes_test.c"), "-o", str(binary), "-ldl", "-lm", "-pthread",
    ], check=True)
    run = subprocess.run([str(binary)], capture_output=True, text=True)
    assert run.returncode == 0, run.stdout + run.stderr


def test_pacing_presents_before_the_display_answers(tmp_path):
    # display.low_latency over a present wait that says nothing until a
    # frame is shown, or nothing ever (the Steam Deck's gamescope, where
    # build 940 showed no frame): the first frames go unpaced, no present
    # waits, a silent display is given up on, an answering one paces the
    # frames from the present after its first answer
    cc = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
    if not cc or not sys.platform.startswith("linux"):
        pytest.skip("needs a C compiler on Linux")
    candidates = [ROOT / f"build/{name}/third_party/vulkan-headers" for name in ("linux_x86_64", "linux_arm64")]
    headers = next((path for path in candidates + [Path("/usr/include")]
                    if (path / "vulkan/vulkan_core.h").exists()), None)
    if headers is None:
        pytest.skip("needs Vulkan headers (configure.py fetches them for the Linux build)")
    binary = tmp_path / "pacing"
    subprocess.run([
        cc, "-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
        f"-I{headers}", f"-I{ROOT / 'port/android/host'}", f"-I{ROOT / 'port/android/include'}",
        f"-I{ROOT / 'port/linux/src'}", str(ROOT / "port/linux/vulkan/tests/pacing_test.c"), "-o", str(binary),
        "-pthread",
    ], check=True)
    run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60)
    assert run.returncode == 0, run.stdout + run.stderr


def test_vr_required_capabilities(tmp_path):
    cc = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
    if not cc:
        pytest.skip("needs a native C compiler")
    # Compile the compositor's actual gate; it must reject an optional GPU
    # feature that the sky needs instead of silently changing its pixels.
    text = (ROOT / "port/linux/vulkan/host_vulkan_vr.c").read_text()
    function = text[text.index("static BOOL supported(void)"):
                    text.index("static BOOL sample_counting(void)")]
    source = tmp_path / "vr_capabilities.c"
    source.write_text(r"""
#include <assert.h>
#include <stdarg.h>
typedef int BOOL;
#define TRUE 1
#define FALSE 0
#define HOST_LOG_WARN 1
struct xgpu_vulkan_capabilities { unsigned int multiview_views; int depth_clamp; };
static struct xgpu_vulkan_capabilities available;
static unsigned int warnings;
static void xgpu_vulkan_capabilities(struct xgpu_vulkan_capabilities *capabilities)
{
    *capabilities = available;
}
static void host_logf(int priority, const char *format, ...)
{
    assert(priority == HOST_LOG_WARN && format);
    warnings++;
}
""" + function + r"""
int main(void)
{
    available.multiview_views = 2;
    available.depth_clamp = 1;
    assert(supported() && !warnings);
    available.multiview_views = 4;
    assert(supported() && !warnings);
    available.depth_clamp = 0;
    assert(!supported() && warnings == 1);
    available.depth_clamp = 1;
    available.multiview_views = 1;
    assert(!supported() && warnings == 2);
    available.multiview_views = 0;
    assert(!supported() && warnings == 3);
    return 0;
}
""")
    binary = tmp_path / "vr_capabilities"
    subprocess.run([cc, "-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror",
                    str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


def test_post_shaders(tmp_path):
    # The anti-aliasing passes' generated file (tools/vulkan_post_shaders.py),
    # with display.hdr's tone map and encoding, display.gamma's curve and
    # display.panini's projection: SMAA's lookup textures are
    # port/third_party/smaa's, inflated, and each module is valid for Vulkan
    # 1.1 where spirv-val is installed.
    text = (ROOT / "port/linux/vulkan/xgpu_vulkan_post_shaders.c").read_text()
    arrays = {name: [int(word, 16) for word in re.findall(r"0x([0-9a-f]{8})", body)]
              for name, body in re.findall(r"const uint32_t (\w+)\[\] =\n\{\n(.*?)\n\};", text, re.S)}
    for name, file, size in (("area", "area_tex.zlib", 160 * 560 * 2), ("search", "search_tex.zlib", 64 * 16)):
        texels = zlib.decompress((ROOT / "port/third_party/smaa" / file).read_bytes())
        words = arrays[f"xgpu_vulkan_post_{name}_texture"]
        assert len(texels) == size
        assert struct.pack(f"<{len(words)}I", *words)[:size] == texels
    modules = sorted(name for name in arrays if name.endswith(("_vert", "_frag")))
    assert modules == ["clear_frag", "fxaa_frag", "gamma_frag", "hdr_encode_frag", "hdr_tone_map_frag", "hdr_tone_map_ms_frag",
                       "panini_frag", "post_vert", "rotate_frag", "smaa_blend_frag", "smaa_edges_frag",
                       "smaa_weights_frag"]
    validator = shutil.which("spirv-val")
    if not validator:
        pytest.skip("needs spirv-val (SPIRV-Tools)")
    for name in modules:
        assert arrays[name][0] == 0x07230203
        module = tmp_path / f"{name}.spv"
        module.write_bytes(struct.pack(f"<{len(arrays[name])}I", *arrays[name]))
        subprocess.run([validator, "--target-env", "vulkan1.1", str(module)], check=True)


def _native_cc():
    cc = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
    if not cc:
        pytest.skip("needs a native C compiler")
    return cc


def test_texture_memory_in_the_guests_ranges_is_not_asked_of_the_kernel(tmp_path):
    # Without the base register (the Steam Frame's guest) readable() asked
    # mincore for each stage's header and palette at each bind. A range in
    # the memory the guest's heaps come from (host_low_owns: mapped whole,
    # reserved where unused) is readable without a system call; any other
    # range is still asked about, page by page as before.
    text = (ROOT / "port/linux/vulkan/xgpu_vulkan_textures.c").read_text()
    function = text[text.index("static BOOL readable(uint32_t address, uint32_t size)"):
                    text.index("static struct xgpu_vulkan_image *xbox_texture_get(")]
    source = tmp_path / "readable.c"
    source.write_text(r"""
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
typedef int BOOL;
#define TRUE 1
#define FALSE 0
#define CONTIGUOUS_BASE 0x80000000ull
#define CONTIGUOUS_SIZE 0x08000000ull
#define POOL_BASE 0x20000000ull
#define POOL_SIZE 0x10000000ull
static unsigned asked, owned_asked;
static int mapped = 1;
static uint64_t host_page_size(void) { return 4096; }
static uintptr_t host_address(uint32_t address) { return address; }
static void *host_pointer(uint32_t address) { return (void *)(uintptr_t)address; }
static int host_low_owns(uintptr_t address, size_t size)
{
    owned_asked++;
    return address >= POOL_BASE && address + size <= POOL_BASE + POOL_SIZE;
}
static int mincore(void *address, size_t length, unsigned char *residency)
{
    (void)address; (void)residency;
    assert(length && length <= 4096 * 64);
    asked++;
    return mapped ? 0 : -1;
}
""" + function + r"""
int main(void)
{
    /* the window, as before: no question at all */
    assert(readable(0x80001000u, 20) && !asked && !owned_asked);
    /* a header in a pool: no system call */
    assert(readable(0x20000010u, 20) && !asked && owned_asked == 1);
    assert(readable(0x2ffffc00u, 1024) && !asked);
    /* across the pool's end, or outside it: mincore, as before */
    assert(readable(0x2ffffc00u, 2048) && asked == 1);
    mapped = 0;
    assert(!readable(0x10000000u, 1024) && asked == 2);
    /* nothing at 0, or past 4 GB */
    assert(!readable(0, 20) && !readable(0xfffffff0u, 64));
    return 0;
}
""")
    binary = tmp_path / "readable"
    subprocess.run([_native_cc(), "-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                    str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


def test_only_the_texture_stages_that_changed_are_looked_up_again(tmp_path):
    # The guest sends the four stages when any changes; each stage's image and
    # sampler depend on that stage alone, so only the changed ones are looked
    # up again. A watched page written, a sampled target drawn into, the
    # targets set and each frame's end look at them all again, and a
    # palettized texture's stage whenever the stages are set (its palette is
    # read as it is looked up, and is not watched).
    text = (ROOT / "port/linux/vulkan/xgpu_vulkan_stream.c").read_text()
    set_textures = text[text.index("static int stage_palettized("):
                        text.index("/* a copy of a target the draw draws into")]
    bind = text[text.index("/* the stages' images and samplers for a draw */"):
                text.index("/* ---------- pipelines */")]
    source = tmp_path / "textures_bind.c"
    source.write_text(r"""
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "xgpu_stream.h"
typedef uint64_t VkSampler;
#define VK_NULL_HANDLE 0
#define TEXTURES_ALL 15u
#define D3DFORMAT_FORMAT_MASK 0x0000ff00u
#define D3DFORMAT_FORMAT_SHIFT 8
enum { _source_none, _source_texture, _source_target };
enum { _xgpu_sampler_2d, _xgpu_sampler_3d, _xgpu_sampler_cube, _xgpu_sampler_2d_layered };
enum { XGPU_VULKAN_IMAGE_2D, XGPU_VULKAN_IMAGE_2D_ARRAY, XGPU_VULKAN_IMAGE_3D, XGPU_VULKAN_IMAGE_CUBE };
struct host_render_texture_stage
{
    struct xgpu_stream_texture_stage stage;
    struct xgpu_stream_texture_level levels[XGPU_STREAM_MIP_LEVELS];
    uint32_t store;
};
struct xgpu_vulkan_image_description { uint8_t type; };
struct xgpu_vulkan_image { struct xgpu_vulkan_image_description description; };
struct xgpu_vulkan_texture { struct xgpu_vulkan_image *image; VkSampler sampler; uint8_t kind; };
struct stream_target { struct xgpu_vulkan_image *image; };
static struct
{
    struct host_render_texture_stage stages[4];
    uint8_t border[4];
    unsigned int textures_dirty;
    int feedback_bound, uniforms_dirty;
    uint32_t textures_serial, color, store_epoch;
    struct xgpu_vulkan_texture textures[4];
} stream;
static struct xgpu_vulkan_image images[4];
static unsigned looked_up[4], samplers, binds;
static uint32_t serial = 1;
static uint32_t host_memory_watch_serial(void) { return serial; }
static uint8_t border_axes(const struct xgpu_stream_sampler *sampler) { (void)sampler; return 0; }
static VkSampler sampler_get(const struct xgpu_stream_sampler *from, uint8_t axes)
{ (void)axes; samplers++; return 1 + from->minify; }
static struct stream_target *target_get(uint32_t number) { (void)number; return NULL; }
static void target_resolve(struct stream_target *target) { (void)target; }
static struct xgpu_vulkan_image *composite_get(const struct host_render_texture_stage *source)
{ (void)source; return NULL; }
static struct xgpu_vulkan_image *feedback_copy(uint32_t number, struct stream_target *target)
{ (void)number; (void)target; return NULL; }
static struct xgpu_vulkan_image *xgpu_vulkan_textures_get(const struct xgpu_stream_texture_stage *stage)
{
    looked_up[stage->resource - 1]++;
    return &images[stage->resource - 1];
}
static const struct xgpu_vulkan_image_description *xgpu_vulkan_image_description(const struct xgpu_vulkan_image *image)
{ return &image->description; }
static void targets_bind(void) { binds++; }
/* display.prism_textures: the store's epoch, and its replacements once
usable */
static uint32_t store_epoch_now;
static int store_usable;
static unsigned store_binds;
static struct xgpu_vulkan_image store_image;
static uint32_t xgpu_vulkan_store_epoch(void) { return store_epoch_now; }
static int store_bind(const struct host_render_texture_stage *source, struct xgpu_vulkan_texture *texture, uint8_t axes)
{
    (void)axes;
    store_binds++;
    if (!store_usable)
        return 0;
    texture->image = &store_image;
    texture->sampler = 100 + source->store;
    return 1;
}
""" + set_textures + bind + r"""
static void set(struct host_render_texture_stage stages[4])
{
    memset(looked_up, 0, sizeof(looked_up));
    stream_set_textures(NULL, stages);
    textures_bind();
}

int main(void)
{
    struct host_render_texture_stage stages[4];
    int index;

    memset(stages, 0, sizeof(stages));
    for (index = 0; index < 4; index++)
    {
        stages[index].stage.source = _source_texture;
        stages[index].stage.resource = (uint32_t)index + 1;
        stages[index].stage.resource_format = 0x0c00; /* DXT1 */
    }
    set(stages);
    for (index = 0; index < 4; index++)
        assert(looked_up[index] == 1 && stream.textures[index].image == &images[index]);
    assert(samplers == 4);
    /* one stage changed: it alone */
    stages[2].stage.sampler.minify = 2;
    set(stages);
    assert(!looked_up[0] && !looked_up[1] && looked_up[2] == 1 && !looked_up[3]);
    assert(stream.textures[2].sampler == 3 && stream.textures[0].image == &images[0]);
    /* the same stages again (another stage's levels written, say): none */
    binds = 0;
    set(stages);
    assert(!looked_up[0] && !looked_up[1] && !looked_up[2] && !looked_up[3] && !binds);
    /* a watched page written: all of them */
    serial++;
    set(stages);
    for (index = 0; index < 4; index++)
        assert(looked_up[index] == 1);
    /* a palettized stage with a palette: whenever the stages are set */
    stages[1].stage.resource_format = 0x0b00;
    stages[1].stage.palette = 0x1000;
    set(stages);
    assert(looked_up[1] == 1 && !looked_up[0]);
    set(stages);
    assert(looked_up[1] == 1 && !looked_up[0] && !looked_up[2] && !looked_up[3]);
    /* the targets set, or a frame's end: all of them */
    stream.textures_dirty = TEXTURES_ALL;
    memset(looked_up, 0, sizeof(looked_up));
    textures_bind();
    for (index = 0; index < 4; index++)
        assert(looked_up[index] == 1);
    /* display.prism_textures: a store id on stage 0 (its stage changed),
    its replacement not usable yet: the stock texture */
    stages[1].stage.resource_format = 0x0c00;
    stages[1].stage.palette = 0;
    set(stages);
    stages[0].store = 7;
    store_binds = 0;
    set(stages);
    assert(store_binds == 1 && looked_up[0] == 1 && !looked_up[1] && stream.textures[0].image == &images[0]);
    /* usable: the store's epoch changed, all looked at again, stage 0 the
    replacement with its own sampler and its stock texture not looked for */
    store_usable = 1;
    store_epoch_now++;
    store_binds = 0;
    set(stages);
    assert(store_binds == 1 && !looked_up[0] && looked_up[1] == 1 && looked_up[2] == 1 && looked_up[3] == 1);
    assert(stream.textures[0].image == &store_image && stream.textures[0].sampler == 107);
    /* the same again: none */
    store_binds = 0;
    set(stages);
    assert(!store_binds && !looked_up[0] && !looked_up[1] && !looked_up[2] && !looked_up[3]);
    /* the id alone changed (the stage's bytes the same): that stage */
    stages[0].store = 8;
    set(stages);
    assert(store_binds == 1 && !looked_up[1] && stream.textures[0].sampler == 108);
    /* no id: the stock texture again */
    stages[0].store = 0;
    set(stages);
    assert(looked_up[0] == 1 && stream.textures[0].image == &images[0]);
    return 0;
}
""")
    binary = tmp_path / "textures_bind"
    subprocess.run([_native_cc(), "-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                    f"-I{ROOT / 'port/linux/src'}", str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


def test_texture_store_formats_and_levels(tmp_path):
    # display.prism_textures: the store's BC1 (no alpha), BC4, BC5 and BC7
    # images are made in their Vulkan formats with 8- and 16-byte blocks; and
    # a replacement's sampler (store_bind) samples the levels standing for the
    # stock texture's: from the resident level (or MAXMIPLEVEL's, shifted by
    # the size's scale) to the stock's last level shifted by it, the first
    # alone without a mip filter; a stage of another size than the stock
    # bitmap's (another bitmap at its header) is drawn stock
    candidates = [ROOT / f"build/{name}/third_party/vulkan-headers" for name in ("linux_x86_64", "linux_arm64")]
    headers = next((path for path in candidates + [Path("/usr/include")]
                    if (path / "vulkan/vulkan_core.h").exists()), None)
    if headers is None:
        pytest.skip("needs Vulkan headers (configure.py fetches them for the Linux build)")
    core = (ROOT / "port/linux/vulkan/xgpu_vulkan.c").read_text()
    formats = core[core.index("static VkFormat vulkan_format(uint8_t format)"):core.index("static VkComponentSwizzle swizzle_of(")]
    stream = (ROOT / "port/linux/vulkan/xgpu_vulkan_stream.c").read_text()
    bind = stream[stream.index("static int store_bind("):stream.index("/* a stage of a palettized Xbox texture")]
    source = tmp_path / "store_formats.c"
    source.write_text(r"""
#include <assert.h>
#include <stdint.h>
#include <string.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include "xgpu_vulkan.h"
#include "xgpu_stream.h"
#include "xgpu_texture_store.h"
static struct { VkFormat depth_format; } vk;
""" + formats + r"""
typedef unsigned long DWORD;
typedef int BOOL;
enum { _filter_none, _filter_point, _filter_linear };
struct xgpu_texture_description { DWORD format; unsigned long width, height, depth, levels; BOOL cube_map, linear; };
struct host_render_texture_stage
{
    struct xgpu_stream_texture_stage stage;
    struct xgpu_stream_texture_level levels[XGPU_STREAM_MIP_LEVELS];
    uint32_t store;
};
static struct xgpu_texture_description described;
static void xgpu_texture_describe(DWORD format, DWORD size, struct xgpu_texture_description *description)
{ (void)format; (void)size; *description = described; }
static struct xgpu_texture_store_view current;
static int usable;
static int xgpu_vulkan_store_use(uint32_t id, struct xgpu_texture_store_view *view)
{ (void)id; *view = current; return usable; }
static float got_min, got_max;
static int got_mip;
static VkSampler sampler_get_levels(const struct xgpu_stream_sampler *from, uint8_t axes, float min_lod, float max_lod)
{ (void)axes; got_min = min_lod; got_max = max_lod; got_mip = from->mip; return (VkSampler)(uintptr_t)1; }
""" + bind + r"""
int main(void)
{
    struct host_render_texture_stage source;
    struct xgpu_vulkan_texture texture;
    static struct xgpu_vulkan_image *image;

    assert(vulkan_format(XGPU_VULKAN_FORMAT_BC1_RGB) == VK_FORMAT_BC1_RGB_UNORM_BLOCK);
    assert(vulkan_format(XGPU_VULKAN_FORMAT_BC4) == VK_FORMAT_BC4_UNORM_BLOCK);
    assert(vulkan_format(XGPU_VULKAN_FORMAT_BC5) == VK_FORMAT_BC5_UNORM_BLOCK);
    assert(vulkan_format(XGPU_VULKAN_FORMAT_BC7) == VK_FORMAT_BC7_UNORM_BLOCK);
    assert(format_blocks(XGPU_VULKAN_FORMAT_BC4) && format_blocks(XGPU_VULKAN_FORMAT_BC5) &&
        format_blocks(XGPU_VULKAN_FORMAT_BC7) && format_blocks(XGPU_VULKAN_FORMAT_BC1_RGB));
    assert(format_block_bytes(XGPU_VULKAN_FORMAT_BC4) == 8 && format_block_bytes(XGPU_VULKAN_FORMAT_BC1_RGB) == 8 &&
        format_block_bytes(XGPU_VULKAN_FORMAT_DXT1) == 8 && format_block_bytes(XGPU_VULKAN_FORMAT_BC5) == 16 &&
        format_block_bytes(XGPU_VULKAN_FORMAT_BC7) == 16 && format_block_bytes(XGPU_VULKAN_FORMAT_DXT5) == 16);

    /* a 4096 BC7 over a 256 stock of 9 levels: scale 4, 13 levels */
    image = (struct xgpu_vulkan_image *)(uintptr_t)0x1000;
    memset(&source, 0, sizeof(source));
    source.store = 5;
    source.stage.sampler.mip = _filter_linear;
    described.width = described.height = 256;
    described.levels = 9;
    memset(&current, 0, sizeof(current));
    current.image = image;
    current.scale = 4;
    current.levels = 13;
    current.stock.width = current.stock.height = 256;
    /* (not usable: stock) */
    usable = 0;
    memset(&texture, 0, sizeof(texture));
    assert(!store_bind(&source, &texture, 0) && !texture.image);
    /* (usable at the stock resolution: levels 4 to 12) */
    usable = 1;
    current.resident = 4;
    assert(store_bind(&source, &texture, 0) && texture.image == image);
    assert(got_min == 4.0f && got_max > 12.0f && got_max < 12.01f);
    /* (whole: from level 0) */
    current.resident = 0;
    assert(store_bind(&source, &texture, 0) && got_min == 0.0f && got_max > 12.0f && got_max < 12.01f);
    /* (MAXMIPLEVEL 2: the stock's level 2 is the image's 6) */
    source.stage.sampler.first_level = 2;
    assert(store_bind(&source, &texture, 0) && got_min == 6.0f);
    source.stage.sampler.first_level = 0;
    /* (a stock of 5 levels: down to its last, the image's 8) */
    described.levels = 5;
    assert(store_bind(&source, &texture, 0) && got_max > 8.0f && got_max < 8.01f);
    described.levels = 9;
    /* (cut back to the stock resolution: the image from the asset's level
    4, its resident level its first) */
    current.top = 4;
    current.resident = 4;
    assert(store_bind(&source, &texture, 0) && got_min == 0.0f && got_max > 8.0f && got_max < 8.01f);
    current.top = 0;
    /* (no mip filter: the stock's first level alone, so the replacement's
    from its resident level down to the one of the stock's size, mipmapped
    between them; point minifying keeps point mips) */
    source.stage.sampler.mip = _filter_none;
    current.resident = 1;
    assert(store_bind(&source, &texture, 0) && got_min == 1.0f && got_max > 4.0f && got_max < 4.01f &&
        got_mip == _filter_linear);
    source.stage.sampler.minify = _filter_point;
    assert(store_bind(&source, &texture, 0) && got_mip == _filter_point);
    source.stage.sampler.minify = _filter_linear;
    current.top = 4;
    current.resident = 4;
    assert(store_bind(&source, &texture, 0) && got_min == 0.0f && got_max > 0.0f && got_max < 0.01f);
    current.top = 0;
    source.stage.sampler.mip = _filter_linear;
    /* (another size at the header, or a cube against a 2D: stock) */
    described.width = 128;
    memset(&texture, 0, sizeof(texture));
    assert(!store_bind(&source, &texture, 0) && !texture.image);
    described.width = 256;
    described.cube_map = 1;
    assert(!store_bind(&source, &texture, 0));
    return 0;
}
""")
    binary = tmp_path / "store_formats"
    subprocess.run([_native_cc(), "-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                    f"-I{headers}", f"-I{ROOT / 'port/linux/vulkan'}", f"-I{ROOT / 'port/linux/src'}", str(source),
                    "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
