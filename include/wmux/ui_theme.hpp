#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace wmux {

enum class UiColorKind {
  Indexed,
  Rgb,
};

struct UiColor {
  UiColorKind kind{UiColorKind::Indexed};
  std::uint8_t index{4};
  std::uint8_t red{0};
  std::uint8_t green{0};
  std::uint8_t blue{0};
};

struct UiTheme {
  bool inherit_terminal_theme{true};
  bool tmux_style{false};
  bool smooth_borders{true};
  UiColor accent;
  std::string accent_spec{"blue"};
};

std::optional<UiColor> parse_ui_color(std::string_view value);
std::string ui_color_foreground_sgr(const UiColor& color);
std::string ui_color_background_sgr(const UiColor& color);

}  // namespace wmux
