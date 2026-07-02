#include "wmux/terminal_input.hpp"

#include <cassert>
#include <string_view>
#include <vector>

namespace {

std::vector<wmux::TerminalInputEvent> decode(std::string_view bytes, bool mouse = true) {
  wmux::TerminalInputDecoderState state;
  wmux::TerminalInputDecoderOptions options;
  options.mouse_enabled = mouse;
  return wmux::decode_terminal_input(state, bytes, options).events;
}

void decodes_printable_and_control_keys() {
  const auto printable = decode("a");
  assert(printable.size() == 1);
  assert(printable[0].kind == wmux::TerminalInputEventKind::Key);
  assert(printable[0].key.key == wmux::Key::Char);
  assert(printable[0].key.character == 'a');
  assert(printable[0].key.printable);
  assert(printable[0].encoded_input == "a");

  const auto ctrl_b = decode(std::string_view{"\x02", 1});
  assert(ctrl_b.size() == 1);
  assert(ctrl_b[0].key.key == wmux::Key::Char);
  assert(ctrl_b[0].key.character == 'b');
  assert(wmux::has_modifier(ctrl_b[0].key.modifiers, wmux::KeyModifier::Ctrl));
}

void decodes_navigation_sequences() {
  const auto up = decode("\x1b[A");
  assert(up.size() == 1);
  assert(up[0].key.key == wmux::Key::Up);
  assert(up[0].key.raw_debug);

  const auto ctrl_left = decode("\x1b[1;5D");
  assert(ctrl_left.size() == 1);
  assert(ctrl_left[0].key.key == wmux::Key::Left);
  assert(wmux::has_modifier(ctrl_left[0].key.modifiers, wmux::KeyModifier::Ctrl));

  const auto page_down = decode("\x1b[6~");
  assert(page_down.size() == 1);
  assert(page_down[0].key.key == wmux::Key::PageDown);
}

void decodes_function_and_alt_keys() {
  const auto f1 = decode("\x1bOP");
  assert(f1.size() == 1);
  assert(f1[0].key.key == wmux::Key::Function);
  assert(f1[0].key.function_number == 1);

  const auto f5 = decode("\x1b[15~");
  assert(f5.size() == 1);
  assert(f5[0].key.key == wmux::Key::Function);
  assert(f5[0].key.function_number == 5);

  const auto alt_q = decode("\x1bq");
  assert(alt_q.size() == 1);
  assert(alt_q[0].key.key == wmux::Key::Char);
  assert(alt_q[0].key.character == 'q');
  assert(wmux::has_modifier(alt_q[0].key.modifiers, wmux::KeyModifier::Alt));
}

void decodes_mouse_and_paste_events() {
  const auto mouse = decode("\x1b[<0;10;4M");
  assert(mouse.size() == 1);
  assert(mouse[0].kind == wmux::TerminalInputEventKind::Mouse);
  assert(mouse[0].mouse.column == 10);
  assert(mouse[0].mouse.row == 4);

  const auto wheel = decode("\x1b[<65;10;4M");
  assert(wheel.size() == 1);
  assert(wheel[0].kind == wmux::TerminalInputEventKind::Mouse);
  assert(wheel[0].mouse.button == wmux::MouseButton::WheelDown);
  assert(wheel[0].mouse.action == wmux::MouseAction::Wheel);

  const auto disabled = decode("\x1b[<0;10;4M", false);
  for (const auto& event : disabled) {
    assert(event.kind != wmux::TerminalInputEventKind::Mouse);
  }

  const auto paste = decode("\x1b[200~hello\r\nworld\x1b[201~");
  assert(paste.size() == 1);
  assert(paste[0].kind == wmux::TerminalInputEventKind::Paste);
  assert(paste[0].paste.bytes == "hello\r\nworld");
  assert(paste[0].paste.bytes_len == paste[0].paste.bytes.size());
}

void keeps_slow_escape_pending_until_flushed() {
  wmux::TerminalInputDecoderState state;
  wmux::TerminalInputDecoderOptions options;
  const auto partial = wmux::decode_terminal_input(state, "\x1b", options);
  assert(partial.events.empty());
  assert(partial.has_pending);
  assert(wmux::terminal_input_decoder_has_pending_escape(state));

  const auto flushed = wmux::flush_terminal_input_decoder(state);
  assert(flushed.events.size() == 1);
  assert(flushed.events[0].key.key == wmux::Key::Escape);
  assert(!flushed.has_pending);
}

void invalid_escape_sequences_do_not_crash() {
  const auto invalid = decode("\x1b[999x");
  assert(invalid.size() == 1);
  assert(invalid[0].key.key == wmux::Key::Unknown);
  assert(invalid[0].key.raw_debug);
}

}  // namespace

void run_terminal_input_tests() {
  decodes_printable_and_control_keys();
  decodes_navigation_sequences();
  decodes_function_and_alt_keys();
  decodes_mouse_and_paste_events();
  keeps_slow_escape_pending_until_flushed();
  invalid_escape_sequences_do_not_crash();
}
