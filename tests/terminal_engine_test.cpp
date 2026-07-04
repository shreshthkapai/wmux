#include "wmux/terminal_engine.hpp"

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

[[noreturn]] void fail_engine_golden(std::string_view message) {
  std::cerr << "terminal engine golden failure: " << message << "\n";
  std::exit(1);
}

void expect_engine_golden(bool condition, std::string_view message) {
  if (!condition) {
    fail_engine_golden(message);
  }
}

void feed_text(wmux::ITerminalEngine& engine, std::string_view text) {
  const auto* bytes = reinterpret_cast<const std::byte*>(text.data());
  engine.feed(std::span<const std::byte>{bytes, text.size()});
}

bool attributes_equal(const wmux::TerminalAttributes& left, const wmux::TerminalAttributes& right) {
  return left.bold == right.bold && left.dim == right.dim && left.italic == right.italic &&
         left.underline == right.underline && left.inverse == right.inverse &&
         left.foreground == right.foreground && left.background == right.background;
}

void assert_line_snapshots_equal(
    const std::vector<wmux::TerminalLineSnapshot>& legacy_lines,
    const std::vector<wmux::TerminalLineSnapshot>& v2_lines) {
  assert(v2_lines.size() == legacy_lines.size());
  for (std::size_t row = 0; row < legacy_lines.size(); ++row) {
    const auto& legacy = legacy_lines[row];
    const auto& v2 = v2_lines[row];
    expect_engine_golden(v2.text == legacy.text, "line text mismatch");
    expect_engine_golden(v2.wrapped == legacy.wrapped, "wrapped flag mismatch");
    expect_engine_golden(v2.cells == legacy.cells, "cell text mismatch");
    expect_engine_golden(v2.cell_widths == legacy.cell_widths, "cell width mismatch");
    expect_engine_golden(v2.attributes.size() == legacy.attributes.size(), "attribute size mismatch");
    for (std::size_t column = 0; column < legacy.attributes.size(); ++column) {
      expect_engine_golden(
          attributes_equal(v2.attributes[column], legacy.attributes[column]),
          "cell attributes mismatch");
    }
  }
}

void assert_screen_snapshots_equal(
    const wmux::TerminalScreenSnapshot& legacy,
    const wmux::TerminalScreenSnapshot& v2) {
  expect_engine_golden(v2.columns == legacy.columns, "column count mismatch");
  expect_engine_golden(v2.rows == legacy.rows, "row count mismatch");
  expect_engine_golden(v2.cursor_column == legacy.cursor_column, "cursor column mismatch");
  expect_engine_golden(v2.cursor_row == legacy.cursor_row, "cursor row mismatch");
  expect_engine_golden(v2.cursor_visible == legacy.cursor_visible, "cursor visibility mismatch");
  expect_engine_golden(v2.cursor_style == legacy.cursor_style, "cursor style mismatch");
  expect_engine_golden(v2.origin_mode == legacy.origin_mode, "origin mode mismatch");
  expect_engine_golden(v2.wrap_mode == legacy.wrap_mode, "wrap mode mismatch");
  expect_engine_golden(
      v2.bracketed_paste_mode == legacy.bracketed_paste_mode,
      "bracketed paste mode mismatch");
  expect_engine_golden(v2.alternate_screen == legacy.alternate_screen, "alternate screen mismatch");
  expect_engine_golden(v2.title == legacy.title, "title mismatch");
  expect_engine_golden(
      v2.scrollback_line_count == legacy.scrollback_line_count,
      "scrollback count mismatch");
  expect_engine_golden(v2.lines == legacy.lines, "visible lines mismatch");
  assert_line_snapshots_equal(legacy.line_snapshots, v2.line_snapshots);
}

void assert_scrollback_snapshots_equal(
    const wmux::TerminalScrollbackSnapshot& legacy,
    const wmux::TerminalScrollbackSnapshot& v2) {
  expect_engine_golden(v2.capacity == legacy.capacity, "scrollback capacity mismatch");
  expect_engine_golden(v2.total_lines == legacy.total_lines, "scrollback total mismatch");
  expect_engine_golden(
      v2.first_line_index == legacy.first_line_index,
      "scrollback first line mismatch");
  expect_engine_golden(v2.partial == legacy.partial, "scrollback partial mismatch");
  expect_engine_golden(v2.lines == legacy.lines, "scrollback lines mismatch");
  assert_line_snapshots_equal(legacy.line_snapshots, v2.line_snapshots);
}

