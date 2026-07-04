#include "daemon_render.hpp"
#include "wmux/terminal_engine.hpp"

#include <cassert>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <span>
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

std::string test_cell_text(const wmux::TerminalCell& cell) {
  if (!cell.extended.empty()) {
    return cell.extended;
  }
  if (cell.width == wmux::TerminalCellWidth::WideContinuation) {
    return {};
  }
  if (cell.codepoint < 0x80) {
    return std::string(1, static_cast<char>(cell.codepoint));
  }
  return wmux::utf8_from_codepoint(cell.codepoint);
}

wmux::TerminalLineSnapshot line_snapshot_from_view(const wmux::TerminalLineView& view) {
  wmux::TerminalLineSnapshot line;
  line.wrapped = view.wrapped;
  line.attributes.reserve(view.cells.size());
  line.cells.reserve(view.cells.size());
  line.cell_widths.reserve(view.cells.size());
  for (const auto& cell : view.cells) {
    const auto text = test_cell_text(cell);
    line.text += text.empty() ? std::string{" "} : text;
    line.attributes.push_back(cell.attributes);
    line.cells.push_back(text.empty() ? std::string{" "} : text);
    line.cell_widths.push_back(cell.width);
  }
  return line;
}

void feed_engine(wmux::ITerminalEngine& engine, std::string_view bytes) {
  const auto* data = reinterpret_cast<const std::byte*>(bytes.data());
  engine.feed(std::span<const std::byte>{data, bytes.size()});
}

wmux::TerminalLineSnapshot styled_blank_line_snapshot(
    int width,
    wmux::TerminalAttributes attributes) {
  wmux::TerminalLineSnapshot line;
  line.text.assign(static_cast<std::size_t>(width), ' ');
  line.attributes.assign(static_cast<std::size_t>(width), attributes);
  line.cells.assign(static_cast<std::size_t>(width), " ");
  line.cell_widths.assign(static_cast<std::size_t>(width), wmux::TerminalCellWidth::Narrow);
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

wmux::PtyOutputSnapshot snapshot_from_pty_bytes(
    std::string_view bytes,
    int columns,
    int rows) {
  wmux::TerminalEngineV2 engine(columns, rows);
  engine.set_scrollback_capacity(1000);
  feed_engine(engine, bytes);

  wmux::PtyOutputSnapshot snapshot;
  snapshot.alive = true;
  snapshot.screen = engine.snapshot(false);
  return snapshot;
}

wmux::TerminalScreenSnapshot simulated_client_after_frame(
    std::string_view frame_bytes,
    int columns,
    int rows) {
  wmux::LegacyTerminalEngine client(columns, rows);
  feed_engine(client, frame_bytes);
  return client.snapshot(false);
}

wmux::TerminalScreenSnapshot render_snapshot_to_simulated_client(
    const wmux::daemon_internal::ActiveWindowFrame& frame,
    const std::unordered_map<wmux::PaneId, wmux::PtyOutputSnapshot>& snapshots,
    const wmux::daemon_internal::PaneViewportStates& viewport_states,
    const wmux::daemon_internal::CopyModeState& copy_mode,
    const wmux::daemon_internal::RenderStatus& status,
    const wmux::daemon_internal::RenderFrameOptions& options,
    wmux::daemon_internal::RenderDiffState* baseline = nullptr) {
  const auto rendered =
      baseline == nullptr
          ? wmux::daemon_internal::render_frame_update(
                frame, snapshots, viewport_states, copy_mode, status, options)
          : wmux::daemon_internal::render_frame_update(
                frame, snapshots, viewport_states, copy_mode, status, options, *baseline);
  return simulated_client_after_frame(rendered, frame.columns, frame.rows);
}

std::string render_snapshot_frame_bytes(
    const wmux::daemon_internal::ActiveWindowFrame& frame,
    const std::unordered_map<wmux::PaneId, wmux::PtyOutputSnapshot>& snapshots,
    const wmux::daemon_internal::PaneViewportStates& viewport_states,
    const wmux::daemon_internal::CopyModeState& copy_mode,
    const wmux::daemon_internal::RenderStatus& status,
    const wmux::daemon_internal::RenderFrameOptions& options,
    wmux::daemon_internal::RenderDiffState& baseline) {
  return wmux::daemon_internal::render_frame_update(
      frame, snapshots, viewport_states, copy_mode, status, options, baseline);
}

wmux::TerminalScreenSnapshot simulated_client_after_frames(
    std::initializer_list<std::string_view> frames,
    int columns,
    int rows) {
  wmux::LegacyTerminalEngine client(columns, rows);
  for (const auto frame : frames) {
    feed_engine(client, frame);
  }
  return client.snapshot(false);
}

std::string visible_line(
    const wmux::TerminalScreenSnapshot& screen,
    int row) {
  assert(row >= 0);
  assert(row < static_cast<int>(screen.lines.size()));
  return screen.lines[static_cast<std::size_t>(row)];
}

bool line_contains(
    const wmux::TerminalScreenSnapshot& screen,
    int row,
    std::string_view needle) {
  return visible_line(screen, row).find(needle) != std::string::npos;
}

bool screen_contains(
    const wmux::TerminalScreenSnapshot& screen,
    std::string_view needle) {
  for (int row = 0; row < static_cast<int>(screen.lines.size()); ++row) {
    if (line_contains(screen, row, needle)) {
      return true;
    }
  }
  return false;
}

[[noreturn]] void fail_screen_contains(
    std::string_view name,
    const wmux::TerminalScreenSnapshot& screen,
    std::string_view needle) {
  std::cerr << "simulated client scene mismatch: " << name << "\nmissing: "
            << needle << "\nvisible rows:\n";
  for (const auto& line : screen.lines) {
    std::cerr << line << "\n";
  }
  std::exit(1);
}

void expect_screen_contains(
    std::string_view name,
    const wmux::TerminalScreenSnapshot& screen,
    std::string_view needle) {
  if (!screen_contains(screen, needle)) {
    fail_screen_contains(name, screen, needle);
  }
}

std::size_t nonblank_count(const wmux::TerminalScreenSnapshot& screen) {
  std::size_t count = 0;
  for (const auto& line : screen.lines) {
    for (const char ch : line) {
      if (ch != ' ') {
        ++count;
      }
    }
  }
  return count;
}

bool has_styled_blank_at(
    const wmux::TerminalScreenSnapshot& screen,
    int row,
    int column,
    std::int32_t background) {
  if (row < 0 || row >= static_cast<int>(screen.line_snapshots.size())) {
    return false;
  }
  const auto& line = screen.line_snapshots[static_cast<std::size_t>(row)];
  if (column < 0 || column >= static_cast<int>(line.cells.size()) ||
      column >= static_cast<int>(line.attributes.size())) {
    return false;
  }
  return line.cells[static_cast<std::size_t>(column)] == " " &&
         line.attributes[static_cast<std::size_t>(column)].background == background;
}

bool screen_has_styled_blank(
    const wmux::TerminalScreenSnapshot& screen,
    std::int32_t background) {
  for (int row = 0; row < static_cast<int>(screen.line_snapshots.size()); ++row) {
    const auto& line = screen.line_snapshots[static_cast<std::size_t>(row)];
    for (int column = 0; column < static_cast<int>(line.cells.size()); ++column) {
      if (has_styled_blank_at(screen, row, column, background)) {
        return true;
      }
    }
  }
  return false;
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
        const auto next = cursor + 1;
        const bool trailing_cursor_visibility =
            next + 5 < rendered.size() && rendered[next] == '\x1b' &&
            rendered[next + 1] == '[' && rendered[next + 2] == '?' &&
            rendered[next + 3] == '2' && rendered[next + 4] == '5' &&
            (rendered[next + 5] == 'h' || rendered[next + 5] == 'l') &&
            next + 6 == rendered.size();
        if (next == rendered.size() || trailing_cursor_visibility) {
          index = trailing_cursor_visibility ? next + 6 : next;
          continue;
        }
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
      } else if ((final_byte == 'h' || final_byte == 'l') && params == "?25") {
        // Cursor visibility is asserted in targeted tests; most frame snapshots
        // care about cells, borders, and status text.
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
  frame.pane_rows = rect.top + rect.height;
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
  assert(viewport_states[1].offset == 26);

  frame = single_pane_frame(20, 12, wmux::PaneLayoutRect{1, 0, 0, 20, 11});
  wmux::daemon_internal::update_viewport_states(
      frame, snapshots, viewport_states, copy_mode);
  assert(viewport_states[1].offset == 19);
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
  assert(copy_mode.cursor_row == 3);
  assert(copy_mode.cursor_column == 4);
  assert(copy_mode.selection_active);
  assert(copy_mode.selection_anchor.line == 9);
  assert(copy_mode.selection_anchor.column == 4);
}

void clears_selection_when_copy_pane_becomes_too_small() {
  const auto frame = single_pane_frame(2, 2, wmux::PaneLayoutRect{1, 0, 0, 0, 2});
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
  assert(wmux::daemon_internal::body_width(wmux::PaneLayoutRect{1, 0, 0, 3, 3}) == 3);
  assert(wmux::daemon_internal::body_height(wmux::PaneLayoutRect{1, 0, 0, 3, 3}) == 3);
  assert(wmux::daemon_internal::body_width(wmux::PaneLayoutRect{1, 0, 0, 3, 3}, 4) == 2);
  assert(wmux::daemon_internal::body_height(wmux::PaneLayoutRect{1, 0, 0, 3, 3}, 4) == 2);

  const auto frame = single_pane_frame(3, 4, wmux::PaneLayoutRect{1, 0, 0, 3, 3});
  std::unordered_map<wmux::PaneId, wmux::PtyOutputSnapshot> snapshots;
  snapshots.emplace(1, snapshot_with_raw_line("Z"));
  wmux::daemon_internal::PaneViewportStates viewport_states;
  wmux::daemon_internal::CopyModeState copy_mode;

  const auto rendered =
      wmux::daemon_internal::render_frame(frame, snapshots, viewport_states, copy_mode, "");

  assert(rendered.find("\x1b[1;1H") != std::string::npos);
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
      [] {
        wmux::daemon_internal::RenderFrameOptions options;
        options.clear_terminal = false;
        options.draw_borders = false;
        options.draw_status = false;
        options.dirty_panes = std::unordered_set<wmux::PaneId>{2};
        return options;
      }());

  assert(rendered.find("\x1b[2J") == std::string::npos);
  assert(rendered.find('+') == std::string::npos);
  assert(rendered.find("wmux [perf]") == std::string::npos);
  assert(rendered.find("\x1b[2;11H") != std::string::npos);
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
  const auto frame = single_pane_frame(3, 4, wmux::PaneLayoutRect{1, 0, 0, 3, 3});
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

  assert(rendered.find("[main] 2:logs*") != std::string::npos);
  assert(rendered.find("wmux: created window") != std::string::npos);
  assert(rendered.find("\"pane 3\"") != std::string::npos);
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

  const auto normalized = normalized_render_snapshot(rendered);
  assert(normalized.find("@1;1|") != std::string::npos);
  assert(normalized.find("left_alpha") != std::string::npos);
  assert(normalized.find("@1;25|") != std::string::npos);
  assert(normalized.find("right_one") != std::string::npos);
  assert(normalized.find("[main]_1:work*") != std::string::npos);
  assert(normalized.find("\"pane_2\"") != std::string::npos);
  assert(normalized.find("@2;24|") != std::string::npos);
  assert(normalized.find("@2;25|│") == std::string::npos);
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

  const auto normalized = normalized_render_snapshot(rendered);
  assert(normalized.find("[copy-") != std::string::npos);
  assert(normalized.find("copy_<sgr:37;48;5;4>one") != std::string::npos);
  assert(normalized.find("<sgr:37;48;5;4>copy_two") != std::string::npos);
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
      [] {
        wmux::daemon_internal::RenderFrameOptions options;
        options.clear_terminal = false;
        options.draw_borders = false;
        options.draw_status = false;
        options.dirty_panes = std::unordered_set<wmux::PaneId>{2};
        return options;
      }());

  assert_render_snapshot(
      "partial-dirty-pane-update",
      rendered,
      R"SNAP(@1;1|<sgr:0>left_____
@2;1|<sgr:0>_________
@3;1|<sgr:0>_________
@4;1|<sgr:0>_________
@5;1|<sgr:0>_________
@1;11|<sgr:0>dirty_____
@2;11|<sgr:0>pane______
@3;11|<sgr:0>__________
@4;11|<sgr:0>__________
@5;11|<sgr:0>__________
@1;10|<sgr:0><sgr:38;5;4>│<sgr:0>
@2;10|<sgr:0><sgr:38;5;4>│<sgr:0>
@3;10|<sgr:0><sgr:38;5;4>│<sgr:0>
@4;10|<sgr:0><sgr:38;5;4>│<sgr:0>
@5;10|<sgr:0><sgr:38;5;4>│<sgr:0>
@6;1|<sgr:0><sgr:37;48;5;4>[perf]_0:m________"p<sgr:0>)SNAP");
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
      [] {
        wmux::daemon_internal::RenderFrameOptions options;
        options.clear_terminal = false;
        options.draw_borders = false;
        options.draw_status = false;
        options.dirty_panes = std::unordered_set<wmux::PaneId>{1};
        return options;
      }(),
      diff_state);

  assert_render_snapshot(
      "dirty-region-cell-diff",
      rendered,
      R"SNAP(@2;7|<sgr:0>2<sgr:0>)SNAP");

  const auto unchanged = wmux::daemon_internal::render_frame_update(
      frame,
      second_snapshots,
      viewport_states,
      copy_mode,
      status,
      [] {
        wmux::daemon_internal::RenderFrameOptions options;
        options.clear_terminal = false;
        options.draw_borders = false;
        options.draw_status = false;
        options.dirty_panes = std::unordered_set<wmux::PaneId>{1};
        return options;
      }(),
      diff_state);
  assert(normalized_render_snapshot(unchanged).empty());
}

