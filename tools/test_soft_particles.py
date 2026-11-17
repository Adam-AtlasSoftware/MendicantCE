"""display.soft_particles (#1116): on a flat screen an effect shader's group is drawn soft against the scene's depth,
as the VR mode's eyes draw it. The front end's choice (d3d8_device.c's soft_effect_key, built alone with the
setting and the backend's capability stubbed): with the setting off the pixel key's vr_effect stays 0 for every
draw, the draw state as it was; forced on (the negative control) an effect group's draw of no views becomes soft
(1), and still none of the eyes' draws, a group the game left hard, or a backend that cannot draw it. The groups
are the VR path's (not the first-person weapon's, not one drawn without depth), the key's only change is that
assignment, and the frame's end leaves the next frame hard. No GPU or game data."""
import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
DEVICE = ROOT / "port" / "linux" / "src" / "d3d8_device.c"
GEOMETRY = ROOT / "source" / "rasterizer" / "xbox" / "rasterizer_xbox_transparent_geometry.c"


def soft_effect_source() -> str:
    text = DEVICE.read_text(encoding="utf-8")
    start = text.index("/* display.soft_particles (#1116)")
    return text[start:text.index("/* the combiner portions", start)]


def run_choice(tmp_path: Path, setting: int) -> str:
    cc = shutil.which("clang") or shutil.which("cc")
    if not cc:
        pytest.skip("needs a C compiler")
    source = tmp_path / "soft_effect_key.c"
    source.write_text(r"""
#include <stdio.h>
typedef int BOOL;
#define TRUE 1
#define FALSE 0
enum { XGPU_MULTIVIEW_NONE, XGPU_MULTIVIEW_EYES, XGPU_MULTIVIEW_LEFT_EYE, XGPU_MULTIVIEW_RIGHT_EYE };
static struct { BOOL soft_effects; } xgpu_backend_capabilities;
static unsigned long taken;
static int setting = SETTING;
static unsigned long halo_gpu_settings_taken(void) { return taken; }
static int config_boolean(const char *name) { (void)name; return setting; }
""" + soft_effect_source() + r"""
int main(void)
{
    int group, views, capable;

    /* group (the game's say), views (XGPU_MULTIVIEW_*), the backend: the key's vr_effect for each */
    for (capable = 0; capable < 2; capable++)
        for (group = 0; group < 2; group++)
            for (views = 0; views < 4; views++)
            {
                xgpu_backend_capabilities.soft_effects = capable;
                halo_effect_soft(group);
                printf("%d", soft_effect_key(views));
            }
    /* the setting taken up again between frames */
    halo_effect_soft(1);
    xgpu_backend_capabilities.soft_effects = 1;
    setting = !setting;
    taken++;
    printf(" %d\n", soft_effect_key(XGPU_MULTIVIEW_NONE));
    return 0;
}
""")
    binary = tmp_path / "soft_effect_key"
    subprocess.run([cc, "-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror", f"-DSETTING={setting}", str(source),
                    "-o", str(binary)], check=True)
    return subprocess.run([str(binary)], capture_output=True, text=True, check=True).stdout.strip()


def test_off_leaves_every_draw_as_it_was(tmp_path):
    # (and turned on between frames, taken up)
    assert run_choice(tmp_path, 0) == "0" * 16 + " 1"


def test_forced_on_softens_only_a_flat_effect_group(tmp_path):
    # (the negative control: on, the one case changes; and off again, taken up)
    assert run_choice(tmp_path, 1) == "0" * 8 + "0000" + "1000" + " 0"


def test_the_key_changes_only_there_and_the_frame_ends_it():
    text = DEVICE.read_text(encoding="utf-8")
    assert len(re.findall(r"soft_effect_key\(", text)) == 2
    assert "\tif (!key.vr_effect)\n\t\tkey.vr_effect = soft_effect_key(key.multiview);\n" in text
    present = text[text.index("void WINAPI D3DDevice_Present("):]
    present = present[:present.index("\n}\n")]
    assert "\tsoft_effects.group = FALSE;\n" in present


def test_the_groups_are_the_vr_paths():
    text = GEOMETRY.read_text(encoding="utf-8")
    call = text[text.index("halo_effect_soft(\n"):]
    call = call[:call.index(");") + 2]
    assert "_rasterizer_geometry_first_person_bit" in call and "_rasterizer_geometry_no_zbuffer_bit" in call
    vr = text[text.index("halo_vr_effect_depth(!vr_render_eye_pass()"):]
    vr = vr[:vr.index(";")]
    assert "_rasterizer_geometry_first_person_bit" in vr and "_rasterizer_geometry_no_zbuffer_bit" in vr
    # (reset after the group, as the VR path's)
    assert text.count("halo_effect_soft(0);") == 1
