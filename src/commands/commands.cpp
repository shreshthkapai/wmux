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

CommandLine parse_server_command(const std::vector<std::string_view>& args) {
  if (args.size() != 2) {
    return invalid("server requires one subcommand: status or stop");
  }

  if (args[1] == "status") {
    CommandLine command;
    command.kind = CommandKind::ServerStatus;
    return command;
  }

  if (args[1] == "stop") {
    CommandLine command;
    command.kind = CommandKind::ServerStop;
    return command;
  }

  std::ostringstream message;
  message << "unknown server subcommand " << quoted(args[1]);
  return invalid(message.str());
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

  if (args[0] == "server") {
    return parse_server_command(args);
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
  out << "  new -s <name>                  Create a session\n";
  out << "  ls                             List sessions\n";
  out << "  attach -t <name>               Attach to a session\n";
  out << "  rename-session -t <old> <new>  Rename a session\n";
  out << "  kill-session -t <name>         Kill a session\n";
  out << "  server status                  Show daemon status\n";
  out << "  server stop                    Stop daemon\n";
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
    case CommandKind::ServerStatus:
      out << "wmux: daemon status is not implemented yet\n";
      break;
    case CommandKind::ServerStop:
      out << "wmux: daemon stop is not implemented yet\n";
      break;
    case CommandKind::Help:
    case CommandKind::Version:
    case CommandKind::Unknown:
      break;
  }

  return out.str();
}

}  // namespace wmux