void renderer_materializes_styled_blank_cells() {
  const auto frame = single_pane_frame(8, 3, wmux::PaneLayoutRect{1, 0, 0, 8, 2});
  wmux::daemon_internal::PaneViewportStates viewport_states;
  wmux::daemon_internal::CopyModeState copy_mode;
  wmux::daemon_internal::RenderStatus status;

  wmux::TerminalAttributes attrs;
  attrs.background = 4;

  wmux::PtyOutputSnapshot snapshot;
  snapshot.alive = true;
  snapshot.screen.columns = 8;
  snapshot.screen.rows = 1;
  snapshot.screen.lines.push_back("        ");
  snapshot.screen.line_snapshots.push_back(styled_blank_line_snapshot(8, attrs));

  std::unordered_map<wmux::PaneId, wmux::PtyOutputSnapshot> snapshots;
  snapshots.emplace(1, std::move(snapshot));

  const auto rendered = wmux::daemon_internal::render_frame_update(
      frame,
      snapshots,
      viewport_states,
      copy_mode,
      status,
      wmux::daemon_internal::RenderFrameOptions{});

  assert(rendered.find("\x1b[0;44m") != std::string::npos);
}

void layout_only_skips_body_when_geometry_is_stable() {
  const auto frame = single_pane_frame(12, 5, wmux::PaneLayoutRect{1, 0, 0, 12, 4});
  wmux::daemon_internal::PaneViewportStates viewport_states;
  wmux::daemon_internal::CopyModeState copy_mode;
  wmux::daemon_internal::RenderStatus status;
  wmux::daemon_internal::RenderDiffState diff_state;

  std::unordered_map<wmux::PaneId, wmux::PtyOutputSnapshot> snapshots;
  snapshots.emplace(1, snapshot_with_text_lines({"stable body"}, 12));
  (void)wmux::daemon_internal::render_frame_update(
      frame,
      snapshots,
      viewport_states,
      copy_mode,
      status,
      wmux::daemon_internal::RenderFrameOptions{},
      diff_state);

  wmux::daemon_internal::RenderFrameOptions layout_only;
  layout_only.clear_terminal = false;
  layout_only.draw_borders = true;
  layout_only.draw_status = true;
  layout_only.draw_pane_bodies = false;
  layout_only.preserve_layout_cache = true;
  layout_only.repaint_body_on_geometry_change = true;

  const auto rendered = wmux::daemon_internal::render_frame_update(
      frame,
      snapshots,
      viewport_states,
      copy_mode,
      status,
      layout_only,
      diff_state);

  assert(rendered.find("stable body") == std::string::npos);
}

