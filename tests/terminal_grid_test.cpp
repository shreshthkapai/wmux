#include "wmux/terminal_grid.hpp"

#include <cassert>
#include <string>

namespace {

void parses_printable_text_and_newlines() {
  wmux::TerminalGrid grid{10, 3};

  grid.feed("hello\r\nworld");

  const auto screen = grid.snapshot();
  assert(screen.columns == 10);
  assert(screen.rows == 3);
  assert(screen.lines.size() == 3);
  assert(screen.lines[0] == "hello     ");
  assert(screen.lines[1] == "world     ");
}

void applies_cursor_movement_and_clear_line() {
  wmux::TerminalGrid grid{10, 2};

  grid.feed("abcde\x1b[1;3HX\x1b[K");

  const auto screen = grid.snapshot();
  assert(screen.lines[0] == "abX       ");
  assert(screen.cursor_row == 0);
  assert(screen.cursor_column == 3);
}

void scrolls_when_output_exceeds_screen_height() {
  wmux::TerminalGrid grid{5, 2};

  grid.feed("a\r\nb\r\nc");

  const auto screen = grid.snapshot();
  assert(screen.lines[0] == "b    ");
  assert(screen.lines[1] == "c    ");
}

void stores_scrolled_lines_in_oldest_to_newest_order() {
  wmux::TerminalGrid grid{5, 2};

  grid.feed("a\r\nb\r\nc\r\nd");

  const auto scrollback = grid.scrollback_snapshot();
  assert(scrollback.lines.size() == 2);
  assert(scrollback.lines[0] == "a    ");
  assert(scrollback.lines[1] == "b    ");

  const auto screen = grid.snapshot();
  assert(screen.scrollback_line_count == 2);
  assert(screen.lines[0] == "c    ");
  assert(screen.lines[1] == "d    ");
}

void exposes_line_snapshots_for_copy_mode() {
  wmux::TerminalGrid grid{5, 2};

  grid.feed("abcdeX");

  const auto screen = grid.snapshot();
  assert(screen.line_snapshots.size() == screen.lines.size());
  assert(screen.line_snapshots[0].text == "abcde");
  assert(screen.line_snapshots[0].wrapped);
  assert(screen.line_snapshots[1].text == "X    ");
  assert(!screen.line_snapshots[1].wrapped);
}

void stores_wrapped_scrollback_metadata() {
  wmux::TerminalGrid grid{5, 2};

  grid.feed("abcdeX\r\nY");

  const auto scrollback = grid.scrollback_snapshot();
  assert(scrollback.line_snapshots.size() == scrollback.lines.size());
  assert(scrollback.line_snapshots.size() == 1);
  assert(scrollback.line_snapshots[0].text == "abcde");
  assert(scrollback.line_snapshots[0].wrapped);
  assert(scrollback.lines[0] == "abcde");
}

void bounds_scrollback_capacity() {
  wmux::TerminalGrid grid{4, 2};
  grid.set_scrollback_capacity(2);

  grid.feed("one\r\ntwo\r\ntri\r\nfor\r\nfiv\r\nsix");

  const auto scrollback = grid.scrollback_snapshot();
  assert(scrollback.capacity == 2);
  assert(scrollback.lines.size() == 2);
  assert(scrollback.lines[0] == "tri ");
  assert(scrollback.lines[1] == "for ");
}

void preserves_scrollback_across_resize() {
  wmux::TerminalGrid grid{5, 2};
  grid.feed("a\r\nb\r\nc");

  grid.resize(3, 3);
  grid.feed("\r\nd");

  const auto scrollback = grid.scrollback_snapshot();
  assert(scrollback.lines.size() == 1);
  assert(scrollback.lines[0] == "a    ");

  const auto screen = grid.snapshot();
  assert(screen.columns == 3);
  assert(screen.rows == 3);
  assert(screen.scrollback_line_count == 1);
}

void does_not_store_alternate_screen_scrolls() {
  wmux::TerminalGrid grid{5, 2};

  grid.feed("base\r\nline\x1b[?1049hA\r\nB\r\nC\x1b[?1049l");

  const auto scrollback = grid.scrollback_snapshot();
  assert(scrollback.lines.empty());

  const auto screen = grid.snapshot();
  assert(!screen.alternate_screen);
  assert(screen.lines[0] == "base ");
  assert(screen.lines[1] == "line ");
}

void csi_3j_clears_scrollback() {
  wmux::TerminalGrid grid{5, 2};

  grid.feed("a\r\nb\r\nc");
  assert(grid.scrollback_snapshot().lines.size() == 1);

  grid.feed("\x1b[3J");

  assert(grid.scrollback_snapshot().lines.empty());
}

void csi_2j_clears_screen_without_clearing_scrollback() {
  wmux::TerminalGrid grid{5, 2};

  grid.feed("a\r\nb\r\nc");
  assert(grid.scrollback_snapshot().lines.size() == 1);

  grid.feed("\x1b[2J");

  const auto scrollback = grid.scrollback_snapshot();
  assert(scrollback.lines.size() == 1);

  const auto screen = grid.snapshot();
  assert(screen.lines[0] == "     ");
  assert(screen.lines[1] == "     ");
  assert(!screen.line_snapshots[0].wrapped);
  assert(!screen.line_snapshots[1].wrapped);
}

void csi_3j_in_alternate_screen_does_not_clear_normal_scrollback() {
  wmux::TerminalGrid grid{5, 2};

  grid.feed("a\r\nb\r\nc");
  assert(grid.scrollback_snapshot().lines.size() == 1);

  grid.feed("\x1b[?1049h\x1b[3J\x1b[?1049l");

  const auto screen = grid.snapshot();
  assert(!screen.alternate_screen);
  assert(grid.scrollback_snapshot().lines.size() == 1);
}

void tracks_sgr_state_without_rendering_escape_bytes() {
  wmux::TerminalGrid grid{14, 1};

  grid.feed(
      "a\x1b[31;1mb\x1b[2;3mc\x1b[38;5;202md\x1b[38;2;1;2;3me"
      "\x1b[48;5;17mf\x1b[22;23mg\x1b[0mh");

  const auto screen = grid.snapshot();
  assert(screen.lines[0] == "abcdefgh      ");
  assert(screen.line_snapshots[0].attributes[1].foreground == 1);
  assert(screen.line_snapshots[0].attributes[1].bold);
  assert(screen.line_snapshots[0].attributes[2].dim);
  assert(screen.line_snapshots[0].attributes[2].italic);
  assert(screen.line_snapshots[0].attributes[3].foreground == 202);
  assert(screen.line_snapshots[0].attributes[4].foreground == 0x01010203);
  assert(screen.line_snapshots[0].attributes[5].background == 17);
  assert(!screen.line_snapshots[0].attributes[6].bold);
  assert(!screen.line_snapshots[0].attributes[6].dim);
  assert(!screen.line_snapshots[0].attributes[6].italic);
  assert(screen.line_snapshots[0].attributes[7].foreground == -1);
  assert(screen.line_snapshots[0].attributes[7].background == -1);
}

void handles_cursor_save_restore_and_line_movement() {
  wmux::TerminalGrid grid{6, 3};

  grid.feed("abc\x1b[s\x1b[2Ezz\x1b[uX");

  const auto screen = grid.snapshot();
  assert(screen.lines[0] == "abcX  ");
  assert(screen.lines[2] == "zz    ");
  assert(screen.cursor_row == 0);
  assert(screen.cursor_column == 4);
}

void handles_insert_delete_and_erase_characters() {
  wmux::TerminalGrid grid{8, 1};

  grid.feed("abcdef\x1b[1;3H\x1b[2@XY\x1b[1;5H\x1b[P\x1b[1;6H\x1b[2X");

  const auto screen = grid.snapshot();
  assert(screen.lines[0] == "abXYd   ");
}

void keeps_scroll_region_changes_inside_region() {
  wmux::TerminalGrid grid{5, 4};

  grid.feed("one  \x1b[2;1Htwo  \x1b[3;1Hthr  \x1b[4;1Hfour ");
  grid.feed("\x1b[2;3r\x1b[3;1H\n");

  const auto screen = grid.snapshot();
  assert(screen.lines[0] == "one  ");
  assert(screen.lines[1] == "thr  ");
  assert(screen.lines[2] == "     ");
  assert(screen.lines[3] == "four ");
  assert(grid.scrollback_snapshot().lines.empty());
}

void supports_insert_and_delete_lines_in_scroll_region() {
  wmux::TerminalGrid grid{5, 4};

  grid.feed("one  \x1b[2;1Htwo  \x1b[3;1Hthr  \x1b[4;1Hfour ");
  grid.feed("\x1b[2;4r\x1b[3;1H\x1b[L");

  auto screen = grid.snapshot();
  assert(screen.lines[0] == "one  ");
  assert(screen.lines[1] == "two  ");
  assert(screen.lines[2] == "     ");
  assert(screen.lines[3] == "thr  ");

  grid.feed("\x1b[2;1H\x1b[M");
  screen = grid.snapshot();
  assert(screen.lines[1] == "     ");
  assert(screen.lines[2] == "thr  ");
  assert(screen.lines[3] == "     ");
}

void resets_scroll_region_when_invalid_or_resized() {
  wmux::TerminalGrid grid{4, 3};

  grid.feed("aa  \x1b[2;1Hbb  \x1b[3;1Hcc  \x1b[2;2r\x1b[3;1H\n");

  const auto screen = grid.snapshot();
  assert(screen.lines[0] == "bb  ");
  assert(screen.lines[1] == "cc  ");
  assert(screen.lines[2] == "    ");
  assert(grid.scrollback_snapshot().lines.size() == 1);
}

void preserves_normal_screen_across_alternate_screen() {
  wmux::TerminalGrid grid{12, 2};

  grid.feed("normal\x1b[?1049halt\x1b[?1049l");

  const auto screen = grid.snapshot();
  assert(!screen.alternate_screen);
  assert(screen.lines[0] == "normal      ");
}

void reports_active_alternate_screen_without_losing_normal_scrollback() {
  wmux::TerminalGrid grid{5, 2};

  grid.feed("a\r\nb\r\nc\x1b[?1049halt");

  const auto screen = grid.snapshot();
  assert(screen.alternate_screen);
  assert(screen.lines[0] == "alt  ");
  assert(grid.scrollback_snapshot().lines.size() == 1);
}

void tracks_bracketed_paste_mode() {
  wmux::TerminalGrid grid{5, 2};

  grid.feed("\x1b[?2004h");
  auto screen = grid.snapshot();
  assert(screen.bracketed_paste_mode);

  grid.feed("\x1b[?2004l");
  screen = grid.snapshot();
  assert(!screen.bracketed_paste_mode);
}

void accepts_utf8_bytes_without_overflowing_grid_rows() {
  wmux::TerminalGrid grid{4, 2};
  std::string input{"a", 1};
  input.push_back(static_cast<char>(0xce));
  input.push_back(static_cast<char>(0xbb));
  input.push_back('z');

  grid.feed(input);

  const auto screen = grid.snapshot();
  assert(screen.lines.size() == 2);
  assert(screen.line_snapshots[0].cells.size() == 4);
  assert(screen.line_snapshots[0].cells[1] == "\xce\xbb");
}

void treats_wide_and_combining_utf8_defensively() {
  wmux::TerminalGrid grid{6, 1};
  std::string input{"a"};
  input.push_back(static_cast<char>(0xe4));
  input.push_back(static_cast<char>(0xb8));
  input.push_back(static_cast<char>(0xad));
  input.push_back('e');
  input.push_back(static_cast<char>(0xcc));
  input.push_back(static_cast<char>(0x81));
  input.push_back('z');

  grid.feed(input);

  const auto screen = grid.snapshot();
  assert(screen.line_snapshots[0].cells.size() == 6);
  assert(screen.line_snapshots[0].cells[0] == "a");
  assert(screen.line_snapshots[0].cells[1] == "\xe4\xb8\xad");
  assert(screen.line_snapshots[0].cells[2].empty());
  assert(screen.line_snapshots[0].cells[3] == "e\xcc\x81");
  assert(screen.line_snapshots[0].cells[4] == "z");
  assert(screen.cursor_column == 5);
}

void groups_zwj_emoji_sequence_as_one_wide_cell() {
  wmux::TerminalGrid grid{6, 1};
  const std::string emoji = "\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb";

  grid.feed(emoji + "x");

  const auto screen = grid.snapshot();
  assert(screen.line_snapshots[0].cells[0] == emoji);
  assert(screen.line_snapshots[0].cells[1].empty());
  assert(screen.line_snapshots[0].cell_widths[0] == wmux::TerminalCellWidth::WideLeading);
  assert(screen.line_snapshots[0].cell_widths[1] == wmux::TerminalCellWidth::WideContinuation);
  assert(screen.line_snapshots[0].cells[2] == "x");
  assert(screen.cursor_column == 3);
}

void groups_regional_indicator_flag_pair_as_one_wide_cell() {
  wmux::TerminalGrid grid{6, 1};
  const std::string flag = "\xf0\x9f\x87\xba\xf0\x9f\x87\xb8";

  grid.feed(flag + "x");

  const auto screen = grid.snapshot();
  assert(screen.line_snapshots[0].cells[0] == flag);
  assert(screen.line_snapshots[0].cells[1].empty());
  assert(screen.line_snapshots[0].cell_widths[0] == wmux::TerminalCellWidth::WideLeading);
  assert(screen.line_snapshots[0].cell_widths[1] == wmux::TerminalCellWidth::WideContinuation);
  assert(screen.line_snapshots[0].cells[2] == "x");
  assert(screen.cursor_column == 3);
}

void groups_emoji_modifier_sequence_as_one_wide_cell() {
  wmux::TerminalGrid grid{6, 1};
  const std::string emoji = "\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbd";

  grid.feed(emoji + "x");

  const auto screen = grid.snapshot();
  assert(screen.line_snapshots[0].cells[0] == emoji);
  assert(screen.line_snapshots[0].cells[1].empty());
  assert(screen.line_snapshots[0].cell_widths[0] == wmux::TerminalCellWidth::WideLeading);
  assert(screen.line_snapshots[0].cell_widths[1] == wmux::TerminalCellWidth::WideContinuation);
  assert(screen.line_snapshots[0].cells[2] == "x");
  assert(screen.cursor_column == 3);
}

void copies_utf8_cells_into_scrollback_snapshots() {
  wmux::TerminalGrid grid{4, 2};
  std::string input;
  input += "a";
  input += "\xe4\xb8\xad";
  input += "\r\nb\r\nc";

  grid.feed(input);

  const auto scrollback = grid.scrollback_snapshot();
  assert(scrollback.line_snapshots.size() == 1);
  assert(scrollback.line_snapshots[0].cells.size() == 4);
  assert(scrollback.line_snapshots[0].cells[0] == "a");
  assert(scrollback.line_snapshots[0].cells[1] == "\xe4\xb8\xad");
  assert(scrollback.line_snapshots[0].cells[2].empty());
}

void resizes_while_preserving_visible_cells() {
  wmux::TerminalGrid grid{8, 2};

  grid.feed("alpha\r\nbeta");
  grid.resize(5, 3);

  const auto screen = grid.snapshot();
  assert(screen.columns == 5);
  assert(screen.rows == 3);
  assert(screen.lines[0] == "alpha");
  assert(screen.lines[1] == "beta ");
}

void preserves_wrapped_line_metadata_across_resize() {
  wmux::TerminalGrid grid{5, 2};

  grid.feed("abcdeX");
  grid.resize(4, 3);

  const auto screen = grid.snapshot();
  assert(screen.line_snapshots.size() == 3);
  assert(screen.line_snapshots[0].text == "abcd");
  assert(screen.line_snapshots[0].wrapped);
  assert(screen.line_snapshots[1].text == "X   ");
  assert(!screen.line_snapshots[1].wrapped);
}

void tracks_terminal_modes_title_and_unknown_sequences() {
  wmux::TerminalGrid grid{12, 2};

  grid.feed("\x1b]2;wmux test\a\x1b[?25l\x1b[3 q\x1b[?7lABCD");
  grid.feed("\x1b[9999z");

  const auto screen = grid.snapshot();
  assert(screen.title == "wmux test");
  assert(!screen.cursor_visible);
  assert(screen.cursor_style == 3);
  assert(!screen.wrap_mode);
  assert(screen.unknown_sequence_count == 1);
  assert(screen.lines[0].find("ABCD") == 0);
}

void honors_origin_mode_inside_scroll_region() {
  wmux::TerminalGrid grid{8, 4};

  grid.feed("11111111\x1b[2;1H22222222\x1b[3;1H33333333\x1b[4;1H44444444");
  grid.feed("\x1b[2;3r\x1b[?6h\x1b[1;1HX\x1b[?6l\x1b[1;1HY");

  const auto screen = grid.snapshot();
  assert(screen.lines[0] == "Y1111111");
  assert(screen.lines[1] == "X2222222");
  assert(!screen.origin_mode);
}

void wrap_mode_can_be_disabled_and_enabled() {
  wmux::TerminalGrid grid{4, 2};

  grid.feed("\x1b[?7labcde");
  auto screen = grid.snapshot();
  assert(screen.lines[0] == "abce");
  assert(screen.lines[1] == "    ");
  assert(!screen.wrap_mode);

  grid.feed("\x1b[?7hZ");
  screen = grid.snapshot();
  assert(screen.lines[0] == "abcZ");
  assert(screen.lines[1] == "    ");
}

void handles_osc_title_with_st_terminator() {
  wmux::TerminalGrid grid{10, 1};

  grid.feed("\x1b]0;build\x1b\\ready");

  const auto screen = grid.snapshot();
  assert(screen.title == "build");
  assert(screen.lines[0] == "ready     ");
}

void golden_powershell_prompt_snapshot() {
  wmux::TerminalGrid grid{32, 2};

  grid.feed("PS C:\\Users\\shres\\code\\wmux> ");

  const auto screen = grid.snapshot();
  const std::string expected = "PS C:\\Users\\shres\\code\\wmux> ";
  assert(screen.lines[0].rfind(expected, 0) == 0);
  assert(screen.lines[1] == "                                ");
}

void golden_cmd_prompt_and_clear_screen_snapshot() {
  wmux::TerminalGrid grid{24, 3};

  grid.feed("C:\\Users\\shres>dir\r\nfile.txt\r\n\x1b[2J\x1b[Hdone");

  const auto screen = grid.snapshot();
  assert(screen.lines[0] == "done                    ");
  assert(screen.lines[1] == "                        ");
  assert(screen.lines[2] == "                        ");
}

void golden_git_diff_color_snapshot() {
  wmux::TerminalGrid grid{16, 2};

  grid.feed("\x1b[31m-deleted\x1b[0m\r\n\x1b[32m+added\x1b[0m");

  const auto screen = grid.snapshot();
  assert(screen.lines[0] == "-deleted        ");
  assert(screen.lines[1] == "+added          ");
  assert(screen.line_snapshots[0].attributes[0].foreground == 1);
  assert(screen.line_snapshots[1].attributes[0].foreground == 2);
}

void golden_progress_bar_snapshot() {
  wmux::TerminalGrid grid{12, 1};

  grid.feed("10%\r20%\r100%");

  const auto screen = grid.snapshot();
  assert(screen.lines[0] == "100%        ");
}

void golden_alternate_screen_snapshot() {
  wmux::TerminalGrid grid{10, 2};

  grid.feed("normal\x1b[?1049hviewer\x1b[2;1Hline2\x1b[?1049l");

  auto screen = grid.snapshot();
  assert(!screen.alternate_screen);
  assert(screen.lines[0] == "normal    ");

  grid.feed("\x1b[?1049hviewer");
  screen = grid.snapshot();
  assert(screen.alternate_screen);
  assert(screen.lines[0] == "viewer    ");
}

}  // namespace

