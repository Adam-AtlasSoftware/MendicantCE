"""PRISM v2 Elite: surface maps (the overlay's display.prism_surface_maps) baked from a high-poly onto
the in-game mesh, at the base maps' own coordinates. Headless Blender, Cycles on every GPU:

  blender -b --factory-startup --python art/prism_elite/elite_bake.py -- <recipe.json>

Per base map bitmap (each Elite shader's): RGBA PNG at bake_size (RG the tangent-space normal's x and
y, y up the picture; B the ambient occlusion, 1 open; A the roughness), as mod_materials.c reads it.
  high-poly  the in-game mesh (build.blend's new/<part>) one Catmull-Clark level smoother, its
             shading bent by a height field made from the detail CE paints into the base map (all
             cited by where it is: the class split is the multipurpose map's armour channel):
               armour  the thin dark lines CE paints on the plates (panel seams, the trim
                       grooves) as grooves: a black top-hat of the base map's luminance;
               bodysuit / skin  the scale and pebble pattern CE paints (the hex cells of the
                       bodysuit, the head's mottling) as relief: a band-pass of the luminance;
               lights (self-illumination) flat.
  bake       tangent-space normal and AO (the whole Elite as occluder), selected to active, CE's
             own UVs; where two parts share a bitmap (base00 and blunt arms) the first fills, the
             second only where the first left none; then a push-pull fill outside the islands.
  roughness  per class (armour polished, bodysuit matte, skin between), seams rougher.
Also writes heights/<bitmap>.png and the overlay.json entries (overlay_surface.json).
Derived from CE vertices and pixels: local only, never shipped.
"""
import json
import math
import os
import sys
import time

import bpy
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)
import bl_util as U  # noqa: E402

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import packs_env as E  # noqa: E402

ROOT = E.WORK
T0 = time.time()


def P(rel):
    return E.path(rel)


def log(*a):
    print("[%6.1fs]" % (time.time() - T0), *a, flush=True)


def load_rgba(path):
    img = bpy.data.images.load(path, check_existing=False)
    w, h = img.size
    a = np.empty(w * h * 4, np.float32)
    img.pixels.foreach_get(a)
    bpy.data.images.remove(img)
    return a.reshape(h, w, 4)   # row 0 = bottom


def blur(a, sigma):
    if sigma <= 0:
        return a.copy()
    r = max(1, int(3 * sigma))
    k = np.exp(-0.5 * (np.arange(-r, r + 1) / sigma) ** 2)
    k /= k.sum()
    out = np.zeros_like(a)
    for i, w in enumerate(k):
        out += w * np.roll(a, i - r, axis=0)
    a2, out = out, np.zeros_like(a)
    for i, w in enumerate(k):
        out += w * np.roll(a2, i - r, axis=1)
    return out


def upsample(a, f):
    """bilinear upsampling by an integer factor (wrapping), texel centres kept"""
    h, w = a.shape[:2]
    ys = (np.arange(h * f) + 0.5) / f - 0.5
    xs = (np.arange(w * f) + 0.5) / f - 0.5
    y0 = np.floor(ys).astype(int)
    x0 = np.floor(xs).astype(int)
    fy = (ys - y0)[:, None]
    fx = (xs - x0)[None, :]
    if a.ndim == 3:
        fy, fx = fy[..., None], fx[..., None]
    y0m, y1m = y0 % h, (y0 + 1) % h
    x0m, x1m = x0 % w, (x0 + 1) % w
    r0 = a[y0m][:, x0m] * (1 - fx) + a[y0m][:, x1m] * fx
    r1 = a[y1m][:, x0m] * (1 - fx) + a[y1m][:, x1m] * fx
    return r0 * (1 - fy) + r1 * fy


def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0, 1)
    return t * t * (3 - 2 * t)