void layout_only_repaints_body_when_geometry_changes() {
  const auto first_frame = single_pane_frame(12, 5, wmux::PaneLayoutRect{1, 0, 0, 12, 4});
  auto second_frame = first_frame;
  second_frame.layout_generation = first_frame.layout_generation + 1;
  second_frame.panes[0].rect = wmux::PaneLayoutRect{1, 0, 0, 10, 4};
  wmux::daemon_internal::PaneViewportStates viewport_states;
  wmux::daemon_internal::CopyModeState copy_mode;
  wmux::daemon_internal::RenderStatus status;
  wmux::daemon_internal::RenderDiffState diff_state;

  std::unordered_map<wmux::PaneId, wmux::PtyOutputSnapshot> snapshots;
  snapshots.emplace(1, snapshot_with_text_lines({"body moved"}, 12));
  (void)wmux::daemon_internal::render_frame_update(
      first_frame,
      snapshots,
      viewport_states,
      copy_mode,
      status,
      wmux::daemon_internal::RenderFrameOptions{},
      diff_state);

  wmux::daemon_internal::RenderFrameOptions layout_only;
  layout_only.clear_terminal = false;
  layout_only.draw_borders = true;
  layout_only.draw_status = true;
  layout_only.draw_pane_bodies = false;
  layout_only.preserve_layout_cache = true;
  layout_only.repaint_body_on_geometry_change = true;

  const auto rendered = wmux::daemon_internal::render_frame_update(
      second_frame,
      snapshots,
      viewport_states,
      copy_mode,
      status,
      layout_only,
      diff_state);

  assert(rendered.find("body move") != std::string::npos);
}

void layout_only_preserves_baseline_across_layout_generation_change() {
  const auto first_frame = single_pane_frame(12, 5, wmux::PaneLayoutRect{1, 0, 0, 12, 4});
  auto second_frame = first_frame;
  second_frame.layout_generation = first_frame.layout_generation + 1;
  wmux::daemon_internal::PaneViewportStates viewport_states;
  wmux::daemon_internal::CopyModeState copy_mode;
  wmux::daemon_internal::RenderStatus status;
  wmux::daemon_internal::RenderDiffState diff_state;

  std::unordered_map<wmux::PaneId, wmux::PtyOutputSnapshot> snapshots;
  snapshots.emplace(1, snapshot_with_text_lines({"preserved body"}, 12));
  (void)wmux::daemon_internal::render_frame_update(
      first_frame,
      snapshots,
      viewport_states,
      copy_mode,
      status,
      wmux::daemon_internal::RenderFrameOptions{},
      diff_state);

  assert(diff_state.baseline_valid);
  assert(diff_state.initialized);
  assert(diff_state.layout_generation == first_frame.layout_generation);
  assert(diff_state.panes.contains(1));

  wmux::daemon_internal::RenderFrameOptions layout_only;
  layout_only.clear_terminal = false;
  layout_only.draw_borders = true;
  layout_only.draw_status = true;
  layout_only.draw_pane_bodies = false;
  layout_only.preserve_layout_cache = true;
  layout_only.repaint_body_on_geometry_change = true;

  const auto rendered = wmux::daemon_internal::render_frame_update(
      second_frame,
      snapshots,
      viewport_states,
      copy_mode,
      status,
      layout_only,
      diff_state);

  assert(rendered.find("preserved body") == std::string::npos);
  assert(diff_state.baseline_valid);
  assert(diff_state.initialized);
  assert(diff_state.layout_generation == second_frame.layout_generation);
  assert(diff_state.panes.contains(1));
}

void layout_only_resize_is_atomic_and_preserves_baseline() {
  const auto first_frame = single_pane_frame(12, 5, wmux::PaneLayoutRect{1, 0, 0, 12, 4});
  auto resized_frame = single_pane_frame(16, 6, wmux::PaneLayoutRect{1, 0, 0, 16, 5});
  resized_frame.window_id = first_frame.window_id;
  resized_frame.layout_generation = first_frame.layout_generation + 1;
  wmux::daemon_internal::PaneViewportStates viewport_states;
  wmux::daemon_internal::CopyModeState copy_mode;
  wmux::daemon_internal::RenderStatus status;
  status.synchronized_output_supported = true;
  wmux::daemon_internal::RenderDiffState diff_state;

  std::unordered_map<wmux::PaneId, wmux::PtyOutputSnapshot> snapshots;
  snapshots.emplace(1, snapshot_with_text_lines({"resize baseline"}, 16));
  (void)wmux::daemon_internal::render_frame_update(
      first_frame,
      snapshots,
      viewport_states,
      copy_mode,
      status,
      wmux::daemon_internal::RenderFrameOptions{},
      diff_state);

  wmux::daemon_internal::RenderFrameOptions layout_only;
  layout_only.clear_terminal = false;
  layout_only.draw_borders = true;
  layout_only.draw_status = true;
  layout_only.draw_pane_bodies = false;
  layout_only.preserve_layout_cache = true;
  layout_only.repaint_body_on_geometry_change = true;

  const auto rendered = wmux::daemon_internal::render_frame_update(
      resized_frame,
      snapshots,
      viewport_states,
      copy_mode,
      status,
      layout_only,
      diff_state);

  assert(rendered.find("\x1b[2J\x1b[H") == std::string::npos);
  assert(rendered.find("\x1b[?2026h") != std::string::npos);
  assert(rendered.find("\x1b[?2026l") != std::string::npos);
  assert(diff_state.baseline_valid);
  assert(diff_state.initialized);
  assert(diff_state.columns == resized_frame.columns);
  assert(diff_state.rows == resized_frame.rows);
  assert(diff_state.layout_generation == resized_frame.layout_generation);
  assert(diff_state.panes.contains(1));
}

void layout_only_removes_disappeared_pane_from_baseline() {
  wmux::daemon_internal::ActiveWindowFrame first_frame;
  first_frame.window_id = 1;
  first_frame.layout_generation = 1;
  first_frame.active_pane_id = 1;
  first_frame.session_name = "s";
  first_frame.window_name = "w";
  first_frame.columns = 20;
  first_frame.rows = 5;
  first_frame.pane_rows = 4;
  first_frame.status_bar_enabled = true;
  first_frame.panes.push_back(wmux::daemon_internal::RenderPane{
      wmux::PaneLayoutRect{1, 0, 0, 10, 4}, true, {}});
  first_frame.panes.push_back(wmux::daemon_internal::RenderPane{
      wmux::PaneLayoutRect{2, 10, 0, 10, 4}, false, {}});
  auto second_frame = first_frame;
  second_frame.layout_generation = 2;
  second_frame.panes.pop_back();
  second_frame.panes[0].rect = wmux::PaneLayoutRect{1, 0, 0, 20, 4};

  wmux::daemon_internal::PaneViewportStates viewport_states;
  wmux::daemon_internal::CopyModeState copy_mode;
  wmux::daemon_internal::RenderStatus status;
  wmux::daemon_internal::RenderDiffState diff_state;
  std::unordered_map<wmux::PaneId, wmux::PtyOutputSnapshot> snapshots;
  snapshots.emplace(1, snapshot_with_text_lines({"left"}, 20));
  snapshots.emplace(2, snapshot_with_text_lines({"right"}, 10));

  (void)wmux::daemon_internal::render_frame_update(
      first_frame,
      snapshots,
      viewport_states,
      copy_mode,
      status,
      wmux::daemon_internal::RenderFrameOptions{},
      diff_state);
  assert(diff_state.panes.contains(1));
  assert(diff_state.panes.contains(2));

  snapshots.erase(2);
  wmux::daemon_internal::RenderFrameOptions layout_only;
  layout_only.clear_terminal = false;
  layout_only.draw_borders = true;
  layout_only.draw_status = true;
  layout_only.draw_pane_bodies = false;
  layout_only.preserve_layout_cache = true;
  layout_only.repaint_body_on_geometry_change = true;

  (void)wmux::daemon_internal::render_frame_update(
      second_frame,
      snapshots,
      viewport_states,
      copy_mode,
      status,
      layout_only,
      diff_state);

  assert(diff_state.baseline_valid);
  assert(diff_state.panes.contains(1));
  assert(!diff_state.panes.contains(2));
}

