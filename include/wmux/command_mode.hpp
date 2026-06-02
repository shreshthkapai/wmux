#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace wmux {

enum class CommandPromptEvent {
  None,
  Redraw,
  Cancelled,
  Submitted,
};

struct CommandPromptState {
  bool active{false};
  std::string command;
  std::string submitted_command;
  std::string status_message;
};

struct CommandTextParseResult {
  bool ok{false};
  std::vector<std::string> args;
  std::string error;
};

CommandPromptEvent start_command_prompt(CommandPromptState& state);
CommandPromptEvent handle_command_prompt_byte(CommandPromptState& state, char byte);
std::string command_prompt_status_text(const CommandPromptState& state);
CommandTextParseResult parse_command_prompt_text(std::string_view text);

}  // namespace wmux
