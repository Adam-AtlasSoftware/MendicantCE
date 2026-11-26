#!/usr/bin/env python3
"""The tuple scanner (PLAN_FINAL.md section 2.4 step 2): proves a sidecar ships no original data.

    scan_original.py <sidecar.prsx> --map <retail map> [--map ...] [--json]
    scan_original.py --negative-controls --map <retail map> [--work <folder>]

It holds sets of every retail tuple of every map version given (render positions from the compressed vertices on
the 1/1024 grid, collision vertices, planes, packed normals, coordinate pairs) and of every 48-byte window of each
BSP's bytes and of its vertices' exact decode, and reads every decoded section:
- a LIT position, collision vertex or plane equal to a retail one is a finding (it must be a REF);
- a 48-byte window of a section's bytes, or of its values laid out as the engine lays them (geom.runtime_bytes),
  equal to a retail window is a finding;
- per section it reports the REF share (a high share flags "the remodel kept the stock mesh" for review) and the
  share of normals and coordinate pairs that coincide with retail ones (reported only: quantization makes them
  coincide by nature);
- per section it reports the near share, the share of LIT positions within NEAR_STEPS grid steps (on every axis)
  of a retail one, and the transform share, the largest share of LIT positions that one affine map of their
  nearest retail positions (within 1/4 world unit) puts within TRANSFORM_TOLERANCE grid steps: a stock mesh moved,
  nudged or scaled a hair is a copy exact equality misses, and either share past its threshold flags the section
  for review.

--negative-controls builds doctored sidecars from the map and requires every one to fail (a finding, or a review
flag), and a clean remodel to pass with neither: a quantized stock vertex run, a compressed-to-uncompressed copy, a
single stolen plane, a stolen run inserted before encoding, raw retail bytes in a runtime section, another BSP's
whole mesh pasted in through the real encoder (which nudges it), and the stock mesh moved 1/1024 and 0.01 world
unit and scaled by 1.0001. Exit 0 when the sidecar is clean (or every control
behaved), 1 otherwise.
"""
import argparse
import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from geopatch import encode, geom, lint_patch, prsx, retail  # noqa: E402
from geopatch.geom import LIT  # noqa: E402

HIGH_REF_SHARE = 0.9
NEAR_STEPS = 8
HIGH_NEAR_SHARE = 0.2
TRANSFORM_REACH = 256
TRANSFORM_TOLERANCE = 2
HIGH_TRANSFORM_SHARE = 0.2
TRANSFORM_MINIMUM = 12
TRANSFORM_TRIALS = 64


def transform_share(ours, theirs):
    """the largest share of pairs (ours[i] near theirs[i], grid positions) one affine map theirs -> ours fits within
    TRANSFORM_TOLERANCE grid steps (a seeded RANSAC over six-pair fits, refitted on its inliers)"""
    n = len(ours)
    if n < TRANSFORM_MINIMUM:
        return 0.0
    a = np.hstack([theirs.astype(np.float64), np.ones((n, 1))])
    b = ours.astype(np.float64)
    rng = np.random.default_rng(1)
    best = np.zeros(n, bool)
    for trial in range(TRANSFORM_TRIALS):
        pick = rng.choice(n, size=min(6, n), replace=False) if trial else np.arange(n)
        for _ in range(2):
            m = np.linalg.lstsq(a[pick], b[pick], rcond=None)[0]
            inliers = np.abs(a @ m - b).max(axis=1) <= TRANSFORM_TOLERANCE
            if inliers.sum() < TRANSFORM_MINIMUM:
                break
            pick = np.nonzero(inliers)[0]
        if inliers.sum() > best.sum():
            best = inliers
    return float(best.sum()) / n if best.sum() >= TRANSFORM_MINIMUM else 0.0


def build_sets(map_paths, native=None):
    sets = retail.RetailSets()
    for path in map_paths:
        _, _, bsps = retail.read_map(path)
        for bsp in bsps:
            sets.add_bsp(bsp)
            if native is not None:
                for material in bsp.materials.values():
                    sets.add_decoded(native.environment_vertices(material.vertices))
    return sets


