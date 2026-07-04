#include "daemon_commands.hpp"

#include "daemon_command_engine.hpp"
#include "daemon_shell.hpp"
#include "wmux/ipc_protocol.hpp"
#include "wmux/logging.hpp"
#include "wmux/platform/services.hpp"
#include "wmux/resource_limits.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>

namespace wmux::daemon_internal {
namespace {

struct IpcCommandContext {
  RequestId request_id{0};
  std::optional<ClientId> client_id;
};

void record_control_request(
    DaemonState& state,
    const IpcRequest& request,
    const IpcCommandContext& context) {
  record_diagnostic_event(
      state,
      DiagnosticEvent{
          0,
          {},
          DiagnosticEventCategory::Command,
          "info",
          "control_request",
          context.request_id,
          context.client_id.value_or(0),
          0,
          0,
          0,
          "control IPC request",
          {{"type", request.type},
           {"session_name", request.session_name},
           {"target_name", request.target_name},
           {"window_name", request.window_name}}});
}

DaemonCommandResult command_result(std::string response, bool should_stop = false) {
  return DaemonCommandResult{std::move(response), should_stop};
}

std::string with_trailing_newline(std::string_view value) {
  std::string text{value};
  if (text.empty() || text.back() != '\n') {
    text.push_back('\n');
  }
  return text;
}

std::string timestamp_text(std::chrono::system_clock::time_point time) {
  const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(time);
  const auto millis =
      std::chrono::duration_cast<std::chrono::milliseconds>(time - seconds).count();
  const std::time_t raw_time = std::chrono::system_clock::to_time_t(time);
  std::tm local_time{};
#ifdef _WIN32
  localtime_s(&local_time, &raw_time);
#else
  localtime_r(&raw_time, &local_time);
#endif

  std::ostringstream out;
  out << std::put_time(&local_time, "%Y-%m-%dT%H:%M:%S") << "."
      << std::setw(3) << std::setfill('0') << millis;
  return out.str();
}

std::string split_direction_name(SplitDirection direction) {
  return direction == SplitDirection::Horizontal ? "LeftRight" : "TopBottom";
}

PaneDirection pane_direction_from_resize_direction(ResizeDirection direction) {
  switch (direction) {
    case ResizeDirection::Left:
      return PaneDirection::Left;
    case ResizeDirection::Right:
      return PaneDirection::Right;
    case ResizeDirection::Up:
      return PaneDirection::Up;
    case ResizeDirection::Down:
      return PaneDirection::Down;
  }
  return PaneDirection::Right;
}

std::string client_mode_name(DaemonState::ClientMode mode) {
  switch (mode) {
    case DaemonState::ClientMode::Normal:
      return "normal";
    case DaemonState::ClientMode::Command:
      return "command";
    case DaemonState::ClientMode::Copy:
      return "copy";
  }

  return "unknown";
}

std::string bool_text(bool value) {
  return value ? "yes" : "no";
}

std::string resource_limit_message(std::string_view object, std::size_t limit) {
  std::ostringstream out;
  out << "wmux: " << object << " limit reached (" << limit << ")\n";
  return out.str();
}

std::string next_window_name(const std::vector<WindowSummary>& windows) {
  for (std::uint64_t candidate = 0; candidate < 100000; ++candidate) {
    const auto name = std::to_string(candidate);
    const auto exists = std::any_of(windows.begin(), windows.end(), [&](const auto& window) {
      return window.name == name;
    });
    if (!exists) {
      return name;
    }
  }

  return "window-" + std::to_string(windows.size());
}

std::string process_lifecycle_summary(const PtyProcessLifecycle& lifecycle) {
  std::ostringstream out;
  out << "pid=" << lifecycle.process_id
      << " terminating=" << bool_text(lifecycle.terminating)
      << " reader_done=" << bool_text(lifecycle.reader_done)
      << " job=" << bool_text(lifecycle.job_object_assigned)
      << " cleanup=" << (lifecycle.job_object_assigned ? "job-object" : "degraded")
      << " conpty=" << bool_text(lifecycle.pseudo_console_open)
      << " stdin=" << bool_text(lifecycle.input_pipe_write_open)
      << " stdout=" << bool_text(lifecycle.output_pipe_read_open);
  return out.str();
}

std::unordered_map<PaneId, PaneLayoutRect> pane_rect_index(
    const WindowSummary& window,
    int columns,
    int rows) {
  std::unordered_map<PaneId, PaneLayoutRect> rects;
  if (columns <= 0 || rows <= 0) {
    return rects;
  }
  for (const auto& rect : compute_pane_layout_rects(window.pane_tree, columns, rows)) {
    rects.emplace(rect.pane_id, rect);
  }
  return rects;
}

void append_metadata(std::ostringstream& out, const std::vector<DiagnosticMetadata>& metadata) {
  for (const auto& field : metadata) {
    out << " " << field.key << "=" << quoted(field.value);
  }
}

void append_diagnostic_ring(
    std::ostringstream& out,
    std::string_view title,
    const std::vector<DiagnosticEvent>& events) {
  out << title << " (" << events.size() << ")\n";
  for (const auto& event : events) {
    out << "  #" << event.sequence
        << " " << timestamp_text(event.timestamp)
        << " level=" << event.level
        << " event=" << event.event_type;
    if (event.request_id != 0) {
      out << " request_id=" << event.request_id;
    }
    if (event.client_id != 0) {
      out << " client_id=" << event.client_id;
    }
    if (event.session_id != 0) {
      out << " session_id=" << event.session_id;
    }
    if (event.window_id != 0) {
      out << " window_id=" << event.window_id;
    }
    if (event.pane_id != 0) {
      out << " pane_id=" << event.pane_id;
    }
    if (!event.message.empty()) {
      out << " message=" << quoted(event.message);
    }
    append_metadata(out, event.metadata);
    out << "\n";
  }
}

void append_layout_tree(
    std::ostringstream& out,
    const PaneNode& node,
    const std::unordered_map<PaneId, PaneLayoutRect>& rects,
    PaneId active_pane_id,
    std::string_view indent) {
  if (node.kind == PaneNode::Kind::Leaf) {
    out << indent << "Pane " << node.pane_id;
    const auto rect = rects.find(node.pane_id);
    if (rect != rects.end()) {
      out << " rect=(" << rect->second.left << "," << rect->second.top << " "
          << rect->second.width << "x" << rect->second.height << ")";
    }
    if (node.pane_id == active_pane_id) {
      out << " active";
    }
    out << "\n";
    return;
  }

  out << indent << split_direction_name(node.direction) << " node=" << node.node_id
      << " weights=[";
  for (std::size_t index = 0; index < node.child_weights.size(); ++index) {
    if (index > 0) {
      out << ",";
    }
    out << std::fixed << std::setprecision(3) << node.child_weights[index];
  }
  out << "]\n";

  const std::string child_indent{indent};
  for (std::size_t index = 0; index < node.children.size(); ++index) {
    out << child_indent << "+- ";
    std::ostringstream child_line;
    append_layout_tree(child_line, node.children[index], rects, active_pane_id, "");
    std::string child_text = child_line.str();
    const auto first_newline = child_text.find('\n');
    if (first_newline == std::string::npos) {
      out << child_text << "\n";
      continue;
    }

    out << child_text.substr(0, first_newline + 1);
    std::string remaining = child_text.substr(first_newline + 1);
    std::size_t line_start = 0;
    while (line_start < remaining.size()) {
      const auto line_end = remaining.find('\n', line_start);
      out << child_indent << "   "
          << remaining.substr(
                 line_start,
                 line_end == std::string::npos ? std::string::npos : line_end - line_start)
          << "\n";
      if (line_end == std::string::npos) {
        break;
      }
      line_start = line_end + 1;
    }
  }
}

std::string render_dump_events_locked(DaemonState& state) {
  std::ostringstream out;
  out << "wmux dump-events\n";
  append_diagnostic_ring(
      out,
      "commands",
      diagnostic_events_snapshot(state.diagnostics, DiagnosticEventCategory::Command));
  append_diagnostic_ring(
      out,
      "keys",
      diagnostic_events_snapshot(state.diagnostics, DiagnosticEventCategory::Key));
  append_diagnostic_ring(
      out,
      "process lifecycle",
      diagnostic_events_snapshot(state.diagnostics, DiagnosticEventCategory::Process));
  append_diagnostic_ring(
      out,
      "resize/layout",
      diagnostic_events_snapshot(state.diagnostics, DiagnosticEventCategory::ResizeLayout));
  append_diagnostic_ring(
      out,
      "errors",
      diagnostic_events_snapshot(state.diagnostics, DiagnosticEventCategory::Error));
  return out.str();
}

std::string render_dump_state(DaemonState& state) {
  std::lock_guard lock(state.mutex);
  const auto sessions = state.sessions.list_sessions();
  const auto resources = platform_services().info().current_process_resources();
  std::ostringstream out;
  out << "wmux dump-state\n";
  out << "uptime seconds: "
      << std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::steady_clock::now() - state.started_at)
             .count()
      << "\n";
  out << "sessions: " << sessions.size() << "\n";
  out << "clients: " << state.attach_clients.size() << "\n";
  out << "attach workers: " << state.attach_workers.size() << "\n";
  out << "event queue depth: " << state.event_queue_depth.load(std::memory_order_relaxed)
      << "\n";
  out << "peak event queue depth: "
      << state.peak_event_queue_depth.load(std::memory_order_relaxed) << "\n";
  if (resources.memory_available) {
    out << "working set bytes: " << resources.working_set_bytes << "\n";
    out << "private bytes: " << resources.private_bytes << "\n";
  } else {
    out << "memory: unavailable\n";
  }
  if (resources.handle_count_available) {
    out << "handle count: " << resources.handle_count << "\n";
  } else {
    out << "handle count: unavailable\n";
  }

