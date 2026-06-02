#include "wmux/mouse_input.hpp"

#include <cassert>
#include <string_view>

void run_mouse_input_tests() {
  assert(wmux::enable_mouse_reporting_sequence().find("\x1b[?1006h") !=
         std::string_view::npos);
  assert(wmux::disable_mouse_reporting_sequence().find("\x1b[?1006l") !=
         std::string_view::npos);

  assert(wmux::is_sgr_mouse_sequence_prefix("\x1b"));
  assert(wmux::is_sgr_mouse_sequence_prefix("\x1b["));
  assert(wmux::is_sgr_mouse_sequence_prefix("\x1b[<"));
  assert(wmux::is_sgr_mouse_sequence_prefix("\x1b[<0;"));
  assert(!wmux::is_sgr_mouse_sequence_prefix("\x1b[A"));

  const auto press = wmux::parse_sgr_mouse_sequence("\x1b[<0;12;8M");
  assert(press.status == wmux::MouseParseStatus::Parsed);
  assert(press.bytes_consumed == 10);
  assert(press.event.has_value());
  assert(press.event->column == 12);
  assert(press.event->row == 8);
  assert(press.event->button_code == 0);
  assert(press.event->button == wmux::MouseButton::Left);
  assert(press.event->action == wmux::MouseAction::Press);

  const auto release = wmux::parse_sgr_mouse_sequence("\x1b[<0;12;8m");
  assert(release.status == wmux::MouseParseStatus::Parsed);
  assert(release.event->button == wmux::MouseButton::Release);
  assert(release.event->action == wmux::MouseAction::Release);

  const auto drag = wmux::parse_sgr_mouse_sequence("\x1b[<32;20;9M");
  assert(drag.status == wmux::MouseParseStatus::Parsed);
  assert(drag.event->button == wmux::MouseButton::Left);
  assert(drag.event->action == wmux::MouseAction::Drag);

  const auto wheel = wmux::parse_sgr_mouse_sequence("\x1b[<64;20;9M");
  assert(wheel.status == wmux::MouseParseStatus::Parsed);
  assert(wheel.event->button == wmux::MouseButton::WheelUp);
  assert(wheel.event->action == wmux::MouseAction::Wheel);

  const auto partial = wmux::parse_sgr_mouse_sequence("\x1b[<0;12;");
  assert(partial.status == wmux::MouseParseStatus::Incomplete);

  const auto malformed = wmux::parse_sgr_mouse_sequence("\x1b[<x");
  assert(malformed.status == wmux::MouseParseStatus::Invalid);

  const auto invalid = wmux::parse_sgr_mouse_sequence("\x1b[A");
  assert(invalid.status == wmux::MouseParseStatus::Invalid);
}
