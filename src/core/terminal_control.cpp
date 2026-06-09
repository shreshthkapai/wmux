#include "wmux/terminal_control.hpp"

#include <algorithm>
#include <iostream>
#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace wmux {

std::string_view terminal_reset_sequence() {
  return "\x1b[?25h\x1b[?1000l\x1b[?1002l\x1b[?1003l\x1b[?1006l\x1b[?1049l\x1b[0m";
}

std::string_view terminal_attach_enter_sequence() {
  return "\x1b[?1049h\x1b[2J\x1b[H\x1b[?25l";
}

int reset_terminal() {
#ifdef _WIN32
  const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
  if (output == nullptr || output == INVALID_HANDLE_VALUE) {
    return 1;
  }

  DWORD original_mode = 0;
  const bool have_mode = GetConsoleMode(output, &original_mode) != 0;
  if (have_mode) {
    SetConsoleMode(output, original_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
  }

  std::string_view reset = terminal_reset_sequence();
  while (!reset.empty()) {
    const auto bytes_to_write =
        static_cast<DWORD>(std::min<std::size_t>(reset.size(), 64 * 1024));
    DWORD bytes_written = 0;
    if (!WriteFile(output, reset.data(), bytes_to_write, &bytes_written, nullptr) ||
        bytes_written == 0) {
      return 1;
    }
    reset.remove_prefix(bytes_written);
  }

  if (have_mode) {
    SetConsoleMode(output, original_mode);
  }
  return 0;
#else
  std::cout << terminal_reset_sequence();
  return std::cout.good() ? 0 : 1;
#endif
}

}  // namespace wmux
