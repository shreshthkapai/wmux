#include "daemon_render.hpp"

#include "wmux/copy_selection.hpp"
#include "wmux/unicode_width.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wmux::daemon_internal {
namespace {

constexpr std::string_view kClearTerminal = "\x1b[2J\x1b[H";

void append_reset(std::string& out) {
  out += "\x1b[0m";
}

UiColor effective_accent(const UiTheme& theme) {
  if (theme.tmux_style) {
    return UiColor{UiColorKind::Indexed, 2};
  }
  return theme.accent;
}

void append_ui_foreground(std::string& out, const UiTheme& theme) {
  out += "\x1b[";
  out += ui_color_foreground_sgr(effective_accent(theme));
  out += "m";
}

void append_ui_background(std::string& out, const UiTheme& theme) {
  out += theme.tmux_style ? "\x1b[30;" : "\x1b[37;";
  out += ui_color_background_sgr(effective_accent(theme));
  out += "m";
}

bool has_left_border(const PaneLayoutRect& rect) {
  return rect.left == 0;
}

bool has_top_border(const PaneLayoutRect& rect) {
  return rect.top == 0;
}

int body_left(const PaneLayoutRect& rect) {
  return rect.left + (has_left_border(rect) && rect.width > 1 ? 1 : 0);
}

int body_top(const PaneLayoutRect& rect) {
  return rect.top + (has_top_border(rect) && rect.height > 1 ? 1 : 0);
}

void append_cursor_move(std::string& out, int row, int column) {
  out += "\x1b[";
  out += std::to_string(row + 1);
  out += ";";
  out += std::to_string(column + 1);
  out += "H";
}

void append_cursor_visible(std::string& out, bool visible) {
  out += visible ? "\x1b[?25h" : "\x1b[?25l";
}

void append_clipped_text(std::string& out, std::string_view line, int width) {
  if (width <= 0) {
    return;
  }

  for (const auto& cell : terminal_text_cells_from_text(line, static_cast<std::size_t>(width))) {
    out.append(cell.text);
  }
}

struct BorderCell {
  unsigned char mask{0};
  bool active{false};
};

constexpr unsigned char kBorderUp = 1 << 0;
constexpr unsigned char kBorderDown = 1 << 1;
constexpr unsigned char kBorderLeft = 1 << 2;
constexpr unsigned char kBorderRight = 1 << 3;

std::string_view smooth_border_glyph(unsigned char mask) {
  switch (mask) {
    case kBorderLeft:
    case kBorderRight:
    case kBorderLeft | kBorderRight:
      return "\xE2\x94\x80";
    case kBorderUp:
    case kBorderDown:
    case kBorderUp | kBorderDown:
      return "\xE2\x94\x82";
    case kBorderDown | kBorderRight:
      return "\xE2\x94\x8C";
    case kBorderDown | kBorderLeft:
      return "\xE2\x94\x90";
    case kBorderUp | kBorderRight:
      return "\xE2\x94\x94";
    case kBorderUp | kBorderLeft:
      return "\xE2\x94\x98";
    case kBorderLeft | kBorderRight | kBorderDown:
      return "\xE2\x94\xAC";
    case kBorderLeft | kBorderRight | kBorderUp:
      return "\xE2\x94\xB4";
    case kBorderUp | kBorderDown | kBorderRight:
      return "\xE2\x94\x9C";
    case kBorderUp | kBorderDown | kBorderLeft:
      return "\xE2\x94\xA4";
    case kBorderUp | kBorderDown | kBorderLeft | kBorderRight:
      return "\xE2\x94\xBC";
    default:
      return "\xE2\x94\xBC";
  }
}

char ascii_border_glyph(unsigned char mask) {
  const bool horizontal = (mask & (kBorderLeft | kBorderRight)) != 0;
  const bool vertical = (mask & (kBorderUp | kBorderDown)) != 0;
  if (horizontal && vertical) {
    return '+';
  }
  return horizontal ? '-' : '|';
}

std::string border_glyph(unsigned char mask, const UiTheme& theme) {
  if (theme.smooth_borders) {
    return std::string{smooth_border_glyph(mask)};
  }
  return std::string(1, ascii_border_glyph(mask));
}

bool attributes_equal(const TerminalAttributes& left, const TerminalAttributes& right) {
  return left.bold == right.bold && left.dim == right.dim && left.italic == right.italic &&
         left.underline == right.underline &&
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
  return !attributes.bold && !attributes.dim && !attributes.italic && !attributes.underline &&
         !attributes.inverse &&
         attributes.foreground == -1 && attributes.background == -1;
}

void append_sgr_for_attributes(
    std::string& out,
    const TerminalAttributes& attributes,
    bool accent_overlay,
    const UiTheme& theme) {
  if (accent_overlay) {
    append_ui_background(out, theme);
    return;
  }

  if (default_attributes(attributes)) {
    append_reset(out);
    return;
  }

  out += "\x1b[0";
  if (attributes.bold) {
    out += ";1";
  }
  if (attributes.dim) {
    out += ";2";
  }
  if (attributes.italic) {
    out += ";3";
  }
  if (attributes.underline) {
    out += ";4";
  }
  if (attributes.inverse) {
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

bool diff_cells_equal(const RenderDiffCell& left, const RenderDiffCell& right) {
  return left.text == right.text && left.width == right.width &&
         left.accent_overlay == right.accent_overlay &&
         attributes_equal(left.attributes, right.attributes);
}

bool pane_rect_equal(const PaneLayoutRect& left, const PaneLayoutRect& right) {
  return left.pane_id == right.pane_id && left.left == right.left && left.top == right.top &&
         left.width == right.width && left.height == right.height;
}

std::vector<TerminalTextCell> line_text_cells_for_width(
    const TerminalLineSnapshot* line,
    int width) {
  if (width <= 0) {
    return {};
  }
  if (line == nullptr || line->cells.empty()) {
    return terminal_text_cells_from_text(
        line == nullptr ? std::string_view{} : std::string_view{line->text},
        static_cast<std::size_t>(width));
  }

  std::vector<TerminalTextCell> cells;
  cells.reserve(static_cast<std::size_t>(width));
  for (std::size_t index = 0;
       index < line->cells.size() && cells.size() < static_cast<std::size_t>(width);
       ++index) {
    TerminalCellWidth cell_width = TerminalCellWidth::Narrow;
    if (index < line->cell_widths.size()) {
      cell_width = line->cell_widths[index];
    } else if (line->cells[index].empty()) {
      cell_width = TerminalCellWidth::WideContinuation;
    }

    cells.push_back(TerminalTextCell{
        cell_width == TerminalCellWidth::WideContinuation ? std::string{} : line->cells[index],
        cell_width});
  }

  for (std::size_t index = 0; index + 1 < cells.size(); ++index) {
    if (cells[index].width == TerminalCellWidth::Narrow &&
        cells[index + 1].width == TerminalCellWidth::WideContinuation) {
      cells[index].width = TerminalCellWidth::WideLeading;
    }
  }

  while (cells.size() < static_cast<std::size_t>(width)) {
    cells.push_back(TerminalTextCell{" ", TerminalCellWidth::Narrow});
  }
  return cells;
}

int leading_column_for(const std::vector<TerminalTextCell>& cells, int column);

std::optional<std::pair<int, int>> expanded_selected_columns(
    const std::vector<TerminalTextCell>& cells,
    std::optional<std::pair<int, int>> selected);

std::vector<RenderDiffCell> render_cells_for_line(
    const TerminalLineSnapshot* line,
    int width,
    const CopyLineOverlay& overlay) {
  std::vector<RenderDiffCell> rendered;
  if (width <= 0) {
    return rendered;
  }

  const auto cells = line_text_cells_for_width(line, width);
  rendered.reserve(cells.size());

  std::optional<int> cursor;
  if (overlay.cursor_column) {
    cursor = leading_column_for(cells, *overlay.cursor_column);
  }

  std::optional<std::pair<int, int>> selected;
  if (overlay.selected_columns) {
    const int first = std::clamp(overlay.selected_columns->first, 0, width - 1);
    const int last = std::clamp(overlay.selected_columns->second, 0, width - 1);
    selected = expanded_selected_columns(cells, std::pair{std::min(first, last), std::max(first, last)});
  }

  for (int column = 0; column < width; ++column) {
    const bool selected_column =
        selected && column >= selected->first && column <= selected->second;
    const TerminalAttributes attributes =
        line != nullptr && column < static_cast<int>(line->attributes.size())
            ? line->attributes[static_cast<std::size_t>(column)]
            : TerminalAttributes{};
    rendered.push_back(RenderDiffCell{
        cells[static_cast<std::size_t>(column)].text,
        cells[static_cast<std::size_t>(column)].width,
        attributes,
        (cursor && column == *cursor) || selected_column});
  }

  return rendered;
}

void append_render_cells(
    std::string& out,
    const std::vector<RenderDiffCell>& cells,
    int first,
    int last_exclusive,
    const UiTheme& theme) {
  if (first < 0 || last_exclusive <= first ||
      first >= static_cast<int>(cells.size())) {
    return;
  }

  last_exclusive = std::min(last_exclusive, static_cast<int>(cells.size()));
  bool accent_overlay = false;
  TerminalAttributes active_attributes;
  bool style_active = false;
  const auto set_style = [&](const TerminalAttributes& attributes, bool enabled_accent) {
    if (style_active && attributes_equal(attributes, active_attributes) &&
        enabled_accent == accent_overlay) {
      return;
    }
    append_sgr_for_attributes(out, attributes, enabled_accent, theme);
    active_attributes = attributes;
    accent_overlay = enabled_accent;
    style_active = true;
  };

  for (int column = first; column < last_exclusive; ++column) {
    const auto& cell = cells[static_cast<std::size_t>(column)];
    set_style(cell.attributes, cell.accent_overlay);
    out.append(cell.text);
  }

  if (style_active) {
    append_reset(out);
  }
}

int leading_column_for(const std::vector<TerminalTextCell>& cells, int column) {
  if (cells.empty()) {
    return 0;
  }

  column = std::clamp(column, 0, static_cast<int>(cells.size() - 1));
  while (column > 0 &&
         cells[static_cast<std::size_t>(column)].width ==
             TerminalCellWidth::WideContinuation) {
    --column;
  }
  return column;
}

std::optional<std::pair<int, int>> expanded_selected_columns(
    const std::vector<TerminalTextCell>& cells,
    std::optional<std::pair<int, int>> selected) {
  if (!selected || cells.empty()) {
    return std::nullopt;
  }

  int first = leading_column_for(cells, selected->first);
  int last = std::clamp(selected->second, 0, static_cast<int>(cells.size() - 1));
  if (cells[static_cast<std::size_t>(last)].width == TerminalCellWidth::WideContinuation) {
    last = leading_column_for(cells, last);
  }
  if (cells[static_cast<std::size_t>(last)].width == TerminalCellWidth::WideLeading &&
      last + 1 < static_cast<int>(cells.size())) {
    ++last;
  }

  return std::pair{std::min(first, last), std::max(first, last)};
}

void append_clipped_text_with_overlay(
    std::string& out,
    const TerminalLineSnapshot* line,
    int width,
    const CopyLineOverlay& overlay,
    const UiTheme& theme) {
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

  const auto cells = line_text_cells_for_width(line, width);
  std::optional<int> cursor;
  if (overlay.cursor_column) {
    cursor = leading_column_for(cells, *overlay.cursor_column);
  }
  std::optional<std::pair<int, int>> selected;
  if (overlay.selected_columns) {
    const int first = std::clamp(overlay.selected_columns->first, 0, width - 1);
    const int last = std::clamp(overlay.selected_columns->second, 0, width - 1);
    selected = expanded_selected_columns(cells, std::pair{std::min(first, last), std::max(first, last)});
  }

  bool accent_overlay = false;
  TerminalAttributes active_attributes;
  bool style_active = false;
  const auto set_style = [&](const TerminalAttributes& attributes, bool enabled_accent) {
    if (style_active && attributes_equal(attributes, active_attributes) &&
        enabled_accent == accent_overlay) {
      return;
    }
    append_sgr_for_attributes(out, attributes, enabled_accent, theme);
    active_attributes = attributes;
    accent_overlay = enabled_accent;
    style_active = true;
  };

  for (int column = 0; column < width; ++column) {
    const auto& cell = cells[static_cast<std::size_t>(column)];
    const bool selected_column =
        selected && column >= selected->first && column <= selected->second;
    const TerminalAttributes attributes =
        line != nullptr && column < static_cast<int>(line->attributes.size())
            ? line->attributes[static_cast<std::size_t>(column)]
            : TerminalAttributes{};
    set_style(attributes, (cursor && column == *cursor) || selected_column);
    out.append(cell.text);
  }
  if (style_active) {
    append_reset(out);
  }
}

bool operator<(const CopyModePoint& left, const CopyModePoint& right) {
  if (left.line != right.line) {
    return left.line < right.line;
  }
  return left.column < right.column;
}

void append_pane_border(
    std::string& out,
    const PaneLayoutRect& rect,
    bool active,
    const UiTheme& theme) {
  if (rect.width <= 0 || rect.height <= 0) {
    return;
  }

  const std::string_view horizontal = theme.smooth_borders ? "─" : "-";
  const std::string_view vertical = theme.smooth_borders ? "│" : "|";
  const std::string_view top_left = theme.smooth_borders ? "┌" : "+";
  const std::string_view top_right = theme.smooth_borders ? "┐" : "+";
  const std::string_view bottom_left = theme.smooth_borders ? "└" : "+";
  const std::string_view bottom_right = theme.smooth_borders ? "┘" : "+";

  if (active) {
    append_ui_foreground(out, theme);
  } else {
    append_reset(out);
  }

  if (rect.height == 1) {
    append_cursor_move(out, rect.top, rect.left);
    for (int column = 0; column < rect.width; ++column) {
      out += horizontal;
    }
    append_reset(out);
    return;
  }

  if (rect.width == 1) {
    for (int row = 0; row < rect.height; ++row) {
      append_cursor_move(out, rect.top + row, rect.left);
      out += vertical;
    }
    append_reset(out);
    return;
  }

  append_cursor_move(out, rect.top, rect.left);
  out += top_left;
  for (int column = 0; column < std::max(0, rect.width - 2); ++column) {
    out += horizontal;
  }
  out += top_right;

  for (int row = 1; row < rect.height - 1; ++row) {
    append_cursor_move(out, rect.top + row, rect.left);
    out += vertical;
    if (rect.width > 2) {
      out.append(static_cast<std::size_t>(rect.width - 2), ' ');
    }
    out += vertical;
  }

  append_cursor_move(out, rect.top + rect.height - 1, rect.left);
  out += bottom_left;
  for (int column = 0; column < std::max(0, rect.width - 2); ++column) {
    out += horizontal;
  }
  out += bottom_right;
  append_reset(out);
}

void mark_border_cell(
    std::vector<BorderCell>& cells,
    int columns,
    int rows,
    int column,
    int row,
    unsigned char mask,
    bool active) {
  if (column < 0 || row < 0 || column >= columns || row >= rows) {
    return;
  }
  auto& cell = cells[static_cast<std::size_t>(row * columns + column)];
  cell.mask = static_cast<unsigned char>(cell.mask | mask);
  cell.active = cell.active || active;
}

void mark_horizontal_border(
    std::vector<BorderCell>& cells,
    int columns,
    int rows,
    int row,
    int left,
    int right,
    bool active) {
  if (row < 0 || row >= rows || right < left) {
    return;
  }
  left = std::clamp(left, 0, columns - 1);
  right = std::clamp(right, 0, columns - 1);
  for (int column = left; column <= right; ++column) {
    unsigned char mask = 0;
    if (column > left) {
      mask = static_cast<unsigned char>(mask | kBorderLeft);
    }
    if (column < right) {
      mask = static_cast<unsigned char>(mask | kBorderRight);
    }
    mark_border_cell(cells, columns, rows, column, row, mask, active);
  }
}

void mark_vertical_border(
    std::vector<BorderCell>& cells,
    int columns,
    int rows,
    int column,
    int top,
    int bottom,
    bool active) {
  if (column < 0 || column >= columns || bottom < top) {
    return;
  }
  top = std::clamp(top, 0, rows - 1);
  bottom = std::clamp(bottom, 0, rows - 1);
  for (int row = top; row <= bottom; ++row) {
    unsigned char mask = 0;
    if (row > top) {
      mask = static_cast<unsigned char>(mask | kBorderUp);
    }
    if (row < bottom) {
      mask = static_cast<unsigned char>(mask | kBorderDown);
    }
    mark_border_cell(cells, columns, rows, column, row, mask, active);
  }
}

void append_shared_pane_borders(
    std::string& out,
    const ActiveWindowFrame& frame,
    const UiTheme& theme) {
  if (frame.columns <= 0 || frame.rows <= 0) {
    return;
  }

  std::vector<BorderCell> cells(
      static_cast<std::size_t>(frame.columns * frame.rows));
  for (const auto& pane : frame.panes) {
    const auto& rect = pane.rect;
    if (rect.width <= 0 || rect.height <= 0) {
      continue;
    }

    const int left = rect.left;
    const int right = rect.left + rect.width - 1;
    const int top = rect.top;
    const int bottom = rect.top + rect.height - 1;
    if (has_top_border(rect)) {
      mark_horizontal_border(cells, frame.columns, frame.rows, top, left, right, pane.active);
    }
    mark_horizontal_border(cells, frame.columns, frame.rows, bottom, left, right, pane.active);
    if (has_left_border(rect)) {
      mark_vertical_border(cells, frame.columns, frame.rows, left, top, bottom, pane.active);
    }
    mark_vertical_border(cells, frame.columns, frame.rows, right, top, bottom, pane.active);
  }

  bool active_style = false;
  for (int row = 0; row < frame.rows; ++row) {
    for (int column = 0; column < frame.columns; ++column) {
      const auto& cell = cells[static_cast<std::size_t>(row * frame.columns + column)];
      if (cell.mask == 0) {
        continue;
      }
      if (cell.active && !active_style) {
        append_ui_foreground(out, theme);
        active_style = true;
      } else if (!cell.active && active_style) {
        append_reset(out);
        active_style = false;
      } else if (!cell.active) {
        append_reset(out);
      }
      append_cursor_move(out, row, column);
      out += border_glyph(cell.mask, theme);
    }
  }
  append_reset(out);
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

std::size_t normalize_copy_column_for_line(
    const PtyOutputSnapshot& snapshot,
    std::size_t line,
    int width,
    std::size_t column) {
  if (width <= 0) {
    return 0;
  }

  const auto cells = line_text_cells_for_width(
      snapshot_line_snapshot_at(snapshot, line),
      width);
  return static_cast<std::size_t>(
      leading_column_for(cells, static_cast<int>(std::min(column, static_cast<std::size_t>(width - 1)))));
}

std::size_t next_copy_column_for_line(
    const PtyOutputSnapshot& snapshot,
    std::size_t line,
    int width,
    std::size_t column) {
  if (width <= 0) {
    return 0;
  }

  const auto cells = line_text_cells_for_width(
      snapshot_line_snapshot_at(snapshot, line),
      width);
  const int current = leading_column_for(
      cells,
      static_cast<int>(std::min(column, static_cast<std::size_t>(width - 1))));
  const auto current_width = cells[static_cast<std::size_t>(current)].width;
  int next = current + (current_width == TerminalCellWidth::WideLeading ? 2 : 1);
  while (next < width &&
         cells[static_cast<std::size_t>(next)].width == TerminalCellWidth::WideContinuation) {
    ++next;
  }
  return next >= width ? static_cast<std::size_t>(current) : static_cast<std::size_t>(next);
}

std::size_t previous_copy_column_for_line(
    const PtyOutputSnapshot& snapshot,
    std::size_t line,
    int width,
    std::size_t column) {
  if (width <= 0) {
    return 0;
  }

  const auto cells = line_text_cells_for_width(
      snapshot_line_snapshot_at(snapshot, line),
      width);
  int current = leading_column_for(
      cells,
      static_cast<int>(std::min(column, static_cast<std::size_t>(width - 1))));
  if (current == 0) {
    return 0;
  }
  --current;
  return static_cast<std::size_t>(leading_column_for(cells, current));
}

std::size_t last_copy_column_for_line(
    const PtyOutputSnapshot& snapshot,
    std::size_t line,
    int width) {
  if (width <= 0) {
    return 0;
  }

  const auto cells = line_text_cells_for_width(
      snapshot_line_snapshot_at(snapshot, line),
      width);
  for (int column = width - 1; column >= 0; --column) {
    const auto leading = leading_column_for(cells, column);
    const auto& cell = cells[static_cast<std::size_t>(leading)];
    if (!cell.text.empty() && cell.text != " ") {
      return static_cast<std::size_t>(leading);
    }
  }
  return 0;
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

std::size_t screen_cursor_line_index(const PtyOutputSnapshot& snapshot) {
  const auto cursor_row = static_cast<std::size_t>(std::max(0, snapshot.screen.cursor_row));
  if (snapshot.screen.alternate_screen) {
    return cursor_row;
  }
  return std::max(snapshot.scrollback.line_snapshots.size(), snapshot.scrollback.lines.size()) +
         cursor_row;
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

std::string tmux_like_status_right(PaneId pane_id) {
  const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm local_time{};
#ifdef _WIN32
  localtime_s(&local_time, &now);
#else
  localtime_r(&now, &local_time);
#endif

  std::ostringstream right;
  right << "\"pane " << pane_id << "\" " << std::put_time(&local_time, "%H:%M %d-%b-%y");
  return right.str();
}

StatusLineMode effective_status_mode(
    const RenderStatus& status,
    const CopyModeState& copy_mode) {
  if (copy_mode.active) {
    return StatusLineMode::Copy;
  }
  if (status.mouse_drag_active) {
    return StatusLineMode::MouseDrag;
  }
  return status.mode;
}

StatusState render_status_state(
    const ActiveWindowFrame& frame,
    const CopyModeState& copy_mode,
    const RenderStatus& status,
    std::size_t active_viewport_offset) {
  StatusState state = status.state;
  const auto mode = effective_status_mode(status, copy_mode);

  std::ostringstream left;
  if (mode == StatusLineMode::Copy) {
    left << "[copy-mode] ";
  } else if (mode == StatusLineMode::Prefix) {
    left << "[prefix] ";
  } else if (mode == StatusLineMode::CommandPrompt) {
    left << "[command] ";
  }
  left << "[" << frame.session_name << "] " << frame.window_index << ":" << frame.window_name
       << "*";
  state.permanent_left.text = left.str();

  const auto pane_id = mode == StatusLineMode::Copy ? copy_mode.pane_id : frame.active_pane_id;
  std::ostringstream right;
  right << tmux_like_status_right(pane_id);
  if (mode == StatusLineMode::Copy) {
    right << " cursor " << copy_mode.cursor_row + 1 << "," << copy_mode.cursor_column + 1;
    if (copy_mode.selection_active) {
      right << " selecting";
    }
  }
  if (active_viewport_offset > 0) {
    right << " scroll:" << active_viewport_offset;
  }
  state.permanent_right.text = right.str();
  return state;
}

RenderStatus render_status_from_override(std::string_view status_override) {
  RenderStatus status;
  if (!status_override.empty()) {
    status_set_temporary(
        status.state,
        status_override,
        std::chrono::steady_clock::now(),
        kDefaultStatusMessageTtl);
  }
  return status;
}

RenderDiffRow build_body_row(
    const RenderPane& pane,
    const PtyOutputSnapshot& snapshot,
    int row,
    int width,
    std::size_t line_index,
    std::size_t total_lines,
    std::size_t viewport_offset,
    const CopyModeState& copy_mode) {
  std::optional<int> cursor_column;
  CopyModePoint cursor_point;
  std::optional<std::pair<int, int>> selected_columns;
  const int height = body_height(pane.rect);
  if (copy_mode.active && copy_mode.pane_id == pane.rect.pane_id &&
      row == static_cast<int>(copy_mode.cursor_row)) {
    cursor_column = static_cast<int>(copy_mode.cursor_column);
  }
  if (copy_mode.active && copy_mode.pane_id == pane.rect.pane_id) {
    cursor_point = copy_mode_cursor_point(copy_mode, snapshot, height, viewport_offset);
    selected_columns = selected_columns_for_line(copy_mode, cursor_point, line_index, width);
  }

  const auto* line = line_index < total_lines ? snapshot_line_snapshot_at(snapshot, line_index)
                                              : nullptr;
  return RenderDiffRow{
      body_top(pane.rect) + row,
      body_left(pane.rect),
      width,
      render_cells_for_line(line, width, CopyLineOverlay{cursor_column, selected_columns})};
}

RenderDiffPane build_pane_diff_state(
    const RenderPane& pane,
    const PtyOutputSnapshot& snapshot,
    const PaneViewportStates& viewport_states,
    const CopyModeState& copy_mode) {
  RenderDiffPane state;
  state.rect = pane.rect;

  const int width = body_width(pane.rect);
  const int height = body_height(pane.rect);
  if (width <= 0 || height <= 0) {
    return state;
  }

  const auto viewport = viewport_states.find(pane.rect.pane_id);
  const auto requested_offset =
      viewport == viewport_states.end() ? std::size_t{0} : viewport->second.offset;
  state.viewport_offset = clamped_viewport_offset(snapshot, height, requested_offset);
  state.first_visible_line = first_visible_line_index(snapshot, height, state.viewport_offset);
  const auto total_lines = snapshot_line_count(snapshot);

  state.body_rows.reserve(static_cast<std::size_t>(height));
  for (int row = 0; row < height; ++row) {
    state.body_rows.push_back(build_body_row(
        pane,
        snapshot,
        row,
        width,
        state.first_visible_line + static_cast<std::size_t>(row),
        total_lines,
        state.viewport_offset,
        copy_mode));
  }
  return state;
}

bool render_diff_state_compatible(
    const RenderDiffState& diff_state,
    const ActiveWindowFrame& frame) {
  if (!diff_state.initialized || diff_state.columns != frame.columns ||
      diff_state.rows != frame.rows ||
      diff_state.status_bar_enabled != frame.status_bar_enabled) {
    return false;
  }

  for (const auto& pane : frame.panes) {
    const auto cached = diff_state.panes.find(pane.rect.pane_id);
    if (cached != diff_state.panes.end() &&
        !pane_rect_equal(cached->second.rect, pane.rect)) {
      return false;
    }
  }
  return true;
}

void expand_changed_cells_for_wide_glyphs(
    const std::vector<RenderDiffCell>& cells,
    std::vector<bool>& changed) {
  for (std::size_t column = 0; column < cells.size() && column < changed.size(); ++column) {
    if (!changed[column]) {
      continue;
    }
    if (cells[column].width == TerminalCellWidth::WideLeading &&
        column + 1 < changed.size()) {
      changed[column + 1] = true;
    } else if (cells[column].width == TerminalCellWidth::WideContinuation && column > 0) {
      changed[column - 1] = true;
    }
  }
}

bool append_changed_row(
    std::string& out,
    const RenderDiffRow& next,
    const RenderDiffRow* previous,
    const UiTheme& theme) {
  if (next.width <= 0 || next.cells.empty()) {
    return false;
  }

  const bool row_changed_shape =
      previous == nullptr || previous->row != next.row || previous->column != next.column ||
      previous->width != next.width || previous->cells.size() != next.cells.size();
  std::vector<bool> changed(next.cells.size(), row_changed_shape);
  if (!row_changed_shape && previous != nullptr) {
    for (std::size_t column = 0; column < next.cells.size(); ++column) {
      changed[column] = !diff_cells_equal(next.cells[column], previous->cells[column]);
    }
  }

  expand_changed_cells_for_wide_glyphs(next.cells, changed);
  if (previous != nullptr) {
    expand_changed_cells_for_wide_glyphs(previous->cells, changed);
  }

  bool wrote = false;
  for (int column = 0; column < static_cast<int>(changed.size());) {
    if (!changed[static_cast<std::size_t>(column)]) {
      ++column;
      continue;
    }

    int start = column;
    while (start > 0 &&
           next.cells[static_cast<std::size_t>(start)].width ==
               TerminalCellWidth::WideContinuation) {
      --start;
    }

    int end = column + 1;
    while (end < static_cast<int>(changed.size()) &&
           changed[static_cast<std::size_t>(end)]) {
      ++end;
    }
    if (end < static_cast<int>(next.cells.size()) &&
        next.cells[static_cast<std::size_t>(end)].width ==
            TerminalCellWidth::WideContinuation) {
      ++end;
    }

    append_cursor_move(out, next.row, next.column + start);
    append_render_cells(out, next.cells, start, end, theme);
    wrote = true;
    column = end;
  }

  return wrote;
}

bool append_pane_body_diff(
    std::string& out,
    RenderDiffState& diff_state,
    const RenderPane& pane,
    const PtyOutputSnapshot& snapshot,
    const PaneViewportStates& viewport_states,
    const CopyModeState& copy_mode,
    const UiTheme& theme) {
  auto next = build_pane_diff_state(pane, snapshot, viewport_states, copy_mode);
  const auto previous = diff_state.panes.find(pane.rect.pane_id);
  bool wrote = false;

  for (std::size_t row = 0; row < next.body_rows.size(); ++row) {
    const RenderDiffRow* previous_row = nullptr;
    if (previous != diff_state.panes.end() && row < previous->second.body_rows.size()) {
      previous_row = &previous->second.body_rows[row];
    }
    wrote = append_changed_row(out, next.body_rows[row], previous_row, theme) || wrote;
  }

  diff_state.panes[pane.rect.pane_id] = std::move(next);
  return wrote;
}

std::string render_partial_body_diff(
    const ActiveWindowFrame& frame,
    const std::unordered_map<PaneId, PtyOutputSnapshot>& snapshots,
    const PaneViewportStates& viewport_states,
    const CopyModeState& copy_mode,
    const RenderFrameOptions& options,
    const UiTheme& theme,
    RenderDiffState& diff_state) {
  std::string out;
  for (const auto& pane : frame.panes) {
    if (!should_render_pane_body(options, pane.rect.pane_id)) {
      continue;
    }

    const auto snapshot = snapshots.find(pane.rect.pane_id);
    if (snapshot == snapshots.end()) {
      diff_state.panes.erase(pane.rect.pane_id);
      continue;
    }

    (void)append_pane_body_diff(
        out, diff_state, pane, snapshot->second, viewport_states, copy_mode, theme);
  }
  return out;
}

void sync_render_diff_state(
    RenderDiffState& diff_state,
    const ActiveWindowFrame& frame,
    const std::unordered_map<PaneId, PtyOutputSnapshot>& snapshots,
    const PaneViewportStates& viewport_states,
    const CopyModeState& copy_mode,
    const RenderStatus& status) {
  RenderDiffState next;
  next.columns = frame.columns;
  next.rows = frame.rows;
  next.status_bar_enabled = frame.status_bar_enabled;
  next.initialized = true;
  next.panes.reserve(frame.panes.size());

  std::size_t active_viewport_offset = 0;
  for (const auto& pane : frame.panes) {
    const auto snapshot = snapshots.find(pane.rect.pane_id);
    if (snapshot == snapshots.end()) {
      continue;
    }

    auto pane_state =
        build_pane_diff_state(pane, snapshot->second, viewport_states, copy_mode);
    if (pane.active) {
      active_viewport_offset = pane_state.viewport_offset;
    }
    next.panes.emplace(pane.rect.pane_id, std::move(pane_state));
  }

  const bool has_visible_temporary =
      status_has_visible_temporary(status.state, std::chrono::steady_clock::now());
  const bool show_status = frame.rows > 1 &&
                           (frame.status_bar_enabled || has_visible_temporary ||
                            copy_mode.active);
  if (show_status) {
    next.status_line = format_status_line(
        render_status_state(frame, copy_mode, status, active_viewport_offset),
        frame.columns);
  }

  diff_state = std::move(next);
}

}  // namespace

int body_width(const PaneLayoutRect& rect) {
  if (rect.width <= 0 || rect.height <= 0) {
    return 0;
  }
  const int left_border = has_left_border(rect) ? 1 : 0;
  const int right_border = rect.width > 1 ? 1 : 0;
  return std::max(0, rect.width - left_border - right_border);
}

int body_height(const PaneLayoutRect& rect) {
  if (rect.width <= 0 || rect.height <= 0) {
    return 0;
  }
  const int top_border = has_top_border(rect) ? 1 : 0;
  const int bottom_border = rect.height > 1 ? 1 : 0;
  return std::max(0, rect.height - top_border - bottom_border);
}

bool append_active_pane_cursor(
    std::string& out,
    const ActiveWindowFrame& frame,
    const std::unordered_map<PaneId, PtyOutputSnapshot>& snapshots,
    const PaneViewportStates& viewport_states,
    const CopyModeState& copy_mode) {
  if (copy_mode.active) {
    append_cursor_visible(out, false);
    return false;
  }

  const auto pane = std::ranges::find_if(frame.panes, [](const RenderPane& candidate) {
    return candidate.active;
  });
  if (pane == frame.panes.end()) {
    append_cursor_visible(out, false);
    return false;
  }

  const auto snapshot = snapshots.find(pane->rect.pane_id);
  if (snapshot == snapshots.end() || !snapshot->second.screen.cursor_visible) {
    append_cursor_visible(out, false);
    return false;
  }

  const int width = body_width(pane->rect);
  const int height = body_height(pane->rect);
  if (width <= 0 || height <= 0) {
    append_cursor_visible(out, false);
    return false;
  }

  const auto viewport = viewport_states.find(pane->rect.pane_id);
  const auto requested_offset =
      viewport == viewport_states.end() ? std::size_t{0} : viewport->second.offset;
  const auto viewport_offset = clamped_viewport_offset(snapshot->second, height, requested_offset);
  if (viewport_offset != 0) {
    append_cursor_visible(out, false);
    return false;
  }

  const int cursor_row = std::clamp(snapshot->second.screen.cursor_row, 0, height - 1);
  const int cursor_column = std::clamp(snapshot->second.screen.cursor_column, 0, width - 1);
  append_cursor_move(out, body_top(pane->rect) + cursor_row, body_left(pane->rect) + cursor_column);
  append_cursor_visible(out, true);
  return true;
}

std::string render_frame(
    const ActiveWindowFrame& frame,
    const std::unordered_map<PaneId, PtyOutputSnapshot>& snapshots,
    const PaneViewportStates& viewport_states,
    const CopyModeState& copy_mode,
    std::string_view status_override) {
  return render_frame(frame, snapshots, viewport_states, copy_mode, render_status_from_override(status_override));
}

std::string render_frame(
    const ActiveWindowFrame& frame,
    const std::unordered_map<PaneId, PtyOutputSnapshot>& snapshots,
    const PaneViewportStates& viewport_states,
    const CopyModeState& copy_mode,
    const RenderStatus& status) {
  return render_frame_update(
      frame,
      snapshots,
      viewport_states,
      copy_mode,
      status,
      RenderFrameOptions{});
}

std::string render_frame_update(
    const ActiveWindowFrame& frame,
    const std::unordered_map<PaneId, PtyOutputSnapshot>& snapshots,
    const PaneViewportStates& viewport_states,
    const CopyModeState& copy_mode,
    std::string_view status_override,
    const RenderFrameOptions& options) {
  return render_frame_update(
      frame,
      snapshots,
      viewport_states,
      copy_mode,
      render_status_from_override(status_override),
      options);
}

std::string render_frame_update_impl(
    const ActiveWindowFrame& frame,
    const std::unordered_map<PaneId, PtyOutputSnapshot>& snapshots,
    const PaneViewportStates& viewport_states,
    const CopyModeState& copy_mode,
    const RenderStatus& status,
    const RenderFrameOptions& options,
    RenderDiffState* diff_state) {
  const bool can_use_diff =
      diff_state != nullptr && !options.clear_terminal && !options.draw_borders &&
      !options.draw_status && !options.dirty_panes.empty() &&
      render_diff_state_compatible(*diff_state, frame);
  if (can_use_diff) {
    auto out = render_partial_body_diff(
        frame, snapshots, viewport_states, copy_mode, options, status.ui, *diff_state);
    append_active_pane_cursor(out, frame, snapshots, viewport_states, copy_mode);
    return out;
  }

  std::string out;
  if (options.clear_terminal) {
    out += kClearTerminal;
  }
  std::size_t active_viewport_offset = 0;

  if (options.draw_borders) {
    append_shared_pane_borders(out, frame, status.ui);
  }

  for (const auto& pane : frame.panes) {
    const bool render_body = should_render_pane_body(options, pane.rect.pane_id);

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
            CopyLineOverlay{cursor_column, selected_columns},
            status.ui);
      } else {
        append_clipped_text_with_overlay(
            out, {}, width, CopyLineOverlay{cursor_column, selected_columns}, status.ui);
      }
    }
  }

  const bool has_visible_temporary =
      status_has_visible_temporary(status.state, std::chrono::steady_clock::now());
  const bool show_status = frame.rows > 1 &&
                           options.draw_status &&
                           (frame.status_bar_enabled || has_visible_temporary ||
                            copy_mode.active);
  if (show_status) {
    const auto status_model =
        render_status_state(frame, copy_mode, status, active_viewport_offset);
    const auto status_line = format_status_line(status_model, frame.columns);

    append_cursor_move(out, frame.rows - 1, 0);
    append_ui_background(out, status.ui);
    append_clipped_text(out, status_line, frame.columns);
    append_reset(out);
  }

  append_active_pane_cursor(out, frame, snapshots, viewport_states, copy_mode);

  const bool rendered_complete_frame =
      options.clear_terminal && options.draw_borders && options.draw_status &&
      options.dirty_panes.empty();
  if (diff_state != nullptr && rendered_complete_frame) {
    sync_render_diff_state(*diff_state, frame, snapshots, viewport_states, copy_mode, status);
  } else if (diff_state != nullptr) {
    reset_render_diff_state(*diff_state);
  }

  return out;
}

std::string render_frame_update(
    const ActiveWindowFrame& frame,
    const std::unordered_map<PaneId, PtyOutputSnapshot>& snapshots,
    const PaneViewportStates& viewport_states,
    const CopyModeState& copy_mode,
    const RenderStatus& status,
    const RenderFrameOptions& options) {
  return render_frame_update_impl(
      frame, snapshots, viewport_states, copy_mode, status, options, nullptr);
}

std::string render_frame_update(
    const ActiveWindowFrame& frame,
    const std::unordered_map<PaneId, PtyOutputSnapshot>& snapshots,
    const PaneViewportStates& viewport_states,
    const CopyModeState& copy_mode,
    const RenderStatus& status,
    const RenderFrameOptions& options,
    RenderDiffState& diff_state) {
  return render_frame_update_impl(
      frame, snapshots, viewport_states, copy_mode, status, options, &diff_state);
}

void reset_render_diff_state(RenderDiffState& diff_state) {
  diff_state = {};
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
    copy_mode.selection_active = false;
    const auto snapshot = snapshots.find(copy_mode.pane_id);
    const auto visible_lines =
        snapshot == snapshots.end() ? std::size_t{1}
                                    : visible_copy_line_count(snapshot->second, height);
    copy_mode.cursor_row =
        width > 0 && height > 0 ? std::min(visible_lines - 1, static_cast<std::size_t>(height - 1))
                                : std::size_t{0};
    copy_mode.cursor_column = 0;

    if (snapshot != snapshots.end() && width > 0 && height > 0 && visible_lines > 0) {
      const auto viewport = viewport_states.find(copy_mode.pane_id);
      const auto viewport_offset =
          viewport == viewport_states.end()
              ? std::size_t{0}
              : clamped_viewport_offset(snapshot->second, height, viewport->second.offset);
      const auto first_visible =
          first_visible_line_index(snapshot->second, height, viewport_offset);
      const auto cursor_line = screen_cursor_line_index(snapshot->second);
      if (cursor_line >= first_visible && cursor_line < first_visible + visible_lines) {
        copy_mode.cursor_row = cursor_line - first_visible;
      }
      copy_mode.cursor_column = normalize_copy_column_for_line(
          snapshot->second,
          first_visible + copy_mode.cursor_row,
          width,
          static_cast<std::size_t>(std::max(0, snapshot->second.screen.cursor_column)));
    }
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
  std::size_t viewport_offset = 0;
  if (snapshot != snapshots.end()) {
    const auto viewport = viewport_states.find(copy_mode.pane_id);
    viewport_offset =
        viewport == viewport_states.end()
            ? std::size_t{0}
            : clamped_viewport_offset(snapshot->second, height, viewport->second.offset);
  }

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
      if (snapshot != snapshots.end() && width > 0 && height > 0) {
        const auto cursor =
            copy_mode_cursor_point(copy_mode, snapshot->second, height, viewport_offset);
        copy_mode.cursor_column = previous_copy_column_for_line(
            snapshot->second,
            cursor.line,
            width,
            copy_mode.cursor_column);
      } else if (copy_mode.cursor_column > 0) {
        --copy_mode.cursor_column;
      }
      break;
    case AttachCopyModeAction::CursorRight:
      if (snapshot != snapshots.end() && width > 0 && height > 0) {
        const auto cursor =
            copy_mode_cursor_point(copy_mode, snapshot->second, height, viewport_offset);
        copy_mode.cursor_column = next_copy_column_for_line(
            snapshot->second,
            cursor.line,
            width,
            copy_mode.cursor_column);
      } else if (width > 0) {
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
    case AttachCopyModeAction::Home:
      copy_mode.cursor_column = 0;
      break;
    case AttachCopyModeAction::End:
      if (snapshot != snapshots.end() && width > 0 && height > 0) {
        const auto cursor =
            copy_mode_cursor_point(copy_mode, snapshot->second, height, viewport_offset);
        copy_mode.cursor_column = last_copy_column_for_line(snapshot->second, cursor.line, width);
      } else if (width > 0) {
        copy_mode.cursor_column = static_cast<std::size_t>(width - 1);
      }
      break;
    case AttachCopyModeAction::StartSelection:
      copy_mode.selection_active = true;
      if (snapshot == snapshots.end()) {
        copy_mode.selection_anchor = CopyModePoint{copy_mode.cursor_row, copy_mode.cursor_column};
      } else {
        const auto viewport = viewport_states.find(copy_mode.pane_id);
        const auto selection_viewport_offset =
            viewport == viewport_states.end()
                ? std::size_t{0}
                : clamped_viewport_offset(snapshot->second, height, viewport->second.offset);
        copy_mode.selection_anchor =
            copy_mode_cursor_point(copy_mode, snapshot->second, height, selection_viewport_offset);
      }
      break;
    case AttachCopyModeAction::CopySelection:
      if (!copy_mode.selection_active || snapshot == snapshots.end() || width <= 0 || height <= 0) {
        return false;
      }
      {
        const auto viewport = viewport_states.find(copy_mode.pane_id);
        const auto copy_viewport_offset =
            viewport == viewport_states.end()
                ? std::size_t{0}
                : clamped_viewport_offset(snapshot->second, height, viewport->second.offset);
        const auto cursor =
            copy_mode_cursor_point(copy_mode, snapshot->second, height, copy_viewport_offset);
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
  if (copy_mode.active && snapshot != snapshots.end() && width > 0 && height > 0) {
    const auto cursor =
        copy_mode_cursor_point(copy_mode, snapshot->second, height, viewport_offset);
    copy_mode.cursor_column = normalize_copy_column_for_line(
        snapshot->second,
        cursor.line,
        width,
        copy_mode.cursor_column);
  }
  return true;
}

}  // namespace wmux::daemon_internal
