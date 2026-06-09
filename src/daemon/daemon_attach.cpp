#include "daemon_attach.hpp"

#include "daemon_render.hpp"
#include "daemon_shell.hpp"
#include "wmux/command_mode.hpp"
#include "wmux/commands.hpp"
#include "wmux/ipc_transport.hpp"
#include "wmux/logging.hpp"
#include "wmux/paste_buffer.hpp"
#include "wmux/pty_process.hpp"
#include "wmux/resource_limits.hpp"
#include "wmux/windows_clipboard.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace wmux::daemon_internal {

#ifdef _WIN32
namespace {

constexpr auto kRequestReadTimeout = std::chrono::seconds{5};
constexpr auto kRequestReadPoll = std::chrono::milliseconds{10};
constexpr auto kOutputRedrawInterval = std::chrono::milliseconds{33};
constexpr auto kSlowClientWriteTimeout = std::chrono::seconds{5};

enum class ReplayKind {
  Full,
  Output,
};

struct AttachTarget {
  SessionId session_id{0};
  std::string session_name;
};

struct ActiveShell {
  WindowId window_id{0};
  PaneId pane_id{0};
  std::shared_ptr<PtyProcess> shell;
};

std::optional<AttachTarget> target_for_attach(
    DaemonState& state,
    const IpcRequest& request,
    std::string& error) {
  if (request.session_name.empty()) {
    error = session_error_message(SessionError::EmptyName, {});
    return {};
  }

  std::lock_guard lock(state.mutex);
  const auto session_id = state.sessions.session_id_for_name(request.session_name);
  if (!session_id) {
    error = session_error_message(SessionError::NotFound, request.session_name);
    return {};
  }

  return AttachTarget{*session_id, request.session_name};
}

std::optional<ActiveShell> active_shell_for_session(
    DaemonState& state,
    SessionId session_id,
    std::string& error) {
  std::lock_guard lock(state.mutex);
  const auto active_window = state.sessions.active_window_id(session_id);
  if (!active_window) {
    error = "wmux: session has no active window\n";
    return {};
  }
  const auto active_pane = state.sessions.active_pane_id(session_id);
  if (!active_pane) {
    error = "wmux: active window has no active pane\n";
    return {};
  }

  const auto runtime = state.runtimes.find(session_id);
  if (runtime == state.runtimes.end()) {
    error = "wmux: session has no runtime\n";
    return {};
  }

  const auto window = runtime->second.windows.find(*active_window);
  if (window == runtime->second.windows.end()) {
    error = "wmux: active window has no runtime\n";
    return {};
  }

  const auto pane = window->second.panes.find(*active_pane);
  if (pane == window->second.panes.end() || !pane->second.shell) {
    error = "wmux: active pane has no shell process\n";
    return {};
  }

  return ActiveShell{*active_window, *active_pane, pane->second.shell};
}

std::optional<ActiveWindowFrame> active_window_frame(
    DaemonState& state,
    SessionId session_id,
    short columns,
    short rows,
    bool status_bar_enabled,
    bool reserve_status_row,
    std::string& error) {
  ActiveWindowFrame frame;
  frame.columns = columns > 0 ? columns : 120;
  frame.rows = rows > 0 ? rows : 30;
  frame.status_bar_enabled = status_bar_enabled;

  std::lock_guard lock(state.mutex);
  for (const auto& session : state.sessions.list_sessions()) {
    if (session.id == session_id) {
      frame.session_name = session.name;
      break;
    }
  }
  const auto window = state.sessions.active_window_summary(session_id);
  if (!window) {
    error = "wmux: session has no active window\n";
    return {};
  }

  const auto runtime = state.runtimes.find(session_id);
  if (runtime == state.runtimes.end()) {
    error = "wmux: session has no runtime\n";
    return {};
  }

  const auto window_runtime = runtime->second.windows.find(window->id);
  if (window_runtime == runtime->second.windows.end()) {
    error = "wmux: active window has no runtime\n";
    return {};
  }

  const int pane_rows =
      std::max(1, frame.rows - (reserve_status_row && frame.rows > 1 ? 1 : 0));
  const auto rects = compute_pane_layout_rects(window->pane_tree, frame.columns, pane_rows);

  frame.window_id = window->id;
  frame.active_pane_id = window->active_pane_id;
  frame.window_name = window->name;
  frame.panes.reserve(rects.size());
  for (const auto& rect : rects) {
    const auto pane_runtime = window_runtime->second.panes.find(rect.pane_id);
    if (pane_runtime == window_runtime->second.panes.end() || !pane_runtime->second.shell) {
      continue;
    }

    frame.panes.push_back(RenderPane{
        rect,
        rect.pane_id == window->active_pane_id,
        pane_runtime->second.shell});
  }

  if (frame.panes.empty()) {
    error = "wmux: active window has no pane runtimes\n";
    return {};
  }

  return frame;
}

short bounded_attach_columns(std::uint16_t value) {
  if (value == 0) {
    return 0;
  }
  return static_cast<short>(std::min<std::uint16_t>(value, kMaxAttachTerminalColumns));
}

short bounded_attach_rows(std::uint16_t value) {
  if (value == 0) {
    return 0;
  }
  return static_cast<short>(std::min<std::uint16_t>(value, kMaxAttachTerminalRows));
}

void run_attach_connection(
    HANDLE pipe,
    DaemonEventLoop& events,
    ClientId client_id,
    SessionId session_id,
    std::string session_name,
    short columns,
    short rows,
    DaemonAttachSettings settings);

ClientId register_attach_client(
    DaemonState& state,
    SessionId session_id,
    std::string session_name,
    HANDLE pipe) {
  std::lock_guard lock(state.mutex);
  const ClientId client_id = state.next_client_id++;
  const auto registered_session_name = session_name;
  DaemonState::AttachClientRuntime client;
  client.session_id = session_id;
  client.session_name = std::move(session_name);
  client.pipe = pipe;
  state.attach_clients.emplace(client_id, std::move(client));
  log_event(
      LogLevel::Info,
      "daemon.attach",
      "register",
      {{"client_id", std::to_string(client_id)},
       {"session_id", std::to_string(session_id)},
       {"session_name", registered_session_name}});
  return client_id;
}

std::string attach_end_reason_name(AttachEndReason reason) {
  switch (reason) {
    case AttachEndReason::Detached:
      return "detached";
    case AttachEndReason::ClientDisconnected:
      return "client_disconnected";
    case AttachEndReason::ShellClosed:
      return "shell_closed";
    case AttachEndReason::OutputClosed:
      return "output_closed";
    case AttachEndReason::ProtocolError:
      return "protocol_error";
  }

  return "unknown";
}

bool is_attach_start_request(std::string_view type) {
  return type == "AttachStart" || type == "AttachSession";
}

void unregister_attach_client(
    DaemonState& state,
    ClientId client_id,
    AttachEndReason reason) {
  SessionId session_id{0};
  std::string session_name;
  {
    std::lock_guard lock(state.mutex);
    if (const auto client = state.attach_clients.find(client_id);
        client != state.attach_clients.end()) {
      session_id = client->second.session_id;
      session_name = client->second.session_name;
    }
    state.attach_clients.erase(client_id);
  }
  log_event(
      LogLevel::Info,
      "daemon.attach",
      "unregister",
      {{"client_id", std::to_string(client_id)},
       {"session_id", std::to_string(session_id)},
       {"session_name", session_name},
       {"reason", attach_end_reason_name(reason)}});
  log_event(
      LogLevel::Info,
      "daemon.attach",
      "lifecycle",
      {{"event", reason == AttachEndReason::Detached ? "Detach" : "AttachEnd"},
       {"client_id", std::to_string(client_id)},
       {"session_id", std::to_string(session_id)},
       {"session_name", session_name},
       {"reason", attach_end_reason_name(reason)}});
  state.attach_clients_changed.notify_all();
}

void start_attach_worker(
    DaemonEventLoop& events,
    HANDLE pipe,
    ClientId client_id,
    SessionId session_id,
    std::string session_name,
    short columns,
    short rows,
    DaemonAttachSettings settings) {
  auto done = std::make_shared<std::atomic_bool>(false);
  std::thread worker{[pipe,
                      &events,
                      client_id,
                      session_id,
                      session_name = std::move(session_name),
                      columns,
                      rows,
                      settings,
                      done] {
    run_attach_connection(
        pipe, events, client_id, session_id, session_name, columns, rows, settings);
    done->store(true, std::memory_order_release);
    events.notify_attach_workers_changed();
  }};

  events.call([&](DaemonState& state) {
    state.attach_workers.push_back(DaemonState::AttachWorkerRuntime{
        client_id,
        done,
        std::move(worker)});
  });

  log_event(
      LogLevel::Info,
      "daemon.attach",
      "worker_start",
      {{"client_id", std::to_string(client_id)},
       {"session_id", std::to_string(session_id)}});
}

void join_workers(
    std::vector<std::pair<ClientId, std::thread>>& workers,
    std::string_view event) {
  for (auto& [client_id, worker] : workers) {
    if (worker.joinable()) {
      worker.join();
      log_event(
          LogLevel::Info,
          "daemon.attach",
          event,
          {{"client_id", std::to_string(client_id)}});
    }
  }
}

std::vector<std::pair<ClientId, std::thread>> take_finished_attach_workers(
    DaemonState& state) {
  std::vector<std::pair<ClientId, std::thread>> workers;
  std::lock_guard lock(state.mutex);

  auto it = state.attach_workers.begin();
  while (it != state.attach_workers.end()) {
    if (it->done && it->done->load(std::memory_order_acquire)) {
      workers.emplace_back(it->client_id, std::move(it->thread));
      it = state.attach_workers.erase(it);
    } else {
      ++it;
    }
  }

  return workers;
}

std::vector<HANDLE> attach_client_pipes_for_session(DaemonState& state, SessionId session_id) {
  std::vector<HANDLE> pipes;
  std::lock_guard lock(state.mutex);
  for (const auto& [client_id, client] : state.attach_clients) {
    (void)client_id;
    if (client.session_id == session_id && client.pipe != nullptr) {
      pipes.push_back(client.pipe);
    }
  }
  return pipes;
}

std::vector<HANDLE> all_attach_client_pipes(DaemonState& state) {
  std::vector<HANDLE> pipes;
  std::lock_guard lock(state.mutex);
  pipes.reserve(state.attach_clients.size());
  for (const auto& [client_id, client] : state.attach_clients) {
    (void)client_id;
    if (client.pipe != nullptr) {
      pipes.push_back(client.pipe);
    }
  }
  return pipes;
}

void disconnect_attach_pipes(const std::vector<HANDLE>& pipes) {
  for (const auto pipe : pipes) {
    CancelIoEx(pipe, nullptr);
    DisconnectNamedPipe(pipe);
  }
}

enum class PipeReadResult {
  Ok,
  Closed,
  Failed,
};

PipeReadResult wait_for_available_bytes(
    HANDLE pipe,
    DWORD minimum_byte_count,
    const std::atomic_bool& stop_requested) {
  while (!stop_requested) {
    DWORD bytes_available = 0;
    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &bytes_available, nullptr)) {
      const DWORD error = GetLastError();
      return error == ERROR_BROKEN_PIPE || error == ERROR_OPERATION_ABORTED
                 ? PipeReadResult::Closed
                 : PipeReadResult::Failed;
    }

