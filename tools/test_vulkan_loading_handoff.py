"""Held guest frames retire GPU work without presenting over loading art."""
from pathlib import Path
import subprocess

from test_load_trace import build


ROOT = Path(__file__).resolve().parents[1]


def test_held_frames_retire_without_acquiring_or_presenting(tmp_path):
    core = (ROOT / "port/linux/vulkan/xgpu_vulkan.c").read_text()
    core = core[core.index("void xgpu_vulkan_frame_end("):
                core.index("void xgpu_vulkan_statistics_peek(")]
    stream = (ROOT / "port/linux/vulkan/xgpu_vulkan_stream.c").read_text()
    # The rest of frame_end only prints aggregate statistics every 600 frames.
    stream = stream[stream.index("static void stream_present("):
                    stream.index('\tif (stream.statistics)')] + "}\n"
    source = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#define VK_NULL_HANDLE 0
#define VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT 1
#define VK_ACCESS_SHADER_WRITE_BIT 2
#define VK_PIPELINE_STAGE_HOST_BIT 4
#define VK_ACCESS_HOST_READ_BIT 8
#define VK_PRESENT_MODE_FIFO_KHR 2
struct xgpu_vulkan_image { int value; } image;
struct query { unsigned counter_count; uint64_t serial; } query;
static struct {
    int gpu_time_armed;
    struct query *queries;
    uint64_t frame;
    struct { unsigned frames; } statistics;
    int interval, present_mode;
} vk;
static unsigned acquisitions, presentations, blank, submissions, recordings;
static unsigned barriers, completions, garbage, texture_frames;
static uint64_t next_serial, published_frame;
static int held;
static int main_commands(void) { return 7; }
static void barrier(int command, int a, int b, int c, int d)
{ assert(command == 7 && a == 1 && b == 2 && c == 4 && d == 8); barriers++; }
static uint64_t present(struct xgpu_vulkan_image *shown, int frame)
{
    acquisitions++; presentations++;
    if (!shown) blank++;
    else assert(shown == &image);
    assert(frame == 1);
    return ++next_serial;
}
static unsigned paced_updates, unshown;
static void frame_pacing_update(void) { paced_updates++; }
static void xgpu_vulkan_pacing_unshown(void) { unshown++; }
static void recording_begin(void) { recordings++; }
static uint64_t submit(int acquired, int shown)
{ assert(!acquired && !shown); submissions++; return ++next_serial; }
static void queries_publish(uint64_t frame) { published_frame = frame; }
static void completed_serial(void) { completions++; }
static void garbage_collect(void) { garbage++; }
''' + core + r'''
struct stream_target { struct xgpu_vulkan_image *image; } target = { &image };
static struct { uint32_t back_buffer; } stream;
static int host_render_stream_loading_holds(void) { return held; }
static struct stream_target *target_get(uint32_t number)
{ assert(number <= 1); return number ? &target : NULL; }
static void target_resolve(struct stream_target *value) { (void)value; }
static void xgpu_vulkan_textures_frame_end(void) { texture_frames++; }
static void world_make_some(void) {}
''' + stream + r'''
int main(void)
{
    vk.frame = 2;
    vk.gpu_time_armed = 1;
    query.counter_count = 1;
    vk.queries = &query;
    held = 1;
    stream_present(NULL, 1);
    stream_frame_end(NULL);
    assert(!acquisitions && !presentations && !blank);
    assert(recordings == 1 && submissions == 1 && barriers == 1);
    assert(query.serial == 1 && !vk.queries && published_frame == 0);
    assert(vk.frame == 3 && vk.statistics.frames == 1 && vk.gpu_time_armed);
    assert(completions == 1 && garbage == 1 && texture_frames == 1);
    /* A normal NULL image still presents black (startup/VR semantics). */
    held = 0;
    stream_present(NULL, 0);
    stream_frame_end(NULL);
    assert(acquisitions == 1 && presentations == 1 && blank == 1);
    assert(submissions == 1 && vk.frame == 4 && vk.gpu_time_armed);
    stream_present(NULL, 1);
    stream_frame_end(NULL);
    assert(acquisitions == 2 && presentations == 2 && blank == 1);
    assert(submissions == 1 && vk.frame == 5 && vk.statistics.frames == 3);
    assert(completions == 3 && garbage == 3 && texture_frames == 3);
    /* (display.low_latency: each frame's pacing looked at, and the held
    frame, which shows nothing, taken at once) */
    assert(paced_updates == 3 && unshown == 1);
    return 0;
}
'''
    subprocess.run([str(build(tmp_path, source))], check=True, timeout=10)
