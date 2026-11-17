"""The VR mode's effects (port/linux/vulkan/README.md, "VR"): the thickness
bake's distances to the edge of known shapes and each eye's transforms
(port/linux/vulkan/tests/effects_test.c, the real xgpu_vulkan_effects.c), and
the effects' and glows' SPIR-V (the emitter's, tools/spirv_corpus) valid for
Vulkan 1.1; and the thickness images' lifetimes: the stream's copy of one
looked up again once the texture cache has destroyed it, and nothing baked
from texels the cache never read. No GPU, display or game data is needed.
"""

import os
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parent.parent


def compiler():
    return os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")


def test_thickness_and_views(tmp_path):
    cc = compiler()
    if not cc:
        pytest.skip("needs a C compiler")
    binary = tmp_path / "effects_test"
    subprocess.run([cc, "-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror",
                    str(ROOT / "port/linux/vulkan/tests/effects_test.c"), "-o", str(binary), "-lm"], check=True)
    run = subprocess.run([str(binary)], capture_output=True, text=True)
    assert run.returncode == 0 and run.stdout.strip() == "ok", run.stdout + run.stderr


def test_effect_shaders(tmp_path):
    cc = compiler()
    if not cc:
        pytest.skip("needs a C compiler")
    build = tmp_path / "build"
    subprocess.run(["sh", str(ROOT / "tools/spirv_corpus/build.sh"), str(ROOT), str(build)], check=True,
                   env={**os.environ, "CC": cc})
    captures = tmp_path / "captures"
    captures.mkdir()
    corpus = tmp_path / "corpus"
    subprocess.run([str(build / "spirv_corpus"), str(captures), str(corpus)], check=True, capture_output=True)
    rows = [line.split("\t") for line in (corpus / "index.txt").read_text().splitlines()
            if line.startswith(("effect-", "glow-", "zsprite-", "screen-effect-"))]
    # soft and thick, one map and two, each fade, flat and foveated; the
    # glows' two shaders flat and foveated; a z-sprite's depth flat and in
    # both eyes, and as an effect soft and thick, each fade, flat and foveated
    assert len([row for row in rows if row[0].startswith("effect-")]) == 2 * 2 * 6 * 2
    assert len([row for row in rows if row[0].startswith("glow-")]) == 4
    # a flat screen's (display.soft_particles): soft, one map and two, each fade
    assert len([row for row in rows if row[0].startswith("screen-effect-")]) == 2 * 6
    assert len([row for row in rows if row[0].startswith("zsprite-")]) == 2 + 2 * 6 * 2
    files = sorted({row[1] for row in rows})
    assert all((corpus / f"{name}.spv").stat().st_size > 0 for name in files)
    validator = shutil.which("spirv-val")
    if not validator:
        pytest.skip("needs spirv-val (SPIRV-Tools)")
    for name in files:
        subprocess.run([validator, "--target-env", "vulkan1.1", str(corpus / f"{name}.spv")], check=True)


