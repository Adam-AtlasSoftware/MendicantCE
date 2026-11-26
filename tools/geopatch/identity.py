"""The identities in Python, the twin of port/linux/game/prism_identity.c (which has their definition):
base_id (SHA-256 of a retail map's inflated body), world_id[variant][bsp]. tools/test_prism_world_ids.py checks the
two agree on synthetic maps and sidecars."""
import hashlib
import struct
import zlib

from . import prsx

SCENE_VERSION = 1
RENDER_FORMAT = 1
HEADER = 0x800


def base_id(data):
    """a map's base_id from its bytes: a disc map (header + zlib stream) or an inflated one (a slot, zeros past its
    length ignored)"""
    if len(data) < HEADER or data[:4] != b"daeh" or data[0x7FC:0x800] != b"toof":
        raise ValueError("not an Xbox map")
    length = struct.unpack_from("<I", data, 8)[0]
    if not HEADER <= length <= 0x11600000:
        raise ValueError("its length is out of range")
    body = length - HEADER
    if len(data) >= length:
        return hashlib.sha256(data[HEADER:length]).hexdigest()
    inflater = zlib.decompressobj()
    inflated = inflater.decompress(data[HEADER:], body)
    if len(inflated) != body:
        raise ValueError("inflated to the wrong length")
    return hashlib.sha256(inflated).hexdigest()


def world_id_classic(classic_world_id, bsp):
    h = hashlib.sha256(b"PRSX-WORLD-C")
    h.update(classic_world_id)
    h.update(struct.pack("<HI", bsp, SCENE_VERSION))
    return h.hexdigest()


def counts(entry, bsp):
    return entry.bake_relevant and (entry.bsp == bsp or entry.bsp == prsx.BSP_ALL)


def world_id_enhanced(sidecar, bsp):
    h = hashlib.sha256(b"PRSX-WORLD-E")
    h.update(sidecar.classic_world_id)
    h.update(struct.pack("<HII", bsp, SCENE_VERSION, RENDER_FORMAT))
    for entry in sidecar.entries:
        if counts(entry, bsp):
            h.update(struct.pack("<I", prsx.fourcc(entry.kind)) + entry.sha256)
    return h.hexdigest()


def world_ids(sidecar):
    """{bsp: (classic, enhanced)} for every BSP the directory names"""
    return {bsp: (world_id_classic(sidecar.classic_world_id, bsp), world_id_enhanced(sidecar, bsp))
            for bsp in sidecar.bsps()}
