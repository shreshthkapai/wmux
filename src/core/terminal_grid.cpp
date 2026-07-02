#include "wmux/terminal_grid.hpp"

#include "wmux/logging.hpp"
#include "wmux/unicode_width.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace wmux {
namespace {

constexpr int kDefaultColumns = 80;
constexpr int kDefaultRows = 24;
constexpr int kTabWidth = 8;
constexpr std::uint32_t kReplacementCodepoint = 0xfffd;
constexpr std::size_t kMaxLoggedUnknownSequences = 8;

int clamped_dimension(int value, int fallback) {
  return value > 0 ? value : fallback;
}

std::vector<TerminalCell> resized_buffer(
    const std::vector<TerminalCell>& old_buffer,
    int old_columns,
    int old_rows,
    int new_columns,
    int new_rows,
    const TerminalCell& blank) {
  std::vector<TerminalCell> next(static_cast<std::size_t>(new_columns * new_rows), blank);
  const int copied_rows = std::min(old_rows, new_rows);
  const int copied_columns = std::min(old_columns, new_columns);
  for (int row = 0; row < copied_rows; ++row) {
    for (int column = 0; column < copied_columns; ++column) {
      next[static_cast<std::size_t>((row * new_columns) + column)] =
          old_buffer[static_cast<std::size_t>((row * old_columns) + column)];
    }
  }
  return next;
}

std::vector<bool> resized_wrapped_lines(
    const std::vector<bool>& old_wrapped_lines,
    int old_rows,
    int new_rows) {
  std::vector<bool> next(static_cast<std::size_t>(new_rows), false);
  const int copied_rows = std::min(old_rows, new_rows);
  for (int row = 0; row < copied_rows; ++row) {
    next[static_cast<std::size_t>(row)] = old_wrapped_lines[static_cast<std::size_t>(row)];
  }
  return next;
}

std::vector<int> csi_params(std::string_view input) {
  std::vector<int> params;
  int value = 0;
  bool have_value = false;

  for (const char ch : input) {
    if (std::isdigit(static_cast<unsigned char>(ch))) {
      have_value = true;
      value = (value * 10) + (ch - '0');
      continue;
    }

    if (ch == ';' || ch == ':') {
      params.push_back(have_value ? value : 0);
      value = 0;
      have_value = false;
      continue;
    }
  }

  params.push_back(have_value ? value : 0);
  return params;
}

int param_or_default(const std::vector<int>& params, std::size_t index, int default_value) {
  if (index >= params.size() || params[index] == 0) {
    return default_value;
  }
  return params[index];
}

}  // namespace

TerminalGrid::TerminalGrid()
    : normal_buffer_(static_cast<std::size_t>(kDefaultColumns * kDefaultRows)),
      alternate_buffer_(static_cast<std::size_t>(kDefaultColumns * kDefaultRows)),
      normal_wrapped_lines_(static_cast<std::size_t>(kDefaultRows), false),
      alternate_wrapped_lines_(static_cast<std::size_t>(kDefaultRows), false) {
  reset_scroll_region();
}

TerminalGrid::TerminalGrid(int columns, int rows)
    : columns_(clamped_dimension(columns, kDefaultColumns)),
      rows_(clamped_dimension(rows, kDefaultRows)),
      normal_buffer_(static_cast<std::size_t>(columns_ * rows_)),
      alternate_buffer_(static_cast<std::size_t>(columns_ * rows_)),
      normal_wrapped_lines_(static_cast<std::size_t>(rows_), false),
      alternate_wrapped_lines_(static_cast<std::size_t>(rows_), false) {
  reset_scroll_region();
}

void TerminalGrid::resize(int columns, int rows) {
  const int next_columns = clamped_dimension(columns, kDefaultColumns);
  const int next_rows = clamped_dimension(rows, kDefaultRows);
  const auto blank = blank_cell();

  normal_buffer_ =
      resized_buffer(normal_buffer_, columns_, rows_, next_columns, next_rows, blank);
  alternate_buffer_ =
      resized_buffer(alternate_buffer_, columns_, rows_, next_columns, next_rows, blank);
  normal_wrapped_lines_ = resized_wrapped_lines(normal_wrapped_lines_, rows_, next_rows);
  alternate_wrapped_lines_ =
      resized_wrapped_lines(alternate_wrapped_lines_, rows_, next_rows);

  columns_ = next_columns;
  rows_ = next_rows;
  repair_wide_cells();
  cursor_column_ = std::clamp(cursor_column_, 0, columns_ - 1);
  cursor_row_ = std::clamp(cursor_row_, 0, rows_ - 1);
  saved_cursor_column_ = std::clamp(saved_cursor_column_, 0, columns_ - 1);
  saved_cursor_row_ = std::clamp(saved_cursor_row_, 0, rows_ - 1);
  reset_scroll_region();
  pending_wrap_ = false;
}

void TerminalGrid::feed(std::string_view bytes) {
  operation_buffer_.clear();
  parser_.feed(bytes, operation_buffer_);
  for (const auto& operation : operation_buffer_) {
    apply_operation(operation);
  }
}

