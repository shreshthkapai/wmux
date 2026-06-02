#include "wmux/command_mode.hpp"

#include <cstddef>
#include <utility>

namespace wmux {
namespace {

constexpr std::size_t kMaxCommandPromptBytes = 4096;

bool printable_ascii(char byte) {
  return byte >= ' ' && byte <= '~';
}

}  // namespace

CommandPromptEvent start_command_prompt(CommandPromptState& state) {
  state.active = true;
  state.command.clear();
  state.submitted_command.clear();
  state.status_message.clear();
  return CommandPromptEvent::Redraw;
}

CommandPromptEvent handle_command_prompt_byte(CommandPromptState& state, char byte) {
  if (!state.active) {
    return CommandPromptEvent::None;
  }

  if (byte == '\x1b') {
    state.active = false;
    state.command.clear();
    state.submitted_command.clear();
    state.status_message.clear();
    return CommandPromptEvent::Cancelled;
  }

  if (byte == '\r' || byte == '\n') {
    const auto submitted = state.command;
    state.active = false;
    state.command.clear();
    state.submitted_command = submitted;
    if (submitted.empty()) {
      state.status_message = "wmux: empty command";
    } else {
      state.status_message.clear();
    }
    return CommandPromptEvent::Submitted;
  }

  if (byte == '\b' || byte == '\x7f') {
    if (!state.command.empty()) {
      state.command.pop_back();
      return CommandPromptEvent::Redraw;
    }
    return CommandPromptEvent::None;
  }

  if (!printable_ascii(byte)) {
    return CommandPromptEvent::None;
  }

  if (state.command.size() >= kMaxCommandPromptBytes) {
    state.status_message = "wmux: command is too long";
    return CommandPromptEvent::Redraw;
  }

  state.command.push_back(byte);
  state.status_message.clear();
  return CommandPromptEvent::Redraw;
}

std::string command_prompt_status_text(const CommandPromptState& state) {
  if (state.active) {
    return ":" + state.command;
  }

  return state.status_message;
}

CommandTextParseResult parse_command_prompt_text(std::string_view text) {
  CommandTextParseResult result;

  std::size_t index = 0;
  while (index < text.size()) {
    while (index < text.size() && (text[index] == ' ' || text[index] == '\t')) {
      ++index;
    }
    if (index >= text.size()) {
      break;
    }

    std::string token;
    bool quoted = false;
    char quote = '\0';

    while (index < text.size()) {
      const char byte = text[index];
      if (!quoted && (byte == ' ' || byte == '\t')) {
        break;
      }

      if ((byte == '\'' || byte == '"') && !quoted) {
        quoted = true;
        quote = byte;
        ++index;
        continue;
      }

      if (quoted && byte == quote) {
        quoted = false;
        quote = '\0';
        ++index;
        continue;
      }

      if (byte == '\\' && index + 1 < text.size()) {
        token.push_back(text[index + 1]);
        index += 2;
        continue;
      }

      token.push_back(byte);
      ++index;
    }

    if (quoted) {
      result.error = "wmux: unterminated quote";
      return result;
    }

    result.args.push_back(std::move(token));
  }

  result.ok = true;
  return result;
}

}  // namespace wmux
