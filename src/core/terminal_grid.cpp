#include "wmux/terminal_grid.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

namespace wmux {
namespace {

constexpr int kDefaultColumns = 80;
constexpr int kDefaultRows = 24;
constexpr int kTabWidth = 4;

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

bool private_csi_mode(std::string_view input, int mode) {
  if (input.empty() || input.front() != '?') {
    return false;
  }

  const auto params = csi_params(input);
  return std::ranges::find(params, mode) != params.end();
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
      alternate_wrapped_lines_(static_cast<std::size_t>(kDefaultRows), false) {}

TerminalGrid::TerminalGrid(int columns, int rows)
    : normal_buffer_(static_cast<std::size_t>(kDefaultColumns * kDefaultRows)),
      alternate_buffer_(static_cast<std::size_t>(kDefaultColumns * kDefaultRows)),
      normal_wrapped_lines_(static_cast<std::size_t>(kDefaultRows), false),
      alternate_wrapped_lines_(static_cast<std::size_t>(kDefaultRows), false) {
  resize(columns, rows);
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
  cursor_column_ = std::clamp(cursor_column_, 0, columns_ - 1);
  cursor_row_ = std::clamp(cursor_row_, 0, rows_ - 1);
  pending_wrap_ = false;
}

void TerminalGrid::feed(std::string_view bytes) {
  for (const unsigned char byte : bytes) {
    switch (parser_state_) {
      case ParserState::Ground:
        if (byte == 0x1b) {
          parser_state_ = ParserState::Escape;
        } else if (byte == '\r') {
          carriage_return();
        } else if (byte == '\n') {
          line_feed();
        } else if (byte == '\b') {
          backspace();
        } else if (byte == '\t') {
          tab();
        } else if (byte >= 0x20 && byte != 0x7f) {
          put_printable(static_cast<char>(byte));
        }
        break;

      case ParserState::Escape:
        if (byte == '[') {
          csi_buffer_.clear();
          parser_state_ = ParserState::Csi;
        } else if (byte == ']') {
          parser_state_ = ParserState::Osc;
        } else if (byte == 'c') {
          clear_all();
          move_cursor(0, 0);
          current_attributes_ = {};
          parser_state_ = ParserState::Ground;
        } else {
          parser_state_ = ParserState::Ground;
        }
        break;

      case ParserState::Csi:
        if (byte >= 0x40 && byte <= 0x7e) {
          apply_csi(static_cast<char>(byte));
          csi_buffer_.clear();
          parser_state_ = ParserState::Ground;
        } else {
          csi_buffer_.push_back(static_cast<char>(byte));
        }
        break;

      case ParserState::Osc:
        if (byte == '\a') {
          parser_state_ = ParserState::Ground;
        } else if (byte == 0x1b) {
          parser_state_ = ParserState::OscEscape;
        }
        break;

      case ParserState::OscEscape:
        parser_state_ = byte == '\\' ? ParserState::Ground : ParserState::Osc;
        break;
    }
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
  screen.alternate_screen = alternate_screen_;
  screen.scrollback_line_count = scrollback_.size();
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
  return TerminalCell{' ', current_attributes_};
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
  return TerminalLineSnapshot{
      row_text(buffer, row),
      wrapped_lines[static_cast<std::size_t>(row)]};
}

std::string TerminalGrid::row_text(const std::vector<TerminalCell>& buffer, int row) const {
  std::string line;
  line.reserve(static_cast<std::size_t>(columns_));
  for (int column = 0; column < columns_; ++column) {
    line.push_back(buffer[offset(row, column)].glyph);
  }
  return line;
}

std::string TerminalGrid::row_text(const std::vector<TerminalCell>& cells) const {
  std::string line;
  line.reserve(cells.size());
  for (const auto& cell : cells) {
    line.push_back(cell.glyph);
  }
  return line;
}

void TerminalGrid::put_printable(char glyph) {
  if (pending_wrap_) {
    active_wrapped_lines()[static_cast<std::size_t>(cursor_row_)] = true;
    cursor_column_ = 0;
    line_feed();
    pending_wrap_ = false;
  }

  active_buffer()[offset(cursor_row_, cursor_column_)] = TerminalCell{glyph, current_attributes_};

  if (cursor_column_ == columns_ - 1) {
    pending_wrap_ = true;
  } else {
    ++cursor_column_;
  }
}

void TerminalGrid::carriage_return() {
  cursor_column_ = 0;
  pending_wrap_ = false;
}

void TerminalGrid::line_feed() {
  pending_wrap_ = false;
  if (cursor_row_ == rows_ - 1) {
    scroll_up();
  } else {
    ++cursor_row_;
  }
}

void TerminalGrid::backspace() {
  pending_wrap_ = false;
  if (cursor_column_ > 0) {
    --cursor_column_;
  }
}

void TerminalGrid::tab() {
  pending_wrap_ = false;
  const int next_tab = ((cursor_column_ / kTabWidth) + 1) * kTabWidth;
  cursor_column_ = std::min(columns_ - 1, next_tab);
}

void TerminalGrid::scroll_up() {
  auto& buffer = active_buffer();
  auto& wrapped_lines = active_wrapped_lines();
  if (!alternate_screen_) {
    append_scrollback_line(row_cells(buffer, 0));
  }

  if (rows_ <= 1) {
    std::fill(buffer.begin(), buffer.end(), blank_cell());
    std::fill(wrapped_lines.begin(), wrapped_lines.end(), false);
    return;
  }

  const auto columns = static_cast<std::size_t>(columns_);
  std::move(buffer.begin() + columns, buffer.end(), buffer.begin());
  std::fill(buffer.end() - columns, buffer.end(), blank_cell());
  std::move(wrapped_lines.begin() + 1, wrapped_lines.end(), wrapped_lines.begin());
  wrapped_lines.back() = false;
}

void TerminalGrid::append_scrollback_line(std::vector<TerminalCell> line) {
  if (scrollback_capacity_ == 0) {
    return;
  }

  const auto wrapped = normal_wrapped_lines_.empty() ? false : normal_wrapped_lines_.front();
  scrollback_.push_back(TerminalLineSnapshot{row_text(line), wrapped});
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
  auto& buffer = active_buffer();
  int first = 0;
  int last = columns_ - 1;
  if (mode == 0) {
    first = cursor_column_;
  } else if (mode == 1) {
    last = cursor_column_;
  }

  for (int column = first; column <= last; ++column) {
    buffer[offset(cursor_row_, column)] = blank_cell();
  }
  if (mode == 2 || last == columns_ - 1) {
    active_wrapped_lines()[static_cast<std::size_t>(cursor_row_)] = false;
  }
}

void TerminalGrid::clear_screen(int mode) {
  auto& buffer = active_buffer();
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
        buffer[offset(row, column)] = blank_cell();
      }
      wrapped_lines[static_cast<std::size_t>(row)] = false;
    }
    for (int column = 0; column <= cursor_column_; ++column) {
      buffer[offset(cursor_row_, column)] = blank_cell();
    }
    if (cursor_column_ == columns_ - 1) {
      wrapped_lines[static_cast<std::size_t>(cursor_row_)] = false;
    }
    return;
  }

