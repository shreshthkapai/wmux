#include "wmux/daemon.hpp"

#include "wmux/ipc_protocol.hpp"
#include "wmux/ipc_transport.hpp"
#include "wmux/session_manager.hpp"

#include <sstream>
#include <string>
#include <string_view>

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

std::string handle_session_request(const IpcRequest& request, SessionManager& sessions) {
  if (request.type == "DefaultSession") {
    return make_response_json(true, "wmux: interactive session startup is not implemented yet\n");
  }

  if (request.type == "NewSession") {
    const auto result = sessions.create_session(request.session_name);
    if (!result.ok) {
      return make_response_json(false, session_error_message(result.error, request.session_name));
    }

    std::ostringstream out;
    out << "wmux: created session " << quoted(request.session_name) << "\n";
    return make_response_json(true, out.str());
  }

  if (request.type == "ListSessions") {
    const auto listed = sessions.list_sessions();
    if (listed.empty()) {
      return make_response_json(true, "wmux: no sessions\n");
    }

    std::ostringstream out;
    for (const auto& session : listed) {
      out << session.name << "\n";
    }
    return make_response_json(true, out.str());
  }

  if (request.type == "AttachSession") {
    if (request.session_name.empty()) {
      return make_response_json(false, session_error_message(SessionError::EmptyName, {}));
    }

    if (!sessions.has_session(request.session_name)) {
      return make_response_json(
          false, session_error_message(SessionError::NotFound, request.session_name));
    }

    std::ostringstream out;
    out << "wmux: attach for session " << quoted(request.session_name)
        << " is not implemented yet\n";
    return make_response_json(true, out.str());
  }

  if (request.type == "RenameSession") {
    const auto result = sessions.rename_session(request.target_name, request.new_name);
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

  if (request.type == "KillSession") {
    const auto result = sessions.kill_session(request.session_name);
    if (!result.ok) {
      return make_response_json(false, session_error_message(result.error, request.session_name));
    }

    std::ostringstream out;
    out << "wmux: killed session " << quoted(request.session_name) << "\n";
    return make_response_json(true, out.str());
  }

  return {};
}

std::string handle_request(
    std::string_view raw_request,
    bool& should_stop,
    SessionManager& sessions) {
  const auto request = parse_request_json(raw_request);
  if (!request) {
    return make_response_json(false, "wmux: malformed daemon request\n");
  }

  if (request->type == "Ping") {
    return make_response_json(true, "wmux: daemon is running\n");
  }

  if (request->type == "ServerStatus") {
    return make_response_json(true, "wmux: daemon is running\n");
  }

  if (request->type == "ServerStop") {
    should_stop = true;
    return make_response_json(true, "wmux: daemon stopping\n");
  }

  const auto session_response = handle_session_request(*request, sessions);
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

int run_windows_daemon() {
  bool should_stop = false;
  SessionManager sessions;
  const auto endpoint = widen(command_endpoint_name());

  while (!should_stop) {
    HANDLE pipe = CreateNamedPipeW(
        endpoint.c_str(),
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,
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
        const auto response = handle_request(request, should_stop, sessions);
        DWORD bytes_written = 0;
        WriteFile(
            pipe,
            response.data(),
            static_cast<DWORD>(response.size()),
            &bytes_written,
            nullptr);
      }
    }

    FlushFileBuffers(pipe);
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
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
  SessionManager sessions;
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
      const auto response = handle_request(request, should_stop, sessions);
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
