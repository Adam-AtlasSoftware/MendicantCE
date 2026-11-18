"""A texture's least alpha (xgpu_texture_minimum_alpha), which the front end
drops a draw's alpha test by, against the decoder's own alphas of the same
texels (port/linux/tests/texture_alpha_test.c): the decoder built as the
Vulkan build's host builds it, beside the stand-in for the guest's
platform.h (tools/vulkan_build.py). No GPU or game data."""
import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
SOURCES = ROOT / "port" / "linux" / "src"
XDK = ROOT / "port" / "include" / "xdk"


def xdk_enums() -> str:
    """the XDK's enumerations and the texture words' fields, as the build's xdk_enums.h has them"""
    pdb = (XDK / "xdk_pdb.h").read_text(encoding="utf-8")
    start = pdb.index("/* ---------- enumerations")
    end = pdb.index("/* ---------- structures and unions", start)
    defines = [line for line in (XDK / "xdk_d3d8.h").read_text(encoding="utf-8").splitlines()
               if re.match(r"#define D3D(FORMAT|SIZE|TEXTURE)_", line)]
    return pdb[start:end] + "\n".join(defines) + "\n"


def test_texture_least_alpha(tmp_path):
    cc = shutil.which("clang") or shutil.which("cc")
    if not cc:
        pytest.skip("needs a C compiler")
    shared = tmp_path / "shared"
    shared.mkdir()
    for name in ("xgpu.h", "xgpu_coverage.h", "xbox_texture_decode.c"):
        shutil.copy(SOURCES / name, shared / name)
    shutil.copy(ROOT / "port/linux/vulkan/xgpu_vulkan_platform.h", shared / "platform.h")
    (shared / "xdk_enums.h").write_text(xdk_enums(), encoding="utf-8")
    binary = tmp_path / "texture_alpha_test"
    decoder = tmp_path / "xbox_texture_decode.o"
    subprocess.run([cc, "-std=gnu11", "-O1", "-w", f"-I{shared}", "-c", str(shared / "xbox_texture_decode.c"), "-o",
                    str(decoder)], check=True)
    subprocess.run([cc, "-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror", f"-I{shared}",
                    str(ROOT / "port/linux/tests/texture_alpha_test.c"), str(decoder), "-o", str(binary), "-lm"],
                   check=True)
    run = subprocess.run([str(binary)], capture_output=True, text=True)
    assert run.returncode == 0, run.stdout + run.stderr
