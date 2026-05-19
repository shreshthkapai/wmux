#include "wmux/client.hpp"

#include "wmux/ipc_protocol.hpp"
#include "wmux/ipc_transport.hpp"

#include <algorithm>
#include <atomic>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace wmux {
namespace {

#ifdef _WIN32

class UniqueHandle {
 public:
  UniqueHandle() = default;
  explicit UniqueHandle(HANDLE handle) : handle_(handle) {}
  UniqueHandle(const UniqueHandle&) = delete;
  UniqueHandle& operator=(const UniqueHandle&) = delete;

  UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.release()) {}

  UniqueHandle& operator=(UniqueHandle&& other) noexcept {
    if (this != &other) {
      reset(other.release());
    }
    return *this;
  }

  ~UniqueHandle() {
    reset();
  }

  HANDLE get() const {
    return handle_;
  }

  HANDLE release() {
    const auto handle = handle_;
    handle_ = nullptr;
    return handle;
  }

  void reset(HANDLE handle = nullptr) {
    if (valid()) {
      CloseHandle(handle_);
    }
    handle_ = handle;
  }

  bool valid() const {
    return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
  }

 private:
  HANDLE handle_{nullptr};
};

class ConsoleModeGuard {
 public:
  explicit ConsoleModeGuard(HANDLE handle) : handle_(handle) {
    active_ = GetConsoleMode(handle_, &original_mode_) != 0;
  }

  ConsoleModeGuard(const ConsoleModeGuard&) = delete;
  ConsoleModeGuard& operator=(const ConsoleModeGuard&) = delete;

  ~ConsoleModeGuard() {
    if (active_) {
      SetConsoleMode(handle_, original_mode_);
    }
  }

  bool set_mode(DWORD mode) const {
    return active_ && SetConsoleMode(handle_, mode) != 0;
  }

  bool active() const {
    return active_;
  }

  DWORD original_mode() const {
    return original_mode_;
  }

 private:
  HANDLE handle_{nullptr};
  DWORD original_mode_{0};
  bool active_{false};
};

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

bool write_all(HANDLE handle, std::string_view bytes) {
  while (!bytes.empty()) {
    const auto bytes_to_write =
        static_cast<DWORD>(std::min<std::size_t>(bytes.size(), 64 * 1024));
    DWORD bytes_written = 0;
    const BOOL ok = WriteFile(handle, bytes.data(), bytes_to_write, &bytes_written, nullptr);
    if (!ok || bytes_written == 0) {
      return false;
    }

    bytes.remove_prefix(bytes_written);
  }

  return true;
}

bool connect_pipe(UniqueHandle& pipe) {
  const auto endpoint = widen(command_endpoint_name());
  HANDLE raw_pipe = CreateFileW(
      endpoint.c_str(),
      GENERIC_READ | GENERIC_WRITE,
      0,
      nullptr,
      OPEN_EXISTING,
      0,
      nullptr);

  if (raw_pipe == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PIPE_BUSY) {
    WaitNamedPipeW(endpoint.c_str(), 1000);
    raw_pipe = CreateFileW(
        endpoint.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);
  }

  if (raw_pipe == INVALID_HANDLE_VALUE) {
    return false;
  }

  pipe.reset(raw_pipe);
  return true;
}

bool read_response_line(HANDLE pipe, std::string& response) {
  response.clear();
  char ch = '\0';
  DWORD bytes_read = 0;
  while (ReadFile(pipe, &ch, 1, &bytes_read, nullptr) && bytes_read == 1) {
    response.push_back(ch);
    if (ch == '\n') {
      return true;
    }
  }

  return !response.empty();
}

bool send_processed_input(
    HANDLE pipe,
    std::string_view bytes,
    bool& prefix_pending,
    std::atomic_bool& stop_requested,
    HANDLE stop_event) {
  std::string to_send;
  to_send.reserve(bytes.size() + 1);

  for (const char byte : bytes) {
    if (prefix_pending) {
      prefix_pending = false;
      if (byte == 'd') {
        stop_requested = true;
        SetEvent(stop_event);
        return true;
      }

      to_send.push_back('\x02');
      to_send.push_back(byte);
      continue;
    }

    if (byte == '\x02') {
      prefix_pending = true;
      continue;
    }

    to_send.push_back(byte);
  }

  return to_send.empty() || write_all(pipe, to_send);
}

