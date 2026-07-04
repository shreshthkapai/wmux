#include "wmux/commands.hpp"

#include <cassert>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

void run_ipc_protocol_tests();
void run_attach_input_mode_tests();
void run_attach_keymap_tests();
void run_command_mode_tests();
void run_command_engine_tests();
void run_config_tests();
void run_copy_selection_tests();
void run_daemon_event_tests();
void run_daemon_render_benchmark_tests();
void run_daemon_render_tests();
void run_mouse_input_tests();
void run_paste_buffer_tests();
void run_platform_boundary_tests();
void run_platform_info_tests();
void run_session_manager_tests();
void run_status_line_tests();
void run_terminal_capabilities_tests();
void run_terminal_control_tests();
void run_terminal_engine_benchmark_tests();
void run_terminal_engine_tests();
void run_terminal_input_tests();
void run_terminal_grid_tests();
void run_terminal_vt_tests();
void run_windows_clipboard_tests();

std::string test_only_filter() {
#if defined(_MSC_VER)
  char* value = nullptr;
  std::size_t size = 0;
  if (_dupenv_s(&value, &size, "WMUX_TEST_ONLY") != 0 || value == nullptr) {
    return {};
  }
  std::string filter{value};
  std::free(value);
  return filter;
#else
  if (const char* value = std::getenv("WMUX_TEST_ONLY")) {
    return value;
  }
  return {};
#endif
}

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

void expects_new_window_command() {
  const std::vector<std::string_view> args{"new-window", "-n", "logs"};
  const auto result = wmux::parse_command_line(args);
  assert(result.kind == wmux::CommandKind::NewWindow);
  assert(result.window_name == "logs");
  assert(result.session_name.empty());
  assert(result.error.empty());
}

void expects_unnamed_new_window_command() {
  const std::vector<std::string_view> args{"new-window"};
  const auto result = wmux::parse_command_line(args);
  assert(result.kind == wmux::CommandKind::NewWindow);
  assert(result.window_name.empty());
  assert(result.error.empty());
}

void expects_targeted_new_window_command() {
  const std::vector<std::string_view> args{"new-window", "-t", "finance", "-n", "logs"};
  const auto result = wmux::parse_command_line(args);
  assert(result.kind == wmux::CommandKind::NewWindow);
  assert(result.session_name == "finance");
  assert(result.window_name == "logs");
  assert(result.error.empty());
}

void expects_select_window_command() {
  const std::vector<std::string_view> args{"select-window", "-t", "1"};
  const auto result = wmux::parse_command_line(args);
  assert(result.kind == wmux::CommandKind::SelectWindow);
  assert(result.target_name == "1");
  assert(result.error.empty());
}

void expects_next_previous_window_commands() {
  {
    const std::vector<std::string_view> args{"next-window"};
    const auto result = wmux::parse_command_line(args);
    assert(result.kind == wmux::CommandKind::NextWindow);
    assert(result.session_name.empty());
    assert(result.error.empty());
  }
  {
    const std::vector<std::string_view> args{"previous-window", "-t", "finance"};
    const auto result = wmux::parse_command_line(args);
    assert(result.kind == wmux::CommandKind::PreviousWindow);
    assert(result.session_name == "finance");
    assert(result.error.empty());
  }
}

void expects_kill_window_and_pane_commands() {
  {
    const std::vector<std::string_view> args{"kill-window", "-t", "finance:1"};
    const auto result = wmux::parse_command_line(args);
    assert(result.kind == wmux::CommandKind::KillWindow);
    assert(result.target_name == "finance:1");
    assert(result.error.empty());
  }
  {
    const std::vector<std::string_view> args{"kill-pane"};
    const auto result = wmux::parse_command_line(args);
    assert(result.kind == wmux::CommandKind::KillPane);
    assert(result.target_name.empty());
    assert(result.error.empty());
  }
}

void expects_list_windows_command() {
  const std::vector<std::string_view> args{"list-windows"};
  const auto result = wmux::parse_command_line(args);
  assert(result.kind == wmux::CommandKind::ListWindows);
  assert(result.session_name.empty());
  assert(result.error.empty());
}

void expects_targeted_list_windows_command() {
  const std::vector<std::string_view> args{"list-windows", "-t", "finance"};
  const auto result = wmux::parse_command_line(args);
  assert(result.kind == wmux::CommandKind::ListWindows);
  assert(result.session_name == "finance");
  assert(result.error.empty());
}