void TerminalGrid::set_scrollback_capacity(std::size_t capacity) {
  scrollback_capacity_ = capacity;
  while (scrollback_.size() > scrollback_capacity_) {
    scrollback_.pop_front();
  }
}

TerminalScreenSnapshot TerminalGrid::snapshot() const {
  TerminalScreenSnapshot screen;
  screen.columns = columns_;
  screen.rows = rows_;
  screen.cursor_column = cursor_column_;
  screen.cursor_row = cursor_row_;
  screen.cursor_visible = cursor_visible_;
  screen.cursor_style = cursor_style_;
  screen.origin_mode = origin_mode_;
  screen.wrap_mode = wrap_mode_;
  screen.bracketed_paste_mode = bracketed_paste_mode_;
  screen.alternate_screen = alternate_screen_;
  screen.title = title_;
  screen.scrollback_line_count = scrollback_.size();
  screen.unknown_sequence_count = unknown_sequence_count_;
  screen.lines.reserve(static_cast<std::size_t>(rows_));
  screen.line_snapshots.reserve(static_cast<std::size_t>(rows_));

  const auto& buffer = active_buffer();
  const auto& wrapped_lines = active_wrapped_lines();
  for (int row = 0; row < rows_; ++row) {
    auto line = row_snapshot(buffer, wrapped_lines, row);
    screen.lines.push_back(line.text);
    screen.line_snapshots.push_back(std::move(line));
  }

  return screen;
}

TerminalScrollbackSnapshot TerminalGrid::scrollback_snapshot() const {
  TerminalScrollbackSnapshot snapshot;
  snapshot.capacity = scrollback_capacity_;
  snapshot.lines.reserve(scrollback_.size());
  snapshot.line_snapshots.reserve(scrollback_.size());
  for (const auto& line : scrollback_) {
    snapshot.lines.push_back(line.text);
    snapshot.line_snapshots.push_back(line);
  }
  return snapshot;
}

std::vector<TerminalCell>& TerminalGrid::active_buffer() {
  return alternate_screen_ ? alternate_buffer_ : normal_buffer_;
}

const std::vector<TerminalCell>& TerminalGrid::active_buffer() const {
  return alternate_screen_ ? alternate_buffer_ : normal_buffer_;
}

std::vector<bool>& TerminalGrid::active_wrapped_lines() {
  return alternate_screen_ ? alternate_wrapped_lines_ : normal_wrapped_lines_;
}

const std::vector<bool>& TerminalGrid::active_wrapped_lines() const {
  return alternate_screen_ ? alternate_wrapped_lines_ : normal_wrapped_lines_;
}

TerminalCell TerminalGrid::blank_cell() const {
  return TerminalCell{" ", current_attributes_, TerminalCellWidth::Narrow};
}

std::size_t TerminalGrid::offset(int row, int column) const {
  return static_cast<std::size_t>((row * columns_) + column);
}

std::vector<TerminalCell> TerminalGrid::row_cells(
    const std::vector<TerminalCell>& buffer,
    int row) const {
  std::vector<TerminalCell> line;
  line.reserve(static_cast<std::size_t>(columns_));
  for (int column = 0; column < columns_; ++column) {
    line.push_back(buffer[offset(row, column)]);
  }
  return line;
}

TerminalLineSnapshot TerminalGrid::row_snapshot(
    const std::vector<TerminalCell>& buffer,
    const std::vector<bool>& wrapped_lines,
    int row) const {
  TerminalLineSnapshot snapshot;
  snapshot.text = row_text(buffer, row);
  snapshot.wrapped = wrapped_lines[static_cast<std::size_t>(row)];
  snapshot.attributes.reserve(static_cast<std::size_t>(columns_));
  snapshot.cells.reserve(static_cast<std::size_t>(columns_));
  snapshot.cell_widths.reserve(static_cast<std::size_t>(columns_));
  for (int column = 0; column < columns_; ++column) {
    const auto& cell = buffer[offset(row, column)];
    snapshot.attributes.push_back(cell.attributes);
    snapshot.cells.push_back(
        cell.width == TerminalCellWidth::WideContinuation ? std::string{} : cell.glyph);
    snapshot.cell_widths.push_back(cell.width);
  }
  return snapshot;
}

std::string TerminalGrid::row_text(const std::vector<TerminalCell>& buffer, int row) const {
  std::string line;
  line.reserve(static_cast<std::size_t>(columns_));
  for (int column = 0; column < columns_; ++column) {
    const auto& cell = buffer[offset(row, column)];
    line += cell.width == TerminalCellWidth::WideContinuation ? std::string{" "} : cell.glyph;
  }
  return line;
}

std::string TerminalGrid::row_text(const std::vector<TerminalCell>& cells) const {
  std::string line;
  line.reserve(cells.size());
  for (const auto& cell : cells) {
    line += cell.width == TerminalCellWidth::WideContinuation ? std::string{" "} : cell.glyph;
  }
  return line;
}

void TerminalGrid::put_printable(char glyph) {
  put_cell(std::string(1, glyph), 1);
}

