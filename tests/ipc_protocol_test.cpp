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

void expects_forced_command_request_json() {
  const std::vector<std::string_view> args{"server", "stop", "--force"};
  const auto command = wmux::parse_command_line(args);
  const auto request = wmux::parse_request_json(wmux::make_command_request_json(command));
  assert(request);
  assert(request->type == "ServerStop");
  assert(request->force);
}

void expects_window_command_request_json() {
  const std::vector<std::string_view> args{"new-window", "-t", "finance", "-n", "logs"};
  const auto command = wmux::parse_command_line(args);
  const auto request = wmux::parse_request_json(wmux::make_command_request_json(command));
  assert(request);
  assert(request->type == "NewWindow");
  assert(request->session_name == "finance");
  assert(request->window_name == "logs");
}

void expects_attach_request_json_with_terminal_size() {
  wmux::CommandLine command;
  command.kind = wmux::CommandKind::AttachSession;
  command.session_name = "finance";

  const auto request = wmux::parse_request_json(wmux::make_attach_request_json(command, 132, 43));
  assert(request);
  assert(request->type == "AttachSession");
  assert(request->session_name == "finance");
  assert(request->terminal_columns == 132);
  assert(request->terminal_rows == 43);
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

void expects_attach_input_frame() {
  const auto frame = wmux::make_attach_input_frame("dir\r\n");
  assert(frame.size() == wmux::kAttachFrameHeaderSize + 5);

  const auto header = wmux::parse_attach_frame_header(
      std::string_view{frame.data(), wmux::kAttachFrameHeaderSize});
  assert(header);
  assert(header->type == wmux::AttachFrameType::Input);
  assert(header->payload_size == 5);
  assert(frame.substr(wmux::kAttachFrameHeaderSize) == "dir\r\n");
}

void expects_attach_detach_frame() {
  const auto frame = wmux::make_attach_detach_frame();
  assert(frame.size() == wmux::kAttachFrameHeaderSize);

  const auto header = wmux::parse_attach_frame_header(frame);
  assert(header);
  assert(header->type == wmux::AttachFrameType::Detach);
  assert(header->payload_size == 0);
}

void expects_attach_command_frame() {
  const auto frame = wmux::make_attach_command_frame("next-window");
  assert(frame.size() == wmux::kAttachFrameHeaderSize + 11);

  const auto header = wmux::parse_attach_frame_header(
      std::string_view{frame.data(), wmux::kAttachFrameHeaderSize});
  assert(header);
  assert(header->type == wmux::AttachFrameType::Command);
  assert(header->payload_size == 11);
  assert(frame.substr(wmux::kAttachFrameHeaderSize) == "next-window");
}

void expects_bad_json_rejected() {
  const auto request = wmux::parse_request_json("{\"nope\":\"ListSessions\"}\n");
  assert(!request);
}

}  // namespace

void run_ipc_protocol_tests() {
  expects_ping_request_json();
  expects_command_request_json();
  expects_forced_command_request_json();
  expects_window_command_request_json();
  expects_attach_request_json_with_terminal_size();
  expects_json_escaping();
  expects_response_json();
  expects_attach_input_frame();
  expects_attach_detach_frame();
  expects_attach_command_frame();
  expects_bad_json_rejected();
}