void expects_rename_window_command() {
  const std::vector<std::string_view> args{"rename-window", "agents"};
  const auto result = wmux::parse_command_line(args);
  assert(result.kind == wmux::CommandKind::RenameWindow);
  assert(result.window_name == "agents");
  assert(result.session_name.empty());
  assert(result.error.empty());
}

void expects_targeted_rename_window_command() {
  const std::vector<std::string_view> args{"rename-window", "-t", "finance", "agents"};
  const auto result = wmux::parse_command_line(args);
  assert(result.kind == wmux::CommandKind::RenameWindow);
  assert(result.session_name == "finance");
  assert(result.window_name == "agents");
  assert(result.error.empty());
}

void expects_split_window_horizontal_command() {
  const std::vector<std::string_view> args{"split-window", "-h"};
  const auto result = wmux::parse_command_line(args);
  assert(result.kind == wmux::CommandKind::SplitWindow);
  assert(result.split_direction == "horizontal");
  assert(result.session_name.empty());
  assert(result.error.empty());
}

void expects_split_window_vertical_command() {
  const std::vector<std::string_view> args{"split-window", "-t", "finance", "-v"};
  const auto result = wmux::parse_command_line(args);
  assert(result.kind == wmux::CommandKind::SplitWindow);
  assert(result.session_name == "finance");
  assert(result.split_direction == "vertical");
  assert(result.error.empty());
}

void expects_resize_pane_command() {
  const std::vector<std::string_view> args{"resize-pane", "-t", "finance:1", "-L"};
  const auto result = wmux::parse_command_line(args);
  assert(result.kind == wmux::CommandKind::ResizePane);
  assert(result.target_name == "finance:1");
  assert(result.resize_direction == "-L");
  assert(result.error.empty());
}

void expects_select_layout_spread_command() {
  const std::vector<std::string_view> args{"select-layout", "-E"};
  const auto result = wmux::parse_command_line(args);
  assert(result.kind == wmux::CommandKind::SelectLayout);
  assert(result.error.empty());
}

void expects_set_mouse_on_command() {
  const std::vector<std::string_view> args{"set", "-g", "mouse", "on"};
  const auto result = wmux::parse_command_line(args);
  assert(result.kind == wmux::CommandKind::SetOption);
  assert(result.option_name == "mouse");
  assert(result.option_value == "on");
  assert(result.error.empty());
}

void expects_set_mouse_off_command() {
  const std::vector<std::string_view> args{"set", "-g", "mouse", "off"};
  const auto result = wmux::parse_command_line(args);
  assert(result.kind == wmux::CommandKind::SetOption);
  assert(result.option_name == "mouse");
  assert(result.option_value == "off");
  assert(result.error.empty());
}

void expects_bind_key_command() {
  const std::vector<std::string_view> args{"bind-key", "z", "new-window"};
  const auto result = wmux::parse_command_line(args);
  assert(result.kind == wmux::CommandKind::BindKey);
  assert(result.key_name == "z");
  assert(result.key_action == "new-window");
  assert(result.error.empty());
}

void expects_bind_key_command_with_multi_token_action() {
  const std::vector<std::string_view> args{"bind-key", "E", "select-layout", "-E"};
  const auto result = wmux::parse_command_line(args);
  assert(result.kind == wmux::CommandKind::BindKey);
  assert(result.key_name == "E");
  assert(result.key_action == "select-layout -E");
  assert(result.error.empty());
}

void expects_unbind_key_command() {
  const std::vector<std::string_view> args{"unbind-key", "e"};
  const auto result = wmux::parse_command_line(args);
  assert(result.kind == wmux::CommandKind::UnbindKey);
  assert(result.key_name == "e");
  assert(result.error.empty());
}

void expects_server_status_command() {
  const std::vector<std::string_view> args{"server", "status"};
  const auto result = wmux::parse_command_line(args);
  assert(result.kind == wmux::CommandKind::ServerStatus);
  assert(result.error.empty());
}

void expects_dump_state_command() {
  const std::vector<std::string_view> args{"dump-state"};
  const auto result = wmux::parse_command_line(args);
  assert(result.kind == wmux::CommandKind::DumpState);
  assert(result.error.empty());
}

void expects_dump_layout_command() {
  const std::vector<std::string_view> args{"dump-layout"};
  const auto result = wmux::parse_command_line(args);
  assert(result.kind == wmux::CommandKind::DumpLayout);
  assert(result.error.empty());
}

