#include "wmux/platform/services.hpp"

#include <cassert>
#include <string_view>

namespace {

bool contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

void reset_sequence_disables_client_terminal_modes() {
  const auto reset = wmux::platform_services().terminal().reset_sequence();
  assert(contains(reset, "\x1b[?25h"));
  assert(contains(reset, "\x1b[0 q"));
  assert(contains(reset, "\x1b[?1000l"));
  assert(contains(reset, "\x1b[?1002l"));
  assert(contains(reset, "\x1b[?1003l"));
  assert(contains(reset, "\x1b[?1004l"));
  assert(contains(reset, "\x1b[?1005l"));
  assert(contains(reset, "\x1b[?1006l"));
  assert(contains(reset, "\x1b[?1015l"));
  assert(contains(reset, "\x1b[?2004l"));
  assert(contains(reset, "\x1b[?47l"));
  assert(contains(reset, "\x1b[?1047l"));
  assert(contains(reset, "\x1b[?1048l"));
  assert(contains(reset, "\x1b[?1049l"));
  assert(contains(reset, "\x1b[0m"));
}

}  // namespace

void run_terminal_control_tests() {
  reset_sequence_disables_client_terminal_modes();
}
