#include "wmux/ipc_protocol.hpp"

#include <cassert>
#include <string>
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

void expects_split_command_request_json() {
  const std::vector<std::string_view> args{"split-window", "-t", "finance", "-v"};
  const auto command = wmux::parse_command_line(args);
  const auto request = wmux::parse_request_json(wmux::make_command_request_json(command));
  assert(request);
  assert(request->type == "SplitWindow");
  assert(request->session_name == "finance");
  assert(request->split_direction == "vertical");
}

void expects_set_option_request_json() {
  const std::vector<std::string_view> args{"set", "-g", "mouse", "on"};
  const auto command = wmux::parse_command_line(args);
  const auto request = wmux::parse_request_json(wmux::make_command_request_json(command));
  assert(request);
  assert(request->type == "SetOption");
  assert(request->option_name == "mouse");
  assert(request->option_value == "on");
}

void expects_attach_request_json_with_terminal_size() {
  wmux::CommandLine command;
  command.kind = wmux::CommandKind::AttachSession;
  command.session_name = "finance";

  const auto request = wmux::parse_request_json(wmux::make_attach_request_json(command, 132, 43));
  assert(request);
  assert(request->type == "AttachStart");
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

void ignores_nested_false_positive_json_fields() {
  const auto request = wmux::parse_request_json(
      "{\"meta\":{\"type\":\"KillSession\"},\"type\":\"ListSessions\"}\n");

  assert(request);
  assert(request->type == "ListSessions");
}

void rejects_json_fields_with_wrong_type() {
  const auto request = wmux::parse_request_json(
      "{\"type\":{\"nested\":\"ListSessions\"},\"session_name\":\"finance\"}\n");

  assert(!request);
}

void expects_response_json() {
  const auto response = wmux::parse_response_json(wmux::make_response_json(true, "wmux: ok\n"));
  assert(response);
  assert(response->ok);
  assert(response->message == "wmux: ok\n");
  assert(!response->mouse_enabled);
  assert(response->prefix == "C-b");
  assert(response->status_bar_enabled);
}

void expects_attach_response_json_with_settings() {
  const auto enabled = wmux::parse_response_json(wmux::make_response_json(true, "", true));
  assert(enabled);
  assert(enabled->ok);
  assert(enabled->message.empty());
  assert(enabled->mouse_enabled);
  assert(enabled->prefix == "C-b");
  assert(enabled->status_bar_enabled);

  const auto disabled = wmux::parse_response_json(wmux::make_response_json(true, "", false));
  assert(disabled);
  assert(disabled->ok);
  assert(disabled->message.empty());
  assert(!disabled->mouse_enabled);

  const auto configured =
      wmux::parse_response_json(wmux::make_response_json(true, "", true, "C-a", false));
  assert(configured);
  assert(configured->ok);
  assert(configured->mouse_enabled);
  assert(configured->prefix == "C-a");
  assert(!configured->status_bar_enabled);
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

void expects_attach_resize_frame() {
  const auto frame = wmux::make_attach_resize_frame(132, 43);
  assert(frame.size() == wmux::kAttachFrameHeaderSize + 4);

  const auto header = wmux::parse_attach_frame_header(
      std::string_view{frame.data(), wmux::kAttachFrameHeaderSize});
  assert(header);
  assert(header->type == wmux::AttachFrameType::Resize);
  assert(header->payload_size == 4);

  const auto dimensions =
      wmux::parse_attach_resize_payload(frame.substr(wmux::kAttachFrameHeaderSize));
  assert(dimensions);
  assert(dimensions->first == 132);
  assert(dimensions->second == 43);
}

void expects_attach_status_frame() {
  const auto frame = wmux::make_attach_status_frame(":rename-session trading");
  assert(frame.size() == wmux::kAttachFrameHeaderSize + 23);

  const auto header = wmux::parse_attach_frame_header(
      std::string_view{frame.data(), wmux::kAttachFrameHeaderSize});
  assert(header);
  assert(header->type == wmux::AttachFrameType::Status);
  assert(header->payload_size == 23);
  assert(frame.substr(wmux::kAttachFrameHeaderSize) == ":rename-session trading");
}

void expects_attach_command_mode_frame() {
  const auto frame = wmux::make_attach_command_mode_frame("new-window -n logs");
  assert(frame.size() == wmux::kAttachFrameHeaderSize + 18);

  const auto header = wmux::parse_attach_frame_header(
      std::string_view{frame.data(), wmux::kAttachFrameHeaderSize});
  assert(header);
  assert(header->type == wmux::AttachFrameType::CommandMode);
  assert(header->payload_size == 18);
  assert(frame.substr(wmux::kAttachFrameHeaderSize) == "new-window -n logs");
}

void expects_attach_mouse_focus_frame() {
  const auto frame = wmux::make_attach_mouse_focus_frame(61, 8);
  assert(frame.size() == wmux::kAttachFrameHeaderSize + 4);

  const auto header = wmux::parse_attach_frame_header(
      std::string_view{frame.data(), wmux::kAttachFrameHeaderSize});
  assert(header);
  assert(header->type == wmux::AttachFrameType::MouseFocus);
  assert(header->payload_size == 4);

  const auto focus =
      wmux::parse_attach_mouse_focus_payload(frame.substr(wmux::kAttachFrameHeaderSize));
  assert(focus);
  assert(focus->column == 61);
  assert(focus->row == 8);
}

void expects_attach_mouse_event_frame() {
  const wmux::AttachMouseEventPayload payload{
      61,
      8,
      32,
      wmux::AttachMouseButton::Left,
      wmux::AttachMouseAction::Drag};
  const auto frame = wmux::make_attach_mouse_event_frame(payload);
  assert(frame.size() == wmux::kAttachFrameHeaderSize + 8);

  const auto header = wmux::parse_attach_frame_header(
      std::string_view{frame.data(), wmux::kAttachFrameHeaderSize});
  assert(header);
  assert(header->type == wmux::AttachFrameType::MouseEvent);
  assert(header->payload_size == 8);

  const auto mouse =
      wmux::parse_attach_mouse_event_payload(frame.substr(wmux::kAttachFrameHeaderSize));
  assert(mouse);
  assert(mouse->column == 61);
  assert(mouse->row == 8);
  assert(mouse->button_code == 32);
  assert(mouse->button == wmux::AttachMouseButton::Left);
  assert(mouse->action == wmux::AttachMouseAction::Drag);
}

void expects_attach_scroll_frame() {
  const auto frame = wmux::make_attach_scroll_frame(wmux::AttachScrollAction::PageUp);
  assert(frame.size() == wmux::kAttachFrameHeaderSize + 1);

  const auto header = wmux::parse_attach_frame_header(
      std::string_view{frame.data(), wmux::kAttachFrameHeaderSize});
  assert(header);
  assert(header->type == wmux::AttachFrameType::Scroll);
  assert(header->payload_size == 1);

  const auto scroll =
      wmux::parse_attach_scroll_payload(frame.substr(wmux::kAttachFrameHeaderSize));
  assert(scroll);
  assert(*scroll == wmux::AttachScrollAction::PageUp);
}

void expects_attach_copy_mode_frame() {
  const auto frame =
      wmux::make_attach_copy_mode_frame(wmux::AttachCopyModeAction::CopySelection);
  assert(frame.size() == wmux::kAttachFrameHeaderSize + 1);

  const auto header = wmux::parse_attach_frame_header(
      std::string_view{frame.data(), wmux::kAttachFrameHeaderSize});
  assert(header);
  assert(header->type == wmux::AttachFrameType::CopyMode);
  assert(header->payload_size == 1);

  const auto action =
      wmux::parse_attach_copy_mode_payload(frame.substr(wmux::kAttachFrameHeaderSize));
  assert(action);
  assert(*action == wmux::AttachCopyModeAction::CopySelection);
}

void expects_attach_paste_frame() {
  const auto frame = wmux::make_attach_paste_frame();
  assert(frame.size() == wmux::kAttachFrameHeaderSize);

  const auto header = wmux::parse_attach_frame_header(frame);
  assert(header);
  assert(header->type == wmux::AttachFrameType::Paste);
  assert(header->payload_size == 0);
}

void names_attach_frame_types() {
  assert(wmux::attach_frame_type_name(wmux::AttachFrameType::Input) == "Input");
  assert(wmux::attach_frame_type_name(wmux::AttachFrameType::Resize) == "Resize");
  assert(wmux::attach_frame_type_name(wmux::AttachFrameType::MouseEvent) == "Mouse");
  assert(wmux::attach_frame_type_name(wmux::AttachFrameType::Detach) == "Detach");
  assert(wmux::attach_frame_type_name(wmux::AttachFrameType::Paste) == "Paste");
}

void rejects_bad_attach_resize_payload() {
  assert(!wmux::parse_attach_resize_payload(""));
  assert(!wmux::parse_attach_resize_payload(std::string_view{"\0\0\x18\0", 4}));
  assert(!wmux::parse_attach_resize_payload(std::string_view{"\x01\x03\x18\0", 4}));
}

void rejects_oversized_attach_frame_payloads() {
  std::string header;
  header.reserve(wmux::kAttachFrameHeaderSize);
  header.push_back('W');
  header.push_back('M');
  header.push_back(static_cast<char>(wmux::AttachFrameType::Input));
  const auto payload_size = wmux::kMaxAttachInputPayloadBytes + 1;
  header.push_back(static_cast<char>(payload_size & 0xFF));
  header.push_back(static_cast<char>((payload_size >> 8) & 0xFF));
  header.push_back(static_cast<char>((payload_size >> 16) & 0xFF));
  header.push_back(static_cast<char>((payload_size >> 24) & 0xFF));

  assert(!wmux::parse_attach_frame_header(header));
}

void rejects_bad_attach_mouse_focus_payload() {
  assert(!wmux::parse_attach_mouse_focus_payload(""));
  assert(!wmux::parse_attach_mouse_focus_payload(std::string_view{"\0\0\x08\0", 4}));
  assert(!wmux::parse_attach_mouse_focus_payload(std::string_view{"\x3d\0\0\0", 4}));
}

void rejects_bad_attach_mouse_event_payload() {
  assert(!wmux::parse_attach_mouse_event_payload(""));
  assert(!wmux::parse_attach_mouse_event_payload(std::string_view{"\x01\0\x01\0\0\0\x07\0", 8}));
  assert(!wmux::parse_attach_mouse_event_payload(std::string_view{"\x01\0\x01\0\0\0\0\x04", 8}));
}

void rejects_bad_attach_scroll_payload() {
  assert(!wmux::parse_attach_scroll_payload(""));
  assert(!wmux::parse_attach_scroll_payload(std::string_view{"\x05", 1}));
}

void rejects_bad_attach_copy_mode_payload() {
  assert(!wmux::parse_attach_copy_mode_payload(""));
  assert(!wmux::parse_attach_copy_mode_payload(std::string_view{"\x0a", 1}));
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
  expects_split_command_request_json();
  expects_set_option_request_json();
  expects_attach_request_json_with_terminal_size();
  expects_json_escaping();
  ignores_nested_false_positive_json_fields();
  rejects_json_fields_with_wrong_type();
  expects_response_json();
  expects_attach_response_json_with_settings();
  expects_attach_input_frame();
  expects_attach_detach_frame();
  expects_attach_command_frame();
  expects_attach_resize_frame();
  expects_attach_status_frame();
  expects_attach_command_mode_frame();
  expects_attach_mouse_focus_frame();
  expects_attach_mouse_event_frame();
  expects_attach_scroll_frame();
  expects_attach_copy_mode_frame();
  expects_attach_paste_frame();
  names_attach_frame_types();
  rejects_bad_attach_resize_payload();
  rejects_oversized_attach_frame_payloads();
  rejects_bad_attach_mouse_focus_payload();
  rejects_bad_attach_mouse_event_payload();
  rejects_bad_attach_scroll_payload();
  rejects_bad_attach_copy_mode_payload();
  expects_bad_json_rejected();
}
