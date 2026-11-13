"""display.prism_rt's GLSL: port/linux/vulkan/xgpu_vulkan_rt_glsl.inc is what tools/prism_rt_glsl/generate.py
writes (the .inc is generated: an edit made to it alone would be lost at the next generation)."""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def test_inc_is_generated():
    subprocess.run([sys.executable, "-B", str(ROOT / "tools/prism_rt_glsl/generate.py"), "--check"], check=True)