struct GoldenEngineCase {
  std::string_view name;
  int columns;
  int rows;
  std::vector<std::string_view> feeds;
};

void run_golden_engine_case(const GoldenEngineCase& test_case) {
  wmux::LegacyTerminalEngine legacy{test_case.columns, test_case.rows};
  wmux::TerminalEngineV2 v2{test_case.columns, test_case.rows};
  legacy.set_scrollback_capacity(8);
  v2.set_scrollback_capacity(8);

  for (const auto feed : test_case.feeds) {
    feed_text(legacy, feed);
    feed_text(v2, feed);
  }

  const auto legacy_screen = legacy.snapshot();
  const auto v2_screen = v2.snapshot();
  if (legacy_screen.lines != v2_screen.lines) {
    std::cerr << "terminal engine golden mismatch: " << test_case.name << "\n";
    for (std::size_t index = 0; index < legacy_screen.lines.size(); ++index) {
      std::cerr << "legacy[" << index << "]=" << legacy_screen.lines[index] << "\n";
      std::cerr << "v2[" << index << "]=" << v2_screen.lines[index] << "\n";
    }
  }
  assert_screen_snapshots_equal(legacy_screen, v2_screen);
  assert_scrollback_snapshots_equal(legacy.scrollback_snapshot(), v2.scrollback_snapshot());
}

void terminal_engine_v2_matches_legacy_golden_cases() {
  const std::string cjk_and_combining =
      std::string{"A"} + "\xe4\xb8\xad" + "e\xcc\x81" + "Z";

  const std::vector<GoldenEngineCase> cases{
      {"plain shell output", 24, 3, {"PS C:\\Users\\shres> dir\r\nfile.txt\r\n"}},
      {"ansi colors", 16, 2, {"\x1b[31;1mred\x1b[0m \x1b[38;5;202morange\x1b[0m"}},
      {"cursor movement", 10, 3, {"abcde\x1b[2;4HX\x1b[1;2HY"}},
      {"clears", 12, 3, {"line1\r\nline2\r\nline3\x1b[2J\x1b[Hdone"}},
      {"alternate screen", 12, 3, {"normal\x1b[?1049halt\x1b[2;1Hview\x1b[?1049l"}},
      {"scrollback", 8, 3, {"one\r\ntwo\r\nthree\r\nfour\r\nfive"}},
      {"wide unicode", 8, 2, {cjk_and_combining}},
      {"wrapped lines", 5, 3, {"abcdeXYZ"}},
  };

  for (const auto& test_case : cases) {
    run_golden_engine_case(test_case);
  }
}

void terminal_engine_v2_matches_legacy_resize_golden_case() {
  wmux::LegacyTerminalEngine legacy{10, 2};
  wmux::TerminalEngineV2 v2{10, 2};
  feed_text(legacy, "alpha\r\nbeta");
  feed_text(v2, "alpha\r\nbeta");

  legacy.resize(6, 3);
  v2.resize(6, 3);
  feed_text(legacy, "\r\ngamma");
  feed_text(v2, "\r\ngamma");

  assert_screen_snapshots_equal(legacy.snapshot(), v2.snapshot());
  assert_scrollback_snapshots_equal(legacy.scrollback_snapshot(), v2.scrollback_snapshot());
}

void terminal_engine_v2_matches_legacy_complex_grapheme_golden_case() {
  wmux::LegacyTerminalEngine legacy{8, 1};
  wmux::TerminalEngineV2 v2{8, 1};

  const std::string emoji = "\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb";
  feed_text(legacy, emoji + "x");
  feed_text(v2, emoji + "x");

  assert_screen_snapshots_equal(legacy.snapshot(), v2.snapshot());
}

void terminal_engine_factory_defaults_to_legacy() {
#if defined(_MSC_VER)
  _putenv_s("WMUX_TERMINAL_ENGINE", "");
#else
  unsetenv("WMUX_TERMINAL_ENGINE");
#endif
  assert(wmux::terminal_engine_kind_from_environment() == wmux::TerminalEngineKind::Legacy);
  auto engine = wmux::make_terminal_engine(8, 3);
  feed_text(*engine, "ok");
  assert(engine->snapshot().lines[0].starts_with("ok"));
}

