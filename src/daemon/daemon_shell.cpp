#include "daemon_shell.hpp"

#include "daemon_state.hpp"
#include "wmux/logging.hpp"

#include <mutex>
#include <string>
#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace wmux::daemon_internal {

std::string default_shell_command() {
#ifdef _WIN32
  constexpr std::string_view kFallbackDefaultShell = "powershell.exe -NoLogo -NoProfile";

  char configured_shell[32768]{};
  const DWORD size = GetEnvironmentVariableA(
      "WMUX_DEFAULT_SHELL",
      configured_shell,
      static_cast<DWORD>(sizeof(configured_shell)));
  if (size > 0 && size < sizeof(configured_shell)) {
    return configured_shell;
  }

  return std::string{kFallbackDefaultShell};
#else
  return {};
#endif
}

std::string configured_shell_command(DaemonState& state) {
  std::lock_guard lock(state.mutex);
  return state.config.values.default_shell.empty()
             ? default_shell_command()
             : state.config.values.default_shell;
}

PtyProcessResult start_shell(std::string_view command_line, short columns, short rows) {
#ifdef _WIN32
  auto result = PtyProcess::start(command_line, columns, rows);
  if (!result.process) {
    log_event(
        LogLevel::Error,
        "daemon.shell",
        "spawn_failed",
        {{"columns", std::to_string(columns)},
         {"rows", std::to_string(rows)},
         {"error", result.error}});
  }
  return result;
#else
  (void)command_line;
  (void)columns;
  (void)rows;
  return {nullptr, "wmux: shell processes require Windows ConPTY\n"};
#endif
}

PtyProcessResult start_default_shell(short columns, short rows) {
  return start_shell(default_shell_command(), columns, rows);
}

PtyProcessResult start_configured_shell(DaemonState& state, short columns, short rows) {
  return start_shell(configured_shell_command(state), columns, rows);
}

}  // namespace wmux::daemon_internal
