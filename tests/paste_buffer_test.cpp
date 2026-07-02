#include "wmux/paste_buffer.hpp"
#include "wmux/resource_limits.hpp"

#include <cassert>
#include <string>

namespace {

void preserves_single_line_text() {
  assert(wmux::normalize_paste_text_for_terminal("echo hello") == "echo hello");
}

void converts_lf_to_terminal_enter() {
  assert(wmux::normalize_paste_text_for_terminal("one\ntwo\n") == "one\rtwo\r");
}

void converts_crlf_to_single_terminal_enter() {
  assert(wmux::normalize_paste_text_for_terminal("one\r\ntwo\r\n") == "one\rtwo\r");
}

void normalizes_mixed_line_endings() {
  assert(wmux::normalize_paste_text_for_terminal("a\r\nb\nc\rd") == "a\rb\rc\rd");
}

void bounds_paste_buffer_text() {
  const std::string large(wmux::kMaxPasteBufferBytes + 32, 'x');
  const auto bounded = wmux::bounded_paste_buffer_text(large);
  assert(bounded.size() == wmux::kMaxPasteBufferBytes);
  assert(wmux::bounded_paste_buffer_text("small") == "small");
}

void creates_metadata_rich_bounded_buffer() {
  const std::string large(wmux::kMaxPasteBufferBytes + 32, 'x');
  const auto buffer =
      wmux::make_paste_buffer(42, large, wmux::PasteBufferSource::CopyMode);
  assert(buffer.id == 42);
  assert(buffer.text.size() == wmux::kMaxPasteBufferBytes);
  assert(buffer.original_bytes == large.size());
  assert(buffer.truncated);
  assert(buffer.source == wmux::PasteBufferSource::CopyMode);
  assert(wmux::paste_buffer_source_name(buffer.source) == "copy-mode");
}

void wraps_only_when_bracketed_paste_is_enabled() {
  assert(wmux::prepare_paste_text_for_terminal("one\ntwo", false) == "one\rtwo");
  assert(wmux::prepare_paste_text_for_terminal("one\ntwo", true) ==
         "\x1b[200~one\rtwo\x1b[201~");
}

}  // namespace

void run_paste_buffer_tests() {
  preserves_single_line_text();
  converts_lf_to_terminal_enter();
  converts_crlf_to_single_terminal_enter();
  normalizes_mixed_line_endings();
  bounds_paste_buffer_text();
  creates_metadata_rich_bounded_buffer();
  wraps_only_when_bracketed_paste_is_enabled();
}
