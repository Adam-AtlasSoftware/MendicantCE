"""Forge's map variant files (port/linux/game/forge_mvar.c): read back as written, damage refused, later versions'
chunks and records skipped, nothing read past the end; and the quaternion of forge_layout.c gives back the axes.

From SaftiGlas's Forge (https://github.com/SaftiGlas/halo-ce-universal,
main 173fc93ae), AGPL-3.0-or-later; ported to OpenH1 (CREDITS.md).
"""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run  # noqa: E402

CASES = ["round-trip", "refuses-damage", "chunk-bounds", "unknown-chunks", "longer-records", "table-bounds",
         "strings", "overflow", "quaternion", "bad-quaternion"]

# a fault in forge_mvar.c (or forge_layout.c's function), and the case that must catch it
NEGATIVE_CONTROLS = {
    "no-crc": (("forge_mvar_get_u32(reader) != forge_mvar_crc32(reader->data + blf_size, length - blf_size))",
                "(forge_mvar_get_u32(reader) & 0) != (forge_mvar_crc32(reader->data + blf_size, length - blf_size) & 0))"),
               None, "refuses-damage"),
    "reads-past-the-end": (("if (reader->failed || reader->length - reader->offset < count)\n\t{\n\t\treader->failed = 1;\n"
                            "\t\treturn NULL;",
                            "if (reader->failed)\n\t{\n\t\treader->failed = 1;\n\t\treturn NULL;"),
                           None, "chunk-bounds"),
    "table-unchecked": (("*count > (reader->length - reader->offset) / *record_size", "0"), None, "table-bounds"),
    "record-end-not-skipped": (("if (!reader->failed && record_size > reader->offset - start)\n"
                                "\t\tforge_mvar_get_bytes(reader, record_size - (reader->offset - start));\n\n"
                                "\treturn !reader->failed;\n}\n\nint forge_mvar_get_editor(",
                                "\n\treturn !reader->failed;\n}\n\nint forge_mvar_get_editor("),
                               None, "longer-records"),
    "string-too-long": (("if (length >= size)", "if (length > size)"), None, "strings"),
    "overflow-unchecked": (("if (writer->overflowed || writer->size - writer->length < count)\n\t{\n"
                            "\t\twriter->overflowed = 1;\n\t\treturn;\n\t}\n\tforge_mvar_put_at(",
                            "if (writer->overflowed)\n\t{\n\t\twriter->overflowed = 1;\n\t\treturn;\n\t}\n"
                            "\tif (writer->size - writer->length < count)\n\t\treturn;\n\tforge_mvar_put_at("),
                           None, "overflow"),
    "forward-mirrored": (("2.f * (x * y + w * z)", "2.f * (x * y - w * z)"), "forge_layout_axes_of_rotation",
                         "quaternion"),
    "w-negative": (("(rotation[3] < 0.f ? -1.f : 1.f)", "1.f"), "forge_layout_rotation_of_axes", "quaternion"),
    "length-unchecked": (("!(length_squared > 0.5f && length_squared < 2.f)", "!(length_squared > 0.f)"),
                         "forge_layout_axes_of_rotation", "bad-quaternion"),
}


def generated(fault=None, faulty_function=None):
    header = read("port/linux/game/forge_mvar.h")
    source = read("port/linux/game/forge_mvar.c")
    source = source.replace('#include "forge_mvar.h"\n', "")
    if fault and faulty_function is None:
        source = mutated(source, *fault)
    layout = read("port/linux/game/forge_layout.c")
    text = header + "\n" + source + "\n"
    for name in ("forge_layout_rotation_of_axes", "forge_layout_axes_of_rotation"):
        code = function(layout, name)
        if fault and name == faulty_function:
            code = mutated(code, *fault)
        text += code + "\n"
    return (("under_test.inc", text),)


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("forge_mvar", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, faulty_function, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("forge_mvar", generated(fault, faulty_function)), case)
    assert status == CHECK_FAILED, f"the code with a fault ({control}) passed '{case}': the test cannot see it"
