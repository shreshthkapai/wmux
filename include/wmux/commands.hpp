#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace wmux {

enum class CommandKind {
  DefaultSession,
  Daemon,
  Help,
  Version,
  NewSession,
  ListSessions,
  AttachSession,
  RenameSession,
  KillSession,
  NewWindow,
  ListWindows,
  RenameWindow,
  ServerStatus,
  ServerStop,
  Unknown,
};

struct CommandLine {
  CommandKind kind{CommandKind::Help};
  std::string error;
  std::string session_name;
  std::string target_name;
  std::string new_name;
  std::string window_name;
  bool force{false};
};

CommandLine parse_command_line(const std::vector<std::string_view>& args);
std::string render_help(std::string_view executable_name);
std::string render_version();
std::string render_placeholder_response(const CommandLine& command);

}  // namespace wmux