void TerminalGrid::put_codepoint(std::uint32_t codepoint) {
  if (codepoint == 0) {
    pending_wrap_ = false;
    return;
  }

  const auto glyph = utf8_from_codepoint(codepoint);
  const int width = terminal_codepoint_width(codepoint);
  if (width == 0) {
    append_zero_width_to_previous_cell(glyph);
    return;
  }

  if (append_codepoint_to_previous_grapheme(glyph, codepoint)) {
    return;
  }

  if (codepoint >= 0x20 && codepoint < 0x7f) {
    put_cell(std::string(1, static_cast<char>(codepoint)), 1);
    return;
  }

  if (codepoint == kReplacementCodepoint) {
    put_cell("?", 1);
    return;
  }

  put_cell(glyph, width);
}

void TerminalGrid::put_cell(std::string glyph, int width) {
  width = std::clamp(width, 1, std::max(1, columns_));
  if (pending_wrap_) {
    active_wrapped_lines()[static_cast<std::size_t>(cursor_row_)] = true;
    cursor_column_ = 0;
    line_feed();
    pending_wrap_ = false;
  }

  if (width > 1 && cursor_column_ + width > columns_) {
    if (wrap_mode_) {
      active_wrapped_lines()[static_cast<std::size_t>(cursor_row_)] = true;
      cursor_column_ = 0;
      line_feed();
      pending_wrap_ = false;
    } else {
      width = 1;
    }
  }

  clear_cell_at(cursor_row_, cursor_column_);
  if (width == 2 && cursor_column_ + 1 < columns_) {
    clear_cell_at(cursor_row_, cursor_column_ + 1);
  }

  active_buffer()[offset(cursor_row_, cursor_column_)] =
      TerminalCell{
          std::move(glyph),
          current_attributes_,
          width == 2 ? TerminalCellWidth::WideLeading : TerminalCellWidth::Narrow};
  for (int column = cursor_column_ + 1;
       column < std::min(columns_, cursor_column_ + width);
       ++column) {
    active_buffer()[offset(cursor_row_, column)] =
        TerminalCell{"", current_attributes_, TerminalCellWidth::WideContinuation};
  }

  if (cursor_column_ + width >= columns_) {
    if (wrap_mode_) {
      pending_wrap_ = true;
    } else {
      cursor_column_ = columns_ - 1;
      pending_wrap_ = false;
    }
  } else {
    cursor_column_ += width;
  }
}

int TerminalGrid::leading_column_for(int row, int column) const {
  if (row < 0 || row >= rows_ || column <= 0 || column >= columns_) {
    return std::clamp(column, 0, std::max(0, columns_ - 1));
  }

  const auto& buffer = active_buffer();
  if (buffer[offset(row, column)].width != TerminalCellWidth::WideContinuation) {
    return column;
  }

  for (int candidate = column - 1; candidate >= 0; --candidate) {
    const auto& cell = buffer[offset(row, candidate)];
    if (cell.width != TerminalCellWidth::WideContinuation) {
      return candidate;
    }
  }

  return column;
}

void TerminalGrid::clear_cell_at(int row, int column) {
  if (row < 0 || row >= rows_ || column < 0 || column >= columns_) {
    return;
  }

  auto& buffer = active_buffer();
  const int leading = leading_column_for(row, column);
  const auto replacement = blank_cell();
  if (leading < 0 || leading >= columns_) {
    return;
  }

  const auto width = buffer[offset(row, leading)].width;
  buffer[offset(row, leading)] = replacement;
  if (width == TerminalCellWidth::WideLeading && leading + 1 < columns_) {
    buffer[offset(row, leading + 1)] = replacement;
  }
  if (column != leading) {
    buffer[offset(row, column)] = replacement;
  }
}

void TerminalGrid::repair_wide_cells_in_row(std::vector<TerminalCell>& buffer, int row) const {
  if (row < 0 || row >= rows_) {
    return;
  }

  const auto replacement = blank_cell();
  for (int column = 0; column < columns_; ++column) {
    auto& cell = buffer[offset(row, column)];
    if (cell.width == TerminalCellWidth::WideContinuation) {
      if (column == 0 ||
          buffer[offset(row, column - 1)].width != TerminalCellWidth::WideLeading) {
        cell = replacement;
      }
      continue;
    }

    if (cell.width == TerminalCellWidth::WideLeading) {
      if (column + 1 >= columns_ ||
          buffer[offset(row, column + 1)].width != TerminalCellWidth::WideContinuation) {
        cell = replacement;
      } else {
        ++column;
      }
    }
  }
}

void TerminalGrid::repair_wide_cells() {
  for (int row = 0; row < rows_; ++row) {
    repair_wide_cells_in_row(normal_buffer_, row);
    repair_wide_cells_in_row(alternate_buffer_, row);
  }
}

