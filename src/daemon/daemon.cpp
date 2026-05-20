#include "wmux/daemon.hpp"

#include "wmux/ipc_protocol.hpp"
#include "wmux/ipc_transport.hpp"
#include "wmux/pty_process.hpp"
#include "wmux/session_manager.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace wmux {
namespace {

#ifdef _WIN32
constexpr std::string_view kDefaultShell = "powershell.exe -NoLogo -NoProfile";
constexpr short kInitialPtyColumns = 120;
constexpr short kInitialPtyRows = 30;
constexpr auto kRequestReadTimeout = std::chrono::seconds{5};
constexpr auto kRequestReadPoll = std::chrono::milliseconds{10};
#endif

using ClientId = std::uint64_t;

enum class AttachEndReason {
  Detached,
  ClientDisconnected,
  ShellClosed,
  OutputClosed,
  ProtocolError,
};

struct DaemonState {
  std::mutex mutex;
  std::condition_variable attach_clients_changed;
  SessionManager sessions;
  struct SessionRuntime {
    std::shared_ptr<PtyProcess> shell;
  };
  struct AttachClientRuntime {
    SessionId session_id{0};
    std::string session_name;
#ifdef _WIN32
    HANDLE pipe{nullptr};
#endif
  };
  std::unordered_map<SessionId, SessionRuntime> runtimes;
  std::unordered_map<ClientId, AttachClientRuntime> attach_clients;
  ClientId next_client_id{1};
};

void disconnect_attach_clients_for_session(DaemonState& state, SessionId session_id);
void disconnect_all_attach_clients(DaemonState& state);

std::string quoted(std::string_view value) {
  std::ostringstream out;
  out << "'" << value << "'";
  return out.str();
}

std::string session_error_message(SessionError error, std::string_view name) {
  std::ostringstream out;

  switch (error) {
    case SessionError::EmptyName:
      out << "wmux: session name cannot be empty\n";
      break;
    case SessionError::DuplicateName:
      out << "wmux: session " << quoted(name) << " already exists\n";
      break;
    case SessionError::NotFound:
      out << "wmux: session " << quoted(name) << " not found\n";
      break;
    case SessionError::None:
      out << "wmux: session operation failed\n";
      break;
  }

  return out.str();
}

struct DaemonStats {
  std::size_t session_count{0};
  std::size_t attach_client_count{0};
};

DaemonStats daemon_stats(DaemonState& state) {
  std::lock_guard lock(state.mutex);
  return {state.sessions.session_count(), state.attach_clients.size()};
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
#ifdef _WIN32
  auto shell = PtyProcess::start(kDefaultShell, kInitialPtyColumns, kInitialPtyRows);
  if (!shell.process) {
    return make_response_json(false, shell.error);
  }
  shell_process = std::move(shell.process);
#endif
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
      state.runtimes[result.id].shell = std::move(shell_process);
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
  std::shared_ptr<PtyProcess> killed_shell;
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
      killed_shell = std::move(runtime->second.shell);
      state.runtimes.erase(runtime);
    }
  }

  disconnect_attach_clients_for_session(state, killed_session_id);

  if (killed_shell) {
    killed_shell->terminate();
  }

  std::ostringstream out;
  out << "wmux: killed session " << quoted(request.session_name) << "\n";
  return make_response_json(true, out.str());
}

std::vector<std::shared_ptr<PtyProcess>> take_all_shells(DaemonState& state) {
  std::vector<std::shared_ptr<PtyProcess>> shells;
  {
    std::lock_guard lock(state.mutex);
    shells.reserve(state.runtimes.size());
    for (auto& [id, runtime] : state.runtimes) {
      (void)id;
      if (runtime.shell) {
        shells.push_back(std::move(runtime.shell));
      }
    }
    state.runtimes.clear();
  }

  return shells;
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

  return {};
}

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

#ifdef _WIN32