  out << "\nsessions\n";
  if (sessions.empty()) {
    out << "  none\n";
  }
  for (const auto& session : sessions) {
    out << "  session id=" << session.id << " name=" << quoted(session.name)
        << " active_window=" << session.active_window_id
        << " windows=" << session.windows.size() << "\n";
    const auto runtime_session = state.runtimes.find(session.id);
    for (const auto& window : session.windows) {
      out << "    window id=" << window.id << " index=" << window.index
          << " name=" << quoted(window.name)
          << " active_pane=" << window.active_pane_id
          << " panes=" << window.panes.size() << "\n";
      for (const auto& pane : window.panes) {
        out << "      pane id=" << pane.id;
        if (pane.id == window.active_pane_id) {
          out << " active";
        }
        if (runtime_session != state.runtimes.end()) {
          const auto runtime_window = runtime_session->second.windows.find(window.id);
          if (runtime_window != runtime_session->second.windows.end()) {
            const auto runtime_pane = runtime_window->second.panes.find(pane.id);
            if (runtime_pane != runtime_window->second.panes.end()) {
              out << " pty=" << runtime_pane->second.pty_columns << "x"
                  << runtime_pane->second.pty_rows;
              if (runtime_pane->second.shell) {
                out << " " << process_lifecycle_summary(runtime_pane->second.shell->lifecycle());
              } else {
                out << " shell=none";
              }
            }
          }
        }
        out << "\n";
      }
    }
  }

  out << "\nclients\n";
  if (state.attach_clients.empty()) {
    out << "  none\n";
  }
  for (const auto& [client_id, client] : state.attach_clients) {
    out << "  client id=" << client_id
        << " session=" << client.client.attached_session.value_or(0)
        << " window=" << client.client.active_window.value_or(0)
        << " pane=" << client.client.active_pane.value_or(0)
        << " size=" << client.client.size.columns << "x" << client.client.size.rows
        << " mode=" << client_mode_name(client.client.mode)
        << " mouse_drag=" << bool_text(client.client.mouse_drag_active)
        << " host=" << terminal_host_name(client.client.terminal_caps.host)
        << " truecolor=" << bool_text(client.client.terminal_caps.supports_truecolor)
        << " mouse=" << bool_text(client.client.terminal_caps.supports_sgr_mouse)
        << "\n";
  }

