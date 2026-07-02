#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wmux {

enum class TerminalCellWidth : std::uint8_t {
  Narrow,
  WideLeading,
  WideContinuation,
};

struct Utf8Codepoint {
  std::string_view bytes;
  std::uint32_t codepoint{0};
  bool valid{false};
};

struct TerminalTextCell {
  std::string text;
  TerminalCellWidth width{TerminalCellWidth::Narrow};
};

struct TerminalGraphemeCluster {
  std::string text;
  int width{1};
};

std::optional<Utf8Codepoint> decode_utf8_codepoint(
    std::string_view input,
    std::size_t index);

std::string utf8_from_codepoint(std::uint32_t codepoint);

bool is_terminal_zero_width_codepoint(std::uint32_t codepoint);
bool is_terminal_wide_codepoint(std::uint32_t codepoint);
bool is_terminal_regional_indicator_codepoint(std::uint32_t codepoint);
int terminal_codepoint_width(std::uint32_t codepoint);
int terminal_grapheme_width(std::string_view text);
bool terminal_codepoint_extends_previous_grapheme(
    std::string_view previous_grapheme,
    std::uint32_t codepoint);

std::vector<TerminalGraphemeCluster> terminal_grapheme_clusters_from_text(
    std::string_view text);

std::vector<TerminalTextCell> terminal_text_cells_from_text(
    std::string_view text,
    std::size_t width);

std::string sanitize_utf8_boundaries(std::string_view value);

}  // namespace wmux
