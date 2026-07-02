#include "wmux/commands.hpp"

#include "wmux/version.hpp"

#include <CLI/CLI.hpp>

#include <sstream>
#include <utility>

namespace wmux {
namespace {

[[maybe_unused]] void validate_cli11_api_surface() {
  CLI::App app{"Native Windows terminal multiplexer", "wmux"};
  app.set_version_flag("--version", std::string{version_string()});
}

bool is_empty(std::string_view value) {
  return value.empty();
}

CommandLine invalid(std::string message) {
  CommandLine command;
  command.kind = CommandKind::Unknown;
  command.error = std::move(message);
  return command;
}

std::string quoted(std::string_view value) {
  std::ostringstream out;
  out << "'" << value << "'";
  return out.str();
}

CommandLine parse_no_args_command(
    const std::vector<std::string_view>& args,
    CommandKind kind,
    std::string_view command_name) {
  if (args.size() == 1) {
    CommandLine command;
    command.kind = kind;
    return command;
  }

  std::ostringstream message;
  message << command_name << " does not accept arguments";
  if (args.size() > 1) {
    message << ": " << quoted(args[1]);
  }
  return invalid(message.str());
}

CommandLine parse_named_session_command(
    const std::vector<std::string_view>& args,
    CommandKind kind,
    std::string_view command_name,
    std::string_view flag_name) {
  if (args.size() != 3 || args[1] != flag_name || is_empty(args[2])) {
    std::ostringstream message;
    message << command_name << " requires " << flag_name << " <name>";
    return invalid(message.str());
  }

  CommandLine command;
  command.kind = kind;
  command.session_name = std::string{args[2]};
  return command;
}

CommandLine parse_rename_session(const std::vector<std::string_view>& args) {
  if (args.size() != 4 || args[1] != "-t" || is_empty(args[2]) || is_empty(args[3])) {
    return invalid("rename-session requires -t <old> <new>");
  }

  CommandLine command;
  command.kind = CommandKind::RenameSession;
  command.target_name = std::string{args[2]};
  command.new_name = std::string{args[3]};
  return command;
}

CommandLine parse_new_window(const std::vector<std::string_view>& args) {
  CommandLine command;
  command.kind = CommandKind::NewWindow;

  for (std::size_t i = 1; i < args.size(); ++i) {
    if (args[i] == "-t") {
      if (i + 1 >= args.size() || is_empty(args[i + 1])) {
        return invalid("new-window requires -t <session>");
      }
      command.session_name = std::string{args[++i]};
      continue;
    }

    if (args[i] == "-n") {
      if (i + 1 >= args.size() || is_empty(args[i + 1])) {
        return invalid("new-window requires -n <name>");
      }
      command.window_name = std::string{args[++i]};
      continue;
    }

    std::ostringstream message;
    message << "new-window does not accept argument " << quoted(args[i]);
    return invalid(message.str());
  }

  if (command.window_name.empty()) {
    return invalid("new-window requires -n <name>");
  }

  return command;
}

CommandLine parse_list_windows(const std::vector<std::string_view>& args) {
  CommandLine command;
  command.kind = CommandKind::ListWindows;

  if (args.size() == 1) {
    return command;
  }

  if (args.size() == 3 && args[1] == "-t" && !is_empty(args[2])) {
    command.session_name = std::string{args[2]};
    return command;
  }

  return invalid("list-windows accepts optional -t <session>");
}

CommandLine parse_rename_window(const std::vector<std::string_view>& args) {
  CommandLine command;
  command.kind = CommandKind::RenameWindow;

  if (args.size() == 2 && !is_empty(args[1])) {
    command.window_name = std::string{args[1]};
    return command;
  }

  if (args.size() == 4 && args[1] == "-t" && !is_empty(args[2]) && !is_empty(args[3])) {
    command.session_name = std::string{args[2]};
    command.window_name = std::string{args[3]};
    return command;
  }

  return invalid("rename-window requires [-t <session>] <new>");
}

CommandLine parse_split_window(const std::vector<std::string_view>& args) {
  CommandLine command;
  command.kind = CommandKind::SplitWindow;

  for (std::size_t i = 1; i < args.size(); ++i) {
    if (args[i] == "-t") {
      if (i + 1 >= args.size() || is_empty(args[i + 1])) {
        return invalid("split-window requires -t <session>");
      }
      command.session_name = std::string{args[++i]};
      continue;
    }

    if (args[i] == "-h" || args[i] == "-v") {
      if (!command.split_direction.empty()) {
        return invalid("split-window accepts only one split direction");
      }
      command.split_direction = args[i] == "-h" ? "horizontal" : "vertical";
      continue;
    }

    std::ostringstream message;
    message << "split-window does not accept argument " << quoted(args[i]);
    return invalid(message.str());
  }

  if (command.split_direction.empty()) {
    return invalid("split-window requires one of -h or -v");
  }

  return command;
}

CommandLine parse_set_option(const std::vector<std::string_view>& args) {
  if (args.size() != 4 || args[1] != "-g" || is_empty(args[2]) || is_empty(args[3])) {
    return invalid("set requires -g <option> <value>");
  }

  CommandLine command;
  command.kind = CommandKind::SetOption;
  command.option_name = std::string{args[2]};
  command.option_value = std::string{args[3]};
  return command;
}

std::string join_args(
    const std::vector<std::string_view>& args,
    std::size_t first,
    std::string_view separator = " ") {
  std::ostringstream out;
  for (std::size_t index = first; index < args.size(); ++index) {
    if (index > first) {
      out << separator;
    }
    out << args[index];
  }
  return out.str();
}

CommandLine parse_bind_key(const std::vector<std::string_view>& args) {
  if (args.size() < 3 || is_empty(args[1])) {
    return invalid("bind-key requires <key> <action>");
  }

  CommandLine command;
  command.kind = CommandKind::BindKey;
  command.key_name = std::string{args[1]};
  command.key_action = join_args(args, 2);
  if (command.key_action.empty()) {
    return invalid("bind-key requires <key> <action>");
  }
  return command;
}

CommandLine parse_unbind_key(const std::vector<std::string_view>& args) {
  if (args.size() != 2 || is_empty(args[1])) {
    return invalid("unbind-key requires <key>");
  }

  CommandLine command;
  command.kind = CommandKind::UnbindKey;
  command.key_name = std::string{args[1]};
  return command;
}

CommandLine parse_server_command(const std::vector<std::string_view>& args) {
  if (args.size() < 2) {
    return invalid("server requires one subcommand: status or stop");
  }

  if (args[1] == "status") {
    if (args.size() != 2) {
      return invalid("server status does not accept arguments");
    }
    CommandLine command;
    command.kind = CommandKind::ServerStatus;
    return command;
  }

  if (args[1] == "stop") {
    if (args.size() > 3 || (args.size() == 3 && args[2] != "--force")) {
      return invalid("server stop accepts only optional --force");
    }
    CommandLine command;
    command.kind = CommandKind::ServerStop;
    command.force = args.size() == 3;
    return command;
  }

  std::ostringstream message;
  message << "unknown server subcommand " << quoted(args[1]);
  return invalid(message.str());
}

CommandLine parse_doctor_command(const std::vector<std::string_view>& args) {
  CommandLine command;
  command.kind = CommandKind::Doctor;

  if (args.size() == 1) {
    return command;
  }

  if (args.size() == 2 && args[1] == "--json") {
    command.json = true;
    return command;
  }

  return invalid("doctor accepts only optional --json");
}

}  // namespace

CommandLine parse_command_line(const std::vector<std::string_view>& args) {
  if (args.empty()) {
    CommandLine command;
    command.kind = CommandKind::DefaultSession;
    return command;
  }

  if (args.size() == 1 && (args[0] == "--help" || args[0] == "-h" || args[0] == "help")) {
    CommandLine command;
    command.kind = CommandKind::Help;
    return command;
  }

  if (args.size() == 1 && (args[0] == "version" || args[0] == "--version")) {
    CommandLine command;
    command.kind = CommandKind::Version;
    return command;
  }

  if (args.size() == 1 && args[0] == "--daemon") {
    CommandLine command;
    command.kind = CommandKind::Daemon;
    return command;
  }

  if (args[0] == "new") {
    return parse_named_session_command(args, CommandKind::NewSession, "new", "-s");
  }

  if (args[0] == "ls") {
    return parse_no_args_command(args, CommandKind::ListSessions, "ls");
  }

  if (args[0] == "attach") {
    return parse_named_session_command(args, CommandKind::AttachSession, "attach", "-t");
  }

  if (args[0] == "rename-session") {
    return parse_rename_session(args);
  }

  if (args[0] == "kill-session") {
    return parse_named_session_command(args, CommandKind::KillSession, "kill-session", "-t");
  }

  if (args[0] == "new-window") {
    return parse_new_window(args);
  }

  if (args[0] == "list-windows") {
    return parse_list_windows(args);
  }

  if (args[0] == "rename-window") {
    return parse_rename_window(args);
  }

  if (args[0] == "split-window") {
    return parse_split_window(args);
  }

  if (args[0] == "set") {
    return parse_set_option(args);
  }

  if (args[0] == "bind-key") {
    return parse_bind_key(args);
  }

  if (args[0] == "unbind-key") {
    return parse_unbind_key(args);
  }

  if (args[0] == "server") {
    return parse_server_command(args);
  }

  if (args[0] == "dump-state") {
    return parse_no_args_command(args, CommandKind::DumpState, "dump-state");
  }

  if (args[0] == "dump-layout") {
    return parse_no_args_command(args, CommandKind::DumpLayout, "dump-layout");
  }

  if (args[0] == "dump-events") {
    return parse_no_args_command(args, CommandKind::DumpEvents, "dump-events");
  }

  if (args[0] == "reset-terminal") {
    return parse_no_args_command(args, CommandKind::ResetTerminal, "reset-terminal");
  }

  if (args[0] == "doctor") {
    return parse_doctor_command(args);
  }

  if (args[0] == "debug-keys") {
    return parse_no_args_command(args, CommandKind::DebugKeys, "debug-keys");
  }

  std::ostringstream message;
  message << "unknown command";
  if (!args.empty()) {
    message << " '" << args[0] << "'";
  }

  return invalid(message.str());
}

std::string render_help(std::string_view executable_name) {
  std::ostringstream out;
  out << "wmux - native Windows terminal multiplexer\n\n";
  out << "Usage:\n";
  out << "  " << executable_name << " [command]\n\n";
  out << "Commands:\n";
  out << "  new -s <name>                  Create and attach to a session\n";
  out << "  ls                             List sessions\n";
  out << "  attach -t <name>               Attach to a session\n";
  out << "  rename-session -t <old> <new>  Rename a session\n";
  out << "  kill-session -t <name>         Kill a session\n";
  out << "  new-window [-t <session>] -n <name>\n";
  out << "                                 Create a window\n";
  out << "  list-windows [-t <session>]    List windows\n";
  out << "  rename-window [-t <session>] <new>\n";
  out << "                                 Rename the active window\n";
  out << "  split-window [-t <session>] -h|-v\n";
  out << "                                 Split the active pane\n";
  out << "  set -g <option> <value>       Set a validated global option\n";
  out << "  bind-key <key> <action>       Bind a prefix key globally\n";
  out << "  unbind-key <key>              Disable a prefix key globally\n";
  out << "  server status                  Show daemon status\n";
  out << "  server stop [--force]          Stop daemon\n";
  out << "  dump-state                     Dump daemon sessions, clients, panes, and metrics\n";
  out << "  dump-layout                    Dump session/window layout trees and pane rects\n";
  out << "  dump-events                    Dump bounded recent diagnostic event rings\n";
  out << "  reset-terminal                 Restore terminal cursor/mouse/display state\n";
  out << "  doctor [--json]                Print environment and runtime diagnostics\n";
  out << "  debug-keys                     Print decoded input events for diagnostics\n";
  out << "  version                        Print wmux version information\n";
  out << "  help                           Print this help message\n\n";
  out << "Options:\n";
  out << "  -h, --help                     Print this help message\n";
  out << "  --version                      Print wmux version information\n";
  out << "  --daemon                       Run the background daemon\n";
  return out.str();
}

std::string render_version() {
  std::ostringstream out;
  out << "wmux " << version_string() << "\n";
  return out.str();
}

std::string render_placeholder_response(const CommandLine& command) {
  std::ostringstream out;

  switch (command.kind) {
    case CommandKind::DefaultSession:
      out << "wmux: interactive session startup is not implemented yet\n";
      break;
    case CommandKind::Daemon:
      break;
    case CommandKind::NewSession:
      out << "wmux: would create session '" << command.session_name << "'\n";
      break;
    case CommandKind::ListSessions:
      out << "wmux: would list sessions\n";
      break;
    case CommandKind::AttachSession:
      out << "wmux: would attach to session '" << command.session_name << "'\n";
      break;
    case CommandKind::RenameSession:
      out << "wmux: would rename session '" << command.target_name << "' to '"
          << command.new_name << "'\n";
      break;
    case CommandKind::KillSession:
      out << "wmux: would kill session '" << command.session_name << "'\n";
      break;
    case CommandKind::NewWindow:
      out << "wmux: would create window '" << command.window_name << "'";
      if (!command.session_name.empty()) {
        out << " in session '" << command.session_name << "'";
      }
      out << "\n";
      break;
    case CommandKind::ListWindows:
      out << "wmux: would list windows";
      if (!command.session_name.empty()) {
        out << " in session '" << command.session_name << "'";
      }
      out << "\n";
      break;
    case CommandKind::RenameWindow:
      out << "wmux: would rename active window to '" << command.window_name << "'";
      if (!command.session_name.empty()) {
        out << " in session '" << command.session_name << "'";
      }
      out << "\n";
      break;
    case CommandKind::SplitWindow:
      out << "wmux: would split active pane " << command.split_direction;
      if (!command.session_name.empty()) {
        out << " in session '" << command.session_name << "'";
      }
      out << "\n";
      break;
    case CommandKind::SetOption:
      out << "wmux: would set " << command.option_name << " to " << command.option_value << "\n";
      break;
    case CommandKind::BindKey:
      out << "wmux: would bind key " << command.key_name << " to "
          << command.key_action << "\n";
      break;
    case CommandKind::UnbindKey:
      out << "wmux: would unbind key " << command.key_name << "\n";
      break;
    case CommandKind::ServerStatus:
      out << "wmux: daemon status is not implemented yet\n";
      break;
    case CommandKind::ServerStop:
      out << "wmux: daemon stop is not implemented yet";
      if (command.force) {
        out << " (forced)";
      }
      out << "\n";
      break;
    case CommandKind::DumpState:
      out << "wmux: would dump daemon state\n";
      break;
    case CommandKind::DumpLayout:
      out << "wmux: would dump daemon layout\n";
      break;
    case CommandKind::DumpEvents:
      out << "wmux: would dump daemon events\n";
      break;
    case CommandKind::ResetTerminal:
      out << "wmux: would reset terminal state\n";
      break;
    case CommandKind::Doctor:
      out << "wmux: would print diagnostics\n";
      break;
    case CommandKind::DebugKeys:
      out << "wmux: would print decoded input events\n";
      break;
    case CommandKind::Help:
    case CommandKind::Version:
    case CommandKind::Unknown:
      break;
  }

  return out.str();
}

}  // namespace wmux