  out << "\nrecent events\n";
  out << "  commands: " << state.diagnostics.commands.size() << "\n";
  out << "  keys: " << state.diagnostics.keys.size() << "\n";
  out << "  process lifecycle: " << state.diagnostics.processes.size() << "\n";
  out << "  resize/layout: " << state.diagnostics.resize_layout.size() << "\n";
  out << "  errors: " << state.diagnostics.errors.size() << "\n";
  return out.str();
}

std::string render_dump_layout(DaemonState& state) {
  std::lock_guard lock(state.mutex);
  const auto sessions = state.sessions.list_sessions();
  std::unordered_map<SessionId, DaemonState::TerminalSize> session_sizes;
  for (const auto& [client_id, client] : state.attach_clients) {
    (void)client_id;
    if (!client.client.attached_session || client.client.size.columns == 0 ||
        client.client.size.rows == 0) {
      continue;
    }
    session_sizes.try_emplace(*client.client.attached_session, client.client.size);
  }

  std::ostringstream out;
  out << "wmux dump-layout\n";
  if (sessions.empty()) {
    out << "sessions: none\n";
    return out.str();
  }

  for (const auto& session : sessions) {
    out << "Session " << session.id << " name=" << quoted(session.name)
        << " active_window=" << session.active_window_id << "\n";
    const auto size = session_sizes.find(session.id);
    const int columns =
        size == session_sizes.end() ? 0 : static_cast<int>(size->second.columns);
    const int rows = size == session_sizes.end() ? 0 : static_cast<int>(size->second.rows);
    const int pane_rows = rows > 0 && state.config.values.status_bar_enabled ? rows - 1 : rows;
    for (const auto& window : session.windows) {
      out << "Window " << window.index << " id=" << window.id
          << " name=" << quoted(window.name)
          << " active_pane=" << window.active_pane_id;
      if (columns > 0 && pane_rows > 0) {
        out << " size=" << columns << "x" << pane_rows;
      } else {
        out << " size=unattached";
      }
      out << "\n";
      const auto rects = pane_rect_index(window, columns, pane_rows);
      append_layout_tree(out, window.pane_tree, rects, window.active_pane_id, "  ");
    }
  }
  return out.str();
}

std::optional<SessionSummary> session_summary_by_id_locked(
    const SessionManager& sessions,
    SessionId session_id) {
  const auto listed = sessions.list_sessions();
  const auto found = std::find_if(listed.begin(), listed.end(), [&](const auto& session) {
    return session.id == session_id;
  });
  if (found == listed.end()) {
    return std::nullopt;
  }

  return *found;
}

std::optional<ResolvedTarget> resolve_only_session_target(DaemonState& state, std::string& error) {
  std::lock_guard lock(state.mutex);
  if (state.sessions.session_count() == 0) {
    error = "wmux: no sessions\n";
    return std::nullopt;
  }

  const auto only_session = state.sessions.only_session_id();
  if (!only_session) {
    error = "wmux: target session required; use -t <session>\n";
    return std::nullopt;
  }

  const auto active_window = state.sessions.active_window_id(*only_session).value_or(0);
  const auto active_pane = state.sessions.active_pane_id(*only_session);
  return ResolvedTarget{
      *only_session,
      active_window,
      active_pane,
      std::nullopt};
}

std::optional<ResolvedTarget> resolve_ipc_target(
    DaemonState& state,
    const RuntimeCommand& command,
    const IpcCommandContext& context,
    std::string& error) {
  if (command.target.kind == TargetKind::Current && !context.client_id) {
    return resolve_only_session_target(state, error);
  }

  const auto resolved = resolve_target(state, command.target, context.client_id);
  if (!resolved.ok) {
    error = with_trailing_newline(resolved.error);
    return std::nullopt;
  }

  return resolved.target;
}

void log_ipc_command_result(
    const IpcCommandContext& context,
    const RuntimeCommand& command,
    CommandStatus status,
    std::string_view message,
    const std::optional<ResolvedTarget>& target,
    std::chrono::steady_clock::duration elapsed) {
  const auto elapsed_us =
      std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
  log_event(
      status == CommandStatus::Success || status == CommandStatus::NoOp
          ? LogLevel::Info
          : LogLevel::Warn,
      "daemon.command",
      "ipc_execute",
      {{"request_id", std::to_string(context.request_id)},
       {"client_id", context.client_id ? std::to_string(*context.client_id) : ""},
       {"session_id", target ? std::to_string(target->session_id) : ""},
       {"window_id", target ? std::to_string(target->window_id) : ""},
       {"pane_id", target && target->pane_id ? std::to_string(*target->pane_id) : ""},
       {"command", std::string{runtime_command_name(command.kind)}},
       {"target", std::string{target_kind_name(command.target.kind)}},
       {"result", std::string{command_status_name(status)}},
       {"message", std::string{message}},
       {"elapsed_us", std::to_string(elapsed_us)}});
}

std::string handle_new_session(
    const RuntimeCommand& command,
    DaemonState& state,
    const IpcCommandContext& context) {
  const auto started_at = std::chrono::steady_clock::now();
  const auto session_name = command.name.value_or("");
  if (session_name.empty()) {
    return make_response_json(false, session_error_message(SessionError::EmptyName, {}));
  }

  {
    std::lock_guard lock(state.mutex);
    if (state.sessions.session_count() >= state.config.values.limits.max_sessions) {
      const auto message =
          resource_limit_message("session", state.config.values.limits.max_sessions);
      log_ipc_command_result(
          context,
          command,
          CommandStatus::UserError,
          message,
          std::nullopt,
          std::chrono::steady_clock::now() - started_at);
      return make_response_json(false, message);
    }
    if (state.sessions.has_session(session_name)) {
      log_ipc_command_result(
          context,
          command,
          CommandStatus::UserError,
          "wmux: duplicate session name",
          std::nullopt,
          std::chrono::steady_clock::now() - started_at);
      return make_response_json(
          false, session_error_message(SessionError::DuplicateName, session_name));
    }
  }

  std::shared_ptr<PtyProcess> shell_process;
  auto shell = start_configured_shell(state);
  if (!shell.process) {
    record_diagnostic_event(
        state,
        DiagnosticEvent{
            0,
            {},
            DiagnosticEventCategory::Error,
            "error",
            "shell_spawn_failed",
            context.request_id,
            context.client_id.value_or(0),
            0,
            0,
            0,
            "shell spawn failed",
            {{"error", shell.error},
             {"shell_source", shell.options.source},
             {"shell_executable", shell.options.executable},
             {"cwd", shell.options.working_directory}}});
    return make_response_json(false, shell.error);
  }
  shell_process = std::move(shell.process);

  SessionOperationResult result;
  {
    std::lock_guard lock(state.mutex);
    result = state.sessions.create_session(session_name);
    if (result.ok && shell_process) {
      install_pane_runtime_shell_locked(
          state,
          result.id,
          result.window_id,
          result.pane_id,
          std::move(shell_process));
    }
  }

  if (!result.ok) {
    if (shell_process) {
      (void)shell_process->terminate();
    }
    log_ipc_command_result(
        context,
        command,
        CommandStatus::UserError,
        "wmux: create session failed",
        std::nullopt,
        std::chrono::steady_clock::now() - started_at);
    return make_response_json(false, session_error_message(result.error, session_name));
  }

  log_event(
      LogLevel::Info,
      "daemon.session",
      "create",
      {{"session_id", std::to_string(result.id)},
       {"window_id", std::to_string(result.window_id)},
       {"pane_id", std::to_string(result.pane_id)},
       {"session_name", session_name}});
  record_diagnostic_event(
      state,
      DiagnosticEvent{
          0,
          {},
          DiagnosticEventCategory::Process,
          "info",
          "session_created",
          context.request_id,
          context.client_id.value_or(0),
          result.id,
          result.window_id,
          result.pane_id,
          "session created",
          {{"session_name", session_name}}});
  log_ipc_command_result(
      context,
      command,
      CommandStatus::Success,
      "wmux: created session",
      ResolvedTarget{result.id, result.window_id, result.pane_id, std::nullopt},
      std::chrono::steady_clock::now() - started_at);

  std::ostringstream out;
  out << "wmux: created session " << quoted(session_name) << "\n";
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

std::string handle_rename_session(
    const RuntimeCommand& command,
    DaemonState& state,
    const IpcCommandContext& context) {
  const auto started_at = std::chrono::steady_clock::now();
  if (!command.name || command.name->empty()) {
    return make_response_json(false, session_error_message(SessionError::EmptyName, {}));
  }

  std::string error;
  const auto target = resolve_ipc_target(state, command, context, error);
  if (!target) {
    log_ipc_command_result(
        context,
        command,
        CommandStatus::UserError,
        error,
        std::nullopt,
        std::chrono::steady_clock::now() - started_at);
    return make_response_json(false, error);
  }

  std::lock_guard lock(state.mutex);
  const auto session = session_summary_by_id_locked(state.sessions, target->session_id);
  if (!session) {
    return make_response_json(false, session_error_message(SessionError::NotFound, {}));
  }

  const auto result = state.sessions.rename_session(session->name, *command.name);
  if (!result.ok) {
    const auto name = result.error == SessionError::DuplicateName ? *command.name : session->name;
    log_ipc_command_result(
        context,
        command,
        CommandStatus::UserError,
        "wmux: rename session failed",
        target,
        std::chrono::steady_clock::now() - started_at);
    return make_response_json(false, session_error_message(result.error, name));
  }

  for (auto& [attached_client_id, attached_client] : state.attach_clients) {
    (void)attached_client_id;
    if (attached_client.client.attached_session == target->session_id) {
      attached_client.session_name = *command.name;
      attached_client.client.render_state.dirty = true;
    }
  }

  std::ostringstream out;
  out << "wmux: renamed session " << quoted(session->name) << " to "
      << quoted(*command.name) << "\n";
  log_ipc_command_result(
      context,
      command,
      CommandStatus::Success,
      out.str(),
      target,
      std::chrono::steady_clock::now() - started_at);
  return make_response_json(true, out.str());
}

std::string handle_kill_session(
    const RuntimeCommand& command,
    DaemonState& state,
    const IpcCommandContext& context) {
  const auto started_at = std::chrono::steady_clock::now();
  std::string error;
  const auto target = resolve_ipc_target(state, command, context, error);
  if (!target) {
    log_ipc_command_result(
        context,
        command,
        CommandStatus::UserError,
        error,
        std::nullopt,
        std::chrono::steady_clock::now() - started_at);
    return make_response_json(false, error);
  }

  std::vector<std::shared_ptr<PtyProcess>> killed_shells;
  SessionId killed_session_id{0};
  std::string killed_session_name;
  {
    std::lock_guard lock(state.mutex);
    const auto session = session_summary_by_id_locked(state.sessions, target->session_id);
    if (!session) {
      return make_response_json(false, session_error_message(SessionError::NotFound, {}));
    }

    killed_session_name = session->name;
    const auto result = state.sessions.kill_session(session->name);
    if (!result.ok) {
      log_ipc_command_result(
          context,
          command,
          CommandStatus::UserError,
          "wmux: kill session failed",
          target,
          std::chrono::steady_clock::now() - started_at);
      return make_response_json(false, session_error_message(result.error, session->name));
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
  log_event(
      LogLevel::Info,
      "daemon.session",
      "kill",
      {{"session_id", std::to_string(killed_session_id)},
       {"session_name", killed_session_name},
       {"shells", std::to_string(killed_shells.size())}});
  record_diagnostic_event(
      state,
      DiagnosticEvent{
          0,
          {},
          DiagnosticEventCategory::Process,
          "info",
          "session_killed",
          context.request_id,
          context.client_id.value_or(0),
          killed_session_id,
          0,
          0,
          "session killed",
          {{"session_name", killed_session_name},
           {"shells", std::to_string(killed_shells.size())}}});

  std::ostringstream out;
  out << "wmux: killed session " << quoted(killed_session_name) << "\n";
  log_ipc_command_result(
      context,
      command,
      CommandStatus::Success,
      out.str(),
      target,
      std::chrono::steady_clock::now() - started_at);
  return make_response_json(true, out.str());
}

std::string handle_new_window(
    const RuntimeCommand& command,
    DaemonState& state,
    const IpcCommandContext& context) {
  const auto started_at = std::chrono::steady_clock::now();
  std::string window_name = command.name.value_or("");

  std::string error;
  const auto target = resolve_ipc_target(state, command, context, error);
  if (!target) {
    log_ipc_command_result(
        context,
        command,
        CommandStatus::UserError,
        error,
        std::nullopt,
        std::chrono::steady_clock::now() - started_at);
    return make_response_json(false, error);
  }
  const SessionId session_id = target->session_id;

  {
    std::lock_guard lock(state.mutex);
    const auto windows = state.sessions.list_windows(session_id);
    if (window_name.empty()) {
      window_name = next_window_name(windows);
    }
    if (windows.size() >= state.config.values.limits.max_windows_per_session) {
      const auto message = resource_limit_message(
          "window per session",
          state.config.values.limits.max_windows_per_session);
      log_ipc_command_result(
          context,
          command,
          CommandStatus::UserError,
          message,
          target,
          std::chrono::steady_clock::now() - started_at);
      return make_response_json(false, message);
    }
  }

  std::shared_ptr<PtyProcess> shell_process;
  auto shell = start_configured_shell(state);
  if (!shell.process) {
    record_diagnostic_event(
        state,
        DiagnosticEvent{
            0,
            {},
            DiagnosticEventCategory::Error,
            "error",
            "shell_spawn_failed",
            context.request_id,
            context.client_id.value_or(0),
            session_id,
            target->window_id,
            target->pane_id.value_or(0),
            "shell spawn failed",
            {{"error", shell.error},
             {"shell_source", shell.options.source},
             {"shell_executable", shell.options.executable},
             {"cwd", shell.options.working_directory}}});
    return make_response_json(false, shell.error);
  }
  shell_process = std::move(shell.process);

  WindowOperationResult result;
  {
    std::lock_guard lock(state.mutex);
    result = state.sessions.create_window(session_id, window_name);
    if (result.ok && shell_process) {
      install_pane_runtime_shell_locked(
          state,
          result.session_id,
          result.window_id,
          result.pane_id,
          std::move(shell_process));
      sync_attach_client_focus_locked(state, result.session_id);
    }
  }

  if (!result.ok) {
    if (shell_process) {
      (void)shell_process->terminate();
    }
    return make_response_json(false, window_error_message(result.error, window_name));
  }

  log_event(
      LogLevel::Info,
      "daemon.window",
      "create",
      {{"session_id", std::to_string(result.session_id)},
       {"window_id", std::to_string(result.window_id)},
       {"pane_id", std::to_string(result.pane_id)},
       {"window_name", window_name}});
  record_diagnostic_event(
      state,
      DiagnosticEvent{
          0,
          {},
          DiagnosticEventCategory::Process,
          "info",
          "window_created",
          context.request_id,
          context.client_id.value_or(0),
          result.session_id,
          result.window_id,
          result.pane_id,
          "window created",
          {{"window_name", window_name}}});

  std::ostringstream out;
  out << "wmux: created window " << quoted(window_name) << "\n";
  log_ipc_command_result(
      context,
      command,
      CommandStatus::Success,
      out.str(),
      ResolvedTarget{result.session_id, result.window_id, result.pane_id, context.client_id},
      std::chrono::steady_clock::now() - started_at);
  return make_response_json(true, out.str());
}

std::string handle_select_window(
    const RuntimeCommand& command,
    DaemonState& state,
    const IpcCommandContext& context) {
  const auto started_at = std::chrono::steady_clock::now();
  std::string error;
  const auto target = resolve_ipc_target(state, command, context, error);
  if (!target) {
    return make_response_json(false, error);
  }

  WindowOperationResult result;
  {
    std::lock_guard lock(state.mutex);
    result = state.sessions.select_window(target->session_id, target->window_id);
    if (result.ok) {
      sync_attach_client_focus_locked(state, result.session_id);
    }
  }
  if (!result.ok) {
    return make_response_json(false, window_error_message(result.error, {}));
  }

  std::ostringstream out;
  out << "wmux: selected window " << result.window_id << "\n";
  log_ipc_command_result(
      context,
      command,
      CommandStatus::Success,
      out.str(),
      ResolvedTarget{result.session_id, result.window_id, result.pane_id, context.client_id},
      std::chrono::steady_clock::now() - started_at);
  return make_response_json(true, out.str());
}

std::string handle_cycle_window(
    const RuntimeCommand& command,
    DaemonState& state,
    const IpcCommandContext& context) {
  const auto started_at = std::chrono::steady_clock::now();
  std::string error;
  const auto target = resolve_ipc_target(state, command, context, error);
  if (!target) {
    return make_response_json(false, error);
  }

  WindowOperationResult result;
  {
    std::lock_guard lock(state.mutex);
    if (command.kind == RuntimeCommandKind::NextWindow) {
      result = state.sessions.select_next_window(target->session_id);
    } else {
      result = state.sessions.select_previous_window(target->session_id);
    }
    if (result.ok) {
      sync_attach_client_focus_locked(state, result.session_id);
    }
  }
  if (!result.ok) {
    return make_response_json(false, window_error_message(result.error, {}));
  }

  std::ostringstream out;
  out << "wmux: selected window " << result.window_id << "\n";
  log_ipc_command_result(
      context,
      command,
      CommandStatus::Success,
      out.str(),
      ResolvedTarget{result.session_id, result.window_id, result.pane_id, context.client_id},
      std::chrono::steady_clock::now() - started_at);
  return make_response_json(true, out.str());
}

std::string handle_list_windows(
    const IpcRequest& request,
    DaemonState& state,
    const IpcCommandContext& context) {
  RuntimeCommand target_command;
  target_command.kind = RuntimeCommandKind::DisplayMessage;
  target_command.target = request.session_name.empty()
                              ? Target::current()
                              : Target::named(request.session_name);
  std::string error;
  const auto target = resolve_ipc_target(state, target_command, context, error);
  if (!target) {
    return make_response_json(false, error);
  }

  std::lock_guard lock(state.mutex);
  const auto active_window = state.sessions.active_window_id(target->session_id);
  const auto windows = state.sessions.list_windows(target->session_id);
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

std::vector<std::shared_ptr<PtyProcess>> take_window_shells_locked(
    DaemonState& state,
    SessionId session_id,
    WindowId window_id) {
  std::vector<std::shared_ptr<PtyProcess>> shells;
  const auto runtime = state.runtimes.find(session_id);
  if (runtime == state.runtimes.end()) {
    return shells;
  }

  const auto window = runtime->second.windows.find(window_id);
  if (window == runtime->second.windows.end()) {
    return shells;
  }

  shells.reserve(window->second.panes.size());
  for (auto& [pane_id, pane] : window->second.panes) {
    (void)pane_id;
    if (pane.shell) {
      shells.push_back(std::move(pane.shell));
    }
  }
  runtime->second.windows.erase(window);
  return shells;
}

void terminate_shells(std::vector<std::shared_ptr<PtyProcess>>& shells) {
  for (auto& shell : shells) {
    if (shell) {
      shell->terminate();
    }
  }
}

std::string handle_kill_window(
    const RuntimeCommand& command,
    DaemonState& state,
    const IpcCommandContext& context) {
  const auto started_at = std::chrono::steady_clock::now();
  std::string error;
  const auto target = resolve_ipc_target(state, command, context, error);
  if (!target) {
    return make_response_json(false, error);
  }

  WindowOperationResult result;
  std::vector<std::shared_ptr<PtyProcess>> removed_shells;
  {
    std::lock_guard lock(state.mutex);
    const auto selected = state.sessions.select_window(target->session_id, target->window_id);
    if (!selected.ok) {
      return make_response_json(false, window_error_message(selected.error, {}));
    }
    result = state.sessions.kill_active_window(target->session_id);
    if (!result.ok) {
      return make_response_json(false, window_error_message(result.error, {}));
    }
    removed_shells = take_window_shells_locked(state, result.session_id, result.removed_window_id);
    sync_attach_client_focus_locked(state, result.session_id);
  }

  terminate_shells(removed_shells);

  std::ostringstream out;
  out << "wmux: killed window " << result.removed_window_id << "\n";
  log_ipc_command_result(
      context,
      command,
      CommandStatus::Success,
      out.str(),
      ResolvedTarget{result.session_id, result.window_id, result.pane_id, context.client_id},
      std::chrono::steady_clock::now() - started_at);
  return make_response_json(true, out.str());
}

std::string handle_kill_pane(
    const RuntimeCommand& command,
    DaemonState& state,
    const IpcCommandContext& context) {
  const auto started_at = std::chrono::steady_clock::now();
  std::string error;
  const auto target = resolve_ipc_target(state, command, context, error);
  if (!target) {
    return make_response_json(false, error);
  }

  PaneOperationResult result;
  WindowOperationResult window_result;
  bool killed_window = false;
  bool killed_session = false;
  std::string killed_session_name;
  std::vector<std::shared_ptr<PtyProcess>> removed_shells;
  {
    std::lock_guard lock(state.mutex);
    const auto selected_window = state.sessions.select_window(target->session_id, target->window_id);
    if (!selected_window.ok) {
      return make_response_json(false, window_error_message(selected_window.error, {}));
    }
    if (target->pane_id) {
      const auto selected_pane = state.sessions.select_pane(target->session_id, *target->pane_id);
      if (!selected_pane.ok) {
        return make_response_json(false, pane_error_message(selected_pane.error));
      }
    }

    result = state.sessions.kill_active_pane(target->session_id);
    if (!result.ok) {
      if (result.error != PaneError::LastPane) {
        return make_response_json(false, pane_error_message(result.error));
      }

      const auto windows = state.sessions.list_windows(target->session_id);
      if (windows.size() <= 1) {
        for (const auto& session : state.sessions.list_sessions()) {
          if (session.id == target->session_id) {
            killed_session_name = session.name;
            break;
          }
        }
        if (killed_session_name.empty()) {
          return make_response_json(false, "wmux: target session not found\n");
        }
        const auto runtime = state.runtimes.find(target->session_id);
        if (runtime != state.runtimes.end()) {
          for (auto& [window_id, window] : runtime->second.windows) {
            (void)window_id;
            for (auto& [pane_id, pane] : window.panes) {
              (void)pane_id;
              if (pane.shell) {
                removed_shells.push_back(std::move(pane.shell));
              }
            }
          }
          state.runtimes.erase(runtime);
        }
        const auto killed = state.sessions.kill_session(killed_session_name);
        if (!killed.ok) {
          return make_response_json(false, session_error_message(killed.error, killed_session_name));
        }
        sync_attach_client_focus_locked(state, target->session_id);
        killed_session = true;
      } else {
        window_result = state.sessions.kill_active_window(target->session_id);
        if (!window_result.ok) {
          return make_response_json(false, window_error_message(window_result.error, {}));
        }
        removed_shells =
            take_window_shells_locked(state, window_result.session_id, window_result.removed_window_id);
        sync_attach_client_focus_locked(state, window_result.session_id);
        killed_window = true;
      }
    } else {
      const auto runtime = state.runtimes.find(result.session_id);
      if (runtime != state.runtimes.end()) {
        const auto window = runtime->second.windows.find(result.window_id);
        if (window != runtime->second.windows.end()) {
          const auto pane = window->second.panes.find(result.removed_pane_id);
          if (pane != window->second.panes.end()) {
            removed_shells.push_back(std::move(pane->second.shell));
            window->second.panes.erase(pane);
            mark_window_layout_changed_locked(state, result.session_id, result.window_id);
          }
        }
      }
      sync_attach_client_focus_locked(state, result.session_id);
    }
  }

  terminate_shells(removed_shells);

  std::ostringstream out;
  if (killed_session) {
    out << "wmux: killed session " << quoted(killed_session_name) << "\n";
  } else if (killed_window) {
    out << "wmux: killed window " << window_result.removed_window_id << "\n";
  } else {
    out << "wmux: killed pane " << result.removed_pane_id << "\n";
  }

  log_ipc_command_result(
      context,
      command,
      CommandStatus::Success,
      out.str(),
      std::nullopt,
      std::chrono::steady_clock::now() - started_at);
  return make_response_json(true, out.str());
}

std::pair<int, int> target_frame_size(
    DaemonState& state,
    SessionId session_id,
    bool status_bar_enabled) {
  std::lock_guard lock(state.mutex);
  for (const auto& [client_id, client] : state.attach_clients) {
    (void)client_id;
    if (client.client.attached_session == session_id &&
        client.client.size.columns > 0 &&
        client.client.size.rows > 0) {
      const int columns = static_cast<int>(client.client.size.columns);
      const int rows = static_cast<int>(client.client.size.rows);
      return {columns, std::max(1, rows - (status_bar_enabled && rows > 1 ? 1 : 0))};
    }
  }
  return {120, status_bar_enabled ? 29 : 30};
}

std::string handle_resize_pane(
    const RuntimeCommand& command,
    DaemonState& state,
    const IpcCommandContext& context) {
  const auto started_at = std::chrono::steady_clock::now();
  std::string error;
  const auto target = resolve_ipc_target(state, command, context, error);
  if (!target) {
    return make_response_json(false, error);
  }

  bool status_bar_enabled = true;
  {
    std::lock_guard lock(state.mutex);
    status_bar_enabled = state.config.values.status_bar_enabled;
  }
  const auto [columns, pane_rows] = target_frame_size(state, target->session_id, status_bar_enabled);

  PaneOperationResult result;
  {
    std::lock_guard lock(state.mutex);
    const auto selected_window = state.sessions.select_window(target->session_id, target->window_id);
    if (!selected_window.ok) {
      return make_response_json(false, window_error_message(selected_window.error, {}));
    }
    if (target->pane_id) {
      const auto selected_pane = state.sessions.select_pane(target->session_id, *target->pane_id);
      if (!selected_pane.ok) {
        return make_response_json(false, pane_error_message(selected_pane.error));
      }
    }
    result = state.sessions.resize_active_pane(
        target->session_id,
        pane_direction_from_resize_direction(command.resize_direction),
        command.amount == 0 ? 1 : command.amount,
        columns,
        pane_rows);
    if (result.ok) {
      if (result.changed) {
        mark_window_layout_changed_locked(state, result.session_id, result.window_id);
      }
      sync_attach_client_focus_locked(state, result.session_id);
    }
  }
  if (!result.ok) {
    return make_response_json(false, pane_error_message(result.error));
  }

  std::ostringstream out;
  out << "wmux: pane resized\n";
  log_ipc_command_result(
      context,
      command,
      CommandStatus::Success,
      out.str(),
      ResolvedTarget{result.session_id, result.window_id, result.pane_id, context.client_id},
      std::chrono::steady_clock::now() - started_at);
  return make_response_json(true, out.str());
}

std::string handle_spread_panes(
    const RuntimeCommand& command,
    DaemonState& state,
    const IpcCommandContext& context) {
  const auto started_at = std::chrono::steady_clock::now();
  std::string error;
  const auto target = resolve_ipc_target(state, command, context, error);
  if (!target) {
    return make_response_json(false, error);
  }

  PaneOperationResult result;
  {
    std::lock_guard lock(state.mutex);
    const auto selected_window = state.sessions.select_window(target->session_id, target->window_id);
    if (!selected_window.ok) {
      return make_response_json(false, window_error_message(selected_window.error, {}));
    }
    result = state.sessions.equalize_active_window_panes(target->session_id);
    if (result.ok) {
      if (result.changed) {
        mark_window_layout_changed_locked(state, result.session_id, result.window_id);
      }
      sync_attach_client_focus_locked(state, result.session_id);
    }
  }
  if (!result.ok) {
    return make_response_json(false, pane_error_message(result.error));
  }

  std::ostringstream out;
  out << "wmux: panes spread evenly\n";
  log_ipc_command_result(
      context,
      command,
      CommandStatus::Success,
      out.str(),
      ResolvedTarget{result.session_id, result.window_id, result.pane_id, context.client_id},
      std::chrono::steady_clock::now() - started_at);
  return make_response_json(true, out.str());
}

std::string handle_rename_window(
    const RuntimeCommand& command,
    DaemonState& state,
    const IpcCommandContext& context) {
  const auto started_at = std::chrono::steady_clock::now();
  const auto window_name = command.name.value_or("");
  if (window_name.empty()) {
    return make_response_json(false, window_error_message(WindowError::EmptyName, {}));
  }

  std::string error;
  const auto target = resolve_ipc_target(state, command, context, error);
  if (!target) {
    return make_response_json(false, error);
  }

  std::lock_guard lock(state.mutex);
  const auto result = state.sessions.rename_active_window(target->session_id, window_name);
  if (!result.ok) {
    return make_response_json(false, window_error_message(result.error, window_name));
  }

  std::ostringstream out;
  out << "wmux: renamed active window to " << quoted(window_name) << "\n";
  log_ipc_command_result(
      context,
      command,
      CommandStatus::Success,
      out.str(),
      target,
      std::chrono::steady_clock::now() - started_at);
  return make_response_json(true, out.str());
}

std::string handle_split_window(
    const RuntimeCommand& command,
    DaemonState& state,
    const IpcCommandContext& context) {
  const auto started_at = std::chrono::steady_clock::now();
  std::string error;
  const auto target = resolve_ipc_target(state, command, context, error);
  if (!target) {
    return make_response_json(false, error);
  }
  const SessionId session_id = target->session_id;

  {
    std::lock_guard lock(state.mutex);
    const auto window = state.sessions.active_window_summary(session_id);
    if (window && window->panes.size() >= state.config.values.limits.max_panes_per_window) {
      const auto message = resource_limit_message(
          "pane per window",
          state.config.values.limits.max_panes_per_window);
      log_ipc_command_result(
          context,
          command,
          CommandStatus::UserError,
          message,
          target,
          std::chrono::steady_clock::now() - started_at);
      return make_response_json(false, message);
    }
  }

  std::shared_ptr<PtyProcess> shell_process;
  auto shell = start_configured_shell(state);
  if (!shell.process) {
    record_diagnostic_event(
        state,
        DiagnosticEvent{
            0,
            {},
            DiagnosticEventCategory::Error,
            "error",
            "shell_spawn_failed",
            context.request_id,
            context.client_id.value_or(0),
            session_id,
            target->window_id,
            target->pane_id.value_or(0),
            "shell spawn failed",
            {{"error", shell.error},
             {"shell_source", shell.options.source},
             {"shell_executable", shell.options.executable},
             {"cwd", shell.options.working_directory}}});
    return make_response_json(false, shell.error);
  }
  shell_process = std::move(shell.process);

  PaneOperationResult result;
  {
    std::lock_guard lock(state.mutex);
    result = state.sessions.split_active_pane(session_id, command.axis);
    if (result.ok && shell_process) {
      install_pane_runtime_shell_locked(
          state,
          result.session_id,
          result.window_id,
          result.pane_id,
          std::move(shell_process));
      mark_window_layout_changed_locked(state, result.session_id, result.window_id);
      sync_attach_client_focus_locked(state, result.session_id);
    }
  }

  if (!result.ok) {
    if (shell_process) {
      (void)shell_process->terminate();
    }
    return make_response_json(false, pane_error_message(result.error));
  }

  log_event(
      LogLevel::Info,
      "daemon.pane",
      "split",
      {{"session_id", std::to_string(result.session_id)},
       {"window_id", std::to_string(result.window_id)},
       {"pane_id", std::to_string(result.pane_id)},
       {"direction", command.axis == SplitDirection::Horizontal ? "horizontal" : "vertical"}});
  record_diagnostic_event(
      state,
      DiagnosticEvent{
          0,
          {},
          DiagnosticEventCategory::ResizeLayout,
          "info",
          "pane_split",
          context.request_id,
          context.client_id.value_or(0),
          result.session_id,
          result.window_id,
          result.pane_id,
          "pane split",
          {{"direction",
            command.axis == SplitDirection::Horizontal ? "horizontal" : "vertical"}}});

  std::ostringstream out;
  out << "wmux: split active pane "
      << (command.axis == SplitDirection::Horizontal ? "horizontally" : "vertically") << "\n";
  log_ipc_command_result(
      context,
      command,
      CommandStatus::Success,
      out.str(),
      ResolvedTarget{result.session_id, result.window_id, result.pane_id, context.client_id},
      std::chrono::steady_clock::now() - started_at);
  return make_response_json(true, out.str());
}

std::string handle_set_option(
    const RuntimeCommand& command,
    DaemonState& state,
    const IpcCommandContext& context) {
  const auto started_at = std::chrono::steady_clock::now();
  if (command.option_scope != OptionScope::Global) {
    return make_response_json(false, "wmux: unsupported option scope\n");
  }

  Config next_config;
  {
    std::lock_guard lock(state.mutex);
    next_config = state.config.values;
  }

  if (auto error = apply_global_config_option(next_config, command.key, command.value)) {
    return make_response_json(false, "wmux: " + error->message + "\n");
  }

  const auto log_max_bytes = next_config.limits.max_log_file_bytes;
  {
    std::lock_guard lock(state.mutex);
    state.config.values = std::move(next_config);
    state.mouse_enabled = state.config.values.mouse_enabled;
  }
  configure_logging(log_max_bytes);

  std::ostringstream out;
  out << "wmux: set " << command.key << " " << command.value << "\n";
  log_ipc_command_result(
      context,
      command,
      CommandStatus::Success,
      out.str(),
      std::nullopt,
      std::chrono::steady_clock::now() - started_at);
  return make_response_json(true, out.str());
}

std::string handle_bind_key(
    const RuntimeCommand& command,
    DaemonState& state,
    const IpcCommandContext& context) {
  const auto started_at = std::chrono::steady_clock::now();
  if (command.option_scope != OptionScope::Global) {
    return make_response_json(false, "wmux: unsupported option scope\n");
  }

  Config next_config;
  {
    std::lock_guard lock(state.mutex);
    next_config = state.config.values;
  }

  if (auto error = apply_key_binding_config(next_config, command.key, command.value)) {
    return make_response_json(false, "wmux: " + error->message + "\n");
  }

  {
    std::lock_guard lock(state.mutex);
    state.config.values = std::move(next_config);
  }

  std::ostringstream out;
  out << "wmux: bound " << command.key << " to " << command.value << "\n";
  log_ipc_command_result(
      context,
      command,
      CommandStatus::Success,
      out.str(),
      std::nullopt,
      std::chrono::steady_clock::now() - started_at);
  return make_response_json(true, out.str());
}

std::string handle_unbind_key(
    const RuntimeCommand& command,
    DaemonState& state,
    const IpcCommandContext& context) {
  const auto started_at = std::chrono::steady_clock::now();
  if (command.option_scope != OptionScope::Global) {
    return make_response_json(false, "wmux: unsupported option scope\n");
  }

  Config next_config;
  {
    std::lock_guard lock(state.mutex);
    next_config = state.config.values;
  }

  if (auto error = apply_key_unbinding_config(next_config, command.key)) {
    return make_response_json(false, "wmux: " + error->message + "\n");
  }

  {
    std::lock_guard lock(state.mutex);
    state.config.values = std::move(next_config);
  }

  std::ostringstream out;
  out << "wmux: unbound " << command.key << "\n";
  log_ipc_command_result(
      context,
      command,
      CommandStatus::Success,
      out.str(),
      std::nullopt,
      std::chrono::steady_clock::now() - started_at);
  return make_response_json(true, out.str());
}

std::string handle_runtime_ipc_command(
    const RuntimeCommand& command,
    DaemonState& state,
    const IpcCommandContext& context) {
  switch (command.kind) {
    case RuntimeCommandKind::NewSession:
      return handle_new_session(command, state, context);
    case RuntimeCommandKind::RenameSession:
      return handle_rename_session(command, state, context);
    case RuntimeCommandKind::KillSession:
      return handle_kill_session(command, state, context);
    case RuntimeCommandKind::NewWindow:
      return handle_new_window(command, state, context);
    case RuntimeCommandKind::RenameWindow:
      return handle_rename_window(command, state, context);
    case RuntimeCommandKind::SelectWindow:
      return handle_select_window(command, state, context);
    case RuntimeCommandKind::NextWindow:
    case RuntimeCommandKind::PreviousWindow:
      return handle_cycle_window(command, state, context);
    case RuntimeCommandKind::KillWindow:
      return handle_kill_window(command, state, context);
    case RuntimeCommandKind::KillPane:
      return handle_kill_pane(command, state, context);
    case RuntimeCommandKind::SplitPane:
      return handle_split_window(command, state, context);
    case RuntimeCommandKind::ResizePane:
      return handle_resize_pane(command, state, context);
    case RuntimeCommandKind::SpreadPanesEvenly:
      return handle_spread_panes(command, state, context);
    case RuntimeCommandKind::SetOption:
      return handle_set_option(command, state, context);
    case RuntimeCommandKind::BindKey:
      return handle_bind_key(command, state, context);
    case RuntimeCommandKind::UnbindKey:
      return handle_unbind_key(command, state, context);
    default:
      break;
  }

  return make_response_json(false, "wmux: command is not supported by daemon IPC\n");
}

bool is_runtime_ipc_request(std::string_view type) {
  return type == "NewSession" ||
         type == "RenameSession" ||
         type == "KillSession" ||
         type == "NewWindow" ||
         type == "RenameWindow" ||
         type == "SelectWindow" ||
         type == "NextWindow" ||
         type == "PreviousWindow" ||
         type == "KillWindow" ||
         type == "KillPane" ||
         type == "SplitWindow" ||
         type == "ResizePane" ||
         type == "SelectLayout" ||
         type == "SetOption" ||
         type == "BindKey" ||
         type == "UnbindKey";
}

std::string handle_session_request(
    const IpcRequest& request,
    DaemonState& state,
    const IpcCommandContext& context) {
  if (request.type == "DefaultSession") {
    return make_response_json(true, "wmux: interactive session startup is not implemented yet\n");
  }

  if (request.type == "ListSessions") {
    return handle_list_sessions(state);
  }

  if (request.type == "AttachStart" || request.type == "AttachSession") {
    return make_response_json(false, "wmux: attach requires the streaming client transport\n");
  }

  if (request.type == "ListWindows") {
    return handle_list_windows(request, state, context);
  }

  if (is_runtime_ipc_request(request.type)) {
    std::string error;
    const auto command = runtime_command_from_ipc_request(request, error);
    if (!command) {
      return make_response_json(false, with_trailing_newline(error));
    }
    return handle_runtime_ipc_command(*command, state, context);
  }

  return {};
}

}  // namespace

DaemonCommandResult handle_request(
    const IpcRequest& request,
    DaemonState& state,
    RequestId request_id,
    std::optional<ClientId> client_id) {
  assert_daemon_state_mutation_allowed("handle_request");
  const IpcCommandContext context{request_id, client_id};
  record_control_request(state, request, context);

  if (request.type == "Ping") {
    return command_result(make_response_json(true, "wmux: daemon is running\n"));
  }

  if (request.type == "ServerStatus") {
    const auto stats = daemon_stats(state);
    const auto resources = platform_services().info().current_process_resources();
    const auto shell =
        platform_services().pty().resolve_shell_command(stats.config.values.default_shell);
    std::ostringstream out;
    out << "wmux: daemon is running\n";
    out << "uptime seconds: " << stats.uptime_seconds << "\n";
    out << "sessions: " << stats.session_count << "\n";
    out << "attach clients: " << stats.attach_client_count << "\n";
    out << "runtime sessions: " << stats.runtime_session_count << "\n";
    out << "runtime windows: " << stats.runtime_window_count << "\n";
    out << "runtime panes: " << stats.runtime_pane_count << "\n";
    out << "live shells: " << stats.live_shell_count << "\n";
    out << "terminating shells: " << stats.terminating_shell_count << "\n";
    out << "job-object shells: " << stats.job_object_shell_count << "\n";
    out << "degraded-cleanup shells: " << stats.degraded_cleanup_shell_count << "\n";
    out << "attach workers: " << stats.attach_worker_count << "\n";
    out << "event queue depth: " << stats.event_queue_depth << "\n";
    out << "peak event queue depth: " << stats.peak_event_queue_depth << "\n";
    out << "render frames: " << stats.render_frames_written << "\n";
    out << "render full frames: " << stats.render_full_frames_written << "\n";
    out << "render partial frames: " << stats.render_partial_frames_written << "\n";
    out << "render skipped frames: " << stats.render_skipped_frames << "\n";
    out << "render coalesced output events: " << stats.render_coalesced_output_events << "\n";
    out << "render dirty panes: " << stats.render_dirty_panes << "\n";
    out << "render bytes: " << stats.render_bytes_written << "\n";
    out << "render pending pane bytes: " << stats.render_pending_pane_output_bytes << "\n";
    out << "render peak pending pane bytes: " << stats.render_peak_pending_pane_output_bytes
        << "\n";
    out << "render pending client bytes: " << stats.render_pending_client_output_bytes << "\n";
    out << "render peak pending client bytes: " << stats.render_peak_pending_client_output_bytes
        << "\n";
    out << "render total frame micros: " << stats.render_frame_duration_us << "\n";
    out << "render max frame micros: " << stats.render_max_frame_duration_us << "\n";
    out << "render geometry micros: " << stats.render_geometry_duration_us << "\n";
    out << "render max geometry micros: " << stats.render_max_geometry_duration_us << "\n";
    out << "render snapshot micros: " << stats.render_snapshot_duration_us << "\n";
    out << "render max snapshot micros: " << stats.render_max_snapshot_duration_us << "\n";
    out << "render state micros: " << stats.render_state_duration_us << "\n";
    out << "render max state micros: " << stats.render_max_state_duration_us << "\n";
    out << "render diff micros: " << stats.render_diff_duration_us << "\n";
    out << "render max diff micros: " << stats.render_max_diff_duration_us << "\n";
    out << "render queue micros: " << stats.render_queue_duration_us << "\n";
    out << "render max queue micros: " << stats.render_max_queue_duration_us << "\n";
    out << "client write micros: " << stats.client_write_duration_us << "\n";
    out << "client max write micros: " << stats.client_max_write_duration_us << "\n";
    out << "render slow clients: " << stats.render_slow_clients << "\n";
    out << "render write failures: " << stats.render_write_failures << "\n";
    out << "render queue pending client bytes: " << stats.render_pending_client_output_bytes
        << "\n";
    out << "render queue peak pending client bytes: "
        << stats.render_peak_pending_client_output_bytes << "\n";
    out << "client resize events: " << stats.client_resize_events << "\n";
    out << "client resize noops: " << stats.client_resize_noops << "\n";
    out << "pty resize requests: " << stats.pty_resize_requests << "\n";
    out << "pty resize applied: " << stats.pty_resize_applied << "\n";
    out << "pty resize skipped: " << stats.pty_resize_skipped << "\n";
    out << "pty resize failures: " << stats.pty_resize_failures << "\n";
    out << "pty output read chunks: " << stats.pty_output_read_chunks << "\n";
    out << "pty output read bytes: " << stats.pty_output_read_bytes << "\n";
    out << "pty output feed micros: " << stats.pty_output_feed_duration_us << "\n";
    out << "pty output max feed micros: " << stats.pty_output_max_feed_duration_us << "\n";
    out << "pty output lock wait micros: " << stats.pty_output_lock_wait_duration_us << "\n";
    out << "pty output max lock wait micros: "
        << stats.pty_output_max_lock_wait_duration_us << "\n";
    out << "pty output grid feed micros: " << stats.pty_output_grid_feed_duration_us << "\n";
    out << "pty output max grid feed micros: "
        << stats.pty_output_max_grid_feed_duration_us << "\n";
    out << "pty output buffer micros: " << stats.pty_output_buffer_duration_us << "\n";
    out << "pty output max buffer micros: " << stats.pty_output_max_buffer_duration_us << "\n";
    out << "paste buffer id: " << stats.paste_buffer_id << "\n";
    out << "paste buffer bytes: " << stats.paste_buffer_bytes << "\n";
    out << "paste buffer original bytes: " << stats.paste_buffer_original_bytes << "\n";
    out << "paste buffer source: " << paste_buffer_source_name(stats.paste_buffer_source) << "\n";
    out << "paste buffer truncated: " << (stats.paste_buffer_truncated ? "yes" : "no") << "\n";
    if (resources.memory_available) {
      out << "memory working set bytes: " << resources.working_set_bytes << "\n";
      out << "memory private bytes: " << resources.private_bytes << "\n";
    } else {
      out << "memory estimate: unavailable\n";
    }
    if (resources.handle_count_available) {
      out << "handle count: " << resources.handle_count << "\n";
    } else {
      out << "handle count: unavailable\n";
    }
    out << "recent errors: " << stats.recent_error_count << "\n";
    out << "limit max sessions: " << stats.config.values.limits.max_sessions << "\n";
    out << "limit max windows per session: "
        << stats.config.values.limits.max_windows_per_session << "\n";
    out << "limit max panes per window: "
        << stats.config.values.limits.max_panes_per_window << "\n";
    out << "limit pane raw bytes: "
        << stats.config.values.limits.max_pane_raw_output_bytes << "\n";
    out << "limit pane scrollback lines: "
        << stats.config.values.limits.max_pane_scrollback_lines << "\n";
    out << "limit paste bytes: " << stats.config.values.limits.max_paste_buffer_bytes << "\n";
    out << "limit paste chunk bytes: " << kMaxPasteWriteChunkBytes << "\n";
    out << "limit ipc frame bytes: "
        << stats.config.values.limits.max_ipc_frame_payload_bytes << "\n";
    out << "limit ipc frame hard cap: " << kMaxIpcFramePayloadBytes << "\n";
    out << "limit attach input bytes: " << kMaxAttachInputPayloadBytes << "\n";
    out << "limit attach render bytes: "
        << stats.config.values.limits.max_attach_render_frame_bytes << "\n";
    out << "limit attach pending output bytes: "
        << stats.config.values.limits.max_client_output_queue_bytes << "\n";
    out << "limit attach pending output frames: "
        << stats.config.values.limits.max_client_output_queue_frames << "\n";
    out << "limit log file bytes: " << stats.config.values.limits.max_log_file_bytes << "\n";
    out << "mouse: " << (stats.mouse_enabled ? "on" : "off") << "\n";
    out << "daemon log: " << wmux::log_file_path(LogRole::Daemon).string() << "\n";
    out << "client log: " << wmux::log_file_path(LogRole::Client).string() << "\n";
    out << "config: " << stats.config.path.string()
        << (stats.config.file_exists ? " (loaded)" : " (not found)") << "\n";
    out << "config prefix: " << stats.config.values.prefix << "\n";
    out << "config default shell: "
        << (stats.config.values.default_shell.empty() ? "(auto)" : stats.config.values.default_shell)
        << "\n";
    out << "effective shell: " << shell.command_line << "\n";
    out << "effective shell source: " << shell.source << "\n";
    out << "effective shell executable: " << shell.executable << "\n";
    out << "effective shell cwd: " << shell.working_directory << "\n";
    out << "config status: " << (stats.config.values.status_bar_enabled ? "on" : "off") << "\n";
    out << "config ui inherit terminal theme: "
        << (stats.config.values.ui.inherit_terminal_theme ? "on" : "off") << "\n";
    out << "config ui tmux style: "
        << (stats.config.values.ui.tmux_style ? "on" : "off") << "\n";
    out << "config accent: " << stats.config.values.ui.accent_spec << "\n";
    out << "config border style: "
        << (stats.config.values.ui.smooth_borders ? "smooth" : "ascii") << "\n";
    if (!stats.config.errors.empty()) {
      out << "config errors: " << stats.config.errors.size() << "\n";
      for (const auto& error : stats.config.errors) {
        out << "  line " << error.line << ": " << error.message << "\n";
      }
    }
    return command_result(make_response_json(true, out.str()));
  }

  if (request.type == "DumpState") {
    return command_result(make_response_json(true, render_dump_state(state)));
  }

  if (request.type == "DumpLayout") {
    return command_result(make_response_json(true, render_dump_layout(state)));
  }

  if (request.type == "DumpEvents") {
    std::lock_guard lock(state.mutex);
    return command_result(make_response_json(true, render_dump_events_locked(state)));
  }

  if (request.type == "ServerStop") {
    if (!request.force) {
      std::lock_guard lock(state.mutex);
      if (state.sessions.session_count() > 0) {
        return command_result(make_response_json(
            false,
            "wmux: live sessions exist; use 'wmux server stop --force' to terminate them\n"));
      }
    }

    log_event(
        LogLevel::Info,
        "daemon.server",
        "stop",
        {{"force", request.force ? "true" : "false"}});
    if (request.force) {
      disconnect_all_attach_clients(state);
      auto shells = take_all_shells(state);
      for (auto& shell : shells) {
        if (shell) {
          shell->terminate();
        }
      }
      return command_result(
          make_response_json(true, "wmux: daemon stopping; live sessions terminated\n"),
          true);
    }

    return command_result(make_response_json(true, "wmux: daemon stopping\n"), true);
  }

  const auto session_response = handle_session_request(request, state, context);
  if (!session_response.empty()) {
    return command_result(session_response);
  }

  return command_result(make_response_json(false, "wmux: daemon does not understand request\n"));
}

}  // namespace wmux::daemon_internal
