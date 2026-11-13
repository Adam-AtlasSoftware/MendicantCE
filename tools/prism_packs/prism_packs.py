#!/usr/bin/env python3
"""PRISM's packs, built on the player's own computer from the player's own game data.

    python prism_packs.py check   [options] [pack ...]      what the packs need, and what this computer lacks
    python prism_packs.py models  [--upscalers <folder>] [--yes]
                                                            fetch the texture packs' upscalers (licences shown)
    python prism_packs.py list                              the packs, what each gives and needs
    python prism_packs.py build   [options] [pack ...]      build them (all by default); again, to go on
    python prism_packs.py status  [options]                 which are built

A pack is a name below, "characters" (every character and texture pack), "levels" (the level packs of
--levels), or "all". A pack's steps are done in order and each is remembered as done in the work folder: a
build stopped part way (or that failed) goes on from the step it stopped at when it is started again;
--again <pack> builds a pack anew.

Options:
  --data <folder>       the game's data folder, the one with maps/ (required: where the game found the maps
                        it extracted from your disc image at its first start)
  --out <folder>        where the packs go, laid out as the game reads them (default: --data): mods/<pack>,
                        prism_world/, prism_lightmaps/, prism/probes/
  --work <folder>       the work folder (default: <data>/prism_packs_work): the extract, the game's model
                        dumps and every step's files. Several GB; delete it when the packs are built
  --levels a10,b30,...  the levels of the level packs (default: the ones the recipes cover)
  --blender <program>   Blender 4.5 (default: blender on the PATH)
  --invader <folder>    the folder of Invader's programs (default: the folder of invader-model on the PATH)
  --extractor <program> the extractor (default: the game's bin/openh1-map-extract, or a source checkout's
                        build/map_extract/map_extract, built if missing)
  --game <program>      the game (default: the one these tools came with), for its model dumps and probes
  --upscalers <folder>  the upscalers (default: <work>/upscalers; prism_packs.py models fetches them)
  --gpu <name part>     the graphics card Cycles renders on, by part of its name (default: any it finds)
  --vulkan-gpu <n>      the graphics card the Vulkan upscaler runs on, by number (default: its own choice)
  --torch <device>      the PyTorch device of the upscalers (default: cuda, else cpu)
  --lightmap-samples N  the rebaked lightmaps' final samples a texel (default: 1024)

Everything these tools make is made from your copy of the game: it stays on your computer. Do not share,
upload or publish the packs, the work folder or anything in them. docs/prism_packs.md (GUIDE.md beside this
file in a download) says what each pack needs and how to turn it on.
"""

import argparse
import glob
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ART = HERE / "art"
# (no bytecode beside the tools: in a download they are in the game's own folder)
sys.dont_write_bytecode = True
sys.path.insert(0, str(HERE))
import packs_fetch  # noqa: E402
import packs_game  # noqa: E402

VERSION = 1
PYTHON = sys.executable
EXE = ".exe" if os.name == "nt" else ""
# the levels the recipes cover (world_geo/wg2_recipes.json), in the play order
LEVELS = ["a10", "a30", "a50", "b30", "c10"]
INVADER_PROGRAMS = ["invader-extract", "invader-bitmap", "invader-model", "invader-edit", "invader-convert"]
NOT_YET_READ = "this build of the game does not read them yet (display.prism_baked_shadows is in a later one)"


# ---------------------------------------------------------------- the packs


class Pack:
    def __init__(self, name, gives, needs=(), uses=(), maps=(), level=False, internal=False, mods=(), off=False):
        self.name, self.gives, self.needs, self.uses = name, gives, list(needs), set(uses)
        self.maps, self.level, self.internal, self.mods, self.off = list(maps), level, internal, list(mods), off


# uses: blender (Blender), cycles (a Blender render: a graphics card makes it bearable), invader, game,
# upscalers:<pack> (packs_fetch.NEEDED_BY), torch (PyTorch, scipy, scikit-image, opencv)
PACKS = [
    Pack("extract", "the maps' bitmaps, models and level geometry, and their light sources (work folder only)",
         internal=True),
    Pack("dumps", "the game's own models as it draws them (work folder only)", uses={"game"}, internal=True,
         maps=["b30", "a30", "a50", "c10"]),
    Pack("class_masks", "the material overlay's per-texel material classes on the Chief, the first-person "
                        "arms, the Elites, Jackals and Marines", needs=["extract"], uses={"science"},
         maps=["b30", "a50"], mods=["prism_class_masks"]),
    Pack("materials", "fine surface grain (micro normal, cavity, roughness) on the characters and first-person "
                      "weapons: MATERIAL DETAIL", needs=["extract"], maps=["b30", "a50"], mods=["prism_materials"]),
    Pack("base_maps", "the characters' and first-person weapons' colour maps upscaled 2x or 4x (Real-ESRGAN)",
         uses={"invader", "upscalers:base_maps"}, mods=["prism_base_local"]),
    Pack("chief_r2", "the third-person Chief remodelled (round 2: crisp plates, cut vents), as a model tag; "
                     "and his surface map", needs=["extract"], uses={"blender", "cycles", "invader", "science"},
         maps=["b30"], mods=["prism_v2r2_chief_3p_model"]),
    Pack("proto", "the Chief's and the first-person Assault Rifle's 4K maps (work folder only)",
         needs=["extract"], uses={"torch", "upscalers:textures"}, maps=["b30"], internal=True),
    Pack("chief_maps", "the Chief's colour and multipurpose maps at 4K, the visor and lamps fixed, with his "
                       "surface map", needs=["proto", "chief_r2"], uses={"invader", "science"},
         mods=["prism_v2r2_chief_3p_maps"]),
    Pack("ar_textures", "the first-person Assault Rifle's colour and multipurpose maps at 4K", needs=["proto"],
         uses={"invader"}, mods=["prism_v2r2_ar_textures"]),
    Pack("tiers", "the Enhanced and Max texture tiers of every character and first-person weapon (work folder "
                  "only: Bold and the round 3 Chief start from them)",
         needs=["extract", "class_masks", "chief_maps"], uses={"torch", "invader", "upscalers:textures"},
         maps=["b30"], internal=True),
    Pack("fp_arms", "the first-person arms and Assault Rifle remodelled (round 2), with their surface maps",
         needs=["extract"], uses={"blender", "cycles", "invader", "science"}, maps=["b30"],
         mods=["prism_v2r2_fp_hands_ar"]),
    Pack("elite_r2", "the Elite and the special Elite remodelled (round 2), with their surface maps",
         needs=["extract"], uses={"blender", "cycles", "invader", "science"}, maps=["b30"],
         mods=["prism_v2_elite_r2"]),
    Pack("bold", "Bold textures: crisper material definition on the Chief, the arms and rifle, the Elites and "
                 "the Marines, with surface maps", needs=["tiers", "chief_maps", "fp_arms", "elite_r2"],
         uses={"torch", "invader"}, mods=["prism_v2_tex_bold"]),
    Pack("chief_r3", "the round 3 Chief for the mesh store (remodelled gloves, pouches, lamps, helper bones) "
                     "and his PBR materials", needs=["chief_r2", "tiers", "dumps"],
         uses={"blender", "cycles", "science"}, mods=["prism_r3_chief_meshes", "prism_r3_chief_materials"]),
    Pack("cast", "the armoured Marine, the Grunt and the Jackal remodelled for the mesh store; and the Elites "
                 "stitched (prism_v3_elite_meshes, left off)", needs=["elite_r2", "dumps"],
         uses={"blender"}, mods=["prism_v3_cast", "prism_v3_elite_meshes"]),
    Pack("rocks", "the scenery rocks and boulders remodelled for the mesh store", needs=["dumps", "extract"],
         maps=["b30", "a30", "a50", "c10"], mods=["prism_v3_rocks"]),
    Pack("world", "each level's remodelled geometry (prism_world/levels/<level>/<bsp>.pwg)", needs=["extract"],
         uses={"blender", "cycles"}, level=True),
    Pack("lightmaps", "each level's lightmaps baked again from its own lamps and sky "
                      "(prism_lightmaps/rebaked/*.prl)", needs=["extract"], uses={"blender", "cycles"}, level=True),
    Pack("sun", "sharp baked sun shadows (prism_lightmaps/sun/*.psm; " + NOT_YET_READ + ")",
         needs=["lightmaps", "world"], uses={"blender", "cycles"}, level=True, off=True),
    Pack("probes", "local reflections: each level's reflection probes (prism/probes/<level>.probes)",
         uses={"game"}, level=True, off=True),
]
BY_NAME = {p.name: p for p in PACKS}
CHARACTERS = ["class_masks", "materials", "base_maps", "chief_r2", "chief_maps", "ar_textures", "fp_arms",
              "elite_r2", "bold", "chief_r3", "cast", "rocks"]
LEVEL_PACKS = ["world", "lightmaps"]
# the load order the packs play in (each over those before it), as the author's kit has it
LOAD_ORDER = ["prism_base_local", "prism_class_masks", "prism_materials", "prism_v2r2_ar_textures",
              "prism_v2r2_chief_3p_maps", "prism_v2r2_fp_hands_ar", "prism_v2r2_chief_3p_model",
              "prism_r3_chief_meshes", "prism_r3_chief_materials", "prism_v2_elite_r2", "prism_v3_elite_meshes",
              "prism_v3_cast", "prism_v3_rocks", "prism_v2_tex_bold"]
OFF_BY_DEFAULT = ["prism_v3_elite_meshes"]
LOCAL_ONLY = "Built on this computer from the player's own Halo maps. Never commit, share, upload or ship it.\n"


class Failure(Exception):
    pass


# ---------------------------------------------------------------- the run's context