    if (bytes_available >= minimum_byte_count) {
      return PipeReadResult::Ok;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }

  return PipeReadResult::Closed;
}

PipeReadResult read_exact(HANDLE pipe, char* buffer, std::size_t byte_count) {
  std::size_t total_read = 0;
  while (total_read < byte_count) {
    DWORD bytes_read = 0;
    const auto bytes_remaining =
        static_cast<DWORD>(std::min<std::size_t>(byte_count - total_read, 64 * 1024));
    const BOOL ok = ReadFile(pipe, buffer + total_read, bytes_remaining, &bytes_read, nullptr);
    if (!ok) {
      const DWORD error = GetLastError();
      return error == ERROR_BROKEN_PIPE || error == ERROR_OPERATION_ABORTED
                 ? PipeReadResult::Closed
                 : PipeReadResult::Failed;
    }
    if (bytes_read == 0) {
      return PipeReadResult::Closed;
    }
    total_read += bytes_read;
  }

  return PipeReadResult::Ok;
}

std::string sanitize_status_text(std::string_view value) {
  constexpr std::size_t kMaxStatusBytes = 4096;
  std::string sanitized;
  sanitized.reserve(std::min(value.size(), kMaxStatusBytes));
  for (const char byte : value.substr(0, kMaxStatusBytes)) {
    sanitized.push_back(byte >= ' ' && byte <= '~' ? byte : ' ');
  }
  return sanitized;
}

struct AttachFrame {
  AttachFrameType type{AttachFrameType::Input};
  std::string payload;
};

struct MouseDragState {
  bool active{false};
  PaneSplitResizeTarget target;
};

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

bool create_interactive_window_named(
    DaemonState& state,
    SessionId session_id,
    std::string_view name,
    std::string& error) {
  auto shell = start_configured_shell(state);
  if (!shell.process) {
    error = shell.error;
    return false;
  }

  std::shared_ptr<PtyProcess> shell_process = std::move(shell.process);
  {
    std::lock_guard lock(state.mutex);
    const auto result = state.sessions.create_window(session_id, std::string{name});
    if (!result.ok) {
      error = window_error_message(result.error, name);
      shell_process->terminate();
      return false;
    }

    state.runtimes[result.session_id].windows[result.window_id].panes[result.pane_id].shell =
        std::move(shell_process);
    log_event(
        LogLevel::Info,
        "daemon.window",
        "interactive_create",
        {{"session_id", std::to_string(result.session_id)},
         {"window_id", std::to_string(result.window_id)},
         {"pane_id", std::to_string(result.pane_id)},
         {"window_name", std::string{name}}});
  }

  return true;
}

bool create_interactive_window(DaemonState& state, SessionId session_id, std::string& error) {
  std::string name;
  {
    std::lock_guard lock(state.mutex);
    name = next_window_name(state.sessions.list_windows(session_id));
  }

  return create_interactive_window_named(state, session_id, name, error);
}

