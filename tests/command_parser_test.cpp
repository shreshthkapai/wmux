#include "wmux/commands.hpp"

#include <cassert>
#include <string>
#include <string_view>
#include <vector>

void run_ipc_protocol_tests();
void run_session_manager_tests();

namespace {

void expects_default_for_empty_args() {
  const auto result = wmux::parse_command_line({});
  assert(result.kind == wmux::CommandKind::DefaultSession);
  assert(result.error.empty());
}

void expects_help_flag() {
  const std::vector<std::string_view> args{"--help"};
  const auto result = wmux::parse_command_line(args);
  assert(result.kind == wmux::CommandKind::Help);
}

void expects_version_command() {
  const std::vector<std::string_view> args{"version"};
  const auto result = wmux::parse_command_line(args);
  assert(result.kind == wmux::CommandKind::Version);
}

void expects_daemon_flag() {
  const std::vector<std::string_view> args{"--daemon"};
  const auto result = wmux::parse_command_line(args);
  assert(result.kind == wmux::CommandKind::Daemon);
}

void expects_new_session_command() {
  const std::vector<std::string_view> args{"new", "-s", "finance"};
  const auto result = wmux::parse_command_line(args);
  assert(result.kind == wmux::CommandKind::NewSession);
  assert(result.session_name == "finance");
  assert(result.error.empty());
}

void expects_list_sessions_command() {
  const std::vector<std::string_view> args{"ls"};
  const auto result = wmux::parse_command_line(args);
  assert(result.kind == wmux::CommandKind::ListSessions);
  assert(result.error.empty());
}

void expects_attach_session_command() {
  const std::vector<std::string_view> args{"attach", "-t", "finance"};
  const auto result = wmux::parse_command_line(args);
  assert(result.kind == wmux::CommandKind::AttachSession);
  assert(result.session_name == "finance");
  assert(result.error.empty());
}

void expects_rename_session_command() {
  const std::vector<std::string_view> args{"rename-session", "-t", "finance", "trading"};
  const auto result = wmux::parse_command_line(args);
  assert(result.kind == wmux::CommandKind::RenameSession);
  assert(result.target_name == "finance");
  assert(result.new_name == "trading");
  assert(result.error.empty());
}

void expects_kill_session_command() {
  const std::vector<std::string_view> args{"kill-session", "-t", "finance"};
  const auto result = wmux::parse_command_line(args);
  assert(result.kind == wmux::CommandKind::KillSession);
  assert(result.session_name == "finance");
  assert(result.error.empty());
}

void expects_server_status_command() {
  const std::vector<std::string_view> args{"server", "status"};
  const auto result = wmux::parse_command_line(args);
  assert(result.kind == wmux::CommandKind::ServerStatus);
  assert(result.error.empty());
}

void expects_server_stop_command() {
  const std::vector<std::string_view> args{"server", "stop"};
  const auto result = wmux::parse_command_line(args);
  assert(result.kind == wmux::CommandKind::ServerStop);
  assert(result.error.empty());
}

void expects_missing_session_name_error() {
  const std::vector<std::string_view> args{"new", "-s"};
  const auto result = wmux::parse_command_line(args);
  assert(result.kind == wmux::CommandKind::Unknown);
  assert(result.error == "new requires -s <name>");
}

void expects_wrong_target_flag_error() {
  const std::vector<std::string_view> args{"attach", "-s", "finance"};
  const auto result = wmux::parse_command_line(args);
  assert(result.kind == wmux::CommandKind::Unknown);
  assert(result.error == "attach requires -t <name>");
}

void expects_server_subcommand_error() {
  const std::vector<std::string_view> args{"server", "restart"};
  const auto result = wmux::parse_command_line(args);
  assert(result.kind == wmux::CommandKind::Unknown);
  assert(result.error == "unknown server subcommand 'restart'");
}

void expects_placeholder_response() {
  const std::vector<std::string_view> args{"rename-session", "-t", "finance", "trading"};
  const auto result = wmux::parse_command_line(args);
  const auto response = wmux::render_placeholder_response(result);
  assert(response == "wmux: would rename session 'finance' to 'trading'\n");
}

void expects_unknown_command_error() {
  const std::vector<std::string_view> args{"wat"};
  const auto result = wmux::parse_command_line(args);
  assert(result.kind == wmux::CommandKind::Unknown);
  assert(!result.error.empty());
}

}  // namespace

int main() {
  expects_default_for_empty_args();
  expects_help_flag();
  expects_version_command();
  expects_daemon_flag();
  expects_new_session_command();
  expects_list_sessions_command();
  expects_attach_session_command();
  expects_rename_session_command();
  expects_kill_session_command();
  expects_server_status_command();
  expects_server_stop_command();
  expects_missing_session_name_error();
  expects_wrong_target_flag_error();
  expects_server_subcommand_error();
  expects_placeholder_response();
  expects_unknown_command_error();
  run_ipc_protocol_tests();
  run_session_manager_tests();
}
