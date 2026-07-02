#include "wmux/client.hpp"

#include "wmux/attach_input_mode.hpp"
#include "wmux/attach_keymap.hpp"
#include "wmux/ipc_protocol.hpp"
#include "wmux/platform/ipc_transport.hpp"
#include "wmux/logging.hpp"
#include "wmux/mouse_input.hpp"
#include "wmux/resource_limits.hpp"
#include "wmux/platform/terminal_control.hpp"
#include "wmux/terminal_input.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <optional>
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

constexpr auto kAttachConnectAttempts = 40;
constexpr auto kAttachConnectSleep = std::chrono::milliseconds{50};

std::atomic<HANDLE> g_attach_stop_event{nullptr};
std::atomic<DWORD> g_attach_console_control_type{0};
std::atomic_bool g_attach_console_control_requested{false};

bool write_all(HANDLE handle, std::string_view bytes);
bool valid_handle(HANDLE handle);

std::string_view console_control_type_name(DWORD control_type) {
  switch (control_type) {
    case CTRL_C_EVENT:
      return "CTRL_C_EVENT";
    case CTRL_BREAK_EVENT:
      return "CTRL_BREAK_EVENT";
    case CTRL_CLOSE_EVENT:
      return "CTRL_CLOSE_EVENT";
    case CTRL_LOGOFF_EVENT:
      return "CTRL_LOGOFF_EVENT";
    case CTRL_SHUTDOWN_EVENT:
      return "CTRL_SHUTDOWN_EVENT";
    default:
      return "UNKNOWN";
  }
}

void reset_attach_console_control_event() {
  g_attach_console_control_type.store(0, std::memory_order_release);
  g_attach_console_control_requested.store(false, std::memory_order_release);
}

void record_attach_console_control_event(DWORD control_type) {
  g_attach_console_control_type.store(control_type, std::memory_order_release);
  g_attach_console_control_requested.store(true, std::memory_order_release);
}

std::optional<DWORD> take_attach_console_control_event() {
  if (!g_attach_console_control_requested.exchange(false, std::memory_order_acq_rel)) {
    return std::nullopt;
  }
  return g_attach_console_control_type.exchange(0, std::memory_order_acq_rel);
}

void log_attach_console_control_event(std::string_view scope, DWORD control_type) {
  log_event(
      LogLevel::Warn,
      std::string{scope},
      "console_control_event",
      {{"control_type", std::to_string(control_type)},
       {"control_name", std::string{console_control_type_name(control_type)}}});
}

std::uint64_t next_attach_request_id() {
  static std::atomic_uint64_t next{1};
  return next.fetch_add(1, std::memory_order_relaxed);
}

struct TerminalSize {
  std::uint16_t columns{0};
  std::uint16_t rows{0};
};

struct AttachClientSettingsSnapshot {
  bool mouse_enabled{false};
  char prefix_byte{'\x02'};
  std::chrono::milliseconds escape_time{50};
  AttachKeyBindingTable key_bindings;
};

struct AttachClientLiveSettings {
  std::mutex mutex;
  bool mouse_enabled{false};
  std::string prefix{"C-b"};
  std::uint16_t escape_time_ms{50};
  AttachKeyBindingTable key_bindings{default_attach_key_bindings()};

  AttachClientSettingsSnapshot snapshot() {
    std::lock_guard lock(mutex);
    return {
        mouse_enabled,
        control_prefix_byte(prefix),
        std::chrono::milliseconds{escape_time_ms},
        key_bindings};
  }

  void apply(const IpcResponse& response) {
    std::lock_guard lock(mutex);
    mouse_enabled = response.mouse_enabled;
    prefix = response.prefix;
    escape_time_ms = response.escape_time_ms;
    key_bindings = attach_key_bindings_from_overrides(
        parse_serialized_attach_key_binding_overrides(response.key_bindings));
  }
};

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
    if (!active_ || SetConsoleMode(handle_, mode) == 0) {
      return false;
    }
    current_mode_ = mode;
    return true;
  }

  bool active() const {
    return active_;
  }

  DWORD original_mode() const {
    return original_mode_;
  }

  DWORD current_mode() const {
    return current_mode_;
  }

 private:
  HANDLE handle_{nullptr};
  DWORD original_mode_{0};
  mutable DWORD current_mode_{0};
  bool active_{false};
};

class ConsoleOutputCodePageGuard {
 public:
  ConsoleOutputCodePageGuard() {
    original_code_page_ = GetConsoleOutputCP();
    active_ = original_code_page_ != 0 && SetConsoleOutputCP(CP_UTF8) != 0;
  }

  ConsoleOutputCodePageGuard(const ConsoleOutputCodePageGuard&) = delete;
  ConsoleOutputCodePageGuard& operator=(const ConsoleOutputCodePageGuard&) = delete;

  ~ConsoleOutputCodePageGuard() {
    if (active_) {
      SetConsoleOutputCP(original_code_page_);
    }
  }

  bool active() const {
    return active_;
  }

  UINT original_code_page() const {
    return original_code_page_;
  }

 private:
  UINT original_code_page_{0};
  bool active_{false};
};

class TerminalVisualGuard {
 public:
  explicit TerminalVisualGuard(HANDLE output) : output_(output) {
    (void)write_all(output_, terminal_attach_enter_sequence());
  }
  TerminalVisualGuard(const TerminalVisualGuard&) = delete;
  TerminalVisualGuard& operator=(const TerminalVisualGuard&) = delete;

  ~TerminalVisualGuard() {
    write_all(output_, terminal_reset_sequence());
  }