void renders_active_pane_cursor_after_frame() {
  const auto frame = single_pane_frame(10, 6, wmux::PaneLayoutRect{1, 0, 0, 10, 5});
  wmux::daemon_internal::PaneViewportStates viewport_states;
  wmux::daemon_internal::CopyModeState copy_mode;
  wmux::daemon_internal::RenderStatus status;

  std::unordered_map<wmux::PaneId, wmux::PtyOutputSnapshot> snapshots;
  auto snapshot = snapshot_with_text_lines({"abc"}, 8);
  snapshot.screen.cursor_row = 0;
  snapshot.screen.cursor_column = 3;
  snapshot.screen.cursor_visible = true;
  snapshots.emplace(1, std::move(snapshot));

  const auto rendered = wmux::daemon_internal::render_frame_update(
      frame,
      snapshots,
      viewport_states,
      copy_mode,
      status,
      wmux::daemon_internal::RenderFrameOptions{});

  assert(rendered.find("\x1b[1;4H\x1b[?25h") != std::string::npos);
}

void renderer_applies_configured_ui_accent_and_tmux_style() {
  wmux::daemon_internal::ActiveWindowFrame frame;
  frame.window_id = 1;
  frame.active_pane_id = 2;
  frame.session_name = "ui";
  frame.window_name = "main";
  frame.columns = 10;
  frame.rows = 4;
  frame.pane_rows = 3;
  frame.status_bar_enabled = true;
  frame.panes.push_back(wmux::daemon_internal::RenderPane{
      wmux::PaneLayoutRect{1, 0, 0, 5, 3}, false, {}});
  frame.panes.push_back(wmux::daemon_internal::RenderPane{
      wmux::PaneLayoutRect{2, 5, 0, 5, 3}, true, {}});
  wmux::daemon_internal::PaneViewportStates viewport_states;
  wmux::daemon_internal::CopyModeState copy_mode;
  std::unordered_map<wmux::PaneId, wmux::PtyOutputSnapshot> snapshots;
  snapshots.emplace(1, snapshot_with_raw_line("ready"));
  snapshots.emplace(2, snapshot_with_raw_line("set"));

  wmux::daemon_internal::RenderStatus red_status;
  red_status.ui.accent = *wmux::parse_ui_color("red");
  red_status.ui.accent_spec = "red";
  const auto red = wmux::daemon_internal::render_frame_update(
      frame,
      snapshots,
      viewport_states,
      copy_mode,
      red_status,
      wmux::daemon_internal::RenderFrameOptions{});
  assert(red.find("\x1b[38;5;1m") != std::string::npos);
  assert(red.find("\x1b[37;48;5;1m") != std::string::npos);

  wmux::daemon_internal::RenderStatus tmux_status;
  tmux_status.ui.tmux_style = true;
  const auto tmux = wmux::daemon_internal::render_frame_update(
      frame,
      snapshots,
      viewport_states,
      copy_mode,
      tmux_status,
      wmux::daemon_internal::RenderFrameOptions{});
  assert(tmux.find("\x1b[38;5;2m") != std::string::npos);
  assert(tmux.find("\x1b[30;48;5;2m") != std::string::npos);

  wmux::daemon_internal::RenderStatus ascii_status;
  ascii_status.ui.smooth_borders = false;
  const auto ascii = wmux::daemon_internal::render_frame_update(
      frame,
      snapshots,
      viewport_states,
      copy_mode,
      ascii_status,
      wmux::daemon_internal::RenderFrameOptions{});
  assert(ascii.find('|') != std::string::npos);
  assert(ascii.find("\xE2\x94\x8C") == std::string::npos);
}

void shared_borders_close_nested_split_connector_gaps() {
  wmux::daemon_internal::ActiveWindowFrame frame;
  frame.window_id = 1;
  frame.active_pane_id = 2;
  frame.session_name = "ui";
  frame.window_name = "main";
  frame.columns = 20;
  frame.rows = 9;
  frame.status_bar_enabled = false;
  frame.panes.push_back(wmux::daemon_internal::RenderPane{
      wmux::PaneLayoutRect{1, 0, 0, 10, 9}, false, {}});
  frame.panes.push_back(wmux::daemon_internal::RenderPane{
      wmux::PaneLayoutRect{2, 10, 0, 10, 4}, true, {}});
  frame.panes.push_back(wmux::daemon_internal::RenderPane{
      wmux::PaneLayoutRect{3, 10, 4, 10, 5}, false, {}});

  std::unordered_map<wmux::PaneId, wmux::PtyOutputSnapshot> snapshots;
  snapshots.emplace(1, snapshot_with_text_lines({"left"}, 8));
  snapshots.emplace(2, snapshot_with_text_lines({"top"}, 8));
  snapshots.emplace(3, snapshot_with_text_lines({"bottom"}, 8));
  wmux::daemon_internal::PaneViewportStates viewport_states;
  wmux::daemon_internal::CopyModeState copy_mode;
  wmux::daemon_internal::RenderStatus status;

  const auto rendered = wmux::daemon_internal::render_frame_update(
      frame,
      snapshots,
      viewport_states,
      copy_mode,
      status,
      wmux::daemon_internal::RenderFrameOptions{});

  assert(rendered.find("\xE2\x94\x9C") != std::string::npos);  // ├
  assert(rendered.find("\xE2\x94\xA4") == std::string::npos);  // ┤
}

void entering_copy_mode_starts_at_live_pane_cursor() {
  const auto frame = single_pane_frame(20, 6, wmux::PaneLayoutRect{1, 0, 0, 20, 5});
  std::unordered_map<wmux::PaneId, wmux::PtyOutputSnapshot> snapshots;
  auto snapshot = snapshot_with_text_lines({"first", "middle", "last"}, 18);
  snapshot.screen.cursor_row = 1;
  snapshot.screen.cursor_column = 4;
  snapshots.emplace(1, std::move(snapshot));

  wmux::daemon_internal::PaneViewportStates viewport_states;
  wmux::daemon_internal::CopyModeState copy_mode;
  std::string copied_text;

  const bool applied = wmux::daemon_internal::apply_copy_mode_action(
      frame,
      snapshots,
      viewport_states,
      copy_mode,
      wmux::AttachCopyModeAction::Enter,
      copied_text);

  assert(applied);
  assert(copy_mode.active);
  assert(copy_mode.pane_id == 1);
  assert(copy_mode.cursor_row == 1);
  assert(copy_mode.cursor_column == 4);
}

void full_scene_materialization_creates_valid_client_baseline() {
  const auto frame = single_pane_frame(12, 5, wmux::PaneLayoutRect{1, 0, 0, 12, 4});
  std::unordered_map<wmux::PaneId, wmux::PtyOutputSnapshot> snapshots;
  snapshots.emplace(1, snapshot_with_text_lines({"baseline"}, 12));
  wmux::daemon_internal::PaneViewportStates viewport_states;
  wmux::daemon_internal::CopyModeState copy_mode;
  wmux::daemon_internal::RenderStatus status;
  wmux::daemon_internal::RenderDiffState baseline;

  const auto rendered = wmux::daemon_internal::render_frame_update(
      frame,
      snapshots,
      viewport_states,
      copy_mode,
      status,
      wmux::daemon_internal::RenderFrameOptions{},
      baseline);

  assert(!rendered.empty());
  assert(baseline.baseline_valid);
  assert(baseline.initialized);
  assert(baseline.window_id == frame.window_id);
  assert(baseline.columns == frame.columns);
  assert(baseline.rows == frame.rows);

  wmux::daemon_internal::reset_render_diff_state(baseline);
  assert(!baseline.baseline_valid);
  assert(!baseline.initialized);
}

std::string scene_row_plain_text(const wmux::daemon_internal::SceneRow& row) {
  std::string text;
  for (const auto& span : row.spans) {
    for (const auto& cell : span.cells) {
      text += cell.text;
    }
  }
  return text;
}

bool scene_contains_text(
    const wmux::daemon_internal::VisibleScene& scene,
    std::string_view needle) {
  for (const auto& pane : scene.panes) {
    for (const auto& row : pane.body_rows) {
      if (scene_row_plain_text(row).find(needle) != std::string::npos) {
        return true;
      }
    }
  }
  if (scene.status.text.find(needle) != std::string::npos) {
    return true;
  }
  return false;
}

bool scene_has_styled_blank(
    const wmux::daemon_internal::VisibleScene& scene,
    std::int32_t background) {
  for (const auto& pane : scene.panes) {
    for (const auto& row : pane.body_rows) {
      for (const auto& span : row.spans) {
        for (const auto& cell : span.cells) {
          if (cell.text == " " && cell.attributes.background == background) {
            return true;
          }
        }
      }
    }
  }
  return false;
}