void expects_dump_events_command() {
  const std::vector<std::string_view> args{"dump-events"};
  const auto result = wmux::parse_command_line(args);
  assert(result.kind == wmux::CommandKind::DumpEvents);
  assert(result.error.empty());
}

void expects_reset_terminal_command() {
  const std::vector<std::string_view> args{"reset-terminal"};
  const auto result = wmux::parse_command_line(args);
  assert(result.kind == wmux::CommandKind::ResetTerminal);
  assert(result.error.empty());
}

void expects_doctor_command() {
  const std::vector<std::string_view> args{"doctor"};
  const auto result = wmux::parse_command_line(args);
  assert(result.kind == wmux::CommandKind::Doctor);
  assert(!result.json);
  assert(result.error.empty());
}

void expects_doctor_json_command() {
  const std::vector<std::string_view> args{"doctor", "--json"};
  const auto result = wmux::parse_command_line(args);
  assert(result.kind == wmux::CommandKind::Doctor);
  assert(result.json);
  assert(result.error.empty());
}

void expects_debug_keys_command() {
  const std::vector<std::string_view> args{"debug-keys"};
  const auto result = wmux::parse_command_line(args);
  assert(result.kind == wmux::CommandKind::DebugKeys);
  assert(result.error.empty());
}

void expects_server_stop_command() {
  const std::vector<std::string_view> args{"server", "stop"};
  const auto result = wmux::parse_command_line(args);
  assert(result.kind == wmux::CommandKind::ServerStop);
  assert(!result.force);
  assert(result.error.empty());
}

