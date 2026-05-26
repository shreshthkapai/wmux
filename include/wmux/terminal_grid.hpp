#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace wmux {

struct TerminalAttributes {
  bool bold{false};
  bool underline{false};
  bool inverse{false};
  std::int16_t foreground{-1};
  std::int16_t background{-1};
};

struct TerminalCell {
  char glyph{' '};
  TerminalAttributes attributes;
};

struct TerminalScreenSnapshot {
  int columns{0};
  int rows{0};
  int cursor_column{0};
  int cursor_row{0};
  bool alternate_screen{false};
  std::vector<std::string> lines;
};

class TerminalGrid final {
 public:
  TerminalGrid();
  TerminalGrid(int columns, int rows);

  void resize(int columns, int rows);
  void feed(std::string_view bytes);
  TerminalScreenSnapshot snapshot() const;

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
  TerminalCell blank_cell() const;
  std::size_t offset(int row, int column) const;

  void put_printable(char glyph);
  void carriage_return();
  void line_feed();
  void backspace();
  void tab();
  void scroll_up();
  void clear_all();
  void clear_line(int mode);
  void clear_screen(int mode);
  void move_cursor(int row, int column);
  void move_cursor_relative(int row_delta, int column_delta);
  void apply_csi(char final_byte);
  void apply_sgr(const std::vector<int>& params);
  void set_alternate_screen(bool enabled);

  int columns_{80};
  int rows_{24};
  int cursor_column_{0};
  int cursor_row_{0};
  bool pending_wrap_{false};
  bool alternate_screen_{false};
  TerminalAttributes current_attributes_;
  std::vector<TerminalCell> normal_buffer_;
  std::vector<TerminalCell> alternate_buffer_;
  ParserState parser_state_{ParserState::Ground};
  std::string csi_buffer_;
};

}  // namespace wmux
