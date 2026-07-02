#include "wmux/doctor.hpp"

#include "wmux/config.hpp"
#include "wmux/commands.hpp"
#include "wmux/ipc_protocol.hpp"
#include "wmux/logging.hpp"
#include "wmux/platform/services.hpp"
#include "wmux/terminal_capabilities.hpp"
#include "wmux/version.hpp"

#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace wmux {
namespace {

std::string yes_no(bool value) {
  return value ? "yes" : "no";
}

std::string on_off(bool value) {
  return value ? "on" : "off";
}

std::string json_escape(std::string_view value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const unsigned char ch : value) {
    switch (ch) {
      case '\\':
        escaped += "\\\\";
        break;
      case '"':
        escaped += "\\\"";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case '\t':
        escaped += "\\t";
        break;
      default:
        if (ch < 0x20) {
          constexpr char kHex[] = "0123456789abcdef";
          escaped += "\\u00";
          escaped.push_back(kHex[(ch >> 4) & 0x0F]);
          escaped.push_back(kHex[ch & 0x0F]);
        } else {
          escaped.push_back(static_cast<char>(ch));
        }
        break;
    }
  }
  return escaped;
}

void append_json_string(std::ostringstream& out, std::string_view key, std::string_view value) {
  out << "\"" << key << "\":\"" << json_escape(value) << "\"";
}

void append_json_bool(std::ostringstream& out, std::string_view key, bool value) {
  out << "\"" << key << "\":" << (value ? "true" : "false");
}

void append_json_size(std::ostringstream& out, std::string_view key, std::size_t value) {
  out << "\"" << key << "\":" << value;
}

std::string environment_value(const char* name) {
  return platform_services().info().environment_variable(name);
}

struct LoadedDoctorConfig {
  std::filesystem::path path;
  ConfigParseResult parsed;
};

LoadedDoctorConfig load_doctor_config() {
  auto path = default_config_path();
  return {path, load_config_file(path)};
}

std::string mode_status(const PlatformConsoleModeProbe& probe) {
  if (!probe.available) {
    return "unavailable";
  }
  return on_off(probe.vt_enabled);
}

IpcResponse query_daemon_status() {
  const auto& ipc = platform_services().ipc();
  const auto ping = ipc.send_request(make_ping_request_json());
  if (!ping.ok) {
    return ping;
  }

  CommandLine status_command;
  status_command.kind = CommandKind::ServerStatus;
  return ipc.send_request(make_command_request_json(status_command));
}

void append_override_entry(
    std::vector<std::string>& entries,
    std::string_view name,
    std::optional<bool> value) {
  if (value) {
    entries.push_back(std::string{name} + "=" + on_off(*value));
  }
}

std::vector<std::string> terminal_override_entries(
    const TerminalCapabilityOverrides& overrides) {
  std::vector<std::string> entries;
  if (overrides.host) {
    entries.push_back("terminal-host=" + terminal_host_name(*overrides.host));
  }

  append_override_entry(entries, "terminal-truecolor", overrides.supports_truecolor);
  append_override_entry(entries, "terminal-256-color", overrides.supports_256_color);
  append_override_entry(entries, "terminal-mouse", overrides.supports_sgr_mouse);
  append_override_entry(entries, "terminal-mouse-drag", overrides.supports_mouse_drag);
  append_override_entry(entries, "terminal-mouse-wheel", overrides.supports_mouse_wheel);
  append_override_entry(
      entries,
      "terminal-bracketed-paste",
      overrides.supports_bracketed_paste);
  append_override_entry(entries, "terminal-focus-events", overrides.supports_focus_events);
  append_override_entry(entries, "terminal-cursor-style", overrides.supports_cursor_style);
  append_override_entry(entries, "terminal-alt-screen", overrides.supports_alt_screen);
  append_override_entry(entries, "terminal-extended-keys", overrides.supports_extended_keys);
  append_override_entry(
      entries,
      "terminal-osc52-clipboard",
      overrides.supports_osc52_clipboard);
  append_override_entry(
      entries,
      "terminal-synchronized-output",
      overrides.supports_synchronized_output);
  append_override_entry(
      entries,
      "terminal-quirk-broken-alt-key-sequences",
      overrides.quirk_broken_alt_key_sequences);
  append_override_entry(
      entries,
      "terminal-quirk-ctrl-break-special-handling",
      overrides.quirk_ctrl_break_special_handling);
  append_override_entry(
      entries,
      "terminal-quirk-mouse-wheel-encoding-differs",
      overrides.quirk_mouse_wheel_encoding_differs);
  append_override_entry(
      entries,
      "terminal-quirk-no-osc52-clipboard",
      overrides.quirk_no_osc52_clipboard);
  append_override_entry(
      entries,
      "terminal-quirk-vscode-key-translation",
      overrides.quirk_vscode_key_translation);
  append_override_entry(
      entries,
      "terminal-quirk-legacy-conhost-mode",
      overrides.quirk_legacy_conhost_mode);
  append_override_entry(
      entries,
      "terminal-quirk-cursor-style-unsupported",
      overrides.quirk_cursor_style_unsupported);
  append_override_entry(
      entries,
      "terminal-quirk-unknown-escape-sequences",
      overrides.quirk_unknown_escape_sequences);
  return entries;
}

