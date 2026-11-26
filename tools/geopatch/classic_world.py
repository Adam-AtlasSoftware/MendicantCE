#!/usr/bin/env python3
"""A map version's classic_world_id (PLAN_FINAL.md section 2.5): the farm's hash of the inputs its bakes depend on,
computed over the map's own tags, so that two map versions whose bake inputs are equal share it (and so share their
packs and sidecars), and a version whose level, lighting, shaders, bitmaps or static scenery differ does not.

    classic_world.py <map>... [--json out.json]

It hashes, under SHA-256 with the label "PRISM-CWID" and CWID_VERSION:
- each structure BSP as the map holds it (its bytes from the scenario's BSP reference: the render geometry, the
  lightmap UVs and pages, the materials, the clusters, the collision BSP the reach walks), in scenario order;
- the tag data of every tag of a bake-input group (INPUT_GROUPS: the scenario, the shaders, the bitmaps, the
  static scenery, devices and light fixtures, their models, skies and lights), each tag's bytes from its address to
  the next tag's in the tag data, in the order of (group, path), with its group and path;
- each of those bitmaps' pixel data (its bitmap entries' offset and size in the map), in the same order.
Bitmaps under the menus' and the HUD's folders (ui\\, interface\\) are left out: no bake reads them, and they may
differ between languages.

What it is not: a canonical hash of the tags' meaning. Tag data holds absolute addresses, so two versions share an
id only where those tags' bytes are equal; a version whose tag layout shifts gets an id of its own (and is baked
on its own), never a wrong one. Whether PAL 01.01.14.2342 shares NTSC 01.10.12.2276's ids is the one farm run of
decision A7.

The game never computes it: the signed manifest (M7; until then the shipped bake's manifest.txt "world" lines,
tools/prism_bake_ship.py) maps each supported base_id to it, and the sidecar's header carries it. The farm runs
this once per map version, before encoding sidecars (encode.py --classic-world-id) and before baking (the bake's
HALO_CLASSIC_WORLD_ID), and records it in versions.json beside the version's base_id. Retail maps never leave the
farm; only hashes come out.
"""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "prism_bake"))
import ce_map  # noqa: E402

from geopatch import identity  # noqa: E402

CWID_VERSION = 1
LABEL = b"PRISM-CWID"


def _group(text):
    return struct.unpack(">I", text.encode())[0]


INPUT_GROUPS = tuple(sorted(_group(g) for g in (
    "scnr",                                                                  # the scenario: placements, lights, sky
    "shdr", "senv", "soso", "sotr", "schi", "scex", "swat", "sgla", "smet", "spla",   # the shaders
    "bitm",                                                                  # their maps (and lightmap pages)
    "scen", "mach", "ctrl", "lifi", "ssce", "plac",                          # static scenery and devices
    "mode", "mod2",                                                          # their models
    "sky ", "ligh",                                                          # skies, lights
)))
BITMAPS_LEFT_OUT = ("ui\\", "interface\\")
BITMAP_GROUP_BITMAPS = 0x60
BITMAP_DATA_SIZE = 0x30
BITMAP_PIXELS_OFFSET = 0x18


def group_text(group):
    return struct.pack(">I", group).decode("ascii", "replace")


def tag_spans(m):
    """{tag index: (start, end)} of every tag whose data lies in the tag data: from its address to the next one's"""
    low, high = ce_map.TAG_CACHE_BASE, ce_map.TAG_CACHE_BASE + m.tag_data_size
    placed = sorted((base, index) for index, (_, _, _, base) in enumerate(m.tags) if low <= base < high)
    spans = {}
    for k, (base, index) in enumerate(placed):
        end = placed[k + 1][0] if k + 1 < len(placed) else high
        spans[index] = (base, end)
    return spans


def bitmap_pixels(m, base, end):
    """the pixel data of a bitmap tag's bitmaps, as (offset, size) in the map, each checked to lie in it"""
    c = m.cache
    if base + BITMAP_GROUP_BITMAPS + 8 > end:
        return []
    out = []
    for element in c.block(base + BITMAP_GROUP_BITMAPS, BITMAP_DATA_SIZE, 4096):
        offset, size = struct.unpack("<ii", c.read(element + BITMAP_PIXELS_OFFSET, 8))
        if size <= 0 or offset < 0 or offset > m.length - size:
            raise ce_map.MapError("a bitmap's pixels lie outside the map")
        out.append((offset, size))
    return out


def classic_world_id(path):
    """(classic_world_id hex, report dict) of one map file"""
    m = ce_map.Map(str(path))
    c = m.cache
    h = hashlib.sha256(LABEL + struct.pack("<I", CWID_VERSION))
    report = {"map": m.name, "build": m.build, "bsps": 0, "tags": 0, "bitmaps": 0, "pixel_bytes": 0}
    references = c.block(m.scenario + ce_map.SCNR_STRUCTURE_BSPS, 0x20, 32)
    for index, reference in enumerate(references):
        file_offset, file_size = struct.unpack("<ii", c.read(reference, 8))
        if file_offset < 0 or file_size <= 0 or file_offset > m.length - file_size:
            raise ce_map.MapError("structure bsp %d is outside the map" % index)
        h.update(b"BSP " + struct.pack("<II", index, file_size))
        h.update(hashlib.sha256(m.file[file_offset:file_offset + file_size]).digest())
        report["bsps"] += 1
    spans = tag_spans(m)
    chosen = []
    for index, (group, tag_id, _, _) in enumerate(m.tags):
        if group not in INPUT_GROUPS or index not in spans:
            continue
        name = m.tag_name(tag_id)
        if group == _group("bitm") and name.lower().startswith(BITMAPS_LEFT_OUT):
            continue
        chosen.append((group, name.lower(), index))
    chosen.sort()
    for group, name, index in chosen:
        start, end = spans[index]
        data = c.read(start, end - start)
        h.update(b"TAG " + struct.pack(">I", group) + struct.pack("<I", len(name)) + name.encode("ascii", "replace"))
        h.update(hashlib.sha256(data).digest())
        report["tags"] += 1
        if group == _group("bitm"):
            for offset, size in bitmap_pixels(m, start, end):
                h.update(b"PIX " + struct.pack("<I", size))
                h.update(hashlib.sha256(m.file[offset:offset + size]).digest())
                report["bitmaps"] += 1
                report["pixel_bytes"] += size
    return h.hexdigest(), report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("maps", nargs="+")
    parser.add_argument("--json", help="write {map: {base_id, classic_world_id, build, ...}} here")
    args = parser.parse_args(argv)
    out = {}
    for path in args.maps:
        data = Path(path).read_bytes()
        cwid, report = classic_world_id(path)
        report["base_id"] = identity.base_id(data)
        report["classic_world_id"] = cwid
        report["cwid_version"] = CWID_VERSION
        out[Path(path).stem] = report
        print("%s %s base %s cwid %s (%d BSPs, %d tags, %d bitmaps)" % (Path(path).stem, report["build"],
              report["base_id"][:16], cwid, report["bsps"], report["tags"], report["bitmaps"]))
    if args.json:
        Path(args.json).write_text(json.dumps(out, indent=1, sort_keys=True) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