void terminal_engine_factory_accepts_v2() {
#if defined(_MSC_VER)
  _putenv_s("WMUX_TERMINAL_ENGINE", "v2");
#else
  setenv("WMUX_TERMINAL_ENGINE", "v2", 1);
#endif
  assert(wmux::terminal_engine_kind_from_environment() == wmux::TerminalEngineKind::V2);
  auto engine = wmux::make_terminal_engine(8, 3);
  feed_text(*engine, "v2");
  assert(engine->snapshot().lines[0].starts_with("v2"));
#if defined(_MSC_VER)
  _putenv_s("WMUX_TERMINAL_ENGINE", "");
#else
  unsetenv("WMUX_TERMINAL_ENGINE");
#endif
}

void terminal_engine_v2_bounds_integrated_scrollback() {
  wmux::TerminalEngineV2 engine{4, 2};
  engine.set_scrollback_capacity(2);

  feed_text(engine, "one\r\ntwo\r\ntri\r\nfor\r\nfiv\r\nsix");

  const auto scrollback = engine.scrollback_snapshot();
  assert(scrollback.capacity == 2);
  assert(scrollback.total_lines == 2);
  assert(scrollback.lines.size() == 2);
  assert(scrollback.lines[0] == "tri ");
  assert(scrollback.lines[1] == "for ");

  const auto visible = engine.snapshot();
  assert(visible.lines[0] == "fiv ");
  assert(visible.lines[1].starts_with("six"));
}

void terminal_engine_v2_reports_row_range_damage() {
  wmux::TerminalEngineV2 engine{8, 3};
  (void)engine.consume_damage();

  feed_text(engine, "\x1b[2;1Hhi");

  const auto damage = engine.consume_damage();
  assert(damage.kind == wmux::DamageKind::RowRange);
  assert(damage.first_row == 1);
  assert(damage.last_row == 1);
  assert((damage.dirty_rows == std::vector<int>{1}));

  const auto clean = engine.consume_damage();
  assert(clean.kind == wmux::DamageKind::None);
  assert(clean.first_row == -1);
  assert(clean.last_row == -1);
  assert(clean.dirty_rows.empty());
}

void terminal_engine_v2_reports_scroll_damage_for_normal_full_screen_scroll() {
  wmux::TerminalEngineV2 engine{4, 3};
  (void)engine.consume_damage();

  feed_text(engine, "aaa\r\nbbb\r\nccc\r\nddd");

  const auto damage = engine.consume_damage();
  assert(damage.kind == wmux::DamageKind::RowRange);
  assert(damage.scroll.has_value());
  assert(damage.scroll->direction == wmux::TerminalScrollDirection::Up);
  assert(damage.scroll->top_row == 0);
  assert(damage.scroll->bottom_row == 2);
  assert(damage.scroll->count == 1);
  assert(damage.dirty_rows.size() >= 1);
}

void terminal_engine_v2_preserves_styled_printed_spaces() {
  wmux::TerminalEngineV2 engine{8, 2};

  feed_text(engine, "\x1b[44m   \x1b[0m");

  const auto line = engine.line_view(0);
  assert(line.cells.size() == 8);
  for (int column = 0; column < 3; ++column) {
    const auto& cell = line.cells[static_cast<std::size_t>(column)];
    assert(cell.codepoint == U' ');
    assert(cell.attributes.background == 4);
  }
  assert(engine.cursor().column == 3);
  assert(engine.cursor().visible);
}

void terminal_engine_v2_preserves_styled_erased_line_blanks() {
  wmux::TerminalEngineV2 engine{8, 2};

  feed_text(engine, "\x1b[44m\x1b[2K");

  const auto line = engine.line_view(0);
  assert(line.cells.size() == 8);
  for (const auto& cell : line.cells) {
    assert(cell.codepoint == U' ');
    assert(cell.attributes.background == 4);
  }
}

void terminal_engine_v2_preserves_styled_erased_screen_blanks() {
  wmux::TerminalEngineV2 engine{8, 2};

  feed_text(engine, "old\r\ntext\x1b[44m\x1b[2J\x1b[H");

  for (int row = 0; row < 2; ++row) {
    const auto line = engine.line_view(row);
    assert(line.cells.size() == 8);
    for (const auto& cell : line.cells) {
      assert(cell.codepoint == U' ');
      assert(cell.attributes.background == 4);
    }
  }
  assert(engine.cursor().row == 0);
  assert(engine.cursor().column == 0);
}

