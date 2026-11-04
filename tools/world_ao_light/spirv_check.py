"""spirv_check.py <scene.json> <model.spv> <lightmap.spv> <tolerance>: the SPIR-V emitter's display.ambient_occlusion
"world" shaders, as debug.world_ao_view writes them (the pixel's colour is the occlusion), run on the CPU
(spirv_run.py) at the made-up scene's points (scene.c) and compared with port/linux/game/world_ao_shading.c's
occlusion there. Prints the largest difference; exits 1 if it is past the tolerance."""
import json
import math
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import spirv_run  # noqa: E402

U_WORLD_AO = 27  # (xgpu_spirv.c's member: past the other 27)


def main():
    scene = json.loads(Path(sys.argv[1]).read_text())
    modules = [spirv_run.Module(_words(Path(path))) for path in sys.argv[2:4]]
    tolerance = float(sys.argv[4])
    width, height, depth = scene["size"]
    voxels = scene["voxels"]
    origin, voxel, range_ = scene["origin"], scene["voxel"], scene["range"]
    parameters = scene["parameters"]
    proxies = scene["proxies"]
    block = {0: [[origin[0], origin[1], origin[2], 1.0 / voxel], [float(width), float(height), float(depth), range_]],
             1: [parameters[0:4], parameters[4:8]],
             2: [proxies[index:index + 4] for index in range(0, len(proxies), 4)],
             3: [scene["grid"][0:4], scene["grid"][4:8]],
             4: [scene["cells"][index:index + 4] for index in range(0, len(scene["cells"]), 4)]}

    def sample(name, uvw):
        if name != "world_sdf":
            return [1.0, 1.0, 1.0, 1.0]  # (the surface's own texture: white)
        size = (width, height, depth)
        base, fraction = [], []
        for axis in range(3):
            coordinate = uvw[axis] * size[axis] - 0.5
            floor_value = math.floor(coordinate)
            base.append(int(floor_value))
            fraction.append(coordinate - floor_value)
        result = 0.0
        for corner in range(8):
            at, weight = [], 1.0
            for axis in range(3):
                bit = (corner >> axis) & 1
                at.append(min(max(base[axis] + bit, 0), size[axis] - 1))
                weight *= fraction[axis] if bit else 1.0 - fraction[axis]
            result += weight * max(voxels[(at[2] * height + at[1]) * width + at[0]] / 127.0, -1.0)
        return [spirv_run.f32(result), 0.0, 0.0, 1.0]

    worst = 0.0
    for case in scene["cases"]:
        inputs = {"u": {U_WORLD_AO: [case["owner"], case["reference"], 0.0, 0.0]}, "world_ao": block,
                  "xWorldPosition": case["point"], "xWorldNormal": case["normal"] + [1.0]}
        colour = spirv_run.run(modules[case["lightmap"]], inputs, sample)
        difference = abs(colour[0] - case["occlusion"])
        if difference > worst:
            worst = difference
            if difference > tolerance:
                print(f"at {case}: {colour[0]}")
    print(f"{len(scene['cases'])} points, largest difference {worst:.3g}")
    return 1 if worst > tolerance else 0


def _words(path):
    data = path.read_bytes()
    return [int.from_bytes(data[at:at + 4], "little") for at in range(0, len(data), 4)]


if __name__ == "__main__":
    sys.exit(main())
