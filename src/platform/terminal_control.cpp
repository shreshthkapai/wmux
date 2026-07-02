#include "wmux/platform/terminal_control.hpp"

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
  return "\x1b[?25h"    // show cursor
         "\x1b[0 q"     // reset cursor style
         "\x1b[?12l"    // stop cursor blink mode if enabled
         "\x1b[?7h"     // restore auto-wrap
         "\x1b[?1000l"  // disable X10 mouse
         "\x1b[?1002l"  // disable button-event mouse
         "\x1b[?1003l"  // disable any-event mouse
         "\x1b[?1004l"  // disable focus events
         "\x1b[?1005l"  // disable UTF-8 mouse
         "\x1b[?1006l"  // disable SGR mouse
         "\x1b[?1015l"  // disable urxvt mouse
         "\x1b[?2004l"  // disable bracketed paste
         "\x1b[?47l"    // leave alternate screen variants
         "\x1b[?1047l"
         "\x1b[?1048l"
         "\x1b[?1049l"
         "\x1b[0m";     // reset text attributes
}

std::string_view terminal_attach_enter_sequence() {
  return "\x1b[?1049h\x1b[2J\x1b[H\x1b[?25l";
}

int reset_terminal() {
#ifdef _WIN32
  const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
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

  if (input != nullptr && input != INVALID_HANDLE_VALUE) {
    DWORD input_mode = 0;
    if (GetConsoleMode(input, &input_mode) != 0) {
      input_mode |= ENABLE_PROCESSED_INPUT;
      input_mode |= ENABLE_LINE_INPUT;
      input_mode |= ENABLE_ECHO_INPUT;
      input_mode |= ENABLE_EXTENDED_FLAGS;
      input_mode &= ~ENABLE_VIRTUAL_TERMINAL_INPUT;
      (void)SetConsoleMode(input, input_mode);
    }
  }
  return 0;
#else
  std::cout << terminal_reset_sequence();
  return std::cout.good() ? 0 : 1;
#endif
}

}  // namespace wmux
