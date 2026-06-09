#pragma once

#include "wmux/resource_limits.hpp"

#include <cstdint>
#include <cstddef>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace wmux {

struct TerminalAttributes {
  bool bold{false};
  bool underline{false};
  bool inverse{false};
  std::int32_t foreground{-1};
  std::int32_t background{-1};
};

struct TerminalCell {
  std::string glyph{" "};
  TerminalAttributes attributes;
  bool wide_continuation{false};
};

struct TerminalLineSnapshot {
  std::string text;
  bool wrapped{false};
  std::vector<TerminalAttributes> attributes;
  std::vector<std::string> cells;
};

struct TerminalScreenSnapshot {
  int columns{0};
  int rows{0};
  int cursor_column{0};
  int cursor_row{0};
  bool alternate_screen{false};
  std::size_t scrollback_line_count{0};
  std::vector<std::string> lines;
  std::vector<TerminalLineSnapshot> line_snapshots;
};

struct TerminalScrollbackSnapshot {
  std::size_t capacity{0};
  std::vector<std::string> lines;
  std::vector<TerminalLineSnapshot> line_snapshots;
};

class TerminalGrid final {
 public:
  TerminalGrid();
  TerminalGrid(int columns, int rows);

  void resize(int columns, int rows);
  void feed(std::string_view bytes);
  void set_scrollback_capacity(std::size_t capacity);
  TerminalScreenSnapshot snapshot() const;
  TerminalScrollbackSnapshot scrollback_snapshot() const;

 private:
  enum class ParserState {
    Ground,
    Escape,
    Csi,
    Osc,
    OscEscape,
  };

  std::vector<TerminalCell>& active_buffer();
  const std::vector<TerminalCell>& active_buffer() const;
  std::vector<bool>& active_wrapped_lines();
  const std::vector<bool>& active_wrapped_lines() const;
  TerminalCell blank_cell() const;
  std::size_t offset(int row, int column) const;
  std::vector<TerminalCell> row_cells(const std::vector<TerminalCell>& buffer, int row) const;
  TerminalLineSnapshot row_snapshot(
      const std::vector<TerminalCell>& buffer,
      const std::vector<bool>& wrapped_lines,
      int row) const;
  std::string row_text(const std::vector<TerminalCell>& buffer, int row) const;
  std::string row_text(const std::vector<TerminalCell>& cells) const;

  void put_printable(char glyph);
  void put_codepoint(std::uint32_t codepoint);
  void put_cell(std::string glyph, int width);
  void carriage_return();
  void line_feed();
  void backspace();
  void tab();
  void scroll_up();
  void scroll_up_region(int top, int bottom, int count);
  void scroll_down_region(int top, int bottom, int count);
  void append_scrollback_line(std::vector<TerminalCell> line, bool wrapped);
  void clear_scrollback();
  void clear_all();
  void clear_line(int mode);
  void clear_screen(int mode);
  void erase_characters(int count);
  void insert_characters(int count);
  void delete_characters(int count);
  void move_cursor(int row, int column);
  void move_cursor_relative(int row_delta, int column_delta);
  void save_cursor();
  void restore_cursor();
  void set_scroll_region(int top, int bottom);
  void reset_scroll_region();
  void apply_csi(char final_byte);
  void apply_sgr(const std::vector<int>& params);
  void set_alternate_screen(bool enabled);

  int columns_{80};
  int rows_{24};
  int cursor_column_{0};
  int cursor_row_{0};
  int saved_cursor_column_{0};
  int saved_cursor_row_{0};
  int scroll_top_{0};
  int scroll_bottom_{23};
  bool pending_wrap_{false};
  bool alternate_screen_{false};
  int utf8_remaining_{0};
  std::uint32_t utf8_codepoint_{0};
  TerminalAttributes current_attributes_;
  std::vector<TerminalCell> normal_buffer_;
  std::vector<TerminalCell> alternate_buffer_;
  std::vector<bool> normal_wrapped_lines_;
  std::vector<bool> alternate_wrapped_lines_;
  std::size_t scrollback_capacity_{kMaxPaneScrollbackLines};
  std::deque<TerminalLineSnapshot> scrollback_;
  ParserState parser_state_{ParserState::Ground};
  std::string csi_buffer_;
};

}  // namespace wmux
