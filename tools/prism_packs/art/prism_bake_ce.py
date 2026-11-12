"""PRISM v2 r2 bake: a remodel's high-poly onto its in-game mesh directly at CE's own base-map coordinates
(no second unwrap, so no fragmentation: every texel the game samples is one CE's islands own), headless
Blender + Cycles on one named GPU. Raw float bakes for prism_surface_pack.py, which packs the surface map.

  blender -b --factory-startup --python art/prism_bake_ce.py -- <remodel.blend> <out dir>
        --game <object> --high <object> --shaders armor,visor [--size 4096] [--gpu <part of a card's name>]
        [--cage-mm 6] [--ray-mm 12] [--ao-mm 30] [--samples 128]

Only the faces of the named shaders (those that share one base map: the map the surface map lines up
with) are baked. Outputs (<out>/raw_*.npy, float16, rows top-down as the picture; local only: derived
from CE vertices and pixels):
  raw_normal    RGB tangent-space normal (OpenGL: y up the picture), 0 where no ray hit the high-poly
  raw_ao        the high-poly's ambient occlusion, -1 where no ray hit it. The game mesh is hidden from
                the occlusion rays: it lies a hair above the high-poly wherever the remodel recesses,
                and seen by the rays it shadowed whole triangles black (round 1's black armour islands)
  raw_ao_game   the game mesh's own occlusion (fallback where the high-poly was missed)
  raw_cover     1 inside CE's islands of the baked faces, 0 elsewhere (no margin)
  raw_island    the UV island's number (+1) of each texel, 0 outside (islands: faces joined by edges
                whose UVs agree on both sides)
  raw_density   millimetres per texel of the face under the texel (for curvature in real units)
  raw_shader    the baked shader's number (+1, bake.json's shader_order) of each texel, 0 outside
  raw_curv_mesh the game mesh's own mean curvature, 0.5 + 0.5 tanh(k * 5 mm), k in 1/mm (convex positive)
  bake.json     settings, timings, coverage and miss shares
Options (r2 Elite): --keep-high-materials keeps the high-poly's own materials (a bump or bevel node in
them bends the baked normal: detail finer than the mesh), --occluders names more objects the occlusion
rays see (the rest of the character), --wrap-uv moves each face of the game mesh by whole UV tiles into
[0, 1) (CE's Elite runs over [1, 2] x [-1, 0] and repeats: Cycles bakes the first tile only).
"""
import argparse
import json
import os
import sys
import time

import bmesh
import bpy
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bl_util as U  # noqa: E402

MM_WU = 1 / 3048.0


def shader_base(name):
    return (name or "").replace("/", "\\").split("\\")[-1].split(".")[0].lower()


def wrap_uv(o):
    me = o.data
    lay = me.uv_layers["UVMap"]
    uv = np.empty(len(me.loops) * 2, np.float32)
    lay.data.foreach_get("uv", uv)
    uv = uv.reshape(-1, 2)
    for p in me.polygons:
        li = list(p.loop_indices)
        uv[li] -= np.floor(uv[li].mean(0))
    lay.data.foreach_set("uv", uv.ravel())


def copy_faces(obj, shaders, suffix):
    o = obj.copy()
    o.data = obj.data.copy()
    o.name = obj.name + suffix
    bpy.context.scene.collection.objects.link(o)
    for m in list(o.modifiers):
        o.modifiers.remove(m)
    keep = {i for i, s in enumerate(o.material_slots) if s.material and shader_base(s.material.name) in shaders}
    bm = bmesh.new()
    bm.from_mesh(o.data)
    bmesh.ops.delete(bm, geom=[f for f in bm.faces if f.material_index not in keep], context="FACES")
    bm.to_mesh(o.data)
    bm.free()
    o.hide_render = False
    o.hide_set(False)
    o.parent = None
    o.matrix_world = obj.matrix_world.copy()
    return o


