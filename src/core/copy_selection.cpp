#include "wmux/copy_selection.hpp"

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <vector>

namespace wmux {
namespace {

bool operator<(const CopySelectionPoint& left, const CopySelectionPoint& right) {
  if (left.line != right.line) {
    return left.line < right.line;
  }
  return left.column < right.column;
}

std::size_t total_line_count(const PtyOutputSnapshot& snapshot) {
  if (snapshot.screen.alternate_screen) {
    return std::max(snapshot.screen.line_snapshots.size(), snapshot.screen.lines.size());
  }

  return std::max(snapshot.scrollback.line_snapshots.size(), snapshot.scrollback.lines.size()) +
         std::max(snapshot.screen.line_snapshots.size(), snapshot.screen.lines.size());
}

TerminalLineSnapshot line_snapshot_at(const PtyOutputSnapshot& snapshot, std::size_t index) {
  if (snapshot.screen.alternate_screen) {
    if (index < snapshot.screen.line_snapshots.size()) {
      return snapshot.screen.line_snapshots[index];
    }
    if (index < snapshot.screen.lines.size()) {
      return TerminalLineSnapshot{snapshot.screen.lines[index], false};
    }
    return {};
  }

  const auto scrollback_count =
      std::max(snapshot.scrollback.line_snapshots.size(), snapshot.scrollback.lines.size());
  if (index < scrollback_count) {
    if (index < snapshot.scrollback.line_snapshots.size()) {
      return snapshot.scrollback.line_snapshots[index];
    }
    return TerminalLineSnapshot{snapshot.scrollback.lines[index], false};
  }

  const auto screen_index = index - scrollback_count;
  if (screen_index < snapshot.screen.line_snapshots.size()) {
    return snapshot.screen.line_snapshots[screen_index];
  }

  if (screen_index < snapshot.screen.lines.size()) {
    return TerminalLineSnapshot{snapshot.screen.lines[screen_index], false};
  }

  return {};
}

void trim_trailing_horizontal_space(std::string& value) {
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
    value.pop_back();
  }
}

bool is_utf8_continuation(unsigned char byte) {
  return (byte & 0xc0) == 0x80;
}

bool is_combining_codepoint(std::uint32_t codepoint) {
  return (codepoint >= 0x0300 && codepoint <= 0x036f) ||
         (codepoint >= 0x1ab0 && codepoint <= 0x1aff) ||
         (codepoint >= 0x1dc0 && codepoint <= 0x1dff) ||
         (codepoint >= 0x20d0 && codepoint <= 0x20ff) ||
         (codepoint >= 0xfe20 && codepoint <= 0xfe2f);
}

bool is_wide_codepoint(std::uint32_t codepoint) {
  return (codepoint >= 0x1100 &&
          (codepoint <= 0x115f || codepoint == 0x2329 || codepoint == 0x232a ||
           (codepoint >= 0x2e80 && codepoint <= 0xa4cf && codepoint != 0x303f) ||
           (codepoint >= 0xac00 && codepoint <= 0xd7a3) ||
           (codepoint >= 0xf900 && codepoint <= 0xfaff) ||
           (codepoint >= 0xfe10 && codepoint <= 0xfe19) ||
           (codepoint >= 0xfe30 && codepoint <= 0xfe6f) ||
           (codepoint >= 0xff00 && codepoint <= 0xff60) ||
           (codepoint >= 0xffe0 && codepoint <= 0xffe6) ||
           (codepoint >= 0x1f300 && codepoint <= 0x1faff)));
}

std::vector<std::string> cells_from_text(std::string_view value, std::size_t width) {
  std::vector<std::string> cells;
  cells.reserve(width);

  for (std::size_t index = 0; index < value.size() && cells.size() < width;) {
    const auto byte = static_cast<unsigned char>(value[index]);
    std::size_t length = 1;
    std::uint32_t codepoint = byte;

    if (byte < 0x80) {
      length = 1;
    } else if (byte >= 0xc2 && byte <= 0xdf && index + 1 < value.size() &&
               is_utf8_continuation(static_cast<unsigned char>(value[index + 1]))) {
      length = 2;
      codepoint = static_cast<std::uint32_t>(
          ((byte & 0x1f) << 6) |
          (static_cast<unsigned char>(value[index + 1]) & 0x3f));
    } else if (byte >= 0xe0 && byte <= 0xef && index + 2 < value.size() &&
               is_utf8_continuation(static_cast<unsigned char>(value[index + 1])) &&
               is_utf8_continuation(static_cast<unsigned char>(value[index + 2]))) {
      length = 3;
      codepoint = static_cast<std::uint32_t>(
          ((byte & 0x0f) << 12) |
          ((static_cast<unsigned char>(value[index + 1]) & 0x3f) << 6) |
          (static_cast<unsigned char>(value[index + 2]) & 0x3f));
    } else if (byte >= 0xf0 && byte <= 0xf4 && index + 3 < value.size() &&
               is_utf8_continuation(static_cast<unsigned char>(value[index + 1])) &&
               is_utf8_continuation(static_cast<unsigned char>(value[index + 2])) &&
               is_utf8_continuation(static_cast<unsigned char>(value[index + 3]))) {
      length = 4;
      codepoint = static_cast<std::uint32_t>(
          ((byte & 0x07) << 18) |
          ((static_cast<unsigned char>(value[index + 1]) & 0x3f) << 12) |
          ((static_cast<unsigned char>(value[index + 2]) & 0x3f) << 6) |
          (static_cast<unsigned char>(value[index + 3]) & 0x3f));
    } else {
      ++index;
      continue;
    }

    const auto glyph = std::string{value.substr(index, length)};
    index += length;
    if (is_combining_codepoint(codepoint)) {
      if (!cells.empty() && !cells.back().empty()) {
        cells.back() += glyph;
      }
      continue;
    }

    cells.push_back(glyph);
    if (is_wide_codepoint(codepoint) && cells.size() < width) {
      cells.emplace_back();
    }
  }

  while (cells.size() < width) {
    cells.emplace_back(" ");
  }
  if (cells.size() > width) {
    cells.resize(width);
  }
  return cells;
}

std::vector<std::string> normalized_line_cells(const TerminalLineSnapshot& line, std::size_t width) {
  auto cells = line.cells.empty() ? cells_from_text(line.text, width) : line.cells;
  if (cells.size() < width) {
    cells.insert(cells.end(), width - cells.size(), " ");
  }
  if (cells.size() > width) {
    cells.resize(width);
  }
  return cells;
}

std::string sanitize_utf8_boundaries(std::string_view value) {
  std::string sanitized;
  sanitized.reserve(value.size());

  for (std::size_t index = 0; index < value.size();) {
    const auto byte = static_cast<unsigned char>(value[index]);
    if (byte < 0x80) {
      sanitized.push_back(value[index]);
      ++index;
      continue;
    }

    std::size_t length = 0;
    if (byte >= 0xc2 && byte <= 0xdf) {
      length = 2;
    } else if (byte >= 0xe0 && byte <= 0xef) {
      length = 3;
    } else if (byte >= 0xf0 && byte <= 0xf4) {
      length = 4;
    } else {
      ++index;
      continue;
    }

    if (index + length > value.size()) {
      break;
    }

    bool valid = true;
    for (std::size_t offset = 1; offset < length; ++offset) {
      if (!is_utf8_continuation(static_cast<unsigned char>(value[index + offset]))) {
        valid = false;
        break;
      }
    }

    if (!valid) {
      ++index;
      continue;
    }

    sanitized.append(value.substr(index, length));
    index += length;
  }

  return sanitized;
}

}  // namespace

