#pragma once

#include <string_view>

namespace wmux {

std::string_view terminal_reset_sequence();
int reset_terminal();

}  // namespace wmux
