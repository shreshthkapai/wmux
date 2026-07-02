#include "wmux/daemon.hpp"

#include "windows_attach_server.hpp"
#include "daemon_commands.hpp"
#include "daemon_state.hpp"
#include "wmux/ipc_protocol.hpp"
#include "wmux/platform/ipc_transport.hpp"
#include "wmux/logging.hpp"

#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <optional>
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

DaemonEventResult handle_runtime_event(DaemonState& state, const DaemonEvent& event) {
  if (event.kind == DaemonEventKind::IpcCommand) {
    const auto command = handle_request(
        event.command,
        state,
        event.request_id,
        event.client_id);
    DaemonEventResult result;
    result.handled = true;
    result.has_response = true;
    result.response = command.response;
    result.request_shutdown = command.should_stop;
    return result;
  }

#ifdef _WIN32
  if (const auto attach_result = handle_attach_daemon_event(state, event)) {
    return *attach_result;
  }
#endif

  return handle_daemon_event(state, event);
}

#ifdef _WIN32

constexpr DWORD kDaemonInstanceWaitMilliseconds = 5000;
constexpr auto kCommandRequestReadTimeout = std::chrono::seconds{5};
constexpr auto kCommandRequestReadPoll = std::chrono::milliseconds{10};

enum class PipeReadResult {
  Ok,
  Closed,
  Failed,
};

struct FramedIpcRequest {
  IpcFrameHeader header;
  std::string payload;
};

std::wstring daemon_instance_mutex_name() {
  std::string endpoint = command_endpoint_name();
  constexpr std::string_view marker = "wmux-";
  const auto marker_pos = endpoint.rfind(marker);
  std::string suffix = marker_pos == std::string::npos
                           ? std::string{"unknown-user"}
                           : endpoint.substr(marker_pos + marker.size());
  for (char& ch : suffix) {
    const auto byte = static_cast<unsigned char>(ch);
    if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
        (byte >= '0' && byte <= '9') || ch == '-' || ch == '_' || ch == '.') {
      continue;
    }
    ch = '_';
  }
  return L"Local\\wmux-daemon-instance-" + widen(suffix);
}

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
    const auto mutex_name = daemon_instance_mutex_name();
    mutex_.reset(CreateMutexW(nullptr, FALSE, mutex_name.c_str()));
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

PipeReadResult wait_for_any_available_byte_until(
    HANDLE pipe,
    DWORD& bytes_available,
    std::chrono::steady_clock::time_point deadline) {
  while (std::chrono::steady_clock::now() < deadline) {
    bytes_available = 0;
    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &bytes_available, nullptr)) {
      const DWORD error = GetLastError();
      return error == ERROR_BROKEN_PIPE || error == ERROR_OPERATION_ABORTED
                 ? PipeReadResult::Closed
                 : PipeReadResult::Failed;
    }

    if (bytes_available > 0) {
      return PipeReadResult::Ok;
    }

    std::this_thread::sleep_for(kCommandRequestReadPoll);
  }

  return PipeReadResult::Failed;
}

PipeReadResult read_exact_pipe(
    HANDLE pipe,
    char* data,
    std::size_t size,
    std::chrono::steady_clock::time_point deadline) {
  std::size_t total = 0;
  while (total < size) {
    DWORD bytes_available = 0;
    const auto ready = wait_for_any_available_byte_until(pipe, bytes_available, deadline);
    if (ready != PipeReadResult::Ok) {
      return ready;
    }

    DWORD bytes_read = 0;
    const auto bytes_to_read = static_cast<DWORD>(
        std::min<std::size_t>({size - total, static_cast<std::size_t>(bytes_available), 4096}));
    const BOOL ok = ReadFile(pipe, data + total, bytes_to_read, &bytes_read, nullptr);
    if (bytes_read > 0) {
      total += bytes_read;
      if (total == size) {
        return PipeReadResult::Ok;
      }
    }
    if (!ok) {
      const DWORD error = GetLastError();
      return error == ERROR_BROKEN_PIPE || error == ERROR_OPERATION_ABORTED
                 ? PipeReadResult::Closed
                 : PipeReadResult::Failed;
    }
    if (bytes_read == 0) {
      return PipeReadResult::Closed;
    }
  }
  return PipeReadResult::Ok;
}