bool select_interactive_window(
    DaemonState& state,
    SessionId session_id,
    std::string_view direction,
    std::string& error) {
  std::lock_guard lock(state.mutex);

  WindowOperationResult result;
  if (direction == "next-window") {
    result = state.sessions.select_next_window(session_id);
  } else if (direction == "previous-window") {
    result = state.sessions.select_previous_window(session_id);
  } else {
    error = "wmux: unknown attach command\n";
    return false;
  }

  if (!result.ok) {
    error = window_error_message(result.error, {});
    return false;
  }

  return true;
}

std::optional<SplitDirection> attach_split_direction(std::string_view command) {
  if (command == "split-horizontal") {
    return SplitDirection::Horizontal;
  }

  if (command == "split-vertical") {
    return SplitDirection::Vertical;
  }

  return std::nullopt;
}

std::optional<PaneDirection> attach_pane_direction(std::string_view command) {
  if (command == "select-pane-left") {
    return PaneDirection::Left;
  }

  if (command == "select-pane-right") {
    return PaneDirection::Right;
  }

  if (command == "select-pane-up") {
    return PaneDirection::Up;
  }

  if (command == "select-pane-down") {
    return PaneDirection::Down;
  }

  return std::nullopt;
}

bool split_interactive_pane(
    DaemonState& state,
    SessionId session_id,
    SplitDirection direction,
    std::string& error) {
  auto shell = start_configured_shell(state);
  if (!shell.process) {
    error = shell.error;
    return false;
  }

  std::shared_ptr<PtyProcess> shell_process = std::move(shell.process);
  {
    std::lock_guard lock(state.mutex);
    const auto result = state.sessions.split_active_pane(session_id, direction);
    if (!result.ok) {
      error = pane_error_message(result.error);
      shell_process->terminate();
      return false;
    }

    state.runtimes[result.session_id]
        .windows[result.window_id]
        .panes[result.pane_id]
        .shell = std::move(shell_process);
    log_event(
        LogLevel::Info,
        "daemon.pane",
        "interactive_split",
        {{"session_id", std::to_string(result.session_id)},
         {"window_id", std::to_string(result.window_id)},
         {"pane_id", std::to_string(result.pane_id)},
         {"direction", direction == SplitDirection::Horizontal ? "horizontal" : "vertical"}});
  }

  return true;
}

bool select_interactive_pane(
    DaemonState& state,
    SessionId session_id,
    PaneDirection direction,
    std::string& error) {
  std::lock_guard lock(state.mutex);
  const auto result = state.sessions.select_pane(session_id, direction);
  if (!result.ok) {
    error = pane_error_message(result.error);
    return false;
  }

  return true;
}

std::optional<bool> select_interactive_pane_at(
    DaemonState& state,
    SessionId session_id,
    std::uint16_t column,
    std::uint16_t row,
    short columns,
    short rows,
    std::string& error) {
  if (column == 0 || row == 0) {
    return false;
  }

  const int zero_based_column = static_cast<int>(column) - 1;
  const int zero_based_row = static_cast<int>(row) - 1;
  const int frame_columns = columns > 0 ? columns : 120;
  const int frame_rows = rows > 0 ? rows : 30;
  const int pane_rows = std::max(1, frame_rows - 1);
  if (zero_based_column < 0 || zero_based_column >= frame_columns ||
      zero_based_row < 0 || zero_based_row >= pane_rows) {
    return false;
  }

  std::lock_guard lock(state.mutex);
  const auto window = state.sessions.active_window_summary(session_id);
  if (!window) {
    error = "wmux: session has no active window\n";
    return std::nullopt;
  }

  const auto rects = compute_pane_layout_rects(window->pane_tree, frame_columns, pane_rows);
  const auto hit = std::find_if(rects.begin(), rects.end(), [&](const auto& rect) {
    return zero_based_column >= rect.left &&
           zero_based_column < rect.left + rect.width &&
           zero_based_row >= rect.top &&
           zero_based_row < rect.top + rect.height;
  });
  if (hit == rects.end()) {
    return false;
  }

  if (hit->pane_id == window->active_pane_id) {
    return false;
  }

  const auto result = state.sessions.select_pane(session_id, hit->pane_id);
  if (!result.ok) {
    error = pane_error_message(result.error);
    return std::nullopt;
  }

  return true;
}

std::optional<PaneSplitResizeTarget> resize_target_at(
    DaemonState& state,
    SessionId session_id,
    int column,
    int row,
    int columns,
    int rows,
    std::string& error) {
  std::lock_guard lock(state.mutex);
  const auto window = state.sessions.active_window_summary(session_id);
  if (!window) {
    error = "wmux: session has no active window\n";
    return std::nullopt;
  }

  return find_pane_split_resize_target(window->pane_tree, column, row, columns, rows);
}

std::optional<bool> resize_interactive_split(
    DaemonState& state,
    SessionId session_id,
    const PaneSplitResizeTarget& target,
    int column,
    int row,
    std::string& error) {
  std::lock_guard lock(state.mutex);
  const auto result = state.sessions.resize_active_window_split(session_id, target, column, row);
  if (!result.ok) {
    error = pane_error_message(result.error);
    return std::nullopt;
  }

  return true;
}

std::optional<bool> handle_interactive_mouse_event(
    DaemonState& state,
    SessionId session_id,
    const AttachMouseEventPayload& mouse,
    short columns,
    short rows,
    MouseDragState& drag,
    std::string& error) {
  const int frame_columns = columns > 0 ? columns : 120;
  const int frame_rows = rows > 0 ? rows : 30;
  const int pane_rows = std::max(1, frame_rows - 1);
  const int column = static_cast<int>(mouse.column) - 1;
  const int row = static_cast<int>(mouse.row) - 1;

  if (mouse.action == AttachMouseAction::Release) {
    drag.active = false;
    return false;
  }

  if (column < 0 || column >= frame_columns || row < 0 || row >= pane_rows) {
    return false;
  }

  if (mouse.action == AttachMouseAction::Drag) {
    if (!drag.active) {
      return false;
    }

    return resize_interactive_split(state, session_id, drag.target, column, row, error);
  }

  if (mouse.action != AttachMouseAction::Press || mouse.button != AttachMouseButton::Left) {
    return false;
  }

  const auto target =
      resize_target_at(state, session_id, column, row, frame_columns, pane_rows, error);
  if (!error.empty()) {
    return std::nullopt;
  }

  if (target) {
    drag.active = true;
    drag.target = *target;
    return resize_interactive_split(state, session_id, drag.target, column, row, error);
  }

  drag.active = false;
  return select_interactive_pane_at(
      state,
      session_id,
      mouse.column,
      mouse.row,
      columns,
      rows,
      error);
}

std::string status_from_error(std::string_view error) {
  std::string status{error};
  while (!status.empty() && (status.back() == '\n' || status.back() == '\r')) {
    status.pop_back();
  }
  return status;
}

