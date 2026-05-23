#pragma once

#include "daemon_state.hpp"
#include "wmux/ipc_protocol.hpp"

#include <atomic>
#include <string>

namespace wmux::daemon_internal {

std::string handle_request(
    const IpcRequest& request,
    std::atomic_bool& should_stop,
    DaemonState& state);

}  // namespace wmux::daemon_internal
