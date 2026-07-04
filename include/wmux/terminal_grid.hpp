#pragma once

#include "wmux/resource_limits.hpp"
#include "wmux/terminal_vt.hpp"
#include "wmux/unicode_width.hpp"

#include <cstdint>
#include <cstddef>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wmux {

struct TerminalAttributes {
  bool bold{false};
  bool dim{false};
  bool italic{false};
  bool underline{false};
  bool inverse{false};
  std::int32_t foreground{-1};
  std::int32_t background{-1};
};

struct TerminalCell {
  char32_t codepoint{U' '};
  std::string extended;
  TerminalAttributes attributes;
  TerminalCellWidth width{TerminalCellWidth::Narrow};
};

struct TerminalLine {
  std::vector<TerminalCell> cells;
  bool wrapped{false};
  std::uint64_t generation{0};
};

struct TerminalLineSnapshot {
  std::string text;
  bool wrapped{false};
  std::vector<TerminalAttributes> attributes;
  std::vector<std::string> cells;
  std::vector<TerminalCellWidth> cell_widths;
};

struct TerminalScrollEvent {
  int top{0};
  int bottom{0};
  int rows{0};
};

enum class DamageKind {
  None = 0,
  RowRange = 1,
  DirtyRows = RowRange,
  FullPane = 2,
  FullWindow = 3,
  FullScreen = 4,
};

DamageKind merge_damage(DamageKind a, DamageKind b);

enum class TerminalScrollDirection {
  Up,
  Down,
};

struct TerminalScrollDamage {
  int top_row{0};
  int bottom_row{0};
  int count{0};
  TerminalScrollDirection direction{TerminalScrollDirection::Up};
};

struct CursorState {
  int column{0};
  int row{0};
  bool visible{true};
  int style{0};
  bool origin_mode{false};
  bool wrap_mode{true};
  bool bracketed_paste_mode{false};
  bool alternate_screen{false};
};

struct TerminalLineView {
  std::span<const TerminalCell> cells;
  bool wrapped{false};
  std::uint64_t generation{0};
};

struct ScrollbackView {
  const std::deque<TerminalLine>* lines{nullptr};
  std::size_t start{0};
  std::size_t count{0};
  std::size_t total_lines{0};
  std::size_t capacity{0};
};

struct TerminalDamage {
  DamageKind kind{DamageKind::None};
  int first_row{-1};
  int last_row{-1};
  std::vector<int> dirty_rows;
  std::optional<TerminalScrollDamage> scroll;
};

struct TerminalScreenSnapshot {
  int columns{0};
  int rows{0};
  int cursor_column{0};
  int cursor_row{0};
  bool cursor_visible{true};
  int cursor_style{0};
  bool origin_mode{false};
  bool wrap_mode{true};
  bool bracketed_paste_mode{false};
  bool alternate_screen{false};
  std::string title;
  std::size_t scrollback_line_count{0};
  std::size_t unknown_sequence_count{0};
  std::vector<std::string> lines;
  std::vector<TerminalLineSnapshot> line_snapshots;
  std::vector<int> dirty_rows;
  int damage_first_row{-1};
  int damage_last_row{-1};
  std::vector<TerminalLineSnapshot> dirty_line_snapshots;
  DamageKind damage{DamageKind::None};
  // Reserved for a future carefully-verified scroll optimization. Child PTY
  // scrolls currently force FullPane damage instead of host-terminal scrolls.
  std::vector<TerminalScrollEvent> scroll_events;
};

struct TerminalScrollbackSnapshot {
  std::size_t capacity{0};
  std::size_t total_lines{0};
  std::size_t first_line_index{0};
  bool partial{false};
  std::vector<std::string> lines;
  std::vector<TerminalLineSnapshot> line_snapshots;
};

class TerminalGrid final {
 public:
  TerminalGrid();
  TerminalGrid(int columns, int rows);

  void resize(int columns, int rows);
  void feed(std::string_view bytes);
  int columns() const noexcept;
  int rows() const noexcept;
  CursorState cursor() const;
  TerminalLineView line_view(int row) const;
  std::uint64_t line_generation(int row) const;
  ScrollbackView scrollback_view(int start, int count) const;
  TerminalDamage consume_damage() const;
  void set_scrollback_capacity(std::size_t capacity);
  TerminalScreenSnapshot snapshot(
      bool consume_dirty = false,
      bool dirty_rows_only = false) const;
  TerminalScrollbackSnapshot scrollback_snapshot() const;
  TerminalScrollbackSnapshot scrollback_snapshot_range(
      std::size_t first_line,
      std::size_t line_count) const;

