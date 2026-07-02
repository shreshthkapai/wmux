#include "wmux/status_line.hpp"

#include <cassert>
#include <chrono>

namespace {

void temporary_messages_expire() {
  wmux::StatusState state;
  const auto now = std::chrono::steady_clock::now();

  wmux::status_set_temporary(state, "hello\nworld", now, std::chrono::milliseconds{10});
  assert(state.temporary_message);
  assert(state.temporary_message->text == "hello world");
  assert(wmux::status_has_visible_temporary(state, now));
  assert(!wmux::status_expire_temporary(state, now + std::chrono::milliseconds{5}));
  assert(wmux::status_expire_temporary(state, now + std::chrono::milliseconds{10}));
  assert(!state.temporary_message);
}

void persistent_messages_do_not_expire() {
  wmux::StatusState state;
  const auto now = std::chrono::steady_clock::now();

  wmux::status_set_persistent(state, ":rename-window logs", now);
  assert(state.temporary_message);
  assert(!state.temporary_message->expires);
  assert(!wmux::status_expire_temporary(state, now + std::chrono::hours{1}));
  assert(state.temporary_message);
}

void formats_context_message_and_right_segment() {
  wmux::StatusState state;
  state.permanent_left.text = " wmux [main] window 1:logs pane 3";
  state.permanent_right.text = "mode:normal mouse:on";
  wmux::status_set_temporary(
      state,
      "wmux: created window",
      std::chrono::steady_clock::now());

  const auto line = wmux::format_status_line(state, 90);
  assert(line.find("wmux [main]") != std::string::npos);
  assert(line.find("wmux: created window") != std::string::npos);
  assert(line.find("mode:normal mouse:on") != std::string::npos);
}

void preserves_bottom_context_with_generous_right_gap() {
  wmux::StatusState state;
  state.permanent_left.text = "[main] 0:editor* 1:logs 2:server";
  state.permanent_right.text = "\"pane 42\" 22:27 02-Jul-26";

  const auto line = wmux::format_status_line(state, 40);
  assert(line.size() == 40);
  assert(line.find("[main] 0:editor*") != std::string::npos);
  assert(line.find("        \"pane") != std::string::npos);
}

void reports_mode_names() {
  assert(wmux::status_line_mode_name(wmux::StatusLineMode::Prefix) == "prefix");
  assert(wmux::status_line_mode_name(wmux::StatusLineMode::Copy) == "copy");
  assert(wmux::status_line_mode_name(wmux::StatusLineMode::MouseDrag) == "mouse");
}

}  // namespace

void run_status_line_tests() {
  temporary_messages_expire();
  persistent_messages_do_not_expire();
  formats_context_message_and_right_segment();
  preserves_bottom_context_with_generous_right_gap();
  reports_mode_names();
}
