#include "wmux/attach_input_mode.hpp"

#include "wmux/attach_keymap.hpp"

#include <algorithm>

namespace wmux {
namespace {

AttachInputAction send_input_action(std::string input) {
  AttachInputAction action;
  action.kind = AttachInputActionKind::SendInput;
  action.text = std::move(input);
  return action;
}

AttachInputAction command_action(std::string command) {
  AttachInputAction action;
  action.kind = AttachInputActionKind::Command;
  action.text = std::move(command);
  return action;
}

AttachInputAction status_action(std::string status) {
  AttachInputAction action;
  action.kind = AttachInputActionKind::Status;
  action.text = std::move(status);
  return action;
}

AttachInputAction command_mode_action(std::string command) {
  AttachInputAction action;
  action.kind = AttachInputActionKind::CommandMode;
  action.text = std::move(command);
  return action;
}

AttachInputAction scroll_action(AttachScrollAction scroll) {
  AttachInputAction action;
  action.kind = AttachInputActionKind::Scroll;
  action.scroll_action = scroll;
  return action;
}

AttachInputAction copy_mode_action(AttachCopyModeAction copy_mode) {
  AttachInputAction action;
  action.kind = AttachInputActionKind::CopyMode;
  action.copy_mode_action = copy_mode;
  return action;
}

AttachInputAction mouse_action(const MouseEvent& mouse) {
  AttachInputAction action;
  action.kind = AttachInputActionKind::Mouse;
  action.mouse = mouse;
  return action;
}

AttachInputAction paste_action() {
  AttachInputAction action;
  action.kind = AttachInputActionKind::Paste;
  return action;
}

AttachInputAction detach_action() {
  AttachInputAction action;
  action.kind = AttachInputActionKind::Detach;
  return action;
}

bool key_is_prefix(const TerminalInputEvent& event, char prefix_byte) {
  return event.kind == TerminalInputEventKind::Key && event.encoded_input.size() == 1 &&
         event.encoded_input[0] == prefix_byte;
}

AttachCopyModeAction copy_mode_key_action(const KeyEvent& key) {
  if (key.key == Key::Escape || (key.key == Key::Char && key.character == 'q' &&
                                 !has_modifier(key.modifiers, KeyModifier::Ctrl))) {
    return AttachCopyModeAction::Exit;
  }
  if (key.key == Key::Up || (key.key == Key::Char && key.character == 'k')) {
    return AttachCopyModeAction::CursorUp;
  }
  if (key.key == Key::Down || (key.key == Key::Char && key.character == 'j')) {
    return AttachCopyModeAction::CursorDown;
  }
  if (key.key == Key::Left || (key.key == Key::Char && key.character == 'h')) {
    return AttachCopyModeAction::CursorLeft;
  }
  if (key.key == Key::Right || (key.key == Key::Char && key.character == 'l')) {
    return AttachCopyModeAction::CursorRight;
  }
  if (key.key == Key::PageUp) {
    return AttachCopyModeAction::PageUp;
  }
  if (key.key == Key::PageDown) {
    return AttachCopyModeAction::PageDown;
  }
  if (key.key == Key::Home) {
    return AttachCopyModeAction::Home;
  }
  if (key.key == Key::End) {
    return AttachCopyModeAction::End;
  }
  if (key.key == Key::Char && key.character == ' ') {
    return AttachCopyModeAction::StartSelection;
  }
  if (key.key == Key::Enter) {
    return AttachCopyModeAction::CopySelection;
  }

  return AttachCopyModeAction::Enter;
}

bool copy_mode_action_is_meaningful(AttachCopyModeAction action) {
  return action != AttachCopyModeAction::Enter;
}

void enter_mode(AttachClientModeState& mode, AttachClientModeKind kind) {
  mode.kind = kind;
  mode.started_at = std::chrono::steady_clock::now();
}

std::vector<AttachInputAction> enter_confirm_prompt(
    AttachClientModeState& mode,
    std::string command,
    std::string prompt) {
  mode.confirm_command = std::move(command);
  mode.confirm_prompt = std::move(prompt);
  enter_mode(mode, AttachClientModeKind::ConfirmPrompt);
  return {status_action(mode.confirm_prompt)};
}

std::vector<AttachInputAction> handle_command_prompt_event(
    AttachClientModeState& mode,
    const TerminalInputEvent& event) {
  std::vector<AttachInputAction> actions;
  if (event.kind == TerminalInputEventKind::Paste) {
    for (const char byte : event.paste.bytes) {
      const auto prompt_event = handle_command_prompt_byte(mode.command_prompt, byte);
      if (prompt_event == CommandPromptEvent::Submitted) {
        actions.push_back(command_mode_action(mode.command_prompt.submitted_command));
        enter_mode(mode, AttachClientModeKind::Normal);
        return actions;
      }
      if (prompt_event == CommandPromptEvent::Cancelled) {
        enter_mode(mode, AttachClientModeKind::Normal);
        actions.push_back(status_action(""));
        return actions;
      }
    }
    actions.push_back(status_action(command_prompt_status_text(mode.command_prompt)));
    return actions;
  }

  if (event.kind != TerminalInputEventKind::Key) {
    return actions;
  }

  if (event.key.key == Key::Escape) {
    (void)handle_command_prompt_byte(mode.command_prompt, '\x1b');
    enter_mode(mode, AttachClientModeKind::Normal);
    actions.push_back(status_action(""));
    return actions;
  }

  if (event.encoded_input.empty()) {
    return actions;
  }

  for (const char byte : event.encoded_input) {
    const auto prompt_event = handle_command_prompt_byte(mode.command_prompt, byte);
    if (prompt_event == CommandPromptEvent::None) {
      continue;
    }
    if (prompt_event == CommandPromptEvent::Submitted) {
      actions.push_back(command_mode_action(mode.command_prompt.submitted_command));
      enter_mode(mode, AttachClientModeKind::Normal);
      return actions;
    }
    if (prompt_event == CommandPromptEvent::Cancelled) {
      enter_mode(mode, AttachClientModeKind::Normal);
      actions.push_back(status_action(""));
      return actions;
    }
    actions.push_back(status_action(command_prompt_status_text(mode.command_prompt)));
  }

  return actions;
}

std::vector<AttachInputAction> handle_prefix_event(
    AttachClientModeState& mode,
    const TerminalInputEvent& event,
    char prefix_byte,
    const AttachKeyBindingTable& key_bindings) {
  std::vector<AttachInputAction> actions;
  enter_mode(mode, AttachClientModeKind::Normal);

  if (event.kind == TerminalInputEventKind::Mouse) {
    actions.push_back(mouse_action(event.mouse));
    return actions;
  }

  if (event.kind == TerminalInputEventKind::Paste) {
    actions.push_back(send_input_action(std::string{1, prefix_byte} + event.paste.bytes));
    return actions;
  }

  const auto action = prefixed_attach_key_action(event.encoded_input, prefix_byte, key_bindings);
  switch (action.kind) {
    case AttachKeyActionKind::Detach:
      actions.push_back(detach_action());
      return actions;
    case AttachKeyActionKind::Command:
      if (action.command == "kill-pane") {
        return enter_confirm_prompt(mode, action.command, "kill-pane? (y/n)");
      }
      actions.push_back(command_action(action.command));
      return actions;
    case AttachKeyActionKind::Scroll:
      actions.push_back(scroll_action(action.scroll_action));
      return actions;
    case AttachKeyActionKind::CommandPrompt:
      start_command_prompt(mode.command_prompt);
      enter_mode(mode, AttachClientModeKind::CommandPrompt);
      actions.push_back(status_action(command_prompt_status_text(mode.command_prompt)));
      return actions;
    case AttachKeyActionKind::CopyMode:
      enter_mode(mode, AttachClientModeKind::CopyMode);
      actions.push_back(copy_mode_action(action.copy_mode_action));
      return actions;
    case AttachKeyActionKind::Paste:
      actions.push_back(paste_action());
      return actions;
    case AttachKeyActionKind::PassThrough:
      actions.push_back(send_input_action(action.input));
      return actions;
    case AttachKeyActionKind::None:
      actions.push_back(status_action("wmux: unknown keybind"));
      return actions;
  }

  return actions;
}

std::vector<AttachInputAction> handle_confirm_prompt_event(
    AttachClientModeState& mode,
    const TerminalInputEvent& event) {
  std::vector<AttachInputAction> actions;
  if (event.kind != TerminalInputEventKind::Key) {
    return actions;
  }

  const bool cancel = event.key.key == Key::Escape ||
                      (event.key.key == Key::Char &&
                       (event.key.character == 'n' || event.key.character == 'N' ||
                        event.key.character == 'q' || event.key.character == 'Q'));
  if (cancel) {
    mode.confirm_command.clear();
    mode.confirm_prompt.clear();
    enter_mode(mode, AttachClientModeKind::Normal);
    actions.push_back(status_action(""));
    return actions;
  }

  const bool accept = event.key.key == Key::Char &&
                      (event.key.character == 'y' || event.key.character == 'Y');
  if (accept) {
    const auto command = mode.confirm_command;
    mode.confirm_command.clear();
    mode.confirm_prompt.clear();
    enter_mode(mode, AttachClientModeKind::Normal);
    actions.push_back(command_action(command));
    return actions;
  }

  if (!mode.confirm_prompt.empty()) {
    actions.push_back(status_action(mode.confirm_prompt));
  }
  return actions;
}

std::vector<AttachInputAction> handle_copy_mode_event(
    AttachClientModeState& mode,
    const TerminalInputEvent& event) {
  std::vector<AttachInputAction> actions;
  if (event.kind == TerminalInputEventKind::Mouse) {
    actions.push_back(mouse_action(event.mouse));
    return actions;
  }
  if (event.kind != TerminalInputEventKind::Key) {
    return actions;
  }

  const auto action = copy_mode_key_action(event.key);
  if (!copy_mode_action_is_meaningful(action)) {
    return actions;
  }

  if (action == AttachCopyModeAction::Exit || action == AttachCopyModeAction::CopySelection) {
    enter_mode(mode, AttachClientModeKind::Normal);
  }
  actions.push_back(copy_mode_action(action));
  return actions;
}

}  // namespace

std::vector<AttachInputAction> handle_attach_input_event(
    AttachClientModeState& mode,
    const TerminalInputEvent& event,
    char prefix_byte) {
  return handle_attach_input_event(mode, event, prefix_byte, default_attach_key_bindings());
}

std::vector<AttachInputAction> handle_attach_input_event(
    AttachClientModeState& mode,
    const TerminalInputEvent& event,
    char prefix_byte,
    const AttachKeyBindingTable& key_bindings) {
  switch (mode.kind) {
    case AttachClientModeKind::CommandPrompt:
      return handle_command_prompt_event(mode, event);
    case AttachClientModeKind::ConfirmPrompt:
      return handle_confirm_prompt_event(mode, event);
    case AttachClientModeKind::PrefixPending:
      return handle_prefix_event(mode, event, prefix_byte, key_bindings);
    case AttachClientModeKind::CopyMode:
      return handle_copy_mode_event(mode, event);
    case AttachClientModeKind::MouseDragResize:
      if (event.kind == TerminalInputEventKind::Mouse) {
        return {mouse_action(event.mouse)};
      }
      if (event.kind == TerminalInputEventKind::Key && event.key.key == Key::Escape) {
        enter_mode(mode, AttachClientModeKind::Normal);
      }
      return {};
    case AttachClientModeKind::Normal:
      break;
  }

  if (event.kind == TerminalInputEventKind::Mouse) {
    return {mouse_action(event.mouse)};
  }
  if (event.kind == TerminalInputEventKind::Paste) {
    return {send_input_action(event.paste.bytes)};
  }
  if (key_is_prefix(event, prefix_byte)) {
    enter_mode(mode, AttachClientModeKind::PrefixPending);
    return {status_action("prefix")};
  }

  return {send_input_action(event.encoded_input)};
}

std::string attach_client_mode_name(AttachClientModeKind mode) {
  switch (mode) {
    case AttachClientModeKind::Normal:
      return "Normal";
    case AttachClientModeKind::PrefixPending:
      return "PrefixPending";
    case AttachClientModeKind::CopyMode:
      return "CopyMode";
    case AttachClientModeKind::CommandPrompt:
      return "CommandPrompt";
    case AttachClientModeKind::ConfirmPrompt:
      return "ConfirmPrompt";
    case AttachClientModeKind::MouseDragResize:
      return "MouseDragResize";
  }
  return "Unknown";
}

std::string attach_input_action_name(AttachInputActionKind action) {
  switch (action) {
    case AttachInputActionKind::None:
      return "None";
    case AttachInputActionKind::SendInput:
      return "SendInput";
    case AttachInputActionKind::Detach:
      return "Detach";
    case AttachInputActionKind::Command:
      return "Command";
    case AttachInputActionKind::CommandMode:
      return "CommandMode";
    case AttachInputActionKind::Status:
      return "Status";
    case AttachInputActionKind::Scroll:
      return "Scroll";
    case AttachInputActionKind::CopyMode:
      return "CopyMode";
    case AttachInputActionKind::Paste:
      return "Paste";
    case AttachInputActionKind::Mouse:
      return "Mouse";
  }
  return "Unknown";
}

}  // namespace wmux
