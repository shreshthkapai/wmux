#include "wmux/daemon.hpp"

#include "daemon_attach.hpp"
#include "daemon_commands.hpp"
#include "daemon_state.hpp"
#include "wmux/ipc_protocol.hpp"
#include "wmux/ipc_transport.hpp"
#include "wmux/logging.hpp"

#include <atomic>
#include <chrono>
#include <exception>
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

constexpr DWORD kDaemonInstanceWaitMilliseconds = 5000;

class UniqueHandle {
 public:
  UniqueHandle() = default;
  explicit UniqueHandle(HANDLE handle) : handle_(handle) {}
  UniqueHandle(const UniqueHandle&) = delete;
  UniqueHandle& operator=(const UniqueHandle&) = delete;

  UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.handle_) {
    other.handle_ = nullptr;
  }

  UniqueHandle& operator=(UniqueHandle&& other) noexcept {
    if (this != &other) {
      reset();
      handle_ = other.handle_;
      other.handle_ = nullptr;
    }
    return *this;
  }

  ~UniqueHandle() {
    reset();
  }

  HANDLE get() const {
    return handle_;
  }

  bool valid() const {
    return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
  }

  void reset(HANDLE handle = nullptr) {
    if (valid()) {
      CloseHandle(handle_);
    }
    handle_ = handle;
  }

 private:
  HANDLE handle_{nullptr};
};

class DaemonInstanceLock {
 public:
  DaemonInstanceLock() {
    mutex_.reset(CreateMutexW(nullptr, FALSE, L"Local\\wmux-daemon-instance"));
    if (!mutex_.valid()) {
      error_ = GetLastError();
      return;
    }

    const DWORD wait_result = WaitForSingleObject(mutex_.get(), kDaemonInstanceWaitMilliseconds);
    if (wait_result == WAIT_OBJECT_0 || wait_result == WAIT_ABANDONED) {
      acquired_ = true;
      recovered_abandoned_ = wait_result == WAIT_ABANDONED;
      return;
    }

    if (wait_result == WAIT_TIMEOUT) {
      already_running_ = true;
      mutex_.reset();
      return;
    }

    error_ = GetLastError();
    mutex_.reset();
  }

  DaemonInstanceLock(const DaemonInstanceLock&) = delete;
  DaemonInstanceLock& operator=(const DaemonInstanceLock&) = delete;

  ~DaemonInstanceLock() {
    if (acquired_ && mutex_.valid()) {
      ReleaseMutex(mutex_.get());
    }
  }

  bool acquired() const {
    return acquired_;
  }

  bool already_running() const {
    return already_running_;
  }

  DWORD error() const {
    return error_;
  }

  bool recovered_abandoned() const {
    return recovered_abandoned_;
  }

 private:
  UniqueHandle mutex_;
  bool acquired_{false};
  bool already_running_{false};
  bool recovered_abandoned_{false};
  DWORD error_{ERROR_SUCCESS};
};

