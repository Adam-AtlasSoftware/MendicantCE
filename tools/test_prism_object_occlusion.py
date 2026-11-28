"""PRISM Raster's object occlusion (display.prism_raster_object_occlusion, docs/prism_bake.md "The moving objects'
occlusion"): the ambient light the moving objects take from what is about them, traced against their structure in
Enhanced lighting, which the bake (made without them) cannot hold.

- The occlusion pass's directions (xgpu_vulkan_bake_glsl.inc's occlusion_source, read from the source) are, over any
  4 x 4 block of pixels the light pass gathers, every one of the block's directions once: their estimate of a known
  occluder (a ceiling over a floor, a wall beside it) is the converged integral's within a few percent, with no
  history and no noise left.
- The level is darkened by the objects alone (the bake has its own occlusion), only the ambient light (the light less
  the sun's, and the reflections) is darkened, never the sun's or the dynamic lights'.
- The game asks for it in RT_FRAME (PRISM_RT_FLAG_RASTER_OCCLUSION, the renderer's XGPU_RT_FLAG_RASTER_OCCLUSION, the
  same bit), which captures the objects with or without the traced sun shadows; the sun shadows stay CE's with Object
  Shadows Classic.
- The setting is Enhanced's, on by default, a row of Enhanced Lighting's ray tracing shown while Ray Tracing is on.
"""
import math
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GLSL = (ROOT / "port/linux/vulkan/xgpu_vulkan_bake_glsl.inc").read_text()
BAKE = (ROOT / "port/linux/vulkan/xgpu_vulkan_bake.c").read_text()
CONFIG = (ROOT / "port/linux/src/port_config.c").read_text()


def _source(name):
    """a shader's GLSL, its strings joined (the C preprocessor's macros left as their names)"""
    body = GLSL.split("static const char %s_source[] =" % name, 1)[1].split('";\n', 1)[0] + '"'
    return "".join(re.findall(r'"((?:\\.|[^"\\])*)"', body)).replace("\\n", "\n")


OCCLUSION = _source("occlusion")
RASTER = _source("raster")


def _constants():
    dither = [int(v) for v in re.search(r"const uint dither\[16\] = uint\[16\]\(([^)]*)\)", OCCLUSION).group(1)
              .replace("u", "").split(",")]
    angle = float(re.search(r"float a = j \* ([\d.]+);", OCCLUSION).group(1))
    rays = int(re.search(r"#define OCCLUSION_RAYS (\d+)u", BAKE).group(1))
    reach = {name: float(re.search(r"#define OCCLUSION_%s ([\d.]+)f" % name, BAKE).group(1))
             for name in ("LEVEL_REACH", "OBJECT_REACH", "LEVEL_STRENGTH", "OBJECT_STRENGTH")}
    return dither, angle, rays, reach


def _directions(k, rays, dither, angle):
    """the pass's directions of a pixel at (x, y) in its block (k = x + 4y), about the normal +z"""
    total = 16 * rays
    out = []
    for r in range(rays):
        j = r * 16 + dither[k]
        u = (j + 0.5) / total
        a = j * angle
        s = math.sqrt(u)
        out.append((s * math.cos(a), s * math.sin(a), math.sqrt(max(1.0 - u, 0.0))))
    return out


def _occluded(d, hit, reach):
    t = hit(d)
    return 1.0 - (t / reach) ** 2 if t < reach else 0.0


def test_every_window_has_every_direction():
    """any 4 x 4 window of pixels (occlusion_at's, offsets -2 to 1) holds each of the dither's 16 places once"""
    dither, _angle, _rays, _reach = _constants()
    assert sorted(dither) == list(range(16))
    for ox in range(4):
        for oy in range(4):
            seen = sorted(dither[((ox + i) & 3) + 4 * ((oy + j) & 3)] for i in range(4) for j in range(4))
            assert seen == list(range(16)), (ox, oy)
    assert "ivec2((k & 3) - 2, (k >> 2) - 2)" in RASTER


def test_block_estimate_matches_the_integral():
    """a block's directions (16 x the rays a pixel) against a ceiling over the floor (a Warthog's underside) and a
    wall beside a point (a Marine's legs): within 3% of the cosine-weighted integral, 200,000 samples'"""
    dither, angle, rays, reach = _constants()
    level = reach["LEVEL_REACH"]
    block = [d for k in range(16) for d in _directions(k, rays, dither, angle)]
    # (the cosine distribution's mean cosine is 2/3)
    assert abs(sum(d[2] for d in block) / len(block) - 2.0 / 3.0) < 0.02
    cases = {
        "ceiling": lambda d, h=0.25: h / d[2] if d[2] > 1e-6 else 1e9,
        "wall": lambda d, x=0.3: x / d[0] if d[0] > 1e-6 else 1e9,
        "far ceiling": lambda d, h=1.4: h / d[2] if d[2] > 1e-6 else 1e9,
    }
    for name, hit in cases.items():
        estimate = sum(_occluded(d, hit, level) for d in block) / len(block)
        n = 200000
        reference = 0.0
        for i in range(n):
            u = (i + 0.5) / n
            a = i * 2.39996323
            s = math.sqrt(u)
            reference += _occluded((s * math.cos(a), s * math.sin(a), math.sqrt(1.0 - u)), hit, level)
        reference /= n
        assert abs(estimate - reference) < 0.03, (name, estimate, reference)


