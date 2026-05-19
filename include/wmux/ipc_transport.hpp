#pragma once

#include "wmux/ipc_protocol.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace wmux {

std::string command_endpoint_name();
IpcResponse send_ipc_request(std::string_view request_json);
bool ensure_daemon_running(const std::filesystem::path& executable_path, std::string& error);

}  // namespace wmux
