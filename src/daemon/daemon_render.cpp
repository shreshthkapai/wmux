#include "daemon_render.hpp"

#include "wmux/copy_selection.hpp"

#include <algorithm>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace wmux::daemon_internal {
namespace {

constexpr std::string_view kClearTerminal = "\x1b[2J\x1b[H";

int body_left(const PaneLayoutRect& rect) {
  return rect.width >= 3 && rect.height >= 3 ? rect.left + 1 : rect.left;
}

int body_top(const PaneLayoutRect& rect) {
  return rect.width >= 3 && rect.height >= 3 ? rect.top + 1 : rect.top;
}

void append_cursor_move(std::string& out, int row, int column) {
  out += "\x1b[";
  out += std::to_string(row + 1);
  out += ";";
  out += std::to_string(column + 1);
  out += "H";
}

void append_clipped_text(std::string& out, std::string_view line, int width) {
  if (width <= 0) {
    return;
  }

  const auto count = static_cast<std::size_t>(std::min<int>(width, static_cast<int>(line.size())));
  out.append(line.substr(0, count));
  if (static_cast<int>(count) < width) {
    out.append(static_cast<std::size_t>(width) - count, ' ');
  }
}

bool attributes_equal(const TerminalAttributes& left, const TerminalAttributes& right) {
  return left.bold == right.bold && left.underline == right.underline &&
         left.inverse == right.inverse && left.foreground == right.foreground &&
         left.background == right.background;
}

void append_color_sgr(std::string& out, int base, std::int32_t color) {
  if (color < 0) {
    out += std::to_string(base + 9);
    return;
  }

  if (color >= 0 && color <= 7) {
    out += std::to_string(base + color);
    return;
  }

  if (color >= 8 && color <= 15) {
    out += std::to_string(base + 60 + color - 8);
    return;
  }

  if ((color & 0x01000000) != 0) {
    out += base == 30 ? "38;2;" : "48;2;";
    out += std::to_string((color >> 16) & 0xFF);
    out += ";";
    out += std::to_string((color >> 8) & 0xFF);
    out += ";";
    out += std::to_string(color & 0xFF);
    return;
  }

  out += base == 30 ? "38;5;" : "48;5;";
  out += std::to_string(std::clamp<std::int32_t>(color, 0, 255));
}

bool default_attributes(const TerminalAttributes& attributes) {
  return !attributes.bold && !attributes.underline && !attributes.inverse &&
         attributes.foreground == -1 && attributes.background == -1;
}

void append_sgr_for_attributes(
    std::string& out,
    const TerminalAttributes& attributes,
    bool overlay_inverse) {
  if (default_attributes(attributes) && !overlay_inverse) {
    out += "\x1b[0m";
    return;
  }

  out += "\x1b[0";
  if (attributes.bold) {
    out += ";1";
  }
  if (attributes.underline) {
    out += ";4";
  }
  if (attributes.inverse || overlay_inverse) {
    out += ";7";
  }
  if (attributes.foreground != -1) {
    out += ";";
    append_color_sgr(out, 30, attributes.foreground);
  }
  if (attributes.background != -1) {
    out += ";";
    append_color_sgr(out, 40, attributes.background);
  }
  out += "m";
}

struct CopyLineOverlay {
  std::optional<int> cursor_column;
  std::optional<std::pair<int, int>> selected_columns;
};

void append_clipped_text_with_overlay(
    std::string& out,
    const TerminalLineSnapshot* line,
    int width,
    const CopyLineOverlay& overlay) {
  const std::string_view text = line == nullptr ? std::string_view{} : line->text;
  if (!overlay.cursor_column && !overlay.selected_columns) {
    if (line == nullptr || (line->attributes.empty() && line->cells.empty())) {
      append_clipped_text(out, text, width);
      return;
    }
  }

  if (width <= 0) {
    return;
  }

  std::optional<int> cursor;
  if (overlay.cursor_column) {
    cursor = std::clamp(*overlay.cursor_column, 0, width - 1);
  }
  std::optional<std::pair<int, int>> selected;
  if (overlay.selected_columns) {
    const int first = std::clamp(overlay.selected_columns->first, 0, width - 1);
    const int last = std::clamp(overlay.selected_columns->second, 0, width - 1);
    selected = std::pair{std::min(first, last), std::max(first, last)};
  }

  bool inverse = false;
  TerminalAttributes active_attributes;
  bool style_active = false;
  const auto set_style = [&](const TerminalAttributes& attributes, bool enabled_inverse) {
    if (style_active && attributes_equal(attributes, active_attributes) &&
        enabled_inverse == inverse) {
      return;
    }
    append_sgr_for_attributes(out, attributes, enabled_inverse);
    active_attributes = attributes;
    inverse = enabled_inverse;
    style_active = true;
  };

  for (int column = 0; column < width; ++column) {
    std::string_view cell_text{" "};
    if (line != nullptr && column < static_cast<int>(line->cells.size())) {
      cell_text = line->cells[static_cast<std::size_t>(column)];
    } else if (column < static_cast<int>(text.size())) {
      cell_text = std::string_view{text}.substr(static_cast<std::size_t>(column), 1);
    }
    const bool selected_column =
        selected && column >= selected->first && column <= selected->second;
    const TerminalAttributes attributes =
        line != nullptr && column < static_cast<int>(line->attributes.size())
            ? line->attributes[static_cast<std::size_t>(column)]
            : TerminalAttributes{};
    set_style(attributes, (cursor && column == *cursor) || selected_column);
    out.append(cell_text);
  }
  if (style_active) {
    out += "\x1b[0m";
  }
}

bool operator<(const CopyModePoint& left, const CopyModePoint& right) {
  if (left.line != right.line) {
    return left.line < right.line;
  }
  return left.column < right.column;
}

void append_pane_border(std::string& out, const PaneLayoutRect& rect, bool active) {
  if (rect.width <= 0 || rect.height <= 0) {
    return;
  }

  const char horizontal = active ? '=' : '-';
  const char vertical = active ? '#' : '|';

  if (rect.height == 1) {
    append_cursor_move(out, rect.top, rect.left);
    out.append(static_cast<std::size_t>(rect.width), horizontal);
    return;
  }

  if (rect.width == 1) {
    for (int row = 0; row < rect.height; ++row) {
      append_cursor_move(out, rect.top + row, rect.left);
      out.push_back(vertical);
    }
    return;
  }

  append_cursor_move(out, rect.top, rect.left);
  out.push_back('+');
  out.append(static_cast<std::size_t>(std::max(0, rect.width - 2)), horizontal);
  out.push_back('+');

  for (int row = 1; row < rect.height - 1; ++row) {
    append_cursor_move(out, rect.top + row, rect.left);
    out.push_back(vertical);
    if (rect.width > 2) {
      out.append(static_cast<std::size_t>(rect.width - 2), ' ');
    }
    out.push_back(vertical);
  }

  append_cursor_move(out, rect.top + rect.height - 1, rect.left);
  out.push_back('+');
  out.append(static_cast<std::size_t>(std::max(0, rect.width - 2)), horizontal);
  out.push_back('+');
}

std::size_t snapshot_line_count(const PtyOutputSnapshot& snapshot) {
  if (snapshot.screen.alternate_screen) {
    return std::max(snapshot.screen.line_snapshots.size(), snapshot.screen.lines.size());
  }

  return std::max(snapshot.scrollback.line_snapshots.size(), snapshot.scrollback.lines.size()) +
         std::max(snapshot.screen.line_snapshots.size(), snapshot.screen.lines.size());
}

std::size_t max_viewport_offset(const PtyOutputSnapshot& snapshot, int height) {
  if (height <= 0) {
    return 0;
  }

  const auto total = snapshot_line_count(snapshot);
  const auto visible = static_cast<std::size_t>(height);
  return total > visible ? total - visible : 0;
}

std::size_t first_visible_line_index(
    const PtyOutputSnapshot& snapshot,
    int height,
    std::size_t viewport_offset) {
  if (height <= 0) {
    return 0;
  }

  const auto total_lines = snapshot_line_count(snapshot);
  const auto visible_lines = std::min(total_lines, static_cast<std::size_t>(height));
  const auto max_offset = total_lines - visible_lines;
  const auto clamped_offset = std::min(viewport_offset, max_offset);
  return total_lines - visible_lines - clamped_offset;
}

std::size_t clamped_viewport_offset(
    const PtyOutputSnapshot& snapshot,
    int height,
    std::size_t viewport_offset) {
  return std::min(viewport_offset, max_viewport_offset(snapshot, height));
}

const TerminalLineSnapshot* snapshot_line_snapshot_at(
    const PtyOutputSnapshot& snapshot,
    std::size_t index) {
  if (snapshot.screen.alternate_screen) {
    if (index < snapshot.screen.line_snapshots.size()) {
      return &snapshot.screen.line_snapshots[index];
    }
    return nullptr;
  }

  if (index < snapshot.scrollback.line_snapshots.size()) {
    return &snapshot.scrollback.line_snapshots[index];
  }

  const auto screen_index = index - snapshot.scrollback.line_snapshots.size();
  if (screen_index < snapshot.screen.line_snapshots.size()) {
    return &snapshot.screen.line_snapshots[screen_index];
  }

  return nullptr;
}

CopyModePoint copy_mode_cursor_point(
    const CopyModeState& copy_mode,
    const PtyOutputSnapshot& snapshot,
    int height,
    std::size_t viewport_offset) {
  return CopyModePoint{
      first_visible_line_index(snapshot, height, viewport_offset) + copy_mode.cursor_row,
      copy_mode.cursor_column};
}

std::optional<std::pair<int, int>> selected_columns_for_line(
    const CopyModeState& copy_mode,
    const CopyModePoint& cursor,
    std::size_t line,
    int width) {
  if (!copy_mode.selection_active || width <= 0) {
    return std::nullopt;
  }

  auto first = copy_mode.selection_anchor;
  auto last = cursor;
  if (last < first) {
    std::swap(first, last);
  }

  if (line < first.line || line > last.line) {
    return std::nullopt;
  }

  if (first.line == last.line) {
    return std::pair{
        static_cast<int>(std::min(first.column, static_cast<std::size_t>(width - 1))),
        static_cast<int>(std::min(last.column, static_cast<std::size_t>(width - 1)))};
  }

  if (line == first.line) {
    return std::pair{
        static_cast<int>(std::min(first.column, static_cast<std::size_t>(width - 1))),
        width - 1};
  }

  if (line == last.line) {
    return std::pair{
        0,
        static_cast<int>(std::min(last.column, static_cast<std::size_t>(width - 1)))};
  }

  return std::pair{0, width - 1};
}

const RenderPane* active_render_pane(const ActiveWindowFrame& frame) {
  const auto pane = std::ranges::find_if(frame.panes, [&](const auto& candidate) {
    return candidate.rect.pane_id == frame.active_pane_id;
  });
  return pane == frame.panes.end() ? nullptr : &*pane;
}

std::size_t visible_copy_line_count(const PtyOutputSnapshot& snapshot, int height) {
  if (height <= 0) {
    return 0;
  }

  const auto total_lines = snapshot_line_count(snapshot);
  if (total_lines == 0) {
    return 1;
  }

  return std::min<std::size_t>(total_lines, static_cast<std::size_t>(height));
}

bool should_render_pane_body(const RenderFrameOptions& options, PaneId pane_id) {
  return options.dirty_panes.empty() || options.dirty_panes.contains(pane_id);
}

}  // namespace