def scan(data, sets):
    """{"findings": [...], "sections": [{kind, bsp, collv, ref_share, coincident: {...}, review}]}"""
    findings, sections = [], []
    sidecar = prsx.Sidecar(data)
    for index, entry in enumerate(sidecar.entries):
        where = "%s bsp %d collv %d" % (entry.kind, entry.bsp, entry.collv)
        section = sidecar.section(index)
        stats = {"kind": entry.kind, "bsp": entry.bsp, "collv": entry.collv, "bytes": len(section)}
        try:
            tuples = geom.section_tuples(entry.kind, section)
            runtime = geom.runtime_bytes(entry.kind, section)
        except geom.LayoutError as error:
            findings.append("%s: %s" % (where, error))
            tuples, runtime = [], b""
        totals, hits, refs = {}, {}, 0
        lit = {}
        for kind, value, is_ref, at in tuples:
            if kind in ("position", "collision_vertex"):
                totals["vertices"] = totals.get("vertices", 0) + 1
                refs += is_ref
                if not is_ref:
                    lit.setdefault(at[0], []).append(value)
            if is_ref:
                continue
            totals[kind] = totals.get(kind, 0) + 1
            if value in sets.of(kind):
                hits[kind] = hits.get(kind, 0) + 1
                if kind in geom.HARD_KINDS:
                    findings.append("%s: %s %s at %s equals a retail one" % (where, kind, value, at))
        stats["ref_share"] = refs / totals["vertices"] if totals.get("vertices") else 0.0
        stats["coincident"] = {k: hits.get(k, 0) / v for k, v in totals.items() if k != "vertices" and v}
        stats["near_share"], stats["transform_share"] = _near_shares(lit, sets)
        stats["review"] = stats["ref_share"] > HIGH_REF_SHARE or stats["near_share"] > HIGH_NEAR_SHARE or \
            stats["transform_share"] > HIGH_TRANSFORM_SHARE
        for name, blob in (("bytes", section), ("runtime", runtime)):
            hits = sets.window_hits(blob)
            if hits:
                findings.append("%s: its %s at %d hold a 48-byte run of retail data (%d windows)" %
                                (where, name, hits[0], len(hits)))
        sections.append(stats)
    return {"findings": findings, "sections": sections, "clean": not findings}


def _near_shares(lit, sets):
    """(near share, transform share) of a section's LIT positions, {part: [grid positions]}: the transform share
    is the largest of its parts' (weighted by the section's LIT count)"""
    count = sum(len(v) for v in lit.values())
    if not count:
        return 0.0, 0.0
    near = 0
    fitted = 0.0
    for points in lit.values():
        points = np.array(points, np.int64).reshape(-1, 3)
        found, nearest, distance = sets.nearest(points, TRANSFORM_REACH)
        near += int((found & (distance <= NEAR_STEPS)).sum())
        fitted = max(fitted, transform_share(points[found], nearest[found]) * found.sum())
    return near / count, float(fitted) / count


# ---------- controls