bool TerminalGrid::append_codepoint_to_previous_grapheme(
    std::string_view glyph,
    std::uint32_t codepoint) {
  auto& buffer = active_buffer();
  int row = cursor_row_;
  int column = pending_wrap_ ? columns_ - 1 : cursor_column_ - 1;
  if (column < 0 && row > 0) {
    --row;
    column = columns_ - 1;
  }
  if (row < 0 || row >= rows_ || column < 0 || column >= columns_) {
    return false;
  }

  const int leading = leading_column_for(row, column);
  if (leading < 0 || leading >= columns_) {
    return false;
  }

  auto& cell = buffer[offset(row, leading)];
  if (cell.width == TerminalCellWidth::WideContinuation ||
      cell.glyph.empty() || cell.glyph == " ") {
    return false;
  }

  if (!terminal_codepoint_extends_previous_grapheme(cell.glyph, codepoint)) {
    return false;
  }

  cell.glyph.append(glyph);
  if (terminal_grapheme_width(cell.glyph) == 2 &&
      cell.width == TerminalCellWidth::Narrow &&
      leading + 1 < columns_) {
    const auto attributes = cell.attributes;
    clear_cell_at(row, leading + 1);
    buffer[offset(row, leading)].width = TerminalCellWidth::WideLeading;
    buffer[offset(row, leading + 1)] =
        TerminalCell{"", attributes, TerminalCellWidth::WideContinuation};
    if (row == cursor_row_ && cursor_column_ == leading + 1) {
      if (leading + 2 < columns_) {
        cursor_column_ = leading + 2;
      } else if (wrap_mode_) {
        pending_wrap_ = true;
      }
    }
  }
  return true;
}

void TerminalGrid::append_zero_width_to_previous_cell(std::string_view glyph) {
  auto& buffer = active_buffer();
  int row = cursor_row_;
  int column = pending_wrap_ ? columns_ - 1 : cursor_column_ - 1;
  if (column < 0 && cursor_row_ > 0) {
    row = cursor_row_ - 1;
    column = columns_ - 1;
  }

  for (; row >= 0; --row, column = columns_ - 1) {
    for (; column >= 0; --column) {
      const int leading = leading_column_for(row, column);
      auto& cell = buffer[offset(row, leading)];
      if (cell.width == TerminalCellWidth::WideContinuation) {
        continue;
      }
      if (!cell.glyph.empty() && cell.glyph != " ") {
        cell.glyph.append(glyph);
        if (terminal_grapheme_width(cell.glyph) == 2 &&
            cell.width == TerminalCellWidth::Narrow &&
            leading + 1 < columns_) {
          const auto attributes = cell.attributes;
          clear_cell_at(row, leading + 1);
          buffer[offset(row, leading)].width = TerminalCellWidth::WideLeading;
          buffer[offset(row, leading + 1)] =
              TerminalCell{"", attributes, TerminalCellWidth::WideContinuation};
          if (row == cursor_row_ && cursor_column_ == leading + 1) {
            if (leading + 2 < columns_) {
              cursor_column_ = leading + 2;
            } else if (wrap_mode_) {
              pending_wrap_ = true;
            }
          }
        }
      }
      return;
    }
  }
}

void TerminalGrid::carriage_return() {
  cursor_column_ = 0;
  pending_wrap_ = false;
}

void TerminalGrid::line_feed() {
  pending_wrap_ = false;
  if (cursor_row_ == scroll_bottom_) {
    scroll_up_region(scroll_top_, scroll_bottom_, 1);
  } else {
    cursor_row_ = std::min(rows_ - 1, cursor_row_ + 1);
  }
}

void TerminalGrid::backspace() {
  pending_wrap_ = false;
  if (cursor_column_ > 0) {
    --cursor_column_;
    cursor_column_ = leading_column_for(cursor_row_, cursor_column_);
  }
}

void TerminalGrid::tab() {
  pending_wrap_ = false;
  const int next_tab = ((cursor_column_ / kTabWidth) + 1) * kTabWidth;
  cursor_column_ = std::min(columns_ - 1, next_tab);
}

void TerminalGrid::backtab(int count) {
  pending_wrap_ = false;
  count = std::max(1, count);
  for (int step = 0; step < count; ++step) {
    if (cursor_column_ == 0) {
      return;
    }
    const int previous_tab = ((cursor_column_ - 1) / kTabWidth) * kTabWidth;
    cursor_column_ = std::max(0, previous_tab);
  }
}

void TerminalGrid::scroll_up() {
  scroll_up_region(0, rows_ - 1, 1);
}

