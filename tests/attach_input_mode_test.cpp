#include "wmux/attach_input_mode.hpp"
#include "wmux/attach_keymap.hpp"

#include <cassert>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

std::vector<wmux::AttachInputAction> actions_for(
    wmux::AttachClientModeState& mode,
    std::string_view bytes,
    const wmux::AttachKeyBindingTable& key_bindings = wmux::default_attach_key_bindings()) {
  wmux::TerminalInputDecoderState decoder;
  wmux::TerminalInputDecoderOptions options;
  options.mouse_enabled = true;
  const auto decoded = wmux::decode_terminal_input(decoder, bytes, options);
  std::vector<wmux::AttachInputAction> actions;
  const char prefix = wmux::control_prefix_byte("C-b");
  for (const auto& event : decoded.events) {
    auto event_actions = wmux::handle_attach_input_event(mode, event, prefix, key_bindings);
    actions.insert(actions.end(), event_actions.begin(), event_actions.end());
  }
  return actions;
}

const wmux::AttachInputAction* find_action(
    const std::vector<wmux::AttachInputAction>& actions,
    wmux::AttachInputActionKind kind) {
  for (const auto& action : actions) {
    if (action.kind == kind) {
      return &action;
    }
  }
  return nullptr;
}

bool has_status_text(
    const std::vector<wmux::AttachInputAction>& actions,
    std::string_view text) {
  for (const auto& action : actions) {
    if (action.kind == wmux::AttachInputActionKind::Status && action.text == text) {
      return true;
    }
  }
  return false;
}

void normal_input_goes_to_shell() {
  wmux::AttachClientModeState mode;
  const auto actions = actions_for(mode, "abc");

  assert(mode.kind == wmux::AttachClientModeKind::Normal);
  assert(actions.size() == 3);
  assert(actions[0].kind == wmux::AttachInputActionKind::SendInput);
  assert(actions[0].text == "a");
}

void prefix_window_bindings_become_commands() {
  wmux::AttachClientModeState mode;

  const auto create = actions_for(mode, "\x02" "c");
  assert(create.size() == 2);
  assert(create[0].kind == wmux::AttachInputActionKind::Status);
  assert(create[0].text == "prefix");
  assert(create[1].kind == wmux::AttachInputActionKind::Command);
  assert(create[1].text == "new-window");
  assert(mode.kind == wmux::AttachClientModeKind::Normal);

  const auto next = actions_for(mode, "\x02" "n");
  const auto next_command = find_action(next, wmux::AttachInputActionKind::Command);
  assert(next_command != nullptr);
  assert(next_command->text == "next-window");

  const auto previous = actions_for(mode, "\x02" "p");
  const auto previous_command = find_action(previous, wmux::AttachInputActionKind::Command);
  assert(previous_command != nullptr);
  assert(previous_command->text == "previous-window");
}

void prefix_pane_bindings_become_commands() {
  wmux::AttachClientModeState mode;

  const auto kill = actions_for(mode, "\x02" "x");
  assert(has_status_text(kill, "kill-pane? (y/n)"));
  assert(mode.kind == wmux::AttachClientModeKind::ConfirmPrompt);

  const auto cancelled = actions_for(mode, "n");
  assert(find_action(cancelled, wmux::AttachInputActionKind::Command) == nullptr);
  assert(mode.kind == wmux::AttachClientModeKind::Normal);

  const auto confirm = actions_for(mode, "\x02" "x");
  assert(has_status_text(confirm, "kill-pane? (y/n)"));
  const auto accepted = actions_for(mode, "y");
  const auto kill_command = find_action(accepted, wmux::AttachInputActionKind::Command);
  assert(kill_command != nullptr);
  assert(kill_command->text == "kill-pane");
  assert(mode.kind == wmux::AttachClientModeKind::Normal);

  const auto spread = actions_for(mode, "\x02" "E");
  const auto spread_command = find_action(spread, wmux::AttachInputActionKind::Command);
  assert(spread_command != nullptr);
  assert(spread_command->text == "equalize-panes");

  const auto resize = actions_for(mode, "\x02\x1b[1;5D");
  const auto resize_command = find_action(resize, wmux::AttachInputActionKind::Command);
  assert(resize_command != nullptr);
  assert(resize_command->text == "resize-pane-left");

  const auto large_resize = actions_for(mode, "\x02\x1b[1;3C");
  const auto large_resize_command = find_action(large_resize, wmux::AttachInputActionKind::Command);
  assert(large_resize_command != nullptr);
  assert(large_resize_command->text == "resize-pane-right-large");

  const auto lower_e = actions_for(mode, "\x02" "e");
  assert(has_status_text(lower_e, "wmux: unknown keybind"));
}

void prefix_copy_and_paste_modes_are_explicit() {
  wmux::AttachClientModeState mode;

  const auto enter_copy = actions_for(mode, "\x02" "[");
  const auto enter_copy_action = find_action(enter_copy, wmux::AttachInputActionKind::CopyMode);
  assert(enter_copy_action != nullptr);
  assert(enter_copy_action->copy_mode_action == wmux::AttachCopyModeAction::Enter);
  assert(mode.kind == wmux::AttachClientModeKind::CopyMode);

  const auto down = actions_for(mode, "j");
  assert(down.size() == 1);
  assert(down[0].kind == wmux::AttachInputActionKind::CopyMode);
  assert(down[0].copy_mode_action == wmux::AttachCopyModeAction::CursorDown);
  assert(mode.kind == wmux::AttachClientModeKind::CopyMode);

  const auto home = actions_for(mode, "\x1b[H");
  assert(home.size() == 1);
  assert(home[0].copy_mode_action == wmux::AttachCopyModeAction::Home);

  const auto end = actions_for(mode, "\x1b[F");
  assert(end.size() == 1);
  assert(end[0].copy_mode_action == wmux::AttachCopyModeAction::End);

  const auto ignored = actions_for(mode, "a");
  assert(ignored.empty());
  assert(mode.kind == wmux::AttachClientModeKind::CopyMode);

  const auto exit = actions_for(mode, "q");
  assert(exit.size() == 1);
  assert(exit[0].copy_mode_action == wmux::AttachCopyModeAction::Exit);
  assert(mode.kind == wmux::AttachClientModeKind::Normal);

  const auto paste = actions_for(mode, "\x02" "]");
  assert(find_action(paste, wmux::AttachInputActionKind::Paste) != nullptr);
}