def remodel_part(material, native, lift=0.25, extra=None):
    """a made-up remodel of a stock material, for the controls: each stock triangle split at its centroid, lifted;
    the stock corners exactly stock (REF), the centroids ours (LIT). extra: [(x, y, z)] floats inserted as further
    vertices of the first surface's run (the stolen-run control) through the real encoder's matching"""
    decoded = np.frombuffer(native.environment_vertices(material.vertices), "<f4").reshape(-1, 14)
    count = material.vertex_count
    vertices = [row.copy() for row in decoded]
    triangles, sft = [], [0]
    for a, b, c in material.surfaces:
        centroid = (decoded[a] + decoded[b] + decoded[c]) / 3.0
        centroid[2] += lift
        vertices.append(centroid.astype(np.float32))
        m = len(vertices) - 1
        triangles += [(a, b, m), (b, c, m), (c, a, m)]
        sft.append(len(triangles))
    if extra:
        start = len(vertices)
        for x, y, z in extra:
            row = decoded[0].copy()
            row[0:3] = (x, y, z)
            vertices.append(row)
        for k in range(start, len(vertices) - 2, 3):
            triangles.insert(0, (k, k + 1, k + 2))
            sft = [0] + [s + 1 for s in sft[1:]]
    p = encode.PwgPart()
    p.lightmap_index, p.material_index = material.lightmap_index, material.material_index
    p.stock_surface_count, p.stock_vertex_count = material.surface_count, count
    p.topology_check = material.topology_check()
    p.first_surface, p.surface_count = 0, material.surface_count
    p.near_triangle_count, p.far_distance = len(triangles), 0.0
    p.vertices = np.stack(vertices).astype(np.float32)
    p.lightmap = np.zeros((len(vertices), 5), np.float32)
    p.triangles = np.array(triangles, np.int64)
    p.sft = np.array(sft, np.uint32)
    p.far_pairs = p.spheres = None
    return p


def _sidecar(sections):
    return prsx.write(b"\x11" * 32, sections)


def caught(report):
    """a control's verdict: a finding of the scanner or the lint, or a section flagged for review"""
    return bool(report["findings"] or report["lint"] or any(s["review"] for s in report["sections"]))


