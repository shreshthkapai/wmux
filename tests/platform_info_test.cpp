#include "wmux/platform/services.hpp"

#include <cassert>

namespace {

void configured_shell_wins() {
  const auto shell =
      wmux::platform_services().pty().resolve_shell_command("custom-shell.exe -arg");

  assert(shell.command_line == "custom-shell.exe -arg");
  assert(shell.executable == "custom-shell.exe");
  assert(shell.source == "config");
}

void auto_shell_resolution_returns_command() {
  const auto shell = wmux::platform_services().pty().resolve_shell_command("");

  assert(!shell.command_line.empty());
  assert(!shell.executable.empty());
  assert(!shell.source.empty());
}

}  // namespace

void run_platform_info_tests() {
  configured_shell_wins();
  auto_shell_resolution_returns_command();
}
