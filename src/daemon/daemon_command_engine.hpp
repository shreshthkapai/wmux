#pragma once

#include "daemon_state.hpp"
#include "wmux/command_engine.hpp"
#include "wmux/ipc_protocol.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wmux::daemon_internal {

struct CommandResult {
  CommandStatus status{CommandStatus::NoOp};
  std::optional<std::string> message;
  RedrawRequest redraw{RedrawRequest::None};
  std::vector<DaemonEvent> events;
};

struct TargetResolutionResult {
  bool ok{false};
  ResolvedTarget target;
  std::string error;
};

TargetResolutionResult resolve_target(
    DaemonState& state,
    const Target& target,
    std::optional<ClientId> current_client_id = std::nullopt);

std::optional<RuntimeCommand> runtime_command_from_attach_command(
    std::string_view command,
    std::string& error);

std::optional<RuntimeCommand> runtime_command_from_command_mode_text(
    std::string_view command_text,
    std::string& error);

std::optional<RuntimeCommand> runtime_command_from_ipc_request(
    const IpcRequest& request,
    std::string& error);

}  // namespace wmux::daemon_internal
