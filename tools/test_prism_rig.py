"""PRISM's extended skeletons: the host's rig (port/linux/vulkan/xgpu_rig.c) over made-up skeletons
(port/linux/vulkan/tests/rig_test.c, plain and with AddressSanitizer and UndefinedBehaviorSanitizer), the
art side's compiler (tools/prism_rig.py) giving the host's bytes, and the recorded frames' runner
(tests/rig_eval.c). No GPU or game data is needed."""

import json
import math
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
VULKAN = ROOT / "port/linux/vulkan"
sys.path.insert(0, str(ROOT / "tools"))
import prism_rig  # noqa: E402

STATUS = {"FORMAT": 1, "SKELETON": 2, "UNKNOWN_BONE": 3}


def compiler():
    return os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")


def build(tmp_path, main, flags=()):
    cc = compiler()
    if not cc or not sys.platform.startswith(("linux", "darwin")):
        pytest.skip("needs a C compiler on Linux or macOS")
    binary = tmp_path / Path(main).stem
    try:
        subprocess.run([cc, "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", *flags, f"-I{VULKAN}",
                        str(VULKAN / "tests" / main), str(VULKAN / "xgpu_rig.c"), "-o", str(binary), "-lm"],
                       check=True, capture_output=True, text=True)
    except subprocess.CalledProcessError as error:
        if flags and "sanitize" in (error.stderr or ""):
            pytest.skip("the compiler has no sanitizers")
        raise AssertionError(error.stderr)
    return binary


@pytest.mark.parametrize("mode", ["plain", "sanitized"])
def test_rig(tmp_path, mode):
    flags = [] if mode == "plain" else ["-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                                        "-fno-sanitize-recover=undefined"]
    binary = build(tmp_path, "rig_test.c", flags)
    run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=300,
                         env=dict(os.environ, ASAN_OPTIONS="detect_leaks=1"))
    assert run.returncode == 0, run.stdout + run.stderr
    assert "all passed" in run.stdout


def translation(x, y, z):
    return [1, 0, 0, x, 0, 1, 0, y, 0, 0, 1, z]


def skeleton():
    """An arm on a body (CE nodes) and six helpers."""
    bones = [("root", -1, (0, 0, 1)), ("upperarm", 0, (0, 0.2, 1.4)), ("forearm", 1, (0.3, 0.2, 1.4)),
             ("hand", 2, (0.58, 0.2, 1.4)), ("pelvis", 0, (0, 0, 0.9)),
             ("deltoid", 0, (0, 0.2, 1.4)), ("forearm twist", 2, (0.44, 0.2, 1.4)), ("aimer", 0, (0, 0, 1.5)),
             ("knee volume", 2, (0.3, 0.2, 1.4)), ("pouch", 4, (0.1, 0.15, 0.9)), ("looker", 0, (0, -0.1, 1.5))]
    return [{"name": n, "parent": p, "bind_world": translation(*t)} for n, p, t in bones], 5


EXAMPLE = {"version": 1, "comment": "every type",
           "helpers": [
               {"bone": "forearm twist", "type": "twist", "source": "forearm", "child": "hand", "axis": "x",
                "fraction": 0.5},
               {"bone": "deltoid", "type": "copy_rotation", "source": "upperarm", "fraction": 0.35, "axes": "yz",
                "space": "parent"},
               {"bone": "knee volume", "type": "driven", "driver": {"bone": "hand", "angle": "y",
                                                                     "range_deg": [0, 140]},
                "translate": [0, 0, 0.012], "scale": [1, 1.08, 1.08], "curve": "smoothstep"},
               {"bone": "pouch", "type": "spring", "parent": "pelvis", "stiffness": 120, "damping": 9,
                "gravity": 0.3, "limit_deg": 25, "length": 0.08,
                "drive": {"function": "A", "rotate_deg": [0, 0, 12]}},
               {"bone": "looker", "type": "look_at", "target": "camera", "axis": "x", "up": "z", "fraction": 0.25},
               {"bone": "aimer", "type": "aim", "target": "hand", "axis": "y", "limit_deg": 40}]}

