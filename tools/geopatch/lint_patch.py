#!/usr/bin/env python3
"""The sidecar lint (PLAN_FINAL.md section 2.4 step 1): the format allows only our sections, in our layouts, and a
literal where a REF is required is refused.

    lint_patch.py <sidecar.prsx> [--map <retail map> ...] [--native] [--json]

Without maps it checks the container and every section's layout. With the retail maps of the versions the sidecar
serves, it also checks every GEOM part against its stock material (counts, topology check, REF indices in range)
and refuses any LIT position or collision vertex equal to a retail one on the grid, any LIT plane equal to a
retail plane (scan_original.py's sets), base map coordinates of a REFP/REFM vertex equal to the referenced stock
vertex's own (they must be taken from it: REFPT/REFMT), and nudged runs: a LIT vertex one to NUDGE_STEPS grid
steps above a retail position is how the encoder moves a vertex off another BSP's position, so a run of
NUDGE_RUN consecutive ones in a part, or more than NUDGE_BUDGET in a section, is a copy the nudge hid. --native has the game's own reader (prism_prsx.c, through native.py) open
the file and verify every section too. Exit 0 clean, 1 with findings.
"""
import argparse
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from geopatch import geom, prsx, retail  # noqa: E402

LAYOUTS = {"GEOM": geom.decode_geom, "COLL": geom.decode_coll, "LMUV": geom.decode_lmuv}
# types whose layout is not final yet (none: LMUV v1 is the game's until M4's own lightmap layout, a new version)
PROVISIONAL = {}
# the encoder's nudge (encode.encode_pwg_part) moves a vertex up one grid step at a time; b30's whole remodel
# nudged 5 vertices, none next to another
NUDGE_STEPS = 4
NUDGE_RUN = 3
NUDGE_BUDGET = 16


def lint(data, retail_bsps=None, sets=None, native=None, path=None):
    """[finding text] for a sidecar's bytes; retail_bsps: {bsp index: RetailBsp} of the map it serves"""
    findings = []
    try:
        sidecar = prsx.Sidecar(data)
    except prsx.PrsxError as error:
        return ["container: %s" % error]
    if native is not None and path is not None:
        verdict = native.open(path)
        if verdict.get("result") != "ok":
            findings.append("game reader: %s" % verdict.get("result"))
        for entry in verdict["entries"]:
            if entry["read"] != "ok":
                findings.append("game reader: section %d %s" % (entry["index"], entry["read"]))
    for index, entry in enumerate(sidecar.entries):
        where = "%s bsp %d collv %d" % (entry.kind, entry.bsp, entry.collv)
        if entry.kind not in prsx.TYPES:
            findings.append("%s: not one of our section types" % where)
            continue
        expected = prsx.default_flags(entry.kind)
        if (entry.flags & ~(prsx.CODEC_MASK | prsx.SET_MASK)) != expected:
            findings.append("%s: flags %04x, expected %04x" % (where, entry.flags, expected))
        if entry.codec not in (prsx.STORED, prsx.ZLIB):
            findings.append("%s: codec %d is not one the game reads" % (where, entry.codec))
            continue
        try:
            section = sidecar.section(index)
        except prsx.PrsxError as error:
            findings.append("%s: %s" % (where, error))
            continue
        if entry.kind in PROVISIONAL:
            if not section.startswith(PROVISIONAL[entry.kind]):
                findings.append("%s: not in its layout" % where)
            continue
        decode = LAYOUTS.get(entry.kind)
        if decode is None:
            findings.append("%s: no layout is defined for this type yet" % where)
            continue
        try:
            decoded = decode(section)
        except geom.LayoutError as error:
            findings.append("%s: %s" % (where, error))
            continue
        if entry.kind == "GEOM" and retail_bsps is not None:
            findings += _check_parts(where, decoded, retail_bsps.get(entry.bsp))
        if entry.kind == "COLL" and retail_bsps is not None:
            stock = retail_bsps.get(entry.bsp)
            for v, vertex in enumerate(decoded[0]):
                if vertex[0] == geom.REF and (stock is None or vertex[1] >= len(stock.collision_vertices)):
                    findings.append("%s: vertex %d refers past the stock collision vertices" % (where, v))
            for k, plane in enumerate(decoded[1]):
                if len(plane) == 2 and (stock is None or plane[1] >= len(stock.planes)):
                    findings.append("%s: plane %d refers past the stock planes" % (where, k))
        if entry.kind == "GEOM" and sets is not None:
            findings += _check_nudges(where, decoded, sets)
        if sets is not None:
            for kind, value, is_ref, at in geom.section_tuples(entry.kind, section):
                if not is_ref and kind in geom.HARD_KINDS and value in sets.of(kind):
                    findings.append("%s: %s %s at %s is a retail value and must be a REF" % (where, kind, value, at))
    return findings