void run_terminal_grid_tests() {
  parses_printable_text_and_newlines();
  applies_cursor_movement_and_clear_line();
  scrolls_when_output_exceeds_screen_height();
  stores_scrolled_lines_in_oldest_to_newest_order();
  exposes_line_snapshots_for_copy_mode();
  stores_wrapped_scrollback_metadata();
  bounds_scrollback_capacity();
  preserves_scrollback_across_resize();
  does_not_store_alternate_screen_scrolls();
  csi_3j_clears_scrollback();
  csi_2j_clears_screen_without_clearing_scrollback();
  csi_3j_in_alternate_screen_does_not_clear_normal_scrollback();
  tracks_sgr_state_without_rendering_escape_bytes();
  handles_cursor_save_restore_and_line_movement();
  handles_insert_delete_and_erase_characters();
  keeps_scroll_region_changes_inside_region();
  supports_insert_and_delete_lines_in_scroll_region();
  resets_scroll_region_when_invalid_or_resized();
  preserves_normal_screen_across_alternate_screen();
  reports_active_alternate_screen_without_losing_normal_scrollback();
  tracks_bracketed_paste_mode();
  accepts_utf8_bytes_without_overflowing_grid_rows();
  treats_wide_and_combining_utf8_defensively();
  groups_zwj_emoji_sequence_as_one_wide_cell();
  groups_regional_indicator_flag_pair_as_one_wide_cell();
  groups_emoji_modifier_sequence_as_one_wide_cell();
  copies_utf8_cells_into_scrollback_snapshots();
  resizes_while_preserving_visible_cells();
  preserves_wrapped_line_metadata_across_resize();
  tracks_terminal_modes_title_and_unknown_sequences();
  honors_origin_mode_inside_scroll_region();
  wrap_mode_can_be_disabled_and_enabled();
  handles_osc_title_with_st_terminator();
  golden_powershell_prompt_snapshot();
  golden_cmd_prompt_and_clear_screen_snapshot();
  golden_git_diff_color_snapshot();
  golden_progress_bar_snapshot();
  golden_alternate_screen_snapshot();
}