VARIANTS = [
    EXAMPLE,
    {"version": 1, "helpers": []},
    {"version": 1, "helpers": [{"bone": "forearm twist", "type": "twist", "source": "forearm", "child": "hand",
                                "axis": "y", "helper_axis": "z", "fraction": 0.333}]},
    {"version": 1, "helpers": [{"bone": "knee volume", "type": "driven", "driver": {"function": "c",
                                                                                   "range": [0.1, 0.9]},
                                "rotate_deg": [5, -7.5, 30]}]},
    {"version": 1, "helpers": [{"bone": "knee volume", "type": "driven",
                                "driver": {"distance": ["hand", "pelvis"], "range": [0.2, 1.1]},
                                "translate": [0.01, 0.02, -0.03], "curve": "linear"}]},
    {"version": 1, "helpers": [{"bone": "deltoid", "type": "copy_rotation", "source": "aimer"},
                               {"bone": "aimer", "type": "aim", "target": "camera", "axis": "z"}]},
]

REFUSED = [
    ({"version": 1, "helpers": [{"bone": "nobody", "type": "twist"}]}, "UNKNOWN_BONE"),
    ({"version": 1, "helpers": [{"bone": "deltoid", "type": "copy_rotation", "source": "nobody"}]}, "UNKNOWN_BONE"),
    ({"version": 1, "helpers": [{"bone": "pouch", "type": "spring", "parent": "nobody"}]}, "UNKNOWN_BONE"),
    ({"version": 1, "helpers": [{"bone": "knee volume", "type": "driven",
                                 "driver": {"distance": ["hand", "nobody"], "range": [0, 1]}}]}, "UNKNOWN_BONE"),
    ({"version": 1, "helpers": [{"bone": "deltoid", "type": "copy_rotation", "source": 3}]}, "FORMAT"),
    ({"version": 1, "helpers": [{"bone": "deltoid", "type": "copy_rotation", "source": "upperarm",
                                 "fractoin": 1}]}, "FORMAT"),
    ({"version": 1, "helpers": [{"bone": "hand", "type": "copy_rotation", "source": "upperarm"}]}, "FORMAT"),
    ({"version": 2, "helpers": []}, "FORMAT"),
    ({"version": 1, "helpers": [{"bone": "deltoid", "type": "wiggle"}]}, "FORMAT"),
    ({"version": 1, "helpers": [{"bone": "deltoid", "type": "copy_rotation", "source": "aimer"},
                                {"bone": "aimer", "type": "aim", "target": "deltoid"}]}, "FORMAT"),
    ({"version": 1, "helpers": [{"bone": "pouch", "type": "spring", "parent": "root"}]}, "FORMAT"),
    ({"version": 1, "helpers": [{"bone": "pouch", "type": "spring", "stiffness": 40000}]}, "FORMAT"),
    ({"version": 1, "helpers": [{"bone": "deltoid", "type": "copy_rotation", "source": "upperarm"},
                                {"bone": "deltoid", "type": "copy_rotation", "source": "upperarm"}]}, "FORMAT"),
    ({"version": 1, "helpers": [{"bone": "knee volume", "type": "driven",
                                 "driver": {"function": "A", "range_deg": [0, 1]}}]}, "FORMAT"),
]


def compile_with_host(tmp_path, binary, bones, ce, rig):
    entries = tmp_path / "skeleton.bin"
    entries.write_bytes(prism_rig.skeleton_entries(bones, ce))
    source = tmp_path / "rig.json"
    source.write_text(json.dumps(rig))
    result = tmp_path / "rig.bin"
    run = subprocess.run([str(binary), "compile", str(entries), str(ce), str(source), str(result)],
                         capture_output=True, text=True)
    return run.returncode, (result.read_bytes() if run.returncode == 0 else run.stdout)


def test_compilers_agree(tmp_path):
    """tools/prism_rig.py and the host compile every rule to the same bytes, and refuse the same rigs
    for the same reasons."""
    binary = build(tmp_path, "rig_test.c")
    bones, ce = skeleton()
    for rig in VARIANTS:
        code, host = compile_with_host(tmp_path, binary, bones, ce, rig)
        assert code == 0, host
        assert host == prism_rig.compile_rig(rig, bones, ce)
    for rig, status in REFUSED:
        code, message = compile_with_host(tmp_path, binary, bones, ce, rig)
        assert code == 10 + STATUS[status], message
        with pytest.raises(prism_rig.RigError) as error:
            prism_rig.compile_rig(rig, bones, ce)
        assert error.value.status == status


