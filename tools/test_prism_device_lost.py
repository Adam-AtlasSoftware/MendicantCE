"""A lost device's diagnosis (docs/prism_pathtraced.md, "A lost device"), read from the source: the vendor's own
record of the steps chosen (NVIDIA's checkpoints on NVIDIA's cards, AMD's buffer markers on the others', one of the
two), the start's line naming what a lost device would log by extension, every submission's first and last commands
a step of their own that is never the last step recorded, the steps of the last submissions and the bake's map logged
with the report, and Enhanced's traced light, the upscaler's and the auto exposure's passes named as they are
recorded. No GPU, no game data."""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VULKAN = ROOT / "port/linux/vulkan"


def _function(text, head):
    start = text.index(head)
    end = text.index("\n}\n", start)
    return text[start:end]


def _ids(text):
    return [int(match) for match in re.findall(r"xgpu_vulkan_breadcrumb\([^,]+, (\d+)(?:u)?, \"", text)]


def test_the_vendors_own_record_is_chosen():
    text = (VULKAN / "xgpu_vulkan.c").read_text()
    # (the checkpoints on NVIDIA's cards unless HALO_VULKAN_CHECKPOINTS says otherwise; AMD's markers only without them)
    assert re.search(r"int checkpoints = diagnosis && device_has_extension\(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_"
                     r"EXTENSION_NAME\) &&\s*\(checkpoints_wanted && \*checkpoints_wanted \? atoi\(checkpoints_wanted\)"
                     r" != 0 :\s*vk\.properties\.vendorID == 0x10de\);", text)
    assert re.search(r"int markers = diagnosis && !checkpoints && device_has_extension\(VK_AMD_BUFFER_MARKER_"
                     r"EXTENSION_NAME\);", text)
    # (the checkpoints' functions both had, or neither used)
    assert "vk.lost.set_checkpoint = NULL, vk.lost.checkpoint_data = NULL;" in text


def test_the_start_line_names_the_extensions():
    text = (VULKAN / "xgpu_vulkan.c").read_text()
    # (the old line read as a fault: "a lost device's diagnosis: its fault AMD's markers")
    assert "a lost device's diagnosis:%s%s%s" not in text
    assert "vulkan: should the device be lost, halo.log gets the last step recorded%s%s" in text
    for named in ("(VK_NV_device_diagnostic_checkpoints)", "(VK_AMD_buffer_marker)", "(VK_EXT_device_fault)"):
        assert named in text


def test_the_submissions_steps_never_hide_the_last_step():
    text = (VULKAN / "xgpu_vulkan.c").read_text()
    write = _function(text, "static void breadcrumb_write(")
    assert "vk.lost.last" not in write
    assert "vk.lost.serials[id] = vk.serial;" in write
    crumb = _function(text, "void xgpu_vulkan_breadcrumb(")
    assert "vk.lost.last = name;" in crumb and "breadcrumb_write(commands, id, name);" in crumb
    # (the main commands' first and last, written as steps of their own, the last before they end)
    assert 'breadcrumb_write(slot->main, SUBMISSION_BEGUN, "the submission\'s first commands");' in text
    submit = _function(text, "static uint64_t submit(")
    assert submit.index("SUBMISSION_DONE") < submit.index("vkEndCommandBuffer(vk.slot->main)")


def test_the_report_logs_the_steps_the_checkpoints_and_the_context():
    text = (VULKAN / "xgpu_vulkan.c").read_text()
    report = _function(text, "static void device_lost_report(void)\n{")
    assert report.index("device_lost_steps();") < report.index("device_lost_where();")
    assert "vk.lost.context[index]();" in report
    where = _function(text, "static void device_lost_where(void)")
    assert "VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT" in where and "VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT" in where
    bake = (VULKAN / "xgpu_vulkan_bake.c").read_text()
    assert "xgpu_vulkan_device_lost_context(lost_context);" in _function(bake, "static int make(void)")


def test_the_passes_are_named_and_their_ids_are_their_own():
    files = {name: (VULKAN / name).read_text() for name in ("xgpu_vulkan_rt.c", "xgpu_vulkan_upscale.c",
                                                            "xgpu_vulkan_bake.c", "xgpu_vulkan_water.c")}
    rt = files["xgpu_vulkan_rt.c"]
    for name in ("the traced light's probes", "the traced light's paths", "the traced light's radiance cache",
                 "the traced light's temporal pass", "the traced light's atrous pass", "the traced sun and occlusion",
                 "the traced sun and occlusion's resolve", "the traced world's levels"):
        assert f'"{name}"' in rt
    upscale = files["xgpu_vulkan_upscale.c"]
    for name in ("FSR 3's upscaling", "the denoiser plugin's traced light"):
        assert f'"{name}"' in upscale
    assert "\"Rebaked's exposure\"" in files["xgpu_vulkan_bake.c"]
    # (each file's ids within its range, xgpu_vulkan.h's list; 1 to 255, a byte of the marker)
    ranges = {"xgpu_vulkan_rt.c": (40, 59), "xgpu_vulkan_upscale.c": (70, 79), "xgpu_vulkan_bake.c": (60, 69),
              "xgpu_vulkan_water.c": (89, 90)}
    for name, (low, high) in ranges.items():
        ids = _ids(files[name])
        assert ids, name
        assert all(low <= value <= high for value in ids), (name, ids)
    upscale_ids = re.findall(r"which == PLUGIN_DENOISER \? (\d+) : (\d+)", upscale)
    assert upscale_ids and all(70 <= int(value) <= 79 for value in upscale_ids[0])


def test_the_test_fault_is_a_real_fault_only_when_asked():
    bake = (VULKAN / "xgpu_vulkan_bake.c").read_text()
    step = _function(bake, "void xgpu_vulkan_bake_exposure_step(VkCommandBuffer commands)")
    assert 'getenv("HALO_VULKAN_TEST_FAULT")' in step
    assert "test_fault > 0 && xgpu_vulkan_serial() >= (uint64_t)test_fault" in step
    readme = (VULKAN / "README.md").read_text()
    assert "`HALO_VULKAN_TEST_FAULT=<n>`" in readme and "For testing only" in readme


def test_the_steps_lines_are_short_and_in_the_order_recorded():
    text = (VULKAN / "xgpu_vulkan.c").read_text()
    steps = _function(text, "static void device_lost_steps(void)")
    assert "vk.lost.orders[ids[at - 1]] > vk.lost.orders[id]" in steps
    assert "used + length > LOST_STEPS_LINE" in steps
    assert "vk.lost.orders[id] = ++vk.lost.order;" in _function(text, "static void breadcrumb_write(")
