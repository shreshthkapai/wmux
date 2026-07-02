#include "wmux/config.hpp"

#include "wmux/attach_keymap.hpp"
#include "wmux/platform/services.hpp"
#include "wmux/resource_limits.hpp"

#include <charconv>
#include <cctype>
#include <fstream>
#include <limits>
#include <sstream>
#include <system_error>
#include <utility>

namespace wmux {
namespace {

bool ascii_space(char byte) {
  return std::isspace(static_cast<unsigned char>(byte)) != 0;
}

bool supported_control_prefix(std::string_view value) {
  return value.size() == 3 && (value[0] == 'C' || value[0] == 'c') && value[1] == '-' &&
         value[2] >= '?' && value[2] <= '~';
}

bool parse_on_off(std::string_view value, bool& out) {
  if (value == "on") {
    out = true;
    return true;
  }
  if (value == "off") {
    out = false;
    return true;
  }
  return false;
}

bool parse_uint16(std::string_view value, std::uint16_t& out) {
  unsigned int parsed = 0;
  const auto [ptr, error] =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || ptr != value.data() + value.size() ||
      parsed > std::numeric_limits<std::uint16_t>::max()) {
    return false;
  }

  out = static_cast<std::uint16_t>(parsed);
  return true;
}

bool parse_size(std::string_view value, std::size_t& out) {
  std::uint64_t parsed = 0;
  const auto [ptr, error] =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || ptr != value.data() + value.size() ||
      parsed > std::numeric_limits<std::size_t>::max()) {
    return false;
  }

  out = static_cast<std::size_t>(parsed);
  return true;
}

bool parse_bounded_size(
    std::string_view value,
    std::size_t minimum,
    std::size_t maximum,
    std::size_t& out) {
  std::size_t parsed = 0;
  if (!parse_size(value, parsed) || parsed < minimum || parsed > maximum) {
    return false;
  }
  out = parsed;
  return true;
}

void set_terminal_quirk_override(
    TerminalCapabilityOverrides& overrides,
    TerminalQuirk quirk,
    bool enabled) {
  switch (quirk) {
    case TerminalQuirk::BrokenAltKeySequences:
      overrides.quirk_broken_alt_key_sequences = enabled;
      break;
    case TerminalQuirk::CtrlBreakSpecialHandling:
      overrides.quirk_ctrl_break_special_handling = enabled;
      break;
    case TerminalQuirk::MouseWheelEncodingDiffers:
      overrides.quirk_mouse_wheel_encoding_differs = enabled;
      break;
    case TerminalQuirk::NoOsc52Clipboard:
      overrides.quirk_no_osc52_clipboard = enabled;
      break;
    case TerminalQuirk::VscodeKeyTranslation:
      overrides.quirk_vscode_key_translation = enabled;
      break;
    case TerminalQuirk::LegacyConhostMode:
      overrides.quirk_legacy_conhost_mode = enabled;
      break;
    case TerminalQuirk::CursorStyleUnsupported:
      overrides.quirk_cursor_style_unsupported = enabled;
      break;
    case TerminalQuirk::UnknownEscapeSequences:
      overrides.quirk_unknown_escape_sequences = enabled;
      break;
  }
}

bool parse_terminal_override(
    Config& config,
    std::string_view option,
    std::string_view value,
    std::size_t line_number,
    std::vector<ConfigParseError>& errors) {
  constexpr std::string_view kTerminalPrefix = "terminal-";
  if (option.rfind(kTerminalPrefix, 0) != 0) {
    return false;
  }

  const auto terminal_option = option.substr(kTerminalPrefix.size());
  if (terminal_option == "host") {
    const auto host = parse_terminal_host(value);
    if (!host) {
      errors.push_back({line_number, "terminal-host is not recognized"});
      return true;
    }
    config.terminal_overrides.host = *host;
    return true;
  }

  bool enabled = false;
  if (!parse_on_off(value, enabled)) {
    errors.push_back({line_number, std::string{option} + " must be on or off"});
    return true;
  }

  auto& overrides = config.terminal_overrides;
  if (terminal_option == "truecolor") {
    overrides.supports_truecolor = enabled;
    return true;
  }
  if (terminal_option == "256-color") {
    overrides.supports_256_color = enabled;
    return true;
  }
  if (terminal_option == "mouse") {
    overrides.supports_sgr_mouse = enabled;
    return true;
  }
  if (terminal_option == "mouse-drag") {
    overrides.supports_mouse_drag = enabled;
    return true;
  }
  if (terminal_option == "mouse-wheel") {
    overrides.supports_mouse_wheel = enabled;
    return true;
  }
  if (terminal_option == "bracketed-paste") {
    overrides.supports_bracketed_paste = enabled;
    return true;
  }
  if (terminal_option == "focus-events") {
    overrides.supports_focus_events = enabled;
    return true;
  }
  if (terminal_option == "cursor-style") {
    overrides.supports_cursor_style = enabled;
    return true;
  }
  if (terminal_option == "alt-screen") {
    overrides.supports_alt_screen = enabled;
    return true;
  }
  if (terminal_option == "extended-keys") {
    overrides.supports_extended_keys = enabled;
    return true;
  }
  if (terminal_option == "osc52-clipboard") {
    overrides.supports_osc52_clipboard = enabled;
    return true;
  }
  if (terminal_option == "synchronized-output") {
    overrides.supports_synchronized_output = enabled;
    return true;
  }

  constexpr std::string_view kQuirkPrefix = "quirk-";
  if (terminal_option.rfind(kQuirkPrefix, 0) == 0) {
    const auto quirk = parse_terminal_quirk(terminal_option.substr(kQuirkPrefix.size()));
    if (!quirk) {
      errors.push_back({line_number, "terminal quirk is not recognized"});
      return true;
    }
    set_terminal_quirk_override(overrides, *quirk, enabled);
    return true;
  }

  errors.push_back({line_number, "unsupported option '" + std::string{option} + "'"});
  return true;
}

