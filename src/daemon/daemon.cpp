#include "wmux/daemon.hpp"

#include "daemon_attach.hpp"
#include "daemon_commands.hpp"
#include "daemon_state.hpp"
#include "wmux/ipc_protocol.hpp"
#include "wmux/ipc_transport.hpp"

#include <atomic>
#include <chrono>
#include <string>
#include <string_view>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
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

using namespace daemon_internal;

#ifdef _WIN32

int run_windows_daemon() {
  std::atomic_bool should_stop{false};
  DaemonState state;
  const auto endpoint = widen(command_endpoint_name());
  std::thread attach_listener{[&] { run_windows_attach_listener(state, should_stop); }};

  while (!should_stop.load()) {
    HANDLE pipe = create_server_pipe(endpoint, 0);
    if (pipe == INVALID_HANDLE_VALUE) {
      should_stop = true;
      wake_attach_listener();
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
  wake_attach_listener();
  if (attach_listener.joinable()) {
    attach_listener.join();
  }
  wait_for_no_attach_clients(state, std::chrono::seconds{2});
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
