"""A key's cap in the prompts and the menus' footers (port/linux/game/hud_key_prompts.c, issue 596): its name's
letters in the middle of the box with as much room either side, for any name and font, by their ink as it is drawn
(the high-res text's glyphs, issue 681), in fractions of a unit (their outline, not its whole units), and its middle
the capitals' (half a unit lower for an odd number of rows); the text after it a gap past
the box; a legend's "=" ("[Esc] = CANCEL") as far from the cap as from its word; the advance the menus' legends lay
their rows out with (source/interface/ui_widget.c) the drawing's; and the legends measuring with it, not a width of
their own."""

import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run  # noqa: E402

CASES = ["centred", "drawn", "fraction", "capitals", "gap", "advance", "wrapped", "equals", "equals-advance",
         "equals-none"]

SOURCE = "port/linux/game/hud_key_prompts.c"
FUNCTIONS = ("key_cap_metrics", "key_cap_layout", "key_cap_equals_layout", "key_text_measure", "key_text_span", "key_equals_length", "hud_key_prompt_cap_advance",
             "hud_key_prompt_legend_advance", "key_cap_draw", "hud_key_prompt_draw_equals")

# a fault in the code, and the case that must catch it
NEGATIVE_CONTROLS = {
    # the name drawn from the margin past the cursor, as before issue 596: its letters off the box's middle
    "name-at-margin": (("*pen = (short)(inset + (width - letters) / 2 - left);", "*pen = margin;"), "centred"),
    # a box an odd unit wider than the letters: half a unit more room on one side
    "odd-room": (("if ((width - letters) % 2)\n\t\twidth++;", "if (0)\n\t\twidth++;"), "centred"),
    # a narrow name's box taller than wide
    "no-least-width": (("width = (short)MAX(letters + 2 * margin, 2 * half - 1);",
                        "width = (short)(letters + 2 * margin);"), "centred"),
    # the cap laid out about the font tag's bitmaps, not the letters' ink as drawn (issue 681's Q and E)
    "cap-about-bitmaps": (("draw_unicode_string_compute_drawn_bounds(bounds, text, &text_bounds, cursor);",
                           "draw_unicode_string_compute_bounds(bounds, text, &text_bounds, cursor);"), "drawn"),
    # the text after the cap started inside its edge, as before issue 596
    "no-gap": (("*after = (short)(*box_x1 + gap - left);", "*after = (short)(*box_x1 - inset + 1);"), "gap"),
    # the legends' advance not the drawing's
    "advance-is-box": (("\treturn after;", "\treturn box_x1;"), "advance"),
    # the word against the "=", as before the follow-up to issue 596 ("[Esc] =CANCEL")
    "equals-against-word": (("*word_pen = (short)(*equals_pen + equals_advance + KEY_CAP_EQUALS_ROOM);",
                             "*word_pen = (short)(*equals_pen + equals_advance);"), "equals"),
    # the "=" against the cap
    "equals-against-cap": (("*equals_pen = (short)(KEY_CAP_EQUALS_ROOM - gap);", "*equals_pen = (short)-gap;"),
                           "equals"),
    # the legends laid out without the "='s" room: the row's width not the drawing's
    "advance-without-equals": (("if (equals && key_text_measure(", "if (0 && key_text_measure("), "equals-advance"),
    # an "=" spaced where no cap was just drawn on its line
    "equals-any-line": (("if (!equals || cursor->y0 != key_cap_after_line)", "if (!equals)"), "equals-none"),
    # the cap about the letters' whole units, not their outline (issue 596: "Esc" nearer its cap's right)
    "cap-about-whole-units": (("if (!draw_unicode_string_drawn_span(&ink_left, &ink_right))", "if (1)"), "fraction"),
    # the cap's middle the capitals' whole unit, as before: half a unit off for an odd number of rows
    "cap-on-whole-unit": (("capitals_middle = (real)middle + (draw_string_capital_height() % 2 ? 0.5f : 0.0f);",
                           "capitals_middle = (real)middle;"), "capitals"),
    # the cap half a unit below the capitals, as before (its top a unit nearer them than its bottom)
    "cap-half-low": (("capitals_middle = (real)middle + (draw_string_capital_height() % 2 ? 0.5f : 0.0f);",
                      "capitals_middle = (real)middle + 0.5f;"), "capitals"),
}


def generated(fault=None):
    source = read(SOURCE)
    # (the last cap's place, which the "=" is spaced from)
    state = "\n".join(re.findall(r"^static short key_cap_(?:after_line|letters_x)\b[^\n]*;$", source, re.M))
    assert state.count("static short") == 2
    state += "\n" + re.search(r"^#define KEY_CAP_EQUALS_ROOM [^\n]+$", source, re.M).group(0)
    code = state + "\n\n" + "\n\n".join(function(source, name) for name in FUNCTIONS)
    if fault:
        code = mutated(code, *fault)
    return (("under_test.inc", code + "\n"),)


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("key_caps", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("key_caps", generated(fault)), case)
    assert status == CHECK_FAILED, f"the code with a fault ({control}) passed '{case}': the test cannot see it"


