#include "wmux/client.hpp"

#include "wmux/command_mode.hpp"
#include "wmux/ipc_protocol.hpp"
#include "wmux/ipc_transport.hpp"
#include "wmux/mouse_input.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
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

bool write_all(HANDLE handle, std::string_view bytes);

struct TerminalSize {
  std::uint16_t columns{0};
  std::uint16_t rows{0};
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

class MouseReportingGuard {
 public:
  MouseReportingGuard(HANDLE output, bool enabled) : output_(output), enabled_(enabled) {
    if (!enabled_) {
      return;
    }

    active_ = write_all(output_, enable_mouse_reporting_sequence());
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

 private:
  HANDLE output_{nullptr};
  bool enabled_{false};
  bool active_{false};
};

bool valid_handle(HANDLE handle) {
  return handle != nullptr && handle != INVALID_HANDLE_VALUE;
}

std::uint16_t clamp_terminal_dimension(int value) {
  if (value <= 0) {
    return 0;
  }
  return static_cast<std::uint16_t>(std::min(value, 32767));
}

TerminalSize current_terminal_size(HANDLE output) {
  CONSOLE_SCREEN_BUFFER_INFO info{};
  if (!GetConsoleScreenBufferInfo(output, &info)) {
    return {};
  }

  const int columns = info.srWindow.Right - info.srWindow.Left + 1;
  const int rows = info.srWindow.Bottom - info.srWindow.Top + 1;
  return {clamp_terminal_dimension(columns), clamp_terminal_dimension(rows)};
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
  const auto endpoint = widen(attach_endpoint_name());
  for (int attempt = 0; attempt < kAttachConnectAttempts; ++attempt) {
    HANDLE raw_pipe = CreateFileW(
        endpoint.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        0,
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
  return write_all(pipe, frame);
}

bool send_attach_input(HANDLE pipe, std::string_view bytes) {
  if (bytes.empty()) {
    return true;
  }

  return write_attach_frame(pipe, make_attach_input_frame(bytes));
}

bool send_attach_detach(HANDLE pipe) {
  return write_attach_frame(pipe, make_attach_detach_frame());
}

bool send_attach_command(HANDLE pipe, std::string_view command) {
  return write_attach_frame(pipe, make_attach_command_frame(command));
}

bool send_attach_command_mode(HANDLE pipe, std::string_view command) {
  return write_attach_frame(pipe, make_attach_command_mode_frame(command));
}

bool send_attach_resize(HANDLE pipe, TerminalSize size) {
  if (size.columns == 0 || size.rows == 0) {
    return true;
  }

  return write_attach_frame(pipe, make_attach_resize_frame(size.columns, size.rows));
}

bool send_attach_status(HANDLE pipe, std::string_view status) {
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

bool same_terminal_size(TerminalSize lhs, TerminalSize rhs) {
  return lhs.columns == rhs.columns && lhs.rows == rhs.rows;
}

std::string_view arrow_attach_command(char direction) {
  switch (direction) {
    case 'A':
      return "select-pane-up";
    case 'B':
      return "select-pane-down";
    case 'C':
      return "select-pane-right";
    case 'D':
      return "select-pane-left";
    default:
      return {};
  }
}

std::optional<AttachScrollAction> prefixed_scroll_sequence(std::string_view bytes) {
  if (bytes.rfind("\x1b[5~", 0) == 0) {
    return AttachScrollAction::PageUp;
  }

  if (bytes.rfind("\x1b[6~", 0) == 0) {
    return AttachScrollAction::PageDown;
  }

  return std::nullopt;
}

std::size_t prefixed_scroll_sequence_size(AttachScrollAction action) {
  switch (action) {
    case AttachScrollAction::PageUp:
    case AttachScrollAction::PageDown:
      return 4;
    case AttachScrollAction::LineUp:
    case AttachScrollAction::LineDown:
    case AttachScrollAction::Bottom:
      return 0;
  }

  return 0;
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
    CommandPromptState& command_prompt,
    std::string& pending_mouse_sequence,
    bool mouse_enabled,
    bool& prefix_pending,
    std::atomic_bool& stop_requested,
    HANDLE stop_event) {
  std::string to_send;
  to_send.reserve(bytes.size() + 1);

  const auto flush_input = [&] {
    if (to_send.empty()) {
      return true;
    }

    const bool sent = send_attach_input(pipe, to_send);
    to_send.clear();
    return sent;
  };

  const auto send_mouse_event = [&](const MouseParseResult& mouse) {
    if (!mouse.event) {
      return true;
    }

    return flush_input() && send_attach_mouse_event(pipe, *mouse.event);
  };

  for (std::size_t index = 0; index < bytes.size(); ++index) {
    const char byte = bytes[index];
    if (mouse_enabled && !pending_mouse_sequence.empty()) {
      pending_mouse_sequence.push_back(byte);
      const auto mouse = parse_sgr_mouse_sequence(pending_mouse_sequence);
      if (mouse.status == MouseParseStatus::Parsed) {
        pending_mouse_sequence.clear();
        if (!send_mouse_event(mouse)) {
          return false;
        }
        continue;
      }
      if (mouse.status == MouseParseStatus::Incomplete) {
        continue;
      }

      to_send.append(pending_mouse_sequence);
      pending_mouse_sequence.clear();
      continue;
    }

    if (mouse_enabled && byte == '\x1b') {
      const auto remaining = bytes.substr(index);
      const auto mouse = parse_sgr_mouse_sequence(remaining);
      if (mouse.status == MouseParseStatus::Parsed) {
        index += mouse.bytes_consumed - 1;
        if (!send_mouse_event(mouse)) {
          return false;
        }
        continue;
      }
      if (mouse.status == MouseParseStatus::Incomplete &&
          remaining.size() >= 3 &&
          is_sgr_mouse_sequence_prefix(remaining)) {
        pending_mouse_sequence.assign(remaining);
        break;
      }
    }

    if (command_prompt.active) {
      const auto event = handle_command_prompt_byte(command_prompt, byte);
      if (event == CommandPromptEvent::None) {
        continue;
      }

      if (event == CommandPromptEvent::Submitted) {
        return send_attach_command_mode(pipe, command_prompt.submitted_command);
      }

      if (!send_attach_status(pipe, command_prompt_status_text(command_prompt))) {
        return false;
      }
      if (event == CommandPromptEvent::Cancelled) {
        return true;
      }

      continue;
    }

    if (prefix_pending) {
      prefix_pending = false;
      if (byte == 'd') {
        const bool sent = send_attach_detach(pipe);
        stop_requested = true;
        SetEvent(stop_event);
        return sent;
      }
      if (byte == 'c') {
        return send_attach_command(pipe, "new-window");
      }
      if (byte == 'n') {
        return send_attach_command(pipe, "next-window");
      }
      if (byte == 'p') {
        return send_attach_command(pipe, "previous-window");
      }
      if (byte == '%') {
        return send_attach_command(pipe, "split-horizontal");
      }
      if (byte == '"') {
        return send_attach_command(pipe, "split-vertical");
      }
      if (byte == 'g') {
        return send_attach_scroll(pipe, AttachScrollAction::Bottom);
      }
      if (byte == ':') {
        start_command_prompt(command_prompt);
        return send_attach_status(pipe, command_prompt_status_text(command_prompt));
      }
      if (byte == '\x1b') {
        const auto scroll = prefixed_scroll_sequence(bytes.substr(index));
        if (scroll) {
          index += prefixed_scroll_sequence_size(*scroll) - 1;
          return send_attach_scroll(pipe, *scroll);
        }
      }
      if (byte == '\x1b' && index + 2 < bytes.size() && bytes[index + 1] == '[') {
        const auto command = arrow_attach_command(bytes[index + 2]);
        if (!command.empty()) {
          return send_attach_command(pipe, command);
        }
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

  return send_attach_input(pipe, to_send);
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
  const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
  const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
  if (!valid_handle(input) || !valid_handle(output)) {
    std::cerr << "wmux: attach requires valid standard input and output handles\n";
    return 1;
  }

  ConsoleModeGuard input_guard{input};
  ConsoleModeGuard output_guard{output};
  if (!input_guard.active() || !output_guard.active()) {
    std::cerr << "wmux: attach requires an interactive Windows console\n";
    return 1;
  }

  const auto size = current_terminal_size(output);

  UniqueHandle pipe;
  if (!connect_pipe(pipe)) {
    std::cerr << "wmux: daemon attach endpoint is not running\n";
    return 1;
  }

  if (!write_all(pipe.get(), make_attach_request_json(command, size.columns, size.rows))) {
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

  {
    DWORD mode = input_guard.original_mode();
    mode &= ~ENABLE_ECHO_INPUT;
    mode &= ~ENABLE_LINE_INPUT;
    mode &= ~ENABLE_PROCESSED_INPUT;
    mode &= ~ENABLE_QUICK_EDIT_MODE;
    mode |= ENABLE_EXTENDED_FLAGS;
    mode |= ENABLE_VIRTUAL_TERMINAL_INPUT;
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

  MouseReportingGuard mouse_reporting{output, response->mouse_enabled};
  if (mouse_reporting.enabled() && !mouse_reporting.active()) {
    std::cerr << "wmux: failed to enable mouse reporting\n";
    return 1;
  }

  UniqueHandle stop_event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
  if (!stop_event.valid()) {
    std::cerr << "wmux: failed to create attach stop event\n";
    return 1;
  }

  std::atomic_bool stop_requested{false};
  std::thread output_thread{
      [&] { stream_output(pipe.get(), output, stop_requested, stop_event.get()); }};

  TerminalSize last_size = size;
  CommandPromptState command_prompt;
  std::string pending_mouse_sequence;
  bool prefix_pending = false;
  char buffer[512];
  while (!stop_requested) {
    HANDLE handles[] = {input, stop_event.get()};
    const DWORD wait_result = WaitForMultipleObjects(2, handles, FALSE, 100);
    const auto latest_size = current_terminal_size(output);
    if (!same_terminal_size(latest_size, last_size) && latest_size.columns > 0 &&
        latest_size.rows > 0) {
      if (!send_attach_resize(pipe.get(), latest_size)) {
        stop_requested = true;
        break;
      }
      last_size = latest_size;
    }

    if (wait_result == WAIT_TIMEOUT) {
      continue;
    }
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
            command_prompt,
            pending_mouse_sequence,
            response->mouse_enabled,
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
