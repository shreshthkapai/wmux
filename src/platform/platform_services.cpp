#include "wmux/platform/services.hpp"

#include "wmux/platform/ipc_transport.hpp"
#include "wmux/platform/terminal_control.hpp"

namespace wmux {
namespace {

class DefaultPtyBackend final : public PtyBackend {
 public:
  PlatformShellResolution resolve_shell_command(std::string_view configured_shell) const override {
    return platform_resolve_shell_command(configured_shell);
  }

  PtyProcessResult spawn(
      const PtySpawnOptions& options,
      short columns,
      short rows) const override {
    return PtyProcess::start(options, columns, rows);
  }

  bool available() const override {
    return platform_pty_available();
  }
};

class DefaultClipboardBackend final : public ClipboardBackend {
 public:
  std::string_view name() const override {
    return platform_clipboard_backend_name();
  }

  ClipboardWriteResult write_text(std::string_view text) const override {
    return write_clipboard_text(text);
  }
};

class DefaultTerminalModeBackend final : public TerminalModeBackend {
 public:
  bool has_interactive_console() const override {
    return platform_has_interactive_console();
  }

  bool has_console_output() const override {
    return platform_has_console_output();
  }

  PlatformConsoleModeProbe probe_console_mode(PlatformConsoleStream stream) const override {
    return platform_probe_console_mode(stream);
  }

  std::string_view reset_sequence() const override {
    return terminal_reset_sequence();
  }

  std::string_view attach_enter_sequence() const override {
    return terminal_attach_enter_sequence();
  }

  int reset_terminal() const override {
    return wmux::reset_terminal();
  }
};

class DefaultIpcTransportBackend final : public IpcTransportBackend {
 public:
  std::string command_endpoint_name() const override {
    return wmux::command_endpoint_name();
  }

  std::string attach_endpoint_name() const override {
    return wmux::attach_endpoint_name();
  }

  IpcResponse send_request(std::string_view request_json) const override {
    return wmux::send_ipc_request(request_json);
  }

  bool ensure_daemon_running(
      const std::filesystem::path& executable_path,
      std::string& error) const override {
    return wmux::ensure_daemon_running(executable_path, error);
  }
};

class DefaultPlatformInfoBackend final : public PlatformInfoBackend {
 public:
  std::string environment_variable(std::string_view name) const override {
    return platform_environment_variable(name);
  }

  std::string current_working_directory() const override {
    return platform_current_working_directory();
  }

  std::string os_version() const override {
    return platform_os_version();
  }

  PlatformProcessResourceSnapshot current_process_resources() const override {
    return platform_current_process_resources();
  }
};

class DefaultPlatformServices final : public PlatformServices {
 public:
  const PtyBackend& pty() const override {
    return pty_;
  }

  const ClipboardBackend& clipboard() const override {
    return clipboard_;
  }

  const TerminalModeBackend& terminal() const override {
    return terminal_;
  }

  const IpcTransportBackend& ipc() const override {
    return ipc_;
  }

  const PlatformInfoBackend& info() const override {
    return info_;
  }

 private:
  DefaultPtyBackend pty_;
  DefaultClipboardBackend clipboard_;
  DefaultTerminalModeBackend terminal_;
  DefaultIpcTransportBackend ipc_;
  DefaultPlatformInfoBackend info_;
};

}  // namespace

const PlatformServices& platform_services() {
  static const DefaultPlatformServices services;
  return services;
}

}  // namespace wmux
