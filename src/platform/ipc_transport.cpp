#include "wmux/platform/ipc_transport.hpp"

#include "wmux/logging.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <sddl.h>
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
constexpr auto kPipeConnectAttempts = 20;
constexpr auto kPipeConnectSleep = std::chrono::milliseconds{25};

std::uint64_t next_ipc_request_id() {
  static std::atomic_uint64_t next{1};
  return next.fetch_add(1, std::memory_order_relaxed);
}

IpcResponse transport_error(std::string message) {
  IpcResponse response;
  response.ok = false;
  response.message = std::move(message);
  return response;
}

IpcResponse response_from_ipc_frame(
    const IpcFrameParseResult& header,
    std::string payload,
    std::uint64_t request_id) {
  if (!header.ok) {
    log_event(
        LogLevel::Warn,
        "client.ipc",
        "invalid_response_frame",
        {{"error", std::string{ipc_frame_error_name(header.error)}},
         {"message", header.message}});
    return transport_error("wmux: daemon returned an invalid IPC frame\n");
  }

  if (header.header.request_id != request_id) {
    log_event(
        LogLevel::Warn,
        "client.ipc",
        "response_request_id_mismatch",
        {{"expected", std::to_string(request_id)},
         {"actual", std::to_string(header.header.request_id)}});
    return transport_error("wmux: daemon response request id did not match\n");
  }

  if (header.header.kind != IpcFrameKind::Control && header.header.kind != IpcFrameKind::Error) {
    log_event(
        LogLevel::Warn,
        "client.ipc",
        "unexpected_response_kind",
        {{"kind", std::string{ipc_frame_kind_name(header.header.kind)}}});
    return transport_error("wmux: daemon returned an unexpected IPC frame\n");
  }

  if (const auto response = parse_response_json(payload)) {
    return *response;
  }

  log_event(
      LogLevel::Error,
      "client.ipc",
      "invalid_response",
      {{"request_id", std::to_string(request_id)},
       {"bytes", std::to_string(payload.size())},
       {"sample", payload.substr(0, 512)}});
  return transport_error("wmux: daemon returned an invalid response\n");
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

std::string narrow(const std::wstring& value) {
  if (value.empty()) {
    return {};
  }

  const int required = WideCharToMultiByte(
      CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  std::string narrow_value(static_cast<std::size_t>(required), '\0');
  WideCharToMultiByte(
      CP_UTF8,
      0,
      value.data(),
      static_cast<int>(value.size()),
      narrow_value.data(),
      required,
      nullptr,
      nullptr);
  return narrow_value;
}

std::string sanitize_pipe_component(std::string value) {
  for (char& ch : value) {
    const auto byte = static_cast<unsigned char>(ch);
    if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
        (byte >= '0' && byte <= '9') || ch == '-' || ch == '_' || ch == '.') {
      continue;
    }
    ch = '_';
  }
  if (value.empty()) {
    return "unknown-user";
  }
  return value;
}

std::string current_user_pipe_tag() {
  HANDLE token = nullptr;
  if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
    DWORD bytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    if (bytes > 0) {
      std::vector<unsigned char> buffer(bytes);
      if (GetTokenInformation(token, TokenUser, buffer.data(), bytes, &bytes)) {
        const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
        LPWSTR sid_text = nullptr;
        if (ConvertSidToStringSidW(user->User.Sid, &sid_text)) {
          std::wstring sid{sid_text};
          LocalFree(sid_text);
          CloseHandle(token);
          return sanitize_pipe_component(narrow(sid));
        }
      }
    }
    CloseHandle(token);
  }

  std::wstring user_name(256, L'\0');
  DWORD user_name_size = static_cast<DWORD>(user_name.size());
  if (GetUserNameW(user_name.data(), &user_name_size) && user_name_size > 0) {
    user_name.resize(user_name_size - 1);
    return sanitize_pipe_component(narrow(user_name));
  }

  return "unknown-user";
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

HANDLE connect_windows_pipe(const std::wstring& endpoint) {
  for (int attempt = 0; attempt < kPipeConnectAttempts; ++attempt) {
    HANDLE pipe = CreateFileW(
        endpoint.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);

    if (pipe != INVALID_HANDLE_VALUE) {
      return pipe;
    }

    const DWORD error = GetLastError();
    if (error == ERROR_PIPE_BUSY) {
      WaitNamedPipeW(endpoint.c_str(), static_cast<DWORD>(kPipeConnectSleep.count()));
      continue;
    }

    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
      break;
    }

    std::this_thread::sleep_for(kPipeConnectSleep);
  }

  return INVALID_HANDLE_VALUE;
}