def line_keep(s, length, lo, hi, angles=12):
    """the weight of the texels of `s` that lie on a line at least `length` texels long: the best mean of s
    along a segment of that length through the texel (over `angles` directions) over the texel's own
    value. A line keeps about 1; a painted speck or a short dash (CE's chips and compression noise on the
    plates, which became pits like water drops) falls to its length over `length`."""
    h, w = s.shape
    best = np.zeros_like(s)
    half = max(1, int(round(length / 2)))
    pad = np.pad(s, half, mode="constant")
    for k in range(angles):
        th = math.pi * k / angles
        dx, dy = math.cos(th), math.sin(th)
        acc = np.zeros_like(s)
        for t in range(-half, half + 1):
            ox, oy = int(round(t * dx)), int(round(t * dy))
            acc += pad[half + oy:half + oy + h, half + ox:half + ox + w]
        np.maximum(best, acc / (2 * half + 1), out=best)
    ratio = best / np.maximum(s, 1e-3)
    return smoothstep(lo, hi, np.clip(ratio, 0, 1))


def detail_height(base, multi, cfg, skin):
    """height in [-1, 1] at f x CE's size, and the class weights (armour, glow) at that size"""
    f = cfg.get("height_factor", 4)
    rgb = upsample(base[..., :3], f)
    lum = 0.2126 * rgb[..., 0] + 0.7152 * rgb[..., 1] + 0.0722 * rgb[..., 2]
    mu = upsample(multi, f)
    pc = cfg["plates"]
    arm = smoothstep(pc.get("lo", 0.35), pc.get("hi", 0.65), mu[..., "rgba".index(pc.get("channel", "b"))])
    glow = smoothstep(0.06, 0.14, mu[..., "rgba".index(pc.get("exclude_channel", "g"))])
    hc = cfg["height"]
    # armour: the thin dark lines (black top-hat: the local mean minus the texel, kept where it is a
    # line, not a broad shadow)
    s = np.clip(blur(lum, hc.get("seam_sigma", 1.6) * f / 4) - lum, 0, None)
    broad = np.clip(blur(lum, hc.get("seam_broad_sigma", 6.0) * f / 4) - blur(lum, hc.get("seam_sigma", 1.6) * f / 4), 0, None)
    s = np.clip(s - 0.5 * broad, 0, None)
    ref = np.percentile(s[arm > 0.5], 99) if (arm > 0.5).any() else 1.0
    seams = smoothstep(hc.get("seam_lo", 0.22), hc.get("seam_hi", 0.75), s / max(ref, 1e-6)) * arm
    if hc.get("seam_min_len", 0) > 0:
        seams = seams * line_keep(seams, hc["seam_min_len"] * f, hc.get("seam_line_lo", 0.3),
                                  hc.get("seam_line_hi", 0.6))
    # bodysuit / skin: the painted cells and pebbles (band-pass, normalised by its own spread)
    band = blur(lum, hc.get("pebble_fine", 0.5) * f / 4) - blur(lum, hc.get("pebble_coarse", 2.2) * f / 4)
    # (the relief only where the texel is clearly bodysuit or skin: none on plates, even where CE's
    # armour channel dips at painted wear)
    soft = np.clip(1 - 3 * arm, 0, 1) * (1 - glow)
    sd = band[soft > 0.5].std() if (soft > 0.5).any() else 1.0
    peb = np.clip(band / max(2.0 * sd, 1e-6), -1, 1) * soft
    amp = hc.get("skin_amp", 0.45) if skin else hc.get("pebble_amp", 0.7)
    hgt = -hc.get("seam_depth", 1.0) * seams + amp * peb
    hgt = blur(hgt, 0.6 * f / 4)
    return hgt, arm, glow, seams


def write_png(path, a):
    """RGBA float [0, 1] array (row 0 = bottom, Blender's) to an 8-bit straight-alpha PNG (Blender's
    own save would un-premultiply by the roughness in A)"""
    import struct
    import zlib
    b = (np.clip(np.flipud(a), 0, 1) * 255 + 0.5).astype(np.uint8)
    h, w = b.shape[:2]
    raw = b"".join(b"\0" + b[y].tobytes() for y in range(h))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def float_image(name, w, h, data=None, non_color=True):
    img = bpy.data.images.new(name, w, h, alpha=True, float_buffer=True)
    img.colorspace_settings.name = "Non-Color" if non_color else "sRGB"
    if data is not None:
        img.pixels.foreach_set(np.ascontiguousarray(data, np.float32).ravel())
    else:
        img.pixels.foreach_set(np.zeros(w * h * 4, np.float32))
    return img


