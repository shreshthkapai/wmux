#include "wmux/command_mode.hpp"

#include <cassert>

void run_command_mode_tests() {
  wmux::CommandPromptState state;
  assert(wmux::start_command_prompt(state) == wmux::CommandPromptEvent::Redraw);
  assert(state.active);
  assert(wmux::command_prompt_status_text(state) == ":");

  assert(wmux::handle_command_prompt_byte(state, 'r') == wmux::CommandPromptEvent::Redraw);
  assert(wmux::handle_command_prompt_byte(state, 'e') == wmux::CommandPromptEvent::Redraw);
  assert(wmux::handle_command_prompt_byte(state, 'n') == wmux::CommandPromptEvent::Redraw);
  assert(wmux::command_prompt_status_text(state) == ":ren");

  assert(wmux::handle_command_prompt_byte(state, '\b') == wmux::CommandPromptEvent::Redraw);
  assert(wmux::command_prompt_status_text(state) == ":re");

  assert(wmux::handle_command_prompt_byte(state, '\r') == wmux::CommandPromptEvent::Submitted);
  assert(!state.active);
  assert(state.submitted_command == "re");
  assert(wmux::command_prompt_status_text(state).empty());

  assert(wmux::start_command_prompt(state) == wmux::CommandPromptEvent::Redraw);
  assert(wmux::handle_command_prompt_byte(state, '\x1b') ==
         wmux::CommandPromptEvent::Cancelled);
  assert(!state.active);
  assert(wmux::command_prompt_status_text(state).empty());

  const auto parsed = wmux::parse_command_prompt_text("new-window -n logs");
  assert(parsed.ok);
  assert(parsed.args.size() == 3);
  assert(parsed.args[0] == "new-window");
  assert(parsed.args[1] == "-n");
  assert(parsed.args[2] == "logs");

  const auto quoted = wmux::parse_command_prompt_text("rename-window \"agent logs\"");
  assert(quoted.ok);
  assert(quoted.args.size() == 2);
  assert(quoted.args[1] == "agent logs");

  const auto bad_quote = wmux::parse_command_prompt_text("rename-window \"logs");
  assert(!bad_quote.ok);
  assert(bad_quote.error == "wmux: unterminated quote");
}
