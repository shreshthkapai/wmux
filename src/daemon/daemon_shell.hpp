#pragma once

#include "wmux/pty_process.hpp"

#include <string>

namespace wmux::daemon_internal {

constexpr short kInitialPtyColumns = 120;
constexpr short kInitialPtyRows = 30;

std::string default_shell_command();
PtyProcessResult start_default_shell(short columns = kInitialPtyColumns, short rows = kInitialPtyRows);

}  // namespace wmux::daemon_internal
