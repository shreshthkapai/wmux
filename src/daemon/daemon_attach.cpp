#include "daemon_attach.hpp"

#include "daemon_shell.hpp"
#include "wmux/command_mode.hpp"
#include "wmux/commands.hpp"
#include "wmux/ipc_transport.hpp"
#include "wmux/pty_process.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wmux::daemon_internal {

#ifdef _WIN32
namespace {

constexpr auto kRequestReadTimeout = std::chrono::seconds{5};
constexpr auto kRequestReadPoll = std::chrono::milliseconds{10};
constexpr auto kOutputRedrawInterval = std::chrono::milliseconds{33};
constexpr std::string_view kClearTerminal = "\x1b[2J\x1b[H";

struct AttachTarget {
  SessionId session_id{0};
  std::string session_name;
};

struct ActiveShell {
  WindowId window_id{0};
  PaneId pane_id{0};
  std::shared_ptr<PtyProcess> shell;
};

struct RenderPane {
  PaneLayoutRect rect;
  bool active{false};
  std::shared_ptr<PtyProcess> shell;
};

struct ActiveWindowFrame {
  WindowId window_id{0};
  PaneId active_pane_id{0};
  std::string session_name;
  std::string window_name;
  int columns{120};
  int rows{30};
  std::vector<RenderPane> panes;
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
    std::string_view session_name,
    short columns,
    short rows,
    std::string& error) {
  ActiveWindowFrame frame;
  frame.columns = columns > 0 ? columns : 120;
  frame.rows = rows > 0 ? rows : 30;

  std::lock_guard lock(state.mutex);
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

  const int pane_rows = std::max(1, frame.rows - 1);
  const auto rects = compute_pane_layout_rects(window->pane_tree, frame.columns, pane_rows);

  frame.window_id = window->id;
  frame.active_pane_id = window->active_pane_id;
  frame.session_name = std::string{session_name};
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

short attach_dimension(std::uint16_t value) {
  if (value == 0 || value > 32767) {
    return 0;
  }
  return static_cast<short>(value);
}

ClientId register_attach_client(
    DaemonState& state,
    SessionId session_id,
    std::string session_name,
    HANDLE pipe) {
  std::lock_guard lock(state.mutex);
  const ClientId client_id = state.next_client_id++;
  DaemonState::AttachClientRuntime client;
  client.session_id = session_id;
  client.session_name = std::move(session_name);
  client.pipe = pipe;
  state.attach_clients.emplace(client_id, std::move(client));
  return client_id;
}

void unregister_attach_client(
    DaemonState& state,
    ClientId client_id,
    AttachEndReason reason) {
  (void)reason;
  {
    std::lock_guard lock(state.mutex);
    state.attach_clients.erase(client_id);
  }
  state.attach_clients_changed.notify_all();
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

int body_left(const PaneLayoutRect& rect) {
  return rect.width >= 3 && rect.height >= 3 ? rect.left + 1 : rect.left;
}

int body_top(const PaneLayoutRect& rect) {
  return rect.width >= 3 && rect.height >= 3 ? rect.top + 1 : rect.top;
}

int body_width(const PaneLayoutRect& rect) {
  return rect.width >= 3 && rect.height >= 3 ? rect.width - 2 : 0;
}

int body_height(const PaneLayoutRect& rect) {
  return rect.width >= 3 && rect.height >= 3 ? rect.height - 2 : 0;
}

void append_cursor_move(std::string& out, int row, int column) {
  out += "\x1b[";
  out += std::to_string(row + 1);
  out += ";";
  out += std::to_string(column + 1);
  out += "H";
}

void append_clipped_text(std::string& out, std::string_view line, int width) {
  if (width <= 0) {
    return;
  }

  const auto count = static_cast<std::size_t>(std::min<int>(width, static_cast<int>(line.size())));
  out.append(line.substr(0, count));
  if (static_cast<int>(count) < width) {
    out.append(static_cast<std::size_t>(width) - count, ' ');
  }
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

void append_pane_border(std::string& out, const PaneLayoutRect& rect, bool active) {
  if (rect.width <= 0 || rect.height <= 0) {
    return;
  }

  const char horizontal = active ? '=' : '-';
  const char vertical = active ? '#' : '|';

  if (rect.height == 1) {
    append_cursor_move(out, rect.top, rect.left);
    out.append(static_cast<std::size_t>(rect.width), horizontal);
    return;
  }

  if (rect.width == 1) {
    for (int row = 0; row < rect.height; ++row) {
      append_cursor_move(out, rect.top + row, rect.left);
      out.push_back(vertical);
    }
    return;
  }

  append_cursor_move(out, rect.top, rect.left);
  out.push_back('+');
  out.append(static_cast<std::size_t>(std::max(0, rect.width - 2)), horizontal);
  out.push_back('+');

  for (int row = 1; row < rect.height - 1; ++row) {
    append_cursor_move(out, rect.top + row, rect.left);
    out.push_back(vertical);
    if (rect.width > 2) {
      out.append(static_cast<std::size_t>(rect.width - 2), ' ');
    }
    out.push_back(vertical);
  }

  append_cursor_move(out, rect.top + rect.height - 1, rect.left);
  out.push_back('+');
  out.append(static_cast<std::size_t>(std::max(0, rect.width - 2)), horizontal);
  out.push_back('+');
}

std::string render_frame(
    const ActiveWindowFrame& frame,
    const std::unordered_map<PaneId, PtyOutputSnapshot>& snapshots,
    std::string_view status_override) {
  std::string out{kClearTerminal};

  for (const auto& pane : frame.panes) {
    append_pane_border(out, pane.rect, pane.active);

    const int left = body_left(pane.rect);
    const int top = body_top(pane.rect);
    const int width = body_width(pane.rect);
    const int height = body_height(pane.rect);
    const auto snapshot = snapshots.find(pane.rect.pane_id);
    if (snapshot == snapshots.end() || width <= 0 || height <= 0) {
      continue;
    }

    const auto& lines = snapshot->second.screen.lines;
    const int first_row = std::max(0, static_cast<int>(lines.size()) - height);
    for (int row = 0; row < height; ++row) {
      append_cursor_move(out, top + row, left);
      const int line_index = first_row + row;
      if (line_index >= 0 && line_index < static_cast<int>(lines.size())) {
        append_clipped_text(out, lines[static_cast<std::size_t>(line_index)], width);
      } else {
        out.append(static_cast<std::size_t>(width), ' ');
      }
    }
  }

  if (frame.rows > 1) {
    std::ostringstream status;
    if (status_override.empty()) {
      status << " wmux [" << frame.session_name << "] window " << frame.window_name
             << " pane " << frame.active_pane_id << " ";
    } else {
      status << status_override;
    }
    std::string status_line = status.str();
    if (static_cast<int>(status_line.size()) > frame.columns) {
      status_line.resize(static_cast<std::size_t>(frame.columns));
    }
    if (static_cast<int>(status_line.size()) < frame.columns) {
      status_line.append(static_cast<std::size_t>(frame.columns) - status_line.size(), ' ');
    }

    append_cursor_move(out, frame.rows - 1, 0);
    out += "\x1b[7m";
    out += status_line;
    out += "\x1b[0m";
  }

  return out;
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
  auto shell = start_default_shell();
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
  auto shell = start_default_shell();
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
    return false;
  }

  frame.type = parsed->type;
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
    DaemonState& state,
    ClientId client_id,
    SessionId session_id,
    std::string session_name,
    short columns,
    short rows,
    bool mouse_enabled) {
  if (!write_all(pipe, make_response_json(true, "", mouse_enabled))) {
    close_attach_pipe(pipe);
    unregister_attach_client(state, client_id, AttachEndReason::OutputClosed);
    return;
  }

  std::atomic_bool stop_requested{false};
  std::atomic_bool output_closed{false};
  std::atomic<short> current_columns{columns > 0 ? columns : static_cast<short>(120)};
  std::atomic<short> current_rows{rows > 0 ? rows : static_cast<short>(30)};
  std::mutex stream_mutex;
  std::unordered_map<PaneId, std::shared_ptr<PtyProcess>> current_shells;
  std::unordered_map<PaneId, std::uint64_t> next_sequences;
  std::string status_override;
  MouseDragState mouse_drag;

  const auto replay_active_window = [&](std::string& error) {
    std::lock_guard stream_lock(stream_mutex);
    auto frame = active_window_frame(
        state,
        session_id,
        session_name,
        current_columns.load(std::memory_order_relaxed),
        current_rows.load(std::memory_order_relaxed),
        error);
    if (!frame) {
      return false;
    }

    std::unordered_map<PaneId, PtyOutputSnapshot> snapshots;
    snapshots.reserve(frame->panes.size());
    for (const auto& pane : frame->panes) {
      const int width = std::max(1, body_width(pane.rect));
      const int height = std::max(1, body_height(pane.rect));
      pane.shell->resize(static_cast<short>(std::min(width, 32767)),
                         static_cast<short>(std::min(height, 32767)));
      snapshots.emplace(pane.rect.pane_id, pane.shell->output_snapshot());
    }

    const auto replay = render_frame(*frame, snapshots, status_override);

    if (!write_all(pipe, replay)) {
      output_closed = true;
      return false;
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
    return true;
  };

  {
    std::string error;
    if (!replay_active_window(error)) {
      if (!error.empty()) {
        write_all(pipe, error);
      }
      close_attach_pipe(pipe);
      unregister_attach_client(state, client_id, AttachEndReason::OutputClosed);
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

      bool changed = false;
      for (const auto& [pane_id, shell] : shells) {
        const auto sequence = sequences.find(pane_id);
        if (sequence == sequences.end()) {
          continue;
        }

        const auto output = shell->wait_for_output(sequence->second, std::chrono::milliseconds{0});
        if (!output.bytes.empty()) {
          changed = true;
          break;
        }
      }

      if (changed) {
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

        std::string error;
        if (!replay_active_window(error)) {
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
      if (!execute_attach_command(state, session_id, frame.payload, error)) {
        end_reason = AttachEndReason::ProtocolError;
        break;
      }
      if (!replay_active_window(error)) {
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
      if (!replay_active_window(error)) {
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
      const auto changed = select_interactive_pane_at(
          state,
          session_id,
          focus->column,
          focus->row,
          current_columns.load(std::memory_order_relaxed),
          current_rows.load(std::memory_order_relaxed),
          error);
      if (!changed) {
        if (!error.empty()) {
          end_reason = AttachEndReason::ProtocolError;
          break;
        }
        continue;
      }

      if (*changed && !replay_active_window(error)) {
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
      const auto changed = handle_interactive_mouse_event(
          state,
          session_id,
          *mouse,
          current_columns.load(std::memory_order_relaxed),
          current_rows.load(std::memory_order_relaxed),
          mouse_drag,
          error);
      if (!changed) {
        if (!error.empty()) {
          end_reason = AttachEndReason::ProtocolError;
          break;
        }
        continue;
      }

      if (*changed && !replay_active_window(error)) {
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
      if (!replay_active_window(error)) {
        end_reason = AttachEndReason::OutputClosed;
        break;
      }
      continue;
    }

    if (frame.type == AttachFrameType::CommandMode) {
      std::string status;
      (void)execute_command_mode_command(state, session_id, session_name, frame.payload, status);
      {
        std::lock_guard stream_lock(stream_mutex);
        status_override = sanitize_status_text(status);
      }

      std::string error;
      if (!replay_active_window(error)) {
        end_reason = AttachEndReason::OutputClosed;
        break;
      }
      continue;
    }

    std::string error;
    auto shell = active_shell_for_session(state, session_id, error);
    if (!shell || !shell->shell->write_input(frame.payload)) {
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
  unregister_attach_client(state, client_id, end_reason);
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
  while (!bytes.empty()) {
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

AttachDispatch dispatch_attach_connection(
    HANDLE pipe,
    const IpcRequest& request,
    DaemonState& state) {
  if (request.type != "AttachSession") {
    return AttachDispatch::NotAttach;
  }

  std::string error;
  auto target = target_for_attach(state, request, error);
  if (!target) {
    write_all(pipe, make_response_json(false, error));
    return AttachDispatch::Completed;
  }

  const short columns = attach_dimension(request.terminal_columns);
  const short rows = attach_dimension(request.terminal_rows);
  const bool mouse_enabled = daemon_mouse_enabled(state);
  const ClientId client_id =
      register_attach_client(state, target->session_id, target->session_name, pipe);
  std::thread{[pipe,
               &state,
               client_id,
               session_id = target->session_id,
               session_name = std::move(target->session_name),
               columns,
               rows,
               mouse_enabled] {
    run_attach_connection(
        pipe, state, client_id, session_id, session_name, columns, rows, mouse_enabled);
  }}
      .detach();
  return AttachDispatch::HandedOff;
}

void run_windows_attach_listener(DaemonState& state, std::atomic_bool& should_stop) {
  const auto endpoint = widen(attach_endpoint_name());

  while (!should_stop.load()) {
    HANDLE pipe = create_server_pipe(endpoint, 0);
    if (pipe == INVALID_HANDLE_VALUE) {
      return;
    }

    if (!connect_named_pipe(pipe)) {
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
        write_all(pipe, make_response_json(false, "wmux: malformed attach request\n"));
      } else {
        const auto attach_dispatch = dispatch_attach_connection(pipe, *parsed, state);
        if (attach_dispatch == AttachDispatch::HandedOff) {
          pipe = INVALID_HANDLE_VALUE;
          continue;
        }

        if (attach_dispatch == AttachDispatch::NotAttach) {
          write_all(pipe, make_response_json(
                              false, "wmux: attach endpoint accepts only attach requests\n"));
        }
      }
    }

    if (pipe != INVALID_HANDLE_VALUE) {
      close_pipe(pipe);
    }
  }
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