void visible_scene_materializes_all_core_surfaces() {
  auto frame = single_pane_frame(24, 5, wmux::PaneLayoutRect{1, 0, 0, 24, 4});
  frame.session_name = "scene";
  frame.window_name = "main";
  std::unordered_map<wmux::PaneId, wmux::PtyOutputSnapshot> snapshots;
  snapshots.emplace(
      1,
      snapshot_from_pty_bytes(
          "\x1b[44m                        \x1b[0m\r\n"
          "\x1b[44m  \x1b[37mAsk wmux>\x1b[44m           \x1b[0m\x1b[2;12H",
          24,
          4));
  wmux::daemon_internal::PaneViewportStates viewport_states;
  wmux::daemon_internal::CopyModeState copy_mode;
  wmux::daemon_internal::RenderStatus status;

  const auto scene = wmux::daemon_internal::build_visible_scene(
      frame,
      snapshots,
      viewport_states,
      copy_mode,
      status);

  assert(scene.window_id == frame.window_id);
  assert(scene.layout_generation == frame.layout_generation);
  assert(scene.columns == frame.columns);
  assert(scene.rows == frame.rows);
  assert(scene.panes.size() == 1);
  assert(scene.panes[0].body_rows.size() == 4);
  assert(!scene.borders.rows.empty() || frame.panes.size() == 1);
  assert(scene.status.visible);
  assert(!scene.status.rows.empty());
  assert(scene.cursor.known);
  assert(scene.cursor.visible);
  assert(scene_contains_text(scene, "Ask wmux>"));
  assert(scene_has_styled_blank(scene, 4));
}

void client_baseline_stores_visible_scene_after_successful_materialization() {
  const auto frame = single_pane_frame(18, 5, wmux::PaneLayoutRect{1, 0, 0, 18, 4});
  std::unordered_map<wmux::PaneId, wmux::PtyOutputSnapshot> snapshots;
  snapshots.emplace(1, snapshot_from_pty_bytes("scene baseline", 18, 4));
  wmux::daemon_internal::PaneViewportStates viewport_states;
  wmux::daemon_internal::CopyModeState copy_mode;
  wmux::daemon_internal::RenderStatus status;
  wmux::daemon_internal::RenderDiffState baseline;

  const auto rendered = wmux::daemon_internal::render_frame_update(
      frame,
      snapshots,
      viewport_states,
      copy_mode,
      status,
      wmux::daemon_internal::RenderFrameOptions{},
      baseline);

  assert(!rendered.empty());
  assert(baseline.baseline_valid);
  assert(baseline.initialized);
  assert(baseline.scene_valid);
  assert(baseline.scene.window_id == frame.window_id);
  assert(baseline.scene.panes.size() == 1);
  assert(scene_contains_text(baseline.scene, "scene baseline"));
  assert(baseline.scene.cursor.known);
}

void layout_only_commits_new_scene_without_forgetting_pane_identity() {
  const auto first_frame = single_pane_frame(20, 6, wmux::PaneLayoutRect{1, 0, 0, 20, 5});
  auto second_frame = first_frame;
  second_frame.layout_generation += 1;
  second_frame.panes[0].rect = wmux::PaneLayoutRect{1, 0, 0, 18, 5};

  std::unordered_map<wmux::PaneId, wmux::PtyOutputSnapshot> snapshots;
  snapshots.emplace(1, snapshot_from_pty_bytes("same pane identity", 20, 5));
  wmux::daemon_internal::PaneViewportStates viewport_states;
  wmux::daemon_internal::CopyModeState copy_mode;
  wmux::daemon_internal::RenderStatus status;
  wmux::daemon_internal::RenderDiffState baseline;

  (void)wmux::daemon_internal::render_frame_update(
      first_frame,
      snapshots,
      viewport_states,
      copy_mode,
      status,
      wmux::daemon_internal::RenderFrameOptions{},
      baseline);

  wmux::daemon_internal::RenderFrameOptions layout_only;
  layout_only.clear_terminal = false;
  layout_only.draw_borders = true;
  layout_only.draw_status = true;
  layout_only.draw_pane_bodies = false;
  layout_only.preserve_layout_cache = true;
  layout_only.repaint_body_on_geometry_change = true;

  (void)wmux::daemon_internal::render_frame_update(
      second_frame,
      snapshots,
      viewport_states,
      copy_mode,
      status,
      layout_only,
      baseline);

  assert(baseline.scene_valid);
  assert(baseline.scene.layout_generation == second_frame.layout_generation);
  assert(baseline.scene.panes.size() == 1);
  assert(baseline.scene.panes[0].pane_id == 1);
  assert(baseline.scene.panes[0].rect.width == 18);
  assert(scene_contains_text(baseline.scene, "same pane ident"));
}

void verification_harness_materializes_model_to_simulated_client() {
  auto frame = single_pane_frame(40, 8, wmux::PaneLayoutRect{1, 0, 0, 40, 7});
  frame.session_name = "attach";
  frame.window_name = "long";

  std::string pty_bytes;
  for (int index = 0; index < 48; ++index) {
    pty_bytes += "history ";
    pty_bytes += std::to_string(index);
    pty_bytes += "\r\n";
  }
  pty_bytes += "READY current prompt>";

  std::unordered_map<wmux::PaneId, wmux::PtyOutputSnapshot> snapshots;
  snapshots.emplace(1, snapshot_from_pty_bytes(pty_bytes, 40, 7));
  wmux::daemon_internal::PaneViewportStates viewport_states;
  wmux::daemon_internal::CopyModeState copy_mode;
  wmux::daemon_internal::RenderStatus status;
  wmux::daemon_internal::RenderDiffState baseline;

  const auto client = render_snapshot_to_simulated_client(
      frame,
      snapshots,
      viewport_states,
      copy_mode,
      status,
      wmux::daemon_internal::RenderFrameOptions{},
      &baseline);

  assert(nonblank_count(client) > 0);
  assert(screen_contains(client, "READY current prompt>"));
  assert(baseline.baseline_valid);
  assert(baseline.initialized);
}

void tmux_reference_attach_long_session_renders_latest_viewport() {
  auto frame = single_pane_frame(32, 7, wmux::PaneLayoutRect{1, 0, 0, 32, 6});
  frame.session_name = "tmux-ref";
  frame.window_name = "attach";

  std::string pty_bytes;
  for (int index = 0; index < 120; ++index) {
    pty_bytes += "line ";
    pty_bytes += std::to_string(index);
    pty_bytes += "\r\n";
  }
  pty_bytes += "final visible row";

  std::unordered_map<wmux::PaneId, wmux::PtyOutputSnapshot> snapshots;
  snapshots.emplace(1, snapshot_from_pty_bytes(pty_bytes, 32, 6));
  wmux::daemon_internal::PaneViewportStates viewport_states;
  wmux::daemon_internal::CopyModeState copy_mode;
  wmux::daemon_internal::RenderStatus status;

  const auto client = render_snapshot_to_simulated_client(
      frame,
      snapshots,
      viewport_states,
      copy_mode,
      status,
      wmux::daemon_internal::RenderFrameOptions{});

  assert(screen_contains(client, "final visible row"));
  assert(!screen_contains(client, "line 0"));
}

void tmux_reference_alternate_screen_tui_renders_current_scene() {
  auto frame = single_pane_frame(18, 6, wmux::PaneLayoutRect{1, 0, 0, 18, 5});
  frame.status_bar_enabled = false;

  const std::string pty_bytes =
      "normal screen\r\n"
      "\x1b[?1049h"
      "\x1b[2J\x1b[1;1H"
      "\xE2\x94\x8C" "box" "\xE2\x94\x90\r\n"
      "\xE2\x94\x82ui \xE2\x94\x82\r\n"
      "\xE2\x94\x94\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x98";

  std::unordered_map<wmux::PaneId, wmux::PtyOutputSnapshot> snapshots;
  snapshots.emplace(1, snapshot_from_pty_bytes(pty_bytes, 18, 5));
  wmux::daemon_internal::PaneViewportStates viewport_states;
  wmux::daemon_internal::CopyModeState copy_mode;
  wmux::daemon_internal::RenderStatus status;

  const auto client = render_snapshot_to_simulated_client(
      frame,
      snapshots,
      viewport_states,
      copy_mode,
      status,
      wmux::daemon_internal::RenderFrameOptions{});

  assert(screen_contains(client, "\xE2\x94\x8C" "box" "\xE2\x94\x90"));
  assert(screen_contains(client, "\xE2\x94\x82ui \xE2\x94\x82"));
  assert(!screen_contains(client, "normal screen"));
}