bool parse_resource_limit(
    Config& config,
    std::string_view option,
    std::string_view value,
    std::size_t line_number,
    std::vector<ConfigParseError>& errors) {
  const auto reject = [&](std::string_view message) {
    errors.push_back({line_number, std::string{message}});
  };

  auto parse_limit = [&](std::size_t minimum, std::size_t maximum, std::size_t& target) {
    if (!parse_bounded_size(value, minimum, maximum, target)) {
      errors.push_back(
          {line_number,
           std::string{option} + " must be between " + std::to_string(minimum) +
               " and " + std::to_string(maximum)});
    }
  };

  if (option == "max-sessions") {
    parse_limit(1, 1024, config.limits.max_sessions);
    return true;
  }
  if (option == "max-windows-per-session") {
    parse_limit(1, 1024, config.limits.max_windows_per_session);
    return true;
  }
  if (option == "max-panes-per-window") {
    parse_limit(1, 1024, config.limits.max_panes_per_window);
    return true;
  }
  if (option == "scrollback-max-lines" || option == "max-scrollback-lines") {
    parse_limit(0, 1'000'000, config.limits.max_pane_scrollback_lines);
    config.session.scrollback_max_lines = config.limits.max_pane_scrollback_lines;
    return true;
  }
  if (option == "paste-buffer-max-bytes" || option == "max-paste-buffer-bytes") {
    parse_limit(1, 64 * 1024 * 1024, config.limits.max_paste_buffer_bytes);
    return true;
  }
  if (option == "pane-raw-output-max-bytes" || option == "max-pane-raw-output-bytes") {
    parse_limit(64 * 1024, 256 * 1024 * 1024, config.limits.max_pane_raw_output_bytes);
    return true;
  }
  if (option == "client-output-queue-max-bytes" ||
      option == "max-client-output-queue-bytes") {
    parse_limit(64 * 1024, 256 * 1024 * 1024, config.limits.max_client_output_queue_bytes);
    config.client.output_queue_bytes = config.limits.max_client_output_queue_bytes;
    return true;
  }
  if (option == "client-output-queue-max-frames" ||
      option == "max-client-output-queue-frames") {
    parse_limit(1, 1024, config.limits.max_client_output_queue_frames);
    config.client.output_queue_frames = config.limits.max_client_output_queue_frames;
    return true;
  }
  if (option == "attach-render-frame-max-bytes" || option == "max-attach-render-frame-bytes") {
    parse_limit(64 * 1024, 256 * 1024 * 1024, config.limits.max_attach_render_frame_bytes);
    return true;
  }
  if (option == "ipc-frame-max-bytes" || option == "max-ipc-frame-bytes") {
    std::size_t parsed = 0;
    if (!parse_bounded_size(value, 1, kMaxIpcFramePayloadBytes, parsed)) {
      reject(
          "ipc-frame-max-bytes must be between 1 and the compiled IPC frame hard limit");
      return true;
    }
    config.limits.max_ipc_frame_payload_bytes = parsed;
    return true;
  }
  if (option == "log-max-bytes" || option == "max-log-file-bytes") {
    parse_limit(64 * 1024, 1024 * 1024 * 1024, config.limits.max_log_file_bytes);
    return true;
  }

  return false;
}

std::string environment_variable(const char* name) {
  return platform_services().info().environment_variable(name);
}