bool kill_interactive_pane(DaemonState& state, SessionId session_id, std::string& status) {
  std::shared_ptr<PtyProcess> removed_shell;
  PaneOperationResult result;
  {
    std::lock_guard lock(state.mutex);
    result = state.sessions.kill_active_pane(session_id);
    if (!result.ok) {
      status = status_from_error(pane_error_message(result.error));
      return false;
    }

    const auto runtime = state.runtimes.find(result.session_id);
    if (runtime != state.runtimes.end()) {
      const auto window = runtime->second.windows.find(result.window_id);
      if (window != runtime->second.windows.end()) {
        const auto pane = window->second.panes.find(result.removed_pane_id);
        if (pane != window->second.panes.end()) {
          removed_shell = std::move(pane->second.shell);
          window->second.panes.erase(pane);
        }
      }
    }
  }

  if (removed_shell) {
    removed_shell->terminate();
  }

  log_event(
      LogLevel::Info,
      "daemon.pane",
      "interactive_kill",
      {{"session_id", std::to_string(result.session_id)},
       {"window_id", std::to_string(result.window_id)},
       {"pane_id", std::to_string(result.removed_pane_id)}});
  status = "wmux: killed pane " + std::to_string(result.removed_pane_id);
  return true;
}

bool kill_interactive_window(DaemonState& state, SessionId session_id, std::string& status) {
  std::vector<std::shared_ptr<PtyProcess>> removed_shells;
  WindowOperationResult result;
  {
    std::lock_guard lock(state.mutex);
    result = state.sessions.kill_active_window(session_id);
    if (!result.ok) {
      status = status_from_error(window_error_message(result.error, {}));
      return false;
    }

    const auto runtime = state.runtimes.find(result.session_id);
    if (runtime != state.runtimes.end()) {
      const auto window = runtime->second.windows.find(result.removed_window_id);
      if (window != runtime->second.windows.end()) {
        removed_shells.reserve(window->second.panes.size());
        for (auto& [pane_id, pane] : window->second.panes) {
          (void)pane_id;
          if (pane.shell) {
            removed_shells.push_back(std::move(pane.shell));
          }
        }
        runtime->second.windows.erase(window);
      }
    }
  }

  for (auto& shell : removed_shells) {
    if (shell) {
      shell->terminate();
    }
  }

  log_event(
      LogLevel::Info,
      "daemon.window",
      "interactive_kill",
      {{"session_id", std::to_string(result.session_id)},
       {"window_id", std::to_string(result.removed_window_id)},
       {"shells", std::to_string(removed_shells.size())}});
  status = "wmux: killed window " + std::to_string(result.removed_window_id);
  return true;
}

std::vector<std::string_view> command_args_as_views(const std::vector<std::string>& args) {
  std::vector<std::string_view> views;
  views.reserve(args.size());
  for (const auto& arg : args) {
    views.push_back(arg);
  }
  return views;
}

bool rename_attached_session(
    DaemonState& state,
    SessionId session_id,
    std::string_view current_session_name,
    std::string_view new_name,
    std::string& session_name,
    std::string& status) {
  std::lock_guard lock(state.mutex);
  const auto current_id = state.sessions.session_id_for_name(current_session_name);
  if (!current_id || *current_id != session_id) {
    status = "wmux: attached session was renamed externally";
    return false;
  }

  const auto result = state.sessions.rename_session(current_session_name, std::string{new_name});
  if (!result.ok) {
    const auto name = result.error == SessionError::DuplicateName ? new_name : current_session_name;
    status = status_from_error(session_error_message(result.error, name));
    return false;
  }

  session_name = std::string{new_name};
  status = "wmux: renamed session to '" + session_name + "'";
  return true;
}

bool rename_attached_window(
    DaemonState& state,
    SessionId session_id,
    std::string_view name,
    std::string& status) {
  std::lock_guard lock(state.mutex);
  const auto result = state.sessions.rename_active_window(session_id, std::string{name});
  if (!result.ok) {
    status = status_from_error(window_error_message(result.error, name));
    return false;
  }

  status = "wmux: renamed active window to '" + std::string{name} + "'";
  return true;
}

bool execute_command_mode_command(
    DaemonState& state,
    SessionId session_id,
    std::string& session_name,
    std::string_view command_text,
    std::string& status) {
  constexpr std::size_t kMaxCommandModeBytes = 4096;
  if (command_text.size() > kMaxCommandModeBytes) {
    status = "wmux: command is too long";
    return false;
  }

  const auto parsed_text = parse_command_prompt_text(command_text);
  if (!parsed_text.ok) {
    status = parsed_text.error;
    return false;
  }

  if (parsed_text.args.empty()) {
    status = "wmux: empty command";
    return false;
  }

  const auto command_name = std::string_view{parsed_text.args[0]};
  if (command_name == "rename-session") {
    if (parsed_text.args.size() != 2 || parsed_text.args[1].empty()) {
      status = "wmux: rename-session requires <new>";
      return false;
    }

    return rename_attached_session(
        state, session_id, session_name, parsed_text.args[1], session_name, status);
  }

  if (command_name == "kill-pane") {
    if (parsed_text.args.size() != 1) {
      status = "wmux: kill-pane does not accept arguments yet";
      return false;
    }

    return kill_interactive_pane(state, session_id, status);
  }

  if (command_name == "kill-window") {
    if (parsed_text.args.size() != 1) {
      status = "wmux: kill-window does not accept arguments yet";
      return false;
    }

    return kill_interactive_window(state, session_id, status);
  }

  const auto args = command_args_as_views(parsed_text.args);
  const auto command = parse_command_line(args);
  if (command.kind == CommandKind::Unknown) {
    status = "wmux: " + command.error;
    return false;
  }

  if (!command.session_name.empty()) {
    status = "wmux: command mode operates on the attached session; omit -t <session>";
    return false;
  }

  switch (command.kind) {
    case CommandKind::NewWindow: {
      std::string error;
      if (!create_interactive_window_named(state, session_id, command.window_name, error)) {
        status = status_from_error(error);
        return false;
      }
      status = "wmux: created window '" + command.window_name + "'";
      return true;
    }
    case CommandKind::RenameWindow:
      return rename_attached_window(state, session_id, command.window_name, status);
    case CommandKind::SplitWindow: {
      const auto direction =
          command.split_direction == "horizontal" ? SplitDirection::Horizontal
                                                  : SplitDirection::Vertical;
      std::string error;
      if (!split_interactive_pane(state, session_id, direction, error)) {
        status = status_from_error(error);
        return false;
      }
      status = "wmux: split active pane " + command.split_direction;
      return true;
    }
    default:
      status = "wmux: command is not supported in command mode yet";
      return false;
  }
}

bool execute_attach_command(
    DaemonState& state,
    SessionId session_id,
    std::string_view command,
    std::string& error) {
  if (command == "new-window") {
    return create_interactive_window(state, session_id, error);
  }

  if (command == "next-window" || command == "previous-window") {
    return select_interactive_window(state, session_id, command, error);
  }

  if (const auto direction = attach_split_direction(command)) {
    return split_interactive_pane(state, session_id, *direction, error);
  }

  if (const auto direction = attach_pane_direction(command)) {
    return select_interactive_pane(state, session_id, *direction, error);
  }

  error = "wmux: unknown attach command\n";
  return false;
}