 private:
  HANDLE output_{nullptr};
};

class MouseReportingGuard {
 public:
  MouseReportingGuard(HANDLE output, bool enabled) : output_(output), enabled_(enabled) {
    if (enabled_) {
      active_ = write_all(output_, enable_mouse_reporting_sequence());
    }
  }

  MouseReportingGuard(const MouseReportingGuard&) = delete;
  MouseReportingGuard& operator=(const MouseReportingGuard&) = delete;

  ~MouseReportingGuard() {
    if (enabled_ && active_) {
      write_all(output_, disable_mouse_reporting_sequence());
    }
  }

  bool active() const {
    return active_;
  }

  bool enabled() const {
    return enabled_;
  }

  bool set_enabled(bool enabled) {
    if (enabled == enabled_) {
      return true;
    }

    if (!enabled) {
      bool ok = true;
      if (active_) {
        ok = write_all(output_, disable_mouse_reporting_sequence());
      }
      active_ = false;
      enabled_ = false;
      return ok;
    }

    enabled_ = true;
    active_ = write_all(output_, enable_mouse_reporting_sequence());
    return active_;
  }

 private:
  HANDLE output_{nullptr};
  bool enabled_{false};
  bool active_{false};
};

std::string_view enable_bracketed_paste_sequence() {
  return "\x1b[?2004h";
}

std::string_view disable_bracketed_paste_sequence() {
  return "\x1b[?2004l";
}

class BracketedPasteGuard {
 public:
  explicit BracketedPasteGuard(HANDLE output) : output_(output) {
    active_ = write_all(output_, enable_bracketed_paste_sequence());
  }

  BracketedPasteGuard(const BracketedPasteGuard&) = delete;
  BracketedPasteGuard& operator=(const BracketedPasteGuard&) = delete;

  ~BracketedPasteGuard() {
    if (active_) {
      write_all(output_, disable_bracketed_paste_sequence());
    }
  }

 private:
  HANDLE output_{nullptr};
  bool active_{false};
};

BOOL WINAPI attach_console_ctrl_handler(DWORD control_type) {
  switch (control_type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT: {
      // Keep the Windows control callback signal-only. The attach loop owns
      // detach and RAII terminal restoration on its normal thread.
      record_attach_console_control_event(control_type);
      if (const HANDLE stop_event = g_attach_stop_event.load(); valid_handle(stop_event)) {
        SetEvent(stop_event);
      }
      return TRUE;
    }
    default:
      return FALSE;
  }
}

class AttachConsoleCtrlGuard {
 public:
  explicit AttachConsoleCtrlGuard(HANDLE stop_event) {
    reset_attach_console_control_event();
    g_attach_stop_event.store(stop_event);
    installed_ = SetConsoleCtrlHandler(attach_console_ctrl_handler, TRUE) != 0;
  }

  AttachConsoleCtrlGuard(const AttachConsoleCtrlGuard&) = delete;
  AttachConsoleCtrlGuard& operator=(const AttachConsoleCtrlGuard&) = delete;

  ~AttachConsoleCtrlGuard() {
    if (installed_) {
      SetConsoleCtrlHandler(attach_console_ctrl_handler, FALSE);
    }
    g_attach_stop_event.store(nullptr);
    reset_attach_console_control_event();
  }

 private:
  bool installed_{false};
};

bool valid_handle(HANDLE handle) {
  return handle != nullptr && handle != INVALID_HANDLE_VALUE;
}

std::uint16_t clamp_terminal_dimension(int value) {
  if (value <= 0) {
    return 0;
  }
  return static_cast<std::uint16_t>(
      std::min(value, static_cast<int>(kMaxAttachTerminalColumns)));
}

std::uint16_t clamp_terminal_rows(int value) {
  if (value <= 0) {
    return 0;
  }
  return static_cast<std::uint16_t>(std::min(value, static_cast<int>(kMaxAttachTerminalRows)));
}

TerminalSize current_terminal_size(HANDLE output) {
  CONSOLE_SCREEN_BUFFER_INFO info{};
  if (!GetConsoleScreenBufferInfo(output, &info)) {
    return {};
  }

  const int columns = info.srWindow.Right - info.srWindow.Left + 1;
  const int rows = info.srWindow.Bottom - info.srWindow.Top + 1;
  return {clamp_terminal_dimension(columns), clamp_terminal_rows(rows)};
}

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
  DWORD console_mode = 0;
  if (GetConsoleMode(handle, &console_mode) != 0) {
    if (bytes.empty()) {
      return true;
    }

    const int required = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        bytes.data(),
        static_cast<int>(bytes.size()),
        nullptr,
        0);
    if (required <= 0) {
      return false;
    }

    std::wstring wide(static_cast<std::size_t>(required), L'\0');
    const int converted = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        bytes.data(),
        static_cast<int>(bytes.size()),
        wide.data(),
        required);
    if (converted != required) {
      return false;
    }

    std::wstring_view remaining{wide};
    while (!remaining.empty()) {
      const auto chars_to_write =
          static_cast<DWORD>(std::min<std::size_t>(remaining.size(), 32 * 1024));
      DWORD chars_written = 0;
      const BOOL ok =
          WriteConsoleW(handle, remaining.data(), chars_to_write, &chars_written, nullptr);
      if (!ok || chars_written == 0) {
        return false;
      }

      remaining.remove_prefix(chars_written);
    }

    return true;
  }

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