std::string first_command_token(std::string_view value) {
  std::size_t index = 0;
  while (index < value.size() && ascii_space(value[index])) {
    ++index;
  }
  if (index >= value.size()) {
    return {};
  }

  std::string token;
  if (value[index] == '"' || value[index] == '\'') {
    const char quote = value[index++];
    while (index < value.size() && value[index] != quote) {
      token.push_back(value[index++]);
    }
    return token;
  }

  while (index < value.size() && !ascii_space(value[index])) {
    token.push_back(value[index++]);
  }
  return token;
}

bool explicit_shell_path_exists(std::string_view value) {
  const auto executable = first_command_token(value);
  if (executable.find('\\') == std::string::npos && executable.find('/') == std::string::npos) {
    return true;
  }

  std::error_code error;
  return std::filesystem::exists(std::filesystem::path{executable}, error) && !error;
}

std::vector<std::string> tokenize_config_line(
    std::string_view line,
    std::size_t line_number,
    std::vector<ConfigParseError>& errors) {
  std::vector<std::string> tokens;
  std::size_t index = 0;

  while (index < line.size()) {
    while (index < line.size() && ascii_space(line[index])) {
      ++index;
    }

    if (index >= line.size() || line[index] == '#') {
      break;
    }

    std::string token;
    if (line[index] == '"' || line[index] == '\'') {
      const char quote = line[index];
      ++index;
      while (index < line.size() && line[index] != quote) {
        token.push_back(line[index]);
        ++index;
      }
      if (index >= line.size()) {
        errors.push_back({line_number, "unterminated quoted value"});
        return {};
      }
      ++index;

      if (index < line.size() && !ascii_space(line[index]) && line[index] != '#') {
        errors.push_back({line_number, "quoted value must be followed by whitespace"});
        return {};
      }
    } else {
      while (index < line.size() && !ascii_space(line[index])) {
        token.push_back(line[index]);
        ++index;
      }
    }

    tokens.push_back(std::move(token));
  }

  return tokens;
}

void parse_config_tokens(
    Config& config,
    const std::vector<std::string>& tokens,
    std::size_t line_number,
    std::vector<ConfigParseError>& errors) {
  if (tokens.empty()) {
    return;
  }

  if (tokens[0] == "bind-key") {
    if (tokens.size() != 3) {
      errors.push_back({line_number, "expected: bind-key <key> <action>"});
      return;
    }
    if (auto error = apply_key_binding_config(config, tokens[1], tokens[2])) {
      error->line = line_number;
      errors.push_back(*error);
    }
    return;
  }

  if (tokens[0] == "unbind-key") {
    if (tokens.size() != 2) {
      errors.push_back({line_number, "expected: unbind-key <key>"});
      return;
    }
    if (auto error = apply_key_unbinding_config(config, tokens[1])) {
      error->line = line_number;
      errors.push_back(*error);
    }
    return;
  }

  if (tokens.size() != 4 || tokens[0] != "set" || tokens[1] != "-g") {
    errors.push_back({line_number, "expected: set -g <option> <value>"});
    return;
  }

  const auto& option = tokens[2];
  const auto& value = tokens[3];

  if (auto error = apply_global_config_option(config, option, value)) {
    error->line = line_number;
    errors.push_back(*error);
  }
}

}  // namespace

Config::Config(const Config& other)
    : global(other.global),
      session(other.session),
      window(other.window),
      pane(other.pane),
      client(other.client),
      keys(other.keys),
      prefix(global.prefix),
      mouse_enabled(global.mouse_enabled),
      default_shell(global.default_shell),
      status_bar_enabled(global.status_bar_enabled),
      escape_time_ms(global.escape_time_ms),
      terminal_overrides(global.terminal_overrides),
      limits(global.limits) {}

Config& Config::operator=(const Config& other) {
  if (this == &other) {
    return *this;
  }
  global = other.global;
  session = other.session;
  window = other.window;
  pane = other.pane;
  client = other.client;
  keys = other.keys;
  return *this;
}

Config::Config(Config&& other) noexcept
    : global(std::move(other.global)),
      session(std::move(other.session)),
      window(std::move(other.window)),
      pane(std::move(other.pane)),
      client(std::move(other.client)),
      keys(std::move(other.keys)),
      prefix(global.prefix),
      mouse_enabled(global.mouse_enabled),
      default_shell(global.default_shell),
      status_bar_enabled(global.status_bar_enabled),
      escape_time_ms(global.escape_time_ms),
      terminal_overrides(global.terminal_overrides),
      limits(global.limits) {}

Config& Config::operator=(Config&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  global = std::move(other.global);
  session = std::move(other.session);
  window = std::move(other.window);
  pane = std::move(other.pane);
  client = std::move(other.client);
  keys = std::move(other.keys);
  return *this;
}

