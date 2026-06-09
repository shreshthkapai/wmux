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

}  // namespace

void run_paste_buffer_tests() {
  preserves_single_line_text();
  converts_lf_to_terminal_enter();
  converts_crlf_to_single_terminal_enter();
  normalizes_mixed_line_endings();
  bounds_paste_buffer_text();
}