bool read_attach_frame(
    HANDLE pipe,
    AttachFrame& frame,
    AttachEndReason& end_reason,
    const std::atomic_bool& stop_requested) {
  std::array<char, kAttachFrameHeaderSize> header{};
  const auto header_ready =
      wait_for_available_bytes(pipe, static_cast<DWORD>(header.size()), stop_requested);
  if (header_ready != PipeReadResult::Ok) {
    end_reason = header_ready == PipeReadResult::Closed ? AttachEndReason::ClientDisconnected
                                                        : AttachEndReason::ProtocolError;
    return false;
  }

  const auto header_read = read_exact(pipe, header.data(), header.size());
  if (header_read != PipeReadResult::Ok) {
    end_reason = header_read == PipeReadResult::Closed ? AttachEndReason::ClientDisconnected
                                                       : AttachEndReason::ProtocolError;
    return false;
  }

  const auto parsed = parse_attach_frame_header(std::string_view{header.data(), header.size()});
  if (!parsed) {
    end_reason = AttachEndReason::ProtocolError;
    log_event(LogLevel::Warn, "daemon.attach", "invalid_frame_header");
    return false;
  }

  frame.type = parsed->type;
  log_event(
      LogLevel::Debug,
      "daemon.attach",
      "frame",
      {{"type", std::string{attach_frame_type_name(frame.type)}},
       {"payload_bytes", std::to_string(parsed->payload_size)}});
  frame.payload.clear();
  if (parsed->payload_size == 0) {
    return true;
  }

  const auto payload_ready =
      wait_for_available_bytes(pipe, parsed->payload_size, stop_requested);
  if (payload_ready != PipeReadResult::Ok) {
    end_reason = payload_ready == PipeReadResult::Closed ? AttachEndReason::ClientDisconnected
                                                         : AttachEndReason::ProtocolError;
    return false;
  }

  frame.payload.resize(parsed->payload_size);
  const auto payload_read = read_exact(pipe, frame.payload.data(), frame.payload.size());
  if (payload_read != PipeReadResult::Ok) {
    end_reason = payload_read == PipeReadResult::Closed ? AttachEndReason::ClientDisconnected
                                                        : AttachEndReason::ProtocolError;
    return false;
  }

  return true;
}

