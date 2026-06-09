#include "daemon_render.hpp"

#include <cassert>
#include <cctype>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace {

wmux::TerminalLineSnapshot line_snapshot(std::string text, bool wrapped = false) {
  wmux::TerminalLineSnapshot line;
  line.text = std::move(text);
  line.wrapped = wrapped;
  line.attributes.resize(line.text.size());
  return line;
}

wmux::PtyOutputSnapshot snapshot_with_lines(int count, int width) {
  wmux::PtyOutputSnapshot snapshot;
  snapshot.alive = true;
  snapshot.screen.columns = width;
  snapshot.screen.rows = count;
  for (int index = 0; index < count; ++index) {
    std::string text = "line" + std::to_string(index);
    if (static_cast<int>(text.size()) < width) {
      text.append(static_cast<std::size_t>(width - static_cast<int>(text.size())), ' ');
    }
    if (static_cast<int>(text.size()) > width) {
      text.resize(static_cast<std::size_t>(width));
    }
    auto line = line_snapshot(text);
    snapshot.screen.lines.push_back(line.text);
    snapshot.screen.line_snapshots.push_back(std::move(line));
  }
  return snapshot;
}

wmux::daemon_internal::ActiveWindowFrame single_pane_frame(
    int columns,
    int rows,
    wmux::PaneLayoutRect rect) {
  wmux::daemon_internal::ActiveWindowFrame frame;
  frame.window_id = 1;
  frame.active_pane_id = rect.pane_id;
  frame.session_name = "resize";
  frame.window_name = "main";
  frame.columns = columns;
  frame.rows = rows;
  frame.status_bar_enabled = true;
  frame.panes.push_back(wmux::daemon_internal::RenderPane{rect, true, {}});
  return frame;
}

bool has_cursor_move_beyond(std::string_view rendered, int max_row, int max_column) {
  for (std::size_t index = 0; index + 1 < rendered.size(); ++index) {
    if (rendered[index] != '\x1b' || rendered[index + 1] != '[') {
      continue;
    }

    std::size_t cursor = index + 2;
    int row = 0;
    bool has_row = false;
    while (cursor < rendered.size() &&
           std::isdigit(static_cast<unsigned char>(rendered[cursor]))) {
      has_row = true;
      row = (row * 10) + (rendered[cursor] - '0');
      ++cursor;
    }
    if (!has_row || cursor >= rendered.size() || rendered[cursor] != ';') {
      continue;
    }

    ++cursor;
    int column = 0;
    bool has_column = false;
    while (cursor < rendered.size() &&
           std::isdigit(static_cast<unsigned char>(rendered[cursor]))) {
      has_column = true;
      column = (column * 10) + (rendered[cursor] - '0');
      ++cursor;
    }
    if (has_column && cursor < rendered.size() && rendered[cursor] == 'H' &&
        (row > max_row || column > max_column)) {
      return true;
    }
  }

  return false;
}

void clamps_viewport_after_resize_growth() {
  auto frame = single_pane_frame(20, 4, wmux::PaneLayoutRect{1, 0, 0, 20, 4});
  std::unordered_map<wmux::PaneId, wmux::PtyOutputSnapshot> snapshots;
  snapshots.emplace(1, snapshot_with_lines(30, 18));

  wmux::daemon_internal::PaneViewportStates viewport_states;
  viewport_states[1].offset = 100;
  wmux::daemon_internal::CopyModeState copy_mode;

  wmux::daemon_internal::update_viewport_states(
      frame, snapshots, viewport_states, copy_mode);
  assert(viewport_states[1].offset == 28);

  frame = single_pane_frame(20, 12, wmux::PaneLayoutRect{1, 0, 0, 20, 11});
  wmux::daemon_internal::update_viewport_states(
      frame, snapshots, viewport_states, copy_mode);
  assert(viewport_states[1].offset == 21);
}

void removes_viewport_state_for_removed_panes() {
  const auto frame = single_pane_frame(20, 6, wmux::PaneLayoutRect{1, 0, 0, 20, 5});
  std::unordered_map<wmux::PaneId, wmux::PtyOutputSnapshot> snapshots;
  snapshots.emplace(1, snapshot_with_lines(5, 18));

  wmux::daemon_internal::PaneViewportStates viewport_states;
  viewport_states[1].offset = 1;
  viewport_states[99].offset = 10;
  wmux::daemon_internal::CopyModeState copy_mode;

  wmux::daemon_internal::update_viewport_states(
      frame, snapshots, viewport_states, copy_mode);
  assert(viewport_states.contains(1));
  assert(!viewport_states.contains(99));
}

void clamps_copy_mode_state_after_resize() {
  const auto frame = single_pane_frame(10, 5, wmux::PaneLayoutRect{1, 0, 0, 6, 4});
  std::unordered_map<wmux::PaneId, wmux::PtyOutputSnapshot> snapshots;
  snapshots.emplace(1, snapshot_with_lines(10, 8));

  wmux::daemon_internal::CopyModeState copy_mode;
  copy_mode.active = true;
  copy_mode.pane_id = 1;
  copy_mode.cursor_row = 99;
  copy_mode.cursor_column = 99;
  copy_mode.selection_active = true;
  copy_mode.selection_anchor = wmux::daemon_internal::CopyModePoint{99, 99};

  wmux::daemon_internal::clamp_copy_mode_cursor(copy_mode, frame, snapshots);

  assert(copy_mode.active);
  assert(copy_mode.cursor_row == 1);
  assert(copy_mode.cursor_column == 3);
  assert(copy_mode.selection_active);
  assert(copy_mode.selection_anchor.line == 9);
  assert(copy_mode.selection_anchor.column == 3);
}

