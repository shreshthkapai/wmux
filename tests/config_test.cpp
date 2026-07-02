#include "wmux/config.hpp"
#include "wmux/resource_limits.hpp"

#include <cassert>
#include <string>

namespace {

void parses_supported_settings() {
  const auto result = wmux::parse_config_text(
      "# wmux config\n"
      "set -g prefix C-b\n"
      "set -g mouse on\n"
      "set -g default-shell powershell.exe\n"
      "set -g status off\n"
      "set -g accent red\n"
      "set -g ui-inherit-terminal-theme off\n"
      "set -g ui-tmux-style on\n"
      "set -g border-style ascii\n"
      "set -g escape-time-ms 75\n"
      "set -g max-sessions 12\n"
      "set -g max-windows-per-session 8\n"
      "set -g max-panes-per-window 6\n"
      "set -g scrollback-max-lines 10000\n"
      "set -g paste-buffer-max-bytes 2048\n"
      "set -g pane-raw-output-max-bytes 131072\n"
      "set -g client-output-queue-max-bytes 262144\n"
      "set -g client-output-queue-max-frames 16\n"
      "set -g attach-render-frame-max-bytes 524288\n"
      "set -g ipc-frame-max-bytes 65536\n"
      "set -g log-max-bytes 1048576\n"
      "set -g terminal-host windows-terminal\n"
      "set -g terminal-truecolor on\n"
      "set -g terminal-256-color on\n"
      "set -g terminal-mouse on\n"
      "set -g terminal-mouse-drag on\n"
      "set -g terminal-mouse-wheel off\n"
      "set -g terminal-bracketed-paste on\n"
      "set -g terminal-focus-events off\n"
      "set -g terminal-cursor-style on\n"
      "set -g terminal-alt-screen on\n"
      "set -g terminal-extended-keys off\n"
      "set -g terminal-osc52-clipboard off\n"
      "set -g terminal-synchronized-output off\n"
      "set -g terminal-quirk-no-osc52-clipboard off\n"
      "bind-key z new-window\n"
      "bind-key Up select-pane-up\n"
      "unbind-key e\n");

  assert(result.ok());
  assert(result.config.prefix == "C-b");
  assert(result.config.mouse_enabled);
  assert(result.config.default_shell == "powershell.exe");
  assert(!result.config.status_bar_enabled);
  assert(result.config.ui.accent_spec == "red");
  assert(result.config.ui.accent.kind == wmux::UiColorKind::Indexed);
  assert(result.config.ui.accent.index == 1);
  assert(!result.config.ui.inherit_terminal_theme);
  assert(result.config.ui.tmux_style);
  assert(!result.config.ui.smooth_borders);
  assert(result.config.escape_time_ms == 75);
  assert(result.config.limits.max_sessions == 12);
  assert(result.config.limits.max_windows_per_session == 8);
  assert(result.config.limits.max_panes_per_window == 6);
  assert(result.config.limits.max_pane_scrollback_lines == 10000);
  assert(result.config.session.scrollback_max_lines == 10000);
  assert(result.config.limits.max_paste_buffer_bytes == 2048);
  assert(result.config.limits.max_pane_raw_output_bytes == 131072);
  assert(result.config.limits.max_client_output_queue_bytes == 262144);
  assert(result.config.client.output_queue_bytes == 262144);
  assert(result.config.limits.max_client_output_queue_frames == 16);
  assert(result.config.client.output_queue_frames == 16);
  assert(result.config.limits.max_attach_render_frame_bytes == 524288);
  assert(result.config.limits.max_ipc_frame_payload_bytes == 65536);
  assert(result.config.limits.max_log_file_bytes == 1048576);
  assert(result.config.terminal_overrides.host == wmux::TerminalHost::WindowsTerminal);
  assert(result.config.terminal_overrides.supports_truecolor == true);
  assert(result.config.terminal_overrides.supports_256_color == true);
  assert(result.config.terminal_overrides.supports_sgr_mouse == true);
  assert(result.config.terminal_overrides.supports_mouse_drag == true);
  assert(result.config.terminal_overrides.supports_mouse_wheel == false);
  assert(result.config.terminal_overrides.supports_bracketed_paste == true);
  assert(result.config.terminal_overrides.supports_focus_events == false);
  assert(result.config.terminal_overrides.supports_cursor_style == true);
  assert(result.config.terminal_overrides.supports_alt_screen == true);
  assert(result.config.terminal_overrides.supports_extended_keys == false);
  assert(result.config.terminal_overrides.supports_osc52_clipboard == false);
  assert(result.config.terminal_overrides.supports_synchronized_output == false);
  assert(result.config.terminal_overrides.quirk_no_osc52_clipboard == false);
  assert(result.config.keys.bindings.at("z") == "new-window");
  assert(result.config.keys.bindings.at("\x1b[A") == "select-pane-up");
  assert(result.config.keys.bindings.at("e") == "none");
}

void preserves_defaults_for_empty_config() {
  const auto result = wmux::parse_config_text("");

  assert(result.ok());
  assert(result.config.prefix == "C-b");
  assert(result.config.mouse_enabled);
  assert(result.config.default_shell.empty());
  assert(result.config.status_bar_enabled);
  assert(result.config.ui.accent_spec == "blue");
  assert(result.config.ui.accent.kind == wmux::UiColorKind::Indexed);
  assert(result.config.ui.accent.index == 4);
  assert(result.config.ui.inherit_terminal_theme);
  assert(!result.config.ui.tmux_style);
  assert(result.config.ui.smooth_borders);
  assert(result.config.escape_time_ms == 50);
  assert(result.config.limits.max_sessions == wmux::kMaxSessions);
  assert(result.config.limits.max_windows_per_session == wmux::kMaxWindowsPerSession);
  assert(result.config.limits.max_panes_per_window == wmux::kMaxPanesPerWindow);
  assert(result.config.limits.max_pane_scrollback_lines == wmux::kMaxPaneScrollbackLines);
  assert(result.config.limits.max_paste_buffer_bytes == wmux::kMaxPasteBufferBytes);
}

void supports_quoted_default_shell() {
  const auto result = wmux::parse_config_text("set -g default-shell \"pwsh.exe -NoLogo\"\n");

  assert(result.ok());
  assert(result.config.default_shell == "pwsh.exe -NoLogo");
}

void rejects_unknown_option() {
  const auto result = wmux::parse_config_text("set -g theme dark\n");

  assert(!result.ok());
  assert(result.errors.size() == 1);
  assert(result.errors[0].line == 1);
  assert(result.errors[0].message == "unsupported option 'theme'");
}

void rejects_bad_boolean_values() {
  const auto result = wmux::parse_config_text(
      "set -g mouse yes\n"
      "set -g status disabled\n"
      "set -g terminal-mouse maybe\n"
      "set -g ui-tmux-style maybe\n"
      "set -g border-style dotted\n");

  assert(!result.ok());
  assert(result.errors.size() == 5);
  assert(result.errors[0].line == 1);
  assert(result.errors[0].message == "mouse must be on or off");
  assert(result.errors[1].line == 2);
  assert(result.errors[1].message == "status must be on or off");
  assert(result.errors[2].line == 3);
  assert(result.errors[2].message == "terminal-mouse must be on or off");
  assert(result.errors[3].line == 4);
  assert(result.errors[3].message == "ui-tmux-style must be on or off");
  assert(result.errors[4].line == 5);
  assert(result.errors[4].message == "border-style must be smooth or ascii");
}

void rejects_bad_prefix_format() {
  const auto result = wmux::parse_config_text("set -g prefix Ctrl-b\n");

  assert(!result.ok());
  assert(result.errors.size() == 1);
  assert(result.errors[0].message == "prefix must use C-<key> format");
}

void rejects_bad_escape_time() {
  const auto result = wmux::parse_config_text(
      "set -g escape-time-ms nope\n"
      "set -g input-escape-time-ms 5001\n");

  assert(!result.ok());
  assert(result.errors.size() == 2);
  assert(result.errors[0].message == "escape-time-ms must be between 0 and 5000");
  assert(result.errors[1].message == "escape-time-ms must be between 0 and 5000");
}

void parses_hex_and_index_accent_colors() {
  const auto hex = wmux::parse_config_text("set -g accent \"#1e90ff\"\n");
  assert(hex.ok());
  assert(hex.config.ui.accent.kind == wmux::UiColorKind::Rgb);
  assert(hex.config.ui.accent.red == 0x1e);
  assert(hex.config.ui.accent.green == 0x90);
  assert(hex.config.ui.accent.blue == 0xff);

  const auto indexed = wmux::parse_config_text("set -g ui-accent 12\n");
  assert(indexed.ok());
  assert(indexed.config.ui.accent.kind == wmux::UiColorKind::Indexed);
  assert(indexed.config.ui.accent.index == 12);
}

void rejects_bad_resource_limits() {
  const auto result = wmux::parse_config_text(
      "set -g max-sessions 0\n"
      "set -g max-windows-per-session nope\n"
      "set -g max-panes-per-window 2000\n"
      "set -g pane-raw-output-max-bytes 1\n"
      "set -g ipc-frame-max-bytes 999999999\n");

  assert(!result.ok());
  assert(result.errors.size() == 5);
  assert(result.errors[0].line == 1);
  assert(result.errors[0].message == "max-sessions must be between 1 and 1024");
  assert(result.errors[1].line == 2);
  assert(result.errors[1].message == "max-windows-per-session must be between 1 and 1024");
  assert(result.errors[2].line == 3);
  assert(result.errors[2].message == "max-panes-per-window must be between 1 and 1024");
  assert(result.errors[3].line == 4);
  assert(result.errors[3].message == "pane-raw-output-max-bytes must be between 65536 and 268435456");
  assert(result.errors[4].line == 5);
  assert(result.errors[4].message ==
         "ipc-frame-max-bytes must be between 1 and the compiled IPC frame hard limit");
}

void rejects_bad_accent_color() {
  const auto result = wmux::parse_config_text("set -g accent nope-blue-ish\n");

  assert(!result.ok());
  assert(result.errors.size() == 1);
  assert(result.errors[0].message ==
         "accent must be a named color, 0-255 color index, or #RRGGBB value");
}

void rejects_missing_explicit_shell_path() {
  const auto result =
      wmux::parse_config_text(R"(set -g default-shell C:\definitely-missing-wmux\pwsh.exe
)");

  assert(!result.ok());
  assert(result.errors.size() == 1);
  assert(result.errors[0].message == "default-shell path does not exist");
}

void applies_single_global_option_with_shared_validation() {
  wmux::Config config;
  const auto ok = wmux::apply_global_config_option(config, "status", "off");
  assert(!ok);
  assert(!config.status_bar_enabled);

  const auto bad = wmux::apply_global_config_option(config, "max-sessions", "0");
  assert(bad);
  assert(bad->message == "max-sessions must be between 1 and 1024");
}

void applies_key_binding_options_with_shared_validation() {
  wmux::Config config;
  const auto bind = wmux::apply_key_binding_config(config, "z", "new-window");
  assert(!bind);
  assert(config.keys.bindings.at("z") == "new-window");

  const auto unbind = wmux::apply_key_unbinding_config(config, "z");
  assert(!unbind);
  assert(config.keys.bindings.at("z") == "none");

  const auto bad_key = wmux::apply_key_binding_config(config, "Imaginary", "new-window");
  assert(bad_key);
  assert(bad_key->message == "unsupported key binding key 'Imaginary'");

  const auto bad_action = wmux::apply_key_binding_config(config, "z", "imaginary-command");
  assert(bad_action);
  assert(bad_action->message == "unsupported key binding action 'imaginary-command'");
}

void rejects_wrong_shape() {
  const auto result = wmux::parse_config_text("mouse on\n");

  assert(!result.ok());
  assert(result.errors.size() == 1);
  assert(result.errors[0].message == "expected: set -g <option> <value>");
}

void rejects_bad_key_binding_config() {
  const auto result = wmux::parse_config_text(
      "bind-key Imaginary new-window\n"
      "bind-key z imaginary-command\n"
      "bind-key z\n"
      "unbind-key Imaginary\n");

  assert(!result.ok());
  assert(result.errors.size() == 4);
  assert(result.errors[0].message == "unsupported key binding key 'Imaginary'");
  assert(result.errors[1].message == "unsupported key binding action 'imaginary-command'");
  assert(result.errors[2].message == "expected: bind-key <key> <action>");
  assert(result.errors[3].message == "unsupported key binding key 'Imaginary'");
}

void rejects_bad_terminal_host() {
  const auto result = wmux::parse_config_text("set -g terminal-host imaginary\n");

  assert(!result.ok());
  assert(result.errors.size() == 1);
  assert(result.errors[0].message == "terminal-host is not recognized");
}

void rejects_bad_terminal_quirk() {
  const auto result = wmux::parse_config_text("set -g terminal-quirk-imaginary on\n");

  assert(!result.ok());
  assert(result.errors.size() == 1);
  assert(result.errors[0].message == "terminal quirk is not recognized");
}

void rejects_unterminated_quote() {
  const auto result = wmux::parse_config_text("set -g default-shell \"pwsh.exe\n");

  assert(!result.ok());
  assert(result.errors.size() == 1);
  assert(result.errors[0].message == "unterminated quoted value");
}

void rejects_oversized_lines() {
  std::string text(wmux::kMaxConfigLineBytes + 1, 'x');
  text += "\nset -g mouse on\n";

  const auto result = wmux::parse_config_text(text);

  assert(!result.ok());
  assert(result.errors.size() == 1);
  assert(result.errors[0].line == 1);
  assert(result.errors[0].message == "config line is too long");
  assert(result.config.mouse_enabled);
}

void missing_config_file_uses_defaults() {
  const auto result = wmux::load_config_file("definitely-missing-wmux-test.conf");

  assert(result.ok());
  assert(result.config.prefix == "C-b");
}

}  // namespace

void run_config_tests() {
  parses_supported_settings();
  preserves_defaults_for_empty_config();
  supports_quoted_default_shell();
  rejects_unknown_option();
  rejects_bad_boolean_values();
  rejects_bad_prefix_format();
  rejects_bad_escape_time();
  parses_hex_and_index_accent_colors();
  rejects_bad_resource_limits();
  rejects_bad_accent_color();
  rejects_missing_explicit_shell_path();
  applies_single_global_option_with_shared_validation();
  applies_key_binding_options_with_shared_validation();
  rejects_wrong_shape();
  rejects_bad_key_binding_config();
  rejects_bad_terminal_host();
  rejects_bad_terminal_quirk();
  rejects_unterminated_quote();
  rejects_oversized_lines();
  missing_config_file_uses_defaults();
}
