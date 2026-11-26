"""PRISM world geometry: does the remodel build to the same file every time? A shipped light bake of the remodel holds
only where the player's own build is the farm's (the bake's geometry hash), so the build is run several times, with
other thread counts and hash seeds, and the .pwg files compared (wg_pwg_hash.py). Run it on each platform a release
builds the remodel on and compare the printed digests across them.

usage: python3 wg_determinism.py --blender <blender> --gltf <bsp.gltf> --extract <the level's extract folder>
           --out <folder> [--reach <bsp<n>.reach>] [--runs 3] [--first 0] [--steps remodel|all] [--gpu NONE]
  Each run's files go to <out>/run<k>; the digests are printed as JSON, one line a run, then "SAME" (exit 0),
  "GEOMETRY SAME" (the drawn fields alone differ: exit 1) or "DIFFERENT" (exit 2).

The runs' environments: prism_packs' deterministic_env (one BLAS thread, numpy's kernels at its baseline) twice, then
the same with 8 BLAS threads (or, with --unpinned, with numpy's CPU dispatch left on: what another processor would
run), each with another PYTHONHASHSEED (Blender's Python ignores it; the scripts must not depend on it either way).
"""
import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent.parent))
import wg_pwg_hash  # noqa: E402

try:
    from prism_packs import deterministic_env  # noqa: E402
except Exception:  # (run from a copy without the pack tool)
    def deterministic_env(blender=True):
        return {"OMP_NUM_THREADS": "1", "OPENBLAS_NUM_THREADS": "1", "MKL_NUM_THREADS": "1"}


def blender(program, script, *args, env=None):
    command = [program, "-b", "--factory-startup", "--python-exit-code", "1", "--python-expr",
               "import sys; sys.dont_write_bytecode = True", "--python", str(HERE / script), "--", *map(str, args)]
    return subprocess.run(command, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--blender", required=True)
    p.add_argument("--gltf", required=True)
    p.add_argument("--extract", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--reach")
    p.add_argument("--runs", type=int, default=3)
    p.add_argument("--first", type=int, default=0, help="the first run's number (runs side by side: one each)")
    p.add_argument("--steps", choices=("remodel", "all"), default="all")
    p.add_argument("--gpu", default="NONE")
    p.add_argument("--unpinned", action="store_true",
                   help="the third run with numpy's CPU dispatch on (what another processor would run), not 8 threads")
    a = p.parse_args()
    name = Path(a.gltf).stem
    variants = [dict(deterministic_env(), PYTHONHASHSEED="0"),
                dict(deterministic_env(), PYTHONHASHSEED="1"),
                dict(deterministic_env(), OMP_NUM_THREADS="8", OPENBLAS_NUM_THREADS="8", PYTHONHASHSEED="2")]
    if a.unpinned:
        variants[2] = dict(deterministic_env(), PYTHONHASHSEED="2")
        variants[2].pop("NPY_DISABLE_CPU_FEATURES", None)
    finals = []
    for k in range(a.first, a.first + a.runs):
        out = Path(a.out) / f"run{k}"
        out.mkdir(parents=True, exist_ok=True)
        env = dict(os.environ, WG_GPU=a.gpu, **variants[k % len(variants)])
        if a.reach:
            env["WG_REACH"] = a.reach
        started = time.time()
        r = blender(a.blender, "wg2_remodel.py", a.gltf, a.extract, HERE / "wg2_recipes.json",
                    out / f"{name}.remodel.pwg", out / f"{name}.report.json", env=env)
        (out / "remodel.log").write_text(r.stdout)
        final = out / f"{name}.remodel.pwg"
        if r.returncode == 0 and a.steps == "all":
            r = blender(a.blender, "wg2_seal.py", a.gltf, final, out / f"{name}.sealed.pwg", out / f"{name}.seal.json",
                        env=env)
            (out / "seal.log").write_text(r.stdout)
            final = out / f"{name}.sealed.pwg"
            if r.returncode == 0:
                r = blender(a.blender, "wg2_far2.py", final, out / f"{name}.pwg", out / f"{name}.far2.json", env=env)
                (out / "far2.log").write_text(r.stdout)
                final = out / f"{name}.pwg"
        if r.returncode:
            print(json.dumps({"run": k, "failed": r.returncode, "log": str(out)}))
            return 3
        d = wg_pwg_hash.digests(str(final))
        d.update(run=k, seconds=round(time.time() - started, 1),
                 env={key: variants[k % len(variants)][key] for key in sorted(variants[k % len(variants)])})
        print(json.dumps(d), flush=True)
        finals.append(final)
    worst = 0
    if a.first:
        for k in range(a.first):
            prior = Path(a.out) / f"run{k}" / finals[0].name
            if prior.is_file():
                finals.insert(k, prior)
    for f in finals[1:]:
        code, what = wg_pwg_hash.compare(str(finals[0]), str(f))
        if code:
            print(f"{f.parent.name} against {finals[0].parent.name}: {what}")
        worst = max(worst, code)
    print(["SAME", "GEOMETRY SAME", "DIFFERENT"][worst])
    return worst


if __name__ == "__main__":
    sys.exit(main())
