#include "wmux/version.hpp"

namespace wmux {

std::string_view version_string() noexcept {
  return WMUX_VERSION;
}

}  // namespace wmux
