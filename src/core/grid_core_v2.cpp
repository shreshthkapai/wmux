#include "terminal_engine_v2_internal.hpp"

#include "wmux/unicode_width.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <string>
#include <utility>

namespace wmux::terminal_engine_v2 {
namespace {

constexpr int kDefaultColumns = 80;
constexpr int kDefaultRows = 24;
constexpr int kTabWidth = 8;
constexpr std::uint32_t kReplacementCodepoint = 0xfffd;

int clamped_dimension(int value, int fallback) {
  return value > 0 ? value : fallback;
}

bool terminal_attributes_default(const TerminalAttributes& attributes) {
  return !attributes.bold && !attributes.dim && !attributes.italic && !attributes.underline &&
         !attributes.inverse && attributes.foreground == -1 && attributes.background == -1;
}

bool terminal_attributes_equal(
    const TerminalAttributes& left,
    const TerminalAttributes& right) {
  return left.bold == right.bold && left.dim == right.dim && left.italic == right.italic &&
         left.underline == right.underline && left.inverse == right.inverse &&
         left.foreground == right.foreground && left.background == right.background;
}

TerminalCellWidth cell_width(const Cell& cell) {
  return static_cast<TerminalCellWidth>(cell.common.width);
}

const CellExtended* cell_extension(const TerminalLine& line, const Cell& cell) {
  if (cell.extended_index == 0) {
    return nullptr;
  }
  const auto index = static_cast<std::size_t>(cell.extended_index - 1);
  if (index >= line.extended_cells.size()) {
    return nullptr;
  }
  return &line.extended_cells[index];
}

bool cell_has_default_ascii_glyph(const Cell& cell) {
  if (cell.extended_index != 0) {
    return false;
  }
  return cell_width(cell) == TerminalCellWidth::Narrow && cell.common.codepoint >= U' ' &&
         cell.common.codepoint < 0x7f;
}

bool cell_is_default_space(const Cell& cell, const StyleTable& styles) {
  return cell.extended_index == 0 && cell.common.codepoint == U' ' &&
         cell_width(cell) == TerminalCellWidth::Narrow && cell.common.flags == 0 &&
         terminal_attributes_default(styles.get(cell.common.style_id));
}

std::string cell_glyph(const TerminalLine& line, const Cell& cell) {
  if (const auto* extended = cell_extension(line, cell)) {
    return extended->grapheme;
  }
  if (cell.common.codepoint < 0x80) {
    return std::string(1, static_cast<char>(cell.common.codepoint));
  }
  return utf8_from_codepoint(cell.common.codepoint);
}

void append_cell_glyph(std::string& out, const TerminalLine& line, const Cell& cell) {
  if (const auto* extended = cell_extension(line, cell)) {
    out += extended->grapheme;
    return;
  }
  if (cell.common.codepoint < 0x80) {
    out.push_back(static_cast<char>(cell.common.codepoint));
    return;
  }
  out += utf8_from_codepoint(cell.common.codepoint);
}

bool row_needs_rich_snapshot(
    const TerminalLine& line,
    const StyleTable& styles) {
  if (!terminal_attributes_default(styles.get(line.blank_style_id))) {
    return true;
  }
  for (const auto& cell : line.cells) {
    const auto* extended = cell_extension(line, cell);
    const auto& attributes = styles.get(extended != nullptr ? extended->style_id : cell.common.style_id);
    if (!terminal_attributes_default(attributes) || !cell_has_default_ascii_glyph(cell)) {
      return true;
    }
  }
  return false;
}

}  // namespace

StyleId StyleTable::intern(const TerminalAttributes& attributes) {
  for (std::size_t index = 0; index < styles_.size(); ++index) {
    if (terminal_attributes_equal(styles_[index], attributes)) {
      return static_cast<StyleId>(index);
    }
  }
  constexpr auto max_style_count =
      static_cast<std::size_t>(std::numeric_limits<StyleId>::max()) + 1;
  if (styles_.size() >= max_style_count) {
    return 0;
  }
  styles_.push_back(attributes);
  return static_cast<StyleId>(styles_.size() - 1);
}

const TerminalAttributes& StyleTable::get(StyleId id) const {
  if (id >= styles_.size()) {
    return styles_.front();
  }
  return styles_[id];
}

void CsiParams::push(int value) {
  if (count >= params.size()) {
    overflow = true;
    return;
  }
  params[count++] = value;
}

int CsiParams::value_or(std::size_t index, int default_value) const {
  if (index >= count || params[index] == 0) {
    return default_value;
  }
  return params[index];
}

GridCore::GridCore()
    : dirty_rows_(static_cast<std::size_t>(rows_), true) {
  initialize_viewport(normal_viewport_, rows_, 0);
  initialize_viewport(alternate_viewport_, rows_, kLineAlternateScreen);
  reset_scroll_region();
  damage_last_row_ = rows_ - 1;
}

GridCore::GridCore(int columns, int rows)
    : columns_(clamped_dimension(columns, kDefaultColumns)),
      rows_(clamped_dimension(rows, kDefaultRows)),
      scroll_bottom_(rows_ - 1),
      dirty_rows_(static_cast<std::size_t>(rows_), true) {
  initialize_viewport(normal_viewport_, rows_, 0);
  initialize_viewport(alternate_viewport_, rows_, kLineAlternateScreen);
  reset_scroll_region();
  damage_last_row_ = rows_ - 1;
}

void GridCore::resize(int columns, int rows) {
  const int next_columns = clamped_dimension(columns, kDefaultColumns);
  const int next_rows = clamped_dimension(rows, kDefaultRows);
  columns_ = next_columns;
  rows_ = next_rows;
  resize_viewport(normal_viewport_, rows_);
  resize_viewport(alternate_viewport_, rows_);
  cursor_column_ = std::clamp(cursor_column_, 0, columns_ - 1);
  cursor_row_ = std::clamp(cursor_row_, 0, rows_ - 1);
  saved_cursor_.column = std::clamp(saved_cursor_.column, 0, columns_ - 1);
  saved_cursor_.row = std::clamp(saved_cursor_.row, 0, rows_ - 1);
  dirty_rows_.assign(static_cast<std::size_t>(rows_), true);
  damage_first_row_ = 0;
  damage_last_row_ = rows_ - 1;
  reset_scroll_region();
  repair_wide_cells();
  pending_wrap_ = false;
  mark_full_pane_dirty();
}

void GridCore::print_ascii_span(std::string_view text) {
  if (!using_ascii_character_set()) {
    for (const unsigned char byte : text) {
      const auto mapped = map_printable_ascii(byte);
      if (mapped < 0x80) {
        const auto ch = static_cast<char>(mapped);
        print_cell(std::string{&ch, 1}, 1);
      } else {
        const auto glyph = utf8_from_codepoint(static_cast<char32_t>(mapped));
        print_codepoint(mapped, glyph);
      }
    }
    return;
  }

  while (!text.empty()) {
    if (pending_wrap_) {
      set_line_wrapped(line_at(cursor_row_), true);
      cursor_column_ = 0;
      line_feed();
      pending_wrap_ = false;
    }
    if (insert_mode_) {
      const char ch = text.front();
      print_cell(std::string{&ch, 1}, 1);
      text.remove_prefix(1);
      continue;
    }
    const auto written = print_ascii_row_segment(text);
    text.remove_prefix(written);
  }
}

void GridCore::print_codepoint(std::uint32_t codepoint, std::string_view glyph) {
  if (codepoint == 0) {
    pending_wrap_ = false;
    return;
  }
  if (codepoint >= 0x20 && codepoint < 0x7f) {
    const char ch = static_cast<char>(codepoint);
    print_ascii_span(std::string_view{&ch, 1});
    return;
  }
  if (codepoint == kReplacementCodepoint) {
    constexpr char replacement = '?';
    print_ascii_span(std::string_view{&replacement, 1});
    return;
  }

  std::string owned_glyph;
  if (glyph.empty()) {
    owned_glyph = utf8_from_codepoint(codepoint);
    glyph = owned_glyph;
  }

  const int width = terminal_codepoint_width(codepoint);
  if (width == 0) {
    append_zero_width_to_previous_cell(glyph);
    return;
  }
  if (append_codepoint_to_previous_grapheme(glyph, codepoint)) {
    return;
  }
  print_cell(std::string{glyph}, width);
}

void GridCore::execute_control(unsigned char byte) {
  switch (byte) {
    case 0x0e:
      active_charset_slot_ = 1;
      break;
    case 0x0f:
      active_charset_slot_ = 0;
      break;
    case '\r':
      carriage_return();
      break;
    case '\n':
    case '\v':
    case '\f':
      line_feed();
      break;
    case '\b':
      backspace();
      break;
    case '\t':
      tab();
      break;
    default:
      break;
  }
}

void GridCore::dispatch_escape(char final_byte) {
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
      set_alternate_screen(false);
      clear_all();
      move_cursor(0, 0);
      reset_scroll_region();
      current_attributes_ = {};
      current_style_id_ = intern_style(current_attributes_);
      saved_cursor_ = {};
      cursor_visible_ = true;
      cursor_style_ = 0;
      origin_mode_ = false;
      wrap_mode_ = true;
      insert_mode_ = false;
      bracketed_paste_mode_ = false;
      g0_charset_ = CharacterSet::Ascii;
      g1_charset_ = CharacterSet::Ascii;
      active_charset_slot_ = 0;
      mark_all_dirty();
      break;
    case '=':
    case '>':
      break;
    default:
      record_unknown(UnknownSequenceClass::Escape, 2, final_byte);
      break;
  }
}

