#pragma once

#include <string_view>

namespace wmux {

std::string_view terminal_reset_sequence();
std::string_view terminal_attach_enter_sequence();
int reset_terminal();

}  // namespace wmux
