#pragma once

#include "wmux/terminal_engine.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wmux::terminal_engine_v2 {

using StyleId = std::uint16_t;
using LineId = std::uint32_t;

class StyleTable {
 public:
  StyleId intern(const TerminalAttributes& attributes);
  const TerminalAttributes& get(StyleId id) const;

 private:
  std::vector<TerminalAttributes> styles_{TerminalAttributes{}};
};

struct CellCommon {
  std::uint32_t codepoint{U' '};
  StyleId style_id{0};
  std::uint8_t flags{0};
  std::uint8_t width{static_cast<std::uint8_t>(TerminalCellWidth::Narrow)};
};

struct CellExtended {
  std::string grapheme;
  StyleId style_id{0};
  std::uint8_t width{static_cast<std::uint8_t>(TerminalCellWidth::Narrow)};
};

struct Cell {
  CellCommon common;
  // 0 means common cell. Non-zero values index TerminalLine::extended_cells
  // using one-based indexing so the common case stays compact.
  std::uint32_t extended_index{0};
};

struct TerminalLine {
  std::vector<Cell> cells;
  std::vector<CellExtended> extended_cells;
  std::uint16_t used{0};
  std::uint16_t capacity{0};
  std::uint32_t flags{0};
  StyleId blank_style_id{0};
  bool wrapped{false};
  std::uint64_t generation{0};
  mutable std::uint64_t materialized_generation{0};
  mutable std::vector<wmux::TerminalCell> materialized_cells;
};

enum TerminalLineFlags : std::uint32_t {
  kLineWrapped = 1u << 0,
  kLineDirty = 1u << 1,
  kLineHistory = 1u << 2,
  kLineAlternateScreen = 1u << 3,
};

enum class UnknownSequenceClass {
  Escape,
  Csi,
  Osc,
  Utf8,
};

struct CsiParams {
  std::array<int, 64> params{};
  std::uint8_t count{0};
  bool private_mode{false};
  bool overflow{false};

  void push(int value);
  int value_or(std::size_t index, int default_value) const;
};

enum class CharacterSet {
  Ascii,
  DecSpecialGraphics,
};

struct SavedCursorState {
  int column{0};
  int row{0};
  bool pending_wrap{false};
  bool cursor_visible{true};
  int cursor_style{0};
  bool origin_mode{false};
  bool wrap_mode{true};
  bool insert_mode{false};
  bool bracketed_paste_mode{false};
  CharacterSet g0_charset{CharacterSet::Ascii};
  CharacterSet g1_charset{CharacterSet::Ascii};
  int active_charset_slot{0};
  TerminalAttributes attributes;
  StyleId style_id{0};
};

// V2 is deliberately split into parser -> writer -> grid-core components so
// the fast path can evolve without mutating the legacy TerminalGrid forever.
class GridCore {
 public:
  GridCore();
  GridCore(int columns, int rows);

  void resize(int columns, int rows);
  void print_ascii_span(std::string_view text);
  void print_codepoint(std::uint32_t codepoint, std::string_view glyph);
  void execute_control(unsigned char byte);
  void dispatch_escape(char final_byte);
  void designate_character_set(int slot, char final_byte);
  void dispatch_csi(const CsiParams& params, char final_byte);
  void dispatch_osc(std::string_view payload);
  void record_unknown(UnknownSequenceClass sequence_class, std::size_t length, char final_byte = 0);
  TerminalAttributes current_attributes() const;
  int columns() const;
  int rows() const;
  CursorState cursor() const;
  TerminalLineView line_view(int row) const;
  std::uint64_t line_generation(int row) const;
  ScrollbackView scrollback_view(int start, int count) const;
  TerminalDamage consume_damage();
  void set_scrollback_capacity(std::size_t capacity);
  TerminalScreenSnapshot snapshot(bool consume_dirty, bool dirty_rows_only) const;
  TerminalScrollbackSnapshot scrollback_snapshot() const;
  TerminalScrollbackSnapshot scrollback_snapshot_range(
      std::size_t first_line,
      std::size_t line_count) const;

