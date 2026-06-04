#include "wmux/copy_selection.hpp"

#include <algorithm>
#include <string_view>

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

std::string normalized_line_text(TerminalLineSnapshot line, std::size_t width) {
  if (line.text.size() < width) {
    line.text.append(width - line.text.size(), ' ');
  }
  if (line.text.size() > width) {
    line.text.resize(width);
  }
  return line.text;
}

bool is_utf8_continuation(unsigned char byte) {
  return (byte & 0xc0) == 0x80;
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
    const auto text = normalized_line_text(line, line_width);
    const auto start_column = line_index == first.line ? first.column : std::size_t{0};
    const auto end_column = line_index == last.line ? last.column : line_width - 1;
    if (start_column <= end_column && start_column < text.size()) {
      copied.append(
          std::string_view{text}.substr(start_column, end_column - start_column + 1));
    }

    if (line_index != last.line && !line.wrapped) {
      trim_trailing_horizontal_space(copied);
      copied.append("\r\n");
    }
  }

  trim_trailing_horizontal_space(copied);
  return sanitize_utf8_boundaries(copied);
}

}  // namespace wmux