def _build_and_run(cc, tmp_path, name, text):
    source = tmp_path / f"{name}.c"
    source.write_text(text)
    binary = tmp_path / name
    subprocess.run([cc, "-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                    f"-I{ROOT / 'port/linux/src'}", str(source), "-o", str(binary)], check=True)
    run = subprocess.run([str(binary)], capture_output=True, text=True)
    assert run.returncode == 0, run.stdout + run.stderr


def test_stream_looks_the_thickness_up_again_once_destroyed(tmp_path):
    # The stream keeps the thickness it looked up for texture 0's stage and
    # the watched pages' serial. The texture cache lets an idle texture go
    # (its thickness destroyed) without either changing, and the same stage
    # draws again later: the image kept would be a destroyed one.
    cc = compiler()
    if not cc:
        pytest.skip("needs a C compiler")
    text = (ROOT / "port/linux/vulkan/xgpu_vulkan_stream.c").read_text()
    function = text[text.index("static int effect_textures(struct xgpu_vulkan_texture textures[4])"):
                    text.index("/* a draw of the current pipeline, made and drawn")]
    _build_and_run(cc, tmp_path, "effect_textures", r"""
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "xgpu_stream.h"
#define XGPU_SPIRV_EFFECT_DEPTH_BINDING 2
#define XGPU_SPIRV_EFFECT_THICKNESS_BINDING 3
enum { XGPU_VULKAN_IMAGE_2D, XGPU_VULKAN_IMAGE_2D_ARRAY };
enum { XGPU_VULKAN_FILTER_POINT, XGPU_VULKAN_FILTER_LINEAR };
enum { XGPU_VULKAN_MIP_NONE };
enum { XGPU_VULKAN_ADDRESS_CLAMP };
typedef uint64_t VkSampler;
struct xgpu_vulkan_image { int destroyed; };
struct xgpu_vulkan_texture { struct xgpu_vulkan_image *image; VkSampler sampler; uint8_t kind; };
struct xgpu_vulkan_sampler_description { uint8_t minify, magnify, mip, address[3], anisotropy; };
struct host_render_texture_stage
{
    struct xgpu_stream_texture_stage stage;
    struct xgpu_stream_texture_level levels[XGPU_STREAM_MIP_LEVELS];
};
struct stream_target { int depth; struct xgpu_vulkan_image *image, *multisampled; };
#define XGPU_MULTIVIEW_NONE 0
static struct
{
    uint32_t depth;
    int samples;
    const struct xgpu_stream_pipeline_define *definition;
    struct host_render_texture_stage stages[4];
    struct xgpu_vulkan_texture textures[4];
    uint32_t textures_serial;
    struct xgpu_vulkan_image *thickness;
    int thickness_known;
    struct xgpu_stream_texture_stage thickness_stage;
    uint32_t thickness_serial, thickness_epoch;
} stream;
static struct stream_target depth_target = { 1, NULL, NULL };
static struct xgpu_vulkan_image images[2], depth_image;
static struct xgpu_vulkan_image *cached = &images[0];
static unsigned lookups;
static uint32_t epoch;
static struct stream_target *target_get(uint32_t number) { return number ? &depth_target : NULL; }
static void target_resolve(struct stream_target *target) { (void)target; }
static VkSampler xgpu_vulkan_sampler(const struct xgpu_vulkan_sampler_description *description)
{ return 1 + description->minify; }
static struct xgpu_vulkan_image *xgpu_vulkan_textures_thickness(const struct xgpu_stream_texture_stage *stage)
{
    (void)stage;
    lookups++;
    return cached;
}
static uint32_t xgpu_vulkan_textures_thickness_epoch(void) { return epoch; }
""" + function + r"""
int main(void)
{
    struct xgpu_vulkan_texture textures[4];

    depth_target.image = &depth_image;
    stream.depth = 1;
    stream.textures_serial = 7;
    stream.stages[0].stage.source = 1;
    stream.stages[0].stage.resource = 0x1000;
    assert(effect_textures(textures) == 1);
    assert(lookups == 1 && textures[XGPU_SPIRV_EFFECT_THICKNESS_BINDING].image == &images[0]);
    assert(textures[XGPU_SPIRV_EFFECT_DEPTH_BINDING].image == &depth_image);
    /* the same stage, serial and epoch: kept */
    effect_textures(textures);
    assert(lookups == 1 && textures[XGPU_SPIRV_EFFECT_THICKNESS_BINDING].image == &images[0]);
    /* the texture let go of (its thickness destroyed), the same stage drawn
    again, the serial as it was: looked up again, the new one drawn */
    images[0].destroyed = 1;
    epoch++;
    cached = &images[1];
    effect_textures(textures);
    assert(lookups == 2 && textures[XGPU_SPIRV_EFFECT_THICKNESS_BINDING].image == &images[1]);
    assert(!textures[XGPU_SPIRV_EFFECT_THICKNESS_BINDING].image->destroyed);
    effect_textures(textures);
    assert(lookups == 2);
    /* the eyes' depth is their layers; a flat screen's (display.soft_particles,
    a pipeline of no views) its one 2D image, as its shader samples it */
    assert(textures[XGPU_SPIRV_EFFECT_DEPTH_BINDING].kind == XGPU_VULKAN_IMAGE_2D_ARRAY);
    {
        static struct xgpu_stream_pipeline_define flat;

        flat.key.views = XGPU_MULTIVIEW_NONE;
        stream.definition = &flat;
        assert(effect_textures(textures) == 1);
        assert(textures[XGPU_SPIRV_EFFECT_DEPTH_BINDING].kind == XGPU_VULKAN_IMAGE_2D);
        assert(textures[XGPU_SPIRV_EFFECT_DEPTH_BINDING].image == &depth_image);
        flat.key.views = 1;
        effect_textures(textures);
        assert(textures[XGPU_SPIRV_EFFECT_DEPTH_BINDING].kind == XGPU_VULKAN_IMAGE_2D_ARRAY);
    }
    return 0;
}
""")


def test_thickness_is_baked_only_from_texels_read_and_forgotten_with_its_palette(tmp_path):
    # An entry whose pages were not readable has never been read (generation
    # 0): the bake would read them anyway. An entry read again with another
    # palette may come back at the generation its thickness was baked at:
    # that thickness is the other palette's.
    cc = compiler()
    if not cc:
        pytest.skip("needs a C compiler")
    text = (ROOT / "port/linux/vulkan/xgpu_vulkan_textures.c").read_text()
    forget = text[text.index("/* the entry's thickness destroyed, and not to be taken as failed */"):
                  text.index("static uint32_t bucket_index(")]
    lookup = text[text.index("struct xgpu_vulkan_image *xgpu_vulkan_textures_thickness("):
                  text.index("/* ---------- images */")]
    reuse = text[text.index("static struct entry *xbox_entry_find("):
                 text.index("entry = calloc(1, sizeof(*entry));")]
    assert "thickness_forget(oldest);" in reuse
    _build_and_run(cc, tmp_path, "thickness_lookup", r"""
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "xgpu_stream.h"
typedef uint32_t D3DCOLOR;
enum { XGPU_SOURCE_NONE, XGPU_SOURCE_TEXTURE };
struct xgpu_vulkan_image { int destroyed; };
struct entry
{
    uint32_t generation;
    struct xgpu_vulkan_image *thickness;
    uint32_t thickness_generation, thickness_failed;
};
static struct { uint32_t thickness_epoch; } cache;
static struct entry the_entry;
static struct xgpu_vulkan_image images[4];
static unsigned bakes, made;
static void xgpu_vulkan_image_destroy(struct xgpu_vulkan_image *image) { image->destroyed = 1; }
static struct entry *xbox_entry_current(const struct xgpu_stream_texture_stage *stage, const D3DCOLOR **palette)
{
    (void)stage;
    *palette = NULL;
    return &the_entry;
}
static struct xgpu_vulkan_image *thickness_bake(struct entry *entry, const D3DCOLOR *palette)
{
    (void)palette;
    bakes++;
    entry->thickness = &images[made++];
    entry->thickness_generation = entry->generation;
    return entry->thickness;
}
""" + forget + lookup + r"""
int main(void)
{
    struct xgpu_stream_texture_stage stage;
    struct xgpu_vulkan_image *first;

    memset(&stage, 0, sizeof(stage));
    stage.source = XGPU_SOURCE_TEXTURE;
    stage.resource = 0x1000;
    /* never read: nothing baked */
    assert(!xgpu_vulkan_textures_thickness(&stage) && !bakes);
    /* read: baked once, then kept */
    the_entry.generation = 3;
    first = xgpu_vulkan_textures_thickness(&stage);
    assert(first == &images[0] && bakes == 1);
    assert(xgpu_vulkan_textures_thickness(&stage) == first && bakes == 1);
    assert(xgpu_vulkan_textures_thickness_epoch() == 0);
    /* read again with another palette, at the same generation: the old
    thickness destroyed, the epoch moved on, baked again */
    thickness_forget(&the_entry);
    assert(first->destroyed && !the_entry.thickness && xgpu_vulkan_textures_thickness_epoch() == 1);
    assert(xgpu_vulkan_textures_thickness(&stage) == &images[1] && bakes == 2);
    /* (forgetting an entry with none moves nothing on) */
    the_entry.thickness = NULL;
    thickness_forget(&the_entry);
    assert(xgpu_vulkan_textures_thickness_epoch() == 1);
    return 0;
}
""")