void clears_selection_when_copy_pane_becomes_too_small() {
  const auto frame = single_pane_frame(2, 2, wmux::PaneLayoutRect{1, 0, 0, 2, 2});
  std::unordered_map<wmux::PaneId, wmux::PtyOutputSnapshot> snapshots;
  snapshots.emplace(1, snapshot_with_lines(4, 2));

  wmux::daemon_internal::CopyModeState copy_mode;
  copy_mode.active = true;
  copy_mode.pane_id = 1;
  copy_mode.cursor_row = 4;
  copy_mode.cursor_column = 4;
  copy_mode.selection_active = true;
  copy_mode.selection_anchor = wmux::daemon_internal::CopyModePoint{4, 4};

  wmux::daemon_internal::clamp_copy_mode_cursor(copy_mode, frame, snapshots);

  assert(copy_mode.active);
  assert(copy_mode.cursor_row == 0);
  assert(copy_mode.cursor_column == 0);
  assert(!copy_mode.selection_active);
}

void disables_copy_mode_when_pane_disappears() {
  wmux::daemon_internal::ActiveWindowFrame frame;
  frame.active_pane_id = 2;
  frame.columns = 10;
  frame.rows = 5;
  frame.panes.push_back(
      wmux::daemon_internal::RenderPane{wmux::PaneLayoutRect{2, 0, 0, 10, 4}, true, {}});

  std::unordered_map<wmux::PaneId, wmux::PtyOutputSnapshot> snapshots;
  wmux::daemon_internal::CopyModeState copy_mode;
  copy_mode.active = true;
  copy_mode.pane_id = 1;

  wmux::daemon_internal::clamp_copy_mode_cursor(copy_mode, frame, snapshots);

  assert(!copy_mode.active);
}

void render_uses_only_frame_bounds_after_resize() {
  const auto frame = single_pane_frame(20, 6, wmux::PaneLayoutRect{1, 0, 0, 20, 5});
  std::unordered_map<wmux::PaneId, wmux::PtyOutputSnapshot> snapshots;
  snapshots.emplace(1, snapshot_with_lines(20, 18));
  wmux::daemon_internal::PaneViewportStates viewport_states;
  wmux::daemon_internal::CopyModeState copy_mode;

  const auto rendered =
      wmux::daemon_internal::render_frame(frame, snapshots, viewport_states, copy_mode, "");

  assert(!has_cursor_move_beyond(rendered, frame.rows, frame.columns));
  assert(rendered.find("\x1b[6;1H") != std::string::npos);
}

void partial_render_updates_only_dirty_pane_body() {
  wmux::daemon_internal::ActiveWindowFrame frame;
  frame.window_id = 1;
  frame.active_pane_id = 2;
  frame.session_name = "perf";
  frame.window_name = "main";
  frame.columns = 20;
  frame.rows = 6;
  frame.status_bar_enabled = true;
  frame.panes.push_back(wmux::daemon_internal::RenderPane{
      wmux::PaneLayoutRect{1, 0, 0, 10, 5}, false, {}});
  frame.panes.push_back(wmux::daemon_internal::RenderPane{
      wmux::PaneLayoutRect{2, 10, 0, 10, 5}, true, {}});

  std::unordered_map<wmux::PaneId, wmux::PtyOutputSnapshot> snapshots;
  snapshots.emplace(1, snapshot_with_lines(3, 8));
  snapshots.emplace(2, snapshot_with_lines(3, 8));
  wmux::daemon_internal::PaneViewportStates viewport_states;
  wmux::daemon_internal::CopyModeState copy_mode;

  const auto rendered = wmux::daemon_internal::render_frame_update(
      frame,
      snapshots,
      viewport_states,
      copy_mode,
      "",
      wmux::daemon_internal::RenderFrameOptions{
          false,
          false,
          false,
          std::unordered_set<wmux::PaneId>{2}});

  assert(rendered.find("\x1b[2J") == std::string::npos);
  assert(rendered.find('+') == std::string::npos);
  assert(rendered.find("wmux [perf]") == std::string::npos);
  assert(rendered.find("\x1b[2;12H") != std::string::npos);
  assert(rendered.find("\x1b[2;2H") == std::string::npos);
}

}  // namespace

void run_daemon_render_tests() {
  clamps_viewport_after_resize_growth();
  removes_viewport_state_for_removed_panes();
  clamps_copy_mode_state_after_resize();
  clears_selection_when_copy_pane_becomes_too_small();
  disables_copy_mode_when_pane_disappears();
  render_uses_only_frame_bounds_after_resize();
  partial_render_updates_only_dirty_pane_body();
}