void append_capability_text(
    std::ostringstream& out,
    const TerminalCapabilities& detected,
    const TerminalCapabilities& effective,
    const TerminalCapabilityOverrides& overrides) {
  out << "host terminal guess: " << terminal_host_name(detected.host) << "\n";
  out << "host terminal effective: " << terminal_host_name(effective.host) << "\n";
  out << "terminal capability overrides:";
  const auto entries = terminal_override_entries(overrides);
  if (entries.empty()) {
    out << " none";
  } else {
    for (const auto& entry : entries) {
      out << " " << entry;
    }
  }
  out << "\n";
  out << "truecolor support effective: " << yes_no(effective.supports_truecolor) << "\n";
  out << "256-color support effective: " << yes_no(effective.supports_256_color) << "\n";
  out << "mouse support effective: " << yes_no(effective.supports_sgr_mouse) << "\n";
  out << "mouse drag support effective: " << yes_no(effective.supports_mouse_drag) << "\n";
  out << "mouse wheel support effective: " << yes_no(effective.supports_mouse_wheel) << "\n";
  out << "bracketed paste support effective: "
      << yes_no(effective.supports_bracketed_paste) << "\n";
  out << "focus events support effective: " << yes_no(effective.supports_focus_events)
      << "\n";
  out << "cursor style support effective: " << yes_no(effective.supports_cursor_style)
      << "\n";
  out << "alternate screen support effective: " << yes_no(effective.supports_alt_screen)
      << "\n";
  out << "extended keys support effective: " << yes_no(effective.supports_extended_keys)
      << "\n";
  out << "OSC52 clipboard support effective: "
      << yes_no(effective.supports_osc52_clipboard) << "\n";
  out << "synchronized output support effective: "
      << yes_no(effective.supports_synchronized_output) << "\n";
  out << "terminal quirks:";
  if (effective.quirks.empty()) {
    out << " none";
  } else {
    for (const auto quirk : effective.quirks) {
      out << " " << terminal_quirk_name(quirk);
    }
  }
  out << "\n";
}

