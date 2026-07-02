#pragma once

#include "wmux/platform/platform_info.hpp"
#include "wmux/platform/pty_process.hpp"

#include <string>
#include <string_view>

namespace wmux::daemon_internal {

struct DaemonState;

constexpr short kInitialPtyColumns = 120;
constexpr short kInitialPtyRows = 30;

std::string default_shell_command();
std::string configured_shell_command(DaemonState& state);
PlatformShellResolution configured_shell_resolution(DaemonState& state);
ResourceLimits configured_resource_limits(DaemonState& state);
PtyProcessResult start_shell(
    std::string_view command_line,
    short columns = kInitialPtyColumns,
    short rows = kInitialPtyRows);
PtyProcessResult start_shell(
    const PlatformShellResolution& shell,
    short columns = kInitialPtyColumns,
    short rows = kInitialPtyRows);
PtyProcessResult start_shell(
    const PlatformShellResolution& shell,
    const ResourceLimits& limits,
    short columns = kInitialPtyColumns,
    short rows = kInitialPtyRows);
PtyProcessResult start_default_shell(short columns = kInitialPtyColumns, short rows = kInitialPtyRows);
PtyProcessResult start_configured_shell(
    DaemonState& state,
    short columns = kInitialPtyColumns,
    short rows = kInitialPtyRows);

}  // namespace wmux::daemon_internal