std::string extract_copy_selection_text(
    const PtyOutputSnapshot& snapshot,
    CopySelectionRange range,
    std::size_t line_width) {
  const auto total_lines = total_line_count(snapshot);
  if (total_lines == 0 || line_width == 0) {
    return {};
  }

  auto first = range.anchor;
  auto last = range.cursor;
  if (last < first) {
    std::swap(first, last);
  }

  first.line = std::min(first.line, total_lines - 1);
  last.line = std::min(last.line, total_lines - 1);
  first.column = std::min(first.column, line_width - 1);
  last.column = std::min(last.column, line_width - 1);

  std::string copied;
  copied.reserve((last.line - first.line + 1) * (line_width + 2));

  for (auto line_index = first.line; line_index <= last.line; ++line_index) {
    const auto line = line_snapshot_at(snapshot, line_index);
    const auto cells = normalized_line_cells(line, line_width);
    const auto start_column = line_index == first.line ? first.column : std::size_t{0};
    const auto end_column = line_index == last.line ? last.column : line_width - 1;
    std::string segment;
    if (start_column <= end_column && start_column < cells.size()) {
      const auto last_column = std::min(end_column, cells.size() - 1);
      for (auto column = start_column; column <= last_column; ++column) {
        segment += cells[column];
      }
    }

    if (line_index != last.line) {
      trim_trailing_horizontal_space(segment);
      copied += segment;
      if (!line.wrapped) {
        copied.append("\r\n");
      }
    } else {
      copied += segment;
    }
  }

  trim_trailing_horizontal_space(copied);
  return sanitize_utf8_boundaries(copied);
}

}  // namespace wmux