def image_np(img):
    w, h = img.size
    a = np.empty(w * h * 4, np.float32)
    img.pixels.foreach_get(a)
    return a.reshape(h, w, 4)


def push_pull(a, mask, levels=10):
    """fill the texels outside mask by the averages of coarser levels (no seams at the islands)"""
    a = a.copy()
    m = mask.astype(np.float32)
    pyr = [(a * m[..., None], m)]
    for _ in range(levels):
        x, w = pyr[-1]
        h2, w2 = x.shape[0] // 2, x.shape[1] // 2
        if h2 < 1 or w2 < 1:
            break
        xs = x[:h2 * 2, :w2 * 2].reshape(h2, 2, w2, 2, -1).sum((1, 3))
        ws = w[:h2 * 2, :w2 * 2].reshape(h2, 2, w2, 2).sum((1, 3))
        pyr.append((xs, ws))
    est = pyr[-1][0] / np.maximum(pyr[-1][1], 1e-6)[..., None]
    for x, w in reversed(pyr[:-1]):
        up = np.repeat(np.repeat(est, 2, 0), 2, 1)
        up = np.pad(up, ((0, x.shape[0] - up.shape[0]), (0, x.shape[1] - up.shape[1]), (0, 0)), mode="edge")
        cur = x / np.maximum(w, 1e-6)[..., None]
        k = np.clip(w, 0, 1)[..., None]
        est = cur * k + up * (1 - k)
    return np.where(mask[..., None], a, est)


def wrap_uvs(obj):
    """a "bake" UV layer: CE's UVs moved by whole tiles per face into [0, 1) (Cycles bakes the first
    tile only; CE's Elite coordinates run over [1, 2] x [-1, 0] and repeat), made the active one"""
    me = obj.data
    src = me.uv_layers["UVMap"]
    uv = np.empty(len(me.loops) * 2, np.float32)
    src.data.foreach_get("uv", uv)
    uv = uv.reshape(-1, 2)
    off = np.zeros_like(uv)
    for p in me.polygons:
        li = list(p.loop_indices)
        off[li] = np.floor(uv[li].mean(0))
    dst = me.uv_layers.get("bake") or me.uv_layers.new(name="bake")
    dst.data.foreach_set("uv", (uv - off).ravel())
    me.uv_layers.active = dst
    dst.active_render = True


def setup_cycles(sc, samples):
    sc.render.engine = "CYCLES"
    prefs = bpy.context.preferences.addons["cycles"].preferences
    _backend, used = E.cycles_gpu(prefs)
    sc.cycles.device = "GPU" if used else "CPU"
    sc.cycles.samples = samples
    return used