bool read_exact(HANDLE handle, char* data, std::size_t size) {
  std::size_t total = 0;
  while (total < size) {
    DWORD bytes_read = 0;
    const auto bytes_to_read =
        static_cast<DWORD>(std::min<std::size_t>(size - total, 64 * 1024));
    const BOOL ok = ReadFile(handle, data + total, bytes_to_read, &bytes_read, nullptr);
    if (bytes_read > 0) {
      total += bytes_read;
      if (total == size) {
        return true;
      }
    }
    if (!ok || bytes_read == 0) {
      return false;
    }
  }

  return true;
}

bool wait_for_pipe_io(
    HANDLE pipe,
    OVERLAPPED& overlapped,
    HANDLE event,
    const std::atomic_bool* stop_requested,
    HANDLE stop_event,
    DWORD& bytes_transferred) {
  while (true) {
    HANDLE wait_handles[2] = {event, stop_event};
    const DWORD handle_count = valid_handle(stop_event) ? 2 : 1;
    const DWORD wait_result = WaitForMultipleObjects(handle_count, wait_handles, FALSE, 50);
    if (wait_result == WAIT_OBJECT_0) {
      return GetOverlappedResult(pipe, &overlapped, &bytes_transferred, FALSE) != 0;
    }
    if (handle_count == 2 && wait_result == WAIT_OBJECT_0 + 1) {
      CancelIoEx(pipe, &overlapped);
      WaitForSingleObject(event, 1000);
      return false;
    }
    if (wait_result != WAIT_TIMEOUT) {
      CancelIoEx(pipe, &overlapped);
      WaitForSingleObject(event, 1000);
      return false;
    }
    if (stop_requested != nullptr && stop_requested->load(std::memory_order_relaxed)) {
      CancelIoEx(pipe, &overlapped);
      WaitForSingleObject(event, 1000);
      return false;
    }
  }
}

bool read_pipe_exact(
    HANDLE pipe,
    char* data,
    std::size_t size,
    const std::atomic_bool* stop_requested = nullptr,
    HANDLE stop_event = nullptr) {
  std::size_t total = 0;
  while (total < size) {
    UniqueHandle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!event.valid()) {
      return false;
    }

    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    DWORD bytes_read = 0;
    const auto bytes_to_read =
        static_cast<DWORD>(std::min<std::size_t>(size - total, 64 * 1024));
    const BOOL ok = ReadFile(pipe, data + total, bytes_to_read, nullptr, &overlapped);
    if (!ok) {
      const DWORD error = GetLastError();
      if (error != ERROR_IO_PENDING) {
        return false;
      }
      if (!wait_for_pipe_io(pipe, overlapped, event.get(), stop_requested, stop_event, bytes_read)) {
        return false;
      }
    } else if (!GetOverlappedResult(pipe, &overlapped, &bytes_read, FALSE)) {
      return false;
    }

    if (bytes_read == 0) {
      return false;
    }
    total += bytes_read;
  }

  return true;
}

bool write_pipe_all(
    HANDLE pipe,
    std::string_view bytes,
    const std::atomic_bool* stop_requested = nullptr,
    HANDLE stop_event = nullptr) {
  while (!bytes.empty()) {
    UniqueHandle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!event.valid()) {
      return false;
    }

    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    DWORD bytes_written = 0;
    const auto bytes_to_write =
        static_cast<DWORD>(std::min<std::size_t>(bytes.size(), 64 * 1024));
    const BOOL ok = WriteFile(pipe, bytes.data(), bytes_to_write, nullptr, &overlapped);
    if (!ok) {
      const DWORD error = GetLastError();
      if (error != ERROR_IO_PENDING) {
        return false;
      }
      if (!wait_for_pipe_io(
              pipe, overlapped, event.get(), stop_requested, stop_event, bytes_written)) {
        return false;
      }
    } else if (!GetOverlappedResult(pipe, &overlapped, &bytes_written, FALSE)) {
      return false;
    }

    if (bytes_written == 0) {
      return false;
    }
    bytes.remove_prefix(bytes_written);
  }

  return true;
}

bool read_ipc_frame(
    HANDLE pipe,
    IpcFrameParseResult& frame,
    const std::atomic_bool* stop_requested = nullptr,
    HANDLE stop_event = nullptr) {
  std::array<char, kIpcFrameHeaderSize> raw_header{};
  if (!read_pipe_exact(pipe, raw_header.data(), raw_header.size(), stop_requested, stop_event)) {
    return false;
  }

  frame = parse_ipc_frame_header(std::string_view{raw_header.data(), raw_header.size()});
  if (!frame.ok) {
    return true;
  }

  frame.payload.clear();
  if (frame.header.payload_size == 0) {
    return true;
  }

  frame.payload.resize(frame.header.payload_size);
  return read_pipe_exact(
      pipe,
      frame.payload.data(),
      frame.payload.size(),
      stop_requested,
      stop_event);
}

bool read_response_frame(HANDLE pipe, std::uint64_t request_id, IpcResponse& response) {
  IpcFrameParseResult frame;
  if (!read_ipc_frame(pipe, frame)) {
    return false;
  }

  if (!frame.ok) {
    log_event(
        LogLevel::Warn,
        "client.attach",
        "invalid_response_frame",
        {{"request_id", std::to_string(request_id)},
         {"error", std::string{ipc_frame_error_name(frame.error)}}});
    return false;
  }

  if (frame.header.request_id != request_id) {
    log_event(
        LogLevel::Warn,
        "client.attach",
        "response_request_id_mismatch",
        {{"expected", std::to_string(request_id)},
         {"actual", std::to_string(frame.header.request_id)}});
    return false;
  }

  if (frame.header.kind != IpcFrameKind::Control && frame.header.kind != IpcFrameKind::Error) {
    log_event(
        LogLevel::Warn,
        "client.attach",
        "unexpected_response_kind",
        {{"request_id", std::to_string(request_id)},
         {"kind", std::string{ipc_frame_kind_name(frame.header.kind)}}});
    return false;
  }

  const auto parsed = parse_response_json(frame.payload);
  if (!parsed) {
    return false;
  }
  response = *parsed;
  return true;
}

