#include "wmux/paste_buffer.hpp"

#include "wmux/resource_limits.hpp"

namespace wmux {

std::string normalize_paste_text_for_terminal(std::string_view text) {
  std::string normalized;
  normalized.reserve(text.size());

  for (std::size_t index = 0; index < text.size(); ++index) {
    const char ch = text[index];
    if (ch == '\r') {
      normalized.push_back('\r');
      if (index + 1 < text.size() && text[index + 1] == '\n') {
        ++index;
      }
      continue;
    }

    if (ch == '\n') {
      normalized.push_back('\r');
      continue;
    }

    normalized.push_back(ch);
  }

  return normalized;
}

std::string bounded_paste_buffer_text(std::string_view text) {
  if (text.size() <= kMaxPasteBufferBytes) {
    return std::string{text};
  }

  return std::string{text.substr(0, kMaxPasteBufferBytes)};
}

}  // namespace wmux