def _stock_tex(material, index):
    return struct.unpack_from("<2I", material.vertices, 32 * index + 24)


def _check_parts(where, parts, stock):
    findings = []
    if stock is None:
        return ["%s: the map has no such BSP" % where]
    for p, part in enumerate(parts):
        if part.material_namespace == geom.NAMESPACE_PRISM:
            # (new content with a PRISM material: no stock material to check against; decode_geom has refused
            # stock fields and REF/REFP vertices in it)
            material = None
            label = "%s part %d (PRISM material %d)" % (where, p, part.prism_material)
        else:
            material = stock.materials.get((part.lightmap_index, part.material_index))
            label = "%s part %d (lightmap %d material %d)" % (where, p, part.lightmap_index, part.material_index)
            if material is None:
                findings.append("%s: no such stock material" % label)
                continue
            if part.stock_surface_count != material.surface_count or \
                    part.stock_vertex_count != material.vertex_count or \
                    part.topology_check != material.topology_check():
                findings.append("%s: made for another revision of the material" % label)
            if part.first_surface + part.surface_count > material.surface_count:
                findings.append("%s: surfaces past the material's" % label)
        copied = 0
        for v, vertex in enumerate(part.vertices):
            op = vertex[0]
            base = geom.base_op(op)
            referenced = None
            if base in (geom.REF, geom.REFP):
                bad = vertex[1] >= material.vertex_count
                referenced = (material, vertex[1])
            elif base == geom.REFM:
                other = stock.materials.get(vertex[1][0:2])
                bad = other is None or vertex[1][2] >= other.vertex_count
                referenced = (other, vertex[1][2])
            elif op == geom.REFC:
                bad = vertex[1] >= len(stock.collision_vertices)
            else:
                continue
            if bad:
                findings.append("%s: vertex %d refers past the stock vertices" % (label, v))
                break
            if op in (geom.REFP, geom.REFM) and vertex[3] == _stock_tex(*referenced):
                copied += 1
        if copied:
            findings.append("%s: %d REFP/REFM vertices write out the referenced stock vertex's base map coordinates "
                            "(they must take them from it: REFPT/REFMT)" % (label, copied))
    return findings


def _check_nudges(where, parts, sets):
    """the nudge rule (module help): LIT vertices just above a retail position, in runs or in bulk"""
    findings, total = [], 0
    for p, part in enumerate(parts):
        run = longest = 0
        for vertex in part.vertices:
            if vertex[0] == geom.LIT:
                x, y, z = vertex[1]
                nudged = any((x, y, z - step) in sets.position for step in range(1, NUDGE_STEPS + 1))
            else:
                nudged = False
            run = run + 1 if nudged else 0
            total += nudged
            longest = max(longest, run)
        if longest >= NUDGE_RUN:
            findings.append("%s part %d: a run of %d LIT vertices just above retail positions (a nudged copy)" %
                            (where, p, longest))
    if total > NUDGE_BUDGET:
        findings.append("%s: %d LIT vertices just above retail positions, more than %d" % (where, total, NUDGE_BUDGET))
    return findings


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("sidecar")
    parser.add_argument("--map", action="append", default=[])
    parser.add_argument("--native", action="store_true")
    parser.add_argument("--native-dir")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    data = Path(args.sidecar).read_bytes()
    retail_bsps = sets = None
    if args.map:
        sets = retail.RetailSets()
        for path in args.map:
            _, _, bsps = retail.read_map(path)
            for bsp in bsps:
                sets.add_bsp(bsp, windows=False)
        # (the REF checks are against the first map: the one the sidecar is built from)
        retail_bsps = {b.index: b for b in retail.read_map(args.map[0])[2]}
    native = None
    if args.native:
        from geopatch import native as native_module
        native = native_module.Native(args.native_dir)
    findings = lint(data, retail_bsps, sets, native, args.sidecar)
    if args.json:
        print(json.dumps({"findings": findings, "clean": not findings}, indent=1))
    else:
        for finding in findings:
            print(finding)
        print("clean" if not findings else "%d findings" % len(findings))
    return 0 if not findings else 1


if __name__ == "__main__":
    sys.exit(main())