std::wstring widen(std::string_view value) {
  if (value.empty()) {
    return {};
  }

  const int required = MultiByteToWideChar(
      CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(required), L'\0');
  MultiByteToWideChar(
      CP_UTF8, 0, value.data(), static_cast<int>(value.size()), wide.data(), required);
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

struct AttachTarget {
  SessionId session_id{0};
  std::string session_name;
  std::shared_ptr<PtyProcess> shell;
};

std::optional<AttachTarget> shell_for_attach(
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

  const auto runtime = state.runtimes.find(*session_id);
  if (runtime == state.runtimes.end() || !runtime->second.shell) {
    error = "wmux: session has no shell process\n";
    return {};
  }

  return AttachTarget{*session_id, request.session_name, runtime->second.shell};
}

void close_pipe(HANDLE pipe) {
  DisconnectNamedPipe(pipe);
  CloseHandle(pipe);
}

void close_attach_pipe(HANDLE pipe) {
  DisconnectNamedPipe(pipe);
  CloseHandle(pipe);
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

enum class PipeReadResult {
  Ok,
  Closed,
  Failed,
};

PipeReadResult read_exact(HANDLE pipe, char* buffer, std::size_t byte_count) {
  std::size_t total_read = 0;
  while (total_read < byte_count) {
    DWORD bytes_read = 0;
    const auto bytes_remaining = static_cast<DWORD>(
        std::min<std::size_t>(byte_count - total_read, 64 * 1024));
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

bool read_attach_frame(HANDLE pipe, AttachFrame& frame, AttachEndReason& end_reason) {
  std::array<char, kAttachFrameHeaderSize> header{};
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
    std::shared_ptr<PtyProcess> shell,
    DaemonState& state,
    ClientId client_id,
    short columns,
    short rows) {
  if (columns > 0 && rows > 0) {
    shell->resize(columns, rows);
  }

  if (!write_all(pipe, make_response_json(true, ""))) {
    close_attach_pipe(pipe);
    unregister_attach_client(state, client_id, AttachEndReason::OutputClosed);
    return;
  }

  const auto snapshot = shell->output_snapshot();
  if (!snapshot.bytes.empty() && !write_all(pipe, snapshot.bytes)) {
    close_attach_pipe(pipe);
    unregister_attach_client(state, client_id, AttachEndReason::OutputClosed);
    return;
  }

  std::atomic_bool stop_requested{false};
  std::atomic_bool output_closed{false};
  auto next_sequence = snapshot.next_sequence;
  std::thread output_thread{[&] {
    while (!stop_requested) {
      const auto output = shell->wait_for_output(next_sequence, std::chrono::milliseconds{100});
      if (!output.bytes.empty()) {
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
    if (!read_attach_frame(pipe, frame, end_reason)) {
      break;
    }

    if (frame.type == AttachFrameType::Detach) {
      end_reason = AttachEndReason::Detached;
      break;
    }

    if (!shell->write_input(frame.payload)) {
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

enum class AttachDispatch {
  NotAttach,
  Completed,
  HandedOff,
};

AttachDispatch dispatch_attach_connection(
    HANDLE pipe,
    const IpcRequest& request,
    DaemonState& state) {
  if (request.type != "AttachSession") {
    return AttachDispatch::NotAttach;
  }

  std::string error;
  auto target = shell_for_attach(state, request, error);
  if (!target) {
    write_all(pipe, make_response_json(false, error));
    return AttachDispatch::Completed;
  }

  const short columns = attach_dimension(request.terminal_columns);
  const short rows = attach_dimension(request.terminal_rows);
  const ClientId client_id =
      register_attach_client(state, target->session_id, target->session_name, pipe);
  std::thread{[pipe,
               shell = std::move(target->shell),
               &state,
               client_id,
               columns,
               rows] { run_attach_connection(pipe, shell, state, client_id, columns, rows); }}
      .detach();
  return AttachDispatch::HandedOff;
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

bool connect_named_pipe_until_stop(HANDLE pipe, std::atomic_bool& should_stop) {
  HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (event == nullptr) {
    return false;
  }

  OVERLAPPED overlapped{};
  overlapped.hEvent = event;
  const BOOL connected = ConnectNamedPipe(pipe, &overlapped);
  if (connected) {
    CloseHandle(event);
    return true;
  }

  const DWORD error = GetLastError();
  if (error == ERROR_PIPE_CONNECTED) {
    CloseHandle(event);
    return true;
  }
  if (error != ERROR_IO_PENDING) {
    CloseHandle(event);
    return false;
  }

  while (!should_stop.load()) {
    const DWORD wait_result = WaitForSingleObject(event, 50);
    if (wait_result == WAIT_OBJECT_0) {
      DWORD transferred = 0;
      const BOOL ok = GetOverlappedResult(pipe, &overlapped, &transferred, FALSE);
      CloseHandle(event);
      return ok != 0;
    }
    if (wait_result != WAIT_TIMEOUT) {
      break;
    }
  }

  CancelIoEx(pipe, &overlapped);
  CloseHandle(event);
  return false;
}

void run_windows_attach_listener(DaemonState& state, std::atomic_bool& should_stop) {
  const auto endpoint = widen(attach_endpoint_name());

  while (!should_stop.load()) {
    HANDLE pipe = create_server_pipe(endpoint, FILE_FLAG_OVERLAPPED);
    if (pipe == INVALID_HANDLE_VALUE) {
      return;
    }

    if (!connect_named_pipe_until_stop(pipe, should_stop)) {
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

int run_windows_daemon() {
  std::atomic_bool should_stop{false};
  DaemonState state;
  const auto endpoint = widen(command_endpoint_name());
  std::thread attach_listener{[&] { run_windows_attach_listener(state, should_stop); }};

  while (!should_stop.load()) {
    HANDLE pipe = create_server_pipe(endpoint, 0);
    if (pipe == INVALID_HANDLE_VALUE) {
      should_stop = true;
      if (attach_listener.joinable()) {
        attach_listener.join();
      }
      return 1;
    }

    if (connect_named_pipe(pipe)) {
      std::string request;
      if (read_request(pipe, request)) {
        const auto parsed = parse_request_json(request);
        if (!parsed) {
          write_all(pipe, make_response_json(false, "wmux: malformed daemon request\n"));
        } else {
          write_all(pipe, handle_request(*parsed, should_stop, state));
        }
      }
    }

    if (pipe != INVALID_HANDLE_VALUE) {
      close_pipe(pipe);
    }
  }

  disconnect_all_attach_clients(state);
  if (attach_listener.joinable()) {
    attach_listener.join();
  }
  wait_for_no_attach_clients(state, std::chrono::seconds{2});
  return 0;
}

#else

void disconnect_attach_clients_for_session(DaemonState& state, SessionId session_id) {
  (void)state;
  (void)session_id;
}

void disconnect_all_attach_clients(DaemonState& state) {
  (void)state;
}

class Fd {
 public:
  Fd() = default;
  explicit Fd(int fd) : fd_(fd) {}
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;

  Fd(Fd&& other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
  }

  Fd& operator=(Fd&& other) noexcept {
    if (this != &other) {
      reset();
      fd_ = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }

  ~Fd() {
    reset();
  }

  int get() const {
    return fd_;
  }

  explicit operator bool() const {
    return fd_ >= 0;
  }

  void reset(int fd = -1) {
    if (fd_ >= 0) {
      close(fd_);
    }
    fd_ = fd;
  }

 private:
  int fd_{-1};
};

bool read_request(int fd, std::string& request) {
  char buffer[512];
  request.clear();

  while (true) {
    const ssize_t count = recv(fd, buffer, sizeof(buffer), 0);
    if (count < 0) {
      return false;
    }
    if (count == 0) {
      return !request.empty();
    }

    request.append(buffer, buffer + count);
    if (request.find('\n') != std::string::npos) {
      return true;
    }
  }
}

bool write_all(int fd, std::string_view value) {
  while (!value.empty()) {
    const ssize_t count = send(fd, value.data(), value.size(), 0);
    if (count <= 0) {
      return false;
    }
    value.remove_prefix(static_cast<std::size_t>(count));
  }

  return true;
}

enum class EndpointState {
  Missing,
  Live,
  Stale,
  Unavailable,
};

EndpointState endpoint_state(const std::string& endpoint) {
  Fd client{socket(AF_UNIX, SOCK_STREAM, 0)};
  if (!client) {
    return EndpointState::Unavailable;
  }

  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (endpoint.size() >= sizeof(address.sun_path)) {
    return EndpointState::Unavailable;
  }
  std::strncpy(address.sun_path, endpoint.c_str(), sizeof(address.sun_path) - 1);

  if (connect(client.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
    return EndpointState::Live;
  }

  if (errno == ENOENT) {
    return EndpointState::Missing;
  }

  if (errno == ECONNREFUSED) {
    return EndpointState::Stale;
  }

  return EndpointState::Unavailable;
}

int run_posix_daemon() {
  Fd server{socket(AF_UNIX, SOCK_STREAM, 0)};
  if (!server) {
    return 10;
  }

  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  const auto endpoint = command_endpoint_name();
  if (endpoint.size() >= sizeof(address.sun_path)) {
    return 11;
  }
  std::strncpy(address.sun_path, endpoint.c_str(), sizeof(address.sun_path) - 1);

  const auto state = endpoint_state(endpoint);
  if (state == EndpointState::Live) {
    return 0;
  }
  if (state == EndpointState::Unavailable) {
    return 14;
  }

  if (state == EndpointState::Stale) {
    unlink(endpoint.c_str());
  }
  if (bind(server.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    return errno;
  }

  if (listen(server.get(), 8) != 0) {
    unlink(endpoint.c_str());
    return errno;
  }

  std::atomic_bool should_stop{false};
  DaemonState state;
  while (!should_stop.load()) {
    Fd client{accept(server.get(), nullptr, nullptr)};
    if (!client) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }

    std::string request;
    if (read_request(client.get(), request)) {
      const auto parsed = parse_request_json(request);
      const auto response = parsed ? handle_request(*parsed, should_stop, state)
                                   : make_response_json(false, "wmux: malformed daemon request\n");
      write_all(client.get(), response);
    }
  }

  unlink(endpoint.c_str());
  return 0;
}

#endif

}  // namespace

int run_daemon() {
#ifdef _WIN32
  return run_windows_daemon();
#else
  return run_posix_daemon();
#endif
}

}  // namespace wmux