int run_windows_daemon() {
  initialize_logging(LogRole::Daemon);
  DaemonInstanceLock instance_lock;
  if (instance_lock.already_running()) {
    log_event(LogLevel::Warn, "daemon.server", "already_running");
    shutdown_logging();
    return 0;
  }
  if (!instance_lock.acquired()) {
    log_event(
        LogLevel::Error,
        "daemon.server",
        "instance_lock_failed",
        {{"win32_error", std::to_string(instance_lock.error())}});
    shutdown_logging();
    return 1;
  }
  if (instance_lock.recovered_abandoned()) {
    log_event(LogLevel::Warn, "daemon.server", "recovered_abandoned_instance_lock");
  }

  log_event(LogLevel::Info, "daemon.server", "start");
  std::atomic_bool should_stop{false};
  DaemonState state;
  DaemonEventLoop events{state};
  events.start();
  events.call([](DaemonState& daemon_state) { load_daemon_config(daemon_state); });
  const auto endpoint = widen(command_endpoint_name());
  std::thread attach_listener{[&] { run_windows_attach_listener(events, should_stop); }};

  while (!should_stop.load()) {
    events.call([](DaemonState& daemon_state) { reap_finished_attach_workers(daemon_state); });

    HANDLE pipe = create_server_pipe(endpoint, 0);
    if (pipe == INVALID_HANDLE_VALUE) {
      log_event(LogLevel::Error, "daemon.server", "command_pipe_create_failed");
      should_stop = true;
      wake_attach_listener();
      if (attach_listener.joinable()) {
        attach_listener.join();
      }
      events.call([](DaemonState& daemon_state) { disconnect_all_attach_clients(daemon_state); });
      if (!wait_for_no_attach_clients(state, std::chrono::seconds{2})) {
        log_event(LogLevel::Warn, "daemon.attach", "clients_drain_timeout");
      }
      join_all_attach_workers(state);
      events.stop();
      return 1;
    }

    if (connect_named_pipe(pipe)) {
      std::string request;
      if (read_request(pipe, request)) {
        const auto parsed = parse_request_json(request);
        std::string response;
        if (!parsed) {
          log_event(LogLevel::Warn, "daemon.ipc", "malformed_request");
          response = make_response_json(false, "wmux: malformed daemon request\n");
        } else {
          log_event(LogLevel::Debug, "daemon.ipc", "request", {{"type", parsed->type}});
          try {
            response = events.call([&](DaemonState& daemon_state) {
              return handle_request(*parsed, should_stop, daemon_state);
            });
          } catch (const std::exception& error) {
            log_event(
                LogLevel::Error,
                "daemon.ipc",
                "request_exception",
                {{"type", parsed->type}, {"error", error.what()}});
            response = make_response_json(false, "wmux: daemon request failed internally\n");
          } catch (...) {
            log_event(
                LogLevel::Error,
                "daemon.ipc",
                "request_exception",
                {{"type", parsed->type}, {"error", "unknown"}});
            response = make_response_json(false, "wmux: daemon request failed internally\n");
          }
        }
        if (!write_all(pipe, response)) {
          log_event(LogLevel::Warn, "daemon.ipc", "response_write_failed");
        } else if (parsed && parsed->type == "ServerStatus") {
          log_event(
              LogLevel::Debug,
              "daemon.ipc",
              "response_written",
              {{"type", parsed->type}, {"bytes", std::to_string(response.size())}});
        }
      }
    }

    if (pipe != INVALID_HANDLE_VALUE) {
      close_pipe(pipe);
    }
    events.call([](DaemonState& daemon_state) { reap_finished_attach_workers(daemon_state); });
  }

  events.call([](DaemonState& daemon_state) { disconnect_all_attach_clients(daemon_state); });
  wake_attach_listener();
  if (attach_listener.joinable()) {
    attach_listener.join();
  }
  if (!wait_for_no_attach_clients(state, std::chrono::seconds{2})) {
    log_event(LogLevel::Warn, "daemon.attach", "clients_drain_timeout");
  }
  join_all_attach_workers(state);
  events.stop();
  log_event(LogLevel::Info, "daemon.server", "stop");
  shutdown_logging();
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
  initialize_logging(LogRole::Daemon);
  log_event(LogLevel::Info, "daemon.server", "start");
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
  DaemonEventLoop events{state};
  events.start();
  events.call([](DaemonState& daemon_state) { load_daemon_config(daemon_state); });
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
      if (!parsed) {
        log_event(LogLevel::Warn, "daemon.ipc", "malformed_request");
      }
      const auto response = parsed
                                ? events.call([&](DaemonState& daemon_state) {
                                    return handle_request(*parsed, should_stop, daemon_state);
                                  })
                                : make_response_json(false, "wmux: malformed daemon request\n");
      write_all(client.get(), response);
    }
  }

  events.stop();
  unlink(endpoint.c_str());
  log_event(LogLevel::Info, "daemon.server", "stop");
  shutdown_logging();
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