void tmux_reference_styled_prompt_input_box_preserves_styled_blanks() {
  auto frame = single_pane_frame(24, 5, wmux::PaneLayoutRect{1, 0, 0, 24, 4});
  frame.status_bar_enabled = false;

  const std::string pty_bytes =
      "\x1b[44m                        \x1b[0m\r\n"
      "\x1b[44m  \x1b[37mAsk wmux>\x1b[44m           \x1b[0m";

  std::unordered_map<wmux::PaneId, wmux::PtyOutputSnapshot> snapshots;
  snapshots.emplace(1, snapshot_from_pty_bytes(pty_bytes, 24, 4));
  wmux::daemon_internal::PaneViewportStates viewport_states;
  wmux::daemon_internal::CopyModeState copy_mode;
  wmux::daemon_internal::RenderStatus status;

  const auto client = render_snapshot_to_simulated_client(
      frame,
      snapshots,
      viewport_states,
      copy_mode,
      status,
      wmux::daemon_internal::RenderFrameOptions{});

  assert(screen_contains(client, "Ask wmux>"));
  assert(screen_has_styled_blank(client, 4));
}

void tmux_reference_cursor_state_materializes_after_scene() {
  auto frame = single_pane_frame(20, 5, wmux::PaneLayoutRect{1, 0, 0, 20, 4});
  frame.status_bar_enabled = false;

  const std::string pty_bytes = "prompt> value\x1b[1;9H";
  std::unordered_map<wmux::PaneId, wmux::PtyOutputSnapshot> snapshots;
  snapshots.emplace(1, snapshot_from_pty_bytes(pty_bytes, 20, 4));
  wmux::daemon_internal::PaneViewportStates viewport_states;
  wmux::daemon_internal::CopyModeState copy_mode;
  wmux::daemon_internal::RenderStatus status;

  const auto client = render_snapshot_to_simulated_client(
      frame,
      snapshots,
      viewport_states,
      copy_mode,
      status,
      wmux::daemon_internal::RenderFrameOptions{});

  assert(screen_contains(client, "prompt> value"));
  assert(client.cursor_visible);
  assert(client.cursor_row == 0);
  assert(client.cursor_column == 8);
}

void tmux_reference_unicode_box_ui_survives_render_to_client() {
  auto frame = single_pane_frame(18, 5, wmux::PaneLayoutRect{1, 0, 0, 18, 4});
  frame.status_bar_enabled = false;

  const std::string pty_bytes =
      "\xE2\x95\xAD\xE2\x94\x80\xE2\x94\x80 wmux "
      "\xE2\x94\x80\xE2\x94\x80\xE2\x95\xAE\r\n"
      "\xE2\x94\x82 \xE2\x9C\x93 ready   \xE2\x94\x82\r\n"
      "\xE2\x95\xB0\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"
      "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x95\xAF";

  std::unordered_map<wmux::PaneId, wmux::PtyOutputSnapshot> snapshots;
  snapshots.emplace(1, snapshot_from_pty_bytes(pty_bytes, 18, 4));
  wmux::daemon_internal::PaneViewportStates viewport_states;
  wmux::daemon_internal::CopyModeState copy_mode;
  wmux::daemon_internal::RenderStatus status;

  const auto client = render_snapshot_to_simulated_client(
      frame,
      snapshots,
      viewport_states,
      copy_mode,
      status,
      wmux::daemon_internal::RenderFrameOptions{});

  expect_screen_contains(
      "unicode box top", client, "\xE2\x95\xAD\xE2\x94\x80\xE2\x94\x80 wmux");
  expect_screen_contains("unicode checkmark", client, "\xE2\x9C\x93");
  expect_screen_contains("unicode box content", client, "ready");
}

void tmux_reference_split_close_keeps_pane_content_authoritative() {
  wmux::daemon_internal::ActiveWindowFrame split_frame;
  split_frame.window_id = 1;
  split_frame.layout_generation = 1;
  split_frame.active_pane_id = 1;
  split_frame.session_name = "layout";
  split_frame.window_name = "main";
  split_frame.columns = 30;
  split_frame.rows = 6;
  split_frame.pane_rows = 5;
  split_frame.status_bar_enabled = false;
  split_frame.panes.push_back(wmux::daemon_internal::RenderPane{
      wmux::PaneLayoutRect{1, 0, 0, 15, 5}, true, {}});
  split_frame.panes.push_back(wmux::daemon_internal::RenderPane{
      wmux::PaneLayoutRect{2, 15, 0, 15, 5}, false, {}});

  std::unordered_map<wmux::PaneId, wmux::PtyOutputSnapshot> snapshots;
  snapshots.emplace(1, snapshot_from_pty_bytes("left pane ready", 15, 5));
  snapshots.emplace(2, snapshot_from_pty_bytes("right pane", 15, 5));
  wmux::daemon_internal::PaneViewportStates viewport_states;
  wmux::daemon_internal::CopyModeState copy_mode;
  wmux::daemon_internal::RenderStatus status;
  wmux::daemon_internal::RenderDiffState baseline;

  (void)wmux::daemon_internal::render_frame_update(
      split_frame,
      snapshots,
      viewport_states,
      copy_mode,
      status,
      wmux::daemon_internal::RenderFrameOptions{},
      baseline);
  assert(baseline.panes.contains(1));
  assert(baseline.panes.contains(2));

  auto closed_frame = split_frame;
  closed_frame.layout_generation = 2;
  closed_frame.panes.clear();
  closed_frame.panes.push_back(wmux::daemon_internal::RenderPane{
      wmux::PaneLayoutRect{1, 0, 0, 30, 5}, true, {}});
  snapshots.erase(2);

  wmux::daemon_internal::RenderFrameOptions layout_only;
  layout_only.clear_terminal = false;
  layout_only.draw_borders = true;
  layout_only.draw_status = true;
  layout_only.draw_pane_bodies = false;
  layout_only.preserve_layout_cache = true;
  layout_only.repaint_body_on_geometry_change = true;

  const auto client = render_snapshot_to_simulated_client(
      closed_frame,
      snapshots,
      viewport_states,
      copy_mode,
      status,
      layout_only,
      &baseline);

  assert(screen_contains(client, "left pane ready"));
  assert(baseline.panes.contains(1));
  assert(!baseline.panes.contains(2));
}

void tmux_reference_scene_delta_preserves_styled_input_box_on_client() {
  auto frame = single_pane_frame(24, 5, wmux::PaneLayoutRect{1, 0, 0, 24, 4});
  frame.status_bar_enabled = false;
  wmux::daemon_internal::PaneViewportStates viewport_states;
  wmux::daemon_internal::CopyModeState copy_mode;
  wmux::daemon_internal::RenderStatus status;
  wmux::daemon_internal::RenderDiffState baseline;

  std::unordered_map<wmux::PaneId, wmux::PtyOutputSnapshot> first_snapshots;
  first_snapshots.emplace(1, snapshot_from_pty_bytes("loading\r\n", 24, 4));
  const auto first_frame = render_snapshot_frame_bytes(
      frame,
      first_snapshots,
      viewport_states,
      copy_mode,
      status,
      wmux::daemon_internal::RenderFrameOptions{},
      baseline);

  const std::string styled_box =
      "\x1b[44m                        \x1b[0m\r\n"
      "\x1b[44m  \x1b[37mAsk wmux>\x1b[44m           \x1b[0m\x1b[2;12H";
  std::unordered_map<wmux::PaneId, wmux::PtyOutputSnapshot> second_snapshots;
  second_snapshots.emplace(1, snapshot_from_pty_bytes(styled_box, 24, 4));
  wmux::daemon_internal::RenderFrameOptions delta_options;
  delta_options.clear_terminal = false;
  const auto second_frame = render_snapshot_frame_bytes(
      frame,
      second_snapshots,
      viewport_states,
      copy_mode,
      status,
      delta_options,
      baseline);

  const auto client =
      simulated_client_after_frames({first_frame, second_frame}, frame.columns, frame.rows);
  assert(second_frame.find("\x1b[2J") == std::string::npos);
  assert(screen_contains(client, "Ask wmux>"));
  assert(screen_has_styled_blank(client, 4));
  assert(client.cursor_visible);
  assert(client.cursor_row == 1);
  assert(client.cursor_column == 11);
}