void run_attach_connection(
    HANDLE pipe,
    DaemonEventLoop& events,
    ClientId client_id,
    SessionId session_id,
    std::string session_name,
    short columns,
    short rows,
    DaemonAttachSettings settings) {
  log_event(
      LogLevel::Info,
      "daemon.attach",
      "start",
      {{"client_id", std::to_string(client_id)},
       {"session_id", std::to_string(session_id)},
       {"session_name", session_name},
       {"columns", std::to_string(columns)},
       {"rows", std::to_string(rows)},
       {"mouse", settings.mouse_enabled ? "on" : "off"},
       {"prefix", settings.prefix},
       {"status", settings.status_bar_enabled ? "on" : "off"}});
  if (!write_all(pipe,
                 make_response_json(
                     true,
                     "",
                     settings.mouse_enabled,
                     settings.prefix,
                     settings.status_bar_enabled))) {
    close_attach_pipe(pipe);
    events.call([&](DaemonState& state) {
      unregister_attach_client(state, client_id, AttachEndReason::OutputClosed);
    });
    return;
  }

  std::atomic_bool stop_requested{false};
  std::atomic_bool output_closed{false};
  std::atomic<short> current_columns{columns > 0 ? columns : static_cast<short>(120)};
  std::atomic<short> current_rows{rows > 0 ? rows : static_cast<short>(30)};
  std::mutex pipe_write_mutex;
  std::mutex stream_mutex;
  std::unordered_map<PaneId, std::shared_ptr<PtyProcess>> current_shells;
  std::unordered_map<PaneId, std::uint64_t> next_sequences;
  PaneViewportStates viewport_states;
  CopyModeState copy_mode;
  std::string status_override;
  MouseDragState mouse_drag;

  const auto replay_active_window = [&](
                                      std::string& error,
                                      std::optional<AttachScrollAction> scroll,
                                      std::optional<AttachCopyModeAction> copy_action,
                                      ReplayKind replay_kind,
                                      const std::unordered_set<PaneId>& dirty_panes) {
    std::string replay;
    bool partial_frame = false;

    {
      std::lock_guard write_order_lock(pipe_write_mutex);
      {
        std::lock_guard stream_lock(stream_mutex);
        auto frame = events.call([&](DaemonState& state) {
          const bool reserve_status_row =
              settings.status_bar_enabled || !status_override.empty() || copy_mode.active ||
              (copy_action && *copy_action != AttachCopyModeAction::Exit);
          return active_window_frame(
              state,
              session_id,
              current_columns.load(std::memory_order_relaxed),
              current_rows.load(std::memory_order_relaxed),
              settings.status_bar_enabled,
              reserve_status_row,
              error);
        });
        if (!frame) {
          return false;
        }

        std::unordered_map<PaneId, PtyOutputSnapshot> snapshots;
        snapshots.reserve(frame->panes.size());
        for (const auto& pane : frame->panes) {
          const int width = std::max(1, body_width(pane.rect));
          const int height = std::max(1, body_height(pane.rect));
          const auto pty_columns = static_cast<short>(std::min(width, 32767));
          const auto pty_rows = static_cast<short>(std::min(height, 32767));
          if (!pane.shell->resize(pty_columns, pty_rows)) {
            log_event(
                LogLevel::Warn,
                "daemon.pane",
                "resize_failed",
                {{"session_id", std::to_string(session_id)},
                 {"window_id", std::to_string(frame->window_id)},
                 {"pane_id", std::to_string(pane.rect.pane_id)},
                 {"process_id", std::to_string(pane.shell->process_id())},
                 {"columns", std::to_string(pty_columns)},
                 {"rows", std::to_string(pty_rows)}});
          }
          snapshots.emplace(pane.rect.pane_id, pane.shell->output_snapshot());
        }

        update_viewport_states(*frame, snapshots, viewport_states, copy_mode);
        if (scroll) {
          (void)apply_active_viewport_scroll(*frame, snapshots, viewport_states, *scroll);
        }
        if (copy_action) {
          std::string copied_text;
          const auto applied = apply_copy_mode_action(
              *frame, snapshots, viewport_states, copy_mode, *copy_action, copied_text);
          if (*copy_action == AttachCopyModeAction::CopySelection) {
            if (applied) {
              {
                events.call([&](DaemonState& state) {
                  state.paste_buffer = bounded_paste_buffer_text(copied_text);
                });
              }
              const auto clipboard = write_windows_clipboard_text(copied_text);
              status_override = "wmux: copied " + std::to_string(copied_text.size()) + " bytes";
              if (copied_text.size() > kMaxPasteBufferBytes) {
                status_override += "; paste buffer truncated";
              }
              if (!clipboard.ok) {
                status_override += "; clipboard unavailable";
              }
            } else {
              status_override = "wmux: no copy selection";
            }
          }
        }
        clamp_copy_mode_cursor(copy_mode, *frame, snapshots);

        partial_frame = replay_kind == ReplayKind::Output && !dirty_panes.empty() &&
                        !scroll && !copy_action && status_override.empty();
        if (partial_frame) {
          replay = render_frame_update(
              *frame,
              snapshots,
              viewport_states,
              copy_mode,
              status_override,
              RenderFrameOptions{
                  false,
                  false,
                  false,
                  dirty_panes});
        } else {
          replay = render_frame(*frame, snapshots, viewport_states, copy_mode, status_override);
        }

        current_shells.clear();
        next_sequences.clear();
        current_shells.reserve(frame->panes.size());
        next_sequences.reserve(frame->panes.size());
        for (const auto& pane : frame->panes) {
          current_shells.emplace(pane.rect.pane_id, pane.shell);
          const auto snapshot = snapshots.find(pane.rect.pane_id);
          next_sequences.emplace(
              pane.rect.pane_id,
              snapshot == snapshots.end() ? 1 : snapshot->second.next_sequence);
        }
      }

      if (replay.empty()) {
        events.call([&](DaemonState& state) {
          state.render_metrics.skipped_frames.fetch_add(1, std::memory_order_relaxed);
        });
        return true;
      }

      if (replay.size() > kMaxAttachRenderFrameBytes) {
        log_event(
            LogLevel::Error,
            "daemon.attach",
            "render_frame_too_large",
            {{"client_id", std::to_string(client_id)},
             {"session_id", std::to_string(session_id)},
             {"bytes", std::to_string(replay.size())},
             {"limit", std::to_string(kMaxAttachRenderFrameBytes)}});
        output_closed = true;
        return false;
      }

      if (!write_all(pipe, replay)) {
        output_closed = true;
        events.call([&](DaemonState& state) {
          state.render_metrics.write_failures.fetch_add(1, std::memory_order_relaxed);
        });
        return false;
      }
    }

    events.call([&](DaemonState& state) {
      state.render_metrics.frames_written.fetch_add(1, std::memory_order_relaxed);
      state.render_metrics.bytes_written.fetch_add(replay.size(), std::memory_order_relaxed);
      if (partial_frame) {
        state.render_metrics.partial_frames_written.fetch_add(1, std::memory_order_relaxed);
        state.render_metrics.dirty_panes_rendered.fetch_add(
            dirty_panes.size(), std::memory_order_relaxed);
      } else {
        state.render_metrics.full_frames_written.fetch_add(1, std::memory_order_relaxed);
      }
    });
    return true;
  };

  const auto replay_full_window = [&](
                                      std::string& error,
                                      std::optional<AttachScrollAction> scroll,
                                      std::optional<AttachCopyModeAction> copy_action) {
    static const std::unordered_set<PaneId> kNoDirtyPanes;
    return replay_active_window(error, scroll, copy_action, ReplayKind::Full, kNoDirtyPanes);
  };

  {
    std::string error;
    if (!replay_full_window(error, std::nullopt, std::nullopt)) {
      if (!error.empty()) {
        write_all(pipe, error);
      }
      close_attach_pipe(pipe);
      events.call([&](DaemonState& state) {
        unregister_attach_client(state, client_id, AttachEndReason::OutputClosed);
      });
      return;
    }
  }

  std::thread output_thread{[&] {
    auto last_output_redraw = std::chrono::steady_clock::time_point{};
    while (!stop_requested) {
      std::vector<std::pair<PaneId, std::shared_ptr<PtyProcess>>> shells;
      std::unordered_map<PaneId, std::uint64_t> sequences;
      {
        std::lock_guard stream_lock(stream_mutex);
        shells.reserve(current_shells.size());
        for (const auto& [pane_id, shell] : current_shells) {
          shells.emplace_back(pane_id, shell);
        }
        sequences = next_sequences;
      }

      if (shells.empty()) {
        std::this_thread::sleep_for(std::chrono::milliseconds{25});
        continue;
      }

      std::unordered_set<PaneId> dirty_panes;
      for (const auto& [pane_id, shell] : shells) {
        const auto sequence = sequences.find(pane_id);
        if (sequence == sequences.end()) {
          continue;
        }

        const auto output = shell->wait_for_output(sequence->second, std::chrono::milliseconds{0});
        if (!output.bytes.empty()) {
          dirty_panes.insert(pane_id);
        }
      }

      if (!dirty_panes.empty()) {
        const auto now = std::chrono::steady_clock::now();
        if (last_output_redraw.time_since_epoch().count() != 0) {
          const auto next_allowed = last_output_redraw + kOutputRedrawInterval;
          if (now < next_allowed) {
            std::this_thread::sleep_for(next_allowed - now);
          }
        }

        if (stop_requested) {
          break;
        }

        {
          std::lock_guard stream_lock(stream_mutex);
          sequences = next_sequences;
        }
        for (const auto& [pane_id, shell] : shells) {
          const auto sequence = sequences.find(pane_id);
          if (sequence == sequences.end()) {
            continue;
          }

          const auto output =
              shell->wait_for_output(sequence->second, std::chrono::milliseconds{0});
          if (!output.bytes.empty()) {
            dirty_panes.insert(pane_id);
          }
        }

        std::string error;
        if (!replay_active_window(
                error, std::nullopt, std::nullopt, ReplayKind::Output, dirty_panes)) {
          stop_requested = true;
          break;
        }
        last_output_redraw = std::chrono::steady_clock::now();
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds{33});
      }
    }
  }};

  AttachEndReason end_reason = AttachEndReason::ClientDisconnected;
  while (!stop_requested) {
    AttachFrame frame;
    if (!read_attach_frame(pipe, frame, end_reason, stop_requested)) {
      break;
    }

    if (frame.type == AttachFrameType::Detach) {
      end_reason = AttachEndReason::Detached;
      break;
    }

    if (frame.type == AttachFrameType::Command) {
      std::string error;
      if (!events.call([&](DaemonState& state) {
            return execute_attach_command(state, session_id, frame.payload, error);
          })) {
        end_reason = AttachEndReason::ProtocolError;
        break;
      }
      if (!replay_full_window(error, std::nullopt, std::nullopt)) {
        end_reason = AttachEndReason::OutputClosed;
        break;
      }
      continue;
    }

    if (frame.type == AttachFrameType::Resize) {
      const auto dimensions = parse_attach_resize_payload(frame.payload);
      if (!dimensions) {
        end_reason = AttachEndReason::ProtocolError;
        break;
      }

      current_columns.store(static_cast<short>(dimensions->first), std::memory_order_relaxed);
      current_rows.store(static_cast<short>(dimensions->second), std::memory_order_relaxed);
      std::string error;
      if (!replay_full_window(error, std::nullopt, std::nullopt)) {
        end_reason = AttachEndReason::OutputClosed;
        break;
      }
      continue;
    }

    if (frame.type == AttachFrameType::MouseFocus) {
      const auto focus = parse_attach_mouse_focus_payload(frame.payload);
      if (!focus) {
        end_reason = AttachEndReason::ProtocolError;
        break;
      }

      std::string error;
      const auto changed = events.call([&](DaemonState& state) {
        return select_interactive_pane_at(
            state,
            session_id,
            focus->column,
            focus->row,
            current_columns.load(std::memory_order_relaxed),
            current_rows.load(std::memory_order_relaxed),
            error);
      });
      if (!changed) {
        if (!error.empty()) {
          end_reason = AttachEndReason::ProtocolError;
          break;
        }
        continue;
      }

      if (*changed && !replay_full_window(error, std::nullopt, std::nullopt)) {
        end_reason = AttachEndReason::OutputClosed;
        break;
      }
      continue;
    }

    if (frame.type == AttachFrameType::MouseEvent) {
      const auto mouse = parse_attach_mouse_event_payload(frame.payload);
      if (!mouse) {
        end_reason = AttachEndReason::ProtocolError;
        break;
      }

      std::string error;
      if (mouse->action == AttachMouseAction::Wheel) {
        const auto scroll =
            mouse->button == AttachMouseButton::WheelUp
                ? AttachScrollAction::LineUp
                : mouse->button == AttachMouseButton::WheelDown
                      ? AttachScrollAction::LineDown
                      : AttachScrollAction::Bottom;
        if (scroll == AttachScrollAction::Bottom) {
          continue;
        }

        if (!replay_full_window(error, scroll, std::nullopt)) {
          end_reason = AttachEndReason::OutputClosed;
          break;
        }
        continue;
      }

      const auto changed = events.call([&](DaemonState& state) {
        return handle_interactive_mouse_event(
            state,
            session_id,
            *mouse,
            current_columns.load(std::memory_order_relaxed),
            current_rows.load(std::memory_order_relaxed),
            mouse_drag,
            error);
      });
      if (!changed) {
        if (!error.empty()) {
          end_reason = AttachEndReason::ProtocolError;
          break;
        }
        continue;
      }

      if (*changed && !replay_full_window(error, std::nullopt, std::nullopt)) {
        end_reason = AttachEndReason::OutputClosed;
        break;
      }
      continue;
    }

    if (frame.type == AttachFrameType::Scroll) {
      const auto scroll = parse_attach_scroll_payload(frame.payload);
      if (!scroll) {
        end_reason = AttachEndReason::ProtocolError;
        break;
      }

      std::string error;
      if (!replay_full_window(error, *scroll, std::nullopt)) {
        end_reason = AttachEndReason::OutputClosed;
        break;
      }
      continue;
    }

    if (frame.type == AttachFrameType::CopyMode) {
      const auto action = parse_attach_copy_mode_payload(frame.payload);
      if (!action) {
        end_reason = AttachEndReason::ProtocolError;
        break;
      }

      {
        std::lock_guard stream_lock(stream_mutex);
        status_override.clear();
      }

      std::string error;
      if (!replay_full_window(error, std::nullopt, *action)) {
        end_reason = AttachEndReason::OutputClosed;
        break;
      }
      continue;
    }

    if (frame.type == AttachFrameType::Status) {
      {
        std::lock_guard stream_lock(stream_mutex);
        status_override = sanitize_status_text(frame.payload);
      }

      std::string error;
      if (!replay_full_window(error, std::nullopt, std::nullopt)) {
        end_reason = AttachEndReason::OutputClosed;
        break;
      }
      continue;
    }

    if (frame.type == AttachFrameType::CommandMode) {
      std::string status;
      (void)events.call([&](DaemonState& state) {
        return execute_command_mode_command(state, session_id, session_name, frame.payload, status);
      });
      {
        std::lock_guard stream_lock(stream_mutex);
        status_override = sanitize_status_text(status);
      }

      std::string error;
      if (!replay_full_window(error, std::nullopt, std::nullopt)) {
        end_reason = AttachEndReason::OutputClosed;
        break;
      }
      continue;
    }

    if (frame.type == AttachFrameType::Paste) {
      if (!frame.payload.empty()) {
        end_reason = AttachEndReason::ProtocolError;
        break;
      }

      std::string paste_buffer;
      events.call([&](DaemonState& state) {
        paste_buffer = state.paste_buffer;
      });

      if (paste_buffer.empty()) {
        {
          std::lock_guard stream_lock(stream_mutex);
          status_override = "wmux: paste buffer empty";
        }
        std::string error;
        if (!replay_full_window(error, std::nullopt, std::nullopt)) {
          end_reason = AttachEndReason::OutputClosed;
          break;
        }
        continue;
      }

      std::string error;
      auto shell = events.call([&](DaemonState& state) {
        return active_shell_for_session(state, session_id, error);
      });
      if (!shell) {
        end_reason = AttachEndReason::ShellClosed;
        break;
      }

      const auto normalized = normalize_paste_text_for_terminal(paste_buffer);
      {
        std::lock_guard stream_lock(stream_mutex);
        viewport_states[shell->pane_id].offset = 0;
        status_override = "wmux: pasted " + std::to_string(paste_buffer.size()) + " bytes";
      }

      if (!normalized.empty() && !shell->shell->write_input(normalized)) {
        end_reason = AttachEndReason::ShellClosed;
        break;
      }

      if (!replay_full_window(error, std::nullopt, std::nullopt)) {
        end_reason = AttachEndReason::OutputClosed;
        break;
      }
      continue;
    }

    std::string error;
    auto shell = events.call([&](DaemonState& state) {
      return active_shell_for_session(state, session_id, error);
    });
    if (!shell) {
      end_reason = AttachEndReason::ShellClosed;
      break;
    }

    {
      std::lock_guard stream_lock(stream_mutex);
      viewport_states[shell->pane_id].offset = 0;
    }

    if (!shell->shell->write_input(frame.payload)) {
      end_reason = AttachEndReason::ShellClosed;
      break;
    }
  }

  stop_requested = true;
  if (output_thread.joinable()) {
    output_thread.join();
  }

  if (output_closed && end_reason == AttachEndReason::ClientDisconnected) {
    end_reason = AttachEndReason::OutputClosed;
  }

  close_attach_pipe(pipe);
  events.call([&](DaemonState& state) {
    unregister_attach_client(state, client_id, end_reason);
  });
}

}  // namespace