class Context:
    def __init__(self, a):
        self.a = a
        self.data = Path(a.data).resolve()
        self.out = Path(a.out).resolve() if a.out else self.data
        self.work = Path(a.work).resolve() if a.work else self.data / "prism_packs_work"
        self.upscalers = Path(a.upscalers).resolve() if a.upscalers else self.work / "upscalers"
        self.levels = [x for x in (a.levels.split(",") if a.levels else LEVELS) if x]
        self.blender = a.blender or shutil.which("blender") or ""
        self.invader = a.invader or (str(Path(shutil.which("invader-model")).parent)
                                     if shutil.which("invader-model") else "")
        self.extractor = a.extractor or default_extractor()
        self.game = a.game or default_game()
        self.tools = default_tools()
        self.log = None
        self.step = ""

    # the environment of every art/ script (art/packs_env.py)
    def env(self, extra=None):
        env = dict(os.environ)
        env.update({"PRISM_PACKS_WORK": str(self.work), "PRISM_PACKS_DATA": str(self.data),
                    "PRISM_PACKS_OUT": str(self.out), "PRISM_PACKS_EXTRACTOR": self.extractor or "",
                    "PRISM_PACKS_INVADER": self.invader or "", "PRISM_PACKS_UPSCALERS": str(self.upscalers),
                    "PRISM_PACKS_TOOLS": str(self.tools), "PRISM_PACKS_GPU": self.a.gpu or "",
                    "PRISM_PACKS_VULKAN_GPU": self.a.vulkan_gpu or "", "PRISM_PACKS_TORCH": self.a.torch or "",
                    "PYTHONDONTWRITEBYTECODE": "1"})
        tmp = self.work / "tmp"
        tmp.mkdir(parents=True, exist_ok=True)
        for name in ("TMPDIR", "TEMP", "TMP"):
            env[name] = str(tmp)
        if not self.a.torch:
            env["PRISM_PACKS_TORCH"] = torch_device()
        env.update(extra or {})
        return env

    def run(self, command, what="", cwd=None, extra=None, check=True):
        """a program, its output in the step's log; a failure stops the step with the log's last lines"""
        command = [str(c) for c in command]
        if what:
            print(f"    {what}", flush=True)
        self.log.write(f"\n$ {' '.join(command)}\n")
        self.log.flush()
        started = time.time()
        result = subprocess.run(command, cwd=str(cwd) if cwd else None, env=self.env(extra), stdout=self.log,
                                stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
        self.log.write(f"(exit {result.returncode}, {time.time() - started:.0f} s)\n")
        self.log.flush()
        if check and result.returncode:
            raise Failure(f"{Path(command[0]).name} {' '.join(command[1:3])} ... failed (exit {result.returncode})")
        return result.returncode

    def output(self, command):
        """a program's standard output (Invader's answers), its errors in the log"""
        result = subprocess.run([str(c) for c in command], env=self.env(invader_env(self.invader)),
                                capture_output=True, text=True)
        self.log.write(result.stderr)
        if result.returncode:
            raise Failure(f"{Path(command[0]).name} failed: {result.stderr.strip()[-300:]}")
        return result.stdout.strip()

    def python(self, script, *args, what="", check=True):
        return self.run([PYTHON, "-B", "-I", ART / script, *args], what=what, check=check)

    def blender_run(self, script, *args, what=""):
        # (no bytecode beside the art scripts: Blender's Python reads no PYTHON* variable)
        return self.run([self.blender, "-b", "--factory-startup", "--python-exit-code", "1", "--python-expr",
                         "import sys; sys.dont_write_bytecode = True", "--python", ART / script, "--", *args],
                        what=what)

    def inv(self, program):
        return str(Path(self.invader) / (program + EXE))

    def invader_run(self, program, *args, what="", check=True):
        return self.run([self.inv(program), *args], what=what, extra=invader_env(self.invader), check=check)

    def extract_run(self, *args, what=""):
        return self.run([*extractor_command(self.extractor), *[wsl_path(a) for a in args]], what=what)

    def w(self, *parts):
        return self.work.joinpath(*parts)

    def mod(self, mod_id):
        return self.out / "mods" / mod_id

    def maps_dir(self):
        return self.data / "maps"

    def map_names(self):
        return sorted(p.stem for p in self.maps_dir().glob("*.map") if p.stem != "ui")

    # steps inside a pack remembered as done (a long pack goes on where it stopped)
    def once(self, key, function):
        stamp = self.w("state", self.step, key.replace("/", "_").replace("\\", "_") + ".done")
        if stamp.is_file():
            print(f"    {key}: done before", flush=True)
            return
        function()
        stamp.parent.mkdir(parents=True, exist_ok=True)
        stamp.write_text(time.strftime("%Y-%m-%d %H:%M:%S\n"))


def invader_env(folder):
    lib = Path(folder).parent / "lib" if folder else None
    if lib and lib.is_dir():
        return {"LD_LIBRARY_PATH": str(lib) + os.pathsep + os.environ.get("LD_LIBRARY_PATH", "")}
    return {}


def torch_device():
    import warnings
    try:
        with warnings.catch_warnings():
            warnings.simplefilter("ignore")
            import torch
            return "cuda" if torch.cuda.is_available() else "cpu"
    except ImportError:
        return "cpu"


def default_tools():
    """the game's own pack format tools (pmesh.py, prism_rig.py, prism_material_pack.py, texstore_synth.py): a
    source checkout's tools/, or formats/ beside this file in a download"""
    return HERE.parent if (HERE.parent / "pmesh.py").is_file() else HERE / "formats"


def default_extractor():
    candidates = [HERE.parent.parent.parent / "bin" / "openh1-map-extract",   # the Linux download: share/openh1/
                  HERE / "openh1-map-extract",                               # (the Windows download, beside it)
                  HERE.parent.parent / "build" / "map_extract" / "map_extract"]  # a source checkout
    for candidate in candidates:
        if candidate.is_file():
            return str(candidate)
    if (HERE.parent / "x86_game_abi.py").is_file() and sys.platform.startswith("linux"):
        return str(candidates[-1])  # (built by check/build from the source checkout)
    return ""


def default_game():
    candidates = [HERE.parent.parent.parent / "bin" / "openh1", HERE.parent / ("halo" + EXE),
                  HERE.parent.parent / "build" / "linux_x86_64" / "halo"]
    for candidate in candidates:
        if candidate.is_file():
            return str(candidate)
    return ""


def wsl_path(argument):
    """on Windows, a path argument of the extractor (a Linux program, run through WSL) as WSL sees it"""
    argument = str(argument)
    if os.name != "nt" or not (len(argument) > 2 and argument[1] == ":"):
        return argument
    return subprocess.run(["wsl.exe", "wslpath", "-a", argument], capture_output=True, text=True,
                          check=True).stdout.strip()


def extractor_command(extractor):
    if os.name == "nt":
        return ["wsl.exe", "-e", wsl_path(extractor)]
    return [extractor]


# ---------------------------------------------------------------- what this computer has


def check(ctx, packs, quiet=False):
    """what the packs need that this computer lacks: a list of lines, empty when nothing"""
    uses = set().union(*(BY_NAME[p].uses for p in packs)) if packs else set()
    problems = []
    if not ctx.maps_dir().is_dir() or not ctx.map_names():
        problems.append(f"maps: no maps in {ctx.maps_dir()} (--data is the folder with the game's maps/: the one "
                        "the game extracted from your disc image at its first start)")
    else:
        maps = set(ctx.map_names())
        wanted = set()
        for p in packs:
            wanted |= set(BY_NAME[p].maps)
            if BY_NAME[p].level:
                wanted |= set(ctx.levels)
        missing = sorted(wanted - maps)
        if missing:
            problems.append(f"maps: {', '.join(m + '.map' for m in missing)} not in {ctx.maps_dir()}")
        for m in sorted(wanted & maps):
            with open(ctx.maps_dir() / f"{m}.map", "rb") as f:
                head = f.read(8)
            if head[:4] not in (b"daeh", b"head"):
                problems.append(f"maps: {m}.map is not a map file")
    if sys.version_info < (3, 10):
        problems.append(f"python: {sys.version.split()[0]}; the tools need Python 3.10 or newer")
    for module, pip in (("numpy", "numpy"), ("PIL", "pillow")):
        if not importable(module):
            problems.append(f"python: no {module} (pip install {pip})")
    if uses & {"science", "torch"}:
        for module, pip in (("scipy", "scipy"), ("cv2", "opencv-python-headless"), ("skimage", "scikit-image")):
            if not importable(module):
                problems.append(f"python: no {module} (pip install {pip})")
    if "torch" in uses:
        for module, pip in (("torch", "torch (pytorch.org says which build suits your graphics card)"),
                            ("spandrel", "spandrel")):
            if not importable(module):
                problems.append(f"python: no {module} (pip install {pip})")
    if any(p in ("extract",) or "extract" in BY_NAME[p].needs or p == "base_maps" or BY_NAME[p].level
           for p in packs):
        if not ctx.extractor:
            problems.append("extractor: none found (--extractor; the Linux download has bin/openh1-map-extract; on "
                            "Windows it runs through WSL: docs/prism_packs.md)")
        elif not Path(ctx.extractor).is_file() and not (HERE.parent / "x86_game_abi.py").is_file():
            problems.append(f"extractor: {ctx.extractor} is not there")
        elif os.name == "nt" and not shutil.which("wsl.exe"):
            problems.append("extractor: it is a Linux program, run through WSL, and this Windows has no WSL "
                            "(wsl --install): docs/prism_packs.md")
    if "blender" in uses:
        version = blender_version(ctx.blender)
        if not version:
            problems.append("blender: not found (--blender <program>; Blender 4.5 LTS from blender.org)")
        elif not version.startswith("4.5"):
            problems.append(f"blender: {ctx.blender} is {version}; the tools are made for 4.5 LTS (another may "
                            "build different shapes or fail)")
    if "invader" in uses:
        lacking = [p for p in INVADER_PROGRAMS if not ctx.invader or not (Path(ctx.invader) / (p + EXE)).is_file()]
        if lacking:
            problems.append(f"invader: {', '.join(lacking)} not found (--invader <folder>; Invader 0.55 or newer, "
                            "https://github.com/SnowyMouse/invader)")
    if "game" in uses and (not ctx.game or not Path(ctx.game).is_file()):
        problems.append("game: not found (--game <program>: bin/openh1, or halo.exe on Windows)")
    for use in uses:
        if use.startswith("upscalers:"):
            lacking = [n for n in packs_fetch.NEEDED_BY[use.split(":")[1]] if not packs_fetch.present(ctx.upscalers, n)]
            if lacking:
                problems.append(f"upscalers: {', '.join(lacking)} not in {ctx.upscalers} (prism_packs.py models "
                                "fetches them, each after its licence is shown)")
    if "cycles" in uses and not quiet and ctx.blender and blender_version(ctx.blender):
        cards = cycles_cards(ctx)
        if not cards:
            print("note: Blender finds no graphics card for Cycles (OptiX, CUDA, HIP, oneAPI or Metal): its bakes "
                  "will run on the processor, many times slower", flush=True)
        else:
            print(f"Cycles: {', '.join(cards)}", flush=True)
    return sorted(set(problems))


def importable(module):
    try:
        __import__(module)
        return True
    except ImportError:
        return False


def blender_version(blender):
    if not blender:
        return ""
    try:
        result = subprocess.run([blender, "--version"], capture_output=True, text=True, timeout=120)
    except (OSError, subprocess.SubprocessError):
        return ""
    for line in result.stdout.splitlines():
        if line.startswith("Blender "):
            return line.split()[1]
    return ""


def cycles_cards(ctx):
    probe = "\n".join([
        "import bpy, sys", "sys.dont_write_bytecode = True", f"sys.path.insert(0, {str(ART)!r})",
        "import packs_env as E",
        "prefs = bpy.context.preferences.addons['cycles'].preferences",
        "backend, cards = E.cycles_gpu(prefs)", "print('CARDS', backend, '|'.join(cards))"])
    script = ctx.w("tmp", "cycles_probe.py")
    script.parent.mkdir(parents=True, exist_ok=True)
    script.write_text(probe)
    try:
        result = subprocess.run([ctx.blender, "-b", "--factory-startup", "--python", str(script)],
                                capture_output=True, text=True, timeout=300, env=ctx.env())
    except (OSError, subprocess.SubprocessError):
        return []
    for line in result.stdout.splitlines():
        if line.startswith("CARDS "):
            _, backend, cards = (line.split(" ", 2) + [""])[:3]
            return [f"{c} ({backend})" for c in cards.split("|") if c] if backend != "None" else []
    return []


def ensure_extractor(ctx):
    """a source checkout's extractor, built if missing (tools/x86_game_abi.py --map-extract)"""
    if ctx.extractor and Path(ctx.extractor).is_file():
        return
    abi = HERE.parent / "x86_game_abi.py"
    if not abi.is_file():
        raise Failure("no extractor (--extractor)")
    print("    building the extractor (tools/x86_game_abi.py --map-extract)", flush=True)
    folder = Path(ctx.extractor).parent
    ctx.run([PYTHON, abi, "--map-extract", folder])


# ---------------------------------------------------------------- shared pieces of the packs


def write_mod(folder, mod_id, name, version, description):
    folder.mkdir(parents=True, exist_ok=True)
    (folder / "mod.toml").write_text(f'name = "{name}"\nid = "{mod_id}"\nversion = "{version}"\n'
                                     f'description = "{description}"\nformat = 1\n', encoding="utf-8")
    (folder / "LOCAL_ONLY.txt").write_text(LOCAL_ONLY)


def extract_map(ctx, name):
    """the map's extract (bitmaps, models, bsps) and its light sources, once"""
    if (ctx.w("extract", name, "index.json").is_file() and ctx.w("extract", f"{name}_lights.json").is_file()):
        return
    ensure_extractor(ctx)
    ctx.extract_run("--data", ctx.data, "--out", ctx.w("extract"), name, what=f"extract {name}")
    ctx.extract_run("lights", "--data", ctx.data, "--out", ctx.w("extract", f"{name}_lights.json"), name,
                    what=f"light sources of {name}")


def ce_shaders(src_root, model_dir):
    """the shader tags beside a model in invader-extract's output"""
    return sorted(Path(src_root, model_dir, "shaders").glob("*.shader_*"))


def check_model_on_maps(ctx, tag, model, kind, work):
    """a mod model tag against every map's own (mod_model_check.py, as mod_models.c checks it)"""
    passed, failed = 0, []
    (work / "checks").mkdir(parents=True, exist_ok=True)
    for m in ctx.map_names():
        ref_root = work / "permap" / m
        ref = ref_root / f"{model}.{kind}"
        none = ref_root / f"{model}.none"
        if not ref.is_file() and not none.is_file():
            ref_root.mkdir(parents=True, exist_ok=True)
            if ctx.invader_run("invader-extract", "-m", ctx.maps_dir(), "-t", ref_root, "-s", f"{model}.{kind}",
                               ctx.maps_dir() / f"{m}.map", check=False) or not ref.is_file():
                none.parent.mkdir(parents=True, exist_ok=True)
                none.write_text("")
                continue
        if none.is_file():
            continue
        if ctx.python("mod_model_check.py", tag, ref, "--out", work / "checks" / f"{Path(model).name}_{m}.json",
                      check=False):
            failed.append(m)
        else:
            passed += 1
    if failed:
        raise Failure(f"{model}: the model is refused on {', '.join(failed)} (mod_model_check.py; {work / 'checks'})")
    print(f"    {model}: checked against {passed} maps' own model", flush=True)


def check_bitmap_on_maps(ctx, tag, bitmap, work):
    passed, failed = 0, []
    for m in ctx.map_names():
        ref_root = work / "ref" / m
        ref = ref_root / f"{bitmap}.bitmap"
        if not ref.is_file():
            ref_root.mkdir(parents=True, exist_ok=True)
            ctx.invader_run("invader-extract", "-m", ctx.maps_dir(), "-t", ref_root, "-s", f"{bitmap}.bitmap",
                            ctx.maps_dir() / f"{m}.map", check=False)
        if not ref.is_file():
            continue
        if ctx.python("mod_bitmap_check.py", tag, ref, check=False):
            failed.append(m)
        else:
            passed += 1
    if failed:
        raise Failure(f"{bitmap}: the bitmap is refused on {', '.join(failed)} (mod_bitmap_check.py)")
    print(f"    {bitmap}: checked against {passed} maps' own bitmap", flush=True)


def copy_node_values(ctx, src_tags, src_name, tags, name, fields=True):
    """the map model's node distances (and LOD cutoffs and base map scale) into the new gbxmodel, where
    mod_models.c compares them"""
    count = int(ctx.output([ctx.inv("invader-edit"), "-t", tags, "-C", "nodes", name]))
    for i in range(count):
        value = ctx.output([ctx.inv("invader-edit"), "-t", src_tags, "-G", f"nodes[{i}].node_distance_from_parent",
                            src_name])
        ctx.output([ctx.inv("invader-edit"), "-n", "-t", tags, "-S", f"nodes[{i}].node_distance_from_parent",
                    value, name])
    if fields:
        for field in ("super_high_detail_cutoff", "high_detail_cutoff", "medium_detail_cutoff", "low_detail_cutoff",
                      "super_low_detail_cutoff", "base_map_u_scale", "base_map_v_scale"):
            try:
                value = ctx.output([ctx.inv("invader-edit"), "-t", src_tags, "-G", field, src_name])
            except Failure:
                continue
            ctx.output([ctx.inv("invader-edit"), "-n", "-t", tags, "-S", field, value, name])


def strip_cull(ctx, tag, ref):
    """triangles joining bones the map's mesh never joins, dropped from the strips (prism_strip_cull.py)"""
    culled = tag.with_name(tag.name + ".culled")
    ctx.python("prism_strip_cull.py", tag, ref, culled)
    culled.replace(tag)


def to_pc_multipurpose(src, dst):
    """a multipurpose map in the Xbox's channel order (R specular, G self-illumination, B change colour, A
    auxiliary) as a Halo PC tag reads it (R auxiliary, G self-illumination, B specular, A change colour);
    pure blue (Invader's colour-plate background) nudged to 254"""
    import numpy as np
    from PIL import Image
    x = np.asarray(Image.open(src).convert("RGBA")).copy()
    pc = np.stack([x[..., 3], x[..., 1], x[..., 0], x[..., 2]], -1)
    blue = (pc[..., 0] == 0) & (pc[..., 1] == 0) & (pc[..., 2] == 255)
    pc[blue, 2] = 254
    Path(dst).parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(pc, "RGBA").save(dst)


def texture_mod(ctx, work, mod, mod_id, name, version, description, maps, surfaces=None):
    """bitmap tags of a base map (DXT1) and its multipurpose map (DXT5, PC order), checked against every map's
    own, as a mod; surfaces: {bitmap tag path: surface PNG} copied next to the tags"""
    shutil.rmtree(work / "data", ignore_errors=True)
    shutil.rmtree(work / "tags", ignore_errors=True)
    (work / "tags").mkdir(parents=True)
    for bitmap, (png, kind) in maps.items():
        dst = work / "data" / f"{bitmap}.png"
        dst.parent.mkdir(parents=True, exist_ok=True)
        if kind == "multi":
            to_pc_multipurpose(png, dst)
        else:
            shutil.copyfile(png, dst)
        ctx.invader_run("invader-bitmap", "-d", work / "data", "-t", work / "tags", "-T", "2d_textures", "-F",
                        "dxt5" if kind == "multi" else "dxt1", "-u", "default", bitmap, what=f"invader-bitmap {bitmap}")
        check_bitmap_on_maps(ctx, work / "tags" / f"{bitmap}.bitmap", bitmap, work)
    shutil.rmtree(mod, ignore_errors=True)
    for bitmap in maps:
        dst = mod / "tags" / f"{bitmap}.bitmap"
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(work / "tags" / f"{bitmap}.bitmap", dst)
    for bitmap, png in (surfaces or {}).items():
        shutil.copyfile(png, mod / "tags" / f"{bitmap}.surface.png")
    write_mod(mod, mod_id, name, version, description)


def bake_ce(ctx, blend, out, game, high, shaders, size, samples, cage, ray, ao, *extra):
    ctx.blender_run("prism_bake_ce.py", blend, out, "--game", game, "--high", high, "--shaders", shaders,
                    "--size", size, "--samples", samples, "--cage-mm", cage, "--ray-mm", ray, "--ao-mm", ao, *extra,
                    *(["--gpu", ctx.a.gpu] if ctx.a.gpu else []), what=f"bake {Path(out).name} (Cycles)")


def recipe_copy(ctx, src, name=None):
    """a recipe as the step uses it: its {work}, {extract} and {art} paths filled in, in the work folder"""
    text = Path(src).read_text(encoding="utf-8")
    for key, value in (("{work}", ctx.work), ("{extract}", ctx.w("extract")), ("{art}", ART)):
        text = text.replace(key, json.dumps(str(value))[1:-1])
    out = ctx.w("recipes", name or Path(src).name)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(text, encoding="utf-8")
    return out


def load_order(ctx):
    """mods/load_order.toml: the packs' mods in the kit's order after the player's own, the stitched Elites off
    the first time they come"""
    mods = ctx.out / "mods"
    path = mods / "load_order.toml"
    order, disabled = [], []
    if path.is_file():
        for line in path.read_text(encoding="utf-8").splitlines():
            key, _, value = line.partition("=")
            if key.strip() in ("order", "disabled"):
                try:
                    values = json.loads(value.strip())
                except ValueError:
                    continue
                (order if key.strip() == "order" else disabled).extend(values)
    present = [m for m in LOAD_ORDER if (mods / m / "mod.toml").is_file()]
    new = [m for m in present if m not in order]
    order = [m for m in order if m not in LOAD_ORDER] + present
    disabled += [m for m in new if m in OFF_BY_DEFAULT and m not in disabled]
    text = ("# The mods' load order (the first loads first, and each plays over those\n"
            "# before it: the Mods menu shows it the other way up), and the mods turned\n"
            "# off. The Mods menu writes this.\n"
            f"order = {json.dumps(order)}\ndisabled = {json.dumps(disabled)}\n")
    mods.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


# ---------------------------------------------------------------- each pack's steps


def step_extract(ctx):
    maps = set()
    for p in ctx.plan:
        maps |= set(BY_NAME[p].maps)
        if BY_NAME[p].level:
            maps |= set(ctx.levels)
    for m in sorted(maps):
        extract_map(ctx, m)


def step_dumps(ctx):
    if not ctx.game:
        raise Failure("no game (--game)")
    for m in BY_NAME["dumps"].maps:
        folder = ctx.w("dump", m, "data", "prism_dump", m)

        def dump(m=m):
            print(f"    the game's models of {m} (the game, hidden and silent)", flush=True)
            packs_game.dump_models(ctx.game, ctx.maps_dir(), m, ctx.w("dump", m))
        if not any(folder.glob("*.json")):
            ctx.once(f"dump_{m}", dump)


def dump_folder(ctx, m):
    return ctx.w("dump", m, "data", "prism_dump", m)


def step_class_masks(ctx):
    stats = ctx.w("class_masks", "island_stats")
    stats.mkdir(parents=True, exist_ok=True)
    for recipes in ("class_mask_recipes.json", "class_mask_recipes_m3.json"):
        for shader, spec in json.load(open(ART / recipes))["shaders"].items():
            m = spec.get("extract", "b30")
            model = ctx.w("extract", m, spec["model"])
            out = stats / (Path(spec["model"]).stem + ".txt")
            if not out.is_file():
                with open(out, "w") as f:
                    subprocess.run([PYTHON, "-B", "-I", ART / "island_stats.py", model, ctx.w("extract", m)], check=True,
                                   stdout=f, env=ctx.env())
    mod = ctx.mod("prism_class_masks")
    shutil.rmtree(mod, ignore_errors=True)
    # (of the Covenant's and the Marines' recipes, the shaders the overlay names a mask for: the others' classes
    # are not all there yet, class_mask_recipes_m3.json's notes)
    named = [k for k, v in json.load(open(ART / "overlay_characters.json"))["shaders"].items() if v and "class_mask" in v]
    for recipes, only in (("class_mask_recipes.json", []), ("class_mask_recipes_m3.json", named)):
        ctx.python("class_masks.py", "--extract", ctx.w("extract", "b30"), "--recipes", ART / recipes, "--stats", stats,
                   "--out", mod, *[x for shader in only for x in ("--shader", shader)], what=f"class masks ({recipes})")
    write_mod(mod, "prism_class_masks", "PRISM class masks (local)", "0.1",
              "Built on this computer from your own maps: the material overlay's per-texel classes. Not for sharing.")


def step_materials(ctx):
    w = ctx.w("materials")
    ctx.python("microtex.py", "--out", w / "tiles", what="the microtexture tiles (procedural: no game data)")
    uvs = []
    for m in ("b30", "a50"):
        uv = w / f"uv_density_{m}.json"
        ctx.python("measure_uv_density.py", "--extract", ctx.w("extract", m), "--json", uv,
                   what=f"texel density of {m}'s models")
        uvs += ["--uv", uv]
    mod = ctx.mod("prism_materials")
    ctx.python("overlay_pack.py", "--tiles", w / "tiles", *uvs, "--out", mod, "--tuning", ART / "overlay_tuning.json",
               what="the material overlay")


def step_base_maps(ctx):
    ensure_extractor(ctx)
    ctx.python("build_base_local.py", "--maps", ctx.maps_dir(), "--work", ctx.w("base_maps"), "--mod",
               ctx.mod("prism_base_local"), "--reuse", what="extract, upscale (Real-ESRGAN x4plus), tags, checks")


def step_chief_r2(ctx):
    w = ctx.w("chief_r2")
    recipe = recipe_copy(ctx, ART / "recipes" / "chief_3p_r2.json")
    cyborg = ctx.w("extract", "b30", "bitmaps", "characters", "cyborg", "bitmaps")
    ctx.once("pick", lambda: (
        ctx.blender_run("ce_ref_blend.py", ctx.w("extract", "b30", "models", "characters", "cyborg", "cyborg.gltf"),
                        w / "ce_ref.blend", what="CE's Chief as a scene"),
        ctx.blender_run("hero_pick.py", w / "ce_ref.blend", cyborg / "cyborg.png", w / "pick", "ce_ref",
                        what="picking views")))
    ctx.once("height2", lambda: ctx.python("hero_height2.py", recipe, w / "pick" / "mesh.npz", w / "height2",
                                           what="terrace fields from the recipe's features"))
    ctx.once("remodel", lambda: ctx.blender_run("hero_remodel2.py", recipe, w / "height2", w / "build",
                                                what="remodel (Blender)"))
    ctx.once("model", lambda: build_model_mod(
        ctx, "characters/cyborg/cyborg", w / "build" / "export", w / "tagwork", ctx.mod("prism_v2r2_chief_3p_model"),
        "prism_v2r2_chief_3p_model", "PRISM v2 r2: Chief (third person) (local)", "0.0.1",
        "PRISM v2 remodel built on this computer from your own maps: CE's rig, markers, regions, UVs and shaders; "
        "never shared or shipped."))

    def bake():
        bake_ce(ctx, w / "build" / "remodel.blend", w / "bake", "lod_sh_bake", "highpoly", "armor,visor", 4096, 128,
                8, 16, 40)
        ctx.blender_run("hero_bake_flat.py", w / "build" / "remodel.blend", w / "build" / "remodel_bake.blend",
                        what="its detail-free twin")
        bake_ce(ctx, w / "build" / "remodel_bake.blend", w / "bake_flat", "lod_sh_bake", "highpoly_flat",
                "armor,visor", 4096, 16, 8, 16, 40)
        ctx.python("hero_bake_delta.py", w / "bake", w / "bake_flat", w / "bake_delta", what="detail-only normals")
    ctx.once("bake", bake)
    ctx.once("pack", lambda: ctx.python("prism_surface_pack.py", w / "bake_delta", w / "pack", "--name", "cyborg",
                                        "--multipurpose", cyborg / "cyborg multipurpose.png", "--recipe",
                                        ART / "recipes" / "chief_3p_r2_pack.json", what="the surface map"))


def build_model_mod(ctx, model, export, w, mod, mod_id, name, version, description):
    """a hero remodel's JMS as a gbxmodel (Invader), agreeing with the map's model where mod_models.c compares,
    checked against every map's own, culled, as a mod"""
    model_dir = str(Path(model).parent).replace("\\", "/")
    src = w / "tags_src" / "b30"
    shutil.rmtree(src, ignore_errors=True)
    src.mkdir(parents=True)
    ctx.invader_run("invader-extract", "-r", "-m", ctx.maps_dir(), "-t", src, "-s", f"{model}.model",
                    ctx.maps_dir() / "b30.map", what="the map's model and shaders (Invader)")
    for sub in ("data", "tags"):
        shutil.rmtree(w / sub, ignore_errors=True)
    (w / "data" / model_dir / "models").mkdir(parents=True)
    (w / "tags" / model_dir / "shaders").mkdir(parents=True)
    for jms in sorted(Path(export).glob("*.jms")):
        ctx.python("jms_for_toolset.py", jms, w / "data" / model_dir / "models" / jms.name)
    for shader in ce_shaders(src, model_dir):
        shutil.copyfile(shader, w / "tags" / model_dir / "shaders" / shader.name)
    ctx.invader_run("invader-model", "-d", w / "data", "-t", w / "tags", "-T", "gbxmodel", model_dir,
                    what="invader-model")
    copy_node_values(ctx, src, f"{model}.model", w / "tags", f"{model}.gbxmodel")
    tag = w / "tags" / f"{model}.gbxmodel"
    strip_cull(ctx, tag, src / f"{model}.model")
    check_model_on_maps(ctx, tag, model, "model", w)
    shutil.rmtree(mod, ignore_errors=True)
    (mod / "tags" / model_dir).mkdir(parents=True)
    shutil.copyfile(tag, mod / "tags" / f"{model}.gbxmodel")
    write_mod(mod, mod_id, name, version, description)


def step_proto(ctx):
    src = ctx.w("textures", "src")
    src.mkdir(parents=True, exist_ok=True)
    cyborg = ctx.w("extract", "b30", "bitmaps", "characters", "cyborg", "bitmaps")
    shutil.copyfile(cyborg / "cyborg.png", src / "chief_diffuse.png")
    shutil.copyfile(cyborg / "cyborg multipurpose.png", src / "chief_multi.png")
    ctx.once("chief_study", lambda: ctx.python("texmethod/final.py", "chief_diffuse",
                                               what="the Chief's 4K maps (Real-ESRGAN general, the material method)"))
    ctx.once("maps", lambda: ctx.python("texmethod/build_textures.py", "--reuse",
                                        what="the Chief's and the Assault Rifle's maps"))


def step_chief_maps(ctx):
    w = ctx.w("chief_maps")
    proto = ctx.w("textures", "proto", "chief")
    cyborg = ctx.w("extract", "b30", "bitmaps", "characters", "cyborg", "bitmaps")
    ctx.once("tex", lambda: ctx.python("hero_tex_r2.py", proto / "albedo.png", proto / "multi.png",
                                       cyborg / "cyborg.png", cyborg / "cyborg multipurpose.png",
                                       ctx.w("chief_r2", "pick", "mesh.npz"), w / "tex", what="visor and lamp fixes"))
    texture_mod(ctx, w / "texwork", ctx.mod("prism_v2r2_chief_3p_maps"), "prism_v2r2_chief_3p_maps",
                "PRISM v2 r2: Chief maps (visor and lamp fixes, surface map) (local)", "0.0.1",
                "PRISM v2 maps built on this computer from your own maps; never shared or shipped.",
                {"characters/cyborg/bitmaps/cyborg": (w / "tex" / "albedo.png", "base"),
                 "characters/cyborg/bitmaps/cyborg multipurpose": (w / "tex" / "multi.png", "multi")},
                {"characters/cyborg/bitmaps/cyborg": ctx.w("chief_r2", "pack", "cyborg.surface.png")})


def step_ar_textures(ctx):
    proto = ctx.w("textures", "proto", "ar")
    texture_mod(ctx, ctx.w("ar_textures"), ctx.mod("prism_v2r2_ar_textures"), "prism_v2r2_ar_textures",
                "PRISM v2 r2: first-person Assault Rifle colour and multipurpose maps (local)", "0.0.1",
                "The first-person Assault Rifle's colour and multipurpose maps at 4096 and 2048 (the Chief's come "
                "with the r2 Chief maps). Built on this computer from your own maps: never shared or shipped.",
                {"weapons/assault rifle/fp/bitmaps/assault rifle_fp": (proto / "albedo.png", "base"),
                 "weapons/assault rifle/fp/bitmaps/assault rifle multipurpose": (proto / "multi.png", "multi")})


def step_tiers(ctx):
    ctx.once("cache", lambda: ctx.python("texmethod/tier_cache.py", what="the five upscalers on every map "
                                         "(Real-ESRGAN x3, Nomos8kSC)"))
    ctx.once("tiers", lambda: ctx.python("texmethod/tiers.py", what="Enhanced and Max (fusion, detail)"))
    ctx.once("tags", lambda: ctx.python("texmethod/build_tier_mods.py", what="their tags, checked on every map"))


def step_fp_arms(ctx):
    w = ctx.w("fp_arms")
    parts = [("ar", "gun,grip", "weapons/assault rifle/fp/bitmaps/assault rifle_fp",
              "weapons/assault rifle/fp/bitmaps/assault rifle multipurpose", "fp_ar_surface_r2.json",
              "weapons/assault rifle/fp/fp", "assault rifle fp superhigh.jms"),
             ("arms", "armor hands,rubber hands", "characters/cyborg/fp/bitmaps/cyborg hands",
              "characters/cyborg/fp/bitmaps/cyborg hands multipurpose", "fp_arms_surface_r2.json",
              "characters/cyborg/fp/fp", "__base.jms")]
    for n, shaders, bitmap, multi, surface, _model, _jms in parts:
        relief = recipe_copy(ctx, ART / "recipes" / f"fp_{n}_relief_r2.json")
        remodel = recipe_copy(ctx, ART / "recipes" / f"fp_{n}_remodel_r2.json")
        ctx.once(f"relief_{n}", lambda r=relief, n=n: ctx.python("fp_relief.py", r, what=f"{n}: relief from CE's maps"))
        ctx.once(f"remodel_{n}", lambda r=remodel, n=n: ctx.blender_run("fp_remodel.py", r, what=f"{n}: remodel"))
        ctx.once(f"bake_{n}", lambda n=n, s=shaders: bake_ce(ctx, w / n / "build" / "remodel.blend", w / "bake" / n,
                                                             "new", "high", s, 4096, 128, 7, 14, 20))
        ctx.once(f"pack_{n}", lambda n=n, b=bitmap, m=multi, s=surface: ctx.python(
            "prism_surface_pack.py", w / "bake" / n, w / "pack" / n, "--name", Path(b).name, "--multipurpose",
            ctx.w("extract", "b30", "bitmaps", m + ".png"), "--recipe", ART / "recipes" / s, what=f"{n}: surface map"))
    mod = ctx.mod("prism_v2r2_fp_hands_ar")
    build_fp_mod(ctx, w / "mod_build", mod, [(model, w / n / "build" / "export" / jms)
                                            for n, _s, _b, _m, _f, model, jms in parts])
    for n, _s, bitmap, _m, _f, _model, _jms in parts:
        (mod / "tags" / bitmap).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(w / "pack" / n / f"{Path(bitmap).name}.surface.png", mod / "tags" / f"{bitmap}.surface.png")
    write_mod(mod, "prism_v2r2_fp_hands_ar", "PRISM v2 round 2: first-person arms and Assault Rifle (local)", "0.0.2",
              "Round 2: the Master Chief's first-person arms and the first-person Assault Rifle remodelled from CE's own "
              "meshes: crisp bevels on CE's hard edges, CE's curves rounded, deeper relief and modelled secondary "
              "forms (panel breaks, ejection port, vents, rail teeth, screw heads) carried by about 42,000 triangles "
              "each and baked surface maps (display.prism_surface_maps). CE's rig, markers, UVs and shaders. Built on "
              "this computer from your own maps: never shared or shipped.")


def build_fp_mod(ctx, w, mod, models):
    """the first-person remodels as gbxmodels (Invader), checked against every map's own, culled"""
    sys.path.insert(0, str(ART))
    import halo_formats as hf
    for sub in ("tags_src", "data", "tags", "checks"):
        shutil.rmtree(w / sub, ignore_errors=True)
    src, gbx = w / "tags_src" / "b30", w / "tags_src" / "b30_gbx"
    src.mkdir(parents=True)
    gbx.mkdir(parents=True)
    (w / "data").mkdir(parents=True)
    (w / "tags").mkdir(parents=True)
    for model, jms in models:
        model_dir = str(Path(model).parent).replace("\\", "/")
        ctx.invader_run("invader-extract", "-r", "-m", ctx.maps_dir(), "-t", src, "-s", f"{model}.model",
                        ctx.maps_dir() / "b30.map", what=f"{model}: the map's tags")
        ctx.invader_run("invader-convert", "-g", "model", "gbxmodel", "-t", src, "-o", gbx, model)
        for shader in src.rglob("*.shader_*"):
            dst = w / "tags" / shader.relative_to(src)
            if not dst.exists():
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(shader, dst)
        out = w / "data" / model_dir / "models" / Path(jms).name
        out.parent.mkdir(parents=True, exist_ok=True)
        ctx.python("jms_for_toolset.py", jms, out)
        # (shader permutations: the Assault Rifle's two "numbers" entries are permutations 1 and 0, the ammo
        # counter's tens and ones; invader-model reads a permutation from digits ending the material's name)
        count = int(ctx.output([ctx.inv("invader-edit"), "-t", gbx, "-C", "shaders", f"{model}.gbxmodel"]))
        perms = [int(ctx.output([ctx.inv("invader-edit"), "-t", gbx, "-G", f"shaders[{i}].permutation",
                                 f"{model}.gbxmodel"])) for i in range(count)]
        j = hf.read_jms(str(out))
        materials = list(j.materials)
        for i, p in enumerate(perms[:len(materials)]):
            if p > 0:
                materials[i] = ("%s%d" % (materials[i][0], p), materials[i][1])
        j.materials = materials
        hf.write_jms(str(out), j)
        ctx.invader_run("invader-model", "-d", w / "data", "-t", w / "tags", "-T", "gbxmodel", model_dir,
                        what=f"{model}: invader-model")
        copy_node_values(ctx, gbx, f"{model}.gbxmodel", w / "tags", f"{model}.gbxmodel", fields=False)
        tag = w / "tags" / f"{model}.gbxmodel"
        strip_cull(ctx, tag, src / f"{model}.model")
        check_model_on_maps(ctx, tag, model, "model", w)
    shutil.rmtree(mod, ignore_errors=True)
    for model, _jms in models:
        dst = mod / "tags" / f"{model}.gbxmodel"
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(w / "tags" / f"{model}.gbxmodel", dst)


def step_elite_r2(ctx):
    w = ctx.w("elite_r2")
    names = {"elite": "characters/elite/elite", "elite_special": "characters/elite/elite_special/elite_special"}
    for name in names:
        recipe = recipe_copy(ctx, ART / "prism_elite" / f"{name}_r2_recipe.json")
        ctx.once(f"build_{name}", lambda r=recipe, n=name: ctx.blender_run("prism_elite/elite_build_r2.py", r,
                                                                            what=f"{n}: remodel"))
        bake_recipe = ART / "prism_elite" / f"{name}_r2_bake.json"
        bake_dir = w / "bake" / name
        ctx.once(f"bake_prep_{name}", lambda r=recipe, b=bake_recipe, d=bake_dir, n=name: ctx.blender_run(
            "prism_elite/elite_bake_prep_r2.py", r, b, d, what=f"{n}: bake scenes"))
        ctx.once(f"bake_{name}", lambda b=bake_recipe, d=bake_dir, n=name: elite_bake(ctx, b, d, w / "pack" / n, n))
    elite_mod(ctx, w, ctx.mod("prism_v2_elite_r2"), names)


def elite_bake(ctx, bake_recipe, bake_dir, pack_dir, name):
    r = json.load(open(bake_recipe))
    jobs = json.load(open(bake_dir / "bake_jobs.json"))
    bitmaps = ctx.w("extract", "b30", "bitmaps", "characters", "elite", *(["elite_special"] if name == "elite_special"
                                                                          else []), "bitmaps")
    for j in jobs:
        if (Path(j["raw"]) / "bake.json").is_file():
            continue
        bake_ce(ctx, j["blend"], j["raw"], "game", "high", ",".join(j["shaders"]), r["size"], r["samples"],
                r["cage_mm"], r["ray_mm"], r["ao_mm"], "--keep-high-materials", "--wrap-uv",
                *(["--occluders", j["occluders"]] if j["occluders"] else []))
    (bake_dir / "pack_recipe.json").write_text(json.dumps(r["pack"]))
    pack_dir.mkdir(parents=True, exist_ok=True)
    for bitmap in sorted({j["bitmap"] for j in jobs}):
        raws = [j["raw"] for j in jobs if j["bitmap"] == bitmap]
        src = raws[0]
        if len(raws) > 1:
            src = bake_dir / ("raw_%s_merged" % bitmap.replace(" ", "_"))
            ctx.python("prism_elite/merge_raw_r2.py", src, *raws)
        ctx.python("prism_surface_pack.py", src, pack_dir, "--name", bitmap, "--multipurpose",
                   bitmaps / f"{bitmap} multi.png", "--recipe", bake_dir / "pack_recipe.json",
                   what=f"{bitmap}: surface map")


def elite_mod(ctx, w, mod, names):
    """the Elites' remodels sealed (CE's own mesh set behind), compiled, checked, culled, with surface maps"""
    src = w / "tags_src" / "b30"
    src.mkdir(parents=True, exist_ok=True)
    if not (src / "characters/elite/elite.model").is_file():
        ctx.invader_run("invader-extract", "-r", "-m", ctx.maps_dir(), "-t", src, "-s", "characters/elite/*",
                        ctx.maps_dir() / "b30.map", what="the map's Elites (Invader)")
    shutil.rmtree(w / "data", ignore_errors=True)
    shutil.rmtree(w / "tags", ignore_errors=True)
    for name, model in names.items():
        model_dir = str(Path(model).parent).replace("\\", "/")
        (w / "data" / model_dir / "models").mkdir(parents=True, exist_ok=True)
        for jms in sorted((w / "build" / name / "jms").glob("*.jms")):
            backing = [] if jms.name.endswith((" low.jms", " superlow.jms")) else [
                "--ce", ctx.w("extract", "b30", "models", model, jms.name), "--exclude",
                "elite teeth,elite inset lights,elite special inset lights,elite special emblem"]
            ctx.python("prism_elite/elite_seal_jms.py", jms, w / "data" / model_dir / "models" / jms.name, *backing)
        for folder in {Path("characters/elite/shaders"), Path(model_dir) / "shaders"}:
            if (src / folder).is_dir():
                (w / "tags" / folder).mkdir(parents=True, exist_ok=True)
                for shader in (src / folder).glob("*.shader_*"):
                    shutil.copyfile(shader, w / "tags" / folder / shader.name)
        ctx.invader_run("invader-model", "-d", w / "data", "-t", w / "tags", "-T", "gbxmodel", model_dir,
                        what=f"{name}: invader-model")
        copy_node_values(ctx, src, f"{model}.model", w / "tags", f"{model}.gbxmodel")
        tag = w / "tags" / f"{model}.gbxmodel"
        tris = w / f"tris_{name}.json"
        with open(tris, "w") as f:
            subprocess.run([PYTHON, "-B", "-I", ART / "prism_review_tris.py", tag, src / f"{model}.model"], stdout=f,
                           env=ctx.env())
        try:
            only_mod = json.load(open(tris)).get("only_mod")
        except ValueError:
            only_mod = True
        if only_mod:
            strip_cull(ctx, tag, src / f"{model}.model")
        check_model_on_maps(ctx, tag, model, "model", w)
    shutil.rmtree(mod, ignore_errors=True)
    for name, model in names.items():
        dst = mod / "tags" / f"{model}.gbxmodel"
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(w / "tags" / f"{model}.gbxmodel", dst)
        for png in sorted((w / "pack" / name).glob("*.surface.png")):
            folder = "characters/elite/elite_special/bitmaps" if png.name.startswith("elite special") \
                else "characters/elite/bitmaps"
            (mod / "tags" / folder).mkdir(parents=True, exist_ok=True)
            shutil.copyfile(png, mod / "tags" / folder / png.name)
    write_mod(mod, "prism_v2_elite_r2", "PRISM v2 r2: Elite and special Elite models (local)", "0.0.2",
              "Round 2: the Elite and the special Elite remodelled from CE's own mesh (CE's proportions kept, plates "
              "with real thickness and crisp rims cut along CE's painted plate lines, CE's rig, markers, UVs and "
              "shaders; rebuilt lower LODs) and their surface maps (normal, occlusion, roughness; "
              "display.prism_surface_maps). Built on this computer from your own maps: never shared or shipped.")


def step_bold(ctx):
    ctx.once("maps", lambda: ctx.python("texmethod/bold.py", what="Bold maps"))
    ctx.once("mod", lambda: ctx.python("texmethod/bold_mods.py", what="Bold tags and mod"))
    weathering = ctx.mod("prism_v2_tex_bold_weathering")
    if weathering.is_dir():  # (a variant the kit leaves out; bold_mods.py writes it beside Bold)
        shutil.rmtree(weathering)


def step_chief_r3(ctx):
    w = ctx.w("chief_r3")
    recipe = recipe_copy(ctx, ART / "recipes" / "chief_3p_r3.json")
    cyborg = ctx.w("extract", "b30", "bitmaps", "characters", "cyborg", "bitmaps")
    tier = ctx.w("textures", "tiers", "max", "data", "characters", "cyborg", "bitmaps")
    ctx.once("pick", lambda: ctx.blender_run("hero_pick.py", ctx.w("chief_r2", "build", "remodel.blend"),
                                             cyborg / "cyborg.png", w / "pick", "ce_ref", what="picking views"))
    ctx.once("height2", lambda: ctx.python("hero_height2.py", recipe, w / "pick" / "mesh.npz", w / "height2",
                                           what="terrace fields"))
    ctx.once("remodel", lambda: ctx.blender_run("hero_remodel2.py", recipe, w / "height2", w / "build",
                                                what="remodel (round 2's construction)"))
    ctx.once("build", lambda: ctx.blender_run("prism_r3_chief.py", w / "build" / "remodel.blend", w / "r3build",
                                              what="round 3: gloves, pads, pouches, lamps, helper bones"))
    ctx.once("bake", lambda: bake_ce(ctx, w / "r3build" / "r3.blend", w / "bake", "l0_bake", "r3_high", "armor,visor",
                                     4096, 128, 6, 12, 40))
    ctx.once("pack", lambda: ctx.python("prism_surface_pack.py", w / "bake", w / "pack", "--name", "cyborg",
                                        "--multipurpose", cyborg / "cyborg multipurpose.png", "--recipe",
                                        ART / "recipes" / "chief_3p_r2_pack.json", what="surface map"))

    def textures():
        (w / "tex").mkdir(parents=True, exist_ok=True)
        (w / "checks").mkdir(parents=True, exist_ok=True)
        ctx.python("prism_r3_albedo.py", tier / "cyborg.png", w / "height2", w / "tex" / "cyborg_albedo.png",
                   what="albedo (CE's painted shading reduced)")
        ctx.blender_run("prism_r3_panel_fill.py", w / "r3build" / "r3.blend", w / "tex" / "cyborg_albedo.png",
                        w / "tex" / "cyborg_albedo.png", what="the back panel and lamps filled")
        ctx.python("tag_export.py", ctx.w("chief_r2", "tagwork", "tags_src", "b30", "characters", "cyborg",
                                          "cyborg.model"), w / "checks" / "ce_sh.npz")
        ctx.python("prism_r3_emissive.py", w / "checks" / "ce_sh.npz", tier / "cyborg.png",
                   w / "tex" / "cyborg_emissive.png", what="emissive")
    ctx.once("textures", textures)
    mat = ctx.mod("prism_r3_chief_materials")
    shutil.rmtree(mat, ignore_errors=True)
    t = mat / "textures" / "materials"
    pack = [PYTHON, "-B", "-I", ctx.tools / "prism_material_pack.py"]
    ctx.run([*pack, "albedo", "--in", w / "tex" / "cyborg_albedo.png", "--out", t / "cyborg" / "armor_albedo.ktx2",
             "--size", 4096], what="PBR images (BC7)")
    ctx.run([*pack, "albedo", "--in", w / "tex" / "cyborg_emissive.png", "--out",
             t / "cyborg" / "armor_emissive.ktx2", "--size", 4096])
    ctx.run([*pack, "normal", "--surface", w / "pack" / "cyborg.surface.png", "--out",
             t / "cyborg" / "armor_normal.ktx2"])
    ctx.run([*pack, "orm", "--surface", w / "pack" / "cyborg.surface.png", "--multipurpose",
             tier / "cyborg multipurpose.png", "--metal", 0.15, "--out", t / "cyborg" / "armor_orm.ktx2"])
    ctx.run([*pack, "detail", "--out", t / "tiles" / "weave_detail.ktx2", "--size", 512])
    shaders = mat / "materials" / "characters" / "cyborg" / "shaders"
    shaders.mkdir(parents=True, exist_ok=True)
    for name, material in R3_MATERIALS.items():
        (shaders / f"{name}.material.json").write_text(json.dumps(material, indent=2) + "\n")
    write_mod(mat, "prism_r3_chief_materials", "PRISM r3: the Chief's materials (local)", "1",
              "The round 3 Chief's armour and visor as PBR materials (display.prism_materials pbr): colour, normal, "
              "roughness, metal, emission. Built on this computer from your own maps: never shared or shipped.")
    meshes = ctx.mod("prism_r3_chief_meshes")
    shutil.rmtree(meshes, ignore_errors=True)
    ctx.blender_run("prism_pmesh_export.py", "export", w / "r3build" / "r3.blend",
                    dump_folder(ctx, "b30") / "characters_cyborg_cyborg.json",
                    meshes / "meshes" / "max" / "characters" / "cyborg" / "cyborg.pmesh", "--rig",
                    w / "r3build" / "cyborg.rig.json", what="the mesh store's file (.pmesh)")
    write_mod(meshes, "prism_r3_chief_meshes", "PRISM r3: the Chief (meshes)", "0.1.0",
              "Round 3 third-person Chief: round 2's crisp design with remodelled gloves, knuckle pads, attached "
              "pouches, lifted lamps, denser vents, cables and plate breaks; helper bones. Built on this computer from "
              "your own maps: never shared or shipped.")


# the round 3 Chief's material files (port/linux/game/mod_pbr.c reads them): settings only
R3_MATERIALS = {
    "armor": {"format": 1, "albedo": "textures/materials/cyborg/armor_albedo.ktx2", "albedo_factor": [1, 1, 1],
              "normal": "textures/materials/cyborg/armor_normal.ktx2", "normal_scale": 1.0,
              "orm": "textures/materials/cyborg/armor_orm.ktx2", "roughness": 1.0, "metal": 1.0, "occlusion": 1.0,
              "detail": {"map": "textures/materials/tiles/weave_detail.ktx2", "scale": 64, "normal": 0.08,
                         "cavity": 0.12, "roughness": 0.15, "mask": "all"},
              "reflection": 1.0, "specular": 0.5, "wrap": True, "direct_specular": 1.0, "ce_reflection": 1.0,
              "multipurpose": {"specular_mask": True, "self_illumination": True, "reflection_tint": True},
              "emissive": "textures/materials/cyborg/armor_emissive.ktx2", "emissive_factor": [1.6, 1.6, 1.6]},
    "visor": {"format": 1, "normal": "textures/materials/cyborg/armor_normal.ktx2", "normal_scale": 0.2,
              "roughness": 0.08, "metal": 1.0, "clearcoat": {"strength": 1.0, "roughness": 0.02},
              "reflection": 1.0, "specular": 0.5, "wrap": True, "direct_specular": 1.0, "ce_reflection": 1.0,
              "multipurpose": {"specular_mask": True, "self_illumination": True, "reflection_tint": True}},
}


def step_cast(ctx):
    w = ctx.w("cast")
    dump = dump_folder(ctx, "b30")
    models = ctx.w("extract", "b30", "models", "characters")
    mod = ctx.mod("prism_v3_cast")
    for c, base in (("marine_armored", "__base"), ("grunt", "base00"), ("jackal", "base00")):
        recipe = recipe_copy(ctx, ART / "prism_cast" / f"{c}_recipe.json", f"cast_{c}_recipe.json")
        ctx.once(f"build_{c}", lambda r=recipe, c=c: ctx.blender_run("prism_elite/elite_build_r2.py", r,
                                                                      what=f"{c}: remodel"))
        ctx.once(f"stitch_{c}", lambda c=c, b=base: ctx.python(
            "prism_cast/cast_stitch.py", models / c / c, w / c / "build" / "jms", w / c / "stitched", "--base", b,
            what=f"{c}: seams closed"))
    shutil.rmtree(mod, ignore_errors=True)
    for c in ("marine_armored", "grunt", "jackal"):
        out = mod / "meshes" / "max" / "characters" / c / f"{c}.pmesh"
        ctx.python("prism_cast/jms_to_pmesh.py", dump / f"characters_{c}_{c}.json", w / c / "stitched",
                   models / c / c, out, what=f"{c}: the mesh store's file")
        ctx.run([PYTHON, "-B", "-I", ctx.tools / "pmesh.py", "check", out, "--dump", dump / f"characters_{c}_{c}.json"])
    write_mod(mod, "prism_v3_cast", "PRISM v3: Marines, Grunts and Jackals (meshes, local)", "0.3.0",
              "The armoured Marine, the Grunt and the Jackal remodelled from CE's own meshes in the round-2 style "
              "(CE's proportions; armour plates cut along CE's painted plate lines with real thickness, crisp "
              "chamfered rims and recessed seams; fabric and skin set back), drawn by the mesh store "
              "(display.prism_models \\\"max\\\"). Faces, eyes, caps, teeth and the Jackal's shield are CE's. Built on "
              "this computer from your own maps: never shared or shipped.")
    # the Elites of round 2, stitched for the mesh store (left off in the load order: see docs/prism_packs.md)
    elites = ctx.mod("prism_v3_elite_meshes")
    shutil.rmtree(elites, ignore_errors=True)
    r2 = ctx.w("elite_r2", "build")
    ctx.python("prism_cast/cast_stitch.py", models / "elite" / "elite", r2 / "elite" / "jms", w / "elite_stitched",
               "--base", "base00", what="the Elite: seams closed")
    ctx.python("prism_cast/cast_stitch.py", models / "elite" / "elite_special" / "elite_special",
               r2 / "elite_special" / "jms", w / "special_stitched", "--base", "__base")
    out = elites / "meshes" / "max" / "characters" / "elite"
    ctx.python("prism_cast/jms_to_pmesh.py", dump / "characters_elite_elite.json", w / "elite_stitched",
               models / "elite" / "elite", out / "elite.pmesh")
    ctx.python("prism_cast/jms_to_pmesh.py", dump / "characters_elite_elite_special_elite_special.json",
               w / "special_stitched", models / "elite" / "elite_special" / "elite_special",
               out / "elite_special" / "elite_special.pmesh", "--cutoffs", "300,150,100,50,0")
    write_mod(elites, "prism_v3_elite_meshes", "PRISM v3: Elite and special Elite (meshes, local)", "0.3.0",
              "Round 2's Elite and special Elite remodels drawn by the mesh store (display.prism_models \\\"max\\\") "
              "with their seams stitched. Keep prism_v2_elite_r2 for its surface maps. Built on this computer from "
              "your own maps: never shared or shipped.")


# a rock's base map: the extract it is in (build_rocks_mod.sh's table)
ROCK_MAPS = {"boulder_granite_": ("a50", "scenery/rocks/boulder_granite_large/bitmaps/boulder_granite_large.png"),
             "a50_rock_large": ("a50", "scenery/rocks/a50_rock_large/bitmaps/a50_rock_large.png"),
             "rock_sharp": ("a50", "scenery/rocks/rock_sharpcurly/bitmaps/rock_sharp.png"),
             "boulder_moss_": ("c10", "scenery/rocks/boulder_moss_large/bitmaps/boulder_moss_large.png"),
             "boulder_crouch": ("b30", "scenery/rocks/boulder_crouch/bitmaps/boulder_crouch.png"),
             "boulder_doublewide": ("b30", "scenery/rocks/boulder_doublewide/bitmaps/boulder_doublewide.png"),
             "boulder": ("b30", "scenery/rocks/boulder/bitmaps/boulder.png")}


def step_rocks(ctx):
    mod = ctx.mod("prism_v3_rocks")
    shutil.rmtree(mod, ignore_errors=True)
    (mod / "reports").mkdir(parents=True)
    done = {}
    for m in ("a50", "a30", "c10", "b30"):
        for j in sorted(dump_folder(ctx, m).glob("scenery_rocks_*.json")):
            name = j.stem[len("scenery_rocks_"):]
            # (the file name repeats the model's folder and name: scenery_rocks_<folder>_<name>)
            half = len(name) // 2
            folder = name[:half] if name[:half] == name[half + 1:] else name
            out = mod / "meshes" / "max" / "scenery" / "rocks" / folder / f"{folder}.pmesh"
            if folder in done:
                ctx.run([PYTHON, "-B", "-I", ctx.tools / "pmesh.py", "check", out, "--dump", j], check=False)
                continue
            done[folder] = True
            base = "-"
            for prefix in sorted(ROCK_MAPS, key=len, reverse=True):
                if folder == prefix or (prefix.endswith("_") and folder.startswith(prefix)) or \
                        (prefix == "rock_sharp" and folder.startswith(prefix)):
                    em, png = ROCK_MAPS[prefix]
                    if ctx.w("extract", em, "bitmaps", png).is_file():
                        base = ctx.w("extract", em, "bitmaps", png)
                    break
            ctx.python("prism_cast/rock_remodel.py", j, base, out, "--relief", 0.003, "--report",
                       mod / "reports" / f"{folder}.json", what=f"{folder}")
    write_mod(mod, "prism_v3_rocks", "PRISM v3: rocks and boulders (meshes, local)", "0.3.0",
              "The scenery rocks and boulders of the campaign remodelled for the mesh store (display.prism_models "
              "\\\"max\\\"): CE's mass rounded, chiselled into flat facets with crisp ridges, CE's painted cracks set "
              "in; CE's collision. Built on this computer from your own maps: never shared or shipped.")


def level_bsps(ctx, level):
    """a level's structure bsps: [(index, name)] (the light sources' file names them)"""
    lights = json.load(open(ctx.w("extract", f"{level}_lights.json")))
    return [(i, b["tag"].replace("\\", "/").rsplit("/", 1)[1]) for i, b in enumerate(lights["bsps"])]


def bsp_gltf(ctx, level, name):
    return ctx.w("extract", level, "bsps", "levels", level, f"{name}.gltf")


def step_world(ctx):
    recipes = json.load(open(ART / "world_geo" / "wg2_recipes.json"))
    for level in ctx.levels:
        for _index, name in level_bsps(ctx, level):
            if f"levels\\{level}\\{name}" not in recipes:
                continue
            w = ctx.w("world", level)
            pwg = w / f"{name}.pwg"

            def build(level=level, name=name, w=w, pwg=pwg):
                w.mkdir(parents=True, exist_ok=True)
                gltf = bsp_gltf(ctx, level, name)
                ctx.blender_run("world_geo/wg2_remodel.py", gltf, ctx.w("extract", level),
                                ART / "world_geo" / "wg2_recipes.json", w / f"{name}.remodel.pwg",
                                w / f"{name}.report.json", what=f"{level} {name}: remodel and its light (Cycles)")
                ctx.python("world_geo/wg2_seal.py", gltf, w / f"{name}.remodel.pwg", w / f"{name}.sealed.pwg",
                           w / f"{name}.seal.json", what=f"{level} {name}: sealed")
                ctx.blender_run("world_geo/wg2_far2.py", w / f"{name}.sealed.pwg", pwg, w / f"{name}.far2.json",
                                what=f"{level} {name}: far levels")
                (w / f"{name}.remodel.pwg").unlink()
                (w / f"{name}.sealed.pwg").unlink()
            ctx.once(f"{level}_{name}", build)
            dst = ctx.out / "prism_world" / "levels" / level / f"{name}.pwg"
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(pwg, dst)


def step_lightmaps(ctx):
    samples = str(ctx.a.lightmap_samples)
    for level in ctx.levels:
        lights = ctx.w("extract", f"{level}_lights.json")
        for index, name in level_bsps(ctx, level):
            gltf = bsp_gltf(ctx, level, name)
            bake = ctx.w("lightmaps", "bake", name)
            gpu = ["--gpu", ctx.a.gpu] if ctx.a.gpu else []

            def relight(index=index, name=name, gltf=gltf, bake=bake, gpu=gpu):
                bake.mkdir(parents=True, exist_ok=True)
                ctx.blender_run("relight/relight_bake.py", "--gltf", gltf, "--lights", lights, "--bsp", index, "--out",
                                bake, "--stage", "calib", "--samples", 64, *gpu,
                                what=f"{level} {name}: calibration bake (Cycles)")
                ctx.python("relight/relight_calib.py", "fit", "--gltf", gltf, "--bake", bake, "--lights", lights,
                           "--bsp", index, what=f"{level} {name}: each light fitted to CE's pools")
                ctx.blender_run("relight/relight_bake.py", "--gltf", gltf, "--lights", lights, "--bsp", index, "--out",
                                bake, "--stage", "final", "--factor", 4, "--samples", samples, *gpu, "--scales",
                                bake / "scales.json", what=f"{level} {name}: final bake (Cycles, {samples} samples)")
                ctx.python("relight/relight_encode.py", "encode", "--gltf", gltf, "--bake", bake, "--map", level,
                           "--bsp", index, "--out", ctx.w("lightmaps", "data"),
                           what=f"{level} {name}: the pages the game reads")
                ctx.python("relight/relight_calib.py", "check", "--gltf", gltf, "--bake", bake, "--map", level,
                           "--bsp", index, "--rebaked", ctx.w("lightmaps", "data", "prism_lightmaps", "rebaked"),
                           "--json", bake / "check.json", what=f"{level} {name}: checked against CE's")
            ctx.once(f"{level}_{name}", relight)
        dst = ctx.out / "prism_lightmaps" / "rebaked"
        dst.mkdir(parents=True, exist_ok=True)
        for prl in glob.glob(str(ctx.w("lightmaps", "data", "prism_lightmaps", "rebaked", f"{level}_*.prl"))):
            shutil.copyfile(prl, dst / Path(prl).name)


def step_sun(ctx):
    for level in ctx.levels:
        lights = ctx.w("extract", f"{level}_lights.json")
        skies = json.load(open(lights)).get("skies", [])
        if not any(s.get("lights") for s in skies):
            print(f"    {level}: no sun in its sky (nothing to bake)", flush=True)
            continue
        for index, name in level_bsps(ctx, level):
            gltf = bsp_gltf(ctx, level, name)
            calib = ctx.w("lightmaps", "bake", name)
            out = ctx.w("sun", name)
            pwg = ctx.w("world", level, f"{name}.pwg")
            gpu = ["--gpu", ctx.a.gpu] if ctx.a.gpu else []

            def bake(index=index, name=name, gltf=gltf, calib=calib, out=out, pwg=pwg, gpu=gpu):
                ctx.blender_run("relight/sunmask_bake.py", "--gltf", gltf, "--lights", lights, "--bsp", index, "--calib",
                                calib, "--out", out, *(["--pwg", pwg] if pwg.is_file() else []), *gpu,
                                what=f"{level} {name}: the sun's visibility (Cycles)")
                ctx.python("relight/sunmask_encode.py", "--gltf", gltf, "--bake", out, "--map", level, "--bsp", index,
                           "--out", ctx.w("sun", "data"), "--calib", calib, what=f"{level} {name}: the masks")
            ctx.once(f"{level}_{name}", bake)
        dst = ctx.out / "prism_lightmaps" / "sun"
        dst.mkdir(parents=True, exist_ok=True)
        for psm in glob.glob(str(ctx.w("sun", "data", "prism_lightmaps", "sun", f"{level}_*.psm"))):
            shutil.copyfile(psm, dst / Path(psm).name)


def step_probes(ctx):
    probes = HERE.parent / "prism_probes.py" if (HERE.parent / "prism_probes.py").is_file() \
        else HERE / "formats" / "prism_probes.py"
    for level in ctx.levels:
        run = ctx.w("probes", level)
        data = packs_game.prepare(run, ctx.maps_dir())
        ctx.once(level, lambda level=level, run=run, data=data: ctx.run(
            [PYTHON, "-B", "-I", probes, "--exe", ctx.game, "--data", data, "--map", level, "--port", 26491, "--work", run],
            what=f"{level}: probes (the game, hidden and silent)"))
        src = data / "prism" / "probes" / f"{level}.probes"
        dst = ctx.out / "prism" / "probes" / f"{level}.probes"
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, dst)