def test_skeleton_section():
    bones, ce = skeleton()
    data = prism_rig.pack_skeleton(bones, ce)
    assert len(data) % 16 == 0
    first = struct.unpack_from(prism_rig.BONE_FORMAT, data, 0)
    assert first[0] == prism_rig.name_hash("root") and first[1] == -1 and first[2] == prism_rig.BONE_CE_NODE
    helper = struct.unpack_from(prism_rig.BONE_FORMAT, data, 108 * 6)
    assert helper[2] == prism_rig.BONE_HELPER and helper[3] == -1
    # the inverse bind of a translation
    assert helper[5 + 12:5 + 24][3] == pytest.approx(-0.44)
    assert data[108 * len(bones):].split(b"\0")[0] == b"root"
    assert prism_rig.name_hash("a") == 0xE40C292C
    assert prism_rig.name_hash("Bip01 L Forearm") == prism_rig.name_hash("bip01 l forearm")
    with pytest.raises(prism_rig.RigError):
        prism_rig.pack_skeleton(bones, 65)
    bad = [dict(b) for b in bones]
    bad[3]["parent"] = 7
    with pytest.raises(prism_rig.RigError):
        prism_rig.pack_skeleton(bad, ce)


def test_weights():
    joints, weights = prism_rig.quantize_influences([(3, 0.5), (7, 0.25), (1, 0.125), (9, 0.0625), (2, 0.0625)])
    assert joints[:4] == [3, 7, 1, 2] and sum(weights) == 255
    assert weights[0] >= weights[1] >= weights[2]
    joints, weights = prism_rig.quantize_influences([(4, 1.0), (4, 1.0)])
    assert joints == [4, 0, 0, 0] and weights == [255, 0, 0, 0]
    joints, weights = prism_rig.quantize_influences([])
    assert weights == [255, 0, 0, 0]
    for influences in ([(0, 1 / 3), (1, 1 / 3), (2, 1 / 3)], [(5, 0.7), (6, 0.2999), (8, 0.0001)]):
        assert sum(prism_rig.quantize_influences(influences)[1]) == 255


def test_rig_eval(tmp_path):
    """The recorded frames' runner: a rig compiled from .rig.json, frames in, each bone's world and skin
    out, and a skin: a vertex on the twist bone follows the hand's twist by half."""
    binary = build(tmp_path, "rig_eval.c")
    bones, ce = skeleton()
    entries = tmp_path / "skeleton.bin"
    entries.write_bytes(prism_rig.skeleton_entries(bones, ce))
    rig = tmp_path / "rig.json"
    rig.write_text(json.dumps({"version": 1, "helpers": [EXAMPLE["helpers"][0]]}))
    frames = tmp_path / "frames.bin"
    angle = 1.0
    c, s = math.cos(angle), math.sin(angle)
    with open(frames, "wb") as f:
        f.write(b"PRFR" + struct.pack("<III", 1, 2, ce))
        for twist in (False, True):
            f.write(struct.pack("<dII4f3ff", 0.0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0))
            for index in range(ce):
                if twist and index == 3:
                    # the hand turned about its x, about its own origin: skin = T R T^-1
                    hx, hy, hz = 0.58, 0.2, 1.4
                    m = [1, 0, 0, 0, 0, c, -s, hy - (c * hy - s * hz), 0, s, c, hz - (s * hy + c * hz)]
                else:
                    m = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0]
                f.write(struct.pack("<12f", *m))
    skin = tmp_path / "skin.pskin"
    prism_rig.write_pskin(skin, [(0.44, 0.3, 1.4)], [(0, 1, 0)], [[6, 0, 0, 0]], [[255, 0, 0, 0]], [])
    out, skinned = tmp_path / "out.bin", tmp_path / "skinned.bin"
    run = subprocess.run([str(binary), str(entries), str(ce), str(rig), str(frames), str(out), str(skin),
                          str(skinned)], capture_output=True, text=True)
    assert run.returncode == 0, run.stdout + run.stderr
    assert "bones 11 (CE nodes 5)" in run.stdout
    data = struct.unpack("<%df" % (2 * 2 * len(bones) * 12), out.read_bytes())
    assert len(data) == 2 * 2 * len(bones) * 12
    p0 = struct.unpack("<3f", skinned.read_bytes()[:12])
    p1 = struct.unpack("<3f", skinned.read_bytes()[12:24])
    assert p0 == pytest.approx((0.44, 0.3, 1.4), abs=1e-6)
    # half the hand's twist (0.5 rad) about the twist bone's x, through its origin (0.44, 0.2, 1.4)
    assert p1 == pytest.approx((0.44, 0.2 + 0.1 * math.cos(0.5), 1.4 + 0.1 * math.sin(0.5)), abs=1e-5)
    assert prism_rig.read_pskin(skin)["joints"][0] == (6, 0, 0, 0)
