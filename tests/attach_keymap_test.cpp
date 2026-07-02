#include "wmux/attach_keymap.hpp"

#include <cassert>
#include <unordered_map>

namespace {

void maps_default_prefix() {
  assert(wmux::control_prefix_byte("C-b") == '\x02');
  assert(wmux::control_prefix_byte("c-a") == '\x01');
  assert(wmux::control_prefix_byte("bad") == '\x02');
}

void maps_window_keybinds() {
  const char prefix = wmux::control_prefix_byte("C-b");

  const auto create = wmux::prefixed_attach_key_action("c", prefix);
  assert(create.kind == wmux::AttachKeyActionKind::Command);
  assert(create.command == "new-window");
  assert(create.bytes_consumed == 1);

  const auto next = wmux::prefixed_attach_key_action("n", prefix);
  assert(next.kind == wmux::AttachKeyActionKind::Command);
  assert(next.command == "next-window");

  const auto previous = wmux::prefixed_attach_key_action("p", prefix);
  assert(previous.kind == wmux::AttachKeyActionKind::Command);
  assert(previous.command == "previous-window");
}

void maps_existing_pane_keybinds() {
  const char prefix = wmux::control_prefix_byte("C-b");

  const auto kill = wmux::prefixed_attach_key_action("x", prefix);
  assert(kill.kind == wmux::AttachKeyActionKind::Command);
  assert(kill.command == "kill-pane");

  const auto equalize_lower = wmux::prefixed_attach_key_action("e", prefix);
  assert(equalize_lower.kind == wmux::AttachKeyActionKind::None);
  assert(equalize_lower.bytes_consumed == 1);

  const auto equalize_upper = wmux::prefixed_attach_key_action("E", prefix);
  assert(equalize_upper.kind == wmux::AttachKeyActionKind::Command);
  assert(equalize_upper.command == "equalize-panes");

  const auto horizontal = wmux::prefixed_attach_key_action("%", prefix);
  assert(horizontal.kind == wmux::AttachKeyActionKind::Command);
  assert(horizontal.command == "split-horizontal");

  const auto vertical = wmux::prefixed_attach_key_action("\"", prefix);
  assert(vertical.kind == wmux::AttachKeyActionKind::Command);
  assert(vertical.command == "split-vertical");
}

void maps_arrow_keybinds() {
  const char prefix = wmux::control_prefix_byte("C-b");

  const auto left = wmux::prefixed_attach_key_action("\x1b[D", prefix);
  assert(left.kind == wmux::AttachKeyActionKind::Command);
  assert(left.command == "select-pane-left");
  assert(left.bytes_consumed == 3);

  const auto right = wmux::prefixed_attach_key_action("\x1b[C", prefix);
  assert(right.kind == wmux::AttachKeyActionKind::Command);
  assert(right.command == "select-pane-right");
  assert(right.bytes_consumed == 3);
}

void passes_unknown_keys_through() {
  const char prefix = wmux::control_prefix_byte("C-b");
  const auto unknown = wmux::prefixed_attach_key_action("z", prefix);

  assert(unknown.kind == wmux::AttachKeyActionKind::PassThrough);
  assert(unknown.input.size() == 2);
  assert(unknown.input[0] == prefix);
  assert(unknown.input[1] == 'z');
}

void supports_custom_key_bindings() {
  std::unordered_map<std::string, std::string> overrides;
  overrides.emplace("z", "new-window");
  overrides.emplace("c", "kill-pane");
  overrides.emplace("e", "equalize-panes");

  const auto bindings = wmux::attach_key_bindings_from_overrides(overrides);
  const char prefix = wmux::control_prefix_byte("C-b");

  const auto custom = wmux::prefixed_attach_key_action("z", prefix, bindings);
  assert(custom.kind == wmux::AttachKeyActionKind::Command);
  assert(custom.command == "new-window");

  const auto overridden = wmux::prefixed_attach_key_action("c", prefix, bindings);
  assert(overridden.kind == wmux::AttachKeyActionKind::Command);
  assert(overridden.command == "kill-pane");

  const auto lower_e = wmux::prefixed_attach_key_action("e", prefix, bindings);
  assert(lower_e.kind == wmux::AttachKeyActionKind::Command);
  assert(lower_e.command == "equalize-panes");
}

void normalizes_key_specs_and_actions() {
  const auto up = wmux::normalize_attach_key_spec("Up");
  assert(up);
  assert(*up == "\x1b[A");

  const auto prefix_c = wmux::normalize_attach_key_spec("prefix c");
  assert(prefix_c);
  assert(*prefix_c == "c");

  const auto control_a = wmux::normalize_attach_key_spec("C-a");
  assert(control_a);
  assert(control_a->size() == 1);
  assert((*control_a)[0] == '\x01');

  const auto spread = wmux::normalize_attach_key_action_name("select-layout -E");
  assert(spread);
  assert(*spread == "equalize-panes");
}

void serializes_key_binding_overrides() {
  std::unordered_map<std::string, std::string> overrides;
  overrides.emplace("z", "new-window");
  overrides.emplace("\x1b[A", "select-pane-up");

  const auto serialized = wmux::serialize_attach_key_binding_overrides(overrides);
  const auto parsed = wmux::parse_serialized_attach_key_binding_overrides(serialized);

  assert(parsed.size() == 2);
  assert(parsed.at("z") == "new-window");
  assert(parsed.at("\x1b[A") == "select-pane-up");
}

}  // namespace

void run_attach_keymap_tests() {
  maps_default_prefix();
  maps_window_keybinds();
  maps_existing_pane_keybinds();
  maps_arrow_keybinds();
  passes_unknown_keys_through();
  supports_custom_key_bindings();
  normalizes_key_specs_and_actions();
  serializes_key_binding_overrides();
}