std::string render_text_report() {
  const auto loaded_config = load_doctor_config();
  const auto detected_capabilities = detect_terminal_capabilities();
  const auto capabilities = apply_terminal_capability_overrides(
      detected_capabilities,
      loaded_config.parsed.config.terminal_overrides);
  const auto& platform = platform_services();
  const auto input_mode = platform.terminal().probe_console_mode(PlatformConsoleStream::Input);
  const auto output_mode = platform.terminal().probe_console_mode(PlatformConsoleStream::Output);
  const auto daemon = query_daemon_status();
  const auto comspec = environment_value("ComSpec");
  const auto shell_guess =
      platform.pty().resolve_shell_command(loaded_config.parsed.config.default_shell);

  std::ostringstream out;
  out << "wmux doctor\n";
  out << "version: " << version_string() << "\n";
  out << "windows version/build: " << platform.info().os_version() << "\n";
  append_capability_text(
      out,
      detected_capabilities,
      capabilities,
      loaded_config.parsed.config.terminal_overrides);
  out << "shell guess: " << shell_guess.command_line << "\n";
  out << "shell source: " << shell_guess.source << "\n";
  out << "shell executable: " << shell_guess.executable << "\n";
  out << "shell cwd: " << shell_guess.working_directory << "\n";
  out << "limit max sessions: " << loaded_config.parsed.config.limits.max_sessions << "\n";
  out << "limit max windows per session: "
      << loaded_config.parsed.config.limits.max_windows_per_session << "\n";
  out << "limit max panes per window: "
      << loaded_config.parsed.config.limits.max_panes_per_window << "\n";
  out << "limit scrollback lines: "
      << loaded_config.parsed.config.limits.max_pane_scrollback_lines << "\n";
  out << "limit paste bytes: "
      << loaded_config.parsed.config.limits.max_paste_buffer_bytes << "\n";
  out << "limit client output queue bytes: "
      << loaded_config.parsed.config.limits.max_client_output_queue_bytes << "\n";
  if (!comspec.empty()) {
    out << "ComSpec: " << comspec << "\n";
  }
  out << "ConPTY available: " << yes_no(platform.pty().available()) << "\n";
  out << "VT input mode: " << mode_status(input_mode) << "\n";
  out << "VT output mode: " << mode_status(output_mode) << "\n";
  out << "clipboard backend: " << platform.clipboard().name() << "\n";
  out << "config path: " << loaded_config.path.string() << "\n";
  if (!loaded_config.parsed.ok()) {
    out << "config errors:\n";
    for (const auto& error : loaded_config.parsed.errors) {
      out << "  line " << error.line << ": " << error.message << "\n";
    }
  }
  out << "log directory: " << log_directory().string() << "\n";
  out << "client log: " << log_file_path(LogRole::Client).string() << "\n";
  out << "daemon log: " << log_file_path(LogRole::Daemon).string() << "\n";
  out << "IPC command path: " << platform.ipc().command_endpoint_name() << "\n";
  out << "IPC attach path: " << platform.ipc().attach_endpoint_name() << "\n";
  out << "daemon status: " << (daemon.ok ? "running" : "not running") << "\n";
  if (!daemon.message.empty()) {
    out << "daemon diagnostics:\n";
    std::istringstream status{daemon.message};
    std::string line;
    while (std::getline(status, line)) {
      out << "  " << line << "\n";
    }
  }
  return out.str();
}

void append_capabilities_json(
    std::ostringstream& out,
    std::string_view key,
    const TerminalCapabilities& capabilities) {
  out << "\"" << key << "\":{";
  append_json_string(out, "host", terminal_host_name(capabilities.host));
  out << ",";
  append_json_bool(out, "supports_truecolor", capabilities.supports_truecolor);
  out << ",";
  append_json_bool(out, "supports_256_color", capabilities.supports_256_color);
  out << ",";
  append_json_bool(out, "supports_sgr_mouse", capabilities.supports_sgr_mouse);
  out << ",";
  append_json_bool(out, "supports_mouse_drag", capabilities.supports_mouse_drag);
  out << ",";
  append_json_bool(out, "supports_mouse_wheel", capabilities.supports_mouse_wheel);
  out << ",";
  append_json_bool(out, "supports_bracketed_paste", capabilities.supports_bracketed_paste);
  out << ",";
  append_json_bool(out, "supports_focus_events", capabilities.supports_focus_events);
  out << ",";
  append_json_bool(out, "supports_cursor_style", capabilities.supports_cursor_style);
  out << ",";
  append_json_bool(out, "supports_alt_screen", capabilities.supports_alt_screen);
  out << ",";
  append_json_bool(out, "supports_extended_keys", capabilities.supports_extended_keys);
  out << ",";
  append_json_bool(out, "supports_osc52_clipboard", capabilities.supports_osc52_clipboard);
  out << ",";
  append_json_bool(
      out, "supports_synchronized_output", capabilities.supports_synchronized_output);
  out << ",\"quirks\":[";
  for (std::size_t i = 0; i < capabilities.quirks.size(); ++i) {
    if (i != 0) {
      out << ",";
    }
    out << "\"" << json_escape(terminal_quirk_name(capabilities.quirks[i])) << "\"";
  }
  out << "]}";
}