void TerminalGrid::scroll_up_region(int top, int bottom, int count) {
  if (count <= 0 || top < 0 || bottom >= rows_ || top > bottom) {
    return;
  }

  auto& buffer = active_buffer();
  auto& wrapped_lines = active_wrapped_lines();
  const auto columns = static_cast<std::size_t>(columns_);
  for (int step = 0; step < count; ++step) {
    if (!alternate_screen_ && top == 0 && bottom == rows_ - 1) {
      const bool wrapped = !wrapped_lines.empty() && wrapped_lines.front();
      append_scrollback_line(row_cells(buffer, 0), wrapped);
    }

    if (top == bottom) {
      std::fill(
          buffer.begin() + static_cast<std::ptrdiff_t>(top * columns_),
          buffer.begin() + static_cast<std::ptrdiff_t>((top + 1) * columns_),
          blank_cell());
      wrapped_lines[static_cast<std::size_t>(top)] = false;
      continue;
    }

    auto first = buffer.begin() + static_cast<std::ptrdiff_t>(top * columns_);
    auto second = first + static_cast<std::ptrdiff_t>(columns);
    auto last = buffer.begin() + static_cast<std::ptrdiff_t>((bottom + 1) * columns_);
    std::move(second, last, first);
    std::fill(last - static_cast<std::ptrdiff_t>(columns), last, blank_cell());

    std::move(
        wrapped_lines.begin() + static_cast<std::ptrdiff_t>(top + 1),
        wrapped_lines.begin() + static_cast<std::ptrdiff_t>(bottom + 1),
        wrapped_lines.begin() + static_cast<std::ptrdiff_t>(top));
    wrapped_lines[static_cast<std::size_t>(bottom)] = false;
  }
}

void TerminalGrid::scroll_down_region(int top, int bottom, int count) {
  if (count <= 0 || top < 0 || bottom >= rows_ || top > bottom) {
    return;
  }

  auto& buffer = active_buffer();
  auto& wrapped_lines = active_wrapped_lines();
  const auto columns = static_cast<std::size_t>(columns_);
  for (int step = 0; step < count; ++step) {
    if (top == bottom) {
      std::fill(
          buffer.begin() + static_cast<std::ptrdiff_t>(top * columns_),
          buffer.begin() + static_cast<std::ptrdiff_t>((top + 1) * columns_),
          blank_cell());
      wrapped_lines[static_cast<std::size_t>(top)] = false;
      continue;
    }

    auto first = buffer.begin() + static_cast<std::ptrdiff_t>(top * columns_);
    auto last = buffer.begin() + static_cast<std::ptrdiff_t>((bottom + 1) * columns_);
    std::move_backward(
        first,
        last - static_cast<std::ptrdiff_t>(columns),
        last);
    std::fill(first, first + static_cast<std::ptrdiff_t>(columns), blank_cell());

    std::move_backward(
        wrapped_lines.begin() + static_cast<std::ptrdiff_t>(top),
        wrapped_lines.begin() + static_cast<std::ptrdiff_t>(bottom),
        wrapped_lines.begin() + static_cast<std::ptrdiff_t>(bottom + 1));
    wrapped_lines[static_cast<std::size_t>(top)] = false;
  }
}

void TerminalGrid::append_scrollback_line(std::vector<TerminalCell> line, bool wrapped) {
  if (scrollback_capacity_ == 0) {
    return;
  }

  TerminalLineSnapshot snapshot;
  snapshot.text = row_text(line);
  snapshot.wrapped = wrapped;
  snapshot.attributes.reserve(line.size());
  snapshot.cells.reserve(line.size());
  snapshot.cell_widths.reserve(line.size());
  for (const auto& cell : line) {
    snapshot.attributes.push_back(cell.attributes);
    snapshot.cells.push_back(
        cell.width == TerminalCellWidth::WideContinuation ? std::string{} : cell.glyph);
    snapshot.cell_widths.push_back(cell.width);
  }
  scrollback_.push_back(std::move(snapshot));
  while (scrollback_.size() > scrollback_capacity_) {
    scrollback_.pop_front();
  }
}

void TerminalGrid::clear_scrollback() {
  scrollback_.clear();
}

void TerminalGrid::clear_all() {
  std::fill(active_buffer().begin(), active_buffer().end(), blank_cell());
  std::fill(active_wrapped_lines().begin(), active_wrapped_lines().end(), false);
}

void TerminalGrid::clear_line(int mode) {
  int first = 0;
  int last = columns_ - 1;
  if (mode == 0) {
    first = cursor_column_;
  } else if (mode == 1) {
    last = cursor_column_;
  }

  for (int column = first; column <= last; ++column) {
    clear_cell_at(cursor_row_, column);
  }
  if (mode == 2 || last == columns_ - 1) {
    active_wrapped_lines()[static_cast<std::size_t>(cursor_row_)] = false;
  }
}

void TerminalGrid::clear_screen(int mode) {
  auto& wrapped_lines = active_wrapped_lines();
  if (mode == 2 || mode == 3) {
    if (mode == 3 && !alternate_screen_) {
      clear_scrollback();
    }
    clear_all();
    return;
  }

  if (mode == 1) {
    for (int row = 0; row < cursor_row_; ++row) {
      for (int column = 0; column < columns_; ++column) {
        clear_cell_at(row, column);
      }
      wrapped_lines[static_cast<std::size_t>(row)] = false;
    }
    for (int column = 0; column <= cursor_column_; ++column) {
      clear_cell_at(cursor_row_, column);
    }
    if (cursor_column_ == columns_ - 1) {
      wrapped_lines[static_cast<std::size_t>(cursor_row_)] = false;
    }
    return;
  }

  for (int column = cursor_column_; column < columns_; ++column) {
    clear_cell_at(cursor_row_, column);
  }
  wrapped_lines[static_cast<std::size_t>(cursor_row_)] = false;
  for (int row = cursor_row_ + 1; row < rows_; ++row) {
    for (int column = 0; column < columns_; ++column) {
      clear_cell_at(row, column);
    }
    wrapped_lines[static_cast<std::size_t>(row)] = false;
  }
}

