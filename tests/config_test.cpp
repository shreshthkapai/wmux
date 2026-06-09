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
      "set -g status off\n");

  assert(result.ok());
  assert(result.config.prefix == "C-b");
  assert(result.config.mouse_enabled);
  assert(result.config.default_shell == "powershell.exe");
  assert(!result.config.status_bar_enabled);
}

void preserves_defaults_for_empty_config() {
  const auto result = wmux::parse_config_text("");

  assert(result.ok());
  assert(result.config.prefix == "C-b");
  assert(!result.config.mouse_enabled);
  assert(result.config.default_shell == "powershell.exe -NoLogo -NoProfile");
  assert(result.config.status_bar_enabled);
}

void supports_quoted_default_shell() {
  const auto result = wmux::parse_config_text(
      "set -g default-shell \"C:\\Program Files\\PowerShell\\7\\pwsh.exe -NoLogo\"\n");

  assert(result.ok());
  assert(result.config.default_shell == "C:\\Program Files\\PowerShell\\7\\pwsh.exe -NoLogo");
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
      "set -g status disabled\n");

  assert(!result.ok());
  assert(result.errors.size() == 2);
  assert(result.errors[0].line == 1);
  assert(result.errors[0].message == "mouse must be on or off");
  assert(result.errors[1].line == 2);
  assert(result.errors[1].message == "status must be on or off");
}

void rejects_bad_prefix_format() {
  const auto result = wmux::parse_config_text("set -g prefix Ctrl-b\n");

  assert(!result.ok());
  assert(result.errors.size() == 1);
  assert(result.errors[0].message == "prefix must use C-<key> format");
}

void rejects_wrong_shape() {
  const auto result = wmux::parse_config_text("mouse on\n");

  assert(!result.ok());
  assert(result.errors.size() == 1);
  assert(result.errors[0].message == "expected: set -g <option> <value>");
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
  rejects_wrong_shape();
  rejects_unterminated_quote();
  rejects_oversized_lines();
  missing_config_file_uses_defaults();
}
