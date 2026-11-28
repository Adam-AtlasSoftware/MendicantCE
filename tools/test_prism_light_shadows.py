"""Enhanced lighting's lights' shadows (display.enhanced_light_shadows, docs/prism_bake.md "The lights' shadows"):
the setting (its default the best, its row on Enhanced Lighting's page, greyed with the traced rows where the card
cannot trace), its way from the game to the light pass (prism_deferred.c's resolve phase bits, the stream's, the
host's, the pass's push flags), the dynamic lights given to the light pass with either composition, the pass's
shadows as Path Traced's (the same owner margin and light size), and a model of the pass's arithmetic: the lights a
pixel's shadows are traced for, the light a shadow takes, the baked lights' share the objects' shadows take."""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CONFIG = (ROOT / "port/linux/src/port_config.c").read_text()
MENU = (ROOT / "port/linux/game/menu_functions.c").read_text()
DEFERRED = (ROOT / "port/linux/game/prism_deferred.c").read_text()
STREAM_H = (ROOT / "port/linux/src/xgpu_stream.h").read_text()
HOST = (ROOT / "port/linux/vulkan/xgpu_vulkan_stream.c").read_text()
BAKE = (ROOT / "port/linux/vulkan/xgpu_vulkan_bake.c").read_text()
LIGHTS = (ROOT / "port/linux/game/prism_scene_lights.c").read_text()
PT = (ROOT / "port/linux/vulkan/xgpu_vulkan_pt.glsl").read_text()


def _glsl():
    """the raster pass's source as GLSL (the .inc's C strings joined)"""
    text = (ROOT / "port/linux/vulkan/xgpu_vulkan_bake_glsl.inc").read_text()
    body = text.split("static const char raster_source[] =", 1)[1].split(";\n", 1)[0]
    return "".join(re.findall(r'"((?:\\.|[^"\\])*)"', body)).replace("\\n", "\n").replace('\\"', '"')


GLSL = _glsl()


def _define(text, name):
    return int(re.search(r"#define %s (0x[0-9a-fA-F]+|\d+)" % name, text).group(1), 0)


# ---------- the setting

def test_setting_default_is_the_best():
    match = re.search(r'\{ "display.enhanced_light_shadows", _config_string, "\\"(\w+)\\"", "HALO_ENHANCED_LIGHT_SHADOWS"',
                      CONFIG)
    assert match and match.group(1) == "ultra"
    assert '{ "display.enhanced_light_shadows", { "ultra", "high", "low", "off", NULL }, "ultra" }' in CONFIG
    assert '{ "display.enhanced_light_shadows", LIGHTING_ENHANCED }' in CONFIG


def test_row_on_enhanced_page_greyed_with_traced_rows():
    import sys
    sys.path.insert(0, str(ROOT / "tools"))
    import port_settings
    assert port_settings.LIGHTING_MODE_ROWS["enhanced_light_shadows"] == "E"
    rows = [row for page in port_settings.PAGES.values() for row in page["rows"]
            if row[0] != port_settings.SECTION and row[1] == "display.enhanced_light_shadows"]
    assert len(rows) == 1
    assert [value for _shown, value in rows[0][2]] == ["ultra", "high", "low", "off"]
    assert '{ "op_enhanced_light_shadows", LIGHTING_E, NULL, NULL }' in MENU
    rt_rows = MENU.split("static void prism_rt_rows_show(", 1)[1].split("};", 1)[0]
    assert '"op_enhanced_light_shadows", "enhanced_light_shadows_spinner"' in rt_rows


def test_setting_reaches_the_pass():
    # (the game's bits are the stream's)
    shift = _define(STREAM_H, "XGPU_GBUFFER_PHASE_RASTER_LIGHT_SHADOWS_SHIFT")
    mask = _define(STREAM_H, "XGPU_GBUFFER_PHASE_RASTER_LIGHT_SHADOWS_MASK")
    assert mask == 3 << shift
    assert _define(DEFERRED, "PHASE_RASTER_LIGHT_SHADOWS_SHIFT") == shift
    assert "prism_deferred.raster_light_shadows << PHASE_RASTER_LIGHT_SHADOWS_SHIFT" in DEFERRED
    assert 'config_string("display.enhanced_light_shadows")' in DEFERRED
    # (clear of Raster's other bits and of the view's)
    for name in ("XGPU_GBUFFER_PHASE_RASTER", "XGPU_GBUFFER_PHASE_RASTER_MATERIALS",
                 "XGPU_GBUFFER_PHASE_RASTER_OBJECTS_VOLUME", "XGPU_GBUFFER_PHASE_VIEW_MASK"):
        assert not _define(STREAM_H, name) & mask, name
    assert "XGPU_GBUFFER_PHASE_RASTER_LIGHT_SHADOWS_MASK) >>" in HOST
    assert "raster->light_shadows, whole" in HOST
    # (the pass's push flags, bits 8 and 9, never in the bake's views)
    assert "flags |= (light_shadows & 3u) << 8;" in BAKE
    assert "uint light_shadows() { return (push.parameters.w >> 8u) & 3u; }" in GLSL