int body_width(const PaneLayoutRect& rect) {
  return rect.width >= 3 && rect.height >= 3 ? rect.width - 2 : 0;
}

int body_height(const PaneLayoutRect& rect) {
  return rect.width >= 3 && rect.height >= 3 ? rect.height - 2 : 0;
}

std::string render_frame(
    const ActiveWindowFrame& frame,
    const std::unordered_map<PaneId, PtyOutputSnapshot>& snapshots,
    const PaneViewportStates& viewport_states,
    const CopyModeState& copy_mode,
    std::string_view status_override) {
  return render_frame_update(
      frame,
      snapshots,
      viewport_states,
      copy_mode,
      status_override,
      RenderFrameOptions{});
}

std::string render_frame_update(
    const ActiveWindowFrame& frame,
    const std::unordered_map<PaneId, PtyOutputSnapshot>& snapshots,
    const PaneViewportStates& viewport_states,
    const CopyModeState& copy_mode,
    std::string_view status_override,
    const RenderFrameOptions& options) {
  std::string out;
  if (options.clear_terminal) {
    out += kClearTerminal;
  }
  std::size_t active_viewport_offset = 0;

  for (const auto& pane : frame.panes) {
    const bool render_body = should_render_pane_body(options, pane.rect.pane_id);
    if (options.draw_borders) {
      append_pane_border(out, pane.rect, pane.active);
    }

    const int left = body_left(pane.rect);
    const int top = body_top(pane.rect);
    const int width = body_width(pane.rect);
    const int height = body_height(pane.rect);
    const auto snapshot = snapshots.find(pane.rect.pane_id);
    if (snapshot == snapshots.end() || width <= 0 || height <= 0) {
      continue;
    }

    const auto total_lines = snapshot_line_count(snapshot->second);
    const auto viewport = viewport_states.find(pane.rect.pane_id);
    const auto requested_offset =
        viewport == viewport_states.end() ? std::size_t{0} : viewport->second.offset;
    const auto viewport_offset = clamped_viewport_offset(snapshot->second, height, requested_offset);
    if (pane.active) {
      active_viewport_offset = viewport_offset;
    }

    if (!render_body) {
      continue;
    }

    const auto first_row =
        first_visible_line_index(snapshot->second, height, viewport_offset);
    for (int row = 0; row < height; ++row) {
      append_cursor_move(out, top + row, left);
      out += "\x1b[0m";
      const auto line_index = first_row + static_cast<std::size_t>(row);
      std::optional<int> cursor_column;
      CopyModePoint cursor_point;
      std::optional<std::pair<int, int>> selected_columns;
      if (copy_mode.active && copy_mode.pane_id == pane.rect.pane_id &&
          row == static_cast<int>(copy_mode.cursor_row)) {
        cursor_column = static_cast<int>(copy_mode.cursor_column);
      }
      if (copy_mode.active && copy_mode.pane_id == pane.rect.pane_id) {
        cursor_point = copy_mode_cursor_point(copy_mode, snapshot->second, height, viewport_offset);
        selected_columns =
            selected_columns_for_line(copy_mode, cursor_point, line_index, width);
      }

      if (line_index < total_lines) {
        append_clipped_text_with_overlay(
            out,
            snapshot_line_snapshot_at(snapshot->second, line_index),
            width,
            CopyLineOverlay{cursor_column, selected_columns});
      } else {
        append_clipped_text_with_overlay(
            out, {}, width, CopyLineOverlay{cursor_column, selected_columns});
      }
    }
  }

  const bool show_status = frame.rows > 1 &&
                           options.draw_status &&
                           (frame.status_bar_enabled || !status_override.empty() ||
                            copy_mode.active);
  if (show_status) {
    std::ostringstream status;
    if (status_override.empty()) {
      if (copy_mode.active) {
        status << " copy-mode [" << frame.session_name << "] window " << frame.window_name
               << " pane " << copy_mode.pane_id << " "
               << "cursor " << copy_mode.cursor_row + 1 << ","
               << copy_mode.cursor_column + 1 << " ";
        if (copy_mode.selection_active) {
          status << "selecting ";
        }
      } else {
        status << " wmux [" << frame.session_name << "] window " << frame.window_name
               << " pane " << frame.active_pane_id << " ";
      }
      if (active_viewport_offset > 0) {
        status << "scroll " << active_viewport_offset << " ";
      }
    } else {
      status << status_override;
    }
    std::string status_line = status.str();
    if (static_cast<int>(status_line.size()) > frame.columns) {
      status_line.resize(static_cast<std::size_t>(frame.columns));
    }
    if (static_cast<int>(status_line.size()) < frame.columns) {
      status_line.append(static_cast<std::size_t>(frame.columns) - status_line.size(), ' ');
    }

    append_cursor_move(out, frame.rows - 1, 0);
    out += "\x1b[7m";
    out += status_line;
    out += "\x1b[0m";
  }

  return out;
}

