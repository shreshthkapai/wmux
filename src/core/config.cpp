#include "wmux/config.hpp"

#include "wmux/resource_limits.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace wmux {
namespace {

bool ascii_space(char byte) {
  return std::isspace(static_cast<unsigned char>(byte)) != 0;
}

bool supported_control_prefix(std::string_view value) {
  return value.size() == 3 && (value[0] == 'C' || value[0] == 'c') && value[1] == '-' &&
         value[2] >= '?' && value[2] <= '~';
}

bool parse_on_off(std::string_view value, bool& out) {
  if (value == "on") {
    out = true;
    return true;
  }
  if (value == "off") {
    out = false;
    return true;
  }
  return false;
}

std::string environment_variable(const char* name) {
#ifdef _WIN32
  char buffer[32768]{};
  const DWORD size =
      GetEnvironmentVariableA(name, buffer, static_cast<DWORD>(sizeof(buffer)));
  if (size > 0 && size < sizeof(buffer)) {
    return buffer;
  }
  return {};
#else
  const char* value = std::getenv(name);
  if (value == nullptr) {
    return {};
  }
  return value;
#endif
}

std::vector<std::string> tokenize_config_line(
    std::string_view line,
    std::size_t line_number,
    std::vector<ConfigParseError>& errors) {
  std::vector<std::string> tokens;
  std::size_t index = 0;

  while (index < line.size()) {
    while (index < line.size() && ascii_space(line[index])) {
      ++index;
    }

    if (index >= line.size() || line[index] == '#') {
      break;
    }

    std::string token;
    if (line[index] == '"' || line[index] == '\'') {
      const char quote = line[index];
      ++index;
      while (index < line.size() && line[index] != quote) {
        token.push_back(line[index]);
        ++index;
      }
      if (index >= line.size()) {
        errors.push_back({line_number, "unterminated quoted value"});
        return {};
      }
      ++index;

      if (index < line.size() && !ascii_space(line[index]) && line[index] != '#') {
        errors.push_back({line_number, "quoted value must be followed by whitespace"});
        return {};
      }
    } else {
      while (index < line.size() && !ascii_space(line[index])) {
        token.push_back(line[index]);
        ++index;
      }
    }

    tokens.push_back(std::move(token));
  }

  return tokens;
}

void parse_config_tokens(
    Config& config,
    const std::vector<std::string>& tokens,
    std::size_t line_number,
    std::vector<ConfigParseError>& errors) {
  if (tokens.empty()) {
    return;
  }

  if (tokens.size() != 4 || tokens[0] != "set" || tokens[1] != "-g") {
    errors.push_back({line_number, "expected: set -g <option> <value>"});
    return;
  }

  const auto& option = tokens[2];
  const auto& value = tokens[3];

  if (option == "prefix") {
    if (!supported_control_prefix(value)) {
      errors.push_back({line_number, "prefix must use C-<key> format"});
      return;
    }
    config.prefix = value;
    return;
  }

  if (option == "mouse") {
    bool enabled = false;
    if (!parse_on_off(value, enabled)) {
      errors.push_back({line_number, "mouse must be on or off"});
      return;
    }
    config.mouse_enabled = enabled;
    return;
  }

  if (option == "default-shell") {
    if (value.empty()) {
      errors.push_back({line_number, "default-shell cannot be empty"});
      return;
    }
    config.default_shell = value;
    return;
  }

  if (option == "status") {
    bool enabled = true;
    if (!parse_on_off(value, enabled)) {
      errors.push_back({line_number, "status must be on or off"});
      return;
    }
    config.status_bar_enabled = enabled;
    return;
  }

  errors.push_back({line_number, "unsupported option '" + option + "'"});
}

}  // namespace

std::filesystem::path default_config_path() {
  if (const auto profile = environment_variable("USERPROFILE"); !profile.empty()) {
    return std::filesystem::path{profile} / ".wmux.conf";
  }

  if (const auto home = environment_variable("HOME"); !home.empty()) {
    return std::filesystem::path{home} / ".wmux.conf";
  }

  return std::filesystem::path{".wmux.conf"};
}

ConfigParseResult parse_config_text(std::string_view text) {
  ConfigParseResult result;

  std::size_t line_number = 1;
  std::size_t line_start = 0;
  while (line_start <= text.size()) {
    std::size_t line_end = text.find('\n', line_start);
    if (line_end == std::string_view::npos) {
      line_end = text.size();
    }

    auto line = text.substr(line_start, line_end - line_start);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }

    if (line.size() > kMaxConfigLineBytes) {
      result.errors.push_back({line_number, "config line is too long"});
      if (line_end == text.size()) {
        break;
      }
      line_start = line_end + 1;
      ++line_number;
      continue;
    }

    auto tokens = tokenize_config_line(line, line_number, result.errors);
    if (result.errors.empty() || result.errors.back().line != line_number) {
      parse_config_tokens(result.config, tokens, line_number, result.errors);
    }

    if (line_end == text.size()) {
      break;
    }
    line_start = line_end + 1;
    ++line_number;
  }

  return result;
}

ConfigParseResult load_config_file(const std::filesystem::path& path) {
  ConfigParseResult result;
  std::ifstream file{path};
  if (!file) {
    return result;
  }

  std::ostringstream contents;
  contents << file.rdbuf();
  return parse_config_text(contents.str());
}

}  // namespace wmux
