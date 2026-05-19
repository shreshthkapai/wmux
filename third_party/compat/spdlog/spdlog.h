#pragma once

#include <iostream>
#include <string_view>

namespace spdlog {

inline void set_pattern(std::string_view) {}

inline void info(std::string_view message) {
  std::clog << "[info] " << message << '\n';
}

inline void debug(std::string_view) {}

}  // namespace spdlog