void GridCore::designate_character_set(int slot, char final_byte) {
  auto charset = CharacterSet::Ascii;
  if (final_byte == '0') {
    charset = CharacterSet::DecSpecialGraphics;
  } else if (final_byte != 'B') {
    record_unknown(UnknownSequenceClass::Escape, 3, final_byte);
    return;
  }

  if (slot == 1) {
    g1_charset_ = charset;
  } else {
    g0_charset_ = charset;
  }
}

void GridCore::dispatch_csi(const CsiParams& params, char final_byte) {
  switch (final_byte) {
    case 'A':
      move_cursor_relative(-params.value_or(0, 1), 0);
      break;
    case 'B':
      move_cursor_relative(params.value_or(0, 1), 0);
      break;
    case 'C':
      move_cursor_relative(0, params.value_or(0, 1));
      break;
    case 'D':
      move_cursor_relative(0, -params.value_or(0, 1));
      break;
    case 'E':
      move_cursor_relative(params.value_or(0, 1), -cursor_column_);
      break;
    case 'F':
      move_cursor_relative(-params.value_or(0, 1), -cursor_column_);
      break;
    case 'G':
      move_cursor(cursor_row_, params.value_or(0, 1) - 1);
      break;
    case 'H':
    case 'f':
      move_cursor_position(params.value_or(0, 1) - 1, params.value_or(1, 1) - 1);
      break;
    case 'I':
      for (int step = 0; step < params.value_or(0, 1); ++step) {
        tab();
      }
      break;
    case 'Z':
      backtab(params.value_or(0, 1));
      break;
    case 'J':
      clear_screen(params.value_or(0, 0));
      break;
    case 'K':
      clear_line(params.value_or(0, 0));
      break;
    case 'L':
      if (cursor_row_ >= scroll_top_ && cursor_row_ <= scroll_bottom_) {
        scroll_down_region(cursor_row_, scroll_bottom_, params.value_or(0, 1));
      }
      break;
    case 'M':
      if (cursor_row_ >= scroll_top_ && cursor_row_ <= scroll_bottom_) {
        scroll_up_region(cursor_row_, scroll_bottom_, params.value_or(0, 1));
      }
      break;
    case 'P':
      delete_characters(params.value_or(0, 1));
      break;
    case 'S':
      scroll_up_region(scroll_top_, scroll_bottom_, params.value_or(0, 1));
      break;
    case 'T':
      scroll_down_region(scroll_top_, scroll_bottom_, params.value_or(0, 1));
      break;
    case 'X':
      erase_characters(params.value_or(0, 1));
      break;
    case '@':
      insert_characters(params.value_or(0, 1));
      break;
    case 'a':
      move_cursor_relative(0, params.value_or(0, 1));
      break;
    case 'b':
      repeat_preceding_character(params.value_or(0, 1));
      break;
    case 'd':
      move_cursor(params.value_or(0, 1) - 1, cursor_column_);
      break;
    case 'e':
      move_cursor_relative(params.value_or(0, 1), 0);
      break;
    case 'h':
    case 'l':
      if (params.private_mode) {
        for (std::size_t index = 0; index < params.count; ++index) {
          apply_private_mode(params.params[index], final_byte == 'h');
        }
      } else {
        for (std::size_t index = 0; index < params.count; ++index) {
          if (params.params[index] == 4) {
            insert_mode_ = final_byte == 'h';
          }
        }
      }
      break;
    case 'm':
      apply_sgr(params);
      break;
    case 'q':
      cursor_style_ = std::clamp(params.value_or(0, 0), 0, 6);
      break;
    case 'r':
      set_scroll_region(params.value_or(0, 1) - 1, params.value_or(1, rows_) - 1);
      break;
    case 's':
      save_cursor();
      break;
    case 'u':
      restore_cursor();
      break;
    default:
      record_unknown(UnknownSequenceClass::Csi, static_cast<std::size_t>(params.count) + 3, final_byte);
      break;
  }
}

void GridCore::dispatch_osc(std::string_view payload) {
  const auto separator = payload.find(';');
  if (separator == std::string_view::npos) {
    return;
  }
  const auto command = payload.substr(0, separator);
  if (command != "0" && command != "2") {
    return;
  }
  title_ = std::string{payload.substr(separator + 1, 512)};
}

void GridCore::record_unknown(UnknownSequenceClass, std::size_t, char) {
  ++unknown_sequence_count_;
  mark_full_pane_dirty();
}

TerminalAttributes GridCore::current_attributes() const {
  return current_attributes_;
}

int GridCore::columns() const {
  return columns_;
}

int GridCore::rows() const {
  return rows_;
}

