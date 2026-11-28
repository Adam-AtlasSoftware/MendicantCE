"""A change of lighting to PRISM Path Traced in play (docs/prism_pathtraced.md, "A change to Path Traced"), read from
the source: the passes' pipelines are made on a thread of their own and CE's frame is drawn meanwhile (the game never
held while the driver compiles them), the traced structure of a new set never reads back the write-combined buffers
it fills, and a BSP's reach walked or read once in a run is taken again from memory when the same inputs are sent
again (a set begun again in play), never a pack's walk where an own walk was asked for. No GPU, no game data."""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VULKAN = ROOT / "port/linux/vulkan"


def _function(text, head):
    start = text.index(head)
    end = text.index("\n}\n", start)
    return text[start:end]


def test_the_passes_are_made_on_their_own_thread():
    text = (VULKAN / "xgpu_vulkan_pt.c").read_text()
    make = _function(text, "static int make(void)")
    # (no pass made on the render thread where a thread can be had: the thread started, CE's frame meanwhile)
    assert "pthread_create(&pt.compile_thread, NULL, pipelines_thread, NULL)" in make
    started = make.index("pthread_create(&pt.compile_thread")
    assert "return 0;" in make[started:make.index("passes_made:")]
    assert "pipeline_make(" not in make
    # (while it runs, make() is 0 and nothing waits for it; once done, joined and the rest made)
    running = make[make.index("if (pt.compile_running)"):]
    assert running.index("__atomic_load_n(&pt.compile_done, __ATOMIC_ACQUIRE)") < running.index("pipelines_wait();")
    assert "goto passes_made;" in running
    # (every pass of before, made by the thread)
    thread = _function(text, "static void *pipelines_thread(void *unused)")
    for name in ("LIGHT", "TEMPORAL", "FIX", "ATROUS", "COMPOSITE", "REFERENCE", "LAMPS", "CACHE", "WATER",
                 "STABILIZE", "UPSAMPLE", "REFERENCE_STATS"):
        assert f"pipeline_make(PASS_{name}," in thread
    assert "__atomic_store_n(&pt.compile_done, 1, __ATOMIC_RELEASE)" in thread
    # (the device never let go of under a compile: the shutdown joins it before anything else)
    shutdown = _function(text, "void xgpu_vulkan_pt_shutdown(void)")
    assert shutdown.index("pipelines_wait();") < shutdown.index("if (!pt.made)")
    # (the frame with no passes yet is CE's: the frame returns before anything is recorded)
    frame = _function(text, "struct xgpu_vulkan_image *xgpu_vulkan_pt_frame(")
    assert re.search(r"!view \|\| !make\(\)\)\s*return NULL;", frame)


def test_a_new_sets_structure_never_reads_back_its_mapped_buffers():
    text = (VULKAN / "xgpu_vulkan_rt.c").read_text()
    build = _function(text, "static void static_build(void)")
    fold = build[build.index("xgpu_vulkan_scene_levels(&words, &serial)"):build.index("free(opaque_from);")]
    # (the levels' fold reads the set received, by each opaque triangle's place in it)
    assert "rt.receiving + (size_t)opaque_from[triangle] * 9" in fold
    assert "received_level_tag(opaque_from[triangle])" in fold
    # (never the host-visible buffers it writes: write-combined, a read of them is uncached)
    for mapped in ("attributes_to[", "to + (size_t)triangle", "vertices.mapped", "attributes.mapped"):
        assert mapped not in fold
    assert "opaque_from[at[0]] = triangle;" in build


def test_a_reach_walked_before_is_taken_from_memory():
    text = (VULKAN / "xgpu_vulkan_bake.c").read_text()
    thread = _function(text, "static void *reach_thread(void *unused)")
    # (its order: the farm's file, the shipped pack's walk of the world drawn, then memory, then a walk)
    file_at = thread.index("reach_file_read(bake.reach_path, input_hash)")
    shipped_at = thread.index("reach_shipped_read(input_hash)")
    kept_at = thread.index("reach_kept_take(input_hash, own)")
    walk_at = thread.index("prism_bake_reach_build(")
    assert file_at < shipped_at < kept_at < walk_at
    # (a walk taken from memory is not kept again; a pack's is kept as not own)
    assert "reach_kept_put(input_hash, from_file != 2)" in thread
    assert "from_file != 3" in thread
    # (after a pack's walk was refused, only a walk made here is taken)
    own = _function(text, "static void reach_walk_own(void)\n{")
    assert "reach_own_asked = 1;" in own
    take = _function(text, "static int reach_kept_take(uint64_t hash, int own)")
    assert "(own && !reach_kept[index].own)" in take
    assert "reach_parse(reach_kept[index].bytes, reach_kept[index].size, hash)" in take
    # (bounded: a few walks, none past its size)
    assert re.search(r"#define REACH_KEPT_MOST \d+\n", text)
    put = _function(text, "static void reach_kept_put(uint64_t hash, int own)")
    assert "size > REACH_KEPT_BYTES_MOST" in put
