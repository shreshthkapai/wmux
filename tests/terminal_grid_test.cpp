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

}  // namespace

void run_terminal_grid_tests() {
  parses_printable_text_and_newlines();
  applies_cursor_movement_and_clear_line();
  scrolls_when_output_exceeds_screen_height();
  tracks_sgr_state_without_rendering_escape_bytes();
  preserves_normal_screen_across_alternate_screen();
  resizes_while_preserving_visible_cells();
}
