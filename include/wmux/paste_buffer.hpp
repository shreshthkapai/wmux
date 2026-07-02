#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace wmux {

using BufferId = std::uint64_t;

enum class PasteBufferSource {
  CopyMode,
  Command,
  Clipboard,
  Unknown,
};

struct PasteBuffer {
  BufferId id{0};
  std::string text;
  std::chrono::steady_clock::time_point created_at{};
  PasteBufferSource source{PasteBufferSource::Unknown};
  std::size_t original_bytes{0};
  bool truncated{false};
};

std::string normalize_paste_text_for_terminal(std::string_view text);
std::string prepare_paste_text_for_terminal(std::string_view text, bool bracketed_paste);
std::string bounded_paste_buffer_text(std::string_view text);
std::string bounded_paste_buffer_text(std::string_view text, std::size_t max_bytes);
PasteBuffer make_paste_buffer(
    BufferId id,
    std::string_view text,
    PasteBufferSource source);
PasteBuffer make_paste_buffer(
    BufferId id,
    std::string_view text,
    PasteBufferSource source,
    std::size_t max_bytes);
std::string_view paste_buffer_source_name(PasteBufferSource source);

}  // namespace wmux