void append_terminal_overrides_json(
    std::ostringstream& out,
    const TerminalCapabilityOverrides& overrides) {
  out << "\"terminal_capability_overrides\":[";
  const auto entries = terminal_override_entries(overrides);
  for (std::size_t i = 0; i < entries.size(); ++i) {
    if (i != 0) {
      out << ",";
    }
    out << "\"" << json_escape(entries[i]) << "\"";
  }
  out << "]";
}

std::string render_json_report() {
  const auto loaded_config = load_doctor_config();
  const auto detected_capabilities = detect_terminal_capabilities();
  const auto capabilities = apply_terminal_capability_overrides(
      detected_capabilities,
      loaded_config.parsed.config.terminal_overrides);
  const auto& platform = platform_services();
  const auto input_mode = platform.terminal().probe_console_mode(PlatformConsoleStream::Input);
  const auto output_mode = platform.terminal().probe_console_mode(PlatformConsoleStream::Output);
  const auto daemon = query_daemon_status();
  const auto shell_guess =
      platform.pty().resolve_shell_command(loaded_config.parsed.config.default_shell);

  std::ostringstream out;
  out << "{";
  append_json_string(out, "version", version_string());
  out << ",";
  append_json_string(out, "windows_version", platform.info().os_version());
  out << ",";
  append_capabilities_json(out, "terminal_capabilities_detected", detected_capabilities);
  out << ",";
  append_capabilities_json(out, "terminal_capabilities", capabilities);
  out << ",";
  append_terminal_overrides_json(out, loaded_config.parsed.config.terminal_overrides);
  out << ",";
  append_json_string(out, "shell_guess", shell_guess.command_line);
  out << ",";
  append_json_string(out, "shell_source", shell_guess.source);
  out << ",";
  append_json_string(out, "shell_executable", shell_guess.executable);
  out << ",";
  append_json_string(out, "shell_cwd", shell_guess.working_directory);
  out << ",\"limits\":{";
  append_json_size(out, "max_sessions", loaded_config.parsed.config.limits.max_sessions);
  out << ",";
  append_json_size(
      out,
      "max_windows_per_session",
      loaded_config.parsed.config.limits.max_windows_per_session);
  out << ",";
  append_json_size(
      out,
      "max_panes_per_window",
      loaded_config.parsed.config.limits.max_panes_per_window);
  out << ",";
  append_json_size(
      out,
      "scrollback_lines",
      loaded_config.parsed.config.limits.max_pane_scrollback_lines);
  out << ",";
  append_json_size(
      out,
      "paste_buffer_bytes",
      loaded_config.parsed.config.limits.max_paste_buffer_bytes);
  out << ",";
  append_json_size(
      out,
      "client_output_queue_bytes",
      loaded_config.parsed.config.limits.max_client_output_queue_bytes);
  out << "}";
  out << ",";
  append_json_bool(out, "conpty_available", platform.pty().available());
  out << ",\"vt_input_mode\":{";
  append_json_bool(out, "available", input_mode.available);
  out << ",";
  append_json_bool(out, "enabled", input_mode.vt_enabled);
  out << ",\"mode\":" << input_mode.mode << "}";
  out << ",\"vt_output_mode\":{";
  append_json_bool(out, "available", output_mode.available);
  out << ",";
  append_json_bool(out, "enabled", output_mode.vt_enabled);
  out << ",\"mode\":" << output_mode.mode << "}";
  out << ",";
  append_json_string(out, "clipboard_backend", platform.clipboard().name());
  out << ",\"paths\":{";
  append_json_string(out, "config", loaded_config.path.string());
  out << ",";
  append_json_string(out, "log_directory", log_directory().string());
  out << ",";
  append_json_string(out, "client_log", log_file_path(LogRole::Client).string());
  out << ",";
  append_json_string(out, "daemon_log", log_file_path(LogRole::Daemon).string());
  out << ",";
  append_json_string(out, "ipc_command", platform.ipc().command_endpoint_name());
  out << ",";
  append_json_string(out, "ipc_attach", platform.ipc().attach_endpoint_name());
  out << "},\"daemon\":{";
  append_json_bool(out, "running", daemon.ok);
  out << ",";
  append_json_string(out, "status", daemon.message);
  out << "}}\n";
  return out.str();
}

}  // namespace

std::string render_doctor_report(bool json) {
  return json ? render_json_report() : render_text_report();
}

}  // namespace wmux