 private:
  std::vector<TerminalLine>& active_lines();
  const std::vector<TerminalLine>& active_lines() const;
  TerminalLine& line_at(int row);
  const TerminalLine& line_at(int row) const;
  TerminalCell& cell_at(int row, int column);
  const TerminalCell& cell_at(int row, int column) const;
  TerminalCell blank_cell() const;
  std::vector<int> dirty_rows_snapshot(bool consume_dirty) const;
  std::vector<TerminalScrollEvent> scroll_events_snapshot(bool consume_dirty) const;
  DamageKind damage_snapshot(bool consume_dirty) const;
  void feed_parser_byte(char byte);
  void feed_ascii_fast_path(std::string_view bytes, std::size_t& index);
  void mark_dirty_row(int row);
  void mark_dirty_range(int first, int last);
  void mark_all_dirty();
  void mark_full_pane_dirty();
  TerminalLineSnapshot row_snapshot(const TerminalLine& line) const;
  std::string row_text(const TerminalLine& line) const;
  std::string row_text(const std::vector<TerminalCell>& cells) const;

  void put_printable(char glyph);
  void put_ascii_run(std::string_view text);
  std::size_t put_ascii_row_segment(std::string_view text);
  void put_codepoint(std::uint32_t codepoint);
  void put_cell(std::string glyph, int width);
  int leading_column_for(int row, int column) const;
  void clear_cell_at(int row, int column);
  void reset_line(TerminalLine& line);
  void repair_wide_cells_in_row(TerminalLine& line) const;
  void repair_wide_cells();
  bool append_codepoint_to_previous_grapheme(std::string_view glyph, std::uint32_t codepoint);
  void append_zero_width_to_previous_cell(std::string_view glyph);
  void carriage_return();
  void line_feed();
  void backspace();
  void tab();
  void backtab(int count);
  void scroll_up();
  void scroll_up_region(int top, int bottom, int count);
  void scroll_down_region(int top, int bottom, int count);
  void append_scrollback_row(
      const TerminalLine& line);
  void clear_scrollback();
  void clear_all();
  void clear_line(int mode);
  void clear_screen(int mode);
  void erase_characters(int count);
  void insert_characters(int count);
  void delete_characters(int count);
  void move_cursor(int row, int column);
  void move_cursor_position(int row, int column);
  void move_cursor_relative(int row_delta, int column_delta);
  void save_cursor();
  void restore_cursor();
  void set_scroll_region(int top, int bottom);
  void reset_scroll_region();
  void apply_operation(const TerminalVtOperation& operation);
  void apply_escape(char final_byte);
  void apply_csi(std::string_view parameters, char final_byte);
  void apply_osc(std::string_view payload);
  void apply_private_mode(int mode, bool enabled);
  void apply_sgr(const std::vector<int>& params);
  void set_alternate_screen(bool enabled);
  void record_unknown_sequence(const TerminalVtUnknownOperation& unknown);

  int columns_{80};
  int rows_{24};
  int cursor_column_{0};
  int cursor_row_{0};
  int saved_cursor_column_{0};
  int saved_cursor_row_{0};
  int scroll_top_{0};
  int scroll_bottom_{23};
  bool pending_wrap_{false};
  bool cursor_visible_{true};
  int cursor_style_{0};
  bool origin_mode_{false};
  bool wrap_mode_{true};
  bool bracketed_paste_mode_{false};
  bool alternate_screen_{false};
  int normal_cursor_column_before_alternate_{0};
  int normal_cursor_row_before_alternate_{0};
  TerminalAttributes current_attributes_;
  std::vector<TerminalLine> normal_lines_;
  std::vector<TerminalLine> alternate_lines_;
  mutable std::vector<bool> dirty_rows_;
  mutable std::vector<TerminalScrollEvent> scroll_events_;
  mutable DamageKind damage_{DamageKind::FullPane};
  std::size_t scrollback_capacity_{kMaxPaneScrollbackLines};
  std::deque<TerminalLine> scrollback_;
  TerminalVtParser parser_;
  std::vector<TerminalVtOperation> operation_buffer_;
  std::string title_;
  std::size_t unknown_sequence_count_{0};
  std::size_t logged_unknown_sequence_count_{0};
};

}  // namespace wmux
