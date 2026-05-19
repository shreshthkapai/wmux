#include "wmux/daemon.hpp"

#include "wmux/ipc_protocol.hpp"
#include "wmux/ipc_transport.hpp"
#include "wmux/pty_process.hpp"
#include "wmux/session_manager.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>

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
#endif

struct DaemonState {
  std::mutex mutex;
  SessionManager sessions;
  std::unordered_map<std::string, std::shared_ptr<PtyProcess>> shells;
};

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
      state.shells[request.session_name] = std::move(shell_process);
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

  auto node = state.shells.extract(request.target_name);
  if (!node.empty()) {
    node.key() = request.new_name;
    state.shells.insert(std::move(node));
  }

  std::ostringstream out;
  out << "wmux: renamed session " << quoted(request.target_name) << " to "
      << quoted(request.new_name) << "\n";
  return make_response_json(true, out.str());
}

std::string handle_kill_session(const IpcRequest& request, DaemonState& state) {
  std::shared_ptr<PtyProcess> killed_shell;
  {
    std::lock_guard lock(state.mutex);
    const auto result = state.sessions.kill_session(request.session_name);
    if (!result.ok) {
      return make_response_json(false, session_error_message(result.error, request.session_name));
    }

    const auto shell = state.shells.find(request.session_name);
    if (shell != state.shells.end()) {
      killed_shell = std::move(shell->second);
      state.shells.erase(shell);
    }
  }

  if (killed_shell) {
    killed_shell->terminate();
  }

  std::ostringstream out;
  out << "wmux: killed session " << quoted(request.session_name) << "\n";
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

  return {};
}

std::string handle_request(
    const IpcRequest& request,
    bool& should_stop,
    DaemonState& state) {
  if (request.type == "Ping") {
    return make_response_json(true, "wmux: daemon is running\n");
  }

  if (request.type == "ServerStatus") {
    return make_response_json(true, "wmux: daemon is running\n");
  }

  if (request.type == "ServerStop") {
    should_stop = true;
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
  DWORD bytes_read = 0;
  request.clear();

  while (ReadFile(pipe, buffer, sizeof(buffer), &bytes_read, nullptr) && bytes_read > 0) {
    request.append(buffer, buffer + bytes_read);
    if (request.find('\n') != std::string::npos) {
      return true;
    }
  }

  return !request.empty();
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

std::shared_ptr<PtyProcess> shell_for_attach(
    DaemonState& state,
    const IpcRequest& request,
    std::string& error) {
  if (request.session_name.empty()) {
    error = session_error_message(SessionError::EmptyName, {});
    return {};
  }

  std::lock_guard lock(state.mutex);
  if (!state.sessions.has_session(request.session_name)) {
    error = session_error_message(SessionError::NotFound, request.session_name);
    return {};
  }

  const auto shell = state.shells.find(request.session_name);
  if (shell == state.shells.end() || !shell->second) {
    error = "wmux: session has no shell process\n";
    return {};
  }

  return shell->second;
}

void close_pipe(HANDLE pipe) {
  FlushFileBuffers(pipe);
  DisconnectNamedPipe(pipe);
  CloseHandle(pipe);
}

void run_attach_connection(HANDLE pipe, std::shared_ptr<PtyProcess> shell) {
  if (!write_all(pipe, make_response_json(true, ""))) {
    close_pipe(pipe);
    return;
  }

  const auto snapshot = shell->output_snapshot();
  if (!snapshot.bytes.empty() && !write_all(pipe, snapshot.bytes)) {
    close_pipe(pipe);
    return;
  }

  std::atomic_bool stop_requested{false};
  auto next_sequence = snapshot.next_sequence;
  std::thread output_thread{[&] {
    while (!stop_requested) {
      const auto output = shell->wait_for_output(next_sequence, std::chrono::milliseconds{100});
      if (!output.bytes.empty()) {
        if (!write_all(pipe, output.bytes)) {
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

  char buffer[4096];
  while (!stop_requested) {
    DWORD bytes_read = 0;
    const BOOL ok = ReadFile(pipe, buffer, static_cast<DWORD>(sizeof(buffer)), &bytes_read, nullptr);
    if (!ok || bytes_read == 0) {
      break;
    }

    if (!shell->write_input(std::string_view{buffer, bytes_read})) {
      break;
    }
  }

  stop_requested = true;
  if (output_thread.joinable()) {
    output_thread.join();
  }

  close_pipe(pipe);
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
  auto shell = shell_for_attach(state, request, error);
  if (!shell) {
    write_all(pipe, make_response_json(false, error));
    return AttachDispatch::Completed;
  }

  std::thread{[pipe, shell = std::move(shell)] { run_attach_connection(pipe, shell); }}.detach();
  return AttachDispatch::HandedOff;
}

int run_windows_daemon() {
  bool should_stop = false;
  DaemonState state;
  const auto endpoint = widen(command_endpoint_name());

  while (!should_stop) {
    HANDLE pipe = CreateNamedPipeW(
        endpoint.c_str(),
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        PIPE_UNLIMITED_INSTANCES,
        4096,
        4096,
        0,
        nullptr);

    if (pipe == INVALID_HANDLE_VALUE) {
      return 1;
    }

    const BOOL connected =
        ConnectNamedPipe(pipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
    if (connected) {
      std::string request;
      if (read_request(pipe, request)) {
        const auto parsed = parse_request_json(request);
        if (!parsed) {
          write_all(pipe, make_response_json(false, "wmux: malformed daemon request\n"));
        } else {
          const auto attach_dispatch = dispatch_attach_connection(pipe, *parsed, state);
          if (attach_dispatch == AttachDispatch::HandedOff) {
            pipe = INVALID_HANDLE_VALUE;
            continue;
          }

          if (attach_dispatch == AttachDispatch::NotAttach) {
            write_all(pipe, handle_request(*parsed, should_stop, state));
          }
        }
      }
    }

    if (pipe != INVALID_HANDLE_VALUE) {
      close_pipe(pipe);
    }
  }

  return 0;
}

#else

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

  bool should_stop = false;
  DaemonState state;
  while (!should_stop) {
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
