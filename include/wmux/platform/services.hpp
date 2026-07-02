#pragma once

#include "wmux/ipc_protocol.hpp"
#include "wmux/platform/clipboard.hpp"
#include "wmux/platform/platform_info.hpp"
#include "wmux/platform/pty_process.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace wmux {

class PtyBackend {
 public:
  virtual ~PtyBackend() = default;

  virtual PlatformShellResolution resolve_shell_command(
      std::string_view configured_shell) const = 0;
  virtual PtyProcessResult spawn(
      const PtySpawnOptions& options,
      short columns,
      short rows) const = 0;
  virtual bool available() const = 0;
};

class ClipboardBackend {
 public:
  virtual ~ClipboardBackend() = default;

  virtual std::string_view name() const = 0;
  virtual ClipboardWriteResult write_text(std::string_view text) const = 0;
};

class TerminalModeBackend {
 public:
  virtual ~TerminalModeBackend() = default;

  virtual bool has_interactive_console() const = 0;
  virtual bool has_console_output() const = 0;
  virtual PlatformConsoleModeProbe probe_console_mode(PlatformConsoleStream stream) const = 0;
  virtual std::string_view reset_sequence() const = 0;
  virtual std::string_view attach_enter_sequence() const = 0;
  virtual int reset_terminal() const = 0;
};

class IpcTransportBackend {
 public:
  virtual ~IpcTransportBackend() = default;

  virtual std::string command_endpoint_name() const = 0;
  virtual std::string attach_endpoint_name() const = 0;
  virtual IpcResponse send_request(std::string_view request_json) const = 0;
  virtual bool ensure_daemon_running(
      const std::filesystem::path& executable_path,
      std::string& error) const = 0;
};

class PlatformInfoBackend {
 public:
  virtual ~PlatformInfoBackend() = default;

  virtual std::string environment_variable(std::string_view name) const = 0;
  virtual std::string current_working_directory() const = 0;
  virtual std::string os_version() const = 0;
  virtual PlatformProcessResourceSnapshot current_process_resources() const = 0;
};

class PlatformServices {
 public:
  virtual ~PlatformServices() = default;

  virtual const PtyBackend& pty() const = 0;
  virtual const ClipboardBackend& clipboard() const = 0;
  virtual const TerminalModeBackend& terminal() const = 0;
  virtual const IpcTransportBackend& ipc() const = 0;
  virtual const PlatformInfoBackend& info() const = 0;
};

const PlatformServices& platform_services();

}  // namespace wmux