STEPS = {"extract": step_extract, "dumps": step_dumps, "class_masks": step_class_masks,
         "materials": step_materials, "base_maps": step_base_maps, "chief_r2": step_chief_r2, "proto": step_proto,
         "chief_maps": step_chief_maps, "ar_textures": step_ar_textures, "tiers": step_tiers,
         "fp_arms": step_fp_arms, "elite_r2": step_elite_r2, "bold": step_bold, "chief_r3": step_chief_r3,
         "cast": step_cast, "rocks": step_rocks, "world": step_world, "lightmaps": step_lightmaps,
         "sun": step_sun, "probes": step_probes}


# ---------------------------------------------------------------- the command line


def expand(names):
    """the packs named (groups expanded), with every pack they need, in build order"""
    wanted = set()
    for name in names or ["all"]:
        if name == "all":
            wanted |= {p.name for p in PACKS if not p.internal and not p.off}
        elif name == "characters":
            wanted |= set(CHARACTERS)
        elif name == "levels":
            wanted |= set(LEVEL_PACKS)
        elif name in BY_NAME:
            wanted.add(name)
        else:
            raise SystemExit(f"prism_packs: no pack {name!r} (prism_packs.py list)")
    stack = list(wanted)
    while stack:
        for need in BY_NAME[stack.pop()].needs:
            if need not in wanted:
                wanted.add(need)
                stack.append(need)
    return [p.name for p in PACKS if p.name in wanted]


