#include "wmux/copy_selection.hpp"

#include <cassert>
#include <string>

namespace {

wmux::PtyOutputSnapshot snapshot_from_lines(
    std::initializer_list<wmux::TerminalLineSnapshot> lines) {
  wmux::PtyOutputSnapshot snapshot;
  for (const auto& line : lines) {
    snapshot.screen.line_snapshots.push_back(line);
    snapshot.screen.lines.push_back(line.text);
  }
  return snapshot;
}

wmux::TerminalLineSnapshot line_with_cells(
    std::initializer_list<std::string> cells,
    bool wrapped = false) {
  wmux::TerminalLineSnapshot line;
  line.wrapped = wrapped;
  for (const auto& cell : cells) {
    line.cells.push_back(cell);
    line.text += cell.empty() ? " " : cell;
    line.attributes.push_back({});
  }
  return line;
}

wmux::PtyOutputSnapshot snapshot_from_scrollback_and_screen(
    std::initializer_list<wmux::TerminalLineSnapshot> scrollback,
    std::initializer_list<wmux::TerminalLineSnapshot> screen) {
  wmux::PtyOutputSnapshot snapshot;
  for (const auto& line : scrollback) {
    snapshot.scrollback.line_snapshots.push_back(line);
    snapshot.scrollback.lines.push_back(line.text);
  }
  for (const auto& line : screen) {
    snapshot.screen.line_snapshots.push_back(line);
    snapshot.screen.lines.push_back(line.text);
  }
  return snapshot;
}

void joins_soft_wrapped_lines_without_newline() {
  const auto snapshot = snapshot_from_lines({
      wmux::TerminalLineSnapshot{"abcde", true},
      wmux::TerminalLineSnapshot{"f    ", false},
  });

  const auto copied = wmux::extract_copy_selection_text(
      snapshot,
      wmux::CopySelectionRange{
          wmux::CopySelectionPoint{0, 0},
          wmux::CopySelectionPoint{1, 0}},
      5);

  assert(copied == "abcdef");
}

void joins_wrapped_scrollback_and_visible_screen_lines() {
  const auto snapshot = snapshot_from_scrollback_and_screen(
      {
          wmux::TerminalLineSnapshot{"abcde", true},
      },
      {
          wmux::TerminalLineSnapshot{"fgh  ", false},
      });

  const auto copied = wmux::extract_copy_selection_text(
      snapshot,
      wmux::CopySelectionRange{
          wmux::CopySelectionPoint{0, 1},
          wmux::CopySelectionPoint{1, 1}},
      5);

  assert(copied == "bcdefg");
}

void selects_across_scrollback_and_live_grid_with_hard_breaks() {
  const auto snapshot = snapshot_from_scrollback_and_screen(
      {
          wmux::TerminalLineSnapshot{"old1 ", false},
          wmux::TerminalLineSnapshot{"old2 ", false},
      },
      {
          wmux::TerminalLineSnapshot{"new1 ", false},
          wmux::TerminalLineSnapshot{"new2 ", false},
      });

  const auto copied = wmux::extract_copy_selection_text(
      snapshot,
      wmux::CopySelectionRange{
          wmux::CopySelectionPoint{1, 1},
          wmux::CopySelectionPoint{2, 3}},
      5);

  assert(copied == "ld2\r\nnew1");
}

void uses_crlf_between_hard_lines() {
  const auto snapshot = snapshot_from_lines({
      wmux::TerminalLineSnapshot{"one  ", false},
      wmux::TerminalLineSnapshot{"two  ", false},
  });

  const auto copied = wmux::extract_copy_selection_text(
      snapshot,
      wmux::CopySelectionRange{
          wmux::CopySelectionPoint{0, 0},
          wmux::CopySelectionPoint{1, 2}},
      5);

  assert(copied == "one\r\ntwo");
}

void preserves_empty_hard_lines() {
  const auto snapshot = snapshot_from_lines({
      wmux::TerminalLineSnapshot{"one  ", false},
      wmux::TerminalLineSnapshot{"     ", false},
      wmux::TerminalLineSnapshot{"two  ", false},
  });

  const auto copied = wmux::extract_copy_selection_text(
      snapshot,
      wmux::CopySelectionRange{
          wmux::CopySelectionPoint{0, 0},
          wmux::CopySelectionPoint{2, 2}},
      5);

  assert(copied == "one\r\n\r\ntwo");
}

void preserves_hard_break_when_selection_ends_at_line_boundary() {
  const auto snapshot = snapshot_from_lines({
      wmux::TerminalLineSnapshot{"abc  ", false},
      wmux::TerminalLineSnapshot{"def  ", false},
  });

  const auto copied = wmux::extract_copy_selection_text(
      snapshot,
      wmux::CopySelectionRange{
          wmux::CopySelectionPoint{0, 0},
          wmux::CopySelectionPoint{1, 0}},
      5);

  assert(copied == "abc\r\nd");
}

void handles_reversed_selection_points() {
  const auto snapshot = snapshot_from_lines({
      wmux::TerminalLineSnapshot{"alpha", false},
  });

  const auto copied = wmux::extract_copy_selection_text(
      snapshot,
      wmux::CopySelectionRange{
          wmux::CopySelectionPoint{0, 4},
          wmux::CopySelectionPoint{0, 1}},
      5);

  assert(copied == "lpha");
}

void clamps_out_of_bounds_selection_after_resize() {
  const auto snapshot = snapshot_from_lines({
      wmux::TerminalLineSnapshot{"abc  ", false},
      wmux::TerminalLineSnapshot{"de   ", false},
  });

  const auto copied = wmux::extract_copy_selection_text(
      snapshot,
      wmux::CopySelectionRange{
          wmux::CopySelectionPoint{99, 99},
          wmux::CopySelectionPoint{0, 0}},
      5);

  assert(copied == "abc\r\nde");
}

void clamps_columns_to_current_line_width_after_resize() {
  const auto snapshot = snapshot_from_lines({
      wmux::TerminalLineSnapshot{"abcdef", false},
  });

  const auto copied = wmux::extract_copy_selection_text(
      snapshot,
      wmux::CopySelectionRange{
          wmux::CopySelectionPoint{0, 0},
          wmux::CopySelectionPoint{0, 20}},
      3);

  assert(copied == "abc");
}

void copies_only_alternate_screen_visible_lines() {
  auto snapshot = snapshot_from_scrollback_and_screen(
      {
          wmux::TerminalLineSnapshot{"old  ", false},
      },
      {
          wmux::TerminalLineSnapshot{"alt1 ", false},
          wmux::TerminalLineSnapshot{"alt2 ", false},
      });
  snapshot.screen.alternate_screen = true;

  const auto copied = wmux::extract_copy_selection_text(
      snapshot,
      wmux::CopySelectionRange{
          wmux::CopySelectionPoint{0, 0},
          wmux::CopySelectionPoint{1, 3}},
      5);

  assert(copied == "alt1\r\nalt2");
}

void ignores_normal_scrollback_while_alternate_screen_is_active() {
  auto snapshot = snapshot_from_scrollback_and_screen(
      {
          wmux::TerminalLineSnapshot{"old1 ", false},
          wmux::TerminalLineSnapshot{"old2 ", false},
      },
      {
          wmux::TerminalLineSnapshot{"top  ", false},
      });
  snapshot.screen.alternate_screen = true;

  const auto copied = wmux::extract_copy_selection_text(
      snapshot,
      wmux::CopySelectionRange{
          wmux::CopySelectionPoint{0, 0},
          wmux::CopySelectionPoint{2, 4}},
      5);

  assert(copied == "top");
}

void copies_visible_cleared_screen_blanks_without_erasing_scrollback() {
  const auto snapshot = snapshot_from_scrollback_and_screen(
      {
          wmux::TerminalLineSnapshot{"old  ", false},
      },
      {
          wmux::TerminalLineSnapshot{"     ", false},
      });

  const auto copied = wmux::extract_copy_selection_text(
      snapshot,
      wmux::CopySelectionRange{
          wmux::CopySelectionPoint{0, 0},
          wmux::CopySelectionPoint{1, 4}},
      5);

  assert(copied == "old\r\n");
}

void keeps_complete_utf8_sequences_when_selected() {
  std::string line{"ab", 2};
  line.push_back(static_cast<char>(0xce));
  line.push_back(static_cast<char>(0xbb));
  line.push_back(' ');

  const auto snapshot = snapshot_from_lines({
      wmux::TerminalLineSnapshot{line, false},
  });

  const auto copied = wmux::extract_copy_selection_text(
      snapshot,
      wmux::CopySelectionRange{
          wmux::CopySelectionPoint{0, 0},
          wmux::CopySelectionPoint{0, 3}},
      5);

  std::string expected{"ab", 2};
  expected.push_back(static_cast<char>(0xce));
  expected.push_back(static_cast<char>(0xbb));
  assert(copied == expected);
}

void copies_wide_utf8_cells_by_terminal_columns() {
  const auto snapshot = snapshot_from_lines({
      line_with_cells({"a", "\xe4\xb8\xad", "", "z", " "}, false),
  });

  const auto copied = wmux::extract_copy_selection_text(
      snapshot,
      wmux::CopySelectionRange{
          wmux::CopySelectionPoint{0, 0},
          wmux::CopySelectionPoint{0, 3}},
      5);

  assert(copied == "a\xe4\xb8\xadz");
}

void copies_wide_utf8_cells_across_wrapped_lines() {
  const auto snapshot = snapshot_from_scrollback_and_screen(
      {
          line_with_cells({"a", "\xe4\xb8\xad", "", " "}, true),
      },
      {
          line_with_cells({"b", " ", " ", " "}, false),
      });

  const auto copied = wmux::extract_copy_selection_text(
      snapshot,
      wmux::CopySelectionRange{
          wmux::CopySelectionPoint{0, 0},
          wmux::CopySelectionPoint{1, 0}},
      4);

  std::string expected{"a"};
  expected += "\xe4\xb8\xad";
  expected += "b";
  assert(copied == expected);
}

void drops_incomplete_utf8_boundary_bytes() {
  std::string line{"ab", 2};
  line.push_back(static_cast<char>(0xce));
  line.push_back(' ');

  const auto snapshot = snapshot_from_lines({
      wmux::TerminalLineSnapshot{line, false},
  });

  const auto copied = wmux::extract_copy_selection_text(
      snapshot,
      wmux::CopySelectionRange{
          wmux::CopySelectionPoint{0, 0},
          wmux::CopySelectionPoint{0, 2}},
      5);

  assert(copied == "ab");
}

void falls_back_to_plain_line_text() {
  wmux::PtyOutputSnapshot snapshot;
  snapshot.screen.lines.push_back("plain");

  const auto copied = wmux::extract_copy_selection_text(
      snapshot,
      wmux::CopySelectionRange{
          wmux::CopySelectionPoint{0, 0},
          wmux::CopySelectionPoint{0, 4}},
      5);

  assert(copied == "plain");
}

}  // namespace

void run_copy_selection_tests() {
  joins_soft_wrapped_lines_without_newline();
  joins_wrapped_scrollback_and_visible_screen_lines();
  selects_across_scrollback_and_live_grid_with_hard_breaks();
  uses_crlf_between_hard_lines();
  preserves_empty_hard_lines();
  preserves_hard_break_when_selection_ends_at_line_boundary();
  handles_reversed_selection_points();
  clamps_out_of_bounds_selection_after_resize();
  clamps_columns_to_current_line_width_after_resize();
  copies_only_alternate_screen_visible_lines();
  ignores_normal_scrollback_while_alternate_screen_is_active();
  copies_visible_cleared_screen_blanks_without_erasing_scrollback();
  keeps_complete_utf8_sequences_when_selected();
  copies_wide_utf8_cells_by_terminal_columns();
  copies_wide_utf8_cells_across_wrapped_lines();
  drops_incomplete_utf8_boundary_bytes();
  falls_back_to_plain_line_text();
}
