#!/usr/bin/env python3
"""Check a mod bitmap tag file the way port/linux/game/mod_bitmaps.c does.

Walks the tag (header, colour plate, pixels, sequences, sprites, bitmaps,
nothing after) and compares it with the map's bitmap of the same name: at
least as many bitmaps, each of the map's type, the same number of sequences,
each with at least as many sprites. Prints a JSON verdict.

usage: mod_bitmap_check.py <mod .bitmap> <map's .bitmap> [--out verdict.json]
"""
import json
import struct
import sys

HDR = 0x40
GROUP, SEQ, SPRITE, DATA = 0x6C, 0x40, 0x20, 0x30
FORMATS = {0: "a8", 1: "y8", 2: "ay8", 3: "a8y8", 6: "r5g6b5", 8: "a1r5g5b5", 9: "a4r4g4b4", 10: "x8r8g8b8",
           11: "a8r8g8b8", 14: "dxt1", 15: "dxt3", 16: "dxt5", 17: "p8_bump"}
TYPES = {0: "2d", 1: "3d", 2: "cube"}


def be32(b, o): return struct.unpack_from(">I", b, o)[0]
def s16(b, o): return struct.unpack_from(">h", b, o)[0]


def read(path):
    b = open(path, "rb").read()
    r = HDR
    g = dict(group=b[0x24:0x28].decode("latin-1"), version=struct.unpack_from(">H", b, 0x38)[0],
             type=s16(b, r), format=s16(b, r + 2), usage=s16(b, r + 4), mipmap_count=s16(b, r + 0x4C))
    plate, pix = be32(b, r + 0x1C), be32(b, r + 0x30)
    nseq, nbm = be32(b, r + 0x54), be32(b, r + 0x60)
    cur = HDR + GROUP + plate
    pix_off = cur
    cur += pix
    seqs = []
    so = cur
    cur += nseq * SEQ
    for i in range(nseq):
        e = so + i * SEQ
        ns = be32(b, e + 0x34)
        seqs.append(dict(name=b[e:e + 32].split(b"\0")[0].decode("latin-1"), first=s16(b, e + 0x20),
                         count=s16(b, e + 0x22), sprites=ns))
        cur += ns * SPRITE
    bms = []
    for i in range(nbm):
        e = cur + i * DATA
        bms.append(dict(sig=b[e:e + 4].decode("latin-1"), width=s16(b, e + 4), height=s16(b, e + 6), depth=s16(b, e + 8),
                        type=TYPES.get(s16(b, e + 0xA), s16(b, e + 0xA)),
                        format=FORMATS.get(s16(b, e + 0xC), s16(b, e + 0xC)), mipmaps=s16(b, e + 0x14),
                        pixels_offset=be32(b, e + 0x18)))
    cur += nbm * DATA
    g.update(pixel_bytes=pix, color_plate_bytes=plate, sequences=seqs, bitmaps=bms, trailing=len(b) - cur)
    return g


def main():
    args = sys.argv[1:]
    out = None
    if "--out" in args:
        i = args.index("--out")
        out = args[i + 1]
        del args[i:i + 2]
    mod, ref = read(args[0]), read(args[1])
    errs = []
    if mod["group"] != "bitm" or mod["version"] != 7:
        errs.append("not a bitmap tag of version 7")
    if mod["trailing"] != 0:
        errs.append(f"file goes on {mod['trailing']} bytes past its last bitmap")
    for bm in mod["bitmaps"]:
        if bm["sig"] != "bitm" or not isinstance(bm["format"], str) or not 1 <= bm["width"] <= 4096 or \
                not 1 <= bm["height"] <= 4096 or bm["mipmaps"] > max(bm["width"], bm["height"]).bit_length() - 1:
            errs.append(f"bitmap not drawable: {bm}")
    if len(mod["bitmaps"]) < len(ref["bitmaps"]):
        errs.append("fewer bitmaps than the map's")
    for a, r in zip(mod["bitmaps"], ref["bitmaps"]):
        if a["type"] != r["type"]:
            errs.append("bitmap type differs")
    if len(mod["sequences"]) != len(ref["sequences"]):
        errs.append(f"sequence count {len(mod['sequences'])} vs map {len(ref['sequences'])}")
    for a, r in zip(mod["sequences"], ref["sequences"]):
        if a["sprites"] < r["sprites"] or (r["count"] > 0 and a["count"] <= 0):
            errs.append("sequence has fewer sprites or no bitmaps")
    v = dict(mod=args[0], reference=args[1], mod_tag=mod, map_tag=ref, errors=errs, verdict=not errs)
    s = json.dumps(v, indent=1)
    if out:
        open(out, "w").write(s)
    print(s)
    sys.exit(0 if not errs else 1)


if __name__ == "__main__":
    main()
