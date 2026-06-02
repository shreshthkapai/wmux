#include "wmux/mouse_input.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace wmux {
namespace {

constexpr auto kSgrMouseStart = std::array<char, 3>{'\x1b', '[', '<'};
constexpr std::size_t kMaxSgrMouseSequenceLength = 64;

enum class ComponentParseStatus {
  Parsed,
  Incomplete,
  Invalid,
};

bool starts_with(std::string_view input, std::string_view prefix) {
  return input.size() >= prefix.size() && input.substr(0, prefix.size()) == prefix;
}

std::string_view sgr_mouse_start_prefix(std::size_t size) {
  const auto prefix_size = std::min(size, kSgrMouseStart.size());
  return {kSgrMouseStart.data(), prefix_size};
}

ComponentParseStatus parse_unsigned_component(
    std::string_view input,
    std::size_t& index,
    unsigned int& value) {
  if (index >= input.size()) {
    return ComponentParseStatus::Incomplete;
  }

  if (input[index] < '0' || input[index] > '9') {
    return ComponentParseStatus::Invalid;
  }

  unsigned int parsed = 0;
  while (index < input.size() && input[index] >= '0' && input[index] <= '9') {
    const auto digit = static_cast<unsigned int>(input[index] - '0');
    if (parsed > (std::numeric_limits<unsigned int>::max() - digit) / 10U) {
      return ComponentParseStatus::Invalid;
    }
    parsed = (parsed * 10U) + digit;
    ++index;
  }

  value = parsed;
  return ComponentParseStatus::Parsed;
}

MouseParseResult component_parse_result(ComponentParseStatus status) {
  if (status == ComponentParseStatus::Incomplete) {
    return {MouseParseStatus::Incomplete, std::nullopt, 0};
  }
  return {MouseParseStatus::Invalid, std::nullopt, 0};
}

std::uint16_t clamp_coordinate(unsigned int value) {
  return static_cast<std::uint16_t>(
      std::min<unsigned int>(value, std::numeric_limits<std::uint16_t>::max()));
}

MouseButton decode_button(int button_code, bool release) {
  if (release) {
    return MouseButton::Release;
  }

  if ((button_code & 64) != 0) {
    return (button_code & 1) == 0 ? MouseButton::WheelUp : MouseButton::WheelDown;
  }

  switch (button_code & 3) {
    case 0:
      return MouseButton::Left;
    case 1:
      return MouseButton::Middle;
    case 2:
      return MouseButton::Right;
    case 3:
      return MouseButton::Release;
    default:
      return MouseButton::Other;
  }
}

MouseAction decode_action(int button_code, char final_byte) {
  if ((button_code & 64) != 0) {
    return MouseAction::Wheel;
  }
  if (final_byte == 'm' || (button_code & 3) == 3) {
    return MouseAction::Release;
  }
  if ((button_code & 32) != 0) {
    return MouseAction::Drag;
  }
  return MouseAction::Press;
}

}  // namespace

std::string_view enable_mouse_reporting_sequence() {
  return "\x1b[?1000h\x1b[?1002h\x1b[?1006h";
}

std::string_view disable_mouse_reporting_sequence() {
  return "\x1b[?1006l\x1b[?1002l\x1b[?1000l";
}

bool is_sgr_mouse_sequence_prefix(std::string_view input) {
  if (input.empty() || input.size() > kMaxSgrMouseSequenceLength) {
    return false;
  }

  const auto prefix = sgr_mouse_start_prefix(input.size());
  return input == prefix || starts_with(input, std::string_view{kSgrMouseStart.data(), 3});
}

MouseParseResult parse_sgr_mouse_sequence(std::string_view input) {
  if (input.empty()) {
    return {MouseParseStatus::Incomplete, std::nullopt, 0};
  }
  if (input.size() > kMaxSgrMouseSequenceLength) {
    return {MouseParseStatus::Invalid, std::nullopt, 0};
  }

  if (input.size() < kSgrMouseStart.size()) {
    const auto prefix = sgr_mouse_start_prefix(input.size());
    if (input == prefix) {
      return {MouseParseStatus::Incomplete, std::nullopt, 0};
    }
    return {MouseParseStatus::Invalid, std::nullopt, 0};
  }

  if (!starts_with(input, std::string_view{kSgrMouseStart.data(), kSgrMouseStart.size()})) {
    return {MouseParseStatus::Invalid, std::nullopt, 0};
  }

  std::size_t index = kSgrMouseStart.size();
  unsigned int button_code = 0;
  unsigned int column = 0;
  unsigned int row = 0;

  if (const auto status = parse_unsigned_component(input, index, button_code);
      status != ComponentParseStatus::Parsed) {
    return component_parse_result(status);
  }
  if (index >= input.size()) {
    return {MouseParseStatus::Incomplete, std::nullopt, 0};
  }
  if (input[index] != ';') {
    return {MouseParseStatus::Invalid, std::nullopt, 0};
  }
  ++index;

  if (const auto status = parse_unsigned_component(input, index, column);
      status != ComponentParseStatus::Parsed) {
    return component_parse_result(status);
  }
  if (index >= input.size()) {
    return {MouseParseStatus::Incomplete, std::nullopt, 0};
  }
  if (input[index] != ';') {
    return {MouseParseStatus::Invalid, std::nullopt, 0};
  }
  ++index;

  if (const auto status = parse_unsigned_component(input, index, row);
      status != ComponentParseStatus::Parsed) {
    return component_parse_result(status);
  }
  if (index >= input.size()) {
    return {MouseParseStatus::Incomplete, std::nullopt, 0};
  }

  const char final_byte = input[index];
  if (final_byte != 'M' && final_byte != 'm') {
    return {MouseParseStatus::Invalid, std::nullopt, 0};
  }

  const auto code = static_cast<int>(button_code);
  const bool released = final_byte == 'm' || (code & 3) == 3;
  MouseEvent event;
  event.column = clamp_coordinate(column);
  event.row = clamp_coordinate(row);
  event.button_code = code;
  event.button = decode_button(code, released);
  event.action = decode_action(code, final_byte);

  return {MouseParseStatus::Parsed, event, index + 1};
}

}  // namespace wmux