void TerminalGrid::erase_characters(int count) {
  count = std::max(1, count);
  const int last = std::min(columns_ - 1, cursor_column_ + count - 1);
  for (int column = cursor_column_; column <= last; ++column) {
    clear_cell_at(cursor_row_, column);
  }
  if (last == columns_ - 1) {
    active_wrapped_lines()[static_cast<std::size_t>(cursor_row_)] = false;
  }
}

void TerminalGrid::insert_characters(int count) {
  count = std::min(std::max(1, count), columns_ - cursor_column_);
  auto& buffer = active_buffer();
  for (int column = columns_ - 1; column >= cursor_column_ + count; --column) {
    buffer[offset(cursor_row_, column)] = buffer[offset(cursor_row_, column - count)];
  }
  for (int column = cursor_column_; column < cursor_column_ + count; ++column) {
    buffer[offset(cursor_row_, column)] = blank_cell();
  }
  repair_wide_cells_in_row(buffer, cursor_row_);
  active_wrapped_lines()[static_cast<std::size_t>(cursor_row_)] = false;
}

void TerminalGrid::delete_characters(int count) {
  count = std::min(std::max(1, count), columns_ - cursor_column_);
  auto& buffer = active_buffer();
  for (int column = cursor_column_; column + count < columns_; ++column) {
    buffer[offset(cursor_row_, column)] = buffer[offset(cursor_row_, column + count)];
  }
  for (int column = columns_ - count; column < columns_; ++column) {
    buffer[offset(cursor_row_, column)] = blank_cell();
  }
  repair_wide_cells_in_row(buffer, cursor_row_);
  active_wrapped_lines()[static_cast<std::size_t>(cursor_row_)] = false;
}

void TerminalGrid::move_cursor(int row, int column) {
  pending_wrap_ = false;
  cursor_row_ = std::clamp(row, 0, rows_ - 1);
  cursor_column_ = std::clamp(column, 0, columns_ - 1);
  cursor_column_ = leading_column_for(cursor_row_, cursor_column_);
}

void TerminalGrid::move_cursor_position(int row, int column) {
  pending_wrap_ = false;
  if (origin_mode_) {
    cursor_row_ = std::clamp(scroll_top_ + row, scroll_top_, scroll_bottom_);
  } else {
    cursor_row_ = std::clamp(row, 0, rows_ - 1);
  }
  cursor_column_ = std::clamp(column, 0, columns_ - 1);
  cursor_column_ = leading_column_for(cursor_row_, cursor_column_);
}

void TerminalGrid::move_cursor_relative(int row_delta, int column_delta) {
  pending_wrap_ = false;
  cursor_row_ = std::clamp(cursor_row_ + row_delta, 0, rows_ - 1);
  cursor_column_ = std::clamp(cursor_column_ + column_delta, 0, columns_ - 1);
  if (active_buffer()[offset(cursor_row_, cursor_column_)].width ==
      TerminalCellWidth::WideContinuation) {
    if (column_delta > 0) {
      while (cursor_column_ + 1 < columns_ &&
             active_buffer()[offset(cursor_row_, cursor_column_)].width ==
                 TerminalCellWidth::WideContinuation) {
        ++cursor_column_;
      }
      if (active_buffer()[offset(cursor_row_, cursor_column_)].width ==
          TerminalCellWidth::WideContinuation) {
        cursor_column_ = leading_column_for(cursor_row_, cursor_column_);
      }
    } else {
      cursor_column_ = leading_column_for(cursor_row_, cursor_column_);
    }
  }
}

void TerminalGrid::save_cursor() {
  saved_cursor_row_ = cursor_row_;
  saved_cursor_column_ = cursor_column_;
}

void TerminalGrid::restore_cursor() {
  move_cursor(saved_cursor_row_, saved_cursor_column_);
}

void TerminalGrid::set_scroll_region(int top, int bottom) {
  if (top < 0 || bottom >= rows_ || top >= bottom) {
    reset_scroll_region();
    return;
  }

  scroll_top_ = top;
  scroll_bottom_ = bottom;
  move_cursor(0, 0);
}

void TerminalGrid::reset_scroll_region() {
  scroll_top_ = 0;
  scroll_bottom_ = std::max(0, rows_ - 1);
}

