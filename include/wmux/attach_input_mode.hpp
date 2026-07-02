#pragma once

#include "wmux/attach_keymap.hpp"
#include "wmux/command_mode.hpp"
#include "wmux/ipc_protocol.hpp"
#include "wmux/mouse_input.hpp"
#include "wmux/terminal_input.hpp"

#include <chrono>
#include <string>
#include <vector>

namespace wmux {

enum class AttachClientModeKind {
  Normal,
  PrefixPending,
  CopyMode,
  CommandPrompt,
  MouseDragResize,
};

struct AttachClientModeState {
  AttachClientModeKind kind{AttachClientModeKind::Normal};
  std::chrono::steady_clock::time_point started_at{};
  CommandPromptState command_prompt;
};

enum class AttachInputActionKind {
  None,
  SendInput,
  Detach,
  Command,
  CommandMode,
  Status,
  Scroll,
  CopyMode,
  Paste,
  Mouse,
};

struct AttachInputAction {
  AttachInputActionKind kind{AttachInputActionKind::None};
  std::string text;
  AttachScrollAction scroll_action{AttachScrollAction::Bottom};
  AttachCopyModeAction copy_mode_action{AttachCopyModeAction::Enter};
  MouseEvent mouse;
};

std::vector<AttachInputAction> handle_attach_input_event(
    AttachClientModeState& mode,
    const TerminalInputEvent& event,
    char prefix_byte);
std::vector<AttachInputAction> handle_attach_input_event(
    AttachClientModeState& mode,
    const TerminalInputEvent& event,
    char prefix_byte,
    const AttachKeyBindingTable& key_bindings);

std::string attach_client_mode_name(AttachClientModeKind mode);
std::string attach_input_action_name(AttachInputActionKind action);

}  // namespace wmux