void update_viewport_states(
    const ActiveWindowFrame& frame,
    const std::unordered_map<PaneId, PtyOutputSnapshot>& snapshots,
    PaneViewportStates& viewport_states,
    const CopyModeState& copy_mode) {
  for (auto it = viewport_states.begin(); it != viewport_states.end();) {
    const auto pane = std::ranges::find_if(frame.panes, [&](const auto& candidate) {
      return candidate.rect.pane_id == it->first;
    });
    if (pane == frame.panes.end()) {
      it = viewport_states.erase(it);
    } else {
      ++it;
    }
  }

  for (const auto& pane : frame.panes) {
    const auto snapshot = snapshots.find(pane.rect.pane_id);
    if (snapshot == snapshots.end()) {
      continue;
    }

    const auto total_lines = snapshot_line_count(snapshot->second);
    auto& viewport = viewport_states[pane.rect.pane_id];
    const bool frozen = copy_mode.active && copy_mode.pane_id == pane.rect.pane_id;
    if ((frozen || viewport.offset > 0) && viewport.observed_line_count > 0 &&
        total_lines > viewport.observed_line_count) {
      viewport.offset += total_lines - viewport.observed_line_count;
    }

    viewport.observed_line_count = total_lines;
    viewport.offset =
        std::min(viewport.offset, max_viewport_offset(snapshot->second, body_height(pane.rect)));
  }
}

