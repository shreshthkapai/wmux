#include "wmux/terminal_capabilities.hpp"

#include <cassert>

namespace {

void detects_windows_terminal() {
  const wmux::EnvironmentSnapshot environment{{"WT_SESSION", "abc"}};
  const auto capabilities =
      wmux::detect_terminal_capabilities_from_environment(environment, true);

  assert(capabilities.host == wmux::TerminalHost::WindowsTerminal);
  assert(capabilities.supports_truecolor);
  assert(capabilities.supports_sgr_mouse);
  assert(capabilities.supports_bracketed_paste);
  assert(capabilities.supports_synchronized_output);
}

void detects_vscode_before_conhost() {
  const wmux::EnvironmentSnapshot environment{{"TERM_PROGRAM", "vscode"}};
  const auto capabilities =
      wmux::detect_terminal_capabilities_from_environment(environment, true);

  assert(capabilities.host == wmux::TerminalHost::VSCode);
  assert(capabilities.supports_extended_keys);
  assert(!capabilities.quirks.empty());
}

void detects_modern_third_party_terminals() {
  const auto wezterm = wmux::detect_terminal_capabilities_from_environment(
      {{"TERM_PROGRAM", "WezTerm"}},
      true);
  const auto alacritty = wmux::detect_terminal_capabilities_from_environment(
      {{"ALACRITTY_SOCKET", "socket"}},
      true);
  const auto alacritty_from_term = wmux::detect_terminal_capabilities_from_environment(
      {{"TERM", "alacritty"}},
      true);

  assert(wezterm.host == wmux::TerminalHost::WezTerm);
  assert(wezterm.supports_osc52_clipboard);
  assert(wezterm.supports_synchronized_output);
  assert(alacritty.host == wmux::TerminalHost::Alacritty);
  assert(alacritty.supports_truecolor);
  assert(alacritty.supports_synchronized_output);
  assert(alacritty_from_term.host == wmux::TerminalHost::Alacritty);
  assert(alacritty_from_term.supports_truecolor);
  assert(alacritty_from_term.supports_synchronized_output);
}

void falls_back_to_conhost_for_plain_console() {
  const auto capabilities = wmux::detect_terminal_capabilities_from_environment({}, true);

  assert(capabilities.host == wmux::TerminalHost::Conhost);
  assert(capabilities.supports_256_color);
  assert(!capabilities.supports_sgr_mouse);
}

void reports_unknown_without_console_or_environment() {
  const auto capabilities = wmux::detect_terminal_capabilities_from_environment({}, false);

  assert(capabilities.host == wmux::TerminalHost::Unknown);
  assert(!capabilities.supports_truecolor);
  assert(!capabilities.quirks.empty());
}

void names_hosts_and_quirks() {
  assert(wmux::terminal_host_name(wmux::TerminalHost::WindowsTerminal) == "windows-terminal");
  assert(wmux::terminal_quirk_name(wmux::TerminalQuirk::VscodeKeyTranslation) ==
         "vscode-key-translation");
  assert(wmux::parse_terminal_host("wt") == wmux::TerminalHost::WindowsTerminal);
  assert(wmux::parse_terminal_quirk("no-osc52-clipboard") ==
         wmux::TerminalQuirk::NoOsc52Clipboard);
  assert(!wmux::parse_terminal_host("imaginary"));
  assert(!wmux::parse_terminal_quirk("imaginary"));
}

void applies_terminal_overrides() {
  auto capabilities = wmux::detect_terminal_capabilities_from_environment({}, true);
  wmux::TerminalCapabilityOverrides overrides;
  overrides.host = wmux::TerminalHost::WezTerm;
  overrides.supports_osc52_clipboard = false;
  overrides.supports_sgr_mouse = false;
  overrides.quirk_no_osc52_clipboard = true;

  const auto effective =
      wmux::apply_terminal_capability_overrides(capabilities, overrides);

  assert(wmux::has_terminal_capability_overrides(overrides));
  assert(effective.host == wmux::TerminalHost::WezTerm);
  assert(!effective.supports_osc52_clipboard);
  assert(!effective.supports_sgr_mouse);
  bool found_no_osc52 = false;
  for (const auto quirk : effective.quirks) {
    if (quirk == wmux::TerminalQuirk::NoOsc52Clipboard) {
      found_no_osc52 = true;
    }
  }
  assert(found_no_osc52);
}

}  // namespace

void run_terminal_capabilities_tests() {
  detects_windows_terminal();
  detects_vscode_before_conhost();
  detects_modern_third_party_terminals();
  falls_back_to_conhost_for_plain_console();
  reports_unknown_without_console_or_environment();
  names_hosts_and_quirks();
  applies_terminal_overrides();
}
