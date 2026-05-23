#include "daemon_shell.hpp"

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

PtyProcessResult start_default_shell(short columns, short rows) {
#ifdef _WIN32
  return PtyProcess::start(default_shell_command(), columns, rows);
#else
  (void)columns;
  (void)rows;
  return {nullptr, "wmux: shell processes require Windows ConPTY\n"};
#endif
}

}  // namespace wmux::daemon_internal