CursorState GridCore::cursor() const {
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

TerminalLineView GridCore::line_view(int row) const {
  if (row < 0 || row >= rows_) {
    return {};
  }
  const auto& line = line_at(row);
  return TerminalLineView{materialized_cells(line), line.wrapped, line.generation};
}

std::uint64_t GridCore::line_generation(int row) const {
  if (row < 0 || row >= rows_) {
    return 0;
  }
  return line_at(row).generation;
}

ScrollbackView GridCore::scrollback_view(int start, int count) const {
  const auto total = scrollback_.size();
  const auto safe_start = static_cast<std::size_t>(std::clamp(start, 0, static_cast<int>(total)));
  const auto safe_count = static_cast<std::size_t>(std::max(0, count));
  const auto view_count = std::min(safe_count, total - safe_start);
  scrollback_view_cache_.clear();
  for (std::size_t index = 0; index < view_count; ++index) {
    const auto& source = line_by_id(scrollback_[safe_start + index]);
    wmux::TerminalLine materialized;
    materialized.wrapped = source.wrapped;
    materialized.generation = source.generation;
    const auto materialized_size = static_cast<std::size_t>(
        std::max<int>(0, source.capacity == 0 ? columns_ : source.capacity));
    materialized.cells.reserve(materialized_size);
    for (std::size_t column = 0; column < materialized_size; ++column) {
      materialized.cells.push_back(
          materialized_cell(source, cell_at_or_blank(source, static_cast<int>(column))));
    }
    scrollback_view_cache_.push_back(std::move(materialized));
  }
  return ScrollbackView{
      &scrollback_view_cache_,
      0,
      scrollback_view_cache_.size(),
      total,
      scrollback_capacity_};
}

TerminalDamage GridCore::consume_damage() {
  return damage_snapshot_with_rows(true);
}

void GridCore::set_scrollback_capacity(std::size_t capacity) {
  scrollback_capacity_ = capacity;
  while (scrollback_.size() > scrollback_capacity_) {
    release_line(scrollback_.front());
    scrollback_.pop_front();
  }
}

TerminalScreenSnapshot GridCore::snapshot(bool consume_dirty, bool dirty_rows_only) const {
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
  screen.damage = damage_snapshot(consume_dirty);
  screen.dirty_rows = dirty_rows_snapshot(consume_dirty);
  if (screen.damage == DamageKind::RowRange && !screen.dirty_rows.empty()) {
    screen.damage_first_row = screen.dirty_rows.front();
    screen.damage_last_row = screen.dirty_rows.back();
  } else if (screen.damage >= DamageKind::FullPane && rows_ > 0) {
    screen.damage_first_row = 0;
    screen.damage_last_row = rows_ - 1;
  }

  const auto& viewport = active_viewport();
  if (!dirty_rows_only) {
    screen.lines.reserve(viewport.size());
    screen.line_snapshots.reserve(viewport.size());
    for (const auto id : viewport) {
      const auto& line = line_by_id(id);
      screen.lines.push_back(row_text(line));
      screen.line_snapshots.push_back(row_snapshot(line));
    }
    return screen;
  }

  screen.dirty_line_snapshots.reserve(screen.dirty_rows.size());
  for (const int row : screen.dirty_rows) {
    if (row >= 0 && row < rows_) {
      screen.dirty_line_snapshots.push_back(
          row_snapshot(line_by_id(viewport[static_cast<std::size_t>(row)])));
    }
  }
  return screen;
}

TerminalScrollbackSnapshot GridCore::scrollback_snapshot() const {
  return scrollback_snapshot_range(0, scrollback_.size());
}

TerminalScrollbackSnapshot GridCore::scrollback_snapshot_range(
    std::size_t first_line,
    std::size_t line_count) const {
  TerminalScrollbackSnapshot snapshot;
  snapshot.capacity = scrollback_capacity_;
  snapshot.total_lines = scrollback_.size();
  if (line_count == 0 || first_line >= scrollback_.size()) {
    snapshot.first_line_index = std::min(first_line, scrollback_.size());
    return snapshot;
  }
  const auto last_line = std::min(scrollback_.size(), first_line + line_count);
  snapshot.first_line_index = first_line;
  snapshot.partial = first_line != 0 || last_line != scrollback_.size();
  snapshot.lines.reserve(last_line - first_line);
  snapshot.line_snapshots.reserve(last_line - first_line);
  for (std::size_t index = first_line; index < last_line; ++index) {
    const auto& line = line_by_id(scrollback_[index]);
    snapshot.lines.push_back(row_text(line));
    snapshot.line_snapshots.push_back(row_snapshot(line));
  }
  return snapshot;
}

std::vector<LineId>& GridCore::active_viewport() {
  return alternate_screen_ ? alternate_viewport_ : normal_viewport_;
}

const std::vector<LineId>& GridCore::active_viewport() const {
  return alternate_screen_ ? alternate_viewport_ : normal_viewport_;
}

TerminalLine& GridCore::line_at(int row) {
  const auto& viewport = active_viewport();
  return line_by_id(viewport[static_cast<std::size_t>(std::clamp(row, 0, rows_ - 1))]);
}

const TerminalLine& GridCore::line_at(int row) const {
  const auto& viewport = active_viewport();
  return line_by_id(viewport[static_cast<std::size_t>(std::clamp(row, 0, rows_ - 1))]);
}

TerminalLine& GridCore::line_by_id(LineId id) {
  return line_pool_[static_cast<std::size_t>(id)];
}

const TerminalLine& GridCore::line_by_id(LineId id) const {
  return line_pool_[static_cast<std::size_t>(id)];
}

Cell& GridCore::compact_cell_at(int row, int column) {
  return mutable_cell_at(line_at(row), std::clamp(column, 0, columns_ - 1));
}

Cell GridCore::compact_cell_at(int row, int column) const {
  return cell_at_or_blank(line_at(row), std::clamp(column, 0, columns_ - 1));
}

Cell& GridCore::mutable_cell_at(TerminalLine& line, int column) {
  column = std::clamp(column, 0, std::max(0, columns_ - 1));
  const auto required = static_cast<std::size_t>(column + 1);
  if (line.cells.size() < required) {
    line.cells.resize(required, line_blank_cell(line));
  }
  line.used = static_cast<std::uint16_t>(
      std::min<std::size_t>(std::max<std::size_t>(line.used, required),
                            std::numeric_limits<std::uint16_t>::max()));
  return line.cells[static_cast<std::size_t>(column)];
}

Cell GridCore::cell_at_or_blank(const TerminalLine& line, int column) const {
  column = std::clamp(column, 0, std::max(0, columns_ - 1));
  if (column < static_cast<int>(line.used) &&
      static_cast<std::size_t>(column) < line.cells.size()) {
    return line.cells[static_cast<std::size_t>(column)];
  }
  return line_blank_cell(line);
}

Cell GridCore::blank_cell() const {
  return blank_cell(current_style_id_);
}

Cell GridCore::blank_cell(StyleId style_id) const {
  return Cell{CellCommon{U' ', style_id, 0, static_cast<std::uint8_t>(TerminalCellWidth::Narrow)}, 0};
}

Cell GridCore::line_blank_cell(const TerminalLine& line) const {
  return blank_cell(line.blank_style_id);
}

Cell GridCore::common_cell(std::uint32_t codepoint, TerminalCellWidth width) const {
  return Cell{
      CellCommon{
          codepoint,
          current_style_id_,
          0,
          static_cast<std::uint8_t>(width)},
      0};
}

Cell GridCore::extended_cell(TerminalLine& line, std::string glyph, int width) const {
  line.extended_cells.push_back(CellExtended{
      std::move(glyph),
      current_style_id_,
      static_cast<std::uint8_t>(
          width == 2 ? TerminalCellWidth::WideLeading : TerminalCellWidth::Narrow)});
  return Cell{
      CellCommon{
          U' ',
          current_style_id_,
          0,
          static_cast<std::uint8_t>(
              width == 2 ? TerminalCellWidth::WideLeading : TerminalCellWidth::Narrow)},
      static_cast<std::uint32_t>(line.extended_cells.size())};
}

bool GridCore::is_blank_cell_for_line(const TerminalLine& line, const Cell& cell) const {
  return cell.extended_index == 0 && cell.common.codepoint == U' ' &&
         cell_width(cell) == TerminalCellWidth::Narrow && cell.common.flags == 0 &&
         cell.common.style_id == line.blank_style_id;
}

void GridCore::trim_trailing_blanks(TerminalLine& line) {
  auto used = std::min<std::size_t>(line.used, line.cells.size());
  while (used > 0 && is_blank_cell_for_line(line, line.cells[used - 1])) {
    --used;
  }
  line.cells.resize(used);
  line.used = static_cast<std::uint16_t>(
      std::min<std::size_t>(used, std::numeric_limits<std::uint16_t>::max()));
  if (line.used == 0) {
    line.extended_cells.clear();
  }
}

TerminalCell GridCore::materialized_cell(const TerminalLine& line, const Cell& cell) const {
  if (const auto* extended = cell_extension(line, cell)) {
    return TerminalCell{
        U' ',
        extended->grapheme,
        style_at(extended->style_id),
        static_cast<TerminalCellWidth>(extended->width)};
  }
  return TerminalCell{
      static_cast<char32_t>(cell.common.codepoint),
      {},
      style_at(cell.common.style_id),
      cell_width(cell)};
}

std::span<const TerminalCell> GridCore::materialized_cells(const TerminalLine& line) const {
  const auto materialized_size = static_cast<std::size_t>(
      std::max<int>(0, line.capacity == 0 ? columns_ : line.capacity));
  if (line.materialized_generation == line.generation &&
      line.materialized_cells.size() == materialized_size) {
    return std::span<const TerminalCell>{line.materialized_cells.data(), line.materialized_cells.size()};
  }
  line.materialized_cells.clear();
  line.materialized_cells.reserve(materialized_size);
  for (std::size_t column = 0; column < materialized_size; ++column) {
    line.materialized_cells.push_back(
        materialized_cell(line, cell_at_or_blank(line, static_cast<int>(column))));
  }
  line.materialized_generation = line.generation;
  return std::span<const TerminalCell>{line.materialized_cells.data(), line.materialized_cells.size()};
}

StyleId GridCore::intern_style(const TerminalAttributes& attributes) {
  return style_table_.intern(attributes);
}

const TerminalAttributes& GridCore::style_at(StyleId style_id) const {
  return style_table_.get(style_id);
}

TerminalLine GridCore::make_line() const {
  TerminalLine line;
  line.capacity = static_cast<std::uint16_t>(
      std::min<int>(columns_, std::numeric_limits<std::uint16_t>::max()));
  line.blank_style_id = current_style_id_;
  return line;
}

void GridCore::reset_line(TerminalLine& line) {
  const auto screen_flags = line.flags & (kLineHistory | kLineAlternateScreen);
  line.cells.clear();
  line.extended_cells.clear();
  line.used = 0;
  line.capacity = static_cast<std::uint16_t>(
      std::min<int>(columns_, std::numeric_limits<std::uint16_t>::max()));
  line.flags = screen_flags;
  line.blank_style_id = current_style_id_;
  line.wrapped = false;
  ++line.generation;
}

void GridCore::set_line_wrapped(TerminalLine& line, bool wrapped) {
  line.wrapped = wrapped;
  if (wrapped) {
    line.flags |= kLineWrapped;
  } else {
    line.flags &= ~kLineWrapped;
  }
  ++line.generation;
}

void GridCore::set_line_screen_flag(TerminalLine& line, std::uint32_t flag) {
  line.flags &= ~(kLineHistory | kLineAlternateScreen);
  line.flags |= flag;
}

LineId GridCore::allocate_line() {
  if (!free_line_ids_.empty()) {
    const auto id = free_line_ids_.back();
    free_line_ids_.pop_back();
    reset_line(line_by_id(id));
    return id;
  }
  line_pool_.push_back(make_line());
  return static_cast<LineId>(line_pool_.size() - 1);
}

void GridCore::release_line(LineId id) {
  if (static_cast<std::size_t>(id) >= line_pool_.size()) {
    return;
  }
  reset_line(line_by_id(id));
  line_by_id(id).flags = 0;
  free_line_ids_.push_back(id);
}

void GridCore::initialize_viewport(
    std::vector<LineId>& viewport,
    int row_count,
    std::uint32_t line_flags) {
  viewport.clear();
  viewport.reserve(static_cast<std::size_t>(std::max(0, row_count)));
  for (int row = 0; row < row_count; ++row) {
    const auto id = allocate_line();
    set_line_screen_flag(line_by_id(id), line_flags);
    viewport.push_back(id);
  }
}

void GridCore::resize_viewport(std::vector<LineId>& viewport, int row_count) {
  const auto preserved_flags =
      viewport.empty() ? 0 : (line_by_id(viewport.front()).flags & kLineAlternateScreen);
  while (viewport.size() > static_cast<std::size_t>(std::max(0, row_count))) {
    release_line(viewport.back());
    viewport.pop_back();
  }
  while (viewport.size() < static_cast<std::size_t>(std::max(0, row_count))) {
    const auto id = allocate_line();
    set_line_screen_flag(line_by_id(id), preserved_flags);
    viewport.push_back(id);
  }
  for (const auto id : viewport) {
    auto& line = line_by_id(id);
    set_line_screen_flag(line, preserved_flags);
    line.capacity = static_cast<std::uint16_t>(
        std::min<int>(columns_, std::numeric_limits<std::uint16_t>::max()));
    if (line.used > line.capacity) {
      line.cells.resize(line.capacity);
      line.used = line.capacity;
    }
    trim_trailing_blanks(line);
    ++line.generation;
  }
}

void GridCore::set_viewport_line_flags(std::vector<LineId>& viewport, std::uint32_t line_flags) {
  for (const auto id : viewport) {
    set_line_screen_flag(line_by_id(id), line_flags);
  }
}

void GridCore::mark_dirty_row(int row) {
  if (row < 0 || row >= rows_) {
    return;
  }
  if (dirty_rows_.size() != static_cast<std::size_t>(rows_)) {
    dirty_rows_.assign(static_cast<std::size_t>(rows_), false);
  }
  dirty_rows_[static_cast<std::size_t>(row)] = true;
  line_at(row).flags |= kLineDirty;
  update_damage_range(row, row);
  damage_ = merge_damage(damage_, DamageKind::RowRange);
}

void GridCore::mark_dirty_range(int first, int last) {
  if (rows_ <= 0) {
    return;
  }
  first = std::clamp(first, 0, rows_ - 1);
  last = std::clamp(last, 0, rows_ - 1);
  if (last < first) {
    return;
  }
  if (dirty_rows_.size() != static_cast<std::size_t>(rows_)) {
    dirty_rows_.assign(static_cast<std::size_t>(rows_), false);
  }
  for (int row = first; row <= last; ++row) {
    dirty_rows_[static_cast<std::size_t>(row)] = true;
    line_at(row).flags |= kLineDirty;
  }
  update_damage_range(first, last);
  damage_ = merge_damage(damage_, DamageKind::RowRange);
}

void GridCore::mark_all_dirty() {
  dirty_rows_.assign(static_cast<std::size_t>(rows_), true);
  for (const auto id : active_viewport()) {
    line_by_id(id).flags |= kLineDirty;
  }
  damage_first_row_ = rows_ > 0 ? 0 : -1;
  damage_last_row_ = rows_ > 0 ? rows_ - 1 : -1;
  damage_ = merge_damage(damage_, DamageKind::FullPane);
  scroll_damage_.reset();
}

void GridCore::mark_full_pane_dirty() {
  std::fill(dirty_rows_.begin(), dirty_rows_.end(), true);
  for (const auto id : active_viewport()) {
    line_by_id(id).flags |= kLineDirty;
  }
  damage_first_row_ = rows_ > 0 ? 0 : -1;
  damage_last_row_ = rows_ > 0 ? rows_ - 1 : -1;
  damage_ = merge_damage(damage_, DamageKind::FullPane);
  scroll_damage_.reset();
}

void GridCore::mark_scroll_damage(
    int top,
    int bottom,
    int count,
    TerminalScrollDirection direction) {
  if (count <= 0 || top < 0 || bottom < top || bottom >= rows_) {
    return;
  }

  const int height = bottom - top + 1;
  if (count >= height) {
    mark_full_pane_dirty();
    return;
  }

  if (scroll_damage_ && scroll_damage_->top_row == top &&
      scroll_damage_->bottom_row == bottom && scroll_damage_->direction == direction &&
      damage_ < DamageKind::FullPane) {
    scroll_damage_->count += count;
    if (scroll_damage_->count >= height) {
      mark_full_pane_dirty();
    }
  } else if (damage_ < DamageKind::FullPane) {
    scroll_damage_ = TerminalScrollDamage{top, bottom, count, direction};
  }

  if (damage_ < DamageKind::FullPane) {
    damage_ = merge_damage(damage_, DamageKind::RowRange);
  }
}

void GridCore::update_damage_range(int first, int last) {
  if (first < 0 || last < first || rows_ <= 0) {
    return;
  }
  first = std::clamp(first, 0, rows_ - 1);
  last = std::clamp(last, 0, rows_ - 1);
  if (damage_first_row_ < 0 || damage_last_row_ < 0) {
    damage_first_row_ = first;
    damage_last_row_ = last;
    return;
  }
  damage_first_row_ = std::min(damage_first_row_, first);
  damage_last_row_ = std::max(damage_last_row_, last);
}

void GridCore::reset_damage_range() const {
  damage_first_row_ = -1;
  damage_last_row_ = -1;
}

DamageKind GridCore::damage_snapshot(bool consume_dirty) const {
  const auto damage = damage_;
  if (consume_dirty) {
    damage_ = DamageKind::None;
    scroll_damage_.reset();
    reset_damage_range();
  }
  return damage;
}

std::vector<int> GridCore::dirty_rows_snapshot(bool consume_dirty) const {
  std::vector<int> rows;
  rows.reserve(dirty_rows_.size());
  for (std::size_t index = 0; index < dirty_rows_.size(); ++index) {
    if (dirty_rows_[index]) {
      rows.push_back(static_cast<int>(index));
    }
  }
  if (consume_dirty) {
    std::fill(dirty_rows_.begin(), dirty_rows_.end(), false);
  }
  return rows;
}

TerminalDamage GridCore::damage_snapshot_with_rows(bool consume_dirty) const {
  TerminalDamage damage;
  damage.kind = damage_;
  damage.first_row = damage_first_row_;
  damage.last_row = damage_last_row_;
  damage.scroll = scroll_damage_;
  damage.dirty_rows = dirty_rows_snapshot(consume_dirty);
  if (damage.kind == DamageKind::RowRange && (damage.first_row < 0 || damage.last_row < 0) &&
      !damage.dirty_rows.empty()) {
    damage.first_row = damage.dirty_rows.front();
    damage.last_row = damage.dirty_rows.back();
  } else if (damage.kind >= DamageKind::FullPane && rows_ > 0) {
    damage.first_row = 0;
    damage.last_row = rows_ - 1;
  } else if (damage.kind == DamageKind::None) {
    damage.first_row = -1;
    damage.last_row = -1;
  }
  if (consume_dirty) {
    damage_ = DamageKind::None;
    scroll_damage_.reset();
    reset_damage_range();
  }
  return damage;
}

TerminalLineSnapshot GridCore::row_snapshot(const TerminalLine& line) const {
  TerminalLineSnapshot snapshot;
  snapshot.text = row_text(line);
  snapshot.wrapped = line.wrapped;
  if (!row_needs_rich_snapshot(line, style_table_)) {
    return snapshot;
  }
  const int width = std::max<int>(0, line.capacity == 0 ? columns_ : line.capacity);
  snapshot.attributes.reserve(static_cast<std::size_t>(width));
  snapshot.cells.reserve(static_cast<std::size_t>(width));
  snapshot.cell_widths.reserve(static_cast<std::size_t>(width));
  for (int column = 0; column < width; ++column) {
    const auto cell = cell_at_or_blank(line, column);
    const auto materialized = materialized_cell(line, cell);
    snapshot.attributes.push_back(materialized.attributes);
    snapshot.cells.push_back(
        materialized.width == TerminalCellWidth::WideContinuation
            ? std::string{}
            : cell_glyph(line, cell));
    snapshot.cell_widths.push_back(materialized.width);
  }
  return snapshot;
}

std::string GridCore::row_text(const TerminalLine& line) const {
  std::string text;
  const int width = std::max<int>(0, line.capacity == 0 ? columns_ : line.capacity);
  text.reserve(static_cast<std::size_t>(width));
  for (int column = 0; column < width; ++column) {
    const auto cell = cell_at_or_blank(line, column);
    if (cell_width(cell) == TerminalCellWidth::WideContinuation) {
      text.push_back(' ');
      continue;
    }
    append_cell_glyph(text, line, cell);
  }
  return text;
}

std::size_t GridCore::print_ascii_row_segment(std::string_view text) {
  if (text.empty() || columns_ <= 0) {
    return 0;
  }
  const int start_column = cursor_column_;
  const int available_columns = std::max(1, columns_ - start_column);
  const auto count = std::min<std::size_t>(text.size(), static_cast<std::size_t>(available_columns));
  auto& line = line_at(cursor_row_);
  const auto blank = line_blank_cell(line);

  if (start_column > 0 &&
      cell_width(cell_at_or_blank(line, start_column)) ==
          TerminalCellWidth::WideContinuation) {
    mutable_cell_at(line, start_column - 1) = blank;
  }
  const int end_column = start_column + static_cast<int>(count);
  if (end_column < columns_ &&
      cell_width(cell_at_or_blank(line, end_column)) ==
          TerminalCellWidth::WideContinuation) {
    mutable_cell_at(line, end_column) = blank;
  }

  if (line.cells.size() < static_cast<std::size_t>(end_column)) {
    line.cells.resize(static_cast<std::size_t>(end_column), line_blank_cell(line));
  }
  line.used = static_cast<std::uint16_t>(
      std::min<std::size_t>(std::max<std::size_t>(line.used, static_cast<std::size_t>(end_column)),
                            std::numeric_limits<std::uint16_t>::max()));
  for (std::size_t offset = 0; offset < count; ++offset) {
    line.cells[static_cast<std::size_t>(start_column) + offset] =
        common_cell(static_cast<unsigned char>(text[offset]), TerminalCellWidth::Narrow);
  }
  trim_trailing_blanks(line);
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

void GridCore::print_cell(std::string glyph, int width) {
  width = std::clamp(width, 1, std::max(1, columns_));
  if (pending_wrap_) {
    set_line_wrapped(line_at(cursor_row_), true);
    cursor_column_ = 0;
    line_feed();
    pending_wrap_ = false;
  }
  if (width > 1 && cursor_column_ + width > columns_) {
    if (wrap_mode_) {
      set_line_wrapped(line_at(cursor_row_), true);
      cursor_column_ = 0;
      line_feed();
      pending_wrap_ = false;
    } else {
      width = 1;
    }
  }
  if (insert_mode_) {
    insert_characters(width);
  }
  clear_cell_at(cursor_row_, cursor_column_);
  if (width == 2 && cursor_column_ + 1 < columns_) {
    clear_cell_at(cursor_row_, cursor_column_ + 1);
  }
  auto& line = line_at(cursor_row_);
  mutable_cell_at(line, cursor_column_) =
      extended_cell(line, std::move(glyph), width);
  for (int column = cursor_column_ + 1; column < std::min(columns_, cursor_column_ + width); ++column) {
    compact_cell_at(cursor_row_, column) =
        common_cell(U' ', TerminalCellWidth::WideContinuation);
  }
  trim_trailing_blanks(line);
  ++line_at(cursor_row_).generation;
  mark_dirty_row(cursor_row_);
  if (cursor_column_ + width >= columns_) {
    pending_wrap_ = wrap_mode_;
    if (!wrap_mode_) {
      cursor_column_ = columns_ - 1;
    }
  } else {
    cursor_column_ += width;
  }
}

int GridCore::leading_column_for(int row, int column) const {
  if (row < 0 || row >= rows_ || column <= 0 || column >= columns_) {
    return std::clamp(column, 0, std::max(0, columns_ - 1));
  }
  if (cell_width(compact_cell_at(row, column)) != TerminalCellWidth::WideContinuation) {
    return column;
  }
  for (int candidate = column - 1; candidate >= 0; --candidate) {
    if (cell_width(compact_cell_at(row, candidate)) != TerminalCellWidth::WideContinuation) {
      return candidate;
    }
  }
  return column;
}

void GridCore::clear_cell_at(int row, int column) {
  if (row < 0 || row >= rows_ || column < 0 || column >= columns_) {
    return;
  }
  const int leading = leading_column_for(row, column);
  const auto replacement = blank_cell();
  const auto width = cell_width(compact_cell_at(row, leading));
  compact_cell_at(row, leading) = replacement;
  if (width == TerminalCellWidth::WideLeading && leading + 1 < columns_) {
    compact_cell_at(row, leading + 1) = replacement;
  }
  if (column != leading) {
    compact_cell_at(row, column) = replacement;
  }
  ++line_at(row).generation;
  mark_dirty_row(row);
}

void GridCore::repair_wide_cells_in_row(TerminalLine& line) {
  const auto replacement = line_blank_cell(line);
  bool changed = false;
  for (int column = 0; column < static_cast<int>(line.used); ++column) {
    auto cell = cell_at_or_blank(line, column);
    if (cell_width(cell) == TerminalCellWidth::WideContinuation) {
      if (column == 0 ||
          cell_width(cell_at_or_blank(line, column - 1)) !=
              TerminalCellWidth::WideLeading) {
        mutable_cell_at(line, column) = replacement;
        changed = true;
      }
      continue;
    }
    if (cell_width(cell) == TerminalCellWidth::WideLeading) {
      if (column + 1 >= columns_ ||
          cell_width(cell_at_or_blank(line, column + 1)) !=
              TerminalCellWidth::WideContinuation) {
        mutable_cell_at(line, column) = replacement;
        changed = true;
      } else {
        ++column;
      }
    }
  }
  if (changed) {
    trim_trailing_blanks(line);
    ++line.generation;
  }
}

void GridCore::repair_wide_cells() {
  for (const auto id : normal_viewport_) {
    repair_wide_cells_in_row(line_by_id(id));
  }
  for (const auto id : alternate_viewport_) {
    repair_wide_cells_in_row(line_by_id(id));
  }
}

bool GridCore::append_codepoint_to_previous_grapheme(std::string_view glyph, std::uint32_t codepoint) {
  int row = cursor_row_;
  int column = pending_wrap_ ? columns_ - 1 : cursor_column_ - 1;
  if (column < 0 && row > 0) {
    --row;
    column = columns_ - 1;
  }
  if (row < 0 || column < 0) {
    return false;
  }
  const int leading = leading_column_for(row, column);
  auto& line = line_at(row);
  auto cell = cell_at_or_blank(line, leading);
  if (cell_width(cell) == TerminalCellWidth::WideContinuation ||
      (cell.extended_index == 0 && cell.common.codepoint == U' ')) {
    return false;
  }
  std::string existing_glyph;
  if (const auto* existing = cell_extension(line, cell)) {
    existing_glyph = existing->grapheme;
  } else {
    existing_glyph = utf8_from_codepoint(cell.common.codepoint);
  }
  const bool joins_previous =
      is_terminal_zero_width_codepoint(codepoint) ||
      terminal_codepoint_extends_previous_grapheme(existing_glyph, codepoint);
  if (!joins_previous) {
    return false;
  }
  existing_glyph.append(glyph);
  mutable_cell_at(line, leading) =
      extended_cell(line, std::move(existing_glyph),
                    cell_width(cell) == TerminalCellWidth::WideLeading ? 2 : 1);
  trim_trailing_blanks(line);
  ++line_at(row).generation;
  mark_dirty_row(row);
  return true;
}

void GridCore::append_zero_width_to_previous_cell(std::string_view glyph) {
  if (!append_codepoint_to_previous_grapheme(glyph, 0x0301)) {
    print_cell(std::string{glyph}, 1);
  }
}

void GridCore::carriage_return() {
  cursor_column_ = 0;
  pending_wrap_ = false;
}

void GridCore::line_feed() {
  pending_wrap_ = false;
  if (cursor_row_ == scroll_bottom_) {
    scroll_up_region(scroll_top_, scroll_bottom_, 1);
  } else {
    cursor_row_ = std::min(rows_ - 1, cursor_row_ + 1);
  }
}

void GridCore::backspace() {
  pending_wrap_ = false;
  if (cursor_column_ > 0) {
    --cursor_column_;
    cursor_column_ = leading_column_for(cursor_row_, cursor_column_);
  }
}

void GridCore::tab() {
  pending_wrap_ = false;
  const int next_tab = ((cursor_column_ / kTabWidth) + 1) * kTabWidth;
  cursor_column_ = std::min(columns_ - 1, next_tab);
}

void GridCore::backtab(int count) {
  for (int step = 0; step < std::max(1, count); ++step) {
    if (cursor_column_ == 0) {
      return;
    }
    cursor_column_ = std::max(0, ((cursor_column_ - 1) / kTabWidth) * kTabWidth);
  }
  pending_wrap_ = false;
}

void GridCore::scroll_up_region(int top, int bottom, int count) {
  if (count <= 0 || top < 0 || bottom >= rows_ || top > bottom) {
    return;
  }
  count = std::min(count, bottom - top + 1);
  const bool can_report_scroll_damage = !alternate_screen_ && top == 0 && bottom == rows_ - 1;
  const auto active_screen_flag = alternate_screen_ ? kLineAlternateScreen : 0u;
  auto& viewport = active_viewport();
  for (int step = 0; step < count; ++step) {
    const auto first_index = static_cast<std::size_t>(top);
    const auto last_index = static_cast<std::size_t>(bottom);
    if (!alternate_screen_ && top == 0 && bottom == rows_ - 1) {
      const auto outgoing = viewport.front();
      viewport.erase(viewport.begin());
      append_scrollback_line(outgoing);
      const auto fresh = allocate_line();
      set_line_screen_flag(line_by_id(fresh), 0);
      viewport.push_back(fresh);
      continue;
    }
    if (top == bottom) {
      reset_line(line_by_id(viewport[first_index]));
      set_line_screen_flag(line_by_id(viewport[first_index]), active_screen_flag);
      continue;
    }
    const auto outgoing = viewport[first_index];
    std::move(
        viewport.begin() + static_cast<std::ptrdiff_t>(top + 1),
        viewport.begin() + static_cast<std::ptrdiff_t>(bottom + 1),
        viewport.begin() + static_cast<std::ptrdiff_t>(top));
    viewport[last_index] = outgoing;
    reset_line(line_by_id(outgoing));
    set_line_screen_flag(line_by_id(outgoing), active_screen_flag);
  }
  if (can_report_scroll_damage) {
    mark_scroll_damage(top, bottom, count, TerminalScrollDirection::Up);
    mark_dirty_range(bottom - count + 1, bottom);
  } else {
    mark_full_pane_dirty();
  }
}

void GridCore::scroll_down_region(int top, int bottom, int count) {
  if (count <= 0 || top < 0 || bottom >= rows_ || top > bottom) {
    return;
  }
  count = std::min(count, bottom - top + 1);
  const bool can_report_scroll_damage = !alternate_screen_ && top == 0 && bottom == rows_ - 1;
  const auto active_screen_flag = alternate_screen_ ? kLineAlternateScreen : 0u;
  auto& viewport = active_viewport();
  for (int step = 0; step < count; ++step) {
    const auto first_index = static_cast<std::size_t>(top);
    const auto last_index = static_cast<std::size_t>(bottom);
    if (top == bottom) {
      reset_line(line_by_id(viewport[first_index]));
      set_line_screen_flag(line_by_id(viewport[first_index]), active_screen_flag);
      continue;
    }
    const auto outgoing = viewport[last_index];
    std::move_backward(
        viewport.begin() + static_cast<std::ptrdiff_t>(top),
        viewport.begin() + static_cast<std::ptrdiff_t>(bottom),
        viewport.begin() + static_cast<std::ptrdiff_t>(bottom + 1));
    viewport[first_index] = outgoing;
    reset_line(line_by_id(outgoing));
    set_line_screen_flag(line_by_id(outgoing), active_screen_flag);
  }
  if (can_report_scroll_damage) {
    mark_scroll_damage(top, bottom, count, TerminalScrollDirection::Down);
    mark_dirty_range(top, top + count - 1);
  } else {
    mark_full_pane_dirty();
  }
}

void GridCore::append_scrollback_line(LineId id) {
  if (scrollback_capacity_ == 0) {
    release_line(id);
    return;
  }
  set_line_screen_flag(line_by_id(id), kLineHistory);
  scrollback_.push_back(id);
  while (scrollback_.size() > scrollback_capacity_) {
    release_line(scrollback_.front());
    scrollback_.pop_front();
  }
}

void GridCore::clear_scrollback() {
  for (const auto id : scrollback_) {
    release_line(id);
  }
  scrollback_.clear();
}

void GridCore::clear_all() {
  const auto active_screen_flag = alternate_screen_ ? kLineAlternateScreen : 0u;
  for (const auto id : active_viewport()) {
    reset_line(line_by_id(id));
    set_line_screen_flag(line_by_id(id), active_screen_flag);
  }
  mark_all_dirty();
}

void GridCore::clear_line(int mode) {
  int first = 0;
  int last = columns_ - 1;
  if (mode == 0) {
    first = cursor_column_;
  } else if (mode == 1) {
    last = cursor_column_;
  }
  auto& line = line_at(cursor_row_);
  if (first == 0 && last == columns_ - 1) {
    reset_line(line);
  } else if (current_style_id_ == line.blank_style_id &&
             last >= static_cast<int>(line.used) - 1) {
    const auto first_to_keep = std::clamp(first, 0, columns_);
    line.cells.resize(static_cast<std::size_t>(std::min<int>(first_to_keep, line.used)));
    line.used = static_cast<std::uint16_t>(line.cells.size());
    ++line.generation;
  } else {
    const auto replacement = blank_cell();
    for (int column = first; column <= last; ++column) {
      mutable_cell_at(line, column) = replacement;
    }
    trim_trailing_blanks(line);
    ++line.generation;
  }
  if (mode == 2 || last == columns_ - 1) {
    set_line_wrapped(line, false);
  }
  mark_dirty_row(cursor_row_);
}

void GridCore::clear_screen(int mode) {
  if (mode == 2 || mode == 3) {
    if (mode == 3 && !alternate_screen_) {
      clear_scrollback();
    }
    clear_all();
    return;
  }
  if (mode == 1) {
    const auto active_screen_flag = alternate_screen_ ? kLineAlternateScreen : 0u;
    for (int row = 0; row < cursor_row_; ++row) {
      reset_line(line_at(row));
      set_line_screen_flag(line_at(row), active_screen_flag);
    }
    for (int column = 0; column <= cursor_column_; ++column) {
      clear_cell_at(cursor_row_, column);
    }
    mark_full_pane_dirty();
    return;
  }
  const auto active_screen_flag = alternate_screen_ ? kLineAlternateScreen : 0u;
  for (int column = cursor_column_; column < columns_; ++column) {
    clear_cell_at(cursor_row_, column);
  }
  for (int row = cursor_row_ + 1; row < rows_; ++row) {
    reset_line(line_at(row));
    set_line_screen_flag(line_at(row), active_screen_flag);
  }
  mark_full_pane_dirty();
}

void GridCore::erase_characters(int count) {
  const int last = std::min(columns_ - 1, cursor_column_ + std::max(1, count) - 1);
  auto& line = line_at(cursor_row_);
  const auto replacement = blank_cell();
  for (int column = cursor_column_; column <= last; ++column) {
    mutable_cell_at(line, column) = replacement;
  }
  trim_trailing_blanks(line);
  ++line.generation;
  mark_dirty_row(cursor_row_);
}

void GridCore::insert_characters(int count) {
  count = std::min(std::max(1, count), columns_ - cursor_column_);
  auto& line = line_at(cursor_row_);
  const int original_used = static_cast<int>(line.used);
  if (cursor_column_ >= original_used) {
    return;
  }
  const int next_used = std::min(columns_, original_used + count);
  if (static_cast<int>(line.cells.size()) < next_used) {
    line.cells.resize(static_cast<std::size_t>(next_used), line_blank_cell(line));
  }
  for (int column = next_used - 1; column >= cursor_column_ + count; --column) {
    mutable_cell_at(line, column) = cell_at_or_blank(line, column - count);
  }
  for (int column = cursor_column_; column < cursor_column_ + count; ++column) {
    mutable_cell_at(line, column) = blank_cell();
  }
  repair_wide_cells_in_row(line);
  trim_trailing_blanks(line);
  set_line_wrapped(line, false);
  ++line.generation;
  mark_dirty_row(cursor_row_);
}

void GridCore::delete_characters(int count) {
  count = std::min(std::max(1, count), columns_ - cursor_column_);
  auto& line = line_at(cursor_row_);
  const int original_used = static_cast<int>(line.used);
  if (cursor_column_ >= original_used) {
    return;
  }
  for (int column = cursor_column_; column + count < original_used; ++column) {
    mutable_cell_at(line, column) = cell_at_or_blank(line, column + count);
  }
  const int first_blank = std::max(cursor_column_, original_used - count);
  for (int column = first_blank; column < original_used; ++column) {
    mutable_cell_at(line, column) = blank_cell();
  }
  repair_wide_cells_in_row(line);
  trim_trailing_blanks(line);
  set_line_wrapped(line, false);
  ++line.generation;
  mark_dirty_row(cursor_row_);
}

void GridCore::move_cursor(int row, int column) {
  pending_wrap_ = false;
  cursor_row_ = std::clamp(row, 0, rows_ - 1);
  cursor_column_ = leading_column_for(cursor_row_, std::clamp(column, 0, columns_ - 1));
}

void GridCore::move_cursor_position(int row, int column) {
  if (origin_mode_) {
    move_cursor(std::clamp(scroll_top_ + row, scroll_top_, scroll_bottom_), column);
  } else {
    move_cursor(row, column);
  }
}

void GridCore::move_cursor_relative(int row_delta, int column_delta) {
  move_cursor(cursor_row_ + row_delta, cursor_column_ + column_delta);
}

void GridCore::save_cursor() {
  saved_cursor_ = SavedCursorState{
      cursor_column_,
      cursor_row_,
      pending_wrap_,
      cursor_visible_,
      cursor_style_,
      origin_mode_,
      wrap_mode_,
      insert_mode_,
      bracketed_paste_mode_,
      g0_charset_,
      g1_charset_,
      active_charset_slot_,
      current_attributes_,
      current_style_id_,
  };
}

void GridCore::restore_cursor() {
  cursor_visible_ = saved_cursor_.cursor_visible;
  cursor_style_ = saved_cursor_.cursor_style;
  origin_mode_ = saved_cursor_.origin_mode;
  wrap_mode_ = saved_cursor_.wrap_mode;
  insert_mode_ = saved_cursor_.insert_mode;
  bracketed_paste_mode_ = saved_cursor_.bracketed_paste_mode;
  g0_charset_ = saved_cursor_.g0_charset;
  g1_charset_ = saved_cursor_.g1_charset;
  active_charset_slot_ = saved_cursor_.active_charset_slot;
  current_attributes_ = saved_cursor_.attributes;
  current_style_id_ = saved_cursor_.style_id;
  move_cursor(saved_cursor_.row, saved_cursor_.column);
  pending_wrap_ = saved_cursor_.pending_wrap;
}

void GridCore::set_scroll_region(int top, int bottom) {
  if (top < 0 || bottom >= rows_ || top >= bottom) {
    reset_scroll_region();
    return;
  }
  scroll_top_ = top;
  scroll_bottom_ = bottom;
  move_cursor(0, 0);
}

void GridCore::reset_scroll_region() {
  scroll_top_ = 0;
  scroll_bottom_ = std::max(0, rows_ - 1);
}

void GridCore::apply_private_mode(int mode, bool enabled) {
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

void GridCore::apply_sgr(const CsiParams& params) {
  if (params.count == 0) {
    current_attributes_ = {};
    current_style_id_ = intern_style(current_attributes_);
    return;
  }
  const auto apply_indexed_or_truecolor = [&](std::size_t& index, bool foreground) {
    if (index + 2 < params.count && params.params[index + 1] == 5) {
      const auto color = std::clamp(params.params[index + 2], 0, 255);
      (foreground ? current_attributes_.foreground : current_attributes_.background) = color;
      index += 2;
      return true;
    }
    if (index + 4 < params.count && params.params[index + 1] == 2) {
      std::size_t red_index = index + 2;
      if (index + 5 < params.count && params.params[index + 2] == 0) {
        // Some terminals emit colon SGR truecolor as 48:2::R:G:B.
        // The empty color-space field is parsed as 0; skip it.
        red_index = index + 3;
      }
      if (red_index + 2 >= params.count) {
        return false;
      }
      const auto red = std::clamp(params.params[red_index], 0, 255);
      const auto green = std::clamp(params.params[red_index + 1], 0, 255);
      const auto blue = std::clamp(params.params[red_index + 2], 0, 255);
      const auto color = 0x01000000 | (red << 16) | (green << 8) | blue;
      (foreground ? current_attributes_.foreground : current_attributes_.background) = color;
      index = red_index + 2;
      return true;
    }
    return false;
  };

  for (std::size_t index = 0; index < params.count; ++index) {
    const int param = params.params[index];
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
  current_style_id_ = intern_style(current_attributes_);
}

void GridCore::repeat_preceding_character(int count) {
  if (count <= 0 || columns_ <= 0 || rows_ <= 0) {
    return;
  }

  int source_row = cursor_row_;
  int source_column = pending_wrap_ ? columns_ - 1 : cursor_column_ - 1;
  if (source_column < 0) {
    return;
  }
  source_column = leading_column_for(source_row, source_column);
  const auto& line = line_at(source_row);
  const auto source = cell_at_or_blank(line, source_column);
  if (cell_width(source) == TerminalCellWidth::WideContinuation) {
    return;
  }

  const auto glyph = cell_glyph(line, source);
  if (glyph.empty()) {
    return;
  }
  const auto width = cell_width(source) == TerminalCellWidth::WideLeading ? 2 : 1;
  for (int index = 0; index < count; ++index) {
    print_cell(glyph, width);
  }
}

std::uint32_t GridCore::map_printable_ascii(unsigned char byte) const {
  const auto charset = active_charset_slot_ == 1 ? g1_charset_ : g0_charset_;
  if (charset != CharacterSet::DecSpecialGraphics) {
    return byte;
  }

  switch (byte) {
    case '`':
      return 0x25c6;  // black diamond
    case 'a':
      return 0x2592;  // checkerboard
    case 'b':
      return 0x2409;  // HT symbol
    case 'c':
      return 0x240c;  // FF symbol
    case 'd':
      return 0x240d;  // CR symbol
    case 'e':
      return 0x240a;  // LF symbol
    case 'f':
      return 0x00b0;  // degree
    case 'g':
      return 0x00b1;  // plus/minus
    case 'h':
      return 0x2424;  // NL symbol
    case 'i':
      return 0x240b;  // VT symbol
    case 'j':
      return 0x2518;  // lower-right corner
    case 'k':
      return 0x2510;  // upper-right corner
    case 'l':
      return 0x250c;  // upper-left corner
    case 'm':
      return 0x2514;  // lower-left corner
    case 'n':
      return 0x253c;  // crossing lines
    case 'o':
      return 0x23ba;  // scan line 1
    case 'p':
      return 0x23bb;  // scan line 3
    case 'q':
      return 0x2500;  // horizontal line
    case 'r':
      return 0x23bc;  // scan line 7
    case 's':
      return 0x23bd;  // scan line 9
    case 't':
      return 0x251c;  // left tee
    case 'u':
      return 0x2524;  // right tee
    case 'v':
      return 0x2534;  // bottom tee
    case 'w':
      return 0x252c;  // top tee
    case 'x':
      return 0x2502;  // vertical line
    case 'y':
      return 0x2264;  // less-than-or-equal
    case 'z':
      return 0x2265;  // greater-than-or-equal
    case '{':
      return 0x03c0;  // pi
    case '|':
      return 0x2260;  // not equal
    case '}':
      return 0x00a3;  // pound
    case '~':
      return 0x00b7;  // middle dot
    default:
      return byte;
  }
}

bool GridCore::using_ascii_character_set() const {
  return (active_charset_slot_ == 1 ? g1_charset_ : g0_charset_) == CharacterSet::Ascii;
}

void GridCore::set_alternate_screen(bool enabled) {
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

std::string GridCore::row_text(const TerminalLine& line, const std::vector<Cell>& cells) const {
  std::string text;
  text.reserve(cells.size());
  for (const auto& cell : cells) {
    if (cell_width(cell) == TerminalCellWidth::WideContinuation) {
      continue;
    }
    append_cell_glyph(text, line, cell);
  }
  return text;
}

}  // namespace wmux::terminal_engine_v2
