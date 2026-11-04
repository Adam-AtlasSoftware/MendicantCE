"""A stock map unpacked into a cache slot off the game's thread (port/linux/src/map_unpack.c): the precache's slot,
byte for byte, marked last, none for a cut map, two at once."""
import struct
import sys
import zlib
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, ROOT, build, mutated, read, run  # noqa: E402

SOURCE = "port/linux/src/map_unpack.c"
ZLIB = ["adler32.c", "crc32.c", "inffast.c", "inflate.c", "inftrees.c", "zutil.c"]
CASES = ["precache_slot", "marker_last", "torn_stream", "short_stream", "two_threads", "workers"]


def synthetic_map():
    """A made-up Xbox map: its header, then one zlib stream of the rest (many reads' and writes' buffers: the
    test's are 16 KB and 64 KB)."""
    body = b"".join(struct.pack("<I", (index * 2654435761) & 0xFFFFFFFF) if index % 7 else b"\0\0\0\0"
                    for index in range(0x20000))
    length = 0x800 + len(body)
    header = bytearray(0x800)
    struct.pack_into("<4sII", header, 0, b"daeh", 5, length)
    header[0x20:0x24] = b"a10\0"
    header[0x40:0x4E] = b"01.01.14.2342\0"
    struct.pack_into("<HI", header, 0x60, 0, 0x5EED1234)
    header[0x7FC:0x800] = b"toof"
    data = bytes(header) + zlib.compress(body, 6)

    def array(name, data):
        return f"static const unsigned char {name}[{len(data)}] = {{" + ",".join(map(str, data)) + "};\n"
    return (array("map_file", data) + array("map_body", body) +
            f"#define MAP_LENGTH {length}UL\n#define MAP_CHECKSUM 0x5EED1234UL\n"
            f"#define MAP_COPY_SIZE {(length + 0xFFFF) & ~0xFFFF}L\n")


MAP = synthetic_map()


def generated(fault=None):
    code = read(SOURCE).replace('#include "platform.h"\n', "").replace(
        '#include "../../third_party/zlib/zlib_prefixed.h"\n', "")
    code = mutated(mutated(code, "MAP_UNPACK_READ_SIZE = 0x40000,", "MAP_UNPACK_READ_SIZE = 0x4000,"),
                   "MAP_UNPACK_WRITE_SIZE = 0x400000,", "MAP_UNPACK_WRITE_SIZE = 0x10000,")
    if fault:
        code = mutated(code, *fault)
    zlib_dir = ROOT / "port/third_party/zlib"
    zlib_all = "#pragma clang diagnostic push\n#pragma clang diagnostic ignored \"-Weverything\"\n" + \
        "".join(f'#include "{zlib_dir / name}"\n' for name in ZLIB) + "#pragma clang diagnostic pop\n"
    return (("zlib_all.inc", zlib_all), ("map.inc", MAP), ("under_test.inc", code))


@pytest.mark.parametrize("case", CASES)
def test_map_unpack(case):
    status, output = run(build("map_unpack", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("fault,case", [
    # the slot not made the map's size (no zeros to its 64 KB)
    (("ftruncate(slot, (off_t)copy_size) != 0", "0"), "precache_slot"),
    # the inflated map written at the slot's start, over the header's place
    (("(off_t)(MAP_UNPACK_HEADER_SIZE + done)", "(off_t)done"), "precache_slot"),
    # the header never written
    (("!map_unpack_write_all(slot, header, sizeof(header), 0) ||", ""), "precache_slot"),
    # the mark made before the slot is whole
    (("	if (!map_unpack_slot(job->map_path, job->slot_path, job->copy_size, name, &checksum, &file_length))\n"
      "		return 0;\n	cache_slot_source_record(slot, job->map_xbox_path, name, checksum, file_length);\n",
      "	memcpy(name, \"a10\", 4);\n	checksum = MAP_CHECKSUM;\n	file_length = MAP_LENGTH;\n"
      "	cache_slot_source_record(slot, job->map_xbox_path, name, checksum, file_length);\n"
      "	if (!map_unpack_slot(job->map_path, job->slot_path, job->copy_size, name, &checksum, &file_length))\n"
      "		return 0;\n"), "marker_last"),
    # a stream that ends before the map's length taken for the map
    (("if (result == Z_STREAM_END && done != body)\n			goto finished;", "if (result == Z_STREAM_END)\n"
      "			break;"), "short_stream"),
])
def test_negative_control(fault, case):
    status, output = run(build("map_unpack", generated(fault)), case)
    assert status == CHECK_FAILED, output