def done(ctx, name):
    return ctx.w("state", f"{name}.done").is_file()


def build(ctx, names):
    plan = expand(names)
    ctx.plan = plan
    problems = check(ctx, plan)
    if problems:
        print("This computer lacks what these packs need:", file=sys.stderr)
        for problem in problems:
            print("  " + problem, file=sys.stderr)
        return 1
    ctx.work.mkdir(parents=True, exist_ok=True)
    (ctx.work / "LOCAL_ONLY.txt").write_text("The pack tools' work folder: everything in it is made from your copy of "
                                             "the game. Keep it to yourself; delete it once the packs are built.\n")
    for name in getattr(ctx.a, "again", None) or []:
        for stamp in [ctx.w("state", f"{name}.done"), *ctx.w("state", name).glob("*.done")]:
            stamp.unlink(missing_ok=True)
    started = time.time()
    for i, name in enumerate(plan, 1):
        pack = BY_NAME[name]
        if done(ctx, name):
            print(f"[{i}/{len(plan)}] {name}: built before", flush=True)
            continue
        print(f"[{i}/{len(plan)}] {name}: {pack.gives}", flush=True)
        ctx.step = name
        log_path = ctx.w("logs", f"{name}.log")
        log_path.parent.mkdir(parents=True, exist_ok=True)
        with open(log_path, "a", encoding="utf-8", errors="replace") as ctx.log:
            ctx.log.write(f"\n==== {time.strftime('%Y-%m-%d %H:%M:%S')} {name}\n")
            t0 = time.time()
            try:
                STEPS[name](ctx)
            except (Failure, subprocess.CalledProcessError, OSError, ValueError, KeyError) as error:
                ctx.log.flush()
                print(f"\n{name} failed: {error}", file=sys.stderr)
                tail = log_path.read_text(encoding="utf-8", errors="replace").splitlines()[-30:]
                print("  " + "\n  ".join(tail), file=sys.stderr)
                print(f"\nThe whole log: {log_path}. What was done is kept: start the same command again to go on "
                      f"from {name}.", file=sys.stderr)
                return 1
        ctx.w("state").mkdir(parents=True, exist_ok=True)
        ctx.w("state", f"{name}.done").write_text(json.dumps({"version": VERSION, "seconds": round(time.time() - t0)}))
        print(f"    {name}: done", flush=True)
    load_order(ctx)
    print(f"\nBuilt: {', '.join(p for p in plan if not BY_NAME[p].internal)} into {ctx.out} "
          f"({(time.time() - started) / 60:.0f} minutes this run). docs/prism_packs.md says how to turn them on.")
    return 0


