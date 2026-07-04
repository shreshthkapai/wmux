#include "wmux/terminal_capabilities.hpp"

#include "wmux/platform/services.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>

namespace wmux {
namespace {

std::string ascii_lower(std::string_view value) {
  std::string lowered;
  lowered.reserve(value.size());
  for (const char ch : value) {
    lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return lowered;
}

std::string_view environment_value(
    const EnvironmentSnapshot& environment,
    std::string_view key) {
  for (const auto& [name, value] : environment) {
    if (name == key) {
      return value;
    }
  }
  return {};
}

bool has_environment_value(const EnvironmentSnapshot& environment, std::string_view key) {
  return !environment_value(environment, key).empty();
}

void add_quirk(TerminalCapabilities& capabilities, TerminalQuirk quirk) {
  if (std::find(capabilities.quirks.begin(), capabilities.quirks.end(), quirk) ==
      capabilities.quirks.end()) {
    capabilities.quirks.push_back(quirk);
  }
}

void set_quirk(TerminalCapabilities& capabilities, TerminalQuirk quirk, bool enabled) {
  auto& quirks = capabilities.quirks;
  const auto existing = std::find(quirks.begin(), quirks.end(), quirk);
  if (enabled) {
    if (existing == quirks.end()) {
      quirks.push_back(quirk);
    }
    return;
  }

  if (existing != quirks.end()) {
    quirks.erase(existing);
  }
}

void apply_bool_override(std::optional<bool> override_value, bool& target) {
  if (override_value) {
    target = *override_value;
  }
}

void apply_quirk_override(
    TerminalCapabilities& capabilities,
    std::optional<bool> override_value,
    TerminalQuirk quirk) {
  if (override_value) {
    set_quirk(capabilities, quirk, *override_value);
  }
}

TerminalCapabilities capabilities_for_host(TerminalHost host) {
  TerminalCapabilities capabilities;
  capabilities.host = host;

  switch (host) {
    case TerminalHost::WindowsTerminal:
      capabilities.supports_truecolor = true;
      capabilities.supports_256_color = true;
      capabilities.supports_sgr_mouse = true;
      capabilities.supports_mouse_drag = true;
      capabilities.supports_mouse_wheel = true;
      capabilities.supports_bracketed_paste = true;
      capabilities.supports_cursor_style = true;
      capabilities.supports_alt_screen = true;
      capabilities.supports_extended_keys = true;
      capabilities.supports_synchronized_output = true;
      add_quirk(capabilities, TerminalQuirk::NoOsc52Clipboard);
      break;

    case TerminalHost::VSCode:
      capabilities.supports_truecolor = true;
      capabilities.supports_256_color = true;
      capabilities.supports_sgr_mouse = true;
      capabilities.supports_mouse_drag = true;
      capabilities.supports_mouse_wheel = true;
      capabilities.supports_bracketed_paste = true;
      capabilities.supports_cursor_style = true;
      capabilities.supports_alt_screen = true;
      capabilities.supports_extended_keys = true;
      capabilities.supports_synchronized_output = true;
      add_quirk(capabilities, TerminalQuirk::VscodeKeyTranslation);
      add_quirk(capabilities, TerminalQuirk::NoOsc52Clipboard);
      break;

    case TerminalHost::WezTerm:
      capabilities.supports_truecolor = true;
      capabilities.supports_256_color = true;
      capabilities.supports_sgr_mouse = true;
      capabilities.supports_mouse_drag = true;
      capabilities.supports_mouse_wheel = true;
      capabilities.supports_bracketed_paste = true;
      capabilities.supports_focus_events = true;
      capabilities.supports_cursor_style = true;
      capabilities.supports_alt_screen = true;
      capabilities.supports_extended_keys = true;
      capabilities.supports_osc52_clipboard = true;
      capabilities.supports_synchronized_output = true;
      break;

    case TerminalHost::Alacritty:
      capabilities.supports_truecolor = true;
      capabilities.supports_256_color = true;
      capabilities.supports_sgr_mouse = true;
      capabilities.supports_mouse_drag = true;
      capabilities.supports_mouse_wheel = true;
      capabilities.supports_bracketed_paste = true;
      capabilities.supports_focus_events = true;
      capabilities.supports_cursor_style = true;
      capabilities.supports_alt_screen = true;
      capabilities.supports_extended_keys = true;
      capabilities.supports_osc52_clipboard = true;
      capabilities.supports_synchronized_output = true;
      break;

    case TerminalHost::Conhost:
      capabilities.supports_256_color = true;
      capabilities.supports_alt_screen = true;
      add_quirk(capabilities, TerminalQuirk::LegacyConhostMode);
      add_quirk(capabilities, TerminalQuirk::CtrlBreakSpecialHandling);
      add_quirk(capabilities, TerminalQuirk::CursorStyleUnsupported);
      add_quirk(capabilities, TerminalQuirk::NoOsc52Clipboard);
      break;

    case TerminalHost::Unknown:
      add_quirk(capabilities, TerminalQuirk::UnknownEscapeSequences);
      add_quirk(capabilities, TerminalQuirk::NoOsc52Clipboard);
      break;
  }

  return capabilities;
}

TerminalHost detect_host_from_environment(
    const EnvironmentSnapshot& environment,
    bool console_available) {
  if (has_environment_value(environment, "WT_SESSION")) {
    return TerminalHost::WindowsTerminal;
  }

  const auto term_program = ascii_lower(environment_value(environment, "TERM_PROGRAM"));
  if (term_program == "vscode" || has_environment_value(environment, "VSCODE_PID")) {
    return TerminalHost::VSCode;
  }

  if (term_program == "wezterm" || has_environment_value(environment, "WEZTERM_PANE")) {
    return TerminalHost::WezTerm;
  }

  const auto term = ascii_lower(environment_value(environment, "TERM"));
  if (term_program == "alacritty" || term == "alacritty" ||
      has_environment_value(environment, "ALACRITTY_SOCKET") ||
      has_environment_value(environment, "ALACRITTY_LOG")) {
    return TerminalHost::Alacritty;
  }

  if (console_available) {
    return TerminalHost::Conhost;
  }

  return TerminalHost::Unknown;
}

std::string read_environment_value(const char* name) {
  return platform_services().info().environment_variable(name);
}

void capture_environment_value(EnvironmentSnapshot& environment, const char* name) {
  auto value = read_environment_value(name);
  if (!value.empty()) {
    environment.emplace_back(name, std::move(value));
  }
}

}  // namespace

TerminalCapabilities detect_terminal_capabilities() {
  EnvironmentSnapshot environment;
  capture_environment_value(environment, "WT_SESSION");
  capture_environment_value(environment, "TERM");
  capture_environment_value(environment, "TERM_PROGRAM");
  capture_environment_value(environment, "VSCODE_PID");
  capture_environment_value(environment, "WEZTERM_PANE");
  capture_environment_value(environment, "ALACRITTY_SOCKET");
  capture_environment_value(environment, "ALACRITTY_LOG");

  return detect_terminal_capabilities_from_environment(
      environment,
      platform_services().terminal().has_console_output());
}

TerminalCapabilities detect_terminal_capabilities_from_environment(
    const EnvironmentSnapshot& environment,
    bool console_available) {
  return capabilities_for_host(detect_host_from_environment(environment, console_available));
}

TerminalCapabilities apply_terminal_capability_overrides(
    TerminalCapabilities capabilities,
    const TerminalCapabilityOverrides& overrides) {
  if (overrides.host) {
    capabilities = capabilities_for_host(*overrides.host);
  }

  apply_bool_override(overrides.supports_truecolor, capabilities.supports_truecolor);
  apply_bool_override(overrides.supports_256_color, capabilities.supports_256_color);
  apply_bool_override(overrides.supports_sgr_mouse, capabilities.supports_sgr_mouse);
  apply_bool_override(overrides.supports_mouse_drag, capabilities.supports_mouse_drag);
  apply_bool_override(overrides.supports_mouse_wheel, capabilities.supports_mouse_wheel);
  apply_bool_override(
      overrides.supports_bracketed_paste,
      capabilities.supports_bracketed_paste);
  apply_bool_override(overrides.supports_focus_events, capabilities.supports_focus_events);
  apply_bool_override(overrides.supports_cursor_style, capabilities.supports_cursor_style);
  apply_bool_override(overrides.supports_alt_screen, capabilities.supports_alt_screen);
  apply_bool_override(overrides.supports_extended_keys, capabilities.supports_extended_keys);
  apply_bool_override(
      overrides.supports_osc52_clipboard,
      capabilities.supports_osc52_clipboard);
  apply_bool_override(
      overrides.supports_synchronized_output,
      capabilities.supports_synchronized_output);

  apply_quirk_override(
      capabilities,
      overrides.quirk_broken_alt_key_sequences,
      TerminalQuirk::BrokenAltKeySequences);
  apply_quirk_override(
      capabilities,
      overrides.quirk_ctrl_break_special_handling,
      TerminalQuirk::CtrlBreakSpecialHandling);
  apply_quirk_override(
      capabilities,
      overrides.quirk_mouse_wheel_encoding_differs,
      TerminalQuirk::MouseWheelEncodingDiffers);
  apply_quirk_override(
      capabilities,
      overrides.quirk_no_osc52_clipboard,
      TerminalQuirk::NoOsc52Clipboard);
  apply_quirk_override(
      capabilities,
      overrides.quirk_vscode_key_translation,
      TerminalQuirk::VscodeKeyTranslation);
  apply_quirk_override(
      capabilities,
      overrides.quirk_legacy_conhost_mode,
      TerminalQuirk::LegacyConhostMode);
  apply_quirk_override(
      capabilities,
      overrides.quirk_cursor_style_unsupported,
      TerminalQuirk::CursorStyleUnsupported);
  apply_quirk_override(
      capabilities,
      overrides.quirk_unknown_escape_sequences,
      TerminalQuirk::UnknownEscapeSequences);

  return capabilities;
}

bool has_terminal_capability_overrides(const TerminalCapabilityOverrides& overrides) {
  return overrides.host || overrides.supports_truecolor || overrides.supports_256_color ||
         overrides.supports_sgr_mouse || overrides.supports_mouse_drag ||
         overrides.supports_mouse_wheel || overrides.supports_bracketed_paste ||
         overrides.supports_focus_events || overrides.supports_cursor_style ||
         overrides.supports_alt_screen || overrides.supports_extended_keys ||
         overrides.supports_osc52_clipboard || overrides.supports_synchronized_output ||
         overrides.quirk_broken_alt_key_sequences ||
         overrides.quirk_ctrl_break_special_handling ||
         overrides.quirk_mouse_wheel_encoding_differs ||
         overrides.quirk_no_osc52_clipboard ||
         overrides.quirk_vscode_key_translation ||
         overrides.quirk_legacy_conhost_mode ||
         overrides.quirk_cursor_style_unsupported ||
         overrides.quirk_unknown_escape_sequences;
}

std::optional<TerminalHost> parse_terminal_host(std::string_view value) {
  const auto normalized = ascii_lower(value);
  if (normalized == "windows-terminal" || normalized == "windows_terminal" ||
      normalized == "wt") {
    return TerminalHost::WindowsTerminal;
  }
  if (normalized == "vscode" || normalized == "vs-code") {
    return TerminalHost::VSCode;
  }
  if (normalized == "wezterm") {
    return TerminalHost::WezTerm;
  }
  if (normalized == "alacritty") {
    return TerminalHost::Alacritty;
  }
  if (normalized == "conhost" || normalized == "console") {
    return TerminalHost::Conhost;
  }
  if (normalized == "unknown") {
    return TerminalHost::Unknown;
  }
  return std::nullopt;
}

std::optional<TerminalQuirk> parse_terminal_quirk(std::string_view value) {
  const auto normalized = ascii_lower(value);
  if (normalized == "broken-alt-key-sequences") {
    return TerminalQuirk::BrokenAltKeySequences;
  }
  if (normalized == "ctrl-break-special-handling") {
    return TerminalQuirk::CtrlBreakSpecialHandling;
  }
  if (normalized == "mouse-wheel-encoding-differs") {
    return TerminalQuirk::MouseWheelEncodingDiffers;
  }
  if (normalized == "no-osc52-clipboard") {
    return TerminalQuirk::NoOsc52Clipboard;
  }
  if (normalized == "vscode-key-translation") {
    return TerminalQuirk::VscodeKeyTranslation;
  }
  if (normalized == "legacy-conhost-mode") {
    return TerminalQuirk::LegacyConhostMode;
  }
  if (normalized == "cursor-style-unsupported") {
    return TerminalQuirk::CursorStyleUnsupported;
  }
  if (normalized == "unknown-escape-sequences") {
    return TerminalQuirk::UnknownEscapeSequences;
  }
  return std::nullopt;
}

std::string terminal_host_name(TerminalHost host) {
  switch (host) {
    case TerminalHost::WindowsTerminal:
      return "windows-terminal";
    case TerminalHost::VSCode:
      return "vscode";
    case TerminalHost::WezTerm:
      return "wezterm";
    case TerminalHost::Alacritty:
      return "alacritty";
    case TerminalHost::Conhost:
      return "conhost";
    case TerminalHost::Unknown:
      return "unknown";
  }

  return "unknown";
}

std::string terminal_quirk_name(TerminalQuirk quirk) {
  switch (quirk) {
    case TerminalQuirk::BrokenAltKeySequences:
      return "broken-alt-key-sequences";
    case TerminalQuirk::CtrlBreakSpecialHandling:
      return "ctrl-break-special-handling";
    case TerminalQuirk::MouseWheelEncodingDiffers:
      return "mouse-wheel-encoding-differs";
    case TerminalQuirk::NoOsc52Clipboard:
      return "no-osc52-clipboard";
    case TerminalQuirk::VscodeKeyTranslation:
      return "vscode-key-translation";
    case TerminalQuirk::LegacyConhostMode:
      return "legacy-conhost-mode";
    case TerminalQuirk::CursorStyleUnsupported:
      return "cursor-style-unsupported";
    case TerminalQuirk::UnknownEscapeSequences:
      return "unknown-escape-sequences";
  }

  return "unknown-escape-sequences";
}

}  // namespace wmux