bool write_console_wide_all(HANDLE handle, std::wstring_view text) {
  while (!text.empty()) {
    const auto chars_to_write =
        static_cast<DWORD>(std::min<std::size_t>(text.size(), 32 * 1024));
    DWORD chars_written = 0;
    const BOOL ok =
        WriteConsoleW(handle, text.data(), chars_to_write, &chars_written, nullptr);
    if (!ok || chars_written == 0) {
      return false;
    }

    text.remove_prefix(chars_written);
  }

  return true;
}

std::size_t complete_utf8_prefix_size(std::string_view bytes) {
  std::size_t index = 0;
  std::size_t valid_end = 0;
  while (index < bytes.size()) {
    const auto byte = static_cast<unsigned char>(bytes[index]);
    std::size_t sequence_size = 0;
    if (byte < 0x80) {
      sequence_size = 1;
    } else if (byte >= 0xc2 && byte <= 0xdf) {
      sequence_size = 2;
    } else if (byte >= 0xe0 && byte <= 0xef) {
      sequence_size = 3;
    } else if (byte >= 0xf0 && byte <= 0xf4) {
      sequence_size = 4;
    } else {
      return std::string_view::npos;
    }

    if (index + sequence_size > bytes.size()) {
      break;
    }

    for (std::size_t offset = 1; offset < sequence_size; ++offset) {
      const auto continuation = static_cast<unsigned char>(bytes[index + offset]);
      if ((continuation & 0xc0) != 0x80) {
        return std::string_view::npos;
      }
    }

    index += sequence_size;
    valid_end = index;
  }

  return valid_end;
}

bool write_console_utf8_all(HANDLE handle, std::string_view bytes, std::string& pending_utf8) {
  std::string combined;
  if (!pending_utf8.empty()) {
    combined.reserve(pending_utf8.size() + bytes.size());
    combined.append(pending_utf8);
    combined.append(bytes);
    bytes = combined;
  }

  const auto complete_size = complete_utf8_prefix_size(bytes);
  if (complete_size == std::string_view::npos) {
    return false;
  }

  pending_utf8.assign(bytes.substr(complete_size));
  if (complete_size == 0) {
    return true;
  }

  const auto complete = bytes.substr(0, complete_size);
  const int required = MultiByteToWideChar(
      CP_UTF8,
      MB_ERR_INVALID_CHARS,
      complete.data(),
      static_cast<int>(complete.size()),
      nullptr,
      0);
  if (required <= 0) {
    return false;
  }

  std::wstring wide(static_cast<std::size_t>(required), L'\0');
  const int converted = MultiByteToWideChar(
      CP_UTF8,
      MB_ERR_INVALID_CHARS,
      complete.data(),
      static_cast<int>(complete.size()),
      wide.data(),
      required);
  if (converted != required) {
    return false;
  }

  return write_console_wide_all(handle, wide);
}

bool connect_pipe(UniqueHandle& pipe) {
  const auto endpoint = widen(attach_endpoint_name());
  for (int attempt = 0; attempt < kAttachConnectAttempts; ++attempt) {
    HANDLE raw_pipe = CreateFileW(
        endpoint.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED,
        nullptr);

    if (raw_pipe != INVALID_HANDLE_VALUE) {
      pipe.reset(raw_pipe);
      return true;
    }

    const DWORD error = GetLastError();
    if (error == ERROR_PIPE_BUSY) {
      WaitNamedPipeW(endpoint.c_str(), static_cast<DWORD>(kAttachConnectSleep.count()));
      continue;
    }

    std::this_thread::sleep_for(kAttachConnectSleep);
  }

  return false;
}

bool write_attach_frame(HANDLE pipe, std::string_view frame) {
  const auto request_id = next_attach_request_id();
  return write_pipe_all(pipe, make_ipc_frame(IpcFrameKind::AttachInput, request_id, frame));
}

bool send_attach_input(HANDLE pipe, std::string_view bytes) {
  while (!bytes.empty()) {
    const auto chunk_size = std::min<std::size_t>(bytes.size(), kMaxAttachInputPayloadBytes);
    if (!write_attach_frame(pipe, make_attach_input_frame(bytes.substr(0, chunk_size)))) {
      return false;
    }
    bytes.remove_prefix(chunk_size);
  }

  return true;
}

bool send_attach_detach(HANDLE pipe) {
  return write_attach_frame(pipe, make_attach_detach_frame());
}

bool send_attach_command(HANDLE pipe, std::string_view command) {
  if (command.size() > kMaxAttachCommandPayloadBytes) {
    log_event(
        LogLevel::Warn,
        "client.keybind",
        "attach_command_too_large",
        {{"command", std::string{command}}, {"bytes", std::to_string(command.size())}});
    return false;
  }
  const bool sent = write_attach_frame(pipe, make_attach_command_frame(command));
  log_event(
      sent ? LogLevel::Info : LogLevel::Error,
      "client.keybind",
      "send_attach_command",
      {{"command", std::string{command}}, {"ok", sent ? "true" : "false"}});
  return sent;
}

