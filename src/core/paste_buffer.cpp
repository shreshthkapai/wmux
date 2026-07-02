#include "wmux/paste_buffer.hpp"

#include "wmux/resource_limits.hpp"

namespace wmux {
namespace {

constexpr std::string_view kBracketedPasteStart = "\x1b[200~";
constexpr std::string_view kBracketedPasteEnd = "\x1b[201~";

}  // namespace

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

std::string prepare_paste_text_for_terminal(std::string_view text, bool bracketed_paste) {
  auto normalized = normalize_paste_text_for_terminal(text);
  if (!bracketed_paste || normalized.empty()) {
    return normalized;
  }

  std::string wrapped;
  wrapped.reserve(kBracketedPasteStart.size() + normalized.size() + kBracketedPasteEnd.size());
  wrapped.append(kBracketedPasteStart);
  wrapped += normalized;
  wrapped.append(kBracketedPasteEnd);
  return wrapped;
}

std::string bounded_paste_buffer_text(std::string_view text) {
  return bounded_paste_buffer_text(text, kMaxPasteBufferBytes);
}

std::string bounded_paste_buffer_text(std::string_view text, std::size_t max_bytes) {
  if (text.size() <= max_bytes) {
    return std::string{text};
  }

  return std::string{text.substr(0, max_bytes)};
}

PasteBuffer make_paste_buffer(
    BufferId id,
    std::string_view text,
    PasteBufferSource source) {
  return make_paste_buffer(id, text, source, kMaxPasteBufferBytes);
}

PasteBuffer make_paste_buffer(
    BufferId id,
    std::string_view text,
    PasteBufferSource source,
    std::size_t max_bytes) {
  PasteBuffer buffer;
  buffer.id = id;
  buffer.text = bounded_paste_buffer_text(text, max_bytes);
  buffer.created_at = std::chrono::steady_clock::now();
  buffer.source = source;
  buffer.original_bytes = text.size();
  buffer.truncated = text.size() > buffer.text.size();
  return buffer;
}

std::string_view paste_buffer_source_name(PasteBufferSource source) {
  switch (source) {
    case PasteBufferSource::CopyMode:
      return "copy-mode";
    case PasteBufferSource::Command:
      return "command";
    case PasteBufferSource::Clipboard:
      return "clipboard";
    case PasteBufferSource::Unknown:
      return "unknown";
  }

  return "unknown";
}

}  // namespace wmux
