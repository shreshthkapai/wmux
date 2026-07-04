#include "daemon_render.hpp"

#include "wmux/copy_selection.hpp"
#include "wmux/platform/pty_process.hpp"
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
  (void)rect;
  return false;
}

bool has_top_border(const PaneLayoutRect& rect) {
  (void)rect;
  return false;
}

int body_left(const PaneLayoutRect& rect) {
  return rect.left;
}

int body_top(const PaneLayoutRect& rect) {
  return rect.top;
}

void append_cursor_move(std::string& out, int row, int column) {
  out += "\x1b[";
  out += std::to_string(row + 1);
  out += ";";
  out += std::to_string(column + 1);
  out += "H";
}

void append_synchronized_output_begin(std::string& out) {
  out += "\x1b[?2026h";
}

void append_synchronized_output_end(std::string& out) {
  out += "\x1b[?2026l";
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

int pane_area_rows(const ActiveWindowFrame& frame) {
  if (frame.pane_rows > 0) {
    return frame.pane_rows;
  }
  return std::max(1, frame.rows - (frame.status_bar_enabled && frame.rows > 1 ? 1 : 0));
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

bool scene_spans_equal(const std::vector<SceneSpan>& left, const std::vector<SceneSpan>& right) {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t span_index = 0; span_index < left.size(); ++span_index) {
    const auto& left_span = left[span_index];
    const auto& right_span = right[span_index];
    if (left_span.column != right_span.column ||
        left_span.cells.size() != right_span.cells.size()) {
      return false;
    }
    for (std::size_t cell_index = 0; cell_index < left_span.cells.size(); ++cell_index) {
      if (!diff_cells_equal(left_span.cells[cell_index], right_span.cells[cell_index])) {
        return false;
      }
    }
  }
  return true;
}

bool scene_rows_equal(const SceneRow& left, const SceneRow& right) {
  return left.kind == right.kind &&
         left.pane_id == right.pane_id &&
         left.row == right.row &&
         left.column == right.column &&
         left.width == right.width &&
         scene_spans_equal(left.spans, right.spans);
}

std::uint64_t ui_theme_generation(const UiTheme& theme) {
  std::uint64_t hash = 1469598103934665603ull;
  const auto mix = [&](std::uint64_t value) {
    hash ^= value;
    hash *= 1099511628211ull;
  };
  mix(theme.inherit_terminal_theme ? 1u : 0u);
  mix(theme.tmux_style ? 1u : 0u);
  mix(theme.smooth_borders ? 1u : 0u);
  mix(static_cast<std::uint64_t>(theme.accent.kind));
  mix(theme.accent.index);
  mix(theme.accent.red);
  mix(theme.accent.green);
  mix(theme.accent.blue);
  for (const unsigned char ch : theme.accent_spec) {
    mix(ch);
  }
  return hash;
}

bool scene_cursor_equal(const SceneCursor& left, const SceneCursor& right) {
  return left.known == right.known &&
         left.visible == right.visible &&
         left.pane_id == right.pane_id &&
         left.row == right.row &&
         left.column == right.column &&
         left.style == right.style;
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

bool border_cell_has_mask(
    const std::vector<BorderCell>& cells,
    int columns,
    int rows,
    int column,
    int row,
    unsigned char mask) {
  if (column < 0 || row < 0 || column >= columns || row >= rows) {
    return false;
  }
  const auto& cell = cells[static_cast<std::size_t>(row * columns + column)];
  return (cell.mask & mask) != 0;
}

bool border_cell_has_horizontal_axis(
    const std::vector<BorderCell>& cells,
    int columns,
    int rows,
    int column,
    int row) {
  if (column < 0 || row < 0 || column >= columns || row >= rows) {
    return false;
  }
  const auto& cell = cells[static_cast<std::size_t>(row * columns + column)];
  return (cell.mask & (kBorderLeft | kBorderRight)) != 0;
}

bool border_cell_has_vertical_axis(
    const std::vector<BorderCell>& cells,
    int columns,
    int rows,
    int column,
    int row) {
  if (column < 0 || row < 0 || column >= columns || row >= rows) {
    return false;
  }
  const auto& cell = cells[static_cast<std::size_t>(row * columns + column)];
  return (cell.mask & (kBorderUp | kBorderDown)) != 0;
}

void close_shared_border_connector_gaps(
    std::vector<BorderCell>& cells,
    int columns,
    int rows) {
  auto closed = cells;
  for (int row = 0; row < rows; ++row) {
    for (int column = 0; column < columns; ++column) {
      auto& cell = closed[static_cast<std::size_t>(row * columns + column)];
      if (cell.mask == 0) {
        continue;
      }

      if ((cell.mask & kBorderLeft) == 0 && (
              border_cell_has_mask(cells, columns, rows, column - 1, row, kBorderRight) ||
              ((cell.mask & (kBorderLeft | kBorderRight)) != 0 &&
               border_cell_has_vertical_axis(cells, columns, rows, column - 1, row)) ||
              ((cell.mask & (kBorderUp | kBorderDown)) != 0 &&
               border_cell_has_horizontal_axis(cells, columns, rows, column - 1, row)))) {
        cell.mask = static_cast<unsigned char>(cell.mask | kBorderLeft);
      }
      if ((cell.mask & kBorderRight) == 0 && (
              border_cell_has_mask(cells, columns, rows, column + 1, row, kBorderLeft) ||
              ((cell.mask & (kBorderLeft | kBorderRight)) != 0 &&
               border_cell_has_vertical_axis(cells, columns, rows, column + 1, row)) ||
              ((cell.mask & (kBorderUp | kBorderDown)) != 0 &&
               border_cell_has_horizontal_axis(cells, columns, rows, column + 1, row)))) {
        cell.mask = static_cast<unsigned char>(cell.mask | kBorderRight);
      }
      if ((cell.mask & kBorderUp) == 0 && (
              border_cell_has_mask(cells, columns, rows, column, row - 1, kBorderDown) ||
              ((cell.mask & (kBorderUp | kBorderDown)) != 0 &&
               border_cell_has_horizontal_axis(cells, columns, rows, column, row - 1)) ||
              ((cell.mask & (kBorderLeft | kBorderRight)) != 0 &&
               border_cell_has_vertical_axis(cells, columns, rows, column, row - 1)))) {
        cell.mask = static_cast<unsigned char>(cell.mask | kBorderUp);
      }
      if ((cell.mask & kBorderDown) == 0 && (
              border_cell_has_mask(cells, columns, rows, column, row + 1, kBorderUp) ||
              ((cell.mask & (kBorderUp | kBorderDown)) != 0 &&
               border_cell_has_horizontal_axis(cells, columns, rows, column, row + 1)) ||
              ((cell.mask & (kBorderLeft | kBorderRight)) != 0 &&
               border_cell_has_vertical_axis(cells, columns, rows, column, row + 1)))) {
        cell.mask = static_cast<unsigned char>(cell.mask | kBorderDown);
      }
    }
  }
  cells = std::move(closed);
}

std::vector<BorderCell> build_shared_border_cells(const ActiveWindowFrame& frame) {
  if (frame.columns <= 0 || frame.rows <= 0) {
    return {};
  }

  std::vector<BorderCell> cells(
      static_cast<std::size_t>(frame.columns * frame.rows));
  const int pane_rows = pane_area_rows(frame);
  const auto active_pane_touches_right = [&](const PaneLayoutRect& rect) {
    const int right = rect.left + rect.width - 1;
    const int top = rect.top;
    const int bottom = rect.top + rect.height - 1;
    return std::ranges::any_of(frame.panes, [&](const RenderPane& candidate) {
      if (!candidate.active) {
        return false;
      }
      const auto& other = candidate.rect;
      const int other_top = other.top;
      const int other_bottom = other.top + other.height - 1;
      return other.left == right + 1 && std::max(top, other_top) <= std::min(bottom, other_bottom);
    });
  };
  const auto active_pane_touches_below = [&](const PaneLayoutRect& rect) {
    const int left = rect.left;
    const int right = rect.left + rect.width - 1;
    const int bottom = rect.top + rect.height - 1;
    return std::ranges::any_of(frame.panes, [&](const RenderPane& candidate) {
      if (!candidate.active) {
        return false;
      }
      const auto& other = candidate.rect;
      const int other_left = other.left;
      const int other_right = other.left + other.width - 1;
      return other.top == bottom + 1 && std::max(left, other_left) <= std::min(right, other_right);
    });
  };
  for (const auto& pane : frame.panes) {
    const auto& rect = pane.rect;
    if (rect.width <= 0 || rect.height <= 0) {
      continue;
    }

    const int left = rect.left;
    const int right = rect.left + rect.width - 1;
    const int top = rect.top;
    const int bottom = rect.top + rect.height - 1;
    if (bottom < pane_rows - 1) {
      mark_horizontal_border(
          cells,
          frame.columns,
          frame.rows,
          bottom,
          left,
          right,
          pane.active || active_pane_touches_below(rect));
    }
    if (right < frame.columns - 1) {
      mark_vertical_border(
          cells,
          frame.columns,
          frame.rows,
          right,
          top,
          bottom,
          pane.active || active_pane_touches_right(rect));
    }
  }
  close_shared_border_connector_gaps(cells, frame.columns, frame.rows);
  return cells;
}

void append_shared_pane_borders(
    std::string& out,
    const ActiveWindowFrame& frame,
    const UiTheme& theme) {
  const auto cells = build_shared_border_cells(frame);
  if (cells.empty()) {
    return;
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
  const auto screen_line_count = [&] {
    const auto full_count =
        std::max(snapshot.screen.line_snapshots.size(), snapshot.screen.lines.size());
    return full_count == 0 ? static_cast<std::size_t>(std::max(0, snapshot.screen.rows))
                           : full_count;
  };
  if (snapshot.screen.alternate_screen) {
    return screen_line_count();
  }

  const auto scrollback_count = std::max(
      snapshot.scrollback.total_lines,
      std::max(snapshot.scrollback.line_snapshots.size(), snapshot.scrollback.lines.size()));
  return scrollback_count +
         screen_line_count();
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
  const auto dirty_screen_line_at = [&](std::size_t screen_index) -> const TerminalLineSnapshot* {
    for (std::size_t dirty_index = 0; dirty_index < snapshot.screen.dirty_rows.size();
         ++dirty_index) {
      if (static_cast<std::size_t>(std::max(0, snapshot.screen.dirty_rows[dirty_index])) !=
          screen_index) {
        continue;
      }
      if (dirty_index < snapshot.screen.dirty_line_snapshots.size()) {
        return &snapshot.screen.dirty_line_snapshots[dirty_index];
      }
      return nullptr;
    }
    return nullptr;
  };

  if (snapshot.screen.alternate_screen) {
    if (index < snapshot.screen.line_snapshots.size()) {
      return &snapshot.screen.line_snapshots[index];
    }
    return dirty_screen_line_at(index);
  }

  const auto full_screen_count =
      std::max(snapshot.screen.line_snapshots.size(), snapshot.screen.lines.size());
  if (snapshot.scrollback.line_snapshots.empty() && snapshot.scrollback.lines.empty() &&
      full_screen_count == 0) {
    return dirty_screen_line_at(index);
  }

  const auto scrollback_count = std::max(
      snapshot.scrollback.total_lines,
      std::max(snapshot.scrollback.line_snapshots.size(), snapshot.scrollback.lines.size()));
  if (index < scrollback_count) {
    if (index < snapshot.scrollback.first_line_index) {
      return nullptr;
    }
    const auto local_index = index - snapshot.scrollback.first_line_index;
    if (local_index < snapshot.scrollback.line_snapshots.size()) {
      return &snapshot.scrollback.line_snapshots[local_index];
    }
    if (local_index < snapshot.scrollback.lines.size()) {
      return nullptr;
    }
    return nullptr;
  }

  const auto screen_offset = scrollback_count;
  const auto screen_index = index - screen_offset;
  if (screen_index < snapshot.screen.line_snapshots.size()) {
    return &snapshot.screen.line_snapshots[screen_index];
  }
  if (screen_index < snapshot.screen.lines.size()) {
    return nullptr;
  }

  return dirty_screen_line_at(screen_index);
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
  return std::max(
             snapshot.scrollback.total_lines,
             std::max(snapshot.scrollback.line_snapshots.size(), snapshot.scrollback.lines.size())) +
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
  return options.draw_pane_bodies &&
         (options.dirty_panes.empty() || options.dirty_panes.contains(pane_id));
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
    int height,
    std::size_t line_index,
    std::size_t total_lines,
    std::size_t viewport_offset,
    const CopyModeState& copy_mode) {
  std::optional<int> cursor_column;
  CopyModePoint cursor_point;
  std::optional<std::pair<int, int>> selected_columns;
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

SceneRow scene_row_from_cells(
    SceneRowKind kind,
    PaneId pane_id,
    int row,
    int column,
    int width,
    std::vector<RenderDiffCell> cells) {
  SceneRow scene_row;
  scene_row.kind = kind;
  scene_row.pane_id = pane_id;
  scene_row.row = row;
  scene_row.column = column;
  scene_row.width = width;
  scene_row.spans.push_back(SceneSpan{column, std::move(cells)});
  return scene_row;
}

SceneRow scene_row_from_diff_row(SceneRowKind kind, PaneId pane_id, RenderDiffRow row) {
  return scene_row_from_cells(
      kind,
      pane_id,
      row.row,
      row.column,
      row.width,
      std::move(row.cells));
}

std::vector<RenderDiffCell> default_blank_cells(int width) {
  std::vector<RenderDiffCell> cells;
  if (width <= 0) {
    return cells;
  }
  cells.reserve(static_cast<std::size_t>(width));
  for (int column = 0; column < width; ++column) {
    cells.push_back(RenderDiffCell{
        " ",
        TerminalCellWidth::Narrow,
        TerminalAttributes{},
        false});
  }
  return cells;
}

std::vector<RenderDiffCell> status_cells_for_text(
    std::string_view text,
    int width) {
  std::vector<RenderDiffCell> cells;
  if (width <= 0) {
    return cells;
  }
  cells.reserve(static_cast<std::size_t>(width));
  for (const auto& cell : terminal_text_cells_from_text(text, static_cast<std::size_t>(width))) {
    cells.push_back(RenderDiffCell{
        cell.text,
        cell.width,
        TerminalAttributes{},
        true});
  }
  return cells;
}

std::vector<RenderDiffCell> border_cells_for_scene(
    const std::vector<BorderCell>& border_cells,
    const ActiveWindowFrame& frame,
    const UiTheme& theme,
    int row,
    int start_column,
    int end_column) {
  std::vector<RenderDiffCell> cells;
  if (row < 0 || row >= frame.rows || start_column < 0 || end_column <= start_column) {
    return cells;
  }
  cells.reserve(static_cast<std::size_t>(end_column - start_column));
  for (int column = start_column; column < end_column; ++column) {
    const auto& border =
        border_cells[static_cast<std::size_t>(row * frame.columns + column)];
    cells.push_back(RenderDiffCell{
        border.mask == 0 ? std::string{" "} : border_glyph(border.mask, theme),
        TerminalCellWidth::Narrow,
        TerminalAttributes{},
        border.active});
  }
  return cells;
}

std::vector<SceneRow> build_border_scene_rows(
    const ActiveWindowFrame& frame,
    const UiTheme& theme) {
  (void)theme;
  std::vector<SceneRow> rows;
  const auto border_cells = build_shared_border_cells(frame);
  if (border_cells.empty()) {
    return rows;
  }

  for (int row = 0; row < frame.rows; ++row) {
    int column = 0;
    while (column < frame.columns) {
      while (column < frame.columns &&
             border_cells[static_cast<std::size_t>(row * frame.columns + column)].mask == 0) {
        ++column;
      }
      if (column >= frame.columns) {
        break;
      }
      const int start = column;
      while (column < frame.columns &&
             border_cells[static_cast<std::size_t>(row * frame.columns + column)].mask != 0) {
        ++column;
      }
      rows.push_back(scene_row_from_cells(
          SceneRowKind::Border,
          0,
          row,
          start,
          column - start,
          border_cells_for_scene(border_cells, frame, theme, row, start, column)));
    }
  }
  return rows;
}

void mark_occupied(
    std::vector<bool>& occupied,
    int columns,
    int rows,
    int row,
    int column,
    int width) {
  if (columns <= 0 || rows <= 0 || row < 0 || row >= rows || width <= 0) {
    return;
  }
  const int start = std::clamp(column, 0, columns);
  const int end = std::clamp(column + width, 0, columns);
  for (int current = start; current < end; ++current) {
    occupied[static_cast<std::size_t>(row * columns + current)] = true;
  }
}

void mark_scene_rows_occupied(
    std::vector<bool>& occupied,
    int columns,
    int rows,
    const std::vector<SceneRow>& scene_rows) {
  for (const auto& row : scene_rows) {
    mark_occupied(occupied, columns, rows, row.row, row.column, row.width);
  }
}

std::vector<SceneRow> build_empty_scene_rows(
    const VisibleScene& scene) {
  if (scene.columns <= 0 || scene.rows <= 0) {
    return {};
  }

  std::vector<bool> occupied(static_cast<std::size_t>(scene.columns * scene.rows), false);
  for (const auto& pane : scene.panes) {
    mark_scene_rows_occupied(occupied, scene.columns, scene.rows, pane.body_rows);
  }
  mark_scene_rows_occupied(occupied, scene.columns, scene.rows, scene.borders.rows);
  mark_scene_rows_occupied(occupied, scene.columns, scene.rows, scene.status.rows);

  std::vector<SceneRow> rows;
  for (int row = 0; row < scene.rows; ++row) {
    int column = 0;
    while (column < scene.columns) {
      while (column < scene.columns &&
             occupied[static_cast<std::size_t>(row * scene.columns + column)]) {
        ++column;
      }
      if (column >= scene.columns) {
        break;
      }
      const int start = column;
      while (column < scene.columns &&
             !occupied[static_cast<std::size_t>(row * scene.columns + column)]) {
        ++column;
      }
      rows.push_back(scene_row_from_cells(
          SceneRowKind::EmptyOutside,
          0,
          row,
          start,
          column - start,
          default_blank_cells(column - start)));
    }
  }
  return rows;
}

RenderDiffPane build_pane_diff_state(
    const ActiveWindowFrame& frame,
    const RenderPane& pane,
    const PtyOutputSnapshot& snapshot,
    const PaneViewportStates& viewport_states,
    const CopyModeState& copy_mode) {
  RenderDiffPane state;
  state.rect = pane.rect;

  const int width = body_width(pane.rect, frame.columns);
  const int height = body_height(pane.rect, pane_area_rows(frame));
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
        height,
        state.first_visible_line + static_cast<std::size_t>(row),
        total_lines,
        state.viewport_offset,
        copy_mode));
  }
  return state;
}

SceneCursor build_snapshot_scene_cursor(
    const ActiveWindowFrame& frame,
    const std::unordered_map<PaneId, PtyOutputSnapshot>& snapshots,
    const PaneViewportStates& viewport_states,
    const CopyModeState& copy_mode) {
  SceneCursor cursor;
  if (copy_mode.active) {
    cursor.known = true;
    cursor.visible = false;
    cursor.pane_id = copy_mode.pane_id;
    return cursor;
  }

  const auto pane = std::ranges::find_if(frame.panes, [](const RenderPane& candidate) {
    return candidate.active;
  });
  if (pane == frame.panes.end()) {
    cursor.known = true;
    cursor.visible = false;
    return cursor;
  }

  cursor.pane_id = pane->rect.pane_id;
  const auto snapshot = snapshots.find(pane->rect.pane_id);
  if (snapshot == snapshots.end()) {
    cursor.known = true;
    cursor.visible = false;
    return cursor;
  }

  const int width = body_width(pane->rect, frame.columns);
  const int height = body_height(pane->rect, pane_area_rows(frame));
  if (width <= 0 || height <= 0) {
    cursor.known = true;
    cursor.visible = false;
    return cursor;
  }

  const auto viewport = viewport_states.find(pane->rect.pane_id);
  const auto requested_offset =
      viewport == viewport_states.end() ? std::size_t{0} : viewport->second.offset;
  const auto viewport_offset = clamped_viewport_offset(snapshot->second, height, requested_offset);
  cursor.known = true;
  cursor.visible = snapshot->second.screen.cursor_visible && viewport_offset == 0;
  cursor.style = snapshot->second.screen.cursor_style;
  cursor.row = body_top(pane->rect) +
               std::clamp(snapshot->second.screen.cursor_row, 0, height - 1);
  cursor.column = body_left(pane->rect) +
                  std::clamp(snapshot->second.screen.cursor_column, 0, width - 1);
  return cursor;
}

VisibleScene build_visible_scene_impl(
    const ActiveWindowFrame& frame,
    const std::unordered_map<PaneId, PtyOutputSnapshot>& snapshots,
    const PaneViewportStates& viewport_states,
    const CopyModeState& copy_mode,
    const RenderStatus& status) {
  VisibleScene scene;
  scene.window_id = frame.window_id;
  scene.layout_generation = frame.layout_generation;
  scene.active_pane_id = frame.active_pane_id;
  scene.columns = frame.columns;
  scene.rows = frame.rows;
  scene.status_bar_enabled = frame.status_bar_enabled;
  scene.panes.reserve(frame.panes.size());

  std::size_t active_viewport_offset = 0;
  for (const auto& pane : frame.panes) {
    ScenePane scene_pane;
    scene_pane.pane_id = pane.rect.pane_id;
    scene_pane.rect = pane.rect;
    scene_pane.active = pane.active;
    scene_pane.body_left = body_left(pane.rect);
    scene_pane.body_top = body_top(pane.rect);
    scene_pane.body_width = body_width(pane.rect, frame.columns);
    scene_pane.body_height = body_height(pane.rect, pane_area_rows(frame));

    const auto snapshot = snapshots.find(pane.rect.pane_id);
    if (snapshot != snapshots.end() &&
        scene_pane.body_width > 0 &&
        scene_pane.body_height > 0) {
      const auto viewport = viewport_states.find(pane.rect.pane_id);
      const auto requested_offset =
          viewport == viewport_states.end() ? std::size_t{0} : viewport->second.offset;
      scene_pane.viewport_offset =
          clamped_viewport_offset(snapshot->second, scene_pane.body_height, requested_offset);
      scene_pane.first_visible_line =
          first_visible_line_index(snapshot->second, scene_pane.body_height, scene_pane.viewport_offset);
      scene_pane.alternate_screen = snapshot->second.screen.alternate_screen;
      if (pane.active) {
        active_viewport_offset = scene_pane.viewport_offset;
      }

      const auto total_lines = snapshot_line_count(snapshot->second);
      scene_pane.body_rows.reserve(static_cast<std::size_t>(scene_pane.body_height));
      for (int row = 0; row < scene_pane.body_height; ++row) {
        scene_pane.body_rows.push_back(scene_row_from_diff_row(
            SceneRowKind::PaneBody,
            pane.rect.pane_id,
            build_body_row(
                pane,
                snapshot->second,
                row,
                scene_pane.body_width,
                scene_pane.body_height,
                scene_pane.first_visible_line + static_cast<std::size_t>(row),
                total_lines,
                scene_pane.viewport_offset,
                copy_mode)));
      }
    }
    scene.panes.push_back(std::move(scene_pane));
  }

  scene.borders.rows = build_border_scene_rows(frame, status.ui);

  const bool has_visible_temporary =
      status_has_visible_temporary(status.state, std::chrono::steady_clock::now());
  scene.status.visible = frame.rows > 1 &&
                         (frame.status_bar_enabled || has_visible_temporary ||
                          copy_mode.active);
  if (scene.status.visible) {
    scene.status.row = frame.rows - 1;
    scene.status.text = format_status_line(
        render_status_state(frame, copy_mode, status, active_viewport_offset),
        frame.columns);
    scene.status.rows.push_back(scene_row_from_cells(
        SceneRowKind::Status,
        0,
        scene.status.row,
        0,
        frame.columns,
        status_cells_for_text(scene.status.text, frame.columns)));
  }

  scene.cursor = build_snapshot_scene_cursor(frame, snapshots, viewport_states, copy_mode);
  scene.empty_rows = build_empty_scene_rows(scene);
  return scene;
}

bool render_diff_state_compatible(
    const RenderDiffState& diff_state,
    const ActiveWindowFrame& frame) {
  if (!diff_state.baseline_valid ||
      !diff_state.initialized ||
      diff_state.window_id != frame.window_id ||
      diff_state.layout_generation != frame.layout_generation ||
      diff_state.columns != frame.columns ||
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

bool render_diff_state_frame_compatible(
    const RenderDiffState& diff_state,
    const ActiveWindowFrame& frame) {
  return diff_state.baseline_valid &&
         diff_state.initialized &&
         diff_state.window_id == frame.window_id &&
         diff_state.columns == frame.columns &&
         diff_state.rows == frame.rows &&
         diff_state.status_bar_enabled == frame.status_bar_enabled;
}

bool render_diff_state_same_window(
    const RenderDiffState& diff_state,
    const ActiveWindowFrame& frame) {
  return diff_state.baseline_valid &&
         diff_state.initialized &&
         diff_state.window_id == frame.window_id;
}

void preserve_render_diff_state_for_layout(
    RenderDiffState& diff_state,
    const ActiveWindowFrame& frame) {
  diff_state.window_id = frame.window_id;
  diff_state.layout_generation = frame.layout_generation;
  diff_state.columns = frame.columns;
  diff_state.rows = frame.rows;
  diff_state.status_bar_enabled = frame.status_bar_enabled;
  diff_state.baseline_valid = true;
  diff_state.initialized = true;

  for (auto it = diff_state.panes.begin(); it != diff_state.panes.end();) {
    const auto pane_id = it->first;
    const auto still_visible = std::ranges::any_of(frame.panes, [&](const RenderPane& pane) {
      return pane.rect.pane_id == pane_id;
    });
    if (still_visible) {
      ++it;
    } else {
      it = diff_state.panes.erase(it);
    }
  }
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
    const ActiveWindowFrame& frame,
    const RenderPane& pane,
    const PtyOutputSnapshot& snapshot,
    const PaneViewportStates& viewport_states,
    const CopyModeState& copy_mode,
    const UiTheme& theme) {
  const int width = body_width(pane.rect, frame.columns);
  const int height = body_height(pane.rect, pane_area_rows(frame));
  if (width <= 0 || height <= 0) {
    diff_state.panes[pane.rect.pane_id] = RenderDiffPane{pane.rect};
    return false;
  }

  const auto viewport = viewport_states.find(pane.rect.pane_id);
  const auto requested_offset =
      viewport == viewport_states.end() ? std::size_t{0} : viewport->second.offset;
  const auto viewport_offset = clamped_viewport_offset(snapshot, height, requested_offset);
  const auto first_visible_line = first_visible_line_index(snapshot, height, viewport_offset);
  const auto total_lines = snapshot_line_count(snapshot);
  const auto previous = diff_state.panes.find(pane.rect.pane_id);

  const bool can_use_dirty_rows =
      previous != diff_state.panes.end() &&
      previous->second.body_rows.size() == static_cast<std::size_t>(height) &&
      previous->second.first_visible_line == first_visible_line &&
      previous->second.viewport_offset == viewport_offset &&
      !copy_mode.active &&
      !snapshot.scrollback_included &&
      snapshot.screen.damage == DamageKind::DirtyRows &&
      !snapshot.screen.dirty_rows.empty();

  if (can_use_dirty_rows) {
    bool wrote = false;
    auto& cached = previous->second;
    cached.rect = pane.rect;
    cached.first_visible_line = first_visible_line;
    cached.viewport_offset = viewport_offset;

    for (const int row : snapshot.screen.dirty_rows) {
      if (row < 0 || row >= height) {
        continue;
      }
      auto next_row = build_body_row(
          pane,
          snapshot,
          row,
          width,
          height,
          first_visible_line + static_cast<std::size_t>(row),
          total_lines,
          viewport_offset,
          copy_mode);
      wrote = append_changed_row(
                  out,
                  next_row,
                  &cached.body_rows[static_cast<std::size_t>(row)],
                  theme) ||
              wrote;
      cached.body_rows[static_cast<std::size_t>(row)] = std::move(next_row);
    }

    return wrote;
  }

  if (previous != diff_state.panes.end() &&
      !copy_mode.active &&
      !snapshot.scrollback_included &&
      snapshot.screen.damage == DamageKind::None &&
      previous->second.body_rows.size() == static_cast<std::size_t>(height) &&
      previous->second.first_visible_line == first_visible_line &&
      previous->second.viewport_offset == viewport_offset) {
    previous->second.rect = pane.rect;
    return false;
  }

  auto next = build_pane_diff_state(frame, pane, snapshot, viewport_states, copy_mode);
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
        out, diff_state, frame, pane, snapshot->second, viewport_states, copy_mode, theme);
  }
  return out;
}

void append_scene_row(std::string& out, const SceneRow& row, const UiTheme& theme) {
  if (row.width <= 0 || row.spans.empty()) {
    return;
  }

  for (const auto& span : row.spans) {
    if (span.cells.empty()) {
      continue;
    }
    append_cursor_move(out, row.row, span.column);
    out += "\x1b[0m";
    if (row.kind == SceneRowKind::Border) {
      bool active_style = false;
      for (const auto& cell : span.cells) {
        if (cell.accent_overlay && !active_style) {
          append_ui_foreground(out, theme);
          active_style = true;
        } else if (!cell.accent_overlay && active_style) {
          append_reset(out);
          active_style = false;
        } else if (!cell.accent_overlay) {
          append_reset(out);
        }
        out.append(cell.text);
      }
      append_reset(out);
      continue;
    }
    const bool default_cells = std::ranges::all_of(span.cells, [](const RenderDiffCell& cell) {
      return !cell.accent_overlay && default_attributes(cell.attributes);
    });
    if (default_cells) {
      for (const auto& cell : span.cells) {
        out.append(cell.text);
      }
      continue;
    }
    append_render_cells(
        out,
        span.cells,
        0,
        static_cast<int>(span.cells.size()),
        theme);
  }
}

void append_scene_cursor(std::string& out, const SceneCursor& cursor) {
  if (!cursor.known || !cursor.visible) {
    append_cursor_visible(out, false);
    return;
  }
  append_cursor_move(out, cursor.row, cursor.column);
  append_cursor_visible(out, true);
}

void append_scene_rows(
    std::string& out,
    const std::vector<SceneRow>& rows,
    const UiTheme& theme) {
  for (const auto& row : rows) {
    append_scene_row(out, row, theme);
  }
}

void append_ordered_scene_rows(
    std::vector<const SceneRow*>& rows,
    const VisibleScene& scene) {
  rows.reserve(
      scene.empty_rows.size() +
      scene.borders.rows.size() +
      scene.status.rows.size() +
      scene.panes.size() * 4);
  for (const auto& row : scene.empty_rows) {
    rows.push_back(&row);
  }
  for (const auto& pane : scene.panes) {
    for (const auto& row : pane.body_rows) {
      rows.push_back(&row);
    }
  }
  for (const auto& row : scene.borders.rows) {
    rows.push_back(&row);
  }
  for (const auto& row : scene.status.rows) {
    rows.push_back(&row);
  }
}

const SceneRow* find_baseline_scene_row(
    const VisibleScene& baseline,
    const SceneRow& wanted) {
  const auto matches_key = [&](const SceneRow& row) {
    return row.kind == wanted.kind &&
           row.pane_id == wanted.pane_id &&
           row.row == wanted.row &&
           row.column == wanted.column &&
           row.width == wanted.width;
  };

  for (const auto& row : baseline.empty_rows) {
    if (matches_key(row)) {
      return &row;
    }
  }
  for (const auto& pane : baseline.panes) {
    for (const auto& row : pane.body_rows) {
      if (matches_key(row)) {
        return &row;
      }
    }
  }
  for (const auto& row : baseline.borders.rows) {
    if (matches_key(row)) {
      return &row;
    }
  }
  for (const auto& row : baseline.status.rows) {
    if (matches_key(row)) {
      return &row;
    }
  }
  return nullptr;
}

RenderDiffRow render_diff_row_from_scene_row(const SceneRow& row) {
  RenderDiffRow diff_row;
  diff_row.row = row.row;
  diff_row.column = row.column;
  diff_row.width = row.width;
  if (!row.spans.empty()) {
    diff_row.column = row.spans.front().column;
    diff_row.cells = row.spans.front().cells;
  }
  return diff_row;
}

bool scene_baseline_can_delta(
    const RenderDiffState& baseline,
    const VisibleScene& scene) {
  return baseline.baseline_valid &&
         baseline.initialized &&
         baseline.scene_valid &&
         baseline.scene.window_id == scene.window_id &&
         baseline.scene.columns == scene.columns &&
         baseline.scene.rows == scene.rows;
}

void sync_render_diff_state_from_scene(RenderDiffState& diff_state, VisibleScene scene) {
  RenderDiffState next;
  next.window_id = scene.window_id;
  next.layout_generation = scene.layout_generation;
  next.columns = scene.columns;
  next.rows = scene.rows;
  next.status_bar_enabled = scene.status_bar_enabled;
  next.baseline_valid = true;
  next.initialized = true;
  next.scene_valid = true;
  next.panes.reserve(scene.panes.size());

  for (const auto& pane : scene.panes) {
    RenderDiffPane pane_state;
    pane_state.rect = pane.rect;
    pane_state.first_visible_line = pane.first_visible_line;
    pane_state.viewport_offset = pane.viewport_offset;
    pane_state.body_rows.reserve(pane.body_rows.size());
    pane_state.encoded_rows.resize(pane.body_rows.size());
    for (const auto& scene_row : pane.body_rows) {
      RenderDiffRow row;
      row.row = scene_row.row;
      row.column = scene_row.column;
      row.width = scene_row.width;
      if (!scene_row.spans.empty()) {
        row.cells = scene_row.spans.front().cells;
      }
      pane_state.body_rows.push_back(std::move(row));
    }
    next.panes.emplace(pane.pane_id, std::move(pane_state));
  }

  if (scene.status.visible) {
    next.status_line = scene.status.text;
  }
  next.scene = std::move(scene);
  diff_state = std::move(next);
}

bool append_visible_scene_delta(
    std::string& out,
    const VisibleScene& scene,
    const RenderDiffState* baseline,
    const UiTheme& theme,
    bool force_complete_scene,
    RenderFrameStats* stats) {
  const bool can_delta =
      baseline != nullptr && !force_complete_scene && scene_baseline_can_delta(*baseline, scene);
  const VisibleScene* previous_scene = can_delta ? &baseline->scene : nullptr;

  std::vector<const SceneRow*> ordered_rows;
  append_ordered_scene_rows(ordered_rows, scene);

  bool wrote = false;
  for (const SceneRow* row : ordered_rows) {
    if (stats != nullptr && row->kind == SceneRowKind::PaneBody) {
      ++stats->rows_considered;
    }

    const SceneRow* previous_row =
        previous_scene == nullptr ? nullptr : find_baseline_scene_row(*previous_scene, *row);
    if (previous_row != nullptr && scene_rows_equal(*row, *previous_row)) {
      if (stats != nullptr && row->kind == SceneRowKind::PaneBody) {
        ++stats->rows_skipped_generation_cache;
      }
      continue;
    }

    const auto before = out.size();
    if (previous_row != nullptr && row->kind == SceneRowKind::PaneBody) {
      auto next_diff_row = render_diff_row_from_scene_row(*row);
      auto previous_diff_row = render_diff_row_from_scene_row(*previous_row);
      (void)append_changed_row(out, next_diff_row, &previous_diff_row, theme);
    } else {
      append_scene_row(out, *row, theme);
    }
    wrote = wrote || out.size() != before;
    if (stats != nullptr && out.size() != before) {
      if (row->kind == SceneRowKind::PaneBody) {
        ++stats->rows_emitted;
        stats->body_bytes_emitted += out.size() - before;
      } else {
        stats->border_status_bytes_emitted += out.size() - before;
      }
    }
  }

  const bool cursor_changed =
      previous_scene == nullptr || !scene_cursor_equal(scene.cursor, previous_scene->cursor);
  if (cursor_changed) {
    const auto before = out.size();
    append_scene_cursor(out, scene.cursor);
    wrote = wrote || out.size() != before;
    if (stats != nullptr) {
      stats->cursor_bytes_emitted += out.size() - before;
    }
  }

  return wrote;
}

void sync_render_diff_state(
    RenderDiffState& diff_state,
    const ActiveWindowFrame& frame,
    const std::unordered_map<PaneId, PtyOutputSnapshot>& snapshots,
    const PaneViewportStates& viewport_states,
    const CopyModeState& copy_mode,
    const RenderStatus& status) {
  sync_render_diff_state_from_scene(
      diff_state,
      build_visible_scene_impl(frame, snapshots, viewport_states, copy_mode, status));
}

}  // namespace

