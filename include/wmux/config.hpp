#pragma once

#include "wmux/resource_limits.hpp"
#include "wmux/terminal_capabilities.hpp"
#include "wmux/ui_theme.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <unordered_map>
#include <string>
#include <string_view>
#include <vector>

namespace wmux {

struct GlobalOptions {
  std::string prefix{"C-b"};
  bool mouse_enabled{true};
  // Empty means auto-resolve using the platform shell contract.
  std::string default_shell;
  bool status_bar_enabled{true};
  std::uint16_t escape_time_ms{50};
  TerminalCapabilityOverrides terminal_overrides;
  UiTheme ui;
  ResourceLimits limits;
};

struct SessionConfigOptions {
  std::size_t scrollback_max_lines{kMaxPaneScrollbackLines};
};

struct WindowConfigOptions {};

struct PaneConfigOptions {};

struct ClientConfigOptions {
  std::size_t output_queue_bytes{kMaxAttachPendingOutputBytes};
  std::size_t output_queue_frames{kMaxAttachPendingOutputFrames};
};

struct KeyBindingConfig {
  std::unordered_map<std::string, std::string> bindings;
};

struct Config {
  GlobalOptions global;
  SessionConfigOptions session;
  WindowConfigOptions window;
  PaneConfigOptions pane;
  ClientConfigOptions client;
  KeyBindingConfig keys;

  std::string& prefix{global.prefix};
  bool& mouse_enabled{global.mouse_enabled};
  std::string& default_shell{global.default_shell};
  bool& status_bar_enabled{global.status_bar_enabled};
  std::uint16_t& escape_time_ms{global.escape_time_ms};
  TerminalCapabilityOverrides& terminal_overrides{global.terminal_overrides};
  UiTheme& ui{global.ui};
  ResourceLimits& limits{global.limits};

  Config() = default;
  Config(const Config& other);
  Config& operator=(const Config& other);
  Config(Config&& other) noexcept;
  Config& operator=(Config&& other) noexcept;
};

struct ConfigParseError {
  std::size_t line{0};
  std::string message;
};

struct ConfigParseResult {
  Config config;
  std::vector<ConfigParseError> errors;

  [[nodiscard]] bool ok() const {
    return errors.empty();
  }
};

std::filesystem::path default_config_path();
std::optional<ConfigParseError> apply_global_config_option(
    Config& config,
    std::string_view option,
    std::string_view value);
std::optional<ConfigParseError> apply_key_binding_config(
    Config& config,
    std::string_view key,
    std::string_view action);
std::optional<ConfigParseError> apply_key_unbinding_config(
    Config& config,
    std::string_view key);
ConfigParseResult parse_config_text(std::string_view text);
ConfigParseResult load_config_file(const std::filesystem::path& path);

}  // namespace wmux
