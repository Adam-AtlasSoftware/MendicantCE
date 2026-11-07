"""Runs the C tests of the renderer's command stream (port/linux/src/xgpu_stream.h):
the guest's encoder (port/linux/src/xgpu_stream_encode.c) and the host's
decoder and render thread (port/android/host/host_render_stream.c), built
together with the build machine's C compiler on a 64-bit Linux or macOS
host (the test reserves 8 GB of address space for a guest window at a base,
as with the base register), and again with ThreadSanitizer where the
compiler has it.

The encoder implements the renderer's interface (xgpu_backend.h, with
xgpu.h and xgpu_shader.h); the test copies those headers, and the encoder
itself (whose quoted includes would otherwise find port/linux/src's
platform.h first), next to a stand-in for the guest's platform.h
(port/android/tests/render_stream). The headers are taken from
port/linux/src, or from the folder in HALO_RENDER_INTERFACE (a checkout of
the renderer split's)."""
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
INTERFACE_HEADERS = ("xgpu.h", "xgpu_backend.h", "xgpu_shader.h")


def compiler():
    for name in ("clang", "cc", "gcc"):
        path = shutil.which(name)
        if path:
            return path
    return None


def interface_folder():
    folder = Path(os.environ.get("HALO_RENDER_INTERFACE", ROOT / "port" / "linux" / "src"))
    if all((folder / name).exists() for name in INTERFACE_HEADERS):
        return folder
    return None


def build(cc, headers, binary, flags):
    subprocess.run(
        [cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O1", "-g", *flags, "-D_GNU_SOURCE", "-D_DARWIN_C_SOURCE",
         "-DHALO_GUEST_BASE=1", "-DXGPU_STREAM_TEST=1",
         f"-I{headers}", f"-I{ROOT / 'port' / 'linux' / 'src'}", f"-I{ROOT / 'port' / 'android' / 'host'}",
         f"-I{ROOT / 'port' / 'android' / 'include'}",
         str(ROOT / "port" / "android" / "tests" / "host_render_stream_test.c"),
         str(headers / "xgpu_stream_encode.c"), str(headers / "xgpu_stream_describe.c"),
         str(ROOT / "port" / "android" / "host" / "host_render_stream.c"),
         str(ROOT / "port" / "android" / "host" / "host_render_record.c"),
         "-o", str(binary), "-lpthread",
         # (the recording's blocks are compressed with Apple's libcompression)
         *(["-lcompression"] if sys.platform == "darwin" else [])],
        check=True,
    )


@pytest.mark.parametrize("sanitizer", [None, "thread"])
def test_render_stream(tmp_path, sanitizer):
    cc = compiler()
    if not cc:
        pytest.skip("no C compiler")
    if sys.platform not in ("linux", "darwin") or platform.architecture()[0] != "64bit":
        pytest.skip("needs a 64-bit Linux or macOS host")
    folder = interface_folder()
    if not folder:
        pytest.skip("the renderer's interface (xgpu_backend.h) is not there yet")
    headers = tmp_path / "headers"
    headers.mkdir()
    for name in INTERFACE_HEADERS:
        shutil.copy(folder / name, headers / name)
    for name in ("xgpu_stream_encode.c", "xgpu_stream_describe.c"):
        shutil.copy(ROOT / "port" / "linux" / "src" / name, headers / name)
    shutil.copy(ROOT / "port" / "android" / "tests" / "render_stream" / "platform.h", headers / "platform.h")
    binary = tmp_path / "host_render_stream_test"
    flags = [f"-fsanitize={sanitizer}"] if sanitizer else []
    try:
        build(cc, headers, binary, flags)
    except subprocess.CalledProcessError:
        if sanitizer:
            pytest.skip(f"the compiler cannot build with -fsanitize={sanitizer}")
        raise
    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=900)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "ThreadSanitizer" not in result.stderr, result.stderr


def test_stock_bytes_are_the_base_encoders(tmp_path):
    # display.prism_textures and display.prism_models stock: the encoder with
    # no texture store hook and no mesh calls
    # (and with a hook that has no ids) writes the same records, byte for
    # byte, as the base branch's encoder (HALO_STREAM_BASE, default
    # h1-integration: the texture store's base), over a repeatable run of
    # texture stages (port/android/tests/render_stream_bytes.c); with ids
    # they differ (the check sees them)
    cc = compiler()
    if not cc:
        pytest.skip("no C compiler")
    if sys.platform not in ("linux", "darwin") or platform.architecture()[0] != "64bit":
        pytest.skip("needs a 64-bit Linux or macOS host")
    base_ref = os.environ.get("HALO_STREAM_BASE", "h1-integration")
    names = ("xgpu.h", "xgpu_backend.h", "xgpu_shader.h", "xgpu_stream.h", "xgpu_stream_encode.c",
             "xgpu_stream_describe.c")
    outputs = {}
    for side in ("base", "new"):
        folder = tmp_path / side
        folder.mkdir()
        for name in names:
            if side == "base":
                shown = subprocess.run(["git", "-C", str(ROOT), "show", f"{base_ref}:port/linux/src/{name}"],
                                       capture_output=True)
                if shown.returncode:
                    pytest.skip(f"the base {base_ref} is not in this checkout")
                (folder / name).write_bytes(shown.stdout)
            else:
                shutil.copy(ROOT / "port" / "linux" / "src" / name, folder / name)
        shutil.copy(ROOT / "port" / "android" / "tests" / "render_stream" / "platform.h", folder / "platform.h")
        binary = folder / "bytes"
        subprocess.run(
            [cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O1", "-g", "-D_GNU_SOURCE", "-D_DARWIN_C_SOURCE",
             "-DHALO_GUEST_BASE=1", "-DXGPU_STREAM_TEST=1",
             *(["-DBYTES_HOOK=1", "-DBYTES_MESH=1"] if side == "new" else []),
             f"-I{folder}", f"-I{ROOT / 'port' / 'linux' / 'src'}", f"-I{ROOT / 'port' / 'android' / 'host'}",
             f"-I{ROOT / 'port' / 'android' / 'include'}",
             str(ROOT / "port" / "android" / "tests" / "render_stream_bytes.c"), str(folder / "xgpu_stream_encode.c"),
             str(folder / "xgpu_stream_describe.c"), "-o", str(binary)],
            check=True)
        for mode in (["none"] if side == "base" else ["none", "zero", "ids", "meshcap", "mesh"]):
            output = folder / f"{mode}.bin"
            subprocess.run([str(binary), str(output), mode], check=True, timeout=120)
            outputs[side, mode] = output.read_bytes()
    assert len(outputs["base", "none"]) > 100000
    assert outputs["new", "none"] == outputs["base", "none"]
    assert outputs["new", "zero"] == outputs["base", "none"]
    assert outputs["new", "ids"] != outputs["base", "none"]
    # display.prism_models stock: the mesh store's capability on, nothing of it
    # called (the guest's stock tier installs no hook): the same bytes; a model
    # drawn with a mesh: its instances and parts' draws in them
    assert outputs["new", "meshcap"] == outputs["base", "none"]
    assert outputs["new", "mesh"] != outputs["base", "none"]
