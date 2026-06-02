#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace wmux {

enum class MouseButton {
  Left,
  Middle,
  Right,
  Release,
  WheelUp,
  WheelDown,
  Other,
};

enum class MouseAction {
  Press,
  Release,
  Drag,
  Wheel,
};

struct MouseEvent {
  std::uint16_t column{0};
  std::uint16_t row{0};
  int button_code{0};
  MouseButton button{MouseButton::Other};
  MouseAction action{MouseAction::Press};
};

enum class MouseParseStatus {
  Invalid,
  Incomplete,
  Parsed,
};

struct MouseParseResult {
  MouseParseStatus status{MouseParseStatus::Invalid};
  std::optional<MouseEvent> event;
  std::size_t bytes_consumed{0};
};

std::string_view enable_mouse_reporting_sequence();
std::string_view disable_mouse_reporting_sequence();
bool is_sgr_mouse_sequence_prefix(std::string_view input);
MouseParseResult parse_sgr_mouse_sequence(std::string_view input);

}  // namespace wmux