void TerminalGrid::apply_operation(const TerminalVtOperation& operation) {
  switch (operation.kind) {
    case TerminalVtOperationKind::Print:
      put_codepoint(operation.codepoint);
      break;
    case TerminalVtOperationKind::CarriageReturn:
      carriage_return();
      break;
    case TerminalVtOperationKind::LineFeed:
      line_feed();
      break;
    case TerminalVtOperationKind::Backspace:
      backspace();
      break;
    case TerminalVtOperationKind::Tab:
      tab();
      break;
    case TerminalVtOperationKind::Escape:
      apply_escape(operation.escape_final);
      break;
    case TerminalVtOperationKind::Csi:
      apply_csi(operation.csi.parameters, operation.csi.final_byte);
      break;
    case TerminalVtOperationKind::Osc:
      apply_osc(operation.osc.payload);
      break;
    case TerminalVtOperationKind::Unknown:
      record_unknown_sequence(operation.unknown);
      break;
  }
}

void TerminalGrid::apply_escape(char final_byte) {
  switch (final_byte) {
    case '7':
      save_cursor();
      break;
    case '8':
      restore_cursor();
      break;
    case 'D':
      line_feed();
      break;
    case 'E':
      carriage_return();
      line_feed();
      break;
    case 'M':
      if (cursor_row_ == scroll_top_) {
        scroll_down_region(scroll_top_, scroll_bottom_, 1);
      } else {
        move_cursor_relative(-1, 0);
      }
      break;
    case 'c':
      parser_.reset();
      set_alternate_screen(false);
      clear_all();
      move_cursor(0, 0);
      saved_cursor_column_ = 0;
      saved_cursor_row_ = 0;
      reset_scroll_region();
      current_attributes_ = {};
      cursor_visible_ = true;
      cursor_style_ = 0;
      origin_mode_ = false;
      wrap_mode_ = true;
      bracketed_paste_mode_ = false;
      break;
    default:
      record_unknown_sequence(
          TerminalVtUnknownOperation{TerminalVtUnknownClass::Escape, 2, final_byte});
      break;
  }
}

void TerminalGrid::apply_csi(std::string_view parameters, char final_byte) {
  const auto params = csi_params(parameters);

  switch (final_byte) {
    case 'A':
      move_cursor_relative(-param_or_default(params, 0, 1), 0);
      break;
    case 'B':
      move_cursor_relative(param_or_default(params, 0, 1), 0);
      break;
    case 'C':
      move_cursor_relative(0, param_or_default(params, 0, 1));
      break;
    case 'D':
      move_cursor_relative(0, -param_or_default(params, 0, 1));
      break;
    case 'E':
      move_cursor_relative(param_or_default(params, 0, 1), -cursor_column_);
      break;
    case 'F':
      move_cursor_relative(-param_or_default(params, 0, 1), -cursor_column_);
      break;
    case 'G':
      move_cursor(cursor_row_, param_or_default(params, 0, 1) - 1);
      break;
    case 'H':
    case 'f':
      move_cursor_position(
          param_or_default(params, 0, 1) - 1,
          param_or_default(params, 1, 1) - 1);
      break;
    case 'I':
      tab();
      for (int step = 1; step < param_or_default(params, 0, 1); ++step) {
        tab();
      }
      break;
    case 'Z':
      backtab(param_or_default(params, 0, 1));
      break;
    case 'J':
      clear_screen(param_or_default(params, 0, 0));
      break;
    case 'K':
      clear_line(param_or_default(params, 0, 0));
      break;
    case 'L':
      if (cursor_row_ >= scroll_top_ && cursor_row_ <= scroll_bottom_) {
        scroll_down_region(cursor_row_, scroll_bottom_, param_or_default(params, 0, 1));
      }
      break;
    case 'M':
      if (cursor_row_ >= scroll_top_ && cursor_row_ <= scroll_bottom_) {
        scroll_up_region(cursor_row_, scroll_bottom_, param_or_default(params, 0, 1));
      }
      break;
    case 'P':
      delete_characters(param_or_default(params, 0, 1));
      break;
    case 'S':
      scroll_up_region(scroll_top_, scroll_bottom_, param_or_default(params, 0, 1));
      break;
    case 'T':
      scroll_down_region(scroll_top_, scroll_bottom_, param_or_default(params, 0, 1));
      break;
    case 'X':
      erase_characters(param_or_default(params, 0, 1));
      break;
    case '@':
      insert_characters(param_or_default(params, 0, 1));
      break;
    case 'a':
      move_cursor_relative(0, param_or_default(params, 0, 1));
      break;
    case 'd':
      move_cursor(param_or_default(params, 0, 1) - 1, cursor_column_);
      break;
    case 'e':
      move_cursor_relative(param_or_default(params, 0, 1), 0);
      break;
    case 'h':
      if (!parameters.empty() && parameters.front() == '?') {
        for (const int mode : params) {
          apply_private_mode(mode, true);
        }
      }
      break;
    case 'l':
      if (!parameters.empty() && parameters.front() == '?') {
        for (const int mode : params) {
          apply_private_mode(mode, false);
        }
      }
      break;
    case 'm':
      apply_sgr(params);
      break;
    case 'q':
      cursor_style_ = std::clamp(param_or_default(params, 0, 0), 0, 6);
      break;
    case 'r':
      set_scroll_region(param_or_default(params, 0, 1) - 1, param_or_default(params, 1, rows_) - 1);
      break;
    case 's':
      save_cursor();
      break;
    case 'u':
      restore_cursor();
      break;
    default:
      record_unknown_sequence(TerminalVtUnknownOperation{
          TerminalVtUnknownClass::Csi,
          parameters.size() + 3,
          final_byte});
      break;
  }
}

