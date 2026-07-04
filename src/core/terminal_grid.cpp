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

TerminalLine make_line(int columns, const TerminalCell& blank) {
  TerminalLine line;
  line.cells.assign(static_cast<std::size_t>(columns), blank);
  return line;
}

std::vector<TerminalLine> make_lines(int rows, int columns, const TerminalCell& blank) {
  std::vector<TerminalLine> lines;
  lines.reserve(static_cast<std::size_t>(rows));
  for (int row = 0; row < rows; ++row) {
    lines.push_back(make_line(columns, blank));
  }
  return lines;
}

std::vector<TerminalLine> resized_lines(
    const std::vector<TerminalLine>& old_lines,
    int old_columns,
    int old_rows,
    int new_columns,
    int new_rows,
    const TerminalCell& blank) {
  auto next = make_lines(new_rows, new_columns, blank);
  const int copied_rows = std::min(old_rows, new_rows);
  const int copied_columns = std::min(old_columns, new_columns);
  for (int row = 0; row < copied_rows; ++row) {
    next[static_cast<std::size_t>(row)].wrapped =
        old_lines[static_cast<std::size_t>(row)].wrapped;
    next[static_cast<std::size_t>(row)].generation =
        old_lines[static_cast<std::size_t>(row)].generation + 1;
    for (int column = 0; column < copied_columns; ++column) {
      next[static_cast<std::size_t>(row)].cells[static_cast<std::size_t>(column)] =
          old_lines[static_cast<std::size_t>(row)].cells[static_cast<std::size_t>(column)];
    }
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

bool terminal_attributes_default(const TerminalAttributes& attributes) {
  return !attributes.bold && !attributes.dim && !attributes.italic && !attributes.underline &&
         !attributes.inverse && attributes.foreground == -1 && attributes.background == -1;
}

bool cell_has_default_ascii_glyph(const TerminalCell& cell) {
  return cell.width == TerminalCellWidth::Narrow && cell.extended.empty() &&
         cell.codepoint >= U' ' && cell.codepoint < 0x7f;
}

std::string cell_glyph(const TerminalCell& cell) {
  if (!cell.extended.empty()) {
    return cell.extended;
  }
  return utf8_from_codepoint(static_cast<std::uint32_t>(cell.codepoint));
}

void append_cell_glyph(std::string& out, const TerminalCell& cell) {
  if (!cell.extended.empty()) {
    out += cell.extended;
    return;
  }
  if (cell.codepoint < 0x80) {
    out.push_back(static_cast<char>(cell.codepoint));
    return;
  }
  out += utf8_from_codepoint(static_cast<std::uint32_t>(cell.codepoint));
}

bool row_needs_rich_snapshot(const TerminalLine& line) {
  for (std::size_t column = 0; column < line.cells.size(); ++column) {
    const auto& cell = line.cells[static_cast<std::size_t>(column)];
    if (!terminal_attributes_default(cell.attributes) ||
        !cell_has_default_ascii_glyph(cell)) {
      return true;
    }
  }
  return false;
}

void apply_damage_row_range(TerminalDamage& damage, int rows) {
  if (damage.kind == DamageKind::None) {
    damage.first_row = -1;
    damage.last_row = -1;
    return;
  }
  if (damage.kind >= DamageKind::FullPane && rows > 0) {
    damage.first_row = 0;
    damage.last_row = rows - 1;
    return;
  }
  if (!damage.dirty_rows.empty()) {
    damage.first_row = damage.dirty_rows.front();
    damage.last_row = damage.dirty_rows.back();
    return;
  }
  damage.first_row = -1;
  damage.last_row = -1;
}

}  // namespace

DamageKind merge_damage(DamageKind a, DamageKind b) {
  return static_cast<int>(a) > static_cast<int>(b) ? a : b;
}

TerminalGrid::TerminalGrid()
    : normal_lines_(make_lines(kDefaultRows, kDefaultColumns, TerminalCell{})),
      alternate_lines_(make_lines(kDefaultRows, kDefaultColumns, TerminalCell{})),
      dirty_rows_(static_cast<std::size_t>(kDefaultRows), true) {
  reset_scroll_region();
}

TerminalGrid::TerminalGrid(int columns, int rows)
    : columns_(clamped_dimension(columns, kDefaultColumns)),
      rows_(clamped_dimension(rows, kDefaultRows)),
      normal_lines_(make_lines(rows_, columns_, TerminalCell{})),
      alternate_lines_(make_lines(rows_, columns_, TerminalCell{})),
      dirty_rows_(static_cast<std::size_t>(rows_), true) {
  reset_scroll_region();
}

void TerminalGrid::resize(int columns, int rows) {
  const int next_columns = clamped_dimension(columns, kDefaultColumns);
  const int next_rows = clamped_dimension(rows, kDefaultRows);
  const auto blank = blank_cell();

  normal_lines_ = resized_lines(normal_lines_, columns_, rows_, next_columns, next_rows, blank);
  alternate_lines_ =
      resized_lines(alternate_lines_, columns_, rows_, next_columns, next_rows, blank);

  columns_ = next_columns;
  rows_ = next_rows;
  repair_wide_cells();
  cursor_column_ = std::clamp(cursor_column_, 0, columns_ - 1);
  cursor_row_ = std::clamp(cursor_row_, 0, rows_ - 1);
  saved_cursor_column_ = std::clamp(saved_cursor_column_, 0, columns_ - 1);
  saved_cursor_row_ = std::clamp(saved_cursor_row_, 0, rows_ - 1);
  reset_scroll_region();
  pending_wrap_ = false;
  mark_full_pane_dirty();
}

void TerminalGrid::feed(std::string_view bytes) {
  std::size_t index = 0;
  while (index < bytes.size()) {
    if (parser_.fast_path_ready()) {
      feed_ascii_fast_path(bytes, index);
      continue;
    }

    feed_parser_byte(bytes[index]);
    ++index;
  }
}

void TerminalGrid::set_scrollback_capacity(std::size_t capacity) {
  scrollback_capacity_ = capacity;
  while (scrollback_.size() > scrollback_capacity_) {
    scrollback_.pop_front();
  }
}

int TerminalGrid::columns() const noexcept {
  return columns_;
}

int TerminalGrid::rows() const noexcept {
  return rows_;
}

CursorState TerminalGrid::cursor() const {
  return CursorState{
      cursor_column_,
      cursor_row_,
      cursor_visible_,
      cursor_style_,
      origin_mode_,
      wrap_mode_,
      bracketed_paste_mode_,
      alternate_screen_};
}

TerminalLineView TerminalGrid::line_view(int row) const {
  if (row < 0 || row >= rows_) {
    return {};
  }
  const auto& line = line_at(row);
  return TerminalLineView{
      std::span<const TerminalCell>{line.cells.data(), line.cells.size()},
      line.wrapped,
      line.generation};
}

std::uint64_t TerminalGrid::line_generation(int row) const {
  if (row < 0 || row >= rows_) {
    return 0;
  }
  return line_at(row).generation;
}

ScrollbackView TerminalGrid::scrollback_view(int start, int count) const {
  const auto total = scrollback_.size();
  const auto first = static_cast<std::size_t>(std::clamp(start, 0, static_cast<int>(total)));
  const auto requested = std::max(0, count);
  const auto available = total - first;
  return ScrollbackView{
      &scrollback_,
      first,
      std::min<std::size_t>(available, static_cast<std::size_t>(requested)),
      total,
      scrollback_capacity_};
}

TerminalDamage TerminalGrid::consume_damage() const {
  TerminalDamage damage;
  damage.dirty_rows = dirty_rows_snapshot(true);
  damage.kind = damage_snapshot(true);
  apply_damage_row_range(damage, rows_);
  scroll_events_snapshot(true);
  return damage;
}

TerminalScreenSnapshot TerminalGrid::snapshot(bool consume_dirty, bool dirty_rows_only) const {
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
  screen.dirty_rows = dirty_rows_snapshot(consume_dirty);
  screen.damage = damage_snapshot(consume_dirty);
  TerminalDamage damage_range{screen.damage, -1, -1, screen.dirty_rows};
  apply_damage_row_range(damage_range, rows_);
  screen.damage_first_row = damage_range.first_row;
  screen.damage_last_row = damage_range.last_row;
  screen.scroll_events = scroll_events_snapshot(consume_dirty);

  const auto& lines = active_lines();
  if (dirty_rows_only) {
    screen.dirty_line_snapshots.reserve(screen.dirty_rows.size());
    for (const int row : screen.dirty_rows) {
      if (row >= 0 && row < rows_) {
        screen.dirty_line_snapshots.push_back(row_snapshot(lines[static_cast<std::size_t>(row)]));
      }
    }
    return screen;
  }

  for (int row = 0; row < rows_; ++row) {
    auto line = row_snapshot(lines[static_cast<std::size_t>(row)]);
    screen.lines.push_back(line.text);
    screen.line_snapshots.push_back(std::move(line));
  }

  return screen;
}

TerminalScrollbackSnapshot TerminalGrid::scrollback_snapshot() const {
  TerminalScrollbackSnapshot snapshot;
  snapshot.capacity = scrollback_capacity_;
  snapshot.total_lines = scrollback_.size();
  snapshot.first_line_index = 0;
  snapshot.partial = false;
  snapshot.lines.reserve(scrollback_.size());
  snapshot.line_snapshots.reserve(scrollback_.size());
  for (const auto& line : scrollback_) {
    auto line_snapshot = row_snapshot(line);
    snapshot.lines.push_back(line_snapshot.text);
    snapshot.line_snapshots.push_back(std::move(line_snapshot));
  }
  return snapshot;
}

TerminalScrollbackSnapshot TerminalGrid::scrollback_snapshot_range(
    std::size_t first_line,
    std::size_t line_count) const {
  TerminalScrollbackSnapshot snapshot;
  snapshot.capacity = scrollback_capacity_;
  snapshot.total_lines = scrollback_.size();
  snapshot.partial = true;
  if (line_count == 0 || first_line >= scrollback_.size()) {
    snapshot.first_line_index = std::min(first_line, scrollback_.size());
    return snapshot;
  }

  const auto last_line = std::min(scrollback_.size(), first_line + line_count);
  snapshot.first_line_index = first_line;
  snapshot.lines.reserve(last_line - first_line);
  snapshot.line_snapshots.reserve(last_line - first_line);
  for (auto index = first_line; index < last_line; ++index) {
    const auto& line = scrollback_[index];
    auto line_snapshot = row_snapshot(line);
    snapshot.lines.push_back(line_snapshot.text);
    snapshot.line_snapshots.push_back(std::move(line_snapshot));
  }
  return snapshot;
}

std::vector<TerminalLine>& TerminalGrid::active_lines() {
  return alternate_screen_ ? alternate_lines_ : normal_lines_;
}

const std::vector<TerminalLine>& TerminalGrid::active_lines() const {
  return alternate_screen_ ? alternate_lines_ : normal_lines_;
}

TerminalLine& TerminalGrid::line_at(int row) {
  return active_lines()[static_cast<std::size_t>(row)];
}

const TerminalLine& TerminalGrid::line_at(int row) const {
  return active_lines()[static_cast<std::size_t>(row)];
}

TerminalCell& TerminalGrid::cell_at(int row, int column) {
  return line_at(row).cells[static_cast<std::size_t>(column)];
}

const TerminalCell& TerminalGrid::cell_at(int row, int column) const {
  return line_at(row).cells[static_cast<std::size_t>(column)];
}

TerminalCell TerminalGrid::blank_cell() const {
  return TerminalCell{U' ', {}, current_attributes_, TerminalCellWidth::Narrow};
}

std::vector<int> TerminalGrid::dirty_rows_snapshot(bool consume_dirty) const {
  std::vector<int> rows;
  rows.reserve(dirty_rows_.size());
  for (int row = 0; row < static_cast<int>(dirty_rows_.size()); ++row) {
    if (dirty_rows_[static_cast<std::size_t>(row)]) {
      rows.push_back(row);
    }
  }
  if (consume_dirty) {
    std::fill(dirty_rows_.begin(), dirty_rows_.end(), false);
  }
  return rows;
}

std::vector<TerminalScrollEvent> TerminalGrid::scroll_events_snapshot(bool consume_dirty) const {
  auto events = scroll_events_;
  if (consume_dirty) {
    scroll_events_.clear();
  }
  return events;
}

DamageKind TerminalGrid::damage_snapshot(bool consume_dirty) const {
  const auto damage = damage_;
  if (consume_dirty) {
    damage_ = DamageKind::None;
  }
  return damage;
}

void TerminalGrid::feed_parser_byte(char byte) {
  operation_buffer_.clear();
  parser_.feed(std::string_view{&byte, 1}, operation_buffer_);
  for (const auto& operation : operation_buffer_) {
    apply_operation(operation);
  }
}

void TerminalGrid::feed_ascii_fast_path(std::string_view bytes, std::size_t& index) {
  const std::size_t start = index;
  while (index < bytes.size()) {
    const auto byte = static_cast<unsigned char>(bytes[index]);
    if (byte < 0x20 || byte >= 0x7f) {
      break;
    }
    ++index;
  }

  if (index > start) {
    put_ascii_run(bytes.substr(start, index - start));
    return;
  }

  const auto byte = static_cast<unsigned char>(bytes[index]);
  switch (byte) {
    case '\r':
      carriage_return();
      ++index;
      return;
    case '\n':
      line_feed();
      ++index;
      return;
    case '\b':
      backspace();
      ++index;
      return;
    case '\t':
      tab();
      ++index;
      return;
    default:
      feed_parser_byte(bytes[index]);
      ++index;
      return;
  }
}

void TerminalGrid::mark_dirty_row(int row) {
  if (row < 0 || row >= rows_) {
    return;
  }
  if (dirty_rows_.size() != static_cast<std::size_t>(rows_)) {
    dirty_rows_.assign(static_cast<std::size_t>(rows_), true);
    damage_ = merge_damage(damage_, DamageKind::FullPane);
    return;
  }
  dirty_rows_[static_cast<std::size_t>(row)] = true;
  damage_ = merge_damage(damage_, DamageKind::RowRange);
}

void TerminalGrid::mark_dirty_range(int first, int last) {
  if (rows_ <= 0) {
    return;
  }
  first = std::clamp(first, 0, rows_ - 1);
  last = std::clamp(last, 0, rows_ - 1);
  if (last < first) {
    return;
  }
  if (dirty_rows_.size() != static_cast<std::size_t>(rows_)) {
    dirty_rows_.assign(static_cast<std::size_t>(rows_), true);
    damage_ = merge_damage(damage_, DamageKind::FullPane);
    return;
  }
  for (int row = first; row <= last; ++row) {
    dirty_rows_[static_cast<std::size_t>(row)] = true;
  }
  damage_ = merge_damage(damage_, DamageKind::RowRange);
}

void TerminalGrid::mark_all_dirty() {
  dirty_rows_.assign(static_cast<std::size_t>(rows_), true);
  scroll_events_.clear();
  damage_ = merge_damage(damage_, DamageKind::FullPane);
}

void TerminalGrid::mark_full_pane_dirty() {
  dirty_rows_.assign(static_cast<std::size_t>(rows_), true);
  scroll_events_.clear();
  damage_ = merge_damage(damage_, DamageKind::FullPane);
}

TerminalLineSnapshot TerminalGrid::row_snapshot(const TerminalLine& line) const {
  TerminalLineSnapshot snapshot;
  snapshot.text = row_text(line);
  snapshot.wrapped = line.wrapped;
  if (!row_needs_rich_snapshot(line)) {
    return snapshot;
  }

  snapshot.attributes.reserve(line.cells.size());
  snapshot.cells.reserve(line.cells.size());
  snapshot.cell_widths.reserve(line.cells.size());
  for (const auto& cell : line.cells) {
    snapshot.attributes.push_back(cell.attributes);
    snapshot.cells.push_back(
        cell.width == TerminalCellWidth::WideContinuation ? std::string{} : cell_glyph(cell));
    snapshot.cell_widths.push_back(cell.width);
  }
  return snapshot;
}

std::string TerminalGrid::row_text(const TerminalLine& row) const {
  std::string line;
  line.reserve(row.cells.size());
  for (const auto& cell : row.cells) {
    if (cell.width == TerminalCellWidth::WideContinuation) {
      line.push_back(' ');
    } else {
      append_cell_glyph(line, cell);
    }
  }
  return line;
}

std::string TerminalGrid::row_text(const std::vector<TerminalCell>& cells) const {
  std::string line;
  line.reserve(cells.size());
  for (const auto& cell : cells) {
    if (cell.width == TerminalCellWidth::WideContinuation) {
      line.push_back(' ');
    } else {
      append_cell_glyph(line, cell);
    }
  }
  return line;
}

void TerminalGrid::put_printable(char glyph) {
  put_ascii_run(std::string_view{&glyph, 1});
}

void TerminalGrid::put_ascii_run(std::string_view text) {
  if (text.empty()) {
    return;
  }

  while (!text.empty()) {
    if (pending_wrap_) {
      line_at(cursor_row_).wrapped = true;
      ++line_at(cursor_row_).generation;
      cursor_column_ = 0;
      line_feed();
      pending_wrap_ = false;
    }

    const auto written = put_ascii_row_segment(text);
    text.remove_prefix(written);
  }
}

std::size_t TerminalGrid::put_ascii_row_segment(std::string_view text) {
  if (text.empty() || columns_ <= 0) {
    return 0;
  }

  const int start_column = cursor_column_;
  const int available_columns = std::max(1, columns_ - start_column);
  const auto count = std::min<std::size_t>(
      text.size(),
      static_cast<std::size_t>(available_columns));
  auto& line = line_at(cursor_row_);
  const auto blank = blank_cell();

  if (start_column > 0 &&
      line.cells[static_cast<std::size_t>(start_column)].width ==
          TerminalCellWidth::WideContinuation) {
    line.cells[static_cast<std::size_t>(start_column - 1)] = blank;
  }

  const int end_column = start_column + static_cast<int>(count);
  if (end_column < columns_ &&
      line.cells[static_cast<std::size_t>(end_column)].width ==
          TerminalCellWidth::WideContinuation) {
    line.cells[static_cast<std::size_t>(end_column)] = blank;
  }

  for (std::size_t offset = 0; offset < count; ++offset) {
    line.cells[static_cast<std::size_t>(start_column) + offset] =
        TerminalCell{
            static_cast<unsigned char>(text[offset]),
            {},
            current_attributes_,
            TerminalCellWidth::Narrow};
  }

  ++line.generation;
  mark_dirty_row(cursor_row_);

  if (end_column >= columns_) {
    if (wrap_mode_) {
      pending_wrap_ = true;
    } else {
      cursor_column_ = columns_ - 1;
      pending_wrap_ = false;
    }
  } else {
    cursor_column_ = end_column;
  }

  return count;
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
    const char ascii = static_cast<char>(codepoint);
    put_ascii_run(std::string_view{&ascii, 1});
    return;
  }

  if (codepoint == kReplacementCodepoint) {
    constexpr char replacement = '?';
    put_ascii_run(std::string_view{&replacement, 1});
    return;
  }

  put_cell(glyph, width);
}