void stream_output(HANDLE pipe, HANDLE output, std::atomic_bool& stop_requested, HANDLE stop_event) {
  char buffer[4096];
  while (!stop_requested) {
    DWORD bytes_read = 0;
    const BOOL ok = ReadFile(pipe, buffer, static_cast<DWORD>(sizeof(buffer)), &bytes_read, nullptr);
    if (!ok || bytes_read == 0) {
      break;
    }

    const std::string_view bytes{buffer, bytes_read};
    if (!write_all(output, bytes)) {
      break;
    }
  }

  stop_requested = true;
  SetEvent(stop_event);
}

#endif

}  // namespace

int run_attach_client(const CommandLine& command) {
#ifdef _WIN32
  UniqueHandle pipe;
  if (!connect_pipe(pipe)) {
    std::cerr << "wmux: daemon is not running\n";
    return 1;
  }

  if (!write_all(pipe.get(), make_command_request_json(command))) {
    std::cerr << "wmux: failed to send attach request\n";
    return 1;
  }

  std::string raw_response;
  if (!read_response_line(pipe.get(), raw_response)) {
    std::cerr << "wmux: daemon returned an invalid attach response\n";
    return 1;
  }

  const auto response = parse_response_json(raw_response);
  if (!response) {
    std::cerr << "wmux: daemon returned an invalid attach response\n";
    return 1;
  }

  if (!response->ok) {
    std::cerr << response->message;
    return 1;
  }

  const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
  const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
  ConsoleModeGuard input_guard{input};
  ConsoleModeGuard output_guard{output};

  if (input_guard.active()) {
    DWORD mode = input_guard.original_mode();
    mode &= ~ENABLE_ECHO_INPUT;
    mode &= ~ENABLE_LINE_INPUT;
    mode &= ~ENABLE_PROCESSED_INPUT;
    mode |= ENABLE_VIRTUAL_TERMINAL_INPUT;
    input_guard.set_mode(mode);
  }

  if (output_guard.active()) {
    DWORD mode = output_guard.original_mode();
    mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    output_guard.set_mode(mode);
  }

  UniqueHandle stop_event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
  if (!stop_event.valid()) {
    std::cerr << "wmux: failed to create attach stop event\n";
    return 1;
  }

  std::atomic_bool stop_requested{false};
  std::thread output_thread{
      [&] { stream_output(pipe.get(), output, stop_requested, stop_event.get()); }};

  bool prefix_pending = false;
  char buffer[512];
  while (!stop_requested) {
    HANDLE handles[] = {input, stop_event.get()};
    const DWORD wait_result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
    if (wait_result == WAIT_OBJECT_0 + 1 || stop_requested) {
      break;
    }
    if (wait_result != WAIT_OBJECT_0) {
      stop_requested = true;
      break;
    }

    DWORD bytes_read = 0;
    const BOOL ok = ReadFile(input, buffer, static_cast<DWORD>(sizeof(buffer)), &bytes_read, nullptr);
    if (!ok || bytes_read == 0) {
      stop_requested = true;
      break;
    }

    if (!send_processed_input(
            pipe.get(),
            std::string_view{buffer, bytes_read},
            prefix_pending,
            stop_requested,
            stop_event.get())) {
      stop_requested = true;
      break;
    }
  }

  stop_requested = true;
  SetEvent(stop_event.get());
  if (output_thread.joinable()) {
    CancelSynchronousIo(output_thread.native_handle());
  }
  pipe.reset();
  if (output_thread.joinable()) {
    output_thread.join();
  }

  return 0;
#else
  const auto response = send_ipc_request(make_command_request_json(command));
  if (response.ok) {
    std::cout << response.message;
    return 0;
  }

  std::cerr << response.message;
  return 1;
#endif
}

}  // namespace wmux
