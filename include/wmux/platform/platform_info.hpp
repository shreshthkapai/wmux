#pragma once

#include <string>
#include <string_view>

namespace wmux {

enum class PlatformConsoleStream {
  Input,
  Output,
};

struct PlatformConsoleModeProbe {
  bool available{false};
  bool vt_enabled{false};
  unsigned long mode{0};
};

struct PlatformProcessResourceSnapshot {
  bool memory_available{false};
  unsigned long long working_set_bytes{0};
  unsigned long long private_bytes{0};
  bool handle_count_available{false};
  unsigned long handle_count{0};
};

struct PlatformShellResolution {
  std::string command_line;
  std::string executable;
  std::string source;
  std::string working_directory;
};

std::string platform_environment_variable(std::string_view name);
std::string platform_current_working_directory();
PlatformShellResolution platform_resolve_shell_command(std::string_view configured_shell);
bool platform_has_interactive_console();
bool platform_has_console_output();
std::string platform_os_version();
bool platform_pty_available();
PlatformConsoleModeProbe platform_probe_console_mode(PlatformConsoleStream stream);
std::string_view platform_clipboard_backend_name();
PlatformProcessResourceSnapshot platform_current_process_resources();

}  // namespace wmux