def mesh_curvature(o):
    """per vertex: the game mesh's own mean curvature in 1/mm (positive convex), from its vertex normals and
    edges: the mean over a vertex's edges of 2 n.(p - q) / |p - q|^2 (the normal curvature along each edge),
    stored as pb_k = 0.5 + 0.5 tanh(k * 5 mm) for the bake"""
    me = o.data
    n = len(me.vertices)
    co = np.empty(n * 3, np.float32)
    me.vertices.foreach_get("co", co)
    co = co.reshape(n, 3) / MM_WU
    nr = np.empty(n * 3, np.float32)
    me.vertices.foreach_get("normal", nr)
    nr = nr.reshape(n, 3)
    ed = np.empty(len(me.edges) * 2, np.int64)
    me.edges.foreach_get("vertices", ed)
    ed = ed.reshape(-1, 2)
    d = co[ed[:, 0]] - co[ed[:, 1]]
    l2 = np.maximum((d * d).sum(1), 1e-6)
    k0 = 2 * (nr[ed[:, 0]] * d).sum(1) / l2
    k1 = -2 * (nr[ed[:, 1]] * d).sum(1) / l2
    ks = np.zeros(n)
    kc = np.zeros(n)
    np.add.at(ks, ed[:, 0], k0)
    np.add.at(ks, ed[:, 1], k1)
    np.add.at(kc, ed[:, 0], 1)
    np.add.at(kc, ed[:, 1], 1)
    k = ks / np.maximum(kc, 1)
    attr = me.attributes.new("pb_k", "FLOAT", "POINT")
    attr.data.foreach_set("value", (0.5 + 0.5 * np.tanh(k * 5.0)).astype(np.float32))
    return float(np.percentile(np.abs(k), 50)), float(np.percentile(np.abs(k), 99))


