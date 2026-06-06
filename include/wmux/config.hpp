#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace wmux {

struct Config {
  std::string prefix{"C-b"};
  bool mouse_enabled{false};
  std::string default_shell{"powershell.exe -NoLogo -NoProfile"};
  bool status_bar_enabled{true};
};

struct ConfigParseError {
  std::size_t line{0};
  std::string message;
};

struct ConfigParseResult {
  Config config;
  std::vector<ConfigParseError> errors;

  [[nodiscard]] bool ok() const {
    return errors.empty();
  }
};

std::filesystem::path default_config_path();
ConfigParseResult parse_config_text(std::string_view text);
ConfigParseResult load_config_file(const std::filesystem::path& path);

}  // namespace wmux
