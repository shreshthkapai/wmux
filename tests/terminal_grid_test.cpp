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
  wmux::TerminalGrid grid{12, 1};

  grid.feed("a\x1b[31;1mb\x1b[0mc");

  const auto screen = grid.snapshot();
  assert(screen.lines[0] == "abc         ");
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

void accepts_utf8_bytes_without_overflowing_grid_rows() {
  wmux::TerminalGrid grid{4, 2};
  std::string input{"a", 1};
  input.push_back(static_cast<char>(0xce));
  input.push_back(static_cast<char>(0xbb));
  input.push_back('z');

  grid.feed(input);

  const auto screen = grid.snapshot();
  assert(screen.lines.size() == 2);
  assert(screen.lines[0].size() == 4);
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
  preserves_normal_screen_across_alternate_screen();
  reports_active_alternate_screen_without_losing_normal_scrollback();
  accepts_utf8_bytes_without_overflowing_grid_rows();
  resizes_while_preserving_visible_cells();
  preserves_wrapped_line_metadata_across_resize();
}
