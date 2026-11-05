"""An older version's word for a setting (port/linux/src/port_config.c's config_renamed_words): display.ambient_occlusion's
"subtle" and "strong" (the screen's occlusion, gone) read as "world", in config.toml (written again with it:
config_migrate_words) and from the environment (config_check_words), in any case; "off", "world" and the rest as
they are."""

import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, enum_with, function, mutated, read, run, structure  # noqa: E402

CASES = ["file", "kept", "read"]
NAMES = ["config_setting_index", "config_append", "config_copy", "config_append_setting", "config_line_key",
         "config_line_section", "config_find_line", "config_line_commented_default", "config_replace_setting",
         "config_word_is", "config_renamed_word", "config_migrate_words", "config_check_words"]

# a fault in the code, the function it is in, and the case that must catch it
NEGATIVE_CONTROLS = {
    # (the older words kept: neither the file nor the environment's)
    "file-kept": (("if (setting < 0 || datum.type != TOML_STRING ||", "if (1 || setting < 0 || datum.type != TOML_STRING ||"),
                  "config_migrate_words", "file"),
    "read-kept": (("if (renamed)", "if (0)"), "config_check_words", "read"),
    # (every value taken for an older word: "off" turned on)
    "every-word": (("config_word_is(value, strlen(value), config_renamed_words[index].old_word)", "1"),
                   "config_renamed_word", "read"),
    "every-file": (("!config_word_is(datum.u.s, strlen(datum.u.s), config_renamed_words[index].old_word)", "0"),
                   "config_migrate_words", "kept"),
    # (the old word written back)
    "old-written": (("word = config_renamed_words[index].word;", "word = config_renamed_words[index].old_word;"),
                    "config_migrate_words", "file"),
}


def table(source, name):
    """a static table's definition, with its anonymous structure"""
    match = re.search(r"^static const struct\n\{[^}]*\} " + re.escape(name) + r"\[\] =\n\{.*?^\};", source, re.M | re.S)
    if not match:
        raise LookupError(name)
    return match.group(0)


def generated(fault=None, faulty_function=None):
    source = read("port/linux/src/port_config.c")
    types = "\n\n".join([
        "#include <stdarg.h>", "#include \"tomlc17.h\"",
        enum_with(source, "_config_boolean,"), enum_with(source, "_environment_set_is_true,"),
        enum_with(source, "_platform_desktop = 1"), structure(source, "config_setting"),
        structure(source, "config_value"), structure(source, "config_text"),
    ]) + "\n"
    words = table(source, "config_words") + "\n#define NUMBER_OF_CONFIG_WORDS (sizeof(config_words) / sizeof(config_words[0]))\n"
    text = table(source, "config_renamed_words") + "\n" + words
    for name in NAMES:
        code = function(source, name)
        if fault and name == faulty_function:
            code = mutated(code, *fault)
        text += code + "\n"
    return (("tomlc17.h", read("port/third_party/tomlc17/tomlc17.h")),
            ("toml.inc", read("port/third_party/tomlc17/tomlc17.c")),
            ("types.inc", types), ("under_test.inc", text))


def test_the_words():
    # (the screen's ambient occlusion's words, gone, are the world's; the setting takes no other)
    source = read("port/linux/src/port_config.c")
    renamed = table(source, "config_renamed_words")
    assert '{ "display.ambient_occlusion", "subtle", "world" },' in renamed
    assert '{ "display.ambient_occlusion", "strong", "world" },' in renamed
    assert '{ "display.ambient_occlusion", { "off", "world", NULL }, "off" },' in table(source, "config_words")


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("config_renamed_words", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, faulty_function, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("config_renamed_words", generated(fault, faulty_function)), case)
    assert status == CHECK_FAILED, f"the code with a fault ({control}) passed '{case}': the test cannot see it"