 private:
  std::vector<LineId>& active_viewport();
  const std::vector<LineId>& active_viewport() const;
  TerminalLine& line_at(int row);
  const TerminalLine& line_at(int row) const;
  TerminalLine& line_by_id(LineId id);
  const TerminalLine& line_by_id(LineId id) const;
  Cell& compact_cell_at(int row, int column);
  Cell compact_cell_at(int row, int column) const;
  Cell& mutable_cell_at(TerminalLine& line, int column);
  Cell cell_at_or_blank(const TerminalLine& line, int column) const;
  Cell blank_cell() const;
  Cell blank_cell(StyleId style_id) const;
  Cell line_blank_cell(const TerminalLine& line) const;
  Cell common_cell(std::uint32_t codepoint, TerminalCellWidth width) const;
  Cell extended_cell(TerminalLine& line, std::string glyph, int width) const;
  bool is_blank_cell_for_line(const TerminalLine& line, const Cell& cell) const;
  void trim_trailing_blanks(TerminalLine& line);
  TerminalCell materialized_cell(const TerminalLine& line, const Cell& cell) const;
  std::span<const TerminalCell> materialized_cells(const TerminalLine& line) const;
  StyleId intern_style(const TerminalAttributes& attributes);
  const TerminalAttributes& style_at(StyleId style_id) const;
  TerminalLine make_line() const;
  void reset_line(TerminalLine& line);
  void set_line_wrapped(TerminalLine& line, bool wrapped);
  void set_line_screen_flag(TerminalLine& line, std::uint32_t flag);
  LineId allocate_line();
  void release_line(LineId id);
  void initialize_viewport(std::vector<LineId>& viewport, int row_count, std::uint32_t line_flags);
  void resize_viewport(std::vector<LineId>& viewport, int row_count);
  void set_viewport_line_flags(std::vector<LineId>& viewport, std::uint32_t line_flags);
  void mark_dirty_row(int row);
  void mark_dirty_range(int first, int last);
  void mark_all_dirty();
  void mark_full_pane_dirty();
  void mark_scroll_damage(int top, int bottom, int count, TerminalScrollDirection direction);
  void update_damage_range(int first, int last);
  void reset_damage_range() const;
  DamageKind damage_snapshot(bool consume_dirty) const;
  std::vector<int> dirty_rows_snapshot(bool consume_dirty) const;
  TerminalDamage damage_snapshot_with_rows(bool consume_dirty) const;
  TerminalLineSnapshot row_snapshot(const TerminalLine& line) const;
  std::string row_text(const TerminalLine& line) const;
  std::string row_text(const TerminalLine& line, const std::vector<Cell>& cells) const;
  std::size_t print_ascii_row_segment(std::string_view text);
  void print_cell(std::string glyph, int width);
  int leading_column_for(int row, int column) const;
  void clear_cell_at(int row, int column);
  void repair_wide_cells_in_row(TerminalLine& line);
  void repair_wide_cells();
  bool append_codepoint_to_previous_grapheme(std::string_view glyph, std::uint32_t codepoint);
  void append_zero_width_to_previous_cell(std::string_view glyph);
  void carriage_return();
  void line_feed();
  void backspace();
  void tab();
  void backtab(int count);
  void scroll_up_region(int top, int bottom, int count);
  void scroll_down_region(int top, int bottom, int count);
  void append_scrollback_line(LineId id);
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
  void apply_private_mode(int mode, bool enabled);
  void apply_sgr(const CsiParams& params);
  void repeat_preceding_character(int count);
  std::uint32_t map_printable_ascii(unsigned char byte) const;
  bool using_ascii_character_set() const;
  void set_alternate_screen(bool enabled);

  int columns_{80};
  int rows_{24};
  int cursor_column_{0};
  int cursor_row_{0};
  SavedCursorState saved_cursor_;
  int scroll_top_{0};
  int scroll_bottom_{23};
  bool pending_wrap_{false};
  bool cursor_visible_{true};
  int cursor_style_{0};
  bool origin_mode_{false};
  bool wrap_mode_{true};
  bool insert_mode_{false};
  bool bracketed_paste_mode_{false};
  bool alternate_screen_{false};
  CharacterSet g0_charset_{CharacterSet::Ascii};
  CharacterSet g1_charset_{CharacterSet::Ascii};
  int active_charset_slot_{0};
  int normal_cursor_column_before_alternate_{0};
  int normal_cursor_row_before_alternate_{0};
  TerminalAttributes current_attributes_;
  StyleId current_style_id_{0};
  StyleTable style_table_;
  std::vector<TerminalLine> line_pool_;
  std::vector<LineId> free_line_ids_;
  std::vector<LineId> normal_viewport_;
  std::vector<LineId> alternate_viewport_;
  mutable std::vector<bool> dirty_rows_;
  mutable DamageKind damage_{DamageKind::FullPane};
  mutable int damage_first_row_{0};
  mutable int damage_last_row_{23};
  mutable std::optional<TerminalScrollDamage> scroll_damage_;
  std::size_t scrollback_capacity_{kMaxPaneScrollbackLines};
  std::deque<LineId> scrollback_;
  mutable std::deque<wmux::TerminalLine> scrollback_view_cache_;
  std::string title_;
  std::size_t unknown_sequence_count_{0};
};

class ScreenWriter {
 public:
  explicit ScreenWriter(GridCore& grid);

  void print_ascii_span(std::string_view text);
  void print_utf8(std::uint32_t codepoint, std::string_view glyph);
  void execute_control(unsigned char byte);
  void dispatch_escape(char final_byte);
  void designate_character_set(int slot, char final_byte);
  void dispatch_csi(const CsiParams& params, char final_byte);
  void dispatch_osc(std::string_view payload);
  void unknown(UnknownSequenceClass sequence_class, std::size_t length, char final_byte = 0);
  void flush_print_run();

 private:
  struct PendingPrintRun {
    int row{0};
    int column{0};
    TerminalAttributes attributes;
    std::string_view ascii;
  };

  void start_print_run(std::string_view text);

  GridCore& grid_;
  PendingPrintRun pending_print_run_;
  bool has_pending_print_run_{false};
};

class VtParser {
 public:
  void parse(std::span<const std::byte> bytes, ScreenWriter& writer);

 private:
  enum class State {
    Ground,
    Escape,
    CharsetG0,
    CharsetG1,
    Csi,
    CsiIgnore,
    Osc,
    OscEscape,
    OscDiscard,
    OscDiscardEscape,
  };

  void reset_utf8();
  bool feed_utf8_byte(unsigned char byte, ScreenWriter& writer);
  void emit_utf8_replacement(ScreenWriter& writer);
  void start_csi();
  void push_csi_param();
  void reset_osc();
  void enter_ground();

  State state_{State::Ground};
  CsiParams csi_params_;
  int csi_value_{0};
  bool csi_have_value_{false};
  std::size_t sequence_length_{0};
  std::string osc_buffer_;
  int utf8_remaining_{0};
  std::uint32_t utf8_codepoint_{0};
  std::string utf8_bytes_;
};

}  // namespace wmux::terminal_engine_v2