def test_dynamic_lights_with_either_composition():
    """CE's diffuse dynamic light passes draw into the light target, whose light the pass replaces: the pass lights
    the dynamic lights itself with CE's composition too (before, the flashlight lit nothing the bake lit there)"""
    fill = BAKE.split("struct xgpu_vulkan_image *xgpu_vulkan_bake_raster(", 1)[1].split("job.volume[2]", 1)[0]
    assert "lights = xgpu_vulkan_scene_light_table(&light_count, &serial);" in fill
    assert "materials ? xgpu_vulkan_scene_light_table" not in fill
    window = LIGHTS.split("void prism_scene_lights_window(", 1)[1].split("\n}\n", 1)[0]
    assert "prism_deferred_raster()" in window
    # (with CE's composition after the exposure and the curve, in CE's encoding)
    ce = GLSL.split("CE's composition (display.prism_raster_composition", 1)[1]
    assert ce.index("light_ = tone_curved(light_, curve);") < ce.index("light_ += dynamic;")


def test_shadows_as_path_traced():
    owner = float(re.search(r"const float DYNAMIC_OWNER = ([\d.]+);", PT).group(1))
    assert float(re.search(r"DYNAMIC_OWNER = ([\d.]+)", GLSL).group(1)) == owner
    # (the level to the light's place less 0.05, the objects to DYNAMIC_OWNER short of it, as light_seen)
    seen = GLSL.split("float dynamic_seen(", 1)[1].split("\n}\n", 1)[0]
    assert "blocked(o, l, max(d - 0.05, 0.0))" in seen
    assert "objects_visibility(o, l, d - DYNAMIC_OWNER, cull)" in seen
    # (the player's own lights not shadowed by his body; the first person's view model is never in the structure)
    assert "distance(c, job.camera[0].xyz) < 1.0 ? (cull & ~16u) : cull" in GLSL


# ---------- a model of the pass's arithmetic

MOST = {0: 0, 1: 2, 2: 4, 3: 8}


def dynamic_at(lights, quality, seen):
    """the pass's dynamic_at: lights (luminance, light) at the pixel; seen(i) the share of light i the rays see.
    Every light lights the pixel; the most that light it (at least 1/500 of white) are shadowed"""
    most = MOST[quality]
    total = sum(e for _w, e in lights)
    kept = sorted([(w, i) for i, (w, _e) in enumerate(lights) if w >= 0.002], key=lambda k: -k[0])[:most]
    for _w, i in kept:
        total -= lights[i][1] * (1.0 - seen(i))
    return max(total, 0.0), [i for _w, i in kept]


def test_model_brightest_shadowed_rest_unshadowed():
    lights = [(0.1, 0.1), (0.5, 0.5), (0.001, 0.001), (0.3, 0.3), (0.2, 0.2), (0.05, 0.05)]
    light, kept = dynamic_at(lights, 1, lambda i: 0.0)
    assert kept == [1, 3]
    assert abs(light - (0.1 + 0.001 + 0.2 + 0.05)) < 1e-9
    light, kept = dynamic_at(lights, 3, lambda i: 0.0)
    assert sorted(kept) == [0, 1, 3, 4, 5]
    assert abs(light - 0.001) < 1e-9
    assert dynamic_at(lights, 0, lambda i: 0.0)[0] == sum(e for _w, e in lights)
    # (a shadow takes at most the light: never under none)
    assert dynamic_at(lights, 3, lambda i: -1.0)[0] >= 0.0


def test_glsl_keeps_the_brightest():
    """the GLSL's insertion keeps the brightest first and drops the faintest: run on its own words"""
    body = GLSL.split("vec3 dynamic_at(vec3 p, vec3 n, vec3 o, uint cull)", 1)[1]
    assert "if (most == 0 || !(w >= 0.002) || (kept == most && w <= kept_w[most - 1]))" in body
    assert "for (; j > 0 && kept_w[j - 1] < w; j--)" in body

    def insert(ws, most):
        kept_w, kept = [0.0] * most, 0
        for w in ws:
            if most == 0 or not w >= 0.002 or (kept == most and w <= kept_w[most - 1]):
                continue
            j = min(kept, most - 1)
            while j > 0 and kept_w[j - 1] < w:
                kept_w[j] = kept_w[j - 1]
                j -= 1
            kept_w[j] = w
            kept = min(kept + 1, most)
        return kept_w[:kept]

    import random
    rng = random.Random(7)
    for _ in range(500):
        ws = [rng.random() ** 3 for _ in range(rng.randrange(0, 20))]
        for most in (2, 4, 8):
            assert insert(ws, most) == sorted([w for w in ws if w >= 0.002], reverse=True)[:most]


def test_baked_lights_never_lit_twice():
    """the objects' shadows of the baked lights take only light the bake gave (the level not blocking the light),
    from the baked light, never under a tenth of it"""
    body = GLSL.split("vec3 baked_lights_blocked(", 1)[1].split("\n}\n", 1)[0]
    assert "job.objects == uvec2(0u)" in body
    assert "lamp_visible(o, l_[best].xyz, l_[best].w)" in body
    assert "!blocked(o, l, max(d - 0.05, 0.0))" in body
    assert "lit *= max(1.0 - luma(taken) / had, 0.1);" in GLSL

    def lit_after(lit, taken):
        return lit * max(1.0 - taken / lit, 0.1) if lit > 1e-5 and taken > 0.0 else lit
    assert lit_after(1.0, 0.0) == 1.0
    assert abs(lit_after(1.0, 0.4) - 0.6) < 1e-9
    assert abs(lit_after(1.0, 5.0) - 0.1) < 1e-9