void command_prompt_mode_consumes_input() {
  wmux::AttachClientModeState mode;

  const auto prompt = actions_for(mode, "\x02" ":");
  assert(!prompt.empty());
  assert(prompt[0].kind == wmux::AttachInputActionKind::Status);
  assert(mode.kind == wmux::AttachClientModeKind::CommandPrompt);

  const auto submitted = actions_for(mode, "rename-window logs\r");
  const auto command = find_action(submitted, wmux::AttachInputActionKind::CommandMode);
  assert(command != nullptr);
  assert(command->text == "rename-window logs");
  assert(mode.kind == wmux::AttachClientModeKind::Normal);
}

void tmux_rename_keybinds_prefill_command_prompt() {
  wmux::AttachClientModeState mode;

  const auto rename_window_prompt = actions_for(mode, "\x02" ",");
  assert(has_status_text(rename_window_prompt, ":rename-window "));
  assert(mode.kind == wmux::AttachClientModeKind::CommandPrompt);

  const auto rename_window = actions_for(mode, "logs\r");
  const auto rename_window_command =
      find_action(rename_window, wmux::AttachInputActionKind::CommandMode);
  assert(rename_window_command != nullptr);
  assert(rename_window_command->text == "rename-window logs");
  assert(mode.kind == wmux::AttachClientModeKind::Normal);

  const auto rename_session_prompt = actions_for(mode, "\x02" "$");
  assert(has_status_text(rename_session_prompt, ":rename-session "));
  assert(mode.kind == wmux::AttachClientModeKind::CommandPrompt);

  const auto rename_session = actions_for(mode, "trading\r");
  const auto rename_session_command =
      find_action(rename_session, wmux::AttachInputActionKind::CommandMode);
  assert(rename_session_command != nullptr);
  assert(rename_session_command->text == "rename-session trading");
  assert(mode.kind == wmux::AttachClientModeKind::Normal);
}

void unknown_prefix_key_passes_prefix_and_key_to_shell() {
  wmux::AttachClientModeState mode;
  const auto actions = actions_for(mode, "\x02" "z");

  assert(actions.size() == 2);
  assert(actions[0].kind == wmux::AttachInputActionKind::Status);
  assert(actions[0].text == "prefix");
  assert(actions[1].kind == wmux::AttachInputActionKind::SendInput);
  const std::string expected{"\x02z", 2};
  assert(actions[1].text == expected);
  assert(mode.kind == wmux::AttachClientModeKind::Normal);
}

void custom_prefix_bindings_route_to_commands() {
  wmux::AttachClientModeState mode;
  std::unordered_map<std::string, std::string> overrides;
  overrides.emplace("z", "new-window");
  overrides.emplace("c", "kill-pane");
  const auto key_bindings = wmux::attach_key_bindings_from_overrides(overrides);

  const auto custom = actions_for(mode, "\x02" "z", key_bindings);
  const auto custom_command = find_action(custom, wmux::AttachInputActionKind::Command);
  assert(custom_command != nullptr);
  assert(custom_command->text == "new-window");

  const auto overridden = actions_for(mode, "\x02" "c", key_bindings);
  assert(has_status_text(overridden, "kill-pane? (y/n)"));
  assert(mode.kind == wmux::AttachClientModeKind::ConfirmPrompt);
  const auto accepted = actions_for(mode, "y", key_bindings);
  const auto overridden_command = find_action(accepted, wmux::AttachInputActionKind::Command);
  assert(overridden_command != nullptr);
  assert(overridden_command->text == "kill-pane");
  assert(mode.kind == wmux::AttachClientModeKind::Normal);
}

void paste_and_mouse_events_route_by_mode() {
  wmux::AttachClientModeState mode;

  const auto paste = actions_for(mode, "\x1b[200~hello\x1b[201~");
  assert(paste.size() == 1);
  assert(paste[0].kind == wmux::AttachInputActionKind::SendInput);
  assert(paste[0].text == "hello");

  const auto mouse = actions_for(mode, "\x1b[<0;10;4M");
  assert(mouse.size() == 1);
  assert(mouse[0].kind == wmux::AttachInputActionKind::Mouse);
  assert(mouse[0].mouse.column == 10);
  assert(mouse[0].mouse.row == 4);
}

}  // namespace

void run_attach_input_mode_tests() {
  normal_input_goes_to_shell();
  prefix_window_bindings_become_commands();
  prefix_pane_bindings_become_commands();
  prefix_copy_and_paste_modes_are_explicit();
  command_prompt_mode_consumes_input();
  tmux_rename_keybinds_prefill_command_prompt();
  unknown_prefix_key_passes_prefix_and_key_to_shell();
  custom_prefix_bindings_route_to_commands();
  paste_and_mouse_events_route_by_mode();
}
