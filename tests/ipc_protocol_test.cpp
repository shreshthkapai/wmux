#include "wmux/ipc_protocol.hpp"

#include <cassert>
#include <string_view>
#include <vector>

namespace {

void expects_ping_request_json() {
  const auto request = wmux::parse_request_json(wmux::make_ping_request_json());
  assert(request);
  assert(request->type == "Ping");
}

void expects_command_request_json() {
  const std::vector<std::string_view> args{"rename-session", "-t", "finance", "trading"};
  const auto command = wmux::parse_command_line(args);
  const auto request = wmux::parse_request_json(wmux::make_command_request_json(command));
  assert(request);
  assert(request->type == "RenameSession");
  assert(request->target_name == "finance");
  assert(request->new_name == "trading");
}

void expects_json_escaping() {
  wmux::CommandLine command;
  command.kind = wmux::CommandKind::NewSession;
  command.session_name = "quote\"slash\\line\n";

  const auto request = wmux::parse_request_json(wmux::make_command_request_json(command));
  assert(request);
  assert(request->session_name == "quote\"slash\\line\n");
}

void expects_response_json() {
  const auto response = wmux::parse_response_json(wmux::make_response_json(true, "wmux: ok\n"));
  assert(response);
  assert(response->ok);
  assert(response->message == "wmux: ok\n");
}

void expects_bad_json_rejected() {
  const auto request = wmux::parse_request_json("{\"nope\":\"ListSessions\"}\n");
  assert(!request);
}

}  // namespace

void run_ipc_protocol_tests() {
  expects_ping_request_json();
  expects_command_request_json();
  expects_json_escaping();
  expects_response_json();
  expects_bad_json_rejected();
}