void tmux_reference_layout_delta_removes_stale_closed_pane_content() {
  wmux::daemon_internal::ActiveWindowFrame split_frame;
  split_frame.window_id = 1;
  split_frame.layout_generation = 1;
  split_frame.active_pane_id = 1;
  split_frame.session_name = "layout";
  split_frame.window_name = "main";
  split_frame.columns = 30;
  split_frame.rows = 6;
  split_frame.pane_rows = 5;
  split_frame.status_bar_enabled = false;
  split_frame.panes.push_back(wmux::daemon_internal::RenderPane{
      wmux::PaneLayoutRect{1, 0, 0, 15, 5}, true, {}});
  split_frame.panes.push_back(wmux::daemon_internal::RenderPane{
      wmux::PaneLayoutRect{2, 15, 0, 15, 5}, false, {}});

  std::unordered_map<wmux::PaneId, wmux::PtyOutputSnapshot> split_snapshots;
  split_snapshots.emplace(1, snapshot_from_pty_bytes("left pane ready", 15, 5));
  split_snapshots.emplace(2, snapshot_from_pty_bytes("right stale", 15, 5));
  wmux::daemon_internal::PaneViewportStates viewport_states;
  wmux::daemon_internal::CopyModeState copy_mode;
  wmux::daemon_internal::RenderStatus status;
  wmux::daemon_internal::RenderDiffState baseline;

  const auto split_bytes = render_snapshot_frame_bytes(
      split_frame,
      split_snapshots,
      viewport_states,
      copy_mode,
      status,
      wmux::daemon_internal::RenderFrameOptions{},
      baseline);

  auto closed_frame = split_frame;
  closed_frame.layout_generation = 2;
  closed_frame.panes.clear();
  closed_frame.panes.push_back(wmux::daemon_internal::RenderPane{
      wmux::PaneLayoutRect{1, 0, 0, 30, 5}, true, {}});
  std::unordered_map<wmux::PaneId, wmux::PtyOutputSnapshot> closed_snapshots;
  closed_snapshots.emplace(1, snapshot_from_pty_bytes("left pane ready full width", 30, 5));

  wmux::daemon_internal::RenderFrameOptions layout_options;
  layout_options.clear_terminal = false;
  layout_options.preserve_layout_cache = true;
  layout_options.repaint_body_on_geometry_change = true;
  const auto closed_bytes = render_snapshot_frame_bytes(
      closed_frame,
      closed_snapshots,
      viewport_states,
      copy_mode,
      status,
      layout_options,
      baseline);

  const auto client =
      simulated_client_after_frames({split_bytes, closed_bytes}, closed_frame.columns, closed_frame.rows);
  assert(screen_contains(client, "left pane ready full width"));
  assert(!screen_contains(client, "right stale"));
  assert(closed_bytes.find("\x1b[2J") == std::string::npos);
}

void tmux_reference_resize_full_scene_materializes_current_viewport() {
  auto first_frame_model = single_pane_frame(24, 5, wmux::PaneLayoutRect{1, 0, 0, 24, 4});
  first_frame_model.status_bar_enabled = false;
  auto resized_frame = single_pane_frame(32, 6, wmux::PaneLayoutRect{1, 0, 0, 32, 5});
  resized_frame.window_id = first_frame_model.window_id;
  resized_frame.layout_generation = first_frame_model.layout_generation + 1;
  resized_frame.status_bar_enabled = false;

  wmux::daemon_internal::PaneViewportStates viewport_states;
  wmux::daemon_internal::CopyModeState copy_mode;
  wmux::daemon_internal::RenderStatus status;
  wmux::daemon_internal::RenderDiffState baseline;

  std::unordered_map<wmux::PaneId, wmux::PtyOutputSnapshot> first_snapshots;
  first_snapshots.emplace(1, snapshot_from_pty_bytes("narrow prompt>", 24, 4));
  const auto first_bytes = render_snapshot_frame_bytes(
      first_frame_model,
      first_snapshots,
      viewport_states,
      copy_mode,
      status,
      wmux::daemon_internal::RenderFrameOptions{},
      baseline);

  std::unordered_map<wmux::PaneId, wmux::PtyOutputSnapshot> resized_snapshots;
  resized_snapshots.emplace(
      1,
      snapshot_from_pty_bytes(
          "\x1b[42m                                \x1b[0m\r\n"
          "wide prompt is ready\x1b[2;21H",
          32,
          5));
  wmux::daemon_internal::RenderFrameOptions resize_options;
  resize_options.clear_terminal = false;
  resize_options.preserve_layout_cache = true;
  resize_options.repaint_body_on_geometry_change = true;
  const auto resized_bytes = render_snapshot_frame_bytes(
      resized_frame,
      resized_snapshots,
      viewport_states,
      copy_mode,
      status,
      resize_options,
      baseline);

  const auto client =
      simulated_client_after_frames({first_bytes, resized_bytes}, resized_frame.columns, resized_frame.rows);
  assert(screen_contains(client, "wide prompt is ready"));
  assert(screen_has_styled_blank(client, 2));
  assert(client.cursor_visible);
  assert(client.cursor_row == 1);
  assert(client.cursor_column == 20);
}

void force_body_repaint_materializes_scene_even_when_baseline_matches() {
  auto frame = single_pane_frame(24, 5, wmux::PaneLayoutRect{1, 0, 0, 24, 4});
  frame.status_bar_enabled = false;

  const std::string styled_box =
      "\x1b[44m                        \x1b[0m\r\n"
      "\x1b[44m  \x1b[37mAsk wmux>\x1b[44m           \x1b[0m\x1b[2;12H";
  std::unordered_map<wmux::PaneId, wmux::PtyOutputSnapshot> snapshots;
  snapshots.emplace(1, snapshot_from_pty_bytes(styled_box, 24, 4));
  wmux::daemon_internal::PaneViewportStates viewport_states;
  wmux::daemon_internal::CopyModeState copy_mode;
  wmux::daemon_internal::RenderStatus status;
  wmux::daemon_internal::RenderDiffState daemon_baseline;

  // Simulate a coalesced/dropped frame: the daemon baseline was advanced to
  // the target scene, but the physical terminal did not receive that scene.
  (void)render_snapshot_frame_bytes(
      frame,
      snapshots,
      viewport_states,
      copy_mode,
      status,
      wmux::daemon_internal::RenderFrameOptions{},
      daemon_baseline);
  assert(daemon_baseline.scene_valid);

  wmux::daemon_internal::RenderFrameOptions recovery;
  recovery.clear_terminal = false;
  recovery.force_body_repaint = true;
  const auto recovery_bytes = render_snapshot_frame_bytes(
      frame,
      snapshots,
      viewport_states,
      copy_mode,
      status,
      recovery,
      daemon_baseline);

  const auto client = simulated_client_after_frame(recovery_bytes, frame.columns, frame.rows);
  assert(screen_contains(client, "Ask wmux>"));
  assert(screen_has_styled_blank(client, 4));
  assert(client.cursor_visible);
  assert(client.cursor_row == 1);
  assert(client.cursor_column == 11);
}

void tmux_reference_truecolor_prompt_box_preserves_styled_blanks() {
  auto frame = single_pane_frame(30, 5, wmux::PaneLayoutRect{1, 0, 0, 30, 4});
  frame.status_bar_enabled = false;

  const std::string prompt_box =
      "\x1b[48;2;24;28;38m                              \x1b[0m\r\n"
      "\x1b[48;2;24;28;38m  \x1b[38;2;230;235;245m\xe2\x80\xba ask agent\x1b[48;2;24;28;38m                 \x1b[0m\r\n"
      "\x1b[48;2;24;28;38m  gpt-5.5 high \xc2\xb7 ~/repo    \x1b[0m\x1b[2;15H";
  std::unordered_map<wmux::PaneId, wmux::PtyOutputSnapshot> snapshots;
  snapshots.emplace(1, snapshot_from_pty_bytes(prompt_box, 30, 4));
  wmux::daemon_internal::PaneViewportStates viewport_states;
  wmux::daemon_internal::CopyModeState copy_mode;
  wmux::daemon_internal::RenderStatus status;

  const auto client = render_snapshot_to_simulated_client(
      frame,
      snapshots,
      viewport_states,
      copy_mode,
      status,
      wmux::daemon_internal::RenderFrameOptions{});

  assert(screen_contains(client, "ask agent"));
  assert(screen_contains(client, "gpt-5.5 high"));
  assert(screen_has_styled_blank(client, 0x01000000 | (24 << 16) | (28 << 8) | 38));
  assert(client.cursor_visible);
  assert(client.cursor_row == 1);
  assert(client.cursor_column == 14);
}