int body_width(const PaneLayoutRect& rect) {
  return body_width(rect, rect.left + rect.width);
}

int body_height(const PaneLayoutRect& rect) {
  return body_height(rect, rect.top + rect.height);
}

int body_width(const PaneLayoutRect& rect, int frame_columns) {
  if (rect.width <= 0 || rect.height <= 0) {
    return 0;
  }
  const int right_border = rect.left + rect.width < frame_columns && rect.width > 1 ? 1 : 0;
  return std::max(0, rect.width - right_border);
}

int body_height(const PaneLayoutRect& rect, int frame_rows) {
  if (rect.width <= 0 || rect.height <= 0) {
    return 0;
  }
  const int bottom_border = rect.top + rect.height < frame_rows && rect.height > 1 ? 1 : 0;
  return std::max(0, rect.height - bottom_border);
}

VisibleScene build_visible_scene(
    const ActiveWindowFrame& frame,
    const std::unordered_map<PaneId, PtyOutputSnapshot>& snapshots,
    const PaneViewportStates& viewport_states,
    const CopyModeState& copy_mode,
    const RenderStatus& status) {
  return build_visible_scene_impl(frame, snapshots, viewport_states, copy_mode, status);
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

  const int width = body_width(pane->rect, frame.columns);
  const int height = body_height(pane->rect, pane_area_rows(frame));
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

std::string terminal_cell_text(const TerminalCell& cell) {
  if (!cell.extended.empty()) {
    return cell.extended;
  }
  if (cell.width == TerminalCellWidth::WideContinuation) {
    return {};
  }
  if (cell.codepoint < 0x80) {
    return std::string(1, static_cast<char>(cell.codepoint));
  }
  return utf8_from_codepoint(cell.codepoint);
}

std::vector<RenderDiffCell> render_cells_for_line_view(
    const TerminalLineView& line,
    int width) {
  std::vector<RenderDiffCell> cells;
  cells.reserve(static_cast<std::size_t>(std::max(0, width)));
  for (int column = 0; column < width; ++column) {
    if (column < static_cast<int>(line.cells.size())) {
      const auto& cell = line.cells[static_cast<std::size_t>(column)];
      cells.push_back(RenderDiffCell{
          terminal_cell_text(cell),
          cell.width,
          cell.attributes,
          false});
    } else {
      cells.push_back(RenderDiffCell{
          " ",
          TerminalCellWidth::Narrow,
          TerminalAttributes{},
          false});
    }
  }
  return cells;
}

std::string encode_line_view_for_render(
    const TerminalLineView& line,
    int width,
    const UiTheme& theme) {
  const auto cells = render_cells_for_line_view(line, width);
  std::string encoded;
  append_render_cells(encoded, cells, 0, width, theme);
  return encoded;
}

std::size_t live_screen_line_count(const PtyScreenRenderView& view) {
  if (view.cursor.alternate_screen) {
    return static_cast<std::size_t>(std::max(0, view.rows));
  }
  return view.scrollback_line_count + static_cast<std::size_t>(std::max(0, view.rows));
}

std::size_t live_first_visible_screen_row(
    const PtyScreenRenderView& view,
    int height,
    std::size_t viewport_offset) {
  if (height <= 0 || view.rows <= 0) {
    return 0;
  }
  if (viewport_offset != 0) {
    return 0;
  }
  return static_cast<std::size_t>(
      std::max(0, view.rows - std::min(height, view.rows)));
}

void reset_live_render_diff_state_if_needed(
    RenderDiffState& diff_state,
    const ActiveWindowFrame& frame,
    const RenderFrameOptions& options) {
  const bool compatible =
      options.preserve_layout_cache
          ? render_diff_state_frame_compatible(diff_state, frame)
          : render_diff_state_compatible(diff_state, frame);
  if (compatible && !options.clear_terminal) {
    diff_state.layout_generation = frame.layout_generation;
    return;
  }

  if (options.preserve_layout_cache &&
      !options.clear_terminal &&
      render_diff_state_same_window(diff_state, frame)) {
    preserve_render_diff_state_for_layout(diff_state, frame);
    return;
  }

  diff_state.window_id = frame.window_id;
  diff_state.layout_generation = frame.layout_generation;
  diff_state.columns = frame.columns;
  diff_state.rows = frame.rows;
  diff_state.status_bar_enabled = frame.status_bar_enabled;
  diff_state.baseline_valid = true;
  diff_state.initialized = true;
  diff_state.scene_valid = false;
  diff_state.scene = {};
  diff_state.panes.clear();
  diff_state.status_line.clear();
}

bool append_live_active_pane_cursor(
    std::string& out,
    const ActiveWindowFrame& frame,
    const PaneViewportStates& viewport_states,
    const CopyModeState& copy_mode) {
  if (copy_mode.active) {
    append_cursor_visible(out, false);
    return false;
  }

  const auto pane = std::ranges::find_if(frame.panes, [](const RenderPane& candidate) {
    return candidate.active;
  });
  if (pane == frame.panes.end() || !pane->shell) {
    append_cursor_visible(out, false);
    return false;
  }

  bool visible = false;
  int cursor_row = 0;
  int cursor_column = 0;
  pane->shell->with_screen_render_view(false, [&](const PtyScreenRenderView& view) {
    visible = view.cursor.visible;
    cursor_row = view.cursor.row;
    cursor_column = view.cursor.column;
  });

  if (!visible) {
    append_cursor_visible(out, false);
    return false;
  }

  const int width = body_width(pane->rect, frame.columns);
  const int height = body_height(pane->rect, pane_area_rows(frame));
  if (width <= 0 || height <= 0) {
    append_cursor_visible(out, false);
    return false;
  }

  const auto viewport = viewport_states.find(pane->rect.pane_id);
  const auto viewport_offset = viewport == viewport_states.end() ? std::size_t{0}
                                                                : viewport->second.offset;
  if (viewport_offset != 0) {
    append_cursor_visible(out, false);
    return false;
  }

  append_cursor_move(
      out,
      body_top(pane->rect) + std::clamp(cursor_row, 0, height - 1),
      body_left(pane->rect) + std::clamp(cursor_column, 0, width - 1));
  append_cursor_visible(out, true);
  return true;
}

struct LiveCursorPlacement {
  bool known{false};
  bool visible{false};
  int row{0};
  int column{0};
};

struct LiveOutputDeltaResult {
  bool handled{false};
  std::string frame;
};

bool append_live_cursor_placement(std::string& out, const LiveCursorPlacement& cursor) {
  if (!cursor.known || !cursor.visible) {
    append_cursor_visible(out, false);
    return false;
  }
  append_cursor_move(out, cursor.row, cursor.column);
  append_cursor_visible(out, true);
  return true;
}

bool can_use_live_output_delta_fast_path(
    const ActiveWindowFrame& frame,
    const PaneViewportStates& viewport_states,
    const CopyModeState& copy_mode,
    const RenderFrameOptions& options,
    const RenderDiffState& diff_state) {
  return !copy_mode.active &&
         !options.clear_terminal &&
         !options.force_body_repaint &&
         !options.preserve_layout_cache &&
         !options.repaint_body_on_geometry_change &&
         options.draw_pane_bodies &&
         !options.dirty_panes.empty() &&
         options.dirty_panes.size() == 1 &&
         viewport_states.empty() &&
         render_diff_state_frame_compatible(diff_state, frame);
}

bool pane_scroll_region_is_host_safe(
    const ActiveWindowFrame& frame,
    const RenderPane& pane,
    int left,
    int top,
    int width,
    int height,
    const TerminalScrollDamage& scroll) {
  return frame.panes.size() == 1 &&
         left == 0 &&
         width == frame.columns &&
         top >= 0 &&
         top + height <= pane_area_rows(frame) &&
         scroll.top_row == 0 &&
         scroll.bottom_row == height - 1 &&
         scroll.count > 0 &&
         scroll.count < height &&
         pane.rect.left == 0 &&
         pane.rect.width == frame.columns;
}

void append_terminal_scroll_region(
    std::string& out,
    int top,
    int bottom,
    int count,
    TerminalScrollDirection direction) {
  if (count <= 0 || bottom < top) {
    return;
  }
  out += "\x1b[";
  out += std::to_string(top + 1);
  out += ";";
  out += std::to_string(bottom + 1);
  out += "r";
  append_cursor_move(out, direction == TerminalScrollDirection::Up ? bottom : top, 0);
  out += "\x1b[";
  out += std::to_string(count);
  out += direction == TerminalScrollDirection::Up ? "S" : "T";
  out += "\x1b[r";
}

RenderDiffRow render_live_body_row(
    const RenderPane& pane,
    const PtyScreenRenderView& view,
    int local_row,
    int screen_row,
    int left,
    int top,
    int width) {
  RenderDiffRow row;
  row.row = top + local_row;
  row.column = left;
  row.width = width;
  row.cells =
      view.engine == nullptr || screen_row < 0 || screen_row >= view.rows
          ? default_blank_cells(width)
          : render_cells_for_line_view(view.engine->line_view(screen_row), width);
  (void)pane;
  return row;
}

void shift_live_scroll_cache(
    RenderDiffPane& pane_cache,
    int top,
    int left,
    int height,
    int count,
    TerminalScrollDirection direction) {
  if (count <= 0 || count >= height ||
      pane_cache.body_rows.size() != static_cast<std::size_t>(height)) {
    pane_cache.body_rows.clear();
    pane_cache.encoded_rows.clear();
    return;
  }

  if (direction == TerminalScrollDirection::Up) {
    for (int row = 0; row < height - count; ++row) {
      pane_cache.body_rows[static_cast<std::size_t>(row)] =
          std::move(pane_cache.body_rows[static_cast<std::size_t>(row + count)]);
      pane_cache.body_rows[static_cast<std::size_t>(row)].row = top + row;
      pane_cache.body_rows[static_cast<std::size_t>(row)].column = left;
    }
  } else {
    for (int row = height - 1; row >= count; --row) {
      pane_cache.body_rows[static_cast<std::size_t>(row)] =
          std::move(pane_cache.body_rows[static_cast<std::size_t>(row - count)]);
      pane_cache.body_rows[static_cast<std::size_t>(row)].row = top + row;
      pane_cache.body_rows[static_cast<std::size_t>(row)].column = left;
    }
  }

  if (pane_cache.encoded_rows.size() == static_cast<std::size_t>(height)) {
    if (direction == TerminalScrollDirection::Up) {
      for (int row = 0; row < height - count; ++row) {
        pane_cache.encoded_rows[static_cast<std::size_t>(row)] =
            std::move(pane_cache.encoded_rows[static_cast<std::size_t>(row + count)]);
      }
    } else {
      for (int row = height - 1; row >= count; --row) {
        pane_cache.encoded_rows[static_cast<std::size_t>(row)] =
            std::move(pane_cache.encoded_rows[static_cast<std::size_t>(row - count)]);
      }
    }
  }
}

LiveOutputDeltaResult try_render_live_output_delta_fast_path(
    const ActiveWindowFrame& frame,
    const PaneViewportStates& viewport_states,
    const CopyModeState& copy_mode,
    const RenderStatus& status,
    const RenderFrameOptions& options,
    RenderDiffState& diff_state,
    std::unordered_map<PaneId, std::uint64_t>& next_sequences,
    RenderFrameStats* stats) {
  if (!can_use_live_output_delta_fast_path(frame, viewport_states, copy_mode, options, diff_state)) {
    return {};
  }

  const auto dirty_pane_id = *options.dirty_panes.begin();
  const auto pane = std::ranges::find_if(frame.panes, [&](const RenderPane& candidate) {
    return candidate.rect.pane_id == dirty_pane_id;
  });
  if (pane == frame.panes.end() || !pane->shell) {
    return {};
  }

  auto pane_state = diff_state.panes.find(dirty_pane_id);
  if (pane_state == diff_state.panes.end()) {
    return {};
  }

  const int left = body_left(pane->rect);
  const int top = body_top(pane->rect);
  const int width = body_width(pane->rect, frame.columns);
  const int height = body_height(pane->rect, pane_area_rows(frame));
  if (width <= 0 || height <= 0 ||
      pane_state->second.body_rows.size() != static_cast<std::size_t>(height) ||
      !pane_rect_equal(pane_state->second.rect, pane->rect)) {
    return {};
  }

  LiveOutputDeltaResult result;
  result.handled = true;
  LiveCursorPlacement cursor;
  bool fallback_required = false;
  bool wrote = false;

  pane->shell->with_screen_render_view(true, [&](const PtyScreenRenderView& view) {
    next_sequences[pane->rect.pane_id] = view.next_sequence;
    if (view.engine == nullptr) {
      fallback_required = true;
      return;
    }

    const auto viewport_offset = std::size_t{0};
    const auto first_screen_row = live_first_visible_screen_row(view, height, viewport_offset);
    pane_state->second.first_visible_line = first_screen_row;
    pane_state->second.viewport_offset = viewport_offset;

    if (pane->active) {
      cursor.known = true;
      cursor.visible = view.cursor.visible;
      cursor.row = top + std::clamp(view.cursor.row, 0, height - 1);
      cursor.column = left + std::clamp(view.cursor.column, 0, width - 1);
    }

    if (view.damage.kind == DamageKind::None) {
      return;
    }
    if (view.cursor.alternate_screen || view.damage.kind >= DamageKind::FullPane) {
      fallback_required = true;
      return;
    }

    auto& cache = pane_state->second;
    if (cache.encoded_rows.size() != static_cast<std::size_t>(height)) {
      cache.encoded_rows.resize(static_cast<std::size_t>(height));
    }

    if (view.damage.scroll) {
      const auto& scroll = *view.damage.scroll;
      if (!pane_scroll_region_is_host_safe(frame, *pane, left, top, width, height, scroll)) {
        fallback_required = true;
        return;
      }

      append_cursor_visible(result.frame, false);
      append_terminal_scroll_region(
          result.frame,
          top,
          top + height - 1,
          scroll.count,
          scroll.direction);
      shift_live_scroll_cache(cache, top, left, height, scroll.count, scroll.direction);

      const int first_exposed =
          scroll.direction == TerminalScrollDirection::Up ? height - scroll.count : 0;
      const int last_exposed =
          scroll.direction == TerminalScrollDirection::Up ? height - 1 : scroll.count - 1;
      for (int local_row = first_exposed; local_row <= last_exposed; ++local_row) {
        const auto screen_row =
            static_cast<int>(first_screen_row + static_cast<std::size_t>(local_row));
        auto next_row =
            render_live_body_row(*pane, view, local_row, screen_row, left, top, width);
        wrote = append_changed_row(result.frame, next_row, nullptr, status.ui) || wrote;
        cache.body_rows[static_cast<std::size_t>(local_row)] = std::move(next_row);
        auto& encoded = cache.encoded_rows[static_cast<std::size_t>(local_row)];
        encoded.generation =
            screen_row < 0 || screen_row >= view.rows ? 0 : view.engine->line_generation(screen_row);
        encoded.style_generation = ui_theme_generation(status.ui);
        encoded.width = width;
        encoded.encoded.clear();
        if (stats != nullptr) {
          ++stats->rows_emitted;
        }
      }
      wrote = true;
      return;
    }

    if (view.damage.dirty_rows.empty()) {
      fallback_required = true;
      return;
    }

    for (const int dirty_screen_row : view.damage.dirty_rows) {
      const int local_row =
          dirty_screen_row - static_cast<int>(first_screen_row);
      if (local_row < 0 || local_row >= height) {
        continue;
      }
      auto next_row =
          render_live_body_row(*pane, view, local_row, dirty_screen_row, left, top, width);
      auto& cached_row = cache.body_rows[static_cast<std::size_t>(local_row)];
      wrote = append_changed_row(result.frame, next_row, &cached_row, status.ui) || wrote;
      cached_row = std::move(next_row);
      auto& encoded = cache.encoded_rows[static_cast<std::size_t>(local_row)];
      encoded.generation = view.engine->line_generation(dirty_screen_row);
      encoded.style_generation = ui_theme_generation(status.ui);
      encoded.width = width;
      encoded.encoded.clear();
      if (stats != nullptr) {
        ++stats->rows_considered;
        ++stats->rows_emitted;
      }
    }
  });

  if (fallback_required) {
    return {};
  }

  if (cursor.known) {
    wrote = append_live_cursor_placement(result.frame, cursor) || wrote;
  }

  if (!wrote) {
    result.frame.clear();
  } else if (status.synchronized_output_supported) {
    std::string wrapped;
    append_synchronized_output_begin(wrapped);
    wrapped += result.frame;
    append_synchronized_output_end(wrapped);
    result.frame = std::move(wrapped);
  }

  diff_state.scene_valid = false;
  return result;
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
  std::string out;
  const auto scene = build_visible_scene_impl(frame, snapshots, viewport_states, copy_mode, status);
  const bool force_complete_scene =
      options.clear_terminal ||
      options.force_body_repaint ||
      diff_state == nullptr ||
      !scene_baseline_can_delta(*diff_state, scene);
  const bool synchronized_frame =
      status.synchronized_output_supported &&
      (force_complete_scene || options.force_body_repaint ||
       options.preserve_layout_cache || options.repaint_body_on_geometry_change);
  if (synchronized_frame) {
    append_synchronized_output_begin(out);
  }
  if (force_complete_scene || options.force_body_repaint) {
    append_cursor_visible(out, false);
  }
  if (options.clear_terminal) {
    out += kClearTerminal;
  }
  (void)append_visible_scene_delta(
      out,
      scene,
      diff_state,
      status.ui,
      force_complete_scene,
      nullptr);
  if (synchronized_frame) {
    append_synchronized_output_end(out);
  }

  if (diff_state != nullptr) {
    sync_render_diff_state_from_scene(*diff_state, scene);
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

std::string render_live_frame_update(
    const ActiveWindowFrame& frame,
    const PaneViewportStates& viewport_states,
    const CopyModeState& copy_mode,
    const RenderStatus& status,
    const RenderFrameOptions& options,
    RenderDiffState& diff_state,
    std::unordered_map<PaneId, std::uint64_t>& next_sequences,
    RenderFrameStats* stats) {
  reset_live_render_diff_state_if_needed(diff_state, frame, options);

  if (auto fast_delta = try_render_live_output_delta_fast_path(
          frame,
          viewport_states,
          copy_mode,
          status,
          options,
          diff_state,
          next_sequences,
          stats);
      fast_delta.handled) {
    return std::move(fast_delta.frame);
  }

  const auto theme_generation = ui_theme_generation(status.ui);

  VisibleScene live_scene;
  live_scene.window_id = frame.window_id;
  live_scene.layout_generation = frame.layout_generation;
  live_scene.active_pane_id = frame.active_pane_id;
  live_scene.columns = frame.columns;
  live_scene.rows = frame.rows;
  live_scene.status_bar_enabled = frame.status_bar_enabled;
  live_scene.panes.reserve(frame.panes.size());
  std::size_t active_viewport_offset = 0;
  LiveCursorPlacement active_cursor;

  live_scene.borders.rows = build_border_scene_rows(frame, status.ui);

  for (const auto& pane : frame.panes) {
    if (!pane.shell) {
      continue;
    }

    const int left = body_left(pane.rect);
    const int top = body_top(pane.rect);
    const int width = body_width(pane.rect, frame.columns);
    const int height = body_height(pane.rect, pane_area_rows(frame));
    if (width <= 0 || height <= 0) {
      diff_state.panes[pane.rect.pane_id] = RenderDiffPane{pane.rect};
      continue;
    }
    if (stats != nullptr) {
      stats->visible_pane_rows += static_cast<std::size_t>(height);
    }

    ScenePane scene_pane;
    scene_pane.pane_id = pane.rect.pane_id;
    scene_pane.rect = pane.rect;
    scene_pane.active = pane.active;
    scene_pane.body_left = left;
    scene_pane.body_top = top;
    scene_pane.body_width = width;
    scene_pane.body_height = height;

    auto& pane_cache = diff_state.panes[pane.rect.pane_id];
    const bool pane_geometry_changed = !pane_rect_equal(pane_cache.rect, pane.rect);
    pane_cache.rect = pane.rect;
    if (pane_cache.encoded_rows.size() != static_cast<std::size_t>(height)) {
      pane_cache.encoded_rows.resize(static_cast<std::size_t>(height));
    }
    const bool render_body =
        should_render_pane_body(options, pane.rect.pane_id) ||
        (options.repaint_body_on_geometry_change && pane_geometry_changed);
    if (!render_body) {
      pane.shell->with_screen_render_view(false, [&](const PtyScreenRenderView& view) {
        next_sequences[pane.rect.pane_id] = view.next_sequence;
        scene_pane.alternate_screen = view.cursor.alternate_screen;
        const auto requested_offset = [&] {
          const auto viewport = viewport_states.find(pane.rect.pane_id);
          return viewport == viewport_states.end() ? std::size_t{0} : viewport->second.offset;
        }();
        const auto total_lines = live_screen_line_count(view);
        const auto visible_lines =
            std::min<std::size_t>(total_lines, static_cast<std::size_t>(height));
        const auto max_offset =
            total_lines > visible_lines ? total_lines - visible_lines : std::size_t{0};
        scene_pane.viewport_offset = std::min(requested_offset, max_offset);
        scene_pane.first_visible_line =
            total_lines > visible_lines ? total_lines - visible_lines - scene_pane.viewport_offset
                                        : std::size_t{0};
        if (pane.active) {
          active_viewport_offset = scene_pane.viewport_offset;
          active_cursor.known = true;
          active_cursor.visible = view.cursor.visible && scene_pane.viewport_offset == 0;
          active_cursor.row =
              body_top(pane.rect) + std::clamp(view.cursor.row, 0, height - 1);
          active_cursor.column =
              body_left(pane.rect) + std::clamp(view.cursor.column, 0, width - 1);
        }
        if (view.engine != nullptr) {
          const auto first_screen_row =
              live_first_visible_screen_row(view, height, scene_pane.viewport_offset);
          scene_pane.body_rows.reserve(static_cast<std::size_t>(height));
          for (int row = 0; row < height; ++row) {
            const auto screen_row =
                static_cast<int>(first_screen_row + static_cast<std::size_t>(row));
            const auto cells =
                screen_row < 0 || screen_row >= view.rows
                    ? default_blank_cells(width)
                    : render_cells_for_line_view(view.engine->line_view(screen_row), width);
            scene_pane.body_rows.push_back(scene_row_from_cells(
                SceneRowKind::PaneBody,
                pane.rect.pane_id,
                top + row,
                left,
                width,
                cells));
          }
        }
      });
      live_scene.panes.push_back(std::move(scene_pane));
      continue;
    }

    pane.shell->with_screen_render_view(true, [&](const PtyScreenRenderView& view) {
      next_sequences[pane.rect.pane_id] = view.next_sequence;
      scene_pane.alternate_screen = view.cursor.alternate_screen;
      if (stats != nullptr && view.cursor.alternate_screen) {
        ++stats->alternate_screen_panes;
      }
      const bool force_pane_redraw =
          options.clear_terminal || options.force_body_repaint ||
          view.damage.kind >= DamageKind::FullPane;

      const auto requested_offset = [&] {
        const auto viewport = viewport_states.find(pane.rect.pane_id);
        return viewport == viewport_states.end() ? std::size_t{0} : viewport->second.offset;
      }();
      const auto total_lines = live_screen_line_count(view);
      const auto visible_lines =
          std::min<std::size_t>(total_lines, static_cast<std::size_t>(height));
      const auto max_offset =
          total_lines > visible_lines ? total_lines - visible_lines : std::size_t{0};
      const auto viewport_offset = std::min(requested_offset, max_offset);
      if (pane.active) {
        active_viewport_offset = viewport_offset;
        active_cursor.known = true;
        active_cursor.visible = view.cursor.visible && viewport_offset == 0;
        active_cursor.row =
            body_top(pane.rect) + std::clamp(view.cursor.row, 0, height - 1);
        active_cursor.column =
            body_left(pane.rect) + std::clamp(view.cursor.column, 0, width - 1);
      }
      pane_cache.viewport_offset = viewport_offset;
      pane_cache.first_visible_line =
          total_lines > visible_lines ? total_lines - visible_lines - viewport_offset
                                      : std::size_t{0};
      scene_pane.viewport_offset = viewport_offset;
      scene_pane.first_visible_line = pane_cache.first_visible_line;
      scene_pane.body_rows.reserve(static_cast<std::size_t>(height));

      const auto first_screen_row =
          live_first_visible_screen_row(view, height, viewport_offset);
      for (int row = 0; row < height; ++row) {
        const auto screen_row = static_cast<int>(first_screen_row + static_cast<std::size_t>(row));
        const auto generation =
            view.engine == nullptr || screen_row < 0 || screen_row >= view.rows
                ? std::uint64_t{0}
                : view.engine->line_generation(screen_row);
        auto& row_cache = pane_cache.encoded_rows[static_cast<std::size_t>(row)];
        const bool row_changed = row_cache.generation != generation ||
                                 row_cache.style_generation != theme_generation ||
                                 row_cache.width != width;
        if (force_pane_redraw || row_changed) {
          row_cache.generation = generation;
          row_cache.style_generation = theme_generation;
          row_cache.width = width;
          row_cache.encoded.clear();
        }
        const auto scene_cells =
            view.engine == nullptr || screen_row < 0 || screen_row >= view.rows
                ? default_blank_cells(width)
                : render_cells_for_line_view(view.engine->line_view(screen_row), width);
        scene_pane.body_rows.push_back(scene_row_from_cells(
            SceneRowKind::PaneBody,
            pane.rect.pane_id,
            top + row,
            left,
            width,
            scene_cells));
      }
    });
    live_scene.panes.push_back(std::move(scene_pane));
  }

  for (auto it = diff_state.panes.begin(); it != diff_state.panes.end();) {
    const auto pane_id = it->first;
    const auto still_visible = std::ranges::any_of(frame.panes, [&](const RenderPane& pane) {
      return pane.rect.pane_id == pane_id;
    });
    if (still_visible) {
      ++it;
    } else {
      it = diff_state.panes.erase(it);
    }
  }

  const bool has_visible_temporary =
      status_has_visible_temporary(status.state, std::chrono::steady_clock::now());
  const bool show_status = frame.rows > 1 &&
                           (frame.status_bar_enabled || has_visible_temporary ||
                            copy_mode.active);
  if (show_status) {
    const auto status_model =
        render_status_state(frame, copy_mode, status, active_viewport_offset);
    const auto status_line = format_status_line(status_model, frame.columns);

    diff_state.status_line = status_line;
    live_scene.status.visible = true;
    live_scene.status.row = frame.rows - 1;
    live_scene.status.text = status_line;
    live_scene.status.rows.push_back(scene_row_from_cells(
        SceneRowKind::Status,
        0,
        live_scene.status.row,
        0,
        frame.columns,
        status_cells_for_text(status_line, frame.columns)));
  }

  if (copy_mode.active) {
    live_scene.cursor = SceneCursor{true, false, copy_mode.pane_id, 0, 0, 0};
  } else if (active_cursor.known) {
    live_scene.cursor = SceneCursor{
        true,
        active_cursor.visible,
        frame.active_pane_id,
        active_cursor.row,
        active_cursor.column,
        0};
  } else {
    live_scene.cursor = SceneCursor{true, false, frame.active_pane_id, 0, 0, 0};
  }
  live_scene.empty_rows = build_empty_scene_rows(live_scene);

  const bool force_complete_scene =
      options.clear_terminal ||
      options.force_body_repaint ||
      !scene_baseline_can_delta(diff_state, live_scene);
  const bool synchronized_frame =
      status.synchronized_output_supported &&
      (force_complete_scene || options.force_body_repaint ||
       options.preserve_layout_cache || options.repaint_body_on_geometry_change);

  std::string body;
  if (force_complete_scene || options.force_body_repaint) {
    append_cursor_visible(body, false);
  }
  if (options.clear_terminal) {
    body += kClearTerminal;
  }
  (void)append_visible_scene_delta(
      body,
      live_scene,
      &diff_state,
      status.ui,
      force_complete_scene,
      stats);

  std::string out;
  if (!body.empty()) {
    if (synchronized_frame) {
      append_synchronized_output_begin(out);
    }
    out += body;
    if (synchronized_frame) {
      append_synchronized_output_end(out);
    }
  }

  sync_render_diff_state_from_scene(diff_state, std::move(live_scene));
  return out;
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
        std::min(
            viewport.offset,
            max_viewport_offset(snapshot->second, body_height(pane.rect, pane_area_rows(frame))));
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

  const int height = body_height(pane->rect, pane_area_rows(frame));
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

  const int width = body_width(pane->rect, frame.columns);
  const int height = body_height(pane->rect, pane_area_rows(frame));
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

    const int width = body_width(pane->rect, frame.columns);
    const int height = body_height(pane->rect, pane_area_rows(frame));
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

  const int height = body_height(pane->rect, pane_area_rows(frame));
  const int width = body_width(pane->rect, frame.columns);
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