def test_legends_lay_out_with_the_drawing():
    """The pause menu's legend row and the help screens' key in a gap lay the cap out with
    hud_key_prompt_cap_advance, from the key's name alone, not a width of their own."""
    widgets = read("source/interface/ui_widget.c")
    measure = function(widgets, "ui_widget_port_legend_measure")
    assert re.search(r"ui_widget_port_definition_text_span\(words_definition, name, &name_left, &name_right\);\s*"
                     r"legend->icon_left = icon_left;\s*legend->words_end = [^;]+;\s*"
                     r"legend->width = hud_key_prompt_legend_advance\(name_left, name_right, words\);",
                     measure), "the legend row is not laid out with the cap's and its words' advance"
    # (the row as drawn and as the pause boxes are made wider for it measure the same way)
    assert "ui_widget_port_legend_measure(&legends[count]" in function(widgets, "ui_widget_port_key_legend_shift")
    assert "ui_widget_port_legend_measure(&legends[count]" in function(widgets, "ui_widget_port_key_legend_growth")
    extra = function(widgets, "ui_widget_port_key_label_cap_extra")
    assert "hud_key_prompt_cap_advance(left, right) - right" in extra
    assert "hud_key_prompt_cap_widening" not in widgets


def test_icons_space_the_equals():
    """Text with icons (the legends' "%b-button=CANCEL") spaces an "=" after a key cap, between the icons and after
    the last, and only after a cap."""
    drawing = function(read("source/interface/ui_widget.c"), "draw_string_and_hack_in_icons")
    assert drawing.count("hud_key_prompt_draw_equals(bounds, &cursor_bounds, current)") == 2
    assert drawing.count("key_drawn = TRUE;") == 4
    assert re.search(r"if \(key_drawn\)\s*current \+= hud_key_prompt_draw_equals", drawing)


def test_drawn_bounds_are_the_glyphs_places():
    """The letters' drawn bounds a cap is laid out about (draw_unicode_string_compute_drawn_bounds) are where the
    high-res text draws its glyphs: rasterizer_text_port_drawn_span places a glyph as rasterizer_draw_hires_glyph
    does (its advance centred on the font tag character's), for the font it is drawn in; the cap and the legends'
    rows measure with them."""
    text = read("source/rasterizer/rasterizer_text.c")
    span = function(text, "rasterizer_text_port_drawn_span")
    glyph = function(text, "rasterizer_draw_hires_glyph")
    for placed in ("x0 - x + font_character->bitmap_origin_x", "(font_character->character_width - glyph.advance) * 0.5f + glyph.left"):
        assert placed in span and placed in glyph, placed
    assert "hires_text_font_get(draw_string_get_font(), MAX(text_scale, 1.0f))" in span
    assert "hires_text_font_get(draw_string_get_font(), MAX(text_scale, 1.0f))" in text.split("rasterizer_draw_string(", 2)[-1]
    bounds = function(read("source/text/draw_string.c"), "text_bounds_draw_character")
    drawn = bounds.split("if (text_bounds_globals.drawn)", 1)[1].split("if (x0 < text_bounds_globals.bounds.x0)")[0]
    assert "rasterizer_text_port_drawn_span(character, x0, x, &x0, &x1);" in drawn
    # (and their outline in fractions of a unit, before x0 is the ink's: issue 596's caps centred on it)
    assert drawn.index("rasterizer_text_port_drawn_span_real(character, x0, x, &left, &right);") < \
        drawn.index("rasterizer_text_port_drawn_span(character, x0, x, &x0, &x1);")
    real = function(text, "rasterizer_text_port_drawn_span_real")
    for placed in ("x0 - x + font_character->bitmap_origin_x",
                   "(font_character->character_width - glyph.advance) * 0.5f + glyph.left",
                   "glyph.ink_left - glyph.left", "glyph.ink_right - glyph.left"):
        assert placed in real, placed
    assert "stbtt_GetGlyphBox(&face->info, index, &ink_x0, NULL, &ink_x1, NULL)" in read("port/linux/src/text_hires.c")
    assert "draw_unicode_string_drawn_span(&ink_left, &ink_right)" in function(read(SOURCE), "key_cap_draw")
    caps = read(SOURCE)
    assert "draw_unicode_string_compute_drawn_bounds(bounds, text, &text_bounds, cursor);" in function(caps, "key_cap_draw")
    assert "draw_unicode_string_compute_drawn_bounds(" in function(caps, "key_text_measure")
    assert "draw_unicode_string_compute_drawn_bounds(" in function(read("source/interface/ui_widget.c"),
                                                                  "ui_widget_port_definition_text_span")