bool apply_viewport_scroll(
    PaneId pane_id,
    const ActiveWindowFrame& frame,
    const std::unordered_map<PaneId, PtyOutputSnapshot>& snapshots,
    PaneViewportStates& viewport_states,
    AttachScrollAction action) {
  const auto pane = std::ranges::find_if(frame.panes, [&](const auto& candidate) {
    return candidate.rect.pane_id == pane_id;
  });
  if (pane == frame.panes.end()) {
    return false;
  }

  const auto snapshot = snapshots.find(pane_id);
  if (snapshot == snapshots.end()) {
    return false;
  }

  const int height = body_height(pane->rect);
  const auto max_offset = max_viewport_offset(snapshot->second, height);
  auto& viewport = viewport_states[pane_id];

  const auto page_delta = static_cast<std::size_t>(std::max(1, height - 1));
  switch (action) {
    case AttachScrollAction::LineUp:
      viewport.offset = std::min(max_offset, viewport.offset + 1);
      break;
    case AttachScrollAction::LineDown:
      viewport.offset = viewport.offset > 0 ? viewport.offset - 1 : 0;
      break;
    case AttachScrollAction::PageUp:
      viewport.offset = std::min(max_offset, viewport.offset + page_delta);
      break;
    case AttachScrollAction::PageDown:
      viewport.offset = viewport.offset > page_delta ? viewport.offset - page_delta : 0;
      break;
    case AttachScrollAction::Bottom:
      viewport.offset = 0;
      break;
  }

  return true;
}