bool send_attach_command_mode(HANDLE pipe, std::string_view command) {
  if (command.size() > kMaxAttachCommandPayloadBytes) {
    return false;
  }
  return write_attach_frame(pipe, make_attach_command_mode_frame(command));
}

bool send_attach_resize(HANDLE pipe, TerminalSize size) {
  if (size.columns == 0 || size.rows == 0) {
    return true;
  }

  return write_attach_frame(pipe, make_attach_resize_frame(size.columns, size.rows));
}

bool send_attach_status(HANDLE pipe, std::string_view status) {
  if (status.size() > kMaxAttachCommandPayloadBytes) {
    status = status.substr(0, kMaxAttachCommandPayloadBytes);
  }
  return write_attach_frame(pipe, make_attach_status_frame(status));
}

AttachMouseButton attach_mouse_button(MouseButton button) {
  switch (button) {
    case MouseButton::Left:
      return AttachMouseButton::Left;
    case MouseButton::Middle:
      return AttachMouseButton::Middle;
    case MouseButton::Right:
      return AttachMouseButton::Right;
    case MouseButton::Release:
      return AttachMouseButton::Release;
    case MouseButton::WheelUp:
      return AttachMouseButton::WheelUp;
    case MouseButton::WheelDown:
      return AttachMouseButton::WheelDown;
    case MouseButton::Other:
      return AttachMouseButton::Other;
  }

  return AttachMouseButton::Other;
}

AttachMouseAction attach_mouse_action(MouseAction action) {
  switch (action) {
    case MouseAction::Press:
      return AttachMouseAction::Press;
    case MouseAction::Release:
      return AttachMouseAction::Release;
    case MouseAction::Drag:
      return AttachMouseAction::Drag;
    case MouseAction::Wheel:
      return AttachMouseAction::Wheel;
  }

  return AttachMouseAction::Press;
}

bool send_attach_mouse_event(HANDLE pipe, const MouseEvent& event) {
  if (event.column == 0 || event.row == 0) {
    return true;
  }

  const AttachMouseEventPayload payload{
      event.column,
      event.row,
      static_cast<std::uint16_t>(std::clamp(event.button_code, 0, 65535)),
      attach_mouse_button(event.button),
      attach_mouse_action(event.action)};
  return write_attach_frame(pipe, make_attach_mouse_event_frame(payload));
}

bool send_attach_scroll(HANDLE pipe, AttachScrollAction action) {
  return write_attach_frame(pipe, make_attach_scroll_frame(action));
}

bool send_attach_copy_mode(HANDLE pipe, AttachCopyModeAction action) {
  return write_attach_frame(pipe, make_attach_copy_mode_frame(action));
}

bool send_attach_paste(HANDLE pipe) {
  return write_attach_frame(pipe, make_attach_paste_frame());
}

bool same_terminal_size(TerminalSize lhs, TerminalSize rhs) {
  return lhs.columns == rhs.columns && lhs.rows == rhs.rows;
}

bool send_attach_action(
    HANDLE pipe,
    const AttachInputAction& action,
    std::string& to_send,
    std::atomic_bool& stop_requested,
    HANDLE stop_event) {
  const auto flush_input = [&] {
    if (to_send.empty()) {
      return true;
    }

    const bool sent = send_attach_input(pipe, to_send);
    to_send.clear();
    return sent;
  };

  switch (action.kind) {
    case AttachInputActionKind::None:
      return true;
    case AttachInputActionKind::SendInput:
      to_send += action.text;
      return true;
    case AttachInputActionKind::Detach: {
      if (!flush_input()) {
        return false;
      }
      log_event(
          LogLevel::Info,
          "client.keybind",
          "send_detach");
      const bool sent = send_attach_detach(pipe);
      stop_requested = true;
      SetEvent(stop_event);
      return sent;
    }
    case AttachInputActionKind::Command:
      return flush_input() && send_attach_command(pipe, action.text);
    case AttachInputActionKind::CommandMode:
      return flush_input() && send_attach_command_mode(pipe, action.text);
    case AttachInputActionKind::Status:
      return flush_input() && send_attach_status(pipe, action.text);
    case AttachInputActionKind::Scroll:
      return flush_input() && send_attach_scroll(pipe, action.scroll_action);
    case AttachInputActionKind::CopyMode:
      return flush_input() && send_attach_copy_mode(pipe, action.copy_mode_action);
    case AttachInputActionKind::Paste:
      return flush_input() && send_attach_paste(pipe);
    case AttachInputActionKind::Mouse:
      return flush_input() && send_attach_mouse_event(pipe, action.mouse);
  }

  return true;
}

bool send_processed_input_events(
    HANDLE pipe,
    const std::vector<TerminalInputEvent>& events,
    AttachClientModeState& mode,
    char prefix_byte,
    const AttachKeyBindingTable& key_bindings,
    std::atomic_bool& stop_requested,
    HANDLE stop_event) {
  std::string to_send;
  for (const auto& event : events) {
    const auto previous_mode = mode.kind;
    const auto actions = handle_attach_input_event(mode, event, prefix_byte, key_bindings);
    if (previous_mode != mode.kind) {
      log_event(
          LogLevel::Debug,
          "client.input",
          "mode_transition",
          {{"from", attach_client_mode_name(previous_mode)}, {"to", attach_client_mode_name(mode.kind)}});
    }

    log_event(
        LogLevel::Debug,
        "client.input",
        "decoded",
        {{"event", terminal_input_event_debug_name(event)}, {"mode", attach_client_mode_name(mode.kind)}});

    for (const auto& action : actions) {
      if (action.kind == AttachInputActionKind::Command) {
        log_event(
            LogLevel::Info,
            "client.keybind",
            "command",
            {{"command", action.text}});
      }
      if (!send_attach_action(pipe, action, to_send, stop_requested, stop_event)) {
        return false;
      }
      if (stop_requested) {
        return true;
      }
    }
  }

  return send_attach_input(pipe, to_send);
}