std::optional<ConfigParseError> apply_global_config_option(
    Config& config,
    std::string_view option,
    std::string_view value) {
  std::vector<ConfigParseError> errors;
  constexpr std::size_t kRuntimeLine = 1;

  if (parse_terminal_override(config, option, value, kRuntimeLine, errors)) {
    return errors.empty() ? std::nullopt : std::make_optional(errors.front());
  }

  if (parse_resource_limit(config, option, value, kRuntimeLine, errors)) {
    return errors.empty() ? std::nullopt : std::make_optional(errors.front());
  }

  if (option == "prefix") {
    if (!supported_control_prefix(value)) {
      return ConfigParseError{kRuntimeLine, "prefix must use C-<key> format"};
    }
    config.prefix = value;
    return std::nullopt;
  }

  if (option == "mouse") {
    bool enabled = false;
    if (!parse_on_off(value, enabled)) {
      return ConfigParseError{kRuntimeLine, "mouse must be on or off"};
    }
    config.mouse_enabled = enabled;
    return std::nullopt;
  }

  if (option == "default-shell") {
    if (value.empty()) {
      return ConfigParseError{kRuntimeLine, "default-shell cannot be empty"};
    }
    if (!explicit_shell_path_exists(value)) {
      return ConfigParseError{kRuntimeLine, "default-shell path does not exist"};
    }
    config.default_shell = value;
    return std::nullopt;
  }

  if (option == "status") {
    bool enabled = true;
    if (!parse_on_off(value, enabled)) {
      return ConfigParseError{kRuntimeLine, "status must be on or off"};
    }
    config.status_bar_enabled = enabled;
    return std::nullopt;
  }

  if (option == "escape-time-ms" || option == "input-escape-time-ms") {
    std::uint16_t escape_time = 0;
    if (!parse_uint16(value, escape_time) || escape_time > 5000) {
      return ConfigParseError{kRuntimeLine, "escape-time-ms must be between 0 and 5000"};
    }
    config.escape_time_ms = escape_time;
    return std::nullopt;
  }

  return ConfigParseError{kRuntimeLine, "unsupported option '" + std::string{option} + "'"};
}

std::optional<ConfigParseError> apply_key_binding_config(
    Config& config,
    std::string_view key_spec,
    std::string_view action_name) {
  const auto key = normalize_attach_key_spec(key_spec);
  if (!key) {
    return ConfigParseError{0, "unsupported key binding key '" + std::string{key_spec} + "'"};
  }

  const auto action = normalize_attach_key_action_name(action_name);
  if (!action) {
    return ConfigParseError{
        0,
        "unsupported key binding action '" + std::string{action_name} + "'"};
  }

  config.keys.bindings[*key] = *action;
  return std::nullopt;
}

std::optional<ConfigParseError> apply_key_unbinding_config(
    Config& config,
    std::string_view key_spec) {
  const auto key = normalize_attach_key_spec(key_spec);
  if (!key) {
    return ConfigParseError{0, "unsupported key binding key '" + std::string{key_spec} + "'"};
  }

  config.keys.bindings[*key] = "none";
  return std::nullopt;
}

std::filesystem::path default_config_path() {
  if (const auto profile = environment_variable("USERPROFILE"); !profile.empty()) {
    return std::filesystem::path{profile} / ".wmux.conf";
  }

  if (const auto home = environment_variable("HOME"); !home.empty()) {
    return std::filesystem::path{home} / ".wmux.conf";
  }

  return std::filesystem::path{".wmux.conf"};
}

ConfigParseResult parse_config_text(std::string_view text) {
  ConfigParseResult result;

  std::size_t line_number = 1;
  std::size_t line_start = 0;
  while (line_start <= text.size()) {
    std::size_t line_end = text.find('\n', line_start);
    if (line_end == std::string_view::npos) {
      line_end = text.size();
    }

    auto line = text.substr(line_start, line_end - line_start);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }

    if (line.size() > kMaxConfigLineBytes) {
      result.errors.push_back({line_number, "config line is too long"});
      if (line_end == text.size()) {
        break;
      }
      line_start = line_end + 1;
      ++line_number;
      continue;
    }

    auto tokens = tokenize_config_line(line, line_number, result.errors);
    if (result.errors.empty() || result.errors.back().line != line_number) {
      parse_config_tokens(result.config, tokens, line_number, result.errors);
    }

    if (line_end == text.size()) {
      break;
    }
    line_start = line_end + 1;
    ++line_number;
  }

  return result;
}

ConfigParseResult load_config_file(const std::filesystem::path& path) {
  ConfigParseResult result;
  std::ifstream file{path};
  if (!file) {
    return result;
  }

  std::ostringstream contents;
  contents << file.rdbuf();
  return parse_config_text(contents.str());
}

}  // namespace wmux
