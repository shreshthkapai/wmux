#pragma once

#include "wmux/commands.hpp"

#include <cstdint>
#include <optional>
#include <utility>
#include <string>
#include <string_view>

namespace wmux {

struct IpcRequest {
  std::string type;
  std::string session_name;
  std::string target_name;
  std::string new_name;
  std::string window_name;
  std::string split_direction;
  std::string option_name;
  std::string option_value;
  std::uint16_t terminal_columns{0};
  std::uint16_t terminal_rows{0};
  bool force{false};
};

struct IpcResponse {
  bool ok{false};
  std::string message;
  bool mouse_enabled{false};
};

enum class AttachFrameType : std::uint8_t {
  Input = 1,
  Detach = 2,
  Command = 3,
  Resize = 4,
  Status = 5,
  CommandMode = 6,
  MouseFocus = 7,
  MouseEvent = 8,
  Scroll = 9,
};

struct AttachFrameHeader {
  AttachFrameType type{AttachFrameType::Input};
  std::uint32_t payload_size{0};
};

enum class AttachMouseButton : std::uint8_t {
  Left = 0,
  Middle = 1,
  Right = 2,
  Release = 3,
  WheelUp = 4,
  WheelDown = 5,
  Other = 6,
};

enum class AttachMouseAction : std::uint8_t {
  Press = 0,
  Release = 1,
  Drag = 2,
  Wheel = 3,
};

enum class AttachScrollAction : std::uint8_t {
  LineUp = 0,
  LineDown = 1,
  PageUp = 2,
  PageDown = 3,
  Bottom = 4,
};

struct AttachMouseFocusPayload {
  std::uint16_t column{0};
  std::uint16_t row{0};
};

struct AttachMouseEventPayload {
  std::uint16_t column{0};
  std::uint16_t row{0};
  std::uint16_t button_code{0};
  AttachMouseButton button{AttachMouseButton::Other};
  AttachMouseAction action{AttachMouseAction::Press};
};

constexpr std::size_t kAttachFrameHeaderSize = 7;
constexpr std::uint32_t kMaxAttachFramePayloadSize = 1024 * 1024;

std::string make_ping_request_json();
std::string make_command_request_json(const CommandLine& command);
std::string make_attach_request_json(
    const CommandLine& command,
    std::uint16_t terminal_columns,
    std::uint16_t terminal_rows);
std::optional<IpcRequest> parse_request_json(std::string_view json);
std::string make_response_json(bool ok, std::string_view message);
std::string make_response_json(bool ok, std::string_view message, bool mouse_enabled);
std::optional<IpcResponse> parse_response_json(std::string_view json);
std::string make_attach_input_frame(std::string_view bytes);
std::string make_attach_detach_frame();
std::string make_attach_command_frame(std::string_view command);
std::string make_attach_command_mode_frame(std::string_view command);
std::string make_attach_resize_frame(std::uint16_t columns, std::uint16_t rows);
std::string make_attach_status_frame(std::string_view status);
std::string make_attach_mouse_focus_frame(std::uint16_t column, std::uint16_t row);
std::string make_attach_mouse_event_frame(const AttachMouseEventPayload& event);
std::string make_attach_scroll_frame(AttachScrollAction action);
std::optional<AttachFrameHeader> parse_attach_frame_header(std::string_view header);
std::optional<std::pair<std::uint16_t, std::uint16_t>> parse_attach_resize_payload(
    std::string_view payload);
std::optional<AttachMouseFocusPayload> parse_attach_mouse_focus_payload(
    std::string_view payload);
std::optional<AttachMouseEventPayload> parse_attach_mouse_event_payload(
    std::string_view payload);
std::optional<AttachScrollAction> parse_attach_scroll_payload(std::string_view payload);

}  // namespace wmux