void TerminalGrid::apply_osc(std::string_view payload) {
  const auto separator = payload.find(';');
  if (separator == std::string_view::npos) {
    return;
  }

  const auto command = payload.substr(0, separator);
  if (command != "0" && command != "2") {
    return;
  }

  auto title = std::string{payload.substr(separator + 1)};
  constexpr std::size_t kMaxTitleLength = 512;
  if (title.size() > kMaxTitleLength) {
    title.resize(kMaxTitleLength);
  }
  title_ = std::move(title);
}

void TerminalGrid::apply_private_mode(int mode, bool enabled) {
  switch (mode) {
    case 6:
      origin_mode_ = enabled;
      move_cursor_position(0, 0);
      break;
    case 7:
      wrap_mode_ = enabled;
      pending_wrap_ = false;
      break;
    case 25:
      cursor_visible_ = enabled;
      break;
    case 47:
    case 1047:
    case 1049:
      set_alternate_screen(enabled);
      break;
    case 2004:
      bracketed_paste_mode_ = enabled;
      break;
    default:
      break;
  }
}

void TerminalGrid::apply_sgr(const std::vector<int>& params) {
  if (params.empty()) {
    current_attributes_ = {};
    return;
  }

  const auto apply_indexed_or_truecolor = [&](std::size_t& index, bool foreground) {
    if (index + 2 < params.size() && params[index + 1] == 5) {
      const auto color = std::clamp(params[index + 2], 0, 255);
      if (foreground) {
        current_attributes_.foreground = color;
      } else {
        current_attributes_.background = color;
      }
      index += 2;
      return true;
    }

    if (index + 4 < params.size() && params[index + 1] == 2) {
      const auto red = std::clamp(params[index + 2], 0, 255);
      const auto green = std::clamp(params[index + 3], 0, 255);
      const auto blue = std::clamp(params[index + 4], 0, 255);
      const auto color = 0x01000000 | (red << 16) | (green << 8) | blue;
      if (foreground) {
        current_attributes_.foreground = color;
      } else {
        current_attributes_.background = color;
      }
      index += 4;
      return true;
    }

    return false;
  };

  for (std::size_t index = 0; index < params.size(); ++index) {
    const int param = params[index];
    if (param == 0) {
      current_attributes_ = {};
    } else if (param == 1) {
      current_attributes_.bold = true;
    } else if (param == 2) {
      current_attributes_.dim = true;
    } else if (param == 3) {
      current_attributes_.italic = true;
    } else if (param == 4) {
      current_attributes_.underline = true;
    } else if (param == 7) {
      current_attributes_.inverse = true;
    } else if (param == 22) {
      current_attributes_.bold = false;
      current_attributes_.dim = false;
    } else if (param == 23) {
      current_attributes_.italic = false;
    } else if (param == 24) {
      current_attributes_.underline = false;
    } else if (param == 27) {
      current_attributes_.inverse = false;
    } else if (param >= 30 && param <= 37) {
      current_attributes_.foreground = param - 30;
    } else if (param == 39) {
      current_attributes_.foreground = -1;
    } else if (param >= 40 && param <= 47) {
      current_attributes_.background = param - 40;
    } else if (param == 49) {
      current_attributes_.background = -1;
    } else if (param == 38) {
      (void)apply_indexed_or_truecolor(index, true);
    } else if (param == 48) {
      (void)apply_indexed_or_truecolor(index, false);
    } else if (param >= 90 && param <= 97) {
      current_attributes_.foreground = 8 + param - 90;
    } else if (param >= 100 && param <= 107) {
      current_attributes_.background = 8 + param - 100;
    }
  }
}

void TerminalGrid::set_alternate_screen(bool enabled) {
  if (alternate_screen_ == enabled) {
    return;
  }

  if (enabled) {
    normal_cursor_column_before_alternate_ = cursor_column_;
    normal_cursor_row_before_alternate_ = cursor_row_;
  }

  alternate_screen_ = enabled;
  if (enabled) {
    move_cursor(0, 0);
    clear_all();
  } else {
    move_cursor(normal_cursor_row_before_alternate_, normal_cursor_column_before_alternate_);
  }
}

void TerminalGrid::record_unknown_sequence(const TerminalVtUnknownOperation& unknown) {
  ++unknown_sequence_count_;
  if (logged_unknown_sequence_count_ >= kMaxLoggedUnknownSequences) {
    return;
  }

  ++logged_unknown_sequence_count_;
  log_event(
      LogLevel::Debug,
      "terminal-grid",
      "unknown-vt-sequence",
      {
          {"class", terminal_vt_unknown_class_name(unknown.sequence_class)},
          {"length", std::to_string(unknown.length)},
          {"final", unknown.final_byte == 0 ? std::string{} : std::string(1, unknown.final_byte)},
      });
}

}  // namespace wmux
