#pragma once

#include "wmux/ipc_protocol.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace wmux {

enum class AttachKeyActionKind {
  None,
  PassThrough,
  Detach,
  Command,
  CommandPrompt,
  CopyMode,
  Paste,
  Scroll,
};

struct AttachKeyAction {
  AttachKeyActionKind kind{AttachKeyActionKind::None};
  std::string command;
  std::string input;
  AttachScrollAction scroll_action{AttachScrollAction::Bottom};
  AttachCopyModeAction copy_mode_action{AttachCopyModeAction::Enter};
  std::size_t bytes_consumed{0};
};

struct AttachKeyBindingTable {
  std::unordered_map<std::string, AttachKeyAction> bindings;
};

char control_prefix_byte(std::string_view prefix);
std::string byte_debug_name(char byte);
AttachKeyAction prefixed_attach_key_action(std::string_view bytes, char prefix_byte);
AttachKeyAction prefixed_attach_key_action(
    std::string_view bytes,
    char prefix_byte,
    const AttachKeyBindingTable& key_bindings);
AttachKeyBindingTable default_attach_key_bindings();
AttachKeyBindingTable attach_key_bindings_from_overrides(
    const std::unordered_map<std::string, std::string>& overrides);
std::optional<std::string> normalize_attach_key_spec(std::string_view spec);
std::optional<std::string> normalize_attach_key_action_name(std::string_view action);
std::string serialize_attach_key_binding_overrides(
    const std::unordered_map<std::string, std::string>& overrides);
std::unordered_map<std::string, std::string> parse_serialized_attach_key_binding_overrides(
    std::string_view serialized);

}  // namespace wmux
