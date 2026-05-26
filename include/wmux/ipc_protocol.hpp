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
  std::uint16_t terminal_columns{0};
  std::uint16_t terminal_rows{0};
  bool force{false};
};

struct IpcResponse {
  bool ok{false};
  std::string message;
};

enum class AttachFrameType : std::uint8_t {
  Input = 1,
  Detach = 2,
  Command = 3,
  Resize = 4,
};

struct AttachFrameHeader {
  AttachFrameType type{AttachFrameType::Input};
  std::uint32_t payload_size{0};
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
std::optional<IpcResponse> parse_response_json(std::string_view json);
std::string make_attach_input_frame(std::string_view bytes);
std::string make_attach_detach_frame();
std::string make_attach_command_frame(std::string_view command);
std::string make_attach_resize_frame(std::uint16_t columns, std::uint16_t rows);
std::optional<AttachFrameHeader> parse_attach_frame_header(std::string_view header);
std::optional<std::pair<std::uint16_t, std::uint16_t>> parse_attach_resize_payload(
    std::string_view payload);

}  // namespace wmux
