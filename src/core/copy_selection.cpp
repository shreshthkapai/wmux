#include "wmux/copy_selection.hpp"

#include "wmux/unicode_width.hpp"

#include <algorithm>
#include <string_view>
#include <vector>

namespace wmux {
namespace {

bool operator<(const CopySelectionPoint& left, const CopySelectionPoint& right) {
  if (left.line != right.line) {
    return left.line < right.line;
  }
  return left.column < right.column;
}

std::size_t total_line_count(const PtyOutputSnapshot& snapshot) {
  if (snapshot.screen.alternate_screen) {
    return std::max(snapshot.screen.line_snapshots.size(), snapshot.screen.lines.size());
  }

  return std::max(
             snapshot.scrollback.total_lines,
             std::max(snapshot.scrollback.line_snapshots.size(), snapshot.scrollback.lines.size())) +
         std::max(snapshot.screen.line_snapshots.size(), snapshot.screen.lines.size());
}

TerminalLineSnapshot line_snapshot_at(const PtyOutputSnapshot& snapshot, std::size_t index) {
  if (snapshot.screen.alternate_screen) {
    if (index < snapshot.screen.line_snapshots.size()) {
      return snapshot.screen.line_snapshots[index];
    }
    if (index < snapshot.screen.lines.size()) {
      return TerminalLineSnapshot{snapshot.screen.lines[index], false};
    }
    return {};
  }

  const auto scrollback_count = std::max(
      snapshot.scrollback.total_lines,
      std::max(snapshot.scrollback.line_snapshots.size(), snapshot.scrollback.lines.size()));
  if (index < scrollback_count) {
    if (index < snapshot.scrollback.first_line_index) {
      return {};
    }
    const auto local_index = index - snapshot.scrollback.first_line_index;
    if (local_index < snapshot.scrollback.line_snapshots.size()) {
      return snapshot.scrollback.line_snapshots[local_index];
    }
    if (local_index < snapshot.scrollback.lines.size()) {
      return TerminalLineSnapshot{snapshot.scrollback.lines[local_index], false};
    }
    return {};
  }

  const auto screen_index = index - scrollback_count;
  if (screen_index < snapshot.screen.line_snapshots.size()) {
    return snapshot.screen.line_snapshots[screen_index];
  }

  if (screen_index < snapshot.screen.lines.size()) {
    return TerminalLineSnapshot{snapshot.screen.lines[screen_index], false};
  }

  return {};
}

void trim_trailing_horizontal_space(std::string& value) {
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
    value.pop_back();
  }
}

std::vector<TerminalTextCell> normalized_line_cells(
    const TerminalLineSnapshot& line,
    std::size_t width) {
  if (line.cells.empty()) {
    return terminal_text_cells_from_text(sanitize_utf8_boundaries(line.text), width);
  }

  std::vector<TerminalTextCell> cells;
  cells.reserve(width);
  for (std::size_t index = 0; index < line.cells.size() && cells.size() < width; ++index) {
    TerminalCellWidth cell_width = TerminalCellWidth::Narrow;
    if (index < line.cell_widths.size()) {
      cell_width = line.cell_widths[index];
    } else if (line.cells[index].empty()) {
      cell_width = TerminalCellWidth::WideContinuation;
    }

    cells.push_back(TerminalTextCell{
        cell_width == TerminalCellWidth::WideContinuation ? std::string{} : line.cells[index],
        cell_width});
  }

  for (std::size_t index = 0; index + 1 < cells.size(); ++index) {
    if (cells[index].width == TerminalCellWidth::Narrow &&
        cells[index + 1].width == TerminalCellWidth::WideContinuation) {
      cells[index].width = TerminalCellWidth::WideLeading;
    }
  }

  if (cells.size() < width) {
    cells.insert(
        cells.end(),
        width - cells.size(),
        TerminalTextCell{" ", TerminalCellWidth::Narrow});
  }
  if (cells.size() > width) {
    cells.resize(width);
  }
  return cells;
}

std::size_t leading_column_for(
    const std::vector<TerminalTextCell>& cells,
    std::size_t column) {
  column = std::min(column, cells.empty() ? std::size_t{0} : cells.size() - 1);
  while (column > 0 && cells[column].width == TerminalCellWidth::WideContinuation) {
    --column;
  }
  return column;
}

std::pair<std::size_t, std::size_t> expanded_copy_columns(
    const std::vector<TerminalTextCell>& cells,
    std::size_t first,
    std::size_t last) {
  if (cells.empty()) {
    return {0, 0};
  }

  first = leading_column_for(cells, std::min(first, cells.size() - 1));
  last = std::min(last, cells.size() - 1);
  if (cells[last].width == TerminalCellWidth::WideContinuation) {
    last = leading_column_for(cells, last);
  }
  if (cells[last].width == TerminalCellWidth::WideLeading && last + 1 < cells.size()) {
    last = last + 1;
  }
  return {std::min(first, last), std::max(first, last)};
}

}  // namespace

std::string extract_copy_selection_text(
    const PtyOutputSnapshot& snapshot,
    CopySelectionRange range,
    std::size_t line_width) {
  const auto total_lines = total_line_count(snapshot);
  if (total_lines == 0 || line_width == 0) {
    return {};
  }

  auto first = range.anchor;
  auto last = range.cursor;
  if (last < first) {
    std::swap(first, last);
  }

  first.line = std::min(first.line, total_lines - 1);
  last.line = std::min(last.line, total_lines - 1);
  first.column = std::min(first.column, line_width - 1);
  last.column = std::min(last.column, line_width - 1);

  std::string copied;
  copied.reserve((last.line - first.line + 1) * (line_width + 2));

  for (auto line_index = first.line; line_index <= last.line; ++line_index) {
    const auto line = line_snapshot_at(snapshot, line_index);
    const auto cells = normalized_line_cells(line, line_width);
    const auto start_column = line_index == first.line ? first.column : std::size_t{0};
    const auto end_column = line_index == last.line ? last.column : line_width - 1;
    std::string segment;
    if (start_column <= end_column && start_column < cells.size()) {
      const auto [first_column, last_column] =
          expanded_copy_columns(cells, start_column, end_column);
      for (auto column = first_column; column <= last_column; ++column) {
        if (cells[column].width != TerminalCellWidth::WideContinuation) {
          segment += cells[column].text;
        }
      }
    }

    if (line_index != last.line) {
      trim_trailing_horizontal_space(segment);
      copied += segment;
      if (!line.wrapped) {
        copied.append("\r\n");
      }
    } else {
      copied += segment;
    }
  }

  trim_trailing_horizontal_space(copied);
  return sanitize_utf8_boundaries(copied);
}

}  // namespace wmux
