#include "wmux/unicode_width.hpp"

#include <algorithm>

namespace wmux {
namespace {

constexpr std::uint32_t kReplacementCodepoint = 0xfffd;

bool is_utf8_continuation(unsigned char byte) {
  return (byte & 0xc0) == 0x80;
}

bool is_combining_mark(std::uint32_t codepoint) {
  return (codepoint >= 0x0300 && codepoint <= 0x036f) ||
         (codepoint >= 0x1ab0 && codepoint <= 0x1aff) ||
         (codepoint >= 0x1dc0 && codepoint <= 0x1dff) ||
         (codepoint >= 0x20d0 && codepoint <= 0x20ff) ||
         (codepoint >= 0xfe20 && codepoint <= 0xfe2f);
}

bool is_variation_selector(std::uint32_t codepoint) {
  return (codepoint >= 0xfe00 && codepoint <= 0xfe0f) ||
         (codepoint >= 0xe0100 && codepoint <= 0xe01ef);
}

bool is_emoji_modifier(std::uint32_t codepoint) {
  return codepoint >= 0x1f3fb && codepoint <= 0x1f3ff;
}

bool is_emoji_tag(std::uint32_t codepoint) {
  return codepoint >= 0xe0020 && codepoint <= 0xe007f;
}

bool is_spacing_combining_mark(std::uint32_t codepoint) {
  return codepoint == 0x0903 || codepoint == 0x093b ||
         (codepoint >= 0x093e && codepoint <= 0x0940) ||
         (codepoint >= 0x0949 && codepoint <= 0x094c) ||
         codepoint == 0x094e || codepoint == 0x094f ||
         codepoint == 0x0982 || codepoint == 0x0983 ||
         (codepoint >= 0x09be && codepoint <= 0x09c0) ||
         (codepoint >= 0x09c7 && codepoint <= 0x09c8) ||
         (codepoint >= 0x09cb && codepoint <= 0x09cc) ||
         codepoint == 0x0a03 ||
         (codepoint >= 0x0abe && codepoint <= 0x0ac0) ||
         codepoint == 0x0ac9 ||
         (codepoint >= 0x0acb && codepoint <= 0x0acc) ||
         (codepoint >= 0x0b3e && codepoint <= 0x0b40) ||
         (codepoint >= 0x0b47 && codepoint <= 0x0b48) ||
         (codepoint >= 0x0b4b && codepoint <= 0x0b4c) ||
         (codepoint >= 0x0bbe && codepoint <= 0x0bc2) ||
         (codepoint >= 0x0bc6 && codepoint <= 0x0bc8) ||
         (codepoint >= 0x0bca && codepoint <= 0x0bcc) ||
         (codepoint >= 0x0c01 && codepoint <= 0x0c03) ||
         (codepoint >= 0x0c41 && codepoint <= 0x0c44) ||
         (codepoint >= 0x0c82 && codepoint <= 0x0c83) ||
         (codepoint >= 0x0cc0 && codepoint <= 0x0cc4) ||
         (codepoint >= 0x0cca && codepoint <= 0x0ccb) ||
         (codepoint >= 0x0d02 && codepoint <= 0x0d03) ||
         (codepoint >= 0x0d3e && codepoint <= 0x0d40) ||
         (codepoint >= 0x0d46 && codepoint <= 0x0d48) ||
         (codepoint >= 0x0d4a && codepoint <= 0x0d4c) ||
         (codepoint >= 0x0f3e && codepoint <= 0x0f3f);
}

bool is_emoji_presentation_candidate(std::uint32_t codepoint) {
  return (codepoint >= 0x1f000 && codepoint <= 0x1faff) ||
         (codepoint >= 0x2600 && codepoint <= 0x27bf);
}

std::vector<std::uint32_t> decoded_codepoints(std::string_view text) {
  std::vector<std::uint32_t> codepoints;
  for (std::size_t index = 0; index < text.size();) {
    const auto decoded = decode_utf8_codepoint(text, index);
    if (!decoded) {
      break;
    }

    if (decoded->valid) {
      codepoints.push_back(decoded->codepoint);
    }
    index += std::max<std::size_t>(1, decoded->bytes.size());
  }
  return codepoints;
}

bool is_grapheme_extend_codepoint(std::uint32_t codepoint) {
  return is_combining_mark(codepoint) ||
         is_spacing_combining_mark(codepoint) ||
         is_variation_selector(codepoint) ||
         is_emoji_modifier(codepoint) ||
         is_emoji_tag(codepoint) ||
         codepoint == 0x200c ||
         codepoint == 0x200d ||
         codepoint == 0xfeff;
}

bool contains_non_zero_width_base(const std::vector<std::uint32_t>& codepoints) {
  return std::ranges::any_of(codepoints, [](std::uint32_t codepoint) {
    return !is_grapheme_extend_codepoint(codepoint) &&
           terminal_codepoint_width(codepoint) > 0;
  });
}

std::size_t regional_indicator_run_length(const std::vector<std::uint32_t>& codepoints) {
  std::size_t count = 0;
  for (auto cursor = codepoints.rbegin(); cursor != codepoints.rend(); ++cursor) {
    if (!is_terminal_regional_indicator_codepoint(*cursor)) {
      break;
    }
    ++count;
  }
  return count;
}

bool should_extend_current_cluster(
    const std::vector<std::uint32_t>& current,
    std::uint32_t codepoint) {
  if (current.empty()) {
    return false;
  }

  if (is_grapheme_extend_codepoint(codepoint)) {
    return true;
  }

  if (current.back() == 0x200d) {
    return true;
  }

  if (is_terminal_regional_indicator_codepoint(codepoint)) {
    const auto trailing_regional_indicators = regional_indicator_run_length(current);
    return trailing_regional_indicators % 2 == 1;
  }

  return false;
}

int cluster_width_for_codepoints(const std::vector<std::uint32_t>& codepoints) {
  if (codepoints.empty()) {
    return 0;
  }

  if (!contains_non_zero_width_base(codepoints)) {
    return 0;
  }

  if (regional_indicator_run_length(codepoints) >= 2) {
    return 2;
  }

  if (std::ranges::find(codepoints, 0x200d) != codepoints.end()) {
    return 2;
  }

  for (const auto codepoint : codepoints) {
    if (terminal_codepoint_width(codepoint) == 2) {
      return 2;
    }
  }
  return 1;
}

void append_zero_width_to_previous(std::vector<TerminalTextCell>& cells, std::string_view bytes) {
  for (auto cursor = cells.rbegin(); cursor != cells.rend(); ++cursor) {
    if (cursor->width == TerminalCellWidth::WideContinuation) {
      continue;
    }

    if (!cursor->text.empty() && cursor->text != " ") {
      cursor->text.append(bytes);
    }
    return;
  }
}

}  // namespace

std::optional<Utf8Codepoint> decode_utf8_codepoint(
    std::string_view input,
    std::size_t index) {
  if (index >= input.size()) {
    return std::nullopt;
  }

  const auto first = static_cast<unsigned char>(input[index]);
  if (first < 0x80) {
    return Utf8Codepoint{input.substr(index, 1), first, true};
  }

  std::size_t sequence_size = 0;
  std::uint32_t codepoint = 0;
  if (first >= 0xc2 && first <= 0xdf) {
    sequence_size = 2;
    codepoint = first & 0x1f;
  } else if (first >= 0xe0 && first <= 0xef) {
    sequence_size = 3;
    codepoint = first & 0x0f;
  } else if (first >= 0xf0 && first <= 0xf4) {
    sequence_size = 4;
    codepoint = first & 0x07;
  } else {
    return Utf8Codepoint{input.substr(index, 1), kReplacementCodepoint, false};
  }

  if (index + sequence_size > input.size()) {
    return Utf8Codepoint{input.substr(index, 1), kReplacementCodepoint, false};
  }

  for (std::size_t offset = 1; offset < sequence_size; ++offset) {
    const auto byte = static_cast<unsigned char>(input[index + offset]);
    if (!is_utf8_continuation(byte)) {
      return Utf8Codepoint{input.substr(index, 1), kReplacementCodepoint, false};
    }
    codepoint = (codepoint << 6) | (byte & 0x3f);
  }

  return Utf8Codepoint{input.substr(index, sequence_size), codepoint, true};
}

std::string utf8_from_codepoint(std::uint32_t codepoint) {
  std::string out;
  if (codepoint <= 0x7f) {
    out.push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7ff) {
    out.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  } else if (codepoint <= 0xffff) {
    out.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  } else {
    out.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  }
  return out;
}

bool is_terminal_zero_width_codepoint(std::uint32_t codepoint) {
  return is_grapheme_extend_codepoint(codepoint) ||
         is_combining_mark(codepoint) ||
         is_variation_selector(codepoint) ||
         codepoint == 0x200c ||
         codepoint == 0x200d ||
         codepoint == 0xfeff;
}

bool is_terminal_wide_codepoint(std::uint32_t codepoint) {
  return (codepoint >= 0x1100 &&
          (codepoint <= 0x115f || codepoint == 0x2329 || codepoint == 0x232a ||
           (codepoint >= 0x2e80 && codepoint <= 0xa4cf && codepoint != 0x303f) ||
           (codepoint >= 0xac00 && codepoint <= 0xd7a3) ||
           (codepoint >= 0xf900 && codepoint <= 0xfaff) ||
           (codepoint >= 0xfe10 && codepoint <= 0xfe19) ||
           (codepoint >= 0xfe30 && codepoint <= 0xfe6f) ||
           (codepoint >= 0xff00 && codepoint <= 0xff60) ||
           (codepoint >= 0xffe0 && codepoint <= 0xffe6))) ||
         is_emoji_presentation_candidate(codepoint);
}

bool is_terminal_regional_indicator_codepoint(std::uint32_t codepoint) {
  return codepoint >= 0x1f1e6 && codepoint <= 0x1f1ff;
}

int terminal_codepoint_width(std::uint32_t codepoint) {
  if (codepoint == 0 || codepoint < 0x20 || (codepoint >= 0x7f && codepoint < 0xa0)) {
    return 0;
  }
  if (is_terminal_zero_width_codepoint(codepoint)) {
    return 0;
  }
  return is_terminal_wide_codepoint(codepoint) ? 2 : 1;
}

int terminal_grapheme_width(std::string_view text) {
  return cluster_width_for_codepoints(decoded_codepoints(text));
}

bool terminal_codepoint_extends_previous_grapheme(
    std::string_view previous_grapheme,
    std::uint32_t codepoint) {
  const auto previous_codepoints = decoded_codepoints(previous_grapheme);
  if (previous_codepoints.empty()) {
    return false;
  }
  return should_extend_current_cluster(previous_codepoints, codepoint);
}

std::vector<TerminalGraphemeCluster> terminal_grapheme_clusters_from_text(
    std::string_view text) {
  std::vector<TerminalGraphemeCluster> clusters;
  std::string current_text;
  std::vector<std::uint32_t> current_codepoints;

  const auto flush_current = [&]() {
    if (current_text.empty()) {
      return;
    }
    clusters.push_back(TerminalGraphemeCluster{
        std::move(current_text),
        cluster_width_for_codepoints(current_codepoints)});
    current_text.clear();
    current_codepoints.clear();
  };

  for (std::size_t index = 0; index < text.size();) {
    const auto decoded = decode_utf8_codepoint(text, index);
    if (!decoded) {
      break;
    }

    const auto bytes = decoded->valid ? decoded->bytes : std::string_view{"?"};
    const auto codepoint = decoded->valid ? decoded->codepoint : kReplacementCodepoint;

    if (!should_extend_current_cluster(current_codepoints, codepoint)) {
      flush_current();
    }

    current_text.append(bytes);
    current_codepoints.push_back(codepoint);
    index += std::max<std::size_t>(1, decoded->bytes.size());
  }

  flush_current();
  return clusters;
}

std::vector<TerminalTextCell> terminal_text_cells_from_text(
    std::string_view text,
    std::size_t width) {
  std::vector<TerminalTextCell> cells(width, TerminalTextCell{" ", TerminalCellWidth::Narrow});
  std::size_t column = 0;

  for (const auto& cluster : terminal_grapheme_clusters_from_text(text)) {
    if (column >= width) {
      break;
    }

    if (cluster.width == 0) {
      append_zero_width_to_previous(cells, cluster.text);
      continue;
    }

    if (column + static_cast<std::size_t>(cluster.width) > width) {
      break;
    }

    cells[column].text = cluster.text;
    cells[column].width =
        cluster.width == 2 ? TerminalCellWidth::WideLeading : TerminalCellWidth::Narrow;
    if (cluster.width == 2 && column + 1 < width) {
      cells[column + 1] = TerminalTextCell{"", TerminalCellWidth::WideContinuation};
    }
    column += static_cast<std::size_t>(cluster.width);
  }

  return cells;
}

std::string sanitize_utf8_boundaries(std::string_view value) {
  std::string sanitized;
  sanitized.reserve(value.size());

  for (std::size_t index = 0; index < value.size();) {
    const auto decoded = decode_utf8_codepoint(value, index);
    if (!decoded) {
      break;
    }

    if (decoded->valid) {
      sanitized.append(decoded->bytes);
    }
    index += std::max<std::size_t>(1, decoded->bytes.size());
  }

  return sanitized;
}

}  // namespace wmux