void performance_guard_layout_only_is_smaller_than_full_repaint() {
  auto frame = single_pane_frame(80, 12, wmux::PaneLayoutRect{1, 0, 0, 80, 11});
  frame.status_bar_enabled = true;
  std::unordered_map<wmux::PaneId, wmux::PtyOutputSnapshot> snapshots;
  snapshots.emplace(
      1,
      snapshot_from_pty_bytes(
          "alpha beta gamma delta epsilon zeta eta theta\r\n"
          "second row with useful terminal content\r\n"
          "third row with useful terminal content\r\n"
          "fourth row with useful terminal content",
          80,
          11));
  wmux::daemon_internal::PaneViewportStates viewport_states;
  wmux::daemon_internal::CopyModeState copy_mode;
  wmux::daemon_internal::RenderStatus status;
  wmux::daemon_internal::RenderDiffState baseline;

  const auto full = wmux::daemon_internal::render_frame_update(
      frame,
      snapshots,
      viewport_states,
      copy_mode,
      status,
      wmux::daemon_internal::RenderFrameOptions{},
      baseline);

  auto layout_frame = frame;
  ++layout_frame.layout_generation;
  wmux::daemon_internal::RenderFrameOptions layout_only;
  layout_only.clear_terminal = false;
  layout_only.draw_borders = true;
  layout_only.draw_status = true;
  layout_only.draw_pane_bodies = false;
  layout_only.preserve_layout_cache = true;
  layout_only.repaint_body_on_geometry_change = false;

  const auto layout = wmux::daemon_internal::render_frame_update(
      layout_frame,
      snapshots,
      viewport_states,
      copy_mode,
      status,
      layout_only,
      baseline);

  assert(!full.empty());
  assert(layout.size() < full.size());
  assert(baseline.baseline_valid);
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
  renderer_materializes_styled_blank_cells();
  layout_only_skips_body_when_geometry_is_stable();
  layout_only_repaints_body_when_geometry_changes();
  layout_only_preserves_baseline_across_layout_generation_change();
  layout_only_resize_is_atomic_and_preserves_baseline();
  layout_only_removes_disappeared_pane_from_baseline();
  renders_active_pane_cursor_after_frame();
  renderer_applies_configured_ui_accent_and_tmux_style();
  shared_borders_close_nested_split_connector_gaps();
  entering_copy_mode_starts_at_live_pane_cursor();
  full_scene_materialization_creates_valid_client_baseline();
  visible_scene_materializes_all_core_surfaces();
  client_baseline_stores_visible_scene_after_successful_materialization();
  layout_only_commits_new_scene_without_forgetting_pane_identity();
  verification_harness_materializes_model_to_simulated_client();
  tmux_reference_attach_long_session_renders_latest_viewport();
  tmux_reference_alternate_screen_tui_renders_current_scene();
  tmux_reference_styled_prompt_input_box_preserves_styled_blanks();
  tmux_reference_cursor_state_materializes_after_scene();
  tmux_reference_unicode_box_ui_survives_render_to_client();
  tmux_reference_split_close_keeps_pane_content_authoritative();
  tmux_reference_scene_delta_preserves_styled_input_box_on_client();
  tmux_reference_layout_delta_removes_stale_closed_pane_content();
  tmux_reference_resize_full_scene_materializes_current_viewport();
  force_body_repaint_materializes_scene_even_when_baseline_matches();
  tmux_reference_truecolor_prompt_box_preserves_styled_blanks();
  performance_guard_layout_only_is_smaller_than_full_repaint();
}

void run_daemon_render_benchmark_tests() {
  using Clock = std::chrono::steady_clock;

  const auto report = [](std::string_view name, std::chrono::microseconds elapsed, std::size_t bytes) {
    std::cout << "daemon render benchmark: " << name << " "
              << elapsed.count() << " us, " << bytes << " bytes\n";
  };

  auto frame = single_pane_frame(100, 32, wmux::PaneLayoutRect{1, 0, 0, 100, 31});
  frame.session_name = "bench";
  frame.window_name = "main";

  std::string heavy_output;
  for (int index = 0; index < 3000; ++index) {
    heavy_output += "render benchmark row ";
    heavy_output += std::to_string(index);
    heavy_output += " with enough text to exercise clipping and row encoding\r\n";
  }
  heavy_output += "latest prompt>";

  std::unordered_map<wmux::PaneId, wmux::PtyOutputSnapshot> snapshots;
  snapshots.emplace(1, snapshot_from_pty_bytes(heavy_output, 100, 31));
  wmux::daemon_internal::PaneViewportStates viewport_states;
  wmux::daemon_internal::CopyModeState copy_mode;
  wmux::daemon_internal::RenderStatus status;
  wmux::daemon_internal::RenderDiffState baseline;

  auto start = Clock::now();
  const auto first_paint = wmux::daemon_internal::render_frame_update(
      frame,
      snapshots,
      viewport_states,
      copy_mode,
      status,
      wmux::daemon_internal::RenderFrameOptions{},
      baseline);
  report("first-paint", std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start), first_paint.size());
  assert(!first_paint.empty());
  assert(baseline.baseline_valid);

  auto resized_frame = frame;
  resized_frame.layout_generation += 1;
  resized_frame.columns = 120;
  resized_frame.rows = 36;
  resized_frame.pane_rows = 35;
  resized_frame.panes[0].rect = wmux::PaneLayoutRect{1, 0, 0, 120, 35};
  snapshots[1] = snapshot_from_pty_bytes(heavy_output, 120, 35);

  wmux::daemon_internal::RenderFrameOptions layout_only;
  layout_only.clear_terminal = false;
  layout_only.draw_borders = true;
  layout_only.draw_status = true;
  layout_only.draw_pane_bodies = false;
  layout_only.preserve_layout_cache = true;
  layout_only.repaint_body_on_geometry_change = true;

  start = Clock::now();
  const auto resize_frame = wmux::daemon_internal::render_frame_update(
      resized_frame,
      snapshots,
      viewport_states,
      copy_mode,
      status,
      layout_only,
      baseline);
  report("resize-layout", std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start), resize_frame.size());
  assert(!resize_frame.empty());

  wmux::daemon_internal::ActiveWindowFrame split_frame;
  split_frame.window_id = 2;
  split_frame.layout_generation = 1;
  split_frame.active_pane_id = 1;
  split_frame.session_name = "bench";
  split_frame.window_name = "split";
  split_frame.columns = 120;
  split_frame.rows = 36;
  split_frame.pane_rows = 35;
  split_frame.status_bar_enabled = true;
  split_frame.panes.push_back(wmux::daemon_internal::RenderPane{
      wmux::PaneLayoutRect{1, 0, 0, 60, 35}, true, {}});
  split_frame.panes.push_back(wmux::daemon_internal::RenderPane{
      wmux::PaneLayoutRect{2, 60, 0, 60, 35}, false, {}});
  snapshots[1] = snapshot_from_pty_bytes(heavy_output, 60, 35);
  snapshots.emplace(2, snapshot_from_pty_bytes("secondary pane ready", 60, 35));
  wmux::daemon_internal::RenderDiffState split_baseline;

  start = Clock::now();
  const auto split_first = wmux::daemon_internal::render_frame_update(
      split_frame,
      snapshots,
      viewport_states,
      copy_mode,
      status,
      wmux::daemon_internal::RenderFrameOptions{},
      split_baseline);
  report("split-first-paint", std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start), split_first.size());
  assert(!split_first.empty());

  auto closed_frame = split_frame;
  closed_frame.layout_generation += 1;
  closed_frame.panes.clear();
  closed_frame.panes.push_back(wmux::daemon_internal::RenderPane{
      wmux::PaneLayoutRect{1, 0, 0, 120, 35}, true, {}});
  snapshots.erase(2);
  snapshots[1] = snapshot_from_pty_bytes(heavy_output, 120, 35);

  start = Clock::now();
  const auto close_frame = wmux::daemon_internal::render_frame_update(
      closed_frame,
      snapshots,
      viewport_states,
      copy_mode,
      status,
      layout_only,
      split_baseline);
  report("split-close-layout", std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start), close_frame.size());
  assert(!close_frame.empty());
}