bool write_all_windows(HANDLE pipe, std::string_view value) {
  while (!value.empty()) {
    DWORD bytes_written = 0;
    const auto bytes_to_write =
        static_cast<DWORD>(std::min<std::size_t>(value.size(), 64 * 1024));
    const BOOL ok = WriteFile(pipe, value.data(), bytes_to_write, &bytes_written, nullptr);
    if (!ok || bytes_written == 0) {
      return false;
    }
    value.remove_prefix(bytes_written);
  }

  return true;
}

bool read_exact_windows(HANDLE pipe, char* data, std::size_t size) {
  std::size_t total = 0;
  while (total < size) {
    DWORD bytes_read = 0;
    const auto remaining = static_cast<DWORD>(std::min<std::size_t>(size - total, 64 * 1024));
    const BOOL ok = ReadFile(pipe, data + total, remaining, &bytes_read, nullptr);
    if (bytes_read > 0) {
      total += bytes_read;
      if (total == size) {
        return true;
      }
    }
    if (!ok) {
      return false;
    }
    if (bytes_read == 0) {
      return false;
    }
  }
  return true;
}

IpcResponse send_windows_request(std::string_view request_json) {
  const auto endpoint = widen(command_endpoint_name());
  HANDLE pipe = connect_windows_pipe(endpoint);
  if (pipe == INVALID_HANDLE_VALUE) {
    return transport_error("wmux: daemon is not running\n");
  }

  const std::uint64_t request_id = next_ipc_request_id();
  const auto request_frame = make_ipc_frame(IpcFrameKind::Control, request_id, request_json);
  if (!write_all_windows(pipe, request_frame)) {
    CloseHandle(pipe);
    return transport_error("wmux: failed to write daemon request\n");
  }

  std::array<char, kIpcFrameHeaderSize> raw_header{};
  if (!read_exact_windows(pipe, raw_header.data(), raw_header.size())) {
    CloseHandle(pipe);
    return transport_error("wmux: failed to read daemon response\n");
  }

  const auto header =
      parse_ipc_frame_header(std::string_view{raw_header.data(), raw_header.size()});
  std::string payload;
  if (header.ok && header.header.payload_size > 0) {
    payload.resize(header.header.payload_size);
    if (!read_exact_windows(pipe, payload.data(), payload.size())) {
      CloseHandle(pipe);
      return transport_error("wmux: failed to read daemon response payload\n");
    }
  }
  CloseHandle(pipe);

  return response_from_ipc_frame(header, std::move(payload), request_id);
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

  const std::uint64_t request_id = next_ipc_request_id();
  const auto request_frame = make_ipc_frame(IpcFrameKind::Control, request_id, request_json);
  if (!write_all(client.get(), request_frame)) {
    return transport_error("wmux: failed to write daemon request\n");
  }

  std::array<char, kIpcFrameHeaderSize> raw_header{};
  if (!read_exact(client.get(), raw_header.data(), raw_header.size())) {
    return transport_error("wmux: failed to read daemon response\n");
  }

  const auto header =
      parse_ipc_frame_header(std::string_view{raw_header.data(), raw_header.size()});
  std::string payload;
  if (header.ok && header.header.payload_size > 0) {
    payload.resize(header.header.payload_size);
    if (!read_exact(client.get(), payload.data(), payload.size())) {
      return transport_error("wmux: failed to read daemon response payload\n");
    }
  }

  return response_from_ipc_frame(header, std::move(payload), request_id);
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

std::string daemon_not_ready_error(const IpcResponse& last_response) {
  std::ostringstream out;
  out << "wmux: daemon did not become ready\n";
  if (!last_response.message.empty()) {
    out << last_response.message;
  }
  out << "wmux: daemon log: " << log_file_path(LogRole::Daemon).string() << "\n";
  return out.str();
}

}  // namespace

std::string command_endpoint_name() {
#ifdef _WIN32
  return R"(\\.\pipe\wmux-)" + current_user_pipe_tag();
#else
  return "/tmp/wmux-" + std::to_string(getuid()) + ".sock";
#endif
}

std::string attach_endpoint_name() {
#ifdef _WIN32
  return R"(\\.\pipe\wmux-)" + current_user_pipe_tag() + "-attach";
#else
  return "/tmp/wmux-" + std::to_string(getuid()) + "-attach.sock";
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
  log_event(
      LogLevel::Debug,
      "client.daemon",
      "ping_failed",
      {{"message", existing.message}});

  if (!start_daemon_process(executable_path, error)) {
    log_event(LogLevel::Error, "client.daemon", "start_failed", {{"error", error}});
    return false;
  }

  if (!wait_for_daemon()) {
    const auto final_probe = send_ipc_request(make_ping_request_json());
    error = daemon_not_ready_error(final_probe);
    log_event(LogLevel::Error, "client.daemon", "ready_timeout", {{"error", error}});
    return false;
  }

  log_event(LogLevel::Info, "client.daemon", "started");
  return true;
}

}  // namespace wmux