void expects_forced_server_stop_command() {
  const std::vector<std::string_view> args{"server", "stop", "--force"};
  const auto result = wmux::parse_command_line(args);
  assert(result.kind == wmux::CommandKind::ServerStop);
  assert(result.force);
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

void expects_missing_window_name_error() {
  const std::vector<std::string_view> args{"new-window", "-n"};
  const auto result = wmux::parse_command_line(args);
  assert(result.kind == wmux::CommandKind::Unknown);
  assert(result.error == "new-window requires -n <name>");
}

void expects_bad_list_windows_argument_error() {
  const std::vector<std::string_view> args{"list-windows", "finance"};
  const auto result = wmux::parse_command_line(args);
  assert(result.kind == wmux::CommandKind::Unknown);
  assert(result.error == "list-windows accepts optional -t <session>");
}

void expects_missing_split_direction_error() {
  const std::vector<std::string_view> args{"split-window"};
  const auto result = wmux::parse_command_line(args);
  assert(result.kind == wmux::CommandKind::Unknown);
  assert(result.error == "split-window requires one of -h or -v");
}

void expects_duplicate_split_direction_error() {
  const std::vector<std::string_view> args{"split-window", "-h", "-v"};
  const auto result = wmux::parse_command_line(args);
  assert(result.kind == wmux::CommandKind::Unknown);
  assert(result.error == "split-window accepts only one split direction");
}

void expects_bad_set_argument_error() {
  const std::vector<std::string_view> args{"set", "mouse", "on"};
  const auto result = wmux::parse_command_line(args);
  assert(result.kind == wmux::CommandKind::Unknown);
  assert(result.error == "set requires -g <option> <value>");
}

void expects_bad_bind_key_argument_error() {
  const std::vector<std::string_view> args{"bind-key", "z"};
  const auto result = wmux::parse_command_line(args);
  assert(result.kind == wmux::CommandKind::Unknown);
  assert(result.error == "bind-key requires <key> <action>");
}

void expects_bad_unbind_key_argument_error() {
  const std::vector<std::string_view> args{"unbind-key"};
  const auto result = wmux::parse_command_line(args);
  assert(result.kind == wmux::CommandKind::Unknown);
  assert(result.error == "unbind-key requires <key>");
}

void expects_bad_set_mouse_value_error() {
  const std::vector<std::string_view> args{"set", "-g", "mouse", "yes"};
  const auto result = wmux::parse_command_line(args);
  assert(result.kind == wmux::CommandKind::SetOption);
  assert(result.option_name == "mouse");
  assert(result.option_value == "yes");
  assert(result.error.empty());
}

void expects_status_set_option_command() {
  const std::vector<std::string_view> args{"set", "-g", "status", "on"};
  const auto result = wmux::parse_command_line(args);
  assert(result.kind == wmux::CommandKind::SetOption);
  assert(result.option_name == "status");
  assert(result.option_value == "on");
  assert(result.error.empty());
}

void expects_server_subcommand_error() {
  const std::vector<std::string_view> args{"server", "restart"};
  const auto result = wmux::parse_command_line(args);
  assert(result.kind == wmux::CommandKind::Unknown);
  assert(result.error == "unknown server subcommand 'restart'");
}

void expects_server_stop_argument_error() {
  const std::vector<std::string_view> args{"server", "stop", "--now"};
  const auto result = wmux::parse_command_line(args);
  assert(result.kind == wmux::CommandKind::Unknown);
  assert(result.error == "server stop accepts only optional --force");
}

void expects_bad_dump_state_argument_error() {
  const std::vector<std::string_view> args{"dump-state", "--json"};
  const auto result = wmux::parse_command_line(args);
  assert(result.kind == wmux::CommandKind::Unknown);
  assert(result.error == "dump-state does not accept arguments: '--json'");
}

void expects_bad_doctor_argument_error() {
  const std::vector<std::string_view> args{"doctor", "--verbose"};
  const auto result = wmux::parse_command_line(args);
  assert(result.kind == wmux::CommandKind::Unknown);
  assert(result.error == "doctor accepts only optional --json");
}

void expects_bad_debug_keys_argument_error() {
  const std::vector<std::string_view> args{"debug-keys", "--json"};
  const auto result = wmux::parse_command_line(args);
  assert(result.kind == wmux::CommandKind::Unknown);
  assert(result.error == "debug-keys does not accept arguments: '--json'");
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
  if (test_only_filter() == "terminal-engine-benchmark") {
    run_terminal_engine_benchmark_tests();
    return 0;
  }

  if (test_only_filter() == "daemon-render-benchmark") {
    run_daemon_render_benchmark_tests();
    return 0;
  }

  if (test_only_filter() == "terminal-engine") {
    run_terminal_engine_tests();
    return 0;
  }

  expects_default_for_empty_args();
  expects_help_flag();
  expects_version_command();
  expects_daemon_flag();
  expects_new_session_command();
  expects_list_sessions_command();
  expects_attach_session_command();
  expects_rename_session_command();
  expects_kill_session_command();
  expects_new_window_command();
  expects_unnamed_new_window_command();
  expects_targeted_new_window_command();
  expects_select_window_command();
  expects_next_previous_window_commands();
  expects_kill_window_and_pane_commands();
  expects_list_windows_command();
  expects_targeted_list_windows_command();
  expects_rename_window_command();
  expects_targeted_rename_window_command();
  expects_split_window_horizontal_command();
  expects_split_window_vertical_command();
  expects_resize_pane_command();
  expects_select_layout_spread_command();
  expects_set_mouse_on_command();
  expects_set_mouse_off_command();
  expects_bind_key_command();
  expects_bind_key_command_with_multi_token_action();
  expects_unbind_key_command();
  expects_server_status_command();
  expects_dump_state_command();
  expects_dump_layout_command();
  expects_dump_events_command();
  expects_reset_terminal_command();
  expects_doctor_command();
  expects_doctor_json_command();
  expects_debug_keys_command();
  expects_server_stop_command();
  expects_forced_server_stop_command();
  expects_missing_session_name_error();
  expects_wrong_target_flag_error();
  expects_missing_window_name_error();
  expects_bad_list_windows_argument_error();
  expects_missing_split_direction_error();
  expects_duplicate_split_direction_error();
  expects_bad_set_argument_error();
  expects_bad_bind_key_argument_error();
  expects_bad_unbind_key_argument_error();
  expects_bad_set_mouse_value_error();
  expects_status_set_option_command();
  expects_server_subcommand_error();
  expects_server_stop_argument_error();
  expects_bad_dump_state_argument_error();
  expects_bad_doctor_argument_error();
  expects_bad_debug_keys_argument_error();
  expects_placeholder_response();
  expects_unknown_command_error();
  run_ipc_protocol_tests();
  run_attach_input_mode_tests();
  run_attach_keymap_tests();
  run_command_mode_tests();
  run_command_engine_tests();
  run_config_tests();
  run_copy_selection_tests();
  run_daemon_event_tests();
  run_daemon_render_tests();
  run_mouse_input_tests();
  run_paste_buffer_tests();
  run_platform_boundary_tests();
  run_platform_info_tests();
  run_session_manager_tests();
  run_status_line_tests();
  run_terminal_capabilities_tests();
  run_terminal_control_tests();
  run_terminal_engine_tests();
  run_terminal_input_tests();
  run_terminal_grid_tests();
  run_terminal_vt_tests();
  run_windows_clipboard_tests();
}
