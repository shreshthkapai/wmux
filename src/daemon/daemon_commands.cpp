#include "daemon_commands.hpp"

#include "daemon_shell.hpp"
#include "wmux/ipc_protocol.hpp"

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>

namespace wmux::daemon_internal {
namespace {

std::optional<SessionId> resolve_target_session_for_window_command(
    DaemonState& state,
    const IpcRequest& request,
    std::string& error) {
  if (!request.session_name.empty()) {
    const auto session_id = state.sessions.session_id_for_name(request.session_name);
    if (!session_id) {
      error = session_error_message(SessionError::NotFound, request.session_name);
      return std::nullopt;
    }

    return session_id;
  }

  if (state.sessions.session_count() == 0) {
    error = "wmux: no sessions\n";
    return std::nullopt;
  }

  const auto only_session = state.sessions.only_session_id();
  if (!only_session) {
    error = "wmux: target session required; use -t <session>\n";
    return std::nullopt;
  }

  return only_session;
}

std::string handle_new_session(const IpcRequest& request, DaemonState& state) {
  if (request.session_name.empty()) {
    return make_response_json(false, session_error_message(SessionError::EmptyName, {}));
  }

  {
    std::lock_guard lock(state.mutex);
    if (state.sessions.has_session(request.session_name)) {
      return make_response_json(
          false, session_error_message(SessionError::DuplicateName, request.session_name));
    }
  }

  std::shared_ptr<PtyProcess> shell_process;
  auto shell = start_default_shell();
  if (!shell.process) {
    return make_response_json(false, shell.error);
  }
  shell_process = std::move(shell.process);

  {
    std::lock_guard lock(state.mutex);
    const auto result = state.sessions.create_session(request.session_name);
    if (!result.ok) {
      if (shell_process) {
        shell_process->terminate();
      }
      return make_response_json(false, session_error_message(result.error, request.session_name));
    }

    if (shell_process) {
      state.runtimes[result.id].windows[result.window_id].panes[result.pane_id].shell =
          std::move(shell_process);
    }
  }

  std::ostringstream out;
  out << "wmux: created session " << quoted(request.session_name) << "\n";
  return make_response_json(true, out.str());
}

std::string handle_list_sessions(DaemonState& state) {
  std::lock_guard lock(state.mutex);
  const auto listed = state.sessions.list_sessions();
  if (listed.empty()) {
    return make_response_json(true, "wmux: no sessions\n");
  }

  std::ostringstream out;
  for (const auto& session : listed) {
    out << session.name << "\n";
  }
  return make_response_json(true, out.str());
}

std::string handle_rename_session(const IpcRequest& request, DaemonState& state) {
  std::lock_guard lock(state.mutex);
  const auto result = state.sessions.rename_session(request.target_name, request.new_name);
  if (!result.ok) {
    const auto name =
        result.error == SessionError::DuplicateName ? request.new_name : request.target_name;
    return make_response_json(false, session_error_message(result.error, name));
  }

  std::ostringstream out;
  out << "wmux: renamed session " << quoted(request.target_name) << " to "
      << quoted(request.new_name) << "\n";
  return make_response_json(true, out.str());
}

std::string handle_kill_session(const IpcRequest& request, DaemonState& state) {
  std::vector<std::shared_ptr<PtyProcess>> killed_shells;
  SessionId killed_session_id{0};
  {
    std::lock_guard lock(state.mutex);
    const auto result = state.sessions.kill_session(request.session_name);
    if (!result.ok) {
      return make_response_json(false, session_error_message(result.error, request.session_name));
    }

    killed_session_id = result.id;
    const auto runtime = state.runtimes.find(result.id);
    if (runtime != state.runtimes.end()) {
      killed_shells.reserve(runtime->second.windows.size());
      for (auto& [window_id, window] : runtime->second.windows) {
        (void)window_id;
        for (auto& [pane_id, pane] : window.panes) {
          (void)pane_id;
          if (pane.shell) {
            killed_shells.push_back(std::move(pane.shell));
          }
        }
      }
      state.runtimes.erase(runtime);
    }
  }

  disconnect_attach_clients_for_session(state, killed_session_id);

  for (auto& killed_shell : killed_shells) {
    if (killed_shell) {
      killed_shell->terminate();
    }
  }

  std::ostringstream out;
  out << "wmux: killed session " << quoted(request.session_name) << "\n";
  return make_response_json(true, out.str());
}

std::string handle_new_window(const IpcRequest& request, DaemonState& state) {
  if (request.window_name.empty()) {
    return make_response_json(false, window_error_message(WindowError::EmptyName, {}));
  }

  SessionId session_id{0};
  {
    std::lock_guard lock(state.mutex);
    std::string error;
    const auto resolved = resolve_target_session_for_window_command(state, request, error);
    if (!resolved) {
      return make_response_json(false, error);
    }
    session_id = *resolved;
  }

  std::shared_ptr<PtyProcess> shell_process;
  auto shell = start_default_shell();
  if (!shell.process) {
    return make_response_json(false, shell.error);
  }
  shell_process = std::move(shell.process);

  WindowOperationResult result;
  {
    std::lock_guard lock(state.mutex);
    result = state.sessions.create_window(session_id, request.window_name);
    if (!result.ok) {
      if (shell_process) {
        shell_process->terminate();
      }
      return make_response_json(false, window_error_message(result.error, request.window_name));
    }

    if (shell_process) {
      state.runtimes[result.session_id].windows[result.window_id].panes[result.pane_id].shell =
          std::move(shell_process);
    }
  }

  std::ostringstream out;
  out << "wmux: created window " << quoted(request.window_name) << "\n";
  return make_response_json(true, out.str());
}

std::string handle_list_windows(const IpcRequest& request, DaemonState& state) {
  std::lock_guard lock(state.mutex);
  std::string error;
  const auto session_id = resolve_target_session_for_window_command(state, request, error);
  if (!session_id) {
    return make_response_json(false, error);
  }

  const auto active_window = state.sessions.active_window_id(*session_id);
  const auto windows = state.sessions.list_windows(*session_id);
  if (windows.empty()) {
    return make_response_json(true, "wmux: no windows\n");
  }

  std::ostringstream out;
  for (const auto& window : windows) {
    out << window.id << ": " << window.name;
    if (active_window && window.id == *active_window) {
      out << " *";
    }
    out << "\n";
  }
  return make_response_json(true, out.str());
}

std::string handle_rename_window(const IpcRequest& request, DaemonState& state) {
  if (request.window_name.empty()) {
    return make_response_json(false, window_error_message(WindowError::EmptyName, {}));
  }

  std::lock_guard lock(state.mutex);
  std::string error;
  const auto session_id = resolve_target_session_for_window_command(state, request, error);
  if (!session_id) {
    return make_response_json(false, error);
  }

  const auto result = state.sessions.rename_active_window(*session_id, request.window_name);
  if (!result.ok) {
    return make_response_json(false, window_error_message(result.error, request.window_name));
  }

  std::ostringstream out;
  out << "wmux: renamed active window to " << quoted(request.window_name) << "\n";
  return make_response_json(true, out.str());
}

std::optional<SplitDirection> parse_split_direction(std::string_view direction) {
  if (direction == "horizontal") {
    return SplitDirection::Horizontal;
  }

  if (direction == "vertical") {
    return SplitDirection::Vertical;
  }

  return std::nullopt;
}

std::string handle_split_window(const IpcRequest& request, DaemonState& state) {
  const auto direction = parse_split_direction(request.split_direction);
  if (!direction) {
    return make_response_json(false, "wmux: split-window requires one of -h or -v\n");
  }

  SessionId session_id{0};
  {
    std::lock_guard lock(state.mutex);
    std::string error;
    const auto resolved = resolve_target_session_for_window_command(state, request, error);
    if (!resolved) {
      return make_response_json(false, error);
    }
    session_id = *resolved;
  }

  std::shared_ptr<PtyProcess> shell_process;
  auto shell = start_default_shell();
  if (!shell.process) {
    return make_response_json(false, shell.error);
  }
  shell_process = std::move(shell.process);

  PaneOperationResult result;
  {
    std::lock_guard lock(state.mutex);
    result = state.sessions.split_active_pane(session_id, *direction);
    if (!result.ok) {
      if (shell_process) {
        shell_process->terminate();
      }
      return make_response_json(false, pane_error_message(result.error));
    }

    if (shell_process) {
      state.runtimes[result.session_id]
          .windows[result.window_id]
          .panes[result.pane_id]
          .shell = std::move(shell_process);
    }
  }

  std::ostringstream out;
  out << "wmux: split active pane "
      << (request.split_direction == "horizontal" ? "horizontally" : "vertically") << "\n";
  return make_response_json(true, out.str());
}

std::string handle_session_request(const IpcRequest& request, DaemonState& state) {
  if (request.type == "DefaultSession") {
    return make_response_json(true, "wmux: interactive session startup is not implemented yet\n");
  }

  if (request.type == "NewSession") {
    return handle_new_session(request, state);
  }

  if (request.type == "ListSessions") {
    return handle_list_sessions(state);
  }

  if (request.type == "AttachSession") {
    return make_response_json(false, "wmux: attach requires the streaming client transport\n");
  }

  if (request.type == "RenameSession") {
    return handle_rename_session(request, state);
  }

  if (request.type == "KillSession") {
    return handle_kill_session(request, state);
  }

  if (request.type == "NewWindow") {
    return handle_new_window(request, state);
  }

  if (request.type == "ListWindows") {
    return handle_list_windows(request, state);
  }

  if (request.type == "RenameWindow") {
    return handle_rename_window(request, state);
  }

  if (request.type == "SplitWindow") {
    return handle_split_window(request, state);
  }

  return {};
}

}  // namespace

std::string handle_request(
    const IpcRequest& request,
    std::atomic_bool& should_stop,
    DaemonState& state) {
  if (request.type == "Ping") {
    return make_response_json(true, "wmux: daemon is running\n");
  }

  if (request.type == "ServerStatus") {
    const auto stats = daemon_stats(state);
    std::ostringstream out;
    out << "wmux: daemon is running\n";
    out << "sessions: " << stats.session_count << "\n";
    out << "attach clients: " << stats.attach_client_count << "\n";
    return make_response_json(true, out.str());
  }

  if (request.type == "ServerStop") {
    if (!request.force) {
      std::lock_guard lock(state.mutex);
      if (state.sessions.session_count() > 0) {
        return make_response_json(
            false,
            "wmux: live sessions exist; use 'wmux server stop --force' to terminate them\n");
      }
    }

    should_stop = true;
    if (request.force) {
      disconnect_all_attach_clients(state);
      auto shells = take_all_shells(state);
      for (auto& shell : shells) {
        if (shell) {
          shell->terminate();
        }
      }
      return make_response_json(true, "wmux: daemon stopping; live sessions terminated\n");
    }

    return make_response_json(true, "wmux: daemon stopping\n");
  }

  const auto session_response = handle_session_request(request, state);
  if (!session_response.empty()) {
    return session_response;
  }

  return make_response_json(false, "wmux: daemon does not understand request\n");
}

}  // namespace wmux::daemon_internal