void stream_output(
    HANDLE pipe,
    HANDLE output,
    AttachClientLiveSettings& settings,
    MouseReportingGuard& mouse_reporting,
    std::atomic_bool& stop_requested,
    HANDLE stop_event) {
  DWORD output_mode = 0;
  const bool output_is_console = GetConsoleMode(output, &output_mode) != 0;
  std::string pending_utf8;
  while (!stop_requested) {
    IpcFrameParseResult frame;
    if (!read_ipc_frame(pipe, frame, &stop_requested, stop_event)) {
      break;
    }

    if (!frame.ok) {
      log_event(
          LogLevel::Warn,
          "client.attach",
          "invalid_output_frame",
          {{"error", std::string{ipc_frame_error_name(frame.error)}}});
      break;
    }

    if (frame.header.kind == IpcFrameKind::Error) {
      const auto response = parse_response_json(frame.payload);
      if (response && !response->message.empty()) {
        (void)write_all(output, response->message);
      }
      break;
    }

    if (frame.header.kind == IpcFrameKind::Event) {
      const auto response = parse_response_json(frame.payload);
      if (!response || !response->ok || response->message != "settings") {
        log_event(
            LogLevel::Warn,
            "client.attach",
            "invalid_event_frame");
        continue;
      }

      const bool mouse_updated = mouse_reporting.set_enabled(response->mouse_enabled);
      IpcResponse applied = *response;
      if (!mouse_updated) {
        applied.mouse_enabled = false;
        log_event(
            LogLevel::Warn,
            "client.attach",
            "mouse_update_failed");
      }
      settings.apply(applied);
      log_event(
          LogLevel::Info,
          "client.attach",
          "settings_updated",
          {{"mouse", applied.mouse_enabled ? "on" : "off"},
           {"prefix", applied.prefix},
           {"status", applied.status_bar_enabled ? "on" : "off"},
           {"escape_time_ms", std::to_string(applied.escape_time_ms)}});
      continue;
    }

    if (frame.header.kind != IpcFrameKind::AttachOutput) {
      log_event(
          LogLevel::Warn,
          "client.attach",
          "unexpected_output_frame_kind",
          {{"kind", std::string{ipc_frame_kind_name(frame.header.kind)}}});
      break;
    }

    const std::string_view bytes{frame.payload};
    const bool written = output_is_console ? write_console_utf8_all(output, bytes, pending_utf8)
                                           : write_all(output, bytes);
    if (!written) {
      break;
    }
  }

  stop_requested = true;
  SetEvent(stop_event);
}

