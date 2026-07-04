#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wmux {

enum class TerminalHost {
  WindowsTerminal,
  VSCode,
  WezTerm,
  Alacritty,
  Conhost,
  Unknown,
};

enum class TerminalQuirk {
  BrokenAltKeySequences,
  CtrlBreakSpecialHandling,
  MouseWheelEncodingDiffers,
  NoOsc52Clipboard,
  VscodeKeyTranslation,
  LegacyConhostMode,
  CursorStyleUnsupported,
  UnknownEscapeSequences,
};

struct TerminalCapabilities {
  TerminalHost host{TerminalHost::Unknown};
  bool supports_truecolor{false};
  bool supports_256_color{false};
  bool supports_sgr_mouse{false};
  bool supports_mouse_drag{false};
  bool supports_mouse_wheel{false};
  bool supports_bracketed_paste{false};
  bool supports_focus_events{false};
  bool supports_cursor_style{false};
  bool supports_alt_screen{false};
  bool supports_extended_keys{false};
  bool supports_osc52_clipboard{false};
  bool supports_synchronized_output{false};
  std::vector<TerminalQuirk> quirks;
};

struct TerminalCapabilityOverrides {
  std::optional<TerminalHost> host;
  std::optional<bool> supports_truecolor;
  std::optional<bool> supports_256_color;
  std::optional<bool> supports_sgr_mouse;
  std::optional<bool> supports_mouse_drag;
  std::optional<bool> supports_mouse_wheel;
  std::optional<bool> supports_bracketed_paste;
  std::optional<bool> supports_focus_events;
  std::optional<bool> supports_cursor_style;
  std::optional<bool> supports_alt_screen;
  std::optional<bool> supports_extended_keys;
  std::optional<bool> supports_osc52_clipboard;
  std::optional<bool> supports_synchronized_output;
  std::optional<bool> quirk_broken_alt_key_sequences;
  std::optional<bool> quirk_ctrl_break_special_handling;
  std::optional<bool> quirk_mouse_wheel_encoding_differs;
  std::optional<bool> quirk_no_osc52_clipboard;
  std::optional<bool> quirk_vscode_key_translation;
  std::optional<bool> quirk_legacy_conhost_mode;
  std::optional<bool> quirk_cursor_style_unsupported;
  std::optional<bool> quirk_unknown_escape_sequences;
};

using EnvironmentSnapshot = std::vector<std::pair<std::string, std::string>>;

TerminalCapabilities detect_terminal_capabilities();
TerminalCapabilities detect_terminal_capabilities_from_environment(
    const EnvironmentSnapshot& environment,
    bool console_available);
TerminalCapabilities apply_terminal_capability_overrides(
    TerminalCapabilities capabilities,
    const TerminalCapabilityOverrides& overrides);
bool has_terminal_capability_overrides(const TerminalCapabilityOverrides& overrides);
std::optional<TerminalHost> parse_terminal_host(std::string_view value);
std::optional<TerminalQuirk> parse_terminal_quirk(std::string_view value);
std::string terminal_host_name(TerminalHost host);
std::string terminal_quirk_name(TerminalQuirk quirk);

}  // namespace wmux