std::wstring widen(std::string_view value) {
  if (value.empty()) {
    return {};
  }

  const int required =
      MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(required), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), wide.data(), required);
  return wide;
}

bool read_request(HANDLE pipe, std::string& request) {
  char buffer[512];
  request.clear();
  const auto deadline = std::chrono::steady_clock::now() + kRequestReadTimeout;

  while (std::chrono::steady_clock::now() < deadline) {
    DWORD bytes_available = 0;
    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &bytes_available, nullptr)) {
      return !request.empty();
    }

    if (bytes_available == 0) {
      std::this_thread::sleep_for(kRequestReadPoll);
      continue;
    }

    DWORD bytes_read = 0;
    const auto bytes_to_read =
        static_cast<DWORD>(std::min<std::size_t>(sizeof(buffer), bytes_available));
    if (!ReadFile(pipe, buffer, bytes_to_read, &bytes_read, nullptr) || bytes_read == 0) {
      return !request.empty();
    }

    request.append(buffer, buffer + bytes_read);
    if (request.find('\n') != std::string::npos) {
      return true;
    }
  }

  return !request.empty() && request.find('\n') != std::string::npos;
}

bool write_all(HANDLE pipe, std::string_view bytes) {
  const auto start = std::chrono::steady_clock::now();
  while (!bytes.empty()) {
    if (std::chrono::steady_clock::now() - start > kSlowClientWriteTimeout) {
      log_event(
          LogLevel::Warn,
          "daemon.attach",
          "write_timeout",
          {{"remaining_bytes", std::to_string(bytes.size())}});
      return false;
    }

    const auto bytes_to_write =
        static_cast<DWORD>(std::min<std::size_t>(bytes.size(), 64 * 1024));
    DWORD bytes_written = 0;
    const BOOL ok = WriteFile(pipe, bytes.data(), bytes_to_write, &bytes_written, nullptr);
    if (!ok || bytes_written == 0) {
      return false;
    }

    bytes.remove_prefix(bytes_written);
  }

  return true;
}