bool apply_active_viewport_scroll(
    const ActiveWindowFrame& frame,
    const std::unordered_map<PaneId, PtyOutputSnapshot>& snapshots,
    PaneViewportStates& viewport_states,
    AttachScrollAction action) {
  return apply_viewport_scroll(
      frame.active_pane_id, frame, snapshots, viewport_states, action);
}

void clamp_copy_mode_cursor(
    CopyModeState& copy_mode,
    const ActiveWindowFrame& frame,
    const std::unordered_map<PaneId, PtyOutputSnapshot>& snapshots) {
  if (!copy_mode.active) {
    return;
  }

  const auto pane = std::ranges::find_if(frame.panes, [&](const auto& candidate) {
    return candidate.rect.pane_id == copy_mode.pane_id;
  });
  if (pane == frame.panes.end()) {
    copy_mode.active = false;
    return;
  }

  const int width = body_width(pane->rect);
  const int height = body_height(pane->rect);
  if (width <= 0 || height <= 0) {
    copy_mode.cursor_row = 0;
    copy_mode.cursor_column = 0;
    copy_mode.selection_active = false;
    return;
  }

  const auto snapshot = snapshots.find(copy_mode.pane_id);
  const auto visible_lines =
      snapshot == snapshots.end() ? std::size_t{1}
                                  : visible_copy_line_count(snapshot->second, height);
  copy_mode.cursor_row = std::min(copy_mode.cursor_row, visible_lines - 1);
  copy_mode.cursor_column =
      std::min(copy_mode.cursor_column, static_cast<std::size_t>(width - 1));
  if (copy_mode.selection_active) {
    const auto total_lines =
        snapshot == snapshots.end()
            ? std::size_t{1}
            : std::max<std::size_t>(1, snapshot_line_count(snapshot->second));
    copy_mode.selection_anchor.line = std::min(copy_mode.selection_anchor.line, total_lines - 1);
    copy_mode.selection_anchor.column =
        std::min(copy_mode.selection_anchor.column, static_cast<std::size_t>(width - 1));
  }
}

