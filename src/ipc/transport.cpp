#include "wmux/ipc_transport.hpp"

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace wmux {
namespace {

constexpr auto kStartupAttempts = 40;
constexpr auto kStartupSleep = std::chrono::milliseconds{50};

IpcResponse transport_error(std::string message) {
  IpcResponse response;
  response.ok = false;
  response.message = std::move(message);
  return response;
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

std::wstring quote_for_command_line(const std::wstring& value) {
  std::wstring quoted = L"\"";
  for (const wchar_t ch : value) {
    if (ch == L'"') {
      quoted += L'\\';
    }
    quoted += ch;
  }
  quoted += L"\"";
  return quoted;
}

std::filesystem::path current_executable_path(const std::filesystem::path& fallback) {
  std::wstring buffer(32768, L'\0');
  const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (size == 0 || size >= buffer.size()) {
    return std::filesystem::absolute(fallback);
  }
  buffer.resize(size);
  return std::filesystem::path{buffer};
}

IpcResponse send_windows_request(std::string_view request_json) {
  const auto endpoint = widen(command_endpoint_name());
  HANDLE pipe = CreateFileW(
      endpoint.c_str(),
      GENERIC_READ | GENERIC_WRITE,
      0,
      nullptr,
      OPEN_EXISTING,
      0,
      nullptr);

  if (pipe == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PIPE_BUSY) {
    WaitNamedPipeW(endpoint.c_str(), 1000);
    pipe = CreateFileW(
        endpoint.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);
  }

  if (pipe == INVALID_HANDLE_VALUE) {
    return transport_error("wmux: daemon is not running\n");
  }

  DWORD bytes_written = 0;
  const BOOL write_ok = WriteFile(
      pipe,
      request_json.data(),
      static_cast<DWORD>(request_json.size()),
      &bytes_written,
      nullptr);
  if (!write_ok || static_cast<std::size_t>(bytes_written) != request_json.size()) {
    CloseHandle(pipe);
    return transport_error("wmux: failed to write daemon request\n");
  }

  std::string raw_response;
  char buffer[512];
  DWORD bytes_read = 0;
  while (ReadFile(pipe, buffer, sizeof(buffer), &bytes_read, nullptr) && bytes_read > 0) {
    raw_response.append(buffer, buffer + bytes_read);
    if (raw_response.find('\n') != std::string::npos) {
      break;
    }
  }

  CloseHandle(pipe);

  if (const auto response = parse_response_json(raw_response)) {
    return *response;
  }
  return transport_error("wmux: daemon returned an invalid response\n");
}

bool start_daemon_process(const std::filesystem::path& executable_path, std::string& error) {
  const auto actual_path = current_executable_path(executable_path);
  auto command_line = quote_for_command_line(actual_path.wstring()) + L" --daemon";

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};

  const BOOL created = CreateProcessW(
      nullptr,
      command_line.data(),
      nullptr,
      nullptr,
      FALSE,
      CREATE_NO_WINDOW | DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
      nullptr,
      nullptr,
      &startup,
      &process);

  if (!created) {
    error = "wmux: failed to start daemon process\n";
    return false;
  }

  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return true;
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

IpcResponse send_posix_request(std::string_view request_json) {
  Fd client{socket(AF_UNIX, SOCK_STREAM, 0)};
  if (!client) {
    return transport_error("wmux: failed to create daemon socket\n");
  }

  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  const auto endpoint = command_endpoint_name();
  if (endpoint.size() >= sizeof(address.sun_path)) {
    return transport_error("wmux: daemon socket path is too long\n");
  }
  std::strncpy(address.sun_path, endpoint.c_str(), sizeof(address.sun_path) - 1);

  if (connect(client.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    return transport_error("wmux: daemon is not running\n");
  }

  if (!write_all(client.get(), request_json)) {
    return transport_error("wmux: failed to write daemon request\n");
  }

  std::string raw_response;
  char buffer[512];
  while (true) {
    const ssize_t count = recv(client.get(), buffer, sizeof(buffer), 0);
    if (count < 0) {
      return transport_error("wmux: failed to read daemon response\n");
    }
    if (count == 0) {
      break;
    }

    raw_response.append(buffer, buffer + count);
    if (raw_response.find('\n') != std::string::npos) {
      break;
    }
  }

  if (const auto response = parse_response_json(raw_response)) {
    return *response;
  }
  return transport_error("wmux: daemon returned an invalid response\n");
}

bool start_daemon_process(const std::filesystem::path& executable_path, std::string& error) {
  const auto absolute_path = std::filesystem::absolute(executable_path);
  const pid_t pid = fork();
  if (pid < 0) {
    error = "wmux: failed to fork daemon process\n";
    return false;
  }

  if (pid > 0) {
    return true;
  }

  setsid();

  const int null_fd = open("/dev/null", O_RDWR);
  if (null_fd >= 0) {
    dup2(null_fd, STDIN_FILENO);
    dup2(null_fd, STDOUT_FILENO);
    dup2(null_fd, STDERR_FILENO);
    if (null_fd > STDERR_FILENO) {
      close(null_fd);
    }
  }

  execl(absolute_path.c_str(), absolute_path.c_str(), "--daemon", static_cast<char*>(nullptr));
  _exit(127);
}

#endif

bool wait_for_daemon() {
  for (int attempt = 0; attempt < kStartupAttempts; ++attempt) {
    const auto response = send_ipc_request(make_ping_request_json());
    if (response.ok) {
      return true;
    }
    std::this_thread::sleep_for(kStartupSleep);
  }

  return false;
}

}  // namespace

std::string command_endpoint_name() {
#ifdef _WIN32
  return R"(\\.\pipe\wmux)";
#else
  return "/tmp/wmux-" + std::to_string(getuid()) + ".sock";
#endif
}

IpcResponse send_ipc_request(std::string_view request_json) {
#ifdef _WIN32
  return send_windows_request(request_json);
#else
  return send_posix_request(request_json);
#endif
}

bool ensure_daemon_running(const std::filesystem::path& executable_path, std::string& error) {
  const auto existing = send_ipc_request(make_ping_request_json());
  if (existing.ok) {
    return true;
  }

  if (!start_daemon_process(executable_path, error)) {
    return false;
  }

  if (!wait_for_daemon()) {
    error = "wmux: daemon did not become ready\n";
    return false;
  }

  return true;
}

}  // namespace wmux