def status(ctx):
    for p in PACKS:
        mark = "built" if done(ctx, p.name) else ("started" if ctx.w("state", p.name).is_dir() else "-")
        print(f"  {p.name:12} {mark:8} {'(work folder only)' if p.internal else ''}")
    return 0


def list_packs():
    for p in PACKS:
        if p.internal:
            continue
        where = ", ".join(f"mods/{m}" for m in p.mods) or ("per level" if p.level else "")
        print(f"  {p.name:12} {p.gives}")
        print(f"  {'':12} needs: {', '.join(expand([p.name])[:-1]) or 'the maps'}; uses: "
              f"{', '.join(sorted(p.uses)) or 'Python'}{'; ' + where if where else ''}"
              f"{'; not in all (name it)' if p.off else ''}")
    print("\n  characters   " + ", ".join(CHARACTERS))
    print("  levels       " + ", ".join(LEVEL_PACKS) + " (of --levels)")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    parser.add_argument("command", choices=["check", "models", "list", "build", "status"])
    parser.add_argument("packs", nargs="*")
    parser.add_argument("--data", default="")
    parser.add_argument("--out", default="")
    parser.add_argument("--work", default="")
    parser.add_argument("--levels", default="")
    parser.add_argument("--blender", default="")
    parser.add_argument("--invader", default="")
    parser.add_argument("--extractor", default="")
    parser.add_argument("--game", default="")
    parser.add_argument("--upscalers", default="")
    parser.add_argument("--gpu", default="")
    parser.add_argument("--vulkan-gpu", default="")
    parser.add_argument("--torch", default="")
    parser.add_argument("--lightmap-samples", type=int, default=1024)
    parser.add_argument("--again", action="append", default=[], help="build this pack anew (repeat)")
    parser.add_argument("--yes", action="store_true", help="models: accept every licence shown")
    a = parser.parse_args(argv)
    if a.command == "list":
        return list_packs()
    if a.command == "models" and not a.data and a.upscalers:
        return 0 if packs_fetch.fetch_all(Path(a.upscalers), list(packs_fetch.DOWNLOADS), a.yes) else 1
    if not a.data:
        parser.error("--data <the game's data folder, with maps/ in it> is needed")
    ctx = Context(a)
    if a.command == "models":
        return 0 if packs_fetch.fetch_all(ctx.upscalers, list(packs_fetch.DOWNLOADS), a.yes) else 1
    if a.command == "status":
        return status(ctx)
    if a.command == "check":
        plan = expand(a.packs)
        ctx.plan = plan
        problems = check(ctx, plan)
        print(f"packs: {', '.join(plan)}")
        for problem in problems:
            print("  missing: " + problem)
        print("nothing missing" if not problems else f"{len(problems)} things missing")
        return 1 if problems else 0
    return build(ctx, a.packs)


if __name__ == "__main__":
    sys.exit(main())
