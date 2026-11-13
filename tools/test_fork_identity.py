"""fork: this fork's identity switch (tools/build_common.py's FORK_DEFINE) is
in every build, a build never names upstream's repository as its own, and the
release gate (tools/fork_gate.py) refuses what a build without the switch
holds."""

import re
from pathlib import Path
from types import SimpleNamespace

import pytest

from tools import build_common, fork_gate

ROOT = Path(__file__).resolve().parent.parent


@pytest.mark.parametrize("options", [{}, {"port_release": True}, {"port_release": True, "port_profile": True},
                                     {"port_release": True, "port_pgo": "generate"}, {"port_pgo": "use"}])
def test_every_configuration_has_the_fork_switch(options):
    assert build_common.FORK_DEFINE == "-DHALO_FORK_MENDICANT"
    assert build_common.FORK_DEFINE in build_common.configuration_defines(SimpleNamespace(**options))
    assert build_common.FORK_DEFINE in build_common.configuration_defines(None)


def test_a_build_is_never_upstreams(monkeypatch):
    monkeypatch.setenv("GITHUB_REPOSITORY", "OpenH1/OpenH1")
    assert build_common.build_repository() == build_common.FORK_REPOSITORY
    monkeypatch.setenv("GITHUB_REPOSITORY", "openh1/openh1")
    assert build_common.build_repository() == build_common.FORK_REPOSITORY
    monkeypatch.setenv("GITHUB_REPOSITORY", "Adam-AtlasSoftware/MendicantCE")
    assert build_common.build_repository() == "Adam-AtlasSoftware/MendicantCE"
    # (someone else's fork of this fork keeps its own name)
    monkeypatch.setenv("GITHUB_REPOSITORY", "someone/their-fork")
    assert build_common.build_repository() == "someone/their-fork"
    monkeypatch.setenv("GITHUB_REPOSITORY", "not a repository")
    assert build_common.build_repository() == build_common.FORK_REPOSITORY
    # (the checkout's origin, whatever it is: never upstream's, never none)
    monkeypatch.delenv("GITHUB_REPOSITORY")
    assert build_common.build_repository() not in ("", build_common.UPSTREAM_REPOSITORY)
    assert f'HALO_BUILD_REPOSITORY=\\"{build_common.UPSTREAM_REPOSITORY}\\"' not in build_common.updater_defines(True)


def test_the_updater_is_left_out_under_the_switch():
    """updater.c drops its platform under the switch before anything is
    compiled for one, and says why where a build has none"""
    text = (ROOT / "port/linux/src/updater.c").read_text()
    guard = text.index("#ifdef HALO_FORK_MENDICANT\n#undef UPDATE_PLATFORM\n#undef UPDATE_NAME\n#undef UPDATER_PACKAGE\n")
    assert text.rindex("#define UPDATE_NAME ", 0, text.index("#ifdef UPDATE_PLATFORM\n")) < guard
    assert guard < text.index("#ifdef UPDATE_PLATFORM\n")
    assert len(re.findall(r"^#define UPDATE_(PLATFORM|NAME) ", text[guard:], re.M)) == 0
    assert text.count("this fork's builds do not update themselves") >= 3


def test_the_switch_names_the_build_and_keeps_its_crashes():
    main = (ROOT / "source/main/main.c").read_text()
    assert '#ifdef HALO_FORK_MENDICANT\n#define HALO_BUILD_PRODUCT "MendicantCE"\n#else' in main
    assert '"OpenH1 %s | ' not in main and '"OpenH1 | ' not in main
    crash = (ROOT / "port/windows/host/host_crash_win.c").read_text()
    enabled = crash[crash.index("static int crash_reports_enabled(void)"):]
    enabled = enabled[:enabled.index("\n}\n")]
    assert re.search(r"#ifdef HALO_FORK_MENDICANT\n(\t/\*.*?\*/\n)?\treturn 0;\n#else", enabled, re.S), enabled


def test_the_gate_refuses_an_upstream_build(tmp_path):
    good = tmp_path / "good"
    (good / "bin").mkdir(parents=True)
    (good / "bin" / "openh1").write_bytes(b"\x7fELF...MendicantCE %s | commit %s (%s)\0...")
    (good / "README.md").write_text("OpenH1-Linux.zip is upstream's; /releases/latest\n")
    assert fork_gate.check([good]) == []
    for text in fork_gate.FORBIDDEN:
        for encoded in fork_gate.encodings(text):
            bad = tmp_path / "bad"
            bad.mkdir(exist_ok=True)
            (bad / "halo.exe").write_bytes(b"MZ..MendicantCE %s | " + encoded + b"\0")
            problems = fork_gate.check([bad])
            assert len(problems) == 1 and text in problems[0], (text, problems)
    unnamed = tmp_path / "unnamed"
    unnamed.mkdir()
    (unnamed / "halo.exe").write_bytes(b"MZ..OpenH1 %s | local build (%s)\0")
    assert any("not a build of this fork" in problem for problem in fork_gate.check([unnamed]))
    assert fork_gate.check([tmp_path / "nothing"]) == ["no files to check"]


def test_a_built_game_passes_the_gate():
    """the build in this tree, where there is one"""
    build = ROOT / "build/linux_x86_64"
    files = [build / "halo", build / "halo_guest.elf"]
    if not all(path.is_file() for path in files):
        pytest.skip("needs ninja linux_x86_64's build")
    assert fork_gate.check(files) == []
