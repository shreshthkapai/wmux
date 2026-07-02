#include "wmux/attach_keymap.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <iomanip>
#include <optional>
#include <sstream>
#include <system_error>
#include <vector>

namespace wmux {
namespace {

constexpr std::string_view kHexDigits = "0123456789abcdef";

std::string passthrough_input(char prefix_byte, char byte) {
  std::string input;
  input.push_back(prefix_byte);
  input.push_back(byte);
  return input;
}

AttachKeyAction command_action(std::string command) {
  AttachKeyAction action;
  action.kind = AttachKeyActionKind::Command;
  action.command = std::move(command);
  action.bytes_consumed = 1;
  return action;
}

AttachKeyAction scroll_action(AttachScrollAction scroll, std::size_t bytes_consumed) {
  AttachKeyAction action;
  action.kind = AttachKeyActionKind::Scroll;
  action.scroll_action = scroll;
  action.bytes_consumed = bytes_consumed;
  return action;
}

AttachKeyAction detach_action() {
  AttachKeyAction action;
  action.kind = AttachKeyActionKind::Detach;
  action.bytes_consumed = 1;
  return action;
}

AttachKeyAction none_action(std::size_t bytes_consumed = 1) {
  AttachKeyAction action;
  action.kind = AttachKeyActionKind::None;
  action.bytes_consumed = bytes_consumed;
  return action;
}

AttachKeyAction command_prompt_action() {
  AttachKeyAction action;
  action.kind = AttachKeyActionKind::CommandPrompt;
  action.bytes_consumed = 1;
  return action;
}

AttachKeyAction copy_mode_action() {
  AttachKeyAction action;
  action.kind = AttachKeyActionKind::CopyMode;
  action.copy_mode_action = AttachCopyModeAction::Enter;
  action.bytes_consumed = 1;
  return action;
}

AttachKeyAction paste_action() {
  AttachKeyAction action;
  action.kind = AttachKeyActionKind::Paste;
  action.bytes_consumed = 1;
  return action;
}

bool starts_with(std::string_view text, std::string_view prefix) {
  return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

std::string lower_ascii(std::string_view text) {
  std::string lowered;
  lowered.reserve(text.size());
  for (const char ch : text) {
    lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return lowered;
}

void bind_action(
    AttachKeyBindingTable& table,
    std::string_view key,
    AttachKeyAction action) {
  action.bytes_consumed = key.size();
  table.bindings[std::string{key}] = std::move(action);
}

std::optional<unsigned int> parse_hex_byte(std::string_view text) {
  unsigned int value = 0;
  const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value, 16);
  if (ec != std::errc{} || ptr != text.data() + text.size() || value > 0xFF) {
    return std::nullopt;
  }
  return value;
}

std::string hex_encode(std::string_view bytes) {
  std::string encoded;
  encoded.reserve(bytes.size() * 2);
  for (const unsigned char byte : bytes) {
    encoded.push_back(static_cast<char>(kHexDigits[(byte >> 4) & 0x0F]));
    encoded.push_back(static_cast<char>(kHexDigits[byte & 0x0F]));
  }
  return encoded;
}

std::optional<std::string> hex_decode(std::string_view encoded) {
  if (encoded.size() % 2 != 0) {
    return std::nullopt;
  }

  std::string decoded;
  decoded.reserve(encoded.size() / 2);
  for (std::size_t index = 0; index < encoded.size(); index += 2) {
    const auto byte = parse_hex_byte(encoded.substr(index, 2));
    if (!byte) {
      return std::nullopt;
    }
    decoded.push_back(static_cast<char>(*byte));
  }
  return decoded;
}

}  // namespace

char control_prefix_byte(std::string_view prefix) {
  if (prefix.size() != 3 || (prefix[0] != 'C' && prefix[0] != 'c') || prefix[1] != '-') {
    return '\x02';
  }

  const auto key = static_cast<unsigned char>(prefix[2]);
  if (key == '?') {
    return '\x7f';
  }

  return static_cast<char>(std::toupper(key) & 0x1F);
}

std::string byte_debug_name(char byte) {
  const auto value = static_cast<unsigned char>(byte);
  if (value == '\x1b') {
    return "Esc";
  }
  if (value == '\r') {
    return "CR";
  }
  if (value == '\n') {
    return "LF";
  }
  if (value < 0x20 || value == 0x7f) {
    std::ostringstream out;
    out << "0x" << std::hex << std::setw(2) << std::setfill('0')
        << static_cast<int>(value);
    return out.str();
  }

  return std::string{1, static_cast<char>(value)};
}

AttachKeyAction prefixed_attach_key_action(std::string_view bytes, char prefix_byte) {
  return prefixed_attach_key_action(bytes, prefix_byte, default_attach_key_bindings());
}

AttachKeyAction prefixed_attach_key_action(
    std::string_view bytes,
    char prefix_byte,
    const AttachKeyBindingTable& key_bindings) {
  AttachKeyAction action;
  if (bytes.empty()) {
    return action;
  }

  std::optional<std::pair<std::string, AttachKeyAction>> longest;
  for (const auto& [key, candidate] : key_bindings.bindings) {
    if (!starts_with(bytes, key)) {
      continue;
    }
    if (!longest || key.size() > longest->first.size()) {
      longest = std::pair{key, candidate};
    }
  }

  if (longest) {
    auto matched = longest->second;
    matched.bytes_consumed = longest->first.size();
    return matched;
  }

  action.kind = AttachKeyActionKind::PassThrough;
  action.input = passthrough_input(prefix_byte, bytes.front());
  action.bytes_consumed = 1;
  return action;
}

AttachKeyBindingTable default_attach_key_bindings() {
  AttachKeyBindingTable table;
  bind_action(table, "d", detach_action());
  bind_action(table, "D", detach_action());
  bind_action(table, "\x04", detach_action());
  bind_action(table, "c", command_action("new-window"));
  bind_action(table, "C", command_action("new-window"));
  bind_action(table, "\x03", command_action("new-window"));
  bind_action(table, "n", command_action("next-window"));
  bind_action(table, "N", command_action("next-window"));
  bind_action(table, "\x0e", command_action("next-window"));
  bind_action(table, "p", command_action("previous-window"));
  bind_action(table, "P", command_action("previous-window"));
  bind_action(table, "\x10", command_action("previous-window"));
  for (char digit = '0'; digit <= '9'; ++digit) {
    bind_action(
        table,
        std::string_view{&digit, 1},
        command_action("select-window-" + std::string(1, digit)));
  }
  bind_action(table, "x", command_action("kill-pane"));
  bind_action(table, "X", command_action("kill-pane"));
  bind_action(table, "\x18", command_action("kill-pane"));
  bind_action(table, "e", none_action());
  bind_action(table, "E", command_action("equalize-panes"));
  bind_action(table, "\x05", command_action("equalize-panes"));
  bind_action(table, "%", command_action("split-horizontal"));
  bind_action(table, "\"", command_action("split-vertical"));
  bind_action(table, "g", scroll_action(AttachScrollAction::Bottom, 1));
  bind_action(table, "G", scroll_action(AttachScrollAction::Bottom, 1));
  bind_action(table, ":", command_prompt_action());
  bind_action(table, "[", copy_mode_action());
  bind_action(table, "]", paste_action());
  bind_action(table, "\x1b[5~", scroll_action(AttachScrollAction::PageUp, 4));
  bind_action(table, "\x1b[6~", scroll_action(AttachScrollAction::PageDown, 4));
  bind_action(table, "\x1b[A", command_action("select-pane-up"));
  bind_action(table, "\x1b[B", command_action("select-pane-down"));
  bind_action(table, "\x1b[C", command_action("select-pane-right"));
  bind_action(table, "\x1b[D", command_action("select-pane-left"));
  bind_action(table, "\x1b[1;5A", command_action("resize-pane-up"));
  bind_action(table, "\x1b[1;5B", command_action("resize-pane-down"));
  bind_action(table, "\x1b[1;5C", command_action("resize-pane-right"));
  bind_action(table, "\x1b[1;5D", command_action("resize-pane-left"));
  bind_action(table, "\x1b[1;3A", command_action("resize-pane-up-large"));
  bind_action(table, "\x1b[1;3B", command_action("resize-pane-down-large"));
  bind_action(table, "\x1b[1;3C", command_action("resize-pane-right-large"));
  bind_action(table, "\x1b[1;3D", command_action("resize-pane-left-large"));
  return table;
}

std::optional<std::string> normalize_attach_key_spec(std::string_view spec) {
  while (!spec.empty() && std::isspace(static_cast<unsigned char>(spec.front())) != 0) {
    spec.remove_prefix(1);
  }
  while (!spec.empty() && std::isspace(static_cast<unsigned char>(spec.back())) != 0) {
    spec.remove_suffix(1);
  }

  constexpr std::string_view kPrefixPrefix = "prefix ";
  const auto lowered = lower_ascii(spec);
  if (lowered.rfind(kPrefixPrefix, 0) == 0) {
    spec.remove_prefix(kPrefixPrefix.size());
  }

  if (spec.empty()) {
    return std::nullopt;
  }

  const auto named = lower_ascii(spec);
  if (named == "up") {
    return std::string{"\x1b[A"};
  }
  if (named == "down") {
    return std::string{"\x1b[B"};
  }
  if (named == "right") {
    return std::string{"\x1b[C"};
  }
  if (named == "left") {
    return std::string{"\x1b[D"};
  }
  if (named == "c-up" || named == "ctrl-up") {
    return std::string{"\x1b[1;5A"};
  }
  if (named == "c-down" || named == "ctrl-down") {
    return std::string{"\x1b[1;5B"};
  }
  if (named == "c-right" || named == "ctrl-right") {
    return std::string{"\x1b[1;5C"};
  }
  if (named == "c-left" || named == "ctrl-left") {
    return std::string{"\x1b[1;5D"};
  }
  if (named == "m-up" || named == "alt-up") {
    return std::string{"\x1b[1;3A"};
  }
  if (named == "m-down" || named == "alt-down") {
    return std::string{"\x1b[1;3B"};
  }
  if (named == "m-right" || named == "alt-right") {
    return std::string{"\x1b[1;3C"};
  }
  if (named == "m-left" || named == "alt-left") {
    return std::string{"\x1b[1;3D"};
  }
  if (named == "pageup" || named == "page-up") {
    return std::string{"\x1b[5~"};
  }
  if (named == "pagedown" || named == "page-down") {
    return std::string{"\x1b[6~"};
  }
  if (named == "space") {
    return std::string{" "};
  }
  if (named == "tab") {
    return std::string{"\t"};
  }
  if (named == "enter" || named == "return") {
    return std::string{"\r"};
  }
  if (named == "esc" || named == "escape") {
    return std::string{"\x1b"};
  }
  if (named == "backspace" || named == "bs") {
    return std::string{"\x7f"};
  }
  if (named == "quote" || named == "double-quote") {
    return std::string{"\""};
  }

  if (spec.size() == 3 && (spec[0] == 'C' || spec[0] == 'c') && spec[1] == '-') {
    return std::string(1, control_prefix_byte(spec));
  }

  if (spec.size() == 1) {
    return std::string{spec};
  }

  return std::nullopt;
}

std::optional<std::string> normalize_attach_key_action_name(std::string_view action) {
  while (!action.empty() && std::isspace(static_cast<unsigned char>(action.front())) != 0) {
    action.remove_prefix(1);
  }
  while (!action.empty() && std::isspace(static_cast<unsigned char>(action.back())) != 0) {
    action.remove_suffix(1);
  }

  const auto lowered = lower_ascii(action);
  if (lowered == "new-window" || lowered == "next-window" ||
      lowered == "previous-window" || lowered == "kill-pane" ||
      lowered == "split-horizontal" || lowered == "split-vertical" ||
      lowered == "select-pane-left" || lowered == "select-pane-right" ||
      lowered == "select-pane-up" || lowered == "select-pane-down" ||
      lowered == "resize-pane-left" || lowered == "resize-pane-right" ||
      lowered == "resize-pane-up" || lowered == "resize-pane-down" ||
      lowered == "resize-pane-left-large" || lowered == "resize-pane-right-large" ||
      lowered == "resize-pane-up-large" || lowered == "resize-pane-down-large") {
    return lowered;
  }
  if (lowered.size() == std::string_view{"select-window-"}.size() + 1 &&
      lowered.rfind("select-window-", 0) == 0 &&
      std::isdigit(static_cast<unsigned char>(lowered.back())) != 0) {
    return lowered;
  }
  constexpr std::string_view kSelectWindowTargetPrefix = "select-window -t ";
  if (lowered.rfind(kSelectWindowTargetPrefix, 0) == 0 &&
      lowered.size() == kSelectWindowTargetPrefix.size() + 1 &&
      std::isdigit(static_cast<unsigned char>(lowered.back())) != 0) {
    return "select-window-" + std::string{lowered.back()};
  }
  if (lowered == "resize-pane -l" || lowered == "resize-pane-l") {
    return std::string{"resize-pane-left"};
  }
  if (lowered == "resize-pane -r" || lowered == "resize-pane-r") {
    return std::string{"resize-pane-right"};
  }
  if (lowered == "resize-pane -u" || lowered == "resize-pane-u") {
    return std::string{"resize-pane-up"};
  }
  if (lowered == "resize-pane -d" || lowered == "resize-pane-d") {
    return std::string{"resize-pane-down"};
  }
  if (lowered == "equalize-panes" || lowered == "spread-panes-evenly" ||
      lowered == "select-layout -e" || lowered == "select-layout-e") {
    return std::string{"equalize-panes"};
  }
  if (lowered == "split-window -h" || lowered == "split-window-h") {
    return std::string{"split-horizontal"};
  }
  if (lowered == "split-window -v" || lowered == "split-window-v") {
    return std::string{"split-vertical"};
  }
  if (lowered == "detach" || lowered == "detach-client") {
    return std::string{"detach"};
  }
  if (lowered == "command-prompt" || lowered == ":") {
    return std::string{"command-prompt"};
  }
  if (lowered == "copy-mode") {
    return std::string{"copy-mode"};
  }
  if (lowered == "paste" || lowered == "paste-buffer") {
    return std::string{"paste-buffer"};
  }
  if (lowered == "scroll-bottom" || lowered == "bottom") {
    return std::string{"scroll-bottom"};
  }
  if (lowered == "scroll-page-up" || lowered == "page-up") {
    return std::string{"scroll-page-up"};
  }
  if (lowered == "scroll-page-down" || lowered == "page-down") {
    return std::string{"scroll-page-down"};
  }
  if (lowered == "none" || lowered == "unbound") {
    return std::string{"none"};
  }

  return std::nullopt;
}

AttachKeyBindingTable attach_key_bindings_from_overrides(
    const std::unordered_map<std::string, std::string>& overrides) {
  auto table = default_attach_key_bindings();
  for (const auto& [key, action_name] : overrides) {
    const auto action = normalize_attach_key_action_name(action_name);
    if (!action) {
      continue;
    }

    if (*action == "detach") {
      bind_action(table, key, detach_action());
    } else if (*action == "command-prompt") {
      bind_action(table, key, command_prompt_action());
    } else if (*action == "copy-mode") {
      bind_action(table, key, copy_mode_action());
    } else if (*action == "paste-buffer") {
      bind_action(table, key, paste_action());
    } else if (*action == "scroll-bottom") {
      bind_action(table, key, scroll_action(AttachScrollAction::Bottom, key.size()));
    } else if (*action == "scroll-page-up") {
      bind_action(table, key, scroll_action(AttachScrollAction::PageUp, key.size()));
    } else if (*action == "scroll-page-down") {
      bind_action(table, key, scroll_action(AttachScrollAction::PageDown, key.size()));
    } else if (*action == "none") {
      bind_action(table, key, none_action(key.size()));
    } else {
      bind_action(table, key, command_action(*action));
    }
  }
  return table;
}

std::string serialize_attach_key_binding_overrides(
    const std::unordered_map<std::string, std::string>& overrides) {
  std::vector<std::pair<std::string, std::string>> sorted;
  sorted.reserve(overrides.size());
  for (const auto& binding : overrides) {
    sorted.push_back(binding);
  }
  std::sort(sorted.begin(), sorted.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.first < rhs.first;
  });

  std::ostringstream out;
  for (const auto& [key, action] : sorted) {
    out << hex_encode(key) << '\t' << action << '\n';
  }
  return out.str();
}

std::unordered_map<std::string, std::string> parse_serialized_attach_key_binding_overrides(
    std::string_view serialized) {
  std::unordered_map<std::string, std::string> overrides;
  std::size_t line_start = 0;
  while (line_start <= serialized.size()) {
    auto line_end = serialized.find('\n', line_start);
    if (line_end == std::string_view::npos) {
      line_end = serialized.size();
    }
    const auto line = serialized.substr(line_start, line_end - line_start);
    if (!line.empty()) {
      const auto separator = line.find('\t');
      if (separator != std::string_view::npos) {
        const auto key = hex_decode(line.substr(0, separator));
        const auto action =
            normalize_attach_key_action_name(line.substr(separator + 1));
        if (key && action) {
          overrides[*key] = *action;
        }
      }
    }
    if (line_end == serialized.size()) {
      break;
    }
    line_start = line_end + 1;
  }
  return overrides;
}

}  // namespace wmux
