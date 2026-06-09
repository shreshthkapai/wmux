#include "wmux/client.hpp"
#include "wmux/commands.hpp"
#include "wmux/daemon.hpp"
#include "wmux/ipc_protocol.hpp"
#include "wmux/ipc_transport.hpp"
#include "wmux/logging.hpp"
#include "wmux/terminal_control.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

bool recoverable_ipc_failure(std::string_view message) {
  return message.find("wmux: daemon is not running") != std::string_view::npos ||
         message.find("wmux: failed to write daemon request") != std::string_view::npos ||
         message.find("wmux: daemon returned an invalid response") != std::string_view::npos ||
         message.find("wmux: daemon did not become ready") != std::string_view::npos;
}

bool running_in_interactive_console() {
#ifdef _WIN32
  const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
  const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
  if (input == nullptr || input == INVALID_HANDLE_VALUE || output == nullptr ||
      output == INVALID_HANDLE_VALUE) {
    return false;
  }

  DWORD mode = 0;
  return GetConsoleMode(input, &mode) != 0 && GetConsoleMode(output, &mode) != 0;
#else
  return false;
#endif
}

}  // namespace

int main(int argc, char** argv) {
  wmux::initialize_logging(wmux::LogRole::Client);
  wmux::log_event(wmux::LogLevel::Debug, "client", "start");

  std::vector<std::string_view> args;
  args.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
  for (int i = 1; i < argc; ++i) {
    args.emplace_back(argv[i]);
  }

  const auto command = wmux::parse_command_line(args);
  const std::string executable_name = argc > 0 ? argv[0] : "wmux";

  switch (command.kind) {
    case wmux::CommandKind::Daemon:
      wmux::shutdown_logging();
      wmux::initialize_logging(wmux::LogRole::Daemon);
      return wmux::run_daemon();

    case wmux::CommandKind::DefaultSession:
    case wmux::CommandKind::NewSession:
    case wmux::CommandKind::ListSessions:
    case wmux::CommandKind::AttachSession:
    case wmux::CommandKind::RenameSession:
    case wmux::CommandKind::KillSession:
    case wmux::CommandKind::NewWindow:
    case wmux::CommandKind::ListWindows:
    case wmux::CommandKind::RenameWindow:
    case wmux::CommandKind::SplitWindow:
    case wmux::CommandKind::SetOption:
    case wmux::CommandKind::ServerStatus:
    case wmux::CommandKind::ServerStop: {
      std::string error;
      if (!wmux::ensure_daemon_running(std::filesystem::path{executable_name}, error)) {
        std::cerr << error;
        return 1;
      }

      if (command.kind == wmux::CommandKind::AttachSession) {
        return wmux::run_attach_client(command);
      }

      const auto request_json = wmux::make_command_request_json(command);
      auto response = wmux::send_ipc_request(request_json);
      if (!response.ok && recoverable_ipc_failure(response.message)) {
        wmux::log_event(
            wmux::LogLevel::Warn,
            "client.ipc",
            "recoverable_failure",
            {{"message", response.message}});
        std::string retry_error;
        if (wmux::ensure_daemon_running(std::filesystem::path{executable_name}, retry_error)) {
          response = wmux::send_ipc_request(request_json);
        }
      }
      if (response.ok) {
        if (command.kind == wmux::CommandKind::NewSession && running_in_interactive_console()) {
          return wmux::run_attach_client(command);
        }
        std::cout << response.message;
        return 0;
      }

      std::cerr << response.message;
      return 1;
    }

    case wmux::CommandKind::Help:
      std::cout << wmux::render_help(executable_name);
      return command.error.empty() ? 0 : 2;

    case wmux::CommandKind::Version:
      std::cout << wmux::render_version();
      return 0;

    case wmux::CommandKind::ResetTerminal:
      return wmux::reset_terminal();

    case wmux::CommandKind::Unknown:
      std::cerr << "wmux: " << command.error << "\n\n";
      std::cerr << wmux::render_help(executable_name);
      return 2;
  }

  return 2;
}