def main():
    cfg = U.load_config(U.script_args()[0])
    bc = cfg["bake"]
    out = P(bc["out_dir"])
    os.makedirs(os.path.join(out, "heights"), exist_ok=True)
    bpy.ops.wm.open_mainfile(filepath=os.path.join(P(cfg["out_dir"]), "build.blend"))
    sc = bpy.context.scene
    used = setup_cycles(sc, bc.get("ao_samples", 128))
    log("devices", used)
    sc.render.bake.use_selected_to_active = True
    sc.render.bake.cage_extrusion = bc.get("cage_extrusion_wu", 0.004)
    sc.render.bake.max_ray_distance = bc.get("max_ray_wu", 0.012)
    sc.render.bake.margin = 0
    sc.render.bake.target = "IMAGE_TEXTURES"
    world = bpy.data.worlds.new("bake_w")
    sc.world = world
    world.light_settings.distance = bc.get("ao_distance_wu", 0.07)
    bmp_dir = P(cfg["bitmap_dir"])
    news = [o for o in bpy.data.objects if o.type == "MESH" and o.name.startswith("new/")]
    for o in bpy.data.objects:
        if o.type == "MESH":
            o.hide_render = True
            o.hide_set(False)
    # the high-poly of every new part: one Catmull-Clark level smoother, with the detail height as bump
    highs = {}
    for o in news:
        hi = o.copy()
        hi.data = o.data.copy()
        hi.name = "high/" + o.name[4:]
        sc.collection.objects.link(hi)
        for m in list(hi.modifiers):
            if m.type != "ARMATURE":
                hi.modifiers.remove(m)
        s = hi.modifiers.new("s", "SUBSURF")
        s.levels = s.render_levels = bc.get("high_levels", 1)
        s.uv_smooth = "NONE"
        s.use_creases = True
        highs[o.name] = hi
        wrap_uvs(o)
    results = {}
    heights = {}
    surfaces = {}
    for bitmap, spec in bc["bitmaps"].items():
        base = load_rgba(os.path.join(bmp_dir, bitmap + ".png"))
        multi = load_rgba(os.path.join(bmp_dir, bitmap + " multi.png"))
        hgt, arm, glow, seams = detail_height(base, multi, cfg, spec.get("skin", False))
        hh, hw = hgt.shape
        himg = float_image("h_" + bitmap, hw, hh, np.dstack([hgt * 0.5 + 0.5] * 3 + [np.ones_like(hgt)]))
        heights[bitmap] = himg
        himg.filepath_raw = os.path.join(out, "heights", bitmap + ".png")
        himg.file_format = "PNG"
        himg.save()
        bw, bh = spec.get("size", [2048, 2048])
        acc = {"normal": np.zeros((bh, bw, 4), np.float32), "ao": np.zeros((bh, bw, 4), np.float32)}
        cover = np.zeros((bh, bw), bool)
        for part, shader in spec["parts"]:
            low = bpy.data.objects["new/" + part]
            hi = highs[low.name]
            # materials: the high's slots of this shader bump by the height; the low's get the target
            tgts = {k: float_image("t_%s_%s_%s" % (k, bitmap, part), bw, bh) for k in ("normal", "ao")}
            lm = bpy.data.materials.new("low_target")
            lm.use_nodes = True
            for k, im in tgts.items():
                tn = lm.node_tree.nodes.new("ShaderNodeTexImage")
                tn.image = im
                tn.name = k
            for slot in low.material_slots:
                slot.material = lm
            for kind in ("normal", "ao"):
                tgt = tgts[kind]
                lm.node_tree.nodes.active = lm.node_tree.nodes[kind]
                for slot in hi.material_slots:
                    sh = (slot.material.name if slot.material else "").replace("/", "\\").split("\\")[-1].split(".")[0].lower() \
                        if not slot.material or not slot.material.name.startswith("hi_") else slot.material["shader"]
                    hm = bpy.data.materials.new("hi_" + sh)
                    hm["shader"] = sh
                    hm.use_nodes = True
                    nt = hm.node_tree
                    bsdf = nt.nodes["Principled BSDF"]
                    if sh == shader:
                        tx = nt.nodes.new("ShaderNodeTexImage")
                        tx.image = himg
                        tx.interpolation = "Cubic"
                        uvn = nt.nodes.new("ShaderNodeUVMap")
                        uvn.uv_map = "UVMap"
                        nt.links.new(uvn.outputs[0], tx.inputs[0])
                        bump = nt.nodes.new("ShaderNodeBump")
                        bump.inputs["Strength"].default_value = 1.0
                        bump.inputs["Distance"].default_value = bc.get("bump_distance_wu", 0.0016)
                        nt.links.new(tx.outputs[0], bump.inputs["Height"])
                        nt.links.new(bump.outputs[0], bsdf.inputs["Normal"])
                    slot.material = hm
                for o in bpy.data.objects:
                    o.select_set(False)
                    o.hide_render = True
                # AO sees the whole Elite of this permutation set (its highs), the normal its own high
                region = part.split("/")[0]
                occl = [highs[n.name] for n in news if n.name.split("/")[1] != region and
                        n.name.split("/")[2] == "base00"] if kind == "ao" else []
                for o in occl + [hi]:
                    o.hide_render = False
                low.hide_render = False
                # the low (the target) is not seen by the AO's rays: only the high-poly and the rest
                for flag in ("visible_camera", "visible_diffuse", "visible_glossy", "visible_transmission",
                             "visible_volume_scatter", "visible_shadow"):
                    setattr(low, flag, False)
                hi.select_set(True)
                low.select_set(True)
                bpy.context.view_layer.objects.active = low
                t = time.time()
                if kind == "normal":
                    sc.cycles.samples = bc.get("normal_samples", 8)
                    bpy.ops.object.bake(type="NORMAL", normal_space="TANGENT", use_selected_to_active=True,
                                        cage_extrusion=sc.render.bake.cage_extrusion,
                                        max_ray_distance=sc.render.bake.max_ray_distance, margin=0, use_clear=False)
                else:
                    sc.cycles.samples = bc.get("ao_samples", 128)
                    bpy.ops.object.bake(type="AO", use_selected_to_active=True,
                                        cage_extrusion=sc.render.bake.cage_extrusion,
                                        max_ray_distance=sc.render.bake.max_ray_distance, margin=0, use_clear=False)
                a = image_np(tgt)
                if kind == "normal":
                    got = a[..., :3].sum(2) > 0
                    new_cover = got & ~cover
                else:
                    got = new_cover | (got & cover)
                acc[kind] = np.where((got & ~cover)[..., None], a, acc[kind])
                results["%s %s %s" % (bitmap, part, kind)] = {"seconds": round(time.time() - t, 1),
                                                              "texels": int(got.sum())}
                log(bitmap, part, kind, results["%s %s %s" % (bitmap, part, kind)])
            cover |= new_cover
        if os.environ.get("ELITE_BAKE_DEBUG"):
            np.save(os.path.join(out, "debug_%s_normal.npy" % bitmap.replace(" ", "_")), acc["normal"])
            np.save(os.path.join(out, "debug_%s_ao.npy" % bitmap.replace(" ", "_")), acc["ao"])
            np.save(os.path.join(out, "debug_%s_cover.npy" % bitmap.replace(" ", "_")), cover)
        # assemble: RG normal, B AO, A roughness; filled outside the islands
        n = acc["normal"][..., :3] * 2 - 1
        n /= np.maximum(np.linalg.norm(n, axis=2, keepdims=True), 1e-6)
        ao = np.clip(acc["ao"][..., 0], 0, 1)
        ao = 1 - bc.get("ao_strength", 1.0) * (1 - ao)
        # roughness by class at the bake size
        f = bw // arm.shape[1] if bw >= arm.shape[1] else 1
        def at_size(x):
            y = x
            if y.shape[1] != bw:
                y = upsample(y, max(1, bw // y.shape[1])) if bw > y.shape[1] else y[::y.shape[0] // bh, ::y.shape[1] // bw]
            return y[:bh, :bw]
        arm_b, glow_b, seam_b = at_size(arm), at_size(glow), at_size(seams)
        r = bc["roughness"]
        rough = (arm_b * r["armour"] + (1 - arm_b) * (r["skin"] if spec.get("skin") else r["bodysuit"]))
        rough = np.clip(rough + r.get("seam_add", 0.2) * seam_b, 0, 1)
        rough = np.where(glow_b > 0.5, r.get("glow", 0.6), rough)
        rgba = np.dstack([n[..., 0] * 0.5 + 0.5, n[..., 1] * 0.5 + 0.5, ao, rough]).astype(np.float32)
        rgba = push_pull(rgba, cover)
        stats = {"coverage": float(cover.mean()), "slope_rms": float(np.sqrt(((n[..., :2] / np.maximum(n[..., 2:3], 0.3)) ** 2).sum(2)[cover].mean())),
                 "ao_mean": float(ao[cover].mean()), "rough_mean": float(rough[cover].mean())}
        results[bitmap] = stats
        name = "characters_elite_%s.png" % bitmap.replace(" ", "_")
        write_png(os.path.join(out, name), rgba)
        surfaces[bitmap] = name
        log("SURFACE", bitmap, stats)
    # overlay entries: the shaders whose base map is the bitmap
    entries = {}
    for sh, bitmap in bc["shader_bitmaps"].items():
        if bitmap in surfaces:
            entries["characters\\elite\\shaders\\" + sh] = {"surface_map": ["surface/" + surfaces[bitmap]],
                                                            "surface": bc["strengths"]}
    with open(os.path.join(out, "overlay_surface.json"), "w") as fh:
        json.dump(entries, fh, indent=1)
    with open(os.path.join(out, "bake_report.json"), "w") as fh:
        json.dump({"devices": used, "results": results, "seconds": round(time.time() - T0, 1)}, fh, indent=1)
    print("BAKE_OK", round(time.time() - T0, 1))


if __name__ == "__main__":
    main()
