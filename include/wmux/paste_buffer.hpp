#pragma once

#include <string>
#include <string_view>

namespace wmux {

std::string normalize_paste_text_for_terminal(std::string_view text);

}  // namespace wmux