def test_level_darkened_by_objects_alone_and_ambient_only():
    """the pass traces the objects' structure only (never the bake's scene: s.tlas); the light pass multiplies the
    light less the sun's (lit) and the reflections, never the sun's light or the dynamic lights"""
    assert "job.objects" in OCCLUSION and "s.tlas" not in OCCLUSION and "sky_blocked" not in OCCLUSION
    assert "hit_t < reach" in OCCLUSION and "1.0 - (hit_t / reach) * (hit_t / reach)" in OCCLUSION
    # (the first-person weapon, the sky and a pixel without a normal: none found)
    assert "instance == 0xffffu" in OCCLUSION
    applied = RASTER.split("float ambient_left = 1.0;", 1)[1].split("vec3 result = whole", 1)[0]
    assert "lit *= ambient_left;" in applied and "sun" not in applied and "dynamic" not in applied
    assert "(push.parameters.w & 8u) != 0u" in applied and "view == 0u" in applied
    assert "reflected * ambient_left * spec_share" in RASTER
    # (the gather never crosses from the level to an object, or from one object to another)
    gather = RASTER.split("float occlusion_at(", 1)[1].split("\n}\n", 1)[0]
    assert "o.w != instance" in gather
    assert "0xffffffffu" in OCCLUSION


def test_classic_shadow_without_traced_occlusion():
    """with the objects not traced (Ray Tracing off), CE's projected shadow darkens the light less the sun's as well
    as the sun's (its whole texel, as Classic's): never where the occlusion is traced, which would darken it twice"""
    block = RASTER.split("float ce_code = float(x.z >> 24u) / 255.0;", 1)[1].split("float v = s.sun.w", 1)[0]
    assert "job.objects == uvec2(0u)" in block
    assert "sunv *= clamp(drawn / ce_light, 0.0, 1.0);" in block
    assert "if ((push.parameters.w & 8u) == 0u)\n                    lit *= clamp(drawn / ce_light, 0.0, 1.0);" in block


def test_flag_and_capture():
    """the game's flag is the renderer's; it captures the objects alone too, and the traced sun shadows stay the
    RASTER_OBJECTS flag's (Object Shadows Classic keeps CE's projected shadows)"""
    game = (ROOT / "port/linux/game/prism_rt.c").read_text()
    stream = (ROOT / "port/linux/src/xgpu_stream.h").read_text()
    rt = (ROOT / "port/linux/vulkan/xgpu_vulkan_rt.c").read_text()
    assert re.search(r"PRISM_RT_FLAG_RASTER_OCCLUSION = 128,", game)
    assert re.search(r"XGPU_RT_FLAG_RASTER_OCCLUSION = 128,", stream)
    assert "prism_deferred_raster_object_occlusion() ? PRISM_RT_FLAG_RASTER_OCCLUSION" in game
    assert "(XGPU_RT_FLAG_RASTER_OBJECTS | XGPU_RT_FLAG_RASTER_OCCLUSION)" in rt
    raster = BAKE.split("struct xgpu_vulkan_image *xgpu_vulkan_bake_raster(", 1)[1]
    assert "frame->flags & XGPU_RT_FLAG_RASTER_OBJECTS" in raster
    assert "frame->flags & XGPU_RT_FLAG_RASTER_OCCLUSION" in BAKE.split("static struct xgpu_vulkan_image "
                                                                         "*raster_occlusion(", 1)[1]
    deferred = (ROOT / "port/linux/game/prism_deferred.c").read_text()
    # (ray tracing off traces nothing; the static scenery the bake holds is left out of the capture for either)
    off = deferred.split('if (value && !strcmp(value, "off"))', 1)[1].split("}", 1)[0]
    assert "raster_object_occlusion = FALSE" in off
    assert "(prism_deferred.raster_object_shadows ||\n\t\tprism_deferred.raster_object_occlusion)" in deferred


def test_setting():
    """Enhanced's, "traced" by default, its words "traced" and "off", shown under Ray Tracing while it is on"""
    entry = CONFIG.split('{ "display.prism_raster_object_occlusion", _config_string,', 1)[1].split("},\n", 1)[0]
    assert entry.lstrip().startswith('"\\"traced\\""') and "_platform_desktop" in entry
    assert '{ "display.prism_raster_object_occlusion", { "traced", "off", NULL }, "traced" }' in CONFIG
    assert '{ "display.prism_raster_object_occlusion", "Enhanced with display.prism_ray_tracing on" }' in CONFIG
    menu = (ROOT / "port/linux/game/menu_functions.c").read_text()
    assert '{ "op_prism_object_occlusion", LIGHTING_E, "display.prism_ray_tracing", "on" }' in menu
    assert '{ "op_prism_object_occlusion", "prism_object_occlusion_spinner" }' in menu
