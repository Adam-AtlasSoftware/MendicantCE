"""OpenEXR files as debug.pt_reference writes them (port/linux/vulkan/xgpu_exr.c): single part, scan lines, no
compression, 16- or 32-bit float channels. read() refuses anything else; write() makes such a file (for tests).

    channels, attributes = read(path)    # {name: float32 array (height, width)}, {name: value}
"""
import struct
from pathlib import Path

import numpy as np

MAGIC = 20000630
_TYPES = {1: np.float16, 2: np.float32}


def _string(data, at):
    end = data.index(b"\0", at)
    return data[at:end].decode("latin-1"), end + 1


def read(path):
    data = Path(path).read_bytes()
    if len(data) < 8 or struct.unpack_from("<I", data, 0)[0] != MAGIC:
        raise ValueError(f"{path}: not an OpenEXR file")
    version = struct.unpack_from("<I", data, 4)[0]
    if version & 0xff != 2 or version & ~0xff & ~0x400:
        raise ValueError(f"{path}: version {version:#x}: only single-part scan-line files are read")
    at = 8
    attributes = {}
    channels = []
    while data[at] != 0:
        name, at = _string(data, at)
        kind, at = _string(data, at)
        size = struct.unpack_from("<i", data, at)[0]
        at += 4
        value = data[at:at + size]
        at += size
        if kind == "chlist":
            k = 0
            while value[k] != 0:
                cname, k = _string(value, k)
                # (the pixel type, a byte of linearity and three reserved, the x and y sampling)
                pixel_type, _, xs, ys = struct.unpack_from("<iIii", value, k)
                k += 16
                if pixel_type not in _TYPES or xs != 1 or ys != 1:
                    raise ValueError(f"{path}: channel {cname}: type {pixel_type}, sampling {xs}x{ys}")
                channels.append((cname, pixel_type))
        elif kind == "compression":
            attributes[name] = value[0]
        elif kind == "box2i":
            attributes[name] = struct.unpack("<4i", value)
        elif kind == "int":
            attributes[name] = struct.unpack("<i", value)[0]
        elif kind == "float":
            attributes[name] = struct.unpack("<f", value)[0]
        else:
            attributes[name] = value
    at += 1
    if attributes.get("compression", 0) != 0:
        raise ValueError(f"{path}: compressed ({attributes['compression']}): only uncompressed files are read")
    x0, y0, x1, y1 = attributes["dataWindow"]
    width, height = x1 - x0 + 1, y1 - y0 + 1
    if width <= 0 or height <= 0:
        raise ValueError(f"{path}: data window {attributes['dataWindow']}")
    offsets = struct.unpack_from(f"<{height}Q", data, at)
    out = {name: np.zeros((height, width), np.float32) for name, _ in channels}
    for offset in offsets:
        y, size = struct.unpack_from("<ii", data, offset)
        row = offset + 8
        for name, pixel_type in channels:
            dtype = _TYPES[pixel_type]
            count = width * np.dtype(dtype).itemsize
            out[name][y - y0] = np.frombuffer(data, dtype=np.dtype(dtype).newbyteorder("<"), count=width,
                                              offset=row).astype(np.float32)
            row += count
        if row - offset - 8 != size:
            raise ValueError(f"{path}: line {y}: {size} bytes, {row - offset - 8} read")
    return out, attributes


def write(path, channels, attributes=None):
    """channels: {name: float array (height, width)}, written as 32-bit floats; attributes: {name: int}"""
    names = sorted(channels)
    height, width = channels[names[0]].shape
    head = struct.pack("<II", MAGIC, 2)

    def attribute(name, kind, value):
        return name.encode() + b"\0" + kind.encode() + b"\0" + struct.pack("<i", len(value)) + value

    chlist = b"".join(n.encode() + b"\0" + struct.pack("<iIii", 2, 0, 1, 1) for n in names) + b"\0"
    head += attribute("channels", "chlist", chlist)
    head += attribute("compression", "compression", b"\0")
    head += attribute("dataWindow", "box2i", struct.pack("<4i", 0, 0, width - 1, height - 1))
    head += attribute("displayWindow", "box2i", struct.pack("<4i", 0, 0, width - 1, height - 1))
    head += attribute("lineOrder", "lineOrder", b"\0")
    head += attribute("pixelAspectRatio", "float", struct.pack("<f", 1.0))
    head += attribute("screenWindowCenter", "v2f", struct.pack("<2f", 0.0, 0.0))
    head += attribute("screenWindowWidth", "float", struct.pack("<f", 1.0))
    for name, value in (attributes or {}).items():
        head += attribute(name, "int", struct.pack("<i", value))
    head += b"\0"
    line = 8 + width * 4 * len(names)
    table = struct.pack(f"<{height}Q", *[len(head) + height * 8 + y * line for y in range(height)])
    body = b"".join(struct.pack("<ii", y, width * 4 * len(names)) +
                    b"".join(np.asarray(channels[n][y], "<f4").tobytes() for n in names) for y in range(height))
    Path(path).write_bytes(head + table + body)
