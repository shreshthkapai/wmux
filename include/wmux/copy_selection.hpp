#pragma once

#include "wmux/pty_process.hpp"

#include <cstddef>
#include <string>

namespace wmux {

struct CopySelectionPoint {
  std::size_t line{0};
  std::size_t column{0};
};

struct CopySelectionRange {
  CopySelectionPoint anchor;
  CopySelectionPoint cursor;
};

std::string extract_copy_selection_text(
    const PtyOutputSnapshot& snapshot,
    CopySelectionRange range,
    std::size_t line_width);

}  // namespace wmux