def controls(map_path, native):
    """[(name, expected to be caught, report)] of the controls built from map_path"""
    _, _, bsps = retail.read_map(map_path)
    sets = build_sets([map_path], native)
    bsp = bsps[0]
    material = next(iter(bsp.materials.values()))
    other = bsps[-1].materials[sorted(bsps[-1].materials)[-1]] if len(bsps) > 1 else \
        bsp.materials[sorted(bsp.materials)[-1]]
    out = []

    def geom_section(parts):
        return prsx.Section("GEOM", bsp.index, encode.encode_geom_arrays(parts))

    def run(name, sections, should_fail):
        data = _sidecar(sections)
        report = scan(data, sets)
        report["lint"] = lint_patch.lint(data, {b.index: b for b in bsps}, sets)
        out.append((name, should_fail, report))

    clean, _ = encode.encode_pwg_part(remodel_part(material, native), material, native)
    run("clean remodel", [geom_section([clean])], False)

    # 1. a quantized stock vertex run, written as our own vertices
    stolen, _ = encode.encode_pwg_part(remodel_part(material, native), None)
    run("quantized stock vertex run", [geom_section([stolen])], True)

    # 2. a compressed-to-uncompressed copy: the stock vertices decoded exactly, kept as LIT
    copy = remodel_part(material, native)
    copy.vertices = copy.vertices[:material.vertex_count]
    copy.triangles = np.array(material.surfaces, np.int64)
    copy.sft = np.arange(material.surface_count + 1, dtype=np.uint32)
    copy.near_triangle_count = len(material.surfaces)
    copied, _ = encode.encode_pwg_part(copy, None)
    run("compressed-to-uncompressed copy", [geom_section([copied])], True)

    # 3. a single retail plane among our own
    plane = bsp.planes[0]
    ours = [(0.0, 0.0, 1.0, 1.5), (0.6, 0.0, 0.8, -2.25)]
    planes = [tuple(geom.float_bits(x) for x in p) for p in ours + [plane]]
    coll = geom.encode_coll([(LIT, (1000, 2000, 3000))], planes)
    run("single plane", [geom_section([clean]), prsx.Section("COLL", bsp.index, coll, collv=1)], True)

    # 4. a stolen run (another material's vertices) inserted before encoding: the encoder cannot REF it
    run_points = [other.position(k) for k in range(min(9, other.vertex_count))]
    inserted, _ = encode.encode_pwg_part(remodel_part(material, native, extra=run_points), material, native)
    run("stolen run inserted before encoding", [geom_section([inserted])], True)

    # 5. raw retail bytes in a runtime section
    at = bsp.raw.find(material.vertices[:96])
    raw = bsp.raw[at:at + 96]
    run("raw retail bytes", [geom_section([clean]), prsx.Section("RHGT", bsp.index, b"RHG0" + raw)], True)

    # 6. another BSP's whole stock mesh pasted into the remodel, through the real encoder: every retail position
    # of the map is avoided, so it nudges each pasted vertex a grid step
    avoid = set()
    for b in bsps:
        avoid |= set(encode._bsp_lookup(b))
    source = bsps[-1] if len(bsps) > 1 else bsp
    foreign = [m for key, m in sorted(source.materials.items()) if source is not bsp or m is not material]
    pasted = [m.position(k) for m in foreign for k in range(m.vertex_count)]
    pasted = pasted[:len(pasted) - len(pasted) % 3]
    nudged, _ = encode.encode_pwg_part(remodel_part(material, native, extra=pasted), material, native, bsp, avoid)
    run("another BSP's mesh pasted through the encoder", [geom_section([nudged])], True)

    # 7-10. the stock mesh kept, moved or scaled a hair: no position exactly retail. The real encoder snaps a
    # vertex within encode.SNAP_STEPS of a stock position to a REF, so the 1/1024 move is written as an encoder
    # without that would write it (all LIT); the others are past the snap and go through the real encoder
    def moved(name, change, resolve=True):
        part = remodel_part(material, native)
        part.vertices = part.vertices.copy()
        change(part.vertices[:material.vertex_count])
        if resolve:
            encoded, _ = encode.encode_pwg_part(part, material, native, bsp, avoid)
        else:
            encoded, _ = encode.encode_pwg_part(part, None)
        run(name, [geom_section([encoded])], True)

    def shift(axis, amount):
        def change(v):
            v[:, axis] += np.float32(amount)
        return change

    def scale(v):
        # (about a point 1000 world units off, as a real level's coordinates are: a tenth of a unit's move,
        # past the near share's reach, which only the transform share sees)
        centre = np.array([-1000.0, -1000.0, 0.0])
        v[:, 0:3] = ((v[:, 0:3] - centre) * 1.0001 + centre).astype(np.float32)

    moved("stock mesh moved 1/1024 world unit", shift(0, 1.0 / 1024), resolve=False)
    moved("stock mesh moved 0.01 world unit", shift(2, 0.01))
    moved("stock mesh moved 0.1 world unit", shift(1, 0.1))
    moved("stock mesh scaled 1.0001", scale)
    return out


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("sidecar", nargs="?")
    parser.add_argument("--map", action="append", default=[], required=True)
    parser.add_argument("--negative-controls", action="store_true")
    parser.add_argument("--native-dir")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    from geopatch import native as native_module
    native = native_module.Native(args.native_dir)
    if args.negative_controls:
        ok = True
        results = []
        for name, should_fail, report in controls(args.map[0], native):
            failed = caught(report)
            behaved = failed == should_fail
            ok &= behaved
            results.append({"control": name, "should_fail": should_fail, "failed": failed, "behaved": behaved,
                            "findings": (report["findings"] + report["lint"])[:3]})
        print(json.dumps(results, indent=1))
        return 0 if ok else 1
    if not args.sidecar:
        parser.error("a sidecar, or --negative-controls")
    report = scan(Path(args.sidecar).read_bytes(), build_sets(args.map, native))
    if args.json:
        print(json.dumps(report, indent=1))
    else:
        for finding in report["findings"]:
            print(finding)
        for s in report["sections"]:
            print("%s bsp %d: REF share %.3f, near share %.3f, transform share %.3f%s, coincident %s" %
                  (s["kind"], s["bsp"], s["ref_share"], s["near_share"], s["transform_share"],
                   " (review)" if s["review"] else "", json.dumps(s["coincident"])))
        print("clean" if report["clean"] else "%d findings" % len(report["findings"]))
    return 0 if report["clean"] else 1


if __name__ == "__main__":
    sys.exit(main())