std::string with_trailing_newline(std::string_view value) {
  std::string result{value};
  if (result.empty() || result.back() != '\n') {
    result.push_back('\n');
  }
  return result;
}

std::optional<FramedIpcRequest> read_control_ipc_request(
    HANDLE pipe,
    std::string& error,
    std::uint64_t& request_id) {
  request_id = 0;
  const auto deadline = std::chrono::steady_clock::now() + kCommandRequestReadTimeout;
  std::array<char, kIpcFrameHeaderSize> raw_header{};
  const auto header_read = read_exact_pipe(pipe, raw_header.data(), raw_header.size(), deadline);
  if (header_read != PipeReadResult::Ok) {
    error = header_read == PipeReadResult::Closed ? "wmux: daemon request frame was closed"
                                                  : "wmux: truncated daemon request frame";
    return std::nullopt;
  }

  auto parsed_header =
      parse_ipc_frame_header(std::string_view{raw_header.data(), raw_header.size()});
  request_id = parsed_header.header.request_id;
  if (!parsed_header.ok) {
    error = parsed_header.message;
    return std::nullopt;
  }

  if (parsed_header.header.kind != IpcFrameKind::Control) {
    error = "wmux: command endpoint accepts only control IPC frames";
    return std::nullopt;
  }

  FramedIpcRequest request;
  request.header = parsed_header.header;
  if (request.header.payload_size > 0) {
    request.payload.resize(request.header.payload_size);
    const auto payload_read =
        read_exact_pipe(pipe, request.payload.data(), request.payload.size(), deadline);
    if (payload_read != PipeReadResult::Ok) {
      error = payload_read == PipeReadResult::Closed ? "wmux: daemon request payload was closed"
                                                     : "wmux: truncated daemon request payload";
      return std::nullopt;
    }
  }
  return request;
}