void TerminalGrid::put_cell(std::string glyph, int width) {
  width = std::clamp(width, 1, std::max(1, columns_));
  if (pending_wrap_) {
    line_at(cursor_row_).wrapped = true;
    ++line_at(cursor_row_).generation;
    cursor_column_ = 0;
    line_feed();
    pending_wrap_ = false;
  }

  if (width > 1 && cursor_column_ + width > columns_) {
    if (wrap_mode_) {
      line_at(cursor_row_).wrapped = true;
      ++line_at(cursor_row_).generation;
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

  cell_at(cursor_row_, cursor_column_) =
      TerminalCell{
          U' ',
          std::move(glyph),
          current_attributes_,
          width == 2 ? TerminalCellWidth::WideLeading : TerminalCellWidth::Narrow};
  for (int column = cursor_column_ + 1;
       column < std::min(columns_, cursor_column_ + width);
       ++column) {
    cell_at(cursor_row_, column) =
        TerminalCell{U' ', {}, current_attributes_, TerminalCellWidth::WideContinuation};
  }
  ++line_at(cursor_row_).generation;
  mark_dirty_row(cursor_row_);

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

  if (cell_at(row, column).width != TerminalCellWidth::WideContinuation) {
    return column;
  }

  for (int candidate = column - 1; candidate >= 0; --candidate) {
    const auto& cell = cell_at(row, candidate);
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

  const int leading = leading_column_for(row, column);
  const auto replacement = blank_cell();
  if (leading < 0 || leading >= columns_) {
    return;
  }

  const auto width = cell_at(row, leading).width;
  cell_at(row, leading) = replacement;
  if (width == TerminalCellWidth::WideLeading && leading + 1 < columns_) {
    cell_at(row, leading + 1) = replacement;
  }
  if (column != leading) {
    cell_at(row, column) = replacement;
  }
  ++line_at(row).generation;
  mark_dirty_row(row);
}

void TerminalGrid::reset_line(TerminalLine& line) {
  const auto replacement = blank_cell();
  if (line.cells.size() != static_cast<std::size_t>(columns_)) {
    line.cells.assign(static_cast<std::size_t>(columns_), replacement);
  } else {
    std::fill(line.cells.begin(), line.cells.end(), replacement);
  }
  line.wrapped = false;
  ++line.generation;
}

void TerminalGrid::repair_wide_cells_in_row(TerminalLine& line) const {
  const auto replacement = blank_cell();
  for (int column = 0; column < columns_; ++column) {
    auto& cell = line.cells[static_cast<std::size_t>(column)];
    if (cell.width == TerminalCellWidth::WideContinuation) {
      if (column == 0 ||
          line.cells[static_cast<std::size_t>(column - 1)].width !=
              TerminalCellWidth::WideLeading) {
        cell = replacement;
        ++line.generation;
      }
      continue;
    }

    if (cell.width == TerminalCellWidth::WideLeading) {
      if (column + 1 >= columns_ ||
          line.cells[static_cast<std::size_t>(column + 1)].width !=
              TerminalCellWidth::WideContinuation) {
        cell = replacement;
        ++line.generation;
      } else {
        ++column;
      }
    }
  }
}

void TerminalGrid::repair_wide_cells() {
  for (int row = 0; row < rows_; ++row) {
    repair_wide_cells_in_row(normal_lines_[static_cast<std::size_t>(row)]);
    repair_wide_cells_in_row(alternate_lines_[static_cast<std::size_t>(row)]);
  }
}

bool TerminalGrid::append_codepoint_to_previous_grapheme(
    std::string_view glyph,
    std::uint32_t codepoint) {
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

  auto& cell = cell_at(row, leading);
  if (cell.width == TerminalCellWidth::WideContinuation ||
      (cell.extended.empty() && cell.codepoint == U' ')) {
    return false;
  }

  auto current_glyph = cell_glyph(cell);
  if (!terminal_codepoint_extends_previous_grapheme(current_glyph, codepoint)) {
    return false;
  }

  if (cell.extended.empty()) {
    cell.extended = std::move(current_glyph);
  }
  cell.extended.append(glyph);
  ++line_at(row).generation;
  mark_dirty_row(row);
  if (terminal_grapheme_width(cell.extended) == 2 &&
      cell.width == TerminalCellWidth::Narrow &&
      leading + 1 < columns_) {
    const auto attributes = cell.attributes;
    clear_cell_at(row, leading + 1);
    cell_at(row, leading).width = TerminalCellWidth::WideLeading;
    cell_at(row, leading + 1) =
        TerminalCell{U' ', {}, attributes, TerminalCellWidth::WideContinuation};
    ++line_at(row).generation;
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
  int row = cursor_row_;
  int column = pending_wrap_ ? columns_ - 1 : cursor_column_ - 1;
  if (column < 0 && cursor_row_ > 0) {
    row = cursor_row_ - 1;
    column = columns_ - 1;
  }

  for (; row >= 0; --row, column = columns_ - 1) {
    for (; column >= 0; --column) {
      const int leading = leading_column_for(row, column);
      auto& cell = cell_at(row, leading);
      if (cell.width == TerminalCellWidth::WideContinuation) {
        continue;
      }
      if (!cell.extended.empty() || cell.codepoint != U' ') {
        if (cell.extended.empty()) {
          cell.extended = cell_glyph(cell);
        }
        cell.extended.append(glyph);
        ++line_at(row).generation;
        mark_dirty_row(row);
        if (terminal_grapheme_width(cell.extended) == 2 &&
            cell.width == TerminalCellWidth::Narrow &&
            leading + 1 < columns_) {
          const auto attributes = cell.attributes;
          clear_cell_at(row, leading + 1);
          cell_at(row, leading).width = TerminalCellWidth::WideLeading;
          cell_at(row, leading + 1) =
              TerminalCell{U' ', {}, attributes, TerminalCellWidth::WideContinuation};
          ++line_at(row).generation;
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

  count = std::min(count, bottom - top + 1);
  auto& lines = active_lines();
  for (int step = 0; step < count; ++step) {
    if (!alternate_screen_ && top == 0 && bottom == rows_ - 1) {
      append_scrollback_row(lines[0]);
    }

    if (top == bottom) {
      reset_line(lines[static_cast<std::size_t>(top)]);
      continue;
    }

    auto first = lines.begin() + static_cast<std::ptrdiff_t>(top);
    auto second = first + 1;
    auto last = lines.begin() + static_cast<std::ptrdiff_t>(bottom + 1);
    std::rotate(first, second, last);
    reset_line(lines[static_cast<std::size_t>(bottom)]);
  }
  mark_full_pane_dirty();
}

void TerminalGrid::scroll_down_region(int top, int bottom, int count) {
  if (count <= 0 || top < 0 || bottom >= rows_ || top > bottom) {
    return;
  }

  count = std::min(count, bottom - top + 1);
  auto& lines = active_lines();
  for (int step = 0; step < count; ++step) {
    if (top == bottom) {
      reset_line(lines[static_cast<std::size_t>(top)]);
      continue;
    }

    auto first = lines.begin() + static_cast<std::ptrdiff_t>(top);
    auto before_last = lines.begin() + static_cast<std::ptrdiff_t>(bottom);
    auto last = lines.begin() + static_cast<std::ptrdiff_t>(bottom + 1);
    std::rotate(first, before_last, last);
    reset_line(lines[static_cast<std::size_t>(top)]);
  }
  mark_full_pane_dirty();
}

void TerminalGrid::append_scrollback_row(const TerminalLine& line) {
  if (scrollback_capacity_ == 0) {
    return;
  }

  scrollback_.push_back(line);
  while (scrollback_.size() > scrollback_capacity_) {
    scrollback_.pop_front();
  }
}

void TerminalGrid::clear_scrollback() {
  scrollback_.clear();
}

void TerminalGrid::clear_all() {
  for (auto& line : active_lines()) {
    reset_line(line);
  }
  mark_all_dirty();
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
    line_at(cursor_row_).wrapped = false;
    ++line_at(cursor_row_).generation;
    mark_dirty_row(cursor_row_);
  }
}

void TerminalGrid::clear_screen(int mode) {
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
      line_at(row).wrapped = false;
      ++line_at(row).generation;
    }
    for (int column = 0; column <= cursor_column_; ++column) {
      clear_cell_at(cursor_row_, column);
    }
    if (cursor_column_ == columns_ - 1) {
      line_at(cursor_row_).wrapped = false;
      ++line_at(cursor_row_).generation;
    }
    mark_full_pane_dirty();
    return;
  }

  for (int column = cursor_column_; column < columns_; ++column) {
    clear_cell_at(cursor_row_, column);
  }
  line_at(cursor_row_).wrapped = false;
  ++line_at(cursor_row_).generation;
  for (int row = cursor_row_ + 1; row < rows_; ++row) {
    for (int column = 0; column < columns_; ++column) {
      clear_cell_at(row, column);
    }
    line_at(row).wrapped = false;
    ++line_at(row).generation;
  }
  mark_full_pane_dirty();
}

void TerminalGrid::erase_characters(int count) {
  count = std::max(1, count);
  const int last = std::min(columns_ - 1, cursor_column_ + count - 1);
  for (int column = cursor_column_; column <= last; ++column) {
    clear_cell_at(cursor_row_, column);
  }
  if (last == columns_ - 1) {
    line_at(cursor_row_).wrapped = false;
    ++line_at(cursor_row_).generation;
    mark_dirty_row(cursor_row_);
  }
}

void TerminalGrid::insert_characters(int count) {
  count = std::min(std::max(1, count), columns_ - cursor_column_);
  auto& line = line_at(cursor_row_);
  for (int column = columns_ - 1; column >= cursor_column_ + count; --column) {
    line.cells[static_cast<std::size_t>(column)] =
        line.cells[static_cast<std::size_t>(column - count)];
  }
  for (int column = cursor_column_; column < cursor_column_ + count; ++column) {
    line.cells[static_cast<std::size_t>(column)] = blank_cell();
  }
  repair_wide_cells_in_row(line);
  line.wrapped = false;
  ++line.generation;
  mark_dirty_row(cursor_row_);
}

void TerminalGrid::delete_characters(int count) {
  count = std::min(std::max(1, count), columns_ - cursor_column_);
  auto& line = line_at(cursor_row_);
  for (int column = cursor_column_; column + count < columns_; ++column) {
    line.cells[static_cast<std::size_t>(column)] =
        line.cells[static_cast<std::size_t>(column + count)];
  }
  for (int column = columns_ - count; column < columns_; ++column) {
    line.cells[static_cast<std::size_t>(column)] = blank_cell();
  }
  repair_wide_cells_in_row(line);
  line.wrapped = false;
  ++line.generation;
  mark_dirty_row(cursor_row_);
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
  if (cell_at(cursor_row_, cursor_column_).width == TerminalCellWidth::WideContinuation) {
    if (column_delta > 0) {
      while (cursor_column_ + 1 < columns_ &&
             cell_at(cursor_row_, cursor_column_).width ==
                 TerminalCellWidth::WideContinuation) {
        ++cursor_column_;
      }
      if (cell_at(cursor_row_, cursor_column_).width == TerminalCellWidth::WideContinuation) {
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
      mark_all_dirty();
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
    mark_all_dirty();
  }
}

void TerminalGrid::record_unknown_sequence(const TerminalVtUnknownOperation& unknown) {
  ++unknown_sequence_count_;
  mark_full_pane_dirty();
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