void close_pipe(HANDLE pipe) {
  DisconnectNamedPipe(pipe);
  CloseHandle(pipe);
}

void close_attach_pipe(HANDLE pipe) {
  DisconnectNamedPipe(pipe);
  CloseHandle(pipe);
}

HANDLE create_server_pipe(const std::wstring& endpoint, DWORD open_mode_flags) {
  return CreateNamedPipeW(
      endpoint.c_str(),
      PIPE_ACCESS_DUPLEX | open_mode_flags,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
      PIPE_UNLIMITED_INSTANCES,
      4096,
      4096,
      0,
      nullptr);
}

bool connect_named_pipe(HANDLE pipe) {
  return ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED;
}

void disconnect_attach_clients_for_session(DaemonState& state, SessionId session_id) {
  disconnect_attach_pipes(attach_client_pipes_for_session(state, session_id));
}

void disconnect_all_attach_clients(DaemonState& state) {
  disconnect_attach_pipes(all_attach_client_pipes(state));
}

bool wait_for_no_attach_clients(DaemonState& state, std::chrono::milliseconds timeout) {
  std::unique_lock lock(state.mutex);
  return state.attach_clients_changed.wait_for(
      lock, timeout, [&] { return state.attach_clients.empty(); });
}

void reap_finished_attach_workers(DaemonState& state) {
  auto workers = take_finished_attach_workers(state);
  join_workers(workers, "worker_join");
}

void join_all_attach_workers(DaemonState& state) {
  std::vector<std::pair<ClientId, std::thread>> workers;
  {
    std::unique_lock lock(state.mutex);
    state.attach_workers_changed.wait(lock, [&] {
      return std::ranges::all_of(state.attach_workers, [](const auto& worker) {
        return worker.done && worker.done->load(std::memory_order_acquire);
      });
    });

    workers.reserve(state.attach_workers.size());
    for (auto& worker : state.attach_workers) {
      workers.emplace_back(worker.client_id, std::move(worker.thread));
    }
    state.attach_workers.clear();
  }

  join_workers(workers, "worker_join_shutdown");
}

AttachDispatch dispatch_attach_connection(
    HANDLE pipe,
    const IpcRequest& request,
    DaemonEventLoop& events) {
  if (!is_attach_start_request(request.type)) {
    return AttachDispatch::NotAttach;
  }

  std::string error;
  auto target = events.call([&](DaemonState& state) {
    return target_for_attach(state, request, error);
  });
  if (!target) {
    write_all(pipe, make_response_json(false, error));
    return AttachDispatch::Completed;
  }

  const short columns = bounded_attach_columns(request.terminal_columns);
  const short rows = bounded_attach_rows(request.terminal_rows);
  const auto settings = events.call([](DaemonState& state) {
    return daemon_attach_settings(state);
  });
  const ClientId client_id = events.call([&](DaemonState& state) {
    return register_attach_client(state, target->session_id, target->session_name, pipe);
  });
  log_event(
      LogLevel::Info,
      "daemon.attach",
      "lifecycle",
      {{"event", "AttachStart"},
       {"client_id", std::to_string(client_id)},
       {"session_id", std::to_string(target->session_id)},
       {"session_name", target->session_name}});
  start_attach_worker(
      events,
      pipe,
      client_id,
      target->session_id,
      std::move(target->session_name),
      columns,
      rows,
      settings);
  return AttachDispatch::HandedOff;
}

void run_windows_attach_listener(DaemonEventLoop& events, std::atomic_bool& should_stop) {
  const auto endpoint = widen(attach_endpoint_name());

  while (!should_stop.load()) {
    events.call([](DaemonState& state) { reap_finished_attach_workers(state); });

    HANDLE pipe = create_server_pipe(endpoint, 0);
    if (pipe == INVALID_HANDLE_VALUE) {
      log_event(
          LogLevel::Error,
          "daemon.attach",
          "pipe_create_failed",
          {{"win32_error", std::to_string(GetLastError())}});
      return;
    }

    if (!connect_named_pipe(pipe)) {
      log_event(
          LogLevel::Warn,
          "daemon.attach",
          "connect_failed",
          {{"win32_error", std::to_string(GetLastError())}});
      CloseHandle(pipe);
      continue;
    }

    if (should_stop.load()) {
      close_attach_pipe(pipe);
      break;
    }

    std::string request;
    if (read_request(pipe, request)) {
      const auto parsed = parse_request_json(request);
      if (!parsed) {
        log_event(LogLevel::Warn, "daemon.attach", "malformed_request");
        write_all(pipe, make_response_json(false, "wmux: malformed attach request\n"));
      } else {
        const auto attach_dispatch = dispatch_attach_connection(pipe, *parsed, events);
        if (attach_dispatch == AttachDispatch::HandedOff) {
          pipe = INVALID_HANDLE_VALUE;
          continue;
        }

        if (attach_dispatch == AttachDispatch::NotAttach) {
          log_event(
              LogLevel::Warn,
              "daemon.attach",
              "wrong_endpoint",
              {{"type", parsed->type}});
          write_all(pipe, make_response_json(
                              false, "wmux: attach endpoint accepts only attach requests\n"));
        }
      }
    } else {
      log_event(LogLevel::Warn, "daemon.attach", "request_read_failed");
    }

    if (pipe != INVALID_HANDLE_VALUE) {
      close_pipe(pipe);
    }
  }

  events.call([](DaemonState& state) { reap_finished_attach_workers(state); });
}

void wake_attach_listener() {
  const auto endpoint = widen(attach_endpoint_name());
  HANDLE pipe = CreateFileW(
      endpoint.c_str(),
      GENERIC_READ | GENERIC_WRITE,
      0,
      nullptr,
      OPEN_EXISTING,
      0,
      nullptr);
  if (pipe != INVALID_HANDLE_VALUE) {
    CloseHandle(pipe);
  }
}

#else

void disconnect_attach_clients_for_session(DaemonState& state, SessionId session_id) {
  (void)state;
  (void)session_id;
}

void disconnect_all_attach_clients(DaemonState& state) {
  (void)state;
}

#endif

}  // namespace wmux::daemon_internal