void stream_input(
    HANDLE input,
    HANDLE pipe,
    std::mutex& pipe_write_mutex,
    AttachClientLiveSettings& settings,
    std::atomic_bool& stop_requested,
    HANDLE stop_event) {
  TerminalInputDecoderState decoder;
  TerminalInputDecoderOptions decoder_options;
  AttachClientModeState mode;

  std::array<char, 4096> buffer{};
  while (!stop_requested) {
    const auto current_settings = settings.snapshot();
    decoder_options.mouse_enabled = current_settings.mouse_enabled;
    decoder_options.escape_time = current_settings.escape_time;
    if (!decoder.pending.empty()) {
      const auto age = terminal_input_pending_age(decoder, std::chrono::steady_clock::now());
      if (age >= decoder_options.escape_time) {
        const auto flushed = flush_terminal_input_decoder(decoder);
        std::lock_guard lock(pipe_write_mutex);
        if (!send_processed_input_events(
                pipe,
                flushed.events,
                mode,
                current_settings.prefix_byte,
                current_settings.key_bindings,
                stop_requested,
                stop_event)) {
          break;
        }
        continue;
      }
    }

    DWORD wait_timeout = INFINITE;
    if (!decoder.pending.empty()) {
      const auto age = terminal_input_pending_age(decoder, std::chrono::steady_clock::now());
      const auto remaining =
          age >= decoder_options.escape_time ? std::chrono::milliseconds{0}
                                             : decoder_options.escape_time - age;
      wait_timeout = static_cast<DWORD>(std::max<std::int64_t>(0, remaining.count()));
    }

    HANDLE wait_handles[2] = {stop_event, input};
    const DWORD wait_result = WaitForMultipleObjects(2, wait_handles, FALSE, wait_timeout);
    if (wait_result == WAIT_OBJECT_0 || stop_requested) {
      break;
    }
    if (wait_result == WAIT_TIMEOUT) {
      const auto flushed = flush_terminal_input_decoder(decoder);
      std::lock_guard lock(pipe_write_mutex);
      if (!send_processed_input_events(
                pipe,
                flushed.events,
                mode,
                current_settings.prefix_byte,
                current_settings.key_bindings,
                stop_requested,
                stop_event)) {
        break;
      }
      continue;
    }
    if (wait_result != WAIT_OBJECT_0 + 1) {
      break;
    }

    DWORD bytes_read = 0;
    const BOOL ok =
        ReadFile(input, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes_read, nullptr);
    if (!ok || bytes_read == 0) {
      break;
    }

    const auto decoded = decode_terminal_input(
        decoder,
        std::string_view{buffer.data(), bytes_read},
        decoder_options);
    std::lock_guard lock(pipe_write_mutex);
    if (!send_processed_input_events(
            pipe,
            decoded.events,
            mode,
            current_settings.prefix_byte,
            current_settings.key_bindings,
            stop_requested,
            stop_event)) {
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
  log_event(
      LogLevel::Info,
      "client.attach",
      "start",
      {{"session_name", command.session_name}});

  const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
  const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
  if (!valid_handle(input) || !valid_handle(output)) {
    std::cerr << "wmux: attach requires valid standard input and output handles\n";
    return 1;
  }

  ConsoleModeGuard input_guard{input};
  ConsoleModeGuard output_guard{output};
  ConsoleOutputCodePageGuard output_code_page_guard;
  if (!input_guard.active() || !output_guard.active()) {
    std::cerr << "wmux: attach requires an interactive Windows console\n";
    return 1;
  }

  const auto size = current_terminal_size(output);

  UniqueHandle pipe;
  if (!connect_pipe(pipe)) {
    const auto ping = send_ipc_request(make_ping_request_json());
    log_event(
        ping.ok ? LogLevel::Warn : LogLevel::Error,
        "client.attach",
        "connect_failed",
        {{"daemon_ping", ping.ok ? "ok" : "failed"}, {"message", ping.message}});
    if (ping.ok) {
      std::cerr << "wmux: daemon attach endpoint is unavailable; try again or run "
                   "'wmux server stop --force' if this persists\n";
    } else {
      std::cerr << "wmux: daemon stopped before attach could start\n";
    }
    return 1;
  }

  const std::uint64_t attach_request_id = next_attach_request_id();
  const auto attach_request = make_ipc_frame(
      IpcFrameKind::Control,
      attach_request_id,
      make_attach_request_json(command, size.columns, size.rows));
  if (!write_pipe_all(pipe.get(), attach_request)) {
    log_event(
        LogLevel::Error,
        "client.attach",
        "request_write_failed",
        {{"session_name", command.session_name}});
    std::cerr << "wmux: failed to send attach request\n";
    return 1;
  }

  IpcResponse response;
  if (!read_response_frame(pipe.get(), attach_request_id, response)) {
    const auto ping = send_ipc_request(make_ping_request_json());
    log_event(
        LogLevel::Error,
        "client.attach",
        "response_read_failed",
        {{"session_name", command.session_name}, {"daemon_ping", ping.ok ? "ok" : "failed"}});
    std::cerr << "wmux: daemon returned an invalid attach response\n";
    return 1;
  }

  if (!response.ok) {
    log_event(
        LogLevel::Warn,
        "client.attach",
        "rejected",
        {{"session_name", command.session_name}, {"message", response.message}});
    std::cerr << response.message;
    return 1;
  }

  UniqueHandle stop_event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
  if (!stop_event.valid()) {
    std::cerr << "wmux: failed to create attach stop event\n";
    return 1;
  }
  AttachConsoleCtrlGuard console_ctrl_guard{stop_event.get()};

  {
    DWORD mode = input_guard.original_mode();
    mode &= ~ENABLE_ECHO_INPUT;
    mode &= ~ENABLE_LINE_INPUT;
    mode &= ~ENABLE_PROCESSED_INPUT;
    mode &= ~ENABLE_QUICK_EDIT_MODE;
    mode |= ENABLE_VIRTUAL_TERMINAL_INPUT;
    mode |= ENABLE_EXTENDED_FLAGS;
    if (!input_guard.set_mode(mode)) {
      std::cerr << "wmux: failed to enter raw input mode\n";
      return 1;
    }
  }

  {
    DWORD mode = output_guard.original_mode();
    mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    if (!output_guard.set_mode(mode)) {
      std::cerr << "wmux: failed to enable virtual terminal output mode\n";
      return 1;
    }
  }

  TerminalVisualGuard terminal_visual_guard{output};
  MouseReportingGuard mouse_reporting{output, response.mouse_enabled};
  if (mouse_reporting.enabled() && !mouse_reporting.active()) {
    std::cerr << "wmux: failed to enable mouse reporting\n";
    return 1;
  }
  BracketedPasteGuard bracketed_paste{output};
  AttachClientLiveSettings live_settings;
  live_settings.apply(response);

  std::atomic_bool stop_requested{false};
  std::mutex pipe_write_mutex;
  std::thread output_thread{
      [&] {
        stream_output(
            pipe.get(),
            output,
            live_settings,
            mouse_reporting,
            stop_requested,
            stop_event.get());
      }};
  std::thread input_thread{[&] {
    stream_input(
        input,
        pipe.get(),
        pipe_write_mutex,
        live_settings,
        stop_requested,
        stop_event.get());
  }};

  TerminalSize last_size = size;
  while (!stop_requested) {
    const DWORD wait_result = WaitForSingleObject(stop_event.get(), 100);
    const auto latest_size = current_terminal_size(output);
    if (!same_terminal_size(latest_size, last_size) && latest_size.columns > 0 &&
        latest_size.rows > 0) {
      std::lock_guard lock(pipe_write_mutex);
      if (!send_attach_resize(pipe.get(), latest_size)) {
        stop_requested = true;
        break;
      }
      last_size = latest_size;
    }

    if (wait_result == WAIT_OBJECT_0 || stop_requested) {
      break;
    }
    if (wait_result != WAIT_TIMEOUT) {
      stop_requested = true;
      break;
    }
  }

  if (const auto control_type = take_attach_console_control_event()) {
    log_attach_console_control_event("client.attach", *control_type);
    std::lock_guard lock(pipe_write_mutex);
    (void)send_attach_detach(pipe.get());
  }

  stop_requested = true;
  SetEvent(stop_event.get());
  if (input_thread.joinable()) {
    CancelSynchronousIo(input_thread.native_handle());
  }
  if (output_thread.joinable()) {
    CancelSynchronousIo(output_thread.native_handle());
  }
  pipe.reset();
  if (input_thread.joinable()) {
    input_thread.join();
  }
  if (output_thread.joinable()) {
    output_thread.join();
  }

  log_event(
      LogLevel::Info,
      "client.attach",
      "stop",
      {{"session_name", command.session_name}});
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

int run_debug_keys() {
#ifdef _WIN32
  const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
  const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
  if (!valid_handle(input) || !valid_handle(output)) {
    std::cerr << "wmux: debug-keys requires valid standard input and output handles\n";
    return 1;
  }

  ConsoleModeGuard input_guard{input};
  ConsoleModeGuard output_guard{output};
  ConsoleOutputCodePageGuard output_code_page_guard;
  if (!input_guard.active() || !output_guard.active()) {
    std::cerr << "wmux: debug-keys requires an interactive Windows console\n";
    return 1;
  }

  UniqueHandle stop_event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
  if (!stop_event.valid()) {
    std::cerr << "wmux: failed to create debug-keys stop event\n";
    return 1;
  }
  AttachConsoleCtrlGuard console_ctrl_guard{stop_event.get()};

  {
    DWORD mode = input_guard.original_mode();
    mode &= ~ENABLE_ECHO_INPUT;
    mode &= ~ENABLE_LINE_INPUT;
    mode &= ~ENABLE_PROCESSED_INPUT;
    mode &= ~ENABLE_QUICK_EDIT_MODE;
    mode |= ENABLE_VIRTUAL_TERMINAL_INPUT;
    mode |= ENABLE_EXTENDED_FLAGS;
    if (!input_guard.set_mode(mode)) {
      std::cerr << "wmux: failed to enter raw input mode\n";
      return 1;
    }
  }

  {
    DWORD mode = output_guard.original_mode();
    mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    if (!output_guard.set_mode(mode)) {
      std::cerr << "wmux: failed to enable virtual terminal output mode\n";
      return 1;
    }
  }

  MouseReportingGuard mouse_reporting{output, true};
  BracketedPasteGuard bracketed_paste{output};
  TerminalInputDecoderState decoder;
  TerminalInputDecoderOptions options;
  options.mouse_enabled = true;
  options.escape_time = std::chrono::milliseconds{50};

  std::cout << "wmux debug-keys\n";
  std::cout << "Press Escape or Ctrl+C to exit. Paste content is reported by byte length only.\n";
  std::cout.flush();

  std::array<char, 4096> buffer{};
  bool stop = false;
  while (!stop) {
    if (!decoder.pending.empty()) {
      const auto age = terminal_input_pending_age(decoder, std::chrono::steady_clock::now());
      if (age >= options.escape_time) {
        const auto flushed = flush_terminal_input_decoder(decoder);
        for (const auto& event : flushed.events) {
          std::cout << terminal_input_event_debug_name(event) << "\n";
          if (event.kind == TerminalInputEventKind::Key && event.key.key == Key::Escape) {
            stop = true;
          }
        }
        std::cout.flush();
        continue;
      }
    }

    DWORD wait_timeout = INFINITE;
    if (!decoder.pending.empty()) {
      const auto age = terminal_input_pending_age(decoder, std::chrono::steady_clock::now());
      const auto remaining =
          age >= options.escape_time ? std::chrono::milliseconds{0} : options.escape_time - age;
      wait_timeout = static_cast<DWORD>(std::max<std::int64_t>(0, remaining.count()));
    }

    HANDLE wait_handles[2] = {stop_event.get(), input};
    const DWORD wait_result = WaitForMultipleObjects(2, wait_handles, FALSE, wait_timeout);
    if (wait_result == WAIT_OBJECT_0) {
      break;
    }
    if (wait_result == WAIT_TIMEOUT) {
      const auto flushed = flush_terminal_input_decoder(decoder);
      for (const auto& event : flushed.events) {
        std::cout << terminal_input_event_debug_name(event) << "\n";
        if (event.kind == TerminalInputEventKind::Key && event.key.key == Key::Escape) {
          stop = true;
        }
      }
      std::cout.flush();
      continue;
    }
    if (wait_result != WAIT_OBJECT_0 + 1) {
      break;
    }

    DWORD bytes_read = 0;
    const BOOL ok =
        ReadFile(input, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes_read, nullptr);
    if (!ok || bytes_read == 0) {
      break;
    }

    const auto decoded =
        decode_terminal_input(decoder, std::string_view{buffer.data(), bytes_read}, options);
    for (const auto& event : decoded.events) {
      std::cout << terminal_input_event_debug_name(event) << "\n";
      if (event.kind != TerminalInputEventKind::Key) {
        continue;
      }
      const bool ctrl_c = event.key.key == Key::Char && event.key.character == 'c' &&
                          has_modifier(event.key.modifiers, KeyModifier::Ctrl);
      if (event.key.key == Key::Escape || ctrl_c) {
        stop = true;
      }
    }
    std::cout.flush();
  }

  if (const auto control_type = take_attach_console_control_event()) {
    log_attach_console_control_event("client.debug_keys", *control_type);
  }

  return 0;
#else
  std::cerr << "wmux: debug-keys is only available on Windows\n";
  return 1;
#endif
}

}  // namespace wmux