void terminal_engine_v2_uses_current_style_for_character_erase_and_delete_fill() {
  wmux::TerminalEngineV2 engine{8, 2};

  feed_text(engine, "abcdef\r\x1b[44m\x1b[3X");
  auto line = engine.line_view(0);
  for (int column = 0; column < 3; ++column) {
    const auto& cell = line.cells[static_cast<std::size_t>(column)];
    assert(cell.codepoint == U' ');
    assert(cell.attributes.background == 4);
  }
  assert(line.cells[3].codepoint == U'd');

  wmux::TerminalEngineV2 delete_engine{8, 2};
  feed_text(delete_engine, "abcdef\r\x1b[44m\x1b[3P");
  line = delete_engine.line_view(0);
  assert(line.cells[0].codepoint == U'd');
  assert(line.cells[1].codepoint == U'e');
  assert(line.cells[2].codepoint == U'f');
  for (int column = 3; column < 6; ++column) {
    const auto& cell = line.cells[static_cast<std::size_t>(column)];
    assert(cell.codepoint == U' ');
    assert(cell.attributes.background == 4);
  }
}

void terminal_engine_v2_accepts_colon_truecolor_sgr() {
  wmux::TerminalEngineV2 engine{8, 2};

  feed_text(engine, "\x1b[48:2::1:2:3m  \x1b[0m");

  const auto line = engine.line_view(0);
  assert(line.cells[0].attributes.background == 0x01010203);
  assert(line.cells[1].attributes.background == 0x01010203);
}

void terminal_engine_v2_keeps_long_rich_sgr_sequences() {
  wmux::TerminalEngineV2 engine{8, 2};

  feed_text(engine, "\x1b[0;1;2;3;4;7;22;23;24;27;38;2;9;8;7;48;2;1;2;3mX");

  const auto line = engine.line_view(0);
  assert(line.cells[0].codepoint == U'X');
  assert(line.cells[0].attributes.foreground == 0x01090807);
  assert(line.cells[0].attributes.background == 0x01010203);
}

void terminal_engine_v2_materializes_box_drawing_cells() {
  wmux::TerminalEngineV2 engine{8, 2};

  feed_text(engine, "\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x90");

  const auto line = engine.line_view(0);
  assert(line.cells[0].extended == "\xe2\x94\x8c");
  assert(line.cells[1].extended == "\xe2\x94\x80");
  assert(line.cells[2].extended == "\xe2\x94\x90");
}

void terminal_engine_v2_supports_dec_special_graphics_charset() {
  wmux::TerminalEngineV2 engine{8, 2};

  feed_text(engine, "\x1b(0lqk\x1b(Bx");

  const auto line = engine.line_view(0);
  assert(line.cells[0].extended == "\xe2\x94\x8c");
  assert(line.cells[1].extended == "\xe2\x94\x80");
  assert(line.cells[2].extended == "\xe2\x94\x90");
  assert(line.cells[3].codepoint == U'x');
}

void terminal_engine_v2_handles_unmapped_ascii_in_dec_special_graphics_charset() {
  wmux::TerminalEngineV2 engine{8, 2};

  feed_text(engine, "\x1b(0A \x1b(B");

  const auto line = engine.line_view(0);
  assert(line.cells[0].extended == "A");
  assert(line.cells[1].extended == " ");
  assert(engine.cursor().column == 2);
}

void terminal_engine_v2_supports_csi_repeat_preceding_character() {
  wmux::TerminalEngineV2 engine{10, 2};

  feed_text(engine, "\x1b(0q\x1b[4b\x1b(B");

  const auto line = engine.line_view(0);
  for (int column = 0; column < 5; ++column) {
    assert(line.cells[static_cast<std::size_t>(column)].extended == "\xe2\x94\x80");
  }
  assert(engine.cursor().column == 5);
}

void terminal_engine_v2_save_restore_preserves_style_and_charset_state() {
  wmux::TerminalEngineV2 engine{8, 3};

  feed_text(engine, "\x1b[44m\x1b(0\x1b[s\x1b[0m\x1b(B\x1b[2;4Hzz\x1b[uq");

  const auto line = engine.line_view(0);
  assert(line.cells[0].extended == "\xe2\x94\x80");
  assert(line.cells[0].attributes.background == 4);
  assert(engine.cursor().row == 0);
  assert(engine.cursor().column == 1);
}

