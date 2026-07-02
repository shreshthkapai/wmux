#include "daemon_shell.hpp"

#include "daemon_state.hpp"
#include "wmux/logging.hpp"
#include "wmux/platform/services.hpp"

#include <mutex>
#include <string>
#include <string_view>


namespace wmux::daemon_internal {
std::string default_shell_command() {
  return platform_services().pty().resolve_shell_command({}).command_line;
}

std::string configured_shell_command(DaemonState& state) {
  return configured_shell_resolution(state).command_line;
}

PlatformShellResolution configured_shell_resolution(DaemonState& state) {
  std::lock_guard lock(state.mutex);
  return platform_services().pty().resolve_shell_command(state.config.values.default_shell);
}

ResourceLimits configured_resource_limits(DaemonState& state) {
  std::lock_guard lock(state.mutex);
  return state.config.values.limits;
}

PtyProcessResult start_shell(std::string_view command_line, short columns, short rows) {
  PlatformShellResolution shell;
  shell.command_line = std::string{command_line};
  shell.executable = std::string{command_line};
  shell.source = "explicit";
  shell.working_directory = platform_services().info().current_working_directory();
  return start_shell(shell, columns, rows);
}

PtyProcessResult start_shell(
    const PlatformShellResolution& shell,
    short columns,
    short rows) {
  return start_shell(shell, ResourceLimits{}, columns, rows);
}

PtyProcessResult start_shell(
    const PlatformShellResolution& shell,
    const ResourceLimits& limits,
    short columns,
    short rows) {
  PtySpawnOptions options;
  options.command_line = shell.command_line;
  options.executable = shell.executable;
  options.source = shell.source;
  options.working_directory = shell.working_directory;
  options.limits = limits;

  log_event(
      LogLevel::Info,
      "daemon.shell",
      "spawn_start",
      {{"shell_source", options.source},
       {"shell_executable", options.executable},
       {"cwd", options.working_directory},
       {"columns", std::to_string(columns)},
       {"rows", std::to_string(rows)}});
  auto result = platform_services().pty().spawn(options, columns, rows);
  if (!result.process) {
    log_event(
        LogLevel::Error,
        "daemon.shell",
        "spawn_failed",
        {{"shell_source", options.source},
         {"shell_executable", options.executable},
         {"cwd", options.working_directory},
         {"columns", std::to_string(columns)},
         {"rows", std::to_string(rows)},
         {"error", result.error}});
  } else {
    log_event(
        LogLevel::Info,
        "daemon.shell",
        "spawn_success",
        {{"shell_source", options.source},
         {"shell_executable", options.executable},
         {"cwd", options.working_directory},
         {"process_id", std::to_string(result.process->process_id())},
         {"columns", std::to_string(columns)},
         {"rows", std::to_string(rows)}});
  }
  return result;
}

PtyProcessResult start_default_shell(short columns, short rows) {
  return start_shell(platform_services().pty().resolve_shell_command({}), columns, rows);
}

PtyProcessResult start_configured_shell(DaemonState& state, short columns, short rows) {
  auto shell = configured_shell_resolution(state);
  auto limits = configured_resource_limits(state);
  return start_shell(shell, limits, columns, rows);
}

}  // namespace wmux::daemon_internal