bool write_ipc_response(
    HANDLE pipe,
    IpcFrameKind kind,
    std::uint64_t request_id,
    std::string_view payload) {
  return write_all(pipe, make_ipc_frame(kind, request_id, payload));
}

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
  events.set_handler(handle_runtime_event);
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
      std::string frame_error;
      std::uint64_t frame_request_id = 0;
      const auto framed_request = read_control_ipc_request(pipe, frame_error, frame_request_id);
      if (framed_request) {
        const auto parsed = parse_request_json(framed_request->payload);
        std::string response;
        if (!parsed) {
          log_event(
              LogLevel::Warn,
              "daemon.ipc",
              "malformed_request",
              {{"request_id", std::to_string(framed_request->header.request_id)}});
          response = make_response_json(false, "wmux: malformed daemon request\n");
        } else {
          log_event(
              LogLevel::Debug,
              "daemon.ipc",
              "request",
              {{"request_id", std::to_string(framed_request->header.request_id)},
               {"type", parsed->type}});
          try {
            const auto result = events.call_event(DaemonEvent::ipc_command(
                std::nullopt,
                framed_request->header.request_id,
                *parsed));
            response =
                result.has_response
                    ? result.response
                    : make_response_json(false, "wmux: daemon request produced no response\n");
            if (result.request_shutdown) {
              should_stop = true;
            }
          } catch (const std::exception& error) {
            log_event(
                LogLevel::Error,
                "daemon.ipc",
                "request_exception",
                {{"request_id", std::to_string(framed_request->header.request_id)},
                 {"type", parsed->type},
                 {"error", error.what()}});
            response = make_response_json(false, "wmux: daemon request failed internally\n");
          } catch (...) {
            log_event(
                LogLevel::Error,
                "daemon.ipc",
                "request_exception",
                {{"request_id", std::to_string(framed_request->header.request_id)},
                 {"type", parsed->type},
                 {"error", "unknown"}});
            response = make_response_json(false, "wmux: daemon request failed internally\n");
          }
        }
        if (!write_ipc_response(
                pipe,
                parsed ? IpcFrameKind::Control : IpcFrameKind::Error,
                framed_request->header.request_id,
                response)) {
          log_event(LogLevel::Warn, "daemon.ipc", "response_write_failed");
        } else if (parsed && parsed->type == "ServerStatus") {
          log_event(
              LogLevel::Debug,
              "daemon.ipc",
              "response_written",
              {{"request_id", std::to_string(framed_request->header.request_id)},
               {"type", parsed->type},
               {"bytes", std::to_string(response.size())}});
        }
      } else {
        log_event(
            LogLevel::Warn,
            "daemon.ipc",
            "invalid_frame",
            {{"request_id", std::to_string(frame_request_id)}, {"error", frame_error}});
        const auto response = make_response_json(false, with_trailing_newline(frame_error));
        (void)write_ipc_response(pipe, IpcFrameKind::Error, frame_request_id, response);
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

struct FramedIpcRequest {
  IpcFrameHeader header;
  std::string payload;
};

std::string with_trailing_newline(std::string_view value) {
  std::string result{value};
  if (result.empty() || result.back() != '\n') {
    result.push_back('\n');
  }
  return result;
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

bool read_exact(int fd, char* data, std::size_t size) {
  std::size_t total = 0;
  while (total < size) {
    const ssize_t count = recv(fd, data + total, size - total, 0);
    if (count <= 0) {
      return false;
    }
    total += static_cast<std::size_t>(count);
  }
  return true;
}

std::optional<FramedIpcRequest> read_control_ipc_request(
    int fd,
    std::string& error,
    std::uint64_t& request_id) {
  request_id = 0;
  std::array<char, kIpcFrameHeaderSize> raw_header{};
  if (!read_exact(fd, raw_header.data(), raw_header.size())) {
    error = "wmux: failed to read daemon request frame";
    return std::nullopt;
  }

  auto parsed_header =
      parse_ipc_frame_header(std::string_view{raw_header.data(), raw_header.size()});
  request_id = parsed_header.header.request_id;
  if (!parsed_header.ok) {
    error = parsed_header.message;
    return std::nullopt;
  }

  if (parsed_header.header.kind != IpcFrameKind::Control) {
    error = "wmux: command endpoint accepts only control IPC frames";
    return std::nullopt;
  }

  FramedIpcRequest request;
  request.header = parsed_header.header;
  if (request.header.payload_size > 0) {
    request.payload.resize(request.header.payload_size);
    if (!read_exact(fd, request.payload.data(), request.payload.size())) {
      error = "wmux: failed to read daemon request payload";
      return std::nullopt;
    }
  }
  return request;
}

bool write_ipc_response(
    int fd,
    IpcFrameKind kind,
    std::uint64_t request_id,
    std::string_view payload) {
  return write_all(fd, make_ipc_frame(kind, request_id, payload));
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
  events.set_handler(handle_runtime_event);
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

    std::string frame_error;
    std::uint64_t frame_request_id = 0;
    const auto framed_request = read_control_ipc_request(client.get(), frame_error, frame_request_id);
    if (framed_request) {
      const auto parsed = parse_request_json(framed_request->payload);
      if (!parsed) {
        log_event(
            LogLevel::Warn,
            "daemon.ipc",
            "malformed_request",
            {{"request_id", std::to_string(framed_request->header.request_id)}});
      }
      std::string response = make_response_json(false, "wmux: malformed daemon request\n");
      if (parsed) {
        const auto result = events.call_event(DaemonEvent::ipc_command(
            std::nullopt,
            framed_request->header.request_id,
            *parsed));
        response = result.has_response
                       ? result.response
                       : make_response_json(false, "wmux: daemon request produced no response\n");
        if (result.request_shutdown) {
          should_stop = true;
        }
      }
      write_ipc_response(
          client.get(),
          parsed ? IpcFrameKind::Control : IpcFrameKind::Error,
          framed_request->header.request_id,
          response);
    } else {
      log_event(
          LogLevel::Warn,
          "daemon.ipc",
          "invalid_frame",
          {{"request_id", std::to_string(frame_request_id)}, {"error", frame_error}});
      const auto response = make_response_json(false, with_trailing_newline(frame_error));
      write_ipc_response(client.get(), IpcFrameKind::Error, frame_request_id, response);
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
