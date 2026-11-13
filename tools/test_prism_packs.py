"""fork: PRISM's pack tools (tools/prism_packs, docs/prism_packs.md) without the game's data: their entry point,
their packs' order, the files a package carries of them, and the release gate that keeps the game's data out of
a package (tools/fork_gate.py's check_game_data)."""

import json
import os
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tools" / "prism_packs"))

import fork_gate  # noqa: E402
import linux_package  # noqa: E402
import packs_fetch  # noqa: E402
import prism_packs  # noqa: E402

PACKS = ROOT / "tools" / "prism_packs"


def test_the_entry_point_lists_the_packs():
    result = subprocess.run([sys.executable, PACKS / "prism_packs.py", "list"], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    for name in prism_packs.CHARACTERS + prism_packs.LEVEL_PACKS:
        assert name in result.stdout


def test_a_pack_comes_after_what_it_needs():
    for pack in prism_packs.PACKS:
        order = prism_packs.expand([pack.name])
        assert order[-1] == pack.name or pack.name in order
        for need in pack.needs:
            assert order.index(need) < order.index(pack.name)
    assert "sun" not in prism_packs.expand(["all"]) and "probes" not in prism_packs.expand(["all"])
    with pytest.raises(SystemExit):
        prism_packs.expand(["no_such_pack"])


def test_every_pack_has_its_step_and_its_mods_their_place_in_the_load_order():
    assert set(prism_packs.STEPS) == {p.name for p in prism_packs.PACKS}
    for pack in prism_packs.PACKS:
        for mod in pack.mods:
            assert mod in prism_packs.LOAD_ORDER
    assert prism_packs.LOAD_ORDER[-1] == "prism_v2_tex_bold"


def test_the_load_order_keeps_the_players_mods(tmp_path):
    mods = tmp_path / "mods"
    for mod in ("mendicant_ai", "prism_v2_tex_bold", "prism_base_local", "prism_v3_elite_meshes"):
        (mods / mod).mkdir(parents=True)
        (mods / mod / "mod.toml").write_text('id = "x"\n')
    (mods / "load_order.toml").write_text('order = ["mendicant_ai", "prism_v2_tex_bold"]\ndisabled = ["other"]\n')

    class Context:
        out = tmp_path
    prism_packs.load_order(Context())
    text = (mods / "load_order.toml").read_text()
    order = json.loads(text.split("order = ")[1].split("\n")[0])
    disabled = json.loads(text.split("disabled = ")[1].split("\n")[0])
    assert order == ["mendicant_ai", "prism_base_local", "prism_v3_elite_meshes", "prism_v2_tex_bold"]
    assert disabled == ["other", "prism_v3_elite_meshes"]


def test_recipes_name_no_folder_of_a_computer():
    """a recipe's paths are the work folder's, the extract's or the tools' own ({work}, {extract}, {art})"""
    for path in PACKS.rglob("*.json"):
        def walk(value):
            if isinstance(value, dict):
                for item in value.values():
                    yield from walk(item)
            elif isinstance(value, list):
                for item in value:
                    yield from walk(item)
            elif isinstance(value, str):
                yield value
        for text in walk(json.loads(path.read_text())):
            assert not text.startswith(("/", "work/", "art_pipeline/")), f"{path}: {text}"
            assert ":\\" not in text, f"{path}: {text}"


def test_the_tools_name_no_folder_of_a_computer():
    for path in PACKS.rglob("*"):
        if path.is_file() and path.suffix in (".py", ".json", ".md", ".txt", ".c", ".h"):
            text = path.read_text(encoding="utf-8")
            for private in ("/home/", "/Users/", "C:\\Users"):
                assert private not in text, f"{path} names {private}"


def test_the_upscalers_are_fetched_from_their_authors_with_their_licences():
    for name, entry in packs_fetch.DOWNLOADS.items():
        for key in ("any", "linux", "windows"):
            if key in entry:
                url, digest = entry[key]
                assert url.startswith("https://github.com/"), name
                assert len(digest) == 64 and int(digest, 16) >= 0, name
        assert entry["licences"] and all(licence in packs_fetch.LICENCES for licence in entry["licences"])
        assert entry["credit"]
    for names in packs_fetch.NEEDED_BY.values():
        assert set(names) <= set(packs_fetch.DOWNLOADS)


def test_a_fetch_asks_first(tmp_path):
    answers = []
    ok = packs_fetch.fetch_all(tmp_path, ["4xNomos8kSC"], accept=False, ask=lambda q: answers.append(q) or "n")
    assert not ok and answers and answers[0].strip().startswith("fetch 4xNomos8kSC")
    assert not any(tmp_path.iterdir())


def test_recipe_paths_are_filled_in(tmp_path, monkeypatch):
    monkeypatch.setenv("PRISM_PACKS_WORK", str(tmp_path))
    sys.path.insert(0, str(PACKS / "art"))
    import importlib
    import packs_env
    importlib.reload(packs_env)
    assert packs_env.path("{extract}/b30/x.gltf") == os.path.join(str(tmp_path), "extract", "b30/x.gltf")
    assert packs_env.path("rel/y") == os.path.join(str(tmp_path), "rel/y")


def test_a_package_carries_the_tools_and_they_pass_the_gate(tmp_path):
    linux_package.stage_pack_tools(tmp_path / "share" / "openh1" / "prism_packs")
    names = {p.relative_to(tmp_path).as_posix() for p in tmp_path.rglob("*") if p.is_file()}
    assert "share/openh1/prism_packs/prism_packs.py" in names
    assert "share/openh1/prism_packs/GUIDE.md" in names
    assert "share/openh1/prism_packs/formats/pmesh.py" in names
    assert not any(name.endswith((".c", ".h")) for name in names)
    assert fork_gate.check_game_data([tmp_path]) == []


@pytest.mark.parametrize("name, data, reason", [
    ("maps/b30.map", b"daeh" + bytes(60), "begins as the game's data"),
    ("x/cyborg.gbxmodel", bytes(60) + b"blam" + bytes(8), "begins as the game's data"),
    ("x/rock.pmesh", b"PMSH" + bytes(16), "begins as the game's data"),
    ("x/b30a.pwg", b"PWG1" + bytes(16), "begins as the game's data"),
    ("x/a10_0_0.prl", b"PRL1" + bytes(16), "begins as the game's data"),
    ("x/armor.ktx2", b"\xabKTX 20\xbb\r\n\x1a\n" + bytes(8), "begins as the game's data"),
    ("x/model.jms", b"8200\n3251\n", "begins as the game's data"),
    ("prism_packs/art/x.png", b"\x89PNG\r\n\x1a\n", "not code, a recipe or text"),
    ("prism_packs/art/table.json", json.dumps({"v": list(range(100))}).encode(), "a table, not a recipe"),
    ("prism_packs/art/many.json", json.dumps({"v": [[i, i] for i in range(3000)]}).encode(), "a table, not a recipe"),
    ("prism_packs/art/table.py", ("V = [" + ", ".join(str(i) for i in range(100)) + "]\n").encode(),
     "a table, not code"),
])
def test_the_gate_refuses_the_games_data(tmp_path, name, data, reason):
    path = tmp_path / name
    path.parent.mkdir(parents=True)
    path.write_bytes(data)
    problems = fork_gate.check_game_data([tmp_path])
    assert problems and reason in problems[0], problems
