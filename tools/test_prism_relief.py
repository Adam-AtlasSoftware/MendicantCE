"""The relief tier (port/linux/game/prism_relief_gen.c, prism_relief.c; docs/prism_materials.md, "The relief tier") on
made-up bitmaps, asset-free: the generator's C built on its own as a command, as the game builds it. A bump map's
grooves and pits come back sunk and its plates raised, a base map's dark seams sunk where there is no bump map, a
surface with no design gets none; the KTX2 file is one the texture store reads (BC5, every level) and decodes to the
normal map; the bytes are pinned (the same integer arithmetic on every platform and compiler: CI runs this on Linux
and Windows); and the relief classes' twins (prism_relief.c's RELIEF_PATTERNS, port_settings.py's RELIEF_CLASSES)
agree and take Forerunner metal's shaders alone."""
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

import numpy as np
import pytest

ROOT = Path(__file__).resolve().parents[1]
GAME = ROOT / "port/linux/game"
sys.path.insert(0, str(ROOT / "tools"))
import port_settings  # noqa: E402

# the command: a source's bitmaps (raw ARGB words, little endian) in, the relief's numbers out (JSON), its normal
# map and KTX2 file written
MAIN = r"""
#include "prism_relief_gen.h"
#include <stdio.h>
#include <stdlib.h>

static uint32_t *load(const char *path, unsigned long count)
{
	FILE *file = fopen(path, "rb");
	uint32_t *words = malloc(count * 4);
	unsigned long index;

	if (!file || !words)
		exit(3);
	for (index = 0; index < count; index++)
	{
		unsigned char b[4];

		if (fread(b, 1, 4, file) != 4)
			exit(4);
		words[index] = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
	}
	fclose(file);
	return words;
}

int main(int argc, char **argv)
{
	struct prism_relief_source source = { 0 };
	struct prism_relief relief;
	unsigned long size = 0;
	unsigned char *file;
	uint64_t key;
	int status;
	FILE *out;

	if (argc != 9)
		return 2;
	source.base_width = strtoul(argv[2], NULL, 10);
	source.base_height = strtoul(argv[3], NULL, 10);
	source.bump_width = strtoul(argv[5], NULL, 10);
	source.bump_height = strtoul(argv[6], NULL, 10);
	if (source.base_width)
		source.base = load(argv[1], source.base_width * source.base_height);
	if (source.bump_width)
		source.bump = load(argv[4], source.bump_width * source.bump_height);
	key = prism_relief_key(&source);
	status = prism_relief_make(&source, &relief);
	printf("{\"status\": %d, \"key\": \"%08lx%08lx\"", status, (unsigned long)(key >> 32),
		(unsigned long)(key & 0xffffffffUL));
	if (status == PRISM_RELIEF_MADE)
	{
		printf(", \"width\": %lu, \"height\": %lu, \"at_bump\": %d, \"bump_range\": %ld, \"base_range\": %ld, "
			"\"depth\": %ld", relief.width, relief.height, relief.at_bump, relief.bump_range, relief.base_range,
			relief.depth);
		out = fopen(argv[7], "wb");
		fwrite(relief.normal, 1, relief.width * relief.height * 2, out);
		fclose(out);
		file = prism_relief_ktx2(relief.normal, relief.width, relief.height, &size);
		out = fopen(argv[8], "wb");
		fwrite(file, 1, size, out);
		fclose(out);
		free(file);
		prism_relief_free(&relief);
	}
	printf("}\n");
	return 0;
}
"""


def compiler():
    return shutil.which("clang") or shutil.which("cc") or shutil.which("gcc")


