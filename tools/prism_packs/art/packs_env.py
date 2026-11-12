"""Where PRISM's pack tools read and write, for every script of art/ (plain Python and Blender's alike).

prism_packs.py sets these in the environment before it runs a step; a script run by hand needs them too:

  PRISM_PACKS_WORK       the work folder: extract/ (map_extract's), dump/ (the game's model dumps) and every
                         step's own folder. Made of the player's maps: it stays on the player's computer.
  PRISM_PACKS_DATA       the game's data folder (maps/ in it)
  PRISM_PACKS_OUT        where the packs are written, laid out as the game's data folder is (mods/<id>,
                         prism_world/, prism_lightmaps/, prism/probes/): the data folder by default
  PRISM_PACKS_EXTRACTOR  the extractor (bin/openh1-map-extract, or tools/x86_game_abi.py --map-extract's)
  PRISM_PACKS_INVADER    the folder of Invader's programs (invader-bitmap, invader-model, ...)
  PRISM_PACKS_UPSCALERS  the folder of the upscalers the player fetched (prism_packs.py models): the
                         realesrgan-ncnn-vulkan program and its models, and the PyTorch weights
  PRISM_PACKS_TOOLS      the folder of the game's own pack format tools (pmesh.py, prism_rig.py,
                         prism_material_pack.py, texstore_synth.py)
  PRISM_PACKS_GPU        the graphics card for Cycles, by part of its name ("" for the first one found)
  PRISM_PACKS_VULKAN_GPU the graphics card for the Vulkan upscaler, by its number ("" for the first)
  PRISM_PACKS_TORCH      the PyTorch device ("cuda", "cuda:1", "cpu")

Nothing here is made of the game's data; what the scripts write from it is the player's alone.
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def _folder(name, default=""):
    value = os.environ.get(name, default)
    return os.path.abspath(value) if value else ""


WORK = _folder("PRISM_PACKS_WORK")
DATA = _folder("PRISM_PACKS_DATA")
OUT = _folder("PRISM_PACKS_OUT") or DATA
EXTRACTOR = _folder("PRISM_PACKS_EXTRACTOR")
INVADER = _folder("PRISM_PACKS_INVADER")
UPSCALERS = _folder("PRISM_PACKS_UPSCALERS")
TOOLS = _folder("PRISM_PACKS_TOOLS", os.path.join(os.path.dirname(os.path.dirname(HERE))))
GPU = os.environ.get("PRISM_PACKS_GPU", "")
VULKAN_GPU = os.environ.get("PRISM_PACKS_VULKAN_GPU", "")
TORCH_DEVICE = os.environ.get("PRISM_PACKS_TORCH", "cuda")
EXE = ".exe" if os.name == "nt" else ""


def need(value, name):
    if not value:
        sys.exit("%s is not set: run this through prism_packs.py, or set it (art/packs_env.py)" % name)
    return value


def work(*parts):
    """a path in the work folder"""
    return os.path.join(need(WORK, "PRISM_PACKS_WORK"), *parts)


def extract(*parts):
    """a path in the extract (<work>/extract/<map>/...: map_extract's folders)"""
    return work("extract", *parts)


def maps(*parts):
    """a path in the data folder's maps/"""
    return os.path.join(need(DATA, "PRISM_PACKS_DATA"), "maps", *parts)


def out(*parts):
    """a path where the packs go (the data folder's layout)"""
    return os.path.join(need(OUT, "PRISM_PACKS_OUT"), *parts)


def mod(mod_id, *parts):
    """a path in a pack's mod folder (<out>/mods/<id>)"""
    return out("mods", mod_id, *parts)


def invader(program):
    """an Invader program's path"""
    return os.path.join(need(INVADER, "PRISM_PACKS_INVADER"), program + EXE)


def invader_env():
    """the environment Invader's programs run in (a build of it from source keeps libinvader beside bin/)"""
    env = dict(os.environ)
    lib = os.path.join(os.path.dirname(INVADER), "lib")
    if os.path.isdir(lib):
        env["LD_LIBRARY_PATH"] = lib + os.pathsep + env.get("LD_LIBRARY_PATH", "")
    return env


def upscaler(*parts):
    """a path in the upscalers' folder"""
    return os.path.join(need(UPSCALERS, "PRISM_PACKS_UPSCALERS"), *parts)


def scratch(*parts):
    """a folder for a step's temporary files, in the work folder"""
    path = work("tmp", *parts)
    os.makedirs(path, exist_ok=True)
    return path


def path(rel):
    """a path a recipe names: {work}, {extract} and {art} (this folder) filled in, else as it is (a relative
    one is the work folder's)"""
    rel = rel.replace("{work}", WORK).replace("{extract}", os.path.join(WORK, "extract")).replace("{art}", HERE)
    return rel if os.path.isabs(rel) else os.path.join(need(WORK, "PRISM_PACKS_WORK"), rel)


CYCLES_BACKENDS = ("OPTIX", "CUDA", "HIP", "ONEAPI", "METAL")


def cycles_gpu(prefs, name=None):
    """Cycles' graphics cards (inside Blender: prefs is the cycles add-on's preferences): the first backend
    that has a card whose name holds name (PRISM_PACKS_GPU's when None; "" any), its cards of that name turned
    on and the rest off. Returns (backend, [card names]), or (None, []): Cycles then renders on the processor,
    which is very much slower but makes the same pictures"""
    name = (GPU if name is None else name).lower()
    want = os.environ.get("PRISM_PACKS_CYCLES", "")
    for backend in ((want,) if want else CYCLES_BACKENDS):
        try:
            prefs.compute_device_type = backend
            prefs.get_devices()
        except Exception:
            continue
        cards = [d for d in prefs.devices if d.type == backend and name in d.name.lower()]
        if not cards:
            continue
        for d in prefs.devices:
            d.use = d in cards
        return backend, [d.name for d in cards]
    return None, []


def extractor_command(*args):
    """the extractor's command line: on Windows the Linux program through WSL, its path arguments as WSL sees them"""
    program = need(EXTRACTOR, "PRISM_PACKS_EXTRACTOR")
    if os.name != "nt":
        return [program, *map(str, args)]
    import subprocess

    def wsl(argument):
        argument = str(argument)
        if len(argument) > 2 and argument[1] == ":":
            return subprocess.run(["wsl.exe", "wslpath", "-a", argument], capture_output=True, text=True,
                                  check=True).stdout.strip()
        return argument
    return ["wsl.exe", "-e", wsl(program), *map(wsl, args)]
