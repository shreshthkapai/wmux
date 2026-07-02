#include "daemon_render.hpp"

#include <cassert>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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

wmux::PtyOutputSnapshot snapshot_with_text_lines(
    const std::vector<std::string>& lines,
    int width) {
  wmux::PtyOutputSnapshot snapshot;
  snapshot.alive = true;
  snapshot.screen.columns = width;
  snapshot.screen.rows = static_cast<int>(lines.size());
  for (const auto& raw : lines) {
    std::string text = raw;
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

wmux::PtyOutputSnapshot snapshot_with_raw_line(std::string text) {
  wmux::PtyOutputSnapshot snapshot;
  snapshot.alive = true;
  snapshot.screen.columns = 80;
  snapshot.screen.rows = 1;
  wmux::TerminalLineSnapshot line;
  line.text = std::move(text);
  snapshot.screen.lines.push_back(line.text);
  snapshot.screen.line_snapshots.push_back(std::move(line));
  return snapshot;
}

std::string normalized_render_snapshot(std::string_view rendered) {
  std::string out;
  for (std::size_t index = 0; index < rendered.size();) {
    if (rendered[index] == '\x1b' && index + 1 < rendered.size() &&
        rendered[index + 1] == '[') {
      std::size_t cursor = index + 2;
      while (cursor < rendered.size()) {
        const auto byte = static_cast<unsigned char>(rendered[cursor]);
        if (byte >= 0x40 && byte <= 0x7e) {
          break;
        }
        ++cursor;
      }
      if (cursor >= rendered.size()) {
        out += "<truncated-csi>";
        break;
      }

      const auto params = rendered.substr(index + 2, cursor - (index + 2));
      const char final_byte = rendered[cursor];
      if (final_byte == 'J' && params == "2") {
        if (!out.empty() && out.back() != '\n') {
          out.push_back('\n');
        }
        out += "<clear>\n";
      } else if (final_byte == 'H') {
        if (!out.empty() && out.back() != '\n') {
          out.push_back('\n');
        }
        out += "@";
        out += params.empty() ? "1;1" : std::string{params};
        out += "|";
      } else if (final_byte == 'm') {
        out += "<sgr:";
        out += params.empty() ? "0" : std::string{params};
        out += ">";
      } else {
        out += "<csi:";
        out += params;
        out.push_back(final_byte);
        out += ">";
      }
      index = cursor + 1;
      continue;
    }

    if (rendered[index] == ' ') {
      out.push_back('_');
    } else if (rendered[index] == '\r') {
      out += "<cr>";
    } else if (rendered[index] == '\n') {
      out += "<lf>\n";
    } else {
      out.push_back(rendered[index]);
    }
    ++index;
  }
  return out;
}

void assert_render_snapshot(
    std::string_view name,
    std::string_view rendered,
    std::string_view expected) {
  const auto actual = normalized_render_snapshot(rendered);
  if (actual != expected) {
    std::cerr << "render snapshot mismatch: " << name << "\nexpected:\n"
              << expected << "\nactual:\n"
              << actual << "\n";
    std::exit(1);
  }
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

void renders_smallest_drawable_pane_body() {
  assert(wmux::daemon_internal::body_width(wmux::PaneLayoutRect{1, 0, 0, 3, 3}) == 1);
  assert(wmux::daemon_internal::body_height(wmux::PaneLayoutRect{1, 0, 0, 3, 3}) == 1);
  assert(wmux::daemon_internal::body_width(wmux::PaneLayoutRect{1, 0, 0, 2, 3}) == 0);
  assert(wmux::daemon_internal::body_height(wmux::PaneLayoutRect{1, 0, 0, 3, 2}) == 0);

  const auto frame = single_pane_frame(3, 4, wmux::PaneLayoutRect{1, 0, 0, 3, 3});
  std::unordered_map<wmux::PaneId, wmux::PtyOutputSnapshot> snapshots;
  snapshots.emplace(1, snapshot_with_raw_line("Z"));
  wmux::daemon_internal::PaneViewportStates viewport_states;
  wmux::daemon_internal::CopyModeState copy_mode;

  const auto rendered =
      wmux::daemon_internal::render_frame(frame, snapshots, viewport_states, copy_mode, "");

  assert(rendered.find("\x1b[2;2H") != std::string::npos);
  assert(rendered.find("Z") != std::string::npos);
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

void render_does_not_split_utf8_fallback_text() {
  const auto frame = single_pane_frame(6, 4, wmux::PaneLayoutRect{1, 0, 0, 6, 3});
  std::unordered_map<wmux::PaneId, wmux::PtyOutputSnapshot> snapshots;
  snapshots.emplace(1, snapshot_with_raw_line("A\xE2\x94\x82" "B"));
  wmux::daemon_internal::PaneViewportStates viewport_states;
  wmux::daemon_internal::CopyModeState copy_mode;

  const auto rendered =
      wmux::daemon_internal::render_frame(frame, snapshots, viewport_states, copy_mode, "");

  assert(rendered.find("A\xE2\x94\x82" "B ") != std::string::npos);
  assert(rendered.find("A\xE2 ") == std::string::npos);
}

void render_clips_wide_utf8_to_pane_columns() {
  const auto frame = single_pane_frame(5, 4, wmux::PaneLayoutRect{1, 0, 0, 5, 3});
  std::unordered_map<wmux::PaneId, wmux::PtyOutputSnapshot> snapshots;
  snapshots.emplace(1, snapshot_with_raw_line("A\xF0\x9F\x99\x82" "B"));
  wmux::daemon_internal::PaneViewportStates viewport_states;
  wmux::daemon_internal::CopyModeState copy_mode;

  const auto rendered =
      wmux::daemon_internal::render_frame(frame, snapshots, viewport_states, copy_mode, "");

  assert(rendered.find("A\xF0\x9F\x99\x82") != std::string::npos);
  assert(rendered.find("A\xF0\x9F\x99\x82" "B") == std::string::npos);
}

void render_status_line_shows_context_mode_and_temporary_message() {
  auto frame = single_pane_frame(80, 5, wmux::PaneLayoutRect{3, 0, 0, 80, 4});
  frame.session_name = "main";
  frame.window_name = "logs";
  frame.window_index = 2;

  std::unordered_map<wmux::PaneId, wmux::PtyOutputSnapshot> snapshots;
  snapshots.emplace(3, snapshot_with_raw_line("ready"));
  wmux::daemon_internal::PaneViewportStates viewport_states;
  wmux::daemon_internal::CopyModeState copy_mode;
  wmux::daemon_internal::RenderStatus status;
  status.mouse_enabled = true;
  wmux::status_set_temporary(
      status.state,
      "wmux: created window",
      std::chrono::steady_clock::now());

  const auto rendered =
      wmux::daemon_internal::render_frame(frame, snapshots, viewport_states, copy_mode, status);

  assert(rendered.find("wmux [main] window 2:logs pane 3") != std::string::npos);
  assert(rendered.find("wmux: created window") != std::string::npos);
  assert(rendered.find("mode:normal") != std::string::npos);
  assert(rendered.find("mouse:on") != std::string::npos);
}

void golden_split_frame_snapshot() {
  wmux::daemon_internal::ActiveWindowFrame frame;
  frame.window_id = 7;
  frame.active_pane_id = 2;
  frame.session_name = "main";
  frame.window_name = "work";
  frame.window_index = 1;
  frame.columns = 48;
  frame.rows = 8;
  frame.status_bar_enabled = true;
  frame.panes.push_back(wmux::daemon_internal::RenderPane{
      wmux::PaneLayoutRect{1, 0, 0, 24, 7}, false, {}});
  frame.panes.push_back(wmux::daemon_internal::RenderPane{
      wmux::PaneLayoutRect{2, 24, 0, 24, 7}, true, {}});

  std::unordered_map<wmux::PaneId, wmux::PtyOutputSnapshot> snapshots;
  snapshots.emplace(
      1,
      snapshot_with_text_lines({"left alpha", "left beta", "left gamma"}, 22));
  snapshots.emplace(
      2,
      snapshot_with_text_lines({"right one", "right two", "right three"}, 22));
  wmux::daemon_internal::PaneViewportStates viewport_states;
  wmux::daemon_internal::CopyModeState copy_mode;
  wmux::daemon_internal::RenderStatus status;
  status.mouse_enabled = true;

  const auto rendered =
      wmux::daemon_internal::render_frame(frame, snapshots, viewport_states, copy_mode, status);

  assert_render_snapshot(
      "split-frame",
      rendered,
      R"SNAP(<clear>
@1;1|
@1;1|+----------------------+
@2;1||______________________|
@3;1||______________________|
@4;1||______________________|
@5;1||______________________|
@6;1||______________________|
@7;1|+----------------------+
@2;2|<sgr:0><sgr:0>left_alpha____________<sgr:0>
@3;2|<sgr:0><sgr:0>left_beta_____________<sgr:0>
@4;2|<sgr:0><sgr:0>left_gamma____________<sgr:0>
@5;2|<sgr:0>______________________
@6;2|<sgr:0>______________________
@1;25|+======================+
@2;25|#______________________#
@3;25|#______________________#
@4;25|#______________________#
@5;25|#______________________#
@6;25|#______________________#
@7;25|+======================+
@2;26|<sgr:0><sgr:0>right_one_____________<sgr:0>
@3;26|<sgr:0><sgr:0>right_two_____________<sgr:0>
@4;26|<sgr:0><sgr:0>right_three___________<sgr:0>
@5;26|<sgr:0>______________________
@6;26|<sgr:0>______________________
@8;1|<sgr:7>_wmux_[main]_window_1:work_pane_2_______________<sgr:0>)SNAP");
}

void golden_copy_mode_selection_snapshot() {
  const auto frame = single_pane_frame(18, 6, wmux::PaneLayoutRect{3, 0, 0, 18, 5});
  std::unordered_map<wmux::PaneId, wmux::PtyOutputSnapshot> snapshots;
  snapshots.emplace(
      3,
      snapshot_with_text_lines({"copy one", "copy two", "copy three"}, 16));
  wmux::daemon_internal::PaneViewportStates viewport_states;
  wmux::daemon_internal::CopyModeState copy_mode;
  copy_mode.active = true;
  copy_mode.pane_id = 3;
  copy_mode.cursor_row = 1;
  copy_mode.cursor_column = 7;
  copy_mode.selection_active = true;
  copy_mode.selection_anchor = wmux::daemon_internal::CopyModePoint{0, 5};

  const auto rendered =
      wmux::daemon_internal::render_frame(frame, snapshots, viewport_states, copy_mode, "");

  assert_render_snapshot(
      "copy-mode-selection",
      rendered,
      R"SNAP(<clear>
@1;1|
@1;1|+================+
@2;1|#________________#
@3;1|#________________#
@4;1|#________________#
@5;1|+================+
@2;2|<sgr:0><sgr:0>copy_<sgr:0;7>one________<sgr:0>
@3;2|<sgr:0><sgr:0;7>copy_two<sgr:0>________<sgr:0>
@4;2|<sgr:0><sgr:0>copy_three______<sgr:0>
@6;1|<sgr:7>_copy-mode_[resize<sgr:0>)SNAP");
}

void golden_partial_dirty_pane_update_snapshot() {
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
  snapshots.emplace(1, snapshot_with_text_lines({"left"}, 8));
  snapshots.emplace(2, snapshot_with_text_lines({"dirty", "pane"}, 8));
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

  assert_render_snapshot(
      "partial-dirty-pane-update",
      rendered,
      R"SNAP(@2;12|<sgr:0><sgr:0>dirty___<sgr:0>
@3;12|<sgr:0><sgr:0>pane____<sgr:0>
@4;12|<sgr:0>________)SNAP");
}

void dirty_region_diff_renders_only_changed_cells() {
  const auto frame = single_pane_frame(10, 6, wmux::PaneLayoutRect{1, 0, 0, 10, 5});
  wmux::daemon_internal::PaneViewportStates viewport_states;
  wmux::daemon_internal::CopyModeState copy_mode;
  wmux::daemon_internal::RenderStatus status;
  wmux::daemon_internal::RenderDiffState diff_state;

  std::unordered_map<wmux::PaneId, wmux::PtyOutputSnapshot> first_snapshots;
  first_snapshots.emplace(
      1,
      snapshot_with_text_lines({"stable", "alpha-1", "stable"}, 8));
  (void)wmux::daemon_internal::render_frame_update(
      frame,
      first_snapshots,
      viewport_states,
      copy_mode,
      status,
      wmux::daemon_internal::RenderFrameOptions{},
      diff_state);

  std::unordered_map<wmux::PaneId, wmux::PtyOutputSnapshot> second_snapshots;
  second_snapshots.emplace(
      1,
      snapshot_with_text_lines({"stable", "alpha-2", "stable"}, 8));
  const auto rendered = wmux::daemon_internal::render_frame_update(
      frame,
      second_snapshots,
      viewport_states,
      copy_mode,
      status,
      wmux::daemon_internal::RenderFrameOptions{
          false,
          false,
          false,
          std::unordered_set<wmux::PaneId>{1}},
      diff_state);

  assert_render_snapshot(
      "dirty-region-cell-diff",
      rendered,
      R"SNAP(@3;8|<sgr:0>2<sgr:0>)SNAP");

  const auto unchanged = wmux::daemon_internal::render_frame_update(
      frame,
      second_snapshots,
      viewport_states,
      copy_mode,
      status,
      wmux::daemon_internal::RenderFrameOptions{
          false,
          false,
          false,
          std::unordered_set<wmux::PaneId>{1}},
      diff_state);
  assert(unchanged.empty());
}

}  // namespace

void run_daemon_render_tests() {
  clamps_viewport_after_resize_growth();
  removes_viewport_state_for_removed_panes();
  clamps_copy_mode_state_after_resize();
  clears_selection_when_copy_pane_becomes_too_small();
  disables_copy_mode_when_pane_disappears();
  render_uses_only_frame_bounds_after_resize();
  renders_smallest_drawable_pane_body();
  partial_render_updates_only_dirty_pane_body();
  render_does_not_split_utf8_fallback_text();
  render_clips_wide_utf8_to_pane_columns();
  render_status_line_shows_context_mode_and_temporary_message();
  golden_split_frame_snapshot();
  golden_copy_mode_selection_snapshot();
  golden_partial_dirty_pane_update_snapshot();
  dirty_region_diff_renders_only_changed_cells();
}