  for (int column = cursor_column_; column < columns_; ++column) {
    buffer[offset(cursor_row_, column)] = blank_cell();
  }
  wrapped_lines[static_cast<std::size_t>(cursor_row_)] = false;
  for (int row = cursor_row_ + 1; row < rows_; ++row) {
    for (int column = 0; column < columns_; ++column) {
      buffer[offset(row, column)] = blank_cell();
    }
    wrapped_lines[static_cast<std::size_t>(row)] = false;
  }
}

void TerminalGrid::move_cursor(int row, int column) {
  pending_wrap_ = false;
  cursor_row_ = std::clamp(row, 0, rows_ - 1);
  cursor_column_ = std::clamp(column, 0, columns_ - 1);
}

void TerminalGrid::move_cursor_relative(int row_delta, int column_delta) {
  move_cursor(cursor_row_ + row_delta, cursor_column_ + column_delta);
}

void TerminalGrid::apply_csi(char final_byte) {
  const auto params = csi_params(csi_buffer_);

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
    case 'G':
      move_cursor(cursor_row_, param_or_default(params, 0, 1) - 1);
      break;
    case 'H':
    case 'f':
      move_cursor(param_or_default(params, 0, 1) - 1, param_or_default(params, 1, 1) - 1);
      break;
    case 'J':
      clear_screen(param_or_default(params, 0, 0));
      break;
    case 'K':
      clear_line(param_or_default(params, 0, 0));
      break;
    case 'd':
      move_cursor(param_or_default(params, 0, 1) - 1, cursor_column_);
      break;
    case 'h':
      if (private_csi_mode(csi_buffer_, 47) || private_csi_mode(csi_buffer_, 1047) ||
          private_csi_mode(csi_buffer_, 1049)) {
        set_alternate_screen(true);
      }
      break;
    case 'l':
      if (private_csi_mode(csi_buffer_, 47) || private_csi_mode(csi_buffer_, 1047) ||
          private_csi_mode(csi_buffer_, 1049)) {
        set_alternate_screen(false);
      }
      break;
    case 'm':
      apply_sgr(params);
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

  for (const int param : params) {
    if (param == 0) {
      current_attributes_ = {};
    } else if (param == 1) {
      current_attributes_.bold = true;
    } else if (param == 4) {
      current_attributes_.underline = true;
    } else if (param == 7) {
      current_attributes_.inverse = true;
    } else if (param == 22) {
      current_attributes_.bold = false;
    } else if (param == 24) {
      current_attributes_.underline = false;
    } else if (param == 27) {
      current_attributes_.inverse = false;
    } else if (param >= 30 && param <= 37) {
      current_attributes_.foreground = static_cast<std::int16_t>(param - 30);
    } else if (param == 39) {
      current_attributes_.foreground = -1;
    } else if (param >= 40 && param <= 47) {
      current_attributes_.background = static_cast<std::int16_t>(param - 40);
    } else if (param == 49) {
      current_attributes_.background = -1;
    } else if (param >= 90 && param <= 97) {
      current_attributes_.foreground = static_cast<std::int16_t>(8 + param - 90);
    } else if (param >= 100 && param <= 107) {
      current_attributes_.background = static_cast<std::int16_t>(8 + param - 100);
    }
  }
}

void TerminalGrid::set_alternate_screen(bool enabled) {
  if (alternate_screen_ == enabled) {
    return;
  }

  alternate_screen_ = enabled;
  move_cursor(0, 0);
  if (enabled) {
    clear_all();
  }
}

}  // namespace wmux