void terminal_engine_v2_supports_insert_mode() {
  wmux::TerminalEngineV2 engine{8, 2};

  feed_text(engine, "abcd\x1b[1G\x1b[4hZ\x1b[4l");

  const auto screen = engine.snapshot();
  assert(screen.lines[0].starts_with("Zabcd"));
}

void terminal_engine_v2_keeps_normal_and_alternate_screens_separate() {
  wmux::TerminalEngineV2 engine{10, 2};

  feed_text(engine, "normal\x1b[?1049h\x1b[44m\x1b[2Jalt\x1b[?1049l");

  auto screen = engine.snapshot();
  assert(!screen.alternate_screen);
  assert(screen.lines[0].starts_with("normal"));

  feed_text(engine, "\x1b[?1049h");
  screen = engine.snapshot();
  assert(screen.alternate_screen);
  assert(screen.lines[0] == "          ");
}

void terminal_engine_v2_supports_insert_and_delete_lines() {
  wmux::TerminalEngineV2 insert_engine{5, 4};
  feed_text(insert_engine, "\x1b[1;1HA\x1b[2;1HB\x1b[3;1HC\x1b[4;1HD\x1b[2;1H\x1b[L");
  auto screen = insert_engine.snapshot();
  assert(screen.lines[0].starts_with("A"));
  assert(screen.lines[1] == "     ");
  assert(screen.lines[2].starts_with("B"));
  assert(screen.lines[3].starts_with("C"));

  wmux::TerminalEngineV2 delete_engine{5, 4};
  feed_text(delete_engine, "\x1b[1;1HA\x1b[2;1HB\x1b[3;1HC\x1b[4;1HD\x1b[2;1H\x1b[M");
  screen = delete_engine.snapshot();
  assert(screen.lines[0].starts_with("A"));
  assert(screen.lines[1].starts_with("C"));
  assert(screen.lines[2].starts_with("D"));
  assert(screen.lines[3] == "     ");
}

void terminal_engine_v2_accepts_c1_csi_sequences() {
  wmux::TerminalEngineV2 engine{6, 2};
  std::string input = "a";
  input.push_back(static_cast<char>(0x9b));
  input += "31mR\x1b[0m";

  feed_text(engine, input);

  const auto line = engine.line_view(0);
  assert(line.cells[0].codepoint == U'a');
  assert(line.cells[1].codepoint == U'R');
  assert(line.cells[1].attributes.foreground == 1);
}

void terminal_engine_v2_unknown_sequences_do_not_corrupt_visible_text() {
  wmux::TerminalEngineV2 engine{12, 2};

  feed_text(engine, "abc\x1b[?9999!zdef");

  const auto screen = engine.snapshot();
  assert(screen.lines[0].starts_with("abcdef"));
  assert(screen.unknown_sequence_count > 0);
}

}  // namespace

void run_terminal_engine_tests() {
  terminal_engine_v2_matches_legacy_golden_cases();
  terminal_engine_v2_matches_legacy_resize_golden_case();
  terminal_engine_v2_matches_legacy_complex_grapheme_golden_case();
  terminal_engine_factory_defaults_to_legacy();
  terminal_engine_factory_accepts_v2();
  terminal_engine_v2_bounds_integrated_scrollback();
  terminal_engine_v2_reports_row_range_damage();
  terminal_engine_v2_reports_scroll_damage_for_normal_full_screen_scroll();
  terminal_engine_v2_preserves_styled_printed_spaces();
  terminal_engine_v2_preserves_styled_erased_line_blanks();
  terminal_engine_v2_preserves_styled_erased_screen_blanks();
  terminal_engine_v2_uses_current_style_for_character_erase_and_delete_fill();
  terminal_engine_v2_accepts_colon_truecolor_sgr();
  terminal_engine_v2_keeps_long_rich_sgr_sequences();
  terminal_engine_v2_materializes_box_drawing_cells();
  terminal_engine_v2_supports_dec_special_graphics_charset();
  terminal_engine_v2_handles_unmapped_ascii_in_dec_special_graphics_charset();
  terminal_engine_v2_supports_csi_repeat_preceding_character();
  terminal_engine_v2_save_restore_preserves_style_and_charset_state();
  terminal_engine_v2_supports_insert_mode();
  terminal_engine_v2_keeps_normal_and_alternate_screens_separate();
  terminal_engine_v2_supports_insert_and_delete_lines();
  terminal_engine_v2_accepts_c1_csi_sequences();
  terminal_engine_v2_unknown_sequences_do_not_corrupt_visible_text();
}