@pytest.fixture(scope="module")
def tool(tmp_path_factory):
    folder = tmp_path_factory.mktemp("relief")
    cc = compiler()
    if not cc:
        # (in CI a missing compiler fails: a green step must have run the game's C)
        (pytest.fail if os.environ.get("CI") or os.environ.get("GITHUB_ACTIONS") else pytest.skip)(
            "no C compiler builds the relief's generator")
    (folder / "main.c").write_text(MAIN)
    windows = sys.platform == "win32"
    exe = folder / ("relief.exe" if windows else "relief")
    flags = ["--target=i686-pc-windows-msvc", "-fuse-ld=lld", "-D_CRT_SECURE_NO_WARNINGS"] if windows else []
    result = subprocess.run([cc, *flags, "-O2", "-std=c99", "-Wall", "-Werror", "-I", str(GAME), str(folder / "main.c"),
                             str(GAME / "prism_relief_gen.c"), "-o", str(exe)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    return exe, folder


def run(tool, base=None, bump=None):
    """the relief of a base map and a bump map (each an (h, w, 3 or 4) array of bytes, ARGB words made of them, or
    None): its numbers, normal map ((h, w, 2) bytes) and KTX2 file"""
    exe, folder = tool

    def words(image, name):
        if image is None:
            return "-", 0, 0
        image = np.asarray(image, np.uint32)
        alpha = image[..., 3] if image.shape[-1] == 4 else np.full(image.shape[:2], 255, np.uint32)
        argb = (alpha << 24) | (image[..., 0] << 16) | (image[..., 1] << 8) | image[..., 2]
        path = folder / name
        path.write_bytes(argb.astype("<u4").tobytes())
        return str(path), image.shape[1], image.shape[0]

    b = words(base, "base.raw")
    n = words(bump, "bump.raw")
    out = subprocess.run([str(exe), b[0], str(b[1]), str(b[2]), n[0], str(n[1]), str(n[2]), str(folder / "normal.raw"),
                          str(folder / "normal.ktx2")], capture_output=True, text=True, check=True)
    numbers = json.loads(out.stdout)
    if numbers["status"]:
        return numbers, None, None
    normal = np.frombuffer((folder / "normal.raw").read_bytes(), np.uint8).reshape(numbers["height"],
                                                                                   numbers["width"], 2)
    return numbers, normal, (folder / "normal.ktx2").read_bytes()


def bump_of(height):
    """CE's bump map of a height field (texels): x red, y green (down the picture), z blue, 128 flat"""
    gx = (np.roll(height, -1, 1) - np.roll(height, 1, 1)) / 2.0
    gy = (np.roll(height, -1, 0) - np.roll(height, 1, 0)) / 2.0
    n = np.stack([-gx, -gy, np.ones_like(height)], -1)
    # (IEEE's own operations alone, correctly rounded: the same bytes on every platform)
    n /= np.sqrt(gx * gx + gy * gy + 1.0)[..., None]
    return np.clip(np.round(128 + 127 * n), 0, 255).astype(np.uint8)


def plates(n=128):
    """plates at 0, a channel 8 texels wide and 3 deep along x = 38, a square pit"""
    h = np.zeros((n, n))
    h[:, 34:42] = -3.0
    h[80:92, 80:92] = -3.0
    return h


def lean(normal):
    return normal.astype(int) - 128


def test_a_bump_maps_grooves_sink_and_its_plates_rise(tool):
    numbers, normal, _ = run(tool, base=np.full((128, 128, 3), 90), bump=bump_of(plates()))
    assert numbers["status"] == 0 and numbers["at_bump"] == 1, numbers
    x = lean(normal)[..., 0]
    # the channel's walls lean in toward its floor (the height falls into it from the left, rises out of it on the
    # right), the plates beside it and its floor flat
    assert x[10:30, 33:35].mean() > 20 and x[10:30, 41:43].mean() < -20
    assert abs(x[10:30, 10:30]).max() <= 2 and abs(x[10:30, 37:39]).max() <= 4
    # the pit likewise, both ways
    y = lean(normal)[..., 1]
    assert x[82:90, 79:81].mean() > 20 and x[82:90, 91:93].mean() < -20
    assert y[79:81, 82:90].mean() > 20 and y[91:93, 82:90].mean() < -20
    # deeper than CE's own (a shallower channel's, whose walls CE's bump map does not lean to the most): the gain
    # over the bump map's slopes
    shallow = 0.3 * plates()
    _, normal, _ = run(tool, base=np.full((128, 128, 3), 90), bump=bump_of(shallow))
    ce = lean(bump_of(shallow))[..., 0]
    assert lean(normal)[10:30, 33:35, 0].mean() > 1.3 * ce[10:30, 33:35].mean()


def test_a_base_maps_dark_seams_sink_where_there_is_no_bump_map(tool):
    base = np.full((128, 128, 3), 150)
    base[:, 60:63] = 40
    base[30:33, :] = 40
    numbers, normal, _ = run(tool, base=base, bump=np.full((128, 128, 3), [128, 128, 255]))
    assert numbers["status"] == 0 and numbers["at_bump"] == 0, numbers
    assert numbers["base_range"] >= 20
    x, y = lean(normal)[..., 0], lean(normal)[..., 1]
    assert x[60:100, 59:61].mean() > 8 and x[60:100, 62:64].mean() < -8
    assert y[29:31, 80:120].mean() > 8 and y[32:34, 80:120].mean() < -8


def test_a_surface_with_no_design_has_none(tool):
    rng = np.random.default_rng(7)
    base = 100 + rng.integers(-3, 4, (64, 64, 3))
    numbers, normal, _ = run(tool, base=base)
    assert numbers["status"] == 1 and normal is None
    numbers, _, _ = run(tool)
    assert numbers["status"] == 1
    # (a bitmap of no power of two's size, or too small)
    numbers, _, _ = run(tool, base=np.zeros((48, 64, 3)))
    assert numbers["status"] == 2


def bc4_decode(block):
    e0, e1 = block[0], block[1]
    if e0 > e1:
        palette = [e0, e1] + [((8 - k) * e0 + (k - 1) * e1) / 7 for k in range(2, 8)]
    else:
        palette = [e0, e1] + [((6 - k) * e0 + (k - 1) * e1) / 5 for k in range(2, 6)] + [0, 255]
    bits = int.from_bytes(bytes(block[2:8]), "little")
    return np.array([palette[(bits >> (3 * i)) & 7] for i in range(16)]).reshape(4, 4)


def ktx2_levels(data):
    """(width, height, levels' (offset, length)) of a KTX2 file, as xgpu_ktx2.c reads it"""
    assert data[:12] == bytes([0xab, 0x4b, 0x54, 0x58, 0x20, 0x32, 0x30, 0xbb, 0x0d, 0x0a, 0x1a, 0x0a])
    vk_format, type_size, width, height, depth, layers, faces, levels, supercompression = struct.unpack_from(
        "<9I", data, 12)
    assert (vk_format, type_size, depth, layers, faces, supercompression) == (141, 1, 0, 0, 1, 0)
    assert levels == max(width, height).bit_length()
    index = [struct.unpack_from("<3Q", data, 80 + 24 * k) for k in range(levels)]
    for k, (offset, length, uncompressed) in enumerate(index):
        w, h = max(width >> k, 1), max(height >> k, 1)
        assert length == uncompressed == ((w + 3) // 4) * ((h + 3) // 4) * 16
        assert offset >= 80 + 24 * levels and offset % 16 == 0 and offset + length <= len(data)
    return width, height, [(o, n) for o, n, _ in index]


def test_the_ktx2_file_is_bc5_with_every_level_and_decodes_to_the_normal_map(tool):
    numbers, normal, data = run(tool, base=np.full((256, 128, 3), 90), bump=bump_of(plates(128)[:, :].repeat(1, 0)))
    width, height, levels = ktx2_levels(data)
    assert (width, height) == (numbers["width"], numbers["height"])
    offset, _ = levels[0]
    decoded = np.zeros((height, width, 2))
    blocks = data[offset:]
    k = 0
    for by in range(height // 4):
        for bx in range(width // 4):
            for channel in range(2):
                decoded[by * 4:by * 4 + 4, bx * 4:bx * 4 + 4, channel] = bc4_decode(blocks[k:k + 8])
                k += 8
    error = np.abs(decoded - normal)
    assert error.mean() < 1.0 and error.max() <= 12
    # (the smallest level last in the index, first in the file)
    assert levels[-1][0] < levels[0][0]


def test_the_bytes_are_pinned(tool):
    """the same integer arithmetic on every platform and compiler; a change to what is made moves these, and then
    PRISM_RELIEF_VERSION (every cache file's key) with them"""
    n = 64
    yy, xx = np.mgrid[0:n, 0:n]
    height = np.where(((xx // 16) + (yy // 16)) % 2 == 0, 0.0, -2.0) + np.where((xx - 32) ** 2 + (yy - 20) ** 2 < 36,
                                                                                -2.0, 0.0)
    base = np.stack([(xx * 3 + yy * 5) % 256, (xx * 7) % 256, (yy * 11) % 256], -1)
    numbers, normal, data = run(tool, base=base, bump=bump_of(height))
    assert numbers == PINNED_NUMBERS
    assert hashlib.sha256(normal.tobytes()).hexdigest() == PINNED_NORMAL
    assert hashlib.sha256(data).hexdigest() == PINNED_KTX2
    version = re.search(r"#define PRISM_RELIEF_VERSION (\d+)u", (GAME / "prism_relief_gen.h").read_text()).group(1)
    assert int(version) == PINNED_VERSION


PINNED_VERSION = 1
PINNED_NUMBERS = {"status": 0, "key": "6cc73e78607b48e1", "width": 64, "height": 64, "at_bump": 1, "bump_range": 547,
                  "base_range": 0, "depth": 1024}
PINNED_NORMAL = "f977458105999df2b85ea4812bc545ae54885931c1cefa9c31a4e765206a435d"
PINNED_KTX2 = "3a12ec4efa61477b9fe17633669930c774f08707eba035a440091a94a61caa41"


def test_the_key_follows_every_input(tool):
    base = np.full((64, 64, 3), 100)
    base[:, 30:33] = 20
    keys = {run(tool, base=base)[0]["key"]}
    other = base.copy()
    other[5, 5] = 101
    keys.add(run(tool, base=other)[0]["key"])
    keys.add(run(tool, base=base, bump=np.full((64, 64, 3), [128, 128, 255]))[0]["key"])
    assert len(keys) == 3


# ---------- the relief classes


def c_patterns():
    code = (GAME / "prism_relief.c").read_text()
    table = code.split("RELIEF_PATTERNS[] =", 1)[1].split("};", 1)[0]
    patterns = {}
    for name, pattern in re.findall(r'\{ "([a-z_]+)", "((?:[^"\\]|\\.)*)" \}', table):
        patterns.setdefault(name, []).append(pattern.replace("\\\\", "\\"))
    return patterns


def matches(pattern, text):
    """prism_relief.c's pattern_matches: '*' any run of characters, case not minded"""
    return re.fullmatch(".*".join(re.escape(part) for part in pattern.lower().split("*")), text.lower()) is not None


def relief_class(tag):
    for name, patterns in port_settings.RELIEF_CLASSES.items():
        if any(matches(p, tag) for p in patterns):
            return name
    return None


def test_the_classes_twins_agree():
    assert c_patterns() == port_settings.RELIEF_CLASSES


# the environment shaders the campaign's Forerunner levels draw (their tag paths, as the maps name them)
FORERUNNER_METAL = [
    "levels\\b30\\shaders\\metal plate floor", "levels\\b30\\shaders\\metal plate floor ext",
    "levels\\b30\\shaders\\metal panels generic", "levels\\b30\\shaders\\metal panels generic ext",
    "levels\\b30\\shaders\\metal panels generic ext clean", "levels\\b30\\shaders\\metal panels ext unearthed",
    "levels\\b30\\shaders\\metal panels generator", "levels\\b30\\shaders\\metal strips wide",
    "levels\\b30\\shaders\\metal strips wide a", "levels\\b30\\shaders\\metal strips narrow",
    "levels\\b30\\shaders\\metal strips narrow ext", "levels\\b30\\shaders\\metal strips decorative wide",
    "levels\\b30\\shaders\\metal strips decorative narrow", "levels\\b30\\shaders\\metal flat generic",
    "levels\\b30\\shaders\\decorative wide circle", "levels\\b30\\shaders\\tech dense circuits",
    "levels\\b40\\shaders\\b40_metal panels generator", "levels\\b40\\shaders\\b40_metal strips narrow ext",
    "levels\\b40\\shaders\\b40_metal plate floor ext", "levels\\b40\\shaders\\b40_panels generic ext",
    "levels\\b40\\shaders\\b40_bridgeramp", "levels\\b40\\shaders\\b40_bridgeramp_mid",
    "levels\\b40\\shaders\\b40_cliffmetal", "levels\\b40\\shaders\\b40_generator broken",
    "levels\\c20\\shaders\\metal panels generator-copper", "levels\\c20\\shaders\\metal flat generic-copper",
    "levels\\c20\\shaders\\metal flat generic-copper a", "levels\\c20\\shaders\\metal strips wide b",
    "levels\\c20\\shaders\\c20metal flat generic-x", "levels\\c20\\shaders\\c20column_strip",
]
NOT_METAL = [
    "levels\\b30\\shaders\\alpha grate", "levels\\c10\\shaders\\alpha grate light", "levels\\b30\\shaders\\beach",
    "levels\\b30\\shaders\\cliffs", "levels\\b40\\shaders\\b40_foglampsource", "levels\\b40\\shaders\\b40_pitlights",
    "levels\\b40\\shaders\\b40_ctrlmirror", "levels\\b40\\shaders\\b40_cliff_wall",
    "levels\\c20\\shaders\\plasma panel", "levels\\a50\\shaders\\covenant plate floor",
    "levels\\a10\\shaders\\panel generic", "levels\\d40\\shaders\\lt steel", "levels\\a30\\shaders\\cliff rock",
]


def test_the_classes_take_forerunner_metal_alone():
    assert [t for t in FORERUNNER_METAL if relief_class(t) != "forerunner_metal"] == []
    assert [t for t in NOT_METAL if relief_class(t)] == []


def test_the_setting_is_an_enhancement_of_materials():
    rows = {row[1]: row for row in port_settings.ENHANCEMENTS}
    group, _setting, _label, choices, levels, modes, kind, content, _help = rows["display.surface_relief"]
    assert (group, levels, modes, kind, content) == ("materials", ("false", "true", "true"), "EP",
                                                     port_settings.RUNTIME, None)
    assert [value for _, value in choices] == ["true", "false"]
