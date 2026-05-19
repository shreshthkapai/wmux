#pragma once

#include "wmux/commands.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace wmux {

struct IpcRequest {
  std::string type;
  std::string session_name;
  std::string target_name;
  std::string new_name;
};

struct IpcResponse {
  bool ok{false};
  std::string message;
};

std::string make_ping_request_json();
std::string make_command_request_json(const CommandLine& command);
std::optional<IpcRequest> parse_request_json(std::string_view json);
std::string make_response_json(bool ok, std::string_view message);
std::optional<IpcResponse> parse_response_json(std::string_view json);

}  // namespace wmux
