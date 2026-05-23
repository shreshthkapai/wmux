#include "daemon_attach.hpp"

#include "daemon_shell.hpp"
#include "wmux/ipc_transport.hpp"
#include "wmux/pty_process.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace wmux::daemon_internal {

#ifdef _WIN32
namespace {

constexpr auto kRequestReadTimeout = std::chrono::seconds{5};
constexpr auto kRequestReadPoll = std::chrono::milliseconds{10};
constexpr std::string_view kClearTerminal = "\x1b[2J\x1b[H";

struct AttachTarget {
  SessionId session_id{0};
  std::string session_name;
};

struct ActiveShell {
  WindowId window_id{0};
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

  const auto runtime = state.runtimes.find(session_id);
  if (runtime == state.runtimes.end()) {
    error = "wmux: session has no runtime\n";
    return {};
  }

  const auto window = runtime->second.windows.find(*active_window);
  if (window == runtime->second.windows.end() || !window->second.shell) {
    error = "wmux: active window has no shell process\n";
    return {};
  }

  return ActiveShell{*active_window, window->second.shell};
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

struct AttachFrame {
  AttachFrameType type{AttachFrameType::Input};
  std::string payload;
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

bool create_interactive_window(DaemonState& state, SessionId session_id, std::string& error) {
  auto shell = start_default_shell();
  if (!shell.process) {
    error = shell.error;
    return false;
  }

  std::shared_ptr<PtyProcess> shell_process = std::move(shell.process);
  {
    std::lock_guard lock(state.mutex);
    const auto name = next_window_name(state.sessions.list_windows(session_id));
    const auto result = state.sessions.create_window(session_id, name);
    if (!result.ok) {
      error = window_error_message(result.error, name);
      shell_process->terminate();
      return false;
    }

    state.runtimes[result.session_id].windows[result.window_id].shell = std::move(shell_process);
  }

  return true;
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
    short columns,
    short rows) {
  if (!write_all(pipe, make_response_json(true, ""))) {
    close_attach_pipe(pipe);
    unregister_attach_client(state, client_id, AttachEndReason::OutputClosed);
    return;
  }

  std::atomic_bool stop_requested{false};
  std::atomic_bool output_closed{false};
  std::mutex stream_mutex;
  WindowId current_window_id = 0;
  std::uint64_t next_sequence = 0;
  std::shared_ptr<PtyProcess> current_shell;

  const auto replay_active_window = [&](std::string& error) {
    auto active_shell = active_shell_for_session(state, session_id, error);
    if (!active_shell) {
      return false;
    }

    if (columns > 0 && rows > 0) {
      active_shell->shell->resize(columns, rows);
    }

    const auto snapshot = active_shell->shell->output_snapshot();
    std::string replay{kClearTerminal};
    replay.append(snapshot.bytes);

    std::lock_guard stream_lock(stream_mutex);
    if (!write_all(pipe, replay)) {
      output_closed = true;
      return false;
    }

    current_window_id = active_shell->window_id;
    current_shell = std::move(active_shell->shell);
    next_sequence = snapshot.next_sequence;
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
    while (!stop_requested) {
      std::shared_ptr<PtyProcess> shell;
      std::uint64_t sequence = 0;
      WindowId window_id = 0;
      {
        std::lock_guard stream_lock(stream_mutex);
        shell = current_shell;
        sequence = next_sequence;
        window_id = current_window_id;
      }

      if (!shell) {
        std::this_thread::sleep_for(std::chrono::milliseconds{25});
        continue;
      }

      const auto output = shell->wait_for_output(sequence, std::chrono::milliseconds{100});
      if (!output.bytes.empty()) {
        std::lock_guard stream_lock(stream_mutex);
        if (shell != current_shell || window_id != current_window_id || sequence != next_sequence) {
          continue;
        }

        if (!write_all(pipe, output.bytes)) {
          output_closed = true;
          stop_requested = true;
          break;
        }
        next_sequence = output.next_sequence;
      }

      if (!output.alive && output.bytes.empty()) {
        stop_requested = true;
        break;
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
  const ClientId client_id =
      register_attach_client(state, target->session_id, target->session_name, pipe);
  std::thread{[pipe,
               &state,
               client_id,
               session_id = target->session_id,
               columns,
               rows] {
    run_attach_connection(pipe, state, client_id, session_id, columns, rows);
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
