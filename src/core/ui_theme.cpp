#include "wmux/ui_theme.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <string>

namespace wmux {
namespace {

std::string lowercase(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (const char byte : value) {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(byte))));
  }
  return out;
}

int hex_digit(char byte) {
  if (byte >= '0' && byte <= '9') {
    return byte - '0';
  }
  if (byte >= 'a' && byte <= 'f') {
    return 10 + byte - 'a';
  }
  if (byte >= 'A' && byte <= 'F') {
    return 10 + byte - 'A';
  }
  return -1;
}

std::optional<std::uint8_t> parse_hex_byte(char high, char low) {
  const int first = hex_digit(high);
  const int second = hex_digit(low);
  if (first < 0 || second < 0) {
    return std::nullopt;
  }
  return static_cast<std::uint8_t>((first << 4) | second);
}

std::optional<UiColor> named_color(std::string_view value) {
  const auto name = lowercase(value);
  if (name == "black") {
    return UiColor{UiColorKind::Indexed, 0};
  }
  if (name == "red") {
    return UiColor{UiColorKind::Indexed, 1};
  }
  if (name == "green") {
    return UiColor{UiColorKind::Indexed, 2};
  }
  if (name == "yellow") {
    return UiColor{UiColorKind::Indexed, 3};
  }
  if (name == "blue") {
    return UiColor{UiColorKind::Indexed, 4};
  }
  if (name == "magenta" || name == "purple") {
    return UiColor{UiColorKind::Indexed, 5};
  }
  if (name == "cyan") {
    return UiColor{UiColorKind::Indexed, 6};
  }
  if (name == "white") {
    return UiColor{UiColorKind::Indexed, 7};
  }
  if (name == "bright-black" || name == "gray" || name == "grey") {
    return UiColor{UiColorKind::Indexed, 8};
  }
  if (name == "bright-red") {
    return UiColor{UiColorKind::Indexed, 9};
  }
  if (name == "bright-green") {
    return UiColor{UiColorKind::Indexed, 10};
  }
  if (name == "bright-yellow") {
    return UiColor{UiColorKind::Indexed, 11};
  }
  if (name == "bright-blue") {
    return UiColor{UiColorKind::Indexed, 12};
  }
  if (name == "bright-magenta" || name == "bright-purple") {
    return UiColor{UiColorKind::Indexed, 13};
  }
  if (name == "bright-cyan") {
    return UiColor{UiColorKind::Indexed, 14};
  }
  if (name == "bright-white") {
    return UiColor{UiColorKind::Indexed, 15};
  }
  return std::nullopt;
}

}  // namespace

std::optional<UiColor> parse_ui_color(std::string_view value) {
  if (value.empty()) {
    return std::nullopt;
  }

  if (auto color = named_color(value)) {
    return color;
  }

  if (value.size() == 7 && value[0] == '#') {
    const auto red = parse_hex_byte(value[1], value[2]);
    const auto green = parse_hex_byte(value[3], value[4]);
    const auto blue = parse_hex_byte(value[5], value[6]);
    if (!red || !green || !blue) {
      return std::nullopt;
    }
    UiColor color;
    color.kind = UiColorKind::Rgb;
    color.red = *red;
    color.green = *green;
    color.blue = *blue;
    return color;
  }

  unsigned int index = 0;
  const auto [ptr, error] =
      std::from_chars(value.data(), value.data() + value.size(), index);
  if (error == std::errc{} && ptr == value.data() + value.size() && index <= 255) {
    return UiColor{UiColorKind::Indexed, static_cast<std::uint8_t>(index)};
  }

  return std::nullopt;
}

std::string ui_color_foreground_sgr(const UiColor& color) {
  if (color.kind == UiColorKind::Rgb) {
    return "38;2;" + std::to_string(color.red) + ";" + std::to_string(color.green) + ";" +
           std::to_string(color.blue);
  }
  return "38;5;" + std::to_string(color.index);
}

std::string ui_color_background_sgr(const UiColor& color) {
  if (color.kind == UiColorKind::Rgb) {
    return "48;2;" + std::to_string(color.red) + ";" + std::to_string(color.green) + ";" +
           std::to_string(color.blue);
  }
  return "48;5;" + std::to_string(color.index);
}

}  // namespace wmux