def islands_and_density(o, size, shader_order):
    """per face: its UV island's number, its millimetres per texel and its shader's number (+1)"""
    bm = bmesh.new()
    bm.from_mesh(o.data)
    bm.faces.ensure_lookup_table()
    uv = bm.loops.layers.uv["UVMap"]
    parent = list(range(len(bm.faces)))

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    for e in bm.edges:
        lf = e.link_loops
        if len(lf) != 2:
            continue
        a, b = lf
        # the loops of the two faces at the edge's two vertices
        a0, a1 = a[uv].uv, a.link_loop_next[uv].uv
        b0, b1 = b.link_loop_next[uv].uv, b[uv].uv
        if (a0 - b0).length < 1e-5 and (a1 - b1).length < 1e-5:
            ra, rb = find(a.face.index), find(b.face.index)
            if ra != rb:
                parent[ra] = rb
    roots = {}
    island = np.zeros(len(bm.faces), np.float32)
    shader_of = np.zeros(len(bm.faces), np.float32)
    slot_shader = [shader_order.index(shader_base(s.material.name)) + 1 if s.material and
                   shader_base(s.material.name) in shader_order else 0 for s in o.material_slots]
    dens = np.zeros(len(bm.faces), np.float32)
    mw = o.matrix_world
    for f in bm.faces:
        island[f.index] = roots.setdefault(find(f.index), len(roots) + 1)
        shader_of[f.index] = slot_shader[f.material_index] if f.material_index < len(slot_shader) else 0
        pts = [mw @ v.co for v in f.verts]
        a3 = 0.0
        for i in range(1, len(pts) - 1):
            a3 += ((pts[i] - pts[0]).cross(pts[i + 1] - pts[0])).length / 2
        uvs = [l[uv].uv for l in f.loops]
        a2 = 0.0
        for i in range(1, len(uvs) - 1):
            d1, d2 = uvs[i] - uvs[0], uvs[i + 1] - uvs[0]
            a2 += abs(d1.x * d2.y - d1.y * d2.x) / 2
        dens[f.index] = np.sqrt((a3 / MM_WU / MM_WU) / max(a2 * size * size, 1e-9)) if a2 > 0 else 0
    bm.free()
    attr = o.data.attributes.new("pb_id", "FLOAT_VECTOR", "FACE")
    vals = np.zeros((len(island), 3), np.float32)
    vals[:, 0] = island
    vals[:, 1] = dens
    vals[:, 2] = shader_of
    attr.data.foreach_set("vector", vals.ravel())
    return len(roots), float(np.median(dens[dens > 0])) if (dens > 0).any() else 0.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("blend")
    ap.add_argument("out")
    ap.add_argument("--game", required=True)
    ap.add_argument("--high", required=True)
    ap.add_argument("--shaders", required=True)
    ap.add_argument("--size", type=int, default=4096)
    ap.add_argument("--gpu", default=None, help="the graphics card, by part of its name (PRISM_PACKS_GPU's)")
    ap.add_argument("--cage-mm", type=float, default=6.0)
    ap.add_argument("--ray-mm", type=float, default=12.0)
    ap.add_argument("--ao-mm", type=float, default=30.0)
    ap.add_argument("--samples", type=int, default=128)
    ap.add_argument("--keep-high-materials", action="store_true")
    ap.add_argument("--occluders", default="")
    ap.add_argument("--wrap-uv", action="store_true")
    a = ap.parse_args(U.script_args())
    shaders = {s.strip().lower() for s in a.shaders.split(",") if s.strip()}
    os.makedirs(a.out, exist_ok=True)
    bpy.ops.wm.open_mainfile(filepath=a.blend)
    sc = bpy.context.scene
    rep = {"blend": a.blend, "game": a.game, "high": a.high, "shaders": sorted(shaders), "size": a.size,
           "cage_mm": a.cage_mm, "ray_mm": a.ray_mm, "ao_mm": a.ao_mm, "samples": a.samples, "seconds": {}}
    rep["device"] = U.set_cycles_gpu(sc, a.gpu, a.samples)
    game = copy_faces(bpy.data.objects[a.game], shaders, "_pb")
    high = copy_faces(bpy.data.objects[a.high], shaders, "_pb")
    occ = [bpy.data.objects[n] for n in a.occluders.split(",") if n]
    if a.wrap_uv:
        wrap_uv(game)
    for o in bpy.data.objects:
        if o not in [game, high] + occ and o.type == "MESH":
            o.hide_render = True
    for o in occ:
        o.hide_render = False
        o.hide_set(False)
    rep["occluders"] = [o.name for o in occ]
    rep["game_faces"], rep["high_faces"] = len(game.data.polygons), len(high.data.polygons)
    order = sorted(shaders)
    rep["shader_order"] = order
    rep["islands"], rep["density_mm_per_texel_median"] = islands_and_density(game, a.size, order)
    rep["mesh_curvature_per_mm_p50_p99"] = mesh_curvature(game)
    game.data.uv_layers.active = game.data.uv_layers["UVMap"]
    if sc.world is None:
        sc.world = bpy.data.worlds.new("w")
    sc.world.light_settings.distance = a.ao_mm * MM_WU

    mat = bpy.data.materials.new("pb_bake")
    mat.use_nodes = True
    nt = mat.node_tree
    nt.nodes.clear()
    at = nt.nodes.new("ShaderNodeAttribute")
    at.attribute_name = "pb_id"
    em = nt.nodes.new("ShaderNodeEmission")
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    nt.links.new(at.outputs["Vector"], em.inputs["Color"])
    nt.links.new(em.outputs["Emission"], out.inputs["Surface"])
    tex = nt.nodes.new("ShaderNodeTexImage")
    nt.nodes.active = tex
    game.data.materials.clear()
    game.data.materials.append(mat)
    for p in game.data.polygons:
        p.material_index = 0
    if not a.keep_high_materials:
        high.data.materials.clear()
        high.data.materials.append(bpy.data.materials.new("pb_high"))
    b = sc.render.bake
    b.margin = 0
    b.cage_extrusion = a.cage_mm * MM_WU
    b.max_ray_distance = a.ray_mm * MM_WU

    def image(name, fill):
        img = bpy.data.images.new(name, a.size, a.size, alpha=True, float_buffer=True)
        img.colorspace_settings.name = "Non-Color"
        img.generated_color = fill
        tex.image = img
        return img

    def pixels(img):
        px = np.empty(a.size * a.size * 4, np.float32)
        img.pixels.foreach_get(px)
        return px.reshape(a.size, a.size, 4)[::-1]

    def run(kind, img, selected, **kw):
        bpy.ops.object.select_all(action="DESELECT")
        if selected:
            high.select_set(True)
        game.select_set(True)
        bpy.context.view_layer.objects.active = game
        t = time.time()
        bpy.ops.object.bake(type=kind, use_selected_to_active=selected, margin=0, uv_layer="UVMap", **kw)
        rep["seconds"][img.name] = round(time.time() - t, 1)
        print("BAKED", img.name, rep["seconds"][img.name], flush=True)
        return pixels(img)

    def save(name, arr):
        np.save(os.path.join(a.out, "raw_%s.npy" % name), arr.astype(np.float16))

    # coverage, islands and density (the game mesh's own faces, no rays)
    high.hide_render = True
    for o in occ:
        o.hide_render = True
    sc.cycles.samples = 1
    idp = run("EMIT", image("ids", (0, 0, 0, 0)), False)
    cover = idp[..., 2] > 0.5
    save("cover", cover.astype(np.float32))
    save("shader", np.where(cover, np.round(idp[..., 2]), 0))
    save("island", np.where(cover, np.round(idp[..., 0]), 0))
    np.save(os.path.join(a.out, "raw_island32.npy"), np.where(cover, np.round(idp[..., 0]), 0).astype(np.int32))
    save("density", np.where(cover, idp[..., 1], 0))
    # the game mesh's own curvature (pb_k), interpolated across its faces
    at.attribute_name = "pb_k"
    nt.links.new(at.outputs["Fac"], em.inputs["Color"])
    kp = run("EMIT", image("curv_mesh", (0.5, 0.5, 0.5, 1)), False)
    save("curv_mesh", np.where(cover, kp[..., 0], 0.5))
    # the game mesh's own occlusion (fallback)
    sc.cycles.samples = a.samples
    aog = run("AO", image("ao_game", (1, 1, 1, 1)), False)
    save("ao_game", aog[..., 0])
    # the high-poly onto the game mesh; the game mesh hidden from the rays (it is still the bake's surface)
    high.hide_render = False
    for o in occ:
        o.hide_render = False
    for attr in ("visible_camera", "visible_diffuse", "visible_glossy", "visible_transmission",
                 "visible_volume_scatter", "visible_shadow"):
        setattr(game, attr, False)
    nrm = run("NORMAL", image("normal", (0, 0, 0, 0)), True, normal_space="TANGENT",
              normal_r="POS_X", normal_g="POS_Y", normal_b="POS_Z")
    save("normal", nrm[..., :3])
    ao = run("AO", image("ao_high", (1, 0, 1, 1)), True)
    missed = (ao[..., 0] - ao[..., 1]) > 0.5
    save("ao", np.where(missed, -1.0, ao[..., 0]))
    hit = (nrm[..., :3].sum(-1) > 0.01)
    rep["coverage_share"] = float(cover.mean())
    rep["normal_missed_share_of_cover"] = float((cover & ~hit).sum() / max(cover.sum(), 1))
    rep["normal_backfacing_share_of_cover"] = float((cover & hit & (nrm[..., 2] < 0.55)).sum() / max(cover.sum(), 1))
    rep["ao_missed_share_of_cover"] = float((cover & missed).sum() / max(cover.sum(), 1))
    rep["ao_mean_hit"] = float(ao[..., 0][cover & ~missed].mean()) if (cover & ~missed).any() else None
    rep["ao_game_mean"] = float(aog[..., 0][cover].mean())
    json.dump(rep, open(os.path.join(a.out, "bake.json"), "w"), indent=1)
    print("BAKE_OK", json.dumps(rep))


main()