bool apply_copy_mode_action(
    const ActiveWindowFrame& frame,
    const std::unordered_map<PaneId, PtyOutputSnapshot>& snapshots,
    PaneViewportStates& viewport_states,
    CopyModeState& copy_mode,
    AttachCopyModeAction action,
    std::string& copied_text) {
  if (action == AttachCopyModeAction::Enter) {
    const auto pane = active_render_pane(frame);
    if (pane == nullptr) {
      return false;
    }

    const int width = body_width(pane->rect);
    const int height = body_height(pane->rect);
    copy_mode.active = true;
    copy_mode.pane_id = frame.active_pane_id;
    copy_mode.cursor_column = 0;
    copy_mode.selection_active = false;
    const auto snapshot = snapshots.find(copy_mode.pane_id);
    const auto visible_lines =
        snapshot == snapshots.end() ? std::size_t{1}
                                    : visible_copy_line_count(snapshot->second, height);
    copy_mode.cursor_row =
        width > 0 && height > 0 ? std::min(visible_lines - 1, static_cast<std::size_t>(height - 1))
                                : std::size_t{0};
    return true;
  }

  if (!copy_mode.active) {
    return true;
  }

  const auto pane = std::ranges::find_if(frame.panes, [&](const auto& candidate) {
    return candidate.rect.pane_id == copy_mode.pane_id;
  });
  if (pane == frame.panes.end()) {
    copy_mode.active = false;
    return false;
  }

  const int height = body_height(pane->rect);
  const int width = body_width(pane->rect);
  const auto snapshot = snapshots.find(copy_mode.pane_id);
  const auto visible_lines =
      snapshot == snapshots.end() ? std::size_t{1}
                                  : visible_copy_line_count(snapshot->second, height);

  switch (action) {
    case AttachCopyModeAction::Enter:
      break;
    case AttachCopyModeAction::Exit:
      viewport_states[copy_mode.pane_id].offset = 0;
      copy_mode = {};
      return true;
    case AttachCopyModeAction::CursorUp:
      if (copy_mode.cursor_row > 0) {
        --copy_mode.cursor_row;
      } else {
        (void)apply_viewport_scroll(
            copy_mode.pane_id, frame, snapshots, viewport_states, AttachScrollAction::LineUp);
      }
      break;
    case AttachCopyModeAction::CursorDown:
      if (copy_mode.cursor_row + 1 < visible_lines) {
        ++copy_mode.cursor_row;
      } else {
        (void)apply_viewport_scroll(
            copy_mode.pane_id, frame, snapshots, viewport_states, AttachScrollAction::LineDown);
      }
      break;
    case AttachCopyModeAction::CursorLeft:
      if (copy_mode.cursor_column > 0) {
        --copy_mode.cursor_column;
      }
      break;
    case AttachCopyModeAction::CursorRight:
      if (width > 0) {
        copy_mode.cursor_column =
            std::min(copy_mode.cursor_column + 1, static_cast<std::size_t>(width - 1));
      }
      break;
    case AttachCopyModeAction::PageUp:
      (void)apply_viewport_scroll(
          copy_mode.pane_id, frame, snapshots, viewport_states, AttachScrollAction::PageUp);
      break;
    case AttachCopyModeAction::PageDown:
      (void)apply_viewport_scroll(
          copy_mode.pane_id, frame, snapshots, viewport_states, AttachScrollAction::PageDown);
      break;
    case AttachCopyModeAction::StartSelection:
      copy_mode.selection_active = true;
      if (snapshot == snapshots.end()) {
        copy_mode.selection_anchor = CopyModePoint{copy_mode.cursor_row, copy_mode.cursor_column};
      } else {
        const auto viewport = viewport_states.find(copy_mode.pane_id);
        const auto viewport_offset =
            viewport == viewport_states.end()
                ? std::size_t{0}
                : clamped_viewport_offset(snapshot->second, height, viewport->second.offset);
        copy_mode.selection_anchor =
            copy_mode_cursor_point(copy_mode, snapshot->second, height, viewport_offset);
      }
      break;
    case AttachCopyModeAction::CopySelection:
      if (!copy_mode.selection_active || snapshot == snapshots.end() || width <= 0 || height <= 0) {
        return false;
      }
      {
        const auto viewport = viewport_states.find(copy_mode.pane_id);
        const auto viewport_offset =
            viewport == viewport_states.end()
                ? std::size_t{0}
                : clamped_viewport_offset(snapshot->second, height, viewport->second.offset);
        const auto cursor =
            copy_mode_cursor_point(copy_mode, snapshot->second, height, viewport_offset);
        copied_text = extract_copy_selection_text(
            snapshot->second,
            CopySelectionRange{
                CopySelectionPoint{
                    copy_mode.selection_anchor.line,
                    copy_mode.selection_anchor.column},
                CopySelectionPoint{cursor.line, cursor.column}},
            static_cast<std::size_t>(width));
      }
      viewport_states[copy_mode.pane_id].offset = 0;
      copy_mode = {};
      return true;
  }

  clamp_copy_mode_cursor(copy_mode, frame, snapshots);
  return true;
}

}  // namespace wmux::daemon_internal
