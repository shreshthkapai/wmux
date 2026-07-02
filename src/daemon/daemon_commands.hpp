#pragma once

#include "daemon_state.hpp"
#include "wmux/ipc_protocol.hpp"

#include <optional>
#include <string>

namespace wmux::daemon_internal {

struct DaemonCommandResult {
  std::string response;
  bool should_stop{false};
};

DaemonCommandResult handle_request(
    const IpcRequest& request,
    DaemonState& state,
    RequestId request_id = 0,
    std::optional<ClientId> client_id = std::nullopt);

}  // namespace wmux::daemon_internal
